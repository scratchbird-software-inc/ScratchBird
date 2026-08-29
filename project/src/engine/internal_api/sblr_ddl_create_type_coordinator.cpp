#include "sblr_ddl_create_type_coordinator.hpp"

#include "api_diagnostics.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {
bool HasPrivateTag(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}

EngineApiDiagnostic Diagnostic(std::string code, std::string message_key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(message_key), {});
}

EngineApiDiagnostic MissingExecutorEvidence() {
  return Diagnostic("SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                    "sblr.opcode.executor_evidence_missing");
}

bool IsCanonicalUuid(const std::string& value) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(value);
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == value;
}
}  // namespace

SblrDdlCreateTypeCoordinationResult CompileSblrDdlCreateTypeDescriptor(
    const EngineRequestContext& context, const std::string& receipt,
    std::uint64_t occurrence, std::uint32_t domain_occurrence,
    std::uint64_t availability) {
  SblrDdlCreateTypeCoordinationResult result;
  if (!HasPrivateTag(context, "private_ddl_create_type_binder") ||
      !context.statement_metadata_snapshot_engine_owned ||
      !IsCanonicalUuid(receipt) ||
      receipt != context.statement_uuid.canonical || !occurrence ||
      !domain_occurrence || !availability) {
    result.diagnostic =
        Diagnostic("SBLR.OPERAND_INVALID",
                   "sblr.ddl_create_type.coordination_invalid");
    return result;
  }
  result.diagnostic = MissingExecutorEvidence();
  return result;
}

SblrDdlCreateTypeCoordinationResult ConsumeSblrDdlCreateTypeDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrDdlCreateTypeDescriptorV1&
        descriptor) {
  SblrDdlCreateTypeCoordinationResult result;
  if (scratchbird::engine::sblr::EncodeSblrDdlCreateTypeDescriptorV1(
          descriptor, true)
          .empty()) {
    result.diagnostic =
        Diagnostic("SBLR.OPERAND_INVALID",
                   "sblr.ddl_create_type.coordination_invalid");
    return result;
  }
  if (!HasPrivateTag(context, "private_ddl_create_type")) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                   "sblr.ddl_create_type.hidden");
    return result;
  }
  result.diagnostic = MissingExecutorEvidence();
  return result;
}
}  // namespace scratchbird::engine::internal_api
