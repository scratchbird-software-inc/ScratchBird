// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"
#include "runtime_filter_executor.hpp"
#include "runtime_filter_pushdown.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid V7(platform::UuidKind kind,
                       platform::u64 unix_epoch_millis,
                       platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(unix_epoch_millis);
  Require(generated.ok(), "ODF-095 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0x95;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "ODF-095 typed UUIDv7 creation failed");
  return typed.value;
}

idx::CandidateSetAuthorityContext Authority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "provider_finality_or_visibility_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "parser_executes_sql=true",
          "client_finality_or_visibility_authority=true",
          "write_ahead_log_finality_or_visibility_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-095 evidence leaked forbidden document or authority token");
    }
  }
}

opt::RuntimeFilterDescriptor Descriptor(opt::RuntimeFilterFamily family,
                                        opt::RuntimeFilterRoute route,
                                        platform::u64 input_rows,
                                        platform::u64 candidate_rows,
                                        platform::u64 pruned_rows) {
  opt::RuntimeFilterDescriptor descriptor;
  descriptor.family = family;
  descriptor.route = route;
  descriptor.filter_id =
      std::string("odf095.") + opt::RuntimeFilterFamilyName(family) + "." +
      opt::RuntimeFilterRouteName(route);
  descriptor.plan_node_id = "plan_node_" + descriptor.filter_id;
  descriptor.provider_id = "provider_" + descriptor.filter_id;
  descriptor.predicate_digest = "predicate_digest_" + descriptor.filter_id;
  descriptor.descriptor_generation = 10;
  descriptor.required_descriptor_generation = 9;
  descriptor.input_rows = input_rows;
  descriptor.estimated_candidate_rows = candidate_rows;
  descriptor.estimated_pruned_rows = pruned_rows;
  descriptor.baseline_cost_units = input_rows * 10;
  descriptor.filter_cost_units = candidate_rows;
  descriptor.exact_recheck_cost_units = candidate_rows;
  descriptor.plan_shape_supported = true;
  descriptor.provider_supports_runtime_filters =
      route == opt::RuntimeFilterRoute::kProvider;
  descriptor.candidate_set_available = true;
  descriptor.security_context_present = true;
  descriptor.security_snapshot_bound = true;
  descriptor.grants_proven = true;
  descriptor.engine_mga_authoritative = true;
  descriptor.exact_recheck_available = true;
  descriptor.exact_fallback_available = true;
  descriptor.mga_visibility_recheck_required = true;
  descriptor.security_authorization_recheck_required = true;
  return descriptor;
}

idx::CandidateSetRow Row(platform::byte suffix,
                         bool exact,
                         bool visible,
                         bool authorized) {
  idx::CandidateSetRow row;
  row.row_uuid = V7(platform::UuidKind::row, 1710000095000ull, suffix);
  row.score = static_cast<double>(suffix);
  row.exact_predicate_match = exact;
  row.mga_visible = visible;
  row.security_authorized = authorized;
  row.exact_payload_available = true;
  row.source = "odf095_runtime_filter";
  return row;
}

platform::byte FamilySuffix(opt::RuntimeFilterFamily family,
                            opt::RuntimeFilterRoute route) {
  platform::byte base = 0x10;
  switch (family) {
    case opt::RuntimeFilterFamily::kJoin:
      base = 0x10;
      break;
    case opt::RuntimeFilterFamily::kGraph:
      base = 0x20;
      break;
    case opt::RuntimeFilterFamily::kSearch:
      base = 0x30;
      break;
    case opt::RuntimeFilterFamily::kVector:
      base = 0x40;
      break;
    case opt::RuntimeFilterFamily::kTimeSeries:
      base = 0x50;
      break;
    case opt::RuntimeFilterFamily::kCandidateSet:
      base = 0x60;
      break;
    case opt::RuntimeFilterFamily::kUnknown:
      base = 0x70;
      break;
  }
  return static_cast<platform::byte>(
      base + (route == opt::RuntimeFilterRoute::kProvider ? 2 : 1));
}

exec::RuntimeFilterProviderResult CandidateProvider(
    const exec::RuntimeFilterProviderRequest& request) {
  exec::RuntimeFilterProviderResult result;
  result.status = {platform::StatusCode::ok, platform::Severity::info,
                   platform::Subsystem::engine};
  result.exact_recheck_evidence_present = true;
  result.mga_recheck_evidence_present = true;
  result.security_recheck_evidence_present = true;
  const auto suffix =
      FamilySuffix(request.descriptor.family, request.descriptor.route);
  result.candidate_rows.push_back(Row(suffix, true, true, true));
  result.candidate_rows.push_back(Row(static_cast<platform::byte>(suffix + 1),
                                      false, true, true));
  result.evidence.push_back(
      std::string("runtime_filter.provider_family=") +
      opt::RuntimeFilterFamilyName(request.descriptor.family));
  result.evidence.push_back(
      std::string("runtime_filter.provider_route=") +
      opt::RuntimeFilterRouteName(request.descriptor.route));
  result.evidence.push_back("runtime_filter.provider_candidate_rows=2");
  result.evidence.push_back("runtime_filter.provider_returns_final_rows=false");
  return result;
}

exec::RuntimeFilterProviderResult FallbackProvider(
    const exec::RuntimeFilterProviderRequest& request) {
  auto result = CandidateProvider(request);
  result.evidence.push_back("runtime_filter.exact_fallback_provider=true");
  return result;
}

void OptimizerSelectsSafeRuntimeFiltersForAllFamiliesAndRoutes() {
  opt::RuntimeFilterPushdownRequest request;
  request.plan_id = "odf095_plan";
  request.min_net_benefit_units = 1;
  request.candidates = {
      Descriptor(opt::RuntimeFilterFamily::kJoin, opt::RuntimeFilterRoute::kScan,
                 100, 20, 80),
      Descriptor(opt::RuntimeFilterFamily::kGraph,
                 opt::RuntimeFilterRoute::kProvider, 110, 30, 80),
      Descriptor(opt::RuntimeFilterFamily::kSearch,
                 opt::RuntimeFilterRoute::kProvider, 120, 40, 80),
      Descriptor(opt::RuntimeFilterFamily::kVector,
                 opt::RuntimeFilterRoute::kProvider, 130, 50, 80),
      Descriptor(opt::RuntimeFilterFamily::kTimeSeries,
                 opt::RuntimeFilterRoute::kProvider, 140, 60, 80),
      Descriptor(opt::RuntimeFilterFamily::kCandidateSet,
                 opt::RuntimeFilterRoute::kScan, 150, 70, 80)};

  const auto decision = opt::EvaluateRuntimeFilterPushdown(request);
  Require(decision.ok, "ODF-095 optimizer pushdown decision failed");
  Require(decision.selected_filters.size() == 6,
          "ODF-095 optimizer did not select all safe filter families");
  Require(EvidenceHas(decision.evidence, "runtime_filter.pushed_filter_count=6"),
          "ODF-095 optimizer pushed-filter counter missing");
  Require(EvidenceHas(decision.evidence, "runtime_filter.fallback_count=0"),
          "ODF-095 optimizer fallback counter missing");
  for (const auto family :
       {"join", "graph", "search", "vector", "time_series",
        "candidate_set"}) {
    Require(opt::SerializeRuntimeFilterPushdownEvidence(decision).find(family) !=
                std::string::npos,
            "ODF-095 serialized optimizer evidence missing family");
  }
  RequireEvidenceHygiene(decision.evidence);
  for (const auto& candidate : decision.candidate_decisions) {
    RequireEvidenceHygiene(candidate.evidence);
  }

  const auto execution = exec::ExecuteRuntimeFilterPushdown(
      decision.selected_filters, Authority(),
      {CandidateProvider, CandidateProvider, FallbackProvider});
  Require(execution.ok(), "ODF-095 executor runtime filter pushdown failed");
  Require(execution.counters.pushed_filter_count == 6,
          "ODF-095 pushed filter counter changed");
  Require(execution.counters.input_rows == 750,
          "ODF-095 input row counter changed");
  Require(execution.counters.candidate_rows == 12,
          "ODF-095 candidate row counter changed");
  Require(execution.counters.pruned_rows == 480,
          "ODF-095 pruned row counter changed");
  Require(execution.counters.fallback_count == 0,
          "ODF-095 fallback counter changed");
  Require(execution.counters.exact_recheck_count == 12,
          "ODF-095 exact recheck counter changed");
  Require(execution.final_row_uuids.size() == 6,
          "ODF-095 final row count did not require exact/MGA/security recheck");
  Require(EvidenceHas(execution.evidence,
                      "runtime_filter.exact_recheck_count=12"),
          "ODF-095 exact recheck evidence missing");
  Require(EvidenceHas(execution.evidence,
                      "runtime_filter.mga_visibility_recheck_required=true"),
          "ODF-095 MGA recheck evidence missing");
  Require(EvidenceHas(execution.evidence,
                      "runtime_filter.security_recheck_required=true"),
          "ODF-095 security recheck evidence missing");
  RequireEvidenceHygiene(execution.evidence);
}

void OptimizerFailsClosedForUnsafeDescriptors() {
  auto RequireOptimizerRefusal = [](opt::RuntimeFilterDescriptor descriptor,
                                    std::string_view diagnostic) {
    opt::RuntimeFilterPushdownRequest request;
    request.plan_id = "odf095_refusal";
    request.candidates.push_back(std::move(descriptor));
    const auto decision = opt::EvaluateRuntimeFilterPushdown(request);
    Require(!decision.ok && decision.fail_closed,
            "ODF-095 optimizer unsafe descriptor was accepted");
    Require(decision.diagnostic_code == diagnostic,
            "ODF-095 optimizer diagnostic changed");
    RequireEvidenceHygiene(decision.evidence);
  };

  auto descriptor = Descriptor(opt::RuntimeFilterFamily::kJoin,
                               opt::RuntimeFilterRoute::kScan, 10, 3, 7);
  descriptor.security_context_present = false;
  RequireOptimizerRefusal(descriptor,
                          "SB_RUNTIME_FILTER.SECURITY_CONTEXT_REQUIRED");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kGraph,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.engine_mga_authoritative = false;
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.MGA_RECHECK_REQUIRED");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kSearch,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.exact_recheck_available = false;
  RequireOptimizerRefusal(descriptor,
                          "SB_RUNTIME_FILTER.EXACT_RECHECK_REQUIRED");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kUnknown,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.UNSUPPORTED_FAMILY");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kVector,
                          opt::RuntimeFilterRoute::kUnknown, 10, 3, 7);
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.UNSUPPORTED_ROUTE");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kTimeSeries,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.stale = true;
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.STALE_DESCRIPTOR");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kCandidateSet,
                          opt::RuntimeFilterRoute::kScan, 10, 3, 7);
  descriptor.descriptor_scan_selected = true;
  RequireOptimizerRefusal(descriptor,
                          "SB_RUNTIME_FILTER.PHYSICAL_ROUTE_REQUIRED");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kJoin,
                          opt::RuntimeFilterRoute::kScan, 10, 3, 7);
  descriptor.behavior_store_scan_selected = true;
  RequireOptimizerRefusal(descriptor,
                          "SB_RUNTIME_FILTER.PHYSICAL_ROUTE_REQUIRED");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kSearch,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.candidate_set_available = false;
  RequireOptimizerRefusal(descriptor,
                          "SB_RUNTIME_FILTER.CANDIDATE_SET_REQUIRED");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kVector,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.lossy_or_false_negative_possible = true;
  descriptor.exact_fallback_available = false;
  RequireOptimizerRefusal(descriptor,
                          "SB_RUNTIME_FILTER.EXACT_FALLBACK_REQUIRED");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kGraph,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.parser_or_donor_finality_or_visibility_authority = true;
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.UNSAFE_AUTHORITY");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kSearch,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.client_finality_or_visibility_authority = true;
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.UNSAFE_AUTHORITY");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kVector,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.provider_finality_or_visibility_authority = true;
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.UNSAFE_AUTHORITY");

  descriptor = Descriptor(opt::RuntimeFilterFamily::kTimeSeries,
                          opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  descriptor.write_ahead_log_finality_or_visibility_authority = true;
  RequireOptimizerRefusal(descriptor, "SB_RUNTIME_FILTER.UNSAFE_AUTHORITY");
}

void ExecutorFailsClosedForProviderDriftAndCorruptRows() {
  auto descriptor = Descriptor(opt::RuntimeFilterFamily::kSearch,
                               opt::RuntimeFilterRoute::kProvider, 10, 3, 7);
  auto RequireExecutorRefusal =
      [&descriptor](exec::RuntimeFilterProvider provider,
                    std::string_view diagnostic) {
        const auto result = exec::ExecuteRuntimeFilterPushdown(
            {descriptor}, Authority(), {CandidateProvider, provider, {}});
        Require(!result.ok() && result.fail_closed,
                "ODF-095 executor unsafe provider was accepted");
        Require(result.diagnostic_code == diagnostic,
                "ODF-095 executor diagnostic changed");
        RequireEvidenceHygiene(result.evidence);
      };

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest&) {
        auto result = CandidateProvider({});
        result.exact_recheck_evidence_present = false;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.EXACT_RECHECK_EVIDENCE_REQUIRED");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest& request) {
        auto result = CandidateProvider(request);
        result.returns_final_rows = true;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_FINAL_ROWS_REFUSED");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest& request) {
        auto result = CandidateProvider(request);
        result.parser_or_donor_finality_or_visibility_authority = true;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest& request) {
        auto result = CandidateProvider(request);
        result.client_finality_or_visibility_authority = true;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest& request) {
        auto result = CandidateProvider(request);
        result.provider_finality_or_visibility_authority = true;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest& request) {
        auto result = CandidateProvider(request);
        result.write_ahead_log_finality_or_visibility_authority = true;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest& request) {
        auto result = CandidateProvider(request);
        result.mga_recheck_evidence_present = false;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.MGA_RECHECK_EVIDENCE_REQUIRED");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest& request) {
        auto result = CandidateProvider(request);
        result.security_recheck_evidence_present = false;
        return result;
      },
      "SB_RUNTIME_FILTER_EXECUTOR.SECURITY_RECHECK_EVIDENCE_REQUIRED");

  RequireExecutorRefusal(
      [](const exec::RuntimeFilterProviderRequest&) {
        exec::RuntimeFilterProviderResult result;
        result.status = {platform::StatusCode::ok, platform::Severity::info,
                         platform::Subsystem::engine};
        result.exact_recheck_evidence_present = true;
        result.mga_recheck_evidence_present = true;
        result.security_recheck_evidence_present = true;
        result.candidate_rows.push_back({});
        return result;
      },
      "SB_CANDIDATE_SET.EXACT_ROW_UUID_REQUIRED");

  auto bad_descriptor = descriptor;
  bad_descriptor.plan_shape_supported = false;
  auto refused = exec::ExecuteRuntimeFilterPushdown(
      {bad_descriptor}, Authority(), {CandidateProvider, CandidateProvider, {}});
  Require(!refused.ok() && refused.fail_closed,
          "ODF-095 executor accepted unsafe plan shape");
  Require(refused.diagnostic_code ==
              "SB_RUNTIME_FILTER_EXECUTOR.PLAN_SHAPE_REFUSED",
          "ODF-095 executor plan-shape diagnostic changed");

  bad_descriptor = descriptor;
  bad_descriptor.estimated_candidate_rows = bad_descriptor.input_rows + 1;
  refused = exec::ExecuteRuntimeFilterPushdown(
      {bad_descriptor}, Authority(), {CandidateProvider, CandidateProvider, {}});
  Require(!refused.ok() && refused.fail_closed,
          "ODF-095 executor accepted corrupt descriptor counters");
  Require(refused.diagnostic_code ==
              "SB_RUNTIME_FILTER_EXECUTOR.COUNTERS_INVALID",
          "ODF-095 executor counter diagnostic changed");

  bad_descriptor = descriptor;
  bad_descriptor.provider_supports_runtime_filters = false;
  bad_descriptor.exact_fallback_available = false;
  refused = exec::ExecuteRuntimeFilterPushdown(
      {bad_descriptor}, Authority(), {CandidateProvider, CandidateProvider, {}});
  Require(!refused.ok() && refused.fail_closed,
          "ODF-095 executor accepted unsupported provider without fallback");
  Require(refused.diagnostic_code ==
              "SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_UNSUPPORTED",
          "ODF-095 executor unsupported-provider diagnostic changed");

  bad_descriptor = descriptor;
  bad_descriptor.provider_supports_runtime_filters = false;
  bad_descriptor.exact_fallback_available = true;
  const auto descriptor_fallback = exec::ExecuteRuntimeFilterPushdown(
      {bad_descriptor}, Authority(),
      {CandidateProvider, CandidateProvider, FallbackProvider});
  Require(descriptor_fallback.ok(),
          "ODF-095 descriptor-driven exact fallback failed");
  Require(descriptor_fallback.counters.fallback_count == 1,
          "ODF-095 descriptor fallback counter missing");
  Require(EvidenceHas(descriptor_fallback.evidence,
                      "runtime_filter.provider_unsupported=true"),
          "ODF-095 descriptor fallback unsupported evidence missing");

  auto unsupported = exec::ExecuteRuntimeFilterPushdown(
      {descriptor}, Authority(),
      {CandidateProvider,
       [](const exec::RuntimeFilterProviderRequest&) {
         return exec::MakeUnsupportedRuntimeFilterProviderResult(
             "runtime_filter.provider=unsupported_fixture");
       },
       {}});
  Require(!unsupported.ok() && unsupported.fail_closed,
          "ODF-095 unsupported provider without fallback was accepted");
  Require(unsupported.diagnostic_code ==
              "SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_UNSUPPORTED",
          "ODF-095 unsupported provider diagnostic changed");

  const auto fallback = exec::ExecuteRuntimeFilterPushdown(
      {descriptor}, Authority(),
      {CandidateProvider,
       [](const exec::RuntimeFilterProviderRequest&) {
         return exec::MakeUnsupportedRuntimeFilterProviderResult(
             "runtime_filter.provider=unsupported_fixture");
       },
       FallbackProvider});
  Require(fallback.ok(), "ODF-095 exact fallback provider failed");
  Require(fallback.counters.fallback_count == 1,
          "ODF-095 exact fallback counter missing");
  Require(EvidenceHas(fallback.evidence, "runtime_filter.fallback=exact_provider"),
          "ODF-095 exact fallback evidence missing");
  RequireEvidenceHygiene(fallback.evidence);

  const auto unsupported_fallback = exec::ExecuteRuntimeFilterPushdown(
      {descriptor}, Authority(),
      {CandidateProvider,
       [](const exec::RuntimeFilterProviderRequest&) {
         return exec::MakeUnsupportedRuntimeFilterProviderResult(
             "runtime_filter.provider=unsupported_fixture");
       },
       [](const exec::RuntimeFilterProviderRequest&) {
         return exec::MakeUnsupportedRuntimeFilterProviderResult(
             "runtime_filter.fallback_provider=unsupported_fixture");
       }});
  Require(!unsupported_fallback.ok() && unsupported_fallback.fail_closed,
          "ODF-095 unsupported exact fallback provider was accepted");
  Require(unsupported_fallback.diagnostic_code ==
              "SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_UNSUPPORTED",
          "ODF-095 unsupported fallback diagnostic changed");
}

}  // namespace

int main() {
  OptimizerSelectsSafeRuntimeFiltersForAllFamiliesAndRoutes();
  OptimizerFailsClosedForUnsafeDescriptors();
  ExecutorFailsClosedForProviderDriftAndCorruptRows();
  return EXIT_SUCCESS;
}
