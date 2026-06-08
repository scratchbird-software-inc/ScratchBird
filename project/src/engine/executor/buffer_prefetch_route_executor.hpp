// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "buffer_prefetch_readahead.hpp"

namespace scratchbird::engine::executor {

scratchbird::storage::page::BufferPrefetchReadaheadResult
ConsumeBufferPrefetchReadaheadRoute(
    const scratchbird::storage::page::BufferPrefetchReadaheadRequest& request);

}  // namespace scratchbird::engine::executor
