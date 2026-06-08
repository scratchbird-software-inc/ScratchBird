// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <mutex>

namespace scratchbird::ceic_021_fixture {

void UsesGlobalAllocatorMutex() {
  static std::mutex global_allocator_mutex;
  std::lock_guard<std::mutex> guard(global_allocator_mutex);
}

}  // namespace scratchbird::ceic_021_fixture
