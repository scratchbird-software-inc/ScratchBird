#include "engine/internal_api/sblr_versioned_branch_delete_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="branch";c.trace_tags={"private_versioned_branch_delete_binder","cluster_provider_admitted","cluster_route_fence_admitted"};auto q=CompileSblrVersionedBranchDeleteDescriptor(c,"branch",7);assert(q.ok);c.trace_tags.push_back("private_versioned_branch_delete");assert(ConsumeSblrVersionedBranchDeleteDescriptor(c,q.descriptor).ok);assert(!ConsumeSblrVersionedBranchDeleteDescriptor(c,q.descriptor).ok);return 0;}
