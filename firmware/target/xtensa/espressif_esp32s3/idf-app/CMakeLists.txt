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

# ---- link ordering ----
#
# IDF puts every component archive on the link line once, in dependency order,
# and does not group them. The escape hatch exists but is broken: project.cmake
# 815 adds -Wl,--start-group under __PROJECT_GROUP_LINK_COMPONENTS, and nothing
# in the tree ever adds the matching --end-group, so turning it on produces a
# malformed command line.
#
# That matters here because Rockbox's archives are attached to the `main`
# component, which lands them after every IDF archive. The direction is wrong:
# libfirmware.a calls gptimer_new_timer() and xthal_dcache_all_writeback(), and
# by the time ld reaches libfirmware.a it has already passed -- and discarded --
# the archives that define them. The failures look like missing drivers:
# undefined reference to `gptimer_start', `xthal_dcache_all_writeback', and so
# on, for symbols that are demonstrably present in the build.
#
# So restate the whole set inside a group at the tail of the line. Repeating an
# archive is harmless -- ld only extracts members that are still unresolved --
# and the group makes resolution order-independent in both directions, which is
# also what lets IDF's app_main() reach Rockbox's main().
#
# The component list comes from IDF's own build property rather than a literal,
# so this keeps working as the sdkconfig enables more of IDF.
include(rockbox_libs.cmake OPTIONAL RESULT_VARIABLE _rockbox_libs_included)

idf_build_get_property(_idf_components BUILD_COMPONENTS)

target_link_libraries(rockbox.elf PRIVATE "-Wl,--start-group")
if(_rockbox_libs_included)
    target_link_libraries(rockbox.elf PRIVATE ${ROCKBOX_LIBS})
endif()
foreach(_idf_component IN LISTS _idf_components)
    if(TARGET ${_idf_component})
        # BUILD_COMPONENTS also lists components that are not libraries at all --
        # bootloader and partition_table are UTILITY targets that produce images,
        # and CMake refuses to link one into an executable outright.
        get_property(_component_type TARGET ${_idf_component} PROPERTY TYPE)
        if(_component_type STREQUAL "STATIC_LIBRARY" OR
           _component_type STREQUAL "INTERFACE_LIBRARY" OR
           _component_type STREQUAL "OBJECT_LIBRARY")
            target_link_libraries(rockbox.elf PRIVATE ${_idf_component})
        endif()
    endif()
endforeach()
target_link_libraries(rockbox.elf PRIVATE "-Wl,--end-group")

# ---- ROM symbol collisions ----
#
# IDF links with esp32s3.rom.ld, which assigns about 2100 absolute symbols naming
# the mask ROM's functions. An absolute assignment in a linker script takes
# precedence over a definition in an object file, and ld does not warn: a Rockbox
# function whose name appears in that list is silently replaced by the ROM routine,
# which has a different signature and lives in a different memory domain.
#
# Fifteen names collide in this tree. Most are libc -- memcpy, memset, atoi,
# strtol, strtoul, strstr, strcasecmp, strcasestr, strncasecmp, strcat, strlcpy,
# strtok_r, rand, srand -- where the ROM version is a correct C implementation and
# the substitution is harmless. That is the reason Rockbox's own memcpy does not
# appear in the image, and why measured symbol lookups land at 0x400011xx.
#
# usb_enable is not harmless. It is Rockbox's target hook (firmware/export/usb.h),
# called by usb_init() with a bool; the ROM's usb_enable is part of the USB
# download/console path. The first boot with HAVE_USBSTACK enabled died with
# LoadProhibited / EXCVADDR 0x0c, and the disassembly of usb_init() showed
#   l32r a8, (4000297c <usb_enable>) ; callx8 a8
# i.e. the call went to the ROM, not to usb-xtensa.c.
#
# --wrap redirects every reference to usb_enable to __wrap_usb_enable, which is the
# real implementation in firmware/target/xtensa/espressif_esp32s3/usb-esp32s3.c. __real_usb_enable is
# the ROM address and is deliberately never called.
target_link_options(rockbox.elf PRIVATE "-Wl,--wrap=usb_enable")

# ---- taking over IDF's panic entry ----
#
# A hardware exception on this board went to IDF's handler, printed to UART0 (which
# Rockbox owns, so nothing arrived) and reset. The fault address was lost, because IDF
# keeps the exception frame on the stack and stores only a reset-reason hint.
#
# idf-app/panic_cdc.c defines esp_panic_handler(), which IDF also defines -- strongly,
# in esp_system/panic.c, in an object that the link pulls anyway for the watchdog
# helpers it contains. Two things make ours the one that survives:
#
#   * it is added as a source of the executable, so ld reads the object before any
#     archive. A file inside the main component would land in libmain.a, which sits
#     after libesp_system.a on the line and would lose.
#   * --allow-multiple-definition stops the duplicate from being an error, and ld
#     keeps the definition it saw first: ours.
#
# Checked after every link, not assumed: rockbox.elf must contain the string
# "*rb-panic*". If it does not, IDF's copy won and the override is dead code.
#
# The includes are spelled out because an executable target does not inherit the
# include directories of IDF's components; only the three headers this file needs.
idf_build_get_property(_idf_path IDF_PATH)

target_sources(rockbox.elf PRIVATE "${CMAKE_CURRENT_LIST_DIR}/panic_cdc.c")
target_include_directories(rockbox.elf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${_idf_path}/components/esp_system/include"
    "${_idf_path}/components/esp_system/port/include/port"
    "${_idf_path}/components/esp_system/port/include/private"
    "${_idf_path}/components/esp_common/include"
    "${_idf_path}/components/esp_rom/include"
    "${_idf_path}/components/esp_rom/esp32s3/include"
    "${_idf_path}/components/soc/esp32s3/include"
    "${_idf_path}/components/esp_hw_support/include"
    "${_idf_path}/components/esp_app_format/include"
    "${_idf_path}/components/xtensa/esp32s3/include"
    "${_idf_path}/components/xtensa/include"
    "${CMAKE_BINARY_DIR}/config")
target_link_options(rockbox.elf PRIVATE "-Wl,--allow-multiple-definition")
