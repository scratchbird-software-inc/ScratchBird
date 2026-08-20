#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_rename_object_vector_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlRenameObjectVectorCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlRenameObjectVectorDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlRenameObjectVectorCoordinationResult CompileSblrDdlRenameObjectVectorDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlRenameObjectVectorCoordinationResult ConsumeSblrDdlRenameObjectVectorDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlRenameObjectVectorDescriptorV1&); }
