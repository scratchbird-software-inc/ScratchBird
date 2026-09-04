// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_to_sbsql.hpp"
#include "uuid.hpp"
#include "canonical_sblr_admission_test_helper.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct UuidFactory {
  std::uint64_t base_millis = NowMillis();

  std::string Text(std::uint64_t salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(UuidKind::object, base_millis + salt);
    Require(generated.ok(), "CDP-025 UUID generation failed");
    return uuid::UuidToString(generated.value.value);
  }
};

struct OperationRow {
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view executor_id;
  bool executor_evidence_accepted;
};

constexpr std::array<OperationRow, 8> kRepresentativeOperations{{
    {"engine.op.txn_commit", "SBLR_TXN_COMMIT", "engine.op.txn_commit", false},
    {"engine.op.insert", "SBLR_INSERT", "engine.op.insert", false},
    {"engine.op.update", "SBLR_UPDATE", "engine.op.update", false},
    {"engine.op.delete", "SBLR_DELETE", "engine.op.delete", false},
    {"engine.op.bulk_import_stream", "SBLR_BULK_IMPORT_STREAM",
     "engine.op.bulk_import_stream", true},
    {"query.execute", "SBLR_QUERY_EXECUTE", "engine.op.query_execute", true},
    {"engine.op.query_explain", "SBLR_QUERY_EXPLAIN",
     "engine.op.query_explain", false},
    {"engine.op.cluster_write_admission", "SBLR_CLUSTER_WRITE_ADMISSION",
     "engine.op.cluster_write_admission", true},
}};

sblr::SblrOperand Operand(std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.type = "text";
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

const sblr::SblrOperand* FindOperand(const sblr::SblrOperationEnvelope& envelope,
                                     std::string_view name) {
  for (const auto& operand : envelope.operands) {
    if (operand.name == name) return &operand;
  }
  return nullptr;
}

std::string CanonicalOperandText(const sblr::SblrOperationEnvelope& envelope,
                                 std::string_view name) {
  const auto* operand = FindOperand(envelope, name);
  Require(operand != nullptr &&
              operand->value_kind == sblr::SblrValueKind::literal_typed &&
              operand->value_body.size() >= 24,
          "CDP-025 canonical typed operand is missing");
  const auto size = scratchbird::engine::SblrReadU64(
      operand->value_body.data() + 16);
  Require(size == operand->value_body.size() - 24,
          "CDP-025 canonical typed operand length drifted");
  return std::string(
      reinterpret_cast<const char*>(operand->value_body.data() + 24),
      static_cast<std::size_t>(size));
}

sblr::SblrSourceSymbolArtifact Symbol(std::string symbol_kind,
                                      std::string stable_key,
                                      std::string resolved_uuid,
                                      std::string render_hint,
                                      std::string scope) {
  sblr::SblrSourceSymbolArtifact symbol;
  symbol.symbol_kind = std::move(symbol_kind);
  symbol.stable_key = std::move(stable_key);
  symbol.resolved_uuid = std::move(resolved_uuid);
  symbol.render_hint = std::move(render_hint);
  symbol.scope = std::move(scope);
  symbol.source_hash = "sha256:cdp025-source-symbol";
  symbol.authoritative = false;
  symbol.contains_sql_text = false;
  return symbol;
}

sblr::SblrOperationRenderHint RenderHint(std::string stable_key,
                                         std::string value) {
  sblr::SblrOperationRenderHint hint;
  hint.hint_kind = "operation";
  hint.stable_key = std::move(stable_key);
  hint.value = std::move(value);
  hint.authoritative = false;
  hint.contains_sql_text = false;
  return hint;
}

void AttachSourcePolicy(sblr::SblrOperationEnvelope* envelope,
                        const UuidFactory& uuids,
                        std::uint64_t salt) {
  envelope->source_artifact_map.policy_status =
      "non_authoritative_render_metadata";
  envelope->source_artifact_map.source_identity =
      "cdp025-source-map:" + uuids.Text(salt);
  envelope->source_artifact_map.source_hash = "sha256:cdp025-source-map";
  envelope->source_artifact_map.render_metadata_only = true;
  envelope->source_artifact_map.contains_sql_text = false;
  envelope->source_artifact_map.raw_sql_text_authoritative = false;
}

sblr::SblrOperationEnvelope CanonicalEnvelope(const sblr::SblrOpcodeEntry& entry,
                                              const UuidFactory& uuids,
                                              std::uint64_t salt) {
  auto envelope = sblr::MakeSblrEnvelope(entry.operation_id,
                                         entry.opcode,
                                         "CDP-025-SBLR-REGISTRY-ROUND-TRIP");
  envelope.parser_package_uuid = uuids.Text(salt);
  envelope.registry_snapshot_uuid = uuids.Text(salt + 1);
  envelope.requires_security_context = entry.requires_security_context;
  envelope.requires_transaction_context = entry.requires_transaction_context;
  envelope.requires_cluster_authority = entry.requires_cluster_authority;
  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));
}

void RequireEnvelopeDiagnostic(const sblr::SblrOperationEnvelope& envelope,
                               std::string_view expected_code,
                               std::string_view message) {
  const auto validation = sblr::ValidateSblrEnvelope(envelope);
  Require(!validation.ok, message);
  for (const auto& diagnostic : validation.diagnostics) {
    if (diagnostic.code == expected_code) return;
  }
  std::cerr << "expected diagnostic=" << expected_code << '\n';
  if (!validation.diagnostics.empty()) {
    std::cerr << "actual diagnostic=" << validation.diagnostics.front().code
              << '\n';
  }
  Fail(message);
}

void RequireConversionDiagnostic(const sblr::SblrToSbsqlResult& result,
                                 std::string_view expected_code,
                                 std::string_view message) {
  Require(!result.ok, message);
  Require(!result.diagnostics.empty(), message);
  if (result.diagnostics.front().code != expected_code) {
    std::cerr << "expected diagnostic=" << expected_code
              << " actual diagnostic=" << result.diagnostics.front().code
              << '\n';
  }
  Require(result.diagnostics.front().code == expected_code, message);
}

void CheckRegistryAdmission() {
  const UuidFactory uuids;
  for (std::size_t index = 0; index < kRepresentativeOperations.size(); ++index) {
    const auto& row = kRepresentativeOperations[index];
    const auto* by_operation = sblr::LookupSblrOperation(row.operation_id);
    Require(by_operation != nullptr, "CDP-025 operation lookup failed");
    const auto* by_opcode = sblr::LookupSblrOpcode(row.opcode);
    Require(by_opcode != nullptr, "CDP-025 opcode lookup failed");
    Require(by_operation == by_opcode,
            "CDP-025 operation/opcode lookup did not bind same registry entry");
    Require(by_operation->operation_id == row.operation_id,
            "CDP-025 registry operation id drifted");
    Require(by_operation->opcode == row.opcode,
            "CDP-025 registry opcode drifted");
    Require(by_operation->executor_id == row.executor_id,
            "CDP-025 Core executor binding drifted");
    Require(by_operation->executor_evidence_required,
            "CDP-025 Core executor evidence gate is not required");
    Require(by_operation->executor_evidence_accepted ==
                row.executor_evidence_accepted,
            "CDP-025 executor evidence publication drifted");

    const auto envelope =
        CanonicalEnvelope(*by_operation, uuids, 100 + index * 10);
    const auto accepted = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    if (!row.executor_evidence_accepted) {
      Require(!accepted.ok &&
                  accepted.diagnostic_id ==
                      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
                  accepted.diagnostic_id ==
                      by_operation->missing_executor_evidence_diagnostic,
              "CDP-025 missing executor evidence did not fail closed");
      continue;
    }
    Require(accepted.ok,
            "CDP-025 evidence-backed SBLR opcode validation rejected required contexts");
    Require(accepted.entry == by_operation,
            "CDP-025 validation did not return canonical registry entry");

    if (by_operation->requires_security_context) {
      auto missing_security = envelope;
      missing_security.requires_security_context = false;
      const auto refused =
          sblr::ValidateSblrOpcodeForEnvelope(missing_security);
      Require(!refused.ok &&
                  refused.diagnostic_id ==
                      "SB_DIAG_SBLR_SECURITY_CONTEXT_REQUIRED",
              "CDP-025 missing security context diagnostic drifted");
    }

    if (by_operation->requires_transaction_context) {
      auto missing_transaction = envelope;
      missing_transaction.requires_transaction_context = false;
      const auto refused =
          sblr::ValidateSblrOpcodeForEnvelope(missing_transaction);
      Require(!refused.ok &&
                  refused.diagnostic_id ==
                      "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED",
              "CDP-025 missing transaction context diagnostic drifted");
    }

    if (by_operation->category == sblr::SblrOpcodeCategory::cluster ||
        by_operation->requires_cluster_authority) {
      Require(by_operation->support == sblr::SblrOpcodeSupport::cluster_refusal,
              "CDP-025 cluster operation is not marked cluster-refusal");
      Require(by_operation->requires_cluster_authority,
              "CDP-025 cluster operation does not require cluster authority");
      Require(by_operation->cluster_private,
              "CDP-025 cluster operation is not cluster-private");
      Require(by_operation->refusal_diagnostic ==
                  "SB_DIAG_CLUSTER_TXN_UNAVAILABLE",
              "CDP-025 cluster refusal diagnostic drifted");

      auto missing_cluster_authority = envelope;
      missing_cluster_authority.requires_cluster_authority = false;
      const auto refused =
          sblr::ValidateSblrOpcodeForEnvelope(missing_cluster_authority);
      Require(!refused.ok &&
                  refused.diagnostic_id == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE",
              "CDP-025 missing cluster authority did not fail closed");
    }
  }
}

sblr::SblrOperationEnvelope BuildDmlInsertEnvelope(const UuidFactory& uuids) {
  const auto* entry = sblr::LookupSblrOperation("engine.op.insert");
  Require(entry != nullptr, "CDP-025 engine.op.insert registry entry missing");
  auto envelope = CanonicalEnvelope(*entry, uuids, 300);
  const std::string table_uuid = uuids.Text(310);
  const std::string descriptor_uuid = uuids.Text(311);
  const std::string column_uuid = uuids.Text(312);
  const std::string parameter_uuid = uuids.Text(313);
  envelope.operands.push_back(
      Operand("sbsql_render_family", "source_preserving_dml_single_row_v1"));
  envelope.operands.push_back(
      Operand("authority_descriptor_uuid", descriptor_uuid));
  envelope.operands.push_back(Operand("target_object_uuid", table_uuid));
  envelope.operands.push_back(Operand("value_column_symbol_key", "col.note"));
  envelope.operands.push_back(Operand("value_column_uuid", column_uuid));
  envelope.operands.push_back(Operand("value_parameter_symbol_key", "param.note"));
  envelope.operands.push_back(Operand("value_parameter_uuid", parameter_uuid));

  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));
}

void AttachDmlSourceArtifacts(sblr::SblrOperationEnvelope* envelope,
                              const UuidFactory& uuids) {
  AttachSourcePolicy(envelope, uuids, 320);
  envelope->source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.cdp025_table",
             CanonicalOperandText(*envelope, "target_object_uuid"),
             "cdp025_table", "dml.target"));
  envelope->source_artifact_map.symbols.push_back(
      Symbol("column_alias", "col.note",
             CanonicalOperandText(*envelope, "value_column_uuid"), "note",
             "dml.value"));
  envelope->source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.note",
             CanonicalOperandText(*envelope, "value_parameter_uuid"), ":p_note",
             "dml.parameter"));
}

void CheckEnvelopeRoundTripAndValidation() {
  const UuidFactory uuids;
  auto envelope = BuildDmlInsertEnvelope(uuids);
  const auto encoded = sblr::EncodeSblrEnvelope(envelope);
  const auto decoded = sblr::DecodeSblrEnvelope(encoded);
  Require(decoded.ok, "CDP-025 encoded SBLR envelope did not decode");
  Require(decoded.envelope.operation_id == envelope.operation_id,
          "CDP-025 operation id did not survive SBLR round trip");
  Require(decoded.envelope.opcode == envelope.opcode,
          "CDP-025 opcode did not survive SBLR round trip");
  Require(decoded.envelope.requires_security_context &&
              !decoded.envelope.requires_transaction_context &&
              !decoded.envelope.requires_cluster_authority,
          "CDP-025 decoded SBOP authority defaults drifted");
  Require(decoded.envelope.parser_package_uuid == envelope.parser_package_uuid,
          "CDP-025 parser package UUID did not survive SBLR round trip");
  Require(decoded.envelope.registry_snapshot_uuid ==
              envelope.registry_snapshot_uuid,
          "CDP-025 registry snapshot UUID did not survive SBLR round trip");
  Require(decoded.envelope.operands.size() == envelope.operands.size(),
          "CDP-025 operands did not survive SBLR round trip");
  Require(FindOperand(decoded.envelope, "target_object_uuid") != nullptr,
          "CDP-025 target object UUID operand did not survive round trip");
  Require(decoded.envelope.source_artifact_map.symbols.empty() &&
              decoded.envelope.source_artifact_map.operation_render_hints.empty(),
          "CDP-025 SBOP round trip acquired SBEE-owned source artifacts");

  const auto* insert_entry =
      sblr::LookupSblrOperation("engine.op.insert");
  Require(insert_entry != nullptr,
          "CDP-025 engine.op.insert registry entry disappeared");
  auto admission_envelope = decoded.envelope;
  admission_envelope.requires_security_context =
      insert_entry->requires_security_context;
  admission_envelope.requires_transaction_context =
      insert_entry->requires_transaction_context;
  admission_envelope.requires_cluster_authority =
      insert_entry->requires_cluster_authority;
  const auto opcode_validation =
      sblr::ValidateSblrOpcodeForEnvelope(admission_envelope);
  Require(insert_entry->executor_evidence_required &&
              !insert_entry->executor_evidence_accepted &&
              !opcode_validation.ok &&
              opcode_validation.diagnostic_id ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "CDP-025 decoded envelope did not fail closed on missing executor evidence");

  auto major_drift = envelope;
  major_drift.envelope_major = sblr::kEngineSblrEnvelopeMajor + 1;
  RequireEnvelopeDiagnostic(major_drift,
                            "SBLR.OPERATION.VERSION_INVALID",
                            "CDP-025 major-version drift was not rejected");

  auto sql_text = envelope;
  sql_text.contains_sql_text = true;
  RequireEnvelopeDiagnostic(sql_text,
                            "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY",
                            "CDP-025 SQL text envelope drift was not rejected");

  auto source_sql_text = envelope;
  AttachDmlSourceArtifacts(&source_sql_text, uuids);
  source_sql_text.source_artifact_map.contains_sql_text = true;
  RequireEnvelopeDiagnostic(source_sql_text,
                            "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY",
                            "CDP-025 in-band source artifacts were not rejected from SBOP");

  auto source_authority = envelope;
  AttachDmlSourceArtifacts(&source_authority, uuids);
  source_authority.source_artifact_map.raw_sql_text_authoritative = true;
  RequireEnvelopeDiagnostic(source_authority,
                            "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY",
                            "CDP-025 source authority was not rejected from SBOP");
}

void CheckSblrToSbsqlConversion() {
  const UuidFactory uuids;
  auto envelope = BuildDmlInsertEnvelope(uuids);
  AttachDmlSourceArtifacts(&envelope, uuids);
  Require(envelope.source_artifact_map.policy_status ==
              "non_authoritative_render_metadata",
          "CDP-025 conversion source policy is not non-authoritative");
  Require(envelope.source_artifact_map.render_metadata_only,
          "CDP-025 conversion source metadata is not render-only");
  Require(!envelope.source_artifact_map.raw_sql_text_authoritative,
          "CDP-025 conversion source metadata claims authority");
  Require(!envelope.source_artifact_map.contains_sql_text,
          "CDP-025 conversion source metadata carries SQL text");

  const sblr::SblrToSbsqlOptions options{.source_preserving = true};
  const auto rendered = sblr::RenderSblrEnvelopeToSbsql(envelope, options);
  if (!rendered.ok) {
    for (const auto& diagnostic : rendered.diagnostics) {
      std::cerr << "conversion diagnostic=" << diagnostic.code << ':'
                << diagnostic.message << '\n';
    }
  }
  Require(rendered.ok,
          "CDP-025 source-preserving SBLR-to-SBsql conversion failed");
  Require(rendered.sbsql_text ==
              "INSERT INTO cdp025_table (note) VALUES (:p_note);",
          "CDP-025 source-preserving DML render drifted");

  auto missing_source = envelope;
  missing_source.source_artifact_map = {};
  RequireConversionDiagnostic(
      sblr::RenderSblrEnvelopeToSbsql(missing_source, options),
      "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
      "CDP-025 missing source metadata was not refused");

  const auto* cluster_entry =
      sblr::LookupSblrOperation("engine.op.cluster_write_admission");
  Require(cluster_entry != nullptr,
          "CDP-025 engine.op.cluster_write_admission registry entry missing");
  auto cluster_envelope = CanonicalEnvelope(*cluster_entry, uuids, 400);
  AttachSourcePolicy(&cluster_envelope, uuids, 410);
  cluster_envelope.source_artifact_map.operation_render_hints.push_back(
      RenderHint("engine.op.cluster_write_admission",
                 "cluster_write_admission"));
  RequireConversionDiagnostic(
      sblr::RenderSblrEnvelopeToSbsql(cluster_envelope, options),
      "SB_SBLR_TO_SBSQL_NON_CORE_OPERATION_REFUSED",
      "CDP-025 cluster/non-core conversion was not refused");
}

}  // namespace

int main() {
  // Search key: CDP_SBLR_REGISTRY_ROUND_TRIP_GATE
  CheckRegistryAdmission();
  CheckEnvelopeRoundTripAndValidation();
  CheckSblrToSbsqlConversion();
  return EXIT_SUCCESS;
}
