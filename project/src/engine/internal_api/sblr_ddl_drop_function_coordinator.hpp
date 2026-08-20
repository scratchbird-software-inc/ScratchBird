#pragma once
#include "sblr_ddl_create_function_coordinator.hpp"
#include "engine/sblr/sblr_ddl_drop_function_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrDdlDropFunctionCoordinationResult=SblrDdlCreateFunctionCoordinationResult; SblrDdlDropFunctionCoordinationResult CompileSblrDdlDropFunctionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropFunctionCoordinationResult ConsumeSblrDdlDropFunctionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropFunctionDescriptorV1&); }
