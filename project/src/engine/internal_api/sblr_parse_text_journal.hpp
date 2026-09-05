// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace scratchbird::engine::internal_api {

using SblrParseTextJournalUuidV1 = std::array<std::uint8_t, 16>;
using SblrParseTextJournalHashV1 = std::array<std::uint8_t, 32>;

enum class SblrParseTextJournalStateV1 : std::uint32_t {
  begun = 1,
  published = 2,
};

// Exact durable recovery key for one engine-issued SPTD. Transport request
// UUIDs and package-reservation handles are deliberately excluded: a retry
// may have fresh transport authority but must present the same descriptor and
// immutable statement/profile generations.
struct SblrParseTextJournalKeyV1 {
  SblrParseTextJournalUuidV1 database_uuid{};
  SblrParseTextJournalUuidV1 statement_receipt_uuid{};
  SblrParseTextJournalUuidV1 parse_uuid{};
  SblrParseTextJournalHashV1 descriptor_sha256{};
  SblrParseTextJournalHashV1 canonical_input_sha256{};
  SblrParseTextJournalUuidV1 language_profile_uuid{};
  std::uint64_t language_profile_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t executor_availability_generation = 0;
};

struct SblrParseTextJournalSnapshotV1 {
  SblrParseTextJournalKeyV1 key;
  SblrParseTextJournalStateV1 state =
      SblrParseTextJournalStateV1::begun;
  std::uint64_t journal_generation = 0;
  std::vector<std::uint8_t> canonical_result_bytes;
  SblrParseTextJournalHashV1 canonical_result_sha256{};
  SblrParseTextJournalHashV1 record_evidence_sha256{};
};

struct SblrParseTextJournalResultV1 {
  bool ok = false;
  bool found = false;
  EngineApiDiagnostic diagnostic;
  SblrParseTextJournalSnapshotV1 snapshot;
};

SblrParseTextJournalResultV1 LookupSblrParseTextJournalV1(
    const EngineRequestContext& context,
    const SblrParseTextJournalKeyV1& key);

SblrParseTextJournalResultV1 EnsureSblrParseTextJournalV1(
    const EngineRequestContext& context,
    const SblrParseTextJournalKeyV1& key);

SblrParseTextJournalResultV1 PublishSblrParseTextJournalResultV1(
    const EngineRequestContext& context,
    const SblrParseTextJournalKeyV1& key,
    const std::vector<std::uint8_t>& canonical_result_bytes);

}  // namespace scratchbird::engine::internal_api
