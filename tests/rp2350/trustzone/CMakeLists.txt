# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.13)
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK checkout")
include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
project(rp2350_trustzone C CXX ASM)
pico_sdk_init()

option(RP2350_TEST_ACCESSCTRL "Exercise real ACCESSCTRL GPIO assignment" OFF)
add_executable(rp2350_trustzone main.c)
target_compile_definitions(rp2350_trustzone PRIVATE
    RP2350_TEST_ACCESSCTRL=$<BOOL:${RP2350_TEST_ACCESSCTRL}>)
target_link_libraries(rp2350_trustzone PRIVATE pico_stdlib hardware_gpio)

option(RP2350_TEST_INTEGRATION "Build the opt-in bus isolation firmware" OFF)
if(RP2350_TEST_INTEGRATION)
    foreach(test_case IN ITEMS filter bootram_read bootram_write dma)
        string(TOUPPER "${test_case}" case_macro)
        add_executable(rp2350_${test_case} integration.c)
        target_compile_definitions(rp2350_${test_case} PRIVATE TEST_${case_macro}=1)
        target_link_libraries(rp2350_${test_case} PRIVATE pico_stdlib)
    endforeach()
endif()
