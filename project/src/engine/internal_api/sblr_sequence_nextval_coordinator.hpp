#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_sequence_nextval_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrSequenceNextvalCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrSequenceNextvalDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrSequenceNextvalCoordinationResult CompileSblrSequenceNextvalDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrSequenceNextvalCoordinationResult ConsumeSblrSequenceNextvalDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrSequenceNextvalDescriptorV1&);}
