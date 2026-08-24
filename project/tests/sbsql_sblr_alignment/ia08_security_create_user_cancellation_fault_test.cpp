#include "engine/internal_api/sblr_security_create_user_coordinator.hpp"
#include <cassert>
int main(){scratchbird::engine::internal_api::EngineRequestContext c;c.trace_tags.push_back("private_security_create_user_binder");auto r=scratchbird::engine::internal_api::CompileSblrSecurityCreateUserDescriptor(c,"00000000-0000-0000-0000-000000000000",1,1,1);assert(!r.ok);return 0;}
