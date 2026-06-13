// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace mem = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

constexpr std::string_view kProtectedMemoryAuthorityScope =
    "protected_memory_evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

mem::AllocationPolicy Policy(std::string_view name = "mmch_015_protected_material") {
  mem::AllocationPolicy policy;
  policy.policy_name = std::string(name);
  policy.hard_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 4ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 2ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_allocate = false;
  policy.zero_memory_on_release = false;
  return policy;
}

mem::MemoryTag Tag(std::string_view context) {
  mem::MemoryTag tag;
  tag.purpose = "protected-material-gate";
  tag.category = mem::MemoryCategory::resource_seed;
  tag.lifetime = mem::MemoryLifetime::temporary;
  tag.owner = "owner-mmch-015";
  tag.context_id = std::string(context);
  tag.database_id = "db-mmch-015";
  tag.session_id = "session-mmch-015";
  tag.transaction_id = "txn-mmch-015";
  tag.statement_id = std::string(context);
  tag.query_id = "query-mmch-015";
  return tag;
}

bool DiagnosticArgEquals(const platform::DiagnosticRecord& diagnostic,
                         std::string_view key,
                         std::string_view value) {
  for (const auto& arg : diagnostic.arguments) {
    if (arg.key == key && arg.value == value) {
      return true;
    }
  }
  return false;
}

bool DiagnosticContainsValue(const platform::DiagnosticRecord& diagnostic, std::string_view value) {
  for (const auto& arg : diagnostic.arguments) {
    if (arg.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return diagnostic.diagnostic_code.find(value) != std::string::npos ||
         diagnostic.message_key.find(value) != std::string::npos ||
         diagnostic.remediation_hint.find(value) != std::string::npos;
}

bool AllBytesEqual(const void* pointer, std::size_t bytes, unsigned char expected) {
  const auto* data = static_cast<const unsigned char*>(pointer);
  return std::all_of(data, data + bytes, [expected](unsigned char value) {
    return value == expected;
  });
}

void SecureZeroPrimitiveZerosMemory() {
  std::array<unsigned char, 64> bytes;
  bytes.fill(0xA5);
  mem::SecureZeroMemory(bytes.data(), bytes.size());
  Require(std::all_of(bytes.begin(), bytes.end(), [](unsigned char value) { return value == 0; }),
          "MMCH-015 secure zero primitive left nonzero bytes");
}

void ProtectedAllocationZeroizeAndRelease() {
  mem::MemoryManager manager(Policy());
  mem::ProtectedMemoryRequest request;
  request.bytes = 256;
  request.alignment = 64;
  request.tag = Tag("ctx-protected-ok");
  request.material_class = "hash_seed_key_material_password=actual-secret-value";
  request.platform_policy = mem::ProtectedMemoryPlatformPolicy::best_effort;
  request.zero_on_allocate = true;

  auto protected_buffer = manager.AllocateProtected(request);
  Require(protected_buffer.ok(), "MMCH-015 protected allocation failed");
  Require(protected_buffer.evidence.protected_material_redacted,
          "MMCH-015 protected allocation did not mark material redacted");
  Require(protected_buffer.evidence.zero_on_allocate,
          "MMCH-015 protected allocation did not record zero-on-allocate");
  Require(protected_buffer.evidence.zero_on_release,
          "MMCH-015 protected allocation did not record zero-on-release");
  Require(protected_buffer.evidence.platform_lock_attempted,
          "MMCH-015 protected allocation did not record platform-lock attempt");
  Require(protected_buffer.evidence.no_dump_attempted,
          "MMCH-015 protected allocation did not record no-dump attempt");
  Require(!protected_buffer.evidence.platform_name.empty(),
          "MMCH-015 protected allocation omitted platform name");
  Require(protected_buffer.evidence.authority_scope ==
              kProtectedMemoryAuthorityScope,
          "MMCH-015 protected allocation authority scope changed");
  Require(AllBytesEqual(protected_buffer.buffer.data(), protected_buffer.buffer.size(), 0),
          "MMCH-015 protected allocation was not zeroed");

  std::memset(protected_buffer.buffer.data(), 0x5A, protected_buffer.buffer.size());
  Require(AllBytesEqual(protected_buffer.buffer.data(), protected_buffer.buffer.size(), 0x5A),
          "MMCH-015 protected test setup did not fill the buffer");
  protected_buffer.buffer.Zeroize();
  Require(AllBytesEqual(protected_buffer.buffer.data(), protected_buffer.buffer.size(), 0),
          "MMCH-015 protected explicit zeroize left nonzero bytes");

  const auto before_release = manager.Snapshot();
  Require(before_release.current_bytes >= request.bytes,
          "MMCH-015 protected allocation was not tracked");
  Require(protected_buffer.buffer.Reset().ok(), "MMCH-015 protected release failed");
  const auto after_release = manager.Snapshot();
  Require(after_release.current_bytes == 0,
          "MMCH-015 protected release leaked allocator bytes");
  Require(after_release.active_allocation_count == 0,
          "MMCH-015 protected release leaked active allocation count");
}

void ProtectedDiagnosticRedactsMaterialClass() {
  mem::MemoryManager manager(Policy("mmch_015_redaction"));
  mem::ProtectedMemoryRequest request;
  request.bytes = 0;
  request.tag = Tag("ctx-protected-redaction");
  request.material_class = "password=actual-secret-value token=second-secret-value";
  request.platform_policy = mem::ProtectedMemoryPlatformPolicy::best_effort;

  const auto rejected = manager.AllocateProtected(request);
  Require(!rejected.ok(), "MMCH-015 zero-size protected allocation unexpectedly succeeded");
  Require(rejected.status.code == platform::StatusCode::memory_invalid_request,
          "MMCH-015 zero-size protected allocation status changed");
  Require(rejected.diagnostic.diagnostic_code == "SB-MEMORY-PROTECTED-ALLOC-ZERO-SIZE",
          "MMCH-015 protected zero-size diagnostic code changed");
  Require(DiagnosticArgEquals(rejected.diagnostic,
                              "protected_material_class",
                              "<protected-material-redacted>"),
          "MMCH-015 protected material class was not redacted");
  Require(DiagnosticArgEquals(rejected.diagnostic,
                              "protected_memory_authority_scope",
                              kProtectedMemoryAuthorityScope),
          "MMCH-015 protected diagnostic authority scope changed");
  Require(!DiagnosticContainsValue(rejected.diagnostic, "actual-secret-value"),
          "MMCH-015 protected diagnostic leaked raw password material");
  Require(!DiagnosticContainsValue(rejected.diagnostic, "second-secret-value"),
          "MMCH-015 protected diagnostic leaked raw token material");
}

void UnderlyingFailureIsRedactedAndReleasesBytes() {
  mem::AllocationPolicy policy = Policy("mmch_015_underlying_failure");
  policy.hard_limit_bytes = 128;
  policy.soft_limit_bytes = 128;
  policy.per_context_limit_bytes = 128;
  mem::MemoryManager manager(policy);

  mem::ProtectedMemoryRequest request;
  request.bytes = 512;
  request.alignment = 64;
  request.tag = Tag("ctx-protected-limit");
  request.material_class = "api_key=actual-secret-value";
  request.platform_policy = mem::ProtectedMemoryPlatformPolicy::best_effort;

  const auto rejected = manager.AllocateProtected(request);
  Require(!rejected.ok(), "MMCH-015 oversized protected allocation unexpectedly succeeded");
  Require(rejected.diagnostic.diagnostic_code == "SB-MEMORY-PROTECTED-ALLOC-FAILED",
          "MMCH-015 oversized protected diagnostic code changed");
  Require(DiagnosticArgEquals(rejected.diagnostic,
                              "underlying_diagnostic",
                              "SB-MEMORY-ALLOC-CONTEXT-LIMIT-EXCEEDED") ||
              DiagnosticArgEquals(rejected.diagnostic,
                                  "underlying_diagnostic",
                                  "SB-MEMORY-ALLOC-LIMIT-EXCEEDED"),
          "MMCH-015 oversized protected diagnostic missing underlying failure");
  Require(!DiagnosticContainsValue(rejected.diagnostic, "actual-secret-value"),
          "MMCH-015 oversized protected diagnostic leaked material");
  Require(manager.Snapshot().current_bytes == 0,
          "MMCH-015 oversized protected allocation leaked bytes");
}

void ManagerWrapperUsesProtectedAllocator() {
  mem::MemoryManager manager(Policy("mmch_015_manager_wrapper"));
  mem::ProtectedMemoryRequest request;
  request.bytes = 64;
  request.alignment = 0;
  request.tag = Tag("ctx-protected-wrapper");
  request.material_class = "temporary_credential";
  request.platform_policy = mem::ProtectedMemoryPlatformPolicy::best_effort;

  auto result = manager.AllocateProtected(request);
  Require(result.ok(), "MMCH-015 memory manager protected wrapper failed");
  Require(result.buffer.size() == 64, "MMCH-015 protected wrapper size mismatch");
  Require(result.buffer.Reset().ok(), "MMCH-015 protected wrapper release failed");
}

}  // namespace

int main() {
  std::cout << "MMCH-015 authority_note=protected_memory_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
            << '\n';
  SecureZeroPrimitiveZerosMemory();
  ProtectedAllocationZeroizeAndRelease();
  ProtectedDiagnosticRedactsMaterialClass();
  UnderlyingFailureIsRedactedAndReleasesBytes();
  ManagerWrapperUsesProtectedAllocator();
  return EXIT_SUCCESS;
}
