// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "temp_workspace_lifecycle.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mem = scratchbird::core::memory;
// Test-only interposition forwards normal operations to real libc. Selected
// operations return explicit injected errors or interrupt the actual syscall
// sequence in a child; no injected path reports fabricated success.
const char* crash_path = nullptr;
const char* manifest_path = nullptr;
unsigned injection = 0, manifest_renames = 0;
dev_t directory_device{};
ino_t directory_inode{};
bool BeforeRemove(const char* path) {
  if (!crash_path || std::strcmp(path, crash_path) != 0) return true;
  if (injection == 1) ::_exit(78);
  if (injection == 6) { errno = EACCES; return false; }
  return true;
}
void AfterRemove(const char* path, int result) {
  if (result == 0 && (injection == 2 || injection == 9) &&
      crash_path && std::strcmp(path, crash_path) == 0)
    ::_exit(77);
}
extern "C" int remove(const char* path) noexcept {
  using Fn = int (*)(const char*);
  static const auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "remove"));
  if (!real) ::_exit(98);
  if (!BeforeRemove(path)) return -1;
  const int result = real(path);
  AfterRemove(path, result);
  return result;
}
extern "C" int unlink(const char* path) noexcept {
  using Fn = int (*)(const char*);
  static const auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "unlink"));
  if (!real) ::_exit(98);
  if (!BeforeRemove(path)) return -1;
  const int result = real(path);
  AfterRemove(path, result);
  return result;
}
extern "C" int rename(const char* from, const char* to) noexcept {
  using Fn = int (*)(const char*, const char*);
  static const auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "rename"));
  if (!real) ::_exit(98);
  const bool selected = manifest_path && std::strcmp(to, manifest_path) == 0;
  if (selected) {
    ++manifest_renames;
    if ((injection == 5 && manifest_renames == 1) ||
        (injection == 7 && manifest_renames == 2)) { errno = EIO; return -1; }
  }
  const int result = real(from, to);
  if (selected && result == 0 && injection == 3 && manifest_renames == 2) ::_exit(79);
  return result;
}
extern "C" int fsync(int fd) {
  using Fn = int (*)(int);
  static const auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fsync"));
  if (!real) ::_exit(98);
  struct stat status {};
  if ((injection == 4 || injection == 10 || (injection == 8 && manifest_renames >= 2)) &&
      ::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode) &&
      status.st_dev == directory_device && status.st_ino == directory_inode) {
    errno = EIO;
    return -1;
  }
  return real(fd);
}
void Require(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    char pattern[] = "/tmp/sb-temp-cleanup-crash-XXXXXX";
    const auto created = ::mkdtemp(pattern);
    Require(created != nullptr, "mkdtemp");
    path = created;
  }
  ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
mem::TempWorkspaceUuid Id(unsigned char suffix) {
  return {{1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,suffix}};
}
mem::TempWorkspaceAllocationRequest Request(unsigned char suffix) {
  mem::TempWorkspaceAllocationRequest request;
  request.bytes = 64;
  request.lifetime = mem::TempWorkspaceLifetime::operation_lifetime;
  request.owner.temp_object_uuid = Id(suffix);
  request.owner.operation_id = Id(suffix + 1);
  request.owner.database_id = Id(1); request.owner.engine_id = Id(2);
  request.owner.session_id = Id(3); request.owner.transaction_id = Id(4);
  request.owner.statement_id = Id(5); request.owner.snapshot_boundary = Id(6);
  request.owner.metadata_boundary = Id(7); request.owner.resource_budget_reference = Id(8);
  request.owner.policy_generation = 31; request.owner.security_generation = 53;
  return request;
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "read real survivor file");
  return {std::istreambuf_iterator<char>(in), {}};
}
void RunCase(unsigned scenario) {
    Directory directory;
    mem::TempWorkspacePolicy policy;
    policy.root_path = directory.path;
    policy.database_uuid = Id(1); policy.engine_uuid = Id(2);
    policy.require_ceic_011_reservation = true;
    mem::TempWorkspaceRecord selected, survivor;
    {
      mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
      policy.reservation_ledger = &ledger;
      mem::TempWorkspaceLifecycleManager manager(policy);
      const auto first = manager.AllocateSpillFile(Request(20));
      auto survivor_request = Request(30);
      if (scenario == 9) survivor_request.recovery_resume_supported = true;
      const auto second = manager.AllocateSpillFile(survivor_request);
      Require(first.ok() && first.record && second.ok() && second.record, "real spill setup");
      selected = *first.record; survivor = *second.record;
      std::ofstream out(survivor.path, std::ios::binary | std::ios::trunc);
      out << "other operation payload survives cleanup crash";
      out.close();
      Require(static_cast<bool>(out), "write real survivor payload");
    }
    const auto before = Read(survivor.path);
    const auto selected_path = selected.path.string();
    const auto manifest = (directory.path / ".scratchbird_temp_workspace_manifest.v3").string();
    const pid_t child = ::fork();
    Require(child >= 0, "fork cleanup process");
    if (child == 0) {
      mem::HierarchicalMemoryBudgetLedger ledger(7, 9);
      policy.reservation_ledger = &ledger;
      mem::TempWorkspaceLifecycleManager manager(policy);
      if (manager.ActiveRecords().size() != 2) ::_exit(90);
      crash_path = selected_path.c_str();
      manifest_path = manifest.c_str();
      struct stat root_stat {};
      if (::stat(directory.path.c_str(), &root_stat) != 0) ::_exit(93);
      directory_device = root_stat.st_dev; directory_inode = root_stat.st_ino;
      injection = scenario;
      if (scenario == 10) {
        mem::TempWorkspaceRecoveryEvidence invalid;
        invalid.integrity_verified = false;
        const auto classified = manager.ClassifyForRecovery(selected.allocation_id, invalid);
        if (classified.ok() || !std::filesystem::exists(selected.path)) ::_exit(94);
        ::_exit(80);
      }
      mem::TempWorkspaceRecoveryEvidence recovery;
      recovery.engine_recovery_authority = true;
      recovery.leaked_after_crash = true;
      const auto result = scenario == 9 ? manager.CleanupRecoverySafe(recovery)
                                       : manager.CleanupOperation(selected.owner.operation_id);
      if (scenario >= 4) {
        if (result.ok()) ::_exit(94);
        const bool expected_file = scenario <= 6;
        if (std::filesystem::exists(selected.path) != expected_file) ::_exit(95);
        if (scenario == 4) {
          // A pending marker after failed directory sync is not sufficient:
          // the second cleanup must retry the barrier and still refuse unlink.
          if (manager.CleanupOperation(selected.owner.operation_id).ok() ||
              !std::filesystem::exists(selected.path)) ::_exit(96);
        }
        ::_exit(80);
      }
      ::_exit(91); // Never credit a test that missed the actual removal boundary.
    }
    int status = 0;
    const int expected_exit = scenario == 1 ? 78 : (scenario == 2 || scenario == 9) ? 77 :
                              scenario == 3 ? 79 : 80;
    Require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == expected_exit, "child must exercise its actual syscall/crash boundary");
    const bool expected_file = scenario == 1 || (scenario >= 4 && scenario <= 6) || scenario == 10;
    Require(std::filesystem::exists(selected.path) == expected_file && Read(survivor.path) == before,
            "interrupted or refused cleanup must preserve the other operation bytes");
    if (scenario == 1) {
      const auto original_manifest = Read(manifest);
      auto refused = [&] {
        mem::HierarchicalMemoryBudgetLedger ledger(17, 19);
        policy.reservation_ledger = &ledger;
        mem::TempWorkspaceLifecycleManager invalid(policy);
        Require(invalid.ActiveRecords().empty() && ledger.Snapshot().current_bytes == 0 &&
                !invalid.AllocateSpillFile(Request(40)).ok() && Read(manifest) == original_manifest,
                "pending cleanup does not admit symlinks or unexpected hardlinks");
      };
      const auto held_link = directory.path / "held-link";
      std::filesystem::create_hard_link(selected.path, held_link);
      refused();
      std::filesystem::remove(held_link);
      const auto retained_file = directory.path / "retained-file";
      std::filesystem::rename(selected.path, retained_file);
      std::filesystem::create_symlink(retained_file, selected.path);
      refused();
      std::filesystem::remove(selected.path);
      std::filesystem::rename(retained_file, selected.path);
    }
    mem::HierarchicalMemoryBudgetLedger recovered_ledger(11, 13);
    policy.reservation_ledger = &recovered_ledger;
    mem::TempWorkspaceLifecycleManager recovered(policy);
    const auto other = recovered.Find(survivor.allocation_id);
    Require(other && other->owner == survivor.owner && Read(other->path) == before,
            "cleanup crash must not prevent reopening an intact unrelated owner");
    const auto pending = recovered.Find(selected.allocation_id);
    if (scenario == 1 || scenario == 2 || scenario == 4 || scenario == 6 || scenario == 7 || scenario == 9)
      Require(pending && pending->state == mem::TempWorkspaceState::cleanup_pending &&
              pending->owner == selected.owner, "recovery retains exact pending cleanup owner");
    if (scenario == 1) {
      mem::TempWorkspaceRecoveryEvidence invalid;
      invalid.integrity_verified = false;
      const auto classified = recovered.ClassifyForRecovery(selected.allocation_id, invalid);
      Require(classified.ok() && classified.recovery_class == mem::TempRecoveryClass::quarantine_required,
              "current recovery integrity failure requires quarantine");
      const auto refused = recovered.CleanupOperation(selected.owner.operation_id);
      const auto retained = recovered.Find(selected.allocation_id);
      Require(!refused.ok() && std::filesystem::exists(selected.path) && retained &&
              retained->state == mem::TempWorkspaceState::cleanup_pending,
              "durable cleanup intent cannot bypass current recovery quarantine");
      mem::TempWorkspaceRecoveryEvidence valid;
      valid.engine_recovery_authority = true; valid.leaked_after_crash = true;
      const auto revalidated = recovered.ClassifyForRecovery(selected.allocation_id, valid);
      Require(revalidated.ok() &&
              revalidated.recovery_class == mem::TempRecoveryClass::leaked_cleanup_required,
              "actual integrity revalidation permits subsequent cleanup");
    }
    if (scenario == 10) {
      Require(pending && pending->recovery_class == mem::TempRecoveryClass::quarantine_required &&
              !recovered.CleanupOperation(selected.owner.operation_id).ok(),
              "failed classification sync retains conservative quarantine");
      mem::TempWorkspaceRecoveryEvidence valid;
      valid.engine_recovery_authority = true; valid.leaked_after_crash = true;
      Require(recovered.ClassifyForRecovery(selected.allocation_id, valid).ok(),
              "classification can be durably retried with current recovery evidence");
    }
    Require(recovered.CleanupOperation(selected.owner.operation_id).ok(),
            "restart can finish selected cleanup without resurrecting deleted file");
    Require(!recovered.Find(selected.allocation_id) && recovered.ActiveRecords().size() == 1 &&
            recovered.Snapshot().active_bytes == 64 && recovered_ledger.Snapshot().current_bytes == 64,
            "restart cleanup leaves only the actual survivor and its reservation");
    Require(recovered.CleanupOperation(survivor.owner.operation_id).ok() &&
            recovered.ActiveRecords().empty() && recovered_ledger.Snapshot().current_bytes == 0 &&
            std::filesystem::is_empty(directory.path), "final exact cleanup removes files and manifest");
}
void RejectUnmarkedMissingFile() {
  Directory directory;
  mem::TempWorkspacePolicy policy;
  policy.root_path = directory.path;
  policy.database_uuid = Id(1); policy.engine_uuid = Id(2);
  mem::TempWorkspaceRecord selected, survivor;
  {
    mem::TempWorkspaceLifecycleManager manager(policy);
    const auto first = manager.AllocateSpillFile(Request(20));
    const auto second = manager.AllocateSpillFile(Request(30));
    Require(first.ok() && first.record && second.ok() && second.record, "unmarked missing-file setup");
    selected = *first.record; survivor = *second.record;
  }
  const auto manifest = directory.path / ".scratchbird_temp_workspace_manifest.v3";
  const auto before_manifest = Read(manifest), before_survivor = Read(survivor.path);
  Require(std::filesystem::remove(selected.path), "remove unmarked active file");
  mem::TempWorkspaceLifecycleManager rejected(policy);
  Require(rejected.ActiveRecords().empty() && !rejected.AllocateSpillFile(Request(40)).ok() &&
          Read(manifest) == before_manifest && Read(survivor.path) == before_survivor,
          "missing active file without durable cleanup intent must remain a preserved refusal");
}
int main() {
  try {
    for (unsigned scenario = 1; scenario != 11; ++scenario) {
      std::printf("temp cleanup scenario=%u\n", scenario);
      std::fflush(stdout);
      RunCase(scenario);
    }
    RejectUnmarkedMissingFile();
    std::puts("temp workspace cleanup crash: 10 interruption/failure cases and 3 admission negatives passed");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
