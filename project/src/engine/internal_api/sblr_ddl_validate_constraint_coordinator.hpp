#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_validate_constraint_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlValidateConstraintCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlValidateConstraintDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlValidateConstraintCoordinationResult CompileSblrDdlValidateConstraintDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlValidateConstraintCoordinationResult ConsumeSblrDdlValidateConstraintDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlValidateConstraintDescriptorV1&); }
