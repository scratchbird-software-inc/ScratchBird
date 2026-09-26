#include "../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_aggregate_registry.hpp"
#include "sblr_aggregate_window_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(const std::string& detail) {
  std::cerr << "rcp025_canonical_aggregate_registry_conformance: " << detail
            << '\n';
  std::exit(1);
}

void Require(const bool condition, const std::string& detail) {
  if (!condition) Fail(detail);
}

bool HasDiagnostic(const sblr::SblrResult& result,
                   const std::string_view diagnostic_id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == diagnostic_id) return true;
  }
  return false;
}

}  // namespace

int main() {
  const auto& registry = exec::CanonicalAggregateRuntimeRegistryV1();
  Require(registry.size() == 43,
          "canonical aggregate registry must contain exactly 43 rows");
  Require(exec::ValidateCanonicalAggregateRuntimeRegistryV1().empty(),
          "canonical aggregate registry self-validation failed");

  for (const auto& entry : registry) {
    Require(exec::LookupCanonicalAggregateByFunctionV1(entry.function) ==
                    &entry &&
                exec::LookupCanonicalAggregateByBuiltinIdV1(
                    entry.builtin_id) == &entry &&
                exec::LookupCanonicalAggregateByUuidV1(
                    entry.function_uuid) == &entry &&
                exec::LookupCanonicalAggregateExactV1(
                    entry.abi_version, entry.function, entry.builtin_id,
                    entry.function_uuid) == &entry,
            entry.builtin_id + " did not resolve to its sole stable row");
  }

  const std::vector<std::string_view> legacy_state_aliases = {
      "count",          "sum",             "avg",
      "min",            "max",             "every",
      "bool_and",       "bool_or",         "variance",
      "variance_samp",  "variance_pop",    "stddev",
      "stddev_samp",    "stddev_pop",      "corr",
      "covar_pop",      "covar_samp",      "regr_avgx",
      "regr_avgy",      "regr_count",      "regr_intercept",
      "regr_r2",        "regr_slope",      "regr_sxx",
      "regr_sxy",       "regr_syy",        "string_agg",
      "listagg",        "array_agg",       "json_agg",
      "json_object_agg", "approx_count_distinct",
      "approx_median",  "approx_percentile_cont",
      "approx_percentile_disc", "approx_top_k", "mode"};

  sblr::SblrExecutionContext context;
  context.database_uuid = scratchbird::tests::FixtureUuid(1208, 4001);
  context.transaction_uuid = scratchbird::tests::FixtureUuid(1208, 4002);
  context.transaction_context_present = true;

  for (const auto alias : legacy_state_aliases) {
    Require(sblr::IsSblrAggregateFunctionSupported(alias),
            std::string(alias) + " was not admitted by the canonical registry");
    const auto builtin_id =
        sblr::ResolveSblrCanonicalAggregateBuiltinId(alias);
    const auto function_uuid =
        sblr::ResolveSblrCanonicalAggregateFunctionUuid(alias);
    const auto* entry =
        exec::LookupCanonicalAggregateByBuiltinIdV1(builtin_id);
    Require(entry != nullptr && entry->function_uuid == function_uuid,
            std::string(alias) + " alias identity bypassed the registry");

    sblr::SblrAggregateWindowState state;
    const auto initialized = sblr::InitializeSblrAggregateState(
        alias, function_uuid, "result_descriptor", context,
        &state);
    Require(initialized.ok() && state.function_id == entry->builtin_id &&
                state.function_uuid == entry->function_uuid,
            std::string(alias) +
                " did not initialize with canonical registry identity");
  }

  const auto* count = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  const auto* avg = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::avg);
  Require(count != nullptr && avg != nullptr,
          "COUNT or AVG canonical registry row is missing");
  sblr::SblrAggregateWindowState mismatched_state;
  Require(sblr::InitializeSblrAggregateState(
              count->builtin_id, count->function_uuid, "int64", context,
              &mismatched_state).ok(), "COUNT state initialization failed");
  // Input labels do not select execution. A cross-bound *runtime state*,
  // however, must still refuse before changing counters or publishing data.
  mismatched_state.function_uuid = avg->function_uuid;
  sblr::SblrAggregateUpdateRequest update;
  update.context = context;
  const auto mismatched = sblr::UpdateSblrAggregateState(&mismatched_state, update);
  Require(!mismatched.ok() && mismatched_state.input_count == 0 &&
              HasDiagnostic(mismatched, "SB_DIAG_AGGREGATE_REGISTRY_IDENTITY_MISMATCH"),
          "cross-row aggregate UUID was not refused canonically");
  sblr::SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  const auto mismatched_result = sblr::FinalizeSblrAggregateState(mismatched_state, finalize);
  Require(!mismatched_result.ok() && mismatched_result.scalar_values.empty() &&
              HasDiagnostic(mismatched_result, "SB_DIAG_AGGREGATE_REGISTRY_IDENTITY_MISMATCH"),
          "cross-row aggregate state published a result");

  sblr::SblrAggregateWindowState uuid_bound_state;
  const auto uuid_bound = sblr::InitializeSblrAggregateState(
      count->builtin_id, avg->function_uuid, "int64", context,
      &uuid_bound_state);
  Require(uuid_bound.ok() && uuid_bound_state.function_id == avg->builtin_id &&
              uuid_bound_state.function_uuid == avg->function_uuid &&
              uuid_bound_state.aggregate_kind == sblr::SblrAggregateFunctionKind::avg,
          "text COUNT label overrode binary AVG authority");
  sblr::SblrValue value;
  value.descriptor_id = "int64";
  value.is_null = false;
  value.payload_kind = sblr::SblrValuePayloadKind::high_precision_numeric_text;
  for (const auto number : {"2", "6"}) {
    value.encoded_value = number;
    update.values = {value};
    Require(sblr::UpdateSblrAggregateState(&uuid_bound_state, update).ok(),
            "UUID-bound AVG failed to consume numeric input");
  }
  const auto average = sblr::FinalizeSblrAggregateState(uuid_bound_state, finalize);
  Require(average.ok() && average.scalar_values.size() == 1 &&
              average.scalar_values.front().encoded_value == "4",
          "binary AVG execution returned COUNT or lost its actual result");

  const std::vector<std::string_view> nonregistry_aliases = {
      "bit_and",                  "data.aggregate.bit_and",
      "sb.aggregate.bit_and",     "bit_or",
      "data.aggregate.bit_or",    "sb.aggregate.bit_or",
      "bit_xor",                  "data.aggregate.bit_xor",
      "sb.aggregate.bit_xor",     "binary_agg",
      "bytea_agg",                "data.aggregate.binary_agg",
      "sb.aggregate.binary_agg"};
  for (const auto unregistered : nonregistry_aliases) {
    Require(sblr::ResolveSblrAggregateFunctionKind(unregistered) ==
                    sblr::SblrAggregateFunctionKind::unknown &&
                !sblr::IsSblrAggregateFunctionSupported(unregistered),
            std::string(unregistered) +
                " was resolved or reported supported without a registry row");
    sblr::SblrAggregateWindowState refused_state;
    const auto unknown_uuid = scratchbird::tests::FixtureUuidLiteral(
        "019f0000-0000-7000-8000-00000000ffff");
    const auto refused = sblr::InitializeSblrAggregateState(
        unregistered, unknown_uuid, "result_descriptor", context,
        &refused_state);
    Require(!refused.ok() && !refused_state.initialized &&
                HasDiagnostic(refused, "SB_DIAG_AGGREGATE_KIND_UNSUPPORTED"),
            std::string(unregistered) +
                " did not refuse through the canonical unsupported route");
  }

  for (auto invalid_uuid : {sblr::SblrUuid{}, scratchbird::tests::FixtureUuid(2054, 1),
                           avg->function_uuid}) {
    if (invalid_uuid == avg->function_uuid) invalid_uuid.bytes[6] = 0x40;
    const auto before = uuid_bound_state;
    const auto refused = sblr::InitializeSblrAggregateState(
        avg->builtin_id, invalid_uuid, "int64", context, &uuid_bound_state);
    Require(!refused.ok() && HasDiagnostic(refused, "SB_DIAG_AGGREGATE_KIND_UNSUPPORTED") &&
                uuid_bound_state.function_uuid == before.function_uuid &&
                uuid_bound_state.input_count == before.input_count &&
                uuid_bound_state.numeric_sum == before.numeric_sum,
            "registered AVG name rescued an invalid UUID or changed prior state");
  }

  Require(exec::LookupCanonicalAggregateByBuiltinIdV1(
              "sb.aggregate.registry_bypass") == nullptr &&
              exec::LookupCanonicalAggregateByUuidV1(
                  scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-00000000ffff")) == nullptr,
          "unknown aggregate registry identity did not fail closed");

  std::cout << "rcp025_canonical_aggregate_registry_conformance=passed "
               "registry_rows=43 legacy_aliases="
            << legacy_state_aliases.size() << " nonregistry_aliases="
            << nonregistry_aliases.size() << '\n';
  return 0;
}
