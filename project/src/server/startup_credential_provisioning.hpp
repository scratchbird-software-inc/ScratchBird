// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "startup_credential_source.hpp"
#include "startup_credential_source_adapter.hpp"

#include <string>
#include <string_view>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#endif

namespace scratchbird::server {

// Obtains the operator-provisioned startup secret and publishes it into a
// StartupCredentialSource as a sealed custody object.
//
// Startup does not own this secret and MUST NOT invent one. Provisioning is a
// separate operator role: this reads what the operator placed, validates its
// custody, seals it and publishes it. There is no generation, derivation,
// default or fallback value anywhere in this path. An absent or untrustworthy
// secret is a refusal, never a quietly uncredentialed start.
//
// Two sources are supported, selected by policy because where to authenticate
// is a configuration decision, not a built-in assumption:
//
//   service_manager  $CREDENTIALS_DIRECTORY/<name>, the descriptor-passing
//                    mechanism a service manager provides (systemd
//                    LoadCredential= and equivalents). Preferred: the secret
//                    never occupies a path ScratchBird chose or controls, and
//                    the service manager restricts it to the service user.
//   operator_file    An explicit path the operator provisioned, which must be
//                    a regular file owned by this process's effective user with
//                    no group or other permission bits.
//
// An environment variable is deliberately not a supported source. A credential
// in the environment is readable through /proc and process listings, which
// makes it indefensible regardless of how carefully it is handled afterwards.
//
// Scope: this publishes transport custody of a secret. It performs no
// authentication, activates no provider, admits no session and grants no right.
// Publishing a credential is not verifying one; the engine still authenticates
// against the committed catalog.

enum class StartupCredentialProvisionSource {
  not_configured,
  service_manager,
  operator_file,
};

enum class StartupCredentialProvisionError {
  none,
  not_configured,       // No source selected. Refusal, not a default.
  unsupported_platform,
  no_service_credentials,  // Selected service_manager, but none was passed.
  unreadable,
  // Custody is three independent conditions, reported separately. A single
  // combined custody_rejected would make a test that accepts the right file
  // unable to prove which condition did the refusing, and a dropped conjunct
  // would stay invisible behind the remaining ones.
  custody_rejected_type,   // Not a regular file.
  custody_rejected_owner,  // Not owned by this effective user.
  custody_rejected_mode,   // Readable or writable beyond its owner.
  empty,
  oversized,
  seal_failed,
  publish_failed,
};

struct StartupCredentialProvisionRequest {
  StartupCredentialProvisionSource source =
      StartupCredentialProvisionSource::not_configured;
  // service_manager: the credential name within the service manager's
  // credentials directory. operator_file: the absolute path.
  std::string name_or_path;
  StartupCredentialTransportBinding binding;
  StartupCredentialSource::Request publish;
};

struct StartupCredentialProvisionResult {
  StartupCredentialProvisionError error =
      StartupCredentialProvisionError::not_configured;
  int native_error = 0;
  StartupCredentialSourceAdapterError adapter_error =
      StartupCredentialSourceAdapterError::none;
  StartupCredentialSource::Outcome publish;

  bool ok() const noexcept {
    return error == StartupCredentialProvisionError::none && publish.ok();
  }
};

// Diagnostic code for a refusal, suitable for the server diagnostic stream.
// Deliberately coarse: it names the stage, never the path, the source contents,
// or any length that would narrow the secret.
inline const char* StartupCredentialProvisionDiagnosticCode(
    StartupCredentialProvisionError error) noexcept {
  using E = StartupCredentialProvisionError;
  switch (error) {
    case E::none: return "";
    case E::not_configured: return "BOOTSTRAP.STARTUP_CREDENTIAL_NOT_CONFIGURED";
    case E::unsupported_platform: return "BOOTSTRAP.STARTUP_CREDENTIAL_UNSUPPORTED_PLATFORM";
    case E::no_service_credentials: return "BOOTSTRAP.STARTUP_CREDENTIAL_SERVICE_SOURCE_ABSENT";
    case E::unreadable: return "BOOTSTRAP.STARTUP_CREDENTIAL_UNREADABLE";
    case E::custody_rejected_type: return "BOOTSTRAP.STARTUP_CREDENTIAL_NOT_A_REGULAR_FILE";
    case E::custody_rejected_owner: return "BOOTSTRAP.STARTUP_CREDENTIAL_WRONG_OWNER";
    case E::custody_rejected_mode: return "BOOTSTRAP.STARTUP_CREDENTIAL_PERMISSIONS_TOO_BROAD";
    case E::empty: return "BOOTSTRAP.STARTUP_CREDENTIAL_EMPTY";
    case E::oversized: return "BOOTSTRAP.STARTUP_CREDENTIAL_OVERSIZED";
    case E::seal_failed: return "BOOTSTRAP.STARTUP_CREDENTIAL_SEAL_FAILED";
    case E::publish_failed: return "BOOTSTRAP.STARTUP_CREDENTIAL_PUBLISH_FAILED";
  }
  return "BOOTSTRAP.STARTUP_CREDENTIAL_UNREADABLE";
}

#if defined(__linux__)
namespace detail {

// Reads a provisioned secret under custody validation. The file must be a
// regular file owned by this effective user and unreadable by group or other.
// O_NOFOLLOW refuses a symlink outright rather than following it to a target
// whose custody was never checked.
inline StartupCredentialProvisionError ReadProvisionedSecret(
    const std::string& path, std::string* out, int* native_error) noexcept {
  using E = StartupCredentialProvisionError;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    *native_error = errno;
    return E::unreadable;
  }
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};

  struct ::stat info {};
  if (::fstat(fd, &info) != 0) {
    *native_error = errno;
    return E::unreadable;
  }
  if (!S_ISREG(info.st_mode)) return E::custody_rejected_type;
  if (info.st_uid != ::geteuid()) return E::custody_rejected_owner;
  if ((info.st_mode & 077) != 0) return E::custody_rejected_mode;
  if (info.st_size == 0) return E::empty;
  // Bound the read before allocating: an oversized file is refused on its
  // declared extent rather than after being copied into process memory.
  if (static_cast<std::uintmax_t>(info.st_size) >
      kStartupCredentialMaximumBytes + 2) {
    return E::oversized;
  }

  std::string buffer(static_cast<std::size_t>(info.st_size), '\0');
  std::size_t filled = 0;
  while (filled < buffer.size()) {
    const auto chunk =
        ::read(fd, buffer.data() + filled, buffer.size() - filled);
    if (chunk < 0) {
      if (errno == EINTR) continue;
      *native_error = errno;
      ::explicit_bzero(buffer.data(), buffer.size());
      return E::unreadable;
    }
    if (chunk == 0) break;
    filled += static_cast<std::size_t>(chunk);
  }
  buffer.resize(filled);

  // A single trailing newline is editor punctuation, not secret material.
  // Exactly one is removed, so a secret that genuinely ends in a newline can
  // still be provisioned by appending a second.
  if (!buffer.empty() && buffer.back() == '\n') buffer.pop_back();
  if (!buffer.empty() && buffer.back() == '\r') buffer.pop_back();

  if (buffer.empty()) {
    return E::empty;
  }
  if (buffer.size() > kStartupCredentialMaximumBytes) {
    ::explicit_bzero(buffer.data(), buffer.size());
    return E::oversized;
  }
  *out = std::move(buffer);
  return E::none;
}

}  // namespace detail
#endif

// Reads, seals and publishes the operator-provisioned secret. The plaintext is
// zeroized before returning on every path, including every failure path.
inline StartupCredentialProvisionResult ProvisionStartupCredential(
    const StartupCredentialProvisionRequest& request,
    StartupCredentialSource& source,
    core::memory::MemoryManager& manager,
    const core::memory::MemoryTag& tag) {
  using E = StartupCredentialProvisionError;
  StartupCredentialProvisionResult result;
#if !defined(__linux__)
  (void)request; (void)source; (void)manager; (void)tag;
  result.error = E::unsupported_platform;
  return result;
#else
  std::string path;
  switch (request.source) {
    case StartupCredentialProvisionSource::not_configured:
      result.error = E::not_configured;
      return result;
    case StartupCredentialProvisionSource::service_manager: {
      if (request.name_or_path.empty()) {
        result.error = E::not_configured;
        return result;
      }
      const char* directory = std::getenv("CREDENTIALS_DIRECTORY");
      if (directory == nullptr || *directory == '\0') {
        // Configured to take the secret from the service manager, but the
        // service manager passed none. Refuse rather than fall back to a path:
        // a fallback would silently accept a weaker custody than configured.
        result.error = E::no_service_credentials;
        return result;
      }
      path = std::string(directory) + "/" + request.name_or_path;
      break;
    }
    case StartupCredentialProvisionSource::operator_file:
      if (request.name_or_path.empty()) {
        result.error = E::not_configured;
        return result;
      }
      path = request.name_or_path;
      break;
  }

  std::string secret;
  const auto read = detail::ReadProvisionedSecret(path, &secret, &result.native_error);
  if (read != E::none) {
    result.error = read;
    return result;
  }
  const auto scrub = [&secret]() noexcept {
    if (!secret.empty()) ::explicit_bzero(secret.data(), secret.size());
  };

  auto sealed = CreateSealedStartupCredentialSource(request.binding, secret);
  scrub();
  if (!sealed.ok()) {
    result.error = E::seal_failed;
    result.adapter_error = sealed.error;
    result.native_error = sealed.native_error;
    return result;
  }
  struct Descriptor {
    int fd;
    ~Descriptor() { if (fd >= 0) ::close(fd); }
  } descriptor{sealed.fd};

  // Publish borrows the descriptor; the source imports under its own custody
  // check, so a disagreement between adapter and transport surfaces here rather
  // than being assumed away.
  result.publish =
      source.Publish(request.publish, descriptor.fd, request.binding, manager, tag);
  if (!result.publish.ok()) {
    result.error = E::publish_failed;
    result.native_error = result.publish.native_error;
    return result;
  }
  result.error = E::none;
  return result;
#endif
}

}  // namespace scratchbird::server
