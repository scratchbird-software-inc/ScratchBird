#include "engine/internal_api/sblr_session_role_switch_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";c.trace_tags={"private_sec_role_switch_binder"};auto x=CompileSblrSessionRoleSwitchDescriptor(c,"r",1,1);c.trace_tags={"private_sec_role_switch"};c.query_cancellation_requested=[](){return true;};assert(!ConsumeSblrSessionRoleSwitchDescriptor(c,x.descriptor).ok);return 0;}
