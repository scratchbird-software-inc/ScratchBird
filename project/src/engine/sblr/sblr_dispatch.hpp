// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "sblr_engine_envelope.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrDispatchRequest {
  scratchbird::engine::internal_api::EngineRequestContext context;
  SblrOperationEnvelope envelope;
  scratchbird::engine::internal_api::EngineApiRequest api_request;
};

struct SblrDispatchResult {
  bool accepted = false;
  bool envelope_validated = false;
  bool dispatched_to_api = false;
  scratchbird::engine::internal_api::EngineApiResult api_result;
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

bool IsClusterOperationId(std::string_view operation_id);
SblrDispatchResult DispatchSblrOperation(const SblrDispatchRequest& request);
SblrDispatchResult DecodeAndDispatchSblrOperation(
    std::string_view encoded_envelope,
    scratchbird::engine::internal_api::EngineRequestContext context,
    scratchbird::engine::internal_api::EngineApiRequest api_request = {});
std::string SerializeSblrDispatchResultToJson(const SblrDispatchResult& result);

}  // namespace scratchbird::engine::sblr
