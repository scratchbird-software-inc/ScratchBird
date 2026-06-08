# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# OEIC_OPTIMIZER_PRODUCTION_BUILD_GATE: fail closed when a production
# release-complete optimizer build attempts to carry fixture/default,
# placeholder, donor/parser, debug-only, relaxed-metric, or cluster-stub live
# authority paths.

if(NOT DEFINED SB_OPTIMIZER_PRODUCTION_BUILD)
  set(SB_OPTIMIZER_PRODUCTION_BUILD OFF)
endif()

set(_sb_optimizer_production_bool_flags
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
  SB_CLUSTER_PROVIDER_STUB
  SCRATCHBIRD_ENABLE_DEBUG_LOGS
  SCRATCHBIRD_ENABLE_HOTPATH_TRACE
  SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
  SCRATCHBIRD_ENABLE_PREPARED_TRACE
)

foreach(_sb_optimizer_flag IN LISTS _sb_optimizer_production_bool_flags)
  if(NOT DEFINED ${_sb_optimizer_flag})
    set(${_sb_optimizer_flag} OFF)
  endif()
endforeach()
unset(_sb_optimizer_flag)

if(SB_OPTIMIZER_PRODUCTION_BUILD)
  set(_sb_optimizer_build_failures)

  if(SB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "fixture optimizer statistics are forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "local default optimizer statistics are forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "policy default optimizer statistics are forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "relaxed optimizer metric evidence is forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "placeholder runtime evidence is forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_DONOR_AUTHORITY_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "donor optimizer authority is forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_PARSER_SHORTCUTS_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "parser execution shortcuts are forbidden in production optimizer builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS)
    list(APPEND _sb_optimizer_build_failures
      "cluster-stub live optimizer claims are forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "debug-only optimizer paths are forbidden in production builds")
  endif()
  if(SB_OPTIMIZER_ALLOW_SYNTHETIC_FEEDBACK_IN_PRODUCTION)
    list(APPEND _sb_optimizer_build_failures
      "synthetic optimizer feedback is forbidden in production builds")
  endif()
  foreach(_sb_trace_flag IN ITEMS
      SCRATCHBIRD_ENABLE_DEBUG_LOGS
      SCRATCHBIRD_ENABLE_HOTPATH_TRACE
      SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
      SCRATCHBIRD_ENABLE_PREPARED_TRACE)
    if(${_sb_trace_flag})
      list(APPEND _sb_optimizer_build_failures
        "${_sb_trace_flag} must be OFF for optimizer production builds")
    endif()
  endforeach()
  unset(_sb_trace_flag)

  if(_sb_optimizer_build_failures)
    foreach(_sb_optimizer_build_failure IN LISTS _sb_optimizer_build_failures)
      message(SEND_ERROR "${_sb_optimizer_build_failure}")
    endforeach()
    message(FATAL_ERROR "OEIC_OPTIMIZER_PRODUCTION_BUILD_GATE failed")
  endif()

  message(STATUS
    "optimizer_production_build_gate=passed profile=release-complete production=ON")
else()
  message(STATUS "optimizer_production_build_gate=passed production=OFF")
endif()

unset(_sb_optimizer_production_bool_flags)
