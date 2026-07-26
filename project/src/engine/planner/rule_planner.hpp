// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

namespace scratchbird::engine::planner {

// The operation-prefix planner was an unbound development scaffold.  It was
// never called by the admitted SBLR query route and selected physical access
// while constructing a nominally logical plan.  Keep this explicit tombstone
// so downstream code cannot mistake the old header for a supported adapter.
inline constexpr bool kOperationPrefixRulePlannerRemoved = true;

}  // namespace scratchbird::engine::planner
