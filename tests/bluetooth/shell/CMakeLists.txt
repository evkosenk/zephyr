# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bluetooth_shell)

zephyr_library_include_directories(${ZEPHYR_BASE}/samples/bluetooth)

target_sources(app PRIVATE src/main.c)

if(CONFIG_TEST_BT_SHELL_TMAP_AUDIO)
  add_subdirectory("${ZEPHYR_BASE}/samples/bluetooth/audio/common"
                   "${CMAKE_BINARY_DIR}/bt_audio_common")
  target_include_directories(app PRIVATE
    "${ZEPHYR_BASE}/samples/bluetooth/audio/tmap_peripheral/src"
    "${ZEPHYR_BASE}/subsys/bluetooth/audio/shell"
  )
  target_sources(app PRIVATE
    src/tmap_audio.c
    "${ZEPHYR_BASE}/samples/bluetooth/audio/tmap_peripheral/src/audio_capture.c"
  )
endif()
