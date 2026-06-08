// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql {

enum class ParserLifecycleState {
  kConstructed,
  kPackageAdmitted,
  kWorkerSpawned,
  kHelloSent,
  kIdlePreauth,
  kAttachedIdle,
  kActive,
  kDraining,
  kDisconnecting,
  kRecycling,
  kTerminating,
  kTerminated,
  kFailed,
  kQuarantined,
};

struct ParserLifecycleDiagnostic {
  std::string code;
  std::string detail;
};

struct ParserLifecycleResult {
  bool accepted = false;
  ParserLifecycleState state = ParserLifecycleState::kConstructed;
  std::vector<ParserLifecycleDiagnostic> diagnostics;
};

struct ParserPackageLifecycleProof {
  bool admitted = false;
  bool attestation_verified = false;
  bool no_authority_bypass = false;
  std::string parser_package_uuid;
};

struct ParserEngineAuthorityProof {
  bool authentication_by_engine = false;
  bool authorization_by_engine = false;
  bool mga_context_by_engine = false;
  bool sblr_admission_by_engine = false;
  bool parser_claims_authentication = false;
  bool parser_claims_authorization = false;
  bool parser_claims_mga = false;
  bool parser_claims_sblr = false;
};

struct ParserFailurePolicy {
  std::uint64_t quarantine_failures_10m = 5;
  std::uint64_t quarantine_failures_1h = 10;
};

class ParserLifecycle {
 public:
  ParserLifecycleState state() const { return state_; }
  std::uint64_t failures_10m() const { return failures_10m_; }
  std::uint64_t failures_1h() const { return failures_1h_; }

  ParserLifecycleResult RecordPackageAdmitted(const ParserPackageLifecycleProof& proof);
  ParserLifecycleResult RecordWorkerSpawned();
  ParserLifecycleResult RecordHelloSent();
  ParserLifecycleResult RecordHelloAck(bool accepted);
  ParserLifecycleResult RecordIdlePreauthRelay(std::string_view operation_name);
  ParserLifecycleResult RecordAttachAccepted(const ParserEngineAuthorityProof& proof);
  ParserLifecycleResult RecordActiveRequestStarted(const ParserEngineAuthorityProof& proof);
  ParserLifecycleResult RecordActiveRequestCompleted();
  ParserLifecycleResult RecordCancelRequested();
  ParserLifecycleResult RecordDrainCompleted();
  ParserLifecycleResult RecordDisconnectRequested();
  ParserLifecycleResult RecordRecycleRequested();
  ParserLifecycleResult RecordTerminateRequested();
  ParserLifecycleResult RecordTerminated();
  ParserLifecycleResult RecordFailure(std::string_view reason);
  ParserLifecycleResult ApplyFailurePolicy(const ParserFailurePolicy& policy);

 private:
  ParserLifecycleState state_ = ParserLifecycleState::kConstructed;
  std::uint64_t failures_10m_ = 0;
  std::uint64_t failures_1h_ = 0;

  ParserLifecycleResult Accept(ParserLifecycleState next);
  ParserLifecycleResult Reject(std::string code, std::string detail) const;
  bool Terminal() const;
};

std::string_view ParserLifecycleStateName(ParserLifecycleState state);
bool ParserLifecyclePreauthOperationAllowed(std::string_view operation_name);
bool ParserEngineAuthorityProofPreservesBoundary(const ParserEngineAuthorityProof& proof);

}  // namespace scratchbird::parser::sbsql
