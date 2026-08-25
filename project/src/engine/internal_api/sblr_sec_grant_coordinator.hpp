#pragma once
#include "engine/sblr/sblr_sec_grant_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api{struct SblrSecGrantCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSecGrantDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};};SblrSecGrantCoordinationResult CompileSblrSecGrantDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSecGrantCoordinationResult ConsumeSblrSecGrantDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecGrantDescriptorV1&);}
