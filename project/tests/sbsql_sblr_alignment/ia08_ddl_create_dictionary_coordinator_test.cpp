#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_ddl_create_dictionary_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){
  EngineRequestContext c; c.security_context_present=true; c.statement_metadata_snapshot_engine_owned=true;
  c.statement_uuid=scratchbird::tests::FixtureUuid(0xc008, 1); c.trace_tags={"private_ddl_create_dictionary_binder"};
  auto made=CompileSblrDdlCreateDictionaryDescriptor(c,c.statement_uuid,1,1,1); assert(made.ok);
  auto cancel=c; cancel.trace_tags={"private_ddl_create_dictionary"};
  auto consumed=ConsumeSblrDdlCreateDictionaryDescriptor(cancel,made.descriptor); assert(consumed.ok);
  auto replay=ConsumeSblrDdlCreateDictionaryDescriptor(cancel,made.descriptor); assert(!replay.ok && replay.diagnostic.code=="MGA.TRANSACTION.STALE");
  return 0;
}
