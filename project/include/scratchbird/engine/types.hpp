// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/engine.h"

#include <cstdint>
#include <string_view>

namespace scratchbird::engine {

using Status = sb_engine_status_t;
using Uuid = sb_engine_uuid_t;
using Budget = sb_engine_budget_v1_t;
using StringView = std::string_view;

constexpr std::uint32_t kAbiVersionPacked = SB_ENGINE_ABI_VERSION_PACKED;

inline StringView to_string_view(sb_engine_string_view_t value) noexcept {
  return value.data == nullptr ? StringView{} : StringView{value.data, static_cast<std::size_t>(value.size_bytes)};
}

}  // namespace scratchbird::engine
