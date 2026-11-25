/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

#include <audio_capture.h>
#include <audio_playback.h>
#include <bap_audio.h>

LOG_MODULE_REGISTER(tmap_shell_audio, LOG_LEVEL_INF);

static K_MUTEX_DEFINE(audio_lock);
static struct bt_bap_stream *sink_stream;
static struct bt_bap_stream *source_stream;

static const struct bt_audio_codec_cap sink_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_32KHZ |
		BT_AUDIO_CODEC_CAP_FREQ_48KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_10, BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1U, 2U),
	30U, AUDIO_CAPTURE_MAX_OCTETS, 1U,
	BT_AUDIO_CONTEXT_TYPE_MEDIA | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);

static const struct bt_audio_codec_cap source_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_32KHZ, BT_AUDIO_CODEC_CAP_DURATION_10,
	BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1U), 30U, AUDIO_CAPTURE_MAX_OCTETS, 1U,
	BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);

static void audio_lock_take(void)
{
	__maybe_unused int err = k_mutex_lock(&audio_lock, K_FOREVER);

	__ASSERT_NO_MSG(err == 0);
}

static void audio_lock_give(void)
{
	__maybe_unused int err = k_mutex_unlock(&audio_lock);

	__ASSERT_NO_MSG(err == 0);
}

static int sink_config(const struct bt_audio_codec_cfg *cfg, struct sample_bt_audio_pcm_cfg *pcm)
{
	enum bt_audio_location allocation;
	int frequency = bt_audio_codec_cfg_freq_to_freq_hz(bt_audio_codec_cfg_get_freq(cfg));
	int octets = bt_audio_codec_cfg_get_octets_per_frame(cfg);
	int frames = bt_audio_codec_cfg_get_frame_blocks_per_sdu(cfg, true);

	if (cfg->id != BT_HCI_CODING_FORMAT_LC3 ||
	    (frequency != 16000 && frequency != 32000 && frequency != 48000) ||
	    bt_audio_codec_cfg_get_frame_dur(cfg) != BT_AUDIO_CODEC_CFG_DURATION_10 ||
	    frames != 1 || octets < 30 || octets > AUDIO_CAPTURE_MAX_OCTETS ||
	    bt_audio_codec_cfg_get_chan_allocation(cfg, &allocation, true) != 0) {
		return -ENOTSUP;
	}
	const uint8_t channels = bt_audio_get_chan_count(allocation);

	if (channels == 0U || channels > 2U) {
		return -ENOTSUP;
	}
	*pcm = (struct sample_bt_audio_pcm_cfg){
		.freq_hz = (uint32_t)frequency,
		.frame_duration_us = 10000U,
		.octets_per_frame = (uint16_t)octets,
		.chan_cnt = channels,
		.frame_blocks_per_sdu = 1U,
	};
	return 0;
}

static int stream_config(enum bt_audio_dir dir, const struct bt_audio_codec_cfg *cfg)
{
	struct sample_bt_audio_pcm_cfg pcm;

	return dir == BT_AUDIO_DIR_SOURCE ? audio_capture_configure(cfg) : sink_config(cfg, &pcm);
}

static int stream_qos(struct bt_bap_stream *stream, const struct bt_bap_qos_cfg *qos,
		      struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(stream);
	if (qos->interval != 10000U) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				      BT_BAP_ASCS_REASON_INTERVAL);
		return -ENOTSUP;
	}
	return 0;
}

static bool stream_started(struct bt_bap_stream *stream, const struct bt_bap_ep_info *info)
{
	struct bt_conn_info conn_info;
	int err;

	if (stream->conn == NULL || bt_conn_get_info(stream->conn, &conn_info) != 0 ||
	    conn_info.role != BT_CONN_ROLE_PERIPHERAL) {
		return false;
	}
	audio_lock_take();
	if (info->dir == BT_AUDIO_DIR_SINK) {
		struct sample_bt_audio_pcm_cfg pcm;

		sink_stream = stream;
		err = sink_config(stream->codec_cfg, &pcm);
		if (err == 0) {
			err = sample_bt_audio_playback_start(&pcm);
		}
		if (err != 0) {
			LOG_ERR("Playback start failed: %d", err);
		}
	} else {
		source_stream = stream;
		audio_capture_start(stream);
	}
	audio_lock_give();
	return true;
}

static void stream_stopped(struct bt_bap_stream *stream)
{
	audio_lock_take();
	if (stream == sink_stream) {
		sample_bt_audio_playback_stop();
		sink_stream = NULL;
	}
	if (stream == source_stream) {
		audio_capture_stop(stream);
		source_stream = NULL;
	}
	audio_lock_give();
}

static void stream_recv(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	audio_lock_take();
	if (stream == sink_stream) {
		sample_bt_audio_playback_recv(info, buf);
	}
	audio_lock_give();
}

static void volume_changed(uint8_t volume, bool mute)
{
	sample_bt_audio_playback_set_volume(volume);
	sample_bt_audio_playback_set_mute(mute);
}

static int board_audio_init(void)
{
	static bool playback_ready;
	int err = 0;

	if (!playback_ready) {
		err = sample_bt_audio_playback_init();
		if (err == 0) {
			playback_ready = true;
		}
	}
	if (err == 0) {
		err = audio_capture_init();
	}
	return err;
}

static const struct bt_shell_audio_ops audio_ops = {
	.init = board_audio_init,
	.sink_cap = &sink_cap,
	.source_cap = &source_cap,
	.config = stream_config,
	.qos = stream_qos,
	.started = stream_started,
	.stopped = stream_stopped,
	.recv = stream_recv,
	.volume_changed = volume_changed,
};

static int register_audio(void)
{
	return bt_shell_audio_register(&audio_ops);
}

SYS_INIT(register_audio, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
