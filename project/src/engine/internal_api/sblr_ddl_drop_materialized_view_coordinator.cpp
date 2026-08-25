#include "sblr_ddl_drop_materialized_view_coordinator.hpp"
#include "api_diagnostics.hpp"
#include <algorithm>
#include <map>
#include <mutex>
namespace scratchbird::engine::internal_api {
namespace { using D=scratchbird::engine::sblr::SblrDdlDropMaterializedViewDescriptorV1; std::mutex m; std::map<std::string,D> live,used;
std::string key(const scratchbird::engine::sblr::DdlCreateViewSha& h){return {reinterpret_cast<const char*>(h.data()),h.size()};}
bool tag(const EngineRequestContext& c,const char* t){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),t)!=c.trace_tags.end();}
EngineApiDiagnostic diag(std::string c,std::string k){return MakeEngineApiDiagnostic(std::move(c),std::move(k),{});} }
SblrDdlDropMaterializedViewCoordinationResult CompileSblrDdlDropMaterializedViewDescriptor(const EngineRequestContext& c,const std::string& r,std::uint64_t o,std::uint32_t d,std::uint64_t a){
 SblrDdlDropMaterializedViewCoordinationResult x; std::lock_guard l(m);
 if(!tag(c,"private_ddl_drop_materialized_view_binder")||!c.statement_metadata_snapshot_engine_owned||r!=c.statement_uuid.canonical||!o||!d||!a){x.diagnostic=diag("SBLR.OPERAND.INVALID","sblr.ddl_drop_materialized_view.coordination_invalid");return x;}
 x.descriptor.body[0]=1; x.descriptor.body[1]=std::uint8_t(o); x.descriptor.body[2]=std::uint8_t(d); x.descriptor.availability=a;
 auto b=scratchbird::engine::sblr::EncodeSblrDdlDropMaterializedViewDescriptorV1(x.descriptor,false);
 if(b.empty()||!scratchbird::engine::sblr::DecodeSblrDdlDropMaterializedViewDescriptorV1(b.data(),b.size(),&x.descriptor,nullptr,false)){x.diagnostic=diag("SBLR.OPERAND.INVALID","sblr.ddl_drop_materialized_view.descriptor_invalid");return x;}
 live[key(x.descriptor.evidence)]=x.descriptor; x.ok=true; x.diagnostic=diag("OK","ok"); return x;
}
SblrDdlDropMaterializedViewCoordinationResult ConsumeSblrDdlDropMaterializedViewDescriptor(const EngineRequestContext& c,const D& v){
 SblrDdlDropMaterializedViewCoordinationResult x; std::lock_guard l(m); auto k=key(v.evidence);
 if(!tag(c,"private_ddl_drop_materialized_view")){x.diagnostic=diag("SECURITY.ACCESS_DENIED","sblr.ddl_drop_materialized_view.hidden");return x;}
 auto it=live.find(k); if(it==live.end()){x.diagnostic=diag(used.count(k)?"MGA.TRANSACTION.STALE":"SECURITY.ACCESS_DENIED","sblr.ddl_drop_materialized_view.replay");return x;}
 if(c.query_cancellation_requested&&c.query_cancellation_requested()){x.diagnostic=diag("PROCESS.CANCELLED","sblr.ddl_drop_materialized_view.cancelled");return x;}
 used[k]=v; live.erase(it); x.ok=true; x.descriptor=v; x.diagnostic=diag("OK","ok"); return x;
}
}
