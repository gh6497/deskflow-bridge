/* macOS playback for scrcpy's RAW audio stream (codec + framed S16_LE PCM).
 * Protocol reference: scrcpy demuxer/audio raw stream; AudioQueue replaces SDL.
 * SPDX-License-Identifier: Apache-2.0 */
#define _DARWIN_C_SOURCE
#include "audio_gain.h"
#include "audio_player.h"

#include <AudioToolbox/AudioQueue.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_SAMPLE_RATE 48000U
#define AUDIO_BYTES_PER_FRAME OTG_AUDIO_PCM_BYTES_PER_FRAME
#define AUDIO_BUFFER_SIZE (480U * AUDIO_BYTES_PER_FRAME) /* 10 ms */
#define AUDIO_BUFFER_COUNT 3U
#define AUDIO_RING_SIZE (4800U * AUDIO_BYTES_PER_FRAME) /* 100 ms */
#define AUDIO_MAX_FRAME_SIZE (1024U * AUDIO_BYTES_PER_FRAME)
#define SCRCPY_CODEC_RAW UINT32_C(0x00726177)
#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) fprintf(stderr, "WARN:  " __VA_ARGS__)

struct audio_player {
    int socket_fd;
    pthread_t thread;
    atomic_bool ready;
    struct audio_gain gain;
    pthread_mutex_t mutex;
    pthread_cond_t space;
    bool stopped;
    OSStatus output_error;
    unsigned char ring[AUDIO_RING_SIZE];
    size_t read_pos;
    size_t buffered;
};

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t now_ns(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static bool is_stopped(struct audio_player *player)
{
    pthread_mutex_lock(&player->mutex);
    bool stopped = player->stopped;
    pthread_mutex_unlock(&player->mutex);
    return stopped;
}

static int read_full(struct audio_player *player, void *buffer, size_t size)
{
    unsigned char *p = buffer;
    while (size && !is_stopped(player)) {
        /* A callback can fail while the phone is paused. Periodically observe
         * its status without requiring the callback to touch the socket. */
        struct pollfd fd = {player->socket_fd, POLLIN, 0};
        int result = poll(&fd, 1, 50);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!result) continue;
        ssize_t n = read(player->socket_fd, p, size);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        p += (size_t)n;
        size -= (size_t)n;
    }
    return size ? -1 : 1;
}

static void output_callback(void *userdata, AudioQueueRef queue,
                            AudioQueueBufferRef buffer)
{
    struct audio_player *player = userdata;
    pthread_mutex_lock(&player->mutex);
    if (player->stopped) {
        pthread_mutex_unlock(&player->mutex);
        return;
    }
    size_t size = player->buffered;
    if (size > AUDIO_BUFFER_SIZE) size = AUDIO_BUFFER_SIZE;
    size_t first = AUDIO_RING_SIZE - player->read_pos;
    if (first > size) first = size;
    unsigned char *data = buffer->mAudioData;
    memcpy(data, player->ring + player->read_pos, first);
    memcpy(data + first, player->ring, size - first);
    memset(data + size, 0, AUDIO_BUFFER_SIZE - size);
    player->read_pos = (player->read_pos + size) % AUDIO_RING_SIZE;
    player->buffered -= size;
    buffer->mAudioDataByteSize = AUDIO_BUFFER_SIZE;
    pthread_cond_signal(&player->space);
    pthread_mutex_unlock(&player->mutex);

    /* Disposal is synchronous on the worker, outside the lock, and waits for
     * any dispatched callback to finish before freeing queue or player data. */
    OSStatus error = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    if (error) {
        pthread_mutex_lock(&player->mutex);
        player->output_error = error;
        player->stopped = true;
        atomic_store(&player->ready, false);
        pthread_cond_signal(&player->space);
        pthread_mutex_unlock(&player->mutex);
    }
}

static OSStatus open_output(struct audio_player *player, AudioQueueRef *queue)
{
    AudioStreamBasicDescription format = {
        .mSampleRate = AUDIO_SAMPLE_RATE,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = AUDIO_BYTES_PER_FRAME,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = AUDIO_BYTES_PER_FRAME,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 16,
    };
    OSStatus error = AudioQueueNewOutput(&format, output_callback, player,
                                        NULL, NULL, 0, queue);
    if (error) return error;
    for (unsigned i = 0; i < AUDIO_BUFFER_COUNT; ++i) {
        AudioQueueBufferRef buffer;
        error = AudioQueueAllocateBuffer(*queue, AUDIO_BUFFER_SIZE, &buffer);
        if (error) return error;
        memset(buffer->mAudioData, 0, AUDIO_BUFFER_SIZE);
        buffer->mAudioDataByteSize = AUDIO_BUFFER_SIZE;
        error = AudioQueueEnqueueBuffer(*queue, buffer, 0, NULL);
        if (error) return error;
    }
    /* Start silence now: readiness must not depend on a paused phone producing
     * PCM. AudioQueue converts the client PCM to the default output format. */
    return AudioQueueStart(*queue, NULL);
}

static int buffer_pcm(struct audio_player *player, const unsigned char *data, size_t size)
{
    uint64_t deadline = now_ns() + UINT64_C(1000000000);
    pthread_mutex_lock(&player->mutex);
    while (!player->stopped && AUDIO_RING_SIZE - player->buffered < size) {
        uint64_t now = now_ns();
        if (now >= deadline) {
            LOG_ERR("macOS audio output stalled for one second\n");
            pthread_mutex_unlock(&player->mutex);
            return -1;
        }
        uint64_t remaining = deadline - now;
        struct timespec delay = {(time_t)(remaining / UINT64_C(1000000000)),
                                 (long)(remaining % UINT64_C(1000000000))};
        /* Darwin's relative wait avoids wall-clock changes extending shutdown. */
        int error = pthread_cond_timedwait_relative_np(&player->space, &player->mutex, &delay);
        if (error && error != ETIMEDOUT) {
            pthread_mutex_unlock(&player->mutex);
            return -1;
        }
    }
    if (player->stopped) {
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }
    size_t write_pos = (player->read_pos + player->buffered) % AUDIO_RING_SIZE;
    size_t first = AUDIO_RING_SIZE - write_pos;
    if (first > size) first = size;
    memcpy(player->ring + write_pos, data, first);
    memcpy(player->ring, data + first, size - first);
    player->buffered += size;
    pthread_mutex_unlock(&player->mutex);
    return 0;
}

static void *audio_thread_main(void *userdata)
{
    struct audio_player *player = userdata;
    unsigned char codec[4], header[12], payload[AUDIO_MAX_FRAME_SIZE];
    AudioQueueRef queue = NULL;
    if (read_full(player, codec, sizeof(codec)) != 1) goto done;
    if (!read_be32(codec)) {
        LOG_WARN("Android could not capture audio; continuing without audio\n");
        goto done;
    }
    if (read_be32(codec) != SCRCPY_CODEC_RAW) {
        LOG_ERR("scrcpy audio codec is not raw PCM; use audio_codec=raw\n");
        goto done;
    }
    if (is_stopped(player)) goto done;
    OSStatus error = open_output(player, &queue);
    if (error) {
        LOG_ERR("could not open macOS AudioQueue playback (OSStatus=%d)\n", (int)error);
        goto done;
    }
    pthread_mutex_lock(&player->mutex);
    if (!player->stopped) atomic_store(&player->ready, true);
    pthread_mutex_unlock(&player->mutex);
    while (read_full(player, header, sizeof(header)) == 1) {
        /* RAW playback has no separate clock; consume PTS/flags with the header. */
        uint32_t size = read_be32(header + 8);
        if (!size || size > AUDIO_MAX_FRAME_SIZE || size % AUDIO_BYTES_PER_FRAME) {
            LOG_ERR("invalid scrcpy audio frame size: %u\n", size);
            break;
        }
        if (read_full(player, payload, size) != 1) break;
        audio_gain_apply(&player->gain, payload, size);
        if (buffer_pcm(player, payload, size)) break;
    }
done:
    pthread_mutex_lock(&player->mutex);
    player->stopped = true;
    atomic_store(&player->ready, false);
    pthread_mutex_unlock(&player->mutex);
    if (queue) {
        OSStatus dispose_error = AudioQueueDispose(queue, true);
        if (dispose_error) LOG_WARN("AudioQueueDispose failed (OSStatus=%d)\n", (int)dispose_error);
    }
    if (player->output_error)
        LOG_ERR("macOS AudioQueue playback failed (OSStatus=%d)\n", (int)player->output_error);
    return NULL;
}

struct audio_player *audio_player_start(intptr_t socket_fd, int gain_db)
{
    struct audio_player *player = calloc(1, sizeof(*player));
    if (!player) return NULL;
    player->socket_fd = (int)socket_fd;
    audio_gain_init(&player->gain, gain_db);
    atomic_init(&player->ready, false);
    if (pthread_mutex_init(&player->mutex, NULL)) goto fail;
    if (pthread_cond_init(&player->space, NULL)) goto fail_mutex;
    if (pthread_create(&player->thread, NULL, audio_thread_main, player)) goto fail_cond;
    return player;
fail_cond:
    pthread_cond_destroy(&player->space);
fail_mutex:
    pthread_mutex_destroy(&player->mutex);
fail:
    free(player);
    return NULL;
}

bool audio_player_ready(struct audio_player *player)
{
    return player && atomic_load(&player->ready);
}

void audio_player_stop(struct audio_player *player)
{
    if (!player) return;
    pthread_mutex_lock(&player->mutex);
    player->stopped = true;
    atomic_store(&player->ready, false);
    pthread_cond_signal(&player->space);
    pthread_mutex_unlock(&player->mutex);
    (void)shutdown(player->socket_fd, SHUT_RDWR);
    (void)pthread_join(player->thread, NULL);
    pthread_cond_destroy(&player->space);
    pthread_mutex_destroy(&player->mutex);
    free(player);
}
