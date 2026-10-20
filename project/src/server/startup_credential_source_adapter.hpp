// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "startup_credential_frame.hpp"

#include <cstdint>
#include <span>
#include <string_view>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace scratchbird::server {

// Writable side of the startup credential source: builds the sealed custody
// object `ImportStartupCredentialTransport` requires, so a configured secret can
// be published into a `StartupCredentialSource`.
//
// The transport accepts only an anonymous, sealed, owner-read-only regular file
// owned by the exact custodian: `st_nlink == 0`, `(st_mode & 07777) == 0400`,
// `st_uid == *binding.custodian_uid`, with seals readable through `F_GET_SEALS`.
// Nothing in the tree produced such an object, which is why the handoff recorded
// the native writable source as pending. This produces it and nothing else.
//
// Explicitly NOT in scope, matching every component it composes with: this issues
// no custodian admission, no authentication, no provider activation, no
// disclosure right and no BEGIN grant. A sealed frame is transport custody of
// bytes the caller already held. Publishing one proves only that the bytes were
// carried under custody; the engine still authenticates, and the committed
// catalog still decides.
//
// The caller owns the secret. This neither generates, derives, persists nor logs
// it, and writes it to no filesystem path: the object is anonymous and dies with
// the last descriptor. A credential containing a NUL is refused by the frame
// encoder rather than silently truncated.

enum class StartupCredentialSourceAdapterError {
  none,
  invalid_request,
  unsupported_platform,
  frame,
  create_failed,
  write_failed,
  protection_failed,
  seal_failed,
};

struct StartupCredentialSourceAdapterResult {
  StartupCredentialSourceAdapterError error =
      StartupCredentialSourceAdapterError::invalid_request;
  // Owned on success; the caller publishes with it and then closes it. Negative
  // on every failure, so a refusal can never leak a usable descriptor.
  int fd = -1;
  int native_error = 0;
  StartupCredentialFrameError frame_error = StartupCredentialFrameError::none;

  bool ok() const noexcept {
    return error == StartupCredentialSourceAdapterError::none && fd >= 0;
  }
};

// Build a sealed custody descriptor carrying `credential` framed against
// `binding`. On success the descriptor satisfies the transport's custody
// predicate; on any failure the descriptor is closed and the frame buffer
// zeroized before returning.
inline StartupCredentialSourceAdapterResult CreateSealedStartupCredentialSource(
    const StartupCredentialTransportBinding& binding,
    std::string_view credential) noexcept {
  using E = StartupCredentialSourceAdapterError;
  StartupCredentialSourceAdapterResult result;
#if !defined(__linux__)
  (void)binding;
  (void)credential;
  result.error = E::unsupported_platform;
  return result;
#else
  if (credential.empty() ||
      credential.size() > kStartupCredentialMaximumBytes ||
      !binding.custodian_uid.has_value()) {
    result.error = E::invalid_request;
    return result;
  }
  // The transport requires the owner to be the exact custodian. Refuse here
  // rather than creating an object that can only fail import.
  if (*binding.custodian_uid != static_cast<std::uint32_t>(::geteuid())) {
    result.error = E::invalid_request;
    return result;
  }

  const std::size_t frame_bytes =
      kStartupCredentialHeaderBytes + credential.size();
  std::uint8_t frame[kStartupCredentialHeaderBytes +
                     kStartupCredentialMaximumBytes]{};
  const auto zeroize = [&]() noexcept {
    ::explicit_bzero(frame, sizeof(frame));
  };
  const auto framed = EncodeStartupCredentialFrame(
      binding,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(credential.data()),
          credential.size()),
      std::span<std::uint8_t>(frame, frame_bytes));
  if (framed != StartupCredentialFrameError::none) {
    zeroize();
    result.error = E::frame;
    result.frame_error = framed;
    return result;
  }

  // Anonymous and sealable: never reachable by path, so it cannot be opened by
  // another process or survive the last descriptor.
  const int fd = ::memfd_create("sb-startup-credential", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    zeroize();
    result.error = E::create_failed;
    result.native_error = errno;
    return result;
  }
  const auto fail = [&](E error) noexcept {
    result.error = error;
    result.native_error = errno;
    ::close(fd);
    zeroize();
    return result;
  };

  std::size_t written = 0;
  while (written < frame_bytes) {
    const auto chunk =
        ::write(fd, frame + written, frame_bytes - written);
    if (chunk <= 0) {
      if (chunk < 0 && errno == EINTR) {
        continue;
      }
      return fail(E::write_failed);
    }
    written += static_cast<std::size_t>(chunk);
  }
  zeroize();

  // Exactly owner-read-only, as the transport's custody predicate requires.
  if (::fchmod(fd, 0400) != 0) {
    return fail(E::protection_failed);
  }
  // Seal before handing the descriptor anywhere. F_SEAL_WRITE makes the frame
  // immutable, F_SEAL_SHRINK and F_SEAL_GROW fix its extent, and F_SEAL_SEAL
  // prevents a later holder from removing any of them.
  if (::fcntl(fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
    return fail(E::seal_failed);
  }

  result.error = E::none;
  result.fd = fd;
  return result;
#endif
}

}  // namespace scratchbird::server
