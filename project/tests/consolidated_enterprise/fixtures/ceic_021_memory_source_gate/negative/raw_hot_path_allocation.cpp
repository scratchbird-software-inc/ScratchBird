// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <vector>

namespace scratchbird::ceic_021_fixture {

struct HotPathRow {
  std::uint64_t value = 0;
};

bool HasMoreRows();
HotPathRow NextRow();

void RawAllocatorHotPath(std::size_t page_size) {
  auto* row = new HotPathRow();
  delete row;

  void* block = std::malloc(page_size);
  std::free(block);

  std::vector<std::uint8_t> page_buffer;
  page_buffer.resize(page_size);

  std::vector<HotPathRow> rows;
  while (HasMoreRows()) {
    rows.push_back(NextRow());
  }
}

}  // namespace scratchbird::ceic_021_fixture
