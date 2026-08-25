#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_extension_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropExtensionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropExtensionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropExtensionCoordinationResult CompileSblrDdlDropExtensionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropExtensionCoordinationResult ConsumeSblrDdlDropExtensionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropExtensionDescriptorV1&);}
