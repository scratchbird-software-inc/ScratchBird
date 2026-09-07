#include "sblr_ddl_alter_trigger_coordinator.hpp"

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

SblrDdlAlterTriggerCoordinationResult Refuse(std::string code,
                                             std::string key,
                                             std::string detail = {}) {
  SblrDdlAlterTriggerCoordinationResult result;
  result.diagnostic = MakeEngineApiDiagnostic(
      std::move(code), std::move(key), std::move(detail));
  return result;
}

}  // namespace

SblrDdlAlterTriggerCoordinationResult CompileSblrDdlAlterTriggerDescriptor(
    const SblrDdlAlterTriggerAuthorityInputV1& input) {
  const auto bytes =
      scratchbird::engine::sblr::EncodeSblrDdlAlterTriggerDescriptorV1(
          input.descriptor, false);
  if (bytes.size() !=
      scratchbird::engine::sblr::kSblrDdlAlterTriggerDescriptorV1Bytes) {
    return Refuse("SBLR.OPERAND_INVALID",
                  "sblr.ddl_alter_trigger.authority_projection_invalid");
  }
  SblrDdlAlterTriggerCoordinationResult result;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrDdlAlterTriggerDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, &detail, false)) {
    return Refuse("SBLR.OPERAND_INVALID",
                  "sblr.ddl_alter_trigger.descriptor_invalid", detail);
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {});
  return result;
}

SblrDdlAlterTriggerCoordinationResult CompileSblrDdlAlterTriggerDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t) {
  return Refuse("MGA.AUTHORITY_MISMATCH",
                "sblr.ddl_alter_trigger.receipt_private_bind_required");
}

SblrDdlAlterTriggerCoordinationResult ConsumeSblrDdlAlterTriggerDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlAlterTriggerDescriptorV1&) {
  return Refuse("MGA.AUTHORITY_MISMATCH",
                "sblr.ddl_alter_trigger.receipt_private_execution_required");
}

}  // namespace scratchbird::engine::internal_api
