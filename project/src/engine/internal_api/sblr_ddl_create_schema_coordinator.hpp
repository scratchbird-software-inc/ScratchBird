#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_schema_runtime.hpp"

#include <cstdint>

namespace scratchbird::engine::internal_api {

// A neutral, engine-owned projection.  It contains no parser atoms, session
// record, receipt handle, or authorization object.  The receipt binder builds
// it only after resolving and authorizing the syntax demand.
struct SblrDdlCreateSchemaAuthorityInputV1 {
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor;
};

struct SblrDdlCreateSchemaCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

SblrDdlCreateSchemaCoordinationResult CompileSblrDdlCreateSchemaDescriptor(
    const SblrDdlCreateSchemaAuthorityInputV1& authority);

SblrDdlCreateSchemaCoordinationResult ValidateSblrDdlCreateSchemaDescriptor(
    const SblrDdlCreateSchemaAuthorityInputV1& authority,
    const scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1& operand,
    bool cancellation_requested);

}  // namespace scratchbird::engine::internal_api
