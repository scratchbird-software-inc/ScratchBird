#pragma once
#include "sblr_ddl_create_package_coordinator.hpp"
#include "engine/sblr/sblr_ddl_alter_package_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlAlterPackageCoordinationResult=SblrDdlCreatePackageCoordinationResult; SblrDdlAlterPackageCoordinationResult CompileSblrDdlAlterPackageDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterPackageCoordinationResult ConsumeSblrDdlAlterPackageDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterPackageDescriptorV1&); }
