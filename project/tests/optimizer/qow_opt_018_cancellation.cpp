// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

#include <thread>

int main() {
  const auto plan = optimizer::CoordinateModelFamilyDependencyDagV1(
      AdmissionForProfile("COMP-3-CANCEL-FANOUT-V1", 349));
  const auto root = UniqueTempRoot("sb_rcp080_qow_opt_018_cancellation");
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_opt_018_cancellation"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4500);
  request.cancellation_requested = [] { return true; };
  const auto cancelled = executor::ExecuteModelFamilyCompositionV1(request);
  bool passed = Require(!cancelled.accepted && !cancelled.execution_started &&
                            !cancelled.root_published &&
                            cancelled.no_partial_root &&
                            cancelled.cancellation_fanout_complete &&
                            !cancelled.failure_frozen &&
                            cancelled.diagnostic_id ==
                                "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                            cancelled.launched_leg_ordinals.empty() &&
                            cancelled.provider_cleanup_count == 0 &&
                            counters.provider_cleanups.load() == 0 &&
                            counters.exchange_cleanups.load() == 0 &&
                            counters.consumer_cleanups.load() == 0 &&
                            counters.publication_revalidations.load() == 0 &&
                            workspace.Snapshot().allocation_count == 0 &&
                            workspace.Snapshot().active_bytes == 0,
                        "pre-access cancellation performed work or published a root");
  RuntimeCounters live_counters;
  auto live = RuntimeRequest(plan, &workspace, &live_counters, 4510);
  std::atomic_uint64_t provider_starts{0};
  std::atomic_bool live_cancel{false};
  for (std::uint16_t ordinal = 0; ordinal < live.legs.size(); ++ordinal) {
    const auto original = live.legs[ordinal].execution.execute_provider;
    live.legs[ordinal].execution.execute_provider =
        [ordinal, original, &provider_starts, &live_cancel](const auto& input) {
          provider_starts.fetch_add(1, std::memory_order_acq_rel);
          while (provider_starts.load(std::memory_order_acquire) != 3)
            std::this_thread::yield();
          if (ordinal == 2)
            live_cancel.store(true, std::memory_order_release);
          while (!live_cancel.load(std::memory_order_acquire))
            std::this_thread::yield();
          return original(input);
        };
    live.legs[ordinal].execution.cancellation_requested = [] { return false; };
  }
  live.cancellation_requested =
      [&live_cancel] { return live_cancel.load(std::memory_order_acquire); };
  const auto live_result = executor::ExecuteModelFamilyCompositionV1(live);
  passed &= Require(!live_result.accepted && !live_result.root_published &&
                        live_result.no_partial_root &&
                        live_result.cancellation_fanout_complete &&
                        !live_result.failure_frozen &&
                        live_result.diagnostic_id ==
                            "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                        live_result.started_leg_ordinals.size() == 3 &&
                        live_result.cancelled_leg_ordinals.size() == 3 &&
                        live_result.provider_cleanup_count == 3 &&
                        live_counters.provider_cleanups.load() == 3 &&
                        live_result.exchange_cleanup_count == 0 &&
                        live_result.relational_consumer_cleanup_count == 0 &&
                        live_counters.publication_revalidations.load() == 0 &&
                        live_result.cleanup_complete &&
                        workspace.Snapshot().active_bytes == 0,
                    "live mid-wave cancellation did not fan out, clean, and suppress root publication");
  if (!passed) return 1;
  std::cout << "QOW-OPT-018-CANCELLATION: passed;preaccess_launched=0;live_started_cancelled_cleaned=3;root_publish=0\n";
  return 0;
}
