// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>
#include "scratchbird/core/status.h"
#include "scratchbird/core/error_context.h"
#include "scratchbird/core/typed_value.h"

namespace scratchbird::core
{
    class FunctionInvoker
    {
    public:
        virtual ~FunctionInvoker() = default;

        virtual auto callFunctionByName(const std::string& function_name,
                                        const std::vector<TypedValue>& args,
                                        TypedValue& result_out,
                                        ErrorContext* ctx = nullptr) -> Status = 0;
    };
} // namespace scratchbird::core
