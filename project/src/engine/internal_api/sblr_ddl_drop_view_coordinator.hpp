#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_view_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropViewCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropViewDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropViewCoordinationResult CompileSblrDdlDropViewDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropViewCoordinationResult ConsumeSblrDdlDropViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropViewDescriptorV1&); }
