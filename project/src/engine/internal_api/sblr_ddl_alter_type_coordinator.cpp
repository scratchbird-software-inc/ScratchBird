#include "sblr_ddl_alter_type_coordinator.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
namespace scratchbird::engine::internal_api {
static EngineRequestContext Alias(const EngineRequestContext& c,const char* from,const char* to){ auto x=c; for(auto& t:x.trace_tags) if(t==from)t=to; return x; }
SblrDdlAlterTypeCoordinationResult CompileSblrDdlAlterTypeDescriptor(const EngineRequestContext& c,const std::string& r,std::uint64_t o,std::uint32_t d,std::uint64_t a) { auto x=Alias(c,"private_ddl_alter_type_binder","private_ddl_create_view_binder"); return CompileSblrDdlCreateViewDescriptor(x,r,o,d,a); }
SblrDdlAlterTypeCoordinationResult ConsumeSblrDdlAlterTypeDescriptor(const EngineRequestContext& c,const scratchbird::engine::sblr::SblrDdlAlterTypeDescriptorV1& v) { auto x=Alias(c,"private_ddl_alter_type","private_ddl_create_view"); return ConsumeSblrDdlCreateViewDescriptor(x,v); }
}
