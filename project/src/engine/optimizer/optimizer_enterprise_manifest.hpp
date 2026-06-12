// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_NO_STUB_OPTIMIZER_SURFACE
// The enterprise manifest is a truthfulness surface. It records which optimizer
// capabilities are live local engine routes, exact-fallback routes, test-only
// gates, removed claims, or cluster-external boundaries. It is not execution
// authority and it never supplies transaction, visibility, security, recovery,
// catalog, reference, or parser authority.

enum class EnterpriseOptimizerSurfaceClass {
  noncluster_live,
  noncluster_exact_fallback,
  test_only,
  cluster_external,
  removed_claim,
};

struct LegacyOptimizerCapabilityClosure {
  std::string capability_id;
  std::string legacy_capability;
  std::string enterprise_outcome;
  std::string private_anchor;
  std::string closure_status;
};

struct EnterpriseOptimizerSurfaceEntry {
  std::string surface_id;
  std::string route_family;
  EnterpriseOptimizerSurfaceClass surface_class = EnterpriseOptimizerSurfaceClass::removed_claim;
  std::string implementation_anchor;
  std::string diagnostic_code;
  bool production_route_admissible = false;
  bool benchmark_clean_admissible = false;
  bool requires_real_metric_producer = false;
};

struct EnterpriseOptimizerManifestValidation {
  bool ok = false;
  std::vector<std::string> diagnostics;
};

const std::vector<LegacyOptimizerCapabilityClosure>&
LegacyOptimizerCapabilityClosures();

const std::vector<EnterpriseOptimizerSurfaceEntry>&
EnterpriseOptimizerSurfaceManifest();

EnterpriseOptimizerManifestValidation ValidateEnterpriseOptimizerManifest();

const char* EnterpriseOptimizerSurfaceClassName(EnterpriseOptimizerSurfaceClass value);

}  // namespace scratchbird::engine::optimizer
