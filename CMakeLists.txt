# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(PROJECT_VER "4.10.2")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-attributes")
# TinyUSB HID control buffer (default 64): the Precision Touchpad's
# certification blob is a 256-byte feature report (+1 report ID byte).
add_compile_options("-DCFG_TUD_HID_EP_BUFSIZE=320")
project(ESDeck)
