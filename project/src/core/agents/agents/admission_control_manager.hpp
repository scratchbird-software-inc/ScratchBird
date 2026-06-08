// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class AdmissionControlDecisionKind : u32 {
  allow,
  throttle_admission,
  deny_admission,
  downgrade_admission,
  refused
};

struct AdmissionControlPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool throttle_allowed = true;
  bool deny_allowed = true;
  bool downgrade_allowed = true;
  u64 min_emergency_reserve_bytes = 0;
  u64 throttle_listener_queue_depth = 1024;
  u64 deny_scheduler_queue_depth = 4096;
  u64 downgrade_slo_burn_rate_per_mille = 1500;
};

struct AdmissionControlSnapshot {
  u64 emergency_reserve_bytes = 0;
  u64 listener_queue_depth = 0;
  u64 scheduler_queue_depth = 0;
  u64 slo_burn_rate_per_mille = 0;
  bool pressure_metrics_authoritative = false;
  bool resource_ledger_authoritative = false;
  bool foreground_database_work_active = false;
  bool request_is_admin = false;
  bool parser_authority = false;
  bool client_authority = false;
};

struct AdmissionControlEvidenceField {
  std::string key;
  std::string value;
};

struct AdmissionControlResult {
  Status status;
  DiagnosticRecord diagnostic;
  AdmissionControlDecisionKind decision =
      AdmissionControlDecisionKind::refused;
  std::vector<AdmissionControlEvidenceField> evidence;
  bool fail_closed = true;
  bool request_allowed = false;
  bool throttled = false;
  bool denied = false;
  bool downgraded = false;
  bool foreground_protected = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* AdmissionControlDecisionKindName(
    AdmissionControlDecisionKind decision);
AdmissionControlResult EvaluateAdmissionControlRequest(
    const AdmissionControlSnapshot& snapshot,
    const AdmissionControlPolicy& policy = {});
DiagnosticRecord MakeAdmissionControlDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

const char* admission_control_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
