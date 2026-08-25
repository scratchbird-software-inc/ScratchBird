#include "sblr_ddl_drop_type_coordinator.hpp"
#include "sblr_ddl_drop_view_coordinator.hpp"
namespace scratchbird::engine::internal_api {
static EngineRequestContext Alias(const EngineRequestContext& c,const char* from,const char* to){ auto x=c; for(auto& t:x.trace_tags) if(t==from)t=to; return x; }
SblrDdlDropTypeCoordinationResult CompileSblrDdlDropTypeDescriptor(const EngineRequestContext& c,const std::string& r,std::uint64_t o,std::uint32_t d,std::uint64_t a) { auto x=Alias(c,"private_ddl_drop_type_binder","private_ddl_drop_view_binder"); auto q=CompileSblrDdlDropViewDescriptor(x,r,o,d,a); SblrDdlDropTypeCoordinationResult out; out.ok=q.ok; out.descriptor=q.descriptor; out.diagnostic=std::move(q.diagnostic); return out; }
SblrDdlDropTypeCoordinationResult ConsumeSblrDdlDropTypeDescriptor(const EngineRequestContext& c,const scratchbird::engine::sblr::SblrDdlDropTypeDescriptorV1& v) { auto x=Alias(c,"private_ddl_drop_type","private_ddl_drop_view"); auto q=ConsumeSblrDdlDropViewDescriptor(x,v); SblrDdlDropTypeCoordinationResult out; out.ok=q.ok; out.descriptor=q.descriptor; out.diagnostic=std::move(q.diagnostic); return out; }
}
