# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.13)

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(rp2350_hello_pio C CXX ASM)
pico_sdk_init()

include("${PICO_EXAMPLES_PATH}/example_auto_set_url.cmake")
add_subdirectory("${PICO_EXAMPLES_PATH}/pio/hello_pio" hello_pio)
target_sources(hello_pio PRIVATE reserve-pio.c)
