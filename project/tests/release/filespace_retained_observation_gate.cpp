// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "disk_device.hpp"
#include "filespace_page_zero.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
namespace disk = scratchbird::storage::disk;
pthread_mutex_t* observed_mutex = nullptr;
thread_local bool probing = false;
unsigned unlocks = 0, admissions = 0, probe_errors = 0;
unsigned reads = 0, sizes = 0, fail_read = 0, fail_size = 0;

void Require(bool ok, const char* detail) {
  if (!ok) throw std::runtime_error(detail);
}
disk::Uuid Id(unsigned char tag) {
  return {{{1, 2, 3, 4, 5, 6, 0x71, 8, 0x89, 10, 11, 12, 13, 14, 15, tag}}};
}

// This fixture qualifies only a non-serving native metadata observation. It
// does not claim actual allocation-map roots, initialization or MGA admission.
struct Fixture {
  std::filesystem::path root, path;
  disk::FileDevice device;
  disk::FilespacePageZero zero;
  disk::FilespaceBootstrapBinding binding;
  std::vector<disk::byte> image;
  explicit Fixture(const disk::CanonicalFilespacePageProfile& profile) {
    zero.bootstrap.database_uuid = Id(1);
    zero.bootstrap.filespace_uuid = Id(2);
    zero.bootstrap.page_size_profile_uuid = profile.uuid;
    zero.bootstrap.page_size_bytes = profile.page_size_bytes;
    zero.bootstrap.checksum_profile_uuid = disk::kNativeBootstrapIntegrityProfile;
    zero.bootstrap.filespace_role = 5;
    zero.bootstrap.lifecycle_state = 7;
    zero.page_uuid = Id(3);
    zero.creation_operation_uuid = Id(4);
    zero.writer_identity_uuid = Id(5);
    zero.page_generation = zero.root_set_generation = 1;
    zero.total_pages = 2;
    zero.creation_utc_millis = 1790000000000ull;
    binding = {Id(1), Id(2), profile.uuid};
    const auto encoded = disk::EncodeFilespacePageZero(zero);
    Require(encoded.ok(), "native fixture encoding");
    image = *encoded.bytes;
    image.resize(profile.page_size_bytes * 2, 0x5a);
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "sb-01e-observation-XXXXXX").string();
    const auto made = ::mkdtemp(pattern.data());
    Require(made != nullptr, "mkdtemp");
    root = made;
    path = root / "filespace.bin";
    try {
      Require(device.Open(path.string(), disk::FileOpenMode::create_new).ok(), "create");
      Require(device.WriteAt(0, image.data(), image.size()).ok(), "seed actual image");
      Require(device.Sync().ok(), "seed sync");
    } catch (...) { Cleanup(); throw; }
  }
  void Cleanup() {
    observed_mutex = nullptr;
    fail_read = fail_size = 0;
    (void)device.Close();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  ~Fixture() { Cleanup(); }
  void Unchanged() const {
    std::ifstream file(path, std::ios::binary);
    const std::vector<disk::byte> actual{std::istreambuf_iterator<char>(file), {}};
    Require(actual == image, "observation changed real file contents");
  }
};

struct Observation {
  explicit Observation(disk::FileDevice& device) {
    auto guard = device.AcquireOperationGuard();
    auto* native = guard.mutex()->native_handle();
    guard.unlock();
    unlocks = admissions = probe_errors = reads = sizes = 0;
    observed_mutex = native;
  }
  ~Observation() { observed_mutex = nullptr; fail_read = fail_size = 0; }
  void Check(unsigned expected_admissions = 1) {
    observed_mutex = nullptr;
    Require(unlocks > expected_admissions, "compound operation not observed");
    Require(probe_errors == 0, "unexpected native mutex result");
    // The sole admission is the final outer unlock, after constructing the
    // result. All intermediate unlocks must retain the compound outer guard.
    Require(admissions == expected_admissions, "device admitted a competing owner between primitive calls");
  }
};

void Profile(const disk::CanonicalFilespacePageProfile& profile) {
  Fixture f(profile);
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespaceBootstrapFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(result.ok() && result.preamble->filespace_uuid == Id(2) &&
            result.preamble->page_size_bytes == profile.page_size_bytes, "bootstrap result");
    Require(reads == 1, "bootstrap read count");
  }
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespacePageZeroFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(result.ok() && result.record->page_uuid == Id(3) &&
            result.record->creation_operation_uuid == Id(4) &&
            result.record->writer_identity_uuid == Id(5) && result.record->total_pages == 2,
            "full page-zero binary identities/capacity");
    Require(reads == 2 && sizes == 2, "full page-zero observation count");
  }
  // Every native read/size failure retains the guard to result construction.
  {
    Observation probe(f.device);
    const auto result = disk::ObserveFilespacePageZeroForRecoveryFromOpenDevice(f.device, f.binding);
    probe.Check();
    Require(result.ok() && result.relation == disk::FilespaceExtentRelation::matching &&
            result.observed_bytes == f.image.size() && result.declared_bytes == f.image.size() &&
            result.complete_pages == 2 && result.trailing_bytes == 0 && reads == 2 && sizes == 2,
            "recovery evidence retains compound observation guard");
  }
  for (unsigned position : {1u, 2u}) {
    for (bool reading : {false, true}) {
      Observation probe(f.device);
      (reading ? fail_read : fail_size) = position;
      const auto result = disk::ObserveFilespacePageZeroForRecoveryFromOpenDevice(f.device, f.binding);
      probe.Check();
      Require(!result.ok() && result.error == disk::FilespacePageZeroError::io_failure &&
              !result.record && !result.observed_bytes && !result.declared_bytes &&
              !result.complete_pages && !result.trailing_bytes &&
              result.relation == disk::FilespaceExtentRelation::unknown,
              "recovery I/O failure preserves guard and withholds partial facts");
    }
  }
  {
    auto outer = f.device.AcquireOperationGuard();
    Observation probe(f.device);
    const auto result = disk::ObserveFilespacePageZeroForRecoveryFromOpenDevice(f.device, f.binding);
    probe.Check(0);
    Require(result.ok(), "nested recovery observation preserves caller guard");
  }
  for (unsigned position : {1u, 2u}) {
    for (bool reading : {false, true}) {
      Observation probe(f.device);
      (reading ? fail_read : fail_size) = position;
      const auto result = disk::ReadFilespacePageZeroFromOpenDevice(f.device, &f.binding);
      probe.Check();
      Require(!result.ok() && result.error == disk::FilespacePageZeroError::io_failure &&
              !result.record, "native observation failure classification");
    }
  }
  {
    Observation probe(f.device);
    fail_read = 1;
    const auto result = disk::ReadFilespaceBootstrapFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(!result.ok() && result.error == disk::FilespaceBootstrapError::io_failure &&
            !result.preamble, "bootstrap native failure classification");
  }
  {
    auto outer = f.device.AcquireOperationGuard();
    Observation probe(f.device);
    const auto result = disk::ReadFilespacePageZeroFromOpenDevice(f.device, &f.binding);
    probe.Check(0);
    Require(result.ok(), "nested compound observation");
  }
  auto wrong = f.binding;
  wrong.filespace_uuid = Id(99);
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespacePageZeroFromOpenDevice(f.device, &wrong);
    probe.Check();
    Require(!result.ok() && result.error == disk::FilespacePageZeroError::invalid_bootstrap,
            "binding mismatch classification");
  }
  f.Unchanged();
  Require(f.device.Close().ok(), "close for read-only observation");
  Require(f.device.Open(f.path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
          "read-only observation open");
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespaceBootstrapFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(result.ok(), "read-only bootstrap");
  }
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespacePageZeroFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(result.ok(), "read-only page zero");
  }
  Require(f.device.Close().ok(), "close read-only observation");
  Require(f.device.Open(f.path.string(), disk::FileOpenMode::open_existing).ok(),
          "reopen for negative fixtures");
  // Corrupt actual media, not a successful mocked read. Preserve that exact
  // corrupt image through the refusal and verify it independently afterward.
  f.image[4448] ^= 1;  // Stored full-page digest, not reserved padding.
  Require(f.device.WriteAt(0, f.image.data(), f.image.size()).ok(), "corrupt full-page fixture");
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespacePageZeroFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(!result.ok() && result.error == disk::FilespacePageZeroError::integrity_mismatch,
            "full-page corruption classification");
  }
  {
    Observation probe(f.device);
    const auto result = disk::ObserveFilespacePageZeroForRecoveryFromOpenDevice(f.device, f.binding);
    probe.Check();
    Require(!result.ok() && result.error == disk::FilespacePageZeroError::integrity_mismatch &&
            !result.record && !result.observed_bytes,
            "corrupt recovery observation holds guard without exposing capacity");
  }
  f.Unchanged();
  f.image[0] ^= 1;
  Require(f.device.WriteAt(0, f.image.data(), f.image.size()).ok(), "corrupt bootstrap fixture");
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespaceBootstrapFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(!result.ok() && result.error == disk::FilespaceBootstrapError::invalid_framing,
            "bootstrap corruption classification");
  }
  f.Unchanged();
  Require(f.device.Close().ok(), "close");
  {
    Observation probe(f.device);
    const auto result = disk::ReadFilespacePageZeroFromOpenDevice(f.device, &f.binding);
    probe.Check();
    Require(!result.ok() && result.error == disk::FilespacePageZeroError::device_not_open &&
            !reads && !sizes, "closed device reached native I/O");
  }
  Require(f.device.Open(f.path.string(), disk::FileOpenMode::open_existing_read_only).ok(), "reopen");
  f.Unchanged();
}
}  // namespace

extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  const int result = __real_pthread_mutex_unlock(mutex);
  if (!result && mutex == observed_mutex && !probing) {
    ++unlocks;
    // A real competing thread attempts admission after EACH primitive unlock,
    // not only while its native read happens. No sleeps or timing assertions.
    std::thread contender([&] {
      probing = true;
      const int acquired = pthread_mutex_trylock(mutex);
      if (!acquired) { ++admissions; __real_pthread_mutex_unlock(mutex); }
      else if (acquired != EBUSY) ++probe_errors;
    });
    contender.join();
  }
  return result;
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t size, off_t offset) {
  if (observed_mutex && ++reads == fail_read) { errno = EIO; return -1; }
  return __real_pread(fd, data, size, offset);
}
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __wrap_fstat(int fd, struct stat* state) {
  if (observed_mutex && ++sizes == fail_size) { errno = EIO; return -1; }
  return __real_fstat(fd, state);
}
int main() {
  unsigned failures = 0;
  for (const auto& profile : disk::kCanonicalFilespacePageProfiles) {
    try { Profile(profile); std::cout << "PASS retained profile " << profile.page_size_bytes << '\n'; }
    catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL retained profile " << profile.page_size_bytes << ": " << error.what() << '\n';
    }
  }
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
