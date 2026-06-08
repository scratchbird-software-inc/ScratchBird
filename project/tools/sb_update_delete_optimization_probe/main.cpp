// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/delete_batch.hpp"
#include "dml/update_batch.hpp"
#include "metric_registry.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

struct ExpectedMetric {
  const char* family;
  scratchbird::core::metrics::MetricType type;
  scratchbird::core::metrics::MetricUnit unit;
  const char* namespace_path;
  const char* producer_owner;
};

bool HasEvidence(const scratchbird::engine::internal_api::EngineApiResult& result,
                 const std::string& kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  using namespace scratchbird::core::metrics;
  using namespace scratchbird::engine::internal_api;

  const std::vector<ExpectedMetric> expected = {
      {"sb_dml_update_batch_started_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.update", "engine_update"},
      {"sb_dml_update_batch_fallback_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.update", "engine_update"},
      {"sb_dml_update_rows_updated_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.update", "engine_update"},
      {"sb_dml_update_cancel_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.update", "engine_update"},
      {"sb_dml_update_trace_event_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.update", "engine_update"},
      {"sb_dml_delete_batch_started_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.delete", "engine_delete"},
      {"sb_dml_delete_batch_fallback_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.delete", "engine_delete"},
      {"sb_dml_delete_rows_deleted_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.delete", "engine_delete"},
      {"sb_dml_delete_cancel_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.delete", "engine_delete"},
      {"sb_dml_delete_trace_event_total", MetricType::counter, MetricUnit::count, "sys.metrics.dml.delete", "engine_delete"},
  };

  bool ok = true;
  auto& registry = DefaultMetricRegistry();
  for (const auto& metric : expected) {
    const auto* descriptor = registry.FindDescriptorOrAlias(metric.family);
    ok &= Require(descriptor != nullptr, std::string("missing update/delete metric descriptor ") + metric.family);
    if (descriptor == nullptr) {
      continue;
    }
    ok &= Require(descriptor->type == metric.type, std::string("wrong metric type ") + metric.family);
    ok &= Require(descriptor->unit == metric.unit, std::string("wrong metric unit ") + metric.family);
    ok &= Require(descriptor->namespace_path == metric.namespace_path, std::string("wrong namespace ") + metric.family);
    ok &= Require(descriptor->producer_owner == metric.producer_owner, std::string("wrong producer ") + metric.family);
    ok &= Require(!descriptor->cluster_only, std::string("update/delete metric must be local-visible ") + metric.family);
    ok &= Require(!descriptor->help.empty(), std::string("missing help ") + metric.family);
  }

  UpdateBatchContext update_context;
  update_context.statement_uuid = "update-statement";
  update_context.target_object_uuid = "table-update";
  update_context.update_mode = UpdateBatchMode::predicate_scan;
  update_context.evidence.push_back({"update_batch_context", update_context.statement_uuid});
  AddUpdateTrace(&update_context, "update.batch.begin", "begin", "predicate_scan");
  EngineApiResult update_result;
  AddUpdateBatchEvidenceToResult(update_context, &update_result);
  ok &= Require(HasEvidence(update_result, "update_batch_context"), "update batch evidence is attached");
  ok &= Require(HasEvidence(update_result, "update_trace"), "update trace evidence is attached");
  RecordUpdateBatchMetric(update_context, "sb_dml_update_batch_started_total", 1.0, "ok");
  RecordUpdateBatchMetric(update_context, "sb_dml_update_rows_updated_total", 2.0, "ok");

  DeleteBatchContext delete_context;
  delete_context.statement_uuid = "delete-statement";
  delete_context.target_object_uuid = "table-delete";
  delete_context.delete_mode = DeleteBatchMode::predicate_scan;
  delete_context.tombstone_only = true;
  delete_context.evidence.push_back({"delete_batch_context", delete_context.statement_uuid});
  AddDeleteTrace(&delete_context, "delete.batch.begin", "begin", "predicate_scan");
  EngineApiResult delete_result;
  AddDeleteBatchEvidenceToResult(delete_context, &delete_result);
  ok &= Require(HasEvidence(delete_result, "delete_batch_context"), "delete batch evidence is attached");
  ok &= Require(HasEvidence(delete_result, "delete_trace"), "delete trace evidence is attached");
  ok &= Require(HasEvidence(delete_result, "delete_tombstone_only"), "delete tombstone evidence is attached");
  RecordDeleteBatchMetric(delete_context, "sb_dml_delete_batch_started_total", 1.0, "ok");
  RecordDeleteBatchMetric(delete_context, "sb_dml_delete_rows_deleted_total", 2.0, "ok");

  ok &= Require(registry.IncrementCounter("sb_dml_update_batch_started_total", {}, 1.0, "engine_update").ok,
                "update batch counter accepts engine_update producer");
  ok &= Require(registry.IncrementCounter("sb_dml_delete_batch_started_total", {}, 1.0, "engine_delete").ok,
                "delete batch counter accepts engine_delete producer");
  ok &= Require(!registry.SetGauge("sb_dml_update_rows_updated_total", {}, 1.0, "engine_update").ok,
                "update row counter rejects gauge update");
  ok &= Require(!registry.IncrementCounter("sb_dml_delete_rows_deleted_total", {}, 1.0, "engine_update").ok,
                "delete row counter rejects wrong producer");

  return ok ? 0 : 1;
}
