#pragma once
#include "api_types.hpp"
#include "sblr_ddl_drop_view_coordinator.hpp"
#include "engine/sblr/sblr_ddl_drop_type_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDdlDropTypeCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDdlDropTypeDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};
SblrDdlDropTypeCoordinationResult CompileSblrDdlDropTypeDescriptor(const EngineRequestContext&, const std::string&, std::uint64_t, std::uint32_t, std::uint64_t);
SblrDdlDropTypeCoordinationResult ConsumeSblrDdlDropTypeDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrDdlDropTypeDescriptorV1&);
}
