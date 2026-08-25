#include "engine/internal_api/sblr_sec_alter_policy_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="s";c.trace_tags={"private_sec_alter_policy_binder"};assert(CompileSblrSecAlterPolicyDescriptor(c,"s",1,1).ok);}
