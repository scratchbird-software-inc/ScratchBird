// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/common.hpp"
#include "lowering/lowering.hpp"
#include "resources/language_resource_contract.hpp"

#include <string>

namespace scratchbird::parser::sbsql {

struct SblrEnvelopeRenderResult {
  bool ok{false};
  std::string text;
  SblrRenderSelection selection;
  MessageVectorSet messages;
};

std::string RenderMessageVectorSet(const MessageVectorSet& messages);
std::string RenderPipelineResult(const PipelineResult& result);
std::string RenderSblrEnvelope(const SblrEnvelope& envelope);
SblrEnvelopeRenderResult RenderSblrEnvelopeWithProfileSelection(
    const SblrEnvelope& envelope,
    const SblrRenderRequest& request);

} // namespace scratchbird::parser::sbsql
