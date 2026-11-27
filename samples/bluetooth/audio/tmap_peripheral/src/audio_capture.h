/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TMAP_PERIPHERAL_AUDIO_CAPTURE_H_
#define TMAP_PERIPHERAL_AUDIO_CAPTURE_H_

#include <zephyr/bluetooth/audio/bap.h>

#define AUDIO_CAPTURE_MAX_OCTETS 155U

int audio_capture_init(void);
int audio_capture_configure(const struct bt_audio_codec_cfg *codec_cfg);
void audio_capture_start(struct bt_bap_stream *stream);
void audio_capture_stop(struct bt_bap_stream *stream);

#endif /* TMAP_PERIPHERAL_AUDIO_CAPTURE_H_ */
