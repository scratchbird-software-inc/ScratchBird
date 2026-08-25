#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_drop_table_runtime.hpp"

namespace scratchbird::engine::internal_api {
struct SblrDdlDropTableCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDdlDropTableDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

SblrDdlDropTableCoordinationResult CompileSblrDdlDropTableDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrDdlDropTableCoordinationResult ConsumeSblrDdlDropTableDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlDropTableDescriptorV1&);
}
