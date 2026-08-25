#include "engine/internal_api/sblr_sec_deauthenticate_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";c.trace_tags={"private_sec_deauthenticate_binder"};auto x=CompileSblrSecDeauthenticateDescriptor(c,"r",1,1);c.trace_tags={"private_sec_deauthenticate"};c.query_cancellation_requested=[](){return true;};assert(!ConsumeSblrSecDeauthenticateDescriptor(c,x.descriptor).ok);return 0;}
