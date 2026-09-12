#include "sblr_ddl_create_procedure_coordinator.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {

using Descriptor = scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1;
using Uuid = scratchbird::engine::sblr::DdlCreateProcedureUuid;
using Sha = scratchbird::engine::sblr::DdlCreateProcedureSha;

std::mutex g_legacy_mutex;
std::map<std::string, Descriptor> g_legacy_live;
std::map<std::string, Descriptor> g_legacy_used;

void PutU64(std::array<std::uint8_t, 400>* body,
            std::size_t offset,
            std::uint64_t value,
            std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    (*body)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8));
  }
}

template <std::size_t N>
void PutBytes(std::array<std::uint8_t, 400>* body,
              std::size_t offset,
              const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), body->begin() + offset);
}

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

bool ValidInput(const SblrDdlCreateProcedureAuthorityInputV1& value) {
  return scratchbird::engine::sblr::
      ValidateSblrDdlCreateProcedureAuthorityV1(value);
}

std::string LegacyKey(const Sha& value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool HasTag(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}

}  // namespace

SblrDdlCreateProcedureCoordinationResult CompileSblrDdlCreateProcedureDescriptor(
    const SblrDdlCreateProcedureAuthorityInputV1& input) {
  SblrDdlCreateProcedureCoordinationResult result;
  if (!ValidInput(input)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.ddl_create_procedure.authority_input_invalid");
    return result;
  }

  auto& body = result.descriptor.body;
  PutBytes(&body, 0, input.receipt);
  PutU64(&body, 16, input.occurrence, 8);
  PutU64(&body, 24, input.procedure_occurrence, 4);
  PutU64(&body, 28, input.command_identity, 2);
  PutU64(&body, 30, input.body_profile, 2);
  PutBytes(&body, 32, input.procedure_uuid);
  PutU64(&body, 48, input.procedure_generation, 8);
  PutBytes(&body, 56, input.schema_uuid);
  PutU64(&body, 72, input.schema_generation, 8);
  PutBytes(&body, 80, input.owning_transaction_uuid);
  PutU64(&body, 96, input.owning_local_transaction_id, 8);
  PutBytes(&body, 104, input.statement_snapshot_uuid);
  PutBytes(&body, 120, input.catalog_epoch_uuid);
  PutU64(&body, 136, input.catalog_generation, 8);
  PutBytes(&body, 144, input.security_context_uuid);
  PutU64(&body, 160, input.security_epoch, 8);
  PutBytes(&body, 168, input.policy_snapshot_uuid);
  PutU64(&body, 184, input.policy_generation, 8);
  PutBytes(&body, 192, input.resource_grant_uuid);
  PutU64(&body, 208, input.resource_generation, 8);
  PutBytes(&body, 216, input.owner_principal_uuid);
  PutBytes(&body, 232, input.body_sblr_uuid);
  PutU64(&body, 248, input.body_sblr_generation, 8);
  PutBytes(&body, 256, input.body_sblr_sha256);
  PutBytes(&body, 288, input.procedure_abi_uuid);
  PutU64(&body, 304, input.procedure_abi_generation, 8);
  PutBytes(&body, 312, input.effect_set_sha256);
  PutBytes(&body, 344, input.recovery_uuid);
  PutU64(&body, 360, input.recovery_generation, 8);
  PutBytes(&body, 368, input.request_evidence_sha256);
  result.descriptor.availability = input.executor_availability_generation;

  const auto encoded = scratchbird::engine::sblr::
      EncodeSblrDdlCreateProcedureDescriptorV1(result.descriptor, false);
  if (encoded.empty() || !scratchbird::engine::sblr::
          DecodeSblrDdlCreateProcedureDescriptorV1(
              encoded.data(), encoded.size(), &result.descriptor, nullptr,
              false)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.ddl_create_procedure.descriptor_invalid");
    return result;
  }
  SblrDdlCreateProcedureAuthorityInputV1 decoded;
  if (!DecodeSblrDdlCreateProcedureAuthorityInputV1(result.descriptor,
                                                     &decoded)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.ddl_create_procedure.descriptor_round_trip_invalid");
    return result;
  }
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

bool DecodeSblrDdlCreateProcedureAuthorityInputV1(
    const Descriptor& descriptor,
    SblrDdlCreateProcedureAuthorityInputV1* out,
    std::string* detail) {
  return scratchbird::engine::sblr::
      DecodeSblrDdlCreateProcedureAuthorityV1(descriptor, out, detail);
}

SblrDdlCreateProcedureCoordinationResult CompileSblrDdlCreateProcedureDescriptor(
    const EngineRequestContext& context, const std::string& receipt,
    std::uint64_t occurrence, std::uint32_t procedure_occurrence,
    std::uint64_t availability) {
  std::lock_guard lock(g_legacy_mutex);
  SblrDdlCreateProcedureCoordinationResult result;
  if (!HasTag(context, "private_ddl_create_procedure_binder") ||
      !context.statement_metadata_snapshot_engine_owned ||
      receipt != context.statement_uuid || occurrence == 0 ||
      procedure_occurrence == 0 || availability == 0) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.ddl_create_procedure.legacy_coordination_invalid");
    return result;
  }
  result.descriptor.body[0] = 1;
  result.descriptor.body[1] = static_cast<std::uint8_t>(occurrence);
  result.descriptor.body[2] = static_cast<std::uint8_t>(procedure_occurrence);
  result.descriptor.availability = availability;
  const auto bytes = scratchbird::engine::sblr::
      EncodeSblrDdlCreateProcedureDescriptorV1(result.descriptor, false);
  if (!scratchbird::engine::sblr::DecodeSblrDdlCreateProcedureDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, nullptr, false)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.ddl_create_procedure.legacy_descriptor_invalid");
    return result;
  }
  g_legacy_live[LegacyKey(result.descriptor.evidence)] = result.descriptor;
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrDdlCreateProcedureCoordinationResult ConsumeSblrDdlCreateProcedureDescriptor(
    const EngineRequestContext& context, const Descriptor& value) {
  std::lock_guard lock(g_legacy_mutex);
  SblrDdlCreateProcedureCoordinationResult result;
  const auto key = LegacyKey(value.evidence);
  const auto found = g_legacy_live.find(key);
  if (!HasTag(context, "private_ddl_create_procedure")) {
    result.diagnostic = Diagnostic(
        "SECURITY.ACCESS_DENIED", "sblr.ddl_create_procedure.hidden");
    return result;
  }
  if (found == g_legacy_live.end()) {
    result.diagnostic = g_legacy_used.contains(key)
        ? Diagnostic("MGA.TRANSACTION.STALE",
                     "sblr.ddl_create_procedure.stale")
        : Diagnostic("SECURITY.ACCESS_DENIED",
                     "sblr.ddl_create_procedure.hidden");
    return result;
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic(
        "PROCESS.CANCELLED", "sblr.ddl_create_procedure.cancelled");
    return result;
  }
  g_legacy_used[key] = value;
  g_legacy_live.erase(found);
  result.ok = true;
  result.descriptor = value;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api
