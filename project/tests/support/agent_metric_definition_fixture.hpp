// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binary_uuid_fixture.hpp"
#include "agent_runtime.hpp"
#include "metric_builtin_definitions.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace scratchbird::tests {
// Definition-only inputs for agent policy/probe tests. This does not bind a
// node queue, admit any series, emit samples, establish freshness or attest
// producer trust. Deliberately leave readiness contract-ready/unwired rather
// than claiming that this component fixture implements the producer.
inline void AdmitAgentMetricDefinitionsForProbe(
    core::metrics::MetricRegistry& registry,
    const std::vector<core::agents::AgentTypeDescriptor>& agents,
    std::uint32_t fixture_domain) {
  namespace m = core::metrics;
  const auto definitions = m::BuiltinMetricDescriptorDefinitions();
  std::set<std::string> admitted;
  std::uint32_t ordinal = 0;
  for (const auto& agent : agents) for (const auto& dependency : agent.metric_dependencies) {
    if (dependency.cluster_only || !dependency.required) continue;
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& row) {
      return row.family == dependency.metric_family ||
          std::find(row.aliases.begin(), row.aliases.end(), dependency.metric_family) != row.aliases.end();
    });
    if (found == definitions.end())
      throw std::runtime_error("required agent metric definition missing: " + dependency.metric_family);
    if (!admitted.insert(found->family).second) continue;
    m::MetricDescriptor descriptor;
    static_cast<m::MetricDescriptorDefinition&>(descriptor) = *found;
    descriptor.metric_uuid = FixtureUuid(fixture_domain, ++ordinal);
    descriptor.descriptor_generation = 1;
    descriptor.label_schema_uuid = FixtureUuid(fixture_domain, ++ordinal);
    descriptor.label_schema_generation = 1;
    descriptor.retention_policy_uuid = FixtureUuid(fixture_domain, ++ordinal);
    descriptor.retention_policy_generation = 1;
    descriptor.visibility_policy_uuid = FixtureUuid(fixture_domain, ++ordinal);
    descriptor.visibility_policy_generation = 1;
    descriptor.readiness = m::MetricReadiness::contract_ready_unwired;
    const auto status = registry.RegisterDescriptor(std::move(descriptor));
    if (!status.ok) throw std::runtime_error(status.diagnostic_code + ":" + status.detail);
  }
  if (!registry.SnapshotCurrent().empty() || !registry.SnapshotHistory().empty())
    throw std::runtime_error("definition-only fixture emitted observations");
}
}  // namespace scratchbird::tests
