// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"
#include "agent_runtime_manifest.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

constexpr std::size_t kExpectedCanonicalAgentCount = 29;

#ifndef SB_PRIVATE_REPO_ROOT
#define SB_PRIVATE_REPO_ROOT "."
#endif

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  Require(input.good(), "missing file: " + path.string());
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void RequireContains(const std::filesystem::path& path,
                     const std::string& needle,
                     const std::string& message = {}) {
  const auto content = ReadFile(path);
  Require(content.find(needle) != std::string::npos,
          message.empty()
              ? "missing expected content in " + path.string() + ": " + needle
              : message);
}

void TestManifestInventory() {
  const auto manifest = agents::CanonicalAgentManifest();
  const auto registry = agents::CanonicalAgentRegistry();

  Require(manifest.size() == kExpectedCanonicalAgentCount,
          "canonical manifest count drifted");
  Require(registry.size() == manifest.size(),
          "canonical registry and manifest counts differ");
  Require(agents::CanonicalAgentManifestCount() == manifest.size(),
          "manifest count helper drifted");
  Require(agents::ValidateCanonicalAgentRegistry().ok,
          "canonical registry validation failed");

  std::set<std::string> manifest_names;
  std::map<std::string, agents::CanonicalAgentManifestEntry> manifest_by_name;
  for (const auto& entry : manifest) {
    Require(!entry.type_id.empty(), "empty manifest agent type id");
    Require(manifest_names.insert(entry.type_id).second,
            "duplicate manifest agent: " + entry.type_id);
    Require(entry.cluster_only == (entry.deployment == agents::AgentDeployment::cluster),
            "manifest cluster flag drift: " + entry.type_id);
    manifest_by_name[entry.type_id] = entry;
  }

  std::set<std::string> registry_names;
  for (const auto& descriptor : registry) {
    Require(registry_names.insert(descriptor.type_id).second,
            "duplicate registry agent: " + descriptor.type_id);
    const auto found = manifest_by_name.find(descriptor.type_id);
    Require(found != manifest_by_name.end(),
            "registry agent missing from manifest: " + descriptor.type_id);
    const auto& entry = found->second;
    Require(descriptor.deployment == entry.deployment,
            "deployment drift: " + descriptor.type_id);
    Require(descriptor.scope == entry.scope, "scope drift: " + descriptor.type_id);
    Require(descriptor.authority == entry.authority,
            "authority drift: " + descriptor.type_id);
    Require(descriptor.default_activation == entry.default_activation,
            "activation drift: " + descriptor.type_id);
    Require(descriptor.cluster_only == entry.cluster_only,
            "registry cluster flag drift: " + descriptor.type_id);
    Require(!descriptor.metric_dependencies.empty(),
            "missing metric dependency contract: " + descriptor.type_id);
  }
}

void TestActionContractOwnership() {
  std::set<std::string> manifest_names;
  for (const auto& entry : agents::CanonicalAgentManifest()) {
    manifest_names.insert(entry.type_id);
  }

  std::set<std::string> owner_action_pairs;
  std::set<std::string> action_contract_owners;
  for (const auto& contract : agents::AgentActionContractRegistry()) {
    Require(manifest_names.count(contract.owning_agent) == 1,
            "action contract owner is not canonical: " + contract.owning_agent);
    Require(!contract.action_id.empty(),
            "action contract has empty action id for " + contract.owning_agent);
    const std::string pair = contract.owning_agent + "|" + contract.action_id;
    Require(owner_action_pairs.insert(pair).second,
            "duplicate action contract: " + pair);
    action_contract_owners.insert(contract.owning_agent);
  }

  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    const bool action_capable =
        descriptor.authority == agents::AgentAuthorityClass::request_action ||
        descriptor.authority == agents::AgentAuthorityClass::direct_bounded_action;
    if (action_capable &&
        descriptor.default_activation != agents::AgentActivationProfile::disabled) {
      Require(action_contract_owners.count(descriptor.type_id) == 1,
              "action-capable agent has no action contract: " +
                  descriptor.type_id);
    }
  }
}

void TestImplementationAnchors() {
  const std::filesystem::path repo_root = SB_PRIVATE_REPO_ROOT;
  for (const auto& entry : agents::CanonicalAgentManifest()) {
    Require(!entry.implementation_anchor.empty(),
            "manifest anchor missing: " + entry.type_id);
    Require(std::filesystem::exists(repo_root / entry.implementation_anchor),
            "manifest anchor file missing: " + entry.implementation_anchor);
  }
}

void TestPublicManifestEvidence() {
  const std::filesystem::path repo_root = SB_PRIVATE_REPO_ROOT;
  RequireContains(repo_root / "project/src/core/agents/agent_runtime_manifest.cpp",
                  "CanonicalAgentManifest",
                  "canonical agent manifest factory missing");
  RequireContains(repo_root / "project/src/core/agents/agent_runtime.cpp",
                  "CanonicalAgentRegistry",
                  "canonical agent registry registration missing");
  RequireContains(repo_root / "project/tests/agents/CMakeLists.txt",
                  "agent_runtime_manifest_consistency_gate",
                  "agent runtime manifest consistency gate not registered");
  RequireContains(repo_root / "project/tests/agents/agent_runtime_static_gates.py",
                  "CanonicalAgentManifest",
                  "agent runtime static gate does not inspect public manifest source");
}

}  // namespace

int main() {
  TestManifestInventory();
  TestActionContractOwnership();
  TestImplementationAnchors();
  TestPublicManifestEvidence();
  return EXIT_SUCCESS;
}
