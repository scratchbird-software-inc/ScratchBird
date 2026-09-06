// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_LARGE_VALUE_STORE_INTERFACE
// Large-value payloads are companion storage. Transaction-inventory-derived
// visibility supplied by the relation store decides whether reclaim markers
// are visible; locator presence never decides transaction finality.
struct MgaLargeValueReclaimLoadResult {
  EngineApiDiagnostic diagnostic;
  std::set<std::string> overflow_uuids;
};

struct MgaTemporaryLargeValueRecoveryResult {
  EngineApiDiagnostic diagnostic;
  std::uint64_t reclaimed_large_value_count = 0;
  std::uint64_t orphaned_large_value_count = 0;
  std::uint64_t rolled_back_event_count = 0;
  std::uint64_t active_or_unresolved_event_count = 0;
  std::uint64_t fenced_event_count = 0;
};

MgaLargeValueReclaimLoadResult LoadVisibleMgaLargeValueReclaims(
    const EngineRequestContext& context);
MgaTemporaryLargeValueRecoveryResult ClassifyMgaTemporaryLargeValueRecovery(
    const EngineRequestContext& context,
    const std::set<std::string>& temporary_tables,
    const std::map<std::uint64_t, std::string>& transaction_states);
bool RowsContainLargeValueLocators(
    const std::vector<CrudRowVersionRecord>& rows);
EngineApiDiagnostic ExpandMgaLargeValueLocators(
    const EngineRequestContext& context,
    std::vector<CrudRowVersionRecord>* rows);
EngineApiDiagnostic AppendMgaLargeValueReclaimMarkersForRowVersion(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    const CrudRowVersionRecord& row,
    const std::string& cleanup_reason,
    std::set<std::string>* already_reclaimed_overflow_uuids,
    std::uint64_t* reclaimed_count);

}  // namespace scratchbird::engine::internal_api
