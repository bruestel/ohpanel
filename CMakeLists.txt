cmake_minimum_required(VERSION 3.16.0)

# The version stamped into the app descriptor, worked out here rather than left
# to ESP-IDF's own `git describe --dirty`.
#
# The build touches dependencies.lock without changing a byte of it, and a stat
# comparison then reports the tree as modified -- so a release built from a
# clean checkout stamped itself "-dirty" and the archived image disagreed with
# its tag. Refreshing the index first compares content instead of timestamps.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} update-index -q --refresh
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE OHPANEL_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(OHPANEL_VERSION)
        # a tag v0.9.0 shows as 0.9.0, as the release files are named (tools/merge_factory.py)
        string(REGEX REPLACE "^v([0-9])" "\\1" OHPANEL_VERSION "${OHPANEL_VERSION}")
        set(PROJECT_VER "${OHPANEL_VERSION}")
        message(STATUS "Firmware version: ${PROJECT_VER}")
    endif()
endif()

# The board the firmware is for: its hardware (boards/<board>/board, a component of its own) and its
# display design (boards/<board>/nextion). One for now: the Sonoff NSPanel.
if(NOT OHPANEL_BOARD)
    set(OHPANEL_BOARD nspanel)
endif()
# appended: PlatformIO passes the project's src directory in this list too
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards/${OHPANEL_BOARD}/board)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# This string becomes project_name in the app descriptor, and the OTA handler
# refuses an image whose project_name differs from the running one -- the guard
# against flashing somebody else's firmware. Never rename it: every deployed
# panel would reject the next update and need a cable. (Renamed from "jpanel"
# once, on 2026-10-06, before any panel ran this firmware.)
project(ohpanel)

# Link-time optimisation for the firmware's own components (not ESP-IDF's: its linker fragments place code in IRAM
# by section, which LTO's renaming would undo). Split into a file per page or route group, they lost the inlining
# across functions a single file had; LTO brings it back. Fat objects: the archives keep ordinary code too, so a
# link without LTO (and the archive index) still works.
foreach(component app_config app_display app_net app_openhab app_store app_web nextion panel qrcodegen board src main)
    if(TARGET __idf_${component})
        target_compile_options(__idf_${component} PRIVATE -flto=auto -ffat-lto-objects)
    endif()
endforeach()
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE -flto=auto)
