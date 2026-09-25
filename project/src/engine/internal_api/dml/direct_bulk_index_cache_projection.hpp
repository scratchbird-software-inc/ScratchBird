// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "direct_bulk_append_cache.hpp"
namespace scratchbird::engine::internal_api::dml::detail {
std::map<EngineUuid, std::set<std::string>> DirectBuildIndexKeyCache(
    const std::vector<CrudIndexEntryRecord>&);
std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>> DirectBuildIndexEntryKeyCache(
    const std::vector<CrudIndexEntryRecord>&);
std::vector<CrudIndexEntryRecord> DirectIndexEntriesFromExactBatches(
    const EngineRequestContext&, const std::vector<MgaExactIndexEntryAppendBatch>&);
std::vector<CrudIndexEntryRecord> DirectIndexEntriesFromRetailBatches(
    const EngineRequestContext&, const std::vector<MgaIndexEntryAppendBatch>&);
}
