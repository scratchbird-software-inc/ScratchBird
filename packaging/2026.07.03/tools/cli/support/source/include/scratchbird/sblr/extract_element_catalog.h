// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include "scratchbird/sblr/opcodes.h"

namespace scratchbird::sblr
{
    struct ElementArgSpec
    {
        uint8_t min_args = 0;
        uint8_t max_args = 0;
    };

    std::optional<ExtractField> resolveExtractFieldName(std::string_view name);
    const char* extractFieldToString(ExtractField field);
    ElementArgSpec extractFieldArgSpec(ExtractField field);
} // namespace scratchbird::sblr
