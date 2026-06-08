// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/index_apply_locality_bridge.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;

bool IndexIsUnique(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(),
                   index.key_envelopes.end(),
                   "unique") != index.key_envelopes.end();
}

std::string NormalizedFamily(const CrudIndexRecord& index) {
  if (!index.family.empty()) {
    return index.family;
  }
  return CrudIndexFamilyForProfile(index.profile);
}

std::string NormalizedProfile(const CrudIndexRecord& index) {
  return NormalizeCrudIndexProfile(index.profile);
}

EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK",
                                 "engine.api.ok",
                                 {},
                                 false);
}

MgaIndexEntryAppendBatch EmptyLike(const MgaIndexEntryAppendBatch& source) {
  MgaIndexEntryAppendBatch batch;
  batch.index = source.index;
  batch.table_uuid = source.table_uuid;
  return batch;
}

}  // namespace

LocalityAwareIndexApplyBatchPlan PlanLocalityAwareIndexApplyBatches(
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  LocalityAwareIndexApplyBatchPlan result;
  result.diagnostic = Ok();
  std::vector<idx::CommitGroupLocalityIndexApplyItem> items;
  for (std::size_t batch_ordinal = 0; batch_ordinal < batches.size();
       ++batch_ordinal) {
    const auto& batch = batches[batch_ordinal];
    const bool unique = IndexIsUnique(batch.index);
    for (std::size_t row_ordinal = 0; row_ordinal < batch.rows.size();
         ++row_ordinal) {
      const auto keys = CrudIndexKeysForValues(batch.index,
                                               batch.rows[row_ordinal].values);
      if (keys.empty()) {
        continue;
      }
      idx::CommitGroupLocalityIndexApplyItem item;
      item.source_batch_ordinal = batch_ordinal;
      item.source_row_ordinal = row_ordinal;
      item.index_uuid = batch.index.index_uuid;
      item.family = NormalizedFamily(batch.index);
      item.profile = NormalizedProfile(batch.index);
      item.unique = unique;
      item.target_keys = keys;
      items.push_back(std::move(item));
    }
  }

  result.core_plan = idx::PlanCommitGroupLocalityIndexApply(items);
  if (!result.core_plan.accepted) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.index_apply_locality",
        result.core_plan.refusal_reason.empty()
            ? "index_apply_locality_plan_refused"
            : result.core_plan.refusal_reason);
    return result;
  }

  for (const auto& group : result.core_plan.groups) {
    std::optional<std::size_t> current_batch_ordinal;
    MgaIndexEntryAppendBatch current;
    bool current_active = false;
    auto flush_current = [&]() {
      if (current_active && !current.rows.empty()) {
        result.batches.push_back(std::move(current));
      }
      current = {};
      current_active = false;
      current_batch_ordinal.reset();
    };

    for (const auto item_ordinal : group.item_ordinals) {
      const auto& item = items[item_ordinal];
      if (!current_batch_ordinal.has_value() ||
          *current_batch_ordinal != item.source_batch_ordinal) {
        flush_current();
        current_batch_ordinal = item.source_batch_ordinal;
        current = EmptyLike(batches[item.source_batch_ordinal]);
        current_active = true;
      }
      current.rows.push_back(
          batches[item.source_batch_ordinal].rows[item.source_row_ordinal]);
    }
    flush_current();
  }
  return result;
}

void AddLocalityAwareIndexApplyEvidence(
    const LocalityAwareIndexApplyBatchPlan& plan,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr || !plan.core_plan.accepted) {
    return;
  }
  evidence->push_back({"index_apply_planner",
                       "commit_group_locality_aware_v1"});
  evidence->push_back({"index_apply_grouping_before_append",
                       plan.core_plan.planned_before_append ? "true" : "false"});
  evidence->push_back({"index_apply_grouped_family_count",
                       std::to_string(plan.core_plan.grouped_family_count)});
  evidence->push_back({"index_apply_locality_group_count",
                       std::to_string(plan.core_plan.locality_group_count)});
  evidence->push_back({"index_apply_pending_item_count",
                       std::to_string(plan.core_plan.pending_item_count)});
  evidence->push_back({"index_apply_output_batch_count",
                       std::to_string(plan.batches.size())});
  evidence->push_back({"index_apply_unique_order_preserved",
                       plan.core_plan.unique_order_preserved ? "true" : "false"});
  evidence->push_back({"mga_finality_authority",
                       "engine_transaction_inventory"});
  for (const auto& family : plan.core_plan.family_profile_keys) {
    evidence->push_back({"index_apply_family_profile_key", family});
  }
  for (const auto& locality :
       plan.core_plan.target_leaf_page_locality_keys) {
    evidence->push_back({"index_apply_target_leaf_page_locality_key",
                         locality});
  }
}

}  // namespace scratchbird::engine::internal_api
