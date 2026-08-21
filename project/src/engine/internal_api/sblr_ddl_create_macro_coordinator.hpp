#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_macro_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateMacroCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlCreateMacroDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlCreateMacroCoordinationResult CompileSblrDdlCreateMacroDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateMacroCoordinationResult ConsumeSblrDdlCreateMacroDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateMacroDescriptorV1&); }
