// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "expression/donor_variable_compatibility.hpp"
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
                                                std::string_view donor_source = {}) {
  sblr::SblrDispatchRequest request;
  request.context = TestContext();
  request.envelope = sblr::MakeSblrEnvelope(
      "expression.system_variable_read",
      "SBLR_SYSTEM_VARIABLE_READ",
      "sbsql-missing-functionality-system-variable-contract");
  request.envelope.requires_transaction_context = false;
  if (!variable_id.empty()) {
    request.envelope.operands.push_back(
        {"text", "variable_id", std::string(variable_id)});
  }
  if (!donor_source.empty()) {
    request.envelope.operands.push_back(
        {"text", "donor_source_spelling", std::string(donor_source)});
  }
  return sblr::DispatchSblrOperation(request);
}

sblr::SblrDispatchResult DispatchDonorVariable(std::string_view donor_spelling) {
  const auto binding =
      parser::LowerDonorVariableCompatibilityBySpelling(donor_spelling);
  sblr::SblrDispatchRequest request;
  request.context = TestContext();
  request.envelope = sblr::MakeSblrEnvelope(
      binding.sblr_operation_id,
      binding.sblr_opcode,
      "sbsql-missing-functionality-donor-variable-lowering-contract");
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
       parser::BuiltinDonorVariableCompatibilityDescriptors()) {
    ok &= Require(!descriptor.native_sbsql_surface,
                  std::string(descriptor.donor_spelling) +
                      " incorrectly marked native SBsql");
    ok &= Require(descriptor.donor_parser_only,
                  std::string(descriptor.donor_spelling) +
                      " must be donor-parser-only metadata");
    ok &= Require(descriptor.sblr_operation_id ==
                      "expression.system_variable_read",
                  std::string(descriptor.donor_spelling) +
                      " must target SBLR system variable read");
    ok &= Require(descriptor.sblr_opcode == "SBLR_SYSTEM_VARIABLE_READ",
                  std::string(descriptor.donor_spelling) +
                      " must target SBLR_SYSTEM_VARIABLE_READ");
    if (!descriptor.surface_id.empty() &&
        descriptor.surface_id.rfind("SBSQL-", 0) == 0) {
      ++generated_row_count;
      ok &= Require(parser::FindExpressionSurfaceById(descriptor.surface_id) ==
                        nullptr,
                    std::string(descriptor.surface_id) +
                        " donor variable row leaked into native expression catalog");
    }
    if (descriptor.exact_refusal) {
      ++refusal_count;
      ok &= Require(!descriptor.diagnostic_id.empty(),
                    std::string(descriptor.donor_spelling) +
                        " exact refusal lacks diagnostic id");
    } else {
      ++accepted_count;
      ok &= Require(!descriptor.canonical_variable_id.empty(),
                    std::string(descriptor.donor_spelling) +
                        " accepted donor variable lacks canonical variable id");
    }
  }
  ok &= Require(generated_row_count == 27,
                "expected 27 generated donor variable compatibility rows");
  ok &= Require(accepted_count >= 14,
                "expected accepted canonical variable translations");
  ok &= Require(refusal_count >= 10,
                "expected explicit donor variable refusal translations");

  const auto* rowcount =
      parser::FindDonorVariableCompatibilityBySpelling("@@ROWCOUNT");
  ok &= Require(rowcount != nullptr, "missing @@ROWCOUNT compatibility row");
  if (rowcount != nullptr) {
    ok &= Require(rowcount->canonical_variable_id == "ctx_last_row_count",
                  "@@ROWCOUNT canonical variable mismatch");
    ok &= Require(!rowcount->exact_refusal,
                  "@@ROWCOUNT should be an accepted compatibility alias");
  }

  const auto* autocommit =
      parser::FindDonorVariableCompatibilityBySpelling("@@autocommit");
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
  ok &= Require(parser::FindDonorVariableCompatibilityBySpelling(
                    "SYSTEM_VAR('var')") == nullptr,
                "SYSTEM_VAR('var') must not be donor-only compatibility metadata");

  const auto rowcount_binding =
      parser::LowerDonorVariableCompatibilityBySpelling("@@ROWCOUNT");
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
      parser::LowerDonorVariableCompatibilityBySpelling("@@autocommit");
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
  return ok;
}

bool ValidateDispatchRoute() {
  bool ok = true;
  const auto rowcount = DispatchSystemVariable("ctx_last_row_count",
                                               "@@ROWCOUNT");
  ok &= Require(rowcount.envelope_validated,
                "rowcount envelope failed validation");
  ok &= Require(rowcount.accepted, "rowcount dispatch was not accepted");
  ok &= Require(rowcount.dispatched_to_api,
                "rowcount dispatch did not reach API");
  ok &= Require(rowcount.api_result.ok, "rowcount API result failed");
  ok &= Require(ReadSingleType(rowcount.api_result) == "uint64",
                "rowcount descriptor mismatch");
  ok &= Require(ReadSingleValue(rowcount.api_result) == "7",
                "rowcount value mismatch");
  ok &= Require(HasEvidence(rowcount.api_result,
                            "sblr_opcode",
                            "SBLR_SYSTEM_VARIABLE_READ"),
                "rowcount SBLR opcode evidence missing");
  ok &= Require(HasEvidence(rowcount.api_result,
                            "donor_source_spelling",
                            "@@ROWCOUNT"),
                "rowcount donor source evidence missing");
  ok &= Require(HasEvidence(rowcount.api_result,
                            "mga_visibility_authority",
                            "unchanged_context_read_no_lock_no_snapshot_mutation"),
                "rowcount MGA authority evidence missing");

  const auto session = DispatchSystemVariable("ctx_current_session_uuid",
                                              "@@SPID");
  ok &= Require(session.api_result.ok, "session variable API result failed");
  ok &= Require(ReadSingleType(session.api_result) == "uuid",
                "session variable descriptor mismatch");
  ok &= Require(ReadSingleValue(session.api_result) ==
                    "019f1000-0000-7000-8000-000000000003",
                "session variable value mismatch");

  const auto version =
      DispatchSystemVariable("ctx_current_engine_version", "@@VERSION");
  ok &= Require(version.api_result.ok, "engine version API result failed");
  ok &= Require(ReadSingleValue(version.api_result) == "ScratchBird 0.1.0",
                "engine version value mismatch");

  const auto timezone =
      DispatchSystemVariable("ctx_current_timezone", "@@time_zone");
  ok &= Require(timezone.api_result.ok, "timezone API result failed");
  ok &= Require(ReadSingleValue(timezone.api_result) == "UTC",
                "timezone value mismatch");

  const auto isolation = DispatchSystemVariable(
      "ctx_current_transaction_isolation", "@@tx_isolation");
  ok &= Require(isolation.api_result.ok, "isolation API result failed");
  ok &= Require(ReadSingleValue(isolation.api_result) == "snapshot",
                "isolation value mismatch");

  const auto missing = DispatchSystemVariable("");
  ok &= Require(!missing.api_result.ok,
                "missing variable id should fail closed");
  ok &= Require(HasDiagnostic(missing.api_result,
                              "SB_DIAG_SYSTEM_VARIABLE_ID_REQUIRED"),
                "missing variable id diagnostic mismatch");

  const auto unknown = DispatchSystemVariable("ctx_not_registered");
  ok &= Require(!unknown.api_result.ok,
                "unknown variable id should fail closed");
  ok &= Require(HasDiagnostic(unknown.api_result,
                              "SB_DIAG_CONTEXT_VARIABLE_UNKNOWN"),
                "unknown variable diagnostic mismatch");

  const auto lowered_rowcount = DispatchDonorVariable("@@ROWCOUNT");
  ok &= Require(lowered_rowcount.envelope_validated,
                "lowered @@ROWCOUNT envelope failed validation");
  ok &= Require(lowered_rowcount.accepted,
                "lowered @@ROWCOUNT dispatch was not accepted");
  ok &= Require(lowered_rowcount.api_result.ok,
                "lowered @@ROWCOUNT API result failed");
  ok &= Require(ReadSingleValue(lowered_rowcount.api_result) == "7",
                "lowered @@ROWCOUNT value mismatch");
  ok &= Require(HasEvidence(lowered_rowcount.api_result,
                            "donor_source_spelling",
                            "@@ROWCOUNT"),
                "lowered @@ROWCOUNT donor source evidence missing");

  const auto lowered_spid = DispatchDonorVariable("@@SPID");
  ok &= Require(lowered_spid.api_result.ok,
                "lowered @@SPID API result failed");
  ok &= Require(ReadSingleValue(lowered_spid.api_result) ==
                    "019f1000-0000-7000-8000-000000000003",
                "lowered @@SPID value mismatch");

  const auto lowered_autocommit = DispatchDonorVariable("@@autocommit");
  ok &= Require(lowered_autocommit.envelope_validated,
                "lowered @@autocommit envelope failed validation");
  ok &= Require(lowered_autocommit.accepted,
                "lowered @@autocommit dispatch was not accepted");
  ok &= Require(!lowered_autocommit.api_result.ok,
                "lowered @@autocommit should fail closed");
  ok &= Require(HasDiagnostic(lowered_autocommit.api_result,
                              "SB_DIAG_FUNCTION_RUNTIME_REFUSAL"),
                "lowered @@autocommit diagnostic mismatch");
  ok &= Require(HasEvidence(lowered_autocommit.api_result,
                            "exact_refusal",
                            "true"),
                "lowered @@autocommit exact refusal evidence missing");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= ValidateOpcodeContract();
  ok &= ValidateParserCompatibilityContract();
  ok &= ValidateContextVariableResolver();
  ok &= ValidateDispatchRoute();
  return ok ? 0 : 1;
}
