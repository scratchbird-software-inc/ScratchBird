// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/descriptor.hpp"

#include <cstdint>
#include <vector>

namespace scratchbird::engine {

struct ExecutionValue {
  Descriptor descriptor;
  bool is_null = true;
  std::vector<std::uint8_t> encoded_value;
};

}  // namespace scratchbird::engine
