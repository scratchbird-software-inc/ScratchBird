// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_page_finality_evidence.hpp"

namespace scratchbird::engine::optimizer {

OptimizerMgaPageFinalityEvidence BuildOptimizerMgaPageFinalityEvidence(
    const scratchbird::storage::page::MgaPageFinalityReadResult& result) {
  OptimizerMgaPageFinalityEvidence evidence;
  evidence.present = true;
  evidence.accepted = result.decision.accepted;
  evidence.all_visible = result.decision.all_visible;
  evidence.all_final = result.decision.all_final;
  evidence.normal_mga_recheck_required = result.decision.normal_mga_recheck_required;
  evidence.finality_map_transaction_authority =
      result.decision.map_is_transaction_finality_authority;
  evidence.evidence_name = result.decision.evidence_name;
  evidence.refusal_reason = result.decision.refusal_reason;
  evidence.evidence_examined = result.map_counters.evidence_examined;
  evidence.accepted_count = result.map_counters.accepted;
  evidence.refused_count = result.map_counters.refused;
  evidence.stale_refusals = result.map_counters.stale_refusals;
  evidence.epoch_refusals = result.map_counters.epoch_refusals;
  evidence.horizon_refusals = result.map_counters.horizon_refusals;
  evidence.provenance_refusals = result.map_counters.provenance_refusals;
  if (!result.decision.accepted) {
    evidence.diagnostics.push_back(result.decision.refusal_reason);
  }
  for (const auto& field : result.decision.evidence) {
    evidence.diagnostics.push_back(field.name + "=" + field.value);
  }
  return evidence;
}

}  // namespace scratchbird::engine::optimizer
