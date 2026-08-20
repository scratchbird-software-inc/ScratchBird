#include "sblr_atomic_rmw_coordinator.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex coordinator_mutex;
std::map<std::string, scratchbird::engine::sblr::SblrAtomicRmwDescriptorV1> live;
std::map<std::string, scratchbird::engine::sblr::SblrAtomicRmwDescriptorV1> used;
std::string Key(const scratchbird::engine::sblr::AtomicRmwSha& evidence) {
  return {reinterpret_cast<const char*>(evidence.data()), evidence.size()};
}
bool Tagged(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}
EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key), {});
}
}  // namespace

SblrAtomicRmwCoordinationResult CompileSblrAtomicRmwDescriptor(
    const EngineRequestContext& context, const std::string& receipt,
    std::uint64_t occurrence, std::uint32_t rmw_occurrence,
    std::uint64_t availability_generation) {
  std::lock_guard lock(coordinator_mutex);
  SblrAtomicRmwCoordinationResult result;
  if (!Tagged(context, "private_atomic_rmw_compiler") ||
      !context.statement_metadata_snapshot_engine_owned ||
      receipt != context.statement_uuid.canonical || occurrence == 0 ||
      rmw_occurrence == 0 || availability_generation == 0) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID", "sblr.atomic_rmw.coordination_invalid");
    return result;
  }
  result.descriptor.canonical_body[0] = 1;
  result.descriptor.canonical_body[1] = static_cast<std::uint8_t>(occurrence);
  result.descriptor.canonical_body[2] = static_cast<std::uint8_t>(rmw_occurrence);
  result.descriptor.availability_generation = availability_generation;
  auto encoded = scratchbird::engine::sblr::EncodeSblrAtomicRmwDescriptorV1(
      result.descriptor, false);
  if (!scratchbird::engine::sblr::DecodeSblrAtomicRmwDescriptorV1(
          encoded.data(), encoded.size(), &result.descriptor, nullptr, false)) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID", "sblr.atomic_rmw.descriptor_invalid");
    return result;
  }
  live[Key(result.descriptor.evidence)] = result.descriptor;
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrAtomicRmwCoordinationResult ConsumeSblrAtomicRmwDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrAtomicRmwDescriptorV1& descriptor) {
  std::lock_guard lock(coordinator_mutex);
  SblrAtomicRmwCoordinationResult result;
  const auto key = Key(descriptor.evidence);
  auto found = live.find(key);
  if (!Tagged(context, "private_atomic_rmw")) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.atomic_rmw.hidden");
    return result;
  }
  if (found == live.end()) {
    result.diagnostic = used.count(key)
        ? Diagnostic("MGA.TRANSACTION.STALE", "sblr.atomic_rmw.stale")
        : Diagnostic("SECURITY.ACCESS_DENIED", "sblr.atomic_rmw.hidden");
    return result;
  }
  if (context.query_cancellation_requested && context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic("PROCESS.CANCELLED", "sblr.atomic_rmw.cancelled");
    return result;
  }
  used[key] = descriptor;
  live.erase(found);
  result.ok = true;
  result.descriptor = descriptor;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}
}  // namespace scratchbird::engine::internal_api
