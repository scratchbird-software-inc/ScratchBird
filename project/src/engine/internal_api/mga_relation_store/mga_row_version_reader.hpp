// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr std::size_t kScopedDecodedRowCacheMaxAutoWarmRows = 60000;

bool LoadDecodedScopedRowsForTable(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::vector<CrudRowVersionRecord>* rows,
    bool* used_segment);

bool LoadDecodedScopedRowsForTableBounded(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    BoundedScopedRowReadControl* control,
    std::vector<CrudRowVersionRecord>* rows,
    bool* used_segment);

void UpdateScopedDecodedRowCacheAfterAppend(
    const std::map<std::string, std::vector<CrudRowVersionRecord>>&
        decoded_appends_by_path,
    const std::map<std::string, std::string>& encoded_appends_by_path);

void ClearScopedDecodedRowCache();

}  // namespace scratchbird::engine::internal_api
