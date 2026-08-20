#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_view_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateViewCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateViewDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateViewCoordinationResult CompileSblrDdlCreateViewDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateViewCoordinationResult ConsumeSblrDdlCreateViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateViewDescriptorV1&); }
