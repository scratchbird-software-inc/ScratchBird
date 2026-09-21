// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_relation_store.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <tuple>

namespace scratchbird::engine::internal_api {
namespace {
// This remaining legacy point-lookup boundary is not a binary companion
// adapter. Its migration belongs with the native relation-reader ownership.
bool BulkImportPublicationUuidV1(std::string_view text) {
  const auto parsed=core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && core::uuid::IsEngineIdentityUuid(parsed.value) &&
      core::uuid::UuidToString(parsed.value)==text;
}
EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK","engine.api.ok",{},false);
}
}
MgaBulkImportRowLineageResultV1 ProbeMgaBulkImportRowIdentityLineageV1(
    const EngineRequestContext& context, const std::string& table_uuid,
    const std::string& row_uuid) {
  MgaBulkImportRowLineageResultV1 result;
  if (context.database_path.empty() || !BulkImportPublicationUuidV1(table_uuid) ||
      !BulkImportPublicationUuidV1(row_uuid)) {
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
