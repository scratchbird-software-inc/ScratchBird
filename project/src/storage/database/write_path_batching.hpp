// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::TypedUuid;

// SEARCH_KEY: ORH_DURABILITY_WRITE_PATH_BATCHING
// Write batching metadata is advisory durability evidence only. Durable MGA
// transaction inventory remains the authority for finality, visibility, row
// identity, authorization, and recovery decisions.
struct WritePathBatchingAuthorityContext {
  bool engine_mga_tip_authoritative = false;
  bool durable_transaction_inventory_proven = false;
  bool parser_client_or_donor_write_batch_authority = false;
  bool batch_metadata_finality_or_visibility_authority = false;
  bool batch_metadata_recovery_authority = false;
  bool recovery_from_batch_metadata_alone = false;
};

struct WritePathBatchingRequest {
  std::string route_label;
  std::filesystem::path scratch_directory;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid transaction_uuid;
  std::uint64_t local_transaction_id = 1;
  std::uint64_t batching_generation = 1;
  std::uint64_t expected_batching_generation = 1;
  std::uint64_t dirty_page_count = 0;
  std::uint64_t page_generation = 1;
  std::uint64_t extent_page_count = 0;
  WritePathBatchingAuthorityContext authority;
  bool runtime_enabled = true;
  bool dirty_page_accounting_available = true;
  bool extent_allocation_matches = true;
  bool fsync_open_proof_available = true;
  bool crash_reopen_recovery_proof_available = true;
  bool exact_fallback_available = true;
  bool resource_pressure = false;
  std::string expected_state_hash;
};

struct WritePathBatchingResult {
  bool ok = false;
  bool benchmark_clean = false;
  bool fallback_used = false;
  bool fail_closed = false;
  bool runtime_consumed = false;
  bool dirty_page_accounting_proven = false;
  bool extent_allocation_proven = false;
  bool fsync_open_proven = false;
  bool crash_reopen_recovery_proven = false;
  std::uint64_t unbatched_flush_operations = 0;
  std::uint64_t batched_flush_operations = 0;
  std::uint64_t flushed_pages = 0;
  std::string diagnostic_code;
  std::string fallback_reason;
  std::string state_hash;
  std::vector<std::string> evidence;
};

WritePathBatchingResult ExecuteDurabilityWritePathBatch(
    const WritePathBatchingRequest& request);

}  // namespace scratchbird::storage::database
