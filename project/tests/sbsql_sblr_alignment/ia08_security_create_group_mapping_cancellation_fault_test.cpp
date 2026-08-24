#include "engine/internal_api/sblr_sec_create_group_mapping_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api;EngineRequestContext c;c.security_context_present=true;c.statement_uuid.canonical="r";c.trace_tags={"private_sec_create_group_mapping_binder","private_sec_create_group_mapping"};auto x=CompileSblrSecCreateGroupMappingDescriptor(c,"r",1,1);assert(x.ok);auto y=ConsumeSblrSecCreateGroupMappingDescriptor(c,x.descriptor);assert(y.ok);auto z=ConsumeSblrSecCreateGroupMappingDescriptor(c,x.descriptor);assert(!z.ok);return 0;}
