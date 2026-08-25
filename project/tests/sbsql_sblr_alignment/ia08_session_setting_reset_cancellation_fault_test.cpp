#include "engine/internal_api/sblr_session_setting_reset_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";auto x=CompileSblrSessionSettingResetDescriptor(c,"r",1,1);assert(x.ok);c.query_cancellation_requested=[](){return true;};assert(!ConsumeSblrSessionSettingResetDescriptor(c,x.descriptor).ok);return 0;}
