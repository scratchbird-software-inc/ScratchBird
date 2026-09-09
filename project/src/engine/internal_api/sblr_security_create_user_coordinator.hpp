#pragma once

#include "api_diagnostics.hpp"
#include "api_types.hpp"
#include "engine/sblr/sblr_security_create_user_runtime.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

namespace scratchbird::engine::internal_api {

struct SblrSecurityCreateUserCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrSecurityCreateUserDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic;
};

namespace security_create_user_coordinator_detail {

inline std::mutex mutex;
inline std::map<std::string,
                scratchbird::engine::sblr::SblrSecurityCreateUserDescriptorV1>
    live;
inline std::map<std::string,
                scratchbird::engine::sblr::SblrSecurityCreateUserDescriptorV1>
    used;

inline std::string Key(
    const scratchbird::engine::sblr::SecurityCreateUserSha& value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline bool HasTag(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}

inline EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key), {});
}

}  // namespace security_create_user_coordinator_detail

inline SblrSecurityCreateUserCoordinationResult
CompileSblrSecurityCreateUserDescriptor(const EngineRequestContext& context,
                                        const std::string& receipt,
                                        std::uint64_t occurrence,
                                        std::uint32_t user_occurrence,
                                        std::uint64_t availability) {
  namespace detail = security_create_user_coordinator_detail;
  std::lock_guard lock(detail::mutex);
  SblrSecurityCreateUserCoordinationResult result;
  if (!detail::HasTag(context, "private_security_create_user_binder") ||
      !context.statement_metadata_snapshot_engine_owned ||
      receipt != context.statement_uuid.canonical || occurrence == 0 ||
      user_occurrence == 0 || availability == 0) {
    result.diagnostic = detail::Diagnostic(
        "SBLR.OPERAND.INVALID", "sblr.security_create_user.coordination_invalid");
    return result;
  }
  result.descriptor.body[0] = 1;
  result.descriptor.availability = availability;
  const auto bytes = scratchbird::engine::sblr::
      EncodeSblrSecurityCreateUserDescriptorV1(result.descriptor, false);
  if (bytes.empty() ||
      !scratchbird::engine::sblr::DecodeSblrSecurityCreateUserDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, nullptr, false)) {
    result.diagnostic = detail::Diagnostic(
        "SBLR.OPERAND.INVALID", "sblr.security_create_user.descriptor_invalid");
    return result;
  }
  detail::live[detail::Key(result.descriptor.evidence)] = result.descriptor;
  result.ok = true;
  return result;
}

inline SblrSecurityCreateUserCoordinationResult
ConsumeSblrSecurityCreateUserDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSecurityCreateUserDescriptorV1&
        descriptor) {
  namespace detail = security_create_user_coordinator_detail;
  std::lock_guard lock(detail::mutex);
  SblrSecurityCreateUserCoordinationResult result;
  if (!detail::HasTag(context, "private_security_create_user")) {
    result.diagnostic = detail::Diagnostic(
        "SECURITY.ACCESS_DENIED", "sblr.security_create_user.hidden");
    return result;
  }
  const auto key = detail::Key(descriptor.evidence);
  const auto iterator = detail::live.find(key);
  if (iterator == detail::live.end()) {
    result.diagnostic = detail::Diagnostic(
        detail::used.count(key) != 0 ? "MGA.TRANSACTION.STALE"
                                     : "SECURITY.ACCESS_DENIED",
        "sblr.security_create_user.replay");
    return result;
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    result.diagnostic = detail::Diagnostic(
        "PROCESS.CANCELLED", "sblr.security_create_user.cancelled");
    return result;
  }
  detail::used[key] = descriptor;
  detail::live.erase(iterator);
  result.descriptor = descriptor;
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api
