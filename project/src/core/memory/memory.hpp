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
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
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

using MemoryBinaryUuid = std::array<std::uint8_t, 16>;
enum class MemoryBinaryScopeKind : std::uint8_t {
  context, owner, database, session, transaction, statement, query,
  process, tenant, user, role, operator_scope, page_cache, background, plugin,
  connection, cursor, plan_cache_entry, prepared_statement, descriptor_snapshot
};
struct MemoryBinaryScopeKey {
  MemoryBinaryScopeKind kind = MemoryBinaryScopeKind::context;
  MemoryBinaryUuid uuid{};
  auto operator<=>(const MemoryBinaryScopeKey&) const = default;
};
constexpr bool MemoryUuidPresent(const MemoryBinaryUuid& uuid) {
  for (auto byte : uuid) if (byte != 0) return true;
  return false;
}
constexpr bool MemorySystemUuidValid(const MemoryBinaryUuid& uuid) {
  return MemoryUuidPresent(uuid) && (uuid[6] & 0xf0u) == 0x70u &&
         (uuid[8] & 0xc0u) == 0x80u;
}
constexpr const char* MemoryBinaryScopeKindName(MemoryBinaryScopeKind kind) {
  switch (kind) {
    case MemoryBinaryScopeKind::context: return "context";
    case MemoryBinaryScopeKind::owner: return "owner";
    case MemoryBinaryScopeKind::database: return "database";
    case MemoryBinaryScopeKind::session: return "session";
    case MemoryBinaryScopeKind::transaction: return "transaction";
    case MemoryBinaryScopeKind::statement: return "statement";
    case MemoryBinaryScopeKind::query: return "query";
    case MemoryBinaryScopeKind::process: return "process";
    case MemoryBinaryScopeKind::tenant: return "tenant";
    case MemoryBinaryScopeKind::user: return "user";
    case MemoryBinaryScopeKind::role: return "role";
    case MemoryBinaryScopeKind::operator_scope: return "operator";
    case MemoryBinaryScopeKind::page_cache: return "page_cache";
    case MemoryBinaryScopeKind::background: return "background";
    case MemoryBinaryScopeKind::plugin: return "plugin";
    case MemoryBinaryScopeKind::connection: return "connection";
    case MemoryBinaryScopeKind::cursor: return "cursor";
    case MemoryBinaryScopeKind::plan_cache_entry: return "plan_cache_entry";
    case MemoryBinaryScopeKind::prepared_statement: return "prepared_statement";
    case MemoryBinaryScopeKind::descriptor_snapshot: return "descriptor_snapshot";
  }
  return "invalid";
}
struct MemoryBinaryOwnership {
  std::array<MemoryBinaryUuid, 7> scopes{};
  bool empty() const {
    for (const auto& uuid : scopes) if (MemoryUuidPresent(uuid)) return false;
    return true;
  }
  auto& operator[](MemoryBinaryScopeKind kind) { return scopes.at(static_cast<usize>(kind)); }
  const auto& operator[](MemoryBinaryScopeKind kind) const { return scopes.at(static_cast<usize>(kind)); }
  bool operator==(const MemoryBinaryOwnership&) const = default;
};
// Legacy label keys remain for existing callers pending their migration.
// Binary identities never pass through the legacy string alternative.
using MemoryContextKey = std::variant<std::pair<std::string, std::string>, MemoryBinaryScopeKey>;

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
  MemoryBinaryOwnership binary_ownership;
};

inline bool MemoryBinaryOwnershipValid(const MemoryTag& tag) {
  if (tag.binary_ownership.empty()) return true;
  if (!tag.owner.empty() || !tag.context_id.empty() || !tag.database_id.empty() ||
      !tag.session_id.empty() || !tag.transaction_id.empty() ||
      !tag.statement_id.empty() || !tag.query_id.empty()) return false;
  for (const auto& uuid : tag.binary_ownership.scopes)
    if (MemoryUuidPresent(uuid) && !MemorySystemUuidValid(uuid)) return false;
  return MemorySystemUuidValid(tag.binary_ownership[MemoryBinaryScopeKind::context]) &&
         MemorySystemUuidValid(tag.binary_ownership[MemoryBinaryScopeKind::owner]);
}

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
  u64 reserved_capacity_bytes = 0;
};

struct MemoryContextSnapshot {
  std::string scope_kind;
  std::string scope_id;
  std::optional<MemoryBinaryScopeKey> binary_scope;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 deallocation_count = 0;
  u64 failure_count = 0;
  u64 active_allocation_count = 0;
  u64 reserved_capacity_bytes = 0;
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
  // Optional diagnostic/telemetry detail dropped under allocation pressure.
  // Ownership and authoritative byte accounting are never dropped.
  u64 telemetry_truncation_count = 0;
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
  // Unconsumed admission credits, not resident/committed storage.
  u64 reserved_capacity_bytes = 0;
  u64 active_capacity_reservation_count = 0;
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
  MemoryBinaryUuid binary_scope_uuid{};
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
  // Empty on an unissued/failed request; successful allocation fills evidence.
  // Default construction and moved-from cleanup must not allocate.
  std::string platform_name;
  std::string authority_scope;
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

class BoundedAllocator;
class MemoryCapacityReservation {
 public:
  // The allocator must outlive this lease and all buffers allocated from it.
  // Quiesce lease users before destruction. Closing releases unused admission
  // credit, not outstanding storage: those buffers remain allocator-owned and
  // charged until explicitly freed. Their release cannot resurrect a closed
  // lease. Reserved allocation uses the exact immutable admitted owner tag.
  MemoryCapacityReservation(const MemoryCapacityReservation&) = delete;
  MemoryCapacityReservation& operator=(const MemoryCapacityReservation&) = delete;
  ~MemoryCapacityReservation();
  AllocationResult Allocate(usize bytes, usize alignment = 0);
 private:
  MemoryCapacityReservation(BoundedAllocator* allocator, u64 id, MemoryTag tag)
      : allocator_(allocator), id_(id), tag_(std::move(tag)) {}
  BoundedAllocator* allocator_;
  u64 id_;
  MemoryTag tag_;
  friend class BoundedAllocator;
};
struct MemoryCapacityReservationResult {
  Status status;
  std::unique_ptr<MemoryCapacityReservation> reservation;
  bool ok() const { return status.ok() && reservation != nullptr; }
};

struct MemoryCapacityAvailability {
  Status status;
  usize available_bytes = 0;
  bool ok() const { return status.ok(); }
};

class BoundedAllocator {
 public:
  explicit BoundedAllocator(AllocationPolicy policy);
  BoundedAllocator(const BoundedAllocator&) = delete;
  BoundedAllocator& operator=(const BoundedAllocator&) = delete;
  ~BoundedAllocator();

  AllocationResult Allocate(usize bytes, usize alignment, MemoryTag tag);
  MemoryCapacityReservationResult ReserveCapacity(usize bytes, MemoryTag tag);
  // Allocation-free, read-only policy headroom under the allocator lock.
  // Includes live bytes and unconsumed capacity credit at every applicable
  // scope. This is a planning hint, not a reservation: Allocate/ReserveCapacity
  // must still perform their authoritative admission checks.
  MemoryCapacityAvailability AvailableCapacity(const MemoryTag& tag) const;
  AllocationResult AllocateZeroed(usize bytes, usize alignment, MemoryTag tag);
  AllocationResult Reallocate(void* pointer, usize bytes, usize alignment, MemoryTag tag);
  DeallocationResult Deallocate(void* pointer, MemoryTag tag);
  // Forced owner cleanup bypasses test refusal and never allocates diagnostics.
  Status DeallocateNoAlloc(void* pointer);
  DeallocationResult DeallocateProtected(void* pointer, MemoryTag tag,
                                       const ProtectedMemoryEvidence& evidence);
  Status DeallocateProtectedNoAlloc(void* pointer, const ProtectedMemoryEvidence& evidence);
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
  friend class MemoryCapacityReservation;
  AllocationResult AllocateImpl(usize bytes, usize alignment, MemoryTag tag, u64 capacity_id);
  void CloseCapacity(u64 id);
  struct CapacityRecord {
    u64 unused_bytes = 0;
    u64 live_bytes = 0;
    bool open = true;
    MemoryCategory category = MemoryCategory::unknown;
    std::vector<MemoryContextKey> context_keys;
  };
  void AddCapacityCredits(CapacityRecord& record, u64 bytes);
  void ConsumeCapacityCredits(CapacityRecord& record, u64 bytes);
  struct FailureTelemetryExit {
    BoundedAllocator* allocator;
    ~FailureTelemetryExit() { allocator->PublishFailureTelemetryNoThrow(); }
  };
  void PublishFailureTelemetryNoThrow();
  using PendingFailureCounts = std::array<std::array<u64, 3>,
      static_cast<usize>(MemoryCategory::test_probe) + 1>;
  PendingFailureCounts pending_failure_counts_{};
  DeallocationResult DeallocateImpl(void* pointer, MemoryTag tag,
                                   const ProtectedMemoryEvidence* evidence);
  Status DeallocateNoAllocImpl(void* pointer, const ProtectedMemoryEvidence* evidence);
  struct AllocationRecord {
    usize bytes = 0;
    usize alignment = 0;
    MemoryTag tag;
    u64 sharded_token_id = 0;
    usize sharded_shard_index = 0;
    bool sharded_accounting_committed = false;
    std::vector<MemoryContextKey> context_keys;
    u64 capacity_id = 0;
  };

  struct CategoryAccounting {
    u64 current_bytes = 0;
    u64 peak_bytes = 0;
    u64 allocation_count = 0;
    u64 deallocation_count = 0;
    u64 failure_count = 0;
    u64 active_allocation_count = 0;
    u64 reserved_capacity_bytes = 0;
  };

  struct ContextAccounting {
    u64 current_bytes = 0;
    u64 peak_bytes = 0;
    u64 allocation_count = 0;
    u64 deallocation_count = 0;
    u64 failure_count = 0;
    u64 active_allocation_count = 0;
    u64 reserved_capacity_bytes = 0;
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
  AllocationResult AllocateRecorded(usize bytes, usize alignment, const MemoryTag& tag);
  void ApplyAllocationRemovalAccounting(const AllocationRecord& record);
  AllocationRecord RemoveAllocation(void* pointer, bool* found);

  AllocationPolicy policy_;
  mutable std::mutex mutex_;
  MemoryAccountingSnapshot accounting_;
  std::unique_ptr<ShardedMemoryAccountingLedger> sharded_accounting_;
  std::unordered_map<void*, AllocationRecord> active_;
  std::map<MemoryCategory, CategoryAccounting> category_accounting_;
  std::map<MemoryContextKey, ContextAccounting> context_accounting_;
  std::map<u64, CapacityRecord> capacity_reservations_;
  u64 next_capacity_id_ = 1;
  FailureInjectionState failure_injection_;
};

// SB-MEMORY-ARENA-ANCHOR
struct ArenaCapacitySnapshot {
  u64 retained_bytes = 0;
  u64 consumed_bytes = 0;
  u64 chunk_count = 0;
};

struct ArenaAllocationPlan {
  Status status;
  usize alignment = 0;
  usize growth_bytes = 0;
  bool ok() const { return status.ok(); }
};

class ArenaAllocator {
 public:
  ArenaAllocator(BoundedAllocator* allocator, MemoryTag tag);
  ArenaAllocator(const ArenaAllocator&) = delete;
  ArenaAllocator& operator=(const ArenaAllocator&) = delete;
  ArenaAllocator(ArenaAllocator&& other) noexcept;
  ArenaAllocator& operator=(ArenaAllocator&& other) noexcept;
  ~ArenaAllocator();

  AllocationResult Allocate(usize bytes, usize alignment = 0);
  // Plans do not mutate the bump cursor or allocate. Callers serialize planning
  // and allocation, as with all other operations on this arena.
  ArenaAllocationPlan PlanAllocation(usize bytes, usize alignment,
                                    usize growth_limit_bytes) const noexcept;
  // New backing is exactly the planned growth, or the allocation fails. This
  // path never silently falls back to a differently sized backing allocation.
  AllocationResult AllocateWithinCapacity(usize bytes, usize alignment,
                                         usize growth_limit_bytes);
  ArenaCapacitySnapshot CapacitySnapshot() const noexcept;
  DeallocationResult Reset();
  // Legacy backing-allocator diagnostics; use CapacitySnapshot for this arena's
  // own physical chunks and admission, never differences of global snapshots.
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
