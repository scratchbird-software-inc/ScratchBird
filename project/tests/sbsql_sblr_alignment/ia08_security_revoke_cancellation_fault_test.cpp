#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_sec_revoke_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid=scratchbird::tests::FixtureUuid(1154, 20);c.trace_tags={"private_sec_revoke_binder"};auto x=CompileSblrSecRevokeDescriptor(c,scratchbird::tests::FixtureUuid(1154, 20),1,1);c.trace_tags={"private_sec_revoke"};c.query_cancellation_requested=[](){return true;};assert(!ConsumeSblrSecRevokeDescriptor(c,x.descriptor).ok);}
