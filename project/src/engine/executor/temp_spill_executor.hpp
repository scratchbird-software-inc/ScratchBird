// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

// SEARCH_KEY: ORH_SORT_TOPN_DISTINCT_TEMP_SPILL
// Bounded temp-spill execution consumes physical temporary-work artifacts as
// advisory runtime storage only. Temp metadata is never row identity,
// visibility, authorization, transaction-finality, or recovery authority.
enum class TempSpillRouteKind {
  kSort,
  kTopN,
  kDistinct,
  kGroupBy,
  kHashAggregate,
};

struct TempSpillInputRow {
  std::string key;
  std::int64_t value = 0;
  std::uint64_t row_ordinal = 0;
};

struct TempSpillAuthorityContext {
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_recheck_required = false;
  bool security_context_bound = false;
  bool exact_recheck_required = false;
  bool parser_client_or_donor_spill_authority = false;
  bool temp_metadata_visibility_or_finality_authority = false;
  bool temp_metadata_recovery_authority = false;
};

struct TempSpillRequest {
  TempSpillRouteKind route_kind = TempSpillRouteKind::kSort;
  std::string route_label;
  std::filesystem::path spill_directory;
  std::uint64_t runtime_generation = 1;
  std::uint64_t memory_quota_bytes = 0;
  std::vector<TempSpillInputRow> rows;
  std::size_t top_n = 0;
  TempSpillAuthorityContext authority;
  bool runtime_enabled = true;
  bool spill_allowed = true;
  bool temp_accounting_available = true;
  bool cleanup_required = true;
  bool cleanup_after_cancellation = true;
  bool cancellation_requested = false;
  bool restart_recovery_proof_available = true;
  bool exact_fallback_available = true;
  std::string expected_result_hash;
  std::uint64_t reopen_runtime_generation = 0;
  bool benchmark_or_donor_dominance_claim = false;
};

struct TempSpillResult {
  bool ok = false;
  bool benchmark_clean = false;
  bool fallback_used = false;
  bool fail_closed = false;
  bool runtime_consumed = false;
  bool spilled = false;
  bool cleanup_proven = false;
  bool reopen_recovery_proven = false;
  std::string diagnostic_code;
  std::string fallback_reason;
  std::string result_hash;
  std::vector<std::string> output_rows;
  std::vector<std::string> evidence;
};

TempSpillResult ExecuteBoundedTempSpillRoute(const TempSpillRequest& request);
const char* TempSpillRouteKindName(TempSpillRouteKind kind);

}  // namespace scratchbird::engine::executor
