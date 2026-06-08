// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_parser_projection.hpp"

namespace scratchbird::engine::sblr {

std::vector<SblrParserProjectionRow> BuildSblrParserProjectionRows() {
  return {
      {"sblr.envelope.decode", "implemented", "", "parser may submit envelope but engine validates authority"},
      {"sblr.function.dispatch", "implemented_unvalidated", "", "function availability is registry/gate driven"},
      {"sblr.cluster.hook", "cluster_disabled", "SB_DIAG_CLUSTER_TXN_UNAVAILABLE", "parser cannot activate cluster authority"},
      {"sblr.llvm.jit", "policy_or_dependency_gated", "SB_DIAG_LLVM_REQUIRED_UNAVAILABLE", "interpreter remains authority"},
  };
}

}  // namespace scratchbird::engine::sblr
