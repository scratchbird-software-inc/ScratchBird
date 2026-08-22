#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_function_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlDropFunctionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropFunctionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlDropFunctionCoordinationResult CompileSblrDdlDropFunctionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlDropFunctionCoordinationResult ConsumeSblrDdlDropFunctionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropFunctionDescriptorV1&); }
