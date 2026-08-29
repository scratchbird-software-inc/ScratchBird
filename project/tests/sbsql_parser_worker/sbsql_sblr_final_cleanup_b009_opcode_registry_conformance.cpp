// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_sblr_admission_test_helper.hpp"
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

namespace sblr = scratchbird::engine::sblr;

struct OpcodeRow {
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view family;
  sblr::SblrOpcodeCategory category;
  sblr::SblrOpcodeTransactionEffect transaction_effect;
  sblr::SblrOpcodeSecurityClass security_class;
};

constexpr std::array<OpcodeRow, 50> kRows{{
    {"engine.op.window", "SBLR_WINDOW", "result-shape", sblr::SblrOpcodeCategory::result_shape, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized},
    {"engine.op.return_result_set", "SBLR_RETURN_RESULT_SET", "result-shape", sblr::SblrOpcodeCategory::result_shape, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized},
    {"engine.op.ddl_create_table", "SBLR_DDL_CREATE_TABLE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_table", "SBLR_DDL_ALTER_TABLE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_table", "SBLR_DDL_DROP_TABLE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_index", "SBLR_DDL_DROP_INDEX", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_domain", "SBLR_DDL_DROP_DOMAIN", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_schema", "SBLR_DDL_DROP_SCHEMA", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_schema", "SBLR_DDL_ALTER_SCHEMA", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_index", "SBLR_DDL_ALTER_INDEX", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_domain", "SBLR_DDL_ALTER_DOMAIN", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_view", "SBLR_DDL_ALTER_VIEW", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_view", "SBLR_DDL_DROP_VIEW", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_trigger", "SBLR_DDL_ALTER_TRIGGER", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_trigger", "SBLR_DDL_DROP_TRIGGER", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_procedure", "SBLR_DDL_ALTER_PROCEDURE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_procedure", "SBLR_DDL_DROP_PROCEDURE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_function", "SBLR_DDL_ALTER_FUNCTION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_function", "SBLR_DDL_DROP_FUNCTION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_create_package", "SBLR_DDL_CREATE_PACKAGE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_package", "SBLR_DDL_ALTER_PACKAGE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_package", "SBLR_DDL_DROP_PACKAGE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_sequence", "SBLR_DDL_ALTER_SEQUENCE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_sequence", "SBLR_DDL_DROP_SEQUENCE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_create_materialized_view", "SBLR_DDL_CREATE_MATERIALIZED_VIEW", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_refresh_materialized_view", "SBLR_DDL_REFRESH_MATERIALIZED_VIEW", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_materialized_view", "SBLR_DDL_DROP_MATERIALIZED_VIEW", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_create_type", "SBLR_DDL_CREATE_TYPE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_alter_type", "SBLR_DDL_ALTER_TYPE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_type", "SBLR_DDL_DROP_TYPE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_rename_object", "SBLR_DDL_RENAME_OBJECT", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_synonym", "SBLR_DDL_DROP_SYNONYM", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_create_foreign_table", "SBLR_DDL_CREATE_FOREIGN_TABLE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_foreign_table", "SBLR_DDL_DROP_FOREIGN_TABLE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_create_fdw", "SBLR_DDL_CREATE_FDW", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.ddl_drop_fdw", "SBLR_DDL_DROP_FDW", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_create_user", "SBLR_SEC_CREATE_USER", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_alter_user", "SBLR_SEC_ALTER_USER", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_create_role", "SBLR_SEC_CREATE_ROLE", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_grant", "SBLR_SEC_GRANT", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_revoke", "SBLR_SEC_REVOKE", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_create_group_mapping", "SBLR_SEC_CREATE_GROUP_MAPPING", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_alter_policy", "SBLR_SEC_ALTER_POLICY", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_drop_user", "SBLR_SEC_DROP_USER", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_alter_role", "SBLR_SEC_ALTER_ROLE", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_drop_role", "SBLR_SEC_DROP_ROLE", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_create_policy", "SBLR_SEC_CREATE_POLICY", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_drop_policy", "SBLR_SEC_DROP_POLICY", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized},
    {"engine.op.sec_authenticate", "SBLR_SEC_AUTHENTICATE", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::authenticated},
    {"engine.op.sec_deauthenticate", "SBLR_SEC_DEAUTHENTICATE", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::authenticated},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
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

sblr::SblrOperationEnvelope EnvelopeFor(const OpcodeRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "opcode-registry-conformance");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      envelope);
}

void RequireCanonicalLookupAndMetadata(const OpcodeRow& row) {
  const auto* by_opcode = sblr::LookupSblrOpcode(row.opcode);
  Require(by_opcode != nullptr,
          EvidenceMessage(row, "lookup", "canonical opcode is not registered"));
  Require(by_opcode->operation_id == row.operation_id,
          EvidenceMessage(row, "lookup", "operation id drifted"));
  Require(by_opcode->family == row.family,
          EvidenceMessage(row, "metadata", "spec family drifted"));
  Require(by_opcode->category == row.category,
          EvidenceMessage(row, "metadata", "category drifted"));
  Require(by_opcode->support == sblr::SblrOpcodeSupport::implemented,
          EvidenceMessage(row, "metadata", "support state drifted"));
  Require(by_opcode->transaction_effect == row.transaction_effect,
          EvidenceMessage(row, "metadata", "transaction effect drifted"));
  Require(by_opcode->security_class == row.security_class,
          EvidenceMessage(row, "metadata", "security class drifted"));
  Require(by_opcode->requires_security_context,
          EvidenceMessage(row, "authority", "security context is not required"));
  Require(by_opcode->requires_transaction_context,
          EvidenceMessage(row, "authority", "transaction context is not required"));
  Require(!by_opcode->requires_cluster_authority,
          EvidenceMessage(row, "authority", "cluster authority leaked into non-cluster row"));
  Require(!by_opcode->cluster_private,
          EvidenceMessage(row, "authority", "cluster-private flag leaked into non-cluster row"));

  Require(by_opcode->executor_id == row.operation_id,
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
  Require(envelope_validation.ok,
          EvidenceMessage(row, "envelope", "base engine envelope rejected valid canonical opcode"));

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

  auto missing_transaction = envelope;
  missing_transaction.requires_transaction_context = false;
  const auto transaction_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_transaction);
  Require(!transaction_validation.ok &&
              transaction_validation.diagnostic_id == "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED",
          EvidenceMessage(row, "transaction_refusal", "missing transaction context was not refused"));
}

void RequireCanonicalOperationPreservation() {
  struct CanonicalOperationRow {
    std::string_view operation_id;
    std::string_view opcode;
  };
  constexpr std::array<CanonicalOperationRow, 4> canonical_operations{{
      {"engine.op.ddl_create_table", "SBLR_DDL_CREATE_TABLE"},
      {"engine.op.ddl_create_view", "SBLR_DDL_CREATE_VIEW"},
      {"engine.op.ddl_create_sequence", "SBLR_DDL_CREATE_SEQUENCE"},
      {"security.grant_right", "SBLR_SECURITY_GRANT_RIGHT"},
  }};

  for (const auto& canonical : canonical_operations) {
    const auto* entry = sblr::LookupSblrOperation(canonical.operation_id);
    Require(entry != nullptr, "existing DDL/security canonical disappeared from opcode registry");
    Require(entry->opcode == canonical.opcode, "existing DDL/security canonical opcode changed");

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
                                                  "SBLR_WINDOW",
                                                  "opcode-unknown-operation");
  unknown_operation.requires_transaction_context = true;
  const auto unknown_operation_validation =
      sblr::ValidateSblrOpcodeForEnvelope(unknown_operation);
  Require(!unknown_operation_validation.ok &&
              unknown_operation_validation.diagnostic_id == "SB_DIAG_SBLR_UNKNOWN_OPERATION",
          "unknown operation diagnostic changed");

  auto mismatch = sblr::MakeSblrEnvelope("engine.op.window",
                                         "SBLR_NOT_A_REAL_OPCODE",
                                         "opcode-mismatch");
  mismatch.requires_transaction_context = true;
  const auto mismatch_validation = sblr::ValidateSblrOpcodeForEnvelope(mismatch);
  Require(!mismatch_validation.ok &&
              mismatch_validation.diagnostic_id == "SB_DIAG_SBLR_OPCODE_MISMATCH",
          "opcode mismatch diagnostic changed");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 37> kForbidden = {
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
      "B009Exact",
      "IsB009",
      "b009_",
      "_b009",
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
  std::cout << "sbsql_sblr_final_cleanup_b009_opcode_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
