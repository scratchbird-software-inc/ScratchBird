// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/common.hpp"
#include "lifecycle/parser_lifecycle.hpp"
#include "parser_admission.hpp"
#include "parser_package_registry.hpp"
#include "sbps.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool HasDiagnostic(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics,
                   const std::string& code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

scratchbird::server::sbps::HelloRequest BuiltinHello() {
  const auto payload = scratchbird::server::sbps::EncodeHelloRequestForTest();
  auto hello = scratchbird::server::sbps::DecodeHelloRequest(payload);
  Require(hello.has_value(), "test HELLO payload did not decode");
  return *hello;
}

scratchbird::server::ParserPackageRegistry RegistryWith(
    scratchbird::server::ParserPackageRegistryEntry entry) {
  scratchbird::server::ParserPackageRegistry registry;
  registry.generation = 13;
  registry.source = "database_lifecycle_parser_conformance";
  registry.entries.push_back(std::move(entry));
  return registry;
}

void PackageAdmissionRejectsAuthorityBypass() {
  auto hello = BuiltinHello();

  scratchbird::server::ParserPackageRegistryEntry good;
  good.parser_api_major = 3;
  good.resource_bundle_hash = "builtin";
  const auto admitted = scratchbird::server::AdmitParserPackage(
      RegistryWith(good), hello, scratchbird::server::sbps::kProtocolMajor,
      scratchbird::server::sbps::kProtocolMinor);
  Require(admitted.admitted, "builtin attested parser package was not admitted");

  scratchbird::server::ParserPackageRegistryEntry bypass = good;
  bypass.resource_bundle_hash =
      "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  bypass.dev_hash_bypass = true;
  const auto rejected = scratchbird::server::AdmitParserPackage(
      RegistryWith(bypass), hello, scratchbird::server::sbps::kProtocolMajor,
      scratchbird::server::sbps::kProtocolMinor);
  Require(!rejected.admitted, "dev hash bypass admitted a parser package");
  Require(!rejected.dev_warning, "dev hash bypass was downgraded to warning");
  Require(HasDiagnostic(rejected.diagnostics, "SERVER.PARSER.PACKAGE_ATTESTATION_REQUIRED"),
          "dev hash bypass did not emit package attestation diagnostic");

  scratchbird::server::ParserPackageRegistryEntry quarantined = good;
  quarantined.failure_count_10m = 5;
  Require(scratchbird::server::ParserPackageShouldQuarantine(quarantined),
          "failure threshold did not mark parser package quarantine-required");
  quarantined.state = "quarantined";
  const auto quarantine_result = scratchbird::server::AdmitParserPackage(
      RegistryWith(quarantined), hello, scratchbird::server::sbps::kProtocolMajor,
      scratchbird::server::sbps::kProtocolMinor);
  Require(!quarantine_result.admitted,
          "quarantined parser package was admitted");
  Require(HasDiagnostic(quarantine_result.diagnostics, "SERVER.PARSER.PACKAGE_REJECTED"),
          "quarantined parser package did not emit package rejected diagnostic");
}

void PreauthRelayAllowsOnlySafeOperations() {
  Require(scratchbird::server::ParserPreauthOperationAllowed("HELLO"),
          "HELLO was not allowed in pre-auth parser relay");
  Require(scratchbird::server::ParserPreauthOperationAllowed("AUTH_START"),
          "AUTH_START was not allowed in pre-auth parser relay");
  Require(scratchbird::server::ParserPreauthOperationAllowed("ATTACH_PREPARE"),
          "ATTACH_PREPARE was not allowed in pre-auth parser relay");
  Require(!scratchbird::server::ParserPreauthOperationAllowed("EXECUTE_SBLR"),
          "EXECUTE_SBLR was allowed in pre-auth parser relay");
  Require(!scratchbird::server::ParserPreauthOperationAllowed("COMMIT"),
          "COMMIT was allowed in pre-auth parser relay");
}

scratchbird::parser::sbsql::ParserEngineAuthorityProof EngineProof() {
  scratchbird::parser::sbsql::ParserEngineAuthorityProof proof;
  proof.authentication_by_engine = true;
  proof.authorization_by_engine = true;
  proof.mga_context_by_engine = true;
  proof.sblr_admission_by_engine = true;
  return proof;
}

void LifecycleStateMachineCoversDBLC013C() {
  using scratchbird::parser::sbsql::ParserLifecycle;
  using scratchbird::parser::sbsql::ParserLifecycleState;
  using scratchbird::parser::sbsql::ParserLifecycleStateName;
  using scratchbird::parser::sbsql::ParserState;
  using scratchbird::parser::sbsql::StateName;

  Require(StateName(ParserState::kPackageAdmitted) == "package_admitted",
          "metrics state package_admitted is missing");
  Require(StateName(ParserState::kIdlePreAuth) == "idle_preauth",
          "metrics state idle_preauth is missing");
  Require(StateName(ParserState::kRecycled) == "recycled",
          "metrics state recycled is missing");
  Require(StateName(ParserState::kDisconnected) == "disconnected",
          "metrics state disconnected is missing");
  Require(StateName(ParserState::kQuarantined) == "quarantined",
          "metrics state quarantined is missing");
  Require(ParserLifecycleStateName(ParserLifecycleState::kIdlePreauth) == "idle_preauth",
          "lifecycle state idle_preauth is missing");

  ParserLifecycle lifecycle;
  Require(lifecycle.RecordPackageAdmitted({true, true, true, "019e-parser-package"}).accepted,
          "package admission proof was rejected");
  Require(lifecycle.RecordWorkerSpawned().accepted, "worker spawn transition failed");
  Require(lifecycle.RecordHelloSent().accepted, "HELLO transition failed");
  Require(lifecycle.RecordHelloAck(true).accepted, "HELLO_ACK transition failed");
  Require(lifecycle.RecordIdlePreauthRelay("AUTH_START").accepted,
          "idle pre-auth AUTH relay failed");
  Require(!lifecycle.RecordIdlePreauthRelay("EXECUTE_SBLR").accepted,
          "active request was permitted during pre-auth relay");
  Require(lifecycle.RecordAttachAccepted(EngineProof()).accepted,
          "engine-authorized attach transition failed");
  Require(lifecycle.RecordActiveRequestStarted(EngineProof()).accepted,
          "engine-admitted active request transition failed");
  Require(lifecycle.RecordCancelRequested().accepted, "active cancel transition failed");
  Require(lifecycle.RecordDrainCompleted().accepted, "drain completion transition failed");
  Require(lifecycle.RecordActiveRequestStarted(EngineProof()).accepted,
          "second active request transition failed");
  Require(lifecycle.RecordActiveRequestCompleted().accepted,
          "active request completion transition failed");
  Require(lifecycle.RecordDisconnectRequested().accepted, "disconnect transition failed");
  Require(lifecycle.RecordTerminateRequested().accepted, "terminate transition failed");
  Require(lifecycle.RecordTerminated().accepted, "terminated transition failed");
  Require(lifecycle.state() == ParserLifecycleState::kTerminated,
          "lifecycle did not end in terminated state");

  ParserLifecycle recycle;
  Require(recycle.RecordWorkerSpawned().accepted, "recycle worker spawn failed");
  Require(recycle.RecordHelloSent().accepted, "recycle HELLO failed");
  Require(recycle.RecordHelloAck(true).accepted, "recycle HELLO_ACK failed");
  Require(recycle.RecordAttachAccepted(EngineProof()).accepted, "recycle attach failed");
  Require(recycle.RecordRecycleRequested().accepted, "recycle transition failed");
  Require(recycle.RecordTerminated().accepted, "recycle termination failed");
}

void AuthorityBypassIsRejected() {
  using scratchbird::parser::sbsql::ParserLifecycle;
  using scratchbird::parser::sbsql::ParserFailurePolicy;
  using scratchbird::parser::sbsql::ParserLifecycleState;

  auto parser_claims_auth = EngineProof();
  parser_claims_auth.parser_claims_authentication = true;
  ParserLifecycle attach_bypass;
  Require(attach_bypass.RecordWorkerSpawned().accepted, "attach bypass worker spawn failed");
  Require(attach_bypass.RecordHelloSent().accepted, "attach bypass HELLO failed");
  Require(attach_bypass.RecordHelloAck(true).accepted, "attach bypass HELLO_ACK failed");
  Require(!attach_bypass.RecordAttachAccepted(parser_claims_auth).accepted,
          "parser-side authentication authority was accepted");

  auto missing_sblr_authority = EngineProof();
  missing_sblr_authority.sblr_admission_by_engine = false;
  ParserLifecycle active_bypass;
  Require(active_bypass.RecordWorkerSpawned().accepted, "active bypass worker spawn failed");
  Require(active_bypass.RecordHelloSent().accepted, "active bypass HELLO failed");
  Require(active_bypass.RecordHelloAck(true).accepted, "active bypass HELLO_ACK failed");
  Require(active_bypass.RecordAttachAccepted(EngineProof()).accepted, "active bypass attach failed");
  Require(!active_bypass.RecordActiveRequestStarted(missing_sblr_authority).accepted,
          "active request without engine SBLR admission was accepted");

  ParserLifecycle failed;
  Require(failed.RecordWorkerSpawned().accepted, "failed worker spawn failed");
  Require(failed.RecordHelloSent().accepted, "failed HELLO failed");
  Require(!failed.RecordHelloAck(false).accepted, "rejected HELLO_ACK was accepted");
  Require(failed.state() == ParserLifecycleState::kFailed,
          "rejected HELLO_ACK did not move worker to failed state");
  Require(failed.ApplyFailurePolicy(ParserFailurePolicy{1, 1}).accepted,
          "failure policy did not accept quarantine evaluation");
  Require(failed.state() == ParserLifecycleState::kQuarantined,
          "failed worker did not quarantine at configured threshold");
}

}  // namespace

int main() {
  PackageAdmissionRejectsAuthorityBypass();
  PreauthRelayAllowsOnlySafeOperations();
  LifecycleStateMachineCoversDBLC013C();
  AuthorityBypassIsRejected();
  std::cout << "database_lifecycle_parser_conformance=passed\n";
  return 0;
}
