#include "sblr_versioned_revert_coordinator.hpp"
#include "api_diagnostics.hpp"
#include <algorithm>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api { namespace {
std::mutex g_mutex; using D=scratchbird::engine::sblr::SblrVersionedRevertDescriptorV1; std::map<std::string,D> live,used;
std::string Key(const D& d){return {(const char*)d.body.data(),d.body.size()};}
bool Tag(const EngineRequestContext& c,const char* t){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),t)!=c.trace_tags.end();}
EngineApiDiagnostic Diag(std::string c,std::string d){return MakeEngineApiDiagnostic(std::move(c),std::move(d),{});}
}
SblrVersionedRevertCoordinationResult CompileSblrVersionedRevertDescriptor(const EngineRequestContext& c,const std::string& n,std::uint64_t g){
 std::lock_guard l(g_mutex); SblrVersionedRevertCoordinationResult o;
 if(!Tag(c,"private_versioned_revert_binder")||!c.statement_metadata_snapshot_engine_owned||n!=c.statement_uuid.canonical||!g||!Tag(c,"cluster_provider_admitted")||!Tag(c,"cluster_route_fence_admitted")){o.diagnostic=Diag("CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN","sblr.versioned_revert.cluster_gate");return o;}
 o.descriptor.body[0]=1; o.descriptor.body[16]=1; for(int i=0;i<8;++i)o.descriptor.body[72+i]=(std::uint8_t)(g>>(8*i));
 if(scratchbird::engine::sblr::EncodeSblrVersionedRevertDescriptorV1(o.descriptor).empty()){o.diagnostic=Diag("SBLR.OPERAND.INVALID","sblr.versioned_revert.descriptor_invalid");return o;}
 live[Key(o.descriptor)]=o.descriptor; o.ok=true; o.diagnostic=Diag("OK","ok"); return o;
}
SblrVersionedRevertCoordinationResult ConsumeSblrVersionedRevertDescriptor(const EngineRequestContext& c,const D& d){
 std::lock_guard l(g_mutex); SblrVersionedRevertCoordinationResult o; auto k=Key(d);
 if(!Tag(c,"private_versioned_revert")){o.diagnostic=Diag("SECURITY.ACCESS_DENIED","sblr.versioned_revert.hidden");return o;}
 auto i=live.find(k); if(i==live.end()){o.diagnostic=used.count(k)?Diag("MGA.TRANSACTION.STALE","sblr.versioned_revert.stale"):Diag("SECURITY.ACCESS_DENIED","sblr.versioned_revert.hidden");return o;}
 if(c.query_cancellation_requested&&c.query_cancellation_requested()){o.diagnostic=Diag("PROCESS.CANCELLED","sblr.versioned_revert.cancelled");return o;}
 used[k]=d; live.erase(i); o.ok=true; o.descriptor=d; o.diagnostic=Diag("OK","ok"); return o;
}
}
