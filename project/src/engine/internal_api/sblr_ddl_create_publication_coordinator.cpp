#include "sblr_ddl_create_publication_coordinator.hpp"
#include "api_diagnostics.hpp"
#include <algorithm>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex mutex;
using Descriptor = scratchbird::engine::sblr::SblrDdlCreatePublicationDescriptorV1;
std::map<std::string, Descriptor> live;
std::map<std::string, Descriptor> used;
std::string Key(const Descriptor& d) {
  return {reinterpret_cast<const char*>(d.body.data()), d.body.size()};
}
bool HasTag(const EngineRequestContext& c, const char* tag) {
  return c.security_context_present &&
      std::find(c.trace_tags.begin(), c.trace_tags.end(), tag) != c.trace_tags.end();
}
EngineApiDiagnostic Diagnostic(std::string code, std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(detail), {});
}
}

SblrDdlCreatePublicationCoordinationResult
CompileSblrDdlCreatePublicationDescriptor(const EngineRequestContext& c,
                                          const std::string& publication,
                                          std::uint64_t occurrence,
                                          std::uint32_t domain_occurrence,
                                          std::uint64_t availability) {
  std::lock_guard lock(mutex);
  SblrDdlCreatePublicationCoordinationResult out;
  if (!HasTag(c, "private_ddl_create_publication_binder") ||
      !c.statement_metadata_snapshot_engine_owned ||
      publication != c.statement_uuid.canonical || !occurrence ||
      !domain_occurrence || !availability) {
    out.diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
                                "sblr.ddl_create_publication.coordination_invalid");
    return out;
  }
  out.descriptor.body[0] = 1;
  out.descriptor.body[1] = static_cast<std::uint8_t>(occurrence);
  out.descriptor.body[2] = static_cast<std::uint8_t>(domain_occurrence);
  out.descriptor.body[3] = static_cast<std::uint8_t>(availability);
  auto bytes = scratchbird::engine::sblr::EncodeSblrDdlCreatePublicationDescriptorV1(
      out.descriptor);
  if (bytes.empty() ||
      !scratchbird::engine::sblr::DecodeSblrDdlCreatePublicationDescriptorV1(
          bytes.data(), bytes.size(), &out.descriptor, nullptr)) {
    out.diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
                                "sblr.ddl_create_publication.descriptor_invalid");
    return out;
  }
  live[Key(out.descriptor)] = out.descriptor;
  out.ok = true;
  out.diagnostic = Diagnostic("OK", "ok");
  return out;
}

SblrDdlCreatePublicationCoordinationResult
ConsumeSblrDdlCreatePublicationDescriptor(
    const EngineRequestContext& c, const Descriptor& descriptor) {
  std::lock_guard lock(mutex);
  SblrDdlCreatePublicationCoordinationResult out;
  const auto key = Key(descriptor);
  if (!HasTag(c, "private_ddl_create_publication")) {
    out.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                "sblr.ddl_create_publication.hidden");
    return out;
  }
  const auto it = live.find(key);
  if (it == live.end()) {
    out.diagnostic = used.count(key)
        ? Diagnostic("MGA.TRANSACTION.STALE", "sblr.ddl_create_publication.stale")
        : Diagnostic("SECURITY.ACCESS_DENIED", "sblr.ddl_create_publication.hidden");
    return out;
  }
  if (c.query_cancellation_requested && c.query_cancellation_requested()) {
    out.diagnostic = Diagnostic("PROCESS.CANCELLED",
                                "sblr.ddl_create_publication.cancelled");
    return out;
  }
  used[key] = descriptor;
  live.erase(it);
  out.ok = true;
  out.descriptor = descriptor;
  out.diagnostic = Diagnostic("OK", "ok");
  return out;
}
}
