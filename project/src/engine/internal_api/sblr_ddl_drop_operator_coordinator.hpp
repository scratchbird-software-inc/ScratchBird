#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_operator_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropOperatorCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropOperatorDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropOperatorCoordinationResult CompileSblrDdlDropOperatorDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropOperatorCoordinationResult ConsumeSblrDdlDropOperatorDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropOperatorDescriptorV1&);}
