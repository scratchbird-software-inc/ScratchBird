// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_page_finality_map.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct OptimizerMgaPageFinalityEvidence {
  bool present = false;
  bool accepted = false;
  bool all_visible = false;
  bool all_final = false;
  bool normal_mga_recheck_required = true;
  bool finality_map_transaction_authority = false;
  std::string evidence_name;
  std::string authority_source = "durable_mga_transaction_inventory";
  std::string refusal_reason;
  std::uint64_t evidence_examined = 0;
  std::uint64_t accepted_count = 0;
  std::uint64_t refused_count = 0;
  std::uint64_t stale_refusals = 0;
  std::uint64_t epoch_refusals = 0;
  std::uint64_t horizon_refusals = 0;
  std::uint64_t provenance_refusals = 0;
  std::vector<std::string> diagnostics;
};

OptimizerMgaPageFinalityEvidence BuildOptimizerMgaPageFinalityEvidence(
    const scratchbird::storage::page::MgaPageFinalityReadResult& result);

}  // namespace scratchbird::engine::optimizer
