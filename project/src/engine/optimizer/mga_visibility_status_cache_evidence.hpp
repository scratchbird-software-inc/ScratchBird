// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "visibility_status_cache.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct OptimizerMgaVisibilityStatusCacheEvidence {
  bool present = false;
  bool accepted = false;
  bool all_committed = false;
  bool all_visible = false;
  bool no_older_reader = false;
  bool authoritative_path_required = true;
  bool cache_transaction_finality_authority = false;
  std::string evidence_name;
  std::string refusal_reason;
  std::uint64_t probes = 0;
  std::uint64_t accepted_count = 0;
  std::uint64_t refused_count = 0;
  std::uint64_t stale_refusals = 0;
  std::uint64_t epoch_refusals = 0;
  std::uint64_t horizon_refusals = 0;
  std::uint64_t authority_refusals = 0;
  std::vector<std::string> diagnostics;
};

OptimizerMgaVisibilityStatusCacheEvidence BuildOptimizerMgaVisibilityStatusCacheEvidence(
    const scratchbird::transaction::mga::VisibilityStatusCacheDecision& decision);

}  // namespace scratchbird::engine::optimizer
