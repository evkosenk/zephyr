.. zephyr:code-sample:: ble_peripheral_tmap_peripheral
   :name: Telephone and Media Audio Profile (TMAP) Peripheral
   :relevant-api: bluetooth bt_audio bt_bap bt_csip bt_mcc bt_tbs bt_tmap bt_vcp

   Implement the TMAP Call Terminal (CT) and Unicast Media Receiver (UMR) roles.

Overview
********

Application demonstrating the TMAP peripheral functionality. Implements the
Call Terminal (CT) and Unicast Media Receiver (UMR) roles. Either role can be
built on its own: disable ``CONFIG_TMAP_PERIPHERAL_ROLE_CT`` for a media-only
peripheral or ``CONFIG_TMAP_PERIPHERAL_ROLE_UMR`` for a call-only peripheral.

Requirements
************

* A board with Bluetooth Low Energy 5.2 support

Building and Running
********************

Build and flash the sample as follows, replacing ``<board>`` with your target
board (e.g. :zephyr:board:`nrf5340dk`):

.. zephyr-app-commands::
   :zephyr-app: samples/bluetooth/audio/tmap_peripheral
   :board: <board>
   :goals: build flash
   :compact:

After flashing, the sample initializes the TMAP Call Terminal (CT) and Unicast
Media Receiver (UMR) roles, then starts advertising as ``TMAP Peripheral``.
After a TMAP Central connects and security is established, the sample discovers
the peer's TMAP role: if the peer is a Call Gateway (CG), it sends an "originate call" request
and terminates it after 2 seconds; if the peer is a Unicast Media Sender (UMS),
it sends a "play media" request and pauses after 2 seconds.

This automatic control demo runs only when ``CONFIG_TMAP_PERIPHERAL_AUTO_CTRL``
is enabled (the default). Disable it to keep the peripheral a passive sink that
waits for the Central to drive call and media control.

Use the :zephyr:code-sample:`ble_peripheral_tmap_central` sample on another
board to act as the TMAP Central (CG and UMS roles).

Playback to an audio codec
**************************

The sample can decode LC3 audio and play it out to a hardware codec over I2S.
Playback is optional: when the required devicetree nodes or Kconfig symbols
are absent, ``audio_playback.c`` is left out of the build and the sample's
``IS_ENABLED(CONFIG_SAMPLE_BT_AUDIO_PLAYBACK)``-guarded calls compile away, so
the sample still runs as a UMR that acknowledges the stream but discards the
audio.

To enable playback on a new board, provide the following in a board overlay
(or board :file:`.dts`):

* an ``i2s-codec-tx`` DT alias pointing at the I2S peripheral wired to the codec
* a devicetree node labelled ``audio_codec`` implementing the Zephyr
  :ref:`audio_codec_api`

and add a matching board :file:`.conf` under :file:`boards/` enabling:

.. code-block:: cfg

   CONFIG_LIBLC3=y
   CONFIG_FPU=y
   CONFIG_I2S=y
   CONFIG_AUDIO=y
   CONFIG_AUDIO_CODEC=y

The shared playback helper (``CONFIG_SAMPLE_BT_AUDIO_PLAYBACK``) is enabled
automatically once those dependencies are met.

On boards without an audio codec, the LC3 decode path can still be exercised by
setting ``CONFIG_LIBLC3=y``, ``CONFIG_FPU=y``,
``CONFIG_SAMPLE_BT_AUDIO_PLAYBACK=y`` and
``CONFIG_SAMPLE_BT_AUDIO_PLAYBACK_SINK_NONE=y``. The audio is then decoded and
the resulting PCM discarded.

For interoperability with commercial smartphone LE Audio sources, you may also
set ``CONFIG_TMAP_PERIPHERAL_STEREO=y`` (advertise a stereo speaker location)
and ``CONFIG_TMAP_PERIPHERAL_AUTO_CTRL=n`` (stay a passive sink instead of
running the CT/MCC control demo after discovery) in your board :file:`.conf` or
in :file:`prj.conf`.

When the peer adjusts volume through the Volume Control Service (VCS), the
sample maps the 0..255 VCS volume linearly onto the codec output volume and
applies mute. Codec volume registers are device-specific, so set
``CONFIG_SAMPLE_BT_AUDIO_PLAYBACK_CODEC_VOL_MIN`` and
``CONFIG_SAMPLE_BT_AUDIO_PLAYBACK_CODEC_VOL_MAX`` to your codec's range in the
board :file:`.conf` (for example ``0`` and ``127`` for the WM8962 register).

RT1170 audio input and output
****************************

The revision B Cortex-M7 configuration of :zephyr:board:`mimxrt1170_evk` uses the WM8962
codec for headphone playback and microphone capture. It retains the board's Bluetooth
controller configuration; the controller and its firmware must support LE isochronous channels.

Enable the ``liblc3`` west module and build::

   west update liblc3
   west build -b mimxrt1170_evk@B/mimxrt1176/cm7 samples/bluetooth/audio/tmap_peripheral

The codec supplies the bit and frame clocks. Both I2S directions use 48 kHz stereo PCM,
including when only the microphone source is streaming. Incoming mono LC3 is duplicated
to both headphone channels; stereo is played directly. Playback accepts 16, 32, and 48 kHz
LC3 with 10 ms frames and upsamples lower rates to 48 kHz without changing the capture clocks.
The microphone source accepts 32 kHz mono LC3 with 10 ms frames. Both directions support
one frame block per SDU and 30 to 155 octets per LC3 frame.

The onboard microphone is on the right capture channel and is selected by default.
For the J101 headset microphone, select ``CONFIG_SAMPLE_TMAP_AUDIO_CAPTURE_LEFT`` and
use an OMTP-wired headset. Disable ``CONFIG_SAMPLE_TMAP_AUDIO_CAPTURE`` and
``CONFIG_SAMPLE_BT_AUDIO_PLAYBACK_FULL_DUPLEX`` for playback without capture.
The automatic call/media control demo is disabled so the peer controls streaming.

The codec is configured once at boot and capture continues between connections,
discarding PCM when no source stream is active. Playback start, stop, and codec
reconfiguration do not reprogram the shared audio interface.

See :zephyr:code-sample-category:`bluetooth` samples for details.
