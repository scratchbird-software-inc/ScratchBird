#include "engine/internal_api/sblr_ddl_alter_collation_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="c";c.trace_tags={"private_ddl_alter_collation_binder","cluster_provider_admitted","cluster_route_fence_admitted"};auto q=CompileSblrDdlAlterCollationDescriptor(c,"c",7);assert(q.ok);c.trace_tags.push_back("private_ddl_alter_collation");assert(ConsumeSblrDdlAlterCollationDescriptor(c,q.descriptor).ok);}
