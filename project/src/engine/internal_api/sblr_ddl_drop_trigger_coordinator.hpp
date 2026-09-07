#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_trigger_runtime.hpp"

namespace scratchbird::engine::internal_api {

struct SblrDdlDropTriggerAuthorityInputV1 {
  scratchbird::engine::sblr::SblrDdlDropTriggerDescriptorV1 descriptor;
};

struct SblrDdlDropTriggerCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDdlDropTriggerDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

SblrDdlDropTriggerCoordinationResult CompileSblrDdlDropTriggerDescriptor(
    const SblrDdlDropTriggerAuthorityInputV1&);
SblrDdlDropTriggerCoordinationResult CompileSblrDdlDropTriggerDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrDdlDropTriggerCoordinationResult ConsumeSblrDdlDropTriggerDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlDropTriggerDescriptorV1&);

}  // namespace scratchbird::engine::internal_api
