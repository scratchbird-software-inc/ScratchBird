// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Consume cancellation, consume-once and stale semantics, over a descriptor
// the coordinator actually issued.
//
// The subject still comes from CompileSblrTableTruncateDescriptor, but the
// coordinator no longer reports ok for a three-byte body: it issues the
// complete SBSQL-TRUNCATE-TABLE-V1 section 6.1 carrier or refuses. So this
// fixture now supplies what the carrier requires -- the authenticated
// database/session/transaction/statement/snapshot tuple, the captured catalog
// and policy epochs, and the engine-resolved target the binder publishes --
// and therefore exercises a real descriptor rather than three bytes.

#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_table_truncate_coordinator.hpp"

#include <cassert>
#include <cstdio>

namespace a = scratchbird::engine::internal_api;

int main() {
  a::EngineRequestContext c;
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  // The authenticated tuple the carrier must match at issuance and execution.
  c.database_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002440");
  c.session_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002441");
  c.transaction_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002442");
  c.statement_snapshot_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002443");
  c.statement_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002444");
  // Captured epochs, both required non-zero by section 6.1.
  c.catalog_generation_id = 1;
  c.security_epoch = 1;
  c.trace_tags = {"private_table_truncate_compiler"};

  // The engine-resolved target. BindStatementTableTruncateTargetV1 publishes
  // this onto the receipt; the coordinator refuses without it.
  a::SblrTableTruncateResolvedTargetV1 target;
  target.persisted_descriptor_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002445");
  target.persisted_descriptor_generation = 3;

  auto r = a::CompileSblrTableTruncateDescriptor(c, c.statement_uuid, 1, 1, 1, target);
  if (!r.ok) {
    std::fprintf(stderr, "compile refused: %s:%s\n", r.diagnostic.code.c_str(),
                 r.diagnostic.message_key.c_str());
  }
  assert(r.ok);
  // A real carrier, not a stub: the encoder validated all 368 canonical body
  // bytes and the decoder recomputed the evidence digest.
  assert(r.descriptor.availability_generation == 1);
  assert(r.descriptor.evidence != a::SblrTableTruncateCoordinationResult{}.descriptor.evidence);

  // An absent target is a refusal, not a default.
  {
    a::SblrTableTruncateResolvedTargetV1 absent;
    const auto refused =
        a::CompileSblrTableTruncateDescriptor(c, c.statement_uuid, 1, 1, 1, absent);
    assert(!refused.ok);
    assert(refused.diagnostic.message_key ==
           "sblr.table_truncate.resolved_target_required");
  }

  c.trace_tags = {"private_table_truncate"};
  c.query_cancellation_requested = []() { return true; };
  assert(a::ConsumeSblrTableTruncateDescriptor(c, r.descriptor).diagnostic.code ==
         "PROCESS.CANCELLED");
  c.query_cancellation_requested = []() { return false; };
  assert(a::ConsumeSblrTableTruncateDescriptor(c, r.descriptor).ok);
  assert(a::ConsumeSblrTableTruncateDescriptor(c, r.descriptor).diagnostic.code ==
         "MGA.TRANSACTION.STALE");
  std::printf("PASS table truncate cancellation fault\n");
}
