#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_versioned_reset_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrVersionedResetCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrVersionedResetDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrVersionedResetCoordinationResult CompileSblrVersionedResetDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrVersionedResetCoordinationResult ConsumeSblrVersionedResetDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrVersionedResetDescriptorV1&);}
