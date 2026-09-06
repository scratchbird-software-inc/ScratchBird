// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

#include <string>

namespace scratchbird::engine::internal_api {

struct SavepointParsedState;

bool ApplyDmlUpdateBinarySavepointRecordsForStoreModule(
    const EngineRequestContext& context,
    SavepointParsedState* state,
    std::string* refusal_detail = nullptr);

}  // namespace scratchbird::engine::internal_api
