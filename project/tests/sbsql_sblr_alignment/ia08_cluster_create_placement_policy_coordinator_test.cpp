#include "engine/internal_api/sblr_cluster_create_placement_policy_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;auto q=CompileSblrClusterCreatePlacementPolicyDescriptor(c,"policy",7);assert(!q.ok);c.trace_tags={"cluster_provider_admitted","cluster_route_fence_admitted"};auto r=ConsumeSblrClusterCreatePlacementPolicyDescriptor(c,q.descriptor);assert(!r.ok);}
