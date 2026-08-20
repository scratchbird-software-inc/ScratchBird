#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_index_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateIndexCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateIndexCoordinationResult CompileSblrDdlCreateIndexDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateIndexCoordinationResult ConsumeSblrDdlCreateIndexDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1&); }
