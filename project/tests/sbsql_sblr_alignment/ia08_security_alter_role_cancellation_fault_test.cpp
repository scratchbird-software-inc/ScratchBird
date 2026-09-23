#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_sec_alter_role_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid=scratchbird::tests::FixtureUuid(1154, 4);c.trace_tags.push_back("private_sec_alter_role_binder");auto x=CompileSblrSecAlterRoleDescriptor(c,scratchbird::tests::FixtureUuid(1154, 4),1,1);assert(x.ok);c.trace_tags.push_back("private_sec_alter_role");auto y=ConsumeSblrSecAlterRoleDescriptor(c,x.descriptor);assert(y.ok);auto z=ConsumeSblrSecAlterRoleDescriptor(c,x.descriptor);assert(!z.ok);return 0;}
