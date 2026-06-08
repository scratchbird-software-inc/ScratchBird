# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# DPC_BENCHMARK_CLEAN_FLAGS_GATE: CTest-visible cache policy gate for
# benchmark-clean instrumentation options.
set(_sb_required_flags
  SCRATCHBIRD_ENABLE_DEBUG_LOGS
  SCRATCHBIRD_ENABLE_HOTPATH_TRACE
  SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
  SCRATCHBIRD_ENABLE_PREPARED_TRACE
)

if(NOT DEFINED SB_CACHE_FILE OR NOT EXISTS "${SB_CACHE_FILE}")
  message(FATAL_ERROR "SB_CACHE_FILE is missing or does not exist: ${SB_CACHE_FILE}")
endif()

file(READ "${SB_CACHE_FILE}" _sb_cache_text)
set(_sb_failures)
set(_sb_benchmark_clean ON)

foreach(_sb_flag IN LISTS _sb_required_flags)
  if(NOT DEFINED ${_sb_flag})
    list(APPEND _sb_failures "${_sb_flag} expected value was not supplied to the gate")
    set(_sb_benchmark_clean OFF)
    continue()
  endif()

  string(REGEX MATCH "(^|\n)${_sb_flag}:([^=]+)=([^\n]*)" _sb_match "${_sb_cache_text}")
  if(NOT _sb_match)
    list(APPEND _sb_failures "${_sb_flag} missing from CMakeCache.txt")
    set(_sb_benchmark_clean OFF)
    continue()
  endif()

  set(_sb_type "${CMAKE_MATCH_2}")
  set(_sb_value "${CMAKE_MATCH_3}")
  if(NOT "${_sb_type}" STREQUAL "BOOL")
    list(APPEND _sb_failures "${_sb_flag} cache type is ${_sb_type}, expected BOOL")
  endif()

  if(${_sb_flag})
    set(_sb_expected ON)
  else()
    set(_sb_expected OFF)
  endif()
  if(_sb_value)
    set(_sb_actual ON)
  else()
    set(_sb_actual OFF)
  endif()
  if(NOT "${_sb_actual}" STREQUAL "${_sb_expected}")
    list(APPEND _sb_failures "${_sb_flag} cache value is ${_sb_value}, expected ${_sb_expected}")
  endif()
  if(_sb_actual)
    set(_sb_benchmark_clean OFF)
  endif()
endforeach()

if(_sb_failures)
  foreach(_sb_failure IN LISTS _sb_failures)
    message(SEND_ERROR "${_sb_failure}")
  endforeach()
  message(FATAL_ERROR "benchmark-clean instrumentation CMake cache gate failed")
endif()

if(SB_BUILD_TYPE STREQUAL "Release" AND _sb_benchmark_clean)
  message(STATUS "benchmark_clean_cmake_instrumentation_flags_gate=passed benchmark_clean=ON")
else()
  message(STATUS "benchmark_clean_cmake_instrumentation_flags_gate=passed benchmark_clean=OFF")
endif()
