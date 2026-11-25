/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_BLUETOOTH_AUDIO_SHELL_BAP_AUDIO_H_
#define ZEPHYR_SUBSYS_BLUETOOTH_AUDIO_SHELL_BAP_AUDIO_H_

#include <zephyr/bluetooth/audio/bap.h>

/* Optional application audio routing. Register static storage before "bap init".
 * Returning true from started() transfers the stream's audio data path to these
 * callbacks, excluding the shell's sine/USB encoder and discard decoder.
 */
struct bt_shell_audio_ops {
	int (*init)(void);
	const struct bt_audio_codec_cap *sink_cap;
	const struct bt_audio_codec_cap *source_cap;
	int (*config)(enum bt_audio_dir dir, const struct bt_audio_codec_cfg *cfg);
	int (*qos)(struct bt_bap_stream *stream, const struct bt_bap_qos_cfg *qos,
		   struct bt_bap_ascs_rsp *rsp);
	bool (*started)(struct bt_bap_stream *stream, const struct bt_bap_ep_info *info);
	void (*stopped)(struct bt_bap_stream *stream);
	void (*recv)(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
		     struct net_buf *buf);
	void (*volume_changed)(uint8_t volume, bool mute);
};

/* Passing NULL removes the routing hooks. Registration is immutable after bap init. */
int bt_shell_audio_register(const struct bt_shell_audio_ops *ops);
int bt_shell_audio_init(void);
void bt_shell_audio_volume_changed(uint8_t volume, bool mute);

#endif /* ZEPHYR_SUBSYS_BLUETOOTH_AUDIO_SHELL_BAP_AUDIO_H_ */
