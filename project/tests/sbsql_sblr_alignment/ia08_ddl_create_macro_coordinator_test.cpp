#include "engine/internal_api/sblr_ddl_create_macro_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="macro";c.trace_tags={"private_ddl_create_macro_binder"};auto x=CompileSblrDdlCreateMacroDescriptor(c,"macro",1,1,1);assert(x.ok);auto y=c;y.trace_tags={"private_ddl_create_macro"};auto z=ConsumeSblrDdlCreateMacroDescriptor(y,x.descriptor);assert(z.ok);auto replay=ConsumeSblrDdlCreateMacroDescriptor(y,x.descriptor);assert(!replay.ok&&replay.diagnostic.code=="MGA.TRANSACTION.STALE");return 0;}
