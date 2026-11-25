cmake_minimum_required(VERSION 3.16)
project(stream_bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(sb_gst INTERFACE)
if(WIN32)
  # Official GStreamer MSVC install (1.28 default: C:\Program Files\gstreamer\1.0\msvc_x86_64).
  # Its .pc files emit paths with spaces, so link explicitly instead of via pkg-config.
  if(NOT GSTREAMER_ROOT)
    if(DEFINED ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64})
      set(GSTREAMER_ROOT "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}")
    else()
      set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64")
    endif()
  endif()
  file(TO_CMAKE_PATH "${GSTREAMER_ROOT}" GSTREAMER_ROOT)
  message(STATUS "GStreamer root: ${GSTREAMER_ROOT}")
  target_include_directories(sb_gst INTERFACE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    "${GSTREAMER_ROOT}/include")
  foreach(lib gstreamer-1.0 gstbase-1.0 gstapp-1.0 gstvideo-1.0 gstrtp-1.0 gobject-2.0 glib-2.0)
    find_library(SB_LIB_${lib} NAMES ${lib} PATHS "${GSTREAMER_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(sb_gst INTERFACE "${SB_LIB_${lib}}")
  endforeach()
  target_link_libraries(sb_gst INTERFACE ws2_32 winmm)
  target_compile_definitions(sb_gst INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtp-1.0)
  find_package(Threads REQUIRED)
  target_link_libraries(sb_gst INTERFACE PkgConfig::GST Threads::Threads)
endif()

if(MSVC)
  add_compile_options(/O2 /utf-8 /W3 /wd4244 /wd4267)
else()
  add_compile_options(-O2 -Wall -Wno-unused-parameter)
endif()

add_library(sb_common STATIC
  src/common/stamp.cpp
  src/common/pattern.cpp
  src/common/util.cpp
  src/common/canon.cpp)
target_link_libraries(sb_common PUBLIC sb_gst)

add_executable(sb_sender src/sender/main.cpp src/sender/sources.cpp src/sender/preview_win.cpp)
target_link_libraries(sb_sender PRIVATE sb_common)

add_executable(sb_receiver src/receiver/main.cpp)
target_link_libraries(sb_receiver PRIVATE sb_common)

add_executable(sb_selftest tools/selftest.cpp)
target_link_libraries(sb_selftest PRIVATE sb_common)

if(WIN32)
  add_executable(sb_vdisplay tools/vdisplay.cpp)
  target_link_libraries(sb_vdisplay PRIVATE setupapi user32)
endif()
