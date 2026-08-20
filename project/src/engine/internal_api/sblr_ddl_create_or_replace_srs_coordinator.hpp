#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_or_replace_srs_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateOrReplaceSrsCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateOrReplaceSrsDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateOrReplaceSrsCoordinationResult CompileSblrDdlCreateOrReplaceSrsDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateOrReplaceSrsCoordinationResult ConsumeSblrDdlCreateOrReplaceSrsDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateOrReplaceSrsDescriptorV1&); }
