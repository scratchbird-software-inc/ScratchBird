#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_set_index_optimizer_eligibility_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrDdlSetIndexOptimizerEligibilityCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrDdlSetIndexOptimizerEligibilityDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrDdlSetIndexOptimizerEligibilityCoordinationResult CompileSblrDdlSetIndexOptimizerEligibilityDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t); SblrDdlSetIndexOptimizerEligibilityCoordinationResult ConsumeSblrDdlSetIndexOptimizerEligibilityDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDdlSetIndexOptimizerEligibilityDescriptorV1&); }
