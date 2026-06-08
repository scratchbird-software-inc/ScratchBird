// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lifecycle/parser_lifecycle.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace scratchbird::parser::sbsql {
namespace {

std::string UpperAscii(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return out;
}

bool CanTerminate(ParserLifecycleState state) {
  return state != ParserLifecycleState::kTerminated &&
         state != ParserLifecycleState::kQuarantined;
}

}  // namespace

std::string_view ParserLifecycleStateName(ParserLifecycleState state) {
  switch (state) {
    case ParserLifecycleState::kConstructed: return "constructed";
    case ParserLifecycleState::kPackageAdmitted: return "package_admitted";
    case ParserLifecycleState::kWorkerSpawned: return "worker_spawned";
    case ParserLifecycleState::kHelloSent: return "hello_sent";
    case ParserLifecycleState::kIdlePreauth: return "idle_preauth";
    case ParserLifecycleState::kAttachedIdle: return "attached_idle";
    case ParserLifecycleState::kActive: return "active";
    case ParserLifecycleState::kDraining: return "draining";
    case ParserLifecycleState::kDisconnecting: return "disconnecting";
    case ParserLifecycleState::kRecycling: return "recycling";
    case ParserLifecycleState::kTerminating: return "terminating";
    case ParserLifecycleState::kTerminated: return "terminated";
    case ParserLifecycleState::kFailed: return "failed";
    case ParserLifecycleState::kQuarantined: return "quarantined";
  }
  return "failed";
}

bool ParserLifecyclePreauthOperationAllowed(std::string_view operation_name) {
  const auto normalized = UpperAscii(operation_name);
  return normalized == "HELLO" ||
         normalized == "AUTH_START" ||
         normalized == "AUTH_CONTINUE" ||
         normalized == "AUTH_CANCEL" ||
         normalized == "ATTACH_PREPARE" ||
         normalized == "PING" ||
         normalized == "DISCONNECT";
}

bool ParserEngineAuthorityProofPreservesBoundary(const ParserEngineAuthorityProof& proof) {
  return !proof.parser_claims_authentication &&
         !proof.parser_claims_authorization &&
         !proof.parser_claims_mga &&
         !proof.parser_claims_sblr;
}

ParserLifecycleResult ParserLifecycle::Accept(ParserLifecycleState next) {
  state_ = next;
  ParserLifecycleResult result;
  result.accepted = true;
  result.state = state_;
  return result;
}

ParserLifecycleResult ParserLifecycle::Reject(std::string code, std::string detail) const {
  ParserLifecycleResult result;
  result.accepted = false;
  result.state = state_;
  result.diagnostics.push_back(ParserLifecycleDiagnostic{std::move(code), std::move(detail)});
  return result;
}

bool ParserLifecycle::Terminal() const {
  return state_ == ParserLifecycleState::kTerminated ||
         state_ == ParserLifecycleState::kQuarantined;
}

ParserLifecycleResult ParserLifecycle::RecordPackageAdmitted(const ParserPackageLifecycleProof& proof) {
  if (state_ != ParserLifecycleState::kConstructed) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "package admission is only valid before worker spawn");
  }
  if (!proof.admitted || !proof.attestation_verified || !proof.no_authority_bypass ||
      proof.parser_package_uuid.empty()) {
    return Reject("PARSER.LIFECYCLE.PACKAGE_ADMISSION_REQUIRED",
                  "parser package admission requires registry attestation and forbids authority bypass");
  }
  return Accept(ParserLifecycleState::kPackageAdmitted);
}

ParserLifecycleResult ParserLifecycle::RecordWorkerSpawned() {
  if (state_ != ParserLifecycleState::kConstructed &&
      state_ != ParserLifecycleState::kPackageAdmitted) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "worker spawn must precede HELLO negotiation");
  }
  return Accept(ParserLifecycleState::kWorkerSpawned);
}

ParserLifecycleResult ParserLifecycle::RecordHelloSent() {
  if (state_ != ParserLifecycleState::kWorkerSpawned) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "HELLO can only be sent by a spawned worker");
  }
  return Accept(ParserLifecycleState::kHelloSent);
}

ParserLifecycleResult ParserLifecycle::RecordHelloAck(bool accepted) {
  if (state_ != ParserLifecycleState::kHelloSent) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "HELLO_ACK can only be consumed after HELLO");
  }
  if (!accepted) {
    ++failures_10m_;
    ++failures_1h_;
    state_ = ParserLifecycleState::kFailed;
    return Reject("PARSER.LIFECYCLE.HELLO_REJECTED",
                  "listener/server rejected parser HELLO");
  }
  return Accept(ParserLifecycleState::kIdlePreauth);
}

ParserLifecycleResult ParserLifecycle::RecordIdlePreauthRelay(std::string_view operation_name) {
  if (state_ != ParserLifecycleState::kIdlePreauth) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "pre-auth relay is only valid while idle before attach");
  }
  if (!ParserLifecyclePreauthOperationAllowed(operation_name)) {
    return Reject("PARSER.LIFECYCLE.PREAUTH_OPERATION_FORBIDDEN",
                  "pre-auth parser relay is limited to HELLO, auth handoff, attach prepare, ping, and disconnect");
  }
  return Accept(ParserLifecycleState::kIdlePreauth);
}

ParserLifecycleResult ParserLifecycle::RecordAttachAccepted(const ParserEngineAuthorityProof& proof) {
  if (state_ != ParserLifecycleState::kIdlePreauth) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "attach requires an idle pre-auth parser channel");
  }
  if (!ParserEngineAuthorityProofPreservesBoundary(proof) ||
      !proof.authentication_by_engine ||
      !proof.authorization_by_engine ||
      !proof.mga_context_by_engine) {
    return Reject("PARSER.LIFECYCLE.ENGINE_AUTHORITY_REQUIRED",
                  "attach requires engine-owned authentication, authorization, and MGA context");
  }
  return Accept(ParserLifecycleState::kAttachedIdle);
}

ParserLifecycleResult ParserLifecycle::RecordActiveRequestStarted(const ParserEngineAuthorityProof& proof) {
  if (state_ != ParserLifecycleState::kAttachedIdle) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "active parser request requires an attached idle channel");
  }
  if (!ParserEngineAuthorityProofPreservesBoundary(proof) ||
      !proof.authentication_by_engine ||
      !proof.authorization_by_engine ||
      !proof.mga_context_by_engine ||
      !proof.sblr_admission_by_engine) {
    return Reject("PARSER.LIFECYCLE.ENGINE_AUTHORITY_REQUIRED",
                  "active parser request requires engine-owned auth, authorization, MGA, and SBLR admission");
  }
  return Accept(ParserLifecycleState::kActive);
}

ParserLifecycleResult ParserLifecycle::RecordActiveRequestCompleted() {
  if (state_ != ParserLifecycleState::kActive) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "request completion requires an active parser request");
  }
  return Accept(ParserLifecycleState::kAttachedIdle);
}

ParserLifecycleResult ParserLifecycle::RecordCancelRequested() {
  if (state_ != ParserLifecycleState::kActive) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "cancel requires an active parser request");
  }
  return Accept(ParserLifecycleState::kDraining);
}

ParserLifecycleResult ParserLifecycle::RecordDrainCompleted() {
  if (state_ != ParserLifecycleState::kDraining) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "drain completion requires draining state");
  }
  return Accept(ParserLifecycleState::kAttachedIdle);
}

ParserLifecycleResult ParserLifecycle::RecordDisconnectRequested() {
  if (Terminal()) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "disconnect is not valid for a terminal parser worker");
  }
  return Accept(ParserLifecycleState::kDisconnecting);
}

ParserLifecycleResult ParserLifecycle::RecordRecycleRequested() {
  if (!CanTerminate(state_)) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "recycle is not valid for a terminal parser worker");
  }
  return Accept(ParserLifecycleState::kRecycling);
}

ParserLifecycleResult ParserLifecycle::RecordTerminateRequested() {
  if (!CanTerminate(state_)) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "terminate is not valid for a terminal parser worker");
  }
  return Accept(ParserLifecycleState::kTerminating);
}

ParserLifecycleResult ParserLifecycle::RecordTerminated() {
  if (state_ != ParserLifecycleState::kDisconnecting &&
      state_ != ParserLifecycleState::kRecycling &&
      state_ != ParserLifecycleState::kTerminating &&
      state_ != ParserLifecycleState::kFailed) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "terminated evidence requires disconnecting, recycling, terminating, or failed state");
  }
  return Accept(ParserLifecycleState::kTerminated);
}

ParserLifecycleResult ParserLifecycle::RecordFailure(std::string_view reason) {
  ++failures_10m_;
  ++failures_1h_;
  state_ = ParserLifecycleState::kFailed;
  ParserLifecycleResult result;
  result.accepted = true;
  result.state = state_;
  result.diagnostics.push_back(ParserLifecycleDiagnostic{"PARSER.LIFECYCLE.WORKER_FAILED",
                                                        std::string(reason)});
  return result;
}

ParserLifecycleResult ParserLifecycle::ApplyFailurePolicy(const ParserFailurePolicy& policy) {
  if (state_ != ParserLifecycleState::kFailed) {
    return Reject("PARSER.LIFECYCLE.INVALID_TRANSITION",
                  "quarantine evaluation requires failed parser state");
  }
  if (failures_10m_ >= policy.quarantine_failures_10m ||
      failures_1h_ >= policy.quarantine_failures_1h) {
    return Accept(ParserLifecycleState::kQuarantined);
  }
  ParserLifecycleResult result;
  result.accepted = true;
  result.state = state_;
  return result;
}

}  // namespace scratchbird::parser::sbsql
