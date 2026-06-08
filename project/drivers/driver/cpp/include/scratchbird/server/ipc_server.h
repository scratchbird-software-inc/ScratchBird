// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

namespace scratchbird {
namespace server {

enum class IPCMethod : uint8_t {
    AUTO = 0,
    UNIX_SOCKET = 1,
    NAMED_PIPE = 2,
    TCP_LOCALHOST = 3
};

} // namespace server
} // namespace scratchbird
