# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# OEIC_OPTIMIZER_PRODUCTION_BUILD_GATE_MATRIX: CTest-visible proof that the
# optimizer production CMake gate accepts clean production settings and refuses
# fixture/default/placeholder/donor/parser/debug/live-cluster-stub paths.

set(_sb_gate_script "${CMAKE_CURRENT_LIST_DIR}/OptimizerProductionBuildGate.cmake")

function(_sb_optimizer_gate_case case_name expected_success)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${_sb_gate_script}"
    RESULT_VARIABLE _sb_result
    OUTPUT_VARIABLE _sb_output
    ERROR_VARIABLE _sb_error)

  if(expected_success)
    if(NOT _sb_result EQUAL 0)
      message(SEND_ERROR
        "${case_name}: expected success but gate failed: ${_sb_output}${_sb_error}")
    endif()
  else()
    if(_sb_result EQUAL 0)
      message(SEND_ERROR
        "${case_name}: expected failure but gate passed: ${_sb_output}${_sb_error}")
    endif()
  endif()
endfunction()

set(_sb_base_args
  -DSB_OPTIMIZER_PRODUCTION_BUILD=ON
  -DSB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_DONOR_AUTHORITY_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_PARSER_SHORTCUTS_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS=OFF
  -DSB_OPTIMIZER_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION=OFF
  -DSB_OPTIMIZER_ALLOW_SYNTHETIC_FEEDBACK_IN_PRODUCTION=OFF
  -DSB_CLUSTER_PROVIDER_STUB=OFF
  -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF
  -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF
  -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF
  -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF
)

_sb_optimizer_gate_case(clean_release TRUE ${_sb_base_args})
_sb_optimizer_gate_case(fixture_stats FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(local_defaults FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(policy_defaults FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(relaxed_metrics FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(placeholder_evidence FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(donor_authority FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_DONOR_AUTHORITY_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(parser_shortcut FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_PARSER_SHORTCUTS_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(cluster_stub_claim FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS=ON)
_sb_optimizer_gate_case(debug_only FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(synthetic_feedback FALSE
  ${_sb_base_args} -DSB_OPTIMIZER_ALLOW_SYNTHETIC_FEEDBACK_IN_PRODUCTION=ON)
_sb_optimizer_gate_case(cluster_stub_link TRUE
  ${_sb_base_args} -DSB_CLUSTER_PROVIDER_STUB=ON)
_sb_optimizer_gate_case(debug_logs FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=ON)
_sb_optimizer_gate_case(hotpath_trace FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=ON)
_sb_optimizer_gate_case(exec_profile_trace FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=ON)
_sb_optimizer_gate_case(prepared_trace FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=ON)
_sb_optimizer_gate_case(non_production_bootstrap TRUE
  -DSB_OPTIMIZER_PRODUCTION_BUILD=OFF
  -DSB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION=ON
  -DSB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION=ON
  -DSB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS=ON
  -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=ON)

message(STATUS "optimizer_production_build_cmake_gate=passed")
