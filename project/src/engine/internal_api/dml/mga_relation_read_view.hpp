// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// A scoped, statement-local read model assembled directly from the MGA
// relation-store result. It is intentionally distinct from the legacy
// compatibility state: normal DML, constraint, trigger, and index decisions
// must never depend on the compatibility event-store representation.
struct MgaRelationReadView : RelationReadSnapshot {};

MgaRelationReadView BuildMgaRelationReadView(
    const MgaRelationStoreState& state);
MgaRelationReadView BuildMgaRelationReadView(MgaRelationStoreState&& state);

bool MgaCreatorVisible(const MgaRelationReadView& view,
                       std::uint64_t creator_tx,
                       std::uint64_t event_sequence,
                       std::uint64_t observer_tx);
bool MgaRowVersionVisibleToContext(const MgaRelationReadView& view,
                                   const CrudRowVersionRecord& row,
                                   const EngineRequestContext& context);
std::optional<CrudTableRecord> FindVisibleMgaTable(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    std::uint64_t observer_tx);
std::optional<CrudRowVersionRecord> FindVisibleMgaRowForContext(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    const std::string& row_uuid,
    const EngineRequestContext& context);
std::vector<CrudRowVersionRecord> VisibleMgaRows(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    std::uint64_t observer_tx);
std::vector<CrudRowVersionRecord> VisibleMgaRowsForContext(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    const EngineRequestContext& context);
std::vector<CrudIndexRecord> VisibleMgaIndexesForTable(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    std::uint64_t observer_tx);
EngineApiDiagnostic ValidateMgaUniqueIndexesForRow(
    const MgaRelationReadView& view,
    const std::string& table_uuid,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
