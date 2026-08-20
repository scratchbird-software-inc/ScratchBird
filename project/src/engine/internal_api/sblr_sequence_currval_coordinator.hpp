#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_sequence_currval_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrSequenceCurrvalCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSequenceCurrvalDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrSequenceCurrvalCoordinationResult CompileSblrSequenceCurrvalDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrSequenceCurrvalCoordinationResult ConsumeSblrSequenceCurrvalDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSequenceCurrvalDescriptorV1&);}
