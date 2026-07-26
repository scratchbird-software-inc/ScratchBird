// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace sblr = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           const std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      const std::string_view code) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.local_transaction_id = 37;
  return context;
}

sblr::SblrOperationEnvelope QueryEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.qry.003");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "1"},
      {"uint32", "relational_root_node_id", "1"},
      {"relational_node_v1", "1", "13|0|-|1"},
  };
  return envelope;
}

bool ValidateRegistryClosure() {
  const auto* canonical = sblr::LookupSblrOperation("query.execute");
  const auto* legacy = sblr::LookupSblrOperation("query.plan_operation");
  bool passed = true;
  passed &= Require(canonical != nullptr,
                    "canonical query.execute registry row is absent");
  passed &= Require(canonical != nullptr &&
                        canonical->opcode == "SBLR_QUERY_EXECUTE" &&
                        canonical->support ==
                            sblr::SblrOpcodeSupport::implemented &&
                        canonical->requires_transaction_context,
                    "canonical query.execute registry contract differs");
  passed &= Require(legacy != nullptr,
                    "legacy query.plan_operation registry row is absent");
  passed &= Require(legacy != nullptr &&
                        legacy->support ==
                            sblr::SblrOpcodeSupport::deprecated_refusal &&
                        legacy->refusal_diagnostic ==
                            "QOW-DIAG-RELATIONAL-ROOT-NONCANONICAL",
                    "legacy query root is not fail-closed in the registry");
  return passed;
}

bool ValidateCanonicalDispatchSeam() {
  sblr::SblrDispatchRequest request;
  request.context = Context();
  request.envelope = QueryEnvelope();
  const auto result = sblr::DispatchSblrOperation(std::move(request));

  bool passed = true;
  passed &= Require(result.envelope_validated,
                    "canonical query envelope was not validated");
  passed &= Require(result.accepted && result.dispatched_to_api,
                    "validated typed DAG did not reach the API dispatch seam");
  passed &= Require(!result.api_result.ok,
                    "QRY-003 synthesized query execution success");
  passed &= Require(
      HasDispatchDiagnostic(
          result, "QOW-DIAG-RELATIONAL-PHYSICAL-DISPATCH-PENDING") &&
          HasApiDiagnostic(
              result, "QOW-DIAG-RELATIONAL-PHYSICAL-DISPATCH-PENDING"),
      "physical-dispatch seam diagnostic did not reach both result paths");
  return passed;
}

bool ValidateDagAndTransportRefusal() {
  auto dangling = QueryEnvelope();
  dangling.operands.back().value = "3|0|99|1";
  auto dangling_result = sblr::DispatchSblrOperation(
      {Context(), std::move(dangling), {}});

  auto unknown = QueryEnvelope();
  unknown.operands.push_back({"text", "silent_default", "forbidden"});
  auto unknown_result = sblr::DispatchSblrOperation(
      {Context(), std::move(unknown), {}});

  auto wrong_shape = QueryEnvelope();
  wrong_shape.result_shape = "engine.api.result.v1";
  auto shape_result = sblr::DispatchSblrOperation(
      {Context(), std::move(wrong_shape), {}});

  auto side_channel = QueryEnvelope();
  api::EngineApiRequest generic_payload;
  generic_payload.option_envelopes.push_back("query_operation:scan");
  auto side_channel_result = sblr::DispatchSblrOperation(
      {Context(), std::move(side_channel), std::move(generic_payload)});

  bool passed = true;
  passed &= Require(!dangling_result.accepted &&
                        !dangling_result.dispatched_to_api,
                    "dangling typed DAG reached physical dispatch");
  passed &= Require(HasApiDiagnostic(
                        dangling_result, "SBLR.PLAN_TREE.INVALID_HANDLE"),
                    "dangling DAG diagnostic differs");
  passed &= Require(!unknown_result.accepted &&
                        !unknown_result.dispatched_to_api,
                    "unknown query.execute operand was silently accepted");
  passed &= Require(HasApiDiagnostic(
                        unknown_result, "SBLR.PLAN_TREE.INVALID_HANDLE"),
                    "unknown-operand diagnostic differs");
  passed &= Require(
      !shape_result.accepted &&
          HasApiDiagnostic(shape_result,
                           "SB_DIAG_SBLR_RESULT_SHAPE_MISMATCH"),
      "noncanonical query result shape was accepted");
  passed &= Require(
      !side_channel_result.accepted &&
          HasApiDiagnostic(side_channel_result,
                           "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "out-of-band flat API payload bypassed the typed DAG route");
  return passed;
}

bool ValidateRegistryAndAuthorityRefusal() {
  auto mismatch = QueryEnvelope();
  mismatch.opcode = "SBLR_QUERY_PLAN_OPERATION";
  const auto mismatch_result = sblr::DispatchSblrOperation(
      {Context(), std::move(mismatch), {}});

  auto missing_declaration = QueryEnvelope();
  missing_declaration.requires_transaction_context = false;
  const auto declaration_result = sblr::DispatchSblrOperation(
      {Context(), std::move(missing_declaration), {}});

  auto missing_authority = QueryEnvelope();
  auto context = Context();
  context.local_transaction_id = 0;
  const auto authority_result = sblr::DispatchSblrOperation(
      {std::move(context), std::move(missing_authority), {}});

  auto legacy = sblr::MakeSblrEnvelope(
      "query.plan_operation", "SBLR_QUERY_PLAN_OPERATION", "qow.legacy");
  legacy.operands.push_back({"uint16", "legacy_wire_version", "1"});
  const auto legacy_result = sblr::DispatchSblrOperation(
      {Context(), std::move(legacy), {}});

  bool passed = true;
  passed &= Require(
      HasDispatchDiagnostic(mismatch_result,
                            "SB_DIAG_SBLR_OPCODE_MISMATCH"),
      "registry opcode mismatch did not reach dispatch diagnostics");
  passed &= Require(
      HasDispatchDiagnostic(declaration_result,
                            "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED"),
      "missing MGA-context declaration was not refused by the registry");
  passed &= Require(
      HasDispatchDiagnostic(authority_result,
                            "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED"),
      "missing engine MGA context was not refused by dispatch");
  passed &= Require(
      HasDispatchDiagnostic(legacy_result,
                            "QOW-DIAG-RELATIONAL-ROOT-NONCANONICAL") &&
          !legacy_result.accepted && !legacy_result.dispatched_to_api,
      "legacy relational root did not fail closed before execution");
  return passed;
}

bool ValidateDecodePath() {
  const auto encoded = sblr::EncodeSblrEnvelope(QueryEnvelope());
  const auto result =
      sblr::DecodeAndDispatchSblrOperation(encoded, Context());
  bool passed = true;
  passed &= Require(result.envelope_validated && result.accepted &&
                        result.dispatched_to_api,
                    "decoded canonical query did not reach the typed seam");
  passed &= Require(!result.api_result.ok &&
                        HasApiDiagnostic(
                            result,
                            "QOW-DIAG-RELATIONAL-PHYSICAL-DISPATCH-PENDING"),
                    "decoded canonical query synthesized completion or lost its diagnostic");
  return passed;
}

}  // namespace

// QOW-ROUTE-STAGE-QRY-003-V1
// QOW-TEST-QRY-003-V1
int main() {
  bool passed = true;
  passed &= ValidateRegistryClosure();
  passed &= ValidateCanonicalDispatchSeam();
  passed &= ValidateDagAndTransportRefusal();
  passed &= ValidateRegistryAndAuthorityRefusal();
  passed &= ValidateDecodePath();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
