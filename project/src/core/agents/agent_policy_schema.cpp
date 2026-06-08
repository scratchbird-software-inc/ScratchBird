// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

namespace scratchbird::core::agents {
namespace {

using FieldList = std::vector<AgentPolicyFieldSchema>;

AgentPolicyFieldSchema Field(std::string name,
                             AgentPolicyFieldType type,
                             std::string units,
                             std::string default_value,
                             bool required,
                             std::optional<double> minimum = std::nullopt,
                             std::optional<double> maximum = std::nullopt,
                             AgentPolicyFieldSensitivity sensitivity =
                                 AgentPolicyFieldSensitivity::operational) {
  AgentPolicyFieldSchema schema;
  schema.name = std::move(name);
  schema.type = type;
  schema.units = std::move(units);
  schema.default_value = std::move(default_value);
  schema.required = required;
  schema.minimum = minimum;
  schema.maximum = maximum;
  schema.sensitivity = sensitivity;
  return schema;
}

AgentPolicyFieldSchema Bool(std::string name,
                            std::string default_value = "true",
                            bool required = true) {
  return Field(std::move(name), AgentPolicyFieldType::boolean, "boolean",
               std::move(default_value), required);
}

AgentPolicyFieldSchema Count(std::string name,
                             std::string default_value = "1",
                             bool required = true,
                             std::optional<double> maximum = std::nullopt) {
  return Field(std::move(name), AgentPolicyFieldType::unsigned_integer, "count",
               std::move(default_value), required, 0.0, maximum);
}

AgentPolicyFieldSchema DurationSeconds(std::string name,
                                       std::string default_value = "1",
                                       bool required = true) {
  return Field(std::move(name), AgentPolicyFieldType::duration_seconds, "seconds",
               std::move(default_value), required, 1.0);
}

AgentPolicyFieldSchema DurationMicros(std::string name,
                                      std::string default_value = "1",
                                      bool required = true) {
  return Field(std::move(name), AgentPolicyFieldType::duration_microseconds,
               "microseconds", std::move(default_value), required, 1.0);
}

AgentPolicyFieldSchema Bytes(std::string name,
                             std::string default_value = "1",
                             bool required = true) {
  return Field(std::move(name), AgentPolicyFieldType::byte_count, "bytes",
               std::move(default_value), required, 1.0);
}

AgentPolicyFieldSchema Percent(std::string name,
                               std::string default_value = "1",
                               bool required = true) {
  return Field(std::move(name), AgentPolicyFieldType::percent, "percent",
               std::move(default_value), required, 0.0, 100.0);
}

AgentPolicyFieldSchema Decimal(std::string name,
                               std::string default_value = "1",
                               bool required = true,
                               std::optional<double> maximum = 1.0) {
  return Field(std::move(name), AgentPolicyFieldType::decimal, "ratio",
               std::move(default_value), required, 0.0, maximum);
}

AgentPolicyFieldSchema Token(std::string name,
                             std::string default_value = "baseline",
                             bool required = true) {
  return Field(std::move(name), AgentPolicyFieldType::token, "token",
               std::move(default_value), required);
}

AgentPolicyFieldSchema TokenList(std::string name,
                                 std::string default_value = "baseline",
                                 bool required = true) {
  return Field(std::move(name), AgentPolicyFieldType::token_list, "token_list",
               std::move(default_value), required);
}

FieldList CommonResourceBudgetFields() {
  return {
      Bool("protect_foreground_work", "true", false),
      DurationMicros("max_cpu_time_microseconds", "1", false),
      Bytes("max_memory_bytes", "1", false),
      Bytes("max_io_bytes", "1", false),
      Count("max_io_ops", "1", false),
      Count("max_thread_slots", "1", false),
      Count("max_queue_depth", "1", false),
      DurationMicros("min_run_interval_microseconds", "1", false),
      DurationMicros("retry_backoff_microseconds", "1", false),
      DurationMicros("watchdog_timeout_microseconds", "1", false),
      Count("max_history_query_rows", "1", false),
      Count("max_evidence_fanout", "1", false),
      Count("max_label_cardinality", "1", false),
  };
}

const std::map<std::string, FieldList>& PolicySchemaMap() {
  static const std::map<std::string, FieldList> schemas = {
      {"node_resource_policy",
       {DurationSeconds("capability_probe_interval"),
        DurationSeconds("suitability_score_window"),
        DurationSeconds("stale_after")}},
      {"metric_registry_policy",
       {Count("schema_version"), Count("cardinality_limit"),
        DurationSeconds("retention_window"), DurationSeconds("rollup_interval"),
        Bool("reject_on_schema_drift")}},
      {"storage_health_policy",
       {DurationMicros("fsync_p99_critical_us"),
        Count("unknown_page_quarantine_threshold"),
        Count("checksum_failure_threshold"),
        DurationSeconds("optimizer_cost_update_window")}},
      {"memory_governor_policy",
       {Percent("emergency_reserve_percent"),
        Count("allocation_failure_pressure"), Percent("spill_threshold"),
        Percent("cache_shrink_limit")}},
      {"index_health_policy",
       {Count("rebuild_threshold"), Count("split_pressure_threshold"),
        DurationSeconds("unused_index_window"),
        DurationSeconds("recommendation_cooldown")}},
      {"cluster_autoscale_policy",
       {Percent("scale_up_threshold"), Percent("scale_down_threshold"),
        DurationSeconds("cooldown_seconds"), Bool("no_scale_down_when_limbo")}},
      {"admission_control_policy",
       {Count("listener_queue_threshold"), Count("scheduler_queue_threshold"),
        Count("hard_limit_threshold"), TokenList("workload_class_rules")}},
      {"parser_health_policy",
       {Count("parser_crash_quarantine_threshold"),
        DurationSeconds("drain_cooldown"), TokenList("package_quarantine_rules"),
        DurationMicros("policy_attach_latency_warn_us")}},
      {"long_transaction_policy",
       {DurationSeconds("oldest_snapshot_pressure_seconds"),
        DurationSeconds("idle_warning_seconds"),
        DurationSeconds("blocker_escalation_seconds"),
        Bool("cancel_requires_approval")}},
      {"storage_version_cleanup_policy",
       {Bool("authoritative_cleanup_horizon_required"),
        DurationSeconds("cleanup_horizon"), Count("max_versions_per_batch"),
        Count("retained_row_pressure"), Count("active_blocker_threshold")}},
      {"cleanup_archive_policy",
       {Count("cleanup_blocked_threshold"),
        DurationMicros("archive_slice_max_age_us"),
        Bool("authoritative_lwm_required"), Bool("verify_before_release")}},
      {"policy_recommendation_policy",
       {Decimal("recommendation_burn_rate"),
        Decimal("confidence_threshold"), DurationSeconds("cooldown_seconds"),
        Bool("no_auto_apply")}},
      {"distributed_query_policy",
       {DurationMicros("fragment_delay_warn_us"),
        DurationSeconds("queue_sample_interval"),
        Count("remote_fragment_trace_level")}},
      {"remote_routing_policy",
       {Decimal("remote_route_latency_weight", "1", true, std::nullopt),
        Bool("stale_route_refusal"), Bool("local_fallback_required"),
        Bool("fence_required")}},
      {"optimizer_learning_policy",
       {Decimal("correction_threshold"), Decimal("confidence_floor"),
        DurationSeconds("max_correction_age"), Bool("no_semantic_override")}},
      {"support_bundle_policy",
       {Bool("completeness_required"), Bool("redaction_required"),
        DurationSeconds("bundle_expiry"), Bool("secret_classes_forbidden")}},
      {"cluster_scheduler_policy",
       {Count("scheduler_queue_threshold"), Decimal("placement_score_floor"),
        Bool("fence_required"), Bool("keep_queued_on_uncertainty")}},
      {"job_control_policy",
       {Bool("cancel_requires_owner_or_admin"), DurationSeconds("retry_cooldown"),
        DurationSeconds("suppression_max_duration"), Bool("audit_required")}},
      {"backup_policy",
       {DurationSeconds("stuck_progress_window"), Bool("verification_required"),
        Token("blocker_policy"), Bool("cancel_requires_approval")}},
      {"archive_policy",
       {Count("archive_queue_pressure"), DurationSeconds("slice_max_age"),
        Bool("verification_required"), Bool("protected_slice_delete_forbidden")}},
      {"restore_drill_policy",
       {Bool("isolated_target_required"), DurationMicros("max_duration_us"),
        Bool("verification_required"), Bool("source_mutation_forbidden")}},
      {"pitr_policy",
       {DurationSeconds("minimum_pitr_window_seconds"),
        DurationSeconds("estimate_refresh_interval"),
        Bool("restore_plan_requires_approval"),
        DurationSeconds("archive_freshness_limit")}},
      {"identity_lifecycle_policy",
       {Token("auth_anomaly_policy"), Bool("lock_requires_sec_right"),
        Bool("evidence_required"), Token("redaction_class")}},
      {"session_control_policy",
       {Bool("disconnect_requires_right"), DurationSeconds("reauth_cooldown"),
        Bool("revoke_requires_sec_right"), Token("self_view_redaction")}},
      {"alerting_baseline",
       {DurationSeconds("dedupe_window"), TokenList("grouping_rules"),
        DurationSeconds("silence_max_duration"),
        Bool("critical_silence_requires_override")}},
      {"export_default_baseline",
       {Bool("residency_required"), Bool("redaction_required"),
        Count("queue_backpressure"), Bool("adapter_health_required")}},
      {"upgrade_policy",
       {Bool("readiness_required"), Bool("fence_required"),
        TokenList("rolling_step_order"), Token("rollback_gate"),
        Bool("version_compatibility_required")}},
      {"filespace_capacity_policy",
       {Bytes("minimum_free_bytes"), Percent("minimum_free_percent"),
        DurationSeconds("growth_rate_window_seconds"),
        DurationSeconds("depletion_eta_warning_seconds"),
        DurationSeconds("depletion_eta_action_seconds"), Bool("expand_allowed"),
        Bool("move_allowed"), Bool("shrink_allowed"), Bool("truncate_allowed"),
        Count("critical_device_error_threshold"),
        DurationMicros("fsync_p99_critical_us"),
        Token("required_approval_class")}},
      {"filespace_shadow_promotion_policy",
       {Count("primary_degradation_threshold"),
        Bool("candidate_readiness_required"),
        Bool("catalog_persistence_move_required"),
        Bool("operator_approval_required")}},
      {"page_preallocation_policy",
       {Bool("preallocation_allowed"), DurationSeconds("forecast_window_seconds"),
        Decimal("history_confidence_threshold"),
        Bytes("max_reserved_bytes_per_filespace"),
        TokenList("allowed_page_families"), TokenList("workload_calendar"),
        Count("allocation_throttle_pages_per_second")}},
      {"page_relocation_policy",
       {Bool("relocation_allowed"), Bool("defragment_allowed"),
        Bool("publish_shrink_ready_allowed"), Count("max_pages_per_interval"),
        DurationSeconds("interval_seconds"),
        TokenList("unmovable_blocker_classes"), Bool("require_checksum_valid"),
        Bool("require_transaction_holds_clear"),
        Bool("require_backup_archive_holds_clear")}},
  };
  return schemas;
}

bool ParseUnsignedInteger(const std::string& value, unsigned long long* out) {
  if (value.empty()) { return false; }
  if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
      })) {
    return false;
  }
  try {
    std::size_t pos = 0;
    const auto parsed = std::stoull(value, &pos);
    if (pos != value.size()) { return false; }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDecimal(const std::string& value, double* out) {
  if (value.empty()) { return false; }
  try {
    std::size_t pos = 0;
    const auto parsed = std::stod(value, &pos);
    if (pos != value.size() || !std::isfinite(parsed)) { return false; }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ContainsControlCharacter(const std::string& value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char c) {
    return std::iscntrl(c) != 0;
  });
}

AgentRuntimeStatus ValidateNumericRange(const AgentPolicyFieldSchema& schema,
                                        double value,
                                        const std::string& policy_family) {
  if (schema.minimum.has_value() && value < *schema.minimum) {
    return AgentError("SB_AGENT_POLICY_SCHEMA.RANGE_VIOLATION",
                      policy_family + ":" + schema.name);
  }
  if (schema.maximum.has_value() && value > *schema.maximum) {
    return AgentError("SB_AGENT_POLICY_SCHEMA.RANGE_VIOLATION",
                      policy_family + ":" + schema.name);
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateFieldValue(const AgentPolicyFieldSchema& schema,
                                      const std::string& value,
                                      const std::string& policy_family) {
  if (value.empty()) {
    return AgentError("SB_AGENT_POLICY_SCHEMA.EMPTY_VALUE",
                      policy_family + ":" + schema.name);
  }
  switch (schema.type) {
    case AgentPolicyFieldType::boolean:
      if (value != "true" && value != "false") {
        return AgentError("SB_AGENT_POLICY_SCHEMA.TYPE_MISMATCH",
                          policy_family + ":" + schema.name + ":boolean");
      }
      return AgentOk();
    case AgentPolicyFieldType::unsigned_integer:
    case AgentPolicyFieldType::duration_seconds:
    case AgentPolicyFieldType::duration_microseconds:
    case AgentPolicyFieldType::byte_count:
    case AgentPolicyFieldType::percent: {
      unsigned long long parsed = 0;
      if (!ParseUnsignedInteger(value, &parsed)) {
        return AgentError("SB_AGENT_POLICY_SCHEMA.TYPE_MISMATCH",
                          policy_family + ":" + schema.name + ":unsigned_integer");
      }
      return ValidateNumericRange(schema, static_cast<double>(parsed), policy_family);
    }
    case AgentPolicyFieldType::decimal: {
      double parsed = 0.0;
      if (!ParseDecimal(value, &parsed)) {
        return AgentError("SB_AGENT_POLICY_SCHEMA.TYPE_MISMATCH",
                          policy_family + ":" + schema.name + ":decimal");
      }
      return ValidateNumericRange(schema, parsed, policy_family);
    }
    case AgentPolicyFieldType::token:
    case AgentPolicyFieldType::token_list:
      if (ContainsControlCharacter(value)) {
        return AgentError("SB_AGENT_POLICY_SCHEMA.TYPE_MISMATCH",
                          policy_family + ":" + schema.name + ":token");
      }
      return AgentOk();
  }
  return AgentError("SB_AGENT_POLICY_SCHEMA.UNKNOWN_TYPE",
                    policy_family + ":" + schema.name);
}

FieldList WithCommonOptionalFields(FieldList fields) {
  const auto optional = CommonResourceBudgetFields();
  fields.insert(fields.end(), optional.begin(), optional.end());
  return fields;
}

}  // namespace

std::string AgentPolicyFieldTypeName(AgentPolicyFieldType type) {
  switch (type) {
    case AgentPolicyFieldType::boolean: return "boolean";
    case AgentPolicyFieldType::unsigned_integer: return "unsigned_integer";
    case AgentPolicyFieldType::decimal: return "decimal";
    case AgentPolicyFieldType::duration_seconds: return "duration_seconds";
    case AgentPolicyFieldType::duration_microseconds: return "duration_microseconds";
    case AgentPolicyFieldType::byte_count: return "byte_count";
    case AgentPolicyFieldType::percent: return "percent";
    case AgentPolicyFieldType::token: return "token";
    case AgentPolicyFieldType::token_list: return "token_list";
  }
  return "unknown";
}

std::string AgentPolicyFieldSensitivityName(
    AgentPolicyFieldSensitivity sensitivity) {
  switch (sensitivity) {
    case AgentPolicyFieldSensitivity::public_evidence:
      return "public_evidence";
    case AgentPolicyFieldSensitivity::operational: return "operational";
    case AgentPolicyFieldSensitivity::sensitive: return "sensitive";
  }
  return "unknown";
}

std::vector<std::string> AgentPolicySchemaFamilies() {
  std::vector<std::string> families;
  for (const auto& entry : PolicySchemaMap()) {
    families.push_back(entry.first);
  }
  return families;
}

std::vector<AgentPolicyFieldSchema> AgentPolicySchemaForFamily(
    const std::string& policy_family) {
  const auto found = PolicySchemaMap().find(policy_family);
  if (found == PolicySchemaMap().end()) { return {}; }
  return WithCommonOptionalFields(found->second);
}

std::vector<std::string> RequiredAgentPolicySchemaFieldsForFamily(
    const std::string& policy_family) {
  std::vector<std::string> fields;
  const auto found = PolicySchemaMap().find(policy_family);
  if (found == PolicySchemaMap().end()) { return fields; }
  for (const auto& schema : found->second) {
    if (schema.required) { fields.push_back(schema.name); }
  }
  return fields;
}

std::map<std::string, std::string> DefaultAgentPolicyConfigFieldsForFamily(
    const std::string& policy_family) {
  std::map<std::string, std::string> fields;
  const auto found = PolicySchemaMap().find(policy_family);
  if (found == PolicySchemaMap().end()) { return fields; }
  for (const auto& schema : found->second) {
    if (schema.required) { fields.emplace(schema.name, schema.default_value); }
  }
  return fields;
}

std::optional<AgentPolicyFieldSchema> FindAgentPolicyFieldSchema(
    const std::string& policy_family,
    const std::string& field_name) {
  for (const auto& schema : AgentPolicySchemaForFamily(policy_family)) {
    if (schema.name == field_name) { return schema; }
  }
  return std::nullopt;
}

AgentRuntimeStatus ValidateAgentPolicyConfigAgainstSchema(
    const AgentPolicy& policy) {
  const auto schemas = AgentPolicySchemaForFamily(policy.policy_family);
  if (schemas.empty()) {
    return AgentError("SB_AGENT_POLICY_SCHEMA.UNKNOWN_FAMILY",
                      policy.policy_family);
  }

  std::map<std::string, AgentPolicyFieldSchema> by_name;
  for (const auto& schema : schemas) {
    by_name.emplace(schema.name, schema);
  }

  for (const auto& schema : schemas) {
    if (!schema.required) { continue; }
    const auto found = policy.config_fields.find(schema.name);
    if (found == policy.config_fields.end() || found->second.empty()) {
      return AgentError("SB_AGENT_POLICY.REQUIRED_FIELD_MISSING",
                        policy.policy_family + ":" + schema.name);
    }
  }

  for (const auto& field : policy.config_fields) {
    const auto schema = by_name.find(field.first);
    if (schema == by_name.end()) {
      return AgentError("SB_AGENT_POLICY_SCHEMA.UNKNOWN_FIELD",
                        policy.policy_family + ":" + field.first);
    }
    const auto status = ValidateFieldValue(schema->second, field.second,
                                           policy.policy_family);
    if (!status.ok) { return status; }
  }
  return AgentOk();
}

}  // namespace scratchbird::core::agents
