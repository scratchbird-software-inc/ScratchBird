// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-MEMORY-MANAGER-ANCHOR
#include "runtime_platform.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::usize;

class ShardedMemoryAccountingLedger;

// SB-MEMORY-DEFERRED-CONSUMER-ANCHOR
// Categories are architecture-visible accounting buckets. Several categories
// are reserved before their consumers exist so policy and diagnostics do not
// drift when executor, LLVM, GPU, UDR, parser, and cluster integrations arrive.
enum class MemoryCategory {
  unknown,
  core_runtime,
  page_buffer,
  catalog_bootstrap,
  resource_seed,
  datatype_payload,
  transaction_local,
  transaction_snapshot,
  version_chain,
  cleanup,
  archive,
  metrics,
  diagnostics,
  executor_query_reserved,
  parser_handoff_reserved,
  udr_reserved,
  llvm_code_reserved,
  llvm_data_reserved,
  gpu_host_pinned_reserved,
  gpu_device_reserved,
  cluster_control_reserved,
  cluster_decision_reserved,
  test_probe
};

enum class MemoryLifetime {
  unknown,
  process,
  database,
  connection,
  transaction,
  statement,
  page_buffer,
  arena,
  temporary,
  deferred_epoch,
  static_reserved
};

enum class AllocationFailureMode {
  return_error,
  fatal_status
};

struct MemoryTag {
  Subsystem subsystem = Subsystem::memory;
  std::string purpose;
  MemoryCategory category = MemoryCategory::core_runtime;
  MemoryLifetime lifetime = MemoryLifetime::temporary;
  std::string owner;
  std::string context_id;
  std::string database_id;
  std::string session_id;
  std::string transaction_id;
  std::string statement_id;
  std::string query_id;
  std::string callsite;
};

// SB-MEMORY-POLICY-ANCHOR
struct AllocationPolicy {
  std::string policy_name = "default";
  u64 byte_limit = 0;
  u64 hard_limit_bytes = 0;
  u64 soft_limit_bytes = 0;
  u64 per_context_limit_bytes = 0;
  u64 page_buffer_pool_limit_bytes = 0;
  AllocationFailureMode failure_mode = AllocationFailureMode::return_error;
  bool track_allocations = true;
  bool zero_memory_on_allocate = false;
  bool zero_memory_on_release = false;
  bool reject_over_soft_limit = false;
  bool refuse_all_allocations = false;
};

// SB-MEMORY-CONTEXT-ANCHOR
struct MemoryContext {
  std::string context_id;
  std::string parent_context_id;
  MemoryTag tag;
  AllocationPolicy policy;
};

struct MemoryCategorySnapshot {
  MemoryCategory category = MemoryCategory::unknown;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 deallocation_count = 0;
  u64 failure_count = 0;
  u64 active_allocation_count = 0;
};

struct MemoryContextSnapshot {
  std::string scope_kind;
  std::string scope_id;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 deallocation_count = 0;
  u64 failure_count = 0;
  u64 active_allocation_count = 0;
};

// SB-MEMORY-METRICS-ANCHOR
struct MemoryAccountingSnapshot {
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 deallocation_count = 0;
  u64 failure_count = 0;
  u64 active_allocation_count = 0;
  u64 policy_rejection_count = 0;
  u64 unknown_pointer_failure_count = 0;
  u64 page_buffer_current_bytes = 0;
  u64 page_buffer_peak_bytes = 0;
  u64 arena_current_bytes = 0;
  u64 arena_peak_bytes = 0;
  u64 leak_candidate_count = 0;
  bool sharded_accounting_bound = false;
  bool active_records_routed_through_sharded_accounting = false;
  u64 sharded_accounting_shard_count = 0;
  u64 sharded_accounting_current_bytes = 0;
  u64 sharded_accounting_peak_bytes = 0;
  u64 sharded_accounting_active_allocation_count = 0;
  u64 sharded_accounting_failed_release_count = 0;
  u64 resident_committed_bytes = 0;
  u64 allocator_metadata_overhead_bytes = 0;
  u64 retained_slab_bytes = 0;
  u64 internal_fragmentation_bytes = 0;
  u64 external_fragmentation_bytes = 0;
  std::vector<MemoryCategorySnapshot> categories;
  std::vector<MemoryContextSnapshot> contexts;
  std::vector<MemoryCategory> reserved_categories;
};

struct AllocationResult {
  Status status;
  void* pointer = nullptr;
  usize bytes = 0;
  usize alignment = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && pointer != nullptr;
  }
};

struct DefaultMemoryManagerConfigurationResult {
  Status status;
  DiagnosticRecord diagnostic;
  AllocationPolicy requested_policy;
  AllocationPolicy active_policy;
  bool applied = false;
  bool already_initialized = false;
  bool fixture_mode = false;

  bool ok() const {
    return status.ok();
  }
};

struct DefaultMemoryManagerStateSnapshot {
  bool initialized = false;
  bool explicitly_configured = false;
  bool fixture_mode = false;
  std::string provenance;
  AllocationPolicy active_policy;
};

struct DeallocationResult {
  Status status;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

// MMCH_ALLOCATION_FAILURE_INJECTION
enum class MemoryFailureInjectionScopeKind {
  any,
  context,
  owner,
  database,
  session,
  transaction,
  statement,
  query
};

class MemoryFailureInjectionTestGuard {
 public:
  constexpr MemoryFailureInjectionTestGuard() = default;

  constexpr bool enabled() const {
    return enabled_;
  }

  constexpr const char* compile_policy() const {
    return compile_policy_;
  }

 private:
  constexpr MemoryFailureInjectionTestGuard(bool enabled, const char* compile_policy)
      : enabled_(enabled), compile_policy_(compile_policy) {}

  bool enabled_ = false;
  const char* compile_policy_ = "production";

  friend constexpr MemoryFailureInjectionTestGuard MakeMemoryFailureInjectionTestGuard();
};

constexpr MemoryFailureInjectionTestGuard MakeMemoryFailureInjectionTestGuard() {
#ifdef SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD
  return MemoryFailureInjectionTestGuard(true, "SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD");
#else
  return MemoryFailureInjectionTestGuard(false, "production");
#endif
}

struct MemoryFailureInjectionRule {
  bool enabled = true;
  std::string rule_id;
  std::string callsite;
  std::string purpose;
  MemoryCategory category = MemoryCategory::unknown;
  MemoryFailureInjectionScopeKind scope_kind = MemoryFailureInjectionScopeKind::any;
  std::string scope_id;
  u64 fail_on_matched_sequence = 1;
};

struct MemoryFailureInjectionConfiguration {
  MemoryFailureInjectionTestGuard test_guard;
  bool fixture_enabled = false;
  std::string fixture_name;
  std::string evidence_note;
  std::vector<MemoryFailureInjectionRule> rules;
};

struct MemoryFailureInjectionConfigurationResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool applied = false;
  bool enabled = false;
  bool blocked = false;

  bool ok() const {
    return status.ok();
  }
};

struct MemoryFailureInjectionRuleSnapshot {
  MemoryFailureInjectionRule rule;
  u64 matched_sequence = 0;
  u64 failure_count = 0;
};

struct MemoryFailureInjectionSnapshot {
  bool enabled = false;
  std::string fixture_name;
  std::string evidence_note;
  u64 observed_allocation_sequence = 0;
  std::vector<MemoryFailureInjectionRuleSnapshot> rules;
};

struct PageBufferRequest {
  usize page_size = 0;
  usize page_count = 1;
  usize alignment = 0;
  MemoryTag tag;
};

struct PageBuffer {
  void* pointer = nullptr;
  usize bytes = 0;
  usize page_size = 0;
  usize page_count = 0;
  usize alignment = 0;

  bool valid() const {
    return pointer != nullptr && bytes != 0 && page_size != 0 && page_count != 0;
  }
};

struct PageBufferResult {
  Status status;
  PageBuffer buffer;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && buffer.valid();
  }
};

// MMCH_PROTECTED_MEMORY_LIFECYCLE
enum class ProtectedMemoryPlatformPolicy {
  best_effort,
  require_platform_lock,
  require_no_dump,
  require_lock_and_no_dump
};

struct ProtectedMemoryEvidence {
  bool protected_material_redacted = true;
  bool zero_on_allocate = true;
  bool zero_on_release = true;
  bool platform_lock_attempted = false;
  bool platform_lock_supported = false;
  bool platform_lock_succeeded = false;
  bool no_dump_attempted = false;
  bool no_dump_supported = false;
  bool no_dump_succeeded = false;
  std::string platform_name = "unknown";
  std::string authority_scope =
      "protected_memory_evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";
};

struct ProtectedMemoryRequest {
  usize bytes = 0;
  usize alignment = 0;
  MemoryTag tag;
  std::string material_class = "protected_material";
  ProtectedMemoryPlatformPolicy platform_policy = ProtectedMemoryPlatformPolicy::best_effort;
  bool zero_on_allocate = true;
};

class BoundedAllocator;

class ScopedAllocation {
 public:
  ScopedAllocation() = default;
  ScopedAllocation(BoundedAllocator* allocator, void* pointer, usize bytes, usize alignment, MemoryTag tag);
  ScopedAllocation(const ScopedAllocation&) = delete;
  ScopedAllocation& operator=(const ScopedAllocation&) = delete;
  ScopedAllocation(ScopedAllocation&& other) noexcept;
  ScopedAllocation& operator=(ScopedAllocation&& other) noexcept;
  ~ScopedAllocation();

  void* data() const { return pointer_; }
  usize size() const { return bytes_; }
  bool valid() const { return pointer_ != nullptr; }
  DeallocationResult Reset();

 private:
  BoundedAllocator* allocator_ = nullptr;
  void* pointer_ = nullptr;
  usize bytes_ = 0;
  usize alignment_ = 0;
  MemoryTag tag_;
};

struct ScopedAllocationResult {
  Status status;
  ScopedAllocation allocation;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && allocation.valid();
  }
};

class ScopedPageBuffer {
 public:
  ScopedPageBuffer() = default;
  ScopedPageBuffer(BoundedAllocator* allocator, PageBuffer buffer, MemoryTag tag);
  ScopedPageBuffer(const ScopedPageBuffer&) = delete;
  ScopedPageBuffer& operator=(const ScopedPageBuffer&) = delete;
  ScopedPageBuffer(ScopedPageBuffer&& other) noexcept;
  ScopedPageBuffer& operator=(ScopedPageBuffer&& other) noexcept;
  ~ScopedPageBuffer();

  void* data() const { return buffer_.pointer; }
  usize size() const { return buffer_.bytes; }
  const PageBuffer& buffer() const { return buffer_; }
  bool valid() const { return buffer_.valid(); }
  DeallocationResult Reset();

 private:
  BoundedAllocator* allocator_ = nullptr;
  PageBuffer buffer_;
  MemoryTag tag_;
};

struct ScopedPageBufferResult {
  Status status;
  ScopedPageBuffer buffer;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && buffer.valid();
  }
};

class ScopedProtectedBuffer {
 public:
  ScopedProtectedBuffer() = default;
  ScopedProtectedBuffer(BoundedAllocator* allocator,
                        void* pointer,
                        usize bytes,
                        usize alignment,
                        MemoryTag tag,
                        ProtectedMemoryEvidence evidence);
  ScopedProtectedBuffer(const ScopedProtectedBuffer&) = delete;
  ScopedProtectedBuffer& operator=(const ScopedProtectedBuffer&) = delete;
  ScopedProtectedBuffer(ScopedProtectedBuffer&& other) noexcept;
  ScopedProtectedBuffer& operator=(ScopedProtectedBuffer&& other) noexcept;
  ~ScopedProtectedBuffer();

  void* data() const { return pointer_; }
  usize size() const { return bytes_; }
  bool valid() const { return pointer_ != nullptr; }
  const ProtectedMemoryEvidence& evidence() const { return evidence_; }
  void Zeroize();
  DeallocationResult Reset();

 private:
  BoundedAllocator* allocator_ = nullptr;
  void* pointer_ = nullptr;
  usize bytes_ = 0;
  usize alignment_ = 0;
  MemoryTag tag_;
  ProtectedMemoryEvidence evidence_;
};

struct ProtectedBufferResult {
  Status status;
  ScopedProtectedBuffer buffer;
  DiagnosticRecord diagnostic;
  ProtectedMemoryEvidence evidence;

  bool ok() const {
    return status.ok() && buffer.valid();
  }
};

class BoundedAllocator {
 public:
  explicit BoundedAllocator(AllocationPolicy policy);
  BoundedAllocator(const BoundedAllocator&) = delete;
  BoundedAllocator& operator=(const BoundedAllocator&) = delete;
  ~BoundedAllocator();

  AllocationResult Allocate(usize bytes, usize alignment, MemoryTag tag);
  AllocationResult AllocateZeroed(usize bytes, usize alignment, MemoryTag tag);
  AllocationResult Reallocate(void* pointer, usize bytes, usize alignment, MemoryTag tag);
  DeallocationResult Deallocate(void* pointer, MemoryTag tag);
  MemoryAccountingSnapshot Snapshot() const;
  const AllocationPolicy& policy() const;
  MemoryFailureInjectionConfigurationResult EnableAllocationFailureInjection(
      MemoryFailureInjectionConfiguration configuration);
  MemoryFailureInjectionConfigurationResult DisableAllocationFailureInjection();
  MemoryFailureInjectionSnapshot FailureInjectionSnapshot() const;

  PageBufferResult AllocatePageBuffer(PageBufferRequest request);
  DeallocationResult ReleasePageBuffer(PageBuffer buffer, MemoryTag tag);
  ProtectedBufferResult AllocateProtected(ProtectedMemoryRequest request);

 private:
  struct AllocationRecord {
    usize bytes = 0;
    usize alignment = 0;
    MemoryTag tag;
    u64 sharded_token_id = 0;
    usize sharded_shard_index = 0;
    bool sharded_accounting_committed = false;
  };

  struct CategoryAccounting {
    u64 current_bytes = 0;
    u64 peak_bytes = 0;
    u64 allocation_count = 0;
    u64 deallocation_count = 0;
    u64 failure_count = 0;
    u64 active_allocation_count = 0;
  };

  struct ContextAccounting {
    u64 current_bytes = 0;
    u64 peak_bytes = 0;
    u64 allocation_count = 0;
    u64 deallocation_count = 0;
    u64 failure_count = 0;
    u64 active_allocation_count = 0;
  };

  struct ContextLimitEvidence {
    bool exceeded = false;
    std::string scope_kind;
    std::string scope_id;
    u64 current_bytes = 0;
    u64 limit_bytes = 0;
  };

  struct FailureInjectionRuleState {
    MemoryFailureInjectionRule rule;
    u64 matched_sequence = 0;
    u64 failure_count = 0;
  };

  struct FailureInjectionState {
    bool enabled = false;
    std::string fixture_name;
    std::string evidence_note;
    u64 observed_allocation_sequence = 0;
    std::vector<FailureInjectionRuleState> rules;
  };

  struct FailureInjectionDecision {
    bool inject = false;
    std::string fixture_name;
    std::string evidence_note;
    u64 observed_allocation_sequence = 0;
    u64 matched_sequence = 0;
    MemoryFailureInjectionRule rule;
  };

  bool WouldExceedHardLimit(usize bytes) const;
  bool WouldExceedSoftLimit(usize bytes) const;
  bool WouldExceedPageBufferPoolLimit(usize bytes, MemoryCategory category) const;
  ContextLimitEvidence WouldExceedPerContextLimit(usize bytes, const MemoryTag& tag) const;
  FailureInjectionDecision EvaluateFailureInjectionLocked(const MemoryTag& tag);
  Status MemoryStatus(scratchbird::core::platform::StatusCode code, Severity severity) const;
  DiagnosticRecord MakeMemoryDiagnostic(Status status,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        MemoryTag tag,
                                        usize bytes = 0,
                                        usize alignment = 0,
                                        std::vector<DiagnosticArgument> extra_arguments = {}) const;
  DiagnosticRecord MakeFailureInjectionDiagnostic(Status status,
                                                 const MemoryTag& tag,
                                                 usize bytes,
                                                 usize alignment,
                                                 const FailureInjectionDecision& decision) const;
  DiagnosticRecord MakeProtectedMemoryDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 MemoryTag tag,
                                                 usize bytes,
                                                 usize alignment,
                                                 const ProtectedMemoryEvidence& evidence,
                                                 std::vector<DiagnosticArgument> extra_arguments = {}) const;
  void RecordFailure(const MemoryTag& tag, bool policy_rejection, bool unknown_pointer);
  void RecordFailure(MemoryCategory category, bool policy_rejection, bool unknown_pointer);
  void RecordAllocation(void* pointer, usize bytes, usize alignment, const MemoryTag& tag);
  void ApplyAllocationRemovalAccounting(const AllocationRecord& record);
  AllocationRecord RemoveAllocation(void* pointer, bool* found);

  AllocationPolicy policy_;
  mutable std::mutex mutex_;
  MemoryAccountingSnapshot accounting_;
  std::unique_ptr<ShardedMemoryAccountingLedger> sharded_accounting_;
  std::unordered_map<void*, AllocationRecord> active_;
  std::map<MemoryCategory, CategoryAccounting> category_accounting_;
  std::map<std::pair<std::string, std::string>, ContextAccounting> context_accounting_;
  FailureInjectionState failure_injection_;
};

// SB-MEMORY-ARENA-ANCHOR
class ArenaAllocator {
 public:
  ArenaAllocator(BoundedAllocator* allocator, MemoryTag tag);
  ArenaAllocator(const ArenaAllocator&) = delete;
  ArenaAllocator& operator=(const ArenaAllocator&) = delete;
  ArenaAllocator(ArenaAllocator&& other) noexcept;
  ArenaAllocator& operator=(ArenaAllocator&& other) noexcept;
  ~ArenaAllocator();

  AllocationResult Allocate(usize bytes, usize alignment = 0);
  DeallocationResult Reset();
  MemoryAccountingSnapshot Snapshot() const;

 private:
  struct Chunk {
    void* pointer = nullptr;
    usize bytes = 0;
    usize alignment = 0;
    usize used = 0;
  };

  BoundedAllocator* allocator_ = nullptr;
  MemoryTag tag_;
  std::vector<Chunk> chunks_;
};

class MemoryManager {
 public:
  explicit MemoryManager(AllocationPolicy policy);
  MemoryManager(const MemoryManager&) = delete;
  MemoryManager& operator=(const MemoryManager&) = delete;

  AllocationResult Allocate(usize bytes, usize alignment, MemoryTag tag);
  AllocationResult AllocateZeroed(usize bytes, usize alignment, MemoryTag tag);
  DeallocationResult Deallocate(void* pointer, MemoryTag tag);
  PageBufferResult AllocatePageBuffer(PageBufferRequest request);
  DeallocationResult ReleasePageBuffer(PageBuffer buffer, MemoryTag tag);
  ProtectedBufferResult AllocateProtected(ProtectedMemoryRequest request);
  ScopedAllocationResult AllocateScoped(usize bytes, usize alignment, MemoryTag tag);
  ScopedPageBufferResult AllocateScopedPageBuffer(PageBufferRequest request);
  ArenaAllocator CreateArena(MemoryTag tag);
  MemoryAccountingSnapshot Snapshot() const;
  const AllocationPolicy& policy() const;
  MemoryFailureInjectionConfigurationResult EnableAllocationFailureInjection(
      MemoryFailureInjectionConfiguration configuration);
  MemoryFailureInjectionConfigurationResult DisableAllocationFailureInjection();
  MemoryFailureInjectionSnapshot FailureInjectionSnapshot() const;
  BoundedAllocator* allocator();

 private:
  BoundedAllocator allocator_;
};

bool IsPowerOfTwo(usize value);
usize DefaultPageBufferAlignment();
bool IsSupportedPageBufferSize(usize page_size);
const char* MemoryCategoryName(MemoryCategory category);
const char* MemoryLifetimeName(MemoryLifetime lifetime);
const char* ProtectedMemoryPlatformPolicyName(ProtectedMemoryPlatformPolicy policy);
void SecureZeroMemory(void* pointer, usize bytes);
std::vector<MemoryCategory> ReservedMemoryCategories();
AllocationPolicy DefaultLocalEngineMemoryPolicy();
DefaultMemoryManagerConfigurationResult ConfigureDefaultMemoryManager(
    AllocationPolicy policy,
    std::string provenance = "runtime_config");
DefaultMemoryManagerConfigurationResult ConfigureDefaultMemoryManagerForFixture(
    AllocationPolicy policy,
    std::string fixture_name);
DefaultMemoryManagerStateSnapshot DefaultMemoryManagerState();
MemoryManager& DefaultMemoryManager();

}  // namespace scratchbird::core::memory
