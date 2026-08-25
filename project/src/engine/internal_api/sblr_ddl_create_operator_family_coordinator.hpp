#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_operator_family_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlCreateOperatorFamilyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlCreateOperatorFamilyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlCreateOperatorFamilyCoordinationResult CompileSblrDdlCreateOperatorFamilyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlCreateOperatorFamilyCoordinationResult ConsumeSblrDdlCreateOperatorFamilyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlCreateOperatorFamilyDescriptorV1&);}
