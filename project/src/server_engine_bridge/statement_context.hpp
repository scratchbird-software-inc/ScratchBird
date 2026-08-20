// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "scratchbird/engine/engine.h"

#include <cstdint>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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
  StatementGatewayDecisionEvidence gateway_evidence;
  StatementPackageExecutorEvidence package_executor_evidence;
  std::vector<std::uint8_t> data_packet;
  std::vector<std::uint8_t> literal_execution_binding;
  std::vector<std::uint8_t> parameter_execution_binding;
  std::vector<std::uint8_t> parameter_value_set;
  std::vector<std::uint8_t> variable_execution_binding;
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

}  // namespace scratchbird::server_engine_bridge
