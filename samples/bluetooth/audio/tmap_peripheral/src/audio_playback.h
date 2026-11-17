/*
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TMAP_PERIPHERAL_AUDIO_PLAYBACK_H_
#define TMAP_PERIPHERAL_AUDIO_PLAYBACK_H_

#include <zephyr/bluetooth/audio/bap.h>

int audio_playback_init(void);
int audio_playback_configure(const struct bt_audio_codec_cfg *codec_cfg);
void audio_playback_start(void);
void audio_playback_stop(void);
void audio_playback_recv(const struct bt_iso_recv_info *info, struct net_buf *buf);

#endif /* TMAP_PERIPHERAL_AUDIO_PLAYBACK_H_ */
