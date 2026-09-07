#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_procedure_invoke_runtime.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

using SblrProcedureInvokeAuthorityInputV1 =
    scratchbird::engine::sblr::SblrProcedureInvokeAuthorityV1;

struct SblrProcedureInvokeCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

SblrProcedureInvokeCoordinationResult CompileSblrProcedureInvokeDescriptor(
    const SblrProcedureInvokeAuthorityInputV1& input);
bool DecodeSblrProcedureInvokeAuthorityInputV1(
    const scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1& descriptor,
    SblrProcedureInvokeAuthorityInputV1* out,
    std::string* detail = nullptr);

// Legacy v1 coordination remains component-only.  Executable routes use the
// receipt-private syntax-demand binder and the typed overload above.
SblrProcedureInvokeCoordinationResult CompileSblrProcedureInvokeDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrProcedureInvokeCoordinationResult ConsumeSblrProcedureInvokeDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1&);

}  // namespace scratchbird::engine::internal_api
