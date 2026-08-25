#include "engine/internal_api/sblr_ddl_alter_extension_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="extension";c.trace_tags={"private_ddl_alter_extension_binder","cluster_provider_admitted","cluster_route_fence_admitted"};auto q=CompileSblrDdlAlterExtensionDescriptor(c,"extension",7);assert(q.ok);c.trace_tags.push_back("private_ddl_alter_extension");assert(ConsumeSblrDdlAlterExtensionDescriptor(c,q.descriptor).ok);assert(!ConsumeSblrDdlAlterExtensionDescriptor(c,q.descriptor).ok);return 0;}
