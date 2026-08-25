#include "engine/internal_api/sblr_versioned_diff_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="diff";c.trace_tags={"private_versioned_diff_binder","cluster_provider_admitted","cluster_route_fence_admitted"};auto q=CompileSblrVersionedDiffDescriptor(c,"diff",7);assert(q.ok);c.trace_tags.push_back("private_versioned_diff");assert(ConsumeSblrVersionedDiffDescriptor(c,q.descriptor).ok);assert(!ConsumeSblrVersionedDiffDescriptor(c,q.descriptor).ok);return 0;}
