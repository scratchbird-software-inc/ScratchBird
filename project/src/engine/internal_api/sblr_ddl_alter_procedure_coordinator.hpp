#pragma once
#include "sblr_ddl_create_procedure_coordinator.hpp"
#include "engine/sblr/sblr_ddl_alter_procedure_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlAlterProcedureCoordinationResult=SblrDdlCreateProcedureCoordinationResult; SblrDdlAlterProcedureCoordinationResult CompileSblrDdlAlterProcedureDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterProcedureCoordinationResult ConsumeSblrDdlAlterProcedureDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterProcedureDescriptorV1&); }
