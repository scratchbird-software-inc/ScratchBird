// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  const auto plan = optimizer::CoordinateModelFamilyDependencyDagV1(
      AdmissionForProfile("COMP-3-FANIN-V1"));
  const auto root = std::filesystem::temp_directory_path() /
                    "sb_rcp080_qow_opt_018_backpressure";
  std::filesystem::remove_all(root);
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_opt_018_backpressure"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4400);
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  bool passed = Require(plan.accepted && plan.parallel_waves.size() == 2 &&
                            executed.accepted && executed.root_published &&
                            executed.rows_received == 9 &&
                            executed.cells_received == 9 &&
                            executed.rows_published == 27 &&
                            executed.pause_count == 6 &&
                            executed.resume_count == 6 &&
                            counters.pauses.load() == 6 &&
                            counters.resumes.load() == 6 &&
                            executed.backpressure_complete &&
                            executed.cleanup_complete &&
                            workspace.Snapshot().active_bytes == 0,
                        "two-wave fan-in backpressure lifecycle drifted");
  if (!passed) return 1;
  std::cout << "QOW-OPT-018-BACKPRESSURE: passed;waves=2;rows=9;pause=6;resume=6\n";
  return 0;
}
