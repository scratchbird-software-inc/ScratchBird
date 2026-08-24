#include "engine/internal_api/sblr_sec_create_group_mapping_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";c.trace_tags.push_back("private_sec_create_group_mapping_binder");auto x=CompileSblrSecCreateGroupMappingDescriptor(c,"r",1,1);assert(x.ok&&x.descriptor.availability==1);return 0;}
