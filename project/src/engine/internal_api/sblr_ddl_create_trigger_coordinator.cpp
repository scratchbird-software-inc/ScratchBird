#include "sblr_ddl_create_trigger_coordinator.hpp"

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

SblrDdlCreateTriggerCoordinationResult Refuse(std::string code,
                                              std::string key,
                                              std::string detail = {}) {
  SblrDdlCreateTriggerCoordinationResult result;
  result.diagnostic = MakeEngineApiDiagnostic(
      std::move(code), std::move(key), std::move(detail));
  return result;
}

}  // namespace

SblrDdlCreateTriggerCoordinationResult CompileSblrDdlCreateTriggerDescriptor(
    const SblrDdlCreateTriggerAuthorityInputV1& input) {
  const auto bytes =
      scratchbird::engine::sblr::EncodeSblrDdlCreateTriggerDescriptorV1(
          input.descriptor, false);
  if (bytes.size() !=
      scratchbird::engine::sblr::kSblrDdlCreateTriggerDescriptorV1Bytes) {
    return Refuse("SBLR.OPERAND_INVALID",
                  "sblr.ddl_create_trigger.authority_projection_invalid");
  }
  SblrDdlCreateTriggerCoordinationResult result;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrDdlCreateTriggerDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, &detail, false)) {
    return Refuse("SBLR.OPERAND_INVALID",
                  "sblr.ddl_create_trigger.descriptor_invalid", detail);
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {});
  return result;
}

SblrDdlCreateTriggerCoordinationResult CompileSblrDdlCreateTriggerDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t) {
  return Refuse("MGA.AUTHORITY_MISMATCH",
                "sblr.ddl_create_trigger.receipt_private_bind_required");
}

SblrDdlCreateTriggerCoordinationResult ConsumeSblrDdlCreateTriggerDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1&) {
  return Refuse("MGA.AUTHORITY_MISMATCH",
                "sblr.ddl_create_trigger.receipt_private_execution_required");
}

}  // namespace scratchbird::engine::internal_api
