#pragma once

#include "engine/sblr/sblr_ddl_alter_type_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_type_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_type_runtime.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#ifdef NDEBUG
#undef assert
#define assert(condition) \
  ((condition) ? static_cast<void>(0) : std::abort())
#endif

namespace scratchbird::tests::ia08 {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

inline std::string TypeDdlDispatchCode(const sblr::SblrDispatchResult& result) {
  if (!result.diagnostics.empty()) return result.diagnostics.front().code;
  if (!result.api_result.diagnostics.empty()) {
    return result.api_result.diagnostics.front().code;
  }
  return {};
}

inline std::string TypeDdlValidationCode(
    const sblr::SblrEnvelopeValidationResult& result) {
  return result.diagnostics.empty() ? std::string{}
                                    : result.diagnostics.front().code;
}

inline void AssertTypeDdlEnvelopeRefusal(
    const api::EngineRequestContext& context,
    const sblr::SblrOperationEnvelope& envelope,
    const std::string& expected_code,
    const std::atomic<unsigned>& cancellation_probes) {
  const auto validation = sblr::ValidateSblrEnvelope(envelope);
  assert(!validation.ok);
  assert(TypeDdlValidationCode(validation) == expected_code);

  const auto preflight = sblr::PreflightSblrQueryOperation(
      {context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(!preflight.ok);
  assert(preflight.diagnostic_id == expected_code);
  assert(cancellation_probes.load() == 0);

  const auto dispatch = sblr::DispatchSblrOperation(
      {context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(!dispatch.envelope_validated);
  assert(!dispatch.accepted);
  assert(!dispatch.dispatched_to_api);
  assert(!dispatch.api_result.ok);
  assert(dispatch.api_result.evidence.empty());
  assert(!dispatch.canonical_result_published);
  assert(TypeDdlDispatchCode(dispatch) == expected_code);
  assert(cancellation_probes.load() == 0);
}

inline sblr::SblrOperand CrosswiredTypeDdlOperand(
    const std::string& operation_id) {
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.name = "type";
  if (operation_id == "engine.op.ddl_create_type") {
    sblr::SblrDdlAlterTypeDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.type = "alter_type_descriptor";
    operand.value_kind = sblr::SblrValueKind::alter_type_descriptor;
    operand.value_body =
        sblr::EncodeSblrDdlAlterTypeDescriptorV1(descriptor, true);
  } else if (operation_id == "engine.op.ddl_alter_type") {
    sblr::SblrDdlDropTypeDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.type = "drop_type_descriptor";
    operand.value_kind = sblr::SblrValueKind::drop_type_descriptor;
    operand.value_body =
        sblr::EncodeSblrDdlDropTypeDescriptorV1(descriptor, true);
  } else {
    sblr::SblrDdlCreateTypeDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.type = "create_type_descriptor";
    operand.value_kind = sblr::SblrValueKind::create_type_descriptor;
    operand.value_body =
        sblr::EncodeSblrDdlCreateTypeDescriptorV1(descriptor, true);
  }
  assert(!operand.value_body.empty());
  return operand;
}

inline void AssertTypeDdlRefusal(
    const api::EngineRequestContext& context,
    const sblr::SblrOperationEnvelope& envelope,
    const std::string& expected_code,
    const std::atomic<unsigned>& cancellation_probes) {
  const auto preflight = sblr::PreflightSblrQueryOperation(
      {context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(!preflight.ok);
  assert(preflight.diagnostic_id == expected_code);
  assert(cancellation_probes.load() == 0);

  const auto dispatch = sblr::DispatchSblrOperation(
      {context, envelope, api::EngineApiRequest{}, std::nullopt});
  assert(dispatch.envelope_validated);
  assert(!dispatch.accepted);
  assert(!dispatch.dispatched_to_api);
  assert(!dispatch.api_result.ok);
  assert(dispatch.api_result.evidence.empty());
  assert(!dispatch.canonical_result_published);
  assert(TypeDdlDispatchCode(dispatch) == expected_code);
  assert(cancellation_probes.load() == 0);
}

inline void VerifyTypeDdlFailsClosed(
    const std::string& operation_id, const std::string& opcode,
    const std::uint16_t opcode_code, const std::string& operand_type,
    const sblr::SblrValueKind value_kind,
    const std::vector<std::uint8_t>& execution_descriptor) {
  const auto* registry = sblr::LookupSblrOperation(operation_id);
  assert(registry != nullptr);
  assert(registry->operation_id == operation_id);
  assert(registry->opcode == opcode);
  assert(registry->code == opcode_code);
  assert(registry->executor_evidence_required);
  assert(!registry->executor_evidence_accepted);

  auto envelope = sblr::MakeSblrEnvelope(
      operation_id, opcode, "ia08.ddl_type.fail_closed");
  envelope.opcode_code = opcode_code;
  envelope.result_shape = "ddl_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019d0000-0000-7000-8000-000000002921";
  envelope.registry_snapshot_uuid =
      "019d0000-0000-7000-8000-000000002922";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = operand_type;
  operand.name = "type";
  operand.value_kind = value_kind;
  operand.value_body = execution_descriptor;
  envelope.operands.push_back(std::move(operand));
  assert(sblr::ValidateSblrEnvelope(envelope).ok);

  std::atomic<unsigned> cancellation_probes{0};
  const auto cancellation_probe = [&]() {
    ++cancellation_probes;
    return true;
  };

  api::EngineRequestContext no_security;
  no_security.local_transaction_id = 1;
  no_security.transaction_uuid.canonical =
      "019d0000-0000-7000-8000-000000002923";
  no_security.query_cancellation_requested = cancellation_probe;

  auto exact_context = no_security;
  exact_context.security_context_present = true;

  const std::string crosswired_operation_id =
      operation_id == "engine.op.ddl_create_type"
          ? "engine.op.ddl_alter_type"
          : "engine.op.ddl_create_type";
  const std::string crosswired_opcode =
      opcode == "SBLR_DDL_CREATE_TYPE" ? "SBLR_DDL_ALTER_TYPE"
                                        : "SBLR_DDL_CREATE_TYPE";
  const std::uint16_t crosswired_opcode_code =
      opcode_code == 1569 ? 1570 : 1569;

  auto invalid_identity = envelope;
  invalid_identity.operation_id = crosswired_operation_id;
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_identity,
                               "SBLR.OPCODE_INVALID", cancellation_probes);
  invalid_identity = envelope;
  invalid_identity.opcode = crosswired_opcode;
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_identity,
                               "SBLR.OPCODE_INVALID", cancellation_probes);
  invalid_identity = envelope;
  invalid_identity.opcode_code = crosswired_opcode_code;
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_identity,
                               "SBLR.OPCODE_INVALID", cancellation_probes);
  invalid_identity = envelope;
  invalid_identity.opcode_code = 0;
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_identity,
                               "SBLR.OPCODE_INVALID", cancellation_probes);
  invalid_identity = envelope;
  invalid_identity.operation_version_minor = 1;
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_identity,
                               "SBLR.OPCODE_INVALID", cancellation_probes);

  auto invalid_operand = envelope;
  invalid_operand.result_shape = "engine.api.result.v1";
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_operand,
                               "SBLR.OPERAND_INVALID", cancellation_probes);
  invalid_operand = envelope;
  invalid_operand.diagnostic_shape = "engine.diagnostic.v1";
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_operand,
                               "SBLR.OPERAND_INVALID", cancellation_probes);
  invalid_operand = envelope;
  invalid_operand.operands.clear();
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_operand,
                               "SBLR.OPERAND_INVALID", cancellation_probes);
  invalid_operand = envelope;
  invalid_operand.operands.front().name = "domain";
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_operand,
                               "SBLR.OPERAND_INVALID", cancellation_probes);
  invalid_operand = envelope;
  invalid_operand.operands.front() = CrosswiredTypeDdlOperand(operation_id);
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_operand,
                               "SBLR.OPERAND_INVALID", cancellation_probes);
  invalid_operand = envelope;
  invalid_operand.operands.push_back(invalid_operand.operands.front());
  invalid_operand.operands.back().ordinal = 2;
  AssertTypeDdlEnvelopeRefusal(exact_context, invalid_operand,
                               "SBLR.OPERAND_INVALID", cancellation_probes);

  AssertTypeDdlRefusal(no_security, envelope, "SECURITY.ACCESS_DENIED",
                       cancellation_probes);

  auto invalid_transaction = no_security;
  invalid_transaction.security_context_present = true;
  invalid_transaction.local_transaction_id = 0;
  invalid_transaction.transaction_uuid.canonical.clear();
  invalid_transaction.cluster_transaction_active = true;
  AssertTypeDdlRefusal(invalid_transaction, envelope,
                       "MGA.TRANSACTION_INVALID", cancellation_probes);

  auto cluster_fallthrough = exact_context;
  cluster_fallthrough.cluster_authority_available = true;
  AssertTypeDdlRefusal(
      cluster_fallthrough, envelope,
      "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
      cancellation_probes);

  cluster_fallthrough = exact_context;
  cluster_fallthrough.cluster_transaction_active = true;
  AssertTypeDdlRefusal(
      cluster_fallthrough, envelope,
      "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
      cancellation_probes);

  cluster_fallthrough = exact_context;
  cluster_fallthrough.route_fence_present = true;
  AssertTypeDdlRefusal(
      cluster_fallthrough, envelope,
      "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
      cancellation_probes);

  AssertTypeDdlRefusal(exact_context, envelope,
                       "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                       cancellation_probes);
}

}  // namespace scratchbird::tests::ia08
