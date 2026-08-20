#include "sblr_procedure_invoke_coordinator.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex mutex;
std::map<std::string, scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1>
    live, consumed;
std::string Key(const scratchbird::engine::sblr::ProcedureInvokeSha& value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}
bool HasTag(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}
EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key), {});
}
}  // namespace

SblrProcedureInvokeCoordinationResult CompileSblrProcedureInvokeDescriptor(
    const EngineRequestContext& context, const std::string& receipt,
    std::uint64_t occurrence, std::uint32_t invocation_occurrence,
    std::uint64_t availability) {
  std::lock_guard lock(mutex);
  SblrProcedureInvokeCoordinationResult result;
  if (!HasTag(context, "private_procedure_invoke_binder") ||
      !context.statement_metadata_snapshot_engine_owned ||
      receipt != context.statement_uuid.canonical || occurrence == 0 ||
      invocation_occurrence == 0 || availability == 0) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
                                   "sblr.procedure_invoke.coordination_invalid");
    return result;
  }
  result.descriptor.body[0] = 1;
  result.descriptor.body[1] = static_cast<std::uint8_t>(occurrence);
  result.descriptor.body[2] = static_cast<std::uint8_t>(invocation_occurrence);
  result.descriptor.availability = availability;
  const auto bytes = scratchbird::engine::sblr::EncodeSblrProcedureInvokeDescriptorV1(
      result.descriptor, false);
  if (!scratchbird::engine::sblr::DecodeSblrProcedureInvokeDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, nullptr, false)) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
                                   "sblr.procedure_invoke.descriptor_invalid");
    return result;
  }
  live[Key(result.descriptor.evidence)] = result.descriptor;
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrProcedureInvokeCoordinationResult ConsumeSblrProcedureInvokeDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1& value) {
  std::lock_guard lock(mutex);
  SblrProcedureInvokeCoordinationResult result;
  const auto key = Key(value.evidence);
  const auto found = live.find(key);
  if (!HasTag(context, "private_procedure_invoke")) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                   "sblr.procedure_invoke.hidden");
    return result;
  }
  if (found == live.end()) {
    result.diagnostic = consumed.count(key)
        ? Diagnostic("MGA.TRANSACTION.STALE", "sblr.procedure_invoke.stale")
        : Diagnostic("SECURITY.ACCESS_DENIED", "sblr.procedure_invoke.hidden");
    return result;
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic("PROCESS.CANCELLED",
                                   "sblr.procedure_invoke.cancelled");
    return result;
  }
  consumed[key] = value;
  live.erase(found);
  result.ok = true;
  result.descriptor = value;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}
}  // namespace scratchbird::engine::internal_api
