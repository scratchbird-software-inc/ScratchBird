#include "sblr_ddl_create_continuous_view_coordinator.hpp"
#include <mutex>
#include <unordered_map>
namespace scratchbird::engine::internal_api { namespace { std::mutex m; std::unordered_map<std::string,bool> live,used; std::string key(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrDdlCreateContinuousViewDescriptorV1&d){return c.session_uuid.canonical+"|"+std::to_string(d.availability)+"|"+std::to_string(d.body[16]);} }
SblrDdlCreateContinuousViewCoordinationResult CompileSblrDdlCreateContinuousViewDescriptor(const EngineRequestContext&c,const std::string&,std::uint64_t occ,std::uint32_t,std::uint64_t av){SblrDdlCreateContinuousViewCoordinationResult r;if(!occ||!av){r.diagnostic.code="SBLR.OPERAND.INVALID";return r;}r.descriptor.availability=av;r.descriptor.body[16]=static_cast<std::uint8_t>(occ);std::lock_guard<std::mutex>l(m);live[key(c,r.descriptor)]=true;r.ok=true;return r;}
SblrDdlCreateContinuousViewCoordinationResult ConsumeSblrDdlCreateContinuousViewDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrDdlCreateContinuousViewDescriptorV1&d){SblrDdlCreateContinuousViewCoordinationResult r;std::lock_guard<std::mutex>l(m);auto k=key(c,d);if(used[k]){r.diagnostic.code="MGA.TRANSACTION.STALE";return r;}if(!live[k]){r.diagnostic.code="SECURITY.ACCESS_DENIED";return r;}live.erase(k);used[k]=true;r.ok=true;return r;}
}
