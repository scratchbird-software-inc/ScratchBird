#pragma once
#include "sblr_ddl_create_function_coordinator.hpp"
#include "engine/sblr/sblr_ddl_alter_function_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlAlterFunctionCoordinationResult=SblrDdlCreateFunctionCoordinationResult; SblrDdlAlterFunctionCoordinationResult CompileSblrDdlAlterFunctionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlAlterFunctionCoordinationResult ConsumeSblrDdlAlterFunctionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterFunctionDescriptorV1&); }
