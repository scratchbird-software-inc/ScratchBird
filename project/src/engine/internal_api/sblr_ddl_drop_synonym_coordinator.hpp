#pragma once
#include "sblr_ddl_drop_package_coordinator.hpp"
#include "engine/sblr/sblr_ddl_drop_synonym_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrDdlDropSynonymCoordinationResult = SblrDdlDropPackageCoordinationResult;
inline SblrDdlDropSynonymCoordinationResult CompileSblrDdlDropSynonymDescriptor(const EngineRequestContext& c,const std::string& r,std::uint64_t o,std::uint32_t s,std::uint64_t a){auto x=c;for(auto& t:x.trace_tags)if(t=="private_ddl_drop_synonym_binder")t="private_ddl_drop_package_binder";return CompileSblrDdlDropPackageDescriptor(x,r,o,s,a);}
inline SblrDdlDropSynonymCoordinationResult ConsumeSblrDdlDropSynonymDescriptor(const EngineRequestContext& c,const scratchbird::engine::sblr::SblrDdlDropSynonymDescriptorV1& v){auto x=c;for(auto& t:x.trace_tags)if(t=="private_ddl_drop_synonym")t="private_ddl_create_package";return ConsumeSblrDdlDropPackageDescriptor(x,v);}
}
