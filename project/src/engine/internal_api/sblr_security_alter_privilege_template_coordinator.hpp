#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_security_alter_privilege_template_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrSecurityAlterPrivilegeTemplateCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecurityAlterPrivilegeTemplateDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrSecurityAlterPrivilegeTemplateCoordinationResult CompileSblrSecurityAlterPrivilegeTemplateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrSecurityAlterPrivilegeTemplateCoordinationResult ConsumeSblrSecurityAlterPrivilegeTemplateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecurityAlterPrivilegeTemplateDescriptorV1&);
}
