// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

#include <atomic>
#include <thread>

int main() {
  auto admission = Admission(349);
  for (std::uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    admission.legs[ordinal].physical_node_uuid = Uuid(100 + ordinal);
  }
  admission.relational_consumers[0].input_physical_node_uuids = {
      admission.legs[0].physical_node_uuid,
      admission.legs[1].physical_node_uuid};
  admission.relational_consumers[1].input_physical_node_uuids[1] =
      admission.legs[2].physical_node_uuid;
  const auto plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(admission);
  bool passed = Require(plan.accepted && plan.parallel_waves.size() == 1 &&
                            plan.parallel_waves[0] ==
                                std::vector<std::uint16_t>({0, 1, 2}),
                        "failure proof did not admit the ordered parallel wave");

  const auto root = UniqueTempRoot("sb_rcp080_qow_opt_018_failure");
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "rcp080_qow_opt_018_failure";
  policy.root_path = root;
  policy.filespace_quota_bytes = 1024 * 1024;
  policy.session_quota_bytes = 1024 * 1024;
  policy.transaction_quota_bytes = 1024 * 1024;
  policy.statement_quota_bytes = 1024 * 1024;
  policy.operation_quota_bytes = 1024 * 1024;
  policy.cleanup_files_on_release = true;
  memory::TempWorkspaceLifecycleManager workspace(policy);

  std::atomic_uint64_t provider_starts{0};
  std::atomic_uint64_t provider_cleanups{0};
  std::atomic_bool origin_released{false};
  std::atomic_uint64_t publication_revalidations{0};
  std::uint64_t exchange_cleanups = 0;
  std::uint64_t consumer_cleanups = 0;

  executor::ModelFamilyCompositionExecutionRequestV1 request;
  request.admitted_plan = plan;
  for (std::uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    executor::ModelFamilyCompositionExecutionLegV1 leg;
    leg.lexical_source_ordinal = ordinal;
    leg.execution = LegExecution(plan, ordinal, &provider_cleanups);
    const auto accepted_provider = leg.execution.execute_provider;
    leg.execution.execute_provider =
        [ordinal, accepted_provider, &provider_starts,
         &origin_released](const auto& input) {
          provider_starts.fetch_add(1, std::memory_order_acq_rel);
          while (provider_starts.load(std::memory_order_acquire) != 3) {
            std::this_thread::yield();
          }
          if (ordinal == 2) {
            origin_released.store(true, std::memory_order_release);
            executor::ModelProviderExecutionResultV1 failed;
            failed.data_access_observed = true;
            failed.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
            failed.detail = "later lexical leg 2 injected origin failure";
            return failed;
          }
          while (!origin_released.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          return accepted_provider(input);
        };
    leg.pause_exchange = [] {};
    leg.resume_exchange = [] {};
    leg.cleanup_exchange = [&exchange_cleanups] { ++exchange_cleanups; };
    request.legs.push_back(std::move(leg));
  }
  request.execute_relational_consumer = [](const auto&, const auto&,
                                           const auto&) {
    return executor::ModelFamilyRelationalConsumerExecutionResultV1{};
  };
  request.cleanup_relational_consumer =
      [&consumer_cleanups](const auto) { ++consumer_cleanups; };
  request.cancellation_requested = [] { return false; };
  request.revalidate_publication_state =
      [&publication_revalidations, plan] {
        publication_revalidations.fetch_add(1, std::memory_order_relaxed);
        return Publication(plan);
      };
  request.engine_temp_workspace = &workspace;
  request.spill_operation_uuid = Uuid(4000);
  request.spill_resource_contract_uuid = Uuid(804);
  request.spill_owner.temp_object_uuid = Uuid(4001);
  request.spill_owner.database_id = Uuid(4002);
  request.spill_owner.engine_id = Uuid(4003);
  request.spill_owner.session_id = Uuid(4004);
  request.spill_owner.transaction_id = Mga().owning_transaction_uuid;
  request.spill_owner.statement_id = Mga().statement_uuid;
  request.spill_owner.operation_id = request.spill_operation_uuid;
  request.spill_owner.policy_generation = 4;
  request.spill_owner.security_generation = 3;
  request.spill_owner.snapshot_boundary = Mga().statement_snapshot_uuid;
  request.spill_owner.metadata_boundary =
      Mga().statement_metadata_snapshot_uuid;
  request.spill_owner.resource_budget_reference =
      request.spill_resource_contract_uuid;
  request.spill_runtime_generation = 1;
  request.backpressure_high_watermark_rows = 2;
  request.backpressure_low_watermark_rows = 1;
  request.current_selected_plan_generation = 9;
  request.current_mga_statement_context = Mga();

  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  const bool earlier_cancelled =
      std::ranges::find(executed.cancelled_leg_ordinals, 0) !=
          executed.cancelled_leg_ordinals.end() &&
      std::ranges::find(executed.cancelled_leg_ordinals, 1) !=
          executed.cancelled_leg_ordinals.end();
  passed &= Require(!executed.accepted && !executed.root_published &&
                        executed.no_partial_root && executed.failure_frozen &&
                        executed.cancellation_fanout_complete &&
                        executed.diagnostic_id ==
                            "SB_MODEL_COORDINATOR_LEG_FAILED_V1" &&
                        executed.started_leg_ordinals.size() == 3 &&
                        earlier_cancelled &&
                        executed.failed_leg_ordinals ==
                            std::vector<std::uint16_t>({2}) &&
                        executed.provider_cleanup_count == 3 &&
                        provider_cleanups.load(std::memory_order_relaxed) == 3 &&
                        executed.exchange_cleanup_count == 0 &&
                        exchange_cleanups == 0 &&
                        executed.relational_consumer_cleanup_count == 0 &&
                        consumer_cleanups == 0 &&
                        executed.spill_cleanup_count == 1 &&
                        executed.total_cleanup_count == 4 &&
                        executed.root_output_batch.rows.empty() &&
                        publication_revalidations.load(
                            std::memory_order_relaxed) == 0 &&
                        workspace.Snapshot().active_bytes == 0,
                    "originating later-leg failure did not deterministically cancel and clean earlier legs");
  if (!passed) return 1;
  std::cout << "QOW-OPT-018-FAILURE: passed;origin=2;cancelled=0,1;providers_cleaned=3;root_publish=0\n";
  return 0;
}
