#include "sblr_procedure_invoke_coordinator.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <map>
#include <mutex>

namespace scratchbird::engine::internal_api {
namespace {

using Descriptor = scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1;
using Uuid = scratchbird::engine::sblr::ProcedureInvokeUuid;
using Sha = scratchbird::engine::sblr::ProcedureInvokeSha;

std::mutex g_legacy_mutex;
std::map<std::string, Descriptor> g_legacy_live;
std::map<std::string, Descriptor> g_legacy_used;

void PutU64(std::array<std::uint8_t, 416>* body, std::size_t offset,
            std::uint64_t value, std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    (*body)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8));
  }
}

template <std::size_t N>
void PutBytes(std::array<std::uint8_t, 416>* body, std::size_t offset,
              const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), body->begin() + offset);
}

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

bool ValidInput(const SblrProcedureInvokeAuthorityInputV1& value) {
  return scratchbird::engine::sblr::
      ValidateSblrProcedureInvokeAuthorityV1(value);
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

SblrProcedureInvokeCoordinationResult CompileSblrProcedureInvokeDescriptor(
    const SblrProcedureInvokeAuthorityInputV1& input) {
  SblrProcedureInvokeCoordinationResult result;
  if (!ValidInput(input)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.procedure_invoke.authority_input_invalid");
    return result;
  }
  auto& body = result.descriptor.body;
  PutBytes(&body, 0, input.invocation_uuid);
  PutU64(&body, 16, input.invocation_generation, 8);
  PutBytes(&body, 24, input.owning_transaction_uuid);
  PutU64(&body, 40, input.owning_local_transaction_id, 8);
  PutBytes(&body, 48, input.statement_snapshot_uuid);
  PutBytes(&body, 64, input.catalog_epoch_uuid);
  PutU64(&body, 80, input.catalog_generation, 8);
  PutBytes(&body, 88, input.security_context_uuid);
  PutBytes(&body, 104, input.policy_snapshot_uuid);
  PutU64(&body, 120, input.policy_generation, 8);
  PutBytes(&body, 128, input.procedure_uuid);
  PutU64(&body, 144, input.procedure_generation, 8);
  PutBytes(&body, 152, input.procedure_body_uuid);
  PutU64(&body, 168, input.procedure_body_generation, 8);
  PutBytes(&body, 176, input.procedure_body_sha256);
  PutBytes(&body, 208, input.procedure_abi_uuid);
  PutU64(&body, 224, input.procedure_abi_generation, 8);
  PutBytes(&body, 232, input.argument_vector_uuid);
  PutU64(&body, 248, input.argument_count, 4);
  PutU64(&body, 252, input.output_parameter_count, 4);
  PutU64(&body, 256, input.invocation_flags, 4);
  PutBytes(&body, 264, input.argument_vector_sha256);
  PutBytes(&body, 296, input.output_descriptor_vector_uuid);
  PutU64(&body, 312, input.output_descriptor_vector_generation, 8);
  PutBytes(&body, 320, input.result_set_shape_uuid);
  PutU64(&body, 336, input.result_set_shape_generation, 8);
  PutBytes(&body, 344, input.effect_set_sha256);
  PutBytes(&body, 376, input.recovery_uuid);
  PutU64(&body, 392, input.recovery_generation, 8);
  PutBytes(&body, 400, input.engine_snapshot_uuid);
  result.descriptor.availability = input.executor_availability_generation;

  const auto encoded = scratchbird::engine::sblr::
      EncodeSblrProcedureInvokeDescriptorV1(result.descriptor, false);
  if (encoded.empty() || !scratchbird::engine::sblr::
          DecodeSblrProcedureInvokeDescriptorV1(
              encoded.data(), encoded.size(), &result.descriptor, nullptr,
              false)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID", "sblr.procedure_invoke.descriptor_invalid");
    return result;
  }
  SblrProcedureInvokeAuthorityInputV1 decoded;
  if (!DecodeSblrProcedureInvokeAuthorityInputV1(result.descriptor, &decoded)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.procedure_invoke.descriptor_round_trip_invalid");
    return result;
  }
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

bool DecodeSblrProcedureInvokeAuthorityInputV1(
    const Descriptor& descriptor, SblrProcedureInvokeAuthorityInputV1* out,
    std::string* detail) {
  return scratchbird::engine::sblr::
      DecodeSblrProcedureInvokeAuthorityV1(descriptor, out, detail);
}

SblrProcedureInvokeCoordinationResult CompileSblrProcedureInvokeDescriptor(
    const EngineRequestContext& context, const std::string& receipt,
    std::uint64_t occurrence, std::uint32_t invocation_occurrence,
    std::uint64_t availability) {
  std::lock_guard lock(g_legacy_mutex);
  SblrProcedureInvokeCoordinationResult result;
  if (!HasTag(context, "private_procedure_invoke_binder") ||
      !context.statement_metadata_snapshot_engine_owned ||
      receipt != context.statement_uuid.canonical || occurrence == 0 ||
      invocation_occurrence == 0 || availability == 0) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.procedure_invoke.legacy_coordination_invalid");
    return result;
  }
  result.descriptor.body[0] = 1;
  result.descriptor.body[1] = static_cast<std::uint8_t>(occurrence);
  result.descriptor.body[2] = static_cast<std::uint8_t>(invocation_occurrence);
  result.descriptor.availability = availability;
  const auto bytes = scratchbird::engine::sblr::
      EncodeSblrProcedureInvokeDescriptorV1(result.descriptor, false);
  if (!scratchbird::engine::sblr::DecodeSblrProcedureInvokeDescriptorV1(
          bytes.data(), bytes.size(), &result.descriptor, nullptr, false)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.procedure_invoke.legacy_descriptor_invalid");
    return result;
  }
  g_legacy_live[LegacyKey(result.descriptor.evidence)] = result.descriptor;
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrProcedureInvokeCoordinationResult ConsumeSblrProcedureInvokeDescriptor(
    const EngineRequestContext& context, const Descriptor& value) {
  std::lock_guard lock(g_legacy_mutex);
  SblrProcedureInvokeCoordinationResult result;
  const auto key = LegacyKey(value.evidence);
  const auto found = g_legacy_live.find(key);
  if (!HasTag(context, "private_procedure_invoke")) {
    result.diagnostic = Diagnostic(
        "SECURITY.ACCESS_DENIED", "sblr.procedure_invoke.hidden");
    return result;
  }
  if (found == g_legacy_live.end()) {
    result.diagnostic = g_legacy_used.contains(key)
        ? Diagnostic("MGA.TRANSACTION.STALE", "sblr.procedure_invoke.stale")
        : Diagnostic("SECURITY.ACCESS_DENIED", "sblr.procedure_invoke.hidden");
    return result;
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic("PROCESS.CANCELLED",
                                   "sblr.procedure_invoke.cancelled");
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
