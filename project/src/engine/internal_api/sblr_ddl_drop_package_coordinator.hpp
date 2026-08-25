#pragma once
#include "sblr_ddl_create_package_coordinator.hpp"
#include "engine/sblr/sblr_ddl_drop_package_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrDdlDropPackageCoordinationResult=SblrDdlCreatePackageCoordinationResult;
SblrDdlDropPackageCoordinationResult CompileSblrDdlDropPackageDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrDdlDropPackageCoordinationResult ConsumeSblrDdlDropPackageDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropPackageDescriptorV1&);
}
