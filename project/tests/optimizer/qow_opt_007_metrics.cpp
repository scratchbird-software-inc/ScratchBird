// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

int main() {
  const auto plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(349));
  const auto root = UniqueTempRoot("sb_rcp080_qow_opt_007_metrics");
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_opt_007_metrics"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4300);
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  const bool receipts_complete =
      executed.rule_receipts.size() == 24 &&
      std::ranges::all_of(executed.rule_receipts, [](const auto& receipt) {
        return receipt.complete && receipt.causal_counter_id != 0 &&
               !receipt.receipt_uuid.empty();
      });
  bool passed = Require(executed.accepted && executed.root_published &&
                            executed.rows_received == 9 &&
                            executed.cells_received == 9 &&
                            executed.rows_published == 27 &&
                            executed.launched_leg_ordinals.size() == 3 &&
                            executed.started_leg_ordinals.size() == 3 &&
                            executed.started_exchange_ordinals.size() == 3 &&
                            executed.started_relational_consumer_ids.size() == 2 &&
                            executed.spill_reserved_bytes == 349 &&
                            executed.spill_cleanup_count == 2 &&
                            executed.provider_cleanup_count == 3 &&
                            executed.exchange_cleanup_count == 3 &&
                            executed.relational_consumer_cleanup_count == 2 &&
                            executed.total_cleanup_count == 10 &&
                            receipts_complete && executed.cleanup_complete,
                        "composition counters or 24 causal rule receipts drifted");
  if (!passed) return 1;
  std::cout << "QOW-OPT-007-METRICS: passed;receipts=24;rows_in=9;cells_in=9;rows_out=27;cleanup=10\n";
  return 0;
}
