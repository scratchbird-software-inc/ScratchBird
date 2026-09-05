// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace scratchbird::engine::internal_api {

using SblrCatalogEpochCheckJournalUuidV1 = std::array<std::uint8_t, 16>;
using SblrCatalogEpochCheckJournalHashV1 = std::array<std::uint8_t, 32>;

enum class SblrCatalogEpochCheckJournalStateV1 : std::uint32_t {
  begun = 1,
  published = 2,
};

// Exact durable recovery key for one engine-issued SECD. Transport request
// UUIDs and package reservations are deliberately excluded: an exact retry
// may use fresh transport authority, but it must present the same immutable
// statement, visibility, epoch, and executor cohort.
struct SblrCatalogEpochCheckJournalKeyV1 {
  SblrCatalogEpochCheckJournalUuidV1 database_uuid{};
  SblrCatalogEpochCheckJournalUuidV1 statement_receipt_uuid{};
  SblrCatalogEpochCheckJournalUuidV1 check_uuid{};
  SblrCatalogEpochCheckJournalHashV1 descriptor_sha256{};
  SblrCatalogEpochCheckJournalHashV1 visibility_scope_sha256{};
  SblrCatalogEpochCheckJournalUuidV1 requested_catalog_epoch_uuid{};
  std::uint64_t requested_catalog_generation = 0;
  std::uint64_t schema_tree_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t executor_availability_generation = 0;
};

struct SblrCatalogEpochCheckJournalSnapshotV1 {
  SblrCatalogEpochCheckJournalKeyV1 key;
  SblrCatalogEpochCheckJournalStateV1 state =
      SblrCatalogEpochCheckJournalStateV1::begun;
  std::uint64_t journal_generation = 0;
  std::vector<std::uint8_t> canonical_result_bytes;
  SblrCatalogEpochCheckJournalHashV1 canonical_result_sha256{};
  SblrCatalogEpochCheckJournalHashV1 record_evidence_sha256{};
};

struct SblrCatalogEpochCheckJournalResultV1 {
  bool ok = false;
  bool found = false;
  EngineApiDiagnostic diagnostic;
  SblrCatalogEpochCheckJournalSnapshotV1 snapshot;
};

SblrCatalogEpochCheckJournalResultV1 LookupSblrCatalogEpochCheckJournalV1(
    const EngineRequestContext& context,
    const SblrCatalogEpochCheckJournalKeyV1& key);

SblrCatalogEpochCheckJournalResultV1 EnsureSblrCatalogEpochCheckJournalV1(
    const EngineRequestContext& context,
    const SblrCatalogEpochCheckJournalKeyV1& key);

SblrCatalogEpochCheckJournalResultV1
PublishSblrCatalogEpochCheckJournalResultV1(
    const EngineRequestContext& context,
    const SblrCatalogEpochCheckJournalKeyV1& key,
    const std::vector<std::uint8_t>& canonical_result_bytes);

}  // namespace scratchbird::engine::internal_api
