#include "engine/sblr/sblr_ddl_create_index_runtime.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <string>

#ifdef NDEBUG
#undef assert
#define assert(condition) \
  ((condition) ? static_cast<void>(0) : std::abort())
#endif

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

sblr::SblrOperationEnvelope CanonicalEnvelope(
    const std::vector<std::uint8_t>& encoded_descriptor) {
  auto envelope = sblr::MakeSblrEnvelope(
      "engine.op.ddl_create_index", "SBLR_DDL_CREATE_INDEX",
      "ia08.ddl_create_index.fail_closed");
  envelope.opcode_code = 1540;
  envelope.result_shape = "ddl_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019d0000-0000-7000-8000-000000002600";
  envelope.registry_snapshot_uuid =
      "019d0000-0000-7000-8000-000000002601";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "create_index_descriptor";
  operand.name = "index";
  operand.value_kind = sblr::SblrValueKind::create_index_descriptor;
  operand.value_body = encoded_descriptor;
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

std::string DispatchCode(const sblr::SblrDispatchResult& result) {
  if (!result.diagnostics.empty()) return result.diagnostics.front().code;
  if (!result.api_result.diagnostics.empty()) {
    return result.api_result.diagnostics.front().code;
  }
  return {};
}

}  // namespace

int main() {
  std::string detail;
  sblr::SblrDdlCreateIndexRequestV1 request;
  request.receipt[0] = 1;
  request.occurrence = request.index_occurrence = 1;
  auto request_bytes = sblr::EncodeSblrDdlCreateIndexRequestV1(request);
  sblr::SblrDdlCreateIndexRequestV1 decoded_request;
  if (request_bytes.size() != 64 ||
      !sblr::DecodeSblrDdlCreateIndexRequestV1(
          request_bytes.data(), request_bytes.size(), &decoded_request,
          &detail)) {
    return 1;
  }
  request_bytes[44] = 1;
  if (sblr::DecodeSblrDdlCreateIndexRequestV1(
          request_bytes.data(), request_bytes.size(), &decoded_request,
          &detail)) {
    return 2;
  }

  sblr::SblrDdlCreateIndexDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto descriptor_bytes =
      sblr::EncodeSblrDdlCreateIndexDescriptorV1(descriptor, false);
  sblr::SblrDdlCreateIndexDescriptorV1 decoded_descriptor;
  if (descriptor_bytes.size() != 488 ||
      !sblr::DecodeSblrDdlCreateIndexDescriptorV1(
          descriptor_bytes.data(), descriptor_bytes.size(),
          &decoded_descriptor, &detail, false)) {
    return 3;
  }
  const auto execution_descriptor =
      sblr::EncodeSblrDdlCreateIndexDescriptorV1(decoded_descriptor, true);
  auto malformed_descriptor = execution_descriptor;
  malformed_descriptor[408] ^= 1;
  if (sblr::DecodeSblrDdlCreateIndexDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail, true)) {
    return 4;
  }

  sblr::SblrDdlCreateIndexResultV1 result_value;
  result_value.body[0] = 1;
  result_value.body[24] = 1;
  result_value.body[25] = 1;
  result_value.body[56] = 1;
  result_value.availability = 1;
  result_value.publication_barrier[0] = 1;
  auto result_bytes = sblr::EncodeSblrDdlCreateIndexResultV1(result_value);
  sblr::SblrDdlCreateIndexResultV1 decoded_result;
  if (result_bytes.size() != 320 ||
      !sblr::DecodeSblrDdlCreateIndexResultV1(
          result_bytes.data(), result_bytes.size(), &decoded_result,
          &detail)) {
    return 5;
  }
  result_bytes[256] ^= 1;
  if (sblr::DecodeSblrDdlCreateIndexResultV1(
          result_bytes.data(), result_bytes.size(), &decoded_result,
          &detail)) {
    return 6;
  }

  const auto* registry =
      sblr::LookupSblrOperation("engine.op.ddl_create_index");
  assert(registry != nullptr);
  assert(registry->support == sblr::SblrOpcodeSupport::implemented);
  assert(registry->executor_evidence_required);
  assert(!registry->executor_evidence_accepted);

  const auto envelope = CanonicalEnvelope(execution_descriptor);
  assert(sblr::ValidateSblrEnvelope(envelope).ok);

  std::atomic<unsigned> cancellation_probes{0};
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.local_transaction_id = 1;
  context.query_cancellation_requested = [&]() {
    ++cancellation_probes;
    return true;
  };
  const auto preflight = sblr::PreflightSblrQueryOperation(
      {context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(!preflight.ok);
  assert(preflight.diagnostic_id ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(cancellation_probes.load() == 0);

  const auto refused = sblr::DispatchSblrOperation(
      {context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(refused.envelope_validated);
  assert(!refused.accepted);
  assert(!refused.dispatched_to_api);
  assert(!refused.api_result.ok);
  assert(refused.api_result.evidence.empty());
  assert(DispatchCode(refused) ==
         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING");
  assert(cancellation_probes.load() == 0);

  auto malformed_envelope = envelope;
  malformed_envelope.operands.front().value_body[408] ^= 1;
  api::EngineRequestContext no_authority;
  const auto malformed_preflight = sblr::PreflightSblrQueryOperation(
      {no_authority, malformed_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!malformed_preflight.ok);
  assert(malformed_preflight.diagnostic_id == "SBLR.OPERAND_INVALID");
  const auto malformed = sblr::DispatchSblrOperation(
      {no_authority, malformed_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!malformed.envelope_validated);
  assert(DispatchCode(malformed) == "SBLR.OPERAND_INVALID");

  auto missing_operand_envelope = envelope;
  missing_operand_envelope.operands.clear();
  const auto missing_operand_preflight = sblr::PreflightSblrQueryOperation(
      {no_authority, missing_operand_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!missing_operand_preflight.ok);
  assert(missing_operand_preflight.diagnostic_id == "SBLR.OPERAND_INVALID");
  const auto missing_operand = sblr::DispatchSblrOperation(
      {no_authority, missing_operand_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!missing_operand.envelope_validated);
  assert(DispatchCode(missing_operand) == "SBLR.OPERAND_INVALID");

  auto wrong_shape_envelope = envelope;
  wrong_shape_envelope.operands.front().type = "create_table_descriptor";
  wrong_shape_envelope.operands.front().name = "request";
  const auto wrong_shape_preflight = sblr::PreflightSblrQueryOperation(
      {no_authority, wrong_shape_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!wrong_shape_preflight.ok);
  assert(wrong_shape_preflight.diagnostic_id == "SBLR.OPERAND_INVALID");
  const auto wrong_shape = sblr::DispatchSblrOperation(
      {no_authority, wrong_shape_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!wrong_shape.envelope_validated);
  assert(DispatchCode(wrong_shape) == "SBLR.OPERAND_INVALID");

  auto extra_operand_envelope = envelope;
  auto extra_operand = extra_operand_envelope.operands.front();
  extra_operand.ordinal = 2;
  extra_operand.name = "second_index";
  extra_operand_envelope.operands.push_back(std::move(extra_operand));
  const auto extra_operand_preflight = sblr::PreflightSblrQueryOperation(
      {no_authority, extra_operand_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!extra_operand_preflight.ok);
  assert(extra_operand_preflight.diagnostic_id == "SBLR.OPERAND_INVALID");
  const auto extra_operand_dispatch = sblr::DispatchSblrOperation(
      {no_authority, extra_operand_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!extra_operand_dispatch.envelope_validated);
  assert(DispatchCode(extra_operand_dispatch) == "SBLR.OPERAND_INVALID");

  auto wrong_result_envelope = envelope;
  wrong_result_envelope.result_shape = "engine.api.result.v1";
  const auto wrong_result_preflight = sblr::PreflightSblrQueryOperation(
      {no_authority, wrong_result_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!wrong_result_preflight.ok);
  assert(wrong_result_preflight.diagnostic_id == "SBLR.OPERAND_INVALID");
  const auto wrong_result_dispatch = sblr::DispatchSblrOperation(
      {no_authority, wrong_result_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!wrong_result_dispatch.envelope_validated);
  assert(DispatchCode(wrong_result_dispatch) == "SBLR.OPERAND_INVALID");

  auto wrong_diagnostic_envelope = envelope;
  wrong_diagnostic_envelope.diagnostic_shape = "engine.diagnostic.v1";
  const auto wrong_diagnostic_preflight = sblr::PreflightSblrQueryOperation(
      {no_authority, wrong_diagnostic_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!wrong_diagnostic_preflight.ok);
  assert(wrong_diagnostic_preflight.diagnostic_id ==
         "SBLR.OPERAND_INVALID");
  const auto wrong_diagnostic_dispatch = sblr::DispatchSblrOperation(
      {no_authority, wrong_diagnostic_envelope, api::EngineApiRequest{},
       std::nullopt});
  assert(!wrong_diagnostic_dispatch.envelope_validated);
  assert(DispatchCode(wrong_diagnostic_dispatch) ==
         "SBLR.OPERAND_INVALID");

  const auto security = sblr::DispatchSblrOperation(
      {no_authority, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(DispatchCode(security) == "SECURITY.ACCESS_DENIED");

  auto clustered_context = no_authority;
  clustered_context.security_context_present = true;
  clustered_context.cluster_transaction_active = true;
  const auto clustered = sblr::DispatchSblrOperation(
      {clustered_context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(DispatchCode(clustered) ==
         "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN");

  auto stale_context = no_authority;
  stale_context.security_context_present = true;
  const auto stale = sblr::DispatchSblrOperation(
      {stale_context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(DispatchCode(stale) == "MGA.TRANSACTION.STALE");
  return 0;
}
