#include "engine/internal_api/sblr_procedure_invoke_coordinator.hpp"

#include <cassert>

int main() {
  namespace a = scratchbird::engine::internal_api;
  a::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_uuid.canonical = "019d0000-0000-7000-8000-000000002504";
  context.trace_tags = {"private_procedure_invoke_binder"};
  const auto compiled = a::CompileSblrProcedureInvokeDescriptor(
      context, context.statement_uuid.canonical, 1, 1, 1);
  assert(compiled.ok);
  context.trace_tags = {"private_procedure_invoke"};
  context.query_cancellation_requested = [] { return true; };
  assert(a::ConsumeSblrProcedureInvokeDescriptor(context, compiled.descriptor)
             .diagnostic.code == "PROCESS.CANCELLED");
  context.query_cancellation_requested = [] { return false; };
  assert(a::ConsumeSblrProcedureInvokeDescriptor(context, compiled.descriptor).ok);
  assert(a::ConsumeSblrProcedureInvokeDescriptor(context, compiled.descriptor)
             .diagnostic.code == "MGA.TRANSACTION.STALE");
}
