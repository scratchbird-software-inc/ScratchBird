#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_package_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreatePackageCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlCreatePackageDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlCreatePackageCoordinationResult CompileSblrDdlCreatePackageDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreatePackageCoordinationResult ConsumeSblrDdlCreatePackageDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreatePackageDescriptorV1&); }
