# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set(SB_OPTIMIZER_EXECUTION_PLAN_PROBE_TARGETS
  sb_optimizer_legacy_reference_audit_probe
  sb_optimizer_current_capability_probe
  sb_optimizer_statistics_contract_probe
  sb_optimizer_statistics_freshness_probe
  sb_optimizer_statistics_api_probe
  sb_optimizer_rule_rewrite_probe
  sb_optimizer_selectivity_probe
  sb_optimizer_projection_expression_probe
  sb_optimizer_access_path_probe
  sb_optimizer_index_choice_probe
  sb_optimizer_sort_limit_probe
  sb_optimizer_join_planner_probe
  sb_optimizer_join_method_cost_probe
  sb_optimizer_cost_model_probe
  sb_optimizer_explain_evidence_probe
  sb_optimizer_cluster_fail_closed_probe
  sb_optimizer_remote_stats_unavailable_probe
  sb_optimizer_runtime_feedback_probe
  sb_optimizer_agent_feedback_probe
  sb_optimizer_no_wal_authority_probe
)

foreach(target_name IN LISTS SB_OPTIMIZER_EXECUTION_PLAN_PROBE_TARGETS)
  if(NOT TARGET ${target_name})
    add_executable(${target_name} main.cpp)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_link_libraries(${target_name} PRIVATE sb_engine_optimizer sb_engine_internal_api)
  endif()
endforeach()

