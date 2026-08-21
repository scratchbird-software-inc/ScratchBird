#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_macro_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropMacroCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropMacroDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropMacroCoordinationResult CompileSblrDdlDropMacroDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropMacroCoordinationResult ConsumeSblrDdlDropMacroDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropMacroDescriptorV1&); }
