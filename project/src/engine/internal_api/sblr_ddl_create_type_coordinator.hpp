#pragma once
#include "api_types.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
#include "engine/sblr/sblr_ddl_create_type_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrDdlCreateTypeCoordinationResult = SblrDdlCreateViewCoordinationResult;
SblrDdlCreateTypeCoordinationResult CompileSblrDdlCreateTypeDescriptor(const EngineRequestContext&, const std::string&, std::uint64_t, std::uint32_t, std::uint64_t);
SblrDdlCreateTypeCoordinationResult ConsumeSblrDdlCreateTypeDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrDdlCreateTypeDescriptorV1&);
}
