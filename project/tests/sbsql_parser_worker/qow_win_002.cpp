// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_012_STATE_FIXTURE_ONLY
#include "qow_win_012_state.cpp"

#include <set>

namespace {

exec::CanonicalWindowRuntimeDescriptor RuntimeDescriptor(
    const exec::CanonicalWindowRuntimeFunction function) {
  const auto registry = exec::CanonicalWindowRuntimeRegistryV1();
  const auto found = std::ranges::find_if(
      registry, [&](const auto& row) { return row.function == function; });
  return found == registry.end()
             ? exec::CanonicalWindowRuntimeDescriptor{}
             : *found;
}

bool SameValue(const api::EngineTypedValue& left,
               const api::EngineTypedValue& right) {
  return left.descriptor.descriptor_uuid.canonical ==
             right.descriptor.descriptor_uuid.canonical &&
         left.descriptor.descriptor_kind == right.descriptor.descriptor_kind &&
         left.descriptor.canonical_type_name ==
             right.descriptor.canonical_type_name &&
         left.descriptor.encoded_descriptor ==
             right.descriptor.encoded_descriptor &&
         left.encoded_value == right.encoded_value &&
         left.binary_value == right.binary_value &&
         left.is_null == right.is_null && left.state == right.state;
}

bool SameValues(const std::vector<api::EngineTypedValue>& left,
                const std::vector<api::EngineTypedValue>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!SameValue(left[index], right[index])) return false;
  }
  return true;
}

exec::CanonicalWindowRankingRequest RuntimeRankingRequest(
    const exec::CanonicalWindowRankingFunction function,
    const exec::CanonicalWindowRuntimeFunction runtime_function) {
  exec::CanonicalWindowRankingRequest request;
  request.frames = ValueFrames(WholePartitionFrame());
  request.function = function;
  request.function_uuid = RuntimeDescriptor(runtime_function).function_uuid;
  const bool real =
      function == exec::CanonicalWindowRankingFunction::percent_rank ||
      function == exec::CanonicalWindowRankingFunction::cume_dist;
  request.output_descriptor = WindowDescriptor(
      real ? 6601 : 6600, real ? "real64" : "int64",
      "type_uuid=" + WindowUuid(real ? 6603 : 6602) +
          ";nullability=non_null");
  if (function == exec::CanonicalWindowRankingFunction::ntile) {
    request.ntile_bucket_count = exec::EncodeInt64Value(3);
  }
  return request;
}

bool RuntimeResultAccepted(
    const exec::CanonicalWindowRuntimeResult& result,
    const exec::CanonicalWindowRuntimeStrategy expected_strategy,
    const std::vector<api::EngineTypedValue>& direct_values) {
  return result.diagnostic.ok &&
         result.executed_strategy == expected_strategy &&
         result.every_descriptor_field_consumed &&
         result.exactly_one_strategy_payload_consumed &&
         result.retained_strategy_reached &&
         result.authority.engine_mga_snapshot_bound &&
         result.selected_plan_uuid == WindowUuid(4301) &&
         result.causal_counter_id == 40102 &&
         SameValues(result.values, direct_values);
}

bool ValidateRuntimeRegistry() {
  const auto registry = exec::CanonicalWindowRuntimeRegistryV1();
  std::set<exec::CanonicalWindowRuntimeFunction> functions;
  std::set<std::string> builtin_ids;
  std::set<std::string> uuids;
  bool passed = Require401(registry.size() == 12,
                           "canonical runtime registry row count drifted");
  for (const auto& row : registry) {
    passed &= Require401(
        row.abi_version == 1 &&
            row.function != exec::CanonicalWindowRuntimeFunction::unknown &&
            !row.builtin_id.empty() && !row.function_uuid.empty() &&
            functions.insert(row.function).second &&
            builtin_ids.insert(row.builtin_id).second &&
            uuids.insert(row.function_uuid).second,
        "canonical runtime registry contains an incomplete or duplicate row");
  }
  return passed;
}

bool ValidateRankingStrategies() {
  struct Case {
    exec::CanonicalWindowRuntimeFunction runtime;
    exec::CanonicalWindowRankingFunction ranking;
  };
  const std::vector<Case> cases = {
      {exec::CanonicalWindowRuntimeFunction::row_number,
       exec::CanonicalWindowRankingFunction::row_number},
      {exec::CanonicalWindowRuntimeFunction::rank,
       exec::CanonicalWindowRankingFunction::rank},
      {exec::CanonicalWindowRuntimeFunction::dense_rank,
       exec::CanonicalWindowRankingFunction::dense_rank},
      {exec::CanonicalWindowRuntimeFunction::percent_rank,
       exec::CanonicalWindowRankingFunction::percent_rank},
      {exec::CanonicalWindowRuntimeFunction::cume_dist,
       exec::CanonicalWindowRankingFunction::cume_dist},
      {exec::CanonicalWindowRuntimeFunction::ntile,
       exec::CanonicalWindowRankingFunction::ntile},
  };
  bool passed = true;
  for (const auto& item : cases) {
    auto ranking = RuntimeRankingRequest(item.ranking, item.runtime);
    const auto direct = exec::ExecuteCanonicalWindowRanking(ranking);
    exec::CanonicalWindowRuntimeRequest request;
    request.descriptor = RuntimeDescriptor(item.runtime);
    request.ranking = ranking;
    request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::ranking;
    passed &= Require401(
        direct.diagnostic.ok &&
            RuntimeResultAccepted(exec::ExecuteCanonicalWindowRuntime(request),
                                  *request.forced_strategy, direct.values),
        "ranking strategy diverged behind the canonical runtime");
  }
  return passed;
}

bool ValidateValueStrategies() {
  struct Case {
    exec::CanonicalWindowRuntimeFunction runtime;
    exec::CanonicalWindowValueFunction value;
  };
  const std::vector<Case> cases = {
      {exec::CanonicalWindowRuntimeFunction::lag,
       exec::CanonicalWindowValueFunction::lag},
      {exec::CanonicalWindowRuntimeFunction::lead,
       exec::CanonicalWindowValueFunction::lead},
      {exec::CanonicalWindowRuntimeFunction::first_value,
       exec::CanonicalWindowValueFunction::first_value},
      {exec::CanonicalWindowRuntimeFunction::last_value,
       exec::CanonicalWindowValueFunction::last_value},
      {exec::CanonicalWindowRuntimeFunction::nth_value,
       exec::CanonicalWindowValueFunction::nth_value},
  };
  bool passed = true;
  for (const auto& item : cases) {
    auto value = ValueRequest(item.value);
    if (item.value == exec::CanonicalWindowValueFunction::nth_value) {
      value.nth_values = RepeatedOperand(
          value.frames.ordered_batch.rows.size(), "int64", "2", 6700);
      value.nth_origin = exec::CanonicalWindowNthOrigin::from_first;
      value.null_treatment =
          exec::CanonicalWindowNullTreatment::respect_nulls;
    }
    const auto direct = exec::ExecuteCanonicalWindowValue(value);
    exec::CanonicalWindowRuntimeRequest request;
    request.descriptor = RuntimeDescriptor(item.runtime);
    request.value = value;
    request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::value;
    passed &= Require401(
        direct.diagnostic.ok &&
            RuntimeResultAccepted(exec::ExecuteCanonicalWindowRuntime(request),
                                  *request.forced_strategy, direct.values),
        "value strategy diverged behind the canonical runtime");
  }
  return passed;
}

bool ValidateAggregateStrategyAndRefusals() {
  auto aggregate = AggregateWindowRequest();
  const auto direct = exec::ExecuteCanonicalWindowAggregate(aggregate);
  exec::CanonicalWindowRuntimeRequest request;
  request.descriptor =
      RuntimeDescriptor(exec::CanonicalWindowRuntimeFunction::int64_sum);
  request.aggregate = aggregate;
  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  bool passed = Require401(
      direct.diagnostic.ok &&
          RuntimeResultAccepted(exec::ExecuteCanonicalWindowRuntime(request),
                                *request.forced_strategy, direct.values),
      "aggregate strategy diverged behind the canonical runtime");

  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::ranking;
  auto refused = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-WINDOW-STRATEGY" &&
          refused.values.empty() &&
          refused.executed_strategy ==
              exec::CanonicalWindowRuntimeStrategy::unknown,
      "incompatible forced strategy entered window execution");

  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  request.ranking = RuntimeRankingRequest(
      exec::CanonicalWindowRankingFunction::row_number,
      exec::CanonicalWindowRuntimeFunction::row_number);
  refused = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD" &&
          refused.values.empty(),
      "multiple strategy payloads entered window execution");
  return passed;
}

}  // namespace

#ifndef QOW_WIN_002_FIXTURE_ONLY
// QOW-TEST-WIN-002-V1
int main() {
  return ValidateRuntimeRegistry() && ValidateRankingStrategies() &&
                 ValidateValueStrategies() &&
                 ValidateAggregateStrategyAndRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
