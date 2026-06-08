// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-104 optional SBLR/native specialization gate.

#include "sblr_native_specialization.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace native = scratchbird::engine::native_compile;
namespace sblr = scratchbird::engine::sblr;
namespace agents = scratchbird::core::agents;

using Kind = native::NativeSblrSpecializationKind;
using Request = native::NativeSblrSpecializationRequest;
using Result = native::NativeSblrSpecializationResult;

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
          "parser_or_donor_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "provider_security_policy_authority=true",
          "provider_redaction_policy_authority=true",
          "write_after_stream_finality_or_recovery_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-104 evidence leaked forbidden authority or document token");
    }
  }
}

native::NativeSblrValueBatch ScalarForKind(
    Kind kind,
    const native::NativeSblrInputBatch& input) {
  native::NativeSblrValueBatch out;
  if (kind == Kind::kAggregate) {
    std::int64_t sum = 0;
    for (const auto value : input.values) {
      sum += value;
    }
    out.values = {sum};
    return out;
  }
  out.values.reserve(input.values.size());
  for (const auto value : input.values) {
    switch (kind) {
      case Kind::kPredicate:
        out.values.push_back((value % 2) == 0 ? 1 : 0);
        break;
      case Kind::kProjection:
        out.values.push_back(value * 3 + 1);
        break;
      case Kind::kRowDecode:
        out.values.push_back(value + 100);
        break;
      case Kind::kPathExtraction:
        out.values.push_back(value % 10);
        break;
      case Kind::kDistanceScoring:
        out.values.push_back(value >= 50 ? value - 50 : 50 - value);
        break;
      case Kind::kAggregate:
      case Kind::kUnknown:
        break;
    }
  }
  return out;
}

std::vector<Kind> AllKinds() {
  return {Kind::kPredicate, Kind::kProjection, Kind::kRowDecode,
          Kind::kPathExtraction, Kind::kDistanceScoring, Kind::kAggregate};
}

struct ProviderFixture {
  bool compile_fail = false;
  bool compile_cancelled = false;
  bool runtime_fail = false;
  bool runtime_generation_mismatch = false;
  bool result_mismatch = false;
  bool compile_throws = false;
  bool runtime_throws = false;
  std::vector<Kind> supported_kinds = AllKinds();
};

native::NativeSblrProvider Provider(ProviderFixture fixture = {}) {
  native::NativeSblrProvider provider;
  provider.manifest.provider_id = "odf104.safe_native_provider";
  provider.manifest.engine_abi_id = "sb_engine_abi_v3";
  provider.manifest.architecture = "x86_64";
  provider.manifest.cpu_capabilities = {"sse2", "sblr_scalar_exact"};
  provider.manifest.supported_kinds = fixture.supported_kinds;
  provider.manifest.safe_to_execute = true;
  provider.compile = [fixture](const Request& request) {
    native::NativeSblrCompileOutcome outcome;
    if (fixture.compile_throws) {
      throw std::runtime_error("odf104_compile_exception");
    }
    if (fixture.compile_cancelled) {
      outcome.cancelled_or_quota = true;
      outcome.diagnostic_detail = "odf104_compile_quota";
      return outcome;
    }
    if (fixture.compile_fail) {
      outcome.ok = false;
      outcome.diagnostic_code = "ODF104.COMPILE_FAIL";
      outcome.diagnostic_detail = "odf104_compile_failed";
      return outcome;
    }
    outcome.ok = true;
    outcome.kernel_id =
        std::string("odf104.kernel.") +
        native::NativeSblrSpecializationKindName(request.kind) + "." +
        std::to_string(request.identity.template_generation);
    return outcome;
  };
  provider.run = [fixture](const Request& request,
                           const native::NativeSblrCompileOutcome& compile) {
    native::NativeSblrKernelOutcome outcome;
    if (fixture.runtime_throws) {
      throw std::runtime_error("odf104_runtime_exception");
    }
    if (fixture.runtime_fail) {
      outcome.ok = false;
      outcome.diagnostic_detail = "odf104_runtime_failed";
      return outcome;
    }
    outcome.ok = true;
    outcome.kernel_id = compile.kernel_id;
    outcome.template_generation =
        request.identity.template_generation +
        (fixture.runtime_generation_mismatch ? 1 : 0);
    outcome.security_epoch = request.epochs.security_epoch;
    outcome.redaction_epoch = request.epochs.redaction_epoch;
    outcome.values = ScalarForKind(request.kind, request.input);
    if (fixture.result_mismatch && !outcome.values.values.empty()) {
      ++outcome.values.values.front();
    }
    return outcome;
  };
  return provider;
}

Request BaseRequest(Kind kind) {
  Request request;
  request.kind = kind;
  request.identity.stable_template_id =
      std::string("odf104.stable_template.") +
      native::NativeSblrSpecializationKindName(kind);
  request.identity.sblr_digest =
      std::string("odf104.sblr_digest.") +
      native::NativeSblrSpecializationKindName(kind);
  request.identity.plan_node_id = "odf104.plan_node";
  request.identity.template_generation = 42;
  request.identity.expected_template_generation = 42;
  request.epochs.security_epoch = 7;
  request.epochs.expected_security_epoch = 7;
  request.epochs.redaction_epoch = 8;
  request.epochs.expected_redaction_epoch = 8;
  request.hotness.observed_invocations = 25;
  request.hotness.observed_rows = 1024;
  request.hotness.observed_expressions = 2048;
  request.hotness.minimum_invocations = 10;
  request.hotness.minimum_rows = 512;
  request.hotness.minimum_expressions = 512;
  request.capabilities.required_engine_abi_id = "sb_engine_abi_v3";
  request.capabilities.required_architecture = "x86_64";
  request.capabilities.required_cpu_capabilities = {"sse2"};
  request.input.values = {2, 11, 50, 63};
  request.input.row_count = request.input.values.size();
  request.scalar_reference = [kind](const native::NativeSblrInputBatch& input) {
    return ScalarForKind(kind, input);
  };
  request.provider = Provider();
  request.sblr_module_payload =
      std::string("sblr native specialization ") +
      native::NativeSblrSpecializationKindName(kind);
  request.resource_governance.operation_id =
      std::string("odf104.native.") +
      native::NativeSblrSpecializationKindName(kind);
  request.resource_governance.descriptor.descriptor_id =
      "odf106.native_sblr.runtime_quota";
  request.resource_governance.descriptor.family =
      agents::ResourceGovernanceFamily::kPreparedNativeSpecialization;
  request.resource_governance.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.resource_governance.descriptor.source_path_or_label =
      "runtime.policy.odf106.native_sblr";
  request.resource_governance.descriptor.descriptor_generation = 104;
  request.resource_governance.descriptor.expected_generation = 104;
  request.resource_governance.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kExactScalarFallback;
  request.resource_governance.descriptor.benchmark_clean = true;
  request.resource_governance.descriptor.runtime_dependency_present = true;
  request.resource_governance.descriptor.limits = {
      65536, 32768, 4096, 4096, 16, 2, 16, 64, 8, 64, 8, 2, 1000000};
  request.resource_governance.requested = {
      4096,
      4096,
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

void RequireValues(const native::NativeSblrValueBatch& got,
                   const native::NativeSblrValueBatch& expected,
                   std::string_view message) {
  Require(got.values == expected.values, message);
}

void ExpectFallback(const Request& request,
                    std::string_view diagnostic_code,
                    std::string_view message) {
  const auto result = sblr::ExecuteSblrNativeSpecialization(request);
  Require(result.ok && result.scalar_fallback_used && !result.native_used,
          message);
  Require(result.diagnostic_code == diagnostic_code,
          "ODF-104 fallback diagnostic changed");
  RequireValues(result.values, ScalarForKind(request.kind, request.input),
                "ODF-104 scalar fallback result changed");
  Require(EvidenceHas(result.evidence, "native_sblr.route=scalar_fallback"),
          "ODF-104 scalar fallback evidence missing");
  Require(EvidenceHas(result.evidence, "parser_or_donor_authority=false"),
          "ODF-104 parser/donor non-authority evidence missing");
  Require(EvidenceHas(result.evidence,
                      "write_after_stream_finality_or_recovery_authority=false"),
          "ODF-104 write-after non-authority evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void ExpectRefusal(const Request& request,
                   std::string_view diagnostic_code,
                   std::string_view message) {
  const auto result = sblr::ExecuteSblrNativeSpecialization(request);
  Require(!result.ok && result.fail_closed, message);
  Require(result.diagnostic_code == diagnostic_code,
          "ODF-104 refusal diagnostic changed");
  Require(EvidenceHas(result.evidence, "native_sblr.route=refused"),
          "ODF-104 refusal evidence missing");
  Require(EvidenceHas(result.evidence, "parser_or_donor_authority=false"),
          "ODF-104 refusal parser/donor non-authority evidence missing");
  Require(EvidenceHas(result.evidence,
                      "write_after_stream_finality_or_recovery_authority=false"),
          "ODF-104 refusal write-after non-authority evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void HotStableTemplatesUseNativeRouteForEveryKind() {
  for (const auto kind : AllKinds()) {
    const auto request = BaseRequest(kind);
    const auto result = sblr::ExecuteSblrNativeSpecialization(request);
    Require(result.ok, "ODF-104 hot stable native route failed");
    Require(result.native_used && !result.scalar_fallback_used,
            "ODF-104 hot stable template did not use native route");
    Require(result.diagnostic_code == "SB_NATIVE_SBLR.OK",
            "ODF-104 native success diagnostic changed");
    RequireValues(result.values, ScalarForKind(kind, request.input),
                  "ODF-104 native result differs from scalar");
    Require(EvidenceHas(result.evidence, "native_sblr.route=native"),
            "ODF-104 native route evidence missing");
    Require(EvidenceHas(result.evidence, "native_sblr.scalar_equivalence=verified"),
            "ODF-104 scalar equivalence evidence missing");
    Require(EvidenceHas(result.evidence, "parser_or_donor_authority=false"),
            "ODF-104 native parser/donor non-authority evidence missing");
    Require(EvidenceHas(result.evidence,
                        "write_after_stream_finality_or_recovery_authority=false"),
            "ODF-104 native write-after non-authority evidence missing");
    RequireEvidenceHygiene(result.evidence);
  }
}

void ColdAndUnstableTemplatesUseExactScalarFallback() {
  auto cold = BaseRequest(Kind::kPredicate);
  cold.hotness.observed_invocations = 0;
  cold.hotness.observed_rows = 0;
  cold.hotness.observed_expressions = 0;
  ExpectFallback(cold, "SB_NATIVE_SBLR.COLD_TEMPLATE_FALLBACK",
                 "ODF-104 cold template was not scalar fallback");

  auto stale = BaseRequest(Kind::kProjection);
  stale.identity.expected_template_generation = 41;
  ExpectFallback(stale, "SB_NATIVE_SBLR.STALE_TEMPLATE_GENERATION_FALLBACK",
                 "ODF-104 stale template was not scalar fallback");

  auto epoch = BaseRequest(Kind::kRowDecode);
  epoch.epochs.expected_redaction_epoch = 9;
  ExpectFallback(
      epoch, "SB_NATIVE_SBLR.UNSTABLE_SECURITY_REDACTION_EPOCH_FALLBACK",
      "ODF-104 unstable security/redaction epoch was not scalar fallback");
}

void StaleCorruptAndMissingScalarInputsFailSafely() {
  auto corrupt = BaseRequest(Kind::kPredicate);
  corrupt.kind = static_cast<Kind>(999);
  ExpectRefusal(corrupt, "SB_NATIVE_SBLR.CORRUPT_INPUT_REFUSED",
                "ODF-104 corrupt specialization enum was accepted");

  auto corrupt_batch = BaseRequest(Kind::kProjection);
  corrupt_batch.input.row_count = corrupt_batch.input.values.size() + 1;
  ExpectRefusal(corrupt_batch, "SB_NATIVE_SBLR.CORRUPT_INPUT_REFUSED",
                "ODF-104 corrupt input batch was accepted");

  auto missing_scalar = BaseRequest(Kind::kAggregate);
  missing_scalar.scalar_reference = {};
  ExpectRefusal(missing_scalar, "SB_NATIVE_SBLR.SCALAR_REFERENCE_REQUIRED",
                "ODF-104 missing scalar reference was accepted");
}

void UnsupportedCapabilityCancellationAndCompileFailureFallback() {
  auto unsupported = BaseRequest(Kind::kAggregate);
  ProviderFixture unsupported_fixture;
  unsupported_fixture.supported_kinds = {Kind::kPredicate};
  unsupported.provider = Provider(unsupported_fixture);
  ExpectFallback(unsupported, "SB_NATIVE_SBLR.UNSUPPORTED_KIND_FALLBACK",
                 "ODF-104 unsupported specialization kind did not fallback");

  auto capability = BaseRequest(Kind::kDistanceScoring);
  capability.capabilities.required_cpu_capabilities = {"avx512_odf104"};
  ExpectFallback(capability,
                 "SB_NATIVE_SBLR.ABI_CPU_CAPABILITY_MISMATCH_FALLBACK",
                 "ODF-104 capability mismatch did not fallback");

  auto cancelled = BaseRequest(Kind::kPathExtraction);
  cancelled.quota_exhausted = true;
  ExpectFallback(cancelled, "SB_NATIVE_SBLR.CANCELLED_OR_QUOTA_FALLBACK",
                 "ODF-104 quota/cancellation did not fallback");

  auto compile_failed = BaseRequest(Kind::kProjection);
  compile_failed.provider = Provider({true});
  ExpectFallback(compile_failed, "SB_NATIVE_SBLR.COMPILE_FAILED_FALLBACK",
                 "ODF-104 compile failure did not fallback");

  auto compile_threw = BaseRequest(Kind::kProjection);
  compile_threw.provider =
      Provider({false, false, false, false, false, true});
  ExpectFallback(compile_threw, "SB_NATIVE_SBLR.COMPILE_FAILED_FALLBACK",
                 "ODF-104 compile exception did not fallback");
}

void UnsafeProviderAndRuntimeMismatchRefuseClosed() {
  auto unsafe = BaseRequest(Kind::kPredicate);
  unsafe.provider.manifest.claims_transaction_finality_authority = true;
  ExpectRefusal(unsafe, "SB_NATIVE_SBLR.UNSAFE_PROVIDER_AUTHORITY",
                "ODF-104 unsafe provider authority claim was accepted");

  auto runtime = BaseRequest(Kind::kRowDecode);
  runtime.provider = Provider({false, false, false, true});
  ExpectRefusal(runtime, "SB_NATIVE_SBLR.RUNTIME_MISMATCH_REFUSED",
                "ODF-104 runtime generation mismatch was accepted");

  auto runtime_threw = BaseRequest(Kind::kAggregate);
  runtime_threw.provider =
      Provider({false, false, false, false, false, false, true});
  ExpectRefusal(runtime_threw, "SB_NATIVE_SBLR.RUNTIME_MISMATCH_REFUSED",
                "ODF-104 runtime exception was accepted");
}

void ResultMismatchFailsClosed() {
  auto mismatch = BaseRequest(Kind::kDistanceScoring);
  mismatch.provider = Provider({false, false, false, false, true});
  ExpectRefusal(mismatch, "SB_NATIVE_SBLR.RESULT_MISMATCH_REFUSED",
                "ODF-104 native/scalar mismatch was accepted");
}

}  // namespace

int main() {
  HotStableTemplatesUseNativeRouteForEveryKind();
  ColdAndUnstableTemplatesUseExactScalarFallback();
  StaleCorruptAndMissingScalarInputsFailSafely();
  UnsupportedCapabilityCancellationAndCompileFailureFallback();
  UnsafeProviderAndRuntimeMismatchRefuseClosed();
  ResultMismatchFailsClosed();
  return EXIT_SUCCESS;
}
