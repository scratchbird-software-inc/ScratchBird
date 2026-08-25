#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_named_collection_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlCreateNamedCollectionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateNamedCollectionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateNamedCollectionCoordinationResult CompileSblrDdlCreateNamedCollectionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlCreateNamedCollectionCoordinationResult ConsumeSblrDdlCreateNamedCollectionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateNamedCollectionDescriptorV1&);}
