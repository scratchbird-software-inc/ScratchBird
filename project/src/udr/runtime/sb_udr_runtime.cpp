// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sb_udr_runtime.hpp"

#include "reservation_backed_memory_resource.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace scratchbird::udr::runtime {
namespace {

struct RuntimePackage {
  UdrPackageDescriptor descriptor;
  bool loaded{false};
  std::size_t active_invocations{0};
};

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, RuntimePackage>& Registry() {
  static std::map<std::string, RuntimePackage> registry;
  return registry;
}

UdrStatus Ok() {
  return {true, "UDR.OK", {}};
}

UdrStatus Error(std::string code, std::string detail) {
  return {false, std::move(code), std::move(detail)};
}

bool Blank(std::string_view value) {
  return value.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

bool IsTrustedCppRuntimeLanguage(std::string_view value) {
  return value.empty() || value == "cpp" || value == "c++" ||
         value == "cxx" || value == "trusted_cpp";
}

UdrStatus ValidateDescriptor(const UdrPackageDescriptor& descriptor) {
  if (Blank(descriptor.package_uuid)) {
    return Error("UDR.RUNTIME.PACKAGE_UUID_REQUIRED", "package_uuid_required");
  }
  if (Blank(descriptor.package_name)) {
    return Error("UDR.RUNTIME.PACKAGE_NAME_REQUIRED", descriptor.package_uuid);
  }
  if (!IsTrustedCppRuntimeLanguage(descriptor.runtime_language)) {
    return Error("UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN", descriptor.package_uuid);
  }
  if (!descriptor.trusted_cpp) {
    return Error("UDR.RUNTIME.TRUSTED_CPP_REQUIRED", descriptor.package_uuid);
  }
  if (descriptor.abi_version != "sb_udr_v1") {
    return Error("UDR.RUNTIME.ABI_UNSUPPORTED", descriptor.package_uuid);
  }
  if (Blank(descriptor.source_revision) || Blank(descriptor.binary_hash) ||
      Blank(descriptor.signature_policy)) {
    return Error("UDR.RUNTIME.PROVENANCE_REQUIRED", descriptor.package_uuid);
  }
  if (descriptor.entrypoints.empty()) {
    return Error("UDR.RUNTIME.ENTRYPOINT_REQUIRED", descriptor.package_uuid);
  }
  for (const auto& entrypoint : descriptor.entrypoints) {
    if (Blank(entrypoint.name) || entrypoint.callback == nullptr) {
      return Error("UDR.RUNTIME.ENTRYPOINT_INVALID", descriptor.package_uuid);
    }
  }
  return Ok();
}

RuntimePackage* FindLocked(std::string_view package_uuid) {
  auto& registry = Registry();
  const auto it = registry.find(std::string(package_uuid));
  return it == registry.end() ? nullptr : &it->second;
}

void ReleaseRef(std::string_view package_uuid) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  RuntimePackage* package = FindLocked(package_uuid);
  if (package != nullptr && package->active_invocations > 0) {
    --package->active_invocations;
  }
}

}  // namespace

UdrInvocationLease::UdrInvocationLease(std::string package_uuid)
    : package_uuid_(std::move(package_uuid)), held_(true) {}

UdrInvocationLease::UdrInvocationLease(UdrInvocationLease&& other) noexcept
    : package_uuid_(std::move(other.package_uuid_)), held_(other.held_) {
  other.held_ = false;
}

UdrInvocationLease& UdrInvocationLease::operator=(UdrInvocationLease&& other) noexcept {
  if (this != &other) {
    Release();
    package_uuid_ = std::move(other.package_uuid_);
    held_ = other.held_;
    other.held_ = false;
  }
  return *this;
}

UdrInvocationLease::~UdrInvocationLease() {
  Release();
}

void UdrInvocationLease::Release() {
  if (!held_) return;
  ReleaseRef(package_uuid_);
  held_ = false;
}

UdrStatus RegisterPackage(const UdrPackageDescriptor& descriptor) {
  const auto valid = ValidateDescriptor(descriptor);
  if (!valid.ok) return valid;

  std::lock_guard<std::mutex> lock(RegistryMutex());
  auto& registry = Registry();
  const auto existing = registry.find(descriptor.package_uuid);
  if (existing != registry.end()) {
    const auto& current = existing->second.descriptor;
    if (current.abi_version != descriptor.abi_version ||
        current.source_revision != descriptor.source_revision ||
        current.binary_hash != descriptor.binary_hash ||
        current.signature_policy != descriptor.signature_policy ||
        current.package_name != descriptor.package_name ||
        current.capability_role != descriptor.capability_role ||
        current.runtime_language != descriptor.runtime_language ||
        current.entrypoints.size() != descriptor.entrypoints.size()) {
      return Error("UDR.RUNTIME.PACKAGE_DESCRIPTOR_CONFLICT", descriptor.package_uuid);
    }
    return Ok();
  }
  registry.emplace(descriptor.package_uuid, RuntimePackage{descriptor, false, 0});
  return Ok();
}

std::optional<UdrPackageDescriptor> FindPackageDescriptor(std::string_view package_uuid) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  RuntimePackage* package = FindLocked(package_uuid);
  if (package == nullptr) return std::nullopt;
  return package->descriptor;
}

std::optional<UdrPackageRuntimeState> GetPackageState(std::string_view package_uuid) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  RuntimePackage* package = FindLocked(package_uuid);
  if (package == nullptr) return std::nullopt;
  UdrPackageRuntimeState state;
  state.registered = true;
  state.loaded = package->loaded;
  state.active_invocations = package->active_invocations;
  state.package_uuid = package->descriptor.package_uuid;
  state.package_name = package->descriptor.package_name;
  state.abi_version = package->descriptor.abi_version;
  state.source_revision = package->descriptor.source_revision;
  state.binary_hash = package->descriptor.binary_hash;
  state.signature_policy = package->descriptor.signature_policy;
  state.capability_role = package->descriptor.capability_role;
  state.runtime_language = package->descriptor.runtime_language;
  for (const auto& entrypoint : package->descriptor.entrypoints) {
    state.entrypoint_names.push_back(entrypoint.name);
  }
  return state;
}

UdrStatus LoadPackage(std::string_view package_uuid) {
  UdrLifecycleCallback init = nullptr;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    RuntimePackage* package = FindLocked(package_uuid);
    if (package == nullptr) {
      return Error("UDR.RUNTIME.PACKAGE_NOT_REGISTERED", std::string(package_uuid));
    }
    if (package->loaded) return Ok();
    init = package->descriptor.init;
  }

  if (init != nullptr) {
    const auto initialized = init(package_uuid);
    if (!initialized.ok) return initialized;
  }

  std::lock_guard<std::mutex> lock(RegistryMutex());
  RuntimePackage* package = FindLocked(package_uuid);
  if (package == nullptr) {
    return Error("UDR.RUNTIME.PACKAGE_NOT_REGISTERED", std::string(package_uuid));
  }
  package->loaded = true;
  return Ok();
}

UdrStatus UnloadPackage(std::string_view package_uuid) {
  UdrLifecycleCallback shutdown = nullptr;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    RuntimePackage* package = FindLocked(package_uuid);
    if (package == nullptr) {
      return Error("UDR.RUNTIME.PACKAGE_NOT_REGISTERED", std::string(package_uuid));
    }
    if (package->active_invocations != 0) {
      return Error("UDR.UNLOAD_BLOCKED", std::string(package_uuid));
    }
    if (!package->loaded) return Ok();
    shutdown = package->descriptor.shutdown;
  }

  if (shutdown != nullptr) {
    const auto stopped = shutdown(package_uuid);
    if (!stopped.ok) return stopped;
  }

  std::lock_guard<std::mutex> lock(RegistryMutex());
  RuntimePackage* package = FindLocked(package_uuid);
  if (package == nullptr) {
    return Error("UDR.RUNTIME.PACKAGE_NOT_REGISTERED", std::string(package_uuid));
  }
  package->loaded = false;
  return Ok();
}

UdrStatus AcquireInvocationRef(std::string_view package_uuid, UdrInvocationLease* out_lease) {
  if (out_lease == nullptr) {
    return Error("UDR.RUNTIME.LEASE_OUTPUT_REQUIRED", std::string(package_uuid));
  }
  std::lock_guard<std::mutex> lock(RegistryMutex());
  RuntimePackage* package = FindLocked(package_uuid);
  if (package == nullptr) {
    return Error("UDR.RUNTIME.PACKAGE_NOT_REGISTERED", std::string(package_uuid));
  }
  if (!package->loaded) {
    return Error("UDR.RUNTIME.PACKAGE_NOT_LOADED", std::string(package_uuid));
  }
  ++package->active_invocations;
  *out_lease = UdrInvocationLease(std::string(package_uuid));
  return Ok();
}

UdrCallResult InvokePackage(const UdrCallInput& input) {
  UdrEntrypointCallback callback = nullptr;
  UdrInvocationLease lease;
  const auto acquired = AcquireInvocationRef(input.package_uuid, &lease);
  if (!acquired.ok) {
    return {false, {}, "{\"diagnostic\":\"" + acquired.diagnostic_code + "\"}"};
  }

  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    RuntimePackage* package = FindLocked(input.package_uuid);
    if (package == nullptr) {
      return {false, {}, "{\"diagnostic\":\"UDR.RUNTIME.PACKAGE_NOT_REGISTERED\"}"};
    }
    const auto it = std::find_if(package->descriptor.entrypoints.begin(),
                                 package->descriptor.entrypoints.end(),
                                 [&](const UdrEntrypointDescriptor& entrypoint) {
                                   return entrypoint.name == input.entrypoint;
                                 });
    if (it == package->descriptor.entrypoints.end()) {
      return {false, {}, "{\"diagnostic\":\"UDR.RUNTIME.ENTRYPOINT_NOT_FOUND\"}"};
    }
    callback = it->callback;
  }
  return callback(input);
}

UdrCallResult InvokePackageWithReservedWorkspace(
    const UdrCallInput& input,
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::size_t workspace_bytes,
    bool sblr_invocation_authority,
    bool parser_or_reference_finality_authority,
    bool debug_or_relaxed_path) {
  if (resource == nullptr || !resource->active()) {
    return {false, {}, "{\"diagnostic\":\"UDR.RUNTIME.CEIC012_RESOURCE_REQUIRED\"}"};
  }
  if (!sblr_invocation_authority || parser_or_reference_finality_authority ||
      debug_or_relaxed_path) {
    return {false, {}, "{\"diagnostic\":\"UDR.RUNTIME.CEIC012_UNSAFE_AUTHORITY\"}"};
  }
  if (workspace_bytes == 0) {
    return {false, {}, "{\"diagnostic\":\"UDR.RUNTIME.CEIC012_WORKSPACE_REQUIRED\"}"};
  }
  if (input.payload.size() >
          std::numeric_limits<std::size_t>::max() - workspace_bytes ||
      input.context_packet.size() >
          std::numeric_limits<std::size_t>::max() - workspace_bytes -
              input.payload.size()) {
    return {false, {}, "{\"diagnostic\":\"UDR.RUNTIME.CEIC012_WORKSPACE_OVERFLOW\"}"};
  }

  scratchbird::core::memory::ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = static_cast<scratchbird::core::platform::u64>(
      workspace_bytes + input.payload.size() + input.context_packet.size());
  allocation.alignment = alignof(std::max_align_t);
  allocation.purpose = "udr.invocation_workspace";
  const auto allocated = resource->Allocate(std::move(allocation));
  if (!allocated.ok()) {
    return {false, {}, "{\"diagnostic\":\"UDR.RUNTIME.CEIC012_ALLOCATION_REFUSED\"}"};
  }
  auto* bytes = static_cast<unsigned char*>(allocated.pointer);
  if (!input.payload.empty()) {
    std::memcpy(bytes, input.payload.data(), input.payload.size());
  }
  if (!input.context_packet.empty()) {
    std::memcpy(bytes + input.payload.size(),
                input.context_packet.data(),
                input.context_packet.size());
  }

  auto result = InvokePackage(input);
  if (result.message_vector_json.empty()) {
    result.message_vector_json =
        "{\"ceic_012\":\"udr_reserved_workspace_consumed\"}";
  }
  return result;
}

void ResetRuntimeForTest() {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  Registry().clear();
}

}  // namespace scratchbird::udr::runtime
