# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.13)
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK checkout")
include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
project(rp2350_flash C CXX ASM)
pico_sdk_init()

add_executable(rp2350_flash flash.c)
target_link_libraries(rp2350_flash PRIVATE pico_stdlib hardware_flash)
pico_enable_stdio_uart(rp2350_flash 1)
pico_enable_stdio_usb(rp2350_flash 0)
