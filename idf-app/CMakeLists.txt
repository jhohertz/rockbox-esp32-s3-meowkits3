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
