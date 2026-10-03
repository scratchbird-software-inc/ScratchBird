// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/startup_credential_transport.hpp"
#include <sys/mman.h>
#include <array>
#include <dirent.h>
#include <iostream>
#include <vector>
namespace srv = scratchbird::server;
namespace mem = scratchbird::core::memory;
using E = srv::StartupCredentialTransportError;
namespace {
unsigned checks = 0, failures = 0, zeroized = 0;
bool partial_payload = false, fail_lock = false;
void* watched = nullptr;
std::size_t page_size = 0;
void Check(bool value, const char* label) {
  ++checks; if (!value) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
struct Fd {
  int value = -1;
  Fd(const Fd&) = delete;
  explicit Fd(int fd) : value(fd) {}
  ~Fd() { if (value >= 0) ::close(value); }
};
using Bytes = std::vector<unsigned char>;
unsigned DescriptorCount() {
  auto* directory = ::opendir("/proc/self/fd");
  if (!directory) throw std::runtime_error("descriptor inventory unavailable");
  unsigned count = 0;
  while (::readdir(directory)) ++count;
  ::closedir(directory);
  return count;
}
void Put(Bytes& bytes, std::size_t at, std::size_t count, std::uint64_t value) {
  while (count) { bytes[at + --count] = value & 255; value >>= 8; }
}
int Source(const Bytes& bytes, int seals, mode_t mode = 0400) {
  Fd fd(::memfd_create("startup-credential-test", MFD_CLOEXEC | MFD_ALLOW_SEALING));
  Check(fd.value >= 0, "actual memfd creation");
  if (fd.value < 0) return -1;
  Check(::write(fd.value, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "fixture provisioning");
  Check(::fchmod(fd.value, mode) == 0, "actual owner-only access mode");
  Check(::fcntl(fd.value, F_ADD_SEALS, seals) == 0, "actual kernel seals");
  const int owned = fd.value; fd.value = -1; return owned;
}
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" int __real_munlock(const void*, size_t);
extern "C" int __real_mlock(const void*, size_t);
extern "C" ssize_t __wrap_pread(int fd, void* p, size_t n, off_t offset) {
  if (offset == 88) {
    watched = p;
    if (partial_payload) { partial_payload = false; return __real_pread(fd, p, 1, offset); }
  }
  return __real_pread(fd, p, n, offset);
}
extern "C" int __wrap_munlock(const void* p, size_t n) {
  if (p == watched) {
    bool erased = n == page_size;
    for (size_t i = 0; i < page_size; ++i) erased &= static_cast<const unsigned char*>(p)[i] == 0;
    Check(erased, "actual imported payload zeroized before native unlock");
    watched = nullptr; ++zeroized;
  }
  return __real_munlock(p, n);
}
extern "C" int __wrap_mlock(const void* p, size_t n) {
  if (fail_lock) { fail_lock = false; errno = EPERM; return -1; }
  return __real_mlock(p, n);
}
int main() {
  page_size = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
  constexpr int all_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
  srv::StartupCredentialTransportBinding binding;
  binding.generation = 7; binding.custodian_uid = ::geteuid();
  std::array ids{&binding.database, &binding.principal, &binding.provider, &binding.credential_reference};
  Bytes bytes(88 + 7);
  std::memcpy(bytes.data(), "SBACRED1", 8);
  for (size_t i = 0; i < ids.size(); ++i) {
    ids[i]->bytes[0] = 1; ids[i]->bytes[6] = 0x70; ids[i]->bytes[8] = 0x80; ids[i]->bytes[15] = i + 1;
    std::memcpy(bytes.data() + 8 + i * 16, ids[i]->bytes.data(), 16);
  }
  Put(bytes, 72, 8, 7); Put(bytes, 80, 4, 1); Put(bytes, 84, 4, 7);
  std::memcpy(bytes.data() + 88, "fixture", 7); // Test evidence, not an authenticated credential.
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.hard_limit_bytes = page_size * 2; policy.per_context_limit_bytes = page_size * 2;
  policy.soft_limit_bytes = 0;
  mem::MemoryManager manager(policy);
  mem::MemoryTag tag;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = binding.database.bytes;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = binding.principal.bytes;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.database.bytes;
  const auto import = [&](int fd, const auto& expected) {
    return srv::ImportStartupCredentialTransport(fd, expected, manager, tag);
  };
  Fd valid(Source(bytes, all_seals));
  const auto descriptors_before = DescriptorCount();
  Check(::pwrite(valid.value, bytes.data(), 1, 0) == -1,
        "actual kernel refuses sealed payload mutation");
  Check(::ftruncate(valid.value, 1) == -1, "actual kernel refuses sealed truncation");
  Check(::lseek(valid.value, 3, SEEK_SET) == 3, "fixture original offset");
  {
    auto result = import(valid.value, binding);
    Check(result.ok() && result.credential() == "fixture", "actual immutable payload imported");
    Check(manager.Snapshot().current_bytes == page_size, "full page charged to real governor");
    Check(result.buffer.evidence().platform_lock_succeeded && result.buffer.evidence().no_dump_succeeded,
          "strict actual platform protections retained");
    auto moved = std::move(result);
    Check(!result.ok() && moved.ok(), "transport buffer has single move owner");
  }
  Check(!watched && zeroized == 1 && manager.Snapshot().current_bytes == 0, "successful release");
  Check(::lseek(valid.value, 0, SEEK_CUR) == 3 && ::fcntl(valid.value, F_GETFD) >= 0,
        "borrowed descriptor and original offset preserved");
  Check(import(-1, binding).error == E::invalid_request, "absent descriptor refused");
  auto wrong_tag = tag;
  wrong_tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.provider.bytes;
  Check(srv::ImportStartupCredentialTransport(valid.value, binding, manager, wrong_tag).error ==
        E::invalid_request, "cross-database memory charge refused");
  {
    Fd ordinary(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    Check(import(ordinary.value, binding).error == E::source_protection,
          "ordinary nonsealed descriptor refused");
  }
  auto wrong = binding; wrong.custodian_uid.reset();
  Check(import(valid.value, wrong).error == E::invalid_request, "no implicit custodian UID");
  wrong = binding; wrong.generation = 8;
  Check(import(valid.value, wrong).error == E::binding_mismatch, "stale source generation refused");
  wrong = binding; wrong.custodian_uid = ::geteuid() == 0 ? 1 : 0;
  Check(import(valid.value, wrong).error == E::source_protection, "wrong OS owner refused");
  for (size_t i = 8; i < 72; ++i) {
    auto changed = bytes; changed[i] ^= 1;
    Fd source(Source(changed, all_seals));
    Check(import(source.value, binding).error == E::binding_mismatch, "every binary identity octet checked");
  }
  for (int seal : {F_SEAL_WRITE, F_SEAL_GROW, F_SEAL_SHRINK, F_SEAL_SEAL}) {
    Fd source(Source(bytes, all_seals & ~seal));
    Check(import(source.value, binding).error == E::source_protection, "each required seal enforced");
  }
  for (mode_t mode : {0600, 0440, 0404, 0500}) {
    Fd source(Source(bytes, all_seals, mode));
    Check(import(source.value, binding).error == E::source_protection, "unsafe access mode refused");
  }
  for (unsigned kind = 0; kind < 6; ++kind) {
    auto changed = bytes;
    if (kind == 0) changed[0] = 0;
    if (kind == 1) Put(changed, 80, 4, 2);
    if (kind == 2) Put(changed, 84, 4, 0);
    if (kind == 3) changed.pop_back();
    if (kind == 4) changed.push_back(42);
    if (kind == 5) changed[90] = 0;
    Fd source(Source(changed, all_seals));
    Check(import(source.value, binding).error == E::invalid_frame, "malformed frame refused");
    Check(manager.Snapshot().current_bytes == 0, "invalid frame retains no protected charge");
  }
  partial_payload = true;
  Check(import(valid.value, binding).error == E::source_io && !partial_payload && !watched,
        "actual partial secret read erased on failure");
  fail_lock = true;
  Check(import(valid.value, binding).error == E::protected_memory_unavailable && !fail_lock,
        "OS protection failure cannot fall back to ordinary memory");
  for (size_t size : {1u, 1024u, 1025u}) {
    auto bounded = bytes; bounded.resize(88 + size, 'x');
    Put(bounded, 84, 4, size);
    Fd source(Source(bounded, all_seals));
    auto read = import(source.value, binding);
    Check(size <= 1024 ? read.ok() && read.credential().size() == size
                       : read.error == E::invalid_frame,
          "exact credential length boundaries");
  }
  Check(manager.Snapshot().current_bytes == 0 && manager.Snapshot().active_allocation_count == 0,
        "failure paths retain no buffer or charge");
  Check(DescriptorCount() == descriptors_before, "every transport duplicate closed");
  std::cout << checks << " transport checks; failures=" << failures << '\n';
  return failures ? 1 : 0;
}
