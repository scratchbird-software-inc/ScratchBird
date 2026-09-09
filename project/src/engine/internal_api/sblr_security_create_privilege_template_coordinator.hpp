#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_security_create_privilege_template_runtime.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

struct SblrSecurityCreatePrivilegeTemplateCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1
      descriptor{};
  EngineApiDiagnostic diagnostic;
};

// Neutral input for the receipt-private binder.  The caller must already have
// resolved every UUID, generation, authorization, policy, and recovery field.
// This boundary validates and freezes the exact PTDD carrier; it does not
// derive engine authority from parser text or a server session record.
struct SblrSecurityCreatePrivilegeTemplateAuthorityInputV1 {
  scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1
      descriptor{};
};

SblrSecurityCreatePrivilegeTemplateCoordinationResult
CompileSblrSecurityCreatePrivilegeTemplateDescriptor(
    const SblrSecurityCreatePrivilegeTemplateAuthorityInputV1&);

// Transitional entry points retained while the server and public ABI migrate
// to the receipt-private authority producer.  They fail closed rather than
// recreating the removed process-global descriptor map.
SblrSecurityCreatePrivilegeTemplateCoordinationResult
CompileSblrSecurityCreatePrivilegeTemplateDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrSecurityCreatePrivilegeTemplateCoordinationResult
ConsumeSblrSecurityCreatePrivilegeTemplateDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::
        SblrSecurityCreatePrivilegeTemplateDescriptorV1&);

}  // namespace scratchbird::engine::internal_api
