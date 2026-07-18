// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "beta_parser.hpp"
#include "neutral_protocol.hpp"

namespace scratchbird::parser::beta {
int Parse() { return scratchbird::parser::neutral::EncodeFrame(); }
}
