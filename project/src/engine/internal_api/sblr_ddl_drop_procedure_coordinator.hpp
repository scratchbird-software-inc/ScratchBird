#pragma once
#include "sblr_ddl_create_procedure_coordinator.hpp"
#include "engine/sblr/sblr_ddl_drop_procedure_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlDropProcedureCoordinationResult=SblrDdlCreateProcedureCoordinationResult; SblrDdlDropProcedureCoordinationResult CompileSblrDdlDropProcedureDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropProcedureCoordinationResult ConsumeSblrDdlDropProcedureDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropProcedureDescriptorV1&); }
