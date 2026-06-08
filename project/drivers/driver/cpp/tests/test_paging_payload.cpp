// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "scratchbird/protocol/sbwp_protocol.h"

TEST(PagingConformance, BuildsStreamControlPayload) {
    using namespace scratchbird::protocol;

    const uint8_t control = 0x02;
    const uint32_t window = 2048;
    const uint32_t timeout = 5000;

    auto payload = buildStreamControlPayload(control, window, timeout);
    ASSERT_EQ(payload.size(), 12u);
    EXPECT_EQ(payload[0], control);

    auto readU32 = [](const std::vector<uint8_t>& data, size_t offset) {
        return static_cast<uint32_t>(data[offset]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    };

    EXPECT_EQ(readU32(payload, 4), window);
    EXPECT_EQ(readU32(payload, 8), timeout);
}
