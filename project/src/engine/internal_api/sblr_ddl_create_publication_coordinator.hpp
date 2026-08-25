#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_publication_runtime.hpp"

namespace scratchbird::engine::internal_api {
struct SblrDdlCreatePublicationCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDdlCreatePublicationDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

SblrDdlCreatePublicationCoordinationResult CompileSblrDdlCreatePublicationDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrDdlCreatePublicationCoordinationResult ConsumeSblrDdlCreatePublicationDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlCreatePublicationDescriptorV1&);
}
