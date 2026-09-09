// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"

namespace scratchbird::engine::sblr {

// Internal leaf route. The public coordinator preserves route precedence and
// supplies the already-bound request. No transaction authority is created here.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeLiteralValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

}  // namespace scratchbird::engine::sblr
