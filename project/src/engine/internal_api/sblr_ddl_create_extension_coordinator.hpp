#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_extension_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateExtensionCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlCreateExtensionDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlCreateExtensionCoordinationResult CompileSblrDdlCreateExtensionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrDdlCreateExtensionCoordinationResult ConsumeSblrDdlCreateExtensionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateExtensionDescriptorV1&); }
