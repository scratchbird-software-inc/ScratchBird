#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_continuous_view_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropContinuousViewCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropContinuousViewDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropContinuousViewCoordinationResult CompileSblrDdlDropContinuousViewDescriptor(const EngineRequestContext&,const EngineUuid&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropContinuousViewCoordinationResult ConsumeSblrDdlDropContinuousViewDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropContinuousViewDescriptorV1&); }
