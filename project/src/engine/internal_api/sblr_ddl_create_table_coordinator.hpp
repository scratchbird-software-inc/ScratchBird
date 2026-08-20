#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_table_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateTableCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateTableDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateTableCoordinationResult CompileSblrDdlCreateTableDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateTableCoordinationResult ConsumeSblrDdlCreateTableDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateTableDescriptorV1&); }
