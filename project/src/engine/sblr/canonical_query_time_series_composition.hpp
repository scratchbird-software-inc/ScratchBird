// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"
#include "canonical_query_model_family_composition_support.hpp"

namespace scratchbird::engine::sblr {

// Executes the admitted production time-series source and its bounded
// relational tail. The engine-issued MGA statement context is consumed and
// revalidated through the selected-DAG gateway; this route cannot create or
// finalize transaction authority.
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalTimeSeriesFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture = nullptr);

}  // namespace scratchbird::engine::sblr
