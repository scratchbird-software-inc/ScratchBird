#include "engine/internal_api/sblr_table_analyze_coordinator.hpp"
#include <cassert>

int main() {
  namespace a = scratchbird::engine::internal_api;
  a::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_uuid.canonical = "019d0000-0000-7000-8000-000000002448";
  context.trace_tags = {"private_table_analyze_compiler"};
  const auto compiled = a::CompileSblrTableAnalyzeDescriptor(context, context.statement_uuid.canonical, 1, 1, 1);
  assert(compiled.ok);
  context.trace_tags = {"private_table_analyze"};
  context.query_cancellation_requested = [] { return true; };
  assert(a::ConsumeSblrTableAnalyzeDescriptor(context, compiled.descriptor).diagnostic.code == "PROCESS.CANCELLED");
  context.query_cancellation_requested = [] { return false; };
  assert(a::ConsumeSblrTableAnalyzeDescriptor(context, compiled.descriptor).ok);
  assert(a::ConsumeSblrTableAnalyzeDescriptor(context, compiled.descriptor).diagnostic.code == "MGA.TRANSACTION.STALE");
}
