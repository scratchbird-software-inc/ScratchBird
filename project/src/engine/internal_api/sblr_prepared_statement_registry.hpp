// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

using SblrPreparedStatementRegistryUuidV1 =
    std::array<std::uint8_t, 16>;
using SblrPreparedStatementRegistryHashV1 =
    std::array<std::uint8_t, 32>;

enum class SblrPreparedStatementRegistryStateV1 : std::uint32_t {
  active = 1,
  freed = 2,
  session_revoked = 3,
};

// Private engine-owned form of one prepared statement. Public statement names
// remain session aliases; UUIDs, generations, descriptors, and byte-exact
// results remain the authority used after process recovery.
struct SblrPreparedStatementRegistryRecordV1 {
  std::string canonical_name;
  bool quoted = false;
  std::string body_operation_id;
  std::string body_operation_family;
  std::string body_result_shape;
  bool source_free_parameterless_query_template = false;
  bool source_free_parameterized_query_template = false;
  std::string parameter_set_uuid;
  std::string parameter_prepared_statement_uuid;
  std::uint64_t parameter_set_generation = 0;
  std::string parameter_set_snapshot_uuid;
  std::uint64_t parameter_set_snapshot_generation = 0;
  std::string ordered_slot_table_sha256;
  SblrPreparedStatementRegistryUuidV1 statement_uuid{};
  SblrPreparedStatementRegistryUuidV1 statement_name_uuid{};
  SblrPreparedStatementRegistryUuidV1 preparing_receipt_uuid{};
  std::uint64_t prepared_generation = 0;
  SblrPreparedStatementRegistryHashV1 descriptor_sha256{};
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_prepare_result_bytes;
  SblrPreparedStatementRegistryStateV1 state =
      SblrPreparedStatementRegistryStateV1::active;
  SblrPreparedStatementRegistryHashV1 free_descriptor_sha256{};
  std::vector<std::uint8_t> canonical_free_result_bytes;
  SblrPreparedStatementRegistryUuidV1 last_execution_uuid{};
  SblrPreparedStatementRegistryUuidV1 last_execution_receipt_uuid{};
  SblrPreparedStatementRegistryUuidV1 last_execution_transaction_uuid{};
  std::uint64_t last_execution_generation = 0;
  bool last_execution_terminal = false;
  bool last_execution_final = false;
  std::uint64_t record_generation = 0;
  SblrPreparedStatementRegistryHashV1 record_evidence_sha256{};
};

struct SblrPreparedStatementRegistrySnapshotV1 {
  SblrPreparedStatementRegistryUuidV1 database_uuid{};
  SblrPreparedStatementRegistryUuidV1 session_uuid{};
  SblrPreparedStatementRegistryUuidV1 principal_uuid{};
  std::uint64_t registry_generation = 0;
  bool session_revoked = false;
  std::vector<SblrPreparedStatementRegistryRecordV1> records;
  SblrPreparedStatementRegistryHashV1 record_evidence_sha256{};
};

struct SblrPreparedStatementRegistryResultV1 {
  bool ok = false;
  bool found = false;
  bool exact_replay = false;
  EngineApiDiagnostic diagnostic;
  SblrPreparedStatementRegistrySnapshotV1 snapshot;
  SblrPreparedStatementRegistryRecordV1 record;
};

// Loads and validates the exact session registry directly from durable state.
// There is deliberately no process-local truth cache.
SblrPreparedStatementRegistryResultV1 LoadSblrPreparedStatementRegistryV1(
    const EngineRequestContext& context);

// Resolves the exact active durable prepared capability used by private
// parameter coordination. A freed or explicitly terminated identity is
// non-disclosing and cannot be recovered from an older coordination journal.
SblrPreparedStatementRegistryResultV1
ResolveActiveSblrPreparedStatementCapabilityV1(
    const EngineRequestContext& context,
    const std::string& prepared_statement_uuid,
    std::uint64_t prepared_generation);

// Durably publishes a prepared object before its public result is returned.
// Exact replay returns the original record; a name/identity collision refuses.
SblrPreparedStatementRegistryResultV1 PublishSblrPreparedStatementV1(
    const EngineRequestContext& context,
    const SblrPreparedStatementRegistryRecordV1& record);

// Durably records revocation and the byte-exact SBLR_STMT_FREE result before
// the in-memory reference is marked freed.
SblrPreparedStatementRegistryResultV1 FreeSblrPreparedStatementV1(
    const EngineRequestContext& context,
    const std::string& canonical_name,
    const SblrPreparedStatementRegistryUuidV1& statement_uuid,
    std::uint64_t prepared_generation,
    const SblrPreparedStatementRegistryHashV1& prepared_descriptor_sha256,
    const SblrPreparedStatementRegistryHashV1& free_descriptor_sha256,
    const std::vector<std::uint8_t>& canonical_free_result_bytes);

// Explicit logical-session termination is a durable terminal barrier. It is
// distinct from a process crash, which leaves the session snapshot recoverable.
SblrPreparedStatementRegistryResultV1 RevokeSblrPreparedStatementSessionV1(
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
