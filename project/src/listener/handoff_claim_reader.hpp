// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>

#include "parser_pool.hpp"

namespace scratchbird::listener {

struct HandoffClaimReadResult {
  ParserHandoffClientEvidence evidence;
  bool recognized{false};
  bool consumed{false};
  bool malformed{false};
  bool timed_out{false};
  bool non_claim_input{false};
  std::string diagnostic_code;
};

#ifndef _WIN32
HandoffClaimReadResult ReadOptionalLprefaceHandoffClaimFromSocket(
    int fd,
    ParserHandoffClientEvidence base_evidence,
    std::uint32_t timeout_ms);
#endif

} // namespace scratchbird::listener
