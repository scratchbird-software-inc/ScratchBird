#pragma once
#include "engine/sblr/sblr_sec_create_role_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecCreateRoleCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecCreateRoleDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSecCreateRoleCoordinationResult CompileSblrSecCreateRoleDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t); SblrSecCreateRoleCoordinationResult ConsumeSblrSecCreateRoleDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecCreateRoleDescriptorV1&); }
