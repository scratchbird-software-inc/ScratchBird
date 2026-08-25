#include "engine/internal_api/sblr_cluster_declare_availability_zone_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;int main(){EngineRequestContext c;c.security_context_present=true;auto q=CompileSblrClusterDeclareAvailabilityZoneDescriptor(c,"zone",7);assert(!q.ok);auto r=ConsumeSblrClusterDeclareAvailabilityZoneDescriptor(c,q.descriptor);assert(!r.ok);}
