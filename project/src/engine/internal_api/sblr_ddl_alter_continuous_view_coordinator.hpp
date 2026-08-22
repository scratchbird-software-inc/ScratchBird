#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_continuous_view_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlAlterContinuousViewCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterContinuousViewDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlAlterContinuousViewCoordinationResult CompileSblrDdlAlterContinuousViewDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterContinuousViewCoordinationResult ConsumeSblrDdlAlterContinuousViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterContinuousViewDescriptorV1&); }
