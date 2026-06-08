// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "query_memory_arena.hpp"
#include "reservation_backed_memory_resource.hpp"
#include "typed_arena.hpp"
#include "typed_slab_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scratchbird::ceic_021_fixture {

struct Row {
  std::uint64_t value = 0;
};

struct Policy {
  std::size_t max_rows = 16;
};

void ApprovedMemoryApiSurface(core::memory::MemoryManager& memory_manager,
                              core::memory::QueryMemoryArena& arena,
                              core::memory::ReservationBackedMemoryResource& resource,
                              core::memory::TypedArena& typed_arena,
                              core::memory::TypedSlabPool<Row>& slab,
                              const Policy& policy) {
  auto page = memory_manager.AcquirePageBuffer();
  auto* arena_row = typed_arena.New<Row>();
  auto* slab_row = slab.Acquire();
  void* reserved_bytes = resource.Allocate(64, alignof(Row));

  std::vector<Row> rows;
  rows.reserve(policy.max_rows);
  for (std::size_t i = 0; i < policy.max_rows; ++i) {
    rows.push_back(Row{i});
  }

  resource.Deallocate(reserved_bytes, 64, alignof(Row));
  slab.Release(slab_row);
  typed_arena.Destroy(arena_row);
  arena.Reset();
  memory_manager.ReleasePageBuffer(std::move(page));
}

}  // namespace scratchbird::ceic_021_fixture
