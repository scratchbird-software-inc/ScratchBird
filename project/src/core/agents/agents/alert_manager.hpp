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

enum class AlertManagerDecisionKind : u32 {
  no_action,
  fire_alert,
  silence_alert,
  clear_alert,
  refused
};

enum class AlertSeverity : u32 {
  info,
  warning,
  critical
};

struct AlertManagerPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool fire_allowed = true;
  bool silence_allowed = true;
  bool clear_allowed = true;
  u64 dedupe_window_microseconds = 300000000;
  u64 max_silence_microseconds = 3600000000ull;
};

struct AlertManagerRequest {
  std::string alert_key;
  AlertSeverity severity = AlertSeverity::warning;
  u64 now_microseconds = 0;
  u64 last_fired_microseconds = 0;
  u64 requested_silence_microseconds = 0;
  bool condition_active = false;
  bool clear_condition = false;
  bool silence_requested = false;
  bool trusted_evidence_present = false;
  bool redaction_policy_valid = true;
  bool parser_authority = false;
  bool sidecar_authority = false;
};

struct AlertManagerEvidenceField {
  std::string key;
  std::string value;
};

struct AlertManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  AlertManagerDecisionKind decision = AlertManagerDecisionKind::refused;
  std::vector<AlertManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool alert_fired = false;
  bool alert_silenced = false;
  bool alert_cleared = false;
  bool deduped = false;
  u64 silence_until_microseconds = 0;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* AlertManagerDecisionKindName(AlertManagerDecisionKind decision);
const char* AlertSeverityName(AlertSeverity severity);
AlertManagerResult EvaluateAlertManagerRequest(
    const AlertManagerRequest& request,
    const AlertManagerPolicy& policy = {});
DiagnosticRecord MakeAlertManagerDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

const char* alert_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
