// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

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
    {"cluster.replication.cdc.ack", "SBLR_REPL_CDC_ACK", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, true},
    {"cluster.replication.two_phase.prewrite", "SBLR_REPL_2PC_PREWRITE", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.replication.two_phase.commit", "SBLR_REPL_2PC_COMMIT", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.replication.two_phase.cleanup", "SBLR_REPL_2PC_CLEANUP", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.replication.two_phase.resolve_lock", "SBLR_REPL_2PC_RESOLVE_LOCK", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.replication.two_phase.pessimistic_lock", "SBLR_REPL_2PC_PESSIMISTIC_LOCK", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.replication.two_phase.pessimistic_rollback", "SBLR_REPL_2PC_PESSIMISTIC_ROLLBACK", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.replication.two_phase.heartbeat", "SBLR_REPL_2PC_HEARTBEAT", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.replication.two_phase.check_status", "SBLR_REPL_2PC_CHECK_STATUS", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"graph.traverse", "SBLR_GRAPH_TRAVERSE", "graph-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"graph.optional_match", "SBLR_GRAPH_OPTIONAL_MATCH", "graph-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"graph.create", "SBLR_GRAPH_CREATE", "graph-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"graph.merge", "SBLR_GRAPH_MERGE", "graph-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"graph.set", "SBLR_GRAPH_SET", "graph-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"graph.remove", "SBLR_GRAPH_REMOVE", "graph-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"graph.delete", "SBLR_GRAPH_DELETE", "graph-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"graph.detach_delete", "SBLR_GRAPH_DETACH_DELETE", "graph-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"vector.search", "SBLR_VECTOR_SEARCH", "vector-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"vector.hybrid_search", "SBLR_VECTOR_HYBRID_SEARCH", "vector-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"vector.similarity", "SBLR_VECTOR_SIMILARITY", "vector-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"vector.index.load", "SBLR_VECTOR_INDEX_LOAD", "vector-execution", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"vector.index.release", "SBLR_VECTOR_INDEX_RELEASE", "vector-execution", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"fulltext.score", "SBLR_FULLTEXT_SCORE", "fulltext-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"fulltext.phrase_score", "SBLR_FULLTEXT_PHRASE_SCORE", "fulltext-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"fulltext.multi_field_score", "SBLR_FULLTEXT_MULTI_FIELD_SCORE", "fulltext-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"fulltext.regex_match", "SBLR_FULLTEXT_REGEX_MATCH", "fulltext-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"fulltext.wildcard_match", "SBLR_FULLTEXT_WILDCARD_MATCH", "fulltext-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"fulltext.prefix_match", "SBLR_FULLTEXT_PREFIX_MATCH", "fulltext-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"fulltext.analyzer_apply", "SBLR_FULLTEXT_ANALYZER_APPLY", "fulltext-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"diagnostic.refusal", "SBLR_DIAGNOSTIC_REFUSAL", "diagnostic-control", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"diagnostic.reset", "SBLR_DIAGNOSTIC_RESET", "diagnostic-control", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"descriptor.transform", "SBLR_DESCRIPTOR_TRANSFORM", "diagnostic-control", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"relational.join", "SBLR_JOIN", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.set_operation", "SBLR_SET_OPERATION", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.cte", "SBLR_CTE", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.recursive_cte", "SBLR_RECURSIVE_CTE", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.pivot", "SBLR_PIVOT", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.unpivot", "SBLR_UNPIVOT", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.values", "SBLR_VALUES", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.match_recognize", "SBLR_MATCH_RECOGNIZE", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"relational.table_function_invoke", "SBLR_TABLE_FUNCTION_INVOKE", "relational-plan-node", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"kv.structured.read", "SBLR_KV_STRUCTURED_READ", "kv-structured-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"kv.structured.mutate", "SBLR_KV_STRUCTURED_MUTATE", "kv-structured-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"kv.structured.scan", "SBLR_KV_STRUCTURED_SCAN", "kv-structured-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"kv.structured.stream_read", "SBLR_KV_STRUCTURED_STREAM_READ", "kv-structured-execution", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"kv.structured.stream_append", "SBLR_KV_STRUCTURED_STREAM_APPEND", "kv-structured-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"kv.structured.timeseries", "SBLR_KV_STRUCTURED_TIMESERIES", "kv-structured-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"system.config.set", "SBLR_SYSTEM_CONFIG_SET", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"system.config.get", "SBLR_SYSTEM_CONFIG_GET", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"system.config.reset", "SBLR_SYSTEM_CONFIG_RESET", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
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
  envelope.requires_cluster_authority = row.requires_cluster_authority;
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
  Require(by_opcode->requires_cluster_authority == row.requires_cluster_authority,
          EvidenceMessage(row, "authority", "cluster authority requirement drifted"));
  Require(by_opcode->cluster_private == row.requires_cluster_authority,
          EvidenceMessage(row, "authority", "cluster-private marker drifted"));

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

  if (row.requires_cluster_authority) {
    auto missing_cluster = envelope;
    missing_cluster.requires_cluster_authority = false;
    const auto cluster_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_cluster);
    Require(!cluster_validation.ok &&
                cluster_validation.diagnostic_id == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE",
            EvidenceMessage(row, "cluster_refusal", "missing cluster authority was not refused"));
  }
}

void RequireClusterProviderBoundary(const OpcodeRow& row) {
  if (!row.requires_cluster_authority) return;
  sblr::SblrDispatchRequest request;
  request.context.security_context_present = true;
  request.context.database_uuid.canonical = "opcode-registry-database";
  request.context.session_uuid.canonical = "opcode-registry-session";
  request.context.principal_uuid.canonical = "opcode-registry-principal";
  request.context.local_transaction_id = 1;
  request.envelope = EnvelopeFor(row);

  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated,
          EvidenceMessage(row, "cluster_boundary", "envelope did not validate"));
  Require(result.accepted,
          EvidenceMessage(row, "cluster_boundary", "dispatch did not accept boundary route"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "cluster_boundary", "route did not reach provider boundary"));

  if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(result.api_result.ok,
            EvidenceMessage(row, "cluster_boundary", "configured provider rejected route"));
  } else {
    Require(!result.api_result.ok,
            EvidenceMessage(row, "cluster_boundary", "no-cluster provider executed core cluster route"));
    Require(result.api_result.cluster_authority_required,
            EvidenceMessage(row, "cluster_boundary", "cluster authority requirement was not preserved"));
    Require(HasApiDiagnostic(result.api_result,
                             cluster_provider::kClusterSupportNotEnabledCode),
            EvidenceMessage(row, "cluster_boundary", "API diagnostic missing"));
    Require(HasDispatchDiagnostic(result,
                                  cluster_provider::kClusterSupportNotEnabledCode),
            EvidenceMessage(row, "cluster_boundary", "dispatch diagnostic missing"));
  }
}

void RequireAliasPreservation() {
  struct AliasRow {
    std::string_view operation_id;
    std::string_view opcode;
  };
  constexpr std::array<AliasRow, 10> aliases{{
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
                                                  "SBLR_GRAPH_TRAVERSE",
                                                  "opcode-unknown-operation");
  unknown_operation.requires_transaction_context = true;
  const auto unknown_operation_validation =
      sblr::ValidateSblrOpcodeForEnvelope(unknown_operation);
  Require(!unknown_operation_validation.ok &&
              unknown_operation_validation.diagnostic_id == "SB_DIAG_SBLR_UNKNOWN_OPERATION",
          "unknown operation diagnostic changed");

  auto mismatch = sblr::MakeSblrEnvelope("graph.traverse",
                                         "SBLR_NOT_A_REAL_OPCODE",
                                         "opcode-mismatch");
  mismatch.requires_transaction_context = true;
  const auto mismatch_validation = sblr::ValidateSblrOpcodeForEnvelope(mismatch);
  Require(!mismatch_validation.ok &&
              mismatch_validation.diagnostic_id == "SB_DIAG_SBLR_OPCODE_MISMATCH",
          "opcode mismatch diagnostic changed");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 49> kForbidden = {
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
    RequireClusterProviderBoundary(row);
  }
  RequireAliasPreservation();
  RequireUnknownAndMismatchDiagnostics();
  std::cout << "sbsql_sblr_final_cleanup_b012_opcode_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
