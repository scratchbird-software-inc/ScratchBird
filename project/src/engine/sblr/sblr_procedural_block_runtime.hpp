// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "procedural/procedural_block_ir.hpp"
#include "sblr_assignment_runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrProceduralBlockDecodeResult {
  internal_api::EngineProceduralBlockV1 block;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  bool present = false;
  bool valid = false;
};

SblrProceduralBlockDecodeResult DecodeSblrProceduralBlockV1(
    const std::vector<std::string>& option_envelopes);

struct SblrProceduralBlockExecutionResult {
  SblrResult result;
  SblrAssignmentFrame assignment_frame;
  std::uint32_t instructions_executed = 0;
  std::uint32_t yields_executed = 0;
};

SblrProceduralBlockExecutionResult ExecuteSblrProceduralBlockV1(
    const internal_api::EngineProceduralBlockV1& block,
    const SblrExecutionContext& context);

}  // namespace scratchbird::engine::sblr
