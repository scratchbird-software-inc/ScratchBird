#include "engine/internal_api/sblr_versioned_tag_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="tag";c.trace_tags={"private_versioned_tag_binder","cluster_provider_admitted","cluster_route_fence_admitted"};auto q=CompileSblrVersionedTagDescriptor(c,"tag",7);assert(q.ok);c.trace_tags.push_back("private_versioned_tag");assert(ConsumeSblrVersionedTagDescriptor(c,q.descriptor).ok);return 0;}
