// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  const auto plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(349));
  const auto root = UniqueTempRoot("sb_rcp080_qow_ias_007");
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_ias_007"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4700);
  auto stale = request;
  ++stale.legs[2].execution.current_provider_generation;
  const auto refused = executor::ExecuteModelFamilyCompositionV1(stale);
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  const bool passed = Require(!refused.accepted &&
                                  !refused.execution_started &&
                                  refused.diagnostic_id ==
                                      "SB_MODEL_BINDING_INCOMPLETE_V1" &&
                                  executed.accepted &&
                                  executed.root_published &&
                                  executed.rows_published == 27 &&
                                  executed.root_output_batch.columns.size() == 3 &&
                                  executed.cleanup_complete &&
                                  workspace.Snapshot().active_bytes == 0,
                              "physical family source registration/generation route drifted");
  if (!passed) return 1;
  std::cout << "QOW-IAS-007: passed;families=relational,document,graph;provider_generation=current;rows=27\n";
  return 0;
}
