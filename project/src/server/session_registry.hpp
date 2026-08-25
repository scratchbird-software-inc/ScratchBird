// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_AUTH_SESSION_ATTACH

#pragma once

#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "sbps.hpp"

#include "agent_background_jobs.hpp"

#include "scratchbird/engine/engine.h"
#include "prepared_metadata_binding.hpp"
#include "statement_context.hpp"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
struct DurableAuthorizationMaterializeResult;
struct EngineLanguageContext;
struct EngineMaterializedAuthorizationContext;
struct EngineRequestContext;
struct EngineSecurityPrincipalLifecycleState;
}

namespace scratchbird::server {

enum class ServerChannelState {
  kProtocolAdmitted,
  kAuthPending,
  kAttachPending,
  kSessionBound,
  kReady,
  kDraining,
  kDetached,
  kClosed,
  kFailed,
};

enum class ServerRequestLifecycleState {
  kAdmitted,
  kActive,
  kCursorOpen,
  kCompleted,
  kCancelled,
  kTimedOut,
  kDrained,
  kDisconnected,
  kUnknownOutcome,
  kFailed,
};

enum class ServerDriverTransactionEvent {
  kAttachInitialBoundary,
  kAutocommitStatementSucceeded,
  kAutocommitStatementFailed,
  kCommitCompleted,
  kRollbackCompleted,
  kPrepareTransactionCompleted,
  kCancelStatement,
  kResetSession,
  kReconnectAfterDisconnect,
  kPoolReturn,
  kSavepointOperation,
  kXaRecoverPrepared,
  kDormantDetach,
  kDormantReattach,
  kRetryAfterUnknownFinality,
};

enum class ServerTransactionPressureAction {
  kNoAction,
  kWarnNotify,
  kRequestRestart,
  kRequestReauth,
  kRequestCancel,
  kForceRollback,
  kForceCommit,
  kForceRestart,
};

struct ServerRequestLifecyclePolicy {
  std::uint64_t fetch_timeout_ms = 30000;
  std::uint64_t cancel_timeout_ms = 5000;
  std::uint64_t drain_timeout_ms = 30000;
};

enum class ServerTransactionLifecycleState {
  kActive,
  kFinalityUnknown,
};

// Engine-issued transaction identity and state owned by one server session.
// The parser/server layer routes this state but never manufactures identity or
// visibility decisions.
struct ServerTransactionState {
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_uuid;
  std::string transaction_timestamp;
  std::string isolation_level = "read_committed";
  bool read_only = false;
  std::string wait_mode;
  std::uint64_t lock_timeout_ms = 0;
  bool lock_timeout_present = false;
  ServerTransactionLifecycleState lifecycle_state =
      ServerTransactionLifecycleState::kActive;
  std::uint64_t begin_ordinal = 0;
  // Catalog/name-cache effects from transaction-routed DDL are not visible to
  // other transactions until engine finality is known applied.  Keep only the
  // neutral mutation operation identifiers here; commit applies invalidation
  // and rollback simply discards this transaction-owned list.
  std::vector<std::string> deferred_catalog_cache_mutations;
};

struct ServerSessionRecord {
  std::array<std::uint8_t, 16> connection_uuid{};
  // Server-issued physical SBPS channel identity.  Unlike connection_uuid,
  // this value is not chosen by the client and prevents a second socket from
  // replaying a negotiated connection/session tuple.
  std::array<std::uint8_t, 16> server_channel_uuid{};
  // Drain/service state is scoped to this bound session's physical channel.
  // The registry-wide lifecycle summary must never drain sibling channels.
  ServerChannelState channel_state = ServerChannelState::kReady;
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> principal_uuid{};
  std::array<std::uint8_t, 16> effective_user_uuid{};
  std::string principal_claim;
  std::string provider_family;
  std::string requested_role_name;
  std::string database_path;
  std::string database_uuid;
  std::string resource_seed_pack_root;
  std::string policy_seed_pack_root;
  std::string attach_mode = "read_write";
  std::uint64_t catalog_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t grant_epoch = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t config_source_epoch = 1;
  std::uint64_t config_reload_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t security_provider_generation = 1;
  std::uint64_t cache_invalidation_epoch = 1;
  std::uint64_t name_resolution_epoch = 1;
  std::uint64_t resource_epoch = 1;
  std::string role_set_hash = "roles/default";
  std::string group_set_hash = "groups/default";
  std::vector<std::string> engine_authorization_trace_tags;
  std::string search_path_hash = "search_path/default";
  std::string language_profile = "sbsql.builtin.recovery.en";
  std::string language_tag;
  std::string default_language_tag = "en";
  std::string input_syntax_profile = "sbsql.syntax.standard";
  std::string input_language_fallback_tag;
  std::string common_resource_hash = "builtin.common.sbsql.v1";
  std::uint64_t language_resource_epoch = 1;
  std::uint64_t localized_name_epoch = 1;
  std::uint64_t message_resource_epoch = 1;
  std::string resource_compatibility_identity = "sbsql.resource.compat.v1";
  std::string resource_version_identity = "sbsql.resource-pack.v1";
  std::string application_name;
  std::string database_engine_agent_state = "not_started";
  std::uint64_t database_engine_agent_health_generation = 0;
  bool database_engine_agent_ordinary_admission_allowed = false;
  std::string database_engine_agent_health_json;
  std::string config_policy_security_lifecycle_json;
  std::string default_transaction_isolation_level = "read_committed";
  bool default_transaction_read_only = false;
  bool embedded_in_process = false;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_uuid;
  std::string transaction_timestamp;
  // The scalar fields above remain the V1/default projection.  V2 callers use
  // an exact entry in this map, validated under transaction_mutex.
  std::map<std::uint64_t, ServerTransactionState> transactions_by_local_id;
  // Exact engine-issued identities that could not safely enter the keyed
  // active map (for example, a duplicate local id).  Recovery accounts for
  // them without ever projecting them as active/default transactions.
  std::vector<ServerTransactionState> quarantined_unpublished_transactions;
  std::uint64_t default_local_transaction_id = 0;
  std::uint64_t next_transaction_begin_ordinal = 1;
  std::shared_ptr<std::mutex> transaction_mutex =
      std::make_shared<std::mutex>();
  // This is server-owned negotiation state copied from the admitted physical
  // SBPS channel at authentication time.  A V2 payload cannot self-enable the
  // capability.
  bool transaction_routing_v2_negotiated = false;
  // A transferable prepared metadata binding is an engine-owned object
  // negotiated by the physical SBPS channel.  It allows prepare-time metadata
  // to be paired with a different exact execute-time data transaction without
  // transferring data visibility or finality authority.
  bool prepared_metadata_transfer_v1_negotiated = false;
  // Server-owned admission evidence for neutral persisted relation
  // projection.  A schema-7007 payload cannot self-enable this capability.
  bool relation_descriptor_projection_v3_negotiated = false;
  // Exact parser identity admitted by HELLO on the owning physical channel.
  // Execute payloads cannot replace or self-assert these values.
  std::array<std::uint8_t, 16> admitted_parser_package_uuid{};
  std::array<std::uint8_t, 16> admitted_dialect_profile_uuid{};
  std::uint32_t admitted_parser_package_version_major = 0;
  std::uint32_t admitted_parser_package_version_minor = 0;
  std::uint32_t admitted_parser_package_version_patch = 0;
  bool detached_recovery_quarantined = false;
  bool session_binding_present = false;
  std::array<std::uint8_t, 16> attachment_id{};
  std::array<std::uint8_t, 16> catalog_session_id{};
  std::array<std::uint8_t, 16> protocol_session_id{};
  std::array<std::uint8_t, 16> authkey_id{};
  std::array<std::uint8_t, 16> active_role_uuid{};
  std::vector<std::array<std::uint8_t, 16>> effective_role_uuids;
  std::vector<std::array<std::uint8_t, 16>> effective_group_uuids;
  std::uint64_t session_binding_generation = 0;
  std::uint64_t session_binding_control_sequence = 0;
  std::uint64_t takeover_generation = 0;
  std::uint64_t takeover_control_sequence = 0;
  std::string session_binding_authority_class;
  std::string session_binding_actor_token;
};

struct ServerFinalityRecord {
  std::array<std::uint8_t, 16> finality_token_uuid{};
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::string operation;
  std::string state = "pending";
  std::string detail;
  std::uint64_t policy_generation = 1;
};

struct ServerAuthorityCacheEpochVector {
  std::uint64_t catalog_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t grant_epoch = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t cache_invalidation_epoch = 1;
  std::uint64_t name_resolution_epoch = 1;
  std::uint64_t resource_epoch = 1;
  std::string role_set_hash = "roles/default";
  std::string group_set_hash = "groups/default";
  std::string search_path_hash = "search_path/default";
};

struct ServerPreparedStatementRecord {
  std::array<std::uint8_t, 16> prepared_statement_uuid{};
  std::array<std::uint8_t, 16> client_statement_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> principal_uuid{};
  std::array<std::uint8_t, 16> effective_user_uuid{};
  std::string database_uuid;
  std::string statement_name;
  std::string encoded_sblr_envelope;
  std::string operation_family;
  std::string operation_id;
  bool requires_public_abi_dispatch = false;
  std::uint64_t row_count_hint = 0;
  std::uint64_t catalog_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t grant_epoch = 1;
  std::uint64_t policy_generation = 1;
  std::string role_set_hash = "roles/default";
  std::string group_set_hash = "groups/default";
  std::string search_path_hash = "search_path/default";
  std::string language_profile = "sbsql.builtin.recovery.en";
  std::string language_tag;
  std::string default_language_tag = "en";
  std::string input_syntax_profile = "sbsql.syntax.standard";
  std::string input_language_fallback_tag;
  std::string common_resource_hash = "builtin.common.sbsql.v1";
  std::uint64_t language_resource_epoch = 1;
  std::uint64_t localized_name_epoch = 1;
  std::uint64_t message_resource_epoch = 1;
  std::string resource_compatibility_identity = "sbsql.resource.compat.v1";
  std::string resource_version_identity = "sbsql.resource-pack.v1";
  std::uint64_t session_object_handle_id = 0;
  std::uint64_t session_object_handle_generation = 0;
  std::string target_object_uuid;
  std::string target_object_kind;
  std::string target_operation_id;
  std::string target_column_set_hash;
  std::string authority_dependency_uuid;
  std::string authority_dependency_kind;
  std::string authority_dependency_operation_id;
  std::string authority_dependency_column_set_hash;
  std::string authority_proof_hash_algorithm = "sha256";
  std::string authority_proof_hash;
  std::uint64_t prepare_local_transaction_id = 0;
  std::string prepare_transaction_uuid;
  std::uint64_t prepare_snapshot_visible_through_local_transaction_id = 0;
  // V1 prepare records the default transaction authority context for
  // validation, but it is not a transaction-routed prepare.  Preserve the
  // wire-origin distinction so V1 and V2 prepared execution cannot be
  // confused merely because both carry a transaction identity.
  bool prepared_transaction_routing_v2 = false;
  // Set only after the server has created an opaque engine-owned metadata
  // binding for this prepared statement.  An envelope flag alone never makes
  // a statement transferable.
  bool prepared_metadata_transferable = false;
  scratchbird::server_engine_bridge::PreparedMetadataBindingHandle
      prepared_metadata_binding = nullptr;
  bool closed = false;
};

struct ServerPreparedExecutionContextRecord {
  std::array<std::uint8_t, 16> prepared_statement_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> principal_uuid{};
  std::array<std::uint8_t, 16> effective_user_uuid{};
  std::string database_uuid;
  std::string operation_id;
  std::string target_object_uuid;
  std::string statement_shape_hash;
  std::string authority_proof_hash;
  ServerAuthorityCacheEpochVector epoch_vector;
  bool grants_authority = false;
};

struct ServerPreparedStatementCloseSummary {
  bool found = false;
  bool already_closed = false;
  std::uint64_t cursors_closed = 0;
  std::uint64_t engine_results_released = 0;
  bool session_object_handle_revoked = false;
};

struct ServerSessionObjectHandleRecord {
  std::uint64_t handle_id = 0;
  std::uint64_t generation = 0;
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> principal_uuid{};
  std::array<std::uint8_t, 16> effective_user_uuid{};
  std::string database_uuid;
  std::string object_uuid;
  std::string object_kind;
  std::string operation_id;
  std::string column_set_hash;
  std::uint64_t catalog_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t grant_epoch = 1;
  std::uint64_t policy_generation = 1;
  std::string role_set_hash = "roles/default";
  std::string group_set_hash = "groups/default";
  std::string search_path_hash = "search_path/default";
  bool closed = false;
};

struct ServerSessionObjectHandleValidation {
  bool accepted = false;
  std::string detail;
  const ServerSessionObjectHandleRecord* handle = nullptr;
};

struct ServerAuthorityCacheRecord {
  std::string cache_key;
  std::string cache_kind;
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> principal_uuid{};
  std::array<std::uint8_t, 16> effective_user_uuid{};
  std::string database_uuid;
  std::string operation_id;
  std::string target_object_uuid;
  std::string statement_shape_hash;
  ServerAuthorityCacheEpochVector epoch_vector;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  bool refusal = false;
  bool grants_authority = false;
  std::uint64_t generation = 0;
  std::uint64_t hit_count = 0;
};

struct ServerAuthorityCacheValidation {
  bool accepted = false;
  bool stale = false;
  bool cross_session = false;
  bool cross_authorization = false;
  bool grants_authority = false;
  std::string detail;
  const ServerAuthorityCacheRecord* record = nullptr;
};

struct ServerPublicNameResolutionCacheRecord {
  std::string cache_key;
  std::array<std::uint8_t, 16> effective_user_uuid{};
  std::string database_uuid;
  std::string object_uuid;
  std::string canonical_name;
  std::string object_class;
  std::uint64_t catalog_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t grant_epoch = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t name_resolution_epoch = 1;
  std::uint64_t language_resource_epoch = 1;
  std::uint64_t localized_name_epoch = 1;
  std::uint64_t message_resource_epoch = 1;
  std::string role_set_hash = "roles/default";
  std::string group_set_hash = "groups/default";
  std::string search_path_hash = "search_path/default";
  std::string language_profile = "sbsql.builtin.recovery.en";
  std::string language_tag;
  std::string input_syntax_profile = "sbsql.syntax.standard";
  std::string input_language_fallback_tag;
  std::string common_resource_hash = "builtin.common.sbsql.v1";
  std::string resource_compatibility_identity = "sbsql.resource.compat.v1";
  std::string resource_version_identity = "sbsql.resource-pack.v1";
  std::uint64_t generation = 0;
  std::uint64_t hit_count = 0;
};

struct ServerLanguageContextIdentity {
  std::string language_profile_id = "sbsql.builtin.recovery.en";
  std::string language_tag = "en";
  std::string default_language_tag = "en";
  std::string input_syntax_profile = "sbsql.syntax.standard";
  std::string input_language_fallback_tag;
  std::string common_resource_hash = "builtin.common.sbsql.v1";
  std::uint64_t language_resource_epoch = 1;
  std::uint64_t localized_name_epoch = 1;
  std::uint64_t message_resource_epoch = 1;
  std::string resource_compatibility_identity = "sbsql.resource.compat.v1";
  std::string resource_version_identity = "sbsql.resource-pack.v1";
};

struct ServerLanguageBundleRecord {
  std::string bundle_uuid;
  std::string language_profile_id;
  std::string language_tag;
  std::string dialect_profile_uuid;
  std::string topology_profile_uuid;
  std::string common_resource_hash;
  std::string resource_hash;
  bool loaded = false;
  bool required_profile = false;
  std::uint64_t language_resource_epoch = 0;
};

struct ServerLanguageResourceDirectoryRecord {
  std::string directory_id;
  std::string directory_path;
  std::string manifest_hash;
  std::string signing_key_id;
  std::string scan_evidence_id;
  std::string audit_reason;
  bool signed_manifest_verified = false;
  bool admitted_by_security_policy = false;
  bool compatible_with_server = true;
  bool active = false;
  std::uint64_t language_resource_epoch = 0;
  std::uint64_t localized_name_epoch = 0;
  std::uint64_t message_resource_epoch = 0;
};

struct ServerCursorRecord {
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> finality_token_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> prepared_statement_uuid{};
  std::string operation_id;
  std::string cursor_name;
  std::string row_packet;
  std::string bulk_stream_kind;
  std::vector<std::string> bulk_reject_records;
  std::string multi_result_kind;
  std::string warning_stream_kind;
  std::string finality_kind;
  std::string finality_state = "active";
  std::string finality_reason;
  std::uint64_t owning_local_transaction_id = 0;
  std::uint64_t owning_snapshot_visible_through_local_transaction_id = 0;
  std::string owning_transaction_uuid;
  bool holdable_after_commit = false;
  sb_engine_result_t engine_result = nullptr;
  // Non-empty only while this cursor owns the live private engine statement
  // receipt associated with its canonical execution. The receipt itself
  // remains in the server-owned statement-context registry and never crosses
  // the protocol boundary.
  std::string statement_context_statement_uuid;
  std::uint64_t bulk_total_rows = 0;
  std::uint64_t bulk_rejected_rows = 0;
  std::uint64_t multi_result_count = 0;
  std::uint64_t warning_count = 0;
  std::uint64_t partial_result_rows = 0;
  std::uint64_t finality_after_fetches = 0;
  std::uint64_t total_row_count = 0;
  std::uint64_t next_row_index = 0;
  std::uint64_t max_chunk_rows = 4;
  std::uint64_t max_chunk_bytes = 65536;
  std::array<std::uint8_t, 16> stream_descriptor_uuid{};
  std::uint16_t stream_descriptor_version = 0;
  std::uint64_t stream_descriptor_generation = 0;
  std::array<std::uint8_t, 16> execution_uuid{};
  std::array<std::uint8_t, 16> result_set_uuid{};
  std::array<std::uint8_t, 16> row_descriptor_uuid{};
  std::array<std::uint8_t, 16> snapshot_uuid{};
  std::array<std::uint8_t, 32> stream_descriptor_receipt_binding_sha256{};
  bool stream_descriptor_live = false;
  std::uint64_t fetch_count = 0;
  bool exhausted = false;
  bool closed = false;
};

struct ServerRequestRecord {
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> finality_token_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> prepared_statement_uuid{};
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::string request_kind;
  std::string operation_id;
  ServerRequestLifecycleState state = ServerRequestLifecycleState::kAdmitted;
  std::string detail;
  std::uint64_t local_transaction_id_at_start = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_uuid_at_start;
  std::uint64_t fetch_timeout_ms = 30000;
  std::uint64_t cancel_timeout_ms = 5000;
  std::uint64_t drain_timeout_ms = 30000;
  bool authorization_proven = false;
  bool transaction_finality_preserved = true;
  bool engine_result_retained = false;
};

struct ServerPublicAbiSessionContext {
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t engine_session = nullptr;
  std::string database_path;
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> effective_user_uuid{};
  bool embedded_in_process = false;
  std::uint64_t reuse_count = 0;
};

// Server-owned association between a parser-visible statement identity and
// the private engine receipt. The opaque handle and complete MGA vector never
// cross SBPS; only the bounded immutable projection is returned to the parser.
struct ServerStatementContextRecord {
  std::array<std::uint8_t, 16> session_uuid{};
  std::string statement_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  std::string owning_transaction_uuid;
  scratchbird::server_engine_bridge::StatementContextReceiptHandle receipt;
  scratchbird::server_engine_bridge::StatementContextReceiptView view;
  bool released = false;
  std::string variable_final_receipt_uuid, variable_admission_token_uuid;
  std::array<std::uint8_t,32> variable_binding_sha256{};
  bool variable_binding_finalized = false, variable_token_consumed = false;
};

struct ServerParameterExecutionCoordinationRecord {
  std::string session_uuid;
  std::string operation_uuid;
  scratchbird::server_engine_bridge::StatementParameterExecutionMode mode =
      scratchbird::server_engine_bridge::StatementParameterExecutionMode::kDirect;
  std::uint64_t private_handle = 0;
  std::uint64_t generation = 0;
  bool sealed = false;
};
struct ServerVariableFrameMappingRecord {
  std::uint64_t occurrence_id = 0;
  std::uint32_t variable_ordinal = 0;
  std::string variable_descriptor_uuid, datatype_descriptor_uuid,
      datatype_type_uuid;
  std::uint64_t variable_descriptor_generation = 0,
      datatype_descriptor_generation = 0, value_generation = 0;
  std::uint8_t nullable = 0, mutability = 0, value_state = 0;
};
struct ServerVariableFrameRecord {
  std::string session_uuid, transaction_uuid, operation_uuid,
      public_coordination_uuid, scope_uuid, frame_uuid,
      registry_snapshot_uuid;
  std::uint64_t scope_generation = 0, frame_generation = 0,
      coordinator_generation = 0, registry_generation = 0;
  std::vector<ServerVariableFrameMappingRecord> mappings;
  bool acquired = false, revoked = false;
};

struct ServerAdmittedParserChannelIdentity {
  std::array<std::uint8_t, 16> parser_package_uuid{};
  std::array<std::uint8_t, 16> dialect_profile_uuid{};
  std::uint32_t parser_package_version_major = 0;
  std::uint32_t parser_package_version_minor = 0;
  std::uint32_t parser_package_version_patch = 0;
};

struct ServerSessionRegistry {
  ServerChannelState channel_state = ServerChannelState::kProtocolAdmitted;
  std::map<std::string, ServerSessionRecord> sessions_by_uuid;
  std::map<std::string, ServerSessionRecord> auth_contexts_by_uuid;
  std::map<std::string, std::array<std::uint8_t, 32>>
      negotiated_capabilities_by_connection_uuid;
  std::map<std::string, ServerAdmittedParserChannelIdentity>
      admitted_parser_identity_by_connection_uuid;
  std::map<std::string, std::array<std::uint8_t, 16>>
      physical_channel_by_connection_uuid;
  std::map<std::string, ServerFinalityRecord> finality_by_request_uuid;
  std::map<std::string, ServerRequestRecord> requests_by_uuid;
  std::map<std::string, ServerPreparedStatementRecord> prepared_by_uuid;
  std::map<std::string, ServerPreparedExecutionContextRecord>
      prepared_execution_contexts_by_uuid;
  std::map<std::string, ServerSessionObjectHandleRecord> object_handles_by_key;
  std::map<std::string, ServerAuthorityCacheRecord> authority_cache_by_key;
  std::map<std::string, ServerPublicNameResolutionCacheRecord>
      public_name_resolution_cache_by_key;
  std::deque<std::string> public_name_resolution_cache_lru;
  std::map<std::string, ServerPublicNameResolutionCacheRecord>
      stable_public_name_resolution_cache_by_key;
  std::deque<std::string> stable_public_name_resolution_cache_lru;
  std::map<std::string, ServerCursorRecord> cursors_by_uuid;
  std::map<std::string, ServerPublicAbiSessionContext>
      public_abi_sessions_by_session_uuid;
  std::map<std::string, ServerStatementContextRecord>
      statement_contexts_by_statement_uuid;
  std::map<std::string, ServerParameterExecutionCoordinationRecord>
      parameter_coordinations_by_uuid;
  std::map<std::string, ServerVariableFrameRecord>
      variable_frames_by_coordination_uuid;
  std::shared_ptr<std::mutex> statement_context_mutex =
      std::make_shared<std::mutex>();
  std::map<std::string, ServerLanguageBundleRecord> language_bundles_by_uuid;
  std::map<std::string, ServerLanguageResourceDirectoryRecord>
      language_resource_directories_by_id;
  std::map<std::string, scratchbird::core::agents::DatabaseLocalBackgroundJobScheduler>
      job_schedulers_by_database_uuid;
  std::map<std::string, scratchbird::core::agents::WorkloadResourceQuotaController>
      job_quotas_by_database_uuid;
  std::uint64_t next_session_object_handle_id = 1;
  std::uint64_t next_authority_cache_generation = 1;
  std::uint64_t next_public_name_resolution_cache_generation = 1;
};

struct AuthHandoffPayload {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::string provider_family = "local_password";
  std::string principal_claim;
  std::string credential_evidence;
  std::string application_name;
  std::string requested_role;
  bool credential_evidence_present = false;
  bool credential_invalid = false;
  bool mfa_required = false;
  bool mfa_evidence_present = false;
  std::string requested_database;
  std::string requested_language = "en";
};

struct AttachPayload {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::string requested_database;
  std::string requested_attachment_mode = "read_write";
};

struct ServerTransactionResponseState {
  enum class Finality : std::uint8_t {
    kNotApplicable = 0,
    kKnownApplied = 1,
    kKnownNotApplied = 2,
    kUnknown = 3,
  };
  enum class ReplacementReason : std::uint8_t {
    kNone = 0,
    kRetaining = 1,
    kLastActiveReady = 2,
    kAutocommitReady = 3,
  };
  bool selected_present = false;
  ServerTransactionState selected;
  Finality finality = Finality::kNotApplicable;
  bool finality_applied = false;
  // Set only when a known-applied commit consumes at least one deferred
  // catalog mutation and applies the corresponding cache invalidation.
  bool catalog_invalidation_applied = false;
  bool finalized_present = false;
  ServerTransactionState finalized;
  bool replacement_present = false;
  ServerTransactionState replacement;
  ReplacementReason replacement_reason = ReplacementReason::kNone;
  std::string outcome_detail;
  std::string diagnostic_code;
};

struct SessionOperationResult {
  bool accepted = false;
  // Set by the authoritative execute ingress handler. Serializers use this
  // typed decision instead of attempting to recover the request schema after
  // routing or V2 result adaptation.
  bool cursor_stream_descriptor_trailer_required = false;
  std::uint16_t response_message_type = 0;
  std::uint32_t response_schema_id = 0;
  std::uint32_t frame_flags = 0;
  std::array<std::uint8_t, 16> session_uuid{};
  std::vector<std::uint8_t> payload;
  std::vector<ServerDiagnostic> diagnostics;
  std::optional<ServerTransactionResponseState> transaction_state;
};

struct ServerRequestLifecycleResult {
  bool accepted = false;
  bool error = false;
  bool unknown_outcome = false;
  std::string outcome;
  std::string records_json = "[]";
  std::optional<ServerRequestRecord> record;
  std::vector<ServerDiagnostic> diagnostics;
};

struct ServerDriverTransactionDecisionInput {
  ServerDriverTransactionEvent event = ServerDriverTransactionEvent::kAttachInitialBoundary;
  bool active_transaction = false;
  bool active_cursor = false;
  bool engine_finality_known = true;
  bool statement_has_side_effects = true;
  bool engine_reported_idempotent = false;
  bool caller_acknowledged_retry_boundary = false;
  bool explicit_dormant_token = false;
  bool dormant_reattach_enabled = false;
  bool server_admitted_reattach = false;
  bool prepared_transaction_present = false;
  bool xa_recovery_enabled = false;
  bool cluster_authority_active = false;
};

struct ServerDriverTransactionDecision {
  bool accepted = false;
  bool driver_may_retry = false;
  bool hidden_retry_forbidden = true;
  bool must_query_engine_finality = false;
  bool opens_replacement_boundary = false;
  bool preserves_current_boundary = false;
  bool invalidates_session = false;
  bool requires_explicit_engine_recovery = false;
  std::string sqlstate = "00000";
  std::string diagnostic_code;
  std::string finality_state = "no_state_change";
  std::string boundary_state = "TX_BOUNDARY_ACTIVE";
  std::string durable_state = "TX_DURABLE_ACTIVE";
  std::string action;
  std::vector<ServerDiagnostic> diagnostics;
};

struct ServerTransactionPressureControlInput {
  ServerTransactionPressureAction action = ServerTransactionPressureAction::kNoAction;
  bool agent_authoritative = false;
  bool policy_authorized = false;
  bool session_authorization_bound = false;
  bool active_transaction = false;
  bool engine_finality_known = true;
  bool force_authority_gate = false;
  bool replacement_transaction_bound = false;
  std::uint64_t current_local_transaction_id = 0;
  std::uint64_t replacement_local_transaction_id = 0;
  std::string stable_session_id;
  std::string evidence_id;
};

struct ServerTransactionPressureControlDecision {
  bool accepted = false;
  bool notifies_client = false;
  bool requests_client_action = false;
  bool mutates_transaction = false;
  bool opens_replacement_boundary = false;
  bool denied_non_authoritative = false;
  bool must_query_engine_finality = false;
  std::string diagnostic_code;
  std::string detail;
  std::string evidence;
  std::vector<ServerDiagnostic> diagnostics;
};

constexpr std::uint16_t kServerTakeoverClaimAttachmentId = 0x0001;
constexpr std::uint16_t kServerTakeoverClaimCatalogSessionId = 0x0002;
constexpr std::uint16_t kServerTakeoverClaimProtocolSessionId = 0x0004;
constexpr std::uint16_t kServerTakeoverClaimAuthkeyId = 0x0008;
constexpr std::uint16_t kServerTakeoverClaimAuthenticatedPrincipalId = 0x0010;
constexpr std::uint16_t kServerTakeoverClaimSessionUserId = 0x0020;
constexpr std::uint16_t kServerTakeoverClaimActiveRoleId = 0x0040;
constexpr std::uint16_t kServerTakeoverClaimCurrentTxnId = 0x0080;

constexpr std::uint8_t kServerTakeoverProbeSessionBound = 0x01;
constexpr std::uint8_t kServerTakeoverProbeAuthorityAccepted = 0x02;
constexpr std::uint8_t kServerTakeoverProbeActiveTransaction = 0x04;
constexpr std::uint8_t kServerTakeoverProbeTakeoverWouldPass = 0x08;

struct ServerSessionBindingReport {
  std::array<std::uint8_t, 16> attachment_id{};
  std::array<std::uint8_t, 16> catalog_session_id{};
  std::array<std::uint8_t, 16> transaction_uuid{};
  std::array<std::uint8_t, 16> protocol_session_id{};
  std::array<std::uint8_t, 16> authenticated_principal_id{};
  std::array<std::uint8_t, 16> session_user_id{};
  std::array<std::uint8_t, 16> active_role_id{};
  std::array<std::uint8_t, 16> authkey_id{};
  std::uint64_t current_txn_id = 0;
  std::vector<std::array<std::uint8_t, 16>> effective_group_ids;
};

struct ServerSessionTakeoverRequest {
  std::uint16_t mask = 0;
  std::array<std::uint8_t, 16> attachment_id{};
  std::array<std::uint8_t, 16> catalog_session_id{};
  std::array<std::uint8_t, 16> protocol_session_id{};
  std::array<std::uint8_t, 16> authkey_id{};
  std::array<std::uint8_t, 16> authenticated_principal_id{};
  std::array<std::uint8_t, 16> session_user_id{};
  std::array<std::uint8_t, 16> active_role_id{};
  std::uint64_t current_txn_id = 0;
  std::vector<std::array<std::uint8_t, 16>> group_ids;
};

struct ServerSessionControlAuthority {
  bool authenticated = false;
  bool may_report_binding = false;
  bool may_clear_binding = false;
  bool may_takeover = false;
  std::uint64_t sequence = 0;
  std::string authority_class;
  std::string actor_token;
};

struct ServerSessionBindingControlResult {
  bool accepted = false;
  bool mutated = false;
  bool replay_rejected = false;
  bool authorization_denied = false;
  bool takeover_allowed = false;
  std::uint8_t probe_flags = 0;
  std::string diagnostic_code;
  std::string detail;
  std::string target_session_uuid;
  std::vector<ServerDiagnostic> diagnostics;
};

const char* ServerChannelStateName(ServerChannelState state);
const char* ServerRequestLifecycleStateName(ServerRequestLifecycleState state);
const char* ServerDriverTransactionEventName(ServerDriverTransactionEvent event);
const char* ServerTransactionPressureActionName(ServerTransactionPressureAction action);
std::string UuidBytesToText(const std::array<std::uint8_t, 16>& uuid);
bool IsCompleteEngineTransactionIdentity(std::uint64_t local_transaction_id,
                                         std::string_view transaction_uuid);
ServerLanguageContextIdentity ServerLanguageContextForSession(
    const ServerSessionRecord& session);
scratchbird::engine::internal_api::EngineMaterializedAuthorizationContext
MaterializeDurableManagementAuthorizationContext(
    const ServerSessionRecord& session,
    const scratchbird::engine::internal_api::EngineRequestContext& context);
void ApplyRequestedLanguageProfile(ServerSessionRecord* session,
                                   std::string_view requested_language_tag);
void PopulateEngineLanguageContextFromSession(
    const ServerSessionRecord& session,
    scratchbird::engine::internal_api::EngineLanguageContext* context);

std::vector<std::uint8_t> EncodeAuthHandoffPayloadForTest(const std::string& principal,
                                                          bool credential_valid,
                                                          bool mfa_required = false,
                                                          bool mfa_present = false,
                                                          const std::string& principal_uuid = "",
                                                          const std::string& storage_authority = "");
std::vector<std::uint8_t> EncodeAttachPayloadForTest(
    const std::array<std::uint8_t, 16>& auth_context_uuid,
    const std::string& mode = "read_write");
std::optional<std::array<std::uint8_t, 16>> DecodeAuthContextUuidForTest(
    const std::vector<std::uint8_t>& auth_result_payload);
std::optional<std::array<std::uint8_t, 16>> DecodeSessionUuidForTest(
    const std::vector<std::uint8_t>& attach_result_payload);

SessionOperationResult HandleAuthHandoff(ServerSessionRegistry* registry,
                                         const HostedEngineState& engine_state,
                                         const sbps::Frame& request);
SessionOperationResult HandleAttachDatabase(ServerSessionRegistry* registry,
                                            const HostedEngineState& engine_state,
                                            const sbps::Frame& request);
SessionOperationResult HandleEmbeddedSysarchAttach(ServerSessionRegistry* registry,
                                                   const HostedEngineState& engine_state,
                                                   std::string requested_database,
                                                   std::string application_name = "sb_isql");
// One controlled bridge from the legacy scalar projection into the canonical
// map, followed by strict active/default identity validation.  Callers must
// hold transaction_mutex when the session is shared.
ServerTransactionState* AdoptAndFindExactActiveDefaultTransaction(
    ServerSessionRecord* session);
SessionOperationResult HandleDisconnectNotice(ServerSessionRegistry* registry,
                                              const sbps::Frame& request);
std::string SessionRegistryStatusJson(const ServerSessionRegistry& registry);

ServerRequestRecord RegisterServerRequestLifecycle(ServerSessionRegistry* registry,
                                                   const sbps::Frame& request,
                                                   const ServerSessionRecord& session,
                                                   std::string request_kind,
                                                   std::string operation_id,
                                                   const ServerTransactionState* transaction = nullptr);
void UpdateServerRequestLifecycleOperation(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& request_uuid,
    std::string operation_id);
void LinkServerRequestPreparedStatement(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& request_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid);
ServerSessionObjectHandleRecord AllocateSessionObjectHandle(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    std::string object_uuid,
    std::string object_kind,
    std::string operation_id,
    std::string column_set_hash = {});
ServerSessionObjectHandleValidation ValidateSessionObjectHandle(
    const ServerSessionRegistry& registry,
    const ServerSessionRecord& session,
    std::uint64_t handle_id,
    std::uint64_t generation,
    const std::string& object_uuid,
    const std::string& operation_id,
    const std::string& column_set_hash = {});
void CloseSessionObjectHandlesForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string detail = "session_closed");
// Releases an engine result and removes all buffered row/stream payloads from
// a cursor while preserving only its identity and finality tombstone. Any
// linked request no longer advertises a retained engine result. The caller
// remains responsible for choosing the cursor/request finality state.
bool ReleaseAndClearServerCursorResources(
    ServerSessionRegistry* registry,
    ServerCursorRecord* cursor);
bool ReleaseServerCursorExecutionAuthority(
    ServerSessionRegistry* registry,
    ServerCursorRecord* cursor);
ServerPreparedStatementCloseSummary CloseServerPreparedStatement(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    std::string detail = "prepared_statement_closed");
void CloseServerPublicAbiSessionForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string detail = "session_closed");
ServerPublicAbiSessionContext* EnsureServerPublicAbiSessionForContext(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    std::string* diagnostic_detail);
SessionOperationResult HandleAcquireStatementContext(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const sbps::Frame& request);
SessionOperationResult HandleNegotiateLiteralDescriptors(
    ServerSessionRegistry* registry,
    const sbps::Frame& request);
SessionOperationResult HandleFinalizeLiteralBinding(
    ServerSessionRegistry* registry,
    const sbps::Frame& request);
SessionOperationResult HandleNegotiateParameterDescriptors(
    ServerSessionRegistry* registry,
    const sbps::Frame& request);
SessionOperationResult HandleFinalizeParameterBinding(
    ServerSessionRegistry* registry,
    const sbps::Frame& request);
SessionOperationResult HandleBeginParameterExecutionCoordination(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const sbps::Frame& request);
SessionOperationResult HandleBeginVariableFrame(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const sbps::Frame& request);
SessionOperationResult HandleCloseVariableFrame(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const sbps::Frame& request);
SessionOperationResult HandleNegotiateVariableDescriptors(
    ServerSessionRegistry* registry, const sbps::Frame& request);
SessionOperationResult HandleFinalizeVariableBinding(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const sbps::Frame& request);
SessionOperationResult HandleAssignVariableValues(
    ServerSessionRegistry* registry, const sbps::Frame& request);
SessionOperationResult HandleIssueSourceMapDescriptor(
    ServerSessionRegistry* registry, const sbps::Frame& request);
SessionOperationResult HandleIssueErrorVectorDescriptor(
    ServerSessionRegistry* registry, const sbps::Frame& request);
SessionOperationResult HandleReserveSavepoint(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request);
SessionOperationResult HandleReserveAutonomousFrame(
    ServerSessionRegistry* registry, const HostedEngineState& engine_state,
    const sbps::Frame& request);
SessionOperationResult HandleCoordinateReservationRelease(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateTemporaryInstanceCleanup(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateCursorOpen(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateReadByKey(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateReadRange(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateReadStream(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateResultSetPass(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAccessCursorOpen(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAccessCursorFetch(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAccessCursorClose(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateInsert(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateUpdate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDelete(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateMerge(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateTableTruncate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateTableAnalyze(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateBulkImportStream(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateBulkExportStream(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateStatementBatch(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAtomicCas(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAtomicRmw(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAdvisoryLock(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAdvisoryLockRelease(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateFunctionCall(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateOperatorCall(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateCast(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateCompare(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDomainOperation(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateUdrInvoke(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateProcedureInvoke(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateFunctionInvoke(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAggregateInvoke(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSequenceNextval(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSequenceCurrval(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSequenceSetval(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateQueryNumeric(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAdvancedDatatypeFamily(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateProject(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateCatalogIntrospect(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateKvStructuredRead(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateKvStructuredMutate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateKvStructuredScan(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateKvStructuredStreamRead(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateKvStructuredStreamAppend(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateKvStructuredTimeseries(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSystemConfigSet(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateDomain(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateSchema(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateOrReplaceSrs(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropSrs(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateRewriteRule(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterRewriteRule(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropRewriteRule(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlValidateConstraint(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityCreatePrivilegeTemplate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityCreateUser(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityAlterUser(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityCreateRole(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityCreatePolicy(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityDropPolicy(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityDropPolicy(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityDropRole(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityAlterRole(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityAlterPrivilegeTemplate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityDropPrivilegeTemplate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDatabaseCreateTemplateClone(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateAggregate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterAggregate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropAggregate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlPurgeSystemHistory(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlSetIndexOptimizerEligibility(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlSetTableTypeEnforcement(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDatabaseSerializeLogicalSnapshot(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDatabaseDeserializeLogicalSnapshot(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateMacro(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateDictionary(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropDictionary(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterDictionary(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateContinuousView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterContinuousView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropContinuousView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDmlAsyncInsertSubmit(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDmlAsyncInsertStatus(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDmlCounterAdd(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDmlTimeseriesSchemaWrite(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlTimeseriesSeriesCardinalityPolicy(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateTimeseriesValueCache(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDmlAsyncInsertCancel(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropMacro(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAdminRegisterExternalRelationResolver(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAdminUnregisterExternalRelationResolver(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateTable(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateIndex(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropIndex(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterDomain(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateMaterializedView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlRefreshMaterializedView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropMaterializedView(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateType(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateTableAsQueryWithData(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateTableAsQueryWithNoData(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropTable(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterType(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropType(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateTrigger(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterTrigger(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropTrigger(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateProcedure(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterProcedure(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropProcedure(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateFunction(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterFunction(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropFunction(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreatePackage(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateSynonym(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateForeignTable(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateFdw(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropFdw(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropForeignTable(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropSynonym(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropPackage(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterPackage(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlAlterSequence(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropSequence(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlCreateTemporaryTable(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlDropTemporaryTable(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlRenameObjectVector(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateDdlRenameObject(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateAggregate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateGroup(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityCreateGroupMapping(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityDropGroupMapping(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityGrant(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityRevoke(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityAlterPolicy(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityDropUser(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityAuthenticate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSecurityDeauthenticate(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleSessionRoleSwitch(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleSessionSettingSet(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleSessionSettingReset(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleSessionSettingGet(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateSort(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateLimit(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateWindow(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleCoordinateReturnResultSet(ServerSessionRegistry*,const HostedEngineState&,const sbps::Frame&);
SessionOperationResult HandleFinalizePreparedSblrParameter(
    ServerSessionRegistry* registry,
    const sbps::Frame& request);
std::uint64_t ReleaseServerStatementContextsForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid);
bool ReleaseServerStatementContext(
    ServerSessionRegistry* registry,
    std::string_view statement_uuid);
std::string ServerAuthorityCacheKey(const std::string& cache_kind,
                                    const ServerSessionRecord& session,
                                    const std::string& operation_id,
                                    const std::string& target_object_uuid,
                                    const std::string& statement_shape_hash);
ServerAuthorityCacheRecord StoreServerAuthorityCacheDecision(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    std::string cache_kind,
    std::string operation_id,
    std::string target_object_uuid,
    std::string statement_shape_hash,
    std::string diagnostic_code,
    std::string diagnostic_detail,
    bool refusal);
ServerAuthorityCacheValidation ValidateServerAuthorityCacheEntry(
    const ServerSessionRegistry& registry,
    const ServerSessionRecord& session,
    const std::string& cache_key,
    const std::string& cache_kind,
    const std::string& operation_id,
    const std::string& target_object_uuid,
    const std::string& statement_shape_hash);
bool MarkServerAuthorityCacheHit(ServerSessionRegistry* registry,
                                 const std::string& cache_key);
void LinkServerRequestCursor(ServerSessionRegistry* registry,
                             const std::array<std::uint8_t, 16>& request_uuid,
                             const std::array<std::uint8_t, 16>& cursor_uuid,
                             bool engine_result_retained);
void CompleteServerRequestLifecycle(ServerSessionRegistry* registry,
                                    const std::array<std::uint8_t, 16>& request_uuid,
                                    ServerRequestLifecycleState state,
                                    std::string detail);
ServerRequestLifecycleResult CancelServerRequestLifecycle(
    ServerSessionRegistry* registry,
    const std::string& target_uuid,
    const ServerSessionRecord& actor,
    bool authorization_proven,
    std::uint64_t cancel_timeout_ms = 5000);
void MarkServerRequestTimedOutByCursor(ServerSessionRegistry* registry,
                                       const std::array<std::uint8_t, 16>& cursor_uuid,
                                       std::string detail);
void MarkServerRequestClosedByCursor(ServerSessionRegistry* registry,
                                     const std::array<std::uint8_t, 16>& cursor_uuid,
                                     ServerRequestLifecycleState state,
                                     std::string detail);
std::string ServerRequestLifecycleRecordsJson(const ServerSessionRegistry& registry,
                                              const std::string& target_uuid = {},
                                              bool include_history = true);
std::optional<ServerRequestRecord> FindServerRequestLifecycle(
    const ServerSessionRegistry& registry,
    const std::string& target_uuid);
ServerDriverTransactionDecision ClassifyDriverTransactionEvent(
    const ServerSessionRecord& session,
    const ServerDriverTransactionDecisionInput& input);
std::string ServerDriverTransactionDecisionJson(
    const ServerDriverTransactionDecision& decision);
ServerTransactionPressureControlDecision ClassifyServerTransactionPressureControl(
    const ServerSessionRecord& session,
    const ServerTransactionPressureControlInput& input);
ServerSessionBindingControlResult ApplyServerSessionBindingReport(
    ServerSessionRegistry* registry,
    const ServerSessionBindingReport& report,
    const ServerSessionControlAuthority& authority);
ServerSessionBindingControlResult ClearServerSessionBinding(
    ServerSessionRegistry* registry,
    const ServerSessionTakeoverRequest& target,
    const ServerSessionControlAuthority& authority);
ServerSessionBindingControlResult EvaluateServerSessionTakeoverProbe(
    const ServerSessionRegistry& registry,
    const ServerSessionTakeoverRequest& request,
    const ServerSessionControlAuthority& authority);
ServerSessionBindingControlResult ApplyServerSessionTakeoverRequest(
    ServerSessionRegistry* registry,
    const ServerSessionTakeoverRequest& request,
    const ServerSessionControlAuthority& authority);

}  // namespace scratchbird::server
