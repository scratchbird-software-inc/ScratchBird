// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/types.hpp"

#include <cstdint>
#include <vector>

namespace scratchbird::engine {

enum class DescriptorFamily : std::uint32_t {
  unknown = 0,
  null_value = 1,
  boolean = 2,
  integer = 3,
  unsigned_integer = 4,
  decimal = 5,
  real = 6,
  text = 7,
  binary = 8,
  temporal = 9,
  uuid = 10,
  json_document = 11,
  vector = 12,
  graph = 13,
  domain = 14
};

struct Descriptor {
  DescriptorFamily family = DescriptorFamily::unknown;
  std::vector<std::uint8_t> canonical_bytes;
};

}  // namespace scratchbird::engine
