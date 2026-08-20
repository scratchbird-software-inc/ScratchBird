#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_procedure_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlCreateProcedureCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlCreateProcedureCoordinationResult CompileSblrDdlCreateProcedureDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateProcedureCoordinationResult ConsumeSblrDdlCreateProcedureDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1&); }
