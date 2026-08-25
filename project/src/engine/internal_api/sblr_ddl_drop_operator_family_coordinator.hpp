#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_operator_family_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlDropOperatorFamilyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlDropOperatorFamilyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlDropOperatorFamilyCoordinationResult CompileSblrDdlDropOperatorFamilyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlDropOperatorFamilyCoordinationResult ConsumeSblrDdlDropOperatorFamilyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlDropOperatorFamilyDescriptorV1&);}
