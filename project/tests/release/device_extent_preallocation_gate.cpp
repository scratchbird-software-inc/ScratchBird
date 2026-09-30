// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "disk_device.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace disk = scratchbird::storage::disk;
int allocation_error = 0;
int allocation_calls = 0;
int size_fail_after = -1;
bool write_failure = false;
std::recursive_mutex* guarded_device = nullptr;
int guard_checks = 0;
int guard_failures = 0;

void CheckDeviceGuard() {
  if (!guarded_device) return;
  ++guard_checks;
  bool escaped = false;
  // A different thread must be unable to enter at each native observation or
  // mutation boundary. No scheduling sleep or elapsed-time inference is used.
  std::thread probe([&] {
    if (guarded_device->try_lock()) {
      escaped = true;
      guarded_device->unlock();
    }
  });
  probe.join();
  if (escaped) ++guard_failures;
}

void Require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

// One exclusively created, bounded fixture per test; cleanup also on failure.
struct Fixture {
  std::filesystem::path root;
  std::filesystem::path path;
  disk::FileDevice device;
  const std::string original = "nonzero-existing-data-END";
  Fixture() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "sb-01e-extent-XXXXXX").string();
    auto* made = ::mkdtemp(pattern.data());
    Require(made != nullptr, "mkdtemp failed");
    root = made;
    path = root / "extent.bin";
    try {
      Require(device.Open(path.string(), disk::FileOpenMode::create_new).ok(), "open");
      Require(device.WriteAt(0, original.data(), original.size()).ok(), "seed");
    } catch (...) {
      if (device.is_open()) (void)device.Close();
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
      throw;
    }
  }
  ~Fixture() {
    allocation_error = 0;
    size_fail_after = -1;
    write_failure = false;
    guarded_device = nullptr;
    if (device.is_open()) (void)device.Close();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  std::string Bytes() {
    Require(device.Sync().ok(), "sync");
    std::ifstream in(path, std::ios::binary);
    Require(in.is_open(), "independent read open");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

void TestUnsupportedContained() {
  Fixture f;
  allocation_error = EOPNOTSUPP;
  for (int i = 0; i != 2; ++i) {
    const auto r = f.device.PreallocateExtent(2, 5);
    Require(r.ok(), "unsupported contained range should be logically available");
    Require(f.Bytes() == f.original, "unsupported contained range corrupted existing bytes");
    Require(!r.logical_size_extended, "contained extent falsely reported growth");
    Require(r.logical_extent_available, "contained logical availability missing");
    Require(!r.platform_preallocation_succeeded, "fallback falsely reported physical reserve");
    Require(!r.fallback_extension_used, "contained extent should not issue fallback write");
  }
}

void TestNativeContained() {
  Fixture f;
  const auto r = f.device.PreallocateExtent(0, f.original.size());
  Require(r.ok() && r.platform_preallocation_succeeded, "actual native reservation failed");
  Require(!r.logical_size_extended, "native contained range falsely reported growth");
  Require(f.Bytes() == f.original, "native contained range corrupted data");
}

void TestGrowthAndRetry(int native_error) {
  Fixture f;
  allocation_error = native_error;
  const auto start = f.original.size() - 4;
  const auto count = 8192u;
  const auto r = f.device.PreallocateExtent(start, count);
  Require(r.ok(), "growth failed: " + r.diagnostic.diagnostic_code);
  Require(r.logical_size_extended, "growth not reported");
  Require(r.logical_extent_available, "successful logical extent not reported");
  Require(r.platform_preallocation_succeeded == (native_error == 0), "physical reserve evidence wrong");
  Require(r.fallback_extension_used == (native_error != 0), "fallback evidence wrong");
  auto bytes = f.Bytes();
  Require(bytes.size() == start + count, "growth size mismatch");
  Require(bytes.substr(0, f.original.size()) == f.original, "growth damaged prefix");
  Require(bytes.find_first_not_of('\0', f.original.size()) == std::string::npos,
          "new logical bytes expose nonzero content");
  const char marker = 'Z';
  Require(f.device.WriteAt(start + count - 1, &marker, 1).ok(), "tail marker write");
  const auto retry = f.device.PreallocateExtent(start, count);
  Require(retry.ok() && !retry.logical_size_extended, "idempotent retry reports growth");
  bytes.back() = marker;
  Require(f.Bytes() == bytes, "retry changed bytes");
  Require(f.device.Close().ok(), "close before reopen");
  Require(f.device.Open(f.path.string(), disk::FileOpenMode::open_existing).ok(), "reopen");
  std::string reopened(bytes.size(), '?');
  const auto read = f.device.ReadAt(0, reopened.data(), reopened.size());
  Require(read.ok() && read.bytes_transferred == reopened.size() && reopened == bytes,
          "real reopen bytes mismatch");
}

void TestFailuresAndBoundaries() {
  Fixture f;
  for (int error : {ENOSPC, EIO}) {
    allocation_error = error;
    const auto r = f.device.PreallocateExtent(0, 16384);
    Require(!r.ok() && r.diagnostic.diagnostic_code == "SB-STORAGE-DISK-PREALLOCATE-FAILED",
            "hard native error was not returned");
    Require(!r.fallback_extension_used && !r.logical_size_extended &&
            !r.platform_preallocation_succeeded, "error falsely reported effects");
    Require(f.Bytes() == f.original, "hard native error mutated contents");
  }
  allocation_error = EOPNOTSUPP;
  write_failure = true;
  const auto failed_write = f.device.PreallocateExtent(0, 16384);
  write_failure = false;
  Require(!failed_write.ok() && failed_write.diagnostic.diagnostic_code ==
          "SB-STORAGE-DISK-WRITE-FAILED", "fallback write failure missing");
  Require(!failed_write.logical_extent_available, "failed extension reported available range");
  Require(f.Bytes() == f.original, "failed extension mutated contents");

  allocation_error = 0;
  const auto calls_before = allocation_calls;
  size_fail_after = 0;
  const auto failed_size = f.device.PreallocateExtent(0, 16384);
  size_fail_after = -1;
  Require(!failed_size.ok() && failed_size.diagnostic.diagnostic_code ==
          "SB-STORAGE-DISK-SIZE-FAILED", "initial size failure missing");
  Require(allocation_calls == calls_before, "allocation attempted without initial size");
  Require(f.Bytes() == f.original, "initial size failure mutated contents");

  const auto zero = f.device.PreallocateExtent(0, 0);
  Require(zero.ok() && !zero.platform_preallocation_attempted && !zero.logical_size_extended,
          "zero extent performed work");
  for (auto count : {0ull, 2ull}) {
    const auto overflow = f.device.PreallocateExtent(std::numeric_limits<disk::u64>::max(), count);
    Require(!overflow.ok() && !overflow.platform_preallocation_attempted,
            "invalid range admitted (including zero length)");
  }
  const auto max_offset = static_cast<disk::u64>(std::numeric_limits<off_t>::max());
  Require(f.device.PreallocateExtent(max_offset, 0).ok(), "representable zero-length boundary rejected");
  const auto invalid_end = f.device.PreallocateExtent(max_offset, 1);
  Require(!invalid_end.ok() && invalid_end.diagnostic.diagnostic_code ==
          "SB-STORAGE-DISK-EXTENT-OVERFLOW", "signed native end overflow not rejected");
  const auto invalid_count = f.device.PreallocateExtent(0, std::numeric_limits<disk::u64>::max());
  Require(!invalid_count.ok() && invalid_count.diagnostic.diagnostic_code ==
          "SB-STORAGE-DISK-BYTE-COUNT-CONVERSION-OVERFLOW", "native count overflow not rejected");
  Require(allocation_calls == calls_before, "validation invoked native allocation");
  Require(f.Bytes() == f.original, "range validation changed contents");
  Require(f.device.Close().ok(), "close");
  Require(!f.device.PreallocateExtent(0, 1).ok(), "closed device admitted");
  Require(f.device.Open(f.path.string(), disk::FileOpenMode::open_existing_read_only).ok(), "readonly open");
  for (auto count : {0ull, 1ull}) {
    const auto readonly = f.device.PreallocateExtent(0, count);
    Require(!readonly.ok() && readonly.diagnostic.diagnostic_code ==
            "SB-STORAGE-DISK-PREALLOCATE-READ-ONLY", "readonly extent admitted");
  }
}

void TestAppliedNativeEffectOnObservationFailure() {
  Fixture f;
  size_fail_after = 1; // Initial observation works, post-reservation observation fails.
  const auto r = f.device.PreallocateExtent(0, 8192);
  size_fail_after = -1;
  Require(!r.ok(), "post-allocation observation failure reported success");
  Require(!r.logical_extent_available, "unobserved range reported available");
  Require(r.platform_preallocation_succeeded, "applied native effect was hidden on error");
  Require(!r.fallback_extension_used, "observation failure fell back to a write");
  Require(f.Bytes().size() == 8192, "real native allocation did not extend file");
  Require(f.Bytes().substr(0, f.original.size()) == f.original, "native effect damaged prefix");
}

void TestEveryProfileExtent() {
  for (auto size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    for (int error : {0, EOPNOTSUPP}) {
      Fixture f;
      allocation_error = error;
      // A gap followed by one profile-sized extent. This proves byte capacity,
      // not profile admission, initialized page headers or page allocation.
      const auto r = f.device.PreallocateExtent(size, size);
      Require(r.ok() && r.logical_extent_available && r.logical_size_extended, "profile extent failed");
      Require(r.platform_preallocation_succeeded == (error == 0), "profile physical evidence mismatch");
      const auto bytes = f.Bytes();
      Require(bytes.size() == 2 * size && bytes.substr(0, f.original.size()) == f.original,
              "profile size/prefix mismatch");
      Require(bytes.find_first_not_of('\0', f.original.size()) == std::string::npos,
              "profile gap/new bytes mismatch");
      const auto adjacent = f.device.PreallocateExtent(2 * size, size);
      Require(adjacent.ok() && adjacent.logical_size_extended && f.Bytes().size() == 3 * size,
              "adjacent profile extent failed");
    }
  }
}

void TestRetainedDeviceSerialization() {
  Fixture f;
  {
    auto guard = f.device.AcquireOperationGuard();
    guarded_device = guard.mutex();
  }
  guard_checks = guard_failures = 0;
  allocation_error = EOPNOTSUPP;
  const auto result = f.device.PreallocateExtent(0, 8192);
  Require(result.ok(), "guarded fallback growth failed");
  Require(guard_checks >= 4 && guard_failures == 0,
          "preallocation did not retain operation guard across native boundaries");
  guard_checks = guard_failures = 0;
  const char marker = 'Q';
  Require(f.device.WriteAt(8191, &marker, 1).ok(), "guarded writer failed");
  Require(guard_checks == 1 && guard_failures == 0,
          "native writes do not participate in retained operation guard");
  guarded_device = nullptr;
  Require(f.Bytes().back() == marker, "serialized writer bytes missing");
}

void TestDiagnosticVectors() {
  Fixture f;
  const auto check = [&](const disk::PreallocateExtentResult& result,
                         const char* code, const char* key) {
    Require(!result.ok() && !result.status.ok() && !result.diagnostic.status.ok(),
            "diagnostic reported successful status");
    const auto& d = result.diagnostic;
    Require(d.diagnostic_code == code && d.message_key == key &&
            d.source_component == "storage.disk", "diagnostic identity mismatch");
    Require(d.arguments.size() == 2, "diagnostic path/detail vector shape mismatch");
    Require(d.arguments[0].key == "path" && d.arguments[0].text() &&
            *d.arguments[0].text() == f.path.string(), "diagnostic lost retained-device path");
    Require(d.arguments[1].key == "detail" && d.arguments[1].text() &&
            !d.arguments[1].text()->empty(), "diagnostic lost error/range detail");
  };
  const auto maximum = std::numeric_limits<disk::u64>::max();
  check(f.device.PreallocateExtent(maximum, 0),
        "SB-STORAGE-DISK-OFFSET-CONVERSION-OVERFLOW", "storage.disk.offset_conversion_overflow");
  check(f.device.PreallocateExtent(0, maximum),
        "SB-STORAGE-DISK-BYTE-COUNT-CONVERSION-OVERFLOW", "storage.disk.byte_count_conversion_overflow");
  check(f.device.PreallocateExtent(maximum, 2),
        "SB-STORAGE-DISK-PREALLOCATE-RANGE-OVERFLOW", "storage.disk.preallocate_range_overflow");
  allocation_error = ENOSPC;
  check(f.device.PreallocateExtent(0, 8192),
        "SB-STORAGE-DISK-PREALLOCATE-FAILED", "storage.disk.preallocate_failed");
  allocation_error = EOPNOTSUPP;
  write_failure = true;
  const auto write = f.device.PreallocateExtent(0, 8192);
  write_failure = false;
  check(write, "SB-STORAGE-DISK-WRITE-FAILED", "storage.disk.write_failed");
  size_fail_after = 0;
  const auto size = f.device.PreallocateExtent(0, 8192);
  size_fail_after = -1;
  check(size, "SB-STORAGE-DISK-SIZE-FAILED", "storage.disk.size_failed");
  Require(f.Bytes() == f.original, "diagnostic failures damaged fixture");
}
}  // namespace

// Link-time wrappers change only error outcomes. Every positive allocation/read/
// write/sync remains the real production native call, not a simulated provider.
extern "C" int __real_posix_fallocate(int, off_t, off_t);
extern "C" int __wrap_posix_fallocate(int fd, off_t offset, off_t length) {
  CheckDeviceGuard();
  ++allocation_calls;
  return allocation_error ? allocation_error : __real_posix_fallocate(fd, offset, length);
}
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __wrap_fstat(int fd, struct stat* state) {
  CheckDeviceGuard();
  if (size_fail_after == 0) { errno = EIO; return -1; }
  if (size_fail_after > 0) --size_fail_after;
  return __real_fstat(fd, state);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t bytes, off_t offset) {
  CheckDeviceGuard();
  if (write_failure) { errno = ENOSPC; return -1; }
  return __real_pwrite(fd, data, bytes, offset);
}

int main() {
  int failures = 0;
  const auto run = [&](const char* name, auto test) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
  };
  run("unsupported-contained", TestUnsupportedContained);
  run("native-contained", TestNativeContained);
  run("native-growth-retry-reopen", [] { TestGrowthAndRetry(0); });
  run("unsupported-growth-retry-reopen", [] { TestGrowthAndRetry(EOPNOTSUPP); });
  run("unavailable-growth-retry-reopen", [] { TestGrowthAndRetry(ENOSYS); });
  run("failure-boundaries", TestFailuresAndBoundaries);
  run("applied-effect-on-error", TestAppliedNativeEffectOnObservationFailure);
  run("every-profile-byte-extent", TestEveryProfileExtent);
  run("retained-device-serialization", TestRetainedDeviceSerialization);
  run("diagnostic-vectors", TestDiagnosticVectors);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
