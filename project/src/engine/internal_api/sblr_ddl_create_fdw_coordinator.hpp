#pragma once
#include "sblr_ddl_create_package_coordinator.hpp"
#include "../sblr/sblr_ddl_create_fdw_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrDdlCreateFdwCoordinationResult = SblrDdlCreatePackageCoordinationResult;
inline SblrDdlCreateFdwCoordinationResult CompileSblrDdlCreateFdwDescriptor(const EngineRequestContext& c,const std::string& r,std::uint64_t o,std::uint32_t s,std::uint64_t a){auto x=c;for(auto& t:x.trace_tags)if(t=="private_ddl_create_fdw_binder")t="private_ddl_create_package_binder";return CompileSblrDdlCreatePackageDescriptor(x,r,o,s,a);}
inline SblrDdlCreateFdwCoordinationResult ConsumeSblrDdlCreateFdwDescriptor(const EngineRequestContext& c,const scratchbird::engine::sblr::SblrDdlCreateFdwDescriptorV1& v){auto x=c;for(auto& t:x.trace_tags)if(t=="private_ddl_create_fdw")t="private_ddl_create_package";return ConsumeSblrDdlCreatePackageDescriptor(x,v);}
}
