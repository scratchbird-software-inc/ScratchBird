// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_runtime.hpp"

namespace scratchbird::engine::sblr {
// Exact civil TIMESTAMP representation adapter, not catalog/security admission.
// The enclosing owner retains both bound endpoint receipts. Never a zoned alias.
SblrResult EvaluateSblrTimestampCast(
    std::string_view operation, const SblrValue& value,
    std::string_view target_name, const SblrExecutionContext& context,
    bool explicit_cast, bool compatibility,
    std::shared_ptr<const internal_api::EngineDescriptor> target);
}
