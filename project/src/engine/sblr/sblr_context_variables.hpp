// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrContextVariableEntry {
  std::string variable_id;
  std::string variable_uuid;
  std::string family;
  std::string canonical_name;
  std::string descriptor_id;
  std::string visibility;
};

const std::vector<SblrContextVariableEntry>& StandardSblrContextVariableRegistry();
SblrResult ResolveSblrContextVariable(std::string_view variable_id, const SblrExecutionContext& context);

}  // namespace scratchbird::engine::sblr
