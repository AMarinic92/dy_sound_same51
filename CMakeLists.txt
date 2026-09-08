cmake_minimum_required(VERSION 3.16)

project(dy_sound_same51 LANGUAGES C)

option(DY_SOUND_ENABLE_FREERTOS "Build the FreeRTOS task layer" OFF)
option(DY_SOUND_LABELS "Keep the bank's label strings in the image" ON)

add_library(dy_sound STATIC
    src/dy_sound.c
    src/dy_sound_bank.c
    src/dy_sound_freertos.c)

target_include_directories(dy_sound PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

if(DY_SOUND_ENABLE_FREERTOS)
    target_compile_definitions(dy_sound PUBLIC DY_SOUND_ENABLE_FREERTOS)
endif()

if(NOT DY_SOUND_LABELS)
    target_compile_definitions(dy_sound PUBLIC DY_SOUND_LABELS=0)
endif()

# This target does not build standalone. It needs device.h and the device pack
# headers, which only exist inside a consuming MPLAB project, plus the FreeRTOS
# headers when the task layer is on. Supply them with:
#
#   target_include_directories(dy_sound PRIVATE <path-to>/src/config/default ...)
#
# In MPLAB X projects whose file set globs the repo, you do not need this file
# at all - the sources are picked up and compiled with the project's own flags.
# See README.md.
