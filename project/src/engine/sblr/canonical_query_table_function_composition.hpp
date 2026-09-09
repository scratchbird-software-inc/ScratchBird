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

// Admitted bounded generate-series and row-pattern composition over existing
// engine statement authority. Public route selection remains in the coordinator.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalGenerateSeriesTableFunctionQuery(
    const CanonicalCurrentHeapExecutionRequest& input);

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalGenerateSeriesMatchRecognizeQuery(
    const CanonicalCurrentHeapExecutionRequest& input);

}  // namespace scratchbird::engine::sblr
