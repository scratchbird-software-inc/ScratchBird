#include "sblr_ddl_set_table_type_enforcement_coordinator.hpp"
#include "api_diagnostics.hpp"
#include <algorithm>
#include <map>
#include <mutex>
namespace scratchbird::engine::internal_api { namespace {
std::mutex m; using D=scratchbird::engine::sblr::SblrDdlSetTableTypeEnforcementDescriptorV1; std::map<std::string,D> live,used;
std::string key(const std::array<std::uint8_t,32>&x){return{reinterpret_cast<const char*>(x.data()),x.size()};}
bool tag(const EngineRequestContext&c,const char*t){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),t)!=c.trace_tags.end();}
EngineApiDiagnostic err(std::string c,std::string k){return MakeEngineApiDiagnostic(std::move(c),std::move(k),{});}
}
SblrDdlSetTableTypeEnforcementCoordinationResult CompileSblrDdlSetTableTypeEnforcementDescriptor(const EngineRequestContext&c,const std::string&r,std::uint64_t o,std::uint32_t t,std::uint64_t g){SblrDdlSetTableTypeEnforcementCoordinationResult x;std::lock_guard l(m);if(!tag(c,"private_ddl_set_table_type_enforcement_binder")||!c.statement_metadata_snapshot_engine_owned||r!=c.statement_uuid.canonical||!o||!t){x.diagnostic=err("SBLR.OPERAND.INVALID","ddl_set_table_type_enforcement.invalid");return x;}x.descriptor.body[0]=1;x.descriptor.body[8]=std::uint8_t(o);x.descriptor.body[12]=std::uint8_t(t);x.descriptor.availability=g?g:1;auto encoded=scratchbird::engine::sblr::EncodeSblrDdlSetTableTypeEnforcementDescriptorV1(x.descriptor,false);if(encoded.empty()||!scratchbird::engine::sblr::DecodeSblrDdlSetTableTypeEnforcementDescriptorV1(encoded.data(),encoded.size(),&x.descriptor,nullptr,false)){x.diagnostic=err("SBLR.OPERAND.INVALID","ddl_set_table_type_enforcement.descriptor_invalid");return x;}live[key(x.descriptor.evidence)]=x.descriptor;x.ok=true;return x;}
SblrDdlSetTableTypeEnforcementCoordinationResult ConsumeSblrDdlSetTableTypeEnforcementDescriptor(const EngineRequestContext&c,const D&v){SblrDdlSetTableTypeEnforcementCoordinationResult x;std::lock_guard l(m);if(!tag(c,"private_ddl_set_table_type_enforcement")){x.diagnostic=err("SECURITY.ACCESS_DENIED","ddl_set_table_type_enforcement.hidden");return x;}auto i=live.find(key(v.evidence));if(i==live.end()){x.diagnostic=err(used.count(key(v.evidence))?"MGA.TRANSACTION.STALE":"SECURITY.ACCESS_DENIED","ddl_set_table_type_enforcement.replay");return x;}if(c.query_cancellation_requested&&c.query_cancellation_requested()){x.diagnostic=err("PROCESS.CANCELLED","ddl_set_table_type_enforcement.cancelled");return x;}used[key(v.evidence)]=v;live.erase(i);x.ok=true;x.descriptor=v;return x;}
}
