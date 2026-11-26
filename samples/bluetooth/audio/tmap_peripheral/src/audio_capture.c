/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <lc3.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "audio_capture.h"

LOG_MODULE_REGISTER(audio_capture, LOG_LEVEL_INF);

#define CAPTURE_RATE_HZ 48000U
#define CAPTURE_DURATION_US 10000U
#define CAPTURE_SAMPLES (CAPTURE_RATE_HZ * CAPTURE_DURATION_US / 1000000U)
#define CAPTURE_BLOCK_SIZE (CAPTURE_SAMPLES * sizeof(int32_t))

K_MEM_SLAB_DEFINE_STATIC(capture_slab, CAPTURE_BLOCK_SIZE, 8, 4);
K_THREAD_STACK_DEFINE(capture_stack, 4096);
K_MUTEX_DEFINE(capture_mutex);
K_SEM_DEFINE(capture_wake, 0, 1);
NET_BUF_POOL_FIXED_DEFINE(capture_tx_pool, CONFIG_BT_ISO_TX_BUF_COUNT,
			  BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static const struct device *const microphone = DEVICE_DT_GET(DT_ALIAS(dmic0));
static struct bt_bap_stream *capture_stream;
static lc3_encoder_mem_48k_t encoder_memory;
static lc3_encoder_t encoder;
static uint16_t octets_per_frame;
static uint16_t seq_num;
static uint32_t generation;
static atomic_t completed_frames;
static uint32_t submitted_frames;
static uint32_t dropped_frames;
static uint32_t no_buffer_frames;

/* Called with capture_mutex held; capture and encoding never run in Bluetooth RX. */
static void capture_send(const int32_t *samples)
{
	static int16_t pcm[CAPTURE_SAMPLES];
	static uint32_t peak;
	struct net_buf *buf;
	int err;

	for (size_t i = 0U; i < ARRAY_SIZE(pcm); i++) {
		int32_t sample = samples[i] >> 16;
		uint32_t magnitude = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;

		pcm[i] = (int16_t)sample;
		peak = MAX(peak, magnitude);
	}

	buf = net_buf_alloc(&capture_tx_pool, K_NO_WAIT);
	if (buf == NULL) {
		dropped_frames++;
		no_buffer_frames++;
		seq_num++;
		goto report;
	}

	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	err = lc3_encode(encoder, LC3_PCM_FORMAT_S16, pcm, 1, octets_per_frame,
			 net_buf_add(buf, octets_per_frame));
	if (err == 0) {
		err = bt_bap_stream_send(capture_stream, buf, seq_num);
	}
	seq_num++;
	if (err != 0) {
		net_buf_unref(buf);
		dropped_frames++;
		LOG_WRN_RATELIMIT("Mic LC3 encode/ISO submission failed: %d", err);
	} else {
		submitted_frames++;
	}

report:
	if (seq_num == 1U || (seq_num < 100U && (seq_num % 10U) == 0U) ||
	    (seq_num % 100U) == 0U) {
		LOG_INF("Mic ISO TX: submitted %u, completed %u, dropped %u (no buffer %u), "
			"PCM peak %u", submitted_frames, (uint32_t)atomic_get(&completed_frames),
			dropped_frames, no_buffer_frames, peak);
		peak = 0U;
	}
}

static void capture_thread_fn(void *arg1, void *arg2, void *arg3)
{
	bool running = false;
	uint32_t capture_generation = 0U;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		void *buffer;
		size_t size;
		int err;

		k_mutex_lock(&capture_mutex, K_FOREVER);
		if (running && (capture_stream == NULL || capture_generation != generation)) {
			err = dmic_trigger(microphone, DMIC_TRIGGER_STOP);
			if (err != 0) {
				LOG_ERR("Microphone stop failed: %d", err);
			}
			running = false;
		}
		if (!running && capture_stream != NULL) {
			err = dmic_trigger(microphone, DMIC_TRIGGER_START);
			if (err != 0) {
				LOG_ERR("Microphone start failed: %d", err);
				capture_stream = NULL;
			} else {
				running = true;
				capture_generation = generation;
				LOG_INF("Microphone started: 48 kHz mono, 10 ms blocks");
			}
		}
		k_mutex_unlock(&capture_mutex);

		if (!running) {
			k_sem_take(&capture_wake, K_FOREVER);
			continue;
		}

		err = dmic_read(microphone, 0, &buffer, &size, 100);
		k_mutex_lock(&capture_mutex, K_FOREVER);
		if (err != 0) {
			if (capture_stream != NULL && capture_generation == generation) {
				LOG_ERR("Microphone read failed: %d", err);
				capture_stream = NULL;
			}
		} else {
			if (capture_stream != NULL && capture_generation == generation) {
				if (size == CAPTURE_BLOCK_SIZE) {
					capture_send(buffer);
				} else {
					LOG_ERR("Unexpected microphone block size: %zu", size);
				}
			}
			k_mem_slab_free(&capture_slab, buffer);
		}
		k_mutex_unlock(&capture_mutex);
	}
}

int audio_capture_init(void)
{
	static struct k_thread capture_thread;
	struct pcm_stream_cfg pcm_cfg = {
		.pcm_rate = CAPTURE_RATE_HZ,
		.pcm_width = 32U,
		.block_size = CAPTURE_BLOCK_SIZE,
		.mem_slab = &capture_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000U,
			.max_pdm_clk_freq = 3500000U,
			.min_pdm_clk_dc = 40U,
			.max_pdm_clk_dc = 60U,
		},
		.streams = &pcm_cfg,
		.channel = {
			.req_num_chan = 1U,
			.req_num_streams = 1U,
		},
	};
	int err;

	if (!device_is_ready(microphone)) {
		return -ENODEV;
	}
	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	err = dmic_configure(microphone, &cfg);
	if (err != 0) {
		return err;
	}
	k_thread_create(&capture_thread, capture_stack, K_THREAD_STACK_SIZEOF(capture_stack),
			capture_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&capture_thread, "le_audio_capture");
	return 0;
}

int audio_capture_configure(const struct bt_audio_codec_cfg *codec_cfg)
{
	enum bt_audio_location allocation;
	int frequency = bt_audio_codec_cfg_freq_to_freq_hz(bt_audio_codec_cfg_get_freq(codec_cfg));
	int octets = bt_audio_codec_cfg_get_octets_per_frame(codec_cfg);
	int err;

	if (codec_cfg->id != BT_HCI_CODING_FORMAT_LC3 ||
	    (frequency != 16000 && frequency != 32000 && frequency != 48000) ||
	    bt_audio_codec_cfg_get_frame_dur(codec_cfg) != BT_AUDIO_CODEC_CFG_DURATION_10 ||
	    bt_audio_codec_cfg_get_frame_blocks_per_sdu(codec_cfg, true) != 1 ||
	    octets < 30 || octets > MIN(155, CONFIG_BT_ISO_TX_MTU)) {
		return -ENOTSUP;
	}
	err = bt_audio_codec_cfg_get_chan_allocation(codec_cfg, &allocation, true);
	if (err != 0 || bt_audio_get_chan_count(allocation) != 1U) {
		return -ENOTSUP;
	}

	k_mutex_lock(&capture_mutex, K_FOREVER);
	if (capture_stream != NULL) {
		k_mutex_unlock(&capture_mutex);
		return -EBUSY;
	}
	encoder = lc3_setup_encoder(CAPTURE_DURATION_US, frequency, CAPTURE_RATE_HZ,
				    &encoder_memory);
	octets_per_frame = (uint16_t)octets;
	k_mutex_unlock(&capture_mutex);
	LOG_INF("Source LC3: %d Hz, 10 ms, %d octets", frequency, octets);
	return encoder == NULL ? -EINVAL : 0;
}

void audio_capture_start(struct bt_bap_stream *stream)
{
	k_mutex_lock(&capture_mutex, K_FOREVER);
	if (encoder != NULL && stream->qos != NULL &&
	    stream->qos->interval == CAPTURE_DURATION_US && stream->qos->sdu >= octets_per_frame) {
		capture_stream = stream;
		seq_num = 0U;
		submitted_frames = 0U;
		dropped_frames = 0U;
		no_buffer_frames = 0U;
		atomic_set(&completed_frames, 0);
		generation++;
	} else {
		LOG_ERR("Source encoder or QoS is not configured for 10 ms LC3");
	}
	k_mutex_unlock(&capture_mutex);
	k_sem_give(&capture_wake);
}

void audio_capture_stop(struct bt_bap_stream *stream)
{
	k_mutex_lock(&capture_mutex, K_FOREVER);
	if (capture_stream == stream) {
		LOG_INF("Mic stopped: submitted %u, completed %u, dropped %u", submitted_frames,
			(uint32_t)atomic_get(&completed_frames), dropped_frames);
		capture_stream = NULL;
		generation++;
	}
	k_mutex_unlock(&capture_mutex);
	k_sem_give(&capture_wake);
}

void audio_capture_sent(struct bt_bap_stream *stream)
{
	ARG_UNUSED(stream);
	atomic_inc(&completed_frames);
}
