#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_temporary_table_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateTemporaryTableCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlCreateTemporaryTableDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlCreateTemporaryTableCoordinationResult CompileSblrDdlCreateTemporaryTableDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateTemporaryTableCoordinationResult ConsumeSblrDdlCreateTemporaryTableDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateTemporaryTableDescriptorV1&); }
