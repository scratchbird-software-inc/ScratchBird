// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/core/gpid.h"
#include <cstdint>
#include <string>

namespace scratchbird::core
{

struct BloomIndexParams
{
    bool enabled = false;
    double target_fpr = 0.01;
    uint8_t bits_per_key = 0;
    uint8_t num_hashes = 0;
    GPID meta_gpid = 0;
};

struct IndexParams
{
    bool has_bloom = false;
    BloomIndexParams bloom{};
};

std::string serializeIndexParams(const IndexParams &params);
bool parseIndexParams(const std::string &params_str, IndexParams *params_out);

} // namespace scratchbird::core
