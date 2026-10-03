// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "memory.hpp"
#include "uuid.hpp"
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::server {
// Private credential transport, NOT source admission, provider authentication,
// generation/revocation fencing or startup permission. The provisioner must
// separately retain its admitted source fence through BEGIN. A sealed copy
// cannot establish current source authority. No implicit FD/environment lookup.
struct StartupCredentialTransportBinding {
  core::platform::Uuid database, principal, provider, credential_reference;
  std::uint64_t generation = 0;
  std::optional<std::uint32_t> custodian_uid;
};
enum class StartupCredentialTransportError {
  none, invalid_request, unsupported_platform, source_io, source_protection,
  invalid_frame, binding_mismatch, protected_memory_unavailable
};
struct StartupCredentialTransportResult {
  StartupCredentialTransportError error = StartupCredentialTransportError::invalid_request;
  int native_error = 0; // Numeric OS error only; never payload/path/secret detail.
  core::memory::ScopedProtectedBuffer buffer;
  std::size_t credential_size = 0;
  bool ok() const noexcept { return error == StartupCredentialTransportError::none && buffer.valid(); }
  std::string_view credential() const noexcept {
    return ok() ? std::string_view(static_cast<const char*>(buffer.data()), credential_size)
                : std::string_view{};
  }
};

// Caller keeps manager alive until result destruction and prevents concurrent
// close/reuse of borrowed_fd until duplication. The duplicate is always closed;
// the caller's FD and file offset are unchanged. Payload is read directly into
// full-page governed locked/no-dump storage, never an ordinary temporary string.
inline StartupCredentialTransportResult ImportStartupCredentialTransport(
    int borrowed_fd, const StartupCredentialTransportBinding& binding,
    core::memory::MemoryManager& manager, const core::memory::MemoryTag& tag) noexcept {
  using E = StartupCredentialTransportError;
  StartupCredentialTransportResult result;
  try {
    const std::array identities{binding.database, binding.principal,
                                binding.provider, binding.credential_reference};
    if (borrowed_fd < 0 || !binding.custodian_uid || !binding.generation ||
        tag.binary_ownership.empty() || !core::memory::MemoryBinaryOwnershipValid(tag) ||
        tag.binary_ownership[core::memory::MemoryBinaryScopeKind::database] != binding.database.bytes)
      return result;
    for (const auto& id : identities) if (!core::uuid::IsEngineIdentityUuid(id)) return result;
#if defined(__linux__)
    struct Descriptor {
      int fd;
      ~Descriptor() { if (fd >= 0) ::close(fd); }
    } owned{::fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 0)};
    result.error = E::source_io;
    if (owned.fd < 0) { result.native_error = errno; return result; }
    struct stat state{};
    if (::fstat(owned.fd, &state) != 0) { result.native_error = errno; return result; }
    constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    const int seals = ::fcntl(owned.fd, F_GET_SEALS);
    if (seals < 0) { result.error = E::source_protection; result.native_error = errno; return result; }
    result.error = E::source_protection;
    if (!S_ISREG(state.st_mode) || state.st_nlink != 0 ||
        state.st_uid != *binding.custodian_uid || (state.st_mode & 07777) != 0400 ||
        (seals & required_seals) != required_seals) return result;
    result.error = E::invalid_frame;
    constexpr std::size_t header_size = 88;
    if (state.st_size < static_cast<off_t>(header_size + 1) ||
        state.st_size > static_cast<off_t>(header_size + 1024)) return result;
    std::array<unsigned char, header_size> header{};
    if (::pread(owned.fd, header.data(), header.size(), 0) != static_cast<ssize_t>(header.size())) {
      result.error = E::source_io; return result;
    }
    if (std::memcmp(header.data(), "SBACRED1", 8) != 0) return result;
    const auto integer = [&](std::size_t offset, std::size_t count) {
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < count; ++i) value = (value << 8) | header[offset + i];
      return value;
    };
    const auto size = integer(84, 4);
    if (integer(80, 4) != 1 || !size || size > 1024 ||
        state.st_size != static_cast<off_t>(header_size + size)) return result;
    result.error = E::binding_mismatch;
    for (std::size_t i = 0; i < identities.size(); ++i)
      if (std::memcmp(header.data() + 8 + 16 * i, identities[i].bytes.data(), 16)) return result;
    if (integer(72, 8) != binding.generation) return result;
    const long native_page = ::sysconf(_SC_PAGESIZE);
    result.error = E::protected_memory_unavailable;
    if (native_page <= 0) return result;
    const auto page = static_cast<std::size_t>(native_page);
    if ((page & (page - 1)) || page < size) return result;
    core::memory::ProtectedMemoryRequest request;
    request.bytes = page; request.alignment = page; request.tag = tag;
    request.material_class = "startup_credential";
    request.platform_policy = core::memory::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto allocated = manager.AllocateProtected(std::move(request));
    if (!allocated.ok()) return result;
    result.error = E::source_io;
    if (::pread(owned.fd, allocated.buffer.data(), size, header_size) != static_cast<ssize_t>(size))
      return result; // RAII zeroizes even a partial read.
    result.error = E::invalid_frame;
    if (std::memchr(allocated.buffer.data(), 0, size)) return result;
    result.buffer = std::move(allocated.buffer);
    result.credential_size = size;
    result.error = E::none;
#else
    (void)manager;
    result.error = E::unsupported_platform;
#endif
  } catch (const std::bad_alloc&) {
    result.error = E::protected_memory_unavailable;
  } catch (const std::length_error&) {
    result.error = E::protected_memory_unavailable;
  } catch (const std::system_error&) {
    result.error = E::protected_memory_unavailable;
  }
  return result;
}
} // namespace scratchbird::server
