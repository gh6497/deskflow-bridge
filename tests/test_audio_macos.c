/* Exercise the real macOS scrcpy player with socket pairs and fake AudioQueue
 * hardware. Catches lost/reordered samples, early readiness, blocked shutdown,
 * malformed stream acceptance and callbacks accessing disposed state.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "audio_player.h"
#include <AudioToolbox/AudioQueue.h>
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static struct {
    AudioQueueOutputCallback callback;
    void *userdata;
    AudioQueueBufferRef buffers[3];
    unsigned allocated;
    atomic_uint enqueues, disposed;
    atomic_int fail;
    pthread_mutex_t lock;
} output = {.lock = PTHREAD_MUTEX_INITIALIZER};
enum { FAIL_NONE, FAIL_NEW, FAIL_ALLOC, FAIL_ENQUEUE, FAIL_START };

OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *format,
                            AudioQueueOutputCallback callback, void *userdata,
                            CFRunLoopRef loop, CFStringRef mode, UInt32 flags,
                            AudioQueueRef *queue)
{
    assert(format->mSampleRate == 48000 && format->mChannelsPerFrame == 2);
    assert(format->mFormatID == kAudioFormatLinearPCM);
    assert(format->mFormatFlags == (kLinearPCMFormatFlagIsSignedInteger |
                                   kLinearPCMFormatFlagIsPacked));
    assert(format->mBitsPerChannel == 16 && format->mBytesPerFrame == 4);
    assert(format->mBytesPerPacket == 4 && format->mFramesPerPacket == 1);
    assert(!loop && !mode && !flags);
    if (atomic_load(&output.fail) == FAIL_NEW) return -1;
    output.callback = callback;
    output.userdata = userdata;
    *queue = (AudioQueueRef)&output;
    return 0;
}

OSStatus AudioQueueAllocateBuffer(AudioQueueRef queue, UInt32 size,
                                 AudioQueueBufferRef *buffer)
{
    assert(queue == (AudioQueueRef)&output && output.allocated < 3);
    if (atomic_load(&output.fail) == FAIL_ALLOC && output.allocated == 1) return -2;
    AudioQueueBuffer value = {.mAudioDataBytesCapacity = size,
                             .mAudioData = calloc(1, size)};
    *buffer = malloc(sizeof(value));
    assert(*buffer && value.mAudioData);
    memcpy(*buffer, &value, sizeof(value));
    output.buffers[output.allocated++] = *buffer;
    return 0;
}

OSStatus AudioQueueEnqueueBuffer(AudioQueueRef queue, AudioQueueBufferRef buffer,
                                UInt32 count, const AudioStreamPacketDescription *packets)
{
    assert(queue == (AudioQueueRef)&output && !count && !packets);
    assert(buffer->mAudioDataByteSize == 1920); /* 10 ms interleaved PCM */
    assert(buffer->mAudioDataByteSize <= buffer->mAudioDataBytesCapacity);
    if (atomic_load(&output.fail) == FAIL_ENQUEUE) return -3;
    atomic_fetch_add(&output.enqueues, 1);
    return 0;
}

OSStatus AudioQueueStart(AudioQueueRef queue, const AudioTimeStamp *when)
{
    assert(queue == (AudioQueueRef)&output && !when && output.allocated == 3);
    return atomic_load(&output.fail) == FAIL_START ? -4 : 0;
}

OSStatus AudioQueueDispose(AudioQueueRef queue, Boolean immediate)
{
    assert(queue == (AudioQueueRef)&output && immediate);
    pthread_mutex_lock(&output.lock);
    /* A callback already dispatched by the OS can finish during disposal.
     * The player must still exist, but must not re-enqueue after stopping. */
    unsigned enqueues = atomic_load(&output.enqueues);
    if (output.allocated) output.callback(output.userdata, queue, output.buffers[0]);
    assert(atomic_load(&output.enqueues) == enqueues);
    for (unsigned i = 0; i < output.allocated; ++i) {
        free(output.buffers[i]->mAudioData);
        free(output.buffers[i]);
    }
    output.allocated = 0;
    atomic_fetch_add(&output.disposed, 1);
    pthread_mutex_unlock(&output.lock);
    return 0;
}

static void delay(void)
{
    struct timespec time = {0, 1000000};
    nanosleep(&time, NULL);
}

static double now(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

static void wait_ready(struct audio_player *player, bool ready)
{
    double deadline = now() + 2;
    while (audio_player_ready(player) != ready && now() < deadline) delay();
    assert(audio_player_ready(player) == ready);
}

static void write_all(int fd, const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        assert(n > 0);
        p += n;
        size -= (size_t)n;
    }
}

static void frame_header(int fd, unsigned size)
{
    unsigned char header[12] = {0};
    header[8] = (unsigned char)(size >> 24);
    header[9] = (unsigned char)(size >> 16);
    header[10] = (unsigned char)(size >> 8);
    header[11] = (unsigned char)size;
    write_all(fd, header, sizeof(header));
}

static struct audio_player *start_with_gain(int sockets[2], int failure, bool raw, int gain_db)
{
    assert(!output.allocated);
    atomic_store(&output.fail, failure);
    atomic_store(&output.disposed, 0);
    atomic_store(&output.enqueues, 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct audio_player *player = audio_player_start(sockets[0], gain_db);
    assert(player && !audio_player_ready(player));
    if (raw) {
        const unsigned char codec[] = {0, 'r', 'a', 'w'};
        /* Fragmentation must not be mistaken for a complete header. */
        write_all(sockets[1], codec, 1);
        assert(!audio_player_ready(player));
        write_all(sockets[1], codec + 1, 3);
    }
    return player;
}

static struct audio_player *start(int sockets[2], int failure, bool raw)
{
    return start_with_gain(sockets, failure, raw, 0);
}

static void stop(struct audio_player *player, int sockets[2])
{
    double start_time = now();
    audio_player_stop(player);
    assert(now() - start_time < .5);
    assert(fcntl(sockets[0], F_GETFD) >= 0); /* borrowed, never closed */
    close(sockets[0]);
    close(sockets[1]);
    assert(!output.allocated);
}

static void test_pcm_and_silence(void)
{
    int sockets[2];
    struct audio_player *player = start(sockets, FAIL_NONE, true);
    wait_ready(player, true); /* A paused phone has not sent any PCM. */
    unsigned char pcm[4096];
    for (unsigned round = 0; round < 12; ++round) {
        for (unsigned i = 0; i < sizeof(pcm); ++i)
            pcm[i] = (unsigned char)(1 + (i + round) % 250);
        frame_header(sockets[1], sizeof(pcm));
        write_all(sockets[1], pcm, 3);
        write_all(sockets[1], pcm + 3, sizeof(pcm) - 3);
        size_t received = 0;
        double deadline = now() + 1;
        while (received < sizeof(pcm) && now() < deadline) {
            pthread_mutex_lock(&output.lock);
            AudioQueueBufferRef buffer = output.buffers[round % 3];
            output.callback(output.userdata, (AudioQueueRef)&output, buffer);
            unsigned char *data = buffer->mAudioData;
            size_t count = sizeof(pcm) - received;
            if (count > 1920) count = 1920;
            if (data[0]) {
                assert(!memcmp(data, pcm + received, count));
                received += count;
            } else count = 0;
            for (size_t i = count; i < 1920; ++i) assert(data[i] == 0);
            pthread_mutex_unlock(&output.lock);
            delay();
        }
        assert(received == sizeof(pcm)); /* includes several ring wraparounds */
    }
    shutdown(sockets[1], SHUT_WR);
    wait_ready(player, false);
    stop(player, sockets);
    assert(atomic_load(&output.disposed) == 1);
}

static void test_errors_and_cancellation(void)
{
    int sockets[2];
    for (int failure = FAIL_NEW; failure <= FAIL_START; ++failure) {
        struct audio_player *player = start(sockets, failure, true);
        /* Wait for the worker to consume the codec and fail initialization. */
        for (unsigned i = 0; i < 30; ++i) delay();
        assert(!audio_player_ready(player));
        stop(player, sockets);
        assert(atomic_load(&output.disposed) == (failure != FAIL_NEW));
    }
    for (unsigned codec = 0; codec < 3; ++codec) {
        struct audio_player *player = start(sockets, FAIL_NONE, false);
        const unsigned char data[] = {0, 0, 0, (unsigned char)codec};
        write_all(sockets[1], data, sizeof(data));
        shutdown(sockets[1], SHUT_WR);
        stop(player, sockets);
        assert(!atomic_load(&output.enqueues));
    }
    const unsigned invalid_sizes[] = {0, 3, 4097, 0xffffffffU};
    for (unsigned i = 0; i < sizeof(invalid_sizes) / sizeof(*invalid_sizes); ++i) {
        struct audio_player *player = start(sockets, FAIL_NONE, true);
        wait_ready(player, true);
        frame_header(sockets[1], invalid_sizes[i]);
        wait_ready(player, false);
        stop(player, sockets);
    }
    struct audio_player *player = start(sockets, FAIL_NONE, true);
    wait_ready(player, true);
    atomic_store(&output.fail, FAIL_ENQUEUE);
    pthread_mutex_lock(&output.lock);
    output.callback(output.userdata, (AudioQueueRef)&output, output.buffers[0]);
    pthread_mutex_unlock(&output.lock);
    wait_ready(player, false); /* Error must wake even a worker blocked in read. */
    stop(player, sockets);
    for (unsigned partial = 0; partial < 3; ++partial) {
        player = start(sockets, FAIL_NONE, false);
        write_all(sockets[1], "\0raw", partial);
        stop(player, sockets);
    }
    player = start(sockets, FAIL_NONE, true);
    wait_ready(player, true);
    frame_header(sockets[1], 4);
    write_all(sockets[1], "\1\2", 2);
    shutdown(sockets[1], SHUT_WR);
    wait_ready(player, false);
    stop(player, sockets);
}

static void test_pcm_gain(void)
{
    int sockets[2];
    struct audio_player *player = start_with_gain(sockets, FAIL_NONE, true, 6);
    wait_ready(player, true);
    const unsigned char pcm[] = {0xe8, 0x03, 0x18, 0xfc}; /* +1000, -1000 */
    const unsigned char expected[] = {0xcb, 0x07, 0x35, 0xf8}; /* +1995, -1995 */
    frame_header(sockets[1], sizeof(pcm));
    write_all(sockets[1], pcm, sizeof(pcm));
    bool received = false;
    const double deadline = now() + 1;
    while (!received && now() < deadline) {
        pthread_mutex_lock(&output.lock);
        AudioQueueBufferRef buffer = output.buffers[0];
        output.callback(output.userdata, (AudioQueueRef)&output, buffer);
        const unsigned char *data = buffer->mAudioData;
        if (data[0]) {
            assert(!memcmp(data, expected, sizeof(expected)));
            received = true;
        }
        pthread_mutex_unlock(&output.lock);
        delay();
    }
    assert(received);
    stop(player, sockets);
}

static void test_stalled_output(void)
{
    int sockets[2];
    unsigned char pcm[4096] = {1};
    for (unsigned timeout = 0; timeout < 2; ++timeout) {
        struct audio_player *player = start(sockets, FAIL_NONE, true);
        wait_ready(player, true);
        for (unsigned i = 0; i < 6; ++i) {
            frame_header(sockets[1], sizeof(pcm));
            write_all(sockets[1], pcm, sizeof(pcm));
        }
        if (timeout) wait_ready(player, false);
        else for (unsigned i = 0; i < 30; ++i) delay();
        stop(player, sockets);
        assert(atomic_load(&output.disposed) == 1);
    }
}

static void *pump_callbacks(void *arg)
{
    atomic_bool *finished = arg;
    while (!atomic_load(finished)) {
        pthread_mutex_lock(&output.lock);
        if (output.allocated)
            output.callback(output.userdata, (AudioQueueRef)&output, output.buffers[0]);
        pthread_mutex_unlock(&output.lock);
        delay();
    }
    return NULL;
}

static void test_callback_stop_race(void)
{
    for (unsigned i = 0; i < 20; ++i) {
        int sockets[2];
        struct audio_player *player = start(sockets, FAIL_NONE, true);
        wait_ready(player, true);
        atomic_bool finished = false;
        pthread_t pump;
        assert(!pthread_create(&pump, NULL, pump_callbacks, &finished));
        /* Stop while a frame header or payload is incomplete, with output live. */
        if (i % 2) frame_header(sockets[1], 4);
        write_all(sockets[1], "\1\2", 2);
        stop(player, sockets);
        atomic_store(&finished, true);
        assert(!pthread_join(pump, NULL));
        assert(atomic_load(&output.disposed) == 1);
    }
}

int main(void)
{
    assert(!audio_player_ready(NULL));
    audio_player_stop(NULL);
    test_pcm_and_silence();
    test_pcm_gain();
    test_errors_and_cancellation();
    test_stalled_output();
    test_callback_stop_race();
    puts("PASS: macOS PCM ordering, silence, readiness, failures and bounded shutdown");
    return 0;
}
