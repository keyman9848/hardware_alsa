/* AudioStreamOutALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <netinet/tcp.h>
#include <netinet/in.h>

#define LOG_TAG "AudioHardwareALSA"
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/sockets.h>
#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

namespace android
{

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

// ----------------------------------------------------------------------------

AudioStreamOutALSA::AudioStreamOutALSA(AudioHardwareALSA *parent, alsa_handle_t *handle) :
    ALSAStreamOps(parent, handle),
    mFrameCount(0),
    pcm_server_socket(0)
{
    SLOGI("Starting pcm server");
    pthread_t server_thread;
    pcm_server_socket = 0;
    // Device open, start a new pcm server in a different thread
    if (pthread_create(&server_thread, NULL, &AudioStreamOutALSA::start_pcm_server, this)) {
        SLOGE("Unable to create pcm_server_open thread");
    }
}

AudioStreamOutALSA::~AudioStreamOutALSA()
{
    close();
}

uint32_t AudioStreamOutALSA::channels() const
{
    int c = ALSAStreamOps::channels();
    return c;
}

status_t AudioStreamOutALSA::setVolume(float left, float right)
{
    return mixer()->setVolume (mHandle->curDev, left, right);
}

ssize_t AudioStreamOutALSA::write(const void *buffer, size_t bytes)
{
    AutoMutex lock(mLock);

    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioOutLock");
        mPowerLock = true;
    }

    acoustic_device_t *aDev = acoustics();

    // For output, we will pass the data on to the acoustics module, but the actual
    // data is expected to be sent to the audio device directly as well.
    if (aDev && aDev->write)
        aDev->write(aDev, buffer, bytes);

    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;

    if (pcm_server_socket) {

        int pcm_sent = 0;

        // pcm server connected, send audio data
        do {
            pcm_sent = ::write(pcm_server_socket, (char *)buffer + pcm_sent, bytes - pcm_sent);
        } while (pcm_sent > 0);
    }

    do {

        n = snd_pcm_writei(mHandle->handle,
                           (char *)buffer + sent,
                           snd_pcm_bytes_to_frames(mHandle->handle, bytes - sent));
        if (n == -EBADFD) {
            // Somehow the stream is in a bad state. The driver probably
            // has a bug and snd_pcm_recover() doesn't seem to handle this.
            mHandle->module->open(mHandle, mHandle->curDev, mHandle->curMode);

            if (aDev && aDev->recover) aDev->recover(aDev, n);
        }
        else if (n < 0) {
            if (mHandle->handle) {
                // snd_pcm_recover() will return 0 if successful in recovering from
                // an error, or -errno if the error was unrecoverable.
                n = snd_pcm_recover(mHandle->handle, n, 1);

                if (aDev && aDev->recover) aDev->recover(aDev, n);

                if (n) return static_cast<ssize_t>(n);
            }
        }
        else {
            mFrameCount += n;
            sent += static_cast<ssize_t>(snd_pcm_frames_to_bytes(mHandle->handle, n));
        }

    } while (mHandle->handle && sent < bytes);

    return sent;
}

status_t AudioStreamOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamOutALSA::open(int mode)
{
    AutoMutex lock(mLock);

    return ALSAStreamOps::open(mode);
}

void *AudioStreamOutALSA::start_pcm_server(void *arg)
{
    AudioStreamOutALSA *out = (AudioStreamOutALSA *)arg;

    SLOGI("out sampling rate %d", out->sampleRate());

    // Wait forever for a new connection
    while(1) {
        int ssocket;
        int csocket;

        ssocket = socket_inaddr_any_server(24296, SOCK_STREAM);

        if (ssocket < 0) {
            SLOGE("Unable to start listening pcm server");
            break;
        }

        // waiting for new connection
        csocket = accept(ssocket, NULL, NULL);

        if (csocket < 0) {
            SLOGE("Unable to accept connection to pcm server");
            ::close(ssocket);
            break;
        }

        SLOGI("pcm server connected");

        int opt_nodelay = 1;
        setsockopt(csocket, IPPROTO_TCP, TCP_NODELAY, &opt_nodelay, sizeof(opt_nodelay));

        ::close(ssocket);

        SLOGI("pcm server starting");
        out->setPCMServerSocket(csocket);

        // Waiting forever for new messages
        while(1) {
            fd_set set_read;
            char buf;

            FD_ZERO(&set_read);
            FD_SET(csocket, &set_read);

            if(select(csocket + 1, &set_read, NULL, NULL, NULL) <= 0) {
                SLOGE("pcm server error during select");
                break;
            }

            // Wen a client socket disconnect, select signal read activitie
            // on the corresponding socket, but read operation will return zero
            // bytes. This is the best way to detect disconnection
            if(read(csocket, &buf, 1) <= 0) {
                SLOGI("pcm server lost connection");
                break;
            }

            SLOGI("pcm server receive message %s", buf);
        }
        // close and wait for a new connection
        // modification to out should be protected by mutex
        // but it makes the audio driver hang for to long
        // and android applications doesn't support it
        out->setPCMServerSocket(0);
        ::close(csocket);
        SLOGI("pcm server closed");
    }

    return NULL;
}

status_t AudioStreamOutALSA::close()
{
    AutoMutex lock(mLock);

    snd_pcm_drain (mHandle->handle);
    ALSAStreamOps::close();

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    if (pcm_server_socket) {
        ::close(pcm_server_socket);
        pcm_server_socket = 0;
    }

    return NO_ERROR;
}

status_t AudioStreamOutALSA::standby()
{
    AutoMutex lock(mLock);

    snd_pcm_drain (mHandle->handle);

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    mFrameCount = 0;

    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioStreamOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mHandle->latency);
}

// return the number of audio frames written by the audio dsp to DAC since
// the output has exited standby
status_t AudioStreamOutALSA::getRenderPosition(uint32_t *dspFrames)
{
    *dspFrames = mFrameCount;
    return NO_ERROR;
}

void AudioStreamOutALSA::setPCMServerSocket(int csocket)
{
    pcm_server_socket = csocket;
}

}       // namespace android
