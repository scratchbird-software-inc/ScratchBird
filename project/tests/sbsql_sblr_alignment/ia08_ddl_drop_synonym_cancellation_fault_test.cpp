#include "engine/internal_api/sblr_ddl_drop_synonym_coordinator.hpp"
#include <cassert>

int main() {
  using namespace scratchbird::engine::internal_api;
  EngineRequestContext c;
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.statement_uuid.canonical = "019d0000-0000-7000-8000-000000002948";
  c.trace_tags = {"private_ddl_drop_synonym_binder"};
  auto compiled = CompileSblrDdlDropSynonymDescriptor(
      c, c.statement_uuid.canonical, 1, 2, 7);
  assert(compiled.ok);
  c.trace_tags = {"private_ddl_drop_synonym"};
  c.query_cancellation_requested = [] { return true; };
  auto cancelled = ConsumeSblrDdlDropSynonymDescriptor(c, compiled.descriptor);
  assert(!cancelled.ok && cancelled.diagnostic.code == "PROCESS.CANCELLED");
  c.query_cancellation_requested = {};
  auto recovered = ConsumeSblrDdlDropSynonymDescriptor(c, compiled.descriptor);
  assert(recovered.ok);
  auto replay = ConsumeSblrDdlDropSynonymDescriptor(c, compiled.descriptor);
  assert(!replay.ok && replay.diagnostic.code == "MGA.TRANSACTION.STALE");
  return 0;
}
