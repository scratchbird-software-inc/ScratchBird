// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "row_data_page.hpp"
#include "transaction_cleanup.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

struct RowDataPhysicalSweepRequest {
  RowDataPageBody page;
  scratchbird::transaction::mga::LocalGarbageCollectionSweepResult sweep;
  u32 page_size = 0;
  bool engine_mga_authoritative = false;
  u64 max_reclaim_rows = 0;
};

struct RowDataPhysicalSweepResult {
  Status status;
  RowDataPageBody page;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;
  u64 scanned_row_count = 0;
  u64 removed_row_count = 0;
  u64 retained_row_count = 0;
  u64 reclaimed_slot_count = 0;
  u32 free_space_before = 0;
  u32 free_space_after = 0;
  bool physical_storage_mutated = false;
  std::vector<std::string> reclaim_evidence_ids;

  bool ok() const {
    return status.ok();
  }
};

RowDataPhysicalSweepResult ApplyRowDataPhysicalSweep(
    const RowDataPhysicalSweepRequest& request);

}  // namespace scratchbird::storage::page
