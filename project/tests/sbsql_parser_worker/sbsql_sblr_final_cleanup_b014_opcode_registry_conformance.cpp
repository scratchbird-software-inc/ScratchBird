// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

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
  sblr::SblrOpcodeSupport support;
  sblr::SblrOpcodeTransactionEffect transaction_effect;
  sblr::SblrOpcodeSecurityClass security_class;
  bool requires_transaction_context;
};

constexpr std::array<OpcodeRow, 10> kRows{{
    {"acceleration.llvm.invalidate", "SBLR_ACCEL_LLVM_INVALIDATE", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true},
    {"acceleration.gpu.policy_set", "SBLR_ACCEL_GPU_POLICY_SET", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true},
    {"acceleration.gpu.compile", "SBLR_ACCEL_GPU_COMPILE", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true},
    {"acceleration.gpu.inspect", "SBLR_ACCEL_GPU_INSPECT", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true},
    {"acceleration.gpu.invalidate", "SBLR_ACCEL_GPU_INVALIDATE", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true},
    {"versioned.verifiable_history.prove", "SBLR_VERIFIABLE_HISTORY_PROVE", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true},
    {"versioned.verify_proof_descriptor", "SBLR_VERIFY_PROOF_DESCRIPTOR", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true},
    {"versioned.merge", "SBLR_VERSIONED_MERGE", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true},
    {"versioned.hash_read", "SBLR_VERSIONED_HASH_READ", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true},
    {"versioned.status_read", "SBLR_VERSIONED_STATUS_READ", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true},
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
  envelope.requires_transaction_context = row.requires_transaction_context;
  envelope.requires_cluster_authority = false;
  return envelope;
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
  Require(!by_opcode->requires_cluster_authority,
          EvidenceMessage(row, "authority", "non-cluster opcode requires cluster authority"));
  Require(!by_opcode->cluster_private,
          EvidenceMessage(row, "authority", "non-cluster opcode is marked cluster-private"));

  const auto* by_operation = sblr::LookupSblrOperation(row.operation_id);
  Require(by_operation == by_opcode,
          EvidenceMessage(row, "lookup", "operation lookup did not return canonical entry"));
}

void RequireCanonicalEnvelopeValidation(const OpcodeRow& row) {
  const auto envelope = EnvelopeFor(row);
  const auto envelope_validation = sblr::ValidateSblrEnvelope(envelope);
  Require(envelope_validation.ok,
          EvidenceMessage(row, "envelope", "base engine envelope rejected valid canonical opcode"));

  const auto opcode_validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(opcode_validation.ok,
          EvidenceMessage(row, "opcode_validation", "canonical opcode validation failed"));
  Require(opcode_validation.entry != nullptr &&
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

  auto unexpected_cluster = envelope;
  unexpected_cluster.requires_cluster_authority = true;
  const auto cluster_validation = sblr::ValidateSblrOpcodeForEnvelope(unexpected_cluster);
  Require(cluster_validation.ok,
          EvidenceMessage(row, "cluster_flag", "non-cluster opcode rejected harmless cluster flag"));
}

void RequireAliasPreservation() {
  struct AliasRow {
    std::string_view operation_id;
    std::string_view opcode;
  };
  constexpr std::array<AliasRow, 12> aliases{{
      {"storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION"},
      {"artifact.export_catalog", "SBLR_ARTIFACT_EXPORT_CATALOG"},
      {"artifact.import_catalog", "SBLR_ARTIFACT_IMPORT_CATALOG"},
      {"management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME"},
      {"management.control_runtime", "SBLR_MANAGEMENT_CONTROL_RUNTIME"},
      {"observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS"},
      {"mga.show_horizons", "SBLR_MGA_SHOW_HORIZONS"},
      {"database.attach", "SBLR_DATABASE_ATTACH"},
      {"cluster.replication.consumer.subscribe", "SBLR_REPL_CONSUMER_SUBSCRIBE"},
      {"graph.traverse", "SBLR_GRAPH_TRAVERSE"},
      {"system.config.set", "SBLR_SYSTEM_CONFIG_SET"},
      {"acceleration.llvm.inspect", "SBLR_ACCEL_LLVM_INSPECT"},
  }};

  for (const auto& alias : aliases) {
    const auto* entry = sblr::LookupSblrOperation(alias.operation_id);
    Require(entry != nullptr, "existing canonical registry operation disappeared");
    Require(entry->opcode == alias.opcode, "existing canonical registry opcode changed");

    auto envelope = sblr::MakeSblrEnvelope(std::string(alias.operation_id),
                                           std::string(alias.opcode),
                                           "opcode-alias-preservation");
    envelope.requires_security_context = entry->requires_security_context;
    envelope.requires_transaction_context = entry->requires_transaction_context;
    envelope.requires_cluster_authority = entry->requires_cluster_authority;
    const auto validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    Require(validation.ok, "existing canonical registry validation regressed");
  }
}

void RequireUnknownAndMismatchDiagnostics() {
  Require(sblr::LookupSblrOpcode("SBLR_NOT_A_REAL_OPCODE") == nullptr,
          "unknown opcode unexpectedly resolved");

  auto unknown_operation = sblr::MakeSblrEnvelope("not.a.real.operation",
                                                  "SBLR_ACCEL_LLVM_INVALIDATE",
                                                  "opcode-unknown-operation");
  unknown_operation.requires_transaction_context = true;
  const auto unknown_operation_validation =
      sblr::ValidateSblrOpcodeForEnvelope(unknown_operation);
  Require(!unknown_operation_validation.ok &&
              unknown_operation_validation.diagnostic_id == "SB_DIAG_SBLR_UNKNOWN_OPERATION",
          "unknown operation diagnostic changed");

  auto mismatch = sblr::MakeSblrEnvelope("acceleration.llvm.invalidate",
                                         "SBLR_NOT_A_REAL_OPCODE",
                                         "opcode-mismatch");
  mismatch.requires_transaction_context = true;
  const auto mismatch_validation = sblr::ValidateSblrOpcodeForEnvelope(mismatch);
  Require(!mismatch_validation.ok &&
              mismatch_validation.diagnostic_id == "SB_DIAG_SBLR_OPCODE_MISMATCH",
          "opcode mismatch diagnostic changed");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 57> kForbidden = {
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
      "B010Exact",
      "IsB010",
      "b010_",
      "_b010",
      "B011Exact",
      "IsB011",
      "b011_",
      "_b011",
      "B012Exact",
      "IsB012",
      "b012_",
      "_b012",
      "B013Exact",
      "IsB013",
      "b013_",
      "_b013",
      "B014Exact",
      "IsB014",
      "b014_",
      "_b014",
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
  RequireAliasPreservation();
  RequireUnknownAndMismatchDiagnostics();
  std::cout << "sbsql_sblr_final_cleanup_b014_opcode_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
