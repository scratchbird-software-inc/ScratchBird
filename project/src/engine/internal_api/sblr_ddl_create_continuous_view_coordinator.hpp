#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_continuous_view_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateContinuousViewCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateContinuousViewDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateContinuousViewCoordinationResult CompileSblrDdlCreateContinuousViewDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateContinuousViewCoordinationResult ConsumeSblrDdlCreateContinuousViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateContinuousViewDescriptorV1&); }
