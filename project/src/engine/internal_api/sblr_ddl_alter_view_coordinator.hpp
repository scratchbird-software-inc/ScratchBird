#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_view_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterViewCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterViewDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlAlterViewCoordinationResult CompileSblrDdlAlterViewDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterViewCoordinationResult ConsumeSblrDdlAlterViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterViewDescriptorV1&); }
