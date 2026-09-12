// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "query/plan_api.hpp"
#include "query/result_metadata.hpp"

namespace scratchbird::engine::sblr {

// Atomic publication: on refusal the output schema is cleared, and no partial
// descriptor vector is attached. This is not a receipt-issuance API; callers
// supply their live engine context and an already validated/executed DAG.
bool PreserveCanonicalQueryResultMetadataV1(
    const internal_api::EngineRequestContext& context,
    const internal_api::TypedRelationalDag& dag,
    internal_api::EngineResultShape* shape,
    std::string* diagnostic_code,
    std::string* detail);

}  // namespace scratchbird::engine::sblr
