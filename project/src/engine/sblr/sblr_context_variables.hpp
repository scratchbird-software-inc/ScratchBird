// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"
#include "../../wire/system_variable_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

using SblrContextVariableEntry = scratchbird::wire::SystemVariableEntry;

inline constexpr std::span<const SblrContextVariableEntry>
StandardSblrContextVariableRegistry() noexcept {
  return scratchbird::wire::StandardSystemVariableRegistry();
}
SblrResult ResolveSblrContextVariable(std::string_view variable_id, const SblrExecutionContext& context);

}  // namespace scratchbird::engine::sblr
