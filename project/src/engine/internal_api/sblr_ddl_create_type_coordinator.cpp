#include "sblr_ddl_create_type_coordinator.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
namespace scratchbird::engine::internal_api {
static EngineRequestContext Alias(const EngineRequestContext& c,const char* from,const char* to){ auto x=c; for(auto& t:x.trace_tags) if(t==from)t=to; return x; }
SblrDdlCreateTypeCoordinationResult CompileSblrDdlCreateTypeDescriptor(const EngineRequestContext& c,const std::string& r,std::uint64_t o,std::uint32_t d,std::uint64_t a) { auto x=Alias(c,"private_ddl_create_type_binder","private_ddl_create_view_binder"); return CompileSblrDdlCreateViewDescriptor(x,r,o,d,a); }
SblrDdlCreateTypeCoordinationResult ConsumeSblrDdlCreateTypeDescriptor(const EngineRequestContext& c,const scratchbird::engine::sblr::SblrDdlCreateTypeDescriptorV1& v) { auto x=Alias(c,"private_ddl_create_type","private_ddl_create_view"); return ConsumeSblrDdlCreateViewDescriptor(x,v); }
}
