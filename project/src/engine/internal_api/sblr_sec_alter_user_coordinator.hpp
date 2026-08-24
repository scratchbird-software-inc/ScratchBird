#pragma once
#include "engine/sblr/sblr_sec_alter_user_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
namespace scratchbird::engine::internal_api {
struct SblrSecAlterUserCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrSecAlterUserDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic{}; };
SblrSecAlterUserCoordinationResult CompileSblrSecAlterUserDescriptor(const EngineRequestContext&, const std::string&, std::uint64_t, std::uint64_t);
SblrSecAlterUserCoordinationResult ConsumeSblrSecAlterUserDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrSecAlterUserDescriptorV1&);
}
