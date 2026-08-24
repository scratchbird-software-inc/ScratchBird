#include "engine/internal_api/sblr_sec_drop_group_mapping_coordinator.hpp"
#include <cassert>
int main(){using namespace scratchbird::engine::internal_api; EngineRequestContext c; c.security_context_present=true;c.statement_uuid.canonical="s";c.trace_tags={"private_sec_drop_group_mapping_binder"};auto x=CompileSblrSecDropGroupMappingDescriptor(c,"s",1,2);assert(x.ok);c.trace_tags={"private_sec_drop_group_mapping"};c.query_cancellation_requested=[](){return true;};auto y=ConsumeSblrSecDropGroupMappingDescriptor(c,x.descriptor);assert(!y.ok);return 0;}
