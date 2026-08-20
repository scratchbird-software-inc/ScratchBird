#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_index_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropIndexCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropIndexDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropIndexCoordinationResult CompileSblrDdlDropIndexDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropIndexCoordinationResult ConsumeSblrDdlDropIndexDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropIndexDescriptorV1&); }
