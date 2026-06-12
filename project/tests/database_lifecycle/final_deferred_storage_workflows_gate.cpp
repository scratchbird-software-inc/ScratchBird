// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "final_deferred_storage_workflows.hpp"
#include "page_registry.hpp"
#include "reserved_page_family_body.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;
namespace filespace = scratchbird::storage::filespace;
namespace page = scratchbird::storage::page;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TempDir() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("sb_final_deferred_storage_" + std::to_string(CurrentUnixMillis()));
  std::filesystem::create_directories(path);
  return path;
}

std::vector<std::uint8_t> Bytes(std::string_view text) {
  return {text.begin(), text.end()};
}

void WriteFile(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  Require(output.good(), "failed to write fixture file");
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool ContainsEvidence(const filespace::FinalDeferredWorkflowResult& result,
                      std::string_view evidence) {
  for (const std::string& value : result.evidence) {
    if (value == evidence) {
      return true;
    }
  }
  return false;
}

void TestFilespaceRelocation(const std::filesystem::path& dir) {
  // MDF-001
  const auto source = dir / "mdf001/source.fs";
  const auto target = dir / "mdf001/target.fs";
  const auto journal = dir / "mdf001/relocate.journal";
  WriteFile(source, "filespace relocation body bytes");
  filespace::FilespaceRelocationRequest request;
  request.source_path = source;
  request.target_path = target;
  request.journal_path = journal;
  request.expected_digest = filespace::ComputeFinalDeferredFileDigest(source);
  const auto relocated = filespace::RelocateFilespaceBytes(request);
  Require(relocated.ok, "MDF-001 relocation did not commit");
  Require(relocated.digest_verified, "MDF-001 relocation digest was not verified");
  Require(ReadFile(target) == ReadFile(source), "MDF-001 target bytes diverged from source");
  Require(ContainsEvidence(relocated, "physical_byte_copy"), "MDF-001 did not expose byte-copy evidence");

  request.active_pin_count = 1;
  const auto pinned = filespace::RelocateFilespaceBytes(request);
  Require(!pinned.ok && pinned.diagnostic_code == "SB-FILESPACE-RELOCATION-PINS-ACTIVE",
          "MDF-001 active pin did not fail closed");
  request.active_pin_count = 0;

  const auto interrupted_target = dir / "mdf001/interrupted.fs";
  const auto interrupted_journal = dir / "mdf001/interrupted.journal";
  request.target_path = interrupted_target;
  request.journal_path = interrupted_journal;
  request.simulate_interruption_after_stage = true;
  const auto interrupted = filespace::RelocateFilespaceBytes(request);
  Require(!interrupted.ok && interrupted.recovery_required,
          "MDF-001 interruption did not require recovery");
  const auto recovered = filespace::RecoverFilespaceRelocation(interrupted_journal);
  Require(recovered.ok, "MDF-001 recovery did not complete staged relocation");
  Require(ReadFile(interrupted_target) == ReadFile(source),
          "MDF-001 recovered target bytes diverged from source");
}

void TestCorePageBodyRelocation(const std::filesystem::path& dir) {
  // MDF-002
  filespace::CorePageBody page1;
  page1.page_id = 42;
  page1.body = Bytes("core page body 42");
  page1.expected_digest = filespace::ComputeFinalDeferredBytesDigest(page1.body);
  filespace::CorePageBody page2;
  page2.page_id = 43;
  page2.body = Bytes("core page body 43");
  page2.expected_digest = filespace::ComputeFinalDeferredBytesDigest(page2.body);

  filespace::CorePageRelocationRequest request;
  request.pages = {page1, page2};
  request.target_directory = dir / "mdf002";
  const auto relocated = filespace::RelocateCorePageBodies(request);
  Require(relocated.ok, "MDF-002 page body relocation did not commit");
  Require(relocated.digest_verified, "MDF-002 page body relocation digest mismatch");
  Require(ReadFile(request.target_directory / "page_42.body") == "core page body 42",
          "MDF-002 page 42 body was not copied");

  request.root_authority_current = false;
  const auto stale = filespace::RelocateCorePageBodies(request);
  Require(!stale.ok && stale.diagnostic_code == "SB-CORE-PAGE-RELOCATION-STALE-ROOT-AUTHORITY",
          "MDF-002 stale root authority did not fail closed");
}

void TestSnapshotShadowPackage(const std::filesystem::path& dir) {
  // MDF-003
  const auto source_a = dir / "mdf003/source_a.page";
  const auto source_b = dir / "mdf003/source_b.page";
  WriteFile(source_a, "snapshot member a");
  WriteFile(source_b, "snapshot member b");

  filespace::SnapshotShadowPackageRequest request;
  request.source_files = {source_a, source_b};
  request.package_directory = dir / "mdf003/package";
  request.provider_name = "local-emulator";
  request.kms_manifest_id = "kms-manifest-1";
  const auto packaged = filespace::BuildSnapshotShadowPackage(request);
  Require(packaged.ok, "MDF-003 snapshot package did not commit");
  Require(packaged.digest_verified, "MDF-003 snapshot package digest mismatch");
  Require(std::filesystem::exists(request.package_directory / "snapshot_shadow.manifest"),
          "MDF-003 package manifest missing");

  request.parser_recovery_authority_claim = true;
  const auto parser_claim = filespace::BuildSnapshotShadowPackage(request);
  Require(!parser_claim.ok &&
              parser_claim.diagnostic_code == "SB-SNAPSHOT-SHADOW-PARSER-RECOVERY-AUTHORITY-REFUSED",
          "MDF-003 parser recovery authority was not refused");
  request.parser_recovery_authority_claim = false;
  request.kms_manifest_admitted = false;
  const auto kms_refused = filespace::BuildSnapshotShadowPackage(request);
  Require(!kms_refused.ok &&
              kms_refused.diagnostic_code == "SB-SNAPSHOT-SHADOW-KMS-MANIFEST-REFUSED",
          "MDF-003 missing KMS manifest was not refused");
}

void TestPhysicalReencryption(const std::filesystem::path& dir) {
  // MDF-004
  const auto source = dir / "mdf004/plain-or-old-key.page";
  const auto target = dir / "mdf004/new-key.page";
  WriteFile(source, "old encrypted bytes with protected payload");

  filespace::PhysicalReencryptionRequest request;
  request.source_path = source;
  request.target_path = target;
  request.old_key_id = 0x1111;
  request.new_key_id = 0x2267;
  const auto rewritten = filespace::RewritePhysicalEncryptionProfile(request);
  Require(rewritten.ok, "MDF-004 physical re-encryption did not commit");
  Require(rewritten.digest_verified, "MDF-004 physical re-encryption did not change ciphertext");
  Require(ReadFile(target) != ReadFile(source), "MDF-004 target retained old physical bytes");

  request.legal_hold = true;
  const auto legal_hold = filespace::RewritePhysicalEncryptionProfile(request);
  Require(!legal_hold.ok &&
              legal_hold.diagnostic_code == "SB-PHYSICAL-REENCRYPTION-RETENTION-BLOCKED",
          "MDF-004 legal hold did not block re-encryption");
  request.legal_hold = false;
  request.allow_plaintext_diagnostic = true;
  const auto plaintext_diag = filespace::RewritePhysicalEncryptionProfile(request);
  Require(!plaintext_diag.ok &&
              plaintext_diag.diagnostic_code ==
                  "SB-PHYSICAL-REENCRYPTION-PLAINTEXT-DIAGNOSTIC-REFUSED",
          "MDF-004 plaintext diagnostic was not refused");
}

void TestRepairRebuildSalvage(const std::filesystem::path& dir) {
  // MDF-005
  const auto source = dir / "mdf005/corrupt.page";
  const auto mirror = dir / "mdf005/mirror.page";
  const auto output = dir / "mdf005/repaired.page";
  const auto quarantine = dir / "mdf005/quarantine.record";
  WriteFile(source, "corrupt body");
  WriteFile(mirror, "verified mirror body");

  filespace::RepairBodyPlan plan;
  plan.operation = filespace::RepairBodyOperation::salvage;
  plan.source_path = source;
  plan.mirror_path = mirror;
  plan.output_path = output;
  plan.quarantine_path = quarantine;
  plan.expected_source_digest = 7;
  const auto repaired = filespace::ExecuteRepairBodyPlan(plan);
  Require(repaired.ok, "MDF-005 repair/rebuild/salvage body plan did not commit");
  Require(ReadFile(output) == "verified mirror body", "MDF-005 mirror body was not selected");
  Require(std::filesystem::exists(quarantine), "MDF-005 salvage quarantine evidence missing");

  plan.mga_transaction_authority = false;
  const auto missing_authority = filespace::ExecuteRepairBodyPlan(plan);
  Require(!missing_authority.ok &&
              missing_authority.diagnostic_code == "SB-REPAIR-BODY-MGA-AUTHORITY-REQUIRED",
          "MDF-005 missing MGA authority did not fail closed");
}

void TestDurableSweep(const std::filesystem::path& dir) {
  // MDF-006
  const auto delete_target = dir / "mdf006/delete.segment";
  const auto retain_target = dir / "mdf006/retain.segment";
  const auto resume_target = dir / "mdf006/resume.segment";
  const auto state = dir / "mdf006/sweep.state";
  WriteFile(delete_target, "delete");
  WriteFile(retain_target, "retain");
  WriteFile(resume_target, "resume");

  filespace::DurableSweepRequest request;
  request.state_path = state;
  request.actions = {
      {delete_target, false, true, false},
      {retain_target, true, true, false},
      {resume_target, false, true, false},
  };
  const auto swept = filespace::PersistAndRunDurableSweep(request);
  Require(swept.ok, "MDF-006 durable sweep did not commit");
  Require(!std::filesystem::exists(delete_target), "MDF-006 delete target survived sweep");
  Require(std::filesystem::exists(retain_target), "MDF-006 legal hold target was deleted");

  WriteFile(resume_target, "resume");
  const auto resumed = filespace::ResumeDurableSweep(state);
  Require(resumed.ok, "MDF-006 durable sweep resume failed");
  Require(!std::filesystem::exists(resume_target), "MDF-006 resumed delete target survived");

  request.persist_before_delete = false;
  const auto missing_state = filespace::PersistAndRunDurableSweep(request);
  Require(!missing_state.ok &&
              missing_state.diagnostic_code == "SB-DURABLE-SWEEP-STATE-FIRST-REQUIRED",
          "MDF-006 sweep without durable state did not fail closed");
}

void TestFilespacePackage(const std::filesystem::path& dir) {
  // MDF-007
  const auto member_a = dir / "mdf007/member_a.bin";
  const auto member_b = dir / "mdf007/member_b.bin";
  WriteFile(member_a, "encrypted member a");
  WriteFile(member_b, "encrypted member b");

  filespace::PackageTransferRequest request;
  request.member_files = {member_a, member_b};
  request.package_directory = dir / "mdf007/package";
  request.restore_directory = dir / "mdf007/restore";
  const auto transferred = filespace::TransferEncryptedPackageMembers(request);
  Require(transferred.ok, "MDF-007 package transfer did not commit");
  Require(transferred.digest_verified, "MDF-007 package transfer digest mismatch");
  Require(ReadFile(request.restore_directory / "member_a.bin") == "encrypted member a",
          "MDF-007 restored package member missing");

  request.parser_restore_authority_claim = true;
  const auto parser_claim = filespace::TransferEncryptedPackageMembers(request);
  Require(!parser_claim.ok &&
              parser_claim.diagnostic_code ==
                  "SB-FILESPACE-PACKAGE-PARSER-RESTORE-AUTHORITY-REFUSED",
          "MDF-007 parser restore authority was not refused");
}

void TestShardPlacement(const std::filesystem::path& dir) {
  // MDF-008
  const std::vector<std::string> operations = {
      "move",      "split",      "merge",     "rebalance", "freeze",
      "archive",   "reattach",   "quarantine", "reconcile", "drop",
  };
  for (const std::string& operation : operations) {
    filespace::ShardPlacementRequest request;
    request.operation = operation;
    request.state_path = dir / "mdf008" / (operation + ".state");
    request.local_transaction_number = 8001;
    const auto placed = filespace::ExecuteShardPlacementOperation(request);
    Require(placed.ok, "MDF-008 shard placement operation did not commit");
    Require(std::filesystem::exists(request.state_path),
            "MDF-008 shard placement state was not persisted");
  }

  filespace::ShardPlacementRequest request;
  request.operation = "move";
  request.state_path = dir / "mdf008/fail.state";
  request.local_transaction_number = 0;
  const auto missing_txn = filespace::ExecuteShardPlacementOperation(request);
  Require(!missing_txn.ok &&
              missing_txn.diagnostic_code == "SB-SHARD-PLACEMENT-LOCAL-TRANSACTION-REQUIRED",
          "MDF-008 missing local transaction was not refused");
  request.local_transaction_number = 8001;
  request.standalone_cluster_surrogate = true;
  const auto surrogate = filespace::ExecuteShardPlacementOperation(request);
  Require(!surrogate.ok &&
              surrogate.diagnostic_code ==
                  "SB-SHARD-PLACEMENT-STANDALONE-CLUSTER-SURROGATE-REFUSED",
          "MDF-008 standalone cluster surrogate was not refused");
}

void TestReservedPageFamilyBodies() {
  // MDF-009
  bool generic_reserved_seen = false;
  bool cluster_reserved_seen = false;
  bool protected_reserved_seen = false;
  for (const page::PageFamilyDescriptor& descriptor : page::BuiltinPageFamilyRegistry()) {
    if (descriptor.registry_status != page::PageRegistryStatus::reserved) {
      continue;
    }
    page::ReservedPageFamilyBodyProfile profile;
    profile.cluster_authority_admitted = descriptor.cluster_only;
    profile.protected_material_authority_admitted = descriptor.encrypted_or_opaque;
    const auto built = page::BuildReservedPageFamilyBody(
        descriptor.page_type, Bytes("reserved family body payload"), profile);
    Require(built.ok, "MDF-009 reserved page-family body did not build");
    const auto parsed = page::ParseReservedPageFamilyBody(descriptor.page_type, built.body, profile);
    Require(parsed.ok, "MDF-009 reserved page-family body did not parse");
    Require(parsed.payload == Bytes("reserved family body payload"),
            "MDF-009 reserved page-family payload changed");
    const auto support_claim =
        page::ValidatePageTypeProductSupportClaim(descriptor.page_type, "implemented");
    Require(!support_claim.ok(),
            "MDF-009 reserved page-family product support overclaim was accepted");
    if (!descriptor.cluster_only && !descriptor.encrypted_or_opaque) {
      generic_reserved_seen = true;
    }
    if (descriptor.cluster_only) {
      cluster_reserved_seen = true;
      page::ReservedPageFamilyBodyProfile refused;
      const auto cluster_refused =
          page::BuildReservedPageFamilyBody(descriptor.page_type, Bytes("payload"), refused);
      Require(!cluster_refused.ok &&
                  cluster_refused.diagnostic_code ==
                      "SB-RESERVED-PAGE-FAMILY-CLUSTER-AUTHORITY-REQUIRED",
              "MDF-009 cluster authority requirement was not enforced");
    }
    if (descriptor.encrypted_or_opaque) {
      protected_reserved_seen = true;
      page::ReservedPageFamilyBodyProfile refused;
      const auto protected_refused =
          page::BuildReservedPageFamilyBody(descriptor.page_type, Bytes("payload"), refused);
      Require(!protected_refused.ok &&
                  protected_refused.diagnostic_code ==
                      "SB-RESERVED-PAGE-FAMILY-PROTECTED-MATERIAL-REQUIRED",
              "MDF-009 protected material authority requirement was not enforced");
    }
  }
  Require(generic_reserved_seen, "MDF-009 did not exercise a local reserved page family");
  Require(cluster_reserved_seen, "MDF-009 did not exercise a cluster reserved page family");
  Require(protected_reserved_seen, "MDF-009 did not exercise a protected reserved page family");
}

void TestStorageTierExecution(const std::filesystem::path& dir) {
  // MDF-010
  const auto source = dir / "mdf010/hot.page";
  const auto target = dir / "mdf010/cold.page";
  const auto journal = dir / "mdf010/tier.journal";
  WriteFile(source, "hot tier page body");

  filespace::StorageTierMigrationRequest request;
  request.source_path = source;
  request.target_path = target;
  request.journal_path = journal;
  request.phase = filespace::StorageTierPhase::stage;
  const auto staged = filespace::ExecuteStorageTierOperation(request);
  Require(staged.ok, "MDF-010 storage-tier stage did not commit");
  Require(staged.digest_verified, "MDF-010 staged digest mismatch");
  Require(ReadFile(target) == ReadFile(source), "MDF-010 staged target bytes diverged");

  request.phase = filespace::StorageTierPhase::commit;
  const auto committed = filespace::ExecuteStorageTierOperation(request);
  Require(committed.ok && committed.durable_state_changed,
          "MDF-010 storage-tier commit did not publish durable state");
  Require(committed.cache_invalidated, "MDF-010 storage-tier commit did not invalidate cache");

  request.phase = filespace::StorageTierPhase::rollback;
  const auto rolled_back = filespace::ExecuteStorageTierOperation(request);
  Require(rolled_back.ok, "MDF-010 storage-tier rollback failed");
  Require(!std::filesystem::exists(target), "MDF-010 rollback left target bytes visible");

  request.phase = filespace::StorageTierPhase::stage;
  request.cache_epoch_current = false;
  const auto stale_cache = filespace::ExecuteStorageTierOperation(request);
  Require(!stale_cache.ok &&
              stale_cache.diagnostic_code == "SB-STORAGE-TIER-STALE-CACHE-EPOCH",
          "MDF-010 stale cache epoch did not fail closed");
}

}  // namespace

int main() {
  const auto dir = TempDir();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{dir};

  TestFilespaceRelocation(dir);
  TestCorePageBodyRelocation(dir);
  TestSnapshotShadowPackage(dir);
  TestPhysicalReencryption(dir);
  TestRepairRebuildSalvage(dir);
  TestDurableSweep(dir);
  TestFilespacePackage(dir);
  TestShardPlacement(dir);
  TestReservedPageFamilyBodies();
  TestStorageTierExecution(dir);

  std::cout << "FINAL-DEFERRED-STORAGE-WORKFLOWS-GATE passed: MDF-001 MDF-002 MDF-003 "
               "MDF-004 MDF-005 MDF-006 MDF-007 MDF-008 MDF-009 MDF-010\n";
  return EXIT_SUCCESS;
}
