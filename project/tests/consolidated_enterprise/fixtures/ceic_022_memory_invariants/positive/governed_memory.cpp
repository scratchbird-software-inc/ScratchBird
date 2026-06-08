// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Positive CEIC-022 fixture: scoped memory use with context, reservations,
// protected zeroization, support-bundle redaction, and non-authority evidence.

#include "memory.hpp"
#include "query_memory_arena.hpp"
#include "reservation_backed_memory_resource.hpp"

namespace ceic_022_positive {

using namespace scratchbird::core::memory;

void GovernedMemoryExample(MemoryManager& manager,
                           HierarchicalMemoryBudgetLedger& ledger) {
  MemoryTag tag;
  tag.context_id = "ctx";
  tag.session_id = "session";
  tag.transaction_id = "transaction";
  tag.statement_id = "statement";
  tag.query_id = "query";
  tag.owner = "owner";
  (void)manager.AllocateScoped(64, 8, tag);

  QueryMemoryContext context;
  context.database_id = "database";
  context.session_id = "session";
  context.transaction_id = "transaction";
  context.statement_id = "statement";
  context.query_id = "query";
  context.engine_mga_authoritative = true;
  QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 1024;
  QueryMemoryArena arena(context, limits, manager.allocator());
  QueryMemoryGrantRequest grant;
  grant.family = QueryMemoryFamily::relational;
  grant.bytes = 32;
  (void)arena.Grant(grant);

  ProtectedMemoryEvidence protected_evidence;
  protected_evidence.zero_on_release = true;
  protected_evidence.protected_material_redacted = true;

  PageBufferRequest page_request;
  page_request.page_size = 4096;
  page_request.page_count = 1;
  page_request.tag = tag;
  ScopedPageBufferResult page = manager.AllocateScopedPageBuffer(page_request);
  (void)page;

  TempWorkspaceBudgetReservationEvidence spill_budget;
  spill_budget.ceic_011_reservation_required = true;
  spill_budget.ceic_011_reservation_released = true;
  UnifiedMemorySpillBudgetLedger unified("fixture", 1024);
  (void)unified.Snapshot();

  HierarchicalMemoryReservationRequest reservation;
  reservation.scope_chain = {{HierarchicalMemoryScopeKind::query, "query"}};
  reservation.requested_bytes = 16;
  reservation.owner_id = "owner";
  auto reserved = ledger.Reserve(reservation);
  if (reserved.ok()) {
    (void)ledger.Release(reserved.token);
  }

  MemorySupportBundleRequest bundle;
  bundle.allow_protected_material = false;
  const char* authority_scope =
      "memory_evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";
  (void)authority_scope;
  (void)bundle;
}

}  // namespace ceic_022_positive
