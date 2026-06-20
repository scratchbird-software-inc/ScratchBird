// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_optimizer_support_services.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

std::int64_t ClampNonNegative(std::int64_t value) {
  return value < 0 ? 0 : value;
}

}  // namespace

IparBulkRoutePlan SelectIparBulkRoute(const IparBulkRouteRequest& request) {
  IparBulkRoutePlan plan;
  Add(&plan.evidence, "IPAR-P4-11");
  if (request.operation_uuid.empty() || request.descriptor_digest.empty() ||
      request.transaction_profile.empty() || request.row_count == 0 ||
      request.average_row_bytes == 0 || !request.source_snapshot_stable) {
    plan.diagnostic_code = "IPAR_BULK_ROUTE_REQUEST_UNSAFE";
    return plan;
  }
  plan.accepted = true;
  plan.selected_once = true;
  if (request.shadow_index_requested) {
    plan.route = IparBulkRouteKind::shadow_index_build;
    plan.batch_rows = std::max<std::uint64_t>(128, request.row_count / 8);
  } else if (request.copy_import) {
    plan.route = IparBulkRouteKind::copy_import_pipeline;
    plan.batch_rows = 1024;
  } else if (request.insert_select) {
    plan.route = IparBulkRouteKind::insert_select_stream;
    plan.batch_rows = 512;
  } else if (request.keyed_merge) {
    plan.route = IparBulkRouteKind::merge_keyed_upsert;
    plan.batch_rows = 256;
  } else if (request.row_count > 1) {
    plan.route = IparBulkRouteKind::multi_row_batch;
    plan.batch_rows = std::min<std::uint64_t>(512, request.row_count);
  }
  plan.diagnostic_code = "IPAR_BULK_ROUTE_READY";
  Add(&plan.evidence, "bulk_route_selected_once=true");
  Add(&plan.evidence, "bulk_route_mga_profile_preserved=true");
  return plan;
}

bool IparColumnStatsDeltaAccumulator::AddDelta(
    const IparColumnStatsDelta& delta) {
  if (delta.table_uuid.empty() || delta.column_uuid.empty()) {
    return false;
  }
  deltas_.push_back(delta);
  return true;
}

IparColumnStatsSnapshot IparColumnStatsDeltaAccumulator::Merge(
    const IparColumnStatsSnapshot& base,
    std::uint64_t publish_epoch) {
  IparColumnStatsSnapshot out = base;
  std::vector<IparColumnStatsDelta> remaining;
  for (const auto& delta : deltas_) {
    if (delta.table_uuid != base.table_uuid ||
        delta.column_uuid != base.column_uuid) {
      remaining.push_back(delta);
      continue;
    }
    out.row_count = ClampNonNegative(out.row_count + delta.row_count_delta);
    out.null_count = ClampNonNegative(out.null_count + delta.null_count_delta);
    out.distinct_hint =
        ClampNonNegative(out.distinct_hint + delta.distinct_hint_delta);
    if (!delta.min_value.empty() &&
        (out.min_value.empty() || delta.min_value < out.min_value)) {
      out.min_value = delta.min_value;
    }
    if (!delta.max_value.empty() &&
        (out.max_value.empty() || delta.max_value > out.max_value)) {
      out.max_value = delta.max_value;
    }
  }
  out.stats_epoch = publish_epoch;
  deltas_ = std::move(remaining);
  return out;
}

IparRuntimeCostFeedback IparRuntimeCostFeedbackAgent::Record(
    const IparRuntimeCostObservation& observation) {
  IparRuntimeCostFeedback feedback;
  Add(&feedback.evidence, "IPAR-P4-19");
  if (observation.route_id.empty() || observation.rows == 0 ||
      !observation.slow_path_explained || !observation.advisory_only) {
    feedback.diagnostic_code = "IPAR_RUNTIME_COST_FEEDBACK_UNSAFE";
    return feedback;
  }
  feedback.accepted = true;
  feedback.route_id = observation.route_id;
  const auto per_row =
      std::max<std::uint64_t>(1, observation.actual_micros / observation.rows);
  feedback.adjusted_cost =
      (observation.estimated_cost + per_row * observation.rows) / 2;
  feedback.diagnostic_code = "IPAR_RUNTIME_COST_FEEDBACK_ACCEPTED";
  Add(&feedback.evidence, "runtime_feedback_advisory_only=true");
  Add(&feedback.evidence, "slow_path_diagnostics_preserved=true");
  feedback_[feedback.route_id] = feedback;
  return feedback;
}

IparRuntimeCostFeedback IparRuntimeCostFeedbackAgent::Lookup(
    const std::string& route_id) const {
  const auto found = feedback_.find(route_id);
  if (found == feedback_.end()) {
    IparRuntimeCostFeedback miss;
    miss.diagnostic_code = "IPAR_RUNTIME_COST_FEEDBACK_MISS";
    return miss;
  }
  return found->second;
}

}  // namespace scratchbird::engine::optimizer
