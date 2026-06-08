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

enum class ParserInterfaceManagerDecisionKind : u32 {
  no_action,
  drain_parser_family,
  quarantine_parser_package,
  refused
};

struct ParserInterfaceManagerPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool drain_allowed = true;
  bool quarantine_allowed = true;
  u64 crash_threshold = 3;
};

struct ParserInterfaceManagerSnapshot {
  std::string parser_family;
  std::string package_uuid;
  u64 parser_crashes_total = 0;
  u64 parser_sessions_active = 0;
  bool parser_metrics_authoritative = false;
  bool package_signature_valid = true;
  bool security_event_present = false;
  bool parser_execution_authority = false;
  bool parser_finality_authority = false;
};

struct ParserInterfaceManagerEvidenceField {
  std::string key;
  std::string value;
};

struct ParserInterfaceManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  ParserInterfaceManagerDecisionKind decision =
      ParserInterfaceManagerDecisionKind::refused;
  std::vector<ParserInterfaceManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool parser_authority_preserved = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* ParserInterfaceManagerDecisionKindName(
    ParserInterfaceManagerDecisionKind decision);
ParserInterfaceManagerResult EvaluateParserInterfaceManager(
    const ParserInterfaceManagerSnapshot& snapshot,
    const ParserInterfaceManagerPolicy& policy = {});
DiagnosticRecord MakeParserInterfaceManagerDiagnostic(Status status,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail = {});

const char* parser_interface_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
