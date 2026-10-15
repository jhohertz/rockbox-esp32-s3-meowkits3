# ESP-IDF project that links Rockbox.
#
# Rockbox's own build compiles every Rockbox source and produces archives; this
# project owns the link, the linker script, the startup and the C library. That
# division is deliberate: ESP-IDF's startup (call_start_cpu0 ->
# esp_startup_start_app -> app_main), cache/MMU bring-up, PSRAM configuration and
# FreeRTOS are exactly what the bare-metal branch had to reimplement by hand, and
# reimplementing them again here would be the whole difficulty of the port for no
# benefit.
#
# The list of Rockbox archives is not written here. tools/root.make generates
# rockbox_libs.cmake with the absolute paths of whatever the Rockbox build
# actually produced, so the two builds cannot drift.
cmake_minimum_required(VERSION 3.16)

# Keeps `idf.py build` working without a separate set-target step.
set(IDF_TARGET esp32s3)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rockbox)
