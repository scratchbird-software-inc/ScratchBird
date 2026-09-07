#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_trigger_runtime.hpp"

namespace scratchbird::engine::internal_api {

struct SblrDdlCreateTriggerAuthorityInputV1 {
  scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1 descriptor;
};

struct SblrDdlCreateTriggerCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

// Accepts only a complete engine-owned authority projection. It does not
// resolve names, synthesize UUIDs, or infer policy from parser input.
SblrDdlCreateTriggerCoordinationResult CompileSblrDdlCreateTriggerDescriptor(
    const SblrDdlCreateTriggerAuthorityInputV1&);

// Transitional fail-closed compatibility entry points. Public trigger routing
// must use the receipt-private bind/copy API before these are removed.
SblrDdlCreateTriggerCoordinationResult CompileSblrDdlCreateTriggerDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrDdlCreateTriggerCoordinationResult ConsumeSblrDdlCreateTriggerDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1&);

}  // namespace scratchbird::engine::internal_api
