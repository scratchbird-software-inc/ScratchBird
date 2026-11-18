// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_NATIVE_DATABASE_OPEN_DISCOVERY (owner 2026-11-16, legacy cut
// + thin-slice step S3a). "The moment you can create a valid database, the
// legacy support is dropped." The opening process (server, embedded client,
// C++ driver embedded mode, the engine ABI open, the lifecycle operations)
// assumes the file is native and has no allowance for any other format. It is
// handed a path and discovers everything from the file itself: the first block
// (the 4096-byte SBFP bootstrap, which states the page size), then the rest of
// page zero. Anything else is "not a valid database" and the open stops, with
// the bootstrap or page-zero refusal as the detail. A legacy (SBDBV001) file is
// therefore refused here, read-only, untouched, and no lock file is made: the
// device is opened with native data-file ownership (S1), whose only authority
// is a flock on the data descriptor.
//
// Decryption (owner Q253): an optional man-in-the-middle translator. With a
// key the opener would assume the file is encrypted and decrypt every block
// before processing it. Encryption is not implemented, so a supplied key is
// refused (DATABASE.OPEN_DECRYPTION_UNAVAILABLE) before anything is opened;
// no decryption is invented here.
//
// This is discovery only: it grants no serving, no MGA admission and no write.

#include "disk_device.hpp"
#include "filespace_bootstrap.hpp"
#include "filespace_page_zero.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::storage::database {

// The specific refusal codes of the legacy cut (owner Q193: specific codes).
inline constexpr std::string_view kOpenNotAValidDatabaseCode = "DATABASE.OPEN_NOT_A_VALID_DATABASE";
inline constexpr std::string_view kOpenDecryptionUnavailableCode = "DATABASE.OPEN_DECRYPTION_UNAVAILABLE";
// Temporary (S3a exit): a valid native file is discovered and its ownership
// released; serving a native database is S3b-S3f.
inline constexpr std::string_view kOpenNativeRuntimePendingCode = "DATABASE.OPEN_NATIVE_RUNTIME_PENDING";
// Owner Q258 = 1: lifecycle operations with no native version yet.
inline constexpr std::string_view kLifecycleOperationUnavailableCode = "DATABASE.LIFECYCLE_OPERATION_UNAVAILABLE";
// Owner 2026-11-16: every legacy create route refuses.
inline constexpr std::string_view kCreateLegacyFormatRefusedCode = "DATABASE.CREATE_LEGACY_FORMAT_REFUSED";

inline const char* FilespaceBootstrapErrorName(disk::FilespaceBootstrapError error) noexcept {
  using E = disk::FilespaceBootstrapError;
  switch (error) {
    case E::none: return "none";
    case E::invalid_framing: return "invalid_framing";
    case E::reserved_nonzero: return "reserved_nonzero";
    case E::digest_mismatch: return "digest_mismatch";
    case E::hash_provider_failure: return "hash_provider_failure";
    case E::invalid_identity: return "invalid_identity";
    case E::unsupported_page_profile: return "unsupported_page_profile";
    case E::unsupported_format_generation: return "unsupported_format_generation";
    case E::unknown_flags: return "unknown_flags";
    case E::unsupported_checksum_profile: return "unsupported_checksum_profile";
    case E::invalid_role: return "invalid_role";
    case E::invalid_state: return "invalid_state";
    case E::invalid_encryption_profile: return "invalid_encryption_profile";
    case E::binding_mismatch: return "binding_mismatch";
    case E::device_not_open: return "device_not_open";
    case E::io_failure: return "io_failure";
    case E::resource_exhausted: return "resource_exhausted";
  }
  return "unknown";
}

inline const char* FilespacePageZeroErrorName(disk::FilespacePageZeroError error) noexcept {
  using E = disk::FilespacePageZeroError;
  switch (error) {
    case E::none: return "none";
    case E::invalid_bootstrap: return "invalid_bootstrap";
    case E::invalid_common_header: return "invalid_common_header";
    case E::invalid_family: return "invalid_family";
    case E::integrity_mismatch: return "integrity_mismatch";
    case E::hash_provider_failure: return "hash_provider_failure";
    case E::invalid_capacity: return "invalid_capacity";
    case E::invalid_root_directory: return "invalid_root_directory";
    case E::required_root_missing: return "required_root_missing";
    case E::probe_changed: return "probe_changed";
    case E::device_not_open: return "device_not_open";
    case E::io_failure: return "io_failure";
    case E::resource_exhausted: return "resource_exhausted";
    case E::invalid_backing: return "invalid_backing";
  }
  return "unknown";
}

enum class NativeOpenDiscoveryError : std::uint8_t {
  none,
  decryption_unavailable,  // a key was supplied (Q253); nothing opened
  open_failed,             // the device open refused (missing, owned, access)
  bootstrap_invalid,       // the first block is not a native bootstrap
  page_zero_invalid,       // the bootstrap is native, page zero is not valid
  close_failed,
};

struct NativeOpenDiscoveryResult {
  NativeOpenDiscoveryError error = NativeOpenDiscoveryError::open_failed;
  // The refusal code: kOpenNotAValidDatabaseCode for bootstrap/page-zero,
  // kOpenDecryptionUnavailableCode for a key, the device's own diagnostic code
  // for an open/close refusal (it is already specific, e.g. an owned file).
  std::string code;
  // bootstrap:<error> | page_zero:<error> | native_open_refused | ...
  std::string detail;
  std::optional<disk::FilespacePageZero> page_zero;
  bool ok() const noexcept { return error == NativeOpenDiscoveryError::none && page_zero.has_value(); }
};

// Reads the first block, then the rest of page zero, from a device the caller
// already opened (natively owned, or served from a native owner). Never writes,
// never closes or reopens the caller's device.
inline NativeOpenDiscoveryResult DiscoverNativeDatabaseOnOpenDevice(disk::FileDevice& device) {
  NativeOpenDiscoveryResult result;
  const auto bootstrap = disk::ReadFilespaceBootstrapFromOpenDevice(device);
  if (!bootstrap.ok()) {
    result.error = NativeOpenDiscoveryError::bootstrap_invalid;
    result.code = std::string(kOpenNotAValidDatabaseCode);
    result.detail = std::string("bootstrap:") + FilespaceBootstrapErrorName(bootstrap.error);
    return result;
  }
  auto zero = disk::ReadFilespacePageZeroFromOpenDevice(device);
  if (!zero.ok()) {
    result.error = NativeOpenDiscoveryError::page_zero_invalid;
    result.code = std::string(kOpenNotAValidDatabaseCode);
    result.detail = std::string("page_zero:") + FilespacePageZeroErrorName(zero.error);
    return result;
  }
  result.error = NativeOpenDiscoveryError::none;
  result.page_zero = std::move(zero.record);
  return result;
}

// Path form for callers that hold no owner: opens the file read-only with
// native data-file ownership (no lock file, flock on the data descriptor),
// discovers, closes (releasing the flock). A refused open names the device's
// own code. `decryption_key_supplied` refuses first (Q253).
inline NativeOpenDiscoveryResult DiscoverNativeDatabaseAtPath(const std::string& path,
                                                              bool decryption_key_supplied = false) {
  NativeOpenDiscoveryResult result;
  if (decryption_key_supplied) {
    result.error = NativeOpenDiscoveryError::decryption_unavailable;
    result.code = std::string(kOpenDecryptionUnavailableCode);
    result.detail = "decryption_translator_not_implemented";
    return result;
  }
  disk::FileDevice device;
  const auto opened = device.Open(path, disk::FileOpenMode::open_existing_read_only,
                                  disk::DatabaseFileOwnership::native_data_file);
  if (!opened.ok()) {
    result.error = NativeOpenDiscoveryError::open_failed;
    result.code = opened.diagnostic.diagnostic_code.empty() ? std::string("SB-STORAGE-DISK-OPEN-FAILED")
                                                            : opened.diagnostic.diagnostic_code;
    result.detail = "native_open_refused";
    return result;
  }
  result = DiscoverNativeDatabaseOnOpenDevice(device);
  const auto closed = device.Close();
  if (!closed.ok() && result.ok()) {
    result.error = NativeOpenDiscoveryError::close_failed;
    result.code = closed.diagnostic.diagnostic_code;
    result.detail = "native_close_failed";
    result.page_zero.reset();
  }
  return result;
}

}  // namespace scratchbird::storage::database
