// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-OPTIMIZER-HOT-POINT-LOOKUP-CACHE-CLOSURE-ANCHOR

#include "hot_point_lookup_cache.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct OptimizerHotPointLookupDecision {
  bool lookup_allowed = false;
  bool admission_allowed = false;
  bool requires_mga_visibility_recheck = true;
  bool requires_security_authorization_recheck = true;
  bool cache_finality_authority = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

OptimizerHotPointLookupDecision PlanOptimizerHotPointLookup(
    scratchbird::core::index::HotPointProbeClass probe_class,
    bool point_probe,
    bool caller_preserves_mga_visibility_recheck,
    bool caller_preserves_security_authorization_recheck);

}  // namespace scratchbird::engine::optimizer
