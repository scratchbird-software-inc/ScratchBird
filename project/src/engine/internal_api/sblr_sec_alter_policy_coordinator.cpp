#include "sblr_sec_alter_policy_coordinator.hpp"

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

}  // namespace

SblrSecAlterPolicyCoordinationResult CompileSblrSecAlterPolicyDescriptor(
    const SblrSecAlterPolicyAuthorityInputV1& authority) {
  SblrSecAlterPolicyCoordinationResult result;
  const auto encoded =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(
          authority.descriptor, false);
  std::string detail;
  if (encoded.empty() ||
      !scratchbird::engine::sblr::DecodeSblrSecAlterPolicyDescriptorV1(
          encoded.data(), encoded.size(), &result.descriptor, &detail,
          false)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.sec_alter_policy.descriptor_invalid", std::move(detail));
    return result;
  }
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrSecAlterPolicyCoordinationResult ValidateSblrSecAlterPolicyDescriptor(
    const SblrSecAlterPolicyAuthorityInputV1& authority,
    const scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1& operand,
    bool cancellation_requested) {
  SblrSecAlterPolicyCoordinationResult result;
  if (cancellation_requested) {
    result.diagnostic =
        Diagnostic("PROCESS.CANCELLED", "sblr.sec_alter_policy.cancelled");
    return result;
  }
  const auto expected =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(
          authority.descriptor, true);
  const auto actual =
      scratchbird::engine::sblr::EncodeSblrSecAlterPolicyDescriptorV1(
          operand, true);
  if (expected.empty() || actual.empty() || actual != expected) {
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.sec_alter_policy.descriptor_authority_mismatch");
    return result;
  }
  result.ok = true;
  result.descriptor = operand;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api
