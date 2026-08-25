#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_publication_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDdlAlterPublicationCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlAlterPublicationDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDdlAlterPublicationCoordinationResult CompileSblrDdlAlterPublicationDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint8_t,std::uint64_t);
SblrDdlAlterPublicationCoordinationResult ConsumeSblrDdlAlterPublicationDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterPublicationDescriptorV1&);
}
