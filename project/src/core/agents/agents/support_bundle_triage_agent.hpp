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

enum class SupportBundleTriageDecisionKind : u32 {
  no_action,
  recommend_support_bundle,
  prepare_redacted_bundle,
  refused
};

struct SupportBundleTriagePolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool recommendation_allowed = true;
  bool redacted_bundle_allowed = true;
  u64 completeness_threshold_per_mille = 850;
};

struct SupportBundleTriageSnapshot {
  u64 completeness_ratio_per_mille = 0;
  u64 agent_actions_total = 0;
  bool evidence_catalog_authoritative = false;
  bool tamper_evidence_valid = false;
  bool redaction_policy_valid = false;
  bool protected_material_present = false;
  bool support_bundle_sink_available = false;
  bool sidecar_authority = false;
};

struct SupportBundleTriageEvidenceField {
  std::string key;
  std::string value;
};

struct SupportBundleTriageResult {
  Status status;
  DiagnosticRecord diagnostic;
  SupportBundleTriageDecisionKind decision =
      SupportBundleTriageDecisionKind::refused;
  std::vector<SupportBundleTriageEvidenceField> evidence;
  bool fail_closed = true;
  bool protected_material_suppressed = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* SupportBundleTriageDecisionKindName(
    SupportBundleTriageDecisionKind decision);
SupportBundleTriageResult EvaluateSupportBundleTriage(
    const SupportBundleTriageSnapshot& snapshot,
    const SupportBundleTriagePolicy& policy = {});
DiagnosticRecord MakeSupportBundleTriageDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

const char* support_bundle_triage_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
