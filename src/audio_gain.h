/* Desktop playback gain for scrcpy's S16LE stereo PCM.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DESKFLOW_BRIDGE_AUDIO_GAIN_H
#define DESKFLOW_BRIDGE_AUDIO_GAIN_H

#include <stddef.h>

#define OTG_AUDIO_GAIN_MAX_DB 12
#define OTG_AUDIO_PCM_BYTES_PER_FRAME 4U

struct audio_gain {
    float requested;
    float current;
};

/* One independent state per 48 kHz playback stream. 0 dB bypasses processing. */
void audio_gain_init(struct audio_gain *gain, int gain_db);
void audio_gain_apply(struct audio_gain *gain, unsigned char *pcm, size_t size);

#endif /* DESKFLOW_BRIDGE_AUDIO_GAIN_H */
