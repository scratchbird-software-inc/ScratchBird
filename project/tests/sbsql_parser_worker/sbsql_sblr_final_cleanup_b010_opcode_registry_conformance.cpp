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

constexpr std::array<OpcodeRow, 53> kRows{{
    {"security.group_mapping.drop", "SBLR_SEC_DROP_GROUP_MAPPING", "security-management", sblr::SblrOpcodeCategory::security, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::security, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"filespace.create", "SBLR_FILESPACE_CREATE", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.preallocate", "SBLR_FILESPACE_PREALLOCATE", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.attach", "SBLR_FILESPACE_ATTACH", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.detach", "SBLR_FILESPACE_DETACH", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.move", "SBLR_FILESPACE_MOVE", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.promote", "SBLR_FILESPACE_PROMOTE", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.compact", "SBLR_FILESPACE_COMPACT", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.truncate", "SBLR_FILESPACE_TRUNCATE", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"filespace.drop", "SBLR_FILESPACE_DROP", "filespace-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"index.rebuild", "SBLR_INDEX_REBUILD", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"index.rebalance", "SBLR_INDEX_REBALANCE", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"index.verify", "SBLR_INDEX_VERIFY", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"index.validate", "SBLR_INDEX_VALIDATE", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"index.repair", "SBLR_INDEX_REPAIR", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"index.discard_unpublished", "SBLR_INDEX_DISCARD_UNPUBLISHED", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"index.gather_statistics", "SBLR_INDEX_GATHER_STATISTICS", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"index.cleanup_mga_versions", "SBLR_INDEX_CLEANUP_MGA_VERSIONS", "index-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"backup.start", "SBLR_BACKUP_START", "backup-archive-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"backup.finish", "SBLR_BACKUP_FINISH", "backup-archive-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"backup.restore", "SBLR_RESTORE_BACKUP", "backup-archive-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"archive.export", "SBLR_ARCHIVE_EXPORT", "backup-archive-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"archive.verify", "SBLR_ARCHIVE_VERIFY", "backup-archive-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"cluster.join", "SBLR_CLUSTER_JOIN", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.leave", "SBLR_CLUSTER_LEAVE", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.route_request", "SBLR_CLUSTER_ROUTE_REQUEST", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.publish_route", "SBLR_CLUSTER_PUBLISH_ROUTE", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.fence_node", "SBLR_CLUSTER_FENCE_NODE", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.reconcile_branch", "SBLR_CLUSTER_RECONCILE_BRANCH", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.publish_epoch", "SBLR_CLUSTER_PUBLISH_EPOCH", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"diagnostic.emit", "SBLR_EMIT_DIAGNOSTIC", "diagnostics-and-metrics", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::none, sblr::SblrOpcodeSecurityClass::authenticated, false, false},
    {"metrics.read", "SBLR_READ_METRICS", "diagnostics-and-metrics", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"metrics.reset", "SBLR_RESET_METRICS", "diagnostics-and-metrics", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"diagnostic.explain_operation", "SBLR_EXPLAIN_OPERATION", "diagnostics-and-metrics", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::authenticated, true, false},
    {"diagnostic.emit_audit_event", "SBLR_EMIT_AUDIT_EVENT", "diagnostics-and-metrics", sblr::SblrOpcodeCategory::observability, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::external_audit, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"management.envelope.operation", "SBLR_MGMT_OPERATION", "management-envelope", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"management.envelope.payload", "SBLR_MGMT_PAYLOAD", "management-envelope", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"management.envelope.result", "SBLR_MGMT_RESULT", "management-envelope", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"management.envelope.progress", "SBLR_MGMT_PROGRESS", "management-envelope", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"management.envelope.diagnostic", "SBLR_MGMT_DIAGNOSTIC", "management-envelope", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"management.envelope.metric_snapshot_ref", "SBLR_MGMT_METRIC_SNAPSHOT_REF", "management-envelope", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.show_horizons", "SBLR_MGA_SHOW_HORIZONS", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.checkpoint", "SBLR_MGA_CHECKPOINT", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.sweep", "SBLR_MGA_SWEEP", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.cleanup_hot_versions", "SBLR_MGA_CLEANUP_HOT_VERSIONS", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.verify_archive_manifest", "SBLR_MGA_VERIFY_ARCHIVE_MANIFEST", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.verify_archive_reachability", "SBLR_MGA_VERIFY_ARCHIVE_REACHABILITY", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.verify_archive_decryptability", "SBLR_MGA_VERIFY_ARCHIVE_DECRYPTABILITY", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.verify_backup_coverage", "SBLR_MGA_VERIFY_BACKUP_COVERAGE", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"mga.prove_stream_truncation", "SBLR_MGA_PROVE_STREAM_TRUNCATION", "mga-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"cluster.mga_txn.inspect", "SBLR_MGA_CLUSTER_TXN_INSPECT", "mga-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::read, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.mga_txn.resolve", "SBLR_MGA_CLUSTER_TXN_RESOLVE", "mga-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
    {"cluster.mga_txn.retry_decision", "SBLR_MGA_CLUSTER_TXN_RETRY_DECISION", "mga-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::cluster_write, sblr::SblrOpcodeSecurityClass::cluster_authorized, true, true},
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
  constexpr std::array<AliasRow, 6> aliases{{
      {"storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION"},
      {"artifact.export_catalog", "SBLR_ARTIFACT_EXPORT_CATALOG"},
      {"artifact.import_catalog", "SBLR_ARTIFACT_IMPORT_CATALOG"},
      {"management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME"},
      {"management.control_runtime", "SBLR_MANAGEMENT_CONTROL_RUNTIME"},
      {"observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS"},
  }};

  for (const auto& alias : aliases) {
    const auto* entry = sblr::LookupSblrOperation(alias.operation_id);
    Require(entry != nullptr, "existing management/storage/archive alias disappeared");
    Require(entry->opcode == alias.opcode, "existing management/storage/archive alias opcode changed");

    auto envelope = sblr::MakeSblrEnvelope(std::string(alias.operation_id),
                                           std::string(alias.opcode),
                                           "opcode-alias-preservation");
    envelope.requires_security_context = entry->requires_security_context;
    envelope.requires_transaction_context = entry->requires_transaction_context;
    envelope.requires_cluster_authority = entry->requires_cluster_authority;
    const auto validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    Require(validation.ok, "existing management/storage/archive alias validation regressed");
  }
}

void RequireUnknownAndMismatchDiagnostics() {
  Require(sblr::LookupSblrOpcode("SBLR_NOT_A_REAL_OPCODE") == nullptr,
          "unknown opcode unexpectedly resolved");

  auto unknown_operation = sblr::MakeSblrEnvelope("not.a.real.operation",
                                                  "SBLR_FILESPACE_CREATE",
                                                  "opcode-unknown-operation");
  unknown_operation.requires_transaction_context = true;
  const auto unknown_operation_validation =
      sblr::ValidateSblrOpcodeForEnvelope(unknown_operation);
  Require(!unknown_operation_validation.ok &&
              unknown_operation_validation.diagnostic_id == "SB_DIAG_SBLR_UNKNOWN_OPERATION",
          "unknown operation diagnostic changed");

  auto mismatch = sblr::MakeSblrEnvelope("filespace.create",
                                         "SBLR_NOT_A_REAL_OPCODE",
                                         "opcode-mismatch");
  mismatch.requires_transaction_context = true;
  const auto mismatch_validation = sblr::ValidateSblrOpcodeForEnvelope(mismatch);
  Require(!mismatch_validation.ok &&
              mismatch_validation.diagnostic_id == "SB_DIAG_SBLR_OPCODE_MISMATCH",
          "opcode mismatch diagnostic changed");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 41> kForbidden = {
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
  std::cout << "sbsql_sblr_final_cleanup_b010_opcode_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
