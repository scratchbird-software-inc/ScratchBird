// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

#include <thread>

int main() {
  struct ProfileSchedule {
    std::string id;
    std::size_t waves;
    std::size_t arity;
  };
  const std::vector<ProfileSchedule> vectors = {
      {"COMP-3-INDEPENDENT-V1", 1, 3},
      {"COMP-3-LINEAR-V1", 3, 3},
      {"COMP-3-FANIN-V1", 2, 3},
      {"COMP-4-MIXED-V1", 3, 4},
      {"COMP-3-LATERAL-V1", 3, 3},
      {"COMP-3-SHARED-LEG-V1", 2, 3},
      {"COMP-3-SHORT-CIRCUIT-V1", 3, 3},
      {"COMP-3-CANCEL-FANOUT-V1", 1, 3},
      {"COMP-3-FAILURE-CLEANUP-V1", 2, 3}};
  bool passed = true;
  for (const auto& profile : vectors) {
    auto admission = AdmissionForProfile(profile.id);
    if (profile.id == "COMP-3-LATERAL-V1")
      admission.edges[0].edge_kind = "correlation";
    if (profile.id == "COMP-4-MIXED-V1") {
      admission.spill_required = false;
      admission.maximum_spill_bytes = 0;
    }
    const auto admitted =
        optimizer::CoordinateModelFamilyDependencyDagV1(admission);
    passed &= Require(admitted.accepted &&
                          admitted.parallel_waves.size() == profile.waves &&
                          admitted.stable_schedule.size() == profile.arity,
                      profile.id + " stable wave schedule drifted: " +
                          admitted.diagnostic_id + ":" + admitted.detail);
  }
  for (const auto& profile : {std::string("COMP-3-LINEAR-V1"),
                              std::string("COMP-3-FANIN-V1")}) {
    const auto profile_plan =
        optimizer::CoordinateModelFamilyDependencyDagV1(
            AdmissionForProfile(profile));
    const auto profile_root = UniqueTempRoot("sb_rcp080_" + profile);
    memory::TempWorkspaceLifecycleManager profile_workspace(
        WorkspacePolicy(profile_root, "rcp080_" + profile));
    RuntimeCounters profile_counters;
    const auto profile_executed = executor::ExecuteModelFamilyCompositionV1(
        RuntimeRequest(profile_plan, &profile_workspace, &profile_counters,
                       profile == "COMP-3-LINEAR-V1" ? 4920 : 4930));
    passed &= Require(profile_plan.accepted && profile_executed.accepted &&
                          profile_executed.root_published &&
                          profile_executed.rows_published == 27 &&
                          profile_executed.root_output_batch.columns.size() == 3 &&
                          profile_executed.provider_cleanup_count == 3 &&
                          profile_executed.exchange_cleanup_count == 3 &&
                          profile_executed.relational_consumer_cleanup_count == 2 &&
                          profile_executed.cleanup_complete &&
                          profile_workspace.Snapshot().active_bytes == 0,
                      profile + " did not complete its admitted runtime DAG");
  }
  const auto shared_plan = optimizer::CoordinateModelFamilyDependencyDagV1(
      AdmissionForProfile("COMP-3-SHARED-LEG-V1"));
  const auto shared_root = UniqueTempRoot("sb_rcp080_qow_opt_018_shared");
  memory::TempWorkspaceLifecycleManager shared_workspace(
      WorkspacePolicy(shared_root, "rcp080_qow_opt_018_shared"));
  RuntimeCounters shared_counters;
  auto shared_request = RuntimeRequest(shared_plan, &shared_workspace,
                                       &shared_counters, 4935);
  std::atomic_uint64_t shared_provider_calls{0};
  const auto shared_provider = shared_request.legs[0].execution.execute_provider;
  shared_request.legs[0].execution.execute_provider =
      [shared_provider, &shared_provider_calls](const auto& input) {
        shared_provider_calls.fetch_add(1);
        return shared_provider(input);
      };
  const auto shared_executed =
      executor::ExecuteModelFamilyCompositionV1(shared_request);
  passed &= Require(shared_plan.accepted && shared_executed.accepted &&
                        shared_executed.root_published &&
                        shared_provider_calls.load() == 1 &&
                        shared_executed.started_leg_ordinals.size() == 3 &&
                        shared_executed.started_relational_consumer_ids.size() == 3 &&
                        shared_executed.root_output_batch.columns.size() == 3 &&
                        shared_executed.rows_published == 81 &&
                        shared_executed.provider_cleanup_count == 3 &&
                        shared_executed.exchange_cleanup_count == 3 &&
                        shared_executed.relational_consumer_cleanup_count == 3 &&
                        shared_executed.cleanup_complete &&
                        shared_workspace.Snapshot().active_bytes == 0,
                    "shared provider did not execute exactly once for two consumers");

  auto mixed_admission = AdmissionForProfile("COMP-4-MIXED-V1", 0);
  mixed_admission.spill_required = false;
  const auto mixed_plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(mixed_admission);
  const auto mixed_root = UniqueTempRoot("sb_rcp080_qow_opt_018_mixed");
  memory::TempWorkspaceLifecycleManager mixed_workspace(
      WorkspacePolicy(mixed_root, "rcp080_qow_opt_018_mixed"));
  RuntimeCounters mixed_counters;
  const auto mixed_executed = executor::ExecuteModelFamilyCompositionV1(
      RuntimeRequest(mixed_plan, &mixed_workspace, &mixed_counters, 4940));
  passed &= Require(mixed_plan.accepted && mixed_executed.accepted &&
                        mixed_executed.root_published &&
                        mixed_executed.started_leg_ordinals.size() == 4 &&
                        mixed_executed.completed_leg_ordinals.size() == 4 &&
                        mixed_executed.started_relational_consumer_ids.size() == 3 &&
                        mixed_executed.root_output_batch.columns.size() == 6 &&
                        mixed_executed.rows_published == 81 &&
                        mixed_executed.provider_cleanup_count == 4 &&
                        mixed_executed.exchange_cleanup_count == 4 &&
                        mixed_executed.relational_consumer_cleanup_count == 3 &&
                        mixed_executed.cleanup_complete &&
                        mixed_workspace.Snapshot().active_bytes == 0,
                    "four-family mixed profile did not execute its exact runtime DAG: " +
                        mixed_executed.diagnostic_id + ":" +
                        mixed_executed.detail + ";started=" +
                        std::to_string(mixed_executed.started_leg_ordinals.size()) +
                        ";completed=" +
                        std::to_string(mixed_executed.completed_leg_ordinals.size()) +
                        ";consumers=" +
                        std::to_string(
                            mixed_executed.started_relational_consumer_ids.size()) +
                        ";columns=" +
                        std::to_string(
                            mixed_executed.root_output_batch.columns.size()) +
                        ";rows=" +
                        std::to_string(mixed_executed.rows_published) +
                        ";failed=" +
                        (mixed_executed.failed_leg_ordinals.empty()
                             ? "none"
                             : std::to_string(
                                   mixed_executed.failed_leg_ordinals.front())) +
                        ";cancelled=" +
                        (mixed_executed.cancelled_leg_ordinals.empty()
                             ? "none"
                             : std::to_string(mixed_executed
                                                  .cancelled_leg_ordinals
                                                  .front())));

  auto short_admission = AdmissionForProfile("COMP-3-SHORT-CIRCUIT-V1", 0);
  short_admission.spill_required = false;
  const auto short_plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(short_admission);
  const auto short_root = UniqueTempRoot("sb_rcp080_qow_opt_018_short");
  memory::TempWorkspaceLifecycleManager short_workspace(
      WorkspacePolicy(short_root, "rcp080_qow_opt_018_short"));
  RuntimeCounters short_counters;
  auto short_request = RuntimeRequest(short_plan, &short_workspace,
                                      &short_counters, 4945);
  const auto short_provider = short_request.legs[0].execution.execute_provider;
  short_request.legs[0].execution.execute_provider =
      [short_provider](const auto& input) {
        auto value = short_provider(input);
        value.rows_examined = 0;
        value.provider_batch.batch.rows.clear();
        value.provider_batch.ordered_row_identities.clear();
        return value;
      };
  const auto short_executed =
      executor::ExecuteModelFamilyCompositionV1(short_request);
  passed &= Require(short_plan.accepted && short_executed.accepted &&
                        short_executed.root_published &&
                        short_executed.root_output_batch.rows.empty() &&
                        short_executed.launched_leg_ordinals ==
                            std::vector<std::uint16_t>({0}) &&
                        short_executed.unstarted_leg_ordinals.size() == 2 &&
                        short_executed.provider_entry_count == 1 &&
                        short_executed.observed_data_access_count == 1 &&
                        std::ranges::contains(short_executed.unstarted_leg_ordinals,
                                              std::uint16_t{1}) &&
                        std::ranges::contains(short_executed.unstarted_leg_ordinals,
                                              std::uint16_t{2}) &&
                        short_executed.provider_cleanup_count == 1 &&
                        short_executed.exchange_cleanup_count == 1 &&
                        short_executed.relational_consumer_cleanup_count == 0 &&
                        short_executed.total_cleanup_count == 2 &&
                        short_executed.cleanup_complete &&
                        short_workspace.Snapshot().active_bytes == 0,
                    "empty short-circuit started a dependent or published a nonempty root");
  auto lateral_admission = AdmissionForProfile("COMP-3-LATERAL-V1");
  lateral_admission.edges[0].edge_kind = "correlation";
  const auto lateral_plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(lateral_admission);
  const auto lateral_root = UniqueTempRoot("sb_rcp080_qow_opt_018_lateral");
  memory::TempWorkspaceLifecycleManager lateral_workspace(
      WorkspacePolicy(lateral_root, "rcp080_qow_opt_018_lateral"));
  RuntimeCounters lateral_counters;
  auto lateral = RuntimeRequest(lateral_plan, &lateral_workspace,
                                &lateral_counters, 4950);
  std::atomic_uint64_t correlated_evaluations{0};
  const auto right_request = lateral.legs[1].execution;
  lateral.legs[1].execute_correlated_provider =
      [right_request, &correlated_evaluations](const auto& execution,
                                               const auto& outer_row,
                                               const auto invocation) {
        if (outer_row.values.empty() || invocation > 2)
          return executor::ModelFamilyExecutionResultV1{};
        correlated_evaluations.fetch_add(1, std::memory_order_relaxed);
        auto result = executor::ExecuteModelFamilySourceV1(execution);
        const auto correlated_row = result.output.batch.rows[invocation];
        result.output.batch.rows = {correlated_row};
        return result;
      };
  const auto lateral_executed =
      executor::ExecuteModelFamilyCompositionV1(lateral);
  passed &= Require(lateral_plan.accepted && lateral_executed.accepted &&
                        lateral_executed.root_published &&
                        lateral_executed.visible_dependency_rows == 3 &&
                        lateral_executed.right_evaluation_count == 3 &&
                        correlated_evaluations.load() == 3 &&
                        std::ranges::count(
                            lateral_executed.launched_leg_ordinals, 1) == 3 &&
                        std::ranges::count(
                            lateral_executed.started_leg_ordinals, 1) == 3 &&
                        lateral_executed.provider_entry_count == 5 &&
                        lateral_executed.observed_data_access_count == 5 &&
                        lateral_executed.provider_cleanup_count == 5 &&
                        lateral_counters.provider_cleanups.load() == 5 &&
                        lateral_executed.exchange_cleanup_count == 5 &&
                        lateral_executed.relational_consumer_cleanup_count == 4 &&
                        lateral_executed.total_cleanup_count == 16 &&
                        lateral_executed.rows_received == 9 &&
                        lateral_executed.rows_published == 9 &&
                        lateral_executed.root_output_batch.rows.size() == 9 &&
                        lateral_executed.root_output_batch.rows[0].values[0]
                                .encoded_value == Uuid(2000) &&
                        lateral_executed.root_output_batch.rows[0].values[1]
                                .encoded_value == Uuid(2010) &&
                        lateral_executed.root_output_batch.rows[3].values[0]
                                .encoded_value == Uuid(2001) &&
                        lateral_executed.root_output_batch.rows[3].values[1]
                                .encoded_value == Uuid(2011) &&
                        lateral_executed.root_output_batch.rows[6].values[0]
                                .encoded_value == Uuid(2002) &&
                        lateral_executed.root_output_batch.rows[6].values[1]
                                .encoded_value == Uuid(2012) &&
                        lateral_executed.cleanup_complete &&
                        lateral_workspace.Snapshot().active_bytes == 0,
                    "lateral right leg did not execute once per visible outer row in order");
  if (!lateral_executed.accepted) {
    std::cerr << "QOW-OPT-018-LATERAL-DETAIL: "
              << lateral_executed.diagnostic_id << ':'
              << lateral_executed.detail
              << ";visible=" << lateral_executed.visible_dependency_rows
              << ";right=" << lateral_executed.right_evaluation_count
              << ";eval=" << correlated_evaluations.load()
              << ";launched1="
              << std::ranges::count(lateral_executed.launched_leg_ordinals, 1)
              << ";started1="
              << std::ranges::count(lateral_executed.started_leg_ordinals, 1)
              << ";providers=" << lateral_executed.provider_cleanup_count
              << ";exchanges=" << lateral_executed.exchange_cleanup_count
              << ";consumers="
              << lateral_executed.relational_consumer_cleanup_count
              << ";total=" << lateral_executed.total_cleanup_count << '\n';
  }
  auto reversed_plan = lateral_plan;
  std::reverse(reversed_plan.dependency_edges.begin(),
               reversed_plan.dependency_edges.end());
  RuntimeCounters reversed_counters;
  auto reversed = RuntimeRequest(reversed_plan, &lateral_workspace,
                                 &reversed_counters, 4960);
  std::atomic_uint64_t reversed_evaluations{0};
  reversed.legs[1].execute_correlated_provider =
      [right_request, &reversed_evaluations](const auto& execution,
                                             const auto& outer_row,
                                             const auto invocation) {
        if (outer_row.values.empty() || invocation > 2)
          return executor::ModelFamilyExecutionResultV1{};
        reversed_evaluations.fetch_add(1);
        auto result = executor::ExecuteModelFamilySourceV1(execution);
        const auto correlated_row = result.output.batch.rows[invocation];
        result.output.batch.rows = {correlated_row};
        return result;
      };
  const auto reversed_executed =
      executor::ExecuteModelFamilyCompositionV1(reversed);
  passed &= Require(reversed_executed.accepted &&
                        reversed_executed.right_evaluation_count == 3 &&
                        reversed_evaluations.load() == 3 &&
                        reversed_executed.provider_entry_count == 5 &&
                        reversed_executed.observed_data_access_count == 5 &&
                        reversed_executed.rows_published == 9 &&
                        reversed_executed.root_output_batch.rows.size() ==
                            lateral_executed.root_output_batch.rows.size() &&
                        reversed_executed.root_output_batch.rows[3].values[0]
                                .encoded_value ==
                            lateral_executed.root_output_batch.rows[3].values[0]
                                .encoded_value &&
                        reversed_executed.root_output_batch.rows[3].values[1]
                                .encoded_value ==
                            lateral_executed.root_output_batch.rows[3].values[1]
                                .encoded_value,
                    "reversed valid edge order bypassed or reordered lateral execution");
  auto refused_input = AdmissionForProfile("COMP-3-INDEPENDENT-V1");
  refused_input.legs[1].parallel_eligible = false;
  const auto refused =
      optimizer::CoordinateModelFamilyDependencyDagV1(refused_input);
  passed &= Require(!refused.accepted &&
                        refused.diagnostic_id ==
                            "SB_MODEL_PARALLEL_ADMISSION_REFUSED_V1",
                    "ineligible independent leg was admitted in parallel");
  const auto plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(349));
  const auto root = UniqueTempRoot("sb_rcp080_qow_opt_018_parallel");
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_opt_018_parallel"));
  RuntimeCounters counters;
  auto request = RuntimeRequest(plan, &workspace, &counters, 4900);
  std::atomic_uint64_t entered{0};
  std::atomic_uint64_t active{0};
  std::atomic_uint64_t peak{0};
  for (auto& leg : request.legs) {
    const auto original = leg.execution.execute_provider;
    leg.execution.execute_provider =
        [original, &entered, &active, &peak](const auto& input) {
          const auto now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
          auto observed = peak.load(std::memory_order_acquire);
          while (observed < now &&
                 !peak.compare_exchange_weak(observed, now,
                                             std::memory_order_acq_rel)) {}
          entered.fetch_add(1, std::memory_order_release);
          while (entered.load(std::memory_order_acquire) != 3)
            std::this_thread::yield();
          auto result = original(input);
          active.fetch_sub(1, std::memory_order_acq_rel);
          return result;
        };
  }
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  passed &= Require(executed.accepted && executed.root_published &&
                        entered.load() == 3 && peak.load() == 3 &&
                        executed.completed_leg_ordinals.size() == 3 &&
                        executed.cleanup_complete &&
                        workspace.Snapshot().active_bytes == 0,
                    "independent wave did not execute three providers concurrently");

  const auto guarded_root = UniqueTempRoot("sb_rcp080_qow_opt_018_guarded_mga");
  memory::TempWorkspaceLifecycleManager guarded_workspace(
      WorkspacePolicy(guarded_root, "rcp080_qow_opt_018_guarded_mga"));
  RuntimeCounters guarded_counters;
  auto guarded = RuntimeRequest(plan, &guarded_workspace, &guarded_counters,
                                4910);
  guarded.engine_mga_inventory_guard_owned_by_caller = true;
  const auto caller_thread = std::this_thread::get_id();
  std::atomic_uint64_t guarded_calls{0};
  std::atomic_bool caller_thread_preserved{true};
  for (auto& leg : guarded.legs) {
    const auto original = leg.execution.execute_provider;
    leg.execution.execute_provider =
        [original, caller_thread, &guarded_calls,
         &caller_thread_preserved](const auto& input) {
          if (std::this_thread::get_id() != caller_thread) {
            caller_thread_preserved.store(false, std::memory_order_release);
          }
          guarded_calls.fetch_add(1, std::memory_order_acq_rel);
          return original(input);
        };
  }
  const auto guarded_executed =
      executor::ExecuteModelFamilyCompositionV1(guarded);
  passed &= Require(
      guarded_executed.accepted && guarded_executed.root_published &&
          guarded_calls.load() == 3 && caller_thread_preserved.load() &&
          guarded_executed.completed_leg_ordinals.size() == 3 &&
          guarded_executed.provider_cleanup_count == 3 &&
          guarded_executed.exchange_cleanup_count == 3 &&
          guarded_executed.relational_consumer_cleanup_count == 2 &&
          guarded_executed.cleanup_complete &&
          guarded_workspace.Snapshot().active_bytes == 0,
      "caller-owned MGA guard mode did not preserve caller-thread execution and cleanup semantics");
  if (!passed) return 1;
  std::cout << "QOW-OPT-018-PARALLEL: passed;independent_waves=1;fanin_waves=2;linear_waves=3;provider_peak=3;guarded_mga_caller_thread=3;lateral_visible_rows=3;lateral_right_evaluations=3\n";
  return 0;
}
