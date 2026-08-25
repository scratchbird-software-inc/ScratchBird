#include "sblr_ddl_drop_table_coordinator.hpp"
#include "api_diagnostics.hpp"

#include <algorithm>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {
using D = scratchbird::engine::sblr::SblrDdlDropTableDescriptorV1;
std::mutex g_mutex;
std::map<std::string, D> g_pending;
std::map<std::string, D> g_used;

std::string Key(const D& d) {
  return std::string(reinterpret_cast<const char*>(d.evidence.data()), d.evidence.size());
}
EngineApiDiagnostic Diagnostic(const char* code, const char* key) {
  return MakeEngineApiDiagnostic(code, key, {});
}
bool HasTag(const EngineRequestContext& c, const char* tag) {
  return c.security_context_present &&
         std::find(c.trace_tags.begin(), c.trace_tags.end(), tag) != c.trace_tags.end();
}
}

SblrDdlDropTableCoordinationResult CompileSblrDdlDropTableDescriptor(
    const EngineRequestContext& c, const std::string& statement,
    std::uint64_t occurrence, std::uint32_t table_occurrence,
    std::uint64_t availability) {
  SblrDdlDropTableCoordinationResult out;
  std::lock_guard lock(g_mutex);
  if (!HasTag(c, "private_ddl_drop_table_binder") ||
      !c.statement_metadata_snapshot_engine_owned ||
      statement != c.statement_uuid.canonical || !occurrence ||
      !table_occurrence || !availability) {
    out.diagnostic = Diagnostic("SBLR.OPERAND.INVALID",
                                "sblr.ddl_drop_table.coordination_invalid");
    return out;
  }
  out.descriptor.body[0] = 1;
  out.descriptor.body[1] = static_cast<std::uint8_t>(occurrence);
  out.descriptor.body[2] = static_cast<std::uint8_t>(table_occurrence);
  out.descriptor.availability = availability;
  auto bytes = scratchbird::engine::sblr::EncodeSblrDdlDropTableDescriptorV1(
      out.descriptor, false);
  if (bytes.empty() ||
      !scratchbird::engine::sblr::DecodeSblrDdlDropTableDescriptorV1(
          bytes.data(), bytes.size(), &out.descriptor, nullptr, false)) {
    out.diagnostic = Diagnostic("SBLR.OPERAND.INVALID",
                                "sblr.ddl_drop_table.descriptor_invalid");
    return out;
  }
  g_pending[Key(out.descriptor)] = out.descriptor;
  out.ok = true;
  out.diagnostic = Diagnostic("OK", "ok");
  return out;
}

SblrDdlDropTableCoordinationResult ConsumeSblrDdlDropTableDescriptor(
    const EngineRequestContext& c, const D& descriptor) {
  SblrDdlDropTableCoordinationResult out;
  std::lock_guard lock(g_mutex);
  if (!HasTag(c, "private_ddl_drop_table")) {
    out.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                "sblr.ddl_drop_table.hidden");
    return out;
  }
  const auto key = Key(descriptor);
  auto it = g_pending.find(key);
  if (it == g_pending.end()) {
    out.diagnostic = Diagnostic(g_used.count(key) ? "MGA.TRANSACTION.STALE"
                                                  : "SECURITY.ACCESS_DENIED",
                                "sblr.ddl_drop_table.replay");
    return out;
  }
  if (c.query_cancellation_requested && c.query_cancellation_requested()) {
    out.diagnostic = Diagnostic("PROCESS.CANCELLED",
                                "sblr.ddl_drop_table.cancelled");
    return out;
  }
  g_used[key] = descriptor;
  g_pending.erase(it);
  out.ok = true;
  out.descriptor = descriptor;
  out.diagnostic = Diagnostic("OK", "ok");
  return out;
}
}
