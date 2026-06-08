// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_page_finality_map.hpp"

namespace scratchbird::storage::page {
namespace {

namespace mga = scratchbird::transaction::mga;

void MergeCounters(PageFinalityDecisionCounters* target,
                   const PageFinalityDecisionCounters& source) {
  target->evidence_examined += source.evidence_examined;
  target->accepted += source.accepted;
  target->refused += source.refused;
  target->stale_refusals += source.stale_refusals;
  target->epoch_refusals += source.epoch_refusals;
  target->horizon_refusals += source.horizon_refusals;
  target->provenance_refusals += source.provenance_refusals;
}

MgaPageFinalityReadResult LookupEvidence(
    MgaPageFinalityMap* map,
    const PageFinalityObservedFacts& observed,
    PageFinalityConsumer consumer,
    mga::PageFinalityScope scope) {
  MgaPageFinalityReadResult result;
  if (map == nullptr || map->entries.empty()) {
    mga::PageFinalityMapEntry missing;
    missing.scope = scope;
    missing.status = mga::PageFinalityMapStatus::missing;
    auto missing_observed = observed;
    missing_observed.requested_scope = scope;
    result.decision = mga::EvaluatePageFinalityEvidence(missing, missing_observed, consumer);
    result.map_counters = result.decision.counters;
    if (map != nullptr) {
      MergeCounters(&map->counters, result.decision.counters);
      result.map_counters = map->counters;
    }
    return result;
  }

  PageFinalityEvidenceDecision last_refusal;
  bool saw_refusal = false;
  for (const auto& entry : map->entries) {
    if (entry.scope != scope) {
      continue;
    }
    auto scoped_observed = observed;
    scoped_observed.requested_scope = scope;
    const auto decision = mga::EvaluatePageFinalityEvidence(entry, scoped_observed, consumer);
    MergeCounters(&map->counters, decision.counters);
    if (decision.accepted) {
      result.decision = decision;
      result.map_counters = map->counters;
      return result;
    }
    last_refusal = decision;
    saw_refusal = true;
  }

  if (saw_refusal) {
    result.decision = last_refusal;
  } else {
    mga::PageFinalityMapEntry missing;
    missing.scope = scope;
    missing.status = mga::PageFinalityMapStatus::missing;
    auto scoped_observed = observed;
    scoped_observed.requested_scope = scope;
    result.decision = mga::EvaluatePageFinalityEvidence(missing, scoped_observed, consumer);
    MergeCounters(&map->counters, result.decision.counters);
  }
  result.map_counters = map->counters;
  return result;
}

}  // namespace

MgaPageFinalityReadResult LookupPageAllVisibleEvidence(
    MgaPageFinalityMap* map,
    const PageFinalityObservedFacts& observed,
    PageFinalityConsumer consumer) {
  return LookupEvidence(map, observed, consumer, mga::PageFinalityScope::page);
}

MgaPageFinalityReadResult LookupExtentAllFinalEvidence(
    MgaPageFinalityMap* map,
    const PageFinalityObservedFacts& observed,
    PageFinalityConsumer consumer) {
  return LookupEvidence(map, observed, consumer, mga::PageFinalityScope::extent);
}

}  // namespace scratchbird::storage::page
