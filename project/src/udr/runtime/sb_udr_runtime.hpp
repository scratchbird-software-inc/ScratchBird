// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::memory {
class ReservationBackedMemoryResource;
}

namespace scratchbird::udr::runtime {

using UdrUuid = scratchbird::core::platform::Uuid;

struct UdrStatus {
  bool ok{false};
  std::string diagnostic_code;
  std::string detail;
  UdrUuid identity;
};

// Binary bindings supplied by the owning engine/session. Context text is not
// an identity source, and missing bindings remain nil.
struct UdrIdentityContext {
  UdrUuid session_uuid;
  UdrUuid connection_uuid;
  UdrUuid database_uuid;
  std::vector<UdrUuid> resolved_objects;
  UdrUuid parser_uuid;
};

struct UdrCallInput {
  UdrUuid package_uuid;
  std::string entrypoint;
  std::string payload;
  std::string context_packet;
  UdrIdentityContext identities;
};

struct UdrCallResult {
  bool ok{false};
  std::string payload;
  std::string message_vector_json;
  UdrUuid package_uuid;
};

using UdrEntrypointCallback = UdrCallResult (*)(const UdrCallInput& input);
using UdrLifecycleCallback = UdrStatus (*)(const UdrUuid& package_uuid);

struct UdrEntrypointDescriptor {
  std::string name;
  std::string role;
  UdrEntrypointCallback callback{nullptr};
};

struct UdrPackageDescriptor {
  UdrUuid package_uuid;
  std::string package_name;
  std::string abi_version;
  std::string source_revision;
  std::string binary_hash;
  std::string signature_policy;
  std::string capability_role;
  std::string runtime_language{"cpp"};
  bool trusted_cpp{false};
  std::vector<UdrEntrypointDescriptor> entrypoints;
  UdrLifecycleCallback init{nullptr};
  UdrLifecycleCallback shutdown{nullptr};
};

struct UdrPackageRuntimeState {
  bool registered{false};
  bool loaded{false};
  std::size_t active_invocations{0};
  UdrUuid package_uuid;
  std::string package_name;
  std::string abi_version;
  std::string source_revision;
  std::string binary_hash;
  std::string signature_policy;
  std::string capability_role;
  std::string runtime_language;
  std::vector<std::string> entrypoint_names;
};

class UdrInvocationLease {
 public:
  UdrInvocationLease() = default;
  UdrInvocationLease(const UdrInvocationLease&) = delete;
  UdrInvocationLease& operator=(const UdrInvocationLease&) = delete;
  UdrInvocationLease(UdrInvocationLease&& other) noexcept;
  UdrInvocationLease& operator=(UdrInvocationLease&& other) noexcept;
  ~UdrInvocationLease();

  bool held() const { return held_; }
  void Release();

 private:
  friend UdrStatus AcquireInvocationRef(const UdrUuid& package_uuid,
                                        UdrInvocationLease* out_lease);
  explicit UdrInvocationLease(UdrUuid package_uuid);

  UdrUuid package_uuid_;
  bool held_{false};
};

UdrStatus RegisterPackage(const UdrPackageDescriptor& descriptor);
std::optional<UdrPackageDescriptor> FindPackageDescriptor(const UdrUuid& package_uuid);
std::optional<UdrPackageRuntimeState> GetPackageState(const UdrUuid& package_uuid);
UdrStatus LoadPackage(const UdrUuid& package_uuid);
UdrStatus UnloadPackage(const UdrUuid& package_uuid);
UdrStatus UnregisterPackage(const UdrUuid& package_uuid);
UdrStatus AcquireInvocationRef(const UdrUuid& package_uuid, UdrInvocationLease* out_lease);
UdrCallResult InvokePackage(const UdrCallInput& input);
UdrCallResult InvokePackageWithReservedWorkspace(
    const UdrCallInput& input,
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::size_t workspace_bytes,
    bool sblr_invocation_authority,
    bool parser_or_reference_finality_authority,
    bool debug_or_relaxed_path);
void ResetRuntimeForTest();

}  // namespace scratchbird::udr::runtime
