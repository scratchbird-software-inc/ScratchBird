#include "engine/internal_api/sblr_window_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){ EngineRequestContext c{}; c.security_context_present=true; c.statement_metadata_snapshot_engine_owned=true; c.statement_uuid.canonical="window-test"; c.trace_tags={"private_window_binder","private_window"}; auto x=CompileSblrWindowDescriptor(c,"window-test",1,1,1); assert(x.ok); c.query_cancellation_requested=[](){return true;}; auto y=ConsumeSblrWindowDescriptor(c,x.descriptor); assert(!y.ok); c.query_cancellation_requested=[](){return false;}; auto z=ConsumeSblrWindowDescriptor(c,x.descriptor); assert(z.ok); auto stale=ConsumeSblrWindowDescriptor(c,x.descriptor); assert(!stale.ok); return 0; }
