#include "sblr_ddl_drop_package_coordinator.hpp"
namespace scratchbird::engine::internal_api {
SblrDdlDropPackageCoordinationResult CompileSblrDdlDropPackageDescriptor(const EngineRequestContext&c,const std::string&r,std::uint64_t o,std::uint32_t p,std::uint64_t a){auto copy=c;copy.trace_tags.push_back("private_ddl_create_package_binder");return CompileSblrDdlCreatePackageDescriptor(copy,r,o,p,a);}
SblrDdlDropPackageCoordinationResult ConsumeSblrDdlDropPackageDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrDdlDropPackageDescriptorV1&v){return ConsumeSblrDdlCreatePackageDescriptor(c,v);}
}
