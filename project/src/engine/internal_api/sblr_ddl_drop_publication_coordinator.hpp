#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_publication_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropPublicationCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropPublicationDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropPublicationCoordinationResult CompileSblrDdlDropPublicationDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t);SblrDdlDropPublicationCoordinationResult ConsumeSblrDdlDropPublicationDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropPublicationDescriptorV1&);}
