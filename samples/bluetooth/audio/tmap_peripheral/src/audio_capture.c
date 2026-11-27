/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <lc3.h>

#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>

#include <pcm_sink.h>

#include "audio_capture.h"

LOG_MODULE_REGISTER(tmap_audio_capture, LOG_LEVEL_INF);

#define CAPTURE_RATE_HZ 48000U
#define CAPTURE_CHANNELS 2U
#define CAPTURE_SAMPLES (CAPTURE_RATE_HZ / 100U)
#define CAPTURE_BLOCK_SIZE (CAPTURE_SAMPLES * CAPTURE_CHANNELS * sizeof(int16_t))
#define CAPTURE_BLOCK_COUNT 8U

BUILD_ASSERT(DT_SAME_NODE(DT_ALIAS(i2s_codec_rx), DT_ALIAS(i2s_codec_tx)),
	     "Capture and playback must share an I2S peripheral");

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(capture_slab, __nocache, CAPTURE_BLOCK_SIZE,
			       CAPTURE_BLOCK_COUNT, 4);

static const struct device *const i2s_rx = DEVICE_DT_GET(DT_ALIAS(i2s_codec_rx));
static K_MUTEX_DEFINE(tx_lock);
static struct bt_bap_stream *tx_stream;
static lc3_encoder_t encoder;
static uint16_t octets_per_frame;
static uint16_t seq_num;

static void tx_lock_take(void)
{
	__maybe_unused int err = k_mutex_lock(&tx_lock, K_FOREVER);

	__ASSERT_NO_MSG(err == 0);
}

static void tx_lock_give(void)
{
	__maybe_unused int err = k_mutex_unlock(&tx_lock);

	__ASSERT_NO_MSG(err == 0);
}

static void capture_send(const int16_t *samples)
{
	NET_BUF_POOL_FIXED_DEFINE(tx_pool, CONFIG_BT_ISO_TX_BUF_COUNT,
				  BT_ISO_SDU_BUF_SIZE(AUDIO_CAPTURE_MAX_OCTETS),
				  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
	const size_t channel = IS_ENABLED(CONFIG_SAMPLE_TMAP_AUDIO_CAPTURE_LEFT) ? 0U : 1U;
	struct net_buf *buf;
	int err;

	buf = net_buf_alloc(&tx_pool, K_NO_WAIT);
	if (buf == NULL) {
		LOG_WRN_RATELIMIT("No ISO TX buffer; dropping microphone frame");
		seq_num++;
		return;
	}

	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	err = lc3_encode(encoder, LC3_PCM_FORMAT_S16, &samples[channel], CAPTURE_CHANNELS,
			 octets_per_frame, net_buf_add(buf, octets_per_frame));
	if (err == 0) {
		err = bt_bap_stream_send(tx_stream, buf, seq_num);
	}
	seq_num++;
	if (err != 0) {
		net_buf_unref(buf);
		LOG_WRN_RATELIMIT("Microphone LC3/ISO TX failed: %d", err);
	}
}

static void capture_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		void *block;
		size_t size;
		int err = i2s_read(i2s_rx, &block, &size);

		if (err != 0) {
			LOG_ERR_RATELIMIT("I2S capture failed: %d", err);
			if (err == -EIO) {
				err = i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_PREPARE);
				if (err == 0) {
					err = i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START);
				}
				if (err != 0) {
					LOG_ERR_RATELIMIT("I2S capture recovery failed: %d", err);
				}
			}
			k_msleep(10);
			continue;
		}
		if (size == CAPTURE_BLOCK_SIZE) {
			tx_lock_take();
			if (tx_stream != NULL) {
				capture_send(block);
			}
			tx_lock_give();
		} else {
			LOG_ERR("Unexpected capture block size: %zu", size);
		}
		k_mem_slab_free(&capture_slab, block);
	}
}

int audio_capture_init(void)
{
	static K_THREAD_STACK_DEFINE(capture_stack, 8192);
	static struct k_thread capture_thread;
	const struct sample_bt_audio_pcm_sink_cfg sink_cfg = {
		.sample_rate_hz = CAPTURE_RATE_HZ,
		.frame_duration_us = 10000U,
		.channels = CAPTURE_CHANNELS,
		.bits = 16U,
		.block_bytes = CAPTURE_BLOCK_SIZE,
	};
	const struct i2s_config rx_cfg = {
		.word_size = 16U,
		.channels = CAPTURE_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET,
		.frame_clk_freq = CAPTURE_RATE_HZ,
		.mem_slab = &capture_slab,
		.block_size = CAPTURE_BLOCK_SIZE,
		.timeout = 1000,
	};
	int err;

	if (!device_is_ready(i2s_rx)) {
		return -ENODEV;
	}

	/* Set the shared codec format once, including for a source-only connection. */
	err = sample_bt_audio_pcm_sink_configure(&sink_cfg);
	if (err != 0) {
		return err;
	}
	err = i2s_configure(i2s_rx, I2S_DIR_RX, &rx_cfg);
	if (err != 0) {
		return err;
	}
	err = i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START);
	if (err != 0) {
		return err;
	}

	k_thread_create(&capture_thread, capture_stack, K_THREAD_STACK_SIZEOF(capture_stack),
			capture_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	return 0;
}

int audio_capture_configure(const struct bt_audio_codec_cfg *codec_cfg)
{
	static lc3_encoder_mem_48k_t encoder_memory;
	enum bt_audio_location allocation;
	int frequency = bt_audio_codec_cfg_freq_to_freq_hz(bt_audio_codec_cfg_get_freq(codec_cfg));
	int octets = bt_audio_codec_cfg_get_octets_per_frame(codec_cfg);

	if (codec_cfg->id != BT_HCI_CODING_FORMAT_LC3 || frequency != 32000 ||
	    bt_audio_codec_cfg_get_frame_dur(codec_cfg) != BT_AUDIO_CODEC_CFG_DURATION_10 ||
	    bt_audio_codec_cfg_get_frame_blocks_per_sdu(codec_cfg, true) != 1 ||
	    bt_audio_codec_cfg_get_chan_allocation(codec_cfg, &allocation, true) != 0 ||
	    bt_audio_get_chan_count(allocation) != 1U ||
	    octets < 30 || octets > AUDIO_CAPTURE_MAX_OCTETS) {
		return -ENOTSUP;
	}

	tx_lock_take();
	if (tx_stream != NULL) {
		tx_lock_give();
		return -EBUSY;
	}
	encoder = lc3_setup_encoder(10000, frequency, CAPTURE_RATE_HZ, &encoder_memory);
	if (encoder == NULL) {
		tx_lock_give();
		return -EINVAL;
	}
	octets_per_frame = (uint16_t)octets;
	tx_lock_give();
	return 0;
}

void audio_capture_start(struct bt_bap_stream *stream)
{
	tx_lock_take();
	if (tx_stream != NULL) {
		LOG_ERR("Microphone source is already active");
		tx_lock_give();
		return;
	}
	if (encoder != NULL && stream->qos != NULL && stream->qos->interval == 10000U &&
	    stream->qos->sdu >= octets_per_frame) {
		tx_stream = stream;
		seq_num = 0U;
	} else {
		LOG_ERR("Invalid microphone codec or QoS configuration");
	}
	tx_lock_give();
}

void audio_capture_stop(struct bt_bap_stream *stream)
{
	tx_lock_take();
	if (tx_stream == stream) {
		tx_stream = NULL;
	}
	tx_lock_give();
}
