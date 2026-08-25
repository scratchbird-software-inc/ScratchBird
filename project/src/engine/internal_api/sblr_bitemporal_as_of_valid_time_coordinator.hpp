#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bitemporal_as_of_valid_time_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrBitemporalAsOfValidTimeCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBitemporalAsOfValidTimeDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrBitemporalAsOfValidTimeCoordinationResult CompileSblrBitemporalAsOfValidTimeDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrBitemporalAsOfValidTimeCoordinationResult ConsumeSblrBitemporalAsOfValidTimeDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBitemporalAsOfValidTimeDescriptorV1&);}
