// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  const auto plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(349));
  const auto root = std::filesystem::temp_directory_path() /
                    "sb_rcp080_qow_opt_018_cleanup";
  std::filesystem::remove_all(root);
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_opt_018_cleanup"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4600);
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  const auto accounting = workspace.Snapshot();
  bool passed = Require(executed.accepted && executed.root_published &&
                            executed.cleanup_complete &&
                            executed.provider_cleanup_count == 3 &&
                            executed.exchange_cleanup_count == 3 &&
                            executed.relational_consumer_cleanup_count == 2 &&
                            executed.spill_cleanup_count == 2 &&
                            executed.total_cleanup_count == 10 &&
                            counters.provider_cleanups.load() == 3 &&
                            counters.exchange_cleanups.load() == 3 &&
                            counters.consumer_cleanups.load() == 2 &&
                            accounting.cleanup_count == 1 &&
                            accounting.active_bytes == 0 &&
                            workspace.ActiveRecords().empty(),
                        "success cleanup was not exactly once for every started/reserved component");
  RuntimeCounters failure_counters;
  auto cleanup_failure =
      RuntimeRequest(plan, &workspace, &failure_counters, 4610);
  cleanup_failure.legs[1].cleanup_exchange = [&failure_counters] {
    failure_counters.exchange_cleanups.fetch_add(1,
                                                  std::memory_order_relaxed);
    throw std::runtime_error("named exchange cleanup failure");
  };
  const auto failed =
      executor::ExecuteModelFamilyCompositionV1(cleanup_failure);
  const auto cleanup_receipt = std::ranges::find_if(
      failed.rule_receipts,
      [](const auto& receipt) { return receipt.rule_id == "COORD-021-V1"; });
  passed &= Require(!failed.accepted && !failed.root_published &&
                        failed.no_partial_root &&
                        failed.root_output_batch.rows.empty() &&
                        failed.root_publication_receipt_uuid.empty() &&
                        failed.diagnostic_id ==
                            "SB_MODEL_CLEANUP_INCOMPLETE_V1" &&
                        cleanup_receipt != failed.rule_receipts.end() &&
                        !cleanup_receipt->complete &&
                        failed.provider_cleanup_count == 3 &&
                        failed.exchange_cleanup_count == 3 &&
                        failed.relational_consumer_cleanup_count == 2 &&
                        failed.spill_cleanup_count == 2 &&
                        failed.total_cleanup_count == 10 &&
                        failure_counters.provider_cleanups.load() == 3 &&
                        failure_counters.exchange_cleanups.load() == 3 &&
                        failure_counters.consumer_cleanups.load() == 2 &&
                        workspace.Snapshot().active_bytes == 0,
                    "cleanup hook failure did not emit exact COORD-021 refusal without double cleanup or root");
  if (!passed) return 1;
  std::cout << "QOW-OPT-018-CLEANUP: passed;success_total=10;failure_total=10;coord021_refusal=exact;root_on_failure=0\n";
  return 0;
}
