#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrCatalogIntrospectCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrCatalogIntrospectDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrCatalogIntrospectCoordinationResult CompileSblrCatalogIntrospectDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);
SblrCatalogIntrospectCoordinationResult ConsumeSblrCatalogIntrospectDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrCatalogIntrospectDescriptorV1&);
}
