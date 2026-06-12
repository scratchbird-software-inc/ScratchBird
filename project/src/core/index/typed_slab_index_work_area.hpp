// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-013 index adapter for typed slab hot work areas.
#include "typed_slab_pool.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

struct IndexTypedSlabWorkAreaRequest {
  scratchbird::core::memory::SizeClassAllocator* allocator = nullptr;
  std::string route_label;
  u64 cursor_count = 0;
  bool engine_mga_authoritative = true;
  bool exact_recheck_required = true;
  bool parser_or_reference_authority = false;
  bool memory_index_finality_authority = false;
};

struct IndexTypedSlabWorkAreaResult {
  Status status;
  bool fail_closed = false;
  u64 cursor_count = 0;
  u64 typed_object_count = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

IndexTypedSlabWorkAreaResult BuildIndexTypedSlabWorkArea(
    IndexTypedSlabWorkAreaRequest request);

}  // namespace scratchbird::core::index
