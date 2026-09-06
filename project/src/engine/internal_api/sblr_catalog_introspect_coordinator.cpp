#include "sblr_catalog_introspect_coordinator.hpp"
#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {

SblrCatalogIntrospectCoordinationResult
BuildSblrCatalogIntrospectDescriptorV1(
    const SblrCatalogIntrospectAuthorityInputV1& input) {
  SblrCatalogIntrospectCoordinationResult result;
  result.descriptor.object_kind = input.object_kind;
  result.descriptor.profile = input.profile;
  result.descriptor.flags = input.flags;
  result.descriptor.object_uuid = input.object_uuid;
  result.descriptor.catalog_epoch = input.catalog_epoch;
  result.descriptor.security_epoch = input.security_epoch;
  result.descriptor.canonical_path_utf8 = input.canonical_path_utf8;
  result.descriptor.availability =
      input.executor_availability_generation;
  result.canonical_descriptor_bytes =
      scratchbird::engine::sblr::EncodeSblrCatalogIntrospectDescriptorV1(
          result.descriptor, false);
  std::string detail;
  scratchbird::engine::sblr::SblrCatalogIntrospectDescriptorV1 decoded;
  if (result.canonical_descriptor_bytes.empty() ||
      !scratchbird::engine::sblr::DecodeSblrCatalogIntrospectDescriptorV1(
          result.canonical_descriptor_bytes.data(),
          result.canonical_descriptor_bytes.size(), &decoded, &detail,
          false)) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SBLR.OPERAND.INVALID",
        "sblr.catalog_introspect.descriptor_authority_invalid", detail);
    return result;
  }
  result.descriptor = std::move(decoded);
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {});
  return result;
}

}  // namespace scratchbird::engine::internal_api
