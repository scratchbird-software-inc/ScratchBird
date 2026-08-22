#include "engine/internal_api/sblr_dml_conditional_mutate_coordinator.hpp"

#include <cassert>

int main() {
  using namespace scratchbird::engine::internal_api;
  EngineRequestContext context;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_uuid.canonical = "conditional-receipt";
  context.trace_tags.push_back("private_dml_conditional_mutate_binder");
  auto compiled = CompileSblrDmlConditionalMutateDescriptor(
      context, "conditional-receipt", 7, 9, 1);
  assert(compiled.ok);
  context.trace_tags.clear();
  context.trace_tags.push_back("private_dml_conditional_mutate");
  auto consumed = ConsumeSblrDmlConditionalMutateDescriptor(context, compiled.descriptor);
  assert(consumed.ok);
  auto replay = ConsumeSblrDmlConditionalMutateDescriptor(context, compiled.descriptor);
  assert(!replay.ok && replay.diagnostic.code == "MGA.TRANSACTION.STALE");
  return 0;
}
