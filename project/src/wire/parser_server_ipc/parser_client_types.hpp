// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../../core/platform/runtime_platform.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::parser::ipc {

// Family-neutral parser/server transport configuration.  It intentionally
// contains no grammar, binder, lowering, cache, renderer, or dialect-default
// behavior.  Parser families supply their own values.
struct ParserClientConfig {
  std::string database_token;
  std::string server_endpoint;
  std::string dialect;
  std::string profile_id;
  std::string default_language_profile;
  std::string input_syntax_profile;
  std::string common_resource_hash;
  std::string resource_compatibility_identity;
  std::string resource_version_identity;
  scratchbird::core::platform::Uuid dialect_profile_uuid;
  std::vector<std::string> default_search_path{"sys", "public"};
  std::uint32_t registry_version{1};
  // Opt-in keeps the legacy hello byte-for-byte unchanged for every existing
  // parser.  A parser that routes independent transactions must require the
  // V2 capability during hello and fail closed if the server omits it.
  bool require_transaction_routing_v2{false};
  // An opted-in parser may execute an engine-owned prepared object on a
  // different exact transaction selector from the selector used at prepare.
  // The transfer binding is opaque and server-owned: no metadata snapshot,
  // catalog generation, or transfer token is exposed to the parser.
  bool require_prepared_metadata_transfer_v1{false};
  // Persisted relation projection has an independent capability bit.  It
  // implies exact V2 transaction routing but remains opt-in so legacy parser
  // clients preserve their existing HELLO bytes and server contract.
  bool require_relation_descriptor_projection_v3{false};
};

// A transaction selector is an opaque routing identity published by the
// engine.  Parser families may retain and return it, but must never synthesize
// either component or treat it as transaction authority.
struct ParserTransactionSelector {
  std::uint64_t local_transaction_id{0};
  scratchbird::core::platform::Uuid transaction_uuid;

  [[nodiscard]] bool present() const {
    return local_transaction_id != 0 && !transaction_uuid.is_nil();
  }
};

enum class ParserTransactionRoute : std::uint8_t {
  // Source- and wire-compatible behavior for SBPS V1 callers.  The server
  // selects the session's engine-owned default transaction.
  kLegacyDefault = 0,
  // Route the request through the exact engine-issued selector supplied by
  // the caller.  Both the id and UUID must match the owning session.
  kSelected = 1,
  // Ask the engine to create one additional independent transaction in the
  // existing session.  No caller-supplied selector is accepted for this mode.
  kBeginAdditional = 2,
};

struct ParserTransactionRouting {
  ParserTransactionRoute route{ParserTransactionRoute::kLegacyDefault};
  ParserTransactionSelector selector;
};

// Bounded parser projection of one engine-issued statement context. The
// private receipt, complete visibility vector, resource policy, and optimizer
// state remain server/engine-owned and are intentionally absent.
struct PreliminaryDiagnosticIdentityV1 {
  std::array<std::uint8_t, 16> diagnostic_uuid{};
  std::uint64_t generation{0};
  std::uint32_t precedence_ordinal{0};
  std::uint8_t severity_code{0};
  std::uint8_t redaction_class{0};
  std::uint32_t max_safe_fields{0};
  std::array<std::uint8_t, 32> identity_sha256{};
};

struct ParserStatementContext {
  struct AggregateFunctionProfile {
    std::uint16_t abi_version{0};
    std::string builtin_id;
    scratchbird::core::platform::Uuid function_uuid;
    bool executable{false};
  };

  struct WindowFunctionProfile {
    std::uint16_t abi_version{0};
    std::string builtin_id;
    scratchbird::core::platform::Uuid function_uuid;
    bool executable{false};
  };

  struct DescriptorProfile {
    std::uint8_t profile_kind{0};
    std::uint16_t slot{0};
    scratchbird::core::platform::Uuid descriptor_uuid;
    scratchbird::core::platform::Uuid type_uuid;
    scratchbird::core::platform::Uuid collation_uuid;
    bool nullable{false};
    std::uint32_t width{0};
    std::uint32_t precision{0};
    std::uint32_t scale{0};
  };

  struct LiteralStatementDescriptorProfileV1 {
    std::uint16_t profile_version{0};
    scratchbird::core::platform::Uuid profile_uuid;
    // Statement-local relational descriptor identity issued for this literal
    // occurrence. The SBLP descriptor_uuid remains the core codec/catalog
    // identity and must not be reused as a DAG descriptor identity. SBLP v2
    // may instead project a separately authenticated persisted relation
    // descriptor handle.
    scratchbird::core::platform::Uuid binding_descriptor_uuid;
    scratchbird::core::platform::Uuid statement_receipt_uuid;
    scratchbird::core::platform::Uuid catalog_snapshot_uuid;
    std::uint64_t catalog_generation{0};
    scratchbird::core::platform::Uuid descriptor_uuid;
    std::uint64_t descriptor_generation{0};
    scratchbird::core::platform::Uuid type_uuid;
    std::string codec_id;
    std::uint16_t codec_version{0};
    std::uint64_t codec_generation{0};
    bool nullable{false};
    std::array<std::uint8_t, 32> profile_binding_sha256{};
    std::vector<std::uint8_t> opaque_projection;
  };

  bool acquired{false};
  scratchbird::core::platform::Uuid statement_uuid;
  ParserTransactionSelector transaction;
  scratchbird::core::platform::Uuid statement_snapshot_uuid;
  scratchbird::core::platform::Uuid statement_metadata_snapshot_uuid;
  scratchbird::core::platform::Uuid catalog_epoch_uuid;
  // Exact statement-visible catalog generation issued with this receipt.
  // This is deliberately distinct from preliminary_catalog_generation,
  // which names the fixed datatype/literal registry generation.
  std::uint64_t preliminary_statement_catalog_generation{0};
  scratchbird::core::platform::Uuid security_context_uuid;
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  // Exact engine-issued UTC bytes from SBPS statement-context V7-V10. This
  // is a statement-stable value carrier only, never transaction finality.
  std::string statement_timestamp;
  scratchbird::core::platform::Uuid bound_ast_uuid;
  scratchbird::core::platform::Uuid count_function_uuid;
  scratchbird::core::platform::Uuid sum_function_uuid;
  scratchbird::core::platform::Uuid avg_function_uuid;
  scratchbird::core::platform::Uuid min_function_uuid;
  scratchbird::core::platform::Uuid max_function_uuid;
  std::vector<AggregateFunctionProfile> aggregate_function_profiles;
  std::vector<WindowFunctionProfile> window_function_profiles;
  std::vector<DescriptorProfile> descriptor_profiles;
  std::vector<LiteralStatementDescriptorProfileV1>
      literal_statement_descriptor_profiles;
  // V11 preliminary statement context shared by admitted leaf negotiations.
  // Leaf-specific projections below remain compatibility storage only.
  scratchbird::core::platform::Uuid preliminary_receipt_uuid;
  scratchbird::core::platform::Uuid preliminary_catalog_snapshot_uuid;
  std::uint64_t preliminary_catalog_generation{0};
  std::uint64_t preliminary_security_epoch{0};
  std::uint64_t preliminary_resource_epoch{0};
  // Schema 7032 v71 copy-only projection of the exact engine-issued statement
  // resource policy.  Parsers may copy it into SBQNDR01 but may not default,
  // narrow, widen, or alias it to any other resource or result bound.
  std::uint64_t preliminary_maximum_mga_relation_decoded_bytes_per_pass{0};
  scratchbird::core::platform::Uuid preliminary_mga_snapshot_uuid;
  std::uint16_t preliminary_extension_version{0};
  scratchbird::core::platform::Uuid preliminary_prepared_statement_uuid;
  std::uint64_t preliminary_prepared_generation{0};
  scratchbird::core::platform::Uuid preliminary_batch_uuid;
  std::uint64_t preliminary_batch_generation{0};
  scratchbird::core::platform::Uuid preliminary_dynamic_package_uuid;
  std::uint64_t preliminary_dynamic_generation{0};
  // Engine-owned generation for the exact engine.op.parameter executor row.
  // Schema 7032 v3 is the sole parser-visible source; callers copy it into
  // SBPT and never derive it from registry order or another executor family.
  std::uint64_t preliminary_parameter_executor_availability_generation{0};
  // Schema 7032 v4 copy-only variable frame projection.
  scratchbird::core::platform::Uuid preliminary_variable_scope_uuid;
  std::uint64_t preliminary_variable_scope_generation{0};
  scratchbird::core::platform::Uuid preliminary_variable_frame_uuid;
  std::uint64_t preliminary_variable_frame_generation{0};
  scratchbird::core::platform::Uuid preliminary_variable_registry_snapshot_uuid;
  std::uint64_t preliminary_variable_executor_availability_generation{0};
  // Schema 7032 v5 copy-only diagnostic cohort projection.
  std::array<std::uint8_t, 16> preliminary_diagnostic_registry_snapshot_uuid{};
  std::uint64_t preliminary_diagnostic_registry_generation{0};
  std::vector<PreliminaryDiagnosticIdentityV1> preliminary_diagnostic_identities;
  scratchbird::core::platform::Uuid preliminary_transaction_isolation_profile_uuid;
  std::uint64_t preliminary_transaction_isolation_profile_generation{0};
  scratchbird::core::platform::Uuid preliminary_transaction_policy_snapshot_uuid;
  std::uint64_t preliminary_transaction_policy_generation{0};
  std::uint64_t preliminary_transaction_executor_availability_generation{0};
  std::uint8_t preliminary_transaction_read_mode{0};
  std::uint8_t preliminary_transaction_authority_scope{0};
  std::uint8_t preliminary_transaction_wait_policy{0};
  std::uint64_t preliminary_transaction_deadline_monotonic_ns{0};
  std::uint64_t preliminary_transaction_commit_executor_availability_generation{0};
  std::uint8_t preliminary_transaction_commit_mode{0};
  std::uint8_t preliminary_transaction_commit_authority_scope{0};
  std::uint8_t preliminary_transaction_commit_wait_policy{0};
  std::uint64_t preliminary_transaction_commit_deadline_monotonic_ns{0};
  std::uint64_t preliminary_transaction_rollback_executor_availability_generation{0};
  std::uint8_t preliminary_transaction_rollback_mode{0};
  std::uint8_t preliminary_transaction_rollback_authority_scope{0};
  std::uint8_t preliminary_transaction_rollback_wait_policy{0};
  std::uint64_t preliminary_transaction_rollback_deadline_monotonic_ns{0};
  std::uint64_t preliminary_transaction_release_savepoint_executor_availability_generation{0};
  std::uint64_t preliminary_transaction_rollback_to_savepoint_executor_availability_generation{0};
  std::uint64_t preliminary_psql_autonomous_frame_executor_availability_generation{0};
  std::uint64_t preliminary_transaction_reservation_release_executor_availability_generation{0};
  std::uint64_t preliminary_temporary_instance_cleanup_executor_availability_generation{0};
  std::uint64_t preliminary_stmt_prepare_executor_availability_generation{0};
  std::uint64_t preliminary_stmt_execute_direct_executor_availability_generation{0};
  std::uint64_t preliminary_stmt_free_executor_availability_generation{0};
  std::uint64_t preliminary_cursor_open_executor_availability_generation{0};
  std::uint64_t preliminary_cursor_fetch_executor_availability_generation{0};
  std::uint64_t preliminary_cursor_close_executor_availability_generation{0};
  std::uint64_t preliminary_read_by_key_executor_availability_generation{0};
  std::uint64_t preliminary_read_range_executor_availability_generation{0};
  std::uint64_t preliminary_read_stream_executor_availability_generation{0};
  std::uint64_t preliminary_result_set_pass_executor_availability_generation{0};
  std::uint64_t preliminary_access_cursor_open_executor_availability_generation{0};
  std::uint64_t preliminary_access_cursor_fetch_executor_availability_generation{0};
  std::uint64_t preliminary_access_cursor_close_executor_availability_generation{0};
  std::uint64_t preliminary_insert_executor_availability_generation{0};
  std::uint64_t preliminary_update_executor_availability_generation{0};
  std::uint64_t preliminary_delete_executor_availability_generation{0};
  std::uint64_t preliminary_merge_executor_availability_generation{0};
  std::uint64_t preliminary_table_truncate_executor_availability_generation{0};
  std::uint64_t preliminary_table_analyze_executor_availability_generation{0};
  std::uint64_t preliminary_bulk_import_stream_executor_availability_generation{0};
  std::uint64_t preliminary_bulk_export_stream_executor_availability_generation{0};
  std::uint64_t preliminary_statement_batch_executor_availability_generation{0};
  std::uint64_t preliminary_atomic_cas_executor_availability_generation{0};
  std::uint64_t preliminary_atomic_rmw_executor_availability_generation{0};
  std::uint64_t preliminary_advisory_lock_acquire_executor_availability_generation{0};
  std::uint64_t preliminary_advisory_lock_release_executor_availability_generation{0};
  std::uint64_t preliminary_function_call_executor_availability_generation{0};
  std::uint64_t preliminary_operator_call_executor_availability_generation{0};
  std::uint64_t preliminary_cast_executor_availability_generation{0};
  std::uint64_t preliminary_compare_executor_availability_generation{0};
  std::uint64_t preliminary_domain_operation_executor_availability_generation{0};
  std::uint64_t preliminary_udr_invoke_executor_availability_generation{0};
  std::uint64_t preliminary_procedure_invoke_executor_availability_generation{0};
  std::uint64_t preliminary_function_invoke_executor_availability_generation{0};
  std::uint64_t preliminary_aggregate_invoke_executor_availability_generation{0};
  std::uint64_t preliminary_sequence_nextval_executor_availability_generation{0};
  std::uint64_t preliminary_sequence_currval_executor_availability_generation{0};
  std::uint64_t preliminary_sequence_setval_executor_availability_generation{0};
  std::uint64_t preliminary_query_numeric_executor_availability_generation{0};
  std::uint64_t preliminary_advanced_datatype_family_executor_availability_generation{0};
  std::uint64_t preliminary_project_executor_availability_generation{0};
  std::uint64_t preliminary_aggregate_executor_availability_generation{0};
  std::uint64_t preliminary_group_executor_availability_generation{0};
  std::uint64_t preliminary_sort_executor_availability_generation{0};
  std::uint64_t preliminary_limit_executor_availability_generation{0};
  std::uint64_t preliminary_window_executor_availability_generation{0};
  std::uint64_t preliminary_return_result_set_executor_availability_generation{0};
  std::uint64_t preliminary_kv_structured_read_executor_availability_generation{0};
  std::uint64_t preliminary_kv_structured_mutate_executor_availability_generation{0};
  std::uint64_t preliminary_kv_structured_scan_executor_availability_generation{0};
  std::uint64_t preliminary_kv_structured_stream_read_executor_availability_generation{0};
  std::uint64_t preliminary_kv_structured_stream_append_executor_availability_generation{0};
  std::uint64_t preliminary_kv_structured_timeseries_executor_availability_generation{0};
  std::uint64_t preliminary_system_config_set_executor_availability_generation{0};
  std::uint64_t preliminary_ddl_create_domain_executor_availability_generation{0};
  std::uint64_t preliminary_ddl_create_schema_executor_availability_generation{0};
  std::uint64_t preliminary_ddl_create_table_executor_availability_generation{0};
  std::uint64_t preliminary_ddl_create_index_executor_availability_generation{0};
  std::uint64_t preliminary_ddl_drop_index_executor_availability_generation{0};
  // Schema 7032 v70+ copy-only projection of the exact engine-issued TXBH for
  // `transaction`.  The parser may copy these bytes into TXCO/TXRO binding;
  // possession of them is never private transaction-handle authority.
  std::vector<std::uint8_t> preliminary_active_transaction_handle;
  scratchbird::core::platform::Uuid literal_preliminary_receipt_uuid;
  scratchbird::core::platform::Uuid literal_catalog_snapshot_uuid;
  std::uint64_t literal_catalog_generation{0};
  std::uint64_t literal_security_epoch{0};
  std::uint64_t literal_resource_epoch{0};
  scratchbird::core::platform::Uuid literal_mga_snapshot_uuid;

  [[nodiscard]] bool complete() const {
    return acquired && transaction.present() && !statement_uuid.is_nil() &&
           !statement_snapshot_uuid.is_nil() &&
           !statement_metadata_snapshot_uuid.is_nil() &&
           !catalog_epoch_uuid.is_nil() && !security_context_uuid.is_nil();
  }

  [[nodiscard]] bool native_v7_complete() const {
    return complete() && !statement_timestamp.empty() &&
           !bound_ast_uuid.is_nil() && !count_function_uuid.is_nil() &&
           !sum_function_uuid.is_nil() && !avg_function_uuid.is_nil() &&
           !min_function_uuid.is_nil() && !max_function_uuid.is_nil() &&
           aggregate_function_profiles.size() == 43 &&
           window_function_profiles.size() == 11 &&
           descriptor_profiles.size() == 320;
  }

  [[nodiscard]] bool native_v8_complete() const {
    return complete() && !statement_timestamp.empty() &&
           !bound_ast_uuid.is_nil() && !count_function_uuid.is_nil() &&
           !sum_function_uuid.is_nil() && !avg_function_uuid.is_nil() &&
           !min_function_uuid.is_nil() && !max_function_uuid.is_nil() &&
           aggregate_function_profiles.size() == 43 &&
           window_function_profiles.size() == 11 &&
           descriptor_profiles.size() == 322;
  }

  [[nodiscard]] bool native_v9_complete() const {
    return complete() && !statement_timestamp.empty() &&
           !bound_ast_uuid.is_nil() && !count_function_uuid.is_nil() &&
           !sum_function_uuid.is_nil() && !avg_function_uuid.is_nil() &&
           !min_function_uuid.is_nil() && !max_function_uuid.is_nil() &&
           aggregate_function_profiles.size() == 43 &&
           window_function_profiles.size() == 11 &&
           descriptor_profiles.size() == 326;
  }

  [[nodiscard]] bool native_v10_complete() const {
    return complete() && !statement_timestamp.empty() &&
           !bound_ast_uuid.is_nil() && !count_function_uuid.is_nil() &&
           !sum_function_uuid.is_nil() && !avg_function_uuid.is_nil() &&
           !min_function_uuid.is_nil() && !max_function_uuid.is_nil() &&
           aggregate_function_profiles.size() == 43 &&
           window_function_profiles.size() == 11 &&
           descriptor_profiles.size() == 646;
  }
};

// Exact canonical ingress bytes for one engine-issued statement identity.
// The private receipt never crosses SBPS and is intentionally absent here.
struct ParserCanonicalSblrSubmission {
  scratchbird::core::platform::Uuid statement_uuid;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_operation_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  scratchbird::core::platform::Uuid literal_final_receipt_uuid;
  scratchbird::core::platform::Uuid literal_admission_token_uuid;
  std::array<std::uint8_t, 32> literal_token_binding_sha256{};
  std::array<std::uint8_t, 32> literal_bound_ast_sha256{};
  std::array<std::uint8_t, 32> literal_sbxn_sha256{};
  std::array<std::uint8_t, 32> literal_sbos_sha256{};
  // Exact engine-issued SBPE and parser-encoded SBPV. The client does not
  // interpret either as identity authority; the server revalidates both.
  std::vector<std::uint8_t> parameter_execution_extension_bytes;
  std::vector<std::uint8_t> parameter_value_set_bytes;
  std::vector<std::uint8_t> variable_execution_extension_bytes;

  [[nodiscard]] bool literal_finalized() const {
    return !literal_final_receipt_uuid.is_nil() &&
           !literal_admission_token_uuid.is_nil();
  }

  [[nodiscard]] bool parameter_finalized() const {
    return parameter_execution_extension_bytes.size() == 176 &&
           !parameter_value_set_bytes.empty() &&
           parameter_value_set_bytes.size() <= 33554432;
  }
  [[nodiscard]] bool variable_finalized() const {
    return variable_execution_extension_bytes.size() == 192;
  }

  [[nodiscard]] bool complete() const {
    return !statement_uuid.is_nil() && !canonical_container_bytes.empty() &&
           !canonical_execution_envelope_bytes.empty();
  }
};

enum class ParserTransactionFinality : std::uint8_t {
  kNotApplicable = 0,
  kKnownApplied = 1,
  kKnownNotApplied = 2,
  kUnknown = 3,
};

enum class ParserTransactionReplacementReason : std::uint8_t {
  kNone = 0,
  kRetaining = 1,
  kOrdinaryReady = 2,
  kLastActiveReady = kOrdinaryReady,  // Historical name, not a cardinality condition.
  kAutocommitReady = 3,
};

// Server-published session state used only to frame SBPS requests.  The engine
// remains the authority for transaction identity, visibility, and finality.
struct ParserSessionContext {
  bool authenticated{false};
  bool transaction_routing_v2_negotiated{false};
  bool prepared_metadata_transfer_v1_negotiated{false};
  bool relation_descriptor_projection_v3_negotiated{false};
  // Copied from the exact HELLO bytes used on this physical parser channel.
  // These fields are identity evidence only; the server independently keeps
  // and cross-checks the admitted values.
  scratchbird::core::platform::Uuid admitted_parser_package_uuid;
  scratchbird::core::platform::Uuid admitted_dialect_profile_uuid;
  std::uint32_t admitted_parser_package_version_major{0};
  std::uint32_t admitted_parser_package_version_minor{0};
  std::uint32_t admitted_parser_package_version_patch{0};
  scratchbird::core::platform::Uuid session_uuid;
  scratchbird::core::platform::Uuid connection_uuid;
  scratchbird::core::platform::Uuid database_uuid;
  scratchbird::core::platform::Uuid authenticated_user_uuid;
  std::string principal_claim;
  std::string auth_provider_family;
  std::vector<scratchbird::core::platform::Uuid> effective_role_uuids;
  std::vector<scratchbird::core::platform::Uuid> effective_group_uuids;
  std::string default_language{"en"};
  std::string language_profile;
  std::string language_tag{"en"};
  std::string input_syntax_profile;
  std::string input_language_fallback_tag;
  std::string common_resource_hash;
  scratchbird::core::platform::Uuid dialect_profile_uuid;
  scratchbird::core::platform::Uuid policy_profile_uuid;
  std::string resource_compatibility_identity;
  std::string resource_version_identity;
  std::uint64_t language_resource_epoch{0};
  std::uint64_t localized_name_epoch{0};
  std::uint64_t message_resource_epoch{0};
  std::uint64_t udr_epoch{0};
  std::vector<std::string> search_path;
  std::string transaction_context;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  scratchbird::core::platform::Uuid transaction_uuid;
  std::string transaction_timestamp;
  std::uint64_t security_policy_epoch{0};
  std::uint64_t grant_epoch{0};
  std::uint64_t catalog_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::string result_rendering_policy{"default"};
  std::string metric_redaction_policy{"default"};
};

} // namespace scratchbird::parser::ipc
