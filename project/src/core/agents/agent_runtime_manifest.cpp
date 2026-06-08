// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_manifest.hpp"

#include <utility>

namespace scratchbird::core::agents {
namespace {

CanonicalAgentManifestEntry Entry(std::string type_id,
                                  AgentDeployment deployment,
                                  std::string scope,
                                  AgentAuthorityClass authority,
                                  AgentActivationProfile activation) {
  CanonicalAgentManifestEntry entry;
  entry.type_id = std::move(type_id);
  entry.deployment = deployment;
  entry.scope = std::move(scope);
  entry.authority = authority;
  entry.default_activation = activation;
  entry.cluster_only = deployment == AgentDeployment::cluster;
  entry.implementation_anchor =
      "project/src/core/agents/agents/" + entry.type_id + ".cpp";
  return entry;
}

}  // namespace

std::vector<CanonicalAgentManifestEntry> CanonicalAgentManifest() {
  return {
#define SB_AGENT_MANIFEST_ENTRY(type_id, deployment, scope, authority, activation) \
  Entry(#type_id, AgentDeployment::deployment, scope, AgentAuthorityClass::authority, \
        AgentActivationProfile::activation),
#include "agent_runtime_manifest.def"
#undef SB_AGENT_MANIFEST_ENTRY
  };
}

std::size_t CanonicalAgentManifestCount() {
  return CanonicalAgentManifest().size();
}

}  // namespace scratchbird::core::agents
