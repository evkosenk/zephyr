/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TMAP_PERIPHERAL_AUDIO_JACK_CAPTURE_H_
#define TMAP_PERIPHERAL_AUDIO_JACK_CAPTURE_H_

#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_TMAP_PERIPHERAL_AUDIO_JACK_MIC)
int audio_jack_capture_init(void);
int audio_jack_capture_configure(const struct bt_audio_codec_cfg *codec_cfg);
void audio_jack_capture_start(struct bt_bap_stream *stream);
void audio_jack_capture_stop(struct bt_bap_stream *stream);
void audio_jack_capture_sent(struct bt_bap_stream *stream);
#else
static inline int audio_jack_capture_init(void)
{
	return 0;
}

static inline int audio_jack_capture_configure(const struct bt_audio_codec_cfg *codec_cfg)
{
	ARG_UNUSED(codec_cfg);
	return 0;
}

static inline void audio_jack_capture_start(struct bt_bap_stream *stream)
{
	ARG_UNUSED(stream);
}

static inline void audio_jack_capture_stop(struct bt_bap_stream *stream)
{
	ARG_UNUSED(stream);
}

static inline void audio_jack_capture_sent(struct bt_bap_stream *stream)
{
	ARG_UNUSED(stream);
}
#endif

#endif /* TMAP_PERIPHERAL_AUDIO_JACK_CAPTURE_H_ */
