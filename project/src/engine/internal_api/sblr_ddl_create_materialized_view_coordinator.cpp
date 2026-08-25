#include "sblr_ddl_create_materialized_view_coordinator.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
namespace scratchbird::engine::internal_api {
SblrDdlCreateMaterializedViewCoordinationResult CompileSblrDdlCreateMaterializedViewDescriptor(const EngineRequestContext& c,const std::string& r,std::uint64_t o,std::uint32_t p,std::uint64_t a) { auto x=c; x.trace_tags.push_back("private_ddl_create_view_binder"); return CompileSblrDdlCreateViewDescriptor(x,r,o,p,a); }
SblrDdlCreateMaterializedViewCoordinationResult ConsumeSblrDdlCreateMaterializedViewDescriptor(const EngineRequestContext& c,const scratchbird::engine::sblr::SblrDdlCreateMaterializedViewDescriptorV1& v) { auto x=c; x.trace_tags.push_back("private_ddl_create_view"); return ConsumeSblrDdlCreateViewDescriptor(x,v); }
}
