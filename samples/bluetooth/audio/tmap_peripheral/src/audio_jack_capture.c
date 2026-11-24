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
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "audio_jack_capture.h"

LOG_MODULE_REGISTER(audio_jack_capture, LOG_LEVEL_INF);

#define I2S_RX_NODE DT_ALIAS(i2s_codec_rx)
#define SAMPLE_RATE_HZ 48000U
#define SAMPLE_COUNT (SAMPLE_RATE_HZ / 100U)
#define CHANNEL_COUNT 2U
#define CAPTURE_BLOCK_SIZE (SAMPLE_COUNT * CHANNEL_COUNT * sizeof(int16_t))
#define CAPTURE_BLOCK_COUNT 4U
#define TX_FRAME_SAMPLES (SAMPLE_RATE_HZ / 100U)

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(capture_slab, __nocache, CAPTURE_BLOCK_SIZE,
				 CAPTURE_BLOCK_COUNT, 4);
K_THREAD_STACK_DEFINE(capture_stack, 8192);
static struct k_thread capture_thread;
static const struct device *const i2s_rx = DEVICE_DT_GET(I2S_RX_NODE);
static struct k_mutex tx_lock;
static struct bt_bap_stream *tx_stream;
static lc3_encoder_mem_48k_t encoder_memory;
static lc3_encoder_t encoder;
static uint16_t octets_per_frame;
static uint16_t seq_num;
static atomic_t completed_frames;
NET_BUF_POOL_FIXED_DEFINE(capture_tx_pool, CONFIG_BT_ISO_TX_BUF_COUNT,
			  BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static void capture_send(const int16_t *samples)
{
	static int16_t pcm[TX_FRAME_SAMPLES];
	const size_t mic_channel = IS_ENABLED(CONFIG_TMAP_PERIPHERAL_AUDIO_JACK_MIC_HEADSET) ? 0U : 1U;
	struct net_buf *buf;
	int err;

	for (size_t i = 0U; i < ARRAY_SIZE(pcm); i++) {
		pcm[i] = samples[i * CHANNEL_COUNT + mic_channel];
	}

	buf = net_buf_alloc(&capture_tx_pool, K_NO_WAIT);
	if (buf == NULL) {
		LOG_WRN_RATELIMIT("No ISO TX buffer; dropping microphone frame");
		seq_num++;
		return;
	}

	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	err = lc3_encode(encoder, LC3_PCM_FORMAT_S16, pcm, 1U, octets_per_frame,
			 net_buf_add(buf, octets_per_frame));
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
	uint32_t blocks = 0U;
	uint32_t left_peak = 0U;
	uint32_t right_peak = 0U;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		void *block;
		size_t size;
		int err = i2s_read(i2s_rx, &block, &size);

		if (err != 0) {
			LOG_ERR("I2S RX read failed: %d", err);
			k_sleep(K_MSEC(100));
			continue;
		}
		if (size != CAPTURE_BLOCK_SIZE) {
			LOG_ERR("Unexpected capture block size: %zu", size);
			(void)k_mem_slab_free(&capture_slab, block);
			continue;
		}

		const int16_t *samples = block;

		for (size_t i = 0U; i < SAMPLE_COUNT; i++) {
			int32_t left = samples[i * CHANNEL_COUNT];
			int32_t right = samples[i * CHANNEL_COUNT + 1U];
			uint32_t left_magnitude = (uint32_t)(left < 0 ? -left : left);
			uint32_t right_magnitude = (uint32_t)(right < 0 ? -right : right);

			left_peak = MAX(left_peak, left_magnitude);
			right_peak = MAX(right_peak, right_magnitude);
		}
		k_mutex_lock(&tx_lock, K_FOREVER);
		if (tx_stream != NULL && encoder != NULL) {
			capture_send(samples);
		}
		k_mutex_unlock(&tx_lock);
		(void)k_mem_slab_free(&capture_slab, block);

		blocks++;
		if ((blocks % 100U) == 0U) {
			LOG_INF("WM8962 capture peak: left %u, onboard-mic-right %u",
				left_peak, right_peak);
			left_peak = 0U;
			right_peak = 0U;
		}
	}
}

int audio_jack_capture_init(void)
{
	struct i2s_config config = {0};
	int err;

	if (!device_is_ready(i2s_rx)) {
		LOG_ERR("SAI1 RX is not ready");
		return -ENODEV;
	}

	config.word_size = 16U;
	config.channels = CHANNEL_COUNT;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
	config.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET;
	config.frame_clk_freq = SAMPLE_RATE_HZ;
	config.mem_slab = &capture_slab;
	config.block_size = CAPTURE_BLOCK_SIZE;
	config.timeout = 1000;
	k_mutex_init(&tx_lock);
	err = i2s_configure(i2s_rx, I2S_DIR_RX, &config);
	if (err != 0) {
		LOG_ERR("SAI1 RX configuration failed: %d", err);
		return err;
	}

	err = i2s_trigger(i2s_rx, I2S_DIR_RX, I2S_TRIGGER_START);
	if (err != 0) {
		LOG_ERR("SAI1 RX start failed: %d", err);
		return err;
	}

	k_thread_create(&capture_thread, capture_stack, K_THREAD_STACK_SIZEOF(capture_stack),
			capture_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&capture_thread, "wm8962_capture");
	LOG_INF("WM8962 capture started: 48 kHz stereo; onboard mic is right channel");

	return 0;
}

int audio_jack_capture_configure(const struct bt_audio_codec_cfg *codec_cfg)
{
	int frequency = bt_audio_codec_cfg_freq_to_freq_hz(bt_audio_codec_cfg_get_freq(codec_cfg));
	int octets = bt_audio_codec_cfg_get_octets_per_frame(codec_cfg);

	if (codec_cfg->id != BT_HCI_CODING_FORMAT_LC3 ||
	    bt_audio_codec_cfg_get_frame_dur(codec_cfg) != BT_AUDIO_CODEC_CFG_DURATION_10 ||
	    bt_audio_codec_cfg_get_frame_blocks_per_sdu(codec_cfg, true) != 1 ||
	    frequency != 32000 || octets < 30 || octets > MIN(155, CONFIG_BT_ISO_TX_MTU)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);
	if (tx_stream != NULL) {
		k_mutex_unlock(&tx_lock);
		return -EBUSY;
	}
	encoder = lc3_setup_encoder(10000U, frequency, SAMPLE_RATE_HZ, &encoder_memory);
	octets_per_frame = (uint16_t)octets;
	k_mutex_unlock(&tx_lock);
	if (encoder == NULL) {
		return -EINVAL;
	}
	LOG_INF("%s LC3 source configured: %d Hz, 10 ms, %d octets",
		IS_ENABLED(CONFIG_TMAP_PERIPHERAL_AUDIO_JACK_MIC_HEADSET) ? "Headset mic" :
												"Onboard mic",
		frequency, octets);
	return 0;
}

void audio_jack_capture_start(struct bt_bap_stream *stream)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	if (encoder != NULL && stream->qos != NULL && stream->qos->interval == 10000U &&
	    stream->qos->sdu >= octets_per_frame) {
		tx_stream = stream;
		seq_num = 0U;
		atomic_set(&completed_frames, 0);
		LOG_INF("%s microphone ISO transmission started",
			IS_ENABLED(CONFIG_TMAP_PERIPHERAL_AUDIO_JACK_MIC_HEADSET) ? "Headset" :
												"Onboard");
	} else {
		LOG_ERR("Onboard microphone source has invalid codec or QoS configuration");
	}
	k_mutex_unlock(&tx_lock);
}

void audio_jack_capture_stop(struct bt_bap_stream *stream)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	if (tx_stream == stream) {
		LOG_INF("Onboard microphone ISO transmission stopped: %u frames sent",
			seq_num);
		tx_stream = NULL;
	}
	k_mutex_unlock(&tx_lock);
}

void audio_jack_capture_sent(struct bt_bap_stream *stream)
{
	ARG_UNUSED(stream);
	atomic_inc(&completed_frames);
}
