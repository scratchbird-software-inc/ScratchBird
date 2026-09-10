// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_binding_authority.hpp"
#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteBindingAuthorityV1::Authority {
  DmlDeleteDurableAuthorityBundleV1 bundle;
  EngineDmlDeleteDatatypeAuthorityResultV1 datatype;
  EngineDmlDeleteSecurityAuthorityResultV1 security;
  EngineDmlDeleteEffectAuthorityResultV1 effects;
  EngineDmlUpdateResourceHandleV1 resource;
  std::shared_ptr<EngineDmlUpdateResourceReceiptV1> receipt;
};
namespace {
EngineApiDiagnostic Error(std::string detail, std::string code = "SECURITY.ACCESS_DENIED") {
  return MakeEngineApiDiagnostic(std::move(code), "sblr.dml_delete_rows.binding_authority_refused",
                                 std::move(detail), true);
}
EngineApiDiagnostic Ok() { return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false); }
bool Phase(const EngineRequestContext& c, std::string_view phase) {
  bool found = false;
  for (const auto& tag : c.trace_tags) {
    if (tag == phase) found = true;
    else if (tag.starts_with("private_dml_delete_rows_") || tag.starts_with("private_dml_update_rows_")) return false;
  }
  return found;
}
EngineRequestContext Consumer(const EngineRequestContext& c) {
  auto value = c;
  std::erase(value.trace_tags, "private_dml_delete_rows_binder");
  value.trace_tags.emplace_back("private_dml_delete_rows_consumer");
  return value;
}
EngineApiDiagnostic Validate(const EngineRequestContext& c, const DmlDeleteDurableAuthorityBundleV1& b,
    const EngineDmlDeleteDatatypeAuthorityResultV1& datatype,
    const EngineDmlDeleteSecurityAuthorityResultV1& security,
    const EngineDmlDeleteEffectAuthorityResultV1& effects,
    const EngineDmlUpdateResourceHandleV1& resource,
    const std::shared_ptr<EngineDmlUpdateResourceReceiptV1>& receipt) {
  if (!MatchesDmlDeleteDurableAuthorityOwnerV1(c, b) || !receipt ||
      c.dml_update_resource_receipt.lock() != receipt || !resource.valid()) return Error("complete_receipt_owner_required");
  auto diagnostic = receipt->Revalidate(c, resource);
  if (diagnostic.error) return diagnostic;
  if (!MatchesDmlDeleteDatatypeCaptureV1(datatype, b.descriptor, b.predicate))
    return Error("exact_datatype_source_capture_required");
  diagnostic = RevalidateDmlDeleteDatatypeAuthorityV1(c, datatype);
  if (diagnostic.error) return diagnostic;
  diagnostic = RevalidateDmlDeleteSecurityAuthorityV1(c, security);
  if (diagnostic.error) return diagnostic;
  diagnostic = RevalidateDmlDeleteEffectAuthorityV1(c, effects);
  if (diagnostic.error) return diagnostic;
  auto actual = b;
  actual.datatypes = datatype.datatypes; actual.operators = datatype.operators;
  actual.security = security.snapshot; actual.matched_grant_uuids = security.matched_grant_uuids;
  actual.effects = effects.snapshot; actual.resource_budget = *resource.carrier();
  std::vector<std::uint8_t> bytes;
  if (!EncodeDmlDeleteDurableAuthorityBundleV1(actual, &bytes, &diagnostic)) return diagnostic;
  if (bytes != b.exact_bytes) return Error("provider_projection_substitution");
  const SblrExecutorAvailabilityRowIdentity identity{
      "dml.delete_rows", 784, "1.0", "dml_delete_rows_descriptor", "mutation_result", 1};
  SblrExecutorAvailabilitySnapshot current;
  diagnostic = RevalidateSblrExecutorAvailability(c, identity, b.executor, &current);
  if (diagnostic.error) return diagnostic;
  if (!current.installed || current.generation != b.executor.generation ||
      current.snapshot_uuid != b.executor.snapshot_uuid ||
      current.decision_evidence_sha256 != b.executor.decision_evidence_sha256 ||
      current.row_identity_sha256 != b.executor.row_identity_sha256)
    return Error("live_DELETE_executor_required", "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  return Ok();
}
}  // namespace
const DmlDeleteDurableAuthorityBundleV1* EngineDmlDeleteBindingAuthorityV1::bundle() const noexcept {
  return authority_ ? &authority_->bundle : nullptr;
}
EngineDmlDeleteBindingAuthorityResultV1 CaptureDmlDeleteBindingAuthorityV1(
    const EngineRequestContext& context, const DmlDeleteDurableAuthorityBundleV1& input,
    const EngineDmlDeleteDatatypeAuthorityResultV1& datatype,
    const EngineDmlDeleteSecurityAuthorityResultV1& security,
    const EngineDmlDeleteEffectAuthorityResultV1& effects,
    const EngineDmlUpdateResourceHandleV1& resource) {
  EngineDmlDeleteBindingAuthorityResultV1 result;
  if (!Phase(context, "private_dml_delete_rows_binder")) {
    result.diagnostic = Error("private_binder_required"); return result;
  }
  std::vector<std::uint8_t> bytes;
  DmlDeleteDurableAuthorityBundleV1 bundle;
  if (!EncodeDmlDeleteDurableAuthorityBundleV1(input, &bytes, &result.diagnostic) ||
      !DecodeDmlDeleteDurableAuthorityBundleV1(bytes, &bundle, &result.diagnostic)) return result;
  const auto receipt = context.dml_update_resource_receipt.lock();
  result.diagnostic = Validate(Consumer(context), bundle, datatype, security, effects, resource, receipt);
  if (result.diagnostic.error) return result;
  auto authority = std::make_shared<EngineDmlDeleteBindingAuthorityV1::Authority>();
  authority->bundle = std::move(bundle); authority->datatype = datatype;
  authority->security = security; authority->effects = effects;
  authority->resource = resource; authority->receipt = receipt;
  result.handle.authority_ = std::move(authority);
  result.ok = true; result.diagnostic = Ok(); return result;
}
EngineApiDiagnostic RevalidateDmlDeleteBindingAuthorityV1(
    const EngineRequestContext& context, const EngineDmlDeleteBindingAuthorityV1& handle) {
  if (!Phase(context, "private_dml_delete_rows_consumer") || !handle.valid())
    return Error("private_consumer_and_engine_handle_required");
  const auto& a = *handle.authority_;
  return Validate(context, a.bundle, a.datatype, a.security, a.effects, a.resource, a.receipt);
}
}  // namespace scratchbird::engine::internal_api
