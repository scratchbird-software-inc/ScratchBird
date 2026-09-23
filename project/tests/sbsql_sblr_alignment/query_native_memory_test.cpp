// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_query_physical_registration.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace s = scratchbird::engine::sblr;
namespace e = scratchbird::engine::executor;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  e::TypedPhysicalNodeDag dag;
  dag.nodes.emplace_back();
  dag.admission_evidence.emplace_back();
  const auto baseline = s::BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag);
  Check(baseline.has_value());
  auto id = scratchbird::tests::FixtureUuid(1072, 1);
  id.bytes[14] = 0; id.bytes[15] = 0xff;
  dag.selected_plan_uuid = id;
  dag.bound_sblr_tree_uuid = id;
  dag.mga_statement_context.statement_uuid = id;
  dag.mga_statement_context.owning_transaction_uuid = id;
  dag.admission_evidence.front().evidence_uuid = id;
  dag.nodes.front().selected_alternative_uuid = id;
  dag.nodes.front().executor_capability_uuid = id;
  Check(s::BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag) == baseline);
  dag.nodes.front().required_property_uuids.push_back(id);
  dag.nodes.front().delivered_property_uuids.push_back(id);
  dag.nodes.front().enforced_property_uuids.push_back(id);
  Check(s::BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag) == *baseline + 48);
  dag.nodes.front().mga_statement_context.active_excluded_local_transaction_ids.push_back(UINT64_MAX);
  Check(s::BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag) == *baseline + 56);
  dag.nodes.front().implementation_id = "abcdefgh";
  Check(s::BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag) == *baseline + 64);
  e::CanonicalPhysicalDispatchStepResult cancelled;
  e::DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.diagnostic_code = "SB_MODEL_EXECUTION_CANCELLED_V1";
  s::BindLiveCancellationFailure(diagnostic, &dag.admission_evidence.front(), &cancelled);
  Check(cancelled.cancellation_observed && cancelled.cancellation_evidence_uuid == id);
  s::BindLiveCancellationFailure(diagnostic, nullptr, &cancelled);
  Check(cancelled.cancellation_observed && cancelled.cancellation_evidence_uuid.is_nil());
}
