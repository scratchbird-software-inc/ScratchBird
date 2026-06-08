// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/function_gate.hpp"
#include "common/function_runtime.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {

scratchbird::engine::sblr::SblrValue MakeNullValue(std::string descriptor_id = {});
scratchbird::engine::sblr::SblrValue MakeTextValue(std::string descriptor_id, std::string value);
scratchbird::engine::sblr::SblrValue MakeInt64Value(std::string descriptor_id, std::int64_t value);
scratchbird::engine::sblr::SblrValue MakeUint64Value(std::string descriptor_id, std::uint64_t value);
scratchbird::engine::sblr::SblrValue MakeReal64Value(std::string descriptor_id, double value);
scratchbird::engine::sblr::SblrValue MakeEncodedNumericValue(std::string descriptor_id, std::string encoded_value);
scratchbird::engine::sblr::SblrValue MakeBinaryValue(std::string descriptor_id, std::vector<std::uint8_t> value);
bool IsSqlNull(const scratchbird::engine::sblr::SblrValue& value);
std::string ValueAsText(const scratchbird::engine::sblr::SblrValue& value);

FunctionCallResult RefuseFunctionWithDiagnostic(const FunctionCallRequest& request,
                                                scratchbird::engine::sblr::SblrStatusCode status,
                                                std::string diagnostic_id,
                                                std::string detail);
FunctionCallResult RefuseFunctionForGate(const FunctionCallRequest& request,
                                         const FunctionGateDecision& decision);
FunctionCallResult RefuseFunctionInvalidInput(const FunctionCallRequest& request, std::string detail);
FunctionCallResult RefuseFunctionOverflow(const FunctionCallRequest& request, std::string detail);
FunctionCallResult RefuseFunctionNumericDomain(const FunctionCallRequest& request, std::string detail);
FunctionCallResult RefuseFunctionNumericDivisionByZero(const FunctionCallRequest& request, std::string detail);

}  // namespace scratchbird::engine::functions
