// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/archive_manager.hpp"
#include "backup_archive/backup_archive_api.hpp"
#include "crud_support/crud_store.hpp"
#include "filespace_header.hpp"
#include "filespace_lifecycle.hpp"
#include "public_release_authz_fixture.hpp"
#include "transaction_cleanup.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace api = scratchbird::engine::internal_api;
namespace filespace = scratchbird::storage::filespace;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770500000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool ExpectApiOk(const api::EngineApiResult& result,
                 std::string_view message) {
  if (!result.ok) {
    std::cerr << message;
    if (!result.diagnostics.empty()) {
      std::cerr << ": " << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail;
    }
    std::cerr << '\n';
    return false;
  }
  return true;
}

bool HasDiagnosticDetail(const api::EngineApiResult& result,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(detail) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(UuidKind kind, u64 offset) {
  return uuid::UuidToString(MakeUuid(kind, offset).value);
}

txn::TransactionIdentity TransactionIdentity(u64 local_id) {
  txn::TransactionIdentity identity;
  identity.local_id = txn::MakeLocalTransactionId(local_id);
  identity.transaction_uuid = MakeUuid(UuidKind::transaction, 100 + local_id);
  identity.scope = txn::TransactionScope::local_node;
  return identity;
}

txn::RowVersionMetadata RetainedHistoryMetadata() {
  txn::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = MakeUuid(UuidKind::row, 20);
  metadata.identity.creator_transaction = TransactionIdentity(10);
  metadata.identity.version_sequence = 1;
  metadata.chain.next_version_sequence = 2;
  metadata.chain.next_version_uuid = MakeUuid(UuidKind::row, 21);
  metadata.successor_transaction_local_id = txn::MakeLocalTransactionId(11);
  metadata.state = txn::RowVersionState::committed;
  metadata.creator_transaction_state = txn::TransactionState::committed;
  metadata.payload_present = true;
  return metadata;
}

api::EngineRequestContext Context(const std::filesystem::path& work_dir,
                                  const TypedUuid& database_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr084-archive-before-reclaim";
  context.database_path = (work_dir / "pcr084.sbdb").string();
  context.database_uuid.canonical = uuid::UuidToString(database_uuid.value);
  context.principal_uuid.canonical = UuidText(UuidKind::principal, 30);
  context.session_uuid.canonical = UuidText(UuidKind::object, 31);
  context.transaction_uuid.canonical = UuidText(UuidKind::transaction, 32);
  context.local_transaction_id = 20;
  context.snapshot_visible_through_local_transaction_id = 20;
  context.security_context_present = true;
  context.trace_tags.push_back("group:OPS");
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_CREATE");
  return context;
}

filespace::PhysicalFilespaceHeader ArchiveHeader(TypedUuid database_uuid,
                                                 TypedUuid filespace_uuid) {
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = filespace_uuid;
  header.role = filespace::FilespaceRole::archive_history;
  header.state = filespace::FilespaceState::online;
  header.page_size = kPageSize;
  header.physical_filespace_id = 7;
  header.total_pages = 32;
  header.free_pages = 8;
  header.preallocated_pages = 4;
  header.allocation_root_page = 2;
  header.header_generation = 3;
  header.writer_identity_uuid = MakeUuid(UuidKind::object, 40);
  header.creation_operation_uuid = "pcr084-archive-history";
  return header;
}

filespace::FilespaceOperationRequest AttachArchiveRequest(
    const std::filesystem::path& archive_path,
    const filespace::PhysicalFilespaceHeader& header) {
  filespace::FilespaceOperationRequest attach;
  attach.operation = filespace::FilespaceOperation::attach_filespace;
  attach.database_uuid = header.database_uuid;
  attach.filespace_uuid = header.filespace_uuid;
  attach.path = archive_path.string();
  attach.role = filespace::FilespaceRole::archive_history;
  attach.page_size = header.page_size;
  attach.physical_filespace_id = header.physical_filespace_id;
  attach.total_pages = header.total_pages;
  attach.free_pages = header.free_pages;
  attach.preallocated_pages = header.preallocated_pages;
  attach.allocation_root_page = header.allocation_root_page;
  attach.header_generation = header.header_generation;
  attach.writer_identity_uuid = header.writer_identity_uuid;
  attach.reason = "pcr084-archive-filespace-attach";
  return attach;
}

api::EngineArchiveRetainedHistoryRecord RetainedHistoryRecord() {
  api::EngineArchiveRetainedHistoryRecord record;
  record.metadata = RetainedHistoryMetadata();
  record.table_uuid = UuidText(UuidKind::object, 50);
  record.payload_digest = "fnv1a64:retained-history-payload";
  record.retention_class = "history_archive";
  record.retention_policy_ref = "retention.history.local.v1";
  record.key_lineage_id = "pcr084-local-key-lineage";
  return record;
}

api::EngineArchiveRetainedHistoryBeforeReclaimRequest ArchiveRequest(
    const api::EngineRequestContext& context,
    const filespace::FilespaceDescriptor& descriptor,
    const std::filesystem::path& manifest_path) {
  api::EngineArchiveRetainedHistoryBeforeReclaimRequest request;
  request.context = context;
  request.archive_filespace = descriptor;
  request.retained_history.push_back(RetainedHistoryRecord());
  request.authoritative_cleanup_horizon_local_transaction_id = 20;
  request.engine_mga_authoritative = true;
  request.cleanup_horizon_authoritative = true;
  request.local_archive_filespace_header_verified = true;
  request.retention_policy_installed = true;
  request.max_row_versions_to_archive = 4;
  request.option_envelopes.push_back("manifest_uri:" + manifest_path.string());
  return request;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream data;
  data << in.rdbuf();
  return data.str();
}

bool ManifestHasField(const std::string& manifest,
                      std::string_view kind,
                      std::string_view key,
                      std::string_view value) {
  std::istringstream lines(manifest);
  std::string line;
  while (std::getline(lines, line)) {
    const auto separator = line.find('\t');
    if (separator == std::string::npos || line.substr(0, separator) != kind) {
      continue;
    }
    for (const auto& field : api::DecodeCrudPairs(line.substr(separator + 1))) {
      if (field.first == key && (value.empty() || field.second == value)) {
        return true;
      }
    }
  }
  return false;
}

bool ContainsEvidenceKind(const api::EngineApiResult& result,
                          std::string_view kind,
                          std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        evidence.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool ArchiveBeforeReclaimProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  const auto archive_path = work_dir / "archive-history.sbfs";
  const auto manifest_path = work_dir / "archive-before-reclaim.manifest";
  const auto header = ArchiveHeader(database_uuid, filespace_uuid);
  const auto created =
      filespace::CreatePhysicalFilespaceFile(archive_path.string(), header, true);
  ok = Expect(created.ok(), "PCR-084 archive filespace should be created") && ok;

  filespace::FilespaceRegistry registry;
  const auto attached =
      filespace::ApplyFilespaceOperation(&registry,
                                         AttachArchiveRequest(archive_path,
                                                              header));
  ok = Expect(attached.ok(), "PCR-084 archive filespace should attach") && ok;
  ok = Expect(attached.descriptor.archive_owner,
              "PCR-084 archive filespace should be archive owner") && ok;
  ok = Expect(attached.descriptor.read_only,
              "PCR-084 archive filespace should be read-only") && ok;
  ok = Expect(attached.evidence.durable_state_changed,
              "PCR-084 archive filespace attach should write evidence") && ok;

  auto rw = AttachArchiveRequest(archive_path, header);
  rw.operation = filespace::FilespaceOperation::set_read_write;
  const auto read_write =
      filespace::ApplyFilespaceOperation(&registry, rw);
  ok = Expect(!read_write.ok(),
              "PCR-084 archive filespace must refuse read-write transition") &&
       ok;
  ok = Expect(read_write.diagnostic.diagnostic_code ==
                  "SB-FILESPACE-LIFECYCLE-ARCHIVE-READ-WRITE-FORBIDDEN",
              "PCR-084 archive read-write refusal should be stable") &&
       ok;

  const auto context = Context(work_dir, database_uuid);
  const auto request = ArchiveRequest(context, attached.descriptor, manifest_path);
  const auto archived = api::EngineArchiveRetainedHistoryBeforeReclaim(request);
  ok = ExpectApiOk(archived,
                   "PCR-084 archive-before-reclaim should succeed") && ok;
  ok = Expect(archived.local_archive_filespace_bound,
              "PCR-084 archive proof should bind local archive filespace") &&
       ok;
  ok = Expect(archived.manifest_written && archived.manifest_verified,
              "PCR-084 archive manifest should be written and verified") &&
       ok;
  ok = Expect(archived.hot_reclaim_authorized,
              "PCR-084 hot reclaim should be authorized after archive proof") &&
       ok;
  ok = Expect(!archived.transaction_finality_authority,
              "PCR-084 archive proof must not become transaction finality authority") &&
       ok;
  ok = Expect(archived.archived_row_version_count == 1 &&
                  archived.movement_record_count == 1,
              "PCR-084 archive proof should record one movement") &&
       ok;
  ok = Expect(archived.reclaim_evidence_records.size() == 1,
              "PCR-084 archive proof should emit reclaim authorization evidence") &&
       ok;
  if (!archived.reclaim_evidence_records.empty()) {
    ok = Expect(archived.reclaim_evidence_records.front()
                    .stable_evidence_id.find("mga-archive-before-reclaim:") == 0,
                "PCR-084 reclaim evidence should be archive-bound") &&
         ok;
  }
  ok = Expect(ContainsEvidenceKind(archived,
                                   "finality_source",
                                   "local_mga_transaction_inventory"),
              "PCR-084 finality source evidence should remain MGA inventory") &&
       ok;
  const std::string manifest = ReadFile(manifest_path);
  ok = Expect(manifest.find("SBARCHIVERECLAIM1") != std::string::npos,
              "PCR-084 archive manifest magic should be present") &&
       ok;
  ok = Expect(ManifestHasField(manifest,
                               "META",
                               "archive_filespace_access_class",
                               "local_archive_history_read_only"),
              "PCR-084 archive manifest should include access class") &&
       ok;
  ok = Expect(ManifestHasField(manifest,
                               "MOVE",
                               "movement_record_checksum",
                               ""),
              "PCR-084 archive manifest should include movement checksum") &&
       ok;
  ok = Expect(ManifestHasField(manifest,
                               "META",
                               "key_lineage_id",
                               "pcr084-local-key-lineage") &&
                  ManifestHasField(manifest,
                                   "MOVE",
                                   "key_lineage_id",
                                   "pcr084-local-key-lineage"),
              "PCR-084 archive manifest should include key lineage") &&
       ok;
  ok = Expect(ManifestHasField(manifest,
                               "META",
                               "transaction_finality_authority",
                               "false"),
              "PCR-084 archive manifest should disclaim finality authority") &&
       ok;
  return ok;
}

bool FailClosedProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  const auto database_uuid = MakeUuid(UuidKind::database, 101);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 102);
  const auto archive_path = work_dir / "archive-fail-closed.sbfs";
  const auto header = ArchiveHeader(database_uuid, filespace_uuid);
  const auto created =
      filespace::CreatePhysicalFilespaceFile(archive_path.string(), header, true);
  ok = Expect(created.ok(), "PCR-084 fail-closed archive filespace should create") &&
       ok;
  filespace::FilespaceRegistry registry;
  const auto attached =
      filespace::ApplyFilespaceOperation(&registry,
                                         AttachArchiveRequest(archive_path,
                                                              header));
  ok = Expect(attached.ok(), "PCR-084 fail-closed archive filespace should attach") &&
       ok;
  const auto context = Context(work_dir, database_uuid);
  const auto manifest_path = work_dir / "archive-fail-closed.manifest";
  const auto request = ArchiveRequest(context, attached.descriptor, manifest_path);

  auto no_mga = request;
  no_mga.engine_mga_authoritative = false;
  const auto no_mga_result =
      api::EngineArchiveRetainedHistoryBeforeReclaim(no_mga);
  ok = Expect(!no_mga_result.ok &&
                  HasDiagnosticDetail(no_mga_result,
                                      "ARCHIVE_MGA_CLEANUP_AUTHORITY_REQUIRED"),
              "PCR-084 missing MGA authority should fail closed") &&
       ok;

  auto legal = request;
  legal.legal_hold_active = true;
  const auto legal_result = api::EngineArchiveRetainedHistoryBeforeReclaim(legal);
  ok = Expect(!legal_result.ok &&
                  HasDiagnosticDetail(legal_result, "ARCHIVE_LEGAL_HOLD_ACTIVE"),
              "PCR-084 legal hold should fail closed") &&
       ok;

  auto cluster = request;
  cluster.option_envelopes.push_back("scope:cluster");
  const auto cluster_result =
      api::EngineArchiveRetainedHistoryBeforeReclaim(cluster);
  ok = Expect(!cluster_result.ok && cluster_result.cluster_route_refused &&
                  cluster_result.cluster_authority_required,
              "PCR-084 cluster archive route should fail closed") &&
       ok;
  ok = Expect(HasDiagnosticDetail(cluster_result,
                                  "ARCHIVE_CLUSTER_PROVIDER_REQUIRED"),
              "PCR-084 cluster refusal diagnostic should be stable") &&
       ok;

  auto live_file = request;
  live_file.option_envelopes.push_back("live_file_shortcut:true");
  const auto live_file_result =
      api::EngineArchiveRetainedHistoryBeforeReclaim(live_file);
  ok = Expect(!live_file_result.ok &&
                  HasDiagnosticDetail(live_file_result,
                                      "ARCHIVE_LIVE_FILE_SHORTCUT_FORBIDDEN"),
              "PCR-084 live-file shortcut should fail closed") &&
       ok;

  auto no_header = request;
  no_header.local_archive_filespace_header_verified = false;
  const auto no_header_result =
      api::EngineArchiveRetainedHistoryBeforeReclaim(no_header);
  ok = Expect(!no_header_result.ok &&
                  HasDiagnosticDetail(no_header_result,
                                      "ARCHIVE_FILESPACE_PROOF_REQUIRED"),
              "PCR-084 missing archive filespace proof should fail closed") &&
       ok;

  auto no_retention = request;
  no_retention.retention_policy_installed = false;
  const auto no_retention_result =
      api::EngineArchiveRetainedHistoryBeforeReclaim(no_retention);
  ok = Expect(!no_retention_result.ok &&
                  HasDiagnosticDetail(no_retention_result,
                                      "ARCHIVE_RETENTION_POLICY_REQUIRED"),
              "PCR-084 missing retention policy should fail closed") &&
       ok;

  auto no_key_lineage = request;
  no_key_lineage.retained_history.front().key_lineage_id.clear();
  const auto no_key_lineage_result =
      api::EngineArchiveRetainedHistoryBeforeReclaim(no_key_lineage);
  ok = Expect(!no_key_lineage_result.ok &&
                  HasDiagnosticDetail(no_key_lineage_result,
                                      "ARCHIVE_KEY_LINEAGE_REQUIRED"),
              "PCR-084 missing key lineage should fail closed") &&
       ok;

  agents::ArchiveManagerRequest archive_agent;
  archive_agent.slice_uuid = UuidText(UuidKind::object, 200);
  archive_agent.database_uuid = context.database_uuid.canonical;
  archive_agent.principal_uuid = context.principal_uuid.canonical;
  archive_agent.mga_transaction_uuid = context.transaction_uuid.canonical;
  archive_agent.evidence_uuid = UuidText(UuidKind::object, 201);
  archive_agent.local_transaction_id = context.local_transaction_id;
  archive_agent.catalog_generation = 1;
  archive_agent.verify_requested = true;
  archive_agent.slice_available = true;
  archive_agent.metadata_authoritative = true;
  archive_agent.durable_catalog_bound = true;
  archive_agent.transaction_inventory_bound = true;
  archive_agent.intended_state_observed = true;
  const auto archive_agent_result =
      agents::EvaluateArchiveManagerRequest(archive_agent);
  ok = Expect(archive_agent_result.ok(),
              "PCR-084 archive manager should request verification locally") &&
       ok;
  ok = Expect(archive_agent_result.decision ==
                  agents::ArchiveManagerDecisionKind::request_verify_slice,
              "PCR-084 archive manager decision should be verify") &&
       ok;

  archive_agent.cluster_route_requested = true;
  const auto cluster_agent_result =
      agents::EvaluateArchiveManagerRequest(archive_agent);
  ok = Expect(!cluster_agent_result.ok() && cluster_agent_result.fail_closed,
              "PCR-084 archive manager cluster route should fail closed") &&
       ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() /
                     "public_archive_before_reclaim_gate";
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  std::filesystem::create_directories(work_dir);

  bool ok = true;
  ok = ArchiveBeforeReclaimProof(work_dir) && ok;
  ok = FailClosedProof(work_dir) && ok;
  return ok ? 0 : 1;
}
