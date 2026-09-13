// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_relation_store.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include <map>
#include <set>

namespace scratchbird::engine::internal_api {

// Stages a derived relation snapshot. The native owner separately performs
// visibility/retention admission and durable row/index publication.
MgaRelationPhysicalSweepResult ApplyMgaRelationPhysicalSweepToState(
    const MgaRelationPhysicalSweepRequest& request) {
  namespace mga = scratchbird::transaction::mga;
  const auto fail = [](const char* reason) {
    MgaRelationPhysicalSweepResult result;
    result.fail_closed = true;
    result.diagnostic = MakeEngineApiDiagnostic("CATALOG.INVALID_INPUT",
        "mga.relation_cleanup.invalid_projection", reason, true);
    return result;
  };
  const auto valid_uuid = scratchbird::core::uuid::IsEngineIdentityUuid;
  const auto horizon = request.authoritative_cleanup_horizon_local_transaction_id;
  if (!request.engine_mga_authoritative || !request.cleanup_horizon_authoritative || horizon == 0)
    return fail("cleanup_authority_required");
  if (!valid_uuid(request.relation_uuid)) return fail("binary_relation_scope_required");
  if (request.max_row_versions_to_scan == 0 ||
      request.state.row_versions.size() > request.max_row_versions_to_scan ||
      request.max_index_entries_to_scan == 0 ||
      request.state.index_entries.size() > request.max_index_entries_to_scan)
    return fail("bounded_projection_required");

  std::map<EngineUuid, const CrudRowVersionRecord*> versions;
  for (const auto& row : request.state.row_versions) {
    if (!valid_uuid(row.table_uuid) || !valid_uuid(row.row_uuid) ||
        !valid_uuid(row.version_uuid) || row.version_uuid == row.row_uuid ||
        !valid_uuid(row.creator_transaction_uuid) || row.creator_tx == 0 || row.sequence == 0 ||
        (row.previous_version_uuid.is_nil() != (row.previous_sequence == 0)) ||
        (!row.previous_version_uuid.is_nil() &&
         (!valid_uuid(row.previous_version_uuid) || row.previous_version_uuid == row.version_uuid ||
          row.previous_sequence >= row.sequence)) ||
        (!row.temporary_session_uuid.is_nil() && !valid_uuid(row.temporary_session_uuid)) ||
        !versions.emplace(row.version_uuid, &row).second)
      return fail("invalid_or_duplicate_version_identity");
  }

  std::set<EngineUuid> reclaimed;
  for (const auto& evidence : request.reclaim_evidence_records) {
    const auto& identity = evidence.row_version_identity;
    if (!mga::ValidateRowVersionIdentity(identity).ok() ||
        evidence.creator_transaction.value != identity.creator_transaction.local_id.value ||
        evidence.authoritative_cleanup_horizon_local_transaction_id != horizon ||
        !reclaimed.insert(identity.version_uuid).second)
      return fail("invalid_or_duplicate_reclaim_evidence");
    const auto found = versions.find(identity.version_uuid);
    if (found == versions.end()) return fail("unused_reclaim_evidence");
    const auto& row = *found->second;
    if (row.table_uuid != request.relation_uuid || row.row_uuid != identity.row.row_uuid.value ||
        row.creator_tx != identity.creator_transaction.local_id.value ||
        row.creator_transaction_uuid != identity.creator_transaction.transaction_uuid.value ||
        row.sequence != identity.version_sequence)
      return fail("reclaim_evidence_version_mismatch");
  }
  for (const auto& entry : request.state.index_entries) {
    if (!valid_uuid(entry.index_uuid) || !valid_uuid(entry.table_uuid) ||
        !valid_uuid(entry.row_uuid) || !valid_uuid(entry.version_uuid))
      return fail("invalid_index_target_identity");
    const auto target = versions.find(entry.version_uuid);
    if (target == versions.end() || target->second->table_uuid != entry.table_uuid ||
        target->second->row_uuid != entry.row_uuid)
      return fail("index_target_version_mismatch");
  }

  MgaRelationPhysicalSweepResult result;
  result.state = request.state;
  result.state.row_versions.clear();
  result.state.index_entries.clear();
  result.scanned_row_version_count = request.state.row_versions.size();
  result.scanned_index_entry_count = request.state.index_entries.size();
  for (const auto& row : request.state.row_versions) {
    if (reclaimed.contains(row.version_uuid)) ++result.removed_row_version_count;
    else { result.state.row_versions.push_back(row); ++result.retained_row_version_count; }
  }
  for (const auto& entry : request.state.index_entries) {
    if (reclaimed.contains(entry.version_uuid)) ++result.removed_index_entry_count;
    else { result.state.index_entries.push_back(entry); ++result.retained_index_entry_count; }
  }
  result.ok = true;
  result.staged_state_changed = result.removed_row_version_count != 0 || result.removed_index_entry_count != 0;
  result.diagnostic.error = false;
  return result;
}
} // namespace scratchbird::engine::internal_api
