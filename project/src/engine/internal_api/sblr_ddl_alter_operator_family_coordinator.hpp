#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_alter_operator_family_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrDdlAlterOperatorFamilyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlAlterOperatorFamilyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrDdlAlterOperatorFamilyCoordinationResult CompileSblrDdlAlterOperatorFamilyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrDdlAlterOperatorFamilyCoordinationResult ConsumeSblrDdlAlterOperatorFamilyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlAlterOperatorFamilyDescriptorV1&);}
