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
#include "scratchbird/core/typed_value.h"
#include "scratchbird/sblr/opcodes.h"

// Reference: public_contract_snapshot
namespace scratchbird::sblr
{
    bool extractElement(const core::TypedValue& source,
                        ExtractField field,
                        const std::vector<core::TypedValue>& args,
                        core::TypedValue* out,
                        std::string* error);

    bool alterElement(const core::TypedValue& source,
                      ExtractField field,
                      const std::vector<core::TypedValue>& args,
                      const core::TypedValue& new_value,
                      core::TypedValue* out,
                      std::string* error);
} // namespace scratchbird::sblr
