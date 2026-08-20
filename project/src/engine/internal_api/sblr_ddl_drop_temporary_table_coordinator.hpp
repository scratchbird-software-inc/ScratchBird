#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_temporary_table_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropTemporaryTableCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlDropTemporaryTableDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlDropTemporaryTableCoordinationResult CompileSblrDdlDropTemporaryTableDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropTemporaryTableCoordinationResult ConsumeSblrDdlDropTemporaryTableDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropTemporaryTableDescriptorV1&); }
