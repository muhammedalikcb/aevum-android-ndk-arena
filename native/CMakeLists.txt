# SPDX-License-Identifier: Apache-2.0
# Aevum native engine - builds libaevum.so for Android (and the same sources
# build on host for correctness verification).
cmake_minimum_required(VERSION 3.22)
project(aevum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(aevum SHARED
    src/allocator.cpp
    src/arena.cpp
    src/dirty_tracker.cpp
    src/safepoint.cpp
    src/compactor.cpp
    src/profiler.cpp
    src/aevum_c.cpp
    jni/aevum_jni.cpp
)

target_include_directories(aevum
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# LSE atomics + hardware CRC32 are universal on Android 14/15-class ARMv8.1 SoCs
# and selected here; the software CRC path remains as a correctness fallback.
target_compile_options(aevum PRIVATE
    -O2 -fno-exceptions -fno-rtti
    -Wall -Wextra
    -fstack-protector-strong
    $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},aarch64>:-march=armv8-a+crc+lse>
)

# Fail the link if any symbol is undefined, and export only JNI_OnLoad + the C
# ABI (everything else stays hidden by the visibility preset).
target_link_options(aevum PRIVATE
    -Wl,--no-undefined
    -Wl,-z,max-page-size=16384   # 16 KB page compatibility (Android 15+ devices)
)

if(ANDROID)
    find_library(log-lib log)
    find_library(android-lib android)   # ATrace_* (Perfetto/atrace instrumentation)
    target_link_libraries(aevum ${log-lib} ${android-lib})
endif()
