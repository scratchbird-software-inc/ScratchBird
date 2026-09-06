// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_schema_runtime.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace scratchbird::engine::internal_api {

using SblrDdlCreateSchemaJournalUuidV1 = std::array<std::uint8_t, 16>;
using SblrDdlCreateSchemaJournalHashV1 = std::array<std::uint8_t, 32>;

enum class SblrDdlCreateSchemaJournalStateV1 : std::uint32_t {
  begun = 1,
  published = 2,
};

// The recovery UUID owns the durable filename.  The complete canonical CSDO
// is retained as the exact authority key so a retry cannot silently change a
// receipt, transaction, catalog/security/resource generation, schema path, or
// executor-evidence generation.
struct SblrDdlCreateSchemaJournalKeyV1 {
  SblrDdlCreateSchemaJournalUuidV1 database_uuid{};
  SblrDdlCreateSchemaJournalUuidV1 recovery_uuid{};
  std::vector<std::uint8_t> canonical_descriptor_bytes;
};

struct SblrDdlCreateSchemaJournalSnapshotV1 {
  SblrDdlCreateSchemaJournalKeyV1 key;
  SblrDdlCreateSchemaJournalStateV1 state =
      SblrDdlCreateSchemaJournalStateV1::begun;
  std::uint64_t journal_generation = 0;
  SblrDdlCreateSchemaJournalUuidV1 catalog_row_uuid{};
  SblrDdlCreateSchemaJournalUuidV1 mutation_uuid{};
  SblrDdlCreateSchemaJournalUuidV1 publication_barrier_uuid{};
  // The exact CSRS is planned and durably recorded with the begun intent, but
  // it is externally publishable only when state == published.
  std::vector<std::uint8_t> canonical_result_bytes;
  SblrDdlCreateSchemaJournalHashV1 canonical_descriptor_sha256{};
  SblrDdlCreateSchemaJournalHashV1 canonical_result_sha256{};
  SblrDdlCreateSchemaJournalHashV1 record_evidence_sha256{};
};

struct SblrDdlCreateSchemaJournalResultV1 {
  bool ok = false;
  bool found = false;
  bool mutation_invoked = false;
  bool replayed_published_result = false;
  EngineApiDiagnostic diagnostic;
  SblrDdlCreateSchemaJournalSnapshotV1 snapshot;
};

// Reopens and validates the exact durable record.  No process-local result
// cache participates in recovery.
SblrDdlCreateSchemaJournalResultV1
LookupSblrDdlCreateSchemaExecutionJournalV1(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key);

// Durably records a begun intent and its exact planned CSRS before any catalog
// mutation.  Existing exact state is returned byte-for-byte.
SblrDdlCreateSchemaJournalResultV1
EnsureSblrDdlCreateSchemaExecutionJournalV1(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key);

// Serializes exact recovery for one database/recovery identity.  The callback
// receives the already-durable planned CSRS.  It must perform or repair the
// idempotent catalog mutation and return success only after its statement
// publication is durable.  The journal then atomically publishes the same
// byte-identical CSRS.  A published retry never invokes the callback.
using SblrDdlCreateSchemaMutationV1 = std::function<EngineApiDiagnostic(
    const scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1&)>;

SblrDdlCreateSchemaJournalResultV1
ExecuteSblrDdlCreateSchemaExecutionJournalV1(
    const EngineRequestContext& context,
    const SblrDdlCreateSchemaJournalKeyV1& key,
    const SblrDdlCreateSchemaMutationV1& mutation);

}  // namespace scratchbird::engine::internal_api
