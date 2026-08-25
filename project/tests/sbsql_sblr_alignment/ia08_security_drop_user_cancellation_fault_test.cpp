#include "engine/internal_api/sblr_sec_drop_user_coordinator.hpp"
#include <cassert>
int main(){scratchbird::engine::internal_api::EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";c.trace_tags={"private_sec_drop_user_binder"};auto x=scratchbird::engine::internal_api::CompileSblrSecDropUserDescriptor(c,"r",1,1);assert(x.ok);c.trace_tags={"private_sec_drop_user"};c.query_cancellation_requested=[](){return true;};auto y=scratchbird::engine::internal_api::ConsumeSblrSecDropUserDescriptor(c,x.descriptor);assert(!y.ok);return 0;}
