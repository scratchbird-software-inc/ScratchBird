#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include <cassert>
int main() {
  namespace a = scratchbird::engine::internal_api;
  a::EngineRequestContext context;
  context.database_path = "/tmp/sb_create_materialized_view_2953";
  context.database_uuid.canonical = "019d0000-0000-7000-8000-000000002953";
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  a::SblrExecutorAvailabilityRowIdentity row{
      "engine.op.ddl_create_materialized_view", 1566, "1.0",
      "create_materialized_view_descriptor", "ddl_result", 1};
  auto snapshot = a::LoadSblrExecutorAvailabilitySnapshot(context, row);
  assert(!snapshot.ok);
  return 0;
}
