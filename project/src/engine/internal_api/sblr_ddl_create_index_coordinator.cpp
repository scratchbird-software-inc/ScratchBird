#include "sblr_ddl_create_index_coordinator.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include <algorithm>
namespace scratchbird::engine::internal_api {
namespace {
bool tag(const EngineRequestContext& c, const char* t) {
  return c.security_context_present &&
         std::find(c.trace_tags.begin(), c.trace_tags.end(), t) !=
             c.trace_tags.end();
}

EngineApiDiagnostic d(std::string c, std::string k) {
  return MakeEngineApiDiagnostic(std::move(c), std::move(k), {});
}

EngineApiDiagnostic MissingExecutorEvidence() {
  return d("SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
           "sblr.opcode.executor_evidence_missing");
}

bool canonical_uuid(const std::string& value) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(value);
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == value;
}
}  // namespace

SblrDdlCreateIndexCoordinationResult CompileSblrDdlCreateIndexDescriptor(
    const EngineRequestContext& c, const std::string& r,
    std::uint64_t occurrence, std::uint32_t index_occurrence,
    std::uint64_t availability) {
  SblrDdlCreateIndexCoordinationResult o;
  if (!tag(c, "private_ddl_create_index_binder") ||
      !c.statement_metadata_snapshot_engine_owned ||
      !canonical_uuid(r) || r != c.statement_uuid.canonical || !occurrence ||
      !index_occurrence || !availability) {
    o.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.ddl_create_index.coordination_invalid");
    return o;
  }
  o.diagnostic = MissingExecutorEvidence();
  return o;
}

SblrDdlCreateIndexCoordinationResult ConsumeSblrDdlCreateIndexDescriptor(
    const EngineRequestContext& c,
    const scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1&
        descriptor) {
  SblrDdlCreateIndexCoordinationResult o;
  if (scratchbird::engine::sblr::EncodeSblrDdlCreateIndexDescriptorV1(
          descriptor, true).empty()) {
    o.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.ddl_create_index.coordination_invalid");
    return o;
  }
  if (!tag(c, "private_ddl_create_index")) {
    o.diagnostic =
        d("SECURITY.ACCESS_DENIED", "sblr.ddl_create_index.hidden");
    return o;
  }
  o.diagnostic = MissingExecutorEvidence();
  return o;
}
}  // namespace scratchbird::engine::internal_api
