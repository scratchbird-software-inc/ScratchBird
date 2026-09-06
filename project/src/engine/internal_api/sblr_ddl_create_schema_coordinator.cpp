#include "sblr_ddl_create_schema_coordinator.hpp"

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

}  // namespace

SblrDdlCreateSchemaCoordinationResult CompileSblrDdlCreateSchemaDescriptor(
    const SblrDdlCreateSchemaAuthorityInputV1& authority) {
  SblrDdlCreateSchemaCoordinationResult result;
  const auto encoded =
      scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaDescriptorV1(
          authority.descriptor, false);
  std::string detail;
  if (encoded.empty() ||
      !scratchbird::engine::sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
          encoded.data(), encoded.size(), &result.descriptor, &detail, false)) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
                                   "sblr.ddl_create_schema.descriptor_invalid",
                                   std::move(detail));
    return result;
  }
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrDdlCreateSchemaCoordinationResult ValidateSblrDdlCreateSchemaDescriptor(
    const SblrDdlCreateSchemaAuthorityInputV1& authority,
    const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1& operand,
    bool cancellation_requested) {
  SblrDdlCreateSchemaCoordinationResult result;
  if (cancellation_requested) {
    result.diagnostic = Diagnostic("PROCESS.CANCELLED",
                                   "sblr.ddl_create_schema.cancelled");
    return result;
  }
  const auto expected =
      scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaDescriptorV1(
          authority.descriptor, true);
  const auto actual =
      scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaDescriptorV1(
          operand, true);
  if (expected.empty() || actual.empty() || actual != expected) {
    result.diagnostic = Diagnostic(
        "MGA.AUTHORITY_MISMATCH",
        "sblr.ddl_create_schema.descriptor_authority_mismatch");
    return result;
  }
  result.ok = true;
  result.descriptor = operand;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api
