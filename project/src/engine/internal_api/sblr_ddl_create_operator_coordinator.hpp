#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_operator_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateOperatorCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDdlCreateOperatorDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; }; SblrDdlCreateOperatorCoordinationResult CompileSblrDdlCreateOperatorDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrDdlCreateOperatorCoordinationResult ConsumeSblrDdlCreateOperatorDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateOperatorDescriptorV1&); }
