#include "engine/internal_api/sblr_sec_create_policy_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";c.trace_tags={"private_sec_create_policy_binder"};auto x=CompileSblrSecCreatePolicyDescriptor(c,"r",1,1);assert(x.ok);c.trace_tags={"private_sec_create_policy"};c.query_cancellation_requested=[](){return true;};auto y=ConsumeSblrSecCreatePolicyDescriptor(c,x.descriptor);assert(!y.ok);return 0;}
