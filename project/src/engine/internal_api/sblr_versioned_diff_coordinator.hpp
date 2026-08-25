#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_versioned_diff_runtime.hpp"
namespace scratchbird::engine::internal_api{struct SblrVersionedDiffCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrVersionedDiffDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrVersionedDiffCoordinationResult CompileSblrVersionedDiffDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t);SblrVersionedDiffCoordinationResult ConsumeSblrVersionedDiffDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrVersionedDiffDescriptorV1&);}
