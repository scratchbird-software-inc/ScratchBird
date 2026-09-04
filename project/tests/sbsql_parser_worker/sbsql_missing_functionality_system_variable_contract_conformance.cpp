// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "expression/reference_variable_compatibility.hpp"
#include "expression/expression_catalog.hpp"
#include "sblr_context_variables.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace parser = scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << "\n";
  return condition;
}

api::EngineRequestContext TestContext() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.database_path = "/tmp/sbsql-missing-system-variable-contract.sbdb";
  context.database_uuid.canonical = "019f1000-0000-7000-8000-000000000001";
  context.node_uuid.canonical = "019f1000-0000-7000-8000-000000000002";
  context.session_uuid.canonical = "019f1000-0000-7000-8000-000000000003";
  context.statement_uuid.canonical = "019f1000-0000-7000-8000-000000000004";
  context.principal_uuid.canonical = "019f1000-0000-7000-8000-000000000005";
  context.transaction_uuid.canonical = "019f1000-0000-7000-8000-000000000006";
  context.current_schema_uuid.canonical = "019f1000-0000-7000-8000-000000000007";
  context.local_transaction_id = 42;
  context.transaction_isolation_level = "snapshot";
  context.security_context_present = true;
  context.last_row_count = 7;
  context.last_row_count_present = true;
  context.statement_timestamp = "2026-06-05T12:00:00Z";
  context.current_timestamp = "2026-06-05T12:00:01Z";
  return context;
}

sblr::SblrDispatchResult DispatchSystemVariable(std::string_view variable_id,
                                                std::string_view reference_source = {}) {
  sblr::SblrDispatchRequest request;
  request.context = TestContext();
  request.envelope = sblr::MakeSblrEnvelope(
      "expression.system_variable_read",
      "SBLR_SYSTEM_VARIABLE_READ",
      "sbsql-missing-functionality-system-variable-contract");
  request.envelope.parser_package_uuid =
      "019f1000-0000-7000-8000-000000000008";
  request.envelope.registry_snapshot_uuid =
      "019f1000-0000-7000-8000-000000000009";
  request.envelope.requires_transaction_context = false;
  if (!variable_id.empty()) {
    request.envelope.operands.push_back(
        {"text", "variable_id", std::string(variable_id)});
  }
  if (!reference_source.empty()) {
    request.envelope.operands.push_back(
        {"text", "reference_source_spelling", std::string(reference_source)});
  }
  return sblr::DispatchSblrOperation(request);
}

sblr::SblrDispatchResult DispatchReferenceVariable(std::string_view reference_spelling) {
  const auto binding =
      parser::LowerReferenceVariableCompatibilityBySpelling(reference_spelling);
  sblr::SblrDispatchRequest request;
  request.context = TestContext();
  request.envelope = sblr::MakeSblrEnvelope(
      binding.sblr_operation_id,
      binding.sblr_opcode,
      "sbsql-missing-functionality-reference-variable-lowering-contract");
  request.envelope.parser_package_uuid =
      "019f1000-0000-7000-8000-000000000008";
  request.envelope.registry_snapshot_uuid =
      "019f1000-0000-7000-8000-000000000009";
  request.envelope.requires_transaction_context = false;
  for (const auto& operand : binding.operands) {
    request.envelope.operands.push_back(
        {operand.type, operand.name, operand.value});
  }
  return sblr::DispatchSblrOperation(request);
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEnvelopeDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string ReadSingleValue(const api::EngineApiResult& result) {
  if (result.result_shape.rows.size() != 1 ||
      result.result_shape.rows.front().fields.size() != 1) {
    return {};
  }
  return result.result_shape.rows.front().fields.front().second.encoded_value;
}

std::string ReadSingleType(const api::EngineApiResult& result) {
  if (result.result_shape.rows.size() != 1 ||
      result.result_shape.rows.front().fields.size() != 1) {
    return {};
  }
  return result.result_shape.rows.front()
      .fields.front()
      .second.descriptor.canonical_type_name;
}

bool ValidateOpcodeContract() {
  bool ok = true;
  const auto* entry =
      sblr::LookupSblrOperation("expression.system_variable_read");
  ok &= Require(entry != nullptr,
                "expression.system_variable_read missing from SBLR opcode registry");
  if (entry != nullptr) {
    ok &= Require(entry->opcode == "SBLR_SYSTEM_VARIABLE_READ",
                  "system variable opcode mismatch");
    ok &= Require(entry->code == 0,
                  "system variable expression child acquired a standalone opcode");
    ok &= Require(entry->support == sblr::SblrOpcodeSupport::implemented,
                  "system variable opcode is not implemented");
    ok &= Require(entry->transaction_effect ==
                      sblr::SblrOpcodeTransactionEffect::read,
                  "system variable read must be read-only");
    ok &= Require(entry->security_class ==
                      sblr::SblrOpcodeSecurityClass::authenticated,
                  "system variable read must require authenticated context");
    ok &= Require(!entry->requires_transaction_context,
                  "system variable read must not require transaction context");
    ok &= Require(!entry->requires_cluster_authority,
                  "system variable read must not require cluster authority");
  }
  return ok;
}

bool ValidateParserCompatibilityContract() {
  bool ok = true;
  std::size_t generated_row_count = 0;
  std::size_t accepted_count = 0;
  std::size_t refusal_count = 0;
  for (const auto& descriptor :
       parser::BuiltinReferenceVariableCompatibilityDescriptors()) {
    ok &= Require(!descriptor.native_sbsql_surface,
                  std::string(descriptor.reference_spelling) +
                      " incorrectly marked native SBsql");
    ok &= Require(descriptor.reference_parser_only,
                  std::string(descriptor.reference_spelling) +
                      " must be reference-parser-only metadata");
    ok &= Require(descriptor.sblr_operation_id ==
                      "expression.system_variable_read",
                  std::string(descriptor.reference_spelling) +
                      " must target SBLR system variable read");
    ok &= Require(descriptor.sblr_opcode == "SBLR_SYSTEM_VARIABLE_READ",
                  std::string(descriptor.reference_spelling) +
                      " must target SBLR_SYSTEM_VARIABLE_READ");
    if (!descriptor.surface_id.empty() &&
        descriptor.surface_id.rfind("SBSQL-", 0) == 0) {
      ++generated_row_count;
      ok &= Require(parser::FindExpressionSurfaceById(descriptor.surface_id) ==
                        nullptr,
                    std::string(descriptor.surface_id) +
                        " reference variable row leaked into native expression catalog");
    }
    if (descriptor.exact_refusal) {
      ++refusal_count;
      ok &= Require(!descriptor.diagnostic_id.empty(),
                    std::string(descriptor.reference_spelling) +
                        " exact refusal lacks diagnostic id");
    } else {
      ++accepted_count;
      ok &= Require(!descriptor.canonical_variable_id.empty(),
                    std::string(descriptor.reference_spelling) +
                        " accepted reference variable lacks canonical variable id");
    }
  }
  ok &= Require(generated_row_count == 27,
                "expected 27 generated reference variable compatibility rows");
  ok &= Require(accepted_count >= 14,
                "expected accepted canonical variable translations");
  ok &= Require(refusal_count >= 10,
                "expected explicit reference variable refusal translations");

  const auto* rowcount =
      parser::FindReferenceVariableCompatibilityBySpelling("@@ROWCOUNT");
  ok &= Require(rowcount != nullptr, "missing @@ROWCOUNT compatibility row");
  if (rowcount != nullptr) {
    ok &= Require(rowcount->canonical_variable_id == "ctx_last_row_count",
                  "@@ROWCOUNT canonical variable mismatch");
    ok &= Require(!rowcount->exact_refusal,
                  "@@ROWCOUNT should be an accepted compatibility alias");
  }

  const auto* autocommit =
      parser::FindReferenceVariableCompatibilityBySpelling("@@autocommit");
  ok &= Require(autocommit != nullptr,
                "missing @@autocommit compatibility row");
  if (autocommit != nullptr) {
    ok &= Require(autocommit->exact_refusal,
                  "@@autocommit should remain an exact refusal");
    ok &= Require(autocommit->canonical_function_id ==
                      "sb.scalar.refusal_system_variable_autocommit",
                  "@@autocommit refusal function mismatch");
  }

  ok &= Require(parser::FindExpressionSurfaceByName("@@ROWCOUNT") == nullptr,
                "@@ROWCOUNT must not be a native SBsql expression surface");
  ok &= Require(parser::FindExpressionSurfaceByName("SYSTEM_VAR('var')") !=
                    nullptr,
                "SYSTEM_VAR('var') must remain native SBsql syntax");
  ok &= Require(parser::FindReferenceVariableCompatibilityBySpelling(
                    "SYSTEM_VAR('var')") == nullptr,
                "SYSTEM_VAR('var') must not be reference-only compatibility metadata");

  const auto rowcount_binding =
      parser::LowerReferenceVariableCompatibilityBySpelling("@@ROWCOUNT");
  ok &= Require(rowcount_binding.sblr_operation_id ==
                    "expression.system_variable_read",
                "@@ROWCOUNT lowering operation mismatch");
  ok &= Require(rowcount_binding.sblr_opcode == "SBLR_SYSTEM_VARIABLE_READ",
                "@@ROWCOUNT lowering opcode mismatch");
  ok &= Require(rowcount_binding.canonical_variable_id == "ctx_last_row_count",
                "@@ROWCOUNT lowering canonical variable mismatch");
  ok &= Require(!rowcount_binding.exact_refusal,
                "@@ROWCOUNT lowering must not be an exact refusal");

  const auto autocommit_binding =
      parser::LowerReferenceVariableCompatibilityBySpelling("@@autocommit");
  ok &= Require(autocommit_binding.exact_refusal,
                "@@autocommit lowering must be an exact refusal");
  ok &= Require(autocommit_binding.diagnostic_id ==
                    "SB_DIAG_FUNCTION_RUNTIME_REFUSAL",
                "@@autocommit lowering diagnostic mismatch");
  return ok;
}

bool ValidateContextVariableResolver() {
  bool ok = true;
  const auto context = TestContext();
  sblr::SblrExecutionContext sblr_context;
  sblr_context.session_uuid = context.session_uuid.canonical;
  sblr_context.user_uuid = context.principal_uuid.canonical;
  sblr_context.local_transaction_id = context.local_transaction_id;
  sblr_context.transaction_isolation_level = context.transaction_isolation_level;
  sblr_context.last_row_count = context.last_row_count;
  sblr_context.last_row_count_present = context.last_row_count_present;
  sblr_context.security_context_present = context.security_context_present;

  const auto row_count =
      sblr::ResolveSblrContextVariable("ctx_last_row_count", sblr_context);
  ok &= Require(row_count.ok() && row_count.scalar_values.size() == 1,
                "ctx_last_row_count did not resolve");
  if (row_count.ok() && !row_count.scalar_values.empty()) {
    ok &= Require(row_count.scalar_values.front().uint64_value == 7,
                  "ctx_last_row_count value mismatch");
  }

  const auto engine_version = sblr::ResolveSblrContextVariable(
      "ctx_current_engine_version", sblr_context);
  ok &= Require(engine_version.ok() &&
                    engine_version.scalar_values.front().text_value ==
                        "ScratchBird 0.1.0",
                "ctx_current_engine_version mismatch");

  const auto session = sblr::ResolveSblrContextVariable(
      "ctx_current_session_uuid", sblr_context);
  ok &= Require(session.ok() && session.scalar_values.size() == 1 &&
                    session.scalar_values.front().text_value ==
                        "019f1000-0000-7000-8000-000000000003",
                "ctx_current_session_uuid mismatch");

  const auto timezone = sblr::ResolveSblrContextVariable(
      "ctx_current_timezone", sblr_context);
  ok &= Require(timezone.ok() && timezone.scalar_values.size() == 1 &&
                    timezone.scalar_values.front().text_value == "UTC",
                "ctx_current_timezone mismatch");

  const auto isolation = sblr::ResolveSblrContextVariable(
      "ctx_current_transaction_isolation", sblr_context);
  ok &= Require(isolation.ok() && isolation.scalar_values.size() == 1 &&
                    isolation.scalar_values.front().text_value == "snapshot",
                "ctx_current_transaction_isolation mismatch");

  const auto unknown =
      sblr::ResolveSblrContextVariable("ctx_not_registered", sblr_context);
  ok &= Require(!unknown.ok() && unknown.diagnostics.size() == 1 &&
                    unknown.diagnostics.front().diagnostic_id ==
                        "SB_DIAG_CONTEXT_VARIABLE_UNKNOWN",
                "unknown context variable did not fail closed");
  return ok;
}

bool ValidateStandaloneSblrRefusal() {
  bool ok = true;
  const auto rowcount = DispatchSystemVariable("ctx_last_row_count",
                                               "@@ROWCOUNT");
  ok &= Require(!rowcount.envelope_validated && !rowcount.accepted &&
                    !rowcount.dispatched_to_api && !rowcount.api_result.ok,
                "standalone system-variable expression reached API dispatch");
  ok &= Require(HasEnvelopeDiagnostic(
                    rowcount, "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"),
                "standalone system-variable expression refusal mismatch");

  const auto lowered_rowcount = DispatchReferenceVariable("@@ROWCOUNT");
  ok &= Require(!lowered_rowcount.envelope_validated &&
                    !lowered_rowcount.accepted &&
                    !lowered_rowcount.dispatched_to_api &&
                    !lowered_rowcount.api_result.ok,
                "lowered @@ROWCOUNT escaped its parent expression route");
  ok &= Require(HasEnvelopeDiagnostic(
                    lowered_rowcount,
                    "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"),
                "lowered @@ROWCOUNT standalone refusal mismatch");

  const auto lowered_spid = DispatchReferenceVariable("@@SPID");
  ok &= Require(!lowered_spid.envelope_validated && !lowered_spid.accepted &&
                    !lowered_spid.dispatched_to_api &&
                    !lowered_spid.api_result.ok,
                "lowered @@SPID escaped its parent expression route");

  const auto lowered_autocommit = DispatchReferenceVariable("@@autocommit");
  ok &= Require(!lowered_autocommit.envelope_validated &&
                    !lowered_autocommit.accepted &&
                    !lowered_autocommit.dispatched_to_api &&
                    !lowered_autocommit.api_result.ok,
                "lowered @@autocommit escaped its parent expression route");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= ValidateOpcodeContract();
  ok &= ValidateParserCompatibilityContract();
  ok &= ValidateContextVariableResolver();
  ok &= ValidateStandaloneSblrRefusal();
  return ok ? 0 : 1;
}
