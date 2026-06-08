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
    {"cluster.mga_txn.quarantine", "SBLR_MGA_CLUSTER_TXN_QUARANTINE", "mga-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"mga.show_archive_orphans", "SBLR_MGA_SHOW_ARCHIVE_ORPHANS", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.reclaim_archive_orphans", "SBLR_MGA_RECLAIM_ARCHIVE_ORPHANS", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.audit_legal_hold", "SBLR_MGA_AUDIT_LEGAL_HOLD", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"optimizer.plan.physical_property_requirement", "SBLR_PLAN_PHYSICAL_PROPERTY_REQUIREMENT", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"optimizer.plan.donor_compatibility_requirement", "SBLR_PLAN_DONOR_COMPATIBILITY_REQUIREMENT", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"optimizer.plan.mga_visibility_requirement", "SBLR_PLAN_MGA_VISIBILITY_REQUIREMENT", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"optimizer.plan.cache_dependency_identity", "SBLR_PLAN_CACHE_DEPENDENCY_IDENTITY", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"optimizer.explain_metadata", "SBLR_EXPLAIN_METADATA", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"optimizer.adaptive_feedback", "SBLR_ADAPTIVE_FEEDBACK", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::admin_authorized, false, false},
    {"cluster.optimizer.remote_operator", "SBLR_REMOTE_OPERATOR", "optimizer-plan", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"optimizer.vector_plan_node", "SBLR_VECTOR_PLAN_NODE", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"optimizer.text_plan_node", "SBLR_TEXT_PLAN_NODE", "optimizer-plan", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"event.channel.alter", "SBLR_EVENT_CHANNEL_ALTER", "event-notification", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::catalog_write, sblr::SblrOpcodeSecurityClass::event_admin, true, false},
    {"event.channel.drop", "SBLR_EVENT_CHANNEL_DROP", "event-notification", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::catalog_write, sblr::SblrOpcodeSecurityClass::event_admin, true, false},
    {"connection.open", "SBLR_CONN_OPEN", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"connection.close", "SBLR_CONN_CLOSE", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"connection.hello", "SBLR_CONN_HELLO", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::public_metadata, false, false},
    {"session.setting.set", "SBLR_SESSION_SETTING_SET", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"session.setting.get", "SBLR_SESSION_SETTING_GET", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"session.setting.reset", "SBLR_SESSION_SETTING_RESET", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"session.default_qualifier.set", "SBLR_SESSION_DEFAULT_QUALIFIER_SET", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"session.role.switch", "SBLR_SESSION_ROLE_SWITCH", "session-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"session.discard", "SBLR_SESSION_DISCARD", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"session.snapshot_handle", "SBLR_SESSION_SNAPSHOT_HANDLE", "session-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"statement.prepare", "SBLR_STMT_PREPARE", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"statement.execute", "SBLR_STMT_EXECUTE", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"statement.execute_direct", "SBLR_STMT_EXECUTE_DIRECT", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"statement.free", "SBLR_STMT_FREE", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"statement.cancel", "SBLR_STMT_CANCEL", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"parameter.bind", "SBLR_PARAMETER_BIND", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"result.page", "SBLR_RESULT_PAGE", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"query.execute", "SBLR_QUERY_EXECUTE", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"query.explain", "SBLR_QUERY_EXPLAIN", "statement-management", sblr::SblrOpcodeCategory::query, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"catalog.introspect", "SBLR_CATALOG_INTROSPECT", "catalog-introspect", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"catalog.name_resolve", "SBLR_NAME_RESOLVE", "catalog-introspect", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"optimizer.stats.read", "SBLR_OPTIMIZER_STATS_READ", "catalog-introspect", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"optimizer.stats.drop", "SBLR_OPTIMIZER_STATS_DROP", "catalog-introspect", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"parse.text", "SBLR_PARSE_TEXT", "catalog-introspect", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"catalog.epoch_check", "SBLR_CATALOG_EPOCH_CHECK", "catalog-introspect", sblr::SblrOpcodeCategory::catalog, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"database.attach", "SBLR_DATABASE_ATTACH", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"database.detach", "SBLR_DATABASE_DETACH", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"database.checkpoint", "SBLR_DATABASE_CHECKPOINT", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"database.vacuum", "SBLR_DATABASE_VACUUM", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"database.alter", "SBLR_DATABASE_ALTER", "database-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"cluster.replication.consumer.subscribe", "SBLR_REPL_CONSUMER_SUBSCRIBE", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, true},
    {"cluster.replication.consumer.resume", "SBLR_REPL_CONSUMER_RESUME", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, true},
    {"cluster.replication.consumer.pause", "SBLR_REPL_CONSUMER_PAUSE", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, true},
    {"cluster.replication.consumer.cancel", "SBLR_REPL_CONSUMER_CANCEL", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, true},
    {"cluster.replication.cdc.receive", "SBLR_REPL_CDC_RECEIVE", "replication-consumer", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, true},
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
  constexpr std::array<AliasRow, 8> aliases{{
      {"storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION"},
      {"artifact.export_catalog", "SBLR_ARTIFACT_EXPORT_CATALOG"},
      {"artifact.import_catalog", "SBLR_ARTIFACT_IMPORT_CATALOG"},
      {"management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME"},
      {"management.control_runtime", "SBLR_MANAGEMENT_CONTROL_RUNTIME"},
      {"observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS"},
      {"filespace.create", "SBLR_FILESPACE_CREATE"},
      {"mga.show_horizons", "SBLR_MGA_SHOW_HORIZONS"},
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
                                                  "SBLR_DATABASE_ATTACH",
                                                  "opcode-unknown-operation");
  unknown_operation.requires_transaction_context = true;
  const auto unknown_operation_validation =
      sblr::ValidateSblrOpcodeForEnvelope(unknown_operation);
  Require(!unknown_operation_validation.ok &&
              unknown_operation_validation.diagnostic_id == "SB_DIAG_SBLR_UNKNOWN_OPERATION",
          "unknown operation diagnostic changed");

  auto mismatch = sblr::MakeSblrEnvelope("database.attach",
                                         "SBLR_NOT_A_REAL_OPCODE",
                                         "opcode-mismatch");
  mismatch.requires_transaction_context = true;
  const auto mismatch_validation = sblr::ValidateSblrOpcodeForEnvelope(mismatch);
  Require(!mismatch_validation.ok &&
              mismatch_validation.diagnostic_id == "SB_DIAG_SBLR_OPCODE_MISMATCH",
          "opcode mismatch diagnostic changed");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 45> kForbidden = {
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
  std::cout << "sbsql_sblr_final_cleanup_b011_opcode_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
