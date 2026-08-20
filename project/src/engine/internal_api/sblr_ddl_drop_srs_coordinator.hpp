#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_srs_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropSrsCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropSrsDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropSrsCoordinationResult CompileSblrDdlDropSrsDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropSrsCoordinationResult ConsumeSblrDdlDropSrsDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropSrsDescriptorV1&); }
