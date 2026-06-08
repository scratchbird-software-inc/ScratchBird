// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-MGA-MEMORY-HOOK-ANCHOR
#include "memory.hpp"

#include <vector>

namespace scratchbird::transaction::mga {

using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryLifetime;
using scratchbird::core::memory::MemoryTag;
using scratchbird::core::platform::Subsystem;

struct MgaMemoryHook {
  MemoryCategory category = MemoryCategory::unknown;
  MemoryLifetime lifetime = MemoryLifetime::unknown;
  const char* purpose = "";
  bool implemented_now = false;
};

const std::vector<MgaMemoryHook>& MgaMemoryHooks();
MemoryTag MakeMgaMemoryTag(MemoryCategory category, const char* purpose, MemoryLifetime lifetime);

}  // namespace scratchbird::transaction::mga
