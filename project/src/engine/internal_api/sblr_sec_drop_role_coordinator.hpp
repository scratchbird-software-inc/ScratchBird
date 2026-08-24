#pragma once
#include "engine/sblr/sblr_sec_drop_role_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecDropRoleCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecDropRoleDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSecDropRoleCoordinationResult CompileSblrSecDropRoleDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t); SblrSecDropRoleCoordinationResult ConsumeSblrSecDropRoleDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecDropRoleDescriptorV1&); }
