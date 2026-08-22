#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_dml_counter_add_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDmlCounterAddCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDmlCounterAddCoordinationResult CompileSblrDmlCounterAddDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint64_t,std::uint64_t);
SblrDmlCounterAddCoordinationResult ConsumeSblrDmlCounterAddDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1&);
}
