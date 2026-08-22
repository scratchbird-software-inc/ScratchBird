#include "sblr_dml_conditional_mutate_coordinator.hpp"

#include "api_diagnostics.hpp"

#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex g_mutex;
std::map<std::string, bool> g_live;
std::map<std::string, bool> g_used;

std::string EvidenceKey(
    const scratchbird::engine::sblr::SblrDmlConditionalMutateDescriptorV1& value) {
  return std::string(reinterpret_cast<const char*>(value.evidence.data()),
                     value.evidence.size());
}
}

SblrDmlConditionalMutateCoordinationResult
CompileSblrDmlConditionalMutateDescriptor(const EngineRequestContext& context,
                                          const std::string& receipt,
                                          std::uint64_t structural_occurrence,
                                          std::uint64_t mutation_occurrence,
                                          std::uint64_t availability_generation) {
  SblrDmlConditionalMutateCoordinationResult result;
  if (!context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      receipt != context.statement_uuid.canonical || !structural_occurrence ||
      !mutation_occurrence || !availability_generation) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SBLR.OPERAND.INVALID", "sblr.dml_conditional_mutate.coordination_invalid",
        {}, false);
    return result;
  }
  result.descriptor.body[0] = 1;
  result.descriptor.body[1] = static_cast<std::uint8_t>(structural_occurrence);
  result.descriptor.body[2] = static_cast<std::uint8_t>(mutation_occurrence);
  result.descriptor.evidence[0] = static_cast<std::uint8_t>(structural_occurrence);
  result.descriptor.evidence[1] = static_cast<std::uint8_t>(mutation_occurrence);
  result.descriptor.availability = availability_generation;
  std::lock_guard lock(g_mutex);
  g_live[EvidenceKey(result.descriptor)] = true;
  result.ok = true;
  return result;
}

SblrDmlConditionalMutateCoordinationResult
ConsumeSblrDmlConditionalMutateDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrDmlConditionalMutateDescriptorV1& descriptor) {
  SblrDmlConditionalMutateCoordinationResult result;
  if (!context.security_context_present) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SECURITY.ACCESS_DENIED", "sblr.dml_conditional_mutate.hidden", {}, false);
    return result;
  }
  const auto key = EvidenceKey(descriptor);
  std::lock_guard lock(g_mutex);
  if (!g_live[key]) {
    result.diagnostic = MakeEngineApiDiagnostic(
        g_used[key] ? "MGA.TRANSACTION.STALE" : "SECURITY.ACCESS_DENIED",
        g_used[key] ? "sblr.dml_conditional_mutate.stale"
                    : "sblr.dml_conditional_mutate.hidden",
        {}, false);
    return result;
  }
  if (context.query_cancellation_requested && context.query_cancellation_requested()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "PROCESS.CANCELLED", "sblr.dml_conditional_mutate.cancelled", {}, false);
    return result;
  }
  g_live.erase(key);
  g_used[key] = true;
  result.ok = true;
  result.descriptor = descriptor;
  return result;
}

}  // namespace scratchbird::engine::internal_api
