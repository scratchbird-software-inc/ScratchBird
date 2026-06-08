// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ceic_022_negative {
struct QueryMemoryArena {
  void Grant(int);
};

void query_grant_without_context(QueryMemoryArena& arena) {
  arena.Grant(128);
}
}  // namespace ceic_022_negative
