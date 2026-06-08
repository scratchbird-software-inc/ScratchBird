// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_SINGLE_SOURCE_AGENT_MANIFEST
// SEARCH_KEY: ARHC_CANONICAL_AGENT_INVENTORY_RECONCILIATION

#include "agent_runtime.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

struct CanonicalAgentManifestEntry {
  std::string type_id;
  AgentDeployment deployment = AgentDeployment::local;
  std::string scope;
  AgentAuthorityClass authority = AgentAuthorityClass::observe_only;
  AgentActivationProfile default_activation = AgentActivationProfile::observe_only;
  bool cluster_only = false;
  std::string implementation_anchor;
};

std::vector<CanonicalAgentManifestEntry> CanonicalAgentManifest();
std::size_t CanonicalAgentManifestCount();

}  // namespace scratchbird::core::agents
