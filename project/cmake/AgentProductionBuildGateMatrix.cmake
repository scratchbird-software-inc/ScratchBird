# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# AEIC_PRODUCTION_AGENT_BUILD_SEPARATION_MATRIX: CTest-visible proof that the
# production CMake gate accepts clean release-complete agent builds and refuses
# fixture/debug/simulated/live-stub-claim paths.

set(_sb_gate_script "${CMAKE_CURRENT_LIST_DIR}/AgentProductionBuildGate.cmake")

function(_sb_agent_gate_case case_name expected_success)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${_sb_gate_script}"
    RESULT_VARIABLE _sb_result
    OUTPUT_VARIABLE _sb_output
    ERROR_VARIABLE _sb_error
  )

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
  -DSB_AGENT_PRODUCTION_BUILD=ON
  -DSB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION=OFF
  -DSB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION=OFF
  -DSB_AGENT_ALLOW_RELAXED_METRICS_IN_PRODUCTION=OFF
  -DSB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION=OFF
  -DSB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION=OFF
  -DSB_AGENT_ALLOW_SIDECAR_STATE_IN_PRODUCTION=OFF
  -DSB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION=OFF
  -DSB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION=OFF
  -DSB_AGENT_CLUSTER_STUB_LIVE_CLAIMS=OFF
  -DSB_CLUSTER_PROVIDER_STUB=OFF
  -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF
  -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF
  -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF
  -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF
)

_sb_agent_gate_case(clean_release TRUE ${_sb_base_args})
_sb_agent_gate_case(fixture_auth FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION=ON)
_sb_agent_gate_case(fixture_policy FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION=ON)
_sb_agent_gate_case(relaxed_metrics FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_RELAXED_METRICS_IN_PRODUCTION=ON)
_sb_agent_gate_case(test_seeds FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION=ON)
_sb_agent_gate_case(forced_collision_hooks FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION=ON)
_sb_agent_gate_case(sidecar_state FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_SIDECAR_STATE_IN_PRODUCTION=ON)
_sb_agent_gate_case(simulated_actuators FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION=ON)
_sb_agent_gate_case(debug_only_paths FALSE
  ${_sb_base_args} -DSB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION=ON)
_sb_agent_gate_case(cluster_stub_claim FALSE
  ${_sb_base_args} -DSB_AGENT_CLUSTER_STUB_LIVE_CLAIMS=ON)
_sb_agent_gate_case(cluster_stub_link TRUE
  ${_sb_base_args} -DSB_CLUSTER_PROVIDER_STUB=ON)
_sb_agent_gate_case(debug_logs FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=ON)
_sb_agent_gate_case(hotpath_trace FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=ON)
_sb_agent_gate_case(exec_profile_trace FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=ON)
_sb_agent_gate_case(prepared_trace FALSE
  ${_sb_base_args} -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=ON)
_sb_agent_gate_case(non_production_bootstrap TRUE
  -DSB_AGENT_PRODUCTION_BUILD=OFF
  -DSB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION=ON
  -DSB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION=ON
  -DSB_CLUSTER_PROVIDER_STUB=ON
  -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=ON)

message(STATUS "agent_production_build_cmake_gate=passed")
