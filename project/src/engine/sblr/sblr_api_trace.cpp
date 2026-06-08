// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_api_trace.hpp"

namespace scratchbird::engine::sblr {

std::vector<SblrApiTracePath> BuildSblrApiTracePaths() {
  return {
      {"TRACE_SBLR_DECODE", "sblr.envelope.decode", "sb_engine_sblr", "decode/validate/refuse"},
      {"TRACE_SBLR_DISPATCH", "sblr.dispatch", "sb_engine_sblr", "dispatch/refusal"},
      {"TRACE_FUNCTION_DISPATCH", "sblr.function.dispatch", "sb_engine_functions", "registry/gates/family-handler"},
      {"TRACE_CLUSTER_REFUSAL", "sblr.cluster.*", "sb_engine_sblr", "single-node deterministic refusal"},
      {"TRACE_LLVM_SEAM", "sblr.llvm.*", "sb_engine_sblr", "interpreter/fallback/refusal"},
  };
}

}  // namespace scratchbird::engine::sblr
