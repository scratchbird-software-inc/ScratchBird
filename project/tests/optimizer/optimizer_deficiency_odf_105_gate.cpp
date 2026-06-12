// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-105 optional SIMD/GPU scoring kernel hook gate.

#include "scoring_kernel_executor.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace agents = scratchbird::core::agents;
namespace gpu = scratchbird::engine::gpu_acceleration;

using Kind = gpu::ScoringKernelKind;
using Request = exec::ScoringKernelExecutionRequest;
using Result = exec::ScoringKernelExecutionResult;
using Values = gpu::ScoringKernelValueBatch;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
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
          "parser_or_reference_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "provider_security_policy_authority=true",
          "provider_redaction_policy_authority=true",
          "provider_recovery_authority=true",
          "provider_page_or_catalog_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-105 evidence leaked forbidden authority or document token");
    }
  }
}

std::vector<Kind> AllKinds() {
  return {Kind::kVectorDistance, Kind::kBm25, Kind::kBitmapIntersection,
          Kind::kTimeAggregate, Kind::kJsonPath, Kind::kGraphMembership};
}

Values ScalarForKind(Kind kind, const gpu::ScoringKernelInputBatch& input) {
  Values out;
  switch (kind) {
    case Kind::kVectorDistance: {
      double sum = 0.0;
      for (std::size_t i = 0; i < input.lhs_values.size(); ++i) {
        const double diff = input.lhs_values[i] - input.rhs_values[i];
        sum += diff * diff;
      }
      out.double_values = {sum};
      break;
    }
    case Kind::kBm25:
      out.double_values.reserve(input.lhs_values.size());
      for (std::size_t i = 0; i < input.lhs_values.size(); ++i) {
        const double length =
            input.document_lengths.empty() ? 0.0 : input.document_lengths[i];
        out.double_values.push_back(input.lhs_values[i] * input.rhs_values[i] +
                                    length);
      }
      break;
    case Kind::kBitmapIntersection: {
      std::set<std::uint64_t> left(input.bitmap_left.begin(),
                                   input.bitmap_left.end());
      for (const auto value : input.bitmap_right) {
        if (left.find(value) != left.end()) {
          out.integer_values.push_back(static_cast<std::int64_t>(value));
        }
      }
      break;
    }
    case Kind::kTimeAggregate: {
      double sum = 0.0;
      double min_value = input.time_values.front();
      double max_value = input.time_values.front();
      for (const auto value : input.time_values) {
        sum += value;
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
      }
      out.double_values = {sum, min_value, max_value};
      out.integer_values = {static_cast<std::int64_t>(input.time_values.size())};
      break;
    }
    case Kind::kJsonPath: {
      const std::string needle = input.json_path_tokens.front() + "=";
      for (const auto& document : input.json_documents) {
        const auto found = document.find(needle);
        if (found == std::string::npos) {
          out.string_values.push_back("");
          out.integer_values.push_back(0);
          continue;
        }
        const auto start = found + needle.size();
        const auto end = document.find(';', start);
        out.string_values.push_back(
            document.substr(start, end == std::string::npos
                                       ? std::string::npos
                                       : end - start));
        out.integer_values.push_back(1);
      }
      break;
    }
    case Kind::kGraphMembership: {
      std::set<std::pair<std::uint64_t, std::uint64_t>> edges;
      for (std::size_t i = 0; i < input.graph_sources.size(); ++i) {
        edges.insert({input.graph_sources[i], input.graph_targets[i]});
      }
      for (std::size_t i = 0; i < input.query_sources.size(); ++i) {
        out.integer_values.push_back(
            edges.find({input.query_sources[i], input.query_targets[i]}) !=
                    edges.end()
                ? 1
                : 0);
      }
      break;
    }
    case Kind::kUnknown:
      break;
  }
  return out;
}

bool SameValues(const Values& left, const Values& right) {
  return left.double_values == right.double_values &&
         left.integer_values == right.integer_values &&
         left.string_values == right.string_values;
}

struct ProviderFixture {
  bool runtime_cancelled = false;
  bool runtime_fail = false;
  bool runtime_identity_mismatch = false;
  bool result_mismatch = false;
  bool runtime_throws = false;
  std::vector<Kind> supported_kinds = AllKinds();
};

gpu::ScoringKernelProvider Provider(ProviderFixture fixture = {}) {
  gpu::ScoringKernelProvider provider;
  provider.manifest.provider_id = "odf105.safe_scoring_provider";
  provider.manifest.engine_abi_id = "sb_engine_abi_v3";
  provider.manifest.runtime_identity_id = "odf105.runtime.identity.1";
  provider.manifest.architecture = "x86_64";
  provider.manifest.cpu_capabilities = {"sse2", "simd_scoring"};
  provider.manifest.gpu_capabilities = {"deterministic_gpu_scoring"};
  provider.manifest.supported_kinds = fixture.supported_kinds;
  provider.manifest.safe_to_execute = true;
  provider.run = [fixture](const Request& request) {
    gpu::ScoringKernelProviderOutcome outcome;
    if (fixture.runtime_throws) {
      throw std::runtime_error("odf105_runtime_exception");
    }
    if (fixture.runtime_cancelled) {
      outcome.cancelled_or_quota = true;
      outcome.diagnostic_detail = "odf105_runtime_cancelled";
      return outcome;
    }
    if (fixture.runtime_fail) {
      outcome.ok = false;
      outcome.diagnostic_detail = "odf105_runtime_failed";
      return outcome;
    }
    outcome.ok = true;
    outcome.kernel_id =
        std::string("odf105.kernel.") +
        gpu::ScoringKernelKindName(request.descriptor.kind);
    outcome.descriptor_generation =
        request.descriptor.descriptor_generation +
        (fixture.runtime_identity_mismatch ? 1 : 0);
    outcome.runtime_identity_id =
        fixture.runtime_identity_mismatch
            ? "odf105.runtime.identity.changed"
            : request.provider.manifest.runtime_identity_id;
    outcome.values = ScalarForKind(request.descriptor.kind, request.input);
    if (fixture.result_mismatch) {
      if (!outcome.values.integer_values.empty()) {
        ++outcome.values.integer_values.front();
      } else if (!outcome.values.double_values.empty()) {
        outcome.values.double_values.front() += 1.0;
      } else if (!outcome.values.string_values.empty()) {
        outcome.values.string_values.front() += "-mismatch";
      }
    }
    return outcome;
  };
  return provider;
}

Request BaseRequest(Kind kind) {
  Request request;
  request.descriptor.kind = kind;
  request.descriptor.descriptor_id =
      std::string("odf105.descriptor.") + gpu::ScoringKernelKindName(kind);
  request.descriptor.kernel_digest =
      std::string("odf105.digest.") + gpu::ScoringKernelKindName(kind);
  request.descriptor.descriptor_generation = 17;
  request.descriptor.expected_descriptor_generation = 17;
  request.capabilities.required_engine_abi_id = "sb_engine_abi_v3";
  request.capabilities.required_runtime_identity_id =
      "odf105.runtime.identity.1";
  request.capabilities.required_architecture = "x86_64";
  request.capabilities.required_cpu_capabilities = {"sse2"};
  request.capabilities.required_gpu_capabilities = {
      "deterministic_gpu_scoring"};
  request.input.batch_id =
      std::string("odf105.batch.") + gpu::ScoringKernelKindName(kind);
  request.input.materialized_memory_bytes = 4096;
  request.input.materialized_authorized_batch = true;
  request.input.safe_materialized_batch = true;
  switch (kind) {
    case Kind::kVectorDistance:
      request.input.lhs_values = {1.0, 5.0, 9.0};
      request.input.rhs_values = {3.0, 1.0, 8.0};
      request.input.row_count = request.input.lhs_values.size();
      break;
    case Kind::kBm25:
      request.input.lhs_values = {2.0, 3.0, 4.0};
      request.input.rhs_values = {7.0, 11.0, 13.0};
      request.input.document_lengths = {5.0, 6.0, 7.0};
      request.input.row_count = request.input.lhs_values.size();
      break;
    case Kind::kBitmapIntersection:
      request.input.bitmap_left = {1, 3, 5, 8};
      request.input.bitmap_right = {2, 3, 5, 13};
      request.input.row_count =
          request.input.bitmap_left.size() + request.input.bitmap_right.size();
      break;
    case Kind::kTimeAggregate:
      request.input.time_values = {10.0, 30.0, 20.0, 40.0};
      request.input.row_count = request.input.time_values.size();
      break;
    case Kind::kJsonPath:
      request.input.json_documents = {"a=7;b=9", "b=3", "a=11;c=1"};
      request.input.json_path_tokens = {"a"};
      request.input.row_count = request.input.json_documents.size();
      break;
    case Kind::kGraphMembership:
      request.input.graph_sources = {1, 1, 2, 3};
      request.input.graph_targets = {2, 3, 4, 4};
      request.input.query_sources = {1, 2, 4};
      request.input.query_targets = {3, 4, 1};
      request.input.row_count = request.input.query_sources.size();
      break;
    case Kind::kUnknown:
      break;
  }
  request.scalar_reference = [kind](const gpu::ScoringKernelInputBatch& input) {
    return ScalarForKind(kind, input);
  };
  request.provider = Provider();
  request.max_rows = 64;
  request.max_memory_bytes = 64 * 1024;
  request.resource_governance.operation_id =
      std::string("odf105.scoring.") + gpu::ScoringKernelKindName(kind);
  request.resource_governance.descriptor.descriptor_id =
      "odf106.scoring_kernel.runtime_quota";
  request.resource_governance.descriptor.family =
      agents::ResourceGovernanceFamily::kScoringKernelAccelerator;
  request.resource_governance.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.resource_governance.descriptor.source_path_or_label =
      "runtime.policy.odf106.scoring_kernel";
  request.resource_governance.descriptor.descriptor_generation = 105;
  request.resource_governance.descriptor.expected_generation = 105;
  request.resource_governance.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kExactScalarFallback;
  request.resource_governance.descriptor.benchmark_clean = true;
  request.resource_governance.descriptor.runtime_dependency_present = true;
  request.resource_governance.descriptor.limits = {
      65536, 65536, 4096, 4096, 16, 2, 16, 64, 8, 64, 8, 2, 1000000};
  request.resource_governance.requested = {
      static_cast<std::int64_t>(request.input.materialized_memory_bytes),
      static_cast<std::int64_t>(request.input.materialized_memory_bytes),
      256,
      0,
      0,
      1,
      1,
      static_cast<std::int64_t>(request.input.row_count),
      1,
      static_cast<std::int64_t>(request.input.row_count),
      1,
      1,
      1000};
  request.resource_governance.require_exact_scalar_fallback_available = true;
  request.resource_governance.exact_scalar_fallback_available = true;
  return request;
}

void ExpectFallback(const Request& request,
                    std::string_view diagnostic_code,
                    std::string_view message) {
  const auto result = exec::ExecuteOptionalScoringKernel(request);
  Require(result.ok && result.scalar_fallback_used && !result.accelerator_used,
          message);
  Require(result.diagnostic_code == diagnostic_code,
          "ODF-105 fallback diagnostic changed");
  Require(SameValues(result.values,
                     ScalarForKind(request.descriptor.kind, request.input)),
          "ODF-105 scalar fallback result changed");
  Require(EvidenceHas(result.evidence,
                      "scoring_kernel.route=scalar_fallback"),
          "ODF-105 scalar fallback evidence missing");
  Require(EvidenceHas(result.evidence, "executor.scoring_kernel_route=odf105"),
          "ODF-105 executor route evidence missing");
  Require(EvidenceHas(result.evidence, "parser_or_reference_authority=false"),
          "ODF-105 parser/reference non-authority evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void ExpectRefusal(const Request& request,
                   std::string_view diagnostic_code,
                   std::string_view message) {
  const auto result = exec::ExecuteOptionalScoringKernel(request);
  Require(!result.ok && result.fail_closed, message);
  Require(result.diagnostic_code == diagnostic_code,
          "ODF-105 refusal diagnostic changed");
  Require(EvidenceHas(result.evidence, "scoring_kernel.route=refused"),
          "ODF-105 refusal evidence missing");
  Require(EvidenceHas(result.evidence, "executor.scoring_kernel_route=odf105"),
          "ODF-105 executor refusal route evidence missing");
  Require(EvidenceHas(result.evidence, "parser_or_reference_authority=false"),
          "ODF-105 refusal parser/reference non-authority evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void EachKernelFamilyUsesAcceleratorRoute() {
  for (const auto kind : AllKinds()) {
    const auto request = BaseRequest(kind);
    const auto result = exec::ExecuteOptionalScoringKernel(request);
    Require(result.ok, "ODF-105 accelerator route failed");
    Require(result.accelerator_used && !result.scalar_fallback_used,
            "ODF-105 accelerator route was not selected");
    Require(result.diagnostic_code == "SB_SCORING_KERNEL.OK",
            "ODF-105 accelerator success diagnostic changed");
    Require(SameValues(result.values, ScalarForKind(kind, request.input)),
            "ODF-105 accelerator result differs from scalar");
    Require(EvidenceHas(result.evidence, "scoring_kernel.route=accelerator"),
            "ODF-105 accelerator route evidence missing");
    Require(EvidenceHas(result.evidence,
                        "scoring_kernel.scalar_equivalence=verified"),
            "ODF-105 scalar equivalence evidence missing");
    Require(EvidenceHas(result.evidence, "executor.scalar_reference_authority=true"),
            "ODF-105 executor scalar authority evidence missing");
    RequireEvidenceHygiene(result.evidence);
  }
}

void ColdMissingUnsupportedCapabilityQuotaAndCancellationFallback() {
  auto cold = BaseRequest(Kind::kVectorDistance);
  cold.minimum_rows_for_acceleration = cold.input.row_count + 1;
  ExpectFallback(cold, "SB_SCORING_KERNEL.COLD_BATCH_FALLBACK",
                 "ODF-105 cold batch did not use scalar fallback");

  auto missing = BaseRequest(Kind::kBm25);
  missing.provider = {};
  ExpectFallback(missing, "SB_SCORING_KERNEL.PROVIDER_MISSING_FALLBACK",
                 "ODF-105 provider-missing path did not use scalar fallback");

  auto unsupported = BaseRequest(Kind::kBitmapIntersection);
  ProviderFixture unsupported_fixture;
  unsupported_fixture.supported_kinds = {Kind::kVectorDistance};
  unsupported.provider = Provider(unsupported_fixture);
  ExpectFallback(unsupported, "SB_SCORING_KERNEL.UNSUPPORTED_KIND_FALLBACK",
                 "ODF-105 unsupported kernel did not use scalar fallback");

  auto capability = BaseRequest(Kind::kTimeAggregate);
  capability.capabilities.required_gpu_capabilities = {"missing_gpu_feature"};
  ExpectFallback(capability, "SB_SCORING_KERNEL.CAPABILITY_MISMATCH_FALLBACK",
                 "ODF-105 capability mismatch did not use scalar fallback");

  auto quota = BaseRequest(Kind::kJsonPath);
  quota.max_rows = 1;
  ExpectFallback(quota, "SB_SCORING_KERNEL.CANCELLED_OR_QUOTA_FALLBACK",
                 "ODF-105 quota exceeded did not use scalar fallback");

  auto explicit_quota = BaseRequest(Kind::kJsonPath);
  explicit_quota.quota_exhausted = true;
  ExpectFallback(explicit_quota,
                 "SB_SCORING_KERNEL.CANCELLED_OR_QUOTA_FALLBACK",
                 "ODF-105 explicit quota exhaustion did not use scalar fallback");

  auto cancelled = BaseRequest(Kind::kGraphMembership);
  cancelled.cancellation_requested = true;
  ExpectFallback(cancelled, "SB_SCORING_KERNEL.CANCELLED_OR_QUOTA_FALLBACK",
                 "ODF-105 cancellation did not use scalar fallback");

  auto runtime_cancelled = BaseRequest(Kind::kVectorDistance);
  runtime_cancelled.provider = Provider({true});
  ExpectFallback(runtime_cancelled,
                 "SB_SCORING_KERNEL.CANCELLED_OR_QUOTA_FALLBACK",
                 "ODF-105 runtime cancellation did not use scalar fallback");

  auto runtime_failed = BaseRequest(Kind::kBm25);
  runtime_failed.provider = Provider({false, true});
  ExpectFallback(runtime_failed, "SB_SCORING_KERNEL.PROVIDER_RUNTIME_FALLBACK",
                 "ODF-105 provider runtime failure did not use scalar fallback");
}

void UnsafeAuthorityAndRawAccessRefuseClosed() {
  auto unsafe_manifest = BaseRequest(Kind::kVectorDistance);
  unsafe_manifest.provider.manifest.safe_to_execute = false;
  ExpectRefusal(unsafe_manifest, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 unsafe provider manifest was accepted");

  auto finality = BaseRequest(Kind::kVectorDistance);
  finality.provider.manifest.claims_transaction_finality_authority = true;
  ExpectRefusal(finality, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 finality authority claim was accepted");

  auto visibility = BaseRequest(Kind::kBm25);
  visibility.provider.manifest.claims_visibility_authority = true;
  ExpectRefusal(visibility, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 visibility authority claim was accepted");

  auto security = BaseRequest(Kind::kBitmapIntersection);
  security.provider.manifest.claims_security_policy_authority = true;
  ExpectRefusal(security, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 security authority claim was accepted");

  auto redaction = BaseRequest(Kind::kBitmapIntersection);
  redaction.provider.manifest.claims_redaction_policy_authority = true;
  ExpectRefusal(redaction, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 redaction authority claim was accepted");

  auto parser = BaseRequest(Kind::kTimeAggregate);
  parser.provider.manifest.claims_parser_or_reference_authority = true;
  ExpectRefusal(parser, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 parser/reference authority claim was accepted");

  auto recovery = BaseRequest(Kind::kJsonPath);
  recovery.provider.manifest.claims_recovery_authority = true;
  ExpectRefusal(recovery, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 recovery authority claim was accepted");

  auto catalog = BaseRequest(Kind::kGraphMembership);
  catalog.provider.manifest.claims_page_or_catalog_authority = true;
  ExpectRefusal(catalog, "SB_SCORING_KERNEL.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-105 page/catalog authority claim was accepted");

  auto direct_page = BaseRequest(Kind::kVectorDistance);
  direct_page.input.direct_page_or_catalog_access = true;
  ExpectRefusal(direct_page, "SB_SCORING_KERNEL.UNSAFE_PAGE_CATALOG_ACCESS",
                "ODF-105 direct page/catalog input was accepted");

  auto raw_parser = BaseRequest(Kind::kBm25);
  raw_parser.input.raw_client_or_parser_input = true;
  ExpectRefusal(raw_parser, "SB_SCORING_KERNEL.RAW_INPUT_REFUSED",
                "ODF-105 raw parser/client input was accepted");
}

void CorruptDescriptorInputAndMissingScalarRefuseClosed() {
  auto corrupt_descriptor = BaseRequest(Kind::kVectorDistance);
  corrupt_descriptor.descriptor.kind = Kind::kUnknown;
  ExpectRefusal(corrupt_descriptor,
                "SB_SCORING_KERNEL.CORRUPT_DESCRIPTOR_REFUSED",
                "ODF-105 corrupt descriptor was accepted");

  auto missing_scalar = BaseRequest(Kind::kBm25);
  missing_scalar.scalar_reference = {};
  ExpectRefusal(missing_scalar, "SB_SCORING_KERNEL.SCALAR_REFERENCE_REQUIRED",
                "ODF-105 missing scalar reference was accepted");

  auto non_deterministic = BaseRequest(Kind::kBitmapIntersection);
  non_deterministic.descriptor.deterministic_equivalence_provable = false;
  ExpectRefusal(non_deterministic, "SB_SCORING_KERNEL.DETERMINISM_REQUIRED",
                "ODF-105 non-deterministic descriptor was accepted");

  auto determinism_not_required = BaseRequest(Kind::kTimeAggregate);
  determinism_not_required.descriptor.deterministic_equivalence_required = false;
  ExpectRefusal(determinism_not_required,
                "SB_SCORING_KERNEL.DETERMINISM_REQUIRED",
                "ODF-105 optional deterministic-equivalence descriptor was accepted");

  auto corrupt_input = BaseRequest(Kind::kJsonPath);
  corrupt_input.input.row_count += 1;
  ExpectRefusal(corrupt_input, "SB_SCORING_KERNEL.CORRUPT_INPUT_REFUSED",
                "ODF-105 corrupt input batch was accepted");

  auto unsafe_batch = BaseRequest(Kind::kGraphMembership);
  unsafe_batch.input.materialized_authorized_batch = false;
  ExpectRefusal(unsafe_batch, "SB_SCORING_KERNEL.CORRUPT_INPUT_REFUSED",
                "ODF-105 unauthorized materialized batch was accepted");
}

void RuntimeIdentityAndResultMismatchRefuseClosed() {
  auto runtime = BaseRequest(Kind::kTimeAggregate);
  runtime.provider =
      Provider({false, false, true, false, false, AllKinds()});
  ExpectRefusal(runtime,
                "SB_SCORING_KERNEL.RUNTIME_IDENTITY_MISMATCH_REFUSED",
                "ODF-105 runtime identity mismatch was accepted");

  auto runtime_throw = BaseRequest(Kind::kVectorDistance);
  runtime_throw.provider =
      Provider({false, false, false, false, true, AllKinds()});
  ExpectRefusal(runtime_throw, "SB_SCORING_KERNEL.RUNTIME_MISMATCH_REFUSED",
                "ODF-105 runtime exception was accepted");

  auto mismatch = BaseRequest(Kind::kGraphMembership);
  mismatch.provider =
      Provider({false, false, false, true, false, AllKinds()});
  ExpectRefusal(mismatch, "SB_SCORING_KERNEL.RESULT_MISMATCH_REFUSED",
                "ODF-105 accelerator/scalar mismatch was accepted");
}

}  // namespace

int main() {
  EachKernelFamilyUsesAcceleratorRoute();
  ColdMissingUnsupportedCapabilityQuotaAndCancellationFallback();
  UnsafeAuthorityAndRawAccessRefuseClosed();
  CorruptDescriptorInputAndMissingScalarRefuseClosed();
  RuntimeIdentityAndResultMismatchRefuseClosed();
  return EXIT_SUCCESS;
}
