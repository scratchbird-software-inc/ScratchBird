// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

using SblrDatabaseAttachJournalUuidV1 = std::array<std::uint8_t, 16>;
using SblrDatabaseAttachJournalHashV1 = std::array<std::uint8_t, 32>;

enum class SblrDatabaseAttachJournalStateV1 : std::uint32_t {
  begun = 1,
  published = 2,
};

// Database-owned durable identity for a session-scoped alias. The filename is
// selected only from the authenticated session UUID and canonical alias hash;
// every remaining authority field is retained in the record and compared on
// replay. A second descriptor for the same session/alias is therefore a
// deterministic alias conflict rather than a second attachment.
struct SblrDatabaseAttachJournalKeyV1 {
  SblrDatabaseAttachJournalUuidV1 database_uuid{};
  SblrDatabaseAttachJournalUuidV1 session_uuid{};
  SblrDatabaseAttachJournalUuidV1 statement_receipt_uuid{};
  SblrDatabaseAttachJournalUuidV1 attach_uuid{};
  SblrDatabaseAttachJournalUuidV1 storage_uuid{};
  SblrDatabaseAttachJournalUuidV1 alias_uuid{};
  SblrDatabaseAttachJournalHashV1 alias_name_sha256{};
  SblrDatabaseAttachJournalHashV1 descriptor_sha256{};
  SblrDatabaseAttachJournalHashV1 storage_alias_binding_sha256{};
  SblrDatabaseAttachJournalUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  SblrDatabaseAttachJournalUuidV1 security_context_uuid{};
  std::uint64_t security_epoch = 0;
  SblrDatabaseAttachJournalUuidV1 policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  SblrDatabaseAttachJournalUuidV1 transaction_uuid{};
  std::uint64_t transaction_generation = 0;
  std::uint8_t mode = 0;
  std::uint8_t alias_scope = 0;
  SblrDatabaseAttachJournalUuidV1 resource_admission_uuid{};
  std::uint64_t resource_epoch = 0;
  std::uint64_t executor_availability_generation = 0;
};

struct SblrDatabaseAttachJournalSnapshotV1 {
  SblrDatabaseAttachJournalKeyV1 key;
  SblrDatabaseAttachJournalStateV1 state =
      SblrDatabaseAttachJournalStateV1::begun;
  std::uint64_t journal_generation = 0;
  std::vector<std::uint8_t> canonical_result_bytes;
  SblrDatabaseAttachJournalHashV1 canonical_result_sha256{};
  SblrDatabaseAttachJournalHashV1 record_evidence_sha256{};
};

struct SblrDatabaseAttachJournalResultV1 {
  bool ok = false;
  bool found = false;
  EngineApiDiagnostic diagnostic;
  SblrDatabaseAttachJournalSnapshotV1 snapshot;
};

SblrDatabaseAttachJournalHashV1 SblrDatabaseAttachAliasNameSha256V1(
    std::string_view raw_name, bool quoted);

SblrDatabaseAttachJournalResultV1 LookupSblrDatabaseAttachJournalV1(
    const EngineRequestContext& context,
    const SblrDatabaseAttachJournalKeyV1& key);

SblrDatabaseAttachJournalResultV1 EnsureSblrDatabaseAttachJournalV1(
    const EngineRequestContext& context,
    const SblrDatabaseAttachJournalKeyV1& key);

SblrDatabaseAttachJournalResultV1 PublishSblrDatabaseAttachJournalResultV1(
    const EngineRequestContext& context,
    const SblrDatabaseAttachJournalKeyV1& key,
    const std::vector<std::uint8_t>& canonical_result_bytes);

}  // namespace scratchbird::engine::internal_api
