// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "query/result_metadata.hpp"

namespace scratchbird::engine::sblr {

// Internal execution values, not SQL or rendered rows. The retained immutable
// schema must match the live receipt and exact registry tuple before any cell
// is materialized. No handle issuance or cursor/lifetime authority is implied.
bool PreserveCanonicalQueryResultValuesV1(
    const internal_api::EngineRequestContext& context,
    internal_api::EngineResultShape* shape,
    std::string* diagnostic_code,
    std::string* detail);

}  // namespace scratchbird::engine::sblr
