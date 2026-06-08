// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include "scratchbird/core/status.h"
#include "scratchbird/core/error_context.h"
#include "scratchbird/core/typed_value.h"
#include "scratchbird/core/function_invoker.h"

namespace scratchbird::core
{
    enum class NormalizationType : uint8_t
    {
        NONE = 0,
        LOWERCASE = 1,
        UPPERCASE = 2,
        TRIM = 3,
        TRIM_LOWERCASE = 4,
        TRIM_UPPERCASE = 5,
        CUSTOM_FUNCTION = 99
    };

    struct NormalizationConfig
    {
        NormalizationType type = NormalizationType::NONE;
        std::string custom_function_name;
    };

    class Normalization
    {
    public:
        static auto resolveConfig(const std::string& function_name) -> NormalizationConfig;

        static auto applyNormalization(const TypedValue& value,
                                       const NormalizationConfig& config,
                                       FunctionInvoker* invoker,
                                       TypedValue& normalized_out,
                                       ErrorContext* ctx = nullptr) -> Status;

    private:
        static auto applyLowercase(const TypedValue& value,
                                   TypedValue& normalized_out,
                                   ErrorContext* ctx) -> Status;

        static auto applyUppercase(const TypedValue& value,
                                   TypedValue& normalized_out,
                                   ErrorContext* ctx) -> Status;

        static auto applyTrim(const TypedValue& value,
                              TypedValue& normalized_out,
                              ErrorContext* ctx) -> Status;

        static auto applyTrimLowercase(const TypedValue& value,
                                       TypedValue& normalized_out,
                                       ErrorContext* ctx) -> Status;

        static auto applyTrimUppercase(const TypedValue& value,
                                       TypedValue& normalized_out,
                                       ErrorContext* ctx) -> Status;

        static auto applyCustomFunction(const TypedValue& value,
                                        const std::string& function_name,
                                        FunctionInvoker* invoker,
                                        TypedValue& normalized_out,
                                        ErrorContext* ctx) -> Status;
    };
} // namespace scratchbird::core
