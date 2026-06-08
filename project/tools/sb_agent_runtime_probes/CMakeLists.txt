# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set(SB_AGENT_RUNTIME_PROBE_TARGETS
  sb_agent_runtime_registry_probe
  sb_agent_lifecycle_probe
  sb_agent_metric_dependency_probe
  sb_agent_policy_binding_probe
  sb_agent_action_evidence_probe
  sb_agent_security_probe
  sb_agent_management_api_probe
  sb_agent_sys_surfaces_probe
  sb_agent_dry_run_arbitration_probe
  sb_agent_state_persistence_probe
  sb_agent_emitted_metrics_probe
  sb_agent_local_agents_probe
  sb_agent_cluster_fail_closed_probe
  sb_agent_all_canonical_agents_probe
  sb_agent_no_wal_authority_probe
  sb_agent_scheduler_ownership_probe
  sb_agent_action_safety_budget_probe
  sb_agent_feedback_loop_probe
  sb_agent_safe_mode_probe
  sb_agent_resource_quota_probe
  sb_agent_simulation_replay_probe
  sb_agent_evidence_privacy_probe
  sb_agent_state_migration_probe
  sb_agent_manual_approval_probe
  sb_agent_overhead_gate_probe
  sb_agent_action_outcome_probe
  sb_agent_catalog_layout_probe
  sb_agent_actuator_capability_probe
  sb_agent_policy_lint_probe
  sb_agent_diagnostic_code_probe
  sb_agent_cardinality_scope_probe
  sb_agent_fault_injection_probe
  sb_agent_time_authority_probe
  sb_agent_engine_lifecycle_probe
  sb_agent_rollout_profile_probe
  sb_agent_explainability_probe
  sb_agent_precedence_probe
  sb_agent_execution_isolation_probe
  sb_agent_feature_gate_probe
  sb_agent_online_maintenance_probe
)

foreach(target_name IN LISTS SB_AGENT_RUNTIME_PROBE_TARGETS)
  add_executable(${target_name} main.cpp)
  target_link_libraries(${target_name} PRIVATE sb_engine_internal_api sb_engine_sblr sb_core_agents)
  target_compile_features(${target_name} PRIVATE cxx_std_20)
endforeach()
