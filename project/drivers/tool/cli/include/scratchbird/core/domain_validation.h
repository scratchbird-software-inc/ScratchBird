// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include "scratchbird/core/status.h"
#include "scratchbird/core/error_context.h"
#include "scratchbird/core/typed_value.h"
#include "scratchbird/core/function_invoker.h"

namespace scratchbird::core
{
    struct ValidationConfig
    {
        std::string function_name;
        std::string error_message;
    };

    class DomainValidation
    {
    public:
        static auto validateValue(const TypedValue& value,
                                  const ValidationConfig& config,
                                  FunctionInvoker* invoker,
                                  bool& is_valid_out,
                                  ErrorContext* ctx = nullptr) -> Status;

        static void setValidationError(const ValidationConfig& config,
                                       const TypedValue& value,
                                       ErrorContext* ctx);
    };
} // namespace scratchbird::core
