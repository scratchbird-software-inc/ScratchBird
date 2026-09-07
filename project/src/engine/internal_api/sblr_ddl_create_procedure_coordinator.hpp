#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_ddl_create_procedure_runtime.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

using SblrDdlCreateProcedureAuthorityInputV1 =
    scratchbird::engine::sblr::SblrDdlCreateProcedureAuthorityV1;

struct SblrDdlCreateProcedureCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

SblrDdlCreateProcedureCoordinationResult CompileSblrDdlCreateProcedureDescriptor(
    const SblrDdlCreateProcedureAuthorityInputV1& input);
bool DecodeSblrDdlCreateProcedureAuthorityInputV1(
    const scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1& descriptor,
    SblrDdlCreateProcedureAuthorityInputV1* out,
    std::string* detail = nullptr);

// Legacy 64-byte coordination is retained only for existing component
// coverage.  The executable SBsql route uses the syntax-only v2 bind and the
// receipt-private authority overload above.
SblrDdlCreateProcedureCoordinationResult CompileSblrDdlCreateProcedureDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrDdlCreateProcedureCoordinationResult ConsumeSblrDdlCreateProcedureDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1&);

}  // namespace scratchbird::engine::internal_api
