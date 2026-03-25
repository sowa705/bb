/*

   Name:
   DRV_COREAUDIO.C

   Description:
   Mikmod driver for output on macOS using AudioToolbox AudioQueue Services.

   Portability: macOS / Apple platforms only.

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>
#include "mikmod.h"

#define NUM_BUFFERS 3
#define BUFFER_SIZE 4096

static AudioQueueRef audioQueue;
static AudioQueueBufferRef buffers[NUM_BUFFERS];
static int fragmentsize;
static volatile int started;  /* has AudioQueueStart been called? */

/* Synchronization: main thread blocks in Update() until a buffer is needed */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static volatile int buffers_pending; /* how many buffers are queued/playing */

/*
   AudioQueue callback — called when a buffer has been consumed and is
   available for refill. We signal the main thread to produce more data.
*/
static void AQBufferCallback(void *userdata, AudioQueueRef queue,
                             AudioQueueBufferRef buf)
{
    (void)userdata;
    (void)queue;
    (void)buf;

    pthread_mutex_lock(&mutex);
    buffers_pending--;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
}


static BOOL CA_IsThere(void)
{
    /* CoreAudio is always available on macOS */
    return 1;
}


static BOOL CA_Init(void)
{
    AudioStreamBasicDescription fmt;
    OSStatus err;
    int i;

    memset(&fmt, 0, sizeof(fmt));

    fmt.mSampleRate = (Float64)md_mixfreq;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsPacked;

    if (md_mode & DMODE_16BITS) {
        fmt.mBitsPerChannel = 16;
        fmt.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    } else {
        fmt.mBitsPerChannel = 8;
        /* 8-bit PCM from MikMod is unsigned */
    }

    fmt.mChannelsPerFrame = (md_mode & DMODE_STEREO) ? 2 : 1;
    fmt.mBytesPerFrame = (fmt.mBitsPerChannel / 8) * fmt.mChannelsPerFrame;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    /* Calculate fragment size similar to OSS driver */
    fragmentsize = BUFFER_SIZE;
    if (md_mode & DMODE_16BITS)
        fragmentsize *= 2;
    if (md_mode & DMODE_STEREO)
        fragmentsize *= 2;

    /* Initialize the virtual channel mixer */
    if (!VC_Init()) {
        return 0;
    }

    /* Create the AudioQueue */
    err = AudioQueueNewOutput(&fmt, AQBufferCallback, NULL,
                              NULL, NULL, 0, &audioQueue);
    if (err != noErr) {
        myerr = "CoreAudio: Cannot create AudioQueue";
        VC_Exit();
        return 0;
    }

    /* Allocate buffers but don't enqueue yet — defer to first Update() */
    for (i = 0; i < NUM_BUFFERS; i++) {
        err = AudioQueueAllocateBuffer(audioQueue, fragmentsize, &buffers[i]);
        if (err != noErr) {
            myerr = "CoreAudio: Cannot allocate audio buffer";
            AudioQueueDispose(audioQueue, true);
            VC_Exit();
            return 0;
        }
    }

    buffers_pending = 0;
    started = 0;

    return 1;
}


static void CA_Exit(void)
{
    if (audioQueue) {
        AudioQueueStop(audioQueue, true);
        AudioQueueDispose(audioQueue, true);
        audioQueue = NULL;
    }
    started = 0;
    VC_Exit();
}


static void CA_Update(void)
{
    /*
       On the first call, prime all buffers with rendered audio and start
       the AudioQueue. This ensures playback begins exactly when the
       server enters its playback loop (after receiving the 'S' command),
       rather than during Init when silence would be playing.
    */
    if (!started) {
        int i;
        OSStatus err;
        for (i = 0; i < NUM_BUFFERS; i++) {
            VC_WriteBytes((SBYTE *)buffers[i]->mAudioData, fragmentsize);
            buffers[i]->mAudioDataByteSize = fragmentsize;
            AudioQueueEnqueueBuffer(audioQueue, buffers[i], 0, NULL);
            buffers_pending++;
        }
        err = AudioQueueStart(audioQueue, NULL);
        if (err != noErr) {
            fprintf(stderr, "CoreAudio: Failed to start AudioQueue\n");
        }
        started = 1;
        return;
    }

    /*
       Wait until at least one buffer has been consumed by the AudioQueue
       callback, then refill and re-enqueue it.
    */
    pthread_mutex_lock(&mutex);
    while (buffers_pending >= NUM_BUFFERS) {
        pthread_cond_wait(&cond, &mutex);
    }
    pthread_mutex_unlock(&mutex);

    /*
       Cycle through buffers in FIFO order — AudioQueue processes them
       in the order they were enqueued.
    */
    {
        static int buf_idx = 0;
        AudioQueueBufferRef buf = buffers[buf_idx];
        buf_idx = (buf_idx + 1) % NUM_BUFFERS;

        /* Have the virtual channel mixer render audio into our buffer */
        VC_WriteBytes((SBYTE *)buf->mAudioData, fragmentsize);
        buf->mAudioDataByteSize = fragmentsize;
        AudioQueueEnqueueBuffer(audioQueue, buf, 0, NULL);

        pthread_mutex_lock(&mutex);
        buffers_pending++;
        pthread_mutex_unlock(&mutex);
    }
}


DRIVER drv_coreaudio =
{
    NULL,
    "CoreAudio",
    "CoreAudio AudioQueue Driver v1.0 for macOS",
    CA_IsThere,
    VC_SampleLoad,
    VC_SampleUnload,
    CA_Init,
    CA_Exit,
    VC_PlayStart,
    VC_PlayStop,
    CA_Update,
    VC_VoiceSetVolume,
    VC_VoiceSetFrequency,
    VC_VoiceSetPanning,
    VC_VoicePlay,
    MD_BlankFunction,
    MD_BlankFunction,
    MD_BlankFunction
};
