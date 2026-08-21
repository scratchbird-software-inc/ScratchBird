#include <cassert>
#include "engine/internal_api/sblr_ddl_drop_macro_coordinator.hpp"
using namespace scratchbird::engine::internal_api;
int main(){
  EngineRequestContext c; c.security_context_present=true;
  c.statement_metadata_snapshot_engine_owned=true; c.statement_uuid.canonical="macro";
  c.trace_tags={"private_ddl_drop_macro_binder"};
  auto x=CompileSblrDdlDropMacroDescriptor(c,"macro",1,1,1); assert(x.ok);
  auto y=c; y.trace_tags={"private_ddl_drop_macro"};
  auto z=ConsumeSblrDdlDropMacroDescriptor(y,x.descriptor); assert(z.ok);
  auto replay=ConsumeSblrDdlDropMacroDescriptor(y,x.descriptor);
  assert(!replay.ok && replay.diagnostic.code=="MGA.TRANSACTION.STALE");
  return 0;
}
