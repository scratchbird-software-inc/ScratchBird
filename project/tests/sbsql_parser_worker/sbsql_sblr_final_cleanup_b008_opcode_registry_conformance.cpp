// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_sblr_admission_test_helper.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef SCRATCHBIRD_PROJECT_SOURCE_DIR
#define SCRATCHBIRD_PROJECT_SOURCE_DIR "."
#endif

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;

struct OpcodeRow {
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view family;
  sblr::SblrOpcodeCategory category;
  sblr::SblrOpcodeSupport support;
  sblr::SblrOpcodeTransactionEffect transaction_effect;
  sblr::SblrOpcodeSecurityClass security_class;
  bool requires_transaction_context;
  bool requires_cluster_authority;
};

constexpr std::array<OpcodeRow, 50> kRows{{
    {"engine.op.package_begin", "SBLR_PACKAGE_BEGIN", "core-envelope", sblr::SblrOpcodeCategory::core, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.package_end", "SBLR_PACKAGE_END", "core-envelope", sblr::SblrOpcodeCategory::core, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.literal", "SBLR_LITERAL", "core-envelope", sblr::SblrOpcodeCategory::core, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.parameter", "SBLR_PARAMETER", "core-envelope", sblr::SblrOpcodeCategory::core, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.variable", "SBLR_VARIABLE", "core-envelope", sblr::SblrOpcodeCategory::core, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.source_map", "SBLR_SOURCE_MAP", "core-envelope", sblr::SblrOpcodeCategory::core, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.error_vector", "SBLR_ERROR_VECTOR", "core-envelope", sblr::SblrOpcodeCategory::core, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.txn_begin", "SBLR_TXN_BEGIN", "transaction-control", sblr::SblrOpcodeCategory::transaction, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"engine.op.txn_commit", "SBLR_TXN_COMMIT", "transaction-control", sblr::SblrOpcodeCategory::transaction, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"engine.op.txn_rollback", "SBLR_TXN_ROLLBACK", "transaction-control", sblr::SblrOpcodeCategory::transaction, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"engine.op.txn_savepoint", "SBLR_TXN_SAVEPOINT", "transaction-control", sblr::SblrOpcodeCategory::transaction, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"engine.op.txn_release_savepoint", "SBLR_TXN_RELEASE_SAVEPOINT", "transaction-control", sblr::SblrOpcodeCategory::transaction, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"engine.op.txn_rollback_to_savepoint", "SBLR_TXN_ROLLBACK_TO_SAVEPOINT", "transaction-control", sblr::SblrOpcodeCategory::transaction, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"engine.op.cursor_open", "SBLR_CURSOR_OPEN", "data-read", sblr::SblrOpcodeCategory::data_read, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.cursor_fetch", "SBLR_CURSOR_FETCH", "data-read", sblr::SblrOpcodeCategory::data_read, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.cursor_close", "SBLR_CURSOR_CLOSE", "data-read", sblr::SblrOpcodeCategory::data_read, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.read_by_key", "SBLR_READ_BY_KEY", "data-read", sblr::SblrOpcodeCategory::data_read, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.read_range", "SBLR_READ_RANGE", "data-read", sblr::SblrOpcodeCategory::data_read, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.read_stream", "SBLR_READ_STREAM", "data-read", sblr::SblrOpcodeCategory::data_read, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.result_set_pass", "SBLR_RESULT_SET_PASS", "data-read", sblr::SblrOpcodeCategory::data_read, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.insert", "SBLR_INSERT", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.update", "SBLR_UPDATE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.delete", "SBLR_DELETE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.merge", "SBLR_MERGE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.cluster_write_admission", "SBLR_CLUSTER_WRITE_ADMISSION", "data-mutation", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"engine.op.table_truncate", "SBLR_TABLE_TRUNCATE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.table_analyze", "SBLR_TABLE_ANALYZE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.bulk_import_stream", "SBLR_BULK_IMPORT_STREAM", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.bulk_export_stream", "SBLR_BULK_EXPORT_STREAM", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.statement_batch", "SBLR_STATEMENT_BATCH", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.atomic_cas", "SBLR_ATOMIC_CAS", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.atomic_read_modify_write", "SBLR_ATOMIC_READ_MODIFY_WRITE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.advisory_lock_acquire", "SBLR_ADVISORY_LOCK_ACQUIRE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.advisory_lock_release", "SBLR_ADVISORY_LOCK_RELEASE", "data-mutation", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.function_call", "SBLR_FUNCTION_CALL", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.operator_call", "SBLR_OPERATOR_CALL", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.cast", "SBLR_CAST", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.compare", "SBLR_COMPARE", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.domain_operation", "SBLR_DOMAIN_OPERATION", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.procedure_invoke", "SBLR_PROCEDURE_INVOKE", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.function_invoke", "SBLR_FUNCTION_INVOKE", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.aggregate_invoke", "SBLR_AGGREGATE_INVOKE", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.sequence_nextval", "SBLR_SEQUENCE_NEXTVAL", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.sequence_currval", "SBLR_SEQUENCE_CURRVAL", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.sequence_setval", "SBLR_SEQUENCE_SETVAL", "expression-eval", sblr::SblrOpcodeCategory::expression, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.project", "SBLR_PROJECT", "result-shape", sblr::SblrOpcodeCategory::result_shape, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.aggregate", "SBLR_AGGREGATE", "result-shape", sblr::SblrOpcodeCategory::result_shape, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.group", "SBLR_GROUP", "result-shape", sblr::SblrOpcodeCategory::result_shape, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.sort", "SBLR_SORT", "result-shape", sblr::SblrOpcodeCategory::result_shape, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.limit", "SBLR_LIMIT", "result-shape", sblr::SblrOpcodeCategory::result_shape, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::string EvidenceMessage(const OpcodeRow& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string out(row.opcode);
  out += ' ';
  out += phase;
  out += ": ";
  out += message;
  return out;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasApiDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

sblr::SblrOperationEnvelope EnvelopeFor(const OpcodeRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "opcode-registry-conformance");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.requires_transaction_context;
  envelope.requires_cluster_authority = row.requires_cluster_authority;
  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      envelope);
}

std::string_view ExpectedExecutorId(const OpcodeRow& row) {
  return row.operation_id;
}

void RequireCanonicalLookupAndMetadata(const OpcodeRow& row) {
  const auto* by_opcode = sblr::LookupSblrOpcode(row.opcode);
  Require(by_opcode != nullptr, EvidenceMessage(row, "lookup", "canonical opcode is not registered"));
  Require(by_opcode->operation_id == row.operation_id,
          EvidenceMessage(row, "lookup", "operation id drifted"));
  Require(by_opcode->family == row.family,
          EvidenceMessage(row, "metadata", "spec family drifted"));
  Require(by_opcode->category == row.category,
          EvidenceMessage(row, "metadata", "category drifted"));
  Require(by_opcode->support == row.support,
          EvidenceMessage(row, "metadata", "support state drifted"));
  Require(by_opcode->transaction_effect == row.transaction_effect,
          EvidenceMessage(row, "metadata", "transaction effect drifted"));
  Require(by_opcode->security_class == row.security_class,
          EvidenceMessage(row, "metadata", "security class drifted"));
  Require(by_opcode->requires_security_context,
          EvidenceMessage(row, "authority", "security context is not required"));
  Require(by_opcode->requires_transaction_context == row.requires_transaction_context,
          EvidenceMessage(row, "authority", "transaction context requirement drifted"));
  Require(by_opcode->requires_cluster_authority == row.requires_cluster_authority,
          EvidenceMessage(row, "authority", "cluster authority requirement drifted"));
  Require(by_opcode->cluster_private == row.requires_cluster_authority,
          EvidenceMessage(row, "authority", "cluster-private marker drifted"));

  Require(by_opcode->executor_id == ExpectedExecutorId(row),
          EvidenceMessage(row, "executor", "Core executor binding drifted"));
  Require(by_opcode->executor_evidence_required,
          EvidenceMessage(row, "executor", "Core executor evidence gate is not required"));

  const auto* by_operation = sblr::LookupSblrOperation(row.operation_id);
  Require(by_operation == by_opcode,
          EvidenceMessage(row, "lookup", "operation lookup did not return canonical entry"));
}

void RequireCanonicalEnvelopeValidation(const OpcodeRow& row) {
  const auto envelope = EnvelopeFor(row);
  const auto envelope_validation = sblr::ValidateSblrEnvelope(envelope);
  const bool expression_leaf_root =
      row.opcode == "SBLR_LITERAL" || row.opcode == "SBLR_PARAMETER" ||
      row.opcode == "SBLR_VARIABLE";
  if (expression_leaf_root) {
    bool operand_refusal = false;
    for (const auto& diagnostic : envelope_validation.diagnostics) {
      if (diagnostic.code == "SBLR.OPERAND_INVALID") {
        operand_refusal = true;
        break;
      }
    }
    Require(!envelope_validation.ok && operand_refusal,
            EvidenceMessage(row, "envelope",
                            "expression leaf was admitted as a top-level root"));
  } else {
    Require(envelope_validation.ok,
            EvidenceMessage(row, "envelope",
                            "base engine envelope rejected valid canonical opcode"));
  }

  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr,
          EvidenceMessage(row, "opcode_validation", "canonical operation disappeared"));
  const auto opcode_validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  if (!entry->executor_evidence_accepted) {
    Require(!opcode_validation.ok &&
                opcode_validation.diagnostic_id ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
                opcode_validation.diagnostic_id ==
                    entry->missing_executor_evidence_diagnostic,
            EvidenceMessage(row, "opcode_validation",
                            "missing executor evidence did not fail closed"));
    return;
  }
  Require(opcode_validation.ok,
          EvidenceMessage(row, "opcode_validation", "canonical opcode validation failed"));
  Require(opcode_validation.entry == entry &&
              opcode_validation.entry->opcode == row.opcode,
          EvidenceMessage(row, "opcode_validation", "validation did not bind canonical entry"));

  auto missing_security = envelope;
  missing_security.requires_security_context = false;
  const auto security_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_security);
  Require(!security_validation.ok &&
              security_validation.diagnostic_id == "SB_DIAG_SBLR_SECURITY_CONTEXT_REQUIRED",
          EvidenceMessage(row, "security_refusal", "missing security context was not refused"));

  if (row.requires_transaction_context) {
    auto missing_transaction = envelope;
    missing_transaction.requires_transaction_context = false;
    const auto transaction_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_transaction);
    Require(!transaction_validation.ok &&
                transaction_validation.diagnostic_id == "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED",
            EvidenceMessage(row, "transaction_refusal", "missing transaction context was not refused"));
  }

  if (row.requires_cluster_authority) {
    auto missing_cluster = envelope;
    missing_cluster.requires_cluster_authority = false;
    const auto cluster_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_cluster);
    Require(!cluster_validation.ok &&
                cluster_validation.diagnostic_id == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE",
            EvidenceMessage(row, "cluster_refusal", "missing cluster authority was not refused"));
  }
}

void RequireCanonicalOperationPreservation() {
  struct CanonicalOperationRow {
    std::string_view operation_id;
    std::string_view opcode;
  };
  constexpr std::array<CanonicalOperationRow, 2> canonical_operations{{
      {"dml.insert_rows", "SBLR_DML_INSERT_ROWS"},
      {"dml.update_rows", "SBLR_DML_UPDATE_ROWS"},
  }};

  for (const auto& canonical : canonical_operations) {
    const auto* entry = sblr::LookupSblrOperation(canonical.operation_id);
    Require(entry != nullptr, "existing operation canonical disappeared from opcode registry");
    Require(entry->opcode == canonical.opcode, "existing operation canonical opcode changed");
    Require(sblr::LookupSblrOpcode(canonical.opcode) == entry,
            "existing opcode canonical lookup no longer resolves to its route");

    auto envelope = sblr::MakeSblrEnvelope(std::string(canonical.operation_id),
                                           std::string(canonical.opcode),
                                           "opcode-canonical-preservation");
    envelope.requires_security_context = entry->requires_security_context;
    envelope.requires_transaction_context = entry->requires_transaction_context;
    envelope.requires_cluster_authority = entry->requires_cluster_authority;
    const auto validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    if (entry->executor_evidence_accepted) {
      Require(validation.ok, "canonical operation validation regressed");
    } else {
      Require(entry->executor_evidence_required && !validation.ok &&
                  validation.diagnostic_id ==
                      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
              "canonical operation did not fail closed on missing executor evidence");
    }
  }
}

void RequireUnknownAndMismatchDiagnostics() {
  Require(sblr::LookupSblrOpcode("SBLR_NOT_A_REAL_OPCODE") == nullptr,
          "unknown opcode unexpectedly resolved");

  auto unknown_operation = sblr::MakeSblrEnvelope("not.a.real.operation",
                                                  "SBLR_INSERT",
                                                  "opcode-unknown-operation");
  unknown_operation.requires_transaction_context = true;
  const auto unknown_operation_validation =
      sblr::ValidateSblrOpcodeForEnvelope(unknown_operation);
  Require(!unknown_operation_validation.ok &&
              unknown_operation_validation.diagnostic_id == "SB_DIAG_SBLR_UNKNOWN_OPERATION",
          "unknown operation diagnostic changed");

  auto mismatch = sblr::MakeSblrEnvelope("engine.op.insert",
                                         "SBLR_NOT_A_REAL_OPCODE",
                                         "opcode-mismatch");
  mismatch.requires_transaction_context = true;
  const auto mismatch_validation = sblr::ValidateSblrOpcodeForEnvelope(mismatch);
  Require(!mismatch_validation.ok &&
              mismatch_validation.diagnostic_id == "SB_DIAG_SBLR_OPCODE_MISMATCH",
          "opcode mismatch diagnostic changed");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical = "opcode-registry-database";
  context.session_uuid.canonical = "opcode-registry-session";
  context.principal_uuid.canonical = "opcode-registry-principal";
  context.local_transaction_id = 1;
  return context;
}

void RequireClusterWriteAdmissionBoundary() {
  const OpcodeRow* row = nullptr;
  for (const auto& candidate : kRows) {
    if (candidate.operation_id == "engine.op.cluster_write_admission") {
      row = &candidate;
      break;
    }
  }
  Require(row != nullptr, "cluster write-admission row missing");

  auto envelope = EnvelopeFor(*row);
  const auto validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(validation.ok, "cluster write-admission canonical validation failed");

  sblr::SblrDispatchRequest request;
  request.context = EngineContext();
  request.envelope = envelope;

  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated,
          "cluster write-admission envelope was not engine-validated");
  Require(result.accepted,
          "cluster write-admission did not reach dispatch acceptance");
  Require(result.dispatched_to_api,
          "cluster write-admission did not route to the provider boundary");

  if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(result.api_result.ok,
            "configured cluster provider rejected cluster write-admission");
  } else {
    Require(!result.api_result.ok,
            "no-cluster provider executed cluster write-admission in core");
    Require(result.api_result.cluster_authority_required,
            "no-cluster provider did not preserve cluster authority requirement");
    Require(HasApiDiagnostic(result.api_result,
                             cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster provider omitted API cluster-disabled diagnostic");
    Require(HasDispatchDiagnostic(result,
                                  cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster provider omitted dispatch cluster-disabled diagnostic");
  }
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 33> kForbidden = {
      "sbsql_sblr_final_cleanup",
      "final_cleanup",
      "B001Exact",
      "IsB001",
      "b001_",
      "_b001",
      "B002Exact",
      "IsB002",
      "b002_",
      "_b002",
      "B003Exact",
      "IsB003",
      "b003_",
      "_b003",
      "B007Exact",
      "IsB007",
      "b007_",
      "_b007",
      "B008Exact",
      "IsB008",
      "b008_",
      "_b008",
      "AUDIT-0",
      "AUDIT-1",
      "AUDIT-2",
      "AUDIT-3",
      "AUDIT-4",
      "AUDIT-5",
      "AUDIT-6",
      "AUDIT-7",
      "AUDIT-8",
      "AUDIT-9",
      "SSFC-",
  };
  const std::filesystem::path source_root =
      std::filesystem::path(SCRATCHBIRD_PROJECT_SOURCE_DIR) / "src";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    for (const auto token : kForbidden) {
      Require(!Contains(text, token),
              std::string("production source contains forbidden batch token ") +
                  std::string(token) + " in " + entry.path().string());
    }
  }
}

}  // namespace

int main() {
  RequireProductionSourceIntegrity();
  for (const auto& row : kRows) {
    RequireCanonicalLookupAndMetadata(row);
    RequireCanonicalEnvelopeValidation(row);
  }
  RequireCanonicalOperationPreservation();
  RequireUnknownAndMismatchDiagnostics();
  RequireClusterWriteAdmissionBoundary();
  std::cout << "sbsql_sblr_final_cleanup_b008_opcode_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
