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
  context.database_uuid = "rcp025-registry-db";
  context.transaction_uuid = "rcp025-registry-tx";
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
        alias, std::string(function_uuid), "result_descriptor", context,
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
  const auto mismatched = sblr::InitializeSblrAggregateState(
      count->builtin_id, avg->function_uuid, "int64", context,
      &mismatched_state);
  Require(!mismatched.ok() &&
              HasDiagnostic(
                  mismatched,
                  "SB_DIAG_AGGREGATE_REGISTRY_IDENTITY_MISMATCH"),
          "cross-row aggregate UUID was not refused canonically");

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
    const auto refused = sblr::InitializeSblrAggregateState(
        unregistered, avg->function_uuid, "result_descriptor", context,
        &refused_state);
    Require(!refused.ok() &&
                HasDiagnostic(refused, "SB_DIAG_AGGREGATE_KIND_UNSUPPORTED"),
            std::string(unregistered) +
                " did not refuse through the canonical unsupported route");
  }

  Require(exec::LookupCanonicalAggregateByBuiltinIdV1(
              "sb.aggregate.registry_bypass") == nullptr &&
              exec::LookupCanonicalAggregateByUuidV1(
                  "019f0000-0000-7000-8000-00000000ffff") == nullptr,
          "unknown aggregate registry identity did not fail closed");

  std::cout << "rcp025_canonical_aggregate_registry_conformance=passed "
               "registry_rows=43 legacy_aliases="
            << legacy_state_aliases.size() << " nonregistry_aliases="
            << nonregistry_aliases.size() << '\n';
  return 0;
}
