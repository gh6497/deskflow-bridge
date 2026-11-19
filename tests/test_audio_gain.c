/* Host PCM gain and limiter regression, no audio device required.
 * SPDX-License-Identifier: Apache-2.0 */
#include "audio_gain.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void frame(unsigned char pcm[4], int left, int right)
{
    const uint16_t l = (uint16_t)left, r = (uint16_t)right;
    pcm[0] = (unsigned char)l;
    pcm[1] = (unsigned char)(l >> 8);
    pcm[2] = (unsigned char)r;
    pcm[3] = (unsigned char)(r >> 8);
}

static void check_frame(const unsigned char pcm[4], int left, int right)
{
    unsigned char expected[4];
    frame(expected, left, right);
    assert(memcmp(pcm, expected, sizeof(expected)) == 0);
}

int main(void)
{
    struct audio_gain gain;
    unsigned char pcm[4];
    audio_gain_init(&gain, 0);
    frame(pcm, 32767, -32768);
    audio_gain_apply(&gain, pcm, sizeof(pcm));
    check_frame(pcm, 32767, -32768); /* zero completely bypasses the limiter */

    const int boosts[] = {3, 6, OTG_AUDIO_GAIN_MAX_DB};
    const int expected[] = {1413, 1995, 3981};
    for (size_t i = 0; i < sizeof(boosts) / sizeof(boosts[0]); ++i) {
        audio_gain_init(&gain, boosts[i]);
        frame(pcm, 1000, -1000);
        audio_gain_apply(&gain, pcm, sizeof(pcm));
        check_frame(pcm, expected[i], -expected[i]);
        frame(pcm, 0, 0);
        audio_gain_apply(&gain, pcm, sizeof(pcm));
        check_frame(pcm, 0, 0);
    }

    unsigned char whole[12], split[12];
    frame(whole, 32767, -16384);
    frame(whole + 4, 16384, -32768);
    frame(whole + 8, 1000, -1000);
    memcpy(split, whole, sizeof(whole));
    audio_gain_init(&gain, OTG_AUDIO_GAIN_MAX_DB);
    audio_gain_apply(&gain, whole, sizeof(whole));
    check_frame(whole, 29204, -14602); /* limiting one side also scales the other */
    check_frame(whole + 4, 14602, -29204);
    check_frame(whole + 8, 893, -893); /* no sudden jump back to full gain */

    audio_gain_init(&gain, OTG_AUDIO_GAIN_MAX_DB);
    audio_gain_apply(&gain, split, 4);
    audio_gain_apply(&gain, split + 4, sizeof(split) - 4);
    assert(memcmp(whole, split, sizeof(whole)) == 0);
    for (unsigned i = 0; i < 24000; ++i) {
        frame(pcm, 1000, -1000);
        audio_gain_apply(&gain, pcm, sizeof(pcm));
    }
    check_frame(pcm, 3981, -3981); /* gain returns after quieter frames */

    puts("PASS: unity bypass, gain, linked peak limiting and recovery across packets");
    return 0;
}
