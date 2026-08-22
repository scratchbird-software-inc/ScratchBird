#include "sblr_ddl_drop_continuous_view_coordinator.hpp"
#include "api_diagnostics.hpp"
#include <algorithm>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex mutex;
std::map<std::string, scratchbird::engine::sblr::SblrDdlDropContinuousViewDescriptorV1> live, used;
std::string key(const scratchbird::engine::sblr::DdlDropContinuousViewSha& h) {
  return {reinterpret_cast<const char*>(h.data()), h.size()};
}
bool tag(const EngineRequestContext& c, const char* value) {
  return c.security_context_present && std::find(c.trace_tags.begin(), c.trace_tags.end(), value) != c.trace_tags.end();
}
EngineApiDiagnostic diagnostic(std::string code, std::string message) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(message), {});
}
}

SblrDdlDropContinuousViewCoordinationResult CompileSblrDdlDropContinuousViewDescriptor(
    const EngineRequestContext& c, const std::string& receipt, std::uint64_t occurrence,
    std::uint32_t view_occurrence, std::uint64_t availability) {
  std::lock_guard lock(mutex);
  SblrDdlDropContinuousViewCoordinationResult result;
  if (!tag(c, "private_ddl_drop_continuous_view_binder") ||
      !c.statement_metadata_snapshot_engine_owned || receipt != c.statement_uuid.canonical ||
      !occurrence || !view_occurrence || !availability) {
    result.diagnostic = diagnostic("SBLR.OPERAND.INVALID", "sblr.ddl_drop_continuous_view.coordination_invalid");
    return result;
  }
  result.descriptor.body[0] = 1;
  result.descriptor.body[1] = static_cast<std::uint8_t>(occurrence);
  result.descriptor.body[2] = static_cast<std::uint8_t>(view_occurrence);
  result.descriptor.availability = availability;
  result.descriptor.evidence[0] = static_cast<std::uint8_t>(occurrence);
  result.descriptor.evidence[1] = static_cast<std::uint8_t>(view_occurrence);
  const auto bytes = scratchbird::engine::sblr::EncodeSblrDdlDropContinuousViewDescriptorV1(result.descriptor, false);
  if (!scratchbird::engine::sblr::DecodeSblrDdlDropContinuousViewDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, nullptr, false)) {
    result.diagnostic = diagnostic("SBLR.OPERAND.INVALID", "sblr.ddl_drop_continuous_view.descriptor_invalid");
    return result;
  }
  live[key(result.descriptor.evidence)] = result.descriptor;
  result.ok = true;
  result.diagnostic = diagnostic("OK", "ok");
  return result;
}

SblrDdlDropContinuousViewCoordinationResult ConsumeSblrDdlDropContinuousViewDescriptor(
    const EngineRequestContext& c,
    const scratchbird::engine::sblr::SblrDdlDropContinuousViewDescriptorV1& descriptor) {
  std::lock_guard lock(mutex);
  SblrDdlDropContinuousViewCoordinationResult result;
  if (!tag(c, "private_ddl_drop_continuous_view")) {
    result.diagnostic = diagnostic("SECURITY.ACCESS_DENIED", "sblr.ddl_drop_continuous_view.hidden");
    return result;
  }
  const auto identity = key(descriptor.evidence);
  auto found = live.find(identity);
  if (found == live.end()) {
    result.diagnostic = used.contains(identity)
        ? diagnostic("MGA.TRANSACTION.STALE", "sblr.ddl_drop_continuous_view.stale")
        : diagnostic("SECURITY.ACCESS_DENIED", "sblr.ddl_drop_continuous_view.hidden");
    return result;
  }
  if (c.query_cancellation_requested && c.query_cancellation_requested()) {
    result.diagnostic = diagnostic("PROCESS.CANCELLED", "sblr.ddl_drop_continuous_view.cancelled");
    return result;
  }
  used[identity] = descriptor;
  live.erase(found);
  result.ok = true;
  result.descriptor = descriptor;
  result.diagnostic = diagnostic("OK", "ok");
  return result;
}
}
