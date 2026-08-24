#include "engine/internal_api/sblr_ddl_create_fdw_coordinator.hpp"
#include <cassert>
int main(){scratchbird::engine::internal_api::EngineRequestContext c;c.trace_tags.push_back("private_ddl_create_fdw_binder");auto r=scratchbird::engine::internal_api::CompileSblrDdlCreateFdwDescriptor(c,"00000000-0000-0000-0000-000000000000",1,1,1);assert(!r.ok);return 0;}
