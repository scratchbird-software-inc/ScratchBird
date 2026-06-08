// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine_internal_api.hpp"
#include "sblr_envelope.hpp"

#include <string>
#include <string_view>

namespace scratchbird::parser::lowering {

struct EngineApiBridgeContext {
  std::string session_uuid;
  bool cluster_authority_active = false;
};

struct EngineApiBridgeDiagnostic {
  std::string code;
  std::string message;
};

struct EngineApiBridgeResult {
  scratchbird::engine::internal_api::EngineDispatchRequest request;
  EngineApiBridgeDiagnostic diagnostic;
  bool accepted = false;

  bool ok() const {
    return accepted;
  }
};

EngineApiBridgeResult BridgeLogicalEnvelopeToEngineRequest(const LogicalEnvelope& envelope,
                                                           const EngineApiBridgeContext& context);
std::string SerializeEngineApiBridgeResultToJson(const EngineApiBridgeResult& result);

}  // namespace scratchbird::parser::lowering
