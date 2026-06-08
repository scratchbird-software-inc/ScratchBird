// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "backup_archive/backup_archive_api.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "public_release_authz_fixture.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770800000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

struct DatabaseFixture {
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
};

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

bool HasEvidence(const api::EngineApiResult& result,
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

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_backup_update_coverage_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(), "public_backup_update_coverage_gate");
  return Expect(configured.ok(),
                "PCR-087 memory manager should configure") &&
         Expect(configured.fixture_mode,
                "PCR-087 memory manager should use fixture mode");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(TypedUuid typed_uuid) {
  return uuid::UuidToString(typed_uuid.value);
}

std::string UuidText(UuidKind kind, u64 offset) {
  return UuidText(MakeUuid(kind, offset));
}

DatabaseFixture CreateDatabaseFixture(const std::filesystem::path& path,
                                      u64 seed) {
  DatabaseFixture fixture;
  fixture.path = path;
  fixture.database_uuid = MakeUuid(UuidKind::database, seed);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, seed + 1);
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + seed;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  return fixture;
}

api::EngineRequestContext Context(const DatabaseFixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = UuidText(fixture.database_uuid);
  context.principal_uuid.canonical = UuidText(UuidKind::principal, 20);
  context.session_uuid.canonical = UuidText(UuidKind::object, 21);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext BackupContext(const DatabaseFixture& fixture,
                                        std::string request_id) {
  api::EngineRequestContext context = Context(fixture, std::move(request_id));
  context.trace_tags.push_back("right:BACKUP_CREATE");
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_CREATE");
  return context;
}

api::EngineRequestContext RestoreContext(const DatabaseFixture& fixture,
                                         std::string request_id,
                                         u64 local_transaction_id) {
  api::EngineRequestContext context = Context(fixture, std::move(request_id));
  context.trace_tags.push_back("right:BACKUP_RESTORE");
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_RESTORE");
  context.local_transaction_id = local_transaction_id;
  context.transaction_uuid.canonical = UuidText(UuidKind::transaction,
                                                100 + local_transaction_id);
  return context;
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
      if (field.first == key && field.second == value) {
        return true;
      }
    }
  }
  return false;
}

api::EnginePackageDeltaStreamResult PackageSegment(
    const api::EngineRequestContext& context,
    const api::EngineUuid& backup_uuid,
    const std::string& filespace_uuid,
    const std::filesystem::path& manifest_path,
    u64 start_transaction_id,
    u64 end_transaction_id,
    const std::string& idempotency_key) {
  api::EnginePackageDeltaStreamRequest request;
  request.context = context;
  request.option_envelopes = {
      "target_uri:" + manifest_path.string(),
      "source_backup_uuid:" + backup_uuid.canonical,
      "filespace_uuid:" + filespace_uuid,
      "start_transaction_id:" + std::to_string(start_transaction_id),
      "end_transaction_id:" + std::to_string(end_transaction_id),
      "coverage_start_transaction_id:" + std::to_string(start_transaction_id),
      "coverage_end_transaction_id:" + std::to_string(end_transaction_id),
      "idempotency_key:" + idempotency_key};
  return api::EnginePackageDeltaStream(request);
}

api::EngineUpdateBackupFromVerifiedCoverageRequest UpdateRequest(
    const api::EngineRequestContext& context,
    const api::EngineUuid& backup_uuid,
    const std::string& filespace_uuid,
    const std::filesystem::path& base_manifest,
    const std::filesystem::path& update_manifest,
    u64 last_covered_transaction_id,
    u64 target_end_transaction_id,
    const std::string& idempotency_key) {
  api::EngineUpdateBackupFromVerifiedCoverageRequest request;
  request.context = context;
  request.backup_uuid = backup_uuid;
  request.last_covered_transaction_id = last_covered_transaction_id;
  request.target_end_transaction_id = target_end_transaction_id;
  request.base_manifest_uri = base_manifest.string();
  request.update_manifest_uri = update_manifest.string();
  request.filespace_uuid = filespace_uuid;
  request.idempotency_key = idempotency_key;
  return request;
}

bool BackupUpdateCoverageProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  const auto source =
      CreateDatabaseFixture(work_dir / "pcr087-source.sbdb", 1);
  const auto target =
      CreateDatabaseFixture(work_dir / "pcr087-target.sbdb", 100);
  const auto source_context = BackupContext(source, "pcr087-source");
  const std::string filespace_uuid = UuidText(source.filespace_uuid);
  const auto base_manifest = work_dir / "base.manifest";
  const auto update_manifest = work_dir / "update.delta";
  const auto gap_manifest = work_dir / "gap.delta";
  const auto mismatch_manifest = work_dir / "mismatch.delta";

  api::EngineStartLogicalBackupRequest base_request;
  base_request.context = source_context;
  base_request.option_envelopes = {
      "target_uri:" + base_manifest.string(),
      "filespace_uuid:" + filespace_uuid};
  const auto base = api::EngineStartLogicalBackup(base_request);
  ok = ExpectApiOk(base, "PCR-087 base logical backup should pass") && ok;
  const u64 base_end = base.snapshot_visible_through_local_transaction_id;
  const u64 target_end = base_end + 2;
  const u64 first_update_tx = base_end + 1;
  const u64 second_update_tx = base_end + 2;

  auto update_request = UpdateRequest(source_context,
                                      base.backup_uuid,
                                      filespace_uuid,
                                      base_manifest,
                                      update_manifest,
                                      base_end,
                                      target_end,
                                      "pcr087-right-key");
  const auto update =
      api::EngineUpdateBackupFromVerifiedCoverage(update_request);
  ok = ExpectApiOk(update, "PCR-087 backup update should pass") && ok;
  ok = Expect(update.historical_coverage_reused,
              "PCR-087 backup update should reuse historical coverage") &&
       ok;
  ok = Expect(update.coverage_contiguous,
              "PCR-087 backup update coverage should be contiguous") &&
       ok;
  ok = Expect(update.packaged_start_transaction_id == first_update_tx &&
                  update.packaged_end_transaction_id == target_end,
              "PCR-087 backup update should package only uncovered tail") &&
       ok;
  ok = Expect(!update.transaction_finality_authority &&
                  !update.write_after_recovery_authority &&
                  !update.cluster_recovery_authority,
              "PCR-087 backup update must remain non-authoritative evidence") &&
       ok;
  ok = Expect(HasEvidence(update,
                          "finality_source",
                          "local_mga_transaction_inventory"),
              "PCR-087 backup update finality source evidence missing") &&
       ok;
  ok = Expect(ManifestHasField(ReadFile(update_manifest),
                               "META",
                               "idempotency_key",
                               "pcr087-right-key"),
              "PCR-087 update manifest idempotency key missing") &&
       ok;

  auto idempotent_request = UpdateRequest(source_context,
                                          base.backup_uuid,
                                          filespace_uuid,
                                          base_manifest,
                                          work_dir / "noop.delta",
                                          target_end,
                                          target_end,
                                          "pcr087-right-key");
  idempotent_request.verified_segment_manifest_uris.push_back(
      update_manifest.string());
  const auto idempotent =
      api::EngineUpdateBackupFromVerifiedCoverage(idempotent_request);
  ok = ExpectApiOk(idempotent,
                   "PCR-087 idempotent backup update should pass") &&
       ok;
  ok = Expect(idempotent.idempotent_noop &&
                  idempotent.reused_segment_count == 1 &&
                  idempotent.packaged_start_transaction_id == 0,
              "PCR-087 idempotent update should reuse existing segment only") &&
       ok;

  const auto gap_segment = PackageSegment(source_context,
                                          base.backup_uuid,
                                          filespace_uuid,
                                          gap_manifest,
                                          second_update_tx,
                                          second_update_tx,
                                          "pcr087-right-key");
  ok = ExpectApiOk(gap_segment, "PCR-087 gap segment package should pass") &&
       ok;
  auto gap_request = UpdateRequest(source_context,
                                   base.backup_uuid,
                                   filespace_uuid,
                                   base_manifest,
                                   work_dir / "gap-update.delta",
                                   second_update_tx,
                                   second_update_tx,
                                   "pcr087-right-key");
  gap_request.verified_segment_manifest_uris.push_back(gap_manifest.string());
  const auto gap = api::EngineUpdateBackupFromVerifiedCoverage(gap_request);
  ok = Expect(!gap.ok && gap.gap_detected,
              "PCR-087 gap should fail closed") &&
       ok;
  ok = Expect(HasDiagnosticDetail(gap, "BACKUP_UPDATE_COVERAGE_GAP"),
              "PCR-087 gap diagnostic mismatch") &&
       ok;

  auto overlap_request = UpdateRequest(source_context,
                                       base.backup_uuid,
                                       filespace_uuid,
                                       base_manifest,
                                       work_dir / "overlap-update.delta",
                                       target_end,
                                       target_end,
                                       "pcr087-right-key");
  overlap_request.verified_segment_manifest_uris.push_back(
      update_manifest.string());
  overlap_request.verified_segment_manifest_uris.push_back(
      gap_manifest.string());
  const auto overlap =
      api::EngineUpdateBackupFromVerifiedCoverage(overlap_request);
  ok = Expect(!overlap.ok && overlap.overlap_detected,
              "PCR-087 overlap should fail closed") &&
       ok;
  ok = Expect(HasDiagnosticDetail(overlap,
                                  "BACKUP_UPDATE_COVERAGE_OVERLAP"),
              "PCR-087 overlap diagnostic mismatch") &&
       ok;

  const auto mismatch_segment = PackageSegment(source_context,
                                               base.backup_uuid,
                                               filespace_uuid,
                                               mismatch_manifest,
                                               first_update_tx,
                                               first_update_tx,
                                               "pcr087-wrong-key");
  ok = ExpectApiOk(mismatch_segment,
                   "PCR-087 mismatch segment package should pass") &&
       ok;
  auto mismatch_request = UpdateRequest(source_context,
                                        base.backup_uuid,
                                        filespace_uuid,
                                        base_manifest,
                                        work_dir / "mismatch-update.delta",
                                        first_update_tx,
                                        first_update_tx,
                                        "pcr087-right-key");
  mismatch_request.verified_segment_manifest_uris.push_back(
      mismatch_manifest.string());
  const auto mismatch =
      api::EngineUpdateBackupFromVerifiedCoverage(mismatch_request);
  ok = Expect(!mismatch.ok,
              "PCR-087 mismatched idempotency key should fail closed") &&
       ok;
  ok = Expect(HasDiagnosticDetail(
                  mismatch,
                  "BACKUP_UPDATE_IDEMPOTENCY_KEY_MISMATCH"),
              "PCR-087 idempotency mismatch diagnostic mismatch") &&
       ok;

  api::EngineApplyDeltaStreamRequest pitr_request;
  pitr_request.context = RestoreContext(target, "pcr087-pitr-ok", 101);
  pitr_request.option_envelopes = {
      "source_manifest_uri:" + update_manifest.string(),
      "expected_previous_end_transaction_id:" + std::to_string(base_end),
      "pitr_target_transaction_id:" + std::to_string(target_end),
      "restore_inspection_open:true",
      "recovery_classification_verified:true"};
  const auto pitr = api::EngineApplyDeltaStream(pitr_request);
  ok = ExpectApiOk(pitr,
                   "PCR-087 PITR target inside coverage should pass") &&
       ok;
  ok = Expect(pitr.pitr_target_transaction_id == target_end,
              "PCR-087 PITR target transaction mismatch") &&
       ok;

  api::EngineApplyDeltaStreamRequest pitr_gap_request = pitr_request;
  pitr_gap_request.context =
      RestoreContext(target, "pcr087-pitr-outside", 102);
  pitr_gap_request.option_envelopes = {
      "source_manifest_uri:" + update_manifest.string(),
      "expected_previous_end_transaction_id:" + std::to_string(base_end),
      "pitr_target_transaction_id:" + std::to_string(target_end + 1),
      "restore_inspection_open:true",
      "recovery_classification_verified:true"};
  const auto pitr_gap = api::EngineApplyDeltaStream(pitr_gap_request);
  ok = Expect(!pitr_gap.ok,
              "PCR-087 PITR target outside coverage should fail") &&
       ok;
  ok = Expect(HasDiagnosticDetail(pitr_gap,
                                  "PITR_TARGET_OUTSIDE_COVERAGE:transaction"),
              "PCR-087 PITR gap diagnostic mismatch") &&
       ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_backup_update_coverage_gate <work-dir>\n";
    return 2;
  }
  if (!ConfigureMemoryFixture()) {
    return 1;
  }
  const std::filesystem::path work_dir = argv[1];
  std::filesystem::create_directories(work_dir);
  return BackupUpdateCoverageProof(work_dir) ? 0 : 1;
}
