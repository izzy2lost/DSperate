# SPDX-License-Identifier: GPL-3.0-or-later
# NinDS: the Android frontend around the DSperate core.
cmake_minimum_required(VERSION 3.22)
project(NinDS LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The DSperate tree is this repository's root. Only the libraries are wanted:
# no tests, no desktop frontends, and RetroAchievements has no Android UI yet.
get_filename_component(DSPERATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(DSPERATE_TESTS    OFF CACHE BOOL "" FORCE)
set(DSPERATE_HEADLESS OFF CACHE BOOL "" FORCE)
set(DSPERATE_SDL      OFF CACHE BOOL "" FORCE)
set(DSPERATE_WAYLAND  OFF CACHE BOOL "" FORCE)
set(DSPERATE_CHEEVOS  OFF CACHE BOOL "" FORCE)
set(DSPERATE_NET      ON  CACHE BOOL "" FORCE)
set(DSPERATE_PGO      OFF CACHE STRING "" FORCE)
add_subdirectory(${DSPERATE_ROOT} dsperate)

add_library(ninds SHARED
  ninds_jni.cpp
  session.cpp
  presenter_gl.cpp
  audio_out.cpp
  mic_in.cpp)
target_include_directories(ninds PRIVATE ${DSPERATE_ROOT}/src)
target_compile_options(ninds PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(ninds PRIVATE
  dsperate_core dsperate_net
  android log EGL GLESv3 aaudio)
# 16 KB page devices (Android 15+) must be able to load the library. The core's
# fastmem views are laid in 4 KB pages, so session.cpp turns fastmem off there.
target_link_options(ninds PRIVATE "-Wl,-z,max-page-size=16384")
