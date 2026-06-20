// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

enum class IparBulkRouteKind {
  single_row_prepared,
  multi_row_batch,
  insert_select_stream,
  copy_import_pipeline,
  merge_keyed_upsert,
  shadow_index_build
};

struct IparBulkRouteRequest {
  std::string operation_uuid;
  std::string descriptor_digest;
  std::string transaction_profile;
  std::uint64_t row_count = 0;
  std::uint64_t average_row_bytes = 0;
  bool source_snapshot_stable = true;
  bool keyed_merge = false;
  bool copy_import = false;
  bool insert_select = false;
  bool shadow_index_requested = false;
};

struct IparBulkRoutePlan {
  bool accepted = false;
  IparBulkRouteKind route = IparBulkRouteKind::single_row_prepared;
  std::uint64_t batch_rows = 1;
  bool selected_once = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

IparBulkRoutePlan SelectIparBulkRoute(const IparBulkRouteRequest& request);

struct IparColumnStatsDelta {
  std::string table_uuid;
  std::string column_uuid;
  std::int64_t row_count_delta = 0;
  std::int64_t null_count_delta = 0;
  std::int64_t distinct_hint_delta = 0;
  std::string min_value;
  std::string max_value;
};

struct IparColumnStatsSnapshot {
  std::string table_uuid;
  std::string column_uuid;
  std::int64_t row_count = 0;
  std::int64_t null_count = 0;
  std::int64_t distinct_hint = 0;
  std::string min_value;
  std::string max_value;
  std::uint64_t stats_epoch = 0;
};

class IparColumnStatsDeltaAccumulator {
 public:
  bool AddDelta(const IparColumnStatsDelta& delta);
  IparColumnStatsSnapshot Merge(
      const IparColumnStatsSnapshot& base,
      std::uint64_t publish_epoch);
  std::size_t pending_count() const { return deltas_.size(); }

 private:
  std::vector<IparColumnStatsDelta> deltas_;
};

struct IparRuntimeCostObservation {
  std::string route_id;
  std::uint64_t estimated_cost = 0;
  std::uint64_t actual_micros = 0;
  std::uint64_t rows = 0;
  bool slow_path_explained = true;
  bool advisory_only = true;
};

struct IparRuntimeCostFeedback {
  bool accepted = false;
  std::string route_id;
  std::uint64_t adjusted_cost = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

class IparRuntimeCostFeedbackAgent {
 public:
  IparRuntimeCostFeedback Record(const IparRuntimeCostObservation& observation);
  IparRuntimeCostFeedback Lookup(const std::string& route_id) const;

 private:
  std::map<std::string, IparRuntimeCostFeedback> feedback_;
};

}  // namespace scratchbird::engine::optimizer
