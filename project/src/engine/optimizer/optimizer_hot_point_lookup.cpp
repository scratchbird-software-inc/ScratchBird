// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_hot_point_lookup.hpp"

namespace scratchbird::engine::optimizer {

OptimizerHotPointLookupDecision PlanOptimizerHotPointLookup(
    scratchbird::core::index::HotPointProbeClass probe_class,
    bool point_probe,
    bool caller_preserves_mga_visibility_recheck,
    bool caller_preserves_security_authorization_recheck) {
  OptimizerHotPointLookupDecision decision;
  decision.requires_mga_visibility_recheck = true;
  decision.requires_security_authorization_recheck = true;
  decision.cache_finality_authority = false;
  decision.evidence.push_back(
      "probe_class=" +
      std::string(scratchbird::core::index::HotPointProbeClassName(probe_class)));
  decision.evidence.push_back("cache_visibility_finality_authority=false");
  decision.evidence.push_back("mga_visibility_recheck=required");
  decision.evidence.push_back("security_authorization_recheck=required");

  if (!point_probe) {
    decision.diagnostic_code =
        "SB_OPTIMIZER_HOT_POINT_LOOKUP_CACHE_POINT_PROBE_REQUIRED";
    decision.evidence.push_back("lookup_refused=not_point_probe");
    return decision;
  }
  if (!caller_preserves_mga_visibility_recheck ||
      !caller_preserves_security_authorization_recheck) {
    decision.diagnostic_code =
        "SB_OPTIMIZER_HOT_POINT_LOOKUP_CACHE_AUTHORITY_REFUSED";
    decision.evidence.push_back("lookup_refused=authority_recheck_missing");
    return decision;
  }

  decision.lookup_allowed = true;
  decision.admission_allowed = true;
  decision.diagnostic_code = "SB_OPTIMIZER_HOT_POINT_LOOKUP_CACHE_ALLOWED";
  decision.evidence.push_back("optimizer_hot_point_lookup_cache_allowed");
  return decision;
}

}  // namespace scratchbird::engine::optimizer
