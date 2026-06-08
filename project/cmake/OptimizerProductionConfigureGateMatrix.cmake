# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# OEIC_OPTIMIZER_PRODUCTION_CONFIGURE_GATE_MATRIX: top-level configure proof
# for non-bypassable optimizer production build separation. Release-complete
# negative cases fail before any production route can accept unsafe evidence;
# bootstrap remains permissive for test/development profiles.

if(NOT DEFINED SB_PROJECT_SOURCE_DIR OR NOT EXISTS "${SB_PROJECT_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "SB_PROJECT_SOURCE_DIR must point at project/CMakeLists.txt")
endif()
if(NOT DEFINED SB_CONFIGURE_GATE_BINARY_ROOT)
  set(SB_CONFIGURE_GATE_BINARY_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/optimizer_production_configure_gate")
endif()

set(_sb_optimizer_configure_common_args)
foreach(_sb_forwarded_var
        CMAKE_C_COMPILER
        CMAKE_CXX_COMPILER
        CMAKE_MAKE_PROGRAM
        SB_LLVM_PROJECT_ROOT
        SB_LLVM_TOOLS_ROOT
        SB_LLVM_LIBRARY
        SB_LLVM_LINK_MODE)
  if(DEFINED ${_sb_forwarded_var} AND NOT "${${_sb_forwarded_var}}" STREQUAL "")
    list(APPEND _sb_optimizer_configure_common_args
         "-D${_sb_forwarded_var}=${${_sb_forwarded_var}}")
  endif()
endforeach()

set(_sb_optimizer_configure_generator_args)
if(DEFINED SB_CONFIGURE_GATE_GENERATOR AND NOT "${SB_CONFIGURE_GATE_GENERATOR}" STREQUAL "")
  set(_sb_optimizer_configure_generator "${SB_CONFIGURE_GATE_GENERATOR}")
elseif(DEFINED CMAKE_GENERATOR AND NOT "${CMAKE_GENERATOR}" STREQUAL "")
  set(_sb_optimizer_configure_generator "${CMAKE_GENERATOR}")
else()
  set(_sb_optimizer_configure_generator "")
endif()
if(NOT "${_sb_optimizer_configure_generator}" STREQUAL "")
  string(REGEX REPLACE "^['\"]|['\"]$" "" _sb_optimizer_configure_generator
         "${_sb_optimizer_configure_generator}")
  list(APPEND _sb_optimizer_configure_generator_args -G "${_sb_optimizer_configure_generator}")
endif()

function(_sb_optimizer_configure_case case_name expected_success)
  set(_sb_case_dir "${SB_CONFIGURE_GATE_BINARY_ROOT}/${case_name}")
  file(REMOVE_RECURSE "${_sb_case_dir}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            ${_sb_optimizer_configure_generator_args}
            -S "${SB_PROJECT_SOURCE_DIR}"
            -B "${_sb_case_dir}"
            -DSB_BUILD_TESTS=OFF
            ${_sb_optimizer_configure_common_args}
            ${ARGN}
    RESULT_VARIABLE _sb_result
    OUTPUT_VARIABLE _sb_output
    ERROR_VARIABLE _sb_error)
  if(expected_success)
    if(NOT _sb_result EQUAL 0)
      message(SEND_ERROR
        "${case_name}: expected configure success but failed: ${_sb_output}${_sb_error}")
    endif()
  else()
    if(_sb_result EQUAL 0)
      message(SEND_ERROR
        "${case_name}: expected configure failure but succeeded: ${_sb_output}${_sb_error}")
    endif()
  endif()
endfunction()

_sb_optimizer_configure_case(enforcement_off FALSE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE=OFF)
_sb_optimizer_configure_case(fixture_stats FALSE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION=ON)
_sb_optimizer_configure_case(local_defaults FALSE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION=ON)
_sb_optimizer_configure_case(policy_defaults FALSE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION=ON)
_sb_optimizer_configure_case(relaxed_metrics FALSE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION=ON)
_sb_optimizer_configure_case(placeholder_evidence FALSE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION=ON)
_sb_optimizer_configure_case(cluster_stub_link TRUE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSB_ENABLE_CLUSTER_PROVIDER=ON
  -DSB_CLUSTER_PROVIDER_STUB=ON)
_sb_optimizer_configure_case(debug_logs FALSE
  -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete
  -DCMAKE_BUILD_TYPE=Release
  -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=ON)
_sb_optimizer_configure_case(bootstrap_fixture_success TRUE
  -DSB_NONCLUSTER_ENGINE_PROFILE=bootstrap
  -DCMAKE_BUILD_TYPE=Release
  -DSB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION=ON
  -DSB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION=ON
  -DSB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION=ON)

message(STATUS "optimizer_production_configure_gate=passed")
