// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string_view>

namespace scratchbird::engine::sblr::canonical_query_execute_detail {

// Parses the exact canonical time-series endpoint representation used by the
// query route. This is validation/comparison support only; it owns neither
// model-family result publication nor MGA visibility authority.
bool ParseTimeSeriesEndpointNsV1(std::string_view value,
                                 std::int64_t* timestamp_ns);

}  // namespace scratchbird::engine::sblr::canonical_query_execute_detail
