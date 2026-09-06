// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "query/contextual_text_literal_authority.hpp"
#include "scratchbird/engine/engine.h"

#include <cstdint>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
class SblrBulkImportStreamRegistry;
}

namespace scratchbird::server_engine_bridge {

// Private server-to-engine handle. The monotonically issued value is opaque to
// the server, is never reused, and identifies engine-owned receipt state. It is
// not part of the public C ABI and must never be encoded into SBLR.
struct StatementContextReceiptHandle {
  std::uint64_t opaque_id = 0;

  [[nodiscard]] explicit operator bool() const { return opaque_id != 0; }
  friend bool operator==(const StatementContextReceiptHandle&,
                         const StatementContextReceiptHandle&) = default;
};

enum class StatementParameterExecutionMode : std::uint8_t {
  kDirect = 0,
  kPrepared = 1,
  kBatch = 2,
  kDynamic = 3,
};

// Private delivery intent selected by the authenticated server request. This
// is not serialized into SBLR and cannot be supplied by a parser operand.
enum class StatementContextResultDeliveryMode : std::uint8_t {
  kDirect = 0,
  kCursorRetainedResult = 1,
};

struct StatementParameterExecutionSelectorV1 {
  std::uint16_t version = 1;
  StatementParameterExecutionMode mode =
      StatementParameterExecutionMode::kDirect;
  std::uint8_t reserved = 0;
  std::uint64_t prepared_binding_handle = 0;
  std::uint64_t batch_execution_handle = 0;
  std::uint64_t dynamic_package_handle = 0;
};

struct StatementParameterCoordinationBeginRequestV1 {
  sb_engine_session_t engine_session = nullptr;
  const scratchbird::engine::internal_api::EngineRequestContext*
      engine_context = nullptr;
  StatementParameterExecutionMode mode =
      StatementParameterExecutionMode::kDirect;
  std::string operation_uuid;
  std::string public_prepared_uuid;
  std::string public_dynamic_package_uuid;
};

struct StatementParameterCoordinationViewV1 {
  std::uint64_t private_handle = 0;
  std::string public_coordination_uuid;
  std::string operation_uuid;
  std::uint64_t coordinator_generation = 0;
  std::string prepared_statement_uuid;
  std::uint64_t prepared_generation = 0;
  std::string batch_uuid;
  std::uint64_t batch_generation = 0;
  std::string dynamic_package_uuid;
  std::uint64_t dynamic_generation = 0;
};

sb_engine_status_t BeginStatementParameterExecutionCoordinationV1(
    const StatementParameterCoordinationBeginRequestV1* request,
    StatementParameterCoordinationViewV1* out_view,
    sb_engine_result_t* out_result);
sb_engine_status_t SealPreparedStatementParameterTemplateV1(
    std::uint64_t private_handle,
    const std::vector<std::uint8_t>& canonical_sbpt,
    StatementParameterCoordinationViewV1* out_view,
    sb_engine_result_t* out_result);

enum class StatementDescriptorProfileKind : std::uint8_t {
  kNumericNonNull = 1,
  kNumericNullable = 2,
  kTextNonNull = 3,
  kTextNullable = 4,
  kBooleanNonNull = 5,
  kBooleanNullable = 6,
  kJsonNonNull = 7,
  kJsonNullable = 8,
  kTextListNonNull = 9,
  kTextListNullable = 10,
  kReal64NonNull = 11,
  kUuidNonNull = 12,
  kUint64NonNull = 13,
  kMultilegUuidNonNull = 14,
  kMultilegUuidNullable = 15,
  kMultilegUint64NonNull = 16,
  kMultilegUint64Nullable = 17,
  kMultilegReal64NonNull = 18,
  kMultilegReal64Nullable = 19,
  kMultilegBooleanNonNull = 20,
  kMultilegBooleanNullable = 21,
  kMultilegGeometryNonNull = 22,
  kMultilegGeometryNullable = 23,
};

struct StatementDescriptorProfile {
  StatementDescriptorProfileKind profile_kind =
      StatementDescriptorProfileKind::kNumericNonNull;
  std::uint16_t slot = 0;
  std::string descriptor_uuid;
  std::string type_uuid;
  std::string collation_uuid;
  bool nullable = false;
  std::uint32_t width = 0;
  std::uint32_t precision = 0;
  std::uint32_t scale = 0;
};

struct StatementAggregateFunctionProfile {
  std::uint16_t abi_version = 0;
  std::string builtin_id;
  std::string function_uuid;
  bool executable = false;
};

struct StatementWindowFunctionProfile {
  std::uint16_t abi_version = 0;
  std::string builtin_id;
  std::string function_uuid;
  bool executable = false;
};

// Engine-issued relation authority for one structural occurrence.  This is
// private bridge state; parser text, AST fields, and generic descriptor
// profiles must never populate it.
struct StatementRelationOccurrenceMappingV1 {
  std::uint64_t occurrence_id = 0;
  std::string persisted_descriptor_uuid;
  std::uint64_t persisted_descriptor_generation = 0;
};

// Syntax-only COPY selector admitted by the private statement bridge.  None of
// these fields is object, descriptor, MGA, policy, route, or resource
// authority.  The exact request bytes are retained solely for canonical replay
// comparison after the engine has independently derived the authority row.
struct StatementBulkImportNameAtomV1 {
  std::string raw_text;
  bool quoted = false;
};

struct StatementBulkImportBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::string command_surface_id;
  std::uint64_t structural_occurrence = 0;
  std::uint32_t import_occurrence = 0;
  std::vector<StatementBulkImportNameAtomV1> target_name_atoms;
  std::uint8_t input_format_demand = 0;
  std::uint8_t character_encoding_demand = 0;
  std::uint8_t conversion_policy_demand = 0;
  std::uint8_t null_default_policy_demand = 0;
  std::uint8_t reject_policy_demand = 0;
  std::uint64_t maximum_rejected_rows = 0;
  std::array<std::uint8_t, 32> syntax_demand_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementBulkImportBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::uint64_t structural_occurrence = 0;
  std::uint32_t import_occurrence = 0;
  std::array<std::uint8_t, 32> syntax_demand_sha256{};
  std::array<std::uint8_t, 32> binding_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  // Private bridge diagnostics are retained directly because the C result
  // handle may cross a static/shared engine boundary before the server reads
  // it.  Success leaves these fields empty; no diagnostic data is serialized
  // into the canonical BindAck payload.
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

// Complete engine-produced COPY authority.  This type is available only to
// authenticated in-process bridge coordinators and is never projected through
// StatementContextReceiptView, SBPS, SBOP, or SBLR.
struct StatementBulkImportAuthorityV1 {
  StatementBulkImportBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::vector<StatementBulkImportNameAtomV1> target_name_atoms;
  std::string admitted_command_surface_id;
  std::string target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  std::string owning_transaction_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  std::string statement_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::uint64_t catalog_generation = 0;
  std::string security_context_uuid;
  std::uint64_t security_epoch = 0;
  std::string row_shape_uuid;
  std::uint64_t row_shape_generation = 0;
  std::array<std::uint8_t, 32> column_descriptor_set_sha256{};
  std::string import_policy_snapshot_uuid;
  std::uint64_t import_policy_generation = 0;
  std::array<std::uint8_t, 32> import_policy_bundle_sha256{};
  std::string import_route_snapshot_uuid;
  std::uint64_t import_route_generation = 0;
  std::string resource_grant_uuid;
  std::uint64_t resource_grant_generation = 0;
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t maximum_stream_bytes = 0;
  std::uint64_t maximum_chunk_count = 0;
  std::uint32_t maximum_chunk_bytes = 0;
  std::uint64_t maximum_affected_plus_rejected_rows = 0;
  std::uint32_t maximum_target_columns = 0;
  bool cluster_bound = false;
  std::uint64_t cluster_epoch = 0;
  std::string cluster_fence_uuid;
  scratchbird::engine::internal_api::EngineMaterializedAuthorizationContext
      authorization_observation;
};

// Syntax-only input for PREPARE. The nested body is already a canonical
// parser submission produced under this same receipt; it is re-decoded and
// rebound by the engine before any prepared identity is issued.
struct StatementPrepareBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  std::vector<std::uint8_t> declared_parameter_type_demands;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementPrepareBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string statement_name_uuid;
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementPrepareAuthorityV1 {
  StatementPrepareBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::string canonical_statement_name;
  bool quoted = false;
  std::string body_operation_id;
  std::string body_operation_family;
  std::string body_result_shape;
  bool source_free_parameterless_query_template = false;
  bool source_free_parameterized_query_template = false;
  std::string parameter_set_uuid;
  // The parameter coordinator owns this UUID.  It is deliberately distinct
  // from canonical_statement_name, which is the session-visible SBsql name.
  std::string parameter_prepared_statement_uuid;
  std::uint64_t parameter_set_generation = 0;
  std::uint64_t parameter_prepared_generation = 0;
  std::string parameter_set_snapshot_uuid;
  std::uint64_t parameter_set_snapshot_generation = 0;
  std::string ordered_slot_table_sha256;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
};

struct StatementExecuteDirectBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_parameter_bytes;
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementExecuteDirectBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string result_descriptor_uuid;
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementExecuteDirectAuthorityV1 {
  StatementExecuteDirectBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::string body_operation_id;
  std::string body_operation_family;
  std::string body_result_shape;
  std::string result_handle_uuid;
  std::string operation_evidence_uuid;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_parameter_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  scratchbird::engine::internal_api::EngineApiResult terminal_api_result;
  bool terminal_result_published = false;
};

struct StatementQueryExplainBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  bool verbose = false;
  std::uint8_t format = 1;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementQueryExplainBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string explain_uuid;
  std::array<std::uint8_t, 32> canonical_query_sblr_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementQueryExplainAuthorityV1 {
  StatementQueryExplainBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  std::vector<std::uint8_t> canonical_plan_material;
  std::string body_operation_id;
  std::string body_operation_family;
  std::string body_result_shape;
  bool verbose = false;
  std::uint8_t format = 1;
  bool terminal_result_published = false;
};

struct StatementNameResolveNameAtomV1 {
  std::string raw_text;
  bool quoted = false;
};

struct StatementNameResolveBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::uint8_t resolution_mode = 0;
  std::uint8_t object_class = 0;
  std::vector<StatementNameResolveNameAtomV1> target_name_atoms;
  std::vector<StatementNameResolveNameAtomV1> namespace_name_atoms;
  std::array<std::uint8_t, 32> target_name_atoms_sha256{};
  std::array<std::uint8_t, 32> namespace_name_atoms_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementNameResolveBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string resolution_uuid;
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementNameResolveAuthorityV1 {
  StatementNameResolveBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::vector<StatementNameResolveNameAtomV1> target_name_atoms;
  std::vector<StatementNameResolveNameAtomV1> namespace_name_atoms;
  std::uint8_t resolution_mode = 0;
  std::uint8_t object_class = 0;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::string redaction_profile_uuid;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

// Syntax-only private bind for PARSE TEXT. The raw UTF-8 input never enters
// the executable descriptor: it is retained under the opaque receipt only so
// exact replay/conflict checks can be performed against the authenticated
// request. The nested SBLR carriers are parser-produced and engine-validated
// under this same receipt before SPTD is issued.
struct StatementParseTextBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string language_profile_id;
  std::string canonical_input_utf8;
  std::uint32_t requested_maximum_bytes = 0;
  std::uint16_t requested_maximum_depth = 0;
  bool allow_donor_extensions = false;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::array<std::uint8_t, 32> canonical_input_sha256{};
  std::array<std::uint8_t, 32> canonical_container_sha256{};
  std::array<std::uint8_t, 32> canonical_execution_envelope_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementParseTextBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string parse_uuid;
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> canonical_input_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementParseTextAuthorityV1 {
  StatementParseTextBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::string language_profile_id;
  std::string canonical_input_utf8;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

struct StatementCatalogEpochCheckNameAtomV1 {
  std::string raw_text;
  bool quoted = false;
};

// Syntax-only private bind for CATALOG EPOCH CHECK. The parser may select the
// database scope with no atoms or present one qualified identifier. It never
// supplies an epoch, object UUID, schema-tree identity, or visibility result.
struct StatementCatalogEpochCheckBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  bool object_scoped = false;
  std::vector<StatementCatalogEpochCheckNameAtomV1> target_name_atoms;
  std::array<std::uint8_t, 32> target_name_atoms_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementCatalogEpochCheckBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string check_uuid;
  std::string object_uuid;
  std::uint64_t object_generation = 0;
  std::string schema_tree_uuid;
  std::uint64_t schema_tree_generation = 0;
  std::array<std::uint8_t, 32> visibility_scope_sha256{};
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementCatalogEpochCheckAuthorityV1 {
  StatementCatalogEpochCheckBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::vector<StatementCatalogEpochCheckNameAtomV1> target_name_atoms;
  std::string object_uuid;
  std::uint64_t object_generation = 0;
  std::string schema_tree_uuid;
  std::uint64_t schema_tree_generation = 0;
  std::string redaction_profile_uuid;
  std::uint64_t redaction_generation = 0;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

struct StatementDatabaseAttachNameAtomV1 {
  std::string raw_text;
  bool quoted = false;
};

// Syntax-only private bind for DATABASE ATTACH REGISTERED. The parser may
// present only the registered-storage spelling, alias spelling, access mode,
// and session alias scope. Every storage, database, catalog, transaction, and
// attachment identity is engine-issued under the same live receipt.
struct StatementDatabaseAttachBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::uint8_t mode = 0;
  std::uint8_t alias_scope = 0;
  StatementDatabaseAttachNameAtomV1 storage_reference;
  StatementDatabaseAttachNameAtomV1 database_alias;
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementDatabaseAttachBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string attach_uuid;
  std::string storage_uuid;
  std::string alias_uuid;
  std::string database_uuid;
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementDatabaseAttachAuthorityV1 {
  StatementDatabaseAttachBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  StatementDatabaseAttachNameAtomV1 storage_reference;
  StatementDatabaseAttachNameAtomV1 database_alias;
  std::uint8_t mode = 0;
  std::uint8_t alias_scope = 0;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

// Receipt-private retention for one canonical external SAM1 artifact.  The
// parser receives only the byte-identical acknowledgement and later copies the
// immutable reference tuple into SBEE fields 24/25/26.  Raw artifact bytes are
// never projected through StatementContextReceiptView.
struct StatementSourceArtifactReferenceV1 {
  std::array<std::uint8_t, 16> sblr_envelope_uuid{};
  std::array<std::uint8_t, 16> artifact_uuid{};
  std::uint64_t declared_size = 0;
  std::uint32_t crc32c = 0;
  std::uint8_t checksum_kind = 0;
  std::uint32_t checksum_crc32c = 0;
  std::array<std::uint8_t, 32> checksum_sha256{};
  std::uint16_t redaction_class = 0;
};

struct StatementSourceArtifactRetentionV1 {
  StatementSourceArtifactReferenceV1 reference;
  std::uint8_t decompile_policy = 0;
  std::uint64_t retention_generation = 0;
  std::vector<std::uint8_t> exact_retain_request_bytes;
  std::vector<std::uint8_t> exact_retain_ack_bytes;
  std::vector<std::uint8_t> canonical_artifact_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

// Receipt-private authority for the narrow catalog statistics reader.  The
// public descriptor exposes only immutable statement/catalog/security/
// resource identities and aggregate MGA counters; it is not an optimizer
// capability or plan handle.
struct StatementOptimizerStatsReadAuthorityV1 {
  std::uint64_t occurrence = 0;
  std::string statistics_snapshot_uuid;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

// Receipt-private authority for one optimizer-statistics epoch advance. The
// exact descriptor is engine-issued after management authorization and is the
// sole key accepted by the durable statistics journal.
struct StatementOptimizerStatsDropAuthorityV1 {
  std::uint64_t occurrence = 0;
  std::string effect_uuid;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

struct StatementExecuteBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  std::vector<std::uint8_t> canonical_parameter_bytes;
  std::vector<std::uint8_t> exact_request_bytes;
};

struct StatementExecuteAuthorityV1 {
  std::uint64_t occurrence = 0;
  std::string canonical_statement_name;
  std::vector<std::uint8_t> exact_request_bytes;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::string body_operation_id;
  std::string body_operation_family;
  std::string body_result_shape;
  bool source_free_parameterless_query_template = false;
  bool source_free_parameterized_query_template = false;
  std::string parameter_set_uuid;
  std::uint64_t parameter_set_generation = 0;
  std::string parameter_set_snapshot_uuid;
  std::uint64_t parameter_set_snapshot_generation = 0;
  std::string ordered_slot_table_sha256;
  std::string parameter_binding_receipt_uuid;
  std::string parameter_binding_execution_uuid;
  std::string parameter_value_sha256;
  std::vector<std::uint8_t> canonical_parameter_bytes;
  std::string result_handle_uuid;
  std::string operation_evidence_uuid;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  scratchbird::engine::internal_api::EngineApiResult terminal_api_result;
  bool terminal_result_published = false;
};

struct StatementFreeBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementFreeBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string statement_uuid;
  std::string statement_name_uuid;
  std::uint64_t prepared_generation = 0;
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementFreeAuthorityV1 {
  StatementFreeBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::string canonical_statement_name;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
};

struct StatementCancelBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  std::uint8_t reason = 0;
  std::uint8_t mode = 0;
  std::uint64_t deadline_monotonic_ns = 0;
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementCancelBindAckV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string binding_uuid;
  std::uint64_t binding_generation = 0;
  std::string target_execution_uuid;
  std::string target_statement_uuid;
  std::string target_statement_receipt_uuid;
  std::string cancel_operation_uuid;
  std::string target_transaction_uuid;
  std::uint64_t target_execution_generation = 0;
  std::uint8_t reason = 0;
  std::uint8_t mode = 0;
  std::uint64_t deadline_monotonic_ns = 0;
  std::uint64_t executor_availability_generation = 0;
  std::array<std::uint8_t, 32> descriptor_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::array<std::uint8_t, 32> acknowledgement_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_ack_bytes;
  std::string failure_code;
  std::string failure_message_key;
  std::string failure_detail;
};

struct StatementCancelAuthorityV1 {
  StatementCancelBindAckV1 acknowledgement;
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::string canonical_statement_name;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

struct StatementParameterBindRequestV1 {
  std::string authenticated_receipt_uuid;
  std::uint64_t occurrence = 0;
  std::string statement_name;
  bool quoted = false;
  std::string prepared_statement_uuid;
  std::uint64_t prepared_generation = 0;
  std::string parameter_set_uuid;
  std::uint64_t parameter_set_generation = 0;
  std::array<std::uint8_t, 32> ordered_slot_table_sha256{};
  std::string batch_uuid;
  std::uint64_t batch_generation = 0;
  std::string dynamic_package_uuid;
  std::uint64_t dynamic_generation = 0;
  std::uint32_t value_count = 0;
  std::vector<std::uint8_t> canonical_value_vector;
  std::array<std::uint8_t, 32> value_vector_sha256{};
  std::array<std::uint8_t, 32> request_evidence_sha256{};
  std::vector<std::uint8_t> exact_bind_request_bytes;
};

struct StatementParameterBindAuthorityV1 {
  std::vector<std::uint8_t> exact_bind_request_bytes;
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_terminal_result_bytes;
  bool terminal_result_published = false;
};

// Exact engine-issued view returned at acquisition. Later Packet 7 stages may
// project the bounded parser fields from this value, but the opaque receipt
// remains the authority presented back to the engine.
struct StatementContextReceiptView {
  std::string receipt_uuid;
  std::string statement_uuid;
  // Captured exactly once by the engine while issuing this receipt. It is a
  // statement-stable value carrier, never transaction visibility/finality.
  std::string statement_timestamp;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  // Statement-scoped engine resource-admission authority.  This is distinct
  // from optimizer resource authority and is copied only through the opaque
  // receipt bridge.
  std::string resource_admission_uuid;
  std::string optimizer_capability_snapshot_uuid;
  std::string optimizer_resource_snapshot_uuid;
  std::string optimizer_route_snapshot_uuid;
  std::string bound_ast_uuid;
  std::string count_function_uuid;
  std::string sum_function_uuid;
  std::string avg_function_uuid;
  std::string min_function_uuid;
  std::string max_function_uuid;
  std::vector<StatementAggregateFunctionProfile> aggregate_function_profiles;
  std::vector<StatementWindowFunctionProfile> window_function_profiles;
  std::vector<StatementDescriptorProfile> descriptor_profiles;
  std::vector<StatementRelationOccurrenceMappingV1>
      relation_occurrence_mappings;

  // V11 literal prebind bootstrap. These are engine-issued receipt values;
  // the parser may echo them only in SBLN/SBLF and never selects them.
  std::string literal_catalog_snapshot_uuid;
  std::uint64_t literal_catalog_generation = 0;
  std::uint64_t literal_registry_generation = 0;

  // Engine-owned parameter execution-mode observation. Optional identity
  // pairs are immutable for the receipt and are nil exactly when generation
  // is zero. Direct mode leaves all three pairs absent.
  std::string parameter_prepared_statement_uuid;
  std::uint64_t parameter_prepared_generation = 0;
  std::string parameter_batch_uuid;
  std::uint64_t parameter_batch_generation = 0;
  std::string parameter_dynamic_package_uuid;
  std::uint64_t parameter_dynamic_generation = 0;
  std::uint64_t parameter_executor_availability_generation = 0;
  struct DiagnosticIdentityProjectionV1 {
    std::string diagnostic_uuid;
    std::uint64_t diagnostic_generation = 0;
    std::uint32_t precedence_ordinal = 0;
    std::uint8_t severity_code = 0;
    std::uint8_t redaction_class = 0;
    std::uint32_t maximum_safe_field_count = 0;
    std::string row_identity_sha256;
  };
  std::string diagnostic_registry_snapshot_uuid;
  std::uint64_t diagnostic_registry_generation = 0;
  std::vector<DiagnosticIdentityProjectionV1> diagnostic_identity_rows;
  std::string txn_begin_isolation_profile_uuid;
  std::uint64_t txn_begin_isolation_profile_generation = 0;
  std::string txn_begin_policy_snapshot_uuid;
  std::uint64_t txn_begin_policy_generation = 0;
  std::uint64_t txn_begin_executor_availability_generation = 0;
  std::uint8_t txn_begin_read_mode = 0;
  std::uint8_t txn_begin_authority_scope = 0;
  std::uint8_t txn_begin_wait_policy = 0;
  std::uint64_t txn_begin_deadline_monotonic_ns = 0;
  std::uint64_t txn_commit_executor_availability_generation = 0;
  std::uint8_t txn_commit_mode = 1;
  std::uint8_t txn_commit_authority_scope = 1;
  std::uint8_t txn_commit_wait_policy = 1;
  std::uint64_t txn_commit_deadline_monotonic_ns = 0;
  std::uint64_t txn_rollback_executor_availability_generation = 0;
  std::uint8_t txn_rollback_mode = 1, txn_rollback_authority_scope = 1,
      txn_rollback_wait_policy = 1;
  std::uint64_t txn_rollback_deadline_monotonic_ns = 0;
  std::uint64_t txn_release_savepoint_executor_availability_generation = 0;
  std::uint64_t txn_rollback_to_savepoint_executor_availability_generation = 0;
  std::uint64_t psql_autonomous_frame_executor_availability_generation = 0;
  std::uint64_t transaction_reservation_release_executor_availability_generation = 0;
  std::uint64_t temporary_instance_cleanup_executor_availability_generation = 0;
  std::uint64_t stmt_prepare_executor_availability_generation = 0;
  std::uint64_t stmt_execute_executor_availability_generation = 0;
  std::uint64_t stmt_execute_direct_executor_availability_generation = 0;
  std::uint64_t stmt_free_executor_availability_generation = 0;
  std::uint64_t stmt_cancel_executor_availability_generation = 0;
  std::uint64_t parameter_bind_executor_availability_generation = 0;
  std::uint64_t result_page_executor_availability_generation = 0;
  std::string result_page_redaction_profile_uuid;
  std::uint64_t result_page_redaction_generation = 0;
  std::string result_page_policy_snapshot_uuid;
  std::uint64_t result_page_policy_generation = 0;
  std::string result_page_resource_budget_uuid;
  std::uint64_t result_page_resource_budget_generation = 0;
  std::uint64_t query_explain_executor_availability_generation = 0;
  std::string query_explain_redaction_profile_uuid;
  std::uint64_t query_explain_redaction_generation = 0;
  std::string query_explain_policy_snapshot_uuid;
  std::uint64_t query_explain_policy_generation = 0;
  std::string query_explain_plan_policy_uuid;
  std::string query_explain_resource_budget_uuid;
  std::uint64_t query_explain_resource_budget_generation = 0;
  // Engine-issued identity for the exact language/rendering profile selected
  // for this statement.  The human-readable identifier profile name in
  // EngineRequestContext is not a UUID and must never be copied into SBXD.
  std::string query_explain_language_profile_uuid;
  // PARSE TEXT has a distinct engine-issued language-profile identity. Its
  // generation is the exact session language-resource epoch captured by this
  // receipt and may not be substituted by the parser profile spelling.
  std::string parse_text_language_profile_uuid;
  std::uint64_t parse_text_language_profile_generation = 0;
  std::uint64_t parse_text_executor_availability_generation = 0;
  std::uint64_t catalog_epoch_check_executor_availability_generation = 0;
  std::uint64_t database_attach_executor_availability_generation = 0;
  std::string catalog_epoch_check_redaction_profile_uuid;
  std::uint64_t catalog_epoch_check_redaction_generation = 0;
  std::string catalog_epoch_check_policy_snapshot_uuid;
  std::uint64_t catalog_epoch_check_policy_generation = 0;
  std::uint64_t catalog_introspect_executor_availability_generation = 0;
  std::uint64_t name_resolve_executor_availability_generation = 0;
  std::uint64_t optimizer_stats_read_executor_availability_generation = 0;
  std::uint64_t optimizer_stats_drop_executor_availability_generation = 0;
  std::uint64_t cursor_open_executor_availability_generation = 0;
  std::uint64_t cursor_fetch_executor_availability_generation = 0;
  std::uint64_t cursor_close_executor_availability_generation = 0;
  std::uint64_t read_by_key_executor_availability_generation = 0;
  std::uint64_t read_range_executor_availability_generation = 0;
  std::uint64_t read_stream_executor_availability_generation = 0;
  std::uint64_t result_set_pass_executor_availability_generation = 0;
  std::uint64_t access_cursor_open_executor_availability_generation = 0;
  std::uint64_t access_cursor_fetch_executor_availability_generation = 0;
  std::uint64_t access_cursor_close_executor_availability_generation = 0;
  std::uint64_t insert_executor_availability_generation = 0;
  std::uint64_t update_executor_availability_generation = 0;
  std::uint64_t delete_executor_availability_generation = 0;
  std::uint64_t merge_executor_availability_generation = 0;
  std::uint64_t table_truncate_executor_availability_generation = 0;
  std::uint64_t table_analyze_executor_availability_generation = 0;
  std::uint64_t bulk_import_stream_executor_availability_generation = 0;
  std::uint64_t bulk_export_stream_executor_availability_generation = 0;
  std::uint64_t statement_batch_executor_availability_generation = 0;
  std::uint64_t atomic_cas_executor_availability_generation = 0;
  std::uint64_t atomic_rmw_executor_availability_generation = 0;
  std::uint64_t advisory_lock_acquire_executor_availability_generation = 0;
  std::uint64_t advisory_lock_release_executor_availability_generation = 0;
  std::uint64_t function_call_executor_availability_generation = 0;
  std::uint64_t operator_call_executor_availability_generation = 0;
  std::uint64_t cast_executor_availability_generation = 0;
  std::uint64_t compare_executor_availability_generation = 0;
  std::uint64_t domain_operation_executor_availability_generation = 0;
  std::uint64_t udr_invoke_executor_availability_generation = 0;
  std::uint64_t procedure_invoke_executor_availability_generation = 0;
  std::uint64_t function_invoke_executor_availability_generation = 0;
  std::uint64_t aggregate_invoke_executor_availability_generation = 0;
  std::uint64_t sequence_nextval_executor_availability_generation = 0;
  std::uint64_t sequence_currval_executor_availability_generation = 0;
  std::uint64_t sequence_setval_executor_availability_generation = 0;
  std::uint64_t query_numeric_executor_availability_generation = 0;
  std::uint64_t advanced_datatype_family_executor_availability_generation = 0;
  std::uint64_t project_executor_availability_generation = 0;
  std::uint64_t aggregate_executor_availability_generation = 0;
  std::uint64_t group_executor_availability_generation = 0;
  std::uint64_t sort_executor_availability_generation = 0;
  std::uint64_t limit_executor_availability_generation = 0;
  std::uint64_t window_executor_availability_generation = 0;
  std::uint64_t return_result_set_executor_availability_generation = 0;
  std::uint64_t kv_structured_read_executor_availability_generation = 0;
  std::uint64_t kv_structured_mutate_executor_availability_generation = 0;
  std::uint64_t kv_structured_scan_executor_availability_generation = 0;
  std::uint64_t kv_structured_stream_read_executor_availability_generation = 0;
  std::uint64_t kv_structured_stream_append_executor_availability_generation = 0;
  std::uint64_t kv_structured_timeseries_executor_availability_generation = 0;
  std::uint64_t system_config_set_executor_availability_generation = 0;
  std::uint64_t ddl_create_domain_executor_availability_generation = 0;
  std::uint64_t ddl_create_schema_executor_availability_generation = 0;
  std::uint64_t ddl_create_table_executor_availability_generation = 0;
  std::uint64_t ddl_create_index_executor_availability_generation = 0;
  std::uint64_t ddl_drop_index_executor_availability_generation = 0;
  // Exact engine-issued TXBH for the selected active transaction.  This is a
  // copy-only public projection; the corresponding private handle remains
  // owned by `session` and is the authority used by commit/rollback.
  std::vector<std::uint8_t> active_transaction_handle_bytes;
  std::array<std::uint8_t, 32>
      parameter_preliminary_execution_mode_binding_sha256{};
  std::string variable_scope_uuid;
  std::uint64_t variable_scope_generation = 0;
  std::string variable_frame_uuid;
  std::uint64_t variable_frame_generation = 0;
  std::string variable_registry_snapshot_uuid;
  std::uint64_t variable_registry_generation = 0;
  std::uint64_t variable_executor_availability_generation = 0;

  std::uint64_t owning_local_transaction_id = 0;
  std::uint64_t visible_committed_high_watermark = 0;
  std::uint64_t oldest_active_local_transaction_id = 0;
  std::uint64_t oldest_interesting_local_transaction_id = 0;
  std::uint64_t oldest_snapshot_local_transaction_id = 0;
  std::uint64_t retention_horizon_local_transaction_id = 0;
  std::uint64_t publication_inventory_next_local_transaction_id = 0;
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  bool inventory_authoritative = false;
  bool snapshot_complete = false;

  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  // Exact statement resource-policy value issued with this receipt.  It is a
  // per-source, per-physical-pass decoded MGA relation byte ceiling and is not
  // an optimizer, result, LIMIT/OFFSET, or transport budget.
  std::uint64_t maximum_mga_relation_decoded_bytes_per_pass = 0;
  // Exact engine-issued, non-serializable ceiling for each typed-result
  // descriptor vector and each row-data packet.  It is immutable for this
  // receipt/resource epoch and is not a scan, row, optimizer, or client cap.
  std::uint64_t maximum_typed_result_transport_bytes_per_packet = 0;
  std::uint64_t optimizer_route_epoch = 0;
  std::uint64_t optimizer_route_generation = 0;
  std::uint64_t optimizer_memory_budget_bytes = 0;
  std::uint64_t optimizer_maximum_candidate_count = 0;
  std::uint64_t optimizer_maximum_memo_groups = 0;
  std::uint64_t optimizer_maximum_search_steps = 0;
  std::uint64_t optimizer_maximum_planning_time_ns = 0;
  bool optimizer_spill_allowed = false;
  bool cluster_context_active = false;
  bool cluster_transaction_active = false;
  bool route_fence_present = false;
};

struct StatementContextAcquireRequest {
  // Copied immediately by the engine. This private internal context preserves
  // the complete materialized authorization, resource, optimizer, and route
  // state without narrowing it through sb_engine_request_context_v1_t.
  const scratchbird::engine::internal_api::EngineRequestContext*
      engine_context = nullptr;
  std::string_view exact_transaction_uuid;
  StatementParameterExecutionSelectorV1 parameter_execution_selector;
  struct VariableFrameSelectorV1 {
    std::uint16_t version = 0;
    std::string public_coordination_uuid;
    std::string operation_uuid;
    std::uint64_t expected_coordinator_generation = 0;
  } variable_frame_selector;
};

enum class StatementGatewayEvidenceSource : std::uint8_t {
  kInvalid = 0,
  kLocalObserved = 1,
};

enum class StatementGatewayDisposition : std::uint8_t {
  kInvalid = 0,
  kPassThrough = 1,
  kHandled = 2,
  kAsyncAccepted = 3,
  kRefused = 4,
};

struct StatementGatewayDecisionEvidence {
  StatementGatewayEvidenceSource source =
      StatementGatewayEvidenceSource::kInvalid;
  StatementGatewayDisposition disposition =
      StatementGatewayDisposition::kInvalid;
  std::uint64_t provider_observation_generation = 0;
  std::array<std::uint8_t, 32> canonical_payload_sha256{};
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch = 0;
  std::uint64_t route_generation = 0;
  std::string security_snapshot_uuid;
  std::uint64_t security_epoch = 0;
  std::uint64_t security_observation_generation = 0;
  bool cluster_context_active = false;
  bool cluster_transaction_active = false;
  bool route_fence_present = false;
};

struct StatementPackageExecutorEvidence {
  std::string begin_executor_id;
  std::string end_executor_id;
  std::string registry_snapshot_uuid;
  std::uint64_t executor_evidence_generation = 0;
  std::array<std::uint8_t, 32> canonical_payload_sha256{};
};

enum class StatementSblrPayloadKind : std::uint8_t {
  kInvalid = 0,
  kOpcodeStream = 1,
  kOperationEnvelope = 2,
};

enum class StatementPackageReservationReleaseReason : std::uint8_t {
  kRelease = 0,
  kCancel = 1,
  kTimeout = 2,
  kDisconnect = 3,
  kShutdown = 4,
};

struct StatementPackageAdmissionReservationHandle {
  std::uint64_t opaque_id = 0;
  [[nodiscard]] explicit operator bool() const { return opaque_id != 0; }
  friend bool operator==(const StatementPackageAdmissionReservationHandle&,
                         const StatementPackageAdmissionReservationHandle&) =
      default;
};

enum class StatementPackageReservationIdAdmission : std::uint8_t {
  kAdmitted = 0,
  kZeroExhausted = 1,
  kCollision = 2,
};

constexpr StatementPackageReservationIdAdmission
ClassifyStatementPackageReservationId(std::uint64_t candidate,
                                      bool already_registered) {
  if (candidate == 0)
    return StatementPackageReservationIdAdmission::kZeroExhausted;
  if (already_registered)
    return StatementPackageReservationIdAdmission::kCollision;
  return StatementPackageReservationIdAdmission::kAdmitted;
}

struct StatementPackageAdmissionReservationRequest {
  StatementContextReceiptHandle receipt;
  const std::uint8_t* canonical_payload_bytes = nullptr;
  std::size_t canonical_payload_size = 0;
  StatementSblrPayloadKind payload_kind = StatementSblrPayloadKind::kInvalid;
};

struct StatementPackageAdmissionReservationView {
  StatementSblrPayloadKind payload_kind = StatementSblrPayloadKind::kInvalid;
  std::uint64_t payload_size = 0;
  std::uint32_t record_count = 0;
  std::uint64_t resource_policy_generation = 0;
  std::array<std::uint8_t, 32> payload_sha256{};
};

struct StatementQueryExecuteResultHandleView {
  std::string execution_uuid;
  std::string result_set_uuid;
  std::string row_descriptor_uuid;
  std::string snapshot_uuid;
};

// Returns only registry-validated typed command-handle metadata retained by
// the engine result. It never parses presentation rows or evidence text.
sb_engine_status_t ReadStatementQueryExecuteResultHandle(
    sb_engine_result_t result,
    StatementQueryExecuteResultHandleView* out_handle);

// Copies the canonical, already-redacted row bytes produced by one exact
// RESULT_PAGE dispatch. The bytes remain engine-owned state on the returned
// operation result; this bridge never exposes the source cursor handle.
sb_engine_status_t ReadStatementResultPageMaterial(
    sb_engine_result_t result, std::vector<std::uint8_t>* out_material);

sb_engine_status_t ReadStatementQueryExplainMaterial(
    sb_engine_result_t result, std::vector<std::uint8_t>* out_material);

// Private immutable dispatch receipt. The server supplies bytes that already
// passed canonical admission plus the admission digests; the engine re-decodes
// and re-hashes all three layers and binds them to the still-live receipt.
// No form of this request is exposed through the public C ABI.
struct StatementContextDispatchRequest {
  StatementContextReceiptHandle receipt;
  StatementPackageAdmissionReservationHandle package_admission_reservation;
  StatementSblrPayloadKind admitted_payload_kind =
      StatementSblrPayloadKind::kInvalid;
  sb_engine_session_t engine_session = nullptr;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_operation_bytes;
  std::array<std::uint8_t, 32> container_sha256{};
  std::array<std::uint8_t, 32> execution_envelope_sha256{};
  std::array<std::uint8_t, 32> operation_sha256{};
  std::array<std::uint8_t, 32> admission_binding_sha256{};
  std::string authenticated_principal_uuid;
  std::string catalog_snapshot_uuid;
  std::string engine_mga_statement_uuid;
  std::string engine_mga_snapshot_uuid;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  StatementContextResultDeliveryMode result_delivery_mode =
      StatementContextResultDeliveryMode::kDirect;
  StatementGatewayDecisionEvidence gateway_evidence;
  StatementPackageExecutorEvidence package_executor_evidence;
  std::vector<std::uint8_t> data_packet;
  std::vector<std::uint8_t> literal_execution_binding;
  std::vector<std::uint8_t> parameter_execution_binding;
  std::vector<std::uint8_t> parameter_value_set;
  std::vector<std::uint8_t> variable_execution_binding;
  // Retained query result selected by the server from the cursor that owns
  // this exact receipt. It is never serialized or supplied by the parser.
  sb_engine_result_t result_page_source_result = nullptr;
  // Database-owned durable executor state for the exact opcode-775 route.
  // The server may project this private pointer only after selecting the
  // hosted database for the authenticated receipt. It is never serialized,
  // accepted from a parser, or used by any other opcode.
  scratchbird::engine::internal_api::SblrBulkImportStreamRegistry*
      bulk_import_stream_registry = nullptr;
};

sb_engine_status_t AcquireStatementPackageAdmissionReservation(
    const StatementPackageAdmissionReservationRequest* request,
    StatementPackageAdmissionReservationHandle* out_handle,
    StatementPackageAdmissionReservationView* out_view,
    sb_engine_result_t* out_result);

sb_engine_status_t ReleaseStatementPackageAdmissionReservation(
    StatementPackageAdmissionReservationHandle handle,
    StatementPackageReservationReleaseReason reason);

// Selects the exact active durable-inventory transaction and publishes one
// statement-stable snapshot. No caller-supplied UUID or numeric high-water is
// accepted as statement authority.
sb_engine_status_t AcquireStatementContextReceipt(
    sb_engine_session_t session,
    const StatementContextAcquireRequest* request,
    StatementContextReceiptHandle* out_receipt,
    StatementContextReceiptView* out_view,
    sb_engine_result_t* out_result);

// Releases the engine-owned receipt exactly once and revokes its published
// statement snapshot. Repeated release returns ALREADY_RELEASED.
sb_engine_status_t ReleaseStatementContextReceipt(
    StatementContextReceiptHandle receipt);

// Copies the immutable engine-owned context behind a live receipt for a
// private authenticated server coordinator. The opaque receipt remains the
// authority boundary; no parser-visible field can populate the returned MGA,
// datatype, security, or transaction identities.
sb_engine_status_t CopyStatementContextEngineContextV1(
    StatementContextReceiptHandle receipt,
    scratchbird::engine::internal_api::EngineRequestContext* out_context,
    sb_engine_result_t* out_result);

// Attaches an engine-resolved relation mapping to a live receipt.  This is
// callable only by the engine binder; the parser/SBPS layers have no route to
// manufacture or replace these values.  Mappings are immutable once literal
// negotiation begins.
sb_engine_status_t AttachStatementRelationOccurrenceMappingsV1(
    StatementContextReceiptHandle receipt,
    const std::vector<StatementRelationOccurrenceMappingV1>& mappings,
    sb_engine_result_t* out_result);

// Resolves and authorizes one canonical COPY bind demand against the exact live
// receipt, then atomically attaches an immutable private authority row.  Exact
// replay returns the byte-identical ACK; either occurrence colliding with a
// different demand is a recovery conflict.
sb_engine_status_t BindStatementBulkImportAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementBulkImportBindRequestV1* request,
    StatementBulkImportBindAckV1* out_ack,
    sb_engine_result_t* out_result);

// Copies one already-attached private authority for an authenticated engine
// coordinator.  It never derives, defaults, or refreshes authority and exposes
// nothing through the public receipt projection.
sb_engine_status_t CopyStatementBulkImportAuthorityV1(
    StatementContextReceiptHandle receipt,
    std::uint64_t structural_occurrence,
    std::uint32_t import_occurrence,
    StatementBulkImportAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Binds and copies the private PREPARE authority selected by authenticated
// SBsql syntax. These functions never expose the nested SQL text or permit
// parser-created statement/name identities.
sb_engine_status_t BindStatementPrepareAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementPrepareBindRequestV1* request,
    StatementPrepareBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementPrepareAuthorityV1(
    StatementContextReceiptHandle receipt,
    std::uint64_t occurrence,
    StatementPrepareAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Binds a freshly compiled canonical statement body to EXECUTE DIRECT under
// the same live receipt. Exact request replay returns the same descriptor;
// changed bytes at the same occurrence are stale and never execute.
sb_engine_status_t BindStatementExecuteDirectAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementExecuteDirectBindRequestV1* request,
    StatementExecuteDirectBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementExecuteDirectAuthorityV1(
    StatementContextReceiptHandle receipt,
    std::uint64_t occurrence,
    StatementExecuteDirectAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Attaches one compile-only canonical query body to the live receipt for
// QUERY_EXPLAIN. The query is validated against the receipt before any public
// SBXD is issued. Exact replay returns the original engine identities.
sb_engine_status_t BindStatementQueryExplainAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementQueryExplainBindRequestV1* request,
    StatementQueryExplainBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementQueryExplainAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementQueryExplainAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Binds presented RESOLVE NAME atoms to the engine-owned catalog authority
// held by the same live statement receipt. The parser supplies syntax only;
// object/namespace UUIDs, generations, redaction, and result identity are
// never accepted from the caller.
sb_engine_status_t BindStatementNameResolveAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementNameResolveBindRequestV1* request,
    StatementNameResolveBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementNameResolveAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementNameResolveAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Binds one PARSE TEXT input and its nested canonical parser carriers to the
// exact live receipt. The parser supplies text/profile/options only; parse,
// binding, descriptor, language-profile, and evidence identities are issued
// by the engine. Exact request replay returns byte-identical authority.
sb_engine_status_t BindStatementParseTextAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementParseTextBindRequestV1* request,
    StatementParseTextBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementParseTextAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementParseTextAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

sb_engine_status_t BindStatementCatalogEpochCheckAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementCatalogEpochCheckBindRequestV1* request,
    StatementCatalogEpochCheckBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementCatalogEpochCheckAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementCatalogEpochCheckAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

sb_engine_status_t BindStatementDatabaseAttachAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementDatabaseAttachBindRequestV1* request,
    StatementDatabaseAttachBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementDatabaseAttachAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementDatabaseAttachAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

sb_engine_status_t RetainStatementSourceArtifactV1(
    StatementContextReceiptHandle receipt,
    const std::uint8_t* exact_request_bytes,
    std::size_t exact_request_size,
    StatementSourceArtifactRetentionV1* out_retention,
    sb_engine_result_t* out_result);
sb_engine_status_t ResolveStatementSourceArtifactV1(
    StatementContextReceiptHandle receipt,
    const StatementSourceArtifactReferenceV1* reference,
    StatementSourceArtifactRetentionV1* out_retention,
    sb_engine_result_t* out_result);

// Issues the fixed catalog-scope OPTIMIZER_STATS_READ descriptor from the
// engine-owned statement receipt.  The caller supplies only its occurrence;
// all authority and the immutable result identity are engine produced.
sb_engine_status_t BindStatementOptimizerStatsReadAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementOptimizerStatsReadAuthorityV1* out_authority,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementOptimizerStatsReadAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementOptimizerStatsReadAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Issues one database-wide OPTIMIZER_STATS_DROP descriptor after exact
// OBS_MANAGEMENT_CONTROL authorization and durable statistics-epoch lookup.
sb_engine_status_t BindStatementOptimizerStatsDropAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementOptimizerStatsDropAuthorityV1* out_authority,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementOptimizerStatsDropAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementOptimizerStatsDropAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Resolves one session-owned prepared name and freezes the exact execution
// descriptor. The request carries syntax and parameter bytes only; prepared
// statement identity, body, result identity, and availability remain engine
// authority behind the live receipt.
sb_engine_status_t BindStatementExecuteAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementExecuteBindRequestV1* request,
    std::vector<std::uint8_t>* out_descriptor_bytes,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementExecuteAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementExecuteAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Resolves a PREPARE name within the exact authenticated engine session and
// attaches the immutable SBLR_STMT_FREE descriptor to the new receipt.
sb_engine_status_t BindStatementFreeAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementFreeBindRequestV1* request,
    StatementFreeBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementFreeAuthorityV1(
    StatementContextReceiptHandle receipt,
    std::uint64_t occurrence,
    StatementFreeAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Resolves CANCEL STATEMENT syntax to the exact last engine-owned execution
// record for that prepared name. The first bounded implementation publishes
// an `already_terminal` result; active cooperative signalling remains a
// distinct continuation of the same canonical carrier.
sb_engine_status_t BindStatementCancelAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementCancelBindRequestV1* request,
    StatementCancelBindAckV1* out_ack,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementCancelAuthorityV1(
    StatementContextReceiptHandle receipt,
    std::uint64_t occurrence,
    StatementCancelAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

// Binds one canonical SBPV vector to an already-issued prepared parameter set
// and attaches the exact public SBKD descriptor to the current receipt.
sb_engine_status_t BindStatementParameterBindAuthorityV1(
    StatementContextReceiptHandle receipt,
    const StatementParameterBindRequestV1* request,
    std::vector<std::uint8_t>* out_descriptor_bytes,
    sb_engine_result_t* out_result);
sb_engine_status_t CopyStatementParameterBindAuthorityV1(
    StatementContextReceiptHandle receipt, std::uint64_t occurrence,
    StatementParameterBindAuthorityV1* out_authority,
    sb_engine_result_t* out_result);

sb_engine_status_t NegotiateStatementLiteralDescriptorsV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_sbln,
    std::vector<std::uint8_t>* out_canonical_sblq,
    sb_engine_result_t* out_result);
sb_engine_status_t FinalizeStatementLiteralBindingV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_sblf,
    std::vector<std::uint8_t>* out_canonical_sbla,
    sb_engine_result_t* out_result);

// Private query-1.1 contextual TEXT V2 coordination. Exact wire and graph
// evidence is always re-decoded by the engine. The bridge owns no target,
// sidecar, policy, or executor authority; absent engine-only resolver services
// fail closed.
sb_engine_status_t IssueStatementContextualTextLiteralProfilesV2(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& exact_sbtlnr02,
    std::vector<std::uint8_t>* out_exact_sbtlns02,
    sb_engine_result_t* out_result);

struct StatementContextualTextLiteralPrepareRequestV2 {
  StatementContextReceiptHandle receipt;
  std::vector<std::uint8_t> exact_sbel_v1;
  std::vector<std::uint8_t> exact_canonical_sbos;
  std::vector<std::uint8_t> exact_sbtlxe02;
  std::vector<std::uint8_t> exact_pre_contextual_operand_records;
  std::uint32_t pre_contextual_operand_count = 0;
  std::vector<std::uint8_t> exact_sbxn;
};

sb_engine_status_t PrepareStatementContextualTextLiteralExecutionV2(
    const StatementContextualTextLiteralPrepareRequestV2* request,
    scratchbird::engine::internal_api::PreparedContextualTextLiteralSetV2*
        out_prepared,
    sb_engine_result_t* out_result);

struct StatementContextualTextLiteralJointConsumeRequestV2 {
  StatementContextReceiptHandle receipt;
  std::vector<std::uint8_t> exact_sbel_v1;
  scratchbird::engine::internal_api::PreparedContextualTextLiteralSetV2*
      prepared = nullptr;
};

sb_engine_status_t JointConsumeStatementContextualTextLiteralExecutionV2(
    const StatementContextualTextLiteralJointConsumeRequestV2* request,
    scratchbird::engine::internal_api::
        ContextualTextExecutionAuthorityLeaseV2* out_lease,
    sb_engine_result_t* out_result);

sb_engine_status_t RevokeStatementContextualTextLiteralProfilesV2(
    StatementContextReceiptHandle receipt,
    std::string_view reason,
    sb_engine_result_t* out_result);
sb_engine_status_t NegotiateStatementParameterDescriptorsV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_sbpr,
    std::vector<std::uint8_t>* out_canonical_sbpg,
    sb_engine_result_t* out_result);
sb_engine_status_t FinalizeStatementParameterBindingV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_sbpf,
    std::vector<std::uint8_t>* out_canonical_sbpa,
    sb_engine_result_t* out_result);
sb_engine_status_t FinalizeStatementVariableBindingV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_sbvf,
    std::vector<std::uint8_t>* out_canonical_sbva,
    sb_engine_result_t* out_result);
sb_engine_status_t AssignStatementVariableValuesV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_sbvy,
    std::vector<std::uint8_t>* out_canonical_sbvw,
    sb_engine_result_t* out_result);
sb_engine_status_t IssueStatementSourceMapDescriptorV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_smrq,
    std::vector<std::uint8_t>* out_canonical_smrs,
    sb_engine_result_t* out_result);
sb_engine_status_t IssueStatementErrorVectorDescriptorV1(
    StatementContextReceiptHandle receipt,
    const std::vector<std::uint8_t>& canonical_evrq,
    std::vector<std::uint8_t>* out_canonical_evrs,
    sb_engine_result_t* out_result);

// Revalidates a server admission token against current MGA receipt authority,
// materializes canonical typed operand bodies, and dispatches query.execute.
sb_engine_status_t DispatchStatementContextReceipt(
    const StatementContextDispatchRequest* request,
    sb_engine_result_t* out_result);

// Deterministic proof of the production query.execute minor/kind-206
// admission classifier. No receipt, authority handle, or mutable global state
// is created by this test seam.
std::uint32_t ContextualTextPublicAbiCarrierProofMaskForTest();

}  // namespace scratchbird::server_engine_bridge
