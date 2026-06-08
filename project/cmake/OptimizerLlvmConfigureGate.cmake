# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# SEARCH_KEY: OEIC_LLVM_STATIC_LINK_OPTION
# Configure-only gate for LLVM link-mode separation. The native compile runtime
# gate validates execution/refusal semantics; this gate proves the build system
# admits dynamic by default, static only by explicit option, and disabled only in
# degraded profiles.

if(NOT DEFINED SB_PROJECT_SOURCE_DIR OR SB_PROJECT_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "SB_PROJECT_SOURCE_DIR is required")
endif()
if(NOT DEFINED SB_CONFIGURE_GATE_BINARY_ROOT OR SB_CONFIGURE_GATE_BINARY_ROOT STREQUAL "")
  message(FATAL_ERROR "SB_CONFIGURE_GATE_BINARY_ROOT is required")
endif()

set(_llvm_library "${SB_LLVM_TEST_LIBRARY}")
if(NOT _llvm_library)
  foreach(_candidate IN ITEMS
      "${SB_PROJECT_SOURCE_DIR}/tools/llvm/lib/libLLVM-23.so"
      "/usr/lib/x86_64-linux-gnu/libLLVM-23.so"
      "/usr/lib/llvm-23/lib/libLLVM-23.so"
      "/usr/lib/x86_64-linux-gnu/libLLVM.so.23.0"
      "/usr/lib/llvm-23/lib/libLLVM.so.23.0"
      "/usr/local/lib/libLLVM-23.so")
    if(EXISTS "${_candidate}")
      set(_llvm_library "${_candidate}")
      break()
    endif()
  endforeach()
endif()
if(NOT _llvm_library)
  message(FATAL_ERROR "OEIC LLVM configure gate requires a versioned libLLVM test library")
endif()

function(_configure_llvm_mode mode)
  set(_binary_dir "${SB_CONFIGURE_GATE_BINARY_ROOT}/${mode}")
  file(REMOVE_RECURSE "${_binary_dir}")
  set(_args
    "${CMAKE_COMMAND}"
    -S "${SB_PROJECT_SOURCE_DIR}"
    -B "${_binary_dir}"
    -DCMAKE_BUILD_TYPE=Release
    -DSB_BUILD_TESTS=OFF
    -DSB_NONCLUSTER_ENGINE_PROFILE=bootstrap
    "-DSB_LLVM_LINK_MODE=${mode}"
    -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF
    -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF
    -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF
    -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF)
  if(NOT mode STREQUAL "disabled")
    list(APPEND _args "-DSB_LLVM_LIBRARY=${_llvm_library}")
  endif()
  execute_process(
    COMMAND ${_args}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "LLVM ${mode} configure failed\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
  endif()
  file(READ "${_binary_dir}/CMakeCache.txt" _cache)
  if(NOT _cache MATCHES "SB_LLVM_LINK_MODE:STRING=${mode}")
    message(FATAL_ERROR "LLVM ${mode} configure did not preserve requested link mode")
  endif()
  if(mode STREQUAL "disabled")
    if(_cache MATCHES "SCRATCHBIRD_HAS_LLVM=1")
      message(FATAL_ERROR "LLVM disabled configure claimed LLVM availability")
    endif()
  else()
    if(NOT _cache MATCHES "SB_LLVM_LIBRARY:FILEPATH=.*LLVM")
      message(FATAL_ERROR "LLVM ${mode} configure did not record library path")
    endif()
  endif()
endfunction()

_configure_llvm_mode(dynamic)
_configure_llvm_mode(static)
_configure_llvm_mode(disabled)

message(STATUS "OEIC LLVM configure mode gate passed")
