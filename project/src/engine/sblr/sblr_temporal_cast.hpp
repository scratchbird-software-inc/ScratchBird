// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_runtime.hpp"

namespace scratchbird::engine::sblr {
constexpr bool IsSblrCivilTemporalType(std::string_view name) noexcept {
  return name == "date" || name == "time" || name == "timestamp";
}
// Exact civil temporal representation adapter, not catalog/security admission.
// The enclosing owner retains both bound endpoint receipts. Never a zoned alias.
SblrResult EvaluateSblrTemporalCast(
    std::string_view operation, const SblrValue& value,
    std::string_view target_name, const SblrExecutionContext& context,
    bool explicit_cast, bool compatibility,
    std::shared_ptr<const internal_api::EngineDescriptor> target,
    bool try_conversion = false);

enum class SblrCalendarField { dow, doy, quarter, isodow, week };
// DATE/civil TIMESTAMP only. Result is an actual bound native INT64, never text.
SblrResult EvaluateSblrTemporalCalendar(
    std::string_view operation, SblrCalendarField field, const SblrValue& value,
    const SblrExecutionContext& context,
    std::shared_ptr<const internal_api::EngineDescriptor> target);
// DATE -> DATE. A null months pointer selects LAST_DAY; otherwise ADD_MONTHS
// consumes the exact bound INT64 operand, including its independent NULL state.
SblrResult EvaluateSblrDateMonth(
    std::string_view operation, const SblrValue& value, const SblrValue* months,
    const SblrExecutionContext& context,
    std::shared_ptr<const internal_api::EngineDescriptor> target);
}
