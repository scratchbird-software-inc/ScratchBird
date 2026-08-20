#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_schema_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateSchemaCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateSchemaCoordinationResult CompileSblrDdlCreateSchemaDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateSchemaCoordinationResult ConsumeSblrDdlCreateSchemaDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1&); }
