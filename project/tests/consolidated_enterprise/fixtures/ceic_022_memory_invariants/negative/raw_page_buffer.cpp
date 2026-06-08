// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <vector>

namespace ceic_022_negative {
void raw_page_buffer() {
  std::vector<std::uint8_t> page_buffer;
  page_buffer.resize(4096);
}
}  // namespace ceic_022_negative
