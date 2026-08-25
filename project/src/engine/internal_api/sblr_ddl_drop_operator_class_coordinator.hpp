#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_operator_class_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropOperatorClassCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropOperatorClassDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropOperatorClassCoordinationResult CompileSblrDdlDropOperatorClassDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropOperatorClassCoordinationResult ConsumeSblrDdlDropOperatorClassDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropOperatorClassDescriptorV1&);}
