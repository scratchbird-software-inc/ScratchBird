#include "engine/internal_api/sblr_sec_authenticate_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";c.trace_tags={"private_sec_authenticate_binder"};auto x=CompileSblrSecAuthenticateDescriptor(c,"r",1,1);c.trace_tags={"private_sec_authenticate"};c.query_cancellation_requested=[](){return true;};assert(!ConsumeSblrSecAuthenticateDescriptor(c,x.descriptor).ok);return 0;}
