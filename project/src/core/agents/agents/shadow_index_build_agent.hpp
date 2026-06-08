// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_SHADOW_INDEX_BUILD_LIFECYCLE
#include "shadow_index_build_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::index::ShadowIndexBuildLedger;
using scratchbird::core::index::ShadowIndexBuildRecord;
using scratchbird::core::index::ShadowIndexLifecycleResult;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;

struct ShadowIndexBuildAgentEvidenceField {
  std::string key;
  std::string value;
};

struct ShadowIndexBuildAgentPublishRequest {
  bool engine_mga_authoritative = false;
  std::string agent_evidence_ref;
};

struct ShadowIndexBuildAgentResult {
  Status status;
  DiagnosticRecord diagnostic;
  ShadowIndexLifecycleResult lifecycle;
  std::vector<ShadowIndexBuildAgentEvidenceField> evidence;
  bool fail_closed = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

ShadowIndexBuildAgentResult PublishShadowIndexBuildAgentStep(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    const ShadowIndexBuildAgentPublishRequest& request);

DiagnosticRecord MakeShadowIndexBuildAgentDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

const char* shadow_index_build_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
