#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_project_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrProjectCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrProjectDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrProjectCoordinationResult CompileSblrProjectDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrProjectCoordinationResult ConsumeSblrProjectDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrProjectDescriptorV1&); }
