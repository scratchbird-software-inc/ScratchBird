#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_named_collection_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropNamedCollectionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropNamedCollectionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropNamedCollectionCoordinationResult CompileSblrDdlDropNamedCollectionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropNamedCollectionCoordinationResult ConsumeSblrDdlDropNamedCollectionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropNamedCollectionDescriptorV1&);}
