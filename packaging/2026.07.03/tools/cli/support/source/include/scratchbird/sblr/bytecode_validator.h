// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <vector>

#include "scratchbird/core/status.h"
#include "scratchbird/core/error_context.h"
#include "scratchbird/sblr/opcodes.h"

namespace scratchbird::sblr {

// Validate SBLR bytecode before execution (version/header sanity).
core::Status validateBytecode(const std::vector<uint8_t>& bytecode,
                              core::ErrorContext* ctx = nullptr);

}  // namespace scratchbird::sblr
