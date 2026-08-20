#pragma once
#include "sblr_ddl_create_procedure_coordinator.hpp"
#include "engine/sblr/sblr_ddl_create_function_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlCreateFunctionCoordinationResult=SblrDdlCreateProcedureCoordinationResult; SblrDdlCreateFunctionCoordinationResult CompileSblrDdlCreateFunctionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlCreateFunctionCoordinationResult ConsumeSblrDdlCreateFunctionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateFunctionDescriptorV1&); }
