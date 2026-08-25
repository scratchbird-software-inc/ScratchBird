#include "engine/internal_api/sblr_ddl_drop_materialized_view_coordinator.hpp"
#include <cassert>
int main() {
  namespace a = scratchbird::engine::internal_api;
  a::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_uuid.canonical = "019d0000-0000-7000-8000-000000002952";
  context.trace_tags = {"private_ddl_create_view_binder"};
  auto compiled = a::CompileSblrDdlDropMaterializedViewDescriptor(
      context, context.statement_uuid.canonical, 1, 1, 1);
  // The current coordinator is only a type alias/delegation seam and does not
  // admit a materialized-view-specific binder route. Preserve the fail-closed
  // contract until that route is authored; this test must not claim
  // cancellation coverage that the route cannot reach.
  assert(!compiled.ok && compiled.diagnostic.code == "SBLR.OPERAND.INVALID");
  return 0;
}
