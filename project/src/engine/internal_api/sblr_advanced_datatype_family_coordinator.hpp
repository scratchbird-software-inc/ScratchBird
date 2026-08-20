#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_advanced_datatype_family_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAdvancedDatatypeFamilyCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAdvancedDatatypeFamilyDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAdvancedDatatypeFamilyCoordinationResult CompileSblrAdvancedDatatypeFamilyDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrAdvancedDatatypeFamilyCoordinationResult ConsumeSblrAdvancedDatatypeFamilyDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAdvancedDatatypeFamilyDescriptorV1&);}
