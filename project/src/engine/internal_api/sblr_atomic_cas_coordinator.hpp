#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_atomic_cas_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrAtomicCasCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrAtomicCasDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrAtomicCasCoordinationResult CompileSblrAtomicCasDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrAtomicCasCoordinationResult ConsumeSblrAtomicCasDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrAtomicCasDescriptorV1&);}
