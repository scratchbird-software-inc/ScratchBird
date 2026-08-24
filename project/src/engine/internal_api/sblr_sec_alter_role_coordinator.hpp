#pragma once
#include "engine/sblr/sblr_sec_alter_role_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api { struct SblrSecAlterRoleCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecAlterRoleDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; }; SblrSecAlterRoleCoordinationResult CompileSblrSecAlterRoleDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t); SblrSecAlterRoleCoordinationResult ConsumeSblrSecAlterRoleDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecAlterRoleDescriptorV1&); }
