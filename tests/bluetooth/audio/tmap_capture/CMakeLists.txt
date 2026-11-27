# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tmap_capture)

target_include_directories(app PRIVATE
  "${ZEPHYR_BASE}/samples/bluetooth/audio/common/include"
  "${ZEPHYR_BASE}/samples/bluetooth/audio/tmap_peripheral/src"
)
target_sources(app PRIVATE
  src/main.c
  "${ZEPHYR_BASE}/samples/bluetooth/audio/tmap_peripheral/src/audio_capture.c"
)
