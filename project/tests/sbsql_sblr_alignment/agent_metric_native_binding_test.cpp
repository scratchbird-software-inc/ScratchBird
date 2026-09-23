// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/core/agents/agent_metric_runtime.cpp"
#include "../../src/core/agents/agent_cluster_boundary.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
namespace agents=scratchbird::core::agents;
void Check(bool ok) { if(!ok) std::abort(); }
auto Id(unsigned n) {
  auto id=scratchbird::tests::FixtureUuid(1224,n);id.bytes[9]=255;id.bytes[10]=0;return id;
}
int main() {
  agents::AgentRuntimeContext context;
  context.database_uuid=Id(1);context.cluster_uuid=Id(2);context.principal_uuid=Id(3);
  const auto engine=agents::ToEngineContext(context);
  Check(engine.database_uuid==Id(1) && engine.cluster_uuid==Id(2) && engine.principal_uuid==Id(3));
  agents::AgentMetricDependency dependency;
  agents::AgentMetricSnapshotEvaluationOptions options;
  Check(agents::ExpectedScopeUuid(context,dependency,options)==Id(1));
  dependency.cluster_only=true;
  Check(agents::ExpectedScopeUuid(context,dependency,options)==Id(2));
  context.cluster_uuid={};
  Check(agents::ExpectedScopeUuid(context,dependency,options).is_nil());
  options.expected_scope_uuid=Id(4);
  Check(agents::ExpectedScopeUuid(context,dependency,options)==Id(4));
  agents::AgentObservedMetricSnapshot snapshot;
  snapshot.metric_family="fixture.metric";snapshot.scope_uuid=Id(1);snapshot.evidence_uuid=Id(5);
  snapshot.namespace_path="ab,c";snapshot.source_id="d";snapshot.snapshot_id="snapshot";
  agents::AgentTypeDescriptor descriptor;descriptor.type_id="fixture.agent";
  const auto first=agents::SnapshotInputDigest(descriptor,context,{snapshot});
  Check(first.size()==64);
  auto collision=snapshot;collision.namespace_path="ab";collision.source_id="c,d";
  Check(first!=agents::SnapshotInputDigest(descriptor,context,{collision}));
  collision=snapshot;collision.scope_uuid=Id(6);
  Check(first!=agents::SnapshotInputDigest(descriptor,context,{collision}));
  auto second=snapshot;second.source_id="second";
  Check(agents::SnapshotInputDigest(descriptor,context,{snapshot,second})==
        agents::SnapshotInputDigest(descriptor,context,{second,snapshot}));
  const auto diagnostic=agents::MetricDiagnostic(dependency,&snapshot,"refused","detail");
  Check(diagnostic.evidence_uuid==Id(5));
  const auto generated=agents::MetricDiagnostic(dependency,nullptr,"refused","detail");
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(generated.evidence_uuid));
  agents::AgentClusterBoundaryResult result;
  agents::AddEvidence(&result,"identity",Id(7));
  Check(std::get<scratchbird::core::platform::Uuid>(result.evidence[0].evidence_id)==Id(7));
  const auto token=agents::DeterministicFenceToken(Id(8),9);
  Check(token.size()==32 && token!=agents::DeterministicFenceToken(Id(8),10) &&
        token!=agents::DeterministicFenceToken(Id(9),9));
  const auto token_identity=Id(8);
  Check(std::equal(token_identity.bytes.begin(),token_identity.bytes.end(),
                   reinterpret_cast<const unsigned char*>(token.data()+8)));
  std::cout << "agent native identity binding passed\n";
}
