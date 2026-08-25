#include "engine/internal_api/sblr_sec_grant_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="s";c.trace_tags={"private_sec_grant_binder"};auto x=CompileSblrSecGrantDescriptor(c,"s",1,1);c.trace_tags={"private_sec_grant"};c.query_cancellation_requested=[](){return true;};assert(!ConsumeSblrSecGrantDescriptor(c,x.descriptor).ok);}
