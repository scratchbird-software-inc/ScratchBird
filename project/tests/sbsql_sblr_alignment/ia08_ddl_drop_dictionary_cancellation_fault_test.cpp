#include "engine/internal_api/sblr_ddl_drop_dictionary_coordinator.hpp"
#include <cassert>

using namespace scratchbird::engine::internal_api;

int main() {
  EngineRequestContext compiler;
  compiler.security_context_present = true;
  compiler.statement_metadata_snapshot_engine_owned = true;
  compiler.statement_uuid.canonical = "dictionary-cancel";
  compiler.trace_tags = {"private_ddl_drop_dictionary_binder"};
  auto compiled = CompileSblrDdlDropDictionaryDescriptor(compiler, "dictionary-cancel", 1, 1, 1);
  assert(compiled.ok);

  EngineRequestContext cancelled = compiler;
  cancelled.trace_tags = {"private_ddl_drop_dictionary"};
  cancelled.query_cancellation_requested = [] { return true; };
  auto refused = ConsumeSblrDdlDropDictionaryDescriptor(cancelled, compiled.descriptor);
  assert(!refused.ok && refused.diagnostic.code == "PROCESS.CANCELLED");

  EngineRequestContext consumer = compiler;
  consumer.trace_tags = {"private_ddl_drop_dictionary"};
  auto consumed = ConsumeSblrDdlDropDictionaryDescriptor(consumer, compiled.descriptor);
  assert(consumed.ok);
  return 0;
}
