#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_operator_class_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlCreateOperatorClassCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateOperatorClassDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateOperatorClassCoordinationResult CompileSblrDdlCreateOperatorClassDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlCreateOperatorClassCoordinationResult ConsumeSblrDdlCreateOperatorClassDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateOperatorClassDescriptorV1&);}
