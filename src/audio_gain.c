/* Host-side gain for the scrcpy RAW PCM format; no upstream DSP code.
 * SPDX-License-Identifier: Apache-2.0 */
#include "audio_gain.h"

#include <stdint.h>

/* -1 dBFS (sample peak) leaves some headroom after boosting. */
#define PEAK_LIMIT 29204.0f
/* 1 - exp(-1 / (48000 * 0.050)): recover smoothly over successive frames. */
#define RELEASE_STEP 0.00041658f

void audio_gain_init(struct audio_gain *gain, int gain_db)
{
    /* 10^(dB/20) for whole dB values; no math library needed. */
    static const float factors[] = {
        1.0f, 1.122018f, 1.258925f, 1.412538f, 1.584893f, 1.778279f,
        1.995262f, 2.238721f, 2.511886f, 2.818383f, 3.162278f, 3.548134f, 3.981072f,
    };
    _Static_assert(sizeof(factors) / sizeof(factors[0]) == OTG_AUDIO_GAIN_MAX_DB + 1,
                   "gain table must cover the supported range");
    gain->requested = gain_db >= 0 && gain_db <= OTG_AUDIO_GAIN_MAX_DB ? factors[gain_db] : 1.0f;
    gain->current = gain->requested;
}

static int read_sample(const unsigned char *p)
{
    const int value = p[0] | ((int)p[1] << 8);
    return value < 32768 ? value : value - 65536;
}

static void write_sample(unsigned char *p, float value)
{
    const uint16_t bits = (uint16_t)(int)(value + (value < 0.0f ? -0.5f : 0.5f));
    p[0] = (unsigned char)bits;
    p[1] = (unsigned char)(bits >> 8);
}

void audio_gain_apply(struct audio_gain *gain, unsigned char *pcm, size_t size)
{
    if (gain->requested == 1.0f) return;
    float current = gain->current;
    for (; size >= OTG_AUDIO_PCM_BYTES_PER_FRAME;
         size -= OTG_AUDIO_PCM_BYTES_PER_FRAME, pcm += OTG_AUDIO_PCM_BYTES_PER_FRAME) {
        const int left = read_sample(pcm), right = read_sample(pcm + 2);
        const int abs_left = left < 0 ? -left : left;
        const int abs_right = right < 0 ? -right : right;
        const int peak = abs_left > abs_right ? abs_left : abs_right;
        current += (gain->requested - current) * RELEASE_STEP;
        /* Limit immediately, recover gradually, and keep both channels linked.
         * State spans packets; no lookahead or extra playback buffer is needed. */
        if (peak * current > PEAK_LIMIT) current = PEAK_LIMIT / peak;
        write_sample(pcm, left * current);
        write_sample(pcm + 2, right * current);
    }
    gain->current = current;
}
