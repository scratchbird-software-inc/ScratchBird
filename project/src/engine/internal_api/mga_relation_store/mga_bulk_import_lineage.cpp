// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_relation_store.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <tuple>

namespace scratchbird::engine::internal_api {
namespace {
EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK","engine.api.ok",{},false);
}
}
MgaBulkImportRowLineageResultV1 ProbeMgaBulkImportRowIdentityLineageV1(
    const EngineRequestContext& context, const EngineUuid& table_uuid,
    const EngineUuid& row_uuid) {
  MgaBulkImportRowLineageResultV1 result;
  if (context.database_path.empty() || !core::uuid::IsEngineIdentityUuid(table_uuid) ||
      !core::uuid::IsEngineIdentityUuid(row_uuid)) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SBLR.OPERAND_INVALID", "mga.bulk_import.row_lineage_lookup_invalid",
        "database, table, and row identities are required", true);
    return result;
  }
  auto loaded =
      LoadMgaRelationStoreRowsForPointLookup(context, table_uuid, row_uuid);
  if (!loaded.ok) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.row_lineage_decode_failed",
        "the retained relation row lineage could not be decoded", true);
    return result;
  }
  auto rows = std::move(loaded.state.row_versions);
  rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& row) {
               return row.table_uuid != table_uuid || row.row_uuid != row_uuid;
             }),
             rows.end());
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return std::tie(left.event_sequence, left.version_uuid) <
           std::tie(right.event_sequence, right.version_uuid);
  });
  rows.erase(std::unique(rows.begin(), rows.end(),
                         [](const auto& left, const auto& right) {
                           return left.event_sequence == right.event_sequence &&
                                  left.version_uuid == right.version_uuid;
                         }),
             rows.end());
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.versions = std::move(rows);
  return result;
}

} // namespace scratchbird::engine::internal_api
