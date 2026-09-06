#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"
namespace scratchbird::engine::internal_api {

// Neutral input assembled only after the server-engine bridge has resolved a
// receipt-private catalog authority.  It intentionally carries no parser
// name atoms, session handle, receipt handle, or authorization object.
struct SblrCatalogIntrospectAuthorityInputV1 {
  std::uint16_t object_kind = 0;
  std::uint16_t profile = 0;
  std::uint16_t flags = 0;
  scratchbird::engine::sblr::CatalogUuid object_uuid{};
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::string canonical_path_utf8;
  std::uint64_t executor_availability_generation = 0;
};

struct SblrCatalogIntrospectCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrCatalogIntrospectDescriptorV1 descriptor{};
  std::vector<std::uint8_t> canonical_descriptor_bytes;
  EngineApiDiagnostic diagnostic;
};

SblrCatalogIntrospectCoordinationResult
BuildSblrCatalogIntrospectDescriptorV1(
    const SblrCatalogIntrospectAuthorityInputV1& input);
}
