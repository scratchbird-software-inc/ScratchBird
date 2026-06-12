// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_filter_executor.hpp"

#include "candidate_set_executor.hpp"

#include <utility>

namespace scratchbird::engine::executor {
namespace {

namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;

platform::Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::engine};
}

platform::Status RefusalStatus() {
  return {platform::StatusCode::diagnostic_invalid_record,
          platform::Severity::error, platform::Subsystem::engine};
}

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

const char* FamilyName(opt::RuntimeFilterFamily family) {
  switch (family) {
    case opt::RuntimeFilterFamily::kJoin:
      return "join";
    case opt::RuntimeFilterFamily::kGraph:
      return "graph";
    case opt::RuntimeFilterFamily::kSearch:
      return "search";
    case opt::RuntimeFilterFamily::kVector:
      return "vector";
    case opt::RuntimeFilterFamily::kTimeSeries:
      return "time_series";
    case opt::RuntimeFilterFamily::kCandidateSet:
      return "candidate_set";
    case opt::RuntimeFilterFamily::kUnknown:
      break;
  }
  return "unknown";
}

const char* RouteName(opt::RuntimeFilterRoute route) {
  switch (route) {
    case opt::RuntimeFilterRoute::kScan:
      return "scan";
    case opt::RuntimeFilterRoute::kProvider:
      return "provider";
    case opt::RuntimeFilterRoute::kUnknown:
      break;
  }
  return "unknown";
}

bool FamilySupported(opt::RuntimeFilterFamily family) {
  return family == opt::RuntimeFilterFamily::kJoin ||
         family == opt::RuntimeFilterFamily::kGraph ||
         family == opt::RuntimeFilterFamily::kSearch ||
         family == opt::RuntimeFilterFamily::kVector ||
         family == opt::RuntimeFilterFamily::kTimeSeries ||
         family == opt::RuntimeFilterFamily::kCandidateSet;
}

bool RouteSupported(opt::RuntimeFilterRoute route) {
  return route == opt::RuntimeFilterRoute::kScan ||
         route == opt::RuntimeFilterRoute::kProvider;
}

RuntimeFilterExecutionResult Refuse(std::string code, std::string evidence) {
  RuntimeFilterExecutionResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "runtime_filter.executor.fail_closed=true");
  Add(&result.evidence, "runtime_filter.candidate_rows_only=true");
  Add(&result.evidence, "runtime_filter.exact_recheck_required=true");
  Add(&result.evidence, "runtime_filter.mga_visibility_recheck_required=true");
  Add(&result.evidence, "runtime_filter.security_recheck_required=true");
  Add(&result.evidence, "parser_or_reference_finality_or_visibility_authority=false");
  Add(&result.evidence, "client_finality_or_visibility_authority=false");
  Add(&result.evidence, "provider_finality_or_visibility_authority=false");
  Add(&result.evidence,
      "write_ahead_log_finality_or_visibility_authority=false");
  return result;
}

bool DescriptorAuthoritySafe(const opt::RuntimeFilterDescriptor& descriptor) {
  return !descriptor.parser_or_reference_finality_or_visibility_authority &&
         !descriptor.client_finality_or_visibility_authority &&
         !descriptor.provider_finality_or_visibility_authority &&
         !descriptor.write_ahead_log_finality_or_visibility_authority;
}

RuntimeFilterExecutionResult ValidateDescriptor(
    const opt::RuntimeFilterDescriptor& descriptor) {
  if (!FamilySupported(descriptor.family)) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.UNSUPPORTED_FAMILY",
                  "unsupported_family");
  }
  if (!RouteSupported(descriptor.route)) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.UNSUPPORTED_ROUTE",
                  "unsupported_route");
  }
  if (descriptor.filter_id.empty() || descriptor.plan_node_id.empty() ||
      descriptor.predicate_digest.empty()) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.DESCRIPTOR_REQUIRED",
                  "descriptor_identity_required");
  }
  if (!descriptor.plan_shape_supported) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PLAN_SHAPE_REFUSED",
                  "plan_shape_not_runtime_filter_safe");
  }
  if (descriptor.estimated_candidate_rows > descriptor.input_rows ||
      descriptor.estimated_pruned_rows > descriptor.input_rows) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.COUNTERS_INVALID",
                  "candidate_or_pruned_rows_exceed_input_rows");
  }
  if (!descriptor.candidate_set_available) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.CANDIDATE_SET_REQUIRED",
                  "candidate_set_required");
  }
  if (!descriptor.security_context_present ||
      !descriptor.security_snapshot_bound ||
      !descriptor.grants_proven) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.SECURITY_CONTEXT_REQUIRED",
                  "security_context_snapshot_and_grants_required");
  }
  if (!descriptor.engine_mga_authoritative ||
      !descriptor.mga_visibility_recheck_required) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.MGA_RECHECK_REQUIRED",
                  "engine_mga_visibility_recheck_required");
  }
  if (!descriptor.exact_recheck_available ||
      !descriptor.security_authorization_recheck_required) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.EXACT_RECHECK_REQUIRED",
                  "exact_and_security_recheck_required");
  }
  if (!DescriptorAuthoritySafe(descriptor)) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY",
                  "unsafe_descriptor_authority_claim");
  }
  if (descriptor.descriptor_scan_selected ||
      descriptor.behavior_store_scan_selected) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PHYSICAL_ROUTE_REQUIRED",
                  "descriptor_or_behavior_store_scan_refused");
  }
  if (descriptor.stale ||
      descriptor.descriptor_generation < descriptor.required_descriptor_generation) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.STALE_DESCRIPTOR",
                  "stale_runtime_filter_descriptor");
  }
  if (descriptor.lossy_or_false_negative_possible &&
      !descriptor.exact_fallback_available) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.EXACT_FALLBACK_REQUIRED",
                  "lossy_or_false_negative_filter_requires_exact_fallback");
  }
  RuntimeFilterExecutionResult ok;
  ok.status = OkStatus();
  return ok;
}

RuntimeFilterExecutionResult ValidateProviderRows(
    const RuntimeFilterProviderResult& provider_result) {
  if (provider_result.unsupported) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_UNSUPPORTED",
                  "provider_runtime_filter_unsupported");
  }
  if (!provider_result.ok()) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_FAILED",
                  "provider_runtime_filter_failed");
  }
  if (provider_result.returns_final_rows) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_FINAL_ROWS_REFUSED",
                  "provider_returned_final_rows");
  }
  if (provider_result.parser_or_reference_finality_or_visibility_authority ||
      provider_result.client_finality_or_visibility_authority ||
      provider_result.provider_finality_or_visibility_authority ||
      provider_result.write_ahead_log_finality_or_visibility_authority) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.UNSAFE_AUTHORITY",
                  "provider_visibility_or_finality_claim_refused");
  }
  if (!provider_result.exact_recheck_evidence_present) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.EXACT_RECHECK_EVIDENCE_REQUIRED",
                  "provider_exact_recheck_evidence_missing");
  }
  if (!provider_result.mga_recheck_evidence_present) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.MGA_RECHECK_EVIDENCE_REQUIRED",
                  "provider_mga_recheck_evidence_missing");
  }
  if (!provider_result.security_recheck_evidence_present) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.SECURITY_RECHECK_EVIDENCE_REQUIRED",
                  "provider_security_recheck_evidence_missing");
  }
  RuntimeFilterExecutionResult ok;
  ok.status = OkStatus();
  return ok;
}

void AddDescriptorEvidence(RuntimeFilterExecutionResult* result,
                           const opt::RuntimeFilterDescriptor& descriptor) {
  Add(&result->evidence,
      std::string("runtime_filter.family=") + FamilyName(descriptor.family));
  Add(&result->evidence,
      std::string("runtime_filter.route=") + RouteName(descriptor.route));
  Add(&result->evidence,
      "runtime_filter.input_rows=" + std::to_string(descriptor.input_rows));
  Add(&result->evidence,
      "runtime_filter.estimated_candidate_rows=" +
          std::to_string(descriptor.estimated_candidate_rows));
  Add(&result->evidence,
      "runtime_filter.estimated_pruned_rows=" +
          std::to_string(descriptor.estimated_pruned_rows));
}

}  // namespace

RuntimeFilterProviderResult MakeUnsupportedRuntimeFilterProviderResult(
    std::string evidence) {
  RuntimeFilterProviderResult result;
  result.status = OkStatus();
  result.unsupported = true;
  result.exact_recheck_evidence_present = true;
  result.mga_recheck_evidence_present = true;
  result.security_recheck_evidence_present = true;
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "runtime_filter.provider_supported=false");
  return result;
}

RuntimeFilterExecutionResult ExecuteRuntimeFilterPushdown(
    const std::vector<opt::RuntimeFilterDescriptor>& filters,
    const idx::CandidateSetAuthorityContext& authority,
    const RuntimeFilterProviderSet& providers) {
  if (filters.empty()) {
    return Refuse("SB_RUNTIME_FILTER_EXECUTOR.CANDIDATE_SET_REQUIRED",
                  "selected_runtime_filters_required");
  }

  RuntimeFilterExecutionResult result;
  result.status = OkStatus();
  result.diagnostic_code = "SB_RUNTIME_FILTER_EXECUTOR.OK";
  Add(&result.evidence, "runtime_filter.executor=pushdown_v1");
  Add(&result.evidence, "runtime_filter.candidate_rows_only=true");

  std::vector<idx::CandidateSetRow> all_rows;
  for (const auto& descriptor : filters) {
    auto descriptor_check = ValidateDescriptor(descriptor);
    if (!descriptor_check.ok()) {
      return descriptor_check;
    }
    result.counters.input_rows += descriptor.input_rows;
    result.counters.pruned_rows += descriptor.estimated_pruned_rows;
    ++result.counters.pushed_filter_count;
    AddDescriptorEvidence(&result, descriptor);

    RuntimeFilterProvider provider;
    bool use_exact_fallback = false;
    if (descriptor.route == opt::RuntimeFilterRoute::kScan) {
      provider = providers.scan_provider;
    } else if (!descriptor.provider_supports_runtime_filters) {
      if (!descriptor.exact_fallback_available) {
        return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_UNSUPPORTED",
                      "provider_runtime_filter_unsupported_no_fallback");
      }
      provider = providers.exact_fallback_provider;
      use_exact_fallback = true;
    } else {
      provider = providers.physical_provider;
    }
    if (!provider) {
      return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_REQUIRED",
                    "runtime_filter_provider_missing");
    }

    RuntimeFilterProviderRequest request{descriptor, authority};
    if (use_exact_fallback) {
      ++result.counters.fallback_count;
      Add(&result.evidence, "runtime_filter.provider_unsupported=true");
      Add(&result.evidence, "runtime_filter.fallback=exact_provider");
    }
    auto provider_result = provider(request);
    if (provider_result.unsupported) {
      if (!descriptor.exact_fallback_available ||
          !providers.exact_fallback_provider) {
        return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_UNSUPPORTED",
                      "provider_runtime_filter_unsupported_no_fallback");
      }
      ++result.counters.fallback_count;
      Add(&result.evidence, "runtime_filter.fallback=exact_provider");
      provider_result = providers.exact_fallback_provider(request);
      if (provider_result.unsupported) {
        return Refuse("SB_RUNTIME_FILTER_EXECUTOR.PROVIDER_UNSUPPORTED",
                      "exact_fallback_provider_unsupported");
      }
    }
    auto provider_check = ValidateProviderRows(provider_result);
    if (!provider_check.ok()) {
      return provider_check;
    }
    result.evidence.insert(result.evidence.end(),
                           provider_result.evidence.begin(),
                           provider_result.evidence.end());
    all_rows.insert(all_rows.end(), provider_result.candidate_rows.begin(),
                    provider_result.candidate_rows.end());
  }

  auto candidates =
      idx::MakeExactRowUuidOrderedCandidateSet(all_rows, authority, false);
  if (!candidates.ok()) {
    auto refused = Refuse(candidates.diagnostic.diagnostic_code,
                          "candidate_rows_corrupt_or_unsafe");
    refused.evidence.insert(refused.evidence.end(), candidates.evidence.begin(),
                            candidates.evidence.end());
    return refused;
  }
  result.candidate_rows = candidates.output;
  result.counters.candidate_rows = candidates.output.rows.size();
  result.counters.exact_recheck_count = candidates.output.rows.size();

  auto finalized =
      FinalizeCandidateSetForExecutor(candidates.output, authority);
  if (!finalized.ok()) {
    auto refused = Refuse(finalized.recheck.diagnostic.diagnostic_code,
                          "exact_mga_security_recheck_failed");
    refused.evidence.insert(refused.evidence.end(), finalized.evidence.begin(),
                            finalized.evidence.end());
    return refused;
  }
  result.final_row_uuids = std::move(finalized.final_row_uuids);
  result.evidence.insert(result.evidence.end(), finalized.evidence.begin(),
                         finalized.evidence.end());

  Add(&result.evidence,
      "runtime_filter.input_rows=" +
          std::to_string(result.counters.input_rows));
  Add(&result.evidence,
      "runtime_filter.candidate_rows=" +
          std::to_string(result.counters.candidate_rows));
  Add(&result.evidence,
      "runtime_filter.pruned_rows=" +
          std::to_string(result.counters.pruned_rows));
  Add(&result.evidence,
      "runtime_filter.pushed_filter_count=" +
          std::to_string(result.counters.pushed_filter_count));
  Add(&result.evidence,
      "runtime_filter.fallback_count=" +
          std::to_string(result.counters.fallback_count));
  Add(&result.evidence,
      "runtime_filter.exact_recheck_count=" +
          std::to_string(result.counters.exact_recheck_count));
  Add(&result.evidence, "runtime_filter.exact_recheck_required=true");
  Add(&result.evidence, "runtime_filter.mga_visibility_recheck_required=true");
  Add(&result.evidence, "runtime_filter.security_recheck_required=true");
  Add(&result.evidence, "parser_or_reference_finality_or_visibility_authority=false");
  Add(&result.evidence, "client_finality_or_visibility_authority=false");
  Add(&result.evidence, "provider_finality_or_visibility_authority=false");
  Add(&result.evidence,
      "write_ahead_log_finality_or_visibility_authority=false");
  return result;
}

}  // namespace scratchbird::engine::executor
