// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_optimizer_stats_drop_runtime.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace scratchbird::engine::internal_api {

struct SblrOptimizerStatsEpochSnapshotV1 {
  bool ok = false;
  std::uint64_t statistics_epoch = 1;
  std::uint64_t journal_generation = 0;
  std::array<std::uint8_t, 32> journal_chain_sha256{};
  std::uint64_t unresolved_other_transaction_count = 0;
  EngineApiDiagnostic diagnostic;
};

struct SblrOptimizerStatsDropPublicationV1 {
  bool ok = false;
  bool exact_replay = false;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t journal_generation = 0;
  std::vector<std::uint8_t> canonical_result_bytes;
  EngineApiDiagnostic diagnostic;
};

// Reads and validates the complete durable statistics epoch chain. Visibility
// is MGA-owned: committed/archived records and the caller's own active record
// participate; rolled-back records remain history but do not advance epoch.
SblrOptimizerStatsEpochSnapshotV1 InspectSblrOptimizerStatsEpochV1(
    const EngineRequestContext& context);

// Appends exactly one transaction-owned epoch advance after matching the
// descriptor's journal/epoch fence. The durable record precedes derived cache
// invalidation. Exact effect replay returns the recorded result byte-for-byte.
SblrOptimizerStatsDropPublicationV1 PublishSblrOptimizerStatsDropV1(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrOptimizerStatsDropDescriptorV1&
        descriptor);

}  // namespace scratchbird::engine::internal_api
