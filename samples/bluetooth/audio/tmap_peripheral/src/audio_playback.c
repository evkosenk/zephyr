/*
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <lc3.h>

#include <zephyr/audio/codec.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include "audio_playback.h"

LOG_MODULE_REGISTER(audio_playback, LOG_LEVEL_INF);

#if DT_HAS_ALIAS(i2s_codec_tx) && DT_NODE_EXISTS(DT_NODELABEL(audio_codec))

#define I2S_TX_NODE DT_ALIAS(i2s_codec_tx)
#define AUDIO_RATE_HZ 48000U
#define AUDIO_CHANNELS 2U
#define AUDIO_FRAME_DURATION_MS 10U
#define SAMPLES_PER_FRAME (AUDIO_RATE_HZ * AUDIO_FRAME_DURATION_MS / 1000U)
#define AUDIO_BLOCK_SIZE (SAMPLES_PER_FRAME * AUDIO_CHANNELS * sizeof(int16_t))
#define AUDIO_BLOCK_COUNT 16U
#define PLAYBACK_QUEUE_COUNT AUDIO_BLOCK_COUNT
#define TX_INITIAL_BLOCKS 2U
#define PLAYBACK_WAIT_MS AUDIO_FRAME_DURATION_MS

BUILD_ASSERT(SAMPLES_PER_FRAME == 480U, "48 kHz LC3 frames must contain 480 samples in 10 ms");

struct playback_block {
	void *data;
	size_t size;
};

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(audio_slab, __nocache, AUDIO_BLOCK_SIZE, AUDIO_BLOCK_COUNT, 4);
K_MSGQ_DEFINE(playback_queue, sizeof(struct playback_block), PLAYBACK_QUEUE_COUNT,
	      __alignof__(struct playback_block));
K_THREAD_STACK_DEFINE(playback_stack, 2048);
K_MUTEX_DEFINE(i2s_tx_mutex);

static const struct device *const i2s_tx = DEVICE_DT_GET(I2S_TX_NODE);
static const struct device *const audio_codec = DEVICE_DT_GET(DT_NODELABEL(audio_codec));
static struct k_thread playback_thread;
static lc3_decoder_t decoder;
static lc3_decoder_mem_48k_t decoder_memory;
static bool audio_configured;
static bool stream_active;
static bool tx_started;
static uint16_t frame_octets;

static void playback_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		struct playback_block block;
		int err = k_msgq_get(&playback_queue, &block, K_MSEC(PLAYBACK_WAIT_MS));

		if (err != 0) {
			if (!stream_active) {
				continue;
			}

			err = k_mem_slab_alloc(&audio_slab, &block.data, K_NO_WAIT);
			if (err != 0) {
				continue;
			}

			memset(block.data, 0, AUDIO_BLOCK_SIZE);
			block.size = AUDIO_BLOCK_SIZE;
		}

		k_mutex_lock(&i2s_tx_mutex, K_FOREVER);
		if (!stream_active) {
			k_mutex_unlock(&i2s_tx_mutex);
			(void)k_mem_slab_free(&audio_slab, block.data);
			continue;
		}

		if (!tx_started) {
			for (uint8_t i = 0U; i < TX_INITIAL_BLOCKS; i++) {
				void *silence_data;

				err = k_mem_slab_alloc(&audio_slab, &silence_data, K_NO_WAIT);
				if (err != 0) {
					LOG_ERR("Failed to allocate I2S priming block: %d", err);
					break;
				}

				memset(silence_data, 0, AUDIO_BLOCK_SIZE);
				err = i2s_write(i2s_tx, silence_data, AUDIO_BLOCK_SIZE);
				if (err != 0) {
					LOG_ERR("Failed to queue I2S priming block: %d", err);
					(void)k_mem_slab_free(&audio_slab, silence_data);
					break;
				}
			}

			if (err != 0) {
				(void)i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
				(void)k_mem_slab_free(&audio_slab, block.data);
				k_mutex_unlock(&i2s_tx_mutex);
				continue;
			}
		}

		err = i2s_write(i2s_tx, block.data, block.size);
		if (err != 0) {
			LOG_ERR("I2S write failed: %d", err);
			if (!tx_started) {
				(void)i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
			}
			(void)k_mem_slab_free(&audio_slab, block.data);
			k_mutex_unlock(&i2s_tx_mutex);
			continue;
		}

		if (!tx_started) {
			audio_codec_start_output(audio_codec);
			err = i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_START);
			if (err != 0) {
				LOG_ERR("I2S start failed: %d", err);
				(void)i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);
				k_mutex_unlock(&i2s_tx_mutex);
				continue;
			}
			tx_started = true;
		}
		k_mutex_unlock(&i2s_tx_mutex);
	}
}

int audio_playback_init(void)
{
	struct audio_codec_cfg codec_cfg = {0};
	struct i2s_config i2s_cfg = {0};
	int err;

	if (!device_is_ready(i2s_tx) || !device_is_ready(audio_codec)) {
		LOG_ERR("SAI or WM8962 is not ready");
		return -ENODEV;
	}

	codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
	codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	codec_cfg.dai_cfg.i2s.word_size = 16U;
	codec_cfg.dai_cfg.i2s.channels = AUDIO_CHANNELS;
	codec_cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
	codec_cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	codec_cfg.dai_cfg.i2s.frame_clk_freq = AUDIO_RATE_HZ;
	codec_cfg.dai_cfg.i2s.mem_slab = &audio_slab;
	codec_cfg.dai_cfg.i2s.block_size = AUDIO_BLOCK_SIZE;

	err = audio_codec_configure(audio_codec, &codec_cfg);
	if (err != 0) {
		LOG_ERR("WM8962 configuration failed: %d", err);
		return err;
	}
	k_msleep(1000);

	i2s_cfg.word_size = 16U;
	i2s_cfg.channels = AUDIO_CHANNELS;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET;
	i2s_cfg.frame_clk_freq = AUDIO_RATE_HZ;
	i2s_cfg.mem_slab = &audio_slab;
	i2s_cfg.block_size = AUDIO_BLOCK_SIZE;
	i2s_cfg.timeout = 1000;
	err = i2s_configure(i2s_tx, I2S_DIR_TX, &i2s_cfg);
	if (err != 0) {
		LOG_ERR("SAI configuration failed: %d", err);
		return err;
	}

	k_thread_create(&playback_thread, playback_stack, K_THREAD_STACK_SIZEOF(playback_stack),
			playback_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&playback_thread, "le_audio_playback");
	audio_configured = true;
	return 0;
}

int audio_playback_configure(const struct bt_audio_codec_cfg *codec_cfg)
{
	int frequency;
	int duration;
	int frames;
	int octets;
	enum bt_audio_location allocation;

	if (!audio_configured || codec_cfg->id != BT_HCI_CODING_FORMAT_LC3) {
		return -ENOTSUP;
	}

	frequency = bt_audio_codec_cfg_freq_to_freq_hz(bt_audio_codec_cfg_get_freq(codec_cfg));
	duration = bt_audio_codec_cfg_get_frame_dur(codec_cfg);
	frames = bt_audio_codec_cfg_get_frame_blocks_per_sdu(codec_cfg, true);
	octets = bt_audio_codec_cfg_get_octets_per_frame(codec_cfg);
	if ((frequency != 16000 && frequency != 32000 && frequency != 48000) ||
	    duration != BT_AUDIO_CODEC_CFG_DURATION_10 || frames != 1) {
		LOG_ERR("Playback supports 16/32/48 kHz, 10 ms, one-frame LC3 SDUs");
		return -ENOTSUP;
	}
	if (octets < 30 || octets > 155 ||
	    bt_audio_codec_cfg_get_chan_allocation(codec_cfg, &allocation, true) != 0 ||
	    bt_audio_get_chan_count(allocation) != 1U) {
		return -ENOTSUP;
	}

	decoder = lc3_setup_decoder(10000U, frequency, AUDIO_RATE_HZ, &decoder_memory);
	if (decoder == NULL) {
		LOG_ERR("LC3 decoder setup failed");
		return -EINVAL;
	}
	frame_octets = (uint16_t)octets;
	LOG_INF("Sink LC3: %d Hz, 10 ms, %d octets; output 48 kHz", frequency, octets);

	return 0;
}

void audio_playback_start(void)
{
	stream_active = true;
}

void audio_playback_stop(void)
{
	struct playback_block block;

	stream_active = false;
	while (k_msgq_get(&playback_queue, &block, K_NO_WAIT) == 0) {
		(void)k_mem_slab_free(&audio_slab, block.data);
	}
	k_mutex_lock(&i2s_tx_mutex, K_FOREVER);
	if (tx_started) {
		int err = i2s_trigger(i2s_tx, I2S_DIR_TX, I2S_TRIGGER_DROP);

		if (err != 0) {
			LOG_ERR("I2S stop failed: %d", err);
		}
		tx_started = false;
	}
	k_mutex_unlock(&i2s_tx_mutex);
}

void audio_playback_recv(const struct bt_iso_recv_info *info, struct net_buf *buf)
{
	static uint32_t received_frames;
	const bool valid = (info->flags & BT_ISO_FLAGS_VALID) != 0U;
	void *block_data;
	int16_t *pcm;
	int err;

	if (!stream_active || decoder == NULL) {
		return;
	}

	err = k_mem_slab_alloc(&audio_slab, &block_data, K_NO_WAIT);
	if (err != 0) {
		LOG_WRN("Playback buffer pool exhausted");
		return;
	}

	pcm = block_data;
	err = lc3_decode(decoder, valid ? buf->data : NULL, valid ? buf->len : frame_octets,
			 LC3_PCM_FORMAT_S16, pcm, 1);
	if (err < 0) {
		LOG_ERR("LC3 decode failed: %d", err);
		(void)k_mem_slab_free(&audio_slab, block_data);
		return;
	}
	received_frames++;
	if ((received_frames % 100U) == 0U) {
		LOG_INF("Sink LC3 RX: %u decoded frames", received_frames);
	}

	for (size_t i = SAMPLES_PER_FRAME; i > 0U; i--) {
		const int16_t sample = pcm[i - 1U];

		pcm[(i - 1U) * AUDIO_CHANNELS] = sample;
		pcm[(i - 1U) * AUDIO_CHANNELS + 1U] = sample;
	}

	const struct playback_block block = {
		.data = block_data,
		.size = AUDIO_BLOCK_SIZE,
	};

	err = k_msgq_put(&playback_queue, &block, K_NO_WAIT);
	if (err != 0) {
		LOG_WRN("Playback queue full");
		(void)k_mem_slab_free(&audio_slab, block_data);
	}
}

#else

int audio_playback_init(void)
{
	return 0;
}

int audio_playback_configure(const struct bt_audio_codec_cfg *codec_cfg)
{
	ARG_UNUSED(codec_cfg);
	return 0;
}

void audio_playback_start(void) {}
void audio_playback_stop(void) {}
void audio_playback_recv(const struct bt_iso_recv_info *info, struct net_buf *buf)
{
	ARG_UNUSED(info);
	ARG_UNUSED(buf);
}

#endif
