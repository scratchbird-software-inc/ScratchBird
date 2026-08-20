#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_sequence_setval_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrSequenceSetvalCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSequenceSetvalDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrSequenceSetvalCoordinationResult CompileSblrSequenceSetvalDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrSequenceSetvalCoordinationResult ConsumeSblrSequenceSetvalDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSequenceSetvalDescriptorV1&);}
