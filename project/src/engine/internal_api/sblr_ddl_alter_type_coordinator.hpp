#pragma once
#include "api_types.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
#include "engine/sblr/sblr_ddl_alter_type_runtime.hpp"
namespace scratchbird::engine::internal_api {
using SblrDdlAlterTypeCoordinationResult = SblrDdlCreateViewCoordinationResult;
SblrDdlAlterTypeCoordinationResult CompileSblrDdlAlterTypeDescriptor(const EngineRequestContext&, const std::string&, std::uint64_t, std::uint32_t, std::uint64_t);
SblrDdlAlterTypeCoordinationResult ConsumeSblrDdlAlterTypeDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrDdlAlterTypeDescriptorV1&);
}
