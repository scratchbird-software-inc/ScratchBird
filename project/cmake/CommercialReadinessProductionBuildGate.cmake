# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# CRP-PRODUCTION-BUILD-SAFETY-GATE: project-level commercial readiness
# production profile guard. This gate composes the subsystem production gates
# and blocks generic fixture, deterministic seed, forced hook, debug
# credential, debug-only authority, and unsupported cluster claim paths. The
# public compile-link cluster stub is allowed only when no commercial cluster
# production claim is made.

if(NOT DEFINED SB_COMMERCIAL_READINESS_PRODUCTION_BUILD)
  set(SB_COMMERCIAL_READINESS_PRODUCTION_BUILD OFF)
endif()

set(_sb_crp_production_bool_flags
  SB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION
  SB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION
  SB_AGENT_ALLOW_RELAXED_METRICS_IN_PRODUCTION
  SB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION
  SB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION
  SB_AGENT_ALLOW_SIDECAR_STATE_IN_PRODUCTION
  SB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION
  SB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION
  SB_AGENT_CLUSTER_STUB_LIVE_CLAIMS
  SB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_DONOR_AUTHORITY_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_PARSER_SHORTCUTS_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS
  SB_OPTIMIZER_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION
  SB_OPTIMIZER_ALLOW_SYNTHETIC_FEEDBACK_IN_PRODUCTION
  SB_COMMERCIAL_ALLOW_FIXTURE_AUTH_IN_PRODUCTION
  SB_COMMERCIAL_ALLOW_TEST_SEEDS_IN_PRODUCTION
  SB_COMMERCIAL_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION
  SB_COMMERCIAL_ALLOW_STUB_PROVIDERS_IN_PRODUCTION
  SB_COMMERCIAL_ALLOW_DEBUG_CREDENTIALS_IN_PRODUCTION
  SB_COMMERCIAL_ALLOW_DEBUG_ONLY_AUTHORITY_PATHS_IN_PRODUCTION
  SB_COMMERCIAL_ALLOW_NO_CLUSTER_PRODUCTION_CLAIMS
  SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS
  SB_CLUSTER_PROVIDER_STUB
  SCRATCHBIRD_ENABLE_DEBUG_LOGS
  SCRATCHBIRD_ENABLE_HOTPATH_TRACE
  SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
  SCRATCHBIRD_ENABLE_PREPARED_TRACE
)

foreach(_sb_crp_flag IN LISTS _sb_crp_production_bool_flags)
  if(NOT DEFINED ${_sb_crp_flag})
    set(${_sb_crp_flag} OFF)
  endif()
endforeach()
unset(_sb_crp_flag)

if(NOT DEFINED SB_ENABLE_CLUSTER_PROVIDER)
  set(SB_ENABLE_CLUSTER_PROVIDER OFF)
endif()
if(NOT DEFINED SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY)
  set(SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY "")
endif()

if(SB_COMMERCIAL_READINESS_PRODUCTION_BUILD)
  set(_sb_crp_build_failures)

  if(SB_COMMERCIAL_ALLOW_FIXTURE_AUTH_IN_PRODUCTION)
    list(APPEND _sb_crp_build_failures
      "fixture authentication is forbidden in commercial production builds")
  endif()
  if(SB_COMMERCIAL_ALLOW_TEST_SEEDS_IN_PRODUCTION)
    list(APPEND _sb_crp_build_failures
      "deterministic test seeds are forbidden in commercial production builds")
  endif()
  if(SB_COMMERCIAL_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION)
    list(APPEND _sb_crp_build_failures
      "forced collision hooks are forbidden in commercial production builds")
  endif()
  if(SB_COMMERCIAL_ALLOW_STUB_PROVIDERS_IN_PRODUCTION)
    list(APPEND _sb_crp_build_failures
      "stub providers are forbidden in commercial production builds")
  endif()
  if(SB_COMMERCIAL_ALLOW_DEBUG_CREDENTIALS_IN_PRODUCTION)
    list(APPEND _sb_crp_build_failures
      "debug-only credentials are forbidden in commercial production builds")
  endif()
  if(SB_COMMERCIAL_ALLOW_DEBUG_ONLY_AUTHORITY_PATHS_IN_PRODUCTION)
    list(APPEND _sb_crp_build_failures
      "debug-only authority paths are forbidden in commercial production builds")
  endif()
  foreach(_sb_crp_subsystem_flag IN ITEMS
      SB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION
      SB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION
      SB_AGENT_ALLOW_RELAXED_METRICS_IN_PRODUCTION
      SB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION
      SB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION
      SB_AGENT_ALLOW_SIDECAR_STATE_IN_PRODUCTION
      SB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION
      SB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION
      SB_AGENT_CLUSTER_STUB_LIVE_CLAIMS
      SB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_DONOR_AUTHORITY_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_PARSER_SHORTCUTS_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS
      SB_OPTIMIZER_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION
      SB_OPTIMIZER_ALLOW_SYNTHETIC_FEEDBACK_IN_PRODUCTION)
    if(${_sb_crp_subsystem_flag})
      list(APPEND _sb_crp_build_failures
        "${_sb_crp_subsystem_flag} must be OFF for commercial production builds")
    endif()
  endforeach()
  unset(_sb_crp_subsystem_flag)
  if(SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS)
    if(NOT SB_ENABLE_CLUSTER_PROVIDER)
      list(APPEND _sb_crp_build_failures
        "cluster production claims require SB_ENABLE_CLUSTER_PROVIDER=ON")
    endif()
    if(SB_COMMERCIAL_ALLOW_NO_CLUSTER_PRODUCTION_CLAIMS)
      list(APPEND _sb_crp_build_failures
        "no_cluster production claims are forbidden")
    endif()
    if(SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY STREQUAL "")
      list(APPEND _sb_crp_build_failures
        "cluster production claims require a non-stub external cluster provider library")
    endif()
  endif()
  foreach(_sb_trace_flag IN ITEMS
      SCRATCHBIRD_ENABLE_DEBUG_LOGS
      SCRATCHBIRD_ENABLE_HOTPATH_TRACE
      SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
      SCRATCHBIRD_ENABLE_PREPARED_TRACE)
    if(${_sb_trace_flag})
      list(APPEND _sb_crp_build_failures
        "${_sb_trace_flag} must be OFF for commercial production builds")
    endif()
  endforeach()
  unset(_sb_trace_flag)

  if(_sb_crp_build_failures)
    foreach(_sb_crp_build_failure IN LISTS _sb_crp_build_failures)
      message(SEND_ERROR "${_sb_crp_build_failure}")
    endforeach()
    message(FATAL_ERROR "CRP-PRODUCTION-BUILD-SAFETY-GATE failed")
  endif()

  message(STATUS
    "commercial_readiness_production_build_gate=passed profile=release-complete production=ON")
else()
  message(STATUS "commercial_readiness_production_build_gate=passed production=OFF")
endif()

unset(_sb_crp_production_bool_flags)
