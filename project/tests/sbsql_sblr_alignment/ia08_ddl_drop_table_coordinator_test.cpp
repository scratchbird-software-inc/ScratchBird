#include "engine/internal_api/sblr_ddl_drop_table_coordinator.hpp"
#include <cassert>

using namespace scratchbird::engine::internal_api;

int main() {
  EngineRequestContext c;
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.statement_uuid.canonical = "drop-table-test";
  c.trace_tags = {"private_ddl_drop_table_binder"};
  auto compiled = CompileSblrDdlDropTableDescriptor(
      c, c.statement_uuid.canonical, 1, 2, 7);
  assert(compiled.ok);

  c.trace_tags = {"private_ddl_drop_table"};
  auto consumed = ConsumeSblrDdlDropTableDescriptor(c, compiled.descriptor);
  assert(consumed.ok && consumed.descriptor.availability == 7);
  auto replay = ConsumeSblrDdlDropTableDescriptor(c, compiled.descriptor);
  assert(!replay.ok && replay.diagnostic.code == "MGA.TRANSACTION.STALE");

  c.trace_tags = {"private_ddl_drop_table_binder"};
  auto cancellable = CompileSblrDdlDropTableDescriptor(
      c, c.statement_uuid.canonical, 2, 3, 8);
  assert(cancellable.ok);
  c.trace_tags = {"private_ddl_drop_table"};
  c.query_cancellation_requested = [] { return true; };
  auto cancelled = ConsumeSblrDdlDropTableDescriptor(c, cancellable.descriptor);
  assert(!cancelled.ok && cancelled.diagnostic.code == "PROCESS.CANCELLED");
  c.query_cancellation_requested = {};
  auto recovered = ConsumeSblrDdlDropTableDescriptor(c, cancellable.descriptor);
  assert(recovered.ok);
  return 0;
}
