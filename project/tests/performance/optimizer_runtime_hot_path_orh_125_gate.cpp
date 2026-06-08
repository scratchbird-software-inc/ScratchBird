// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_consumption_evidence.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-GATE-125 failure: " << message << '\n';
  std::exit(1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool HasDiagnostic(const opt::CrossRouteEquivalenceValidation& validation,
                   const std::string& diagnostic) {
  for (const auto& item : validation.diagnostics) {
    if (item.find(diagnostic) != std::string::npos) return true;
  }
  return false;
}

std::vector<std::string> RequiredRoutes() {
  return {"embedded", "ipc", "inet", "cli", "driver"};
}

opt::CrossRouteResultEvidence Route(std::string route_kind,
                                    std::string route_label) {
  opt::CrossRouteResultEvidence route;
  route.route_kind = std::move(route_kind);
  route.route_label = std::move(route_label);
  route.live_route_executed = true;
  route.benchmark_clean_claim = true;
  route.database_parameters_hash = "db:orh125:param-shape:v1";
  route.session_rights_digest = "rights:alice:reader:security-125";
  route.security_epoch = 125;
  route.redaction_epoch = 125;
  route.transaction_snapshot_id = "mga:snapshot:orh125";
  route.local_transaction_id = 1250001;
  route.result_contract_hash = "sha256:orh125-rowset-contract";
  route.required_ordering = "ORDER BY route_equivalence_key ASC";
  route.accepted = true;
  route.rows = {"1|alpha", "2|beta", "3|gamma"};
  route.diagnostic_code = "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ROUTE_CAPTURED";
  return route;
}

std::vector<opt::CrossRouteResultEvidence> EquivalentRoutes() {
  return {
      Route("embedded", "embedded:sb_isql:engine_dispatch"),
      Route("ipc", "ipc:sb_isql:local_sbps"),
      Route("inet", "inet:sb_isql:listener_sbps"),
      Route("cli", "cli:sb_isql:public_tool"),
      Route("driver", "driver:cpp_or_python:public_api"),
  };
}

void AcceptsCompleteEquivalentLiveRouteEvidence() {
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(EquivalentRoutes(),
                                               RequiredRoutes());
  Require(validation.ok, validation.diagnostic_code);
  Require(validation.benchmark_clean,
          "complete live equivalent route evidence should allow benchmark-clean");
}

void RejectsMismatchedSnapshot() {
  auto routes = EquivalentRoutes();
  routes[2].transaction_snapshot_id = "mga:snapshot:other";
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "snapshot mismatch should fail equivalence");
  Require(HasDiagnostic(validation,
                        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.SNAPSHOT_MISMATCH"),
          "missing snapshot mismatch diagnostic");
}

void RejectsMismatchedSessionRights() {
  auto routes = EquivalentRoutes();
  routes[3].session_rights_digest = "rights:bob:reader:security-125";
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "session rights mismatch should fail equivalence");
  Require(HasDiagnostic(
              validation,
              "SB_ORH_CROSS_ROUTE_EQUIVALENCE.SESSION_RIGHTS_MISMATCH"),
          "missing session rights mismatch diagnostic");
}

void RejectsMissingRouteEvidenceAndBenchmarkCleanOverclaim() {
  auto routes = EquivalentRoutes();
  routes.pop_back();
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "missing driver route should fail equivalence");
  Require(!validation.benchmark_clean,
          "missing route evidence must not be benchmark-clean");
  Require(validation.exact_blocker,
          "missing route evidence should be an exact blocker");
  Require(HasDiagnostic(
              validation,
              "driver:SB_ORH_CROSS_ROUTE_EQUIVALENCE.MISSING_ROUTE_EVIDENCE"),
          "missing route evidence diagnostic not present");
}

void AllowsEquivalenceButNotBenchmarkCleanForPartialClaim() {
  auto routes = EquivalentRoutes();
  routes[3].benchmark_clean_claim = false;
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(validation.ok, "equivalent routes should pass without perf claim");
  Require(!validation.benchmark_clean,
          "all live routes must claim benchmark-clean before the set can");
}

void RejectsUnsupportedRouteAsExactBlocker() {
  auto routes = EquivalentRoutes();
  auto& driver = routes.back();
  driver.unsupported_route = true;
  driver.live_route_executed = false;
  driver.benchmark_clean_claim = false;
  driver.failure_diagnostic_code =
      "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DRIVER_ROUTE_LIVE_EVIDENCE_UNAVAILABLE";
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "unsupported route should not pass equivalence");
  Require(!validation.benchmark_clean,
          "unsupported route must not be benchmark-clean");
  Require(validation.exact_blocker,
          "unsupported route should be an exact blocker");
  Require(validation.diagnostic_code ==
              "SB_ORH_CROSS_ROUTE_EQUIVALENCE.EXACT_BLOCKER",
          "unsupported route should return exact blocker diagnostic");
}

void RejectsStaticDescriptorOnlyEvidence() {
  auto routes = EquivalentRoutes();
  routes[0].live_route_executed = false;
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "static descriptor route should fail equivalence");
  Require(HasDiagnostic(
              validation,
              "SB_ORH_CROSS_ROUTE_EQUIVALENCE.STATIC_DESCRIPTOR_ONLY"),
          "missing static-descriptor diagnostic");
}

void RejectsMissingResultContract() {
  auto routes = EquivalentRoutes();
  routes[1].result_contract_hash.clear();
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "missing result contract should fail equivalence");
  Require(HasDiagnostic(
              validation,
              "SB_ORH_CROSS_ROUTE_EQUIVALENCE.RESULT_CONTRACT_MISSING"),
          "missing result contract diagnostic");
}

void RejectsDuplicateRouteEvidence() {
  auto routes = EquivalentRoutes();
  routes[1].route_kind = "embedded";
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "duplicate route should fail equivalence");
  Require(HasDiagnostic(validation,
                        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DUPLICATE_ROUTE"),
          "missing duplicate route diagnostic");
}

void RejectsResultAndDiagnosticDivergence() {
  auto routes = EquivalentRoutes();
  routes[1].rows = {"1|alpha", "2|DIFFERENT", "3|gamma"};
  auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "row divergence should fail equivalence");
  Require(HasDiagnostic(validation,
                        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ROWS_MISMATCH"),
          "missing row mismatch diagnostic");

  routes = EquivalentRoutes();
  for (auto& route : routes) {
    route.accepted = false;
    route.rows.clear();
    route.diagnostics = {"SBSQL.SECURITY.ACCESS_DENIED"};
    route.failure_diagnostic_code = "SBSQL.SECURITY.ACCESS_DENIED";
  }
  routes[4].diagnostics = {"SBSQL.SECURITY.DIFFERENT"};
  routes[4].failure_diagnostic_code = "SBSQL.SECURITY.DIFFERENT";
  validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "diagnostic divergence should fail equivalence");
  Require(HasDiagnostic(validation,
                        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.DIAGNOSTICS_MISMATCH"),
          "missing diagnostic mismatch diagnostic");
}

void RejectsParserClientDriverAuthorityDrift() {
  auto routes = EquivalentRoutes();
  routes[4].client_or_driver_owns_transaction_finality = true;
  routes[1].parser_owns_visibility_authority = true;
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "authority drift should fail equivalence");
  Require(HasDiagnostic(validation,
                        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.MGA_AUTHORITY_DRIFT"),
          "missing MGA authority drift diagnostic");
}

void RejectsMissingRouteLabel() {
  auto routes = EquivalentRoutes();
  routes[0].route_label.clear();
  const auto validation =
      opt::ValidateCrossRouteResultEquivalence(routes, RequiredRoutes());
  Require(!validation.ok, "missing route label should fail equivalence");
  Require(HasDiagnostic(validation,
                        "SB_ORH_CROSS_ROUTE_EQUIVALENCE.ROUTE_LABEL_MISSING"),
          "missing route label diagnostic");
}

}  // namespace

int main() {
  AcceptsCompleteEquivalentLiveRouteEvidence();
  RejectsMismatchedSnapshot();
  RejectsMismatchedSessionRights();
  RejectsMissingRouteEvidenceAndBenchmarkCleanOverclaim();
  AllowsEquivalenceButNotBenchmarkCleanForPartialClaim();
  RejectsUnsupportedRouteAsExactBlocker();
  RejectsStaticDescriptorOnlyEvidence();
  RejectsMissingResultContract();
  RejectsDuplicateRouteEvidence();
  RejectsResultAndDiagnosticDivergence();
  RejectsParserClientDriverAuthorityDrift();
  RejectsMissingRouteLabel();
  return 0;
}
