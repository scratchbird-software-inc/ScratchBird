#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_table_as_query_runtime.hpp"
namespace scratchbird::engine::internal_api {
struct SblrDdlCreateTableAsQueryCoordinationResult { bool ok=false; scratchbird::engine::sblr::SblrCreateTableAsQueryDescriptorV1 descriptor{}; EngineApiDiagnostic diagnostic; };
SblrDdlCreateTableAsQueryCoordinationResult CompileSblrDdlCreateTableAsQueryDescriptor(const EngineRequestContext&, const std::string&, std::uint16_t, std::uint64_t);
SblrDdlCreateTableAsQueryCoordinationResult ConsumeSblrDdlCreateTableAsQueryDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrCreateTableAsQueryDescriptorV1&);
SblrDdlCreateTableAsQueryCoordinationResult RegisterSblrDdlCreateTableAsQueryDescriptor(const EngineRequestContext&, const scratchbird::engine::sblr::SblrCreateTableAsQueryDescriptorV1&);
}
