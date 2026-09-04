// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

#include <thread>

int main() {
  const auto plan = optimizer::CoordinateModelFamilyDependencyDagV1(
      AdmissionForProfile("COMP-3-FAILURE-CLEANUP-V1", 349));
  const auto root = UniqueTempRoot("sb_rcp080_qow_opt_007_failure");
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_opt_007_failure"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4200);
  std::atomic_uint64_t entered{0};
  std::atomic_bool failure_returned{false};
  const auto sibling_provider = request.legs[0].execution.execute_provider;
  request.legs[0].execution.cancellation_requested = [&failure_returned] {
    return failure_returned.load(std::memory_order_acquire);
  };
  request.legs[0].execution.execute_provider =
      [sibling_provider, &entered, &failure_returned](const auto& input) {
        entered.fetch_add(1, std::memory_order_release);
        while (entered.load(std::memory_order_acquire) != 2)
          std::this_thread::yield();
        while (!failure_returned.load(std::memory_order_acquire))
          std::this_thread::yield();
        return sibling_provider(input);
      };
  request.legs[1].execution.execute_provider =
      [&entered, &failure_returned](const auto&) {
        entered.fetch_add(1, std::memory_order_release);
        while (entered.load(std::memory_order_acquire) != 2)
          std::this_thread::yield();
        failure_returned.store(true, std::memory_order_release);
        executor::ModelProviderExecutionResultV1 result;
        result.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
        result.detail = "named F2 provider failure after sibling start";
        return result;
      };
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  bool passed = Require(!executed.accepted && !executed.root_published &&
                            executed.no_partial_root &&
                            executed.root_output_batch.rows.empty() &&
                            executed.failure_frozen &&
                            executed.cancellation_fanout_complete &&
                            executed.diagnostic_id ==
                                "SB_MODEL_COORDINATOR_LEG_FAILED_V1" &&
                            executed.launched_leg_ordinals.size() == 2 &&
                            entered.load() == 2 &&
                            executed.started_leg_ordinals.size() == 2 &&
                            std::ranges::contains(
                                executed.started_leg_ordinals,
                                std::uint16_t{0}) &&
                            std::ranges::contains(
                                executed.started_leg_ordinals,
                                std::uint16_t{1}) &&
                            executed.unstarted_leg_ordinals.empty() &&
                            executed.provider_cleanup_count == 2 &&
                            counters.provider_cleanups.load() == 2 &&
                            executed.failed_leg_ordinals.size() == 1 &&
                            executed.failed_leg_ordinals.front() == 1 &&
                            executed.cancelled_leg_ordinals ==
                                std::vector<std::uint16_t>{0} &&
                            executed.completed_leg_ordinals.empty() &&
                            std::ranges::find(
                                executed.launched_leg_ordinals, 2) ==
                                executed.launched_leg_ordinals.end() &&
                            executed.exchange_cleanup_count == 0 &&
                            executed.relational_consumer_cleanup_count == 0 &&
                            counters.publication_revalidations.load() == 0 &&
                            executed.cleanup_complete &&
                            workspace.Snapshot().active_bytes == 0,
                        "provider failure did not freeze, fan out, clean, and suppress root publication: " +
                            executed.diagnostic_id + ":" + executed.detail +
                            ";started=" + std::to_string(executed.started_leg_ordinals.size()) +
                            ";provider_cleanup=" + std::to_string(executed.provider_cleanup_count) +
                            ";callback_cleanup=" + std::to_string(counters.provider_cleanups.load()) +
                            ";exchange_cleanup=" + std::to_string(executed.exchange_cleanup_count) +
                            ";consumer_cleanup=" + std::to_string(executed.relational_consumer_cleanup_count) +
                            ";spill_cleanup=" + std::to_string(executed.spill_cleanup_count) +
                            ";cleanup=" + std::to_string(executed.cleanup_complete));
  if (!passed) return 1;
  std::cout << "QOW-OPT-007-FAILURE: passed;profile=COMP-3-FAILURE-CLEANUP-V1;launched_wave0=2;dependent_unlaunched=1;providers_started_cleaned="
            << executed.started_leg_ordinals.size()
            << ";providers_cancelled_after_start="
            << executed.cancelled_leg_ordinals.size()
            << ";terminal=3;consumers_started=0;root_publish=0\n";
  return 0;
}
