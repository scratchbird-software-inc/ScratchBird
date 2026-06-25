// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_BASE_TYPES
// Engine-owned API types. This API is not SQL authority and is not a parser API.

using EngineApiU64 = std::uint64_t;
using EngineApiI64 = std::int64_t;

struct EngineUuid {
  std::string canonical;
};

struct EngineObjectReference {
  EngineUuid uuid;
  std::string object_kind;
};

struct EngineLocalizedName {
  std::string language_tag;
  std::string name_class;
  std::string path;
  std::string name;
  bool default_name = false;
  std::string reference_id;
  std::string dialect_profile_uuid;
  std::string identifier_profile_uuid;
  std::string case_fold_profile_uuid;
  std::string quoted_identifier_profile_uuid;
  std::string raw_name_text;
  std::string display_name;
  bool was_quoted = false;
  std::string quote_style;
  bool requires_exact_match = false;
  std::string normalized_lookup_key;
  std::string exact_lookup_key;
  std::string full_path_lookup_key;
};

struct EngineIdentifierResolutionProfile {
  EngineUuid identifier_profile_uuid;
  std::string profile_name;
  std::string unquoted_fold_mode;
  std::string comparison_mode;
  std::string quote_styles;
  bool quoted_preserves_case = true;
  std::string storage_display_mode;
};

struct EngineLanguageContext {
  std::string language_profile_id = "sbsql.builtin.recovery.en";
  std::string language_tag = "en";
  std::string default_language_tag = "en";
  std::string input_syntax_profile = "sbsql.syntax.standard";
  std::string input_language_fallback_tag;
  std::string common_resource_hash = "builtin.common.sbsql.v1";
  EngineApiU64 language_resource_epoch = 0;
  EngineApiU64 localized_name_epoch = 0;
  EngineApiU64 message_resource_epoch = 0;
  std::string resource_compatibility_identity = "sbsql.resource.compat.v1";
  std::string resource_version_identity = "sbsql.resource-pack.v1";
};

struct EngineIdentifierAtom {
  std::string raw_text;
  bool was_quoted = false;
  std::string quote_style;
  std::string identifier_profile_uuid;
  std::string normalized_lookup_key;
  std::string exact_lookup_key;
  bool requires_exact_match = false;
  std::string source_span;
};

struct EngineSqlObjectReference {
  std::string expected_object_type;
  std::string path_type = "unqualified";
  bool no_search_path = false;
  std::vector<EngineIdentifierAtom> path_components;
  EngineIdentifierAtom object_name;
};

struct EngineBoundObjectIdentity {
  EngineUuid object_uuid;
  std::string resolved_object_type;
  EngineUuid resolved_schema_uuid;
  EngineUuid parent_object_uuid;
  EngineApiU64 catalog_generation_id = 0;
  EngineApiU64 security_epoch = 0;
  EngineApiU64 resource_epoch = 0;
};

struct EngineDescriptor {
  EngineUuid descriptor_uuid;
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_descriptor;
};

// SEARCH_KEY: EDR_ENGINE_VALUE_STATE
enum class EngineValueState : std::uint8_t {
  value = 0,
  sql_null = 1,
  missing = 2,
  default_requested = 3,
  unknown = 4,
  error = 5,
  lob_handle = 6,
  protected_value = 7
};

constexpr bool EngineValueStateIsSqlNull(EngineValueState state) noexcept {
  return state == EngineValueState::sql_null;
}

constexpr bool EngineValueStateHasPayload(EngineValueState state) noexcept {
  switch (state) {
    case EngineValueState::value:
    case EngineValueState::error:
    case EngineValueState::lob_handle:
    case EngineValueState::protected_value:
      return true;
    case EngineValueState::sql_null:
    case EngineValueState::missing:
    case EngineValueState::default_requested:
    case EngineValueState::unknown:
      return false;
  }
  return false;
}

struct EngineTypedValue {
  EngineDescriptor descriptor;
  std::string encoded_value;
  std::vector<std::uint8_t> binary_value;
  bool is_null = false;
  EngineValueState state = EngineValueState::value;

  EngineTypedValue() = default;

  EngineTypedValue(EngineDescriptor descriptor,
                   std::string encoded_value,
                   bool is_null = false)
      : descriptor(std::move(descriptor)),
        encoded_value(std::move(encoded_value)),
        is_null(is_null),
        state(is_null ? EngineValueState::sql_null : EngineValueState::value) {}

  bool isSqlNull() const noexcept {
    return EngineValueStateIsSqlNull(state) ||
           (state == EngineValueState::value && is_null);
  }

  bool isNull() const noexcept {
    return isSqlNull();
  }

  bool hasPayload() const noexcept {
    return EngineValueStateHasPayload(state) && !isSqlNull();
  }

  void setState(EngineValueState new_state) noexcept {
    state = new_state;
    is_null = EngineValueStateIsSqlNull(new_state);
  }
};

struct EngineColumnDefinition {
  EngineUuid requested_column_uuid;
  std::vector<EngineLocalizedName> names;
  EngineDescriptor descriptor;
  std::string default_expression_envelope;
  std::uint32_t ordinal = 0;
  bool nullable = true;
};

struct EngineConstraintDefinition {
  EngineUuid requested_constraint_uuid;
  std::vector<EngineLocalizedName> names;
  std::string constraint_kind;
  std::string canonical_constraint_envelope;
};

struct EngineIndexDefinition {
  EngineUuid requested_index_uuid;
  std::vector<EngineLocalizedName> names;
  std::string index_kind;
  std::vector<std::string> key_envelopes;
  std::string physical_profile;
};

struct EngineProfileSet {
  std::vector<std::string> names;
  std::vector<std::string> encoded_profiles;
};

struct EnginePredicateEnvelope {
  std::string predicate_kind;
  std::string canonical_predicate_envelope;
  std::vector<EngineTypedValue> bound_values;
};

struct EngineProjectionEnvelope {
  std::vector<std::string> canonical_projection_envelopes;
};

struct EngineOrderingEnvelope {
  std::vector<std::string> canonical_ordering_envelopes;
};

struct EngineRowValue {
  EngineUuid requested_row_uuid;
  std::vector<std::pair<std::string, EngineTypedValue>> fields;
};

struct EngineResultShape {
  std::string result_kind;
  std::vector<EngineDescriptor> columns;
  std::vector<EngineRowValue> rows;
};

struct EngineUnsupportedFeature {
  std::string feature;
  std::string reason;
};

struct EngineEvidenceReference {
  std::string evidence_kind;
  std::string evidence_id;
};

struct EngineAuthorizationSubject {
  EngineUuid subject_uuid;
  std::string subject_kind;
};

struct EngineMaterializedAuthorizationGrant {
  EngineUuid grant_uuid;
  EngineUuid subject_uuid;
  std::string subject_kind;
  EngineUuid target_uuid;
  std::string right;
  bool deny = false;
  EngineApiU64 security_epoch = 0;
};

struct EngineMaterializedAuthorizationPolicy {
  EngineUuid policy_uuid;
  EngineUuid subject_uuid;
  std::string subject_kind;
  EngineUuid target_uuid;
  std::string right;
  std::string policy_kind;
  bool deny = false;
  bool requires_runtime_recheck = false;
  EngineApiU64 policy_epoch = 0;
  std::string canonical_policy_envelope;
};

struct EngineMaterializedAuthorizationContext {
  bool present = false;
  EngineUuid authority_uuid;
  EngineUuid principal_uuid;
  EngineApiU64 security_epoch = 0;
  EngineApiU64 policy_epoch = 0;
  EngineApiU64 catalog_generation_id = 0;
  std::vector<EngineAuthorizationSubject> effective_subjects;
  std::vector<EngineMaterializedAuthorizationGrant> grants;
  std::vector<EngineMaterializedAuthorizationPolicy> policies;
  std::vector<std::string> evidence_tags;
};

enum class EngineTrustMode {
  server_isolated,
  embedded_in_process,
};

struct EngineRequestContext {
  EngineTrustMode trust_mode = EngineTrustMode::server_isolated;
  std::string request_id;
  std::string database_path;
  EngineUuid database_uuid;
  EngineUuid cluster_uuid;
  EngineUuid node_uuid;
  EngineUuid principal_uuid;
  EngineUuid session_uuid;
  EngineUuid transaction_uuid;
  EngineUuid statement_uuid;
  EngineApiU64 local_transaction_id = 0;
  std::string transaction_isolation_level = "read_committed";
  std::string current_sqlstate = "00000";
  EngineUuid current_diagnostic_uuid;
  std::string client_protocol_uuid;
  std::string application_name;
  EngineApiU64 snapshot_visible_through_local_transaction_id = 0;
  std::string statement_timestamp;
  std::string transaction_timestamp;
  std::string current_timestamp;
  std::string current_monotonic_ns;
  std::string deterministic_random_bytes_hex;
  std::string deterministic_uuid_text;
  EngineApiU64 deterministic_random_u64 = 0;
  bool deterministic_random_u64_present = false;
  bool security_context_present = false;
  bool cluster_authority_available = false;
  bool read_only_mode = false;
  EngineLanguageContext language_context;
  EngineUuid default_root_uuid;
  EngineUuid current_schema_uuid;
  EngineUuid current_role_uuid;
  EngineUuid current_package_uuid;
  std::vector<EngineUuid> search_path_schema_uuids;
  std::string identifier_profile_uuid = "sbsql_v3";
  std::string reference_profile_uuid;
  EngineApiU64 catalog_generation_id = 0;
  EngineApiU64 security_epoch = 0;
  EngineApiU64 resource_epoch = 0;
  EngineApiU64 name_resolution_epoch = 0;
  EngineApiU64 last_row_count = 0;
  bool last_row_count_present = false;
  EngineMaterializedAuthorizationContext authorization_context;
  std::vector<std::string> trace_tags;
};

struct EngineApiDiagnostic {
  std::string code;
  std::string message_key;
  std::string detail;
  bool error = true;
};

struct EngineDmlSummaryCounters {
  EngineApiU64 rows_changed = 0;
  EngineApiU64 visible_rows_scanned = 0;
  EngineApiU64 index_probes = 0;
  EngineApiU64 append_calls = 0;
  EngineApiU64 file_opens = 0;
  EngineApiU64 flushes = 0;
  EngineApiU64 page_reservations = 0;
  EngineApiU64 row_extent_reservations = 0;
  EngineApiU64 version_extent_reservations = 0;
  EngineApiU64 page_extent_reservations = 0;
  EngineApiU64 index_extent_reservations = 0;
  EngineApiU64 preallocation_requests = 0;
  EngineApiU64 preallocation_granted_pages = 0;
  EngineApiU64 preallocation_capped = 0;
  EngineApiU64 preallocation_refused = 0;
  std::vector<std::string> fallback_reasons;
  bool benchmark_clean = true;
};

struct EngineApiRequest {
  EngineRequestContext context;
  std::string operation_id;
  EngineObjectReference target_database;
  EngineObjectReference target_schema;
  EngineObjectReference target_object;
  std::vector<EngineObjectReference> related_objects;
  std::vector<EngineLocalizedName> localized_names;
  EngineSqlObjectReference sql_object_reference;
  EngineBoundObjectIdentity bound_object_identity;
  std::vector<EngineDescriptor> descriptors;
  std::vector<EngineColumnDefinition> columns;
  std::vector<EngineConstraintDefinition> constraints;
  std::vector<EngineIndexDefinition> indexes;
  std::vector<EngineRowValue> rows;
  std::vector<std::string> shared_row_field_order;
  std::vector<std::pair<std::string, EngineTypedValue>> assignments;
  EnginePredicateEnvelope predicate;
  EngineProjectionEnvelope projection;
  EngineOrderingEnvelope ordering;
  EngineProfileSet physical_profile;
  EngineProfileSet policy_profile;
  EngineProfileSet compatibility_profile;
  std::vector<std::string> option_envelopes;
  std::vector<std::string> diagnostic_options;
};

struct EngineApiResult {
  bool ok = false;
  std::string operation_id;
  std::vector<EngineApiDiagnostic> diagnostics;
  std::vector<EngineUnsupportedFeature> unsupported_features;
  std::vector<EngineEvidenceReference> evidence;
  EngineResultShape result_shape;
  EngineObjectReference primary_object;
  EngineUuid catalog_row_uuid;
  EngineUuid transaction_uuid;
  EngineApiU64 local_transaction_id = 0;
  EngineDmlSummaryCounters dml_summary;
  bool embedded_trust_mode_observed = false;
  bool cluster_authority_required = false;
};

}  // namespace scratchbird::engine::internal_api
