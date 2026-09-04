// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  const auto plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(349));
  const auto root = UniqueTempRoot("sb_rcp080_qow_opt_007_backpressure");
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_opt_007_backpressure"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4100);
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  bool passed = Require(plan.accepted && executed.accepted &&
                            executed.root_published &&
                            executed.backpressure_complete &&
                            executed.rows_received == 9 &&
                            executed.rows_published == 27 &&
                            executed.cells_received == 9 &&
                            executed.pause_count == 6 &&
                            executed.resume_count == 6 &&
                            counters.pauses.load() == 6 &&
                            counters.resumes.load() == 6 &&
                            executed.cleanup_complete &&
                            workspace.Snapshot().active_bytes == 0,
                        "accepted high/low watermark run lost, duplicated, reordered, or leaked rows");

  auto invalid = request;
  invalid.backpressure_low_watermark_rows = 2;
  invalid.backpressure_high_watermark_rows = 2;
  const auto providers_before = counters.provider_cleanups.load();
  const auto allocations_before = workspace.Snapshot().allocation_count;
  const auto refused = executor::ExecuteModelFamilyCompositionV1(invalid);
  passed &= Require(!refused.accepted && !refused.execution_started &&
                        refused.diagnostic_id ==
                            "SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1" &&
                        counters.provider_cleanups.load() == providers_before &&
                        workspace.Snapshot().allocation_count ==
                            allocations_before,
                    "invalid watermarks were not refused before access and spill allocation");
  if (!passed) return 1;
  std::cout << "QOW-OPT-007-BACKPRESSURE: passed;rows=9;pause=6;resume=6;loss=0;duplicate=0;reorder=0\n";
  return 0;
}
