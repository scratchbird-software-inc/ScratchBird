#include "engine/internal_api/sblr_bitemporal_as_of_valid_time_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){
  EngineRequestContext c; c.security_context_present=true; c.statement_metadata_snapshot_engine_owned=true;
  c.statement_uuid.canonical="bitemporal";
  c.trace_tags={"private_bitemporal_as_of_valid_time_binder","cluster_provider_admitted","cluster_route_fence_admitted"};
  auto q=CompileSblrBitemporalAsOfValidTimeDescriptor(c,"bitemporal",7); assert(q.ok);
  c.trace_tags.push_back("private_bitemporal_as_of_valid_time");
  assert(ConsumeSblrBitemporalAsOfValidTimeDescriptor(c,q.descriptor).ok);
  assert(!ConsumeSblrBitemporalAsOfValidTimeDescriptor(c,q.descriptor).ok);
  return 0;
}
