#pragma once
#include "engine/sblr/sblr_sec_drop_user_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecDropUserCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSecDropUserDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic{};}; SblrSecDropUserCoordinationResult CompileSblrSecDropUserDescriptor(const EngineRequestContext&,const std::string&,uint64_t,uint64_t);SblrSecDropUserCoordinationResult ConsumeSblrSecDropUserDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecDropUserDescriptorV1&);}
