// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "prepared_execution_template.hpp"
#include "sblr_engine_envelope.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

// SEARCH_KEY: SB_SBLR_PREPARED_TEMPLATE_BINDING
// SBLR prepared-template binding uses the operation envelope, UUID-bound engine
// descriptors, and engine request context. Parser SQL text is never authority.

struct SblrPreparedTemplateBuildResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  scratchbird::engine::executor::PreparedTemplateAdmission admission;
  scratchbird::engine::executor::PreparedTemplateBindContext bind_context;
  std::vector<std::string> evidence;
};

SblrPreparedTemplateBuildResult BuildPreparedTemplateFromSblr(
    const SblrOperationEnvelope& envelope,
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const scratchbird::engine::internal_api::EngineApiRequest& request);

scratchbird::engine::executor::PreparedTemplatePrepareResult PrepareSblrExecutionTemplate(
    scratchbird::engine::executor::PreparedTemplateCache* cache,
    const SblrOperationEnvelope& envelope,
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const scratchbird::engine::internal_api::EngineApiRequest& request);

}  // namespace scratchbird::engine::sblr
