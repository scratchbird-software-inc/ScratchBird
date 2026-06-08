// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_UNIQUE_INDEX_DEFERRAL_POLICY
#include "runtime_platform.hpp"

#include <string>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;

enum class IndexUniquenessClass : u32 {
  non_unique_secondary,
  unique_primary,
  unique_secondary
};

enum class IndexDeferralRequestKind : u32 {
  synchronous_immediate,
  generic_deferred,
  unique_deferred_with_reservation
};

struct UniqueIndexDeferralPolicyRequest {
  IndexUniquenessClass uniqueness = IndexUniquenessClass::non_unique_secondary;
  IndexDeferralRequestKind request_kind = IndexDeferralRequestKind::synchronous_immediate;
  bool non_unique_deferral_policy_enabled = false;
  bool reservation_protocol_present = false;
  bool reservation_protocol_proven = false;
  bool reservation_protocol_enabled = false;
  std::string reservation_protocol_gate_token;
};

struct UniqueIndexDeferralPolicyDecision {
  Status status;
  bool accepted = false;
  bool deferred_eligible = false;
  bool synchronous_required = true;
  bool reservation_protocol_required = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted; }
};

const char* IndexUniquenessClassName(IndexUniquenessClass uniqueness);
const char* IndexDeferralRequestKindName(IndexDeferralRequestKind request_kind);
UniqueIndexDeferralPolicyDecision EvaluateUniqueIndexDeferralPolicy(
    const UniqueIndexDeferralPolicyRequest& request);
DiagnosticRecord MakeUniqueIndexDeferralPolicyDiagnostic(Status status,
                                                         std::string diagnostic_code,
                                                         std::string message_key,
                                                         std::string detail = {});

}  // namespace scratchbird::core::index
