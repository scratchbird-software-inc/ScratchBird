#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_versioned_revert_runtime.hpp"

namespace scratchbird::engine::internal_api {
struct SblrVersionedRevertCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrVersionedRevertDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrVersionedRevertCoordinationResult CompileSblrVersionedRevertDescriptor(const EngineRequestContext&, const std::string&, std::uint64_t);
SblrVersionedRevertCoordinationResult ConsumeSblrVersionedRevertDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrVersionedRevertDescriptorV1&);
}
