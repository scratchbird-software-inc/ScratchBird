// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "optimizer_request.hpp"
#include "optimizer_explain.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
using Uuid = scratchbird::core::platform::Uuid;
using Refusal = opt::OptimizerRequestIdentityRefusal;
using scratchbird::tests::FixtureUuid;
static_assert(sizeof(Uuid) == 16);
static_assert(!std::is_constructible_v<Uuid, std::string>);
static_assert(std::is_same_v<decltype(opt::OptimizerRequestContext{}.request_uuid), Uuid>);
static_assert(std::is_same_v<decltype(opt::OptimizerExplainDocument{}.request_uuid), Uuid>);

namespace {
std::size_t checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::cerr << "native optimizer request: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
bool V7(const Uuid& id) {
  return (id.bytes[6] & 0xf0) == 0x70 && (id.bytes[8] & 0xc0) == 0x80;
}
std::string Presentation(const Uuid& id) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (unsigned i = 0; i != 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
    text += digits[id.bytes[i] >> 4];
    text += digits[id.bytes[i] & 15];
  }
  return text;
}
opt::BoundOptimizerRequest Request(const Uuid& id) {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = id;
  request.context.operation_id = "dml.select_rows";
  request.context.sblr_digest = "sblr:bound-select";
  request.context.descriptor_set_digest = "descriptor:bound-row";
  request.context.statistics_snapshot_id = "stats:fixture";
  request.context.executor_capability_set_id = "executor:fixture";
  request.context.catalog_epoch = request.context.security_epoch = request.context.policy_epoch = 1;
  request.context.security_context_present = request.context.transaction_context_present = true;
  request.logical_plan.ok = true;
  request.logical_plan.plan_id = "fixture-plan";
  request.logical_plan.nodes.push_back(planner::MakeLogicalPlanNode(
      planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kTableScan,
      "dml.select_rows", "scan"));
  request.statistics = opt::DefaultLocalStatisticsCatalog();
  return request;
}
void CheckRefusal(const opt::BoundOptimizerRequest& request, Refusal reason,
                  const opt::BoundOptimizerResult& unrelated_success) {
  const auto validation = opt::ValidateBoundOptimizerRequest(request);
  Require(!validation.ok && validation.request_identity_refusal == reason,
          "invalid request identity passed validation or lost reason");
  Require(std::find(validation.diagnostics.begin(), validation.diagnostics.end(), "SB-OPT-0001") !=
          validation.diagnostics.end(), "registered identity diagnostic missing");
  Require(std::any_of(validation.authority_facts.begin(), validation.authority_facts.end(),
      [reason](const auto& fact) {
        return fact.fact_name == "request_uuid" && fact.required &&
            fact.status == (reason == Refusal::kMissing ? opt::OptimizerAuthorityStatus::kMissing
                                                       : opt::OptimizerAuthorityStatus::kRejected);
      }), "request identity failure was treated as an advisory fact");
  const auto result = opt::OptimizeBoundRequest(request);
  Require(!result.ok && result.candidates.empty() && result.diagnostic_code == "SB-OPT-0001",
          "invalid request reached candidate planning");
  for (const auto* supplied : {&result, &unrelated_success}) {
    const auto document = opt::BuildOptimizerExplainDocument(request, *supplied);
    Require(document.request_uuid == Uuid{} && document.request_identity_refusal == reason &&
            document.candidates.empty() && document.selected_candidate_id.empty() && document.plan_hash.empty(),
            "invalid identity published a candidate-bearing explain document");
    const auto json = opt::RenderOptimizerExplainJson(document);
    Require(json.find("\"request_uuid\": null") != std::string::npos &&
            json.find("SB-OPT-0001") != std::string::npos,
            "explain renderer advertised invalid identity or hid refusal");
  }
}
void EveryByteThroughPlanningAndExplain() {
  const auto fixed = FixtureUuid(2209, 500);
  const auto successful = opt::OptimizeBoundRequest(Request(fixed));
  Require(successful.ok && !successful.candidates.empty(), "native baseline did not plan");
  const auto original = opt::BuildOptimizerExplainDocument(Request(fixed), successful);
  for (unsigned position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto id = fixed;
      id.bytes[position] = static_cast<scratchbird::core::platform::byte>(octet);
      auto request = Request(id);
      if (!V7(id)) {
        CheckRefusal(request, Refusal::kMalformed, successful);
        // Advisory authority claims cannot rescue malformed identity bytes.
        request.authority_facts.push_back(opt::MakeAuthorityFact(
            "request_uuid", opt::OptimizerAuthorityStatus::kPresent, true));
        CheckRefusal(request, Refusal::kMalformed, successful);
        continue;
      }
      const auto validation = opt::ValidateBoundOptimizerRequest(request);
      Require(validation.ok && validation.request_identity_refusal == Refusal::kNone,
              "valid binary request identity rejected");
      const auto result = opt::OptimizeBoundRequest(request);
      Require(result.ok && !result.candidates.empty(), "valid native request did not plan");
      const auto document = opt::BuildOptimizerExplainDocument(request, result);
      Require(document.request_uuid == id && document.request_identity_refusal == Refusal::kNone &&
              document.candidates.size() == result.candidates.size() && document.plan_hash == original.plan_hash,
              "explain lost native identity or confused occurrence identity with semantic plan hash");
      for (const auto& evidence : document.route_evidence) {
        Require(!evidence.starts_with("request_uuid="), "internal route evidence retained text UUID");
      }
      const auto json = opt::RenderOptimizerExplainJson(document);
      Require(json.find("\"request_uuid\": \"" + Presentation(id) + "\"") != std::string::npos,
              "JSON boundary did not render exact native bytes");
      Require(request.context.request_uuid == id && document.request_uuid == id,
              "planning or presentation mutated caller identity");
    }
  }
  CheckRefusal(Request({}), Refusal::kMissing, successful);
  auto denied = Request(fixed);
  denied.context.security_context_present = false;
  Require(!opt::OptimizeBoundRequest(denied).ok, "valid UUID bypassed security admission");
  denied = Request(fixed);
  denied.context.parser_owned_claims_present = true;
  Require(!opt::OptimizeBoundRequest(denied).ok, "valid UUID bypassed parser authority rejection");
  auto invalid_document = original;
  invalid_document.request_uuid.bytes[6] = 0x40;
  Require(opt::RenderOptimizerExplainJson(invalid_document).find("\"request_uuid\": null") != std::string::npos,
          "direct renderer advertised a non-v7 request identity");
}
}  // namespace
int main() {
  EveryByteThroughPlanningAndExplain();
  std::cout << "native optimizer request identity: " << checks << " checks PASS\n";
}
