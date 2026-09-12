// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_prepare_metric_collector.hpp"

#include "../../core/hash/hash_digest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <mutex>
#include <type_traits>
#include <map>
#include <ranges>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {


std::uint64_t MonotonicNowNs() {
  const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return value <= 0 ? 1 : static_cast<std::uint64_t>(value);
}

bool CanonicalUuid(const internal_api::EngineUuid& value) noexcept {
  return scratchbird::core::uuid::IsEngineIdentityUuid(value);
}

template <typename F>
struct ScopeExit {
  F action;
  ~ScopeExit() noexcept { action(); }
};

// Versioned opaque content, not a UUID or a SQL/name lookup key.
void AppendNumber(std::string& out, std::uint64_t value) {
  for (unsigned byte = 0; byte != 8; ++byte) {
    out.push_back(static_cast<char>(value & 0xff));
    value >>= 8;
  }
}
void AppendText(std::string& out, std::string_view value) {
  AppendNumber(out, value.size());
  out.append(value);
}
void AppendUuid(std::string& out, const internal_api::EngineUuid& value) {
  out.append(reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size());
}

std::string DerivedDigest(const std::string_view payload) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(
          payload.data()),
      payload.size());
  return digest.ok() ? scratchbird::core::hash::HexLower(digest.digest)
                     : std::string{};
}

bool SortedDistinct(const std::vector<internal_api::EngineUuid>& values,
                    const bool require_nonempty) {
  if (require_nonempty && values.empty()) return false;
  return std::ranges::is_sorted(values) &&
         std::ranges::adjacent_find(values) == values.end() &&
         std::ranges::all_of(values,
                             [](const auto& value) { return CanonicalUuid(value); });
}

std::string MetricReceiptSeed(
    const CanonicalPrepareWithMetricCollectionRequest& request,
    const CanonicalPreparedMetricCollectionReceipt& receipt) {
  std::string seed{"SBPM\x01\x01", 6};
  AppendUuid(seed, request.coordinator_policy_uuid);
  AppendNumber(seed, request.coordinator_policy_generation);
  AppendUuid(seed, request.bound_sblr_tree_uuid);
  AppendUuid(seed, request.route_snapshot_uuid);
  AppendNumber(seed, request.route_epoch);
  AppendNumber(seed, request.route_generation);
  AppendText(seed, request.cluster_scope_id);
  AppendUuid(seed, receipt.leg_uuid);
  AppendText(seed, receipt.family_id);
  AppendUuid(seed, receipt.metric_snapshot_uuid);
  AppendNumber(seed, receipt.metric_snapshot_generation);
  AppendNumber(seed, receipt.dependency_leg_uuids.size());
  for (const auto& dependency : receipt.dependency_leg_uuids) {
    AppendUuid(seed, dependency);
  }
  AppendNumber(seed, receipt.required_metric_ids.size());
  for (const auto& required : receipt.required_metric_ids) AppendText(seed, required);
  AppendNumber(seed, receipt.metrics.size());
  for (const auto& metric : receipt.metrics) {
    AppendText(seed, metric.metric_id);
    AppendText(seed, metric.unit_id);
    AppendNumber(seed, metric.unsigned_value);
    AppendUuid(seed, metric.source_snapshot_uuid);
    AppendNumber(seed, metric.source_generation);
  }
  return seed;
}

struct TaskResult {
  CanonicalPreparedMetricCollectionReceipt metric;
  CanonicalPreparedLegPlanReceipt plan;
  bool started{false};
  bool succeeded{false};
  bool cancelled{false};
  bool timed_out{false};
  bool cleanup_complete{false};
  std::string_view field_id;
  std::string detail;
  bool resource_exhausted{false};
};

}  // namespace

std::vector<std::string> CanonicalRequiredPrepareMetricIdsForFamily(
    const std::string& family_id) {
  static const std::map<std::string, std::vector<std::string>> inventories = {
      {"relational",
       {"available_memory_bytes", "filespace_available_bytes",
        "index_coverage_basis_points", "page_density_basis_points",
        "row_count", "version_chain_depth"}},
      {"document",
       {"average_document_bytes", "document_count",
        "document_index_coverage_basis_points"}},
      {"graph",
       {"adjacency_edge_count", "connectivity_density_basis_points",
        "vertex_count"}},
      {"time_series",
       {"retention_horizon_ns", "sample_count", "series_count"}},
      {"text_search",
       {"document_frequency_count", "search_index_generation",
        "term_count"}},
      {"vector",
       {"approximate_neighbor_count", "precision_basis_points",
        "recall_basis_points", "vector_index_state"}},
      {"spatial",
       {"geometry_count", "spatial_index_coverage_basis_points",
        "spatial_partition_count"}},
      {"key_value", {"key_count", "tombstone_count", "value_bytes"}},
  };
  const auto found = inventories.find(family_id);
  return found == inventories.end() ? std::vector<std::string>{}
                                    : found->second;
}

static CanonicalPrepareWithMetricCollectionResult
PrepareMetricCollectionImpl(
    const CanonicalPrepareWithMetricCollectionRequest& request,
    CanonicalPreparedPlanStore* prepared_plan_store,
    CanonicalPrepareWithMetricCollectionResult& result) {
  const auto refuse = [&](std::string field_id, std::string detail) {
    result.accepted = false;
    result.metrics_collected = false;
    result.legs_planned = false;
    result.prepared = false;
    result.persisted = false;
    result.prepare_result = {};
    std::vector<CanonicalPreparedMetricCollectionReceipt>{}.swap(result.metric_receipts);
    std::vector<CanonicalPreparedLegPlanReceipt>{}.swap(result.leg_plan_receipts);
    result.issues = {{"QOW-DIAG-OPT-PREPARE-METRIC-COLLECTION-REFUSAL-V1",
                      std::move(field_id), std::move(detail)}};
    return std::move(result);
  };

  if (!request.engine_prepare_authorized ||
      !request.global_security_admitted || !request.global_mga_admitted ||
      request.parser_execution_authority_claimed ||
      request.transaction_visibility_authority_claimed ||
      request.transaction_finality_authority_claimed ||
      request.recovery_authority_claimed) {
    return refuse("prepare_metric_authority_scope",
                  "engine PREPARE, security, and MGA admission are required");
  }
  if (!CanonicalUuid(request.coordinator_policy_uuid) ||
      request.coordinator_policy_generation == 0 ||
      !CanonicalUuid(request.bound_sblr_tree_uuid) ||
      !CanonicalUuid(request.route_snapshot_uuid) || request.route_epoch == 0 ||
      request.route_generation == 0 || request.cluster_scope_id.empty() ||
      request.metric_thread_budget == 0 ||
      request.timeout_ns == 0 ||
      request.legs.empty() ||
      request.legs.size() > std::numeric_limits<std::uint32_t>::max() ||
      !request.assemble_selected_plan || prepared_plan_store == nullptr) {
    return refuse("prepare_metric_request",
                  "policy, snapshot, worker, timeout, leg, or store is invalid");
  }

  std::map<internal_api::EngineUuid, std::size_t> index_by_uuid;
  for (std::size_t index = 0; index < request.legs.size(); ++index) {
    const auto& leg = request.legs[index];
    const auto required =
        CanonicalRequiredPrepareMetricIdsForFamily(leg.family_id);
    if (!CanonicalUuid(leg.leg_uuid) || required.empty() ||
        leg.required_metric_ids != required ||
        !SortedDistinct(leg.dependency_leg_uuids, false) ||
        !index_by_uuid.emplace(leg.leg_uuid, index).second ||
        !leg.collect_metrics ||
        !leg.plan_leg || !leg.cleanup_transient_state) {
      return refuse("prepare_metric_leg_request",
                    "leg identity, family inventory, or callbacks are invalid");
    }
  }

  std::vector<std::vector<std::size_t>> outgoing(request.legs.size());
  std::vector<std::size_t> indegree(request.legs.size(), 0);
  for (std::size_t index = 0; index < request.legs.size(); ++index) {
    for (const auto& dependency_uuid :
         request.legs[index].dependency_leg_uuids) {
      const auto dependency = index_by_uuid.find(dependency_uuid);
      if (dependency == index_by_uuid.end() || dependency->second == index) {
        return refuse("prepare_metric_dependency_chain",
                      "dependency endpoint is absent or self-referential");
      }
      outgoing[dependency->second].push_back(index);
      ++indegree[index];
    }
  }

  std::vector<std::vector<std::size_t>> waves;
  auto remaining_indegree = indegree;
  std::vector<bool> scheduled(request.legs.size(), false);
  std::size_t scheduled_count = 0;
  while (scheduled_count < request.legs.size()) {
    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < request.legs.size(); ++index) {
      if (!scheduled[index] && remaining_indegree[index] == 0) {
        ready.push_back(index);
      }
    }
    if (ready.empty()) {
      return refuse("prepare_metric_dependency_chain",
                    "dependency chain is cyclic");
    }
    std::ranges::sort(ready, [&](const auto left, const auto right) {
      return request.legs[left].leg_uuid < request.legs[right].leg_uuid;
    });
    for (const auto index : ready) {
      scheduled[index] = true;
      ++scheduled_count;
      for (const auto consumer : outgoing[index]) {
        --remaining_indegree[consumer];
      }
    }
    waves.push_back(std::move(ready));
  }

  const auto started_ns = MonotonicNowNs();
  if (request.timeout_ns > std::numeric_limits<std::uint64_t>::max() - started_ns) {
    return refuse("prepare_metric_timeout", "PREPARE deadline overflows the clock");
  }
  const auto deadline_ns = started_ns + request.timeout_ns;
  std::stop_source stop_source;
  std::atomic<std::uint16_t> active_workers{0};
  std::atomic<std::uint16_t> maximum_active_workers{0};
  std::atomic<std::uint64_t> started_workers{0};
  std::atomic<std::uint64_t> collector_invocations{0};
  std::atomic<std::uint64_t> planner_invocations{0};
  std::atomic<std::uint64_t> cleanup_invocations{0};
  std::atomic<std::uint64_t> successful_cleanups{0};
  std::map<internal_api::EngineUuid, CanonicalPreparedLegPlanReceipt>
      completed_plans;
  std::vector<TaskResult> completed_tasks;
  completed_tasks.reserve(request.legs.size());

  std::mutex cancellation_mutex;
  std::atomic<bool> cancellation_observed{false};
  const auto external_cancelled = [&]() noexcept {
    if (cancellation_observed.load()) return true;
    if (!request.cancellation_requested) return false;
    try {
      // The caller need not make a stateful probe safe for concurrent workers.
      // Cancellation is terminal even if a later probe would return false.
      std::lock_guard lock(cancellation_mutex);
      if (!cancellation_observed.load() && request.cancellation_requested())
        cancellation_observed = true;
    } catch (...) {
      cancellation_observed = true;
    }
    return cancellation_observed.load();
  };
  const auto deadline_reached = [&]() {
    return MonotonicNowNs() >= deadline_ns;
  };

  const auto publish_worker_counters = [&]() noexcept {
    result.maximum_observed_concurrency = maximum_active_workers.load();
    result.metric_collector_invocation_count = collector_invocations.load();
    result.leg_planner_invocation_count = planner_invocations.load();
    result.cleanup_invocation_count = cleanup_invocations.load();
    result.transient_state_cleaned =
        result.cleanup_invocation_count == started_workers.load() &&
        successful_cleanups.load() == started_workers.load();
  };
  ScopeExit counter_guard{publish_worker_counters};
  bool orchestration_failed = false;
  result.all_workers_joined = true;
  std::string failure_field;
  std::string failure_detail;
  for (std::size_t wave_ordinal = 0;
       wave_ordinal < waves.size() && !orchestration_failed;
       ++wave_ordinal) {
    const auto& wave = waves[wave_ordinal];
    for (std::size_t offset = 0;
         offset < wave.size() && !orchestration_failed;) {
      if (external_cancelled() || deadline_reached()) {
        orchestration_failed = true;
        result.cancelled = external_cancelled();
        result.timed_out = !result.cancelled;
        failure_field = result.cancelled ? "prepare_metric_cancelled"
                                         : "prepare_metric_timeout";
        failure_detail = "PREPARE stopped before the next bounded worker batch";
        stop_source.request_stop();
        break;
      }
      const auto batch_size = std::min<std::size_t>(
          request.metric_thread_budget, wave.size() - offset);
      std::vector<TaskResult> batch(batch_size);
      const auto& dependency_plan_snapshot = completed_plans;
      std::vector<std::jthread> workers;
      workers.reserve(batch_size);
      bool spawn_failed = false;
      for (std::size_t batch_index = 0; batch_index < batch_size;
           ++batch_index) {
        const auto leg_index = wave[offset + batch_index];
        try {
          workers.emplace_back([&, batch_index, leg_index, wave_ordinal]() noexcept {
            auto& task = batch[batch_index];
            const auto& leg = request.legs[leg_index];
            task.started = true;
            ++started_workers;
            const auto active = active_workers.fetch_add(1) + 1;
            auto observed = maximum_active_workers.load();
            while (observed < active &&
                   !maximum_active_workers.compare_exchange_weak(observed,
                                                                 active)) {
            }
            const auto finish = [&]() noexcept {
              ++cleanup_invocations;
              try {
                task.cleanup_complete = leg.cleanup_transient_state();
              } catch (...) {
                task.cleanup_complete = false;
              }
              if (task.cleanup_complete) ++successful_cleanups;
              if (!task.cleanup_complete && task.field_id.empty()) {
                task.field_id = "prepare_metric_cleanup";
                stop_source.request_stop();
              }
              active_workers.fetch_sub(1);
            };
            ScopeExit cleanup_guard{finish};
            try {
              const auto cancel = [&]() {
                return stop_source.stop_requested() || external_cancelled() ||
                       deadline_reached();
              };
              task.metric.abi_version = 1;
              task.metric.dependency_wave =
                  static_cast<std::uint32_t>(wave_ordinal);
              task.metric.leg_uuid = leg.leg_uuid;
              task.metric.family_id = leg.family_id;
              task.metric.dependency_leg_uuids = leg.dependency_leg_uuids;
              task.metric.required_metric_ids = leg.required_metric_ids;
              task.metric.started_at_monotonic_ns = MonotonicNowNs();
              std::vector<CanonicalPreparedLegPlanReceipt> dependencies;
              for (const auto& dependency_uuid : leg.dependency_leg_uuids) {
                const auto dependency =
                    dependency_plan_snapshot.find(dependency_uuid);
                if (dependency != dependency_plan_snapshot.end()) {
                  dependencies.push_back(dependency->second);
                }
              }
              if (dependencies.size() != leg.dependency_leg_uuids.size()) {
                task.field_id = "prepare_metric_dependency_completion";
                task.detail = "a dependent leg started before its producer plan";
                stop_source.request_stop();
                return;
              }
              if (cancel()) {
                task.cancelled = external_cancelled() ||
                                 stop_source.stop_requested();
                task.timed_out = !task.cancelled && deadline_reached();
                task.field_id = task.timed_out ? "prepare_metric_timeout"
                                               : "prepare_metric_cancelled";
                task.detail = "leg cancelled before metric collection";
                return;
              }
              CanonicalPrepareMetricCollectionContext collection_context;
              collection_context.leg_uuid = leg.leg_uuid;
              collection_context.family_id = leg.family_id;
              collection_context.required_metric_ids = leg.required_metric_ids;
              collection_context.completed_dependency_plans = dependencies;
              collection_context.deadline_monotonic_ns = deadline_ns;
              collection_context.cancellation_requested = cancel;
              CanonicalPrepareMetricCollectionOutput collected;
              try {
                ++collector_invocations;
                collected = leg.collect_metrics(collection_context);
              } catch (const std::bad_alloc&) {
                throw;
              } catch (...) {
                task.field_id = "prepare_metric_collector_exception";
                task.detail = "metric collector threw an exception";
                stop_source.request_stop();
                return;
              }
              task.metric.metric_snapshot_uuid =
                  std::move(collected.metric_snapshot_uuid);
              task.metric.metric_snapshot_generation =
                  collected.metric_snapshot_generation;
              task.metric.metrics = std::move(collected.metrics);
              std::ranges::sort(task.metric.metrics, {},
                                &CanonicalPreparedMetricValue::metric_id);
              task.metric.collected = collected.collected;
              task.metric.parser_execution_authority_claimed =
                  collected.parser_execution_authority_claimed;
              task.metric.transaction_visibility_authority_claimed =
                  collected.transaction_visibility_authority_claimed;
              task.metric.transaction_finality_authority_claimed =
                  collected.transaction_finality_authority_claimed;
              task.metric.recovery_authority_claimed =
                  collected.recovery_authority_claimed;
              if (external_cancelled() || deadline_reached()) {
                task.cancelled = external_cancelled();
                task.timed_out = !task.cancelled;
                task.field_id = task.timed_out ? "prepare_metric_timeout"
                                               : "prepare_metric_cancelled";
                task.detail = "leg stopped during metric collection";
                stop_source.request_stop();
                return;
              }
              if (!collected.collected ||
                  collected.parser_execution_authority_claimed ||
                  collected.transaction_visibility_authority_claimed ||
                  collected.transaction_finality_authority_claimed ||
                  collected.recovery_authority_claimed) {
                task.field_id = "prepare_metric_collection";
                task.detail = collected.detail.empty()
                                  ? "metric collection refused or overclaimed authority"
                                  : std::move(collected.detail);
                stop_source.request_stop();
                return;
              }
              const auto metric_shape_valid =
                  CanonicalUuid(task.metric.metric_snapshot_uuid) &&
                  task.metric.metric_snapshot_generation != 0 &&
                  !task.metric.metrics.empty() &&
                  std::ranges::adjacent_find(
                      task.metric.metrics, {},
                      &CanonicalPreparedMetricValue::metric_id) ==
                      task.metric.metrics.end() &&
                  std::ranges::all_of(task.metric.metrics, [](const auto& value) {
                    return !value.metric_id.empty() && !value.unit_id.empty() &&
                           CanonicalUuid(value.source_snapshot_uuid) &&
                           value.source_generation != 0;
                  }) &&
                  std::ranges::all_of(
                      task.metric.required_metric_ids,
                      [&](const auto& required_metric_id) {
                        return std::ranges::any_of(
                            task.metric.metrics, [&](const auto& value) {
                              return value.metric_id == required_metric_id;
                            });
                      });
              if (!metric_shape_valid) {
                task.field_id = "prepare_metric_collection_shape";
                task.detail = "metric collector returned incomplete evidence";
                stop_source.request_stop();
                return;
              }
              const auto metric_seed = MetricReceiptSeed(request, task.metric);
              const auto collection_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
              if (!collection_identity) {
                task.field_id = "prepare_metric_receipt_identity";
                stop_source.request_stop();
                return;
              }
              task.metric.collection_receipt_uuid = *collection_identity;
              task.metric.dependency_definition_digest =
                  DerivedDigest(metric_seed);
              if (task.metric.dependency_definition_digest.empty()) {
                task.field_id = "prepare_metric_dependency_digest";
                task.detail = "metric snapshot dependency SHA-256 failed";
                stop_source.request_stop();
                return;
              }
              task.metric.completed_at_monotonic_ns = MonotonicNowNs();
              if (cancel()) {
                task.cancelled = external_cancelled() ||
                                 stop_source.stop_requested();
                task.timed_out = !task.cancelled && deadline_reached();
                task.field_id = task.timed_out ? "prepare_metric_timeout"
                                               : "prepare_metric_cancelled";
                task.detail = "leg stopped after metric collection";
                stop_source.request_stop();
                return;
              }
              CanonicalPrepareLegPlanningContext planning_context;
              planning_context.metric_receipt = task.metric;
              planning_context.completed_dependency_plans = dependencies;
              planning_context.deadline_monotonic_ns = deadline_ns;
              planning_context.cancellation_requested = cancel;
              CanonicalPrepareLegPlanningOutput planned;
              try {
                ++planner_invocations;
                planned = leg.plan_leg(planning_context);
              } catch (const std::bad_alloc&) {
                throw;
              } catch (...) {
                task.field_id = "prepare_leg_planner_exception";
                task.detail = "family-local planner threw an exception";
                stop_source.request_stop();
                return;
              }
              task.plan.abi_version = 1;
              task.plan.leg_uuid = leg.leg_uuid;
              task.plan.family_id = leg.family_id;
              task.plan.dependency_leg_uuids = leg.dependency_leg_uuids;
              task.plan.metric_collection_receipt_uuid =
                  task.metric.collection_receipt_uuid;
              task.plan.selected_leg_plan_uuid =
                  std::move(planned.selected_leg_plan_uuid);
              task.plan.selected_alternative_uuid =
                  std::move(planned.selected_alternative_uuid);
              task.plan.family_local_cost_vector_uuid =
                  std::move(planned.family_local_cost_vector_uuid);
              task.plan.retained_alternative_uuids =
                  std::move(planned.retained_alternative_uuids);
              std::ranges::sort(task.plan.retained_alternative_uuids);
              task.plan.estimated_output_rows = planned.estimated_output_rows;
              task.plan.planned = planned.planned;
              task.plan.family_local_selection = planned.family_local_selection;
              task.plan.cross_family_cost_comparison_performed =
                  planned.cross_family_cost_comparison_performed;
              task.plan.parser_execution_authority_claimed =
                  planned.parser_execution_authority_claimed;
              task.plan.transaction_visibility_authority_claimed =
                  planned.transaction_visibility_authority_claimed;
              task.plan.transaction_finality_authority_claimed =
                  planned.transaction_finality_authority_claimed;
              task.plan.recovery_authority_claimed =
                  planned.recovery_authority_claimed;
              if (external_cancelled() || deadline_reached()) {
                task.cancelled = external_cancelled();
                task.timed_out = !task.cancelled;
                task.field_id = task.timed_out ? "prepare_metric_timeout"
                                               : "prepare_metric_cancelled";
                task.detail = "leg stopped during family-local planning";
                stop_source.request_stop();
                return;
              }
              if (!planned.planned || !planned.family_local_selection ||
                  planned.cross_family_cost_comparison_performed ||
                  planned.parser_execution_authority_claimed ||
                  planned.transaction_visibility_authority_claimed ||
                  planned.transaction_finality_authority_claimed ||
                  planned.recovery_authority_claimed) {
                task.field_id = "prepare_leg_planning";
                task.detail = planned.detail.empty()
                                  ? "family-local planning refused or crossed authority"
                                  : std::move(planned.detail);
                stop_source.request_stop();
                return;
              }
              const auto plan_shape_valid =
                  CanonicalUuid(task.plan.selected_leg_plan_uuid) &&
                  CanonicalUuid(task.plan.selected_alternative_uuid) &&
                  CanonicalUuid(task.plan.family_local_cost_vector_uuid) &&
                  !task.plan.retained_alternative_uuids.empty() &&
                  std::ranges::adjacent_find(
                      task.plan.retained_alternative_uuids) ==
                      task.plan.retained_alternative_uuids.end() &&
                  std::ranges::all_of(
                      task.plan.retained_alternative_uuids,
                      [](const auto& alternative_uuid) {
                        return CanonicalUuid(alternative_uuid);
                      }) &&
                  std::ranges::find(task.plan.retained_alternative_uuids,
                                    task.plan.selected_alternative_uuid) !=
                      task.plan.retained_alternative_uuids.end();
              if (!plan_shape_valid) {
                task.field_id = "prepare_leg_plan_shape";
                task.detail = "family-local planner returned incomplete evidence";
                stop_source.request_stop();
                return;
              }
              const auto planning_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
              if (!planning_identity) {
                task.field_id = "prepare_leg_receipt_identity";
                stop_source.request_stop();
                return;
              }
              task.plan.planning_receipt_uuid = *planning_identity;
              task.succeeded = !deadline_reached();
              if (!task.succeeded) {
                task.timed_out = true;
                task.field_id = "prepare_metric_timeout";
                task.detail = "leg exceeded the PREPARE metric deadline";
                stop_source.request_stop();
              }
            } catch (const std::bad_alloc&) {
              task.succeeded = false;
              task.resource_exhausted = true;
              task.field_id = "prepare_metric_resource_exhausted";
              task.detail.clear();
              stop_source.request_stop();
            } catch (...) {
              task.succeeded = false;
              task.field_id = "prepare_metric_worker_exception";
              task.detail.clear();
              stop_source.request_stop();
            }
          });
        } catch (...) {
          spawn_failed = true;
          stop_source.request_stop();
          failure_field = "prepare_metric_worker_spawn";
          failure_detail = "bounded metric worker could not be created";
          break;
        }
      }
      for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
      }
      result.all_workers_joined = true;
      if (spawn_failed) {
        orchestration_failed = true;
      }
      for (std::size_t index = 0; index < batch.size(); ++index) {
        auto& task = batch[index];
        if (!task.started && spawn_failed) continue;
        if (!task.succeeded || !task.cleanup_complete) {
          orchestration_failed = true;
          if (task.resource_exhausted) {
            result.status = CanonicalPrepareMetricCollectionStatus::kResourceExhausted;
          }
          if (failure_field.empty()) {
            failure_field = task.field_id.empty() ? "prepare_metric_leg"
                                                  : task.field_id;
            failure_detail = task.detail;
          }
          result.cancelled = result.cancelled || task.cancelled;
          result.timed_out = result.timed_out || task.timed_out;
        }
      }
      if (orchestration_failed) break;
      std::ranges::sort(batch, {},
                        [](const auto& task) { return task.metric.leg_uuid; });
      for (auto& task : batch) {
        completed_plans.emplace(task.plan.leg_uuid, task.plan);
        completed_tasks.push_back(std::move(task));
      }
      offset += batch_size;
    }
  }

  publish_worker_counters();
  if (orchestration_failed) {
    return refuse(failure_field.empty() ? "prepare_metric_orchestration"
                                        : failure_field,
                  failure_detail.empty() ? "bounded PREPARE orchestration failed"
                                         : failure_detail);
  }

  std::uint32_t stable_ordinal = 0;
  for (auto& task : completed_tasks) {
    task.metric.stable_leg_ordinal = ++stable_ordinal;
    task.metric.cleanup_complete = true;
    task.plan.stable_leg_ordinal = stable_ordinal;
    result.metric_receipts.push_back(std::move(task.metric));
    result.leg_plan_receipts.push_back(std::move(task.plan));
  }

  CanonicalPreparedMetricCoordinatorReceipt coordinator;
  coordinator.coordinator_policy_uuid = request.coordinator_policy_uuid;
  coordinator.coordinator_policy_generation =
      request.coordinator_policy_generation;
  coordinator.bound_sblr_tree_uuid = request.bound_sblr_tree_uuid;
  coordinator.route_snapshot_uuid = request.route_snapshot_uuid;
  coordinator.route_epoch = request.route_epoch;
  coordinator.route_generation = request.route_generation;
  coordinator.cluster_scope_id = request.cluster_scope_id;
  coordinator.metric_thread_budget = request.metric_thread_budget;
  coordinator.maximum_observed_concurrency =
      result.maximum_observed_concurrency;
  coordinator.timeout_ns = request.timeout_ns;
  const auto chain_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!chain_identity) {
    return refuse("prepare_metric_receipt_identity", "coordinator identity issuance failed");
  }
  coordinator.dependency_chain_receipt_uuid = *chain_identity;
  coordinator.all_workers_joined = result.all_workers_joined;
  coordinator.transient_state_cleaned = result.transient_state_cleaned;
  coordinator.dependency_chain_acyclic = true;

  CanonicalPreparePhysicalPlanRequest prepare_request;
  try {
    prepare_request = request.assemble_selected_plan(
        coordinator, result.metric_receipts, result.leg_plan_receipts);
  } catch (...) {
    return refuse("prepare_metric_plan_assembly",
                  "selected-plan assembler threw an exception");
  }
  prepare_request.publication_cancelled = [&] {
    // These flags are set at the final store boundary, not inferred from a
    // callback refusal or a later observation of the clock.
    result.cancelled = external_cancelled();
    result.timed_out = deadline_reached();
    return result.cancelled || result.timed_out;
  };
  prepare_request.prepare_metric_coordinator_receipt = coordinator;
  prepare_request.prepare_metric_collection_receipts = result.metric_receipts;
  prepare_request.prepare_leg_plan_receipts = result.leg_plan_receipts;
  for (const auto& metric : result.metric_receipts) {
    prepare_request.dependencies.push_back(
        {CanonicalPreparedPlanDependencyKind::kMetricSnapshot,
         metric.metric_snapshot_uuid, metric.metric_snapshot_generation,
         metric.dependency_definition_digest});
  }
  std::ranges::sort(prepare_request.dependencies, [](const auto& left,
                                                      const auto& right) {
    if (left.dependency_kind != right.dependency_kind) {
      return left.dependency_kind < right.dependency_kind;
    }
    return left.dependency_uuid < right.dependency_uuid;
  });
  if (external_cancelled() || deadline_reached()) {
    result.timed_out = deadline_reached();
    result.cancelled = !result.timed_out;
    return refuse("prepare_metric_publication_cancelled",
                  "PREPARE stopped before selected-plan publication");
  }
  result.prepare_result =
      PrepareCanonicalPhysicalPlan(prepare_request, prepared_plan_store);
  if (!result.prepare_result.accepted || !result.prepare_result.prepared ||
      !result.prepare_result.persisted ||
      !result.prepare_result.prepare_metric_receipts_retained) {
    const auto field = result.prepare_result.issues.empty()
                           ? "prepare_metric_plan_persistence"
                           : result.prepare_result.issues.front().field_id;
    return refuse(field, "metric-planned physical plan was not persisted");
  }
  result.status = CanonicalPrepareMetricCollectionStatus::kPrepared;
  result.accepted = true;
  result.metrics_collected = true;
  result.legs_planned = true;
  result.prepared = true;
  result.persisted = true;
  result.issues.clear();
  return std::move(result);
}

CanonicalPrepareWithMetricCollectionResult
PrepareCanonicalPhysicalPlanWithMetricCollection(
    const CanonicalPrepareWithMetricCollectionRequest& request,
    CanonicalPreparedPlanStore* prepared_plan_store) {
  static_assert(std::is_nothrow_move_constructible_v<CanonicalPrepareWithMetricCollectionResult>);
  static_assert(std::is_nothrow_move_assignable_v<CanonicalPreparePhysicalPlanResult>);
  CanonicalPrepareWithMetricCollectionResult result;
  try {
    return PrepareMetricCollectionImpl(request, prepared_plan_store, result);
  } catch (const std::bad_alloc&) {
    result.status = CanonicalPrepareMetricCollectionStatus::kResourceExhausted;
  } catch (...) {
    result.status = CanonicalPrepareMetricCollectionStatus::kInternalFailure;
  }
  // All workers have joined on unwind; refusal must never retain staged success.
  // This fallback deliberately needs no allocation, including diagnostic text.
  result.accepted = result.metrics_collected = result.legs_planned = false;
  result.prepared = result.persisted = false;
  result.prepare_result = {};
  std::vector<CanonicalPreparedMetricCollectionReceipt>{}.swap(result.metric_receipts);
  std::vector<CanonicalPreparedLegPlanReceipt>{}.swap(result.leg_plan_receipts);
  result.issues.clear();
  return result;
}

}  // namespace scratchbird::engine::optimizer
