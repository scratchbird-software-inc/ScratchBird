#include "engine/internal_api/sblr_ddl_create_materialized_view_coordinator.hpp"
#include <cassert>

int main() {
  namespace a = scratchbird::engine::internal_api;
  a::EngineRequestContext c;
  c.database_path = "/tmp/sb_create_materialized_view_coord_2955";
  c.database_uuid.canonical = "019d0000-0000-7000-8000-000000002955";
  c.statement_uuid.canonical = "019d0000-0000-7000-8000-000000002956";
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.trace_tags = {"private_ddl_create_materialized_view_binder"};
  auto bad = a::CompileSblrDdlCreateMaterializedViewDescriptor(c, "wrong", 1, 1, 1);
  assert(!bad.ok && bad.diagnostic.code == "SBLR.OPERAND_INVALID");
  auto compiled = a::CompileSblrDdlCreateMaterializedViewDescriptor(
      c, c.statement_uuid.canonical, 11, 3, 17);
  assert(compiled.ok && compiled.descriptor.availability == 17);

  auto hidden_context = c;
  hidden_context.security_context_present = false;
  hidden_context.trace_tags.clear();
  auto hidden = a::ConsumeSblrDdlCreateMaterializedViewDescriptor(hidden_context, compiled.descriptor);
  assert(!hidden.ok && hidden.diagnostic.code == "SECURITY.ACCESS_DENIED");
  auto consume = c;
  consume.trace_tags = {"private_ddl_create_materialized_view"};
  auto done = a::ConsumeSblrDdlCreateMaterializedViewDescriptor(consume, compiled.descriptor);
  assert(done.ok);
  auto replay = a::ConsumeSblrDdlCreateMaterializedViewDescriptor(consume, compiled.descriptor);
  assert(!replay.ok && replay.diagnostic.code == "MGA.TRANSACTION.STALE");

  auto c2 = c;
  c2.statement_uuid.canonical = "019d0000-0000-7000-8000-000000002957";
  auto pending = a::CompileSblrDdlCreateMaterializedViewDescriptor(
      c2, c2.statement_uuid.canonical, 12, 4, 18);
  assert(pending.ok);
  auto cancel = c2;
  cancel.trace_tags = {"private_ddl_create_materialized_view"};
  cancel.query_cancellation_requested = [] { return true; };
  auto cancelled = a::ConsumeSblrDdlCreateMaterializedViewDescriptor(cancel, pending.descriptor);
  assert(!cancelled.ok && cancelled.diagnostic.code == "PROCESS.CANCELLED");
  auto retry = c2;
  retry.trace_tags = {"private_ddl_create_materialized_view"};
  auto recovered = a::ConsumeSblrDdlCreateMaterializedViewDescriptor(retry, pending.descriptor);
  assert(recovered.ok);
  return 0;
}
