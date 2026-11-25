Bluetooth: Telephone and Media Audio Profile Shell
##################################################

This document describes how to run the Telephone and Media Audio Profile functionality.
Unlike most other low-layer profiles, TMAP is a profile that exists and has a service (TMAS) on all
devices. Thus both the initiator and acceptor (or central and peripheral) should do a discovery of
the remote device's TMAS to see what TMAP roles they support.

Using the TMAP Shell
********************

When the Bluetooth stack has been initialized (:code:`bt init`), the TMAS can be registered by
calling :code:`tmap init`.

.. code-block:: console


   tmap --help
   tmap - Bluetooth TMAP shell commands
   Subcommands:
     init          :Initialize and register the TMAS
     discover      :Discover TMAS on remote device

Board audio input and output
***************************

The Bluetooth shell test application can route a peripheral's unicast sink to an I2S codec
and transmit microphone audio on its unicast source. This is an optional application backend:
the ordinary BAP sine-wave, USB, and decode-only paths remain unchanged when it is not built in.

For the RT1170 EVKB Cortex-M7, enable the ``liblc3`` module and build::

   west update liblc3
   west build -b mimxrt1170_evk@B/mimxrt1176/cm7 tests/bluetooth/shell -- \
     -DCONF_FILE="prj.conf;tmap-audio.conf" \
     -DEXTRA_DTC_OVERLAY_FILE=tmap-audio-rt1170.overlay

This retains the board's Bluetooth controller and UART configuration. The controller and its firmware
must support LE isochronous channels. The audio overlay selects SDRAM instead of the shell
application's DTCM region for DMA-compatible audio buffers.

Initialize the hardware before registering BAP so that the PAC records describe the hardware's
supported formats. With this configuration, ``tmap init`` also initializes playback and capture:

.. code-block:: console

   uart:~$ bt init
   uart:~$ tmap init
   uart:~$ bap init 1 1
   uart:~$ cap_acceptor init
   uart:~$ vcp_vol_rend init
   uart:~$ bap set_location sink 3
   uart:~$ bt adv-create conn-nscan ext-adv
   uart:~$ bt adv-data dev-name discov
   uart:~$ bt adv-start

Pair a compatible LE Audio central and start playback or a call. Accepted sink streams play
through the WM8962 headphone output; source streams send the selected microphone channel.
VCP volume and mute changes are applied to the output codec. The codec shares fixed 48 kHz clocks
between playback and capture. Sink streams support mono or stereo 16/32/48 kHz LC3; source streams
support mono 32 kHz LC3. Both directions use 10 ms frames, one frame block per SDU, and
30 to 155 octets per codec frame.

The overlay's ``zephyr,user`` property ``audio-capture-channel`` selects the microphone:
``1`` is the onboard right-channel microphone; ``0`` selects the left channel for an OMTP-wired
J101 headset microphone. If the property is absent, the capture helper uses
:kconfig:option:`CONFIG_SAMPLE_TMAP_AUDIO_CAPTURE_LEFT`.

No additional shell commands are needed. Streams are configured and stopped through BAP, CAP,
or the central. The ADC and shared codec clocks remain active after initialization.
Hardware-owned streams exclude ``bap send`` and ``bap start_sine`` to avoid competing transmitters.
Rebuild without the audio configuration to use the ordinary shell data paths.
