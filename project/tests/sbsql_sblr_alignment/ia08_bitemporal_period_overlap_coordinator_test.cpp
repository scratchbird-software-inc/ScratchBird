#include "engine/internal_api/sblr_bitemporal_period_overlap_coordinator.hpp"
#include <cassert>
using namespace scratchbird::engine::internal_api;
int main(){EngineRequestContext c;c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;c.statement_uuid.canonical="bitemporal";c.trace_tags={"private_bitemporal_period_overlap_binder","cluster_provider_admitted","cluster_route_fence_admitted"};auto q=CompileSblrBitemporalPeriodOverlapDescriptor(c,"bitemporal",7);assert(q.ok);c.trace_tags.push_back("private_bitemporal_period_overlap");assert(ConsumeSblrBitemporalPeriodOverlapDescriptor(c,q.descriptor).ok);assert(!ConsumeSblrBitemporalPeriodOverlapDescriptor(c,q.descriptor).ok);return 0;}
