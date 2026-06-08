// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ODF-052 bottom-up exact-index cleanup before leaf split selection.

#include "index_family_registry.hpp"
#include "secondary_index_garbage_cleanup.hpp"
#include "page_finality_evidence.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u64;

enum class ExactIndexLeafPressureAction : u32 {
  insert_without_split,
  cleanup_avoided_split,
  split_selected
};

struct ExactIndexLeafCleanupEvidenceField {
  std::string name;
  std::string value;
};

struct ExactIndexLeafPressureRequest {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  IndexFamily family = IndexFamily::btree;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  u64 current_leaf_entry_count = 0;
  u64 pending_insert_entry_count = 1;
  u64 leaf_entry_capacity = 128;
  u64 max_cleanup_steps = 0;
  scratchbird::transaction::mga::ExactIndexCleanupAuthorityDecision
      mga_cleanup_authority;
  SecondaryIndexGarbageCleanupRequest cleanup;
};

struct ExactIndexLeafPressureDecisionCounters {
  u64 cleanup_attempted = 0;
  u64 cleanup_accepted = 0;
  u64 cleanup_refused = 0;
  u64 budget_exhausted = 0;
  u64 cleaned_count = 0;
  u64 retained_count = 0;
  u64 split_avoided = 0;
  u64 split_selected = 0;
};

struct ExactIndexLeafPressureDecision {
  Status status;
  ExactIndexLeafPressureAction action =
      ExactIndexLeafPressureAction::split_selected;
  ExactIndexLeafPressureDecisionCounters counters;
  bool leaf_pressure_detected = false;
  bool cleanup_attempted = false;
  bool cleanup_accepted = false;
  bool cleanup_refused = false;
  bool budget_exhausted = false;
  bool split_avoided = false;
  bool split_selected = true;
  bool unique_exact_recheck_required = false;
  u64 required_reclaim_count = 0;
  u64 projected_leaf_entry_count = 0;
  std::string mga_authority_source = "none";
  std::string fail_open_reason;
  SecondaryIndexGarbageCleanupResult cleanup_result;
  PersistentSecondaryIndexDeltaLedger selected_ledger;
  std::vector<ExactIndexLeafCleanupEvidenceField> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* ExactIndexLeafPressureActionName(
    ExactIndexLeafPressureAction action);

ExactIndexLeafPressureDecision PlanExactIndexLeafPressureAction(
    const ExactIndexLeafPressureRequest& request);

DiagnosticRecord MakeExactIndexLeafCleanupDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
