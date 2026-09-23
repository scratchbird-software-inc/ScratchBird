// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/core/agents/agent_tenant_coordination.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
namespace agents = scratchbird::core::agents;
void Check(bool ok) { if (!ok) std::abort(); }
auto Id(unsigned n) {
  auto id = scratchbird::tests::FixtureUuid(1231, n);
  id.bytes[9] = 0xff; id.bytes[10] = 0;
  return id;
}
int main() {
  agents::AgentTenantCoordinationRequest request;
  request.tenant_budget.tenant_uuid = Id(1);
  request.coordination_group.tenant_uuid = Id(1);
  request.coordination_group.database_uuid = Id(2);
  request.runtime_context.database_uuid = Id(2);
  Check(agents::SameTenantAndScope(request));
  request.runtime_context.database_uuid = Id(3);
  Check(!agents::SameTenantAndScope(request));
  request.runtime_context.database_uuid = Id(2);
  request.coordination_group.tenant_uuid = Id(3);
  Check(!agents::SameTenantAndScope(request));
  request.coordination_group.tenant_uuid = Id(1);
  request.tenant_budget.tenant_uuid.bytes[6] = 0x40;
  Check(!agents::SameTenantAndScope(request));
  request.tenant_budget.tenant_uuid = Id(1);
  request.coordination_group.group_id = "group|one";
  request.lock_request.resource_key = "resource";
  request.requester_instance_id = "instance";
  request.action_id = "action";
  const auto token = agents::CoordinationToken(request);
  Check(token.size() == 64 && token == agents::CoordinationToken(request));
  auto crossed = request;
  crossed.tenant_budget.tenant_uuid = Id(4);
  Check(token != agents::CoordinationToken(crossed));
  crossed = request; crossed.coordination_group.database_uuid = Id(4);
  Check(token != agents::CoordinationToken(crossed));
  crossed = request; crossed.requester_instance_id = "instance|action";
  crossed.action_id.clear();
  Check(token != agents::CoordinationToken(crossed));
  agents::AgentTenantSharedMetricSnapshot metric;
  metric.tenant_uuid = Id(1); metric.scope_uuid = Id(2);
  metric.evidence_uuid = Id(5); metric.metric_family = "a|b";
  metric.source_id = "c"; metric.digest = "digest"; metric.generation = 1;
  const auto digest = agents::MetricsInputDigest({metric});
  Check(digest.size() == 64);
  auto changed = metric; changed.metric_family = "a"; changed.source_id = "b|c";
  Check(digest != agents::MetricsInputDigest({changed}));
  changed = metric; changed.evidence_uuid = Id(6);
  Check(digest != agents::MetricsInputDigest({changed}));
  changed = metric; changed.scope_uuid = Id(6);
  Check(digest != agents::MetricsInputDigest({changed}));
  agents::AgentTenantCoordinationDecision decision;
  decision.identity_evidence.emplace("tenant_uuid", Id(1));
  Check(decision.identity_evidence.at("tenant_uuid") == Id(1));
  std::cout << "PASS tenant binary identity, scope and framed digest binding\n";
}
