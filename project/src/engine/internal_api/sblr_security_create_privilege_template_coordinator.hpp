#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_security_create_privilege_template_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrSecurityCreatePrivilegeTemplateCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrSecurityCreatePrivilegeTemplateCoordinationResult CompileSblrSecurityCreatePrivilegeTemplateDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrSecurityCreatePrivilegeTemplateCoordinationResult ConsumeSblrSecurityCreatePrivilegeTemplateDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1&);
}
