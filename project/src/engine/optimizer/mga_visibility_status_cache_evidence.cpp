// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_visibility_status_cache_evidence.hpp"

namespace scratchbird::engine::optimizer {

OptimizerMgaVisibilityStatusCacheEvidence BuildOptimizerMgaVisibilityStatusCacheEvidence(
    const scratchbird::transaction::mga::VisibilityStatusCacheDecision& decision) {
  OptimizerMgaVisibilityStatusCacheEvidence evidence;
  evidence.present = true;
  evidence.accepted = decision.accepted;
  evidence.all_committed = decision.all_committed;
  evidence.all_visible = decision.all_visible;
  evidence.no_older_reader = decision.no_older_reader;
  evidence.authoritative_path_required = decision.authoritative_path_required;
  evidence.cache_transaction_finality_authority =
      decision.cache_is_transaction_finality_authority;
  evidence.evidence_name = decision.evidence_name;
  evidence.refusal_reason = decision.refusal_reason;
  evidence.probes = decision.counters.probes;
  evidence.accepted_count = decision.counters.accepted;
  evidence.refused_count = decision.counters.refused;
  evidence.stale_refusals = decision.counters.stale_refusals;
  evidence.epoch_refusals = decision.counters.epoch_refusals;
  evidence.horizon_refusals = decision.counters.horizon_refusals;
  evidence.authority_refusals = decision.counters.authority_refusals;
  if (!decision.accepted) {
    evidence.diagnostics.push_back(decision.refusal_reason);
  }
  for (const auto& field : decision.evidence) {
    evidence.diagnostics.push_back(field.name + "=" + field.value);
  }
  return evidence;
}

}  // namespace scratchbird::engine::optimizer
