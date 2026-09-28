// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "nosql_approx_filter_decision.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>

namespace opt = scratchbird::engine::optimizer;
namespace api = scratchbird::engine::internal_api;
using Uuid = scratchbird::core::platform::Uuid;
using Refusal = opt::NoSqlApproxFilterIdentityRefusal;
using scratchbird::tests::FixtureUuid;
static_assert(sizeof(Uuid) == 16);
static_assert(!std::is_constructible_v<Uuid, const char*>);
static_assert(std::is_same_v<decltype(opt::NoSqlApproxFilterDecisionRequest{}.object_uuid), Uuid>);
static_assert(std::is_same_v<decltype(opt::NoSqlApproxFilterBenchmarkInput{}.object_uuid), Uuid>);
static_assert(std::is_same_v<decltype(opt::NoSqlApproxFilterCandidateDecision{}.object_uuid), Uuid>);
static_assert(std::is_same_v<decltype(opt::NoSqlApproxSelectedFilter{}.object_uuid), Uuid>);
static_assert(std::is_same_v<decltype(opt::NoSqlApproxFilterDecisionResult{}.object_uuid), Uuid>);

namespace {
std::size_t checks = 0;
void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::cerr << "native approximate filter: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
bool V7(const Uuid& id) {
  return (id.bytes[6] & 0xf0) == 0x70 && (id.bytes[8] & 0xc0) == 0x80;
}
opt::NoSqlApproxFilterDecisionRequest Request(const Uuid& object) {
  opt::NoSqlApproxFilterDecisionRequest request;
  request.object_uuid = object;
  request.security_context_present = request.security_snapshot_bound = request.grants_proven = true;
  request.engine_mga_authoritative = request.exact_fallback_available = true;
  // Independent family and kind inventory, not read back from production.
  for (const auto family : {api::EngineNoSqlProviderFamily::kKeyValue,
                           api::EngineNoSqlProviderFamily::kDocument,
                           api::EngineNoSqlProviderFamily::kSearch,
                           api::EngineNoSqlProviderFamily::kVector,
                           api::EngineNoSqlProviderFamily::kGraph,
                           api::EngineNoSqlProviderFamily::kTimeSeries}) {
    for (const auto kind : {opt::NoSqlApproxFilterKind::kMinMaxSet,
                           opt::NoSqlApproxFilterKind::kBloom,
                           opt::NoSqlApproxFilterKind::kRangeFilter}) {
      opt::NoSqlApproxFilterBenchmarkInput input;
      input.object_uuid = object;
      input.family = family;
      input.kind = kind;
      input.candidate_id = "local-candidate-" + std::to_string(request.candidates.size());
      input.benchmark_epoch = input.required_benchmark_epoch = 17;
      input.input_rows = 100;
      input.candidate_rows = 20;
      input.pruned_rows = 80;
      input.baseline_cost_units = 100;
      input.filter_cost_units = 10;
      input.exact_fallback_cost_units = 20;
      input.encoded_min = "001";
      input.encoded_max = "999";
      input.predicate_low = "100";
      input.predicate_high = "200";
      input.benchmark_authoritative = input.physical_provider_backed = true;
      input.exact_fallback_available = true;
      request.candidates.push_back(input);
    }
  }
  return request;
}
void NoPartialSelection(const opt::NoSqlApproxFilterDecisionResult& result) {
  Require(!result.ok && result.fail_closed && result.selected_filters.empty(),
          "request refusal published executable filter selections");
  for (const auto& candidate : result.candidate_decisions) {
    Require(!candidate.selected && !candidate.returns_final_rows,
            "request refusal left a candidate marked selected/final");
  }
}
void AllBytesAndExactBinding() {
  const auto fixed = FixtureUuid(2207, 1);
  for (unsigned position = 0; position < 16; ++position) {
    for (unsigned octet = 0; octet < 256; ++octet) {
      auto probe = fixed;
      probe.bytes[position] = static_cast<scratchbird::core::platform::byte>(octet);
      auto request = Request(probe);
      const auto result = opt::EvaluateNoSqlApproxFilterDecision(request);
      Require(request.object_uuid == probe && request.candidates.front().object_uuid == probe,
              "evaluation mutated caller identity");
      if (!V7(probe)) {
        NoPartialSelection(result);
        Require(result.object_uuid == Uuid{} && result.candidate_decisions.empty() &&
                result.identity_refusal == Refusal::kInvalidRequestObject &&
                result.diagnostic_code == "SB-OPT-0001", "invalid request identity was admitted");
      } else {
        Require(result.ok && !result.fail_closed && result.object_uuid == probe &&
                result.identity_refusal == Refusal::kNone &&
                result.candidate_decisions.size() == 18 && result.selected_filters.size() == 18,
                "valid binary identity was lost or rejected");
        for (const auto& selected : result.selected_filters) {
          Require(selected.object_uuid == probe && selected.exact_fallback_required &&
                  selected.row_mga_recheck_required && selected.row_security_recheck_required &&
                  selected.candidate_only, "selection lost exact object or required rechecks");
        }
        for (const auto& candidate : result.candidate_decisions) {
          Require(candidate.object_uuid == probe && candidate.identity_refusal == Refusal::kNone,
                  "candidate result lost exact benchmark scope");
        }
        if (probe != fixed) {
          request = Request(fixed);
          request.object_uuid = probe;
          const auto crossed = opt::EvaluateNoSqlApproxFilterDecision(request);
          NoPartialSelection(crossed);
          Require(crossed.object_uuid == probe && crossed.candidate_decisions.size() == 18,
                  "crossed request lost target or rejection evidence");
          for (const auto& candidate : crossed.candidate_decisions) {
            Require(candidate.object_uuid == fixed && candidate.refused && !candidate.safe &&
                    candidate.identity_refusal == Refusal::kBenchmarkObjectMismatch &&
                    candidate.diagnostic_code == "SB-OPT-0001",
                    "another object's benchmark was accepted or relabeled");
          }
        }
      }
      // A refused benchmark must not prevent a different safe candidate for
      // that same family from being considered; its label cannot rescue it.
      request = Request(fixed);
      request.candidates.front().object_uuid = probe;
      const auto partial = opt::EvaluateNoSqlApproxFilterDecision(request);
      const auto& first = partial.candidate_decisions.front();
      const bool matches = probe == fixed;
      Require(partial.ok && partial.object_uuid == fixed &&
              partial.selected_filters.size() == (matches ? 18U : 17U),
              "safe alternate candidate lost or wrong-object filter selected");
      Require(first.selected == matches && first.refused == !matches &&
              first.object_uuid == (V7(probe) ? probe : Uuid{}),
              "benchmark identity validation or retained evidence is incorrect");
      Require(first.identity_refusal == (matches ? Refusal::kNone : V7(probe)
                  ? Refusal::kBenchmarkObjectMismatch : Refusal::kInvalidBenchmarkObject),
              "benchmark refusal lost exact typed reason");
    }
  }
  NoPartialSelection(opt::EvaluateNoSqlApproxFilterDecision(Request({})));
  auto nil_benchmark = Request(fixed);
  nil_benchmark.candidates.front().object_uuid = {};
  const auto nil_result = opt::EvaluateNoSqlApproxFilterDecision(nil_benchmark);
  Require(nil_result.ok && nil_result.candidate_decisions.front().identity_refusal ==
          Refusal::kInvalidBenchmarkObject, "nil benchmark became wildcard scope");
}

void UnsignedBenefitBoundaries() {
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  constexpr std::array<std::uint64_t, 10> values{0, 1, 2, 31, 63, 64, max / 2,
                                               max / 2 + 1, max - 1, max};
  auto request = Request(FixtureUuid(2207, 2));
  for (const auto baseline : values) {
    for (const auto filter : values) {
      for (const auto fallback : values) {
        for (const auto threshold : {std::uint64_t{0}, std::uint64_t{1}, max}) {
          auto& input = request.candidates.front();
          input.baseline_cost_units = baseline;
          input.filter_cost_units = filter;
          input.exact_fallback_cost_units = fallback;
          request.min_net_benefit_units = threshold;
          // Subtraction-only oracle, independent of production sum/saturation.
          const auto after_filter = baseline >= filter ? baseline - filter : 0;
          const auto benefit = after_filter >= fallback ? after_filter - fallback : 0;
          const auto result = opt::EvaluateNoSqlApproxFilterDecision(request);
          const auto& decision = result.candidate_decisions.front();
          Require(decision.net_benefit_units == benefit, "unsigned overflow fabricated benefit");
          // At max threshold every ordinary alternate is below threshold, so
          // overall refusal must also retract this candidate's selection.
          const bool selected = result.ok && benefit != 0 && benefit >= threshold;
          Require(decision.selected == selected, "unprofitable/overflowed candidate selected");
          if (!result.ok) NoPartialSelection(result);
          if (benefit == 0) {
            Require(decision.diagnostic_code == "SB_NOSQL_APPROX_FILTER.INSUFFICIENT_BENEFIT",
                    "zero threshold permitted a non-beneficial filter");
          }
        }
      }
    }
  }
}

void RequestFailureIsAtomic() {
  auto request = Request(FixtureUuid(2207, 3));
  request.candidates.pop_back(); // Late failure after seventeen qualified candidates.
  auto result = opt::EvaluateNoSqlApproxFilterDecision(request);
  NoPartialSelection(result);
  Require(result.object_uuid == request.object_uuid && result.candidate_decisions.size() == 17 &&
          result.diagnostic_code == "SB_NOSQL_APPROX_FILTER.FAMILY_CANDIDATES_INCOMPLETE",
          "incomplete family evidence disappeared or refusal reason changed");
  const auto evidence = opt::SerializeNoSqlApproxFilterDecisionEvidence(result);
  Require(evidence.find("selected_filters=0") != std::string::npos &&
          evidence.find(",selected=true") == std::string::npos,
          "serialized refusal advertised partial execution selections");
  request = Request(FixtureUuid(2207, 3));
  for (std::size_t i = 15; i < 18; ++i) request.candidates[i].observed_false_negative_ppm = 1;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  NoPartialSelection(result);
  Require(result.candidate_decisions.size() == 18 &&
          result.diagnostic_code == "SB_NOSQL_APPROX_FILTER.NO_SAFE_FILTER_FOR_FAMILY",
          "unsafe final family was silently omitted");
  request.security_context_present = false;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  NoPartialSelection(result);
  Require(result.object_uuid == request.object_uuid && result.candidate_decisions.empty(),
          "authority refusal lost valid target scope or evaluated candidates");
}
}  // namespace

int main() {
  AllBytesAndExactBinding();
  UnsignedBenefitBoundaries();
  RequestFailureIsAtomic();
  std::cout << "native approximate-filter identity: " << checks << " checks PASS\n";
}
