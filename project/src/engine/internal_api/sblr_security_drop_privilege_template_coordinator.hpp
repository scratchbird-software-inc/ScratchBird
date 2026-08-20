#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_security_drop_privilege_template_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrSecurityDropPrivilegeTemplateCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecurityDropPrivilegeTemplateDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrSecurityDropPrivilegeTemplateCoordinationResult CompileSblrSecurityDropPrivilegeTemplateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrSecurityDropPrivilegeTemplateCoordinationResult ConsumeSblrSecurityDropPrivilegeTemplateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecurityDropPrivilegeTemplateDescriptorV1&);
}
