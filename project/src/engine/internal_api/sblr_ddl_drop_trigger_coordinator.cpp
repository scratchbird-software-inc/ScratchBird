#include "sblr_ddl_drop_trigger_coordinator.hpp"

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

SblrDdlDropTriggerCoordinationResult Refuse(std::string code,
                                            std::string key,
                                            std::string detail = {}) {
  SblrDdlDropTriggerCoordinationResult result;
  result.diagnostic = MakeEngineApiDiagnostic(
      std::move(code), std::move(key), std::move(detail));
  return result;
}

}  // namespace

SblrDdlDropTriggerCoordinationResult CompileSblrDdlDropTriggerDescriptor(
    const SblrDdlDropTriggerAuthorityInputV1& input) {
  const auto bytes =
      scratchbird::engine::sblr::EncodeSblrDdlDropTriggerDescriptorV1(
          input.descriptor, false);
  if (bytes.size() !=
      scratchbird::engine::sblr::kSblrDdlDropTriggerDescriptorV1Bytes) {
    return Refuse("SBLR.OPERAND_INVALID",
                  "sblr.ddl_drop_trigger.authority_projection_invalid");
  }
  SblrDdlDropTriggerCoordinationResult result;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrDdlDropTriggerDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, &detail, false)) {
    return Refuse("SBLR.OPERAND_INVALID",
                  "sblr.ddl_drop_trigger.descriptor_invalid", detail);
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {});
  return result;
}

SblrDdlDropTriggerCoordinationResult CompileSblrDdlDropTriggerDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t) {
  return Refuse("MGA.AUTHORITY_MISMATCH",
                "sblr.ddl_drop_trigger.receipt_private_bind_required");
}

SblrDdlDropTriggerCoordinationResult ConsumeSblrDdlDropTriggerDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlDropTriggerDescriptorV1&) {
  return Refuse("MGA.AUTHORITY_MISMATCH",
                "sblr.ddl_drop_trigger.receipt_private_execution_required");
}

}  // namespace scratchbird::engine::internal_api
