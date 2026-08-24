// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint32_t kEngineSblrEnvelopeMajor = 1;
inline constexpr std::uint32_t kEngineSblrEnvelopeMinor = 0;
inline constexpr std::uint32_t kSblrOperationMagic = 0x504f4253u;
inline constexpr std::uint32_t kSblrOperationTrailerMagic = 0x544f4253u;
inline constexpr std::uint16_t kSblrOperationHeaderSize = 64;
inline constexpr std::uint16_t kSblrOperationSectionCount = 9;
inline constexpr std::uint32_t kSblrOperationSectionTableSize = 216;
inline constexpr std::uint32_t kSblrOperationSectionPayloadOffset = 280;
inline constexpr std::uint64_t kSblrOperationMaximumBytes = 33'554'432;
inline constexpr std::uint32_t kSblrOperationMaximumOperands = 262'144;
inline constexpr std::uint32_t kSblrOperationMaximumValues = 1'048'576;
inline constexpr std::uint32_t kSblrOperationMaximumDepth = 256;
inline constexpr std::uint64_t kSblrOperationMaximumScalarBytes = 65'536;

enum class SblrValueKind : std::uint16_t {
  uuid_ref = 1,
  descriptor_ref = 2,
  policy_ref = 3,
  principal_ref = 4,
  literal_typed = 5,
  parameter_slot = 6,
  result_target = 7,
  proof_token = 8,
  epoch_token = 9,
  profile_ref = 10,
  artifact_ref = 11,
  udr_ref = 12,
  list = 13,
  map = 14,
  null_value = 15,
  expression_node_table = 16,
  expression_node_ref = 17,
  parameter_node_table = 18,
  parameter_node_ref = 19,
  variable_node_table = 20,
  variable_node_ref = 21,
  transaction_begin_options = 22,
  transaction_commit_options = 23,
  transaction_rollback_options = 24,
  savepoint_descriptor = 25,
  savepoint_release_handle = 26,
  savepoint_rollback_handle = 27,
  psql_autonomous_frame_descriptor = 28,
  relation_reservation_release_descriptor = 29,
  temporary_instance_cleanup_descriptor = 30,
  cursor_open_plan_ref = 31,
  cursor_fetch_handle = 32,
  cursor_close_handle = 33,
  read_by_key_descriptor = 34,
  read_range_descriptor = 35,
  read_stream_descriptor = 36,
  result_set_pass_descriptor = 37,
  access_cursor_open_descriptor = 38,
  access_cursor_fetch_descriptor = 39,
  access_cursor_close_descriptor = 40,
  insert_descriptor = 41,
  update_descriptor = 42,
  delete_descriptor = 43,
  merge_descriptor = 44,
  truncate_table_descriptor = 45,
  analyze_table_descriptor = 46,
  bulk_import_stream_descriptor = 47,
  bulk_export_stream_descriptor = 48,
  statement_batch_descriptor = 49,
  atomic_cas_descriptor = 50,
  atomic_rmw_descriptor = 51,
  advisory_lock_descriptor = 52,
  advisory_lock_release_descriptor = 53,
  function_call_descriptor = 54,
  operator_call_descriptor = 55,
  cast_descriptor = 56,
  comparison_descriptor = 57,
  domain_operation_descriptor = 58,
  registered_cpp_udr_invocation = 59,
  procedure_invoke_descriptor = 60,
  function_invoke_descriptor = 61,
  aggregate_invoke_descriptor = 62,
  sequence_nextval_descriptor = 63,
  sequence_currval_descriptor = 64,
  privilege_template_descriptor = 110,
  privilege_template_alter_descriptor = 111,
  privilege_template_drop_descriptor = 112,
  template_database_creation_descriptor = 113,
  ddl_create_aggregate_descriptor = 114,
  ddl_alter_aggregate_descriptor = 115,
  ddl_drop_aggregate_descriptor = 116,
  system_history_purge_descriptor = 117,
  index_optimizer_eligibility_descriptor = 118,
  table_type_enforcement_descriptor = 119,
  logical_snapshot_serialization_descriptor = 120,
  logical_snapshot_deserialization_descriptor = 121,
  macro_descriptor = 122,
  macro_drop_descriptor = 123,
  external_relation_resolver_registration_descriptor = 124,
  external_relation_resolver_unregistration_descriptor = 125,
  external_dictionary_descriptor = 126,
  external_dictionary_drop_descriptor = 127,
  external_dictionary_alter_descriptor = 130,
  continuous_view_descriptor = 131,
  continuous_view_alter_descriptor = 132,
  continuous_view_drop_descriptor = 133,
  async_insert_submission_descriptor = 134,
  async_insert_status_descriptor = 135,
  async_insert_cancel_descriptor = 136,
  conditional_mutation_descriptor = 137,
  counter_delta_descriptor = 138,
  timeseries_schema_write_descriptor = 139,
  timeseries_series_cardinality_policy_descriptor = 140,
  timeseries_value_cache_descriptor = 141,
  alter_sequence_descriptor = 143,
  create_materialized_view_descriptor = 144,
  create_type_descriptor = 145,
  alter_type_descriptor = 146,
  drop_materialized_view_descriptor = 147,
  drop_type_descriptor = 148,
  refresh_materialized_view_descriptor = 149,
  drop_table_descriptor = 152,
  drop_synonym_descriptor = 158,
  sequence_setval_descriptor = 65,
  numeric_operation_descriptor = 66,
  advanced_datatype_family_descriptor = 67,
  projection_descriptor = 68,
  aggregate_descriptor = 69,
  group_descriptor = 70,
  sort_descriptor = 71,
  limit_descriptor = 72,
  window_descriptor = 73,
  result_set_return_descriptor = 74,
  kv_structured_read_descriptor = 75,
  kv_structured_mutate_descriptor = 76,
  kv_structured_scan_descriptor = 77,
  kv_structured_stream_read_descriptor = 78,
  kv_structured_stream_append_descriptor = 79,
  kv_structured_timeseries_descriptor = 80,
  system_config_set_descriptor = 81,
  create_domain_descriptor = 82,
  create_schema_descriptor = 83,
  create_table_descriptor = 84,
  create_table_as_query_with_data_descriptor = 200,
  create_table_as_query_with_no_data_descriptor = 201,
  create_index_descriptor = 85,
  drop_index_descriptor = 86,
  alter_domain_descriptor = 87,
  create_view_descriptor = 88,
  alter_view_descriptor = 89,
  drop_view_descriptor = 90,
  create_trigger_descriptor = 91,
  alter_trigger_descriptor = 92,
  drop_trigger_descriptor = 93,
  create_procedure_descriptor = 94,
  alter_procedure_descriptor = 95,
  drop_procedure_descriptor = 96,
  create_function_descriptor = 97,
  alter_function_descriptor = 98, drop_function_descriptor = 99, create_synonym_descriptor = 156, create_foreign_table_descriptor = 160, create_fdw_descriptor = 161, drop_foreign_table_descriptor = 159, drop_fdw_descriptor = 162, security_create_user_descriptor = 163, create_package_descriptor = 100, create_temporary_table_descriptor = 101,
  alter_user_descriptor = 164,
  security_create_role_descriptor = 165,
  security_drop_role_descriptor = 166,
  security_create_policy_descriptor = 167,
  security_drop_policy_descriptor = 168,
  security_alter_role_descriptor = 169,
  drop_temporary_table_descriptor = 102,
  drop_package_descriptor = 142,
  alter_package_descriptor = 150,
  create_sequence_descriptor = 151,
  drop_sequence_descriptor = 154,
  object_rename_vector_descriptor = 103,
  rename_object_descriptor = 155,
  spatial_reference_system_descriptor = 104,
  spatial_reference_system_drop_descriptor = 105,
  rewrite_rule_descriptor = 106,
  rewrite_rule_alter_descriptor = 107,
  rewrite_rule_drop_descriptor = 108,
  constraint_validation_descriptor = 109,
  observability_show_version_descriptor = 128,
  catalog_introspect_descriptor = 129,
};

struct SblrOperand {
  std::string type;
  std::string name;
  std::string value;
  std::uint32_t ordinal = 0;
  SblrValueKind value_kind = SblrValueKind::null_value;
  std::uint16_t value_flags = 0;
  std::vector<std::uint8_t> value_body;
};

struct SblrSourceSymbolArtifact {
  std::string symbol_kind;
  std::string stable_key;
  std::string resolved_uuid;
  std::string render_hint;
  std::string scope;
  std::string source_hash;
  bool authoritative = false;
  bool contains_sql_text = false;
};

struct SblrOperationRenderHint {
  std::string hint_kind;
  std::string stable_key;
  std::string value;
  bool authoritative = false;
  bool contains_sql_text = false;
};

struct SblrSourceArtifactMap {
  std::string policy_status = "absent";
  std::string source_identity;
  std::string source_hash;
  std::string artifact_format = "sblr.source_artifact_map.v1";
  bool render_metadata_only = true;
  bool contains_sql_text = false;
  bool raw_sql_text_authoritative = false;
  std::vector<SblrSourceSymbolArtifact> symbols;
  std::vector<SblrOperationRenderHint> operation_render_hints;
};

struct SblrOperationEnvelope {
  std::uint32_t envelope_major = kEngineSblrEnvelopeMajor;
  std::uint32_t envelope_minor = kEngineSblrEnvelopeMinor;
  std::uint16_t opcode_code = 0;
  std::uint16_t operation_version_major = 1;
  std::uint16_t operation_version_minor = 0;
  std::string operation_id;
  std::string opcode;
  std::string result_shape;
  std::string diagnostic_shape;
  std::string parser_package_uuid;
  std::uint32_t parser_package_version_major = 1;
  std::uint32_t parser_package_version_minor = 0;
  std::uint32_t parser_package_version_patch = 0;
  std::string registry_snapshot_uuid;
  std::string trace_key;
  std::vector<SblrOperand> operands;
  SblrSourceArtifactMap source_artifact_map;
  bool contains_sql_text = false;
  bool parser_resolved_names_to_uuids = false;
  bool requires_security_context = true;
  bool requires_transaction_context = false;
  bool requires_cluster_authority = false;
};

struct SblrEnvelopeDiagnostic {
  std::string code;
  std::string message;
  bool error = true;
};

struct SblrEnvelopeValidationResult {
  bool ok = false;
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

struct SblrDecodeResult {
  bool ok = false;
  SblrOperationEnvelope envelope;
  std::vector<std::uint8_t> canonical_bytes;
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

SblrOperationEnvelope MakeSblrEnvelope(std::string operation_id,
                                       std::string opcode,
                                       std::string trace_key = {});
SblrEnvelopeValidationResult ValidateSblrEnvelope(const SblrOperationEnvelope& envelope);
SblrDecodeResult DecodeSblrEnvelope(std::string_view encoded);
std::string EncodeSblrEnvelope(const SblrOperationEnvelope& envelope);
std::uint32_t SblrCrc32c(const std::uint8_t* data, std::size_t size) noexcept;
std::string SerializeSblrEnvelopeToJson(const SblrOperationEnvelope& envelope);
std::string SerializeSblrValidationToJson(const SblrEnvelopeValidationResult& result);

}  // namespace scratchbird::engine::sblr
