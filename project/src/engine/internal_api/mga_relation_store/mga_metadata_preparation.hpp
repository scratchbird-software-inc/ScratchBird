// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

namespace scratchbird::engine::internal_api {

// Pure preparation material, not publication, visibility or write authority.
// An owning coordinator supplies an actually reserved event number and retains
// its native inventory owner throughout preparation and eventual publication.
// It must not change that event number after the contextual seal is built.
struct MgaPreparedSealedTableMetadata {
  std::string frame;
  MgaRelationStorageDescriptor descriptor;
};

EngineApiDiagnostic PrepareMgaSealedTableMetadata(
    const EngineRequestContext& context, const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes, std::uint64_t event_sequence,
    MgaPreparedSealedTableMetadata* output);

EngineApiDiagnostic PrepareMgaIndexMetadataFrame(
    const EngineRequestContext& context, const CrudIndexRecord& index,
    std::uint64_t event_sequence, std::string* output);

} // namespace scratchbird::engine::internal_api
