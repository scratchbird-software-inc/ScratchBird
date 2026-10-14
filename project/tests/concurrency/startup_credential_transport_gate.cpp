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
  {
    using F = srv::StartupCredentialFrameError;
    mem::ProtectedMemoryRequest request;
    request.bytes = page_size; request.alignment = page_size; request.tag = tag;
    request.material_class = "startup_credential_frame_test";
    request.platform_policy = mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto staging = manager.AllocateProtected(std::move(request));
    Check(staging.ok(), "frame encoder uses actual governed protected staging");
    if (!staging.ok()) return 1;
    auto* storage = static_cast<std::uint8_t*>(staging.buffer.data());
    const auto secret = std::span<const std::uint8_t>(bytes).subspan(88);
    auto output = std::span<std::uint8_t>(storage, bytes.size());
    Check(srv::EncodeStartupCredentialFrame(binding, secret, output) == F::none &&
          std::memcmp(output.data(), bytes.data(), bytes.size()) == 0,
          "encoder matches independently constructed exact wire bytes");
    Check(srv::InspectStartupCredentialFrame(output, binding).ok(), "complete encoded frame inspection");
    auto changed_binding = binding;
    changed_binding.custodian_uid.reset();
    Check(srv::InspectStartupCredentialFrame(output, changed_binding).ok(),
          "OS custodian is not invented as a wire field or authority");
    for (std::size_t byte = 8; byte < 80; ++byte) {
      storage[byte] ^= 1;
      Check(srv::InspectStartupCredentialFrame(output, binding).error == F::binding_mismatch,
            "complete frame checks every binary identity and generation byte");
      storage[byte] ^= 1;
    }
    const auto untouched = [&] {
      for (std::size_t i = 0; i < bytes.size(); ++i) if (storage[i] != 0xa5) return false;
      return true;
    };
    std::memset(storage, 0xa5, bytes.size());
    changed_binding.generation = 0;
    Check(srv::EncodeStartupCredentialFrame(changed_binding, secret, output) == F::invalid_binding && untouched(),
          "invalid generation leaves output untouched");
    for (auto identity : {&changed_binding.database, &changed_binding.principal,
                          &changed_binding.provider, &changed_binding.credential_reference}) {
      changed_binding = binding;
      identity->bytes[6] = 0x40;
      Check(srv::EncodeStartupCredentialFrame(changed_binding, secret, output) == F::invalid_binding && untouched(),
            "non-system UUID encoder refusal preserves output");
    }
    const std::array<std::uint8_t, 3> nul_secret{1, 0, 2};
    Check(srv::EncodeStartupCredentialFrame(binding, nul_secret, output.first(91)) == F::invalid_credential && untouched(),
          "embedded NUL encoder refusal preserves output");
    Check(srv::EncodeStartupCredentialFrame(binding, secret, output.first(output.size() - 1)) == F::invalid_extent && untouched(),
          "short output encoder refusal preserves output");
    Check(srv::EncodeStartupCredentialFrame(binding, {}, output.first(88)) == F::invalid_extent && untouched(),
          "empty credential encoder refusal preserves output");
    for (std::size_t offset : {0u, 1u, 87u, 88u}) {
      Check(srv::EncodeStartupCredentialFrame(binding, {storage + offset, 7}, output) == F::invalid_extent && untouched(),
            "overlap at header or payload cannot corrupt staging");
    }
    for (std::size_t size : {1u, 1024u, 1025u}) {
      Bytes bounded(size, 'x');
      auto region = std::span<std::uint8_t>(storage, 88 + size);
      std::memset(storage, 0xa5, region.size());
      auto high_generation = binding;
      high_generation.generation = UINT64_MAX;
      const auto encoded = srv::EncodeStartupCredentialFrame(high_generation, bounded, region);
      if (size <= 1024) {
        Check(encoded == F::none && srv::InspectStartupCredentialFrame(region, high_generation).credential_bytes == size,
              "minimum and maximum body with maximum generation roundtrip");
        bool exact = true;
        for (std::size_t i = 72; i < 80; ++i) exact &= storage[i] == 255;
        Check(exact && storage[80] == 0 && storage[81] == 0 && storage[82] == 0 && storage[83] == 1 &&
              storage[84] == 0 && storage[85] == 0 && storage[86] == (size >> 8) && storage[87] == (size & 255),
              "numeric fields use exact unsigned big-endian bytes");
        storage[88] = 0;
        Check(srv::InspectStartupCredentialFrame(region, high_generation).error == F::invalid_credential,
              "header acceptance cannot certify a malformed body");
      } else {
        bool preserved = true;
        for (auto byte : region) preserved &= byte == 0xa5;
        Check(encoded == F::invalid_extent && preserved, "oversize body cannot change output");
      }
    }
    for (std::size_t size = 0; size < bytes.size(); ++size)
      Check(!srv::InspectStartupCredentialFrame(std::span<const std::uint8_t>(bytes).first(size), binding).ok(),
            "all truncations refused by complete-frame inspector");
    for (auto at : {0u, 7u, 80u, 83u, 84u, 87u, 88u}) {
      auto malformed = bytes; malformed[at] = 0;
      if (malformed == bytes) malformed[at] = 2;
      Check(!srv::InspectStartupCredentialFrame(malformed, binding).ok(),
            "malformed magic purpose length and body refused");
    }
  }
  Check(manager.Snapshot().current_bytes == 0, "frame staging releases actual memory grant");
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
