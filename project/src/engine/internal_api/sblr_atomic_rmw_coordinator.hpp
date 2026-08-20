#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_atomic_read_modify_write_runtime.hpp"

namespace scratchbird::engine::internal_api {
struct SblrAtomicRmwCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrAtomicRmwDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};
SblrAtomicRmwCoordinationResult CompileSblrAtomicRmwDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrAtomicRmwCoordinationResult ConsumeSblrAtomicRmwDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrAtomicRmwDescriptorV1&);
}  // namespace scratchbird::engine::internal_api
