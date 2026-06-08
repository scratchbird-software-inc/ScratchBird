// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-ENGINE-REPAIR-HISTORY-INSPECT-ANCHOR
#include "api_types.hpp"
#include "repair_history_inspection.hpp"

#include <string>

namespace scratchbird::engine::internal_api {

struct EngineInspectRepairHistoryRequest : EngineApiRequest {
  scratchbird::storage::database::RepairHistoryInspectionRequest inspection;
  bool load_repair_ledger_from_path = false;
  std::string repair_ledger_path;
};

struct EngineInspectRepairHistoryResult : EngineApiResult {
  bool repair_history_ready = false;
  bool durable_mga_inventory_authority = false;
  bool repair_evidence_is_transaction_authority = false;
  bool data_loss_possible = false;
  bool restore_required = false;
  bool quarantine_present = false;
  EngineApiU64 ordinary_version_count = 0;
  EngineApiU64 archive_entry_count = 0;
  EngineApiU64 repair_event_count = 0;
  EngineApiU64 salvage_evidence_count = 0;
  EngineApiU64 diagnostic_count = 0;
};

EngineInspectRepairHistoryResult EngineInspectRepairHistory(
    const EngineInspectRepairHistoryRequest& request);

}  // namespace scratchbird::engine::internal_api
