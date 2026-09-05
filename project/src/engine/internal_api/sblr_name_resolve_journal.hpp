// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace scratchbird::engine::internal_api {

using SblrNameResolveJournalUuidV1 = std::array<std::uint8_t, 16>;
using SblrNameResolveJournalHashV1 = std::array<std::uint8_t, 32>;

enum class SblrNameResolveJournalStateV1 : std::uint32_t {
  begun = 1,
  published = 2,
};

// Exact recovery key for one engine-issued NAME_RESOLVE descriptor. The
// transient SBPS request UUID and package-reservation identity are
// intentionally absent: an exact retry may receive fresh transport authority.
struct SblrNameResolveJournalKeyV1 {
  SblrNameResolveJournalUuidV1 database_uuid{};
  SblrNameResolveJournalUuidV1 statement_receipt_uuid{};
  SblrNameResolveJournalUuidV1 resolution_uuid{};
  SblrNameResolveJournalHashV1 descriptor_sha256{};
  SblrNameResolveJournalHashV1 canonical_name_sha256{};
  std::uint64_t namespace_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrNameResolveJournalSnapshotV1 {
  SblrNameResolveJournalKeyV1 key;
  SblrNameResolveJournalStateV1 state =
      SblrNameResolveJournalStateV1::begun;
  std::uint64_t journal_generation = 0;
  std::vector<std::uint8_t> canonical_result_bytes;
  SblrNameResolveJournalHashV1 canonical_result_sha256{};
  SblrNameResolveJournalHashV1 record_evidence_sha256{};
};

struct SblrNameResolveJournalResultV1 {
  bool ok = false;
  bool found = false;
  EngineApiDiagnostic diagnostic;
  SblrNameResolveJournalSnapshotV1 snapshot;
};

// Ensures that the exact resolution identity is durably journaled before the
// catalog lookup. Existing exact state is returned byte-for-byte; a key
// collision is fail-closed.
SblrNameResolveJournalResultV1 EnsureSblrNameResolveJournalV1(
    const EngineRequestContext& context,
    const SblrNameResolveJournalKeyV1& key);

// Reopens and validates the durable record on every call. This intentionally
// has no process-local truth cache, so recovery behavior is identical after an
// engine restart.
SblrNameResolveJournalResultV1 LookupSblrNameResolveJournalV1(
    const EngineRequestContext& context,
    const SblrNameResolveJournalKeyV1& key);

// Atomically transitions begun -> published with the exact canonical SNRR.
// Exact replay returns the recorded bytes and never republishes an identity.
SblrNameResolveJournalResultV1 PublishSblrNameResolveJournalResultV1(
    const EngineRequestContext& context,
    const SblrNameResolveJournalKeyV1& key,
    const std::vector<std::uint8_t>& canonical_result_bytes);

}  // namespace scratchbird::engine::internal_api
