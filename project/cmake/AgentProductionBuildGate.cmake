# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# AEIC_PRODUCTION_AGENT_BUILD_SEPARATION: fail closed when a production
# release-complete agent build attempts to carry fixture, sidecar, simulated,
# debug-only, or cluster-stub live authority paths.

if(NOT DEFINED SB_AGENT_PRODUCTION_BUILD)
  set(SB_AGENT_PRODUCTION_BUILD OFF)
endif()

set(_sb_agent_production_bool_flags
  SB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION
  SB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION
  SB_AGENT_ALLOW_RELAXED_METRICS_IN_PRODUCTION
  SB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION
  SB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION
  SB_AGENT_ALLOW_SIDECAR_STATE_IN_PRODUCTION
  SB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION
  SB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION
  SB_AGENT_CLUSTER_STUB_LIVE_CLAIMS
  SB_CLUSTER_PROVIDER_STUB
  SCRATCHBIRD_ENABLE_DEBUG_LOGS
  SCRATCHBIRD_ENABLE_HOTPATH_TRACE
  SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
  SCRATCHBIRD_ENABLE_PREPARED_TRACE
)

foreach(_sb_agent_flag IN LISTS _sb_agent_production_bool_flags)
  if(NOT DEFINED ${_sb_agent_flag})
    set(${_sb_agent_flag} OFF)
  endif()
endforeach()
unset(_sb_agent_flag)

if(SB_AGENT_PRODUCTION_BUILD)
  set(_sb_agent_build_failures)

  if(SB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "fixture authentication is forbidden in agent production builds")
  endif()
  if(SB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "fixture policies are forbidden in agent production builds")
  endif()
  if(SB_AGENT_ALLOW_RELAXED_METRICS_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "relaxed/registry-only metrics are forbidden in agent production builds")
  endif()
  if(SB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "test seeds are forbidden in agent production builds")
  endif()
  if(SB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "forced collision/test hooks are forbidden in agent production builds")
  endif()
  if(SB_AGENT_ALLOW_SIDECAR_STATE_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "sidecar-only agent state/evidence is forbidden in production builds")
  endif()
  if(SB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "simulated/default-only actuator execution is forbidden in production builds")
  endif()
  if(SB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION)
    list(APPEND _sb_agent_build_failures
      "debug-only agent paths are forbidden in production builds")
  endif()
  if(SB_AGENT_CLUSTER_STUB_LIVE_CLAIMS)
    list(APPEND _sb_agent_build_failures
      "cluster stub live claims are forbidden in production builds")
  endif()
  foreach(_sb_trace_flag IN ITEMS
      SCRATCHBIRD_ENABLE_DEBUG_LOGS
      SCRATCHBIRD_ENABLE_HOTPATH_TRACE
      SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
      SCRATCHBIRD_ENABLE_PREPARED_TRACE)
    if(${_sb_trace_flag})
      list(APPEND _sb_agent_build_failures
        "${_sb_trace_flag} must be OFF for agent production builds")
    endif()
  endforeach()
  unset(_sb_trace_flag)

  if(_sb_agent_build_failures)
    foreach(_sb_agent_build_failure IN LISTS _sb_agent_build_failures)
      message(SEND_ERROR "${_sb_agent_build_failure}")
    endforeach()
    message(FATAL_ERROR
      "AEIC_PRODUCTION_AGENT_BUILD_SEPARATION failed")
  endif()

  message(STATUS
    "agent_production_build_gate=passed profile=release-complete production=ON")
else()
  message(STATUS
    "agent_production_build_gate=passed production=OFF")
endif()

unset(_sb_agent_production_bool_flags)
