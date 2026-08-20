#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_procedure_invoke_runtime.hpp"

namespace scratchbird::engine::internal_api {
struct SblrProcedureInvokeCoordinationResult {
  bool ok{false};
  scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};
SblrProcedureInvokeCoordinationResult CompileSblrProcedureInvokeDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrProcedureInvokeCoordinationResult ConsumeSblrProcedureInvokeDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1&);
}  // namespace scratchbird::engine::internal_api
