// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_CAREFUL_WRITE_PRECEDENCE
// Owner ruling 2026-11-05: careful write, no intent records. Process-loss
// coverage of the careful-write primitive on a real file.
//
// A small test-only format (not a ScratchBird page format): page 0 is a root
// that points to data pages, pages 1..10 are data pages, and the last page is
// an allocation map with one bit per page; every page carries a checksum. The
// map sits at the highest offset on purpose: without the map -> data edge the
// offset tie-break would issue it after the root, so the edge is exercised,
// not implied. The plan moves the file from an old state to a new one with
// edges map -> data and data -> root. For every k from 0 to the number of writes, a forked child
// runs the plan through a sink that _exits after k writes; the parent then
// reopens the file read-only and checks that the reachable state is the old
// or the new one, every root pointer names a page with a valid checksum, and
// every reachable page is marked in the map. Written but unreachable pages
// (orphans) are counted, never an error. The parent then re-runs the plan to
// show that a retry after the loss reaches the new state.
//
// Limit: fork + _exit models process death only. The kernel page cache
// survives, so this proves issue order and the format invariants for every
// write prefix; it proves nothing about power loss or device reordering, and
// it does not model a write torn inside one page.
#include "careful_write.hpp"
#include "disk_device.hpp"
#include "../support/owned_temp_directory.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace disk = scratchbird::storage::disk;
namespace fs = std::filesystem;
using disk::CarefulWriteBarrierPolicy;
using disk::CarefulWriteError;
using disk::CarefulWriteNodeId;
using disk::CarefulWritePlan;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::usize;

namespace {

unsigned checks = 0;
void Check(bool condition, const std::string& message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

// Distinct child exit codes, so an unrelated child failure never passes as
// the planned crash.
constexpr int kPlannedCrash = 86;
constexpr int kChildCompleted = 70;
constexpr int kChildPlanFailed = 71;
constexpr int kChildOpenFailed = 72;
constexpr int kChildException = 73;

constexpr usize kPageSize = 1024;
constexpr u32 kPages = 12;
constexpr u32 kRootPage = 0;
constexpr u32 kFirstDataPage = 1;
constexpr u32 kMapPage = kPages - 1;
constexpr u32 kMaxPointers = 8;
constexpr u32 kRootMagic = 0x544F4F52u;  // "ROOT"
constexpr u32 kMapMagic = 0x2150414Du;   // "MAP!"
constexpr u32 kDataMagic = 0x41544144u;  // "DATA"
constexpr usize kChecksumAt = kPageSize - 8;

using Page = std::vector<byte>;

void Put32(Page& page, usize at, u32 value) { std::memcpy(page.data() + at, &value, sizeof value); }
u32 Get32(const Page& page, usize at) {
  u32 value = 0;
  std::memcpy(&value, page.data() + at, sizeof value);
  return value;
}
u64 Fnv1a(const byte* data, usize size) {
  u64 hash = 1469598103934665603ull;
  for (usize i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}
void Seal(Page& page) {
  const u64 sum = Fnv1a(page.data(), kChecksumAt);
  std::memcpy(page.data() + kChecksumAt, &sum, sizeof sum);
}
bool Sealed(const Page& page) {
  u64 stored = 0;
  std::memcpy(&stored, page.data() + kChecksumAt, sizeof stored);
  return stored == Fnv1a(page.data(), kChecksumAt);
}

Page RootPage(u32 generation, const std::vector<u32>& pointers) {
  Page page(kPageSize, 0);
  Put32(page, 0, kRootMagic);
  Put32(page, 4, generation);
  Put32(page, 8, static_cast<u32>(pointers.size()));
  for (usize i = 0; i < pointers.size(); ++i) Put32(page, 12 + 4 * i, pointers[i]);
  Seal(page);
  return page;
}
Page MapPage(const std::set<u32>& allocated) {
  Page page(kPageSize, 0);
  Put32(page, 0, kMapMagic);
  u32 bits = 0;
  for (const auto p : allocated) bits |= 1u << p;
  Put32(page, 4, bits);
  Seal(page);
  return page;
}
Page DataPage(u32 page_number, u32 generation) {
  Page page(kPageSize, 0);
  Put32(page, 0, kDataMagic);
  Put32(page, 4, page_number);
  Put32(page, 8, generation);
  for (usize i = 12; i < kChecksumAt; ++i) page[i] = static_cast<byte>(page_number * 31 + generation * 7 + i);
  Seal(page);
  return page;
}

// Old state: root generation 1 -> {1,2,3}. New state: root generation 2 ->
// {1,4,5,6,7}; page 1 is shared, 2 and 3 become unreachable (still marked:
// freeing them is a later step that must follow the root).
const std::vector<u32> kOldPointers = {1, 2, 3};
const std::vector<u32> kNewPointers = {1, 4, 5, 6, 7};
const std::vector<u32> kNewDataPages = {4, 5, 6, 7};

std::vector<Page> OldFile() {
  std::vector<Page> pages(kPages, Page(kPageSize, 0));
  pages[kRootPage] = RootPage(1, kOldPointers);
  pages[kMapPage] = MapPage({kRootPage, kMapPage, 1, 2, 3});
  for (const auto p : kOldPointers) pages[p] = DataPage(p, 1);
  return pages;
}
std::vector<Page> NewFile() {
  auto pages = OldFile();
  pages[kRootPage] = RootPage(2, kNewPointers);
  pages[kMapPage] = MapPage({kRootPage, kMapPage, 1, 2, 3, 4, 5, 6, 7});
  for (const auto p : kNewDataPages) pages[p] = DataPage(p, 2);
  return pages;
}

struct TransitionPlan {
  CarefulWritePlan plan;
  std::vector<Page> old_pages = OldFile();
  std::vector<Page> new_pages = NewFile();
  u32 writes = 0;
  TransitionPlan() {
    const auto add = [&](u32 p) {
      return plan.Add(u64{p} * kPageSize, new_pages[p], std::span<const byte>(old_pages[p])).node;
    };
    const auto map = add(kMapPage);
    const auto root = add(kRootPage);
    for (const auto p : kNewDataPages) {
      const auto data = add(p);
      Check(plan.Before(map, data) == CarefulWriteError::none, "map before data");
      Check(plan.Before(data, root) == CarefulWriteError::none, "data before root");
    }
    Check(plan.build_error() == CarefulWriteError::none, "transition plan builds");
    writes = static_cast<u32>(plan.node_count());
  }
};

void WriteFile(const std::string& path, const std::vector<Page>& pages) {
  disk::FileDevice device;
  Check(device.Open(path, disk::FileOpenMode::create_new).ok(), "create fixture file");
  for (u32 p = 0; p < pages.size(); ++p)
    Check(device.WriteAt(u64{p} * kPageSize, pages[p].data(), kPageSize).ok(), "write fixture page");
  Check(device.Close().ok(), "close fixture file");
}

std::vector<Page> ReadFileReadOnly(const std::string& path) {
  disk::FileDevice device;
  Check(device.Open(path, disk::FileOpenMode::open_existing_read_only).ok(), "reopen read-only");
  Check(!device.forced_ordered_writes(), "read-only reopen reports no forced writes");
  std::vector<Page> pages(kPages, Page(kPageSize, 0));
  for (u32 p = 0; p < kPages; ++p)
    Check(device.ReadAt(u64{p} * kPageSize, pages[p].data(), kPageSize).ok(), "read page");
  Check(device.Close().ok(), "close read-only");
  return pages;
}

struct Observation {
  u32 generation = 0;
  unsigned orphans = 0;
};

// The format invariants. Returns the reachable generation and orphan count.
Observation Verify(const std::vector<Page>& pages, const std::string& label) {
  const auto old_pages = OldFile();
  const auto new_pages = NewFile();
  const Page& root = pages[kRootPage];
  Check(Sealed(root) && Get32(root, 0) == kRootMagic, label + ": root checksum valid");
  const u32 generation = Get32(root, 4);
  const u32 count = Get32(root, 8);
  Check(count <= kMaxPointers, label + ": root pointer count in range");
  std::vector<u32> pointers;
  for (u32 i = 0; i < count; ++i) pointers.push_back(Get32(root, 12 + 4 * i));
  // Reachable state is exactly the old one or exactly the new one.
  const bool is_old = generation == 1 && pointers == kOldPointers;
  const bool is_new = generation == 2 && pointers == kNewPointers;
  Check(is_old || is_new, label + ": reachable state is old or new");
  const auto& expected = is_old ? old_pages : new_pages;
  const Page& map = pages[kMapPage];
  Check(Sealed(map) && Get32(map, 0) == kMapMagic, label + ": map checksum valid");
  const u32 bits = Get32(map, 4);
  Check((bits & (1u << kRootPage)) && (bits & (1u << kMapPage)), label + ": root and map marked");
  std::set<u32> reachable;
  for (const auto p : pointers) {
    Check(p >= kFirstDataPage && p < kMapPage, label + ": pointer names a data page");
    const Page& data = pages[p];
    Check(Sealed(data) && Get32(data, 0) == kDataMagic && Get32(data, 4) == p,
          label + ": pointer " + std::to_string(p) + " targets a page with a valid checksum");
    Check(data == expected[p], label + ": reachable page " + std::to_string(p) + " holds its state's image");
    Check((bits & (1u << p)) != 0, label + ": reachable page " + std::to_string(p) + " marked in map");
    reachable.insert(p);
  }
  Observation observation;
  observation.generation = generation;
  for (u32 p = kFirstDataPage; p < kMapPage; ++p) {
    if (reachable.count(p)) continue;
    if (Sealed(pages[p]) && Get32(pages[p], 0) == kDataMagic) ++observation.orphans;  // counted only
  }
  return observation;
}

// Passes every call to the file sink, but _exits after `limit` writes: before
// the first write when the limit is 0, otherwise right after the limit-th.
class CrashAfterWritesSink final : public disk::CarefulWriteSink {
 public:
  CrashAfterWritesSink(disk::CarefulWriteSink& inner, u32 limit) : inner_(inner), limit_(limit) {}
  disk::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (writes_ == limit_) _exit(kPlannedCrash);
    const auto r = inner_.WriteAt(offset, bytes);
    if (r.ok() && ++writes_ == limit_) _exit(kPlannedCrash);
    return r;
  }
  disk::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  disk::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  disk::CarefulWriteSink& inner_;
  u32 limit_;
  u32 writes_ = 0;
};

constexpr u32 kNoCrash = ~u32{0};

[[noreturn]] void RunChild(const std::string& path, u32 limit, CarefulWriteBarrierPolicy policy) {
  try {
    TransitionPlan transition;
    disk::FileDevice device;
    if (!device.Open(path, disk::FileOpenMode::open_existing).ok()) _exit(kChildOpenFailed);
    int code = kChildCompleted;
    {
      disk::FileDeviceCarefulWriteSink file_sink(device);
      CrashAfterWritesSink sink(file_sink, limit);
      const auto r = transition.plan.Execute(sink, policy);
      if (!r.ok()) code = kChildPlanFailed;
    }
    if (!device.Close().ok()) code = kChildPlanFailed;
    _exit(code);
  } catch (...) {
    _exit(kChildException);
  }
}

void CrashAtEveryWrite(const fs::path& directory, CarefulWriteBarrierPolicy policy, const char* policy_name) {
  TransitionPlan reference;
  const u32 total = reference.writes;
  Check(total == 6, "transition plan has six writes");
  for (u32 k = 0; k <= total + 1; ++k) {
    const bool full_run = k == total + 1;
    const u32 limit = full_run ? kNoCrash : k;
    const std::string label = std::string(policy_name) + (full_run ? " full run" : " crash after " + std::to_string(k));
    const auto path = (directory / (std::string(policy_name) + "-" + std::to_string(k) + ".db")).string();
    WriteFile(path, OldFile());
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = fork();
    Check(child >= 0, label + ": fork");
    if (child == 0) RunChild(path, limit, policy);
    int status = 0;
    Check(waitpid(child, &status, 0) == child, label + ": wait for child");
    Check(WIFEXITED(status), label + ": child exited (not signalled)");
    const int expected_code = full_run ? kChildCompleted : kPlannedCrash;
    Check(WEXITSTATUS(status) == expected_code,
          label + ": child exit code " + std::to_string(WEXITSTATUS(status)) + ", expected " +
              std::to_string(expected_code));
    const auto observed = Verify(ReadFileReadOnly(path), label);
    // Root is the last write, so only a run that reached it shows the new state.
    const bool root_written = full_run || k == total;
    Check(observed.generation == (root_written ? 2u : 1u), label + ": state matches the write prefix");
    std::cout << "careful_write_crash " << label << ": generation=" << observed.generation
              << " orphans=" << observed.orphans << '\n';
    // Retry after the loss: already-written nodes are skipped, the rest written.
    {
      TransitionPlan retry;
      disk::FileDevice device;
      Check(device.Open(path, disk::FileOpenMode::open_existing).ok(), label + ": retry open");
      disk::CarefulWriteResult r;
      {
        disk::FileDeviceCarefulWriteSink sink(device);
        r = retry.plan.Execute(sink, policy);
      }
      Check(device.Close().ok(), label + ": retry close");
      Check(r.ok(), label + ": retry succeeds: " + disk::CarefulWriteErrorName(r.error));
      const u32 already = full_run ? total : k;
      Check(r.skipped == already && r.written == total - already,
            label + ": retry skips exactly the written prefix (skipped=" + std::to_string(r.skipped) +
                " written=" + std::to_string(r.written) + ")");
      const auto after = Verify(ReadFileReadOnly(path), label + " retry");
      Check(after.generation == 2 && ReadFileReadOnly(path) == NewFile(), label + ": retry reaches the new state");
    }
  }
}

void ForcedWriteFlag(const fs::path& directory) {
  const auto path = (directory / "forced-flag.db").string();
  WriteFile(path, OldFile());
  disk::FileDevice device;
  Check(device.Open(path, disk::FileOpenMode::open_existing).ok(), "open writable");
#if defined(__linux__) && (defined(O_DSYNC) || defined(O_SYNC))
  Check(device.forced_ordered_writes(), "writable file device has forced ordered writes on Linux");
  // Find the data descriptor by its /proc link (not the .sb.owner.lock file)
  // and read its status flags. The device also reads F_GETFL itself, so this
  // is a consistency check of the OS-held descriptor, not independent proof.
  const auto canonical = fs::canonical(path);
  int matched = 0;
  int forced_flags = 0;
  for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
    std::error_code ec;
    const auto target = fs::read_symlink(entry.path(), ec);
    if (ec || target != canonical) continue;
    const int fd = std::stoi(entry.path().filename().string());
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) continue;
    ++matched;
    forced_flags = flags;
  }
  Check(matched == 1, "exactly one descriptor names the data file (found " + std::to_string(matched) + ")");
  int forced_bits = 0;
#ifdef O_DSYNC
  forced_bits |= O_DSYNC;
#endif
#ifdef O_SYNC
  forced_bits |= O_SYNC;
#endif
  Check((forced_flags & forced_bits) != 0, "F_GETFL on the data descriptor includes O_DSYNC or O_SYNC");
  Check((forced_flags & O_ACCMODE) == O_RDWR, "data descriptor is read-write");
  std::cout << "careful_write_crash: data descriptor F_GETFL=0x" << std::hex << forced_flags << std::dec
            << " O_DSYNC=" << ((forced_flags & O_DSYNC) == O_DSYNC) << '\n';
#else
  std::cout << "careful_write_crash: forced-write flag check not available on this platform\n";
#endif
  Check(device.Close().ok(), "close writable");
  Check(!device.forced_ordered_writes(), "closed device reports no forced writes");
  disk::FileDevice reader;
  Check(reader.Open(path, disk::FileOpenMode::open_existing_read_only).ok(), "open read-only");
  Check(!reader.forced_ordered_writes(), "read-only device reports no forced writes");
  Check(reader.Close().ok(), "close read-only");
}

}  // namespace

int main() {
  try {
    scratchbird::tests::OwnedTempDirectory directory;
    {
      // The format checker itself must accept both endpoint states.
      const auto old_state = Verify(OldFile(), "old fixture");
      const auto new_state = Verify(NewFile(), "new fixture");
      Check(old_state.generation == 1 && old_state.orphans == 0, "old fixture has no orphans");
      Check(new_state.generation == 2 && new_state.orphans == 2, "new fixture leaves pages 2 and 3 unreachable");
      // ...and reject a root that points at an unwritten page.
      auto broken = OldFile();
      broken[kRootPage] = RootPage(2, kNewPointers);
      bool rejected = false;
      try {
        (void)Verify(broken, "broken fixture");
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      Check(rejected, "verifier rejects a root that points at an unwritten page");
    }
    ForcedWriteFlag(directory.path());
    CrashAtEveryWrite(directory.path(), CarefulWriteBarrierPolicy::trust_forced_writes, "trust_forced");
    CrashAtEveryWrite(directory.path(), CarefulWriteBarrierPolicy::sync_every_dependency, "sync_every");
    directory.Cleanup();
  } catch (const std::exception& error) {
    std::cerr << "careful_write_crash_test FAILED after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
  std::cout << "careful_write_crash_test passed checks=" << checks << '\n';
  return 0;
}
