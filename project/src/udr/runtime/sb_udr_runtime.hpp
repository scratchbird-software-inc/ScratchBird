// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::memory {
class ReservationBackedMemoryResource;
}

namespace scratchbird::udr::runtime {

struct UdrStatus {
  bool ok{false};
  std::string diagnostic_code;
  std::string detail;
};

struct UdrCallInput {
  std::string package_uuid;
  std::string entrypoint;
  std::string payload;
  std::string context_packet;
};

struct UdrCallResult {
  bool ok{false};
  std::string payload;
  std::string message_vector_json;
};

using UdrEntrypointCallback = UdrCallResult (*)(const UdrCallInput& input);
using UdrLifecycleCallback = UdrStatus (*)(std::string_view package_uuid);

struct UdrEntrypointDescriptor {
  std::string name;
  std::string role;
  UdrEntrypointCallback callback{nullptr};
};

struct UdrPackageDescriptor {
  std::string package_uuid;
  std::string package_name;
  std::string abi_version;
  std::string source_revision;
  std::string binary_hash;
  std::string signature_policy;
  std::string capability_role;
  bool trusted_cpp{false};
  std::vector<UdrEntrypointDescriptor> entrypoints;
  UdrLifecycleCallback init{nullptr};
  UdrLifecycleCallback shutdown{nullptr};
};

struct UdrPackageRuntimeState {
  bool registered{false};
  bool loaded{false};
  std::size_t active_invocations{0};
  std::string package_uuid;
  std::string package_name;
  std::string abi_version;
  std::string source_revision;
  std::string binary_hash;
  std::string signature_policy;
  std::string capability_role;
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
  friend UdrStatus AcquireInvocationRef(std::string_view package_uuid,
                                        UdrInvocationLease* out_lease);
  explicit UdrInvocationLease(std::string package_uuid);

  std::string package_uuid_;
  bool held_{false};
};

UdrStatus RegisterPackage(const UdrPackageDescriptor& descriptor);
std::optional<UdrPackageDescriptor> FindPackageDescriptor(std::string_view package_uuid);
std::optional<UdrPackageRuntimeState> GetPackageState(std::string_view package_uuid);
UdrStatus LoadPackage(std::string_view package_uuid);
UdrStatus UnloadPackage(std::string_view package_uuid);
UdrStatus AcquireInvocationRef(std::string_view package_uuid, UdrInvocationLease* out_lease);
UdrCallResult InvokePackage(const UdrCallInput& input);
UdrCallResult InvokePackageWithReservedWorkspace(
    const UdrCallInput& input,
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::size_t workspace_bytes,
    bool sblr_invocation_authority,
    bool parser_or_donor_finality_authority,
    bool debug_or_relaxed_path);
void ResetRuntimeForTest();

}  // namespace scratchbird::udr::runtime
