// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_QRY_011_REGISTRY_FIXTURE_ONLY
#include "qow_qry_011_registry.cpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

exec::CanonicalAggregateRuntimeRequest SpillProfile(
    const exec::CanonicalAggregateFunction function) {
  using Function = exec::CanonicalAggregateFunction;
  switch (function) {
    case Function::count:
      return Request(function, 0, 0, "int64", true);
    case Function::sum:
      return Request(function, 0, 2101, "int64");
    case Function::avg:
      return Request(function, 1, 2102, "real64");
    case Function::min:
    case Function::max:
      return Request(function, 3, 2104, "int64");
    case Function::bool_and:
    case Function::bool_or:
    case Function::every:
      return Request(function, 2, 2103, "boolean");
    case Function::array_agg:
    case Function::string_agg:
    case Function::json_agg:
    case Function::json_object_agg:
    case Function::listagg:
      return CollectionRequest(function);
    case Function::stddev_pop:
    case Function::variance_pop:
    case Function::stddev:
    case Function::variance:
    case Function::stddev_samp:
    case Function::variance_samp:
      return StatisticalRequest(function);
    case Function::corr:
    case Function::covar_pop:
    case Function::covar_samp:
    case Function::regr_count:
    case Function::regr_avgx:
    case Function::regr_avgy:
    case Function::regr_intercept:
    case Function::regr_r2:
    case Function::regr_slope:
    case Function::regr_sxx:
    case Function::regr_sxy:
    case Function::regr_syy:
      return PairStatisticalRequest(function);
    case Function::rank:
    case Function::dense_rank:
    case Function::percent_rank:
    case Function::cume_dist:
      return HypotheticalRequest(function);
    case Function::mode: {
      auto request = OrderedNumericRequest(function, "int64");
      const auto descriptor = request.input_batch.columns[0].descriptor;
      request.input_batch.rows[0].values[0] = Value(descriptor, "5");
      request.input_batch.rows[1].values[0] = Value(descriptor, "4");
      request.input_batch.rows[2].values[0] = Value(descriptor, "5");
      request.input_batch.rows[3].values[0] = Value(descriptor, "4");
      return request;
    }
    case Function::percentile_cont:
    case Function::percentile_disc:
    case Function::approx_percentile_cont:
    case Function::approx_percentile_disc: {
      auto request = OrderedNumericRequest(function, "real64");
      const auto real_descriptor = request.input_batch.columns[1].descriptor;
      request.direct_arguments = {Value(real_descriptor, "0.5")};
      return request;
    }
    case Function::approx_count_distinct: {
      auto request = Request(function, 0, 2101, "int64");
      request.result_column.nullable = false;
      return request;
    }
    case Function::approx_median: {
      auto request = OrderedNumericRequest(function, "real64");
      request.aggregate_order_terms.clear();
      return request;
    }
    case Function::approx_top_k:
      return TopKRequest();
    case Function::unknown:
      break;
  }
  std::abort();
}

std::string SpillOwnerUuid(const std::size_t ordinal) {
  std::ostringstream stream;
  stream << "019f0000-0000-7200-8000-" << std::setw(12)
         << std::setfill('0') << 3100 + ordinal;
  return stream.str();
}

exec::CanonicalAggregateStateSpillRequest StateSpillRequest(
    const exec::CanonicalAggregateFunction function,
    const std::size_t ordinal,
    const std::filesystem::path& root) {
  exec::CanonicalAggregateStateSpillRequest request;
  request.aggregate_request = SpillProfile(function);
  request.aggregate_request.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
  request.aggregate_request.physical_dag.spill_allowed = true;
  request.aggregate_request.physical_dag.nodes.back().implementation_id =
      "aggregate.registry-state-spill.v1";
  request.spill_root = root;
  request.spill_owner_uuid = SpillOwnerUuid(ordinal);
  request.runtime_generation = 3110 + ordinal;
  request.memory_quota_bytes = 128;
  return request;
}

bool HasStateSpillArtifact(const std::filesystem::path& root,
                           const std::string& owner_uuid) {
  const auto directory = root / owner_uuid;
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return false;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto filename = iterator->path().filename().string();
    if (filename.rfind("orh283_temp_spill-", 0) == 0 &&
        iterator->path().extension() == ".sbtmpidx") {
      return true;
    }
  }
  return static_cast<bool>(error);
}

bool HasStateSpillEvidence(const std::vector<std::string>& evidence,
                           const std::string_view expected) {
  return std::find(evidence.begin(), evidence.end(), expected) !=
         evidence.end();
}

bool ValidateAllRegistryStateSpillProfiles(
    const std::filesystem::path& root) {
  bool passed = true;
  const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  std::size_t admitted = 0;
  for (std::size_t ordinal = 0; ordinal < registry.size(); ++ordinal) {
    auto request = StateSpillRequest(registry[ordinal].function, ordinal, root);
    std::error_code error;
    std::filesystem::create_directories(
        root / request.spill_owner_uuid, error);
    const auto baseline =
        exec::ExecuteCanonicalAggregateRuntime(request.aggregate_request);
    const auto result = exec::ExecuteCanonicalAggregateStateSpill(request);
    const bool accepted =
        result.diagnostic.ok && result.state_serialized && result.spilled &&
        result.spill_reopened && result.state_restored &&
        result.restored_result_equivalent && result.cleanup_proven &&
        result.serialized_state_bytes != 0 &&
        result.spilled_state_record_count != 0 &&
        SameScalar(baseline, result.aggregate_result) &&
        result.aggregate_result.descriptor.function ==
            registry[ordinal].function &&
        result.aggregate_result.authority.engine_mga_snapshot_bound &&
        !result.aggregate_result.authority.owns_transaction_finality &&
        HasStateSpillEvidence(
            result.spill_evidence,
            "temporary_work.spill_payload_checksum=validated") &&
        HasStateSpillEvidence(
            result.spill_evidence,
            "orh283.temp_metadata.finality_authority=false") &&
        !HasStateSpillArtifact(root, request.spill_owner_uuid);
    passed &= Require(accepted,
                      "aggregate registry state did not spill/restore exactly");
    if (accepted) ++admitted;
  }
  passed &= Require(admitted == 43,
                    "not every aggregate registry state profile was restored");

  auto modifiers = StateSpillRequest(
      exec::CanonicalAggregateFunction::sum, 44, root);
  std::filesystem::create_directories(root / modifiers.spill_owner_uuid);
  modifiers.aggregate_request.distinct = true;
  modifiers.aggregate_request.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>{
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::unknown};
  auto modified = exec::ExecuteCanonicalAggregateStateSpill(modifiers);
  passed &= Require(
      modified.diagnostic.ok && modified.restored_result_equivalent &&
          modified.cleanup_proven &&
          modified.aggregate_result.filter_applied_before_distinct &&
          modified.aggregate_result.distinct_tuple_count == 2 &&
          modified.aggregate_result.transition_count == 2 &&
          modified.aggregate_result.output_batch.rows[0].values[0]
                  .encoded_value == "3" &&
          !HasStateSpillArtifact(root, modifiers.spill_owner_uuid),
      "FILTER/DISTINCT state spill diverged from shared transition preparation");

  auto empty = StateSpillRequest(
      exec::CanonicalAggregateFunction::sum, 45, root);
  std::filesystem::create_directories(root / empty.spill_owner_uuid);
  empty.aggregate_request.input_batch.rows.clear();
  empty.aggregate_request.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::serial;
  auto empty_result = exec::ExecuteCanonicalAggregateStateSpill(empty);
  passed &= Require(
      empty_result.diagnostic.ok && empty_result.state_restored &&
          empty_result.cleanup_proven &&
          empty_result.aggregate_result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null &&
          !HasStateSpillArtifact(root, empty.spill_owner_uuid),
      "empty serial SUM state did not spill/restore typed NULL finalization");
  return passed;
}

bool ValidateRegistryStateSpillRefusals(const std::filesystem::path& root) {
  bool passed = true;
  const auto owner = SpillOwnerUuid(50);
  std::error_code error;
  std::filesystem::create_directories(root / owner, error);
  auto request = StateSpillRequest(
      exec::CanonicalAggregateFunction::sum, 50, root);

  request.cancellation_requested = true;
  auto result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cancellation_observed &&
                        result.cleanup_proven &&
                        result.aggregate_result.output_batch.rows.empty() &&
                        !HasStateSpillArtifact(root, owner),
                    "cancelled aggregate state spill survived or published");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.reopen_runtime_generation = request.runtime_generation + 1;
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cleanup_proven &&
                        !HasStateSpillArtifact(root, owner),
                    "stale aggregate state spill generation survived cleanup");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.restart_recovery_proof_available = false;
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cleanup_proven &&
                        !HasStateSpillArtifact(root, owner),
                    "aggregate state reopened without exact recovery proof");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.memory_quota_bytes = 1048576;
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && result.cleanup_proven &&
                        !HasStateSpillArtifact(root, owner),
                    "non-spilled aggregate state published benchmark output");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.maximum_serialized_state_bytes = 1;
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && !result.spilled &&
                        !HasStateSpillArtifact(root, owner),
                    "aggregate state exceeded its serialization bound");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.maximum_spill_record_count = 1;
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && !result.spilled &&
                        !HasStateSpillArtifact(root, owner),
                    "aggregate state exceeded its spill-record bound");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.aggregate_request.physical_dag.spill_allowed = false;
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && !result.spilled,
                    "aggregate state spilled without optimizer permission");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.aggregate_request.physical_dag.nodes.back().implementation_id =
      "aggregate.registry-core.v1";
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && !result.spilled,
                    "aggregate state spill payload overrode the selected plan");

  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  request.aggregate_request.transaction_finality_claimed = true;
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok && !result.spilled,
                    "aggregate state spill claimed transaction finality");

  const auto collision = root / owner /
                         "orh283_temp_spill-preexisting.sbtmpidx";
  {
    std::ofstream output(collision);
    output << "foreign";
  }
  request = StateSpillRequest(exec::CanonicalAggregateFunction::sum, 50, root);
  result = exec::ExecuteCanonicalAggregateStateSpill(request);
  passed &= Require(!result.diagnostic.ok &&
                        std::filesystem::exists(collision),
                    "aggregate state spill overwrote an owner collision");
  std::filesystem::remove(collision, error);
  return passed;
}

}  // namespace

#ifndef QOW_QRY_011_STATE_SPILL_FIXTURE_ONLY
int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("scratchbird_qow205_registry_state_spill_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  const bool passed = ValidateAllRegistryStateSpillProfiles(root) &&
                      ValidateRegistryStateSpillRefusals(root);
  std::filesystem::remove_all(root, error);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
