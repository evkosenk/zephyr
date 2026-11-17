.. zephyr:code-sample:: ble_peripheral_tmap_peripheral
   :name: Telephone and Media Audio Profile (TMAP) Peripheral
   :relevant-api: bluetooth bt_audio bt_bap bt_csip bt_mcc bt_tbs bt_tmap bt_vcp

   Implement the TMAP Call Terminal (CT) and Unicast Media Receiver (UMR) roles.

Overview
********

Application demonstrating the TMAP peripheral functionality. Implements the CT and UMR roles.

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

Use the :zephyr:code-sample:`ble_peripheral_tmap_central` sample on another
board to act as the TMAP Central (CG and UMS roles).

See :zephyr:code-sample-category:`bluetooth` samples for details.

MIMXRT1170-EVKB playback
=======================

The revision B Cortex-M7 configuration receives 48 kHz, 10 ms LC3 frames from a
Silicon Labs NCP on LPUART2 at 921600 baud with RTS/CTS. Decoded mono audio is
duplicated to both channels of the WM8962 headphone output over SAI1. The codec
provides the bit and frame clocks. Call and media control timers are disabled so
the phone controls playback.

Enable the ``liblc3`` west module and build with the UART transport snippet::

   west build -b mimxrt1170_evk@B/mimxrt1176/cm7 -S silabs-ncp-uart \
     samples/bluetooth/audio/tmap_peripheral

No external devicetree overlay is required. This configuration supports one
received LC3 frame per SDU; it does not implement microphone capture.
