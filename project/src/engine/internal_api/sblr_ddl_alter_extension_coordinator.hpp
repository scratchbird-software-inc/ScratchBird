#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_extension_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterExtensionCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlAlterExtensionDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlAlterExtensionCoordinationResult CompileSblrDdlAlterExtensionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrDdlAlterExtensionCoordinationResult ConsumeSblrDdlAlterExtensionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterExtensionDescriptorV1&); }
