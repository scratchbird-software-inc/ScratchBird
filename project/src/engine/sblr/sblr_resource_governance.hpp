// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace scratchbird::engine::sblr {

struct SblrResourceBudget {
  std::size_t max_frame_depth = 1024;
  std::uint64_t max_rows = 0;
  std::uint64_t max_steps = 0;
  std::uint64_t max_memory_bytes = 0;
};

struct SblrResourceUsage {
  std::size_t frame_depth = 0;
  std::uint64_t rows = 0;
  std::uint64_t steps = 0;
  std::uint64_t memory_bytes = 0;
};

SblrResult CheckSblrResourceBudget(const SblrResourceBudget& budget,
                                   const SblrResourceUsage& usage,
                                   const SblrExecutionContext& context,
                                   std::string operation_id);

}  // namespace scratchbird::engine::sblr
