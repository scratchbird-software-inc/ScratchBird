// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SYS-INFORMATION-PROJECTION-ANCHOR
#include "api_types.hpp"
#include <variant>
#include <map>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kSysInformationDiagnosticUuidExposed =
    "SB_DIAG_INFORMATION_SCHEMA_UUID_EXPOSED";
inline constexpr const char* kSysInformationDiagnosticViewUnsupported =
    "SB_DIAG_INFORMATION_SCHEMA_VIEW_UNSUPPORTED";
inline constexpr const char* kSysInformationDiagnosticNameNotFound =
    "SB_DIAG_INFORMATION_SCHEMA_NAME_NOT_FOUND";
inline constexpr const char* kSysInformationDiagnosticSecurityLeakBlocked =
    "SB_DIAG_INFORMATION_SCHEMA_SECURITY_LEAK_BLOCKED";
inline constexpr const char* kSysInformationDiagnosticClusterScopeForbidden =
    "SB_DIAG_INFORMATION_SCHEMA_CLUSTER_SCOPE_FORBIDDEN";
inline constexpr const char* kSysInformationDiagnosticInvalidDefinition =
    "SB_DIAG_INFORMATION_SCHEMA_INVALID_DEFINITION";

enum class SysInformationProjectionFamily : std::uint16_t {
  standard_information_schema,
  scratchbird_extension,
  catalog_readable,
  frontend_projection,
};

enum class SysInformationSourceKind : std::uint16_t {
  catalog_object_identity,
  identity_resolver,
  comment_resolver,
  catalog_index_profile,
  security_policy,
  datatype_descriptor,
  routine_parameter_metadata,
  agent_runtime,
  agent_metric_dependency,
  agent_policy,
  agent_action,
  agent_evidence,
  storage_agent_state,
  ipar_agent_lifecycle,
  ipar_metric_counter,
  ipar_telemetry_control,
  ipar_slow_path_reason,
  ipar_contention_quota,
};

struct SysInformationProjectionColumn {
  std::string column_name;
  std::string logical_type;
  bool nullable = false;
  bool resolver_backed_name = false;
  bool comment_backed_text = false;
  bool exposes_internal_uuid = false;
  bool scratchbird_extension = false;
};

struct SysInformationProjectionDefinition {
  std::string view_path;
  std::string view_scope = "local";
  std::string ui_area;
  std::vector<std::string> key_columns;
  std::string description;
  SysInformationProjectionFamily family =
      SysInformationProjectionFamily::standard_information_schema;
  std::vector<SysInformationProjectionColumn> columns;
  std::vector<SysInformationSourceKind> source_kinds;
  bool resolver_join_required = true;
  bool comment_join_required = false;
  bool authorization_filter_required = true;
  bool redaction_required = true;
  bool language_fallback_required = true;
  bool mga_snapshot_visibility_required = true;
  bool exposes_internal_uuid = false;
  bool cluster_path_fail_closed = true;
};

struct SysInformationCatalogObjectSource {
  EngineUuid object_uuid;
  std::string object_class;
  EngineUuid schema_uuid;
  EngineUuid parent_object_uuid;
  std::string table_type;
  bool temporary = false;
  std::string temporary_scope;
  EngineUuid temporary_session_uuid;
  std::string on_commit_action;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t created_local_transaction_id = 0;
  std::uint64_t dropped_local_transaction_id = 0;
  bool hidden = false;
  bool cluster_path = false;
};

struct SysInformationResolverNameSource {
  EngineUuid object_uuid;
  std::string object_class;
  EngineUuid scope_uuid;
  std::string language_tag = "en";
  std::string name_class = "primary";
  std::string display_name;
  std::string raw_name_text;
  std::string normalized_lookup_key;
  std::string exact_lookup_key;
  std::string full_path_lookup_key;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationCommentSource {
  EngineUuid object_uuid;
  std::string object_class;
  std::string language_tag = "en";
  std::string comment_text;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationDatatypeDescriptorSource {
  std::string type_catalog;
  std::string type_schema;
  std::string type_name;
  std::string standard_type_name;
  std::string canonical_type_family;
  std::string canonical_type_code;
  std::string driver_family = "native";
  std::string native_type_code;
  std::string sql_type_code;
  std::string precision;
  std::string scale;
  std::string display_size;
  std::string character_maximum_length;
  std::string character_octet_length;
  std::string numeric_precision_radix;
  std::string is_nullable = "UNKNOWN";
  std::string is_signed = "UNKNOWN";
  std::string is_case_sensitive = "UNKNOWN";
  std::string is_searchable = "UNKNOWN";
  std::string is_currency = "NO";
  std::string is_auto_increment_capable = "NO";
  std::string literal_prefix;
  std::string literal_suffix;
  std::string create_params;
  std::string compatibility_class = "native_or_better";
  std::string support_state = "supported";
  std::string backend_profile;
  std::string unsupported_reason;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationColumnSource {
  EngineUuid column_uuid;
  EngineUuid relation_object_uuid;
  EngineUuid schema_uuid;
  std::string column_name;
  std::uint32_t ordinal_position = 0;
  std::string datatype_name;
  std::string domain_name;
  std::string is_nullable = "YES";
  std::string column_default;
  std::string comment_text;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationDomainSource {
  EngineUuid domain_uuid;
  EngineUuid row_uuid;
  EngineUuid schema_uuid;
  std::string source_type_name;
  std::string base_type_name;
  std::string domain_kind = "scalar";
  std::string nullable = "YES";
  std::string default_expression_envelope;
  std::string check_constraint_envelope;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t created_local_transaction_id = 0;
  bool hidden = false;
};

struct SysInformationSettingSource {
  std::string setting_name;
  std::string setting_value_display;
  std::string authority = "engine";
  std::string default_source = "compiled_default";
  std::string redaction_state = "not_redacted";
  std::string visibility_state = "visible";
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationFrontendAgentSource {
  std::string agent_name;
  std::string agent_type_id;
  std::string scope_kind = "local";
  std::string state = "available";
  std::string health_state = "healthy";
  std::string enabled = "YES";
  std::string policy_name;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationAgentSource {
  EngineUuid agent_uuid;
  std::string agent_ref;
  std::string agent_name;
  std::string agent_type_id;
  std::string scope_kind = "local";
  EngineUuid scope_uuid;
  std::string scope_ref;
  std::string component;
  std::string state = "available";
  std::string health_state = "healthy";
  std::string enabled = "YES";
  EngineUuid policy_uuid;
  std::string policy_ref;
  std::string policy_name;
  std::string last_transition_at;
  std::string last_diagnostic_code;
  EngineUuid last_evidence_uuid;
  std::string last_decision;
  std::string retry_not_before;
  std::string diagnostic_redaction_state = "not_redacted";
  std::uint64_t policy_generation = 0;
  std::uint64_t queue_depth = 0;
  std::uint64_t action_backlog = 0;
  std::uint64_t failure_count = 0;
  std::uint64_t quarantine_count = 0;
  std::uint64_t overhead_budget_units = 0;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
  bool scope_visible = true;
};

struct SysInformationAgentMetricDependencySource {
  EngineUuid agent_uuid;
  std::string agent_ref;
  std::string metric_family;
  std::string metric_namespace;
  std::string required_or_optional = "required";
  std::string freshness_limit;
  std::string current_freshness;
  std::string quality_state = "fresh";
  std::string fail_behavior = "fail_closed";
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
  bool metric_values_visible = false;
};

struct SysInformationAgentPolicySource {
  EngineUuid agent_uuid;
  std::string agent_ref;
  EngineUuid policy_uuid;
  std::string policy_ref;
  std::string policy_name;
  std::string policy_family;
  EngineUuid version_uuid;
  std::string version_ref;
  std::string active_state = "active";
  std::string validation_state = "valid";
  std::string attached_at;
  std::string attached_by;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationAgentActionSource {
  EngineUuid action_uuid;
  std::string action_ref;
  EngineUuid agent_uuid;
  std::string agent_ref;
  std::string action_id;
  std::string state = "recommended";
  std::string risk_class;
  std::string created_at;
  std::string expires_at;
  std::string approval_required = "NO";
  EngineUuid actor_uuid;
  std::string actor_ref;
  std::string diagnostic_code;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
  bool actor_visible = false;
};

struct SysInformationAgentOverrideSource {
  EngineUuid override_uuid;
  std::string override_ref;
  EngineUuid target_uuid;
  std::string target_ref;
  EngineUuid scope_uuid;
  std::string scope_ref;
  std::string suppression_class;
  std::string starts_at;
  std::string expires_at;
  std::string state = "active";
  std::string reason_code;
  EngineUuid created_by;
  std::string created_by_ref;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
  bool actor_visible = false;
  bool scope_visible = true;
};

struct SysInformationAgentEvidenceSource {
  EngineUuid evidence_uuid;
  std::string evidence_ref;
  EngineUuid agent_uuid;
  std::string agent_ref;
  std::string evidence_type;
  EngineUuid action_uuid;
  std::string action_ref;
  std::string redaction_class = "summary";
  std::string created_at;
  EngineUuid actor_uuid;
  std::string actor_ref;
  std::string payload_digest;
  std::string payload_redacted = "YES";
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
  bool actor_visible = false;
};

struct SysInformationAgentAuditSource {
  EngineUuid audit_uuid;
  std::string audit_ref;
  EngineUuid evidence_uuid;
  std::string evidence_ref;
  EngineUuid actor_uuid;
  std::string actor_ref;
  std::string command_name;
  std::string sblr_operation;
  std::string api_call;
  std::string result_state;
  std::string diagnostic_code;
  std::string created_at;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
  bool actor_visible = false;
};

struct SysInformationFilespaceCapacityAgentStateSource {
  EngineUuid binding_uuid;
  EngineUuid agent_uuid;
  std::string agent_ref;
  EngineUuid filespace_uuid;
  std::string filespace_ref;
  EngineUuid policy_uuid;
  std::string policy_ref;
  std::string mode = "observe";
  std::string health_state = "healthy";
  std::string last_capacity_metric_at;
  std::string last_health_metric_at;
  std::string last_recommendation_code;
  std::string last_refusal_code;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationPageAllocationAgentStateSource {
  EngineUuid binding_uuid;
  EngineUuid agent_uuid;
  std::string agent_ref;
  EngineUuid filespace_uuid;
  std::string filespace_ref;
  std::string page_family;
  std::string page_type;
  EngineUuid policy_uuid;
  std::string policy_ref;
  std::string mode = "observe";
  std::string last_scan_generation;
  std::string last_shrink_ready_state;
  std::string last_refusal_code;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationFilespaceShrinkReadinessSource {
  EngineUuid filespace_uuid;
  std::string filespace_ref;
  std::string safe_start_byte;
  std::string safe_end_byte;
  std::string truncate_ready_bytes;
  std::string blocker_count;
  std::string readiness_state;
  std::string scan_generation;
  EngineUuid evidence_uuid;
  std::string evidence_ref;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationIparAgentLifecycleSource {
  std::string runtime_id;
  std::string source_kind = "server_agent_runtime";
  std::string lifecycle_state = "stopped";
  std::string idle_state = "idle";
  std::string worker_wake_policy;
  std::uint64_t worker_thread_count = 0;
  std::uint64_t background_worker_slots = 0;
  std::uint64_t foreground_reserved_capacity = 0;
  std::uint64_t scheduler_ticks = 0;
  std::uint64_t total_worker_ticks = 0;
  std::uint64_t total_actions_accepted = 0;
  std::uint64_t total_actions_refused = 0;
  std::uint64_t total_actions_failed = 0;
  std::uint64_t scheduled_worker_count = 0;
  std::uint64_t min_worker_ticks = 0;
  std::uint64_t max_worker_ticks = 0;
  std::uint64_t starvation_events = 0;
  std::string last_diagnostic_agent_type_id;
  std::string last_diagnostic_action;
  std::string last_diagnostic_outcome;
  std::string last_diagnostic_code;
  std::string last_diagnostic_detail;
  std::uint64_t durable_lease_count = 0;
  std::uint64_t durable_action_backlog_count = 0;
  std::uint64_t durable_replay_pending_action_count = 0;
  std::uint64_t process_rss_kb = 0;
  std::uint64_t process_vsize_kb = 0;
  std::string source_state = "observed";
  std::uint64_t catalog_generation_id = 0;
  bool started = false;
  bool stopping = false;
  bool hidden = false;
};

struct SysInformationIparMetricCounterSource {
  std::string metric_id;
  std::string metric_path;
  std::string metric_type = "counter";
  std::string metric_unit = "count";
  std::string label_summary;
  std::string producer;
  std::string source_state = "observed";
  std::uint64_t value = 0;
  std::uint64_t sample_count = 0;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationIparTelemetryControlSource {
  std::string budget_id = "IPAR-M031";
  std::string control_name;
  std::string metric_path;
  std::string source_state = "observed";
  std::string overhead_budget_percent = "5.0";
  std::uint64_t configured_value = 0;
  std::uint64_t observed_value = 0;
  std::uint64_t sample_rate_per_mille = 0;
  std::uint64_t persist_stride = 0;
  std::uint64_t skipped_count = 0;
  std::uint64_t dropped_metric_count = 0;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationIparSlowPathReasonSource {
  std::string metric_id = "IPAR-M035";
  std::string statement_id;
  std::string chosen_path = "fast_path";
  std::string reason_code;
  std::string validation_stage;
  std::string driver_visible_message;
  std::string diagnostic_code;
  std::string source_state = "observed";
  std::uint64_t fallback_count = 0;
  std::uint64_t sample_count = 0;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationIparContentionQuotaSource {
  std::string row_id;
  std::string metric_id = "IPAR-P4-06/IPAR-P6-06";
  std::string category;
  std::string subject;
  std::string metric_path;
  std::string metric_type = "gauge";
  std::string metric_unit = "count";
  std::string producer;
  std::string source_kind = "server_observability_metric";
  std::string source_ref;
  std::string provenance = "recorded_metric_hook";
  std::string diagnostic_code;
  std::string source_state = "observed";
  std::uint64_t observed_value = 0;
  std::uint64_t limit_value = 0;
  std::uint64_t refusal_count = 0;
  std::uint64_t wait_count = 0;
  std::uint64_t queue_depth = 0;
  std::uint64_t sample_count = 0;
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationProtectedMaterialSource {
  std::string material_path;
  std::string material_name;
  std::string purpose_class;
  std::string storage_class = "wrapped";
  std::string lifecycle_state = "active";
  std::string active_version_number;
  std::string retention_policy_name;
  std::string access_policy_name;
  std::string release_policy_name;
  std::string purge_policy_name;
  std::string audit_policy_name;
  std::string visibility_state = "security_redacted";
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationProtectedMaterialVersionSource {
  std::string material_path;
  std::string material_name;
  std::string version_number;
  std::string storage_class = "wrapped";
  std::string rotation_state = "current";
  std::string valid_from_state = "visible";
  std::string valid_until_state;
  std::string retention_state = "active";
  std::string purged = "NO";
  std::string payload_hash_present = "YES";
  std::string audit_state = "audit_present";
  std::uint64_t catalog_generation_id = 0;
  bool hidden = false;
};

struct SysInformationProjectionContext {
  std::string source_diagnostic_code;
  std::string source_diagnostic_detail;
  std::string catalog_display_name = "ScratchBird";
  std::string session_language = "en";
  std::string default_language = "en";
  EngineUuid session_uuid;
  std::string principal_name;
  EngineUuid principal_uuid;
  std::string requested_role_name;
  std::string active_role_name;
  EngineUuid active_role_uuid;
  std::vector<std::string> effective_role_names;
  std::vector<EngineUuid> effective_role_uuids;
  std::vector<EngineUuid> effective_group_uuids;
  std::uint64_t visible_catalog_generation_id = 0;
  bool strict_mode = false;
  bool cluster_authority_available = false;
};

struct SysInformationBinaryValue {
  std::string bytes;
  bool operator==(const SysInformationBinaryValue&) const = default;
};

using SysInformationProjectionValue =
    std::variant<std::string, EngineUuid, SysInformationBinaryValue>;

struct SysInformationProjectionRow {
  std::vector<std::pair<std::string, SysInformationProjectionValue>> fields;
};

inline EngineTypedValue SysInformationTypedValue(const SysInformationProjectionValue& value) {
  EngineTypedValue result;
  result.descriptor.descriptor_kind = "scalar";
  if (const auto* text = std::get_if<std::string>(&value)) {
    result.descriptor.canonical_type_name = "text";
    result.encoded_value = *text;
  } else if (const auto* uuid = std::get_if<EngineUuid>(&value)) {
    result.descriptor.canonical_type_name = "uuid";
    result.is_null = uuid->is_nil();
    if (result.is_null) {
      result.state = EngineValueState::sql_null;
    } else {
      result.binary_value.assign(uuid->bytes.begin(), uuid->bytes.end());
    }
  } else {
    result.descriptor.canonical_type_name = "binary";
    const auto& bytes = std::get<SysInformationBinaryValue>(value).bytes;
    result.binary_value.assign(bytes.begin(), bytes.end());
  }
  return result;
}

inline const SysInformationProjectionValue* SysInformationField(
    const SysInformationProjectionRow& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return &field.second; }
  }
  return nullptr;
}

inline bool SysInformationFieldIsNull(const SysInformationProjectionValue* value) {
  if (value == nullptr) { return true; }
  if (const auto* uuid = std::get_if<EngineUuid>(value)) { return uuid->is_nil(); }
  // Existing text projections represent absent text with an empty value.
  if (const auto* text = std::get_if<std::string>(value)) { return text->empty(); }
  return false;
}

// Definitions describe available surfaces; only catalog-resolved names own
// identities. Do not turn a definition path into a fabricated object UUID.
inline EngineUuid SysInformationObjectIdentity(
    const std::vector<SysInformationResolverNameSource>& names,
    const std::map<std::string, EngineUuid>& schemas,
    std::string_view path, std::string_view object_class) {
  const auto dot = path.rfind('.');
  if (dot == std::string_view::npos) { return {}; }
  const auto schema = schemas.find(std::string(path.substr(0, dot)));
  if (schema == schemas.end()) { return {}; }
  EngineUuid found;
  for (const auto& name : names) {
    if (name.object_class != object_class || name.scope_uuid != schema->second ||
        (name.display_name != path.substr(dot + 1) && name.raw_name_text != path.substr(dot + 1))) { continue; }
    if (!found.is_nil() && found != name.object_uuid) { return {}; }
    found = name.object_uuid;
  }
  return found;
}

struct SysInformationProjectionResult {
  bool ok = true;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  EngineUuid diagnostic_identity;
  std::vector<SysInformationProjectionRow> rows;
};

struct SysInformationProjectionValidationResult {
  bool ok = true;
  std::vector<std::string> diagnostic_codes;
};

const char* SysInformationProjectionFamilyName(SysInformationProjectionFamily family);
const char* SysInformationSourceKindName(SysInformationSourceKind source_kind);

const std::vector<SysInformationProjectionDefinition>& BuiltinSysInformationProjectionDefinitions();
const SysInformationProjectionDefinition* FindSysInformationProjectionDefinition(
    std::string_view view_path);
std::string SysInformationCanonicalViewPath(std::string_view view_path);

bool SysInformationPathIsClusterScoped(std::string_view view_path);
bool SysInformationProjectionColumnNameExposesUuid(std::string_view column_name);

SysInformationProjectionValidationResult ValidateSysInformationProjectionDefinition(
    const SysInformationProjectionDefinition& definition);
SysInformationProjectionValidationResult ValidateBuiltinSysInformationProjectionDefinitions();

SysInformationProjectionResult BuildSysInformationProjection(
    std::string_view view_path,
    const SysInformationProjectionContext& context,
    const std::vector<SysInformationCatalogObjectSource>& catalog_objects,
    const std::vector<SysInformationResolverNameSource>& resolver_names,
    const std::vector<SysInformationCommentSource>& comments = {},
    const std::vector<SysInformationDatatypeDescriptorSource>& datatype_descriptors = {},
    const std::vector<SysInformationColumnSource>& columns = {},
    const std::vector<SysInformationSettingSource>& settings = {},
    const std::vector<SysInformationFrontendAgentSource>& frontend_agents = {},
    const std::vector<SysInformationProtectedMaterialSource>& protected_material = {},
    const std::vector<SysInformationProtectedMaterialVersionSource>& protected_material_versions = {},
    const std::vector<SysInformationAgentSource>& agents = {},
    const std::vector<SysInformationAgentMetricDependencySource>& agent_metric_dependencies = {},
    const std::vector<SysInformationAgentPolicySource>& agent_policies = {},
    const std::vector<SysInformationAgentActionSource>& agent_actions = {},
    const std::vector<SysInformationAgentOverrideSource>& agent_overrides = {},
    const std::vector<SysInformationAgentEvidenceSource>& agent_evidence = {},
    const std::vector<SysInformationAgentAuditSource>& agent_audit = {},
    const std::vector<SysInformationFilespaceCapacityAgentStateSource>& filespace_capacity_agent_state = {},
    const std::vector<SysInformationPageAllocationAgentStateSource>& page_allocation_agent_state = {},
    const std::vector<SysInformationFilespaceShrinkReadinessSource>& filespace_shrink_readiness = {},
    const std::vector<SysInformationDomainSource>& domains = {},
    const std::vector<SysInformationIparAgentLifecycleSource>& ipar_agent_lifecycle = {},
    const std::vector<SysInformationIparMetricCounterSource>& ipar_metric_counters = {},
    const std::vector<SysInformationIparTelemetryControlSource>& ipar_telemetry_controls = {},
    const std::vector<SysInformationIparSlowPathReasonSource>& ipar_slow_path_reasons = {},
    const std::vector<SysInformationIparContentionQuotaSource>& ipar_contention_quota = {});

}  // namespace scratchbird::engine::internal_api
