/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/ztest.h>

#include <audio_capture.h>
#include <pcm_sink.h>

/* Codec negotiation tests do not start the capture hardware. */
int sample_bt_audio_pcm_sink_configure(const struct sample_bt_audio_pcm_sink_cfg *cfg)
{
	ARG_UNUSED(cfg);
	return 0;
}

static struct bt_audio_codec_cfg capture_cfg(void)
{
	const struct bt_audio_codec_cfg cfg = BT_AUDIO_CODEC_LC3_CONFIG(
		BT_AUDIO_CODEC_CFG_FREQ_32KHZ, BT_AUDIO_CODEC_CFG_DURATION_10,
		BT_AUDIO_LOCATION_FRONT_RIGHT, 80U, 1U, BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);

	return cfg;
}

ZTEST(tmap_capture, test_mono_configuration)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	zassert_ok(audio_capture_configure(&cfg));
	zassert_true(bt_audio_codec_cfg_set_chan_allocation(
		&cfg, BT_AUDIO_LOCATION_MONO_AUDIO) > 0);
	zassert_ok(audio_capture_configure(&cfg));
}

ZTEST(tmap_capture, test_reject_stereo)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	zassert_true(bt_audio_codec_cfg_set_chan_allocation(
		&cfg, BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT) > 0);
	zassert_equal(audio_capture_configure(&cfg), -ENOTSUP);
}

ZTEST(tmap_capture, test_default_mono_allocation)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	zassert_true(bt_audio_codec_cfg_unset_val(&cfg, BT_AUDIO_CODEC_CFG_CHAN_ALLOC) >= 0);
	zassert_ok(audio_capture_configure(&cfg));
}

ZTEST(tmap_capture, test_reject_codec)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	cfg.id = BT_HCI_CODING_FORMAT_CVSD;
	zassert_equal(audio_capture_configure(&cfg), -ENOTSUP);
}

ZTEST(tmap_capture, test_reject_frequency)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	zassert_true(bt_audio_codec_cfg_set_freq(&cfg, BT_AUDIO_CODEC_CFG_FREQ_48KHZ) > 0);
	zassert_equal(audio_capture_configure(&cfg), -ENOTSUP);
}

ZTEST(tmap_capture, test_reject_duration)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	zassert_true(bt_audio_codec_cfg_set_frame_dur(&cfg, BT_AUDIO_CODEC_CFG_DURATION_7_5) > 0);
	zassert_equal(audio_capture_configure(&cfg), -ENOTSUP);
}

ZTEST(tmap_capture, test_reject_frame_blocks)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	zassert_true(bt_audio_codec_cfg_set_frame_blocks_per_sdu(&cfg, 2U) > 0);
	zassert_equal(audio_capture_configure(&cfg), -ENOTSUP);
}

ZTEST(tmap_capture, test_frame_size_bounds)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();

	zassert_true(bt_audio_codec_cfg_set_octets_per_frame(&cfg, 29U) > 0);
	zassert_equal(audio_capture_configure(&cfg), -ENOTSUP);
	zassert_true(bt_audio_codec_cfg_set_octets_per_frame(&cfg, 156U) > 0);
	zassert_equal(audio_capture_configure(&cfg), -ENOTSUP);
	zassert_true(bt_audio_codec_cfg_set_octets_per_frame(&cfg, 30U) > 0);
	zassert_ok(audio_capture_configure(&cfg));
	zassert_true(bt_audio_codec_cfg_set_octets_per_frame(&cfg, 155U) > 0);
	zassert_ok(audio_capture_configure(&cfg));
}

ZTEST(tmap_capture, test_reconfigure_after_stop)
{
	struct bt_audio_codec_cfg cfg = capture_cfg();
	struct bt_bap_qos_cfg qos = {.interval = 10000U, .sdu = 80U};
	struct bt_bap_stream stream = {.qos = &qos};

	zassert_ok(audio_capture_configure(&cfg));
	audio_capture_start(&stream);
	zassert_equal(audio_capture_configure(&cfg), -EBUSY);
	audio_capture_stop(&stream);
	zassert_ok(audio_capture_configure(&cfg));
}

ZTEST_SUITE(tmap_capture, NULL, NULL, NULL, NULL, NULL);
