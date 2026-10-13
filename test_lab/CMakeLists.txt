# SPDX-License-Identifier: Apache-2.0
# Aevum headless test lab.
#
# Links the SEALED v1.0 engine sources (../native/src) unmodified and builds the
# multi-process LMKD/reattach simulator in two flavours:
#   lmkd_simulator          - Release (the flavour that produces the proof numbers)
#   lmkd_simulator_asan     - ASan + UBSan (correctness under the same workload)
# plus focused single-process engine checks (C ABI bounds validation, errno in
# the fault handler) in the same two flavours:
#   engine_checks / engine_checks_asan
#
# Standalone use:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#   ./build/lmkd_simulator <outdir>
cmake_minimum_required(VERSION 3.22)
project(aevum_test_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(AEVUM_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/../native)

# The sealed engine. These files are NOT modified by the lab; they are compiled
# exactly as shipped.
set(AEVUM_CORE_SRC
    ${AEVUM_NATIVE}/src/allocator.cpp
    ${AEVUM_NATIVE}/src/arena.cpp
    ${AEVUM_NATIVE}/src/dirty_tracker.cpp
    ${AEVUM_NATIVE}/src/safepoint.cpp
    ${AEVUM_NATIVE}/src/compactor.cpp
    ${AEVUM_NATIVE}/src/profiler.cpp
)

set(LAB_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lmkd_simulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/trace_emitter.cpp
)

set(CHECKS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine_checks.cpp
    ${AEVUM_NATIVE}/src/aevum_c.cpp
)

find_package(Threads REQUIRED)

# aevum_lab_target(<name> <sources...>): one lab binary over the core engine.
function(aevum_lab_target name)
  add_executable(${name} ${ARGN} ${AEVUM_CORE_SRC})
  target_include_directories(${name} PRIVATE
      ${AEVUM_NATIVE}/src
      ${AEVUM_NATIVE}/include
      ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  target_link_libraries(${name} PRIVATE Threads::Threads)
  # LSE + hardware CRC where the host is ARM64 (matches the Android target).
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(${name} PRIVATE -march=armv8-a+crc+lse)
  endif()
endfunction()

# Release: the flavour whose numbers go into the proof report.
aevum_lab_target(lmkd_simulator ${LAB_SRC})
target_compile_options(lmkd_simulator PRIVATE -O2)

# ASan + UBSan: same multi-process workload, memory/UB checked.
aevum_lab_target(lmkd_simulator_asan ${LAB_SRC})
target_compile_options(lmkd_simulator_asan PRIVATE
    -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined)
target_link_options(lmkd_simulator_asan PRIVATE -fsanitize=address,undefined)

# Engine checks, same two flavours.
aevum_lab_target(engine_checks ${CHECKS_SRC})
target_compile_options(engine_checks PRIVATE -O2)

aevum_lab_target(engine_checks_asan ${CHECKS_SRC})
target_compile_options(engine_checks_asan PRIVATE
    -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined)
target_link_options(engine_checks_asan PRIVATE -fsanitize=address,undefined)
