#include "sblr_security_create_privilege_template_coordinator.hpp"

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

}  // namespace

SblrSecurityCreatePrivilegeTemplateCoordinationResult
CompileSblrSecurityCreatePrivilegeTemplateDescriptor(
    const SblrSecurityCreatePrivilegeTemplateAuthorityInputV1& input) {
  SblrSecurityCreatePrivilegeTemplateCoordinationResult result;
  const auto bytes = scratchbird::engine::sblr::
      EncodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
          input.descriptor, false);
  std::string detail;
  if (bytes.empty() ||
      !scratchbird::engine::sblr::
          DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(
              bytes.data(), bytes.size(), &result.descriptor, &detail,
              false)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND.INVALID",
        "sblr.security_create_privilege_template.descriptor_invalid",
        std::move(detail));
    return result;
  }
  result.ok = true;
  return result;
}

SblrSecurityCreatePrivilegeTemplateCoordinationResult
CompileSblrSecurityCreatePrivilegeTemplateDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t) {
  SblrSecurityCreatePrivilegeTemplateCoordinationResult result;
  result.diagnostic = Diagnostic(
      "SBLR.OPERAND.INVALID",
      "sblr.security_create_privilege_template.receipt_private_bind_required");
  return result;
}

SblrSecurityCreatePrivilegeTemplateCoordinationResult
ConsumeSblrSecurityCreatePrivilegeTemplateDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::
        SblrSecurityCreatePrivilegeTemplateDescriptorV1&) {
  SblrSecurityCreatePrivilegeTemplateCoordinationResult result;
  result.diagnostic = Diagnostic(
      "MGA.AUTHORITY_MISMATCH",
      "sblr.security_create_privilege_template.receipt_private_authority_required");
  return result;
}

}  // namespace scratchbird::engine::internal_api
