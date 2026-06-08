// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "page_finality_evidence.hpp"

#include <vector>

namespace scratchbird::storage::page {

using scratchbird::transaction::mga::PageFinalityConsumer;
using scratchbird::transaction::mga::PageFinalityDecisionCounters;
using scratchbird::transaction::mga::PageFinalityEvidenceDecision;
using scratchbird::transaction::mga::PageFinalityMapEntry;
using scratchbird::transaction::mga::PageFinalityObservedFacts;

struct MgaPageFinalityMap {
  std::vector<PageFinalityMapEntry> entries;
  PageFinalityDecisionCounters counters;
};

struct MgaPageFinalityReadResult {
  PageFinalityEvidenceDecision decision;
  PageFinalityDecisionCounters map_counters;
};

MgaPageFinalityReadResult LookupPageAllVisibleEvidence(
    MgaPageFinalityMap* map,
    const PageFinalityObservedFacts& observed,
    PageFinalityConsumer consumer);

MgaPageFinalityReadResult LookupExtentAllFinalEvidence(
    MgaPageFinalityMap* map,
    const PageFinalityObservedFacts& observed,
    PageFinalityConsumer consumer);

}  // namespace scratchbird::storage::page
