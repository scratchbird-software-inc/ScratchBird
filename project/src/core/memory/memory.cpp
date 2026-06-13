// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"

#include "metric_producer.hpp"
#include "sharded_memory_accounting_ledger.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef SecureZeroMemory
#undef SecureZeroMemory
#endif
#else
#include <cstdint>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/mman.h>
#endif
#endif

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::StatusCode;

constexpr usize kDefaultAlignment = alignof(std::max_align_t);
constexpr usize kDefaultPageBufferAlignment = 4096;
constexpr usize kDefaultArenaChunkBytes = 4096;
constexpr usize kDefaultAllocatorAccountingShards = 64;
constexpr usize kMinimumPageSize = 1024;
constexpr usize kMaximumPageSize = 1024 * 1024;
constexpr const char* kDefaultAllocationCallsite = "core.memory.allocate";
constexpr const char* kDefaultDeallocationCallsite = "core.memory.deallocate";
constexpr const char* kReallocateActiveMapValidationCallsite = "core.memory.reallocate.active_map_validation";
constexpr const char* kPageBufferAllocationCallsite = "core.memory.page_buffer.allocate";
constexpr const char* kFailureInjectionAuthorityNote =
    "test_fixture_evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority";
constexpr const char* kProtectedMemoryAllocationCallsite = "core.memory.protected.allocate";
constexpr const char* kProtectedMemoryAuthorityNote =
    "protected_memory_evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";

u64 EffectiveHardLimit(const AllocationPolicy& policy) {
  if (policy.hard_limit_bytes != 0) {
    return policy.hard_limit_bytes;
  }
  return policy.byte_limit;
}

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

using Clock = std::chrono::steady_clock;

double ElapsedMicros(Clock::time_point start) {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

void PublishMemorySnapshot(const MemoryAccountingSnapshot& snapshot, const AllocationPolicy& policy) {
  (void)scratchbird::core::metrics::SetGauge(
      "sb_memory_allocated_bytes",
      scratchbird::core::metrics::Labels({{"component", "core.memory"}, {"operation", "snapshot"}}),
      static_cast<double>(snapshot.current_bytes),
      "core_memory");
  (void)scratchbird::core::metrics::SetGauge(
      "sb_memory_emergency_reserve_bytes",
      scratchbird::core::metrics::Labels({{"component", "core.memory"}, {"operation", "snapshot"}}),
      EffectiveHardLimit(policy) > snapshot.current_bytes ? static_cast<double>(EffectiveHardLimit(policy) - snapshot.current_bytes) : 0.0,
      "core_memory");
  for (const auto& category : snapshot.categories) {
    (void)scratchbird::core::metrics::SetGauge(
        "sb_memory_allocated_bytes",
        scratchbird::core::metrics::Labels({{"component", "core.memory"}, {"operation", "snapshot"},
                                            {"producer", MemoryCategoryName(category.category)}}),
        static_cast<double>(category.current_bytes),
        "core_memory");
  }
}

std::string PlatformNameForProtectedMemory() {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  return "bsd";
#else
  return "unsupported";
#endif
}

bool ProtectedPolicyRequiresLock(ProtectedMemoryPlatformPolicy policy) {
  return policy == ProtectedMemoryPlatformPolicy::require_platform_lock ||
         policy == ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
}

bool ProtectedPolicyRequiresNoDump(ProtectedMemoryPlatformPolicy policy) {
  return policy == ProtectedMemoryPlatformPolicy::require_no_dump ||
         policy == ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
}

std::string RedactProtectedMemoryText(std::string text) {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  const char* markers[] = {"secret", "password", "passwd", "pwd=", "credential", "verifier",
                           "private_key", "key_material", "plaintext", "cleartext", "token",
                           "api_key", "apikey", "seed"};
  for (const char* marker : markers) {
    if (lower.find(marker) != std::string::npos) {
      return "<protected-material-redacted>";
    }
  }
  return text.empty() ? std::string("protected_material") : std::move(text);
}

#if !defined(_WIN32) && (defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__))
std::uintptr_t PageAlignDown(std::uintptr_t value, std::uintptr_t page_size) {
  return value & ~(page_size - 1u);
}

usize PageAlignLength(void* pointer, usize bytes) {
  const long sys_page_size = ::sysconf(_SC_PAGESIZE);
  const auto page_size = sys_page_size > 0 ? static_cast<std::uintptr_t>(sys_page_size) : 4096u;
  const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
  const auto aligned_begin = PageAlignDown(begin, page_size);
  const auto end = begin + static_cast<std::uintptr_t>(bytes);
  const auto aligned_end = (end + page_size - 1u) & ~(page_size - 1u);
  return static_cast<usize>(aligned_end - aligned_begin);
}

void* PageAlignPointer(void* pointer) {
  const long sys_page_size = ::sysconf(_SC_PAGESIZE);
  const auto page_size = sys_page_size > 0 ? static_cast<std::uintptr_t>(sys_page_size) : 4096u;
  const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
  return reinterpret_cast<void*>(PageAlignDown(begin, page_size));
}
#endif

void ApplyProtectedPlatformEvidence(void* pointer,
                                    usize bytes,
                                    ProtectedMemoryPlatformPolicy policy,
                                    ProtectedMemoryEvidence* evidence) {
  evidence->platform_name = PlatformNameForProtectedMemory();
  evidence->platform_lock_attempted = true;
  evidence->no_dump_attempted = true;
#if defined(_WIN32)
  evidence->platform_lock_supported = true;
  evidence->platform_lock_succeeded = ::VirtualLock(pointer, bytes) != 0;
  evidence->no_dump_supported = false;
  evidence->no_dump_succeeded = false;
  (void)policy;
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  evidence->platform_lock_supported = true;
  evidence->platform_lock_succeeded = ::mlock(pointer, bytes) == 0;
#if defined(MADV_DONTDUMP)
  evidence->no_dump_supported = true;
  evidence->no_dump_succeeded =
      ::madvise(PageAlignPointer(pointer), PageAlignLength(pointer, bytes), MADV_DONTDUMP) == 0;
#else
  evidence->no_dump_supported = false;
  evidence->no_dump_succeeded = false;
#endif
  (void)policy;
#else
  evidence->platform_lock_supported = false;
  evidence->platform_lock_succeeded = false;
  evidence->no_dump_supported = false;
  evidence->no_dump_succeeded = false;
  (void)pointer;
  (void)bytes;
  (void)policy;
#endif
}

void ReleaseProtectedPlatformEvidence(void* pointer, usize bytes, const ProtectedMemoryEvidence& evidence) {
#if defined(_WIN32)
  if (evidence.platform_lock_succeeded) {
    (void)::VirtualUnlock(pointer, bytes);
  }
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#if defined(MADV_DODUMP)
  if (evidence.no_dump_succeeded) {
    (void)::madvise(PageAlignPointer(pointer), PageAlignLength(pointer, bytes), MADV_DODUMP);
  }
#endif
  if (evidence.platform_lock_succeeded) {
    (void)::munlock(pointer, bytes);
  }
#else
  (void)pointer;
  (void)bytes;
  (void)evidence;
#endif
}

bool AllocationPoliciesEquivalent(const AllocationPolicy& lhs, const AllocationPolicy& rhs) {
  return lhs.policy_name == rhs.policy_name &&
         lhs.byte_limit == rhs.byte_limit &&
         lhs.hard_limit_bytes == rhs.hard_limit_bytes &&
         lhs.soft_limit_bytes == rhs.soft_limit_bytes &&
         lhs.per_context_limit_bytes == rhs.per_context_limit_bytes &&
         lhs.page_buffer_pool_limit_bytes == rhs.page_buffer_pool_limit_bytes &&
         lhs.failure_mode == rhs.failure_mode &&
         lhs.track_allocations == rhs.track_allocations &&
         lhs.zero_memory_on_allocate == rhs.zero_memory_on_allocate &&
         lhs.zero_memory_on_release == rhs.zero_memory_on_release &&
         lhs.reject_over_soft_limit == rhs.reject_over_soft_limit &&
         lhs.refuse_all_allocations == rhs.refuse_all_allocations;
}

void SubtractCounter(u64& value, u64 delta) {
  value = value >= delta ? value - delta : 0;
}

void DecrementCounter(u64& value) {
  if (value > 0) {
    --value;
  }
}

std::mutex& DefaultMemoryManagerMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unique_ptr<MemoryManager>& DefaultMemoryManagerStorage() {
  static std::unique_ptr<MemoryManager> manager;
  return manager;
}

AllocationPolicy& DefaultMemoryManagerConfiguredPolicy() {
  static AllocationPolicy policy = DefaultLocalEngineMemoryPolicy();
  return policy;
}

struct DefaultMemoryManagerRuntimeState {
  bool explicitly_configured = false;
  bool fixture_mode = false;
  std::string provenance;
};

DefaultMemoryManagerRuntimeState& DefaultMemoryManagerRuntimeStateStorage() {
  static DefaultMemoryManagerRuntimeState state;
  return state;
}

AllocationPolicy UnconfiguredDefaultMemoryManagerPolicy() {
  AllocationPolicy policy;
  policy.policy_name = "default_memory_manager_unconfigured_refuse_all";
  policy.hard_limit_bytes = 1;
  policy.soft_limit_bytes = 1;
  policy.per_context_limit_bytes = 1;
  policy.page_buffer_pool_limit_bytes = 1;
  policy.failure_mode = AllocationFailureMode::fatal_status;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  policy.reject_over_soft_limit = true;
  policy.refuse_all_allocations = true;
  return policy;
}

MemoryManager& UnconfiguredDefaultMemoryManager() {
  static MemoryManager manager(UnconfiguredDefaultMemoryManagerPolicy());
  return manager;
}

DiagnosticRecord DefaultManagerDiagnostic(Status status,
                                          std::string code,
                                          std::string message_key,
                                          std::vector<DiagnosticArgument> arguments) {
  return MakeDiagnostic(status.code,
                        status.severity,
                        Subsystem::memory,
                        std::move(code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.default_manager",
                        "Install the memory policy during server bootstrap before memory-managed subsystems are initialized.");
}

using ContextKey = std::pair<std::string, std::string>;

void AddContextKey(std::vector<ContextKey>* keys,
                   std::set<ContextKey>* seen,
                   std::string scope_kind,
                   const std::string& scope_id) {
  if (scope_id.empty()) {
    return;
  }
  ContextKey key{std::move(scope_kind), scope_id};
  if (seen->insert(key).second) {
    keys->push_back(std::move(key));
  }
}

std::vector<ContextKey> ContextKeysForTag(const MemoryTag& tag) {
  std::vector<ContextKey> keys;
  std::set<ContextKey> seen;
  AddContextKey(&keys, &seen, "context", tag.context_id);
  AddContextKey(&keys, &seen, "owner", tag.owner);
  AddContextKey(&keys, &seen, "database", tag.database_id);
  AddContextKey(&keys, &seen, "session", tag.session_id);
  AddContextKey(&keys, &seen, "transaction", tag.transaction_id);
  AddContextKey(&keys, &seen, "statement", tag.statement_id);
  AddContextKey(&keys, &seen, "query", tag.query_id);
  return keys;
}

std::vector<std::string> ShardedScopeIdsForTag(const MemoryTag& tag) {
  std::vector<std::string> scopes;
  for (const auto& key : ContextKeysForTag(tag)) {
    if (key.first == "context" || key.second.empty()) {
      continue;
    }
    scopes.push_back(key.first + ":" + key.second);
  }
  return scopes;
}

const char* MemoryFailureInjectionScopeKindName(MemoryFailureInjectionScopeKind kind) {
  switch (kind) {
    case MemoryFailureInjectionScopeKind::any: return "any";
    case MemoryFailureInjectionScopeKind::context: return "context";
    case MemoryFailureInjectionScopeKind::owner: return "owner";
    case MemoryFailureInjectionScopeKind::database: return "database";
    case MemoryFailureInjectionScopeKind::session: return "session";
    case MemoryFailureInjectionScopeKind::transaction: return "transaction";
    case MemoryFailureInjectionScopeKind::statement: return "statement";
    case MemoryFailureInjectionScopeKind::query: return "query";
  }
  return "unknown";
}

std::string ScopeValueForTag(MemoryFailureInjectionScopeKind kind, const MemoryTag& tag) {
  switch (kind) {
    case MemoryFailureInjectionScopeKind::any: return {};
    case MemoryFailureInjectionScopeKind::context: return tag.context_id;
    case MemoryFailureInjectionScopeKind::owner: return tag.owner;
    case MemoryFailureInjectionScopeKind::database: return tag.database_id;
    case MemoryFailureInjectionScopeKind::session: return tag.session_id;
    case MemoryFailureInjectionScopeKind::transaction: return tag.transaction_id;
    case MemoryFailureInjectionScopeKind::statement: return tag.statement_id;
    case MemoryFailureInjectionScopeKind::query: return tag.query_id;
  }
  return {};
}

bool FailureInjectionRuleMatches(const MemoryFailureInjectionRule& rule, const MemoryTag& tag) {
  if (!rule.enabled) {
    return false;
  }
  if (!rule.callsite.empty() && rule.callsite != tag.callsite) {
    return false;
  }
  if (!rule.purpose.empty() && rule.purpose != tag.purpose) {
    return false;
  }
  if (rule.category != MemoryCategory::unknown && rule.category != tag.category) {
    return false;
  }
  if (rule.scope_kind != MemoryFailureInjectionScopeKind::any) {
    return !rule.scope_id.empty() && ScopeValueForTag(rule.scope_kind, tag) == rule.scope_id;
  }
  return true;
}

MemoryFailureInjectionConfigurationResult FailureInjectionConfigurationFailure(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments) {
  MemoryFailureInjectionConfigurationResult result;
  result.status = status;
  result.blocked = true;
  arguments.push_back({"authority_scope", kFailureInjectionAuthorityNote});
  result.diagnostic = MakeDiagnostic(status.code,
                                     status.severity,
                                     status.subsystem,
                                     std::move(diagnostic_code),
                                     std::move(message_key),
                                     std::move(arguments),
                                     {},
                                     "core.memory.failure_injection",
                                     "Enable allocation failure injection only from an explicit test fixture binary.");
  return result;
}

}  // namespace

bool IsPowerOfTwo(usize value) {
  return value != 0 && (value & (value - 1)) == 0;
}

usize DefaultPageBufferAlignment() {
  return kDefaultPageBufferAlignment;
}

bool IsSupportedPageBufferSize(usize page_size) {
  return page_size >= kMinimumPageSize &&
         page_size <= kMaximumPageSize &&
         IsPowerOfTwo(page_size);
}

const char* MemoryCategoryName(MemoryCategory category) {
  switch (category) {
    case MemoryCategory::unknown: return "unknown";
    case MemoryCategory::core_runtime: return "core_runtime";
    case MemoryCategory::page_buffer: return "page_buffer";
    case MemoryCategory::catalog_bootstrap: return "catalog_bootstrap";
    case MemoryCategory::resource_seed: return "resource_seed";
    case MemoryCategory::datatype_payload: return "datatype_payload";
    case MemoryCategory::transaction_local: return "transaction_local";
    case MemoryCategory::transaction_snapshot: return "transaction_snapshot";
    case MemoryCategory::version_chain: return "version_chain";
    case MemoryCategory::cleanup: return "cleanup";
    case MemoryCategory::archive: return "archive";
    case MemoryCategory::metrics: return "metrics";
    case MemoryCategory::diagnostics: return "diagnostics";
    case MemoryCategory::executor_query_reserved: return "executor_query_reserved";
    case MemoryCategory::parser_handoff_reserved: return "parser_handoff_reserved";
    case MemoryCategory::udr_reserved: return "udr_reserved";
    case MemoryCategory::llvm_code_reserved: return "llvm_code_reserved";
    case MemoryCategory::llvm_data_reserved: return "llvm_data_reserved";
    case MemoryCategory::gpu_host_pinned_reserved: return "gpu_host_pinned_reserved";
    case MemoryCategory::gpu_device_reserved: return "gpu_device_reserved";
    case MemoryCategory::cluster_control_reserved: return "cluster_control_reserved";
    case MemoryCategory::cluster_decision_reserved: return "cluster_decision_reserved";
    case MemoryCategory::test_probe: return "test_probe";
  }
  return "unknown";
}

const char* MemoryLifetimeName(MemoryLifetime lifetime) {
  switch (lifetime) {
    case MemoryLifetime::unknown: return "unknown";
    case MemoryLifetime::process: return "process";
    case MemoryLifetime::database: return "database";
    case MemoryLifetime::connection: return "connection";
    case MemoryLifetime::transaction: return "transaction";
    case MemoryLifetime::statement: return "statement";
    case MemoryLifetime::page_buffer: return "page_buffer";
    case MemoryLifetime::arena: return "arena";
    case MemoryLifetime::temporary: return "temporary";
    case MemoryLifetime::deferred_epoch: return "deferred_epoch";
    case MemoryLifetime::static_reserved: return "static_reserved";
  }
  return "unknown";
}

const char* ProtectedMemoryPlatformPolicyName(ProtectedMemoryPlatformPolicy policy) {
  switch (policy) {
    case ProtectedMemoryPlatformPolicy::best_effort:
      return "best_effort";
    case ProtectedMemoryPlatformPolicy::require_platform_lock:
      return "require_platform_lock";
    case ProtectedMemoryPlatformPolicy::require_no_dump:
      return "require_no_dump";
    case ProtectedMemoryPlatformPolicy::require_lock_and_no_dump:
      return "require_lock_and_no_dump";
  }
  return "best_effort";
}

void SecureZeroMemory(void* pointer, usize bytes) {
  if (pointer == nullptr || bytes == 0) {
    return;
  }
  volatile unsigned char* out = static_cast<volatile unsigned char*>(pointer);
  for (usize i = 0; i < bytes; ++i) {
    out[i] = 0;
  }
}

std::vector<MemoryCategory> ReservedMemoryCategories() {
  return {MemoryCategory::executor_query_reserved,
          MemoryCategory::parser_handoff_reserved,
          MemoryCategory::udr_reserved,
          MemoryCategory::llvm_code_reserved,
          MemoryCategory::llvm_data_reserved,
          MemoryCategory::gpu_host_pinned_reserved,
          MemoryCategory::gpu_device_reserved,
          MemoryCategory::cluster_control_reserved,
          MemoryCategory::cluster_decision_reserved};
}

AllocationPolicy DefaultLocalEngineMemoryPolicy() {
  AllocationPolicy policy;
  policy.policy_name = "local_engine_default_bounded";
  policy.hard_limit_bytes = 256ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 192ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.failure_mode = AllocationFailureMode::return_error;
  policy.track_allocations = true;
  policy.zero_memory_on_allocate = false;
  policy.zero_memory_on_release = true;
  policy.reject_over_soft_limit = false;
  policy.refuse_all_allocations = false;
  return policy;
}

ScopedAllocation::ScopedAllocation(BoundedAllocator* allocator,
                                   void* pointer,
                                   usize bytes,
                                   usize alignment,
                                   MemoryTag tag)
    : allocator_(allocator), pointer_(pointer), bytes_(bytes), alignment_(alignment), tag_(std::move(tag)) {}

ScopedAllocation::ScopedAllocation(ScopedAllocation&& other) noexcept
    : allocator_(other.allocator_),
      pointer_(other.pointer_),
      bytes_(other.bytes_),
      alignment_(other.alignment_),
      tag_(std::move(other.tag_)) {
  other.allocator_ = nullptr;
  other.pointer_ = nullptr;
  other.bytes_ = 0;
  other.alignment_ = 0;
}

ScopedAllocation& ScopedAllocation::operator=(ScopedAllocation&& other) noexcept {
  if (this != &other) {
    Reset();
    allocator_ = other.allocator_;
    pointer_ = other.pointer_;
    bytes_ = other.bytes_;
    alignment_ = other.alignment_;
    tag_ = std::move(other.tag_);
    other.allocator_ = nullptr;
    other.pointer_ = nullptr;
    other.bytes_ = 0;
    other.alignment_ = 0;
  }
  return *this;
}

ScopedAllocation::~ScopedAllocation() {
  Reset();
}

DeallocationResult ScopedAllocation::Reset() {
  if (allocator_ == nullptr || pointer_ == nullptr) {
    return {OkStatus(), {}};
  }
  void* pointer = pointer_;
  pointer_ = nullptr;
  bytes_ = 0;
  alignment_ = 0;
  return allocator_->Deallocate(pointer, tag_);
}

ScopedPageBuffer::ScopedPageBuffer(BoundedAllocator* allocator, PageBuffer buffer, MemoryTag tag)
    : allocator_(allocator), buffer_(buffer), tag_(std::move(tag)) {}

ScopedPageBuffer::ScopedPageBuffer(ScopedPageBuffer&& other) noexcept
    : allocator_(other.allocator_), buffer_(other.buffer_), tag_(std::move(other.tag_)) {
  other.allocator_ = nullptr;
  other.buffer_ = {};
}

ScopedPageBuffer& ScopedPageBuffer::operator=(ScopedPageBuffer&& other) noexcept {
  if (this != &other) {
    Reset();
    allocator_ = other.allocator_;
    buffer_ = other.buffer_;
    tag_ = std::move(other.tag_);
    other.allocator_ = nullptr;
    other.buffer_ = {};
  }
  return *this;
}

ScopedPageBuffer::~ScopedPageBuffer() {
  Reset();
}

DeallocationResult ScopedPageBuffer::Reset() {
  if (allocator_ == nullptr || !buffer_.valid()) {
    return {OkStatus(), {}};
  }
  PageBuffer buffer = buffer_;
  buffer_ = {};
  return allocator_->ReleasePageBuffer(buffer, tag_);
}

ScopedProtectedBuffer::ScopedProtectedBuffer(BoundedAllocator* allocator,
                                             void* pointer,
                                             usize bytes,
                                             usize alignment,
                                             MemoryTag tag,
                                             ProtectedMemoryEvidence evidence)
    : allocator_(allocator),
      pointer_(pointer),
      bytes_(bytes),
      alignment_(alignment),
      tag_(std::move(tag)),
      evidence_(std::move(evidence)) {}

ScopedProtectedBuffer::ScopedProtectedBuffer(ScopedProtectedBuffer&& other) noexcept
    : allocator_(other.allocator_),
      pointer_(other.pointer_),
      bytes_(other.bytes_),
      alignment_(other.alignment_),
      tag_(std::move(other.tag_)),
      evidence_(std::move(other.evidence_)) {
  other.allocator_ = nullptr;
  other.pointer_ = nullptr;
  other.bytes_ = 0;
  other.alignment_ = 0;
  other.evidence_ = {};
}

ScopedProtectedBuffer& ScopedProtectedBuffer::operator=(ScopedProtectedBuffer&& other) noexcept {
  if (this != &other) {
    Reset();
    allocator_ = other.allocator_;
    pointer_ = other.pointer_;
    bytes_ = other.bytes_;
    alignment_ = other.alignment_;
    tag_ = std::move(other.tag_);
    evidence_ = std::move(other.evidence_);
    other.allocator_ = nullptr;
    other.pointer_ = nullptr;
    other.bytes_ = 0;
    other.alignment_ = 0;
    other.evidence_ = {};
  }
  return *this;
}

ScopedProtectedBuffer::~ScopedProtectedBuffer() {
  Reset();
}

void ScopedProtectedBuffer::Zeroize() {
  SecureZeroMemory(pointer_, bytes_);
}

DeallocationResult ScopedProtectedBuffer::Reset() {
  if (allocator_ == nullptr || pointer_ == nullptr) {
    return {OkStatus(), {}};
  }
  void* pointer = pointer_;
  const usize bytes = bytes_;
  pointer_ = nullptr;
  bytes_ = 0;
  alignment_ = 0;
  SecureZeroMemory(pointer, bytes);
  ReleaseProtectedPlatformEvidence(pointer, bytes, evidence_);
  return allocator_->Deallocate(pointer, tag_);
}

BoundedAllocator::BoundedAllocator(AllocationPolicy policy)
    : policy_(std::move(policy)),
      sharded_accounting_(std::make_unique<ShardedMemoryAccountingLedger>(
          kDefaultAllocatorAccountingShards)) {
  accounting_.reserved_categories = ReservedMemoryCategories();
}

BoundedAllocator::~BoundedAllocator() = default;

AllocationResult BoundedAllocator::Allocate(usize bytes, usize alignment, MemoryTag tag) {
  const auto metric_start = Clock::now();
  AllocationResult result;
  result.status = OkStatus();
  if (tag.callsite.empty()) {
    tag.callsite = kDefaultAllocationCallsite;
  }

  if (bytes == 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordFailure(tag, false, false);
    result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeMemoryDiagnostic(result.status,
                                             "SB-MEMORY-ALLOC-ZERO-SIZE",
                                             "memory.allocate.zero_size",
                                             std::move(tag),
                                             bytes,
                                             alignment);
    return result;
  }

  if (alignment == 0) {
    alignment = kDefaultAlignment;
  }
  if (alignment < kDefaultAlignment) {
    alignment = kDefaultAlignment;
  }

  if (!IsPowerOfTwo(alignment)) {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordFailure(tag, false, false);
    result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeMemoryDiagnostic(result.status,
                                             "SB-MEMORY-ALLOC-ALIGNMENT-INVALID",
                                             "memory.allocate.alignment_invalid",
                                             std::move(tag),
                                             bytes,
                                             alignment);
    return result;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto injected = EvaluateFailureInjectionLocked(tag);
    if (injected.inject) {
      RecordFailure(tag, false, false);
      result.status = MemoryStatus(StatusCode::memory_allocation_failed, Severity::error);
      result.diagnostic = MakeFailureInjectionDiagnostic(result.status,
                                                         tag,
                                                         bytes,
                                                         alignment,
                                                         injected);
      return result;
    }

    if (policy_.refuse_all_allocations) {
      RecordFailure(tag, true, false);
      result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
      result.diagnostic = MakeMemoryDiagnostic(
          result.status,
          "SB-MEMORY-ALLOC-DEFAULT-MANAGER-UNCONFIGURED",
          "memory.allocate.default_manager_unconfigured",
          std::move(tag),
          bytes,
          alignment,
          {{"policy_name", policy_.policy_name},
           {"authority_scope", "memory_default_manager_requires_explicit_startup_or_fixture_configuration"}});
      return result;
    }

    const auto context_limit = WouldExceedPerContextLimit(bytes, tag);
    if (context_limit.exceeded) {
      RecordFailure(tag, true, false);
      result.status = MemoryStatus(StatusCode::memory_limit_exceeded,
                                   policy_.failure_mode == AllocationFailureMode::fatal_status ? Severity::fatal : Severity::error);
      result.diagnostic = MakeMemoryDiagnostic(result.status,
                                               "SB-MEMORY-ALLOC-CONTEXT-LIMIT-EXCEEDED",
                                               "memory.allocate.context_limit_exceeded",
                                               std::move(tag),
                                               bytes,
                                               alignment,
                                               {{"scope_kind", context_limit.scope_kind},
                                                {"scope_id", context_limit.scope_id},
                                                {"scope_current_bytes", std::to_string(context_limit.current_bytes)},
                                                {"scope_limit_bytes", std::to_string(context_limit.limit_bytes)}});
      return result;
    }
    if (WouldExceedHardLimit(bytes)) {
      RecordFailure(tag, true, false);
      result.status = MemoryStatus(StatusCode::memory_limit_exceeded,
                                   policy_.failure_mode == AllocationFailureMode::fatal_status ? Severity::fatal : Severity::error);
      result.diagnostic = MakeMemoryDiagnostic(result.status,
                                               "SB-MEMORY-ALLOC-LIMIT-EXCEEDED",
                                               "memory.allocate.limit_exceeded",
                                               std::move(tag),
                                               bytes,
                                               alignment);
      return result;
    }
    if (WouldExceedSoftLimit(bytes) && policy_.reject_over_soft_limit) {
      RecordFailure(tag, true, false);
      result.status = MemoryStatus(StatusCode::memory_limit_exceeded, Severity::warning);
      result.diagnostic = MakeMemoryDiagnostic(result.status,
                                               "SB-MEMORY-ALLOC-SOFT-LIMIT-REJECTED",
                                               "memory.allocate.soft_limit_rejected",
                                               std::move(tag),
                                               bytes,
                                               alignment);
      return result;
    }
    if (WouldExceedPageBufferPoolLimit(bytes, tag.category)) {
      RecordFailure(tag, true, false);
      result.status = MemoryStatus(StatusCode::memory_limit_exceeded, Severity::error);
      result.diagnostic = MakeMemoryDiagnostic(result.status,
                                               "SB-MEMORY-PAGE-BUFFER-POOL-LIMIT-EXCEEDED",
                                               "memory.page_buffer.pool_limit_exceeded",
                                               std::move(tag),
                                               bytes,
                                               alignment);
      return result;
    }

    void* pointer = nullptr;
    try {
      pointer = ::operator new(bytes, std::align_val_t(alignment));
    } catch (const std::bad_alloc&) {
      RecordFailure(tag, false, false);
      result.status = MemoryStatus(StatusCode::memory_allocation_failed, Severity::error);
      result.diagnostic = MakeMemoryDiagnostic(result.status,
                                               "SB-MEMORY-ALLOC-UNDERLYING-BAD-ALLOC",
                                               "memory.allocate.underlying_bad_alloc",
                                               std::move(tag),
                                               bytes,
                                               alignment);
      return result;
    }

    RecordAllocation(pointer, bytes, alignment, tag);
    result.pointer = pointer;
  }

  if (policy_.zero_memory_on_allocate) {
    std::memset(result.pointer, 0, bytes);
  }

  result.bytes = bytes;
  result.alignment = alignment;
  PublishMemorySnapshot(Snapshot(), policy_);
  (void)scratchbird::core::metrics::ObserveHistogram(
      "sb_memory_allocation_latency_microseconds",
      scratchbird::core::metrics::Labels({{"component", "core.memory"}, {"operation", "allocate"}, {"result", "ok"},
                                          {"producer", MemoryCategoryName(tag.category)}}),
      ElapsedMicros(metric_start),
      "core_memory");
  return result;
}

AllocationResult BoundedAllocator::AllocateZeroed(usize bytes, usize alignment, MemoryTag tag) {
  AllocationResult result = Allocate(bytes, alignment, std::move(tag));
  if (result.ok()) {
    std::memset(result.pointer, 0, result.bytes);
  }
  return result;
}

AllocationResult BoundedAllocator::Reallocate(void* pointer, usize bytes, usize alignment, MemoryTag tag) {
  if (pointer == nullptr) {
    return Allocate(bytes, alignment, std::move(tag));
  }

  AllocationRecord old_record;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_.find(pointer);
    if (it != active_.end()) {
      old_record = it->second;
      found = true;
    }
  }
  if (!found) {
    AllocationResult result;
    result.status = MemoryStatus(StatusCode::memory_unknown_pointer, Severity::error);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      RecordFailure(tag, false, true);
    }
    result.diagnostic = MakeMemoryDiagnostic(result.status,
                                             "SB-MEMORY-REALLOC-UNKNOWN-POINTER",
                                             "memory.reallocate.unknown_pointer",
                                             std::move(tag),
                                             bytes,
                                             alignment);
    return result;
  }

  AllocationResult replacement = Allocate(bytes, alignment, tag);
  if (!replacement.ok()) {
    return replacement;
  }

  bool replacement_accounting_removed = false;
  {
    MemoryTag validation_tag = tag;
    validation_tag.callsite = kReallocateActiveMapValidationCallsite;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto injected = EvaluateFailureInjectionLocked(validation_tag);
    if (injected.inject) {
      bool replacement_found = false;
      (void)RemoveAllocation(replacement.pointer, &replacement_found);
      replacement_accounting_removed = replacement_found;
    }
  }

  bool replacement_active = false;
  {
    MemoryTag validation_tag = tag;
    validation_tag.callsite = kReallocateActiveMapValidationCallsite;
    std::lock_guard<std::mutex> lock(mutex_);
    replacement_active = active_.find(replacement.pointer) != active_.end();
    if (!replacement_active && !replacement_accounting_removed) {
      ApplyAllocationRemovalAccounting({replacement.bytes, replacement.alignment, tag});
      RecordFailure(validation_tag, false, true);
    } else if (!replacement_active) {
      RecordFailure(validation_tag, false, true);
    }
  }
  if (!replacement_active) {
    if (policy_.zero_memory_on_release) {
      std::memset(replacement.pointer, 0, replacement.bytes);
    }
    ::operator delete(replacement.pointer, std::align_val_t(replacement.alignment));
    AllocationResult result;
    result.status = MemoryStatus(StatusCode::memory_unknown_pointer, Severity::error);
    result.diagnostic = MakeMemoryDiagnostic(result.status,
                                             "SB-MEMORY-REALLOC-ACTIVE-MAP-VALIDATION-FAILED",
                                             "memory.reallocate.active_map_validation_failed",
                                             std::move(tag),
                                             bytes,
                                             alignment);
    return result;
  }

  std::memcpy(replacement.pointer, pointer, std::min(old_record.bytes, replacement.bytes));
  const auto freed = Deallocate(pointer, old_record.tag);
  if (!freed.ok()) {
    (void)Deallocate(replacement.pointer, tag);
    AllocationResult result;
    result.status = freed.status;
    result.diagnostic = freed.diagnostic;
    return result;
  }
  return replacement;
}

DeallocationResult BoundedAllocator::Deallocate(void* pointer, MemoryTag tag) {
  DeallocationResult result;
  result.status = OkStatus();
  if (tag.callsite.empty() || tag.callsite == kDefaultAllocationCallsite) {
    tag.callsite = kDefaultDeallocationCallsite;
  }

  if (pointer == nullptr) {
    return result;
  }

  bool found = false;
  AllocationRecord record;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto injected = EvaluateFailureInjectionLocked(tag);
    if (injected.inject) {
      RecordFailure(tag, false, false);
      result.status = MemoryStatus(StatusCode::memory_allocation_failed, Severity::error);
      result.diagnostic = MakeFailureInjectionDiagnostic(result.status,
                                                         tag,
                                                         0,
                                                         0,
                                                         injected);
      return result;
    }
    record = RemoveAllocation(pointer, &found);
    if (!found) {
      RecordFailure(tag, false, true);
      result.status = MemoryStatus(StatusCode::memory_unknown_pointer, Severity::error);
      result.diagnostic = MakeMemoryDiagnostic(result.status,
                                               "SB-MEMORY-DEALLOC-UNKNOWN-POINTER",
                                               "memory.deallocate.unknown_pointer",
                                               std::move(tag));
      return result;
    }
  }

  if (policy_.zero_memory_on_release) {
    std::memset(pointer, 0, record.bytes);
  }
  ::operator delete(pointer, std::align_val_t(record.alignment));
  return result;
}

MemoryAccountingSnapshot BoundedAllocator::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  MemoryAccountingSnapshot snapshot = accounting_;
  snapshot.leak_candidate_count = snapshot.active_allocation_count;
  snapshot.resident_committed_bytes = snapshot.current_bytes;
  snapshot.allocator_metadata_overhead_bytes =
      static_cast<u64>(active_.size()) *
      static_cast<u64>(sizeof(void*) + sizeof(AllocationRecord));
  snapshot.retained_slab_bytes = 0;
  snapshot.internal_fragmentation_bytes = 0;
  snapshot.external_fragmentation_bytes =
      snapshot.peak_bytes > snapshot.current_bytes
          ? snapshot.peak_bytes - snapshot.current_bytes
          : 0;
  if (sharded_accounting_ != nullptr) {
    const auto sharded = sharded_accounting_->Snapshot();
    snapshot.sharded_accounting_bound = true;
    snapshot.active_records_routed_through_sharded_accounting = true;
    snapshot.sharded_accounting_shard_count = sharded.shard_count;
    snapshot.sharded_accounting_current_bytes = sharded.current_bytes;
    snapshot.sharded_accounting_peak_bytes = sharded.peak_bytes;
    snapshot.sharded_accounting_active_allocation_count =
        sharded.active_allocation_count;
    snapshot.sharded_accounting_failed_release_count =
        sharded.failed_release_count;
  }
  snapshot.categories.clear();
  for (const auto& entry : category_accounting_) {
    MemoryCategorySnapshot category;
    category.category = entry.first;
    category.current_bytes = entry.second.current_bytes;
    category.peak_bytes = entry.second.peak_bytes;
    category.allocation_count = entry.second.allocation_count;
    category.deallocation_count = entry.second.deallocation_count;
    category.failure_count = entry.second.failure_count;
    category.active_allocation_count = entry.second.active_allocation_count;
    snapshot.categories.push_back(category);
  }
  snapshot.contexts.clear();
  for (const auto& entry : context_accounting_) {
    MemoryContextSnapshot context;
    context.scope_kind = entry.first.first;
    context.scope_id = entry.first.second;
    context.current_bytes = entry.second.current_bytes;
    context.peak_bytes = entry.second.peak_bytes;
    context.allocation_count = entry.second.allocation_count;
    context.deallocation_count = entry.second.deallocation_count;
    context.failure_count = entry.second.failure_count;
    context.active_allocation_count = entry.second.active_allocation_count;
    snapshot.contexts.push_back(std::move(context));
  }
  snapshot.reserved_categories = ReservedMemoryCategories();
  return snapshot;
}

const AllocationPolicy& BoundedAllocator::policy() const {
  return policy_;
}

MemoryFailureInjectionConfigurationResult BoundedAllocator::EnableAllocationFailureInjection(
    MemoryFailureInjectionConfiguration configuration) {
  const Status invalid_request = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
  if (!configuration.test_guard.enabled()) {
    return FailureInjectionConfigurationFailure(
        invalid_request,
        "SB-MEMORY-FAILURE-INJECTION-ENABLE-BLOCKED",
        "memory.failure_injection.enable_blocked",
        {{"reason", "compile_time_test_guard_missing"},
         {"compile_policy", configuration.test_guard.compile_policy()},
         {"fixture_name", configuration.fixture_name}});
  }
  if (!configuration.fixture_enabled || configuration.fixture_name.empty()) {
    return FailureInjectionConfigurationFailure(
        invalid_request,
        "SB-MEMORY-FAILURE-INJECTION-FIXTURE-REQUIRED",
        "memory.failure_injection.fixture_required",
        {{"reason", "explicit_fixture_configuration_missing"},
         {"compile_policy", configuration.test_guard.compile_policy()},
         {"fixture_name", configuration.fixture_name}});
  }
  if (configuration.rules.empty()) {
    return FailureInjectionConfigurationFailure(
        invalid_request,
        "SB-MEMORY-FAILURE-INJECTION-RULE-INVALID",
        "memory.failure_injection.rule_invalid",
        {{"reason", "no_rules"},
         {"compile_policy", configuration.test_guard.compile_policy()},
         {"fixture_name", configuration.fixture_name}});
  }

  FailureInjectionState next;
  next.enabled = true;
  next.fixture_name = std::move(configuration.fixture_name);
  next.evidence_note = std::move(configuration.evidence_note);
  next.rules.reserve(configuration.rules.size());
  for (auto& rule : configuration.rules) {
    if (!rule.enabled) {
      continue;
    }
    if (rule.rule_id.empty()) {
      return FailureInjectionConfigurationFailure(
          invalid_request,
          "SB-MEMORY-FAILURE-INJECTION-RULE-INVALID",
          "memory.failure_injection.rule_invalid",
          {{"reason", "rule_id_required"},
           {"compile_policy", configuration.test_guard.compile_policy()},
           {"fixture_name", next.fixture_name}});
    }
    if (rule.fail_on_matched_sequence == 0) {
      return FailureInjectionConfigurationFailure(
          invalid_request,
          "SB-MEMORY-FAILURE-INJECTION-RULE-INVALID",
          "memory.failure_injection.rule_invalid",
          {{"reason", "fail_on_matched_sequence_zero"},
           {"rule_id", rule.rule_id},
           {"compile_policy", configuration.test_guard.compile_policy()},
           {"fixture_name", next.fixture_name}});
    }
    if (rule.scope_kind != MemoryFailureInjectionScopeKind::any && rule.scope_id.empty()) {
      return FailureInjectionConfigurationFailure(
          invalid_request,
          "SB-MEMORY-FAILURE-INJECTION-RULE-INVALID",
          "memory.failure_injection.rule_invalid",
          {{"reason", "scope_id_required"},
           {"rule_id", rule.rule_id},
           {"scope_kind", MemoryFailureInjectionScopeKindName(rule.scope_kind)},
           {"compile_policy", configuration.test_guard.compile_policy()},
           {"fixture_name", next.fixture_name}});
    }
    next.rules.push_back({std::move(rule), 0, 0});
  }
  if (next.rules.empty()) {
    return FailureInjectionConfigurationFailure(
        invalid_request,
        "SB-MEMORY-FAILURE-INJECTION-RULE-INVALID",
        "memory.failure_injection.rule_invalid",
        {{"reason", "no_enabled_rules"},
         {"compile_policy", configuration.test_guard.compile_policy()},
         {"fixture_name", next.fixture_name}});
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_injection_ = std::move(next);
  }

  MemoryFailureInjectionConfigurationResult result;
  result.status = OkStatus();
  result.applied = true;
  result.enabled = true;
  return result;
}

MemoryFailureInjectionConfigurationResult BoundedAllocator::DisableAllocationFailureInjection() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_injection_ = {};
  }
  MemoryFailureInjectionConfigurationResult result;
  result.status = OkStatus();
  result.applied = true;
  result.enabled = false;
  return result;
}

MemoryFailureInjectionSnapshot BoundedAllocator::FailureInjectionSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  MemoryFailureInjectionSnapshot snapshot;
  snapshot.enabled = failure_injection_.enabled;
  snapshot.fixture_name = failure_injection_.fixture_name;
  snapshot.evidence_note = failure_injection_.evidence_note;
  snapshot.observed_allocation_sequence = failure_injection_.observed_allocation_sequence;
  snapshot.rules.reserve(failure_injection_.rules.size());
  for (const auto& state : failure_injection_.rules) {
    snapshot.rules.push_back({state.rule, state.matched_sequence, state.failure_count});
  }
  return snapshot;
}

PageBufferResult BoundedAllocator::AllocatePageBuffer(PageBufferRequest request) {
  PageBufferResult result;
  result.status = OkStatus();
  if (request.tag.callsite.empty()) {
    request.tag.callsite = kPageBufferAllocationCallsite;
  }

  if (!IsSupportedPageBufferSize(request.page_size)) {
    std::lock_guard<std::mutex> lock(mutex_);
    request.tag.category = MemoryCategory::page_buffer;
    RecordFailure(request.tag, false, false);
    result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeMemoryDiagnostic(result.status,
                                             "SB-MEMORY-PAGE-BUFFER-SIZE-INVALID",
                                             "memory.page_buffer.size_invalid",
                                             std::move(request.tag),
                                             request.page_size,
                                             request.alignment);
    return result;
  }

  if (request.page_count == 0 ||
      request.page_count > std::numeric_limits<usize>::max() / request.page_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    request.tag.category = MemoryCategory::page_buffer;
    RecordFailure(request.tag, false, false);
    result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeMemoryDiagnostic(result.status,
                                             "SB-MEMORY-PAGE-BUFFER-COUNT-INVALID",
                                             "memory.page_buffer.count_invalid",
                                             std::move(request.tag),
                                             request.page_size,
                                             request.alignment);
    return result;
  }

  if (request.alignment == 0) {
    request.alignment = DefaultPageBufferAlignment();
  }
  request.tag.category = MemoryCategory::page_buffer;
  request.tag.lifetime = MemoryLifetime::page_buffer;

  const usize bytes = request.page_size * request.page_count;
  AllocationResult allocation = AllocateZeroed(bytes, request.alignment, request.tag);
  if (!allocation.ok()) {
    result.status = allocation.status;
    result.diagnostic = allocation.diagnostic;
    return result;
  }

  result.buffer.pointer = allocation.pointer;
  result.buffer.bytes = bytes;
  result.buffer.page_size = request.page_size;
  result.buffer.page_count = request.page_count;
  result.buffer.alignment = allocation.alignment;
  return result;
}

DeallocationResult BoundedAllocator::ReleasePageBuffer(PageBuffer buffer, MemoryTag tag) {
  if (!buffer.valid()) {
    DeallocationResult result;
    result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeMemoryDiagnostic(result.status,
                                             "SB-MEMORY-PAGE-BUFFER-INVALID-RELEASE",
                                             "memory.page_buffer.invalid_release",
                                             std::move(tag),
                                             buffer.bytes,
                                             buffer.alignment);
    return result;
  }

  tag.category = MemoryCategory::page_buffer;
  tag.lifetime = MemoryLifetime::page_buffer;
  return Deallocate(buffer.pointer, std::move(tag));
}

ProtectedBufferResult BoundedAllocator::AllocateProtected(ProtectedMemoryRequest request) {
  ProtectedBufferResult result;
  result.status = OkStatus();
  result.evidence.protected_material_redacted = true;
  result.evidence.zero_on_allocate = request.zero_on_allocate;
  result.evidence.zero_on_release = true;
  result.evidence.platform_name = PlatformNameForProtectedMemory();
  result.evidence.authority_scope = kProtectedMemoryAuthorityNote;

  MemoryTag tag = request.tag;
  tag.callsite = kProtectedMemoryAllocationCallsite;
  tag.purpose = "protected_material";
  if (tag.category == MemoryCategory::unknown || tag.category == MemoryCategory::core_runtime) {
    tag.category = MemoryCategory::resource_seed;
  }
  if (tag.lifetime == MemoryLifetime::unknown || tag.lifetime == MemoryLifetime::temporary) {
    tag.lifetime = MemoryLifetime::temporary;
  }
  const std::string material_class = RedactProtectedMemoryText(request.material_class);

  if (request.bytes == 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordFailure(tag, false, false);
    result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeProtectedMemoryDiagnostic(
        result.status,
        "SB-MEMORY-PROTECTED-ALLOC-ZERO-SIZE",
        "memory.protected.allocate.zero_size",
        std::move(tag),
        request.bytes,
        request.alignment,
        result.evidence,
        {{"protected_material_class", material_class},
         {"platform_policy", ProtectedMemoryPlatformPolicyName(request.platform_policy)}});
    return result;
  }

  AllocationResult allocation = Allocate(request.bytes, request.alignment, tag);
  if (!allocation.ok()) {
    result.status = allocation.status;
    result.diagnostic = MakeProtectedMemoryDiagnostic(
        result.status,
        "SB-MEMORY-PROTECTED-ALLOC-FAILED",
        "memory.protected.allocate.failed",
        std::move(tag),
        request.bytes,
        request.alignment,
        result.evidence,
        {{"protected_material_class", material_class},
         {"platform_policy", ProtectedMemoryPlatformPolicyName(request.platform_policy)},
         {"underlying_diagnostic", allocation.diagnostic.diagnostic_code}});
    return result;
  }

  if (request.zero_on_allocate) {
    std::memset(allocation.pointer, 0, allocation.bytes);
  }
  ApplyProtectedPlatformEvidence(allocation.pointer,
                                 allocation.bytes,
                                 request.platform_policy,
                                 &result.evidence);

  const bool required_lock_failed =
      ProtectedPolicyRequiresLock(request.platform_policy) &&
      (!result.evidence.platform_lock_supported || !result.evidence.platform_lock_succeeded);
  const bool required_no_dump_failed =
      ProtectedPolicyRequiresNoDump(request.platform_policy) &&
      (!result.evidence.no_dump_supported || !result.evidence.no_dump_succeeded);
  if (required_lock_failed || required_no_dump_failed) {
    SecureZeroMemory(allocation.pointer, allocation.bytes);
    ReleaseProtectedPlatformEvidence(allocation.pointer, allocation.bytes, result.evidence);
    (void)Deallocate(allocation.pointer, tag);
    result.status = MemoryStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeProtectedMemoryDiagnostic(
        result.status,
        "SB-MEMORY-PROTECTED-PLATFORM-PROTECTION-REQUIRED",
        "memory.protected.platform_protection_required",
        std::move(tag),
        request.bytes,
        request.alignment,
        result.evidence,
        {{"protected_material_class", material_class},
         {"platform_policy", ProtectedMemoryPlatformPolicyName(request.platform_policy)},
         {"required_lock_failed", required_lock_failed ? "true" : "false"},
         {"required_no_dump_failed", required_no_dump_failed ? "true" : "false"}});
    return result;
  }

  result.buffer = ScopedProtectedBuffer(this,
                                        allocation.pointer,
                                        allocation.bytes,
                                        allocation.alignment,
                                        tag,
                                        result.evidence);
  return result;
}

bool BoundedAllocator::WouldExceedHardLimit(usize bytes) const {
  const u64 hard_limit = EffectiveHardLimit(policy_);
  if (hard_limit == 0) {
    return false;
  }
  return bytes > hard_limit || accounting_.current_bytes > hard_limit - bytes;
}

bool BoundedAllocator::WouldExceedSoftLimit(usize bytes) const {
  if (policy_.soft_limit_bytes == 0) {
    return false;
  }
  return bytes > policy_.soft_limit_bytes || accounting_.current_bytes > policy_.soft_limit_bytes - bytes;
}

bool BoundedAllocator::WouldExceedPageBufferPoolLimit(usize bytes, MemoryCategory category) const {
  if (category != MemoryCategory::page_buffer || policy_.page_buffer_pool_limit_bytes == 0) {
    return false;
  }
  return bytes > policy_.page_buffer_pool_limit_bytes ||
         accounting_.page_buffer_current_bytes > policy_.page_buffer_pool_limit_bytes - bytes;
}

BoundedAllocator::ContextLimitEvidence BoundedAllocator::WouldExceedPerContextLimit(
    usize bytes,
    const MemoryTag& tag) const {
  ContextLimitEvidence evidence;
  if (policy_.per_context_limit_bytes == 0) {
    return evidence;
  }
  const auto keys = ContextKeysForTag(tag);
  if (keys.empty()) {
    return evidence;
  }
  for (const auto& key : keys) {
    const auto it = context_accounting_.find(key);
    const u64 current = it == context_accounting_.end() ? 0 : it->second.current_bytes;
    if (bytes > policy_.per_context_limit_bytes ||
        current > policy_.per_context_limit_bytes - bytes) {
      evidence.exceeded = true;
      evidence.scope_kind = key.first;
      evidence.scope_id = key.second;
      evidence.current_bytes = current;
      evidence.limit_bytes = policy_.per_context_limit_bytes;
      return evidence;
    }
  }
  return evidence;
}

BoundedAllocator::FailureInjectionDecision BoundedAllocator::EvaluateFailureInjectionLocked(const MemoryTag& tag) {
  FailureInjectionDecision decision;
  if (!failure_injection_.enabled) {
    return decision;
  }
  ++failure_injection_.observed_allocation_sequence;
  decision.observed_allocation_sequence = failure_injection_.observed_allocation_sequence;
  for (auto& state : failure_injection_.rules) {
    if (!FailureInjectionRuleMatches(state.rule, tag)) {
      continue;
    }
    ++state.matched_sequence;
    if (state.matched_sequence == state.rule.fail_on_matched_sequence) {
      ++state.failure_count;
      decision.inject = true;
      decision.fixture_name = failure_injection_.fixture_name;
      decision.evidence_note = failure_injection_.evidence_note;
      decision.matched_sequence = state.matched_sequence;
      decision.rule = state.rule;
      return decision;
    }
  }
  return decision;
}

Status BoundedAllocator::MemoryStatus(StatusCode code, Severity severity) const {
  return {code, severity, Subsystem::memory};
}

DiagnosticRecord BoundedAllocator::MakeMemoryDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        MemoryTag tag,
                                                        usize bytes,
                                                        usize alignment,
                                                        std::vector<DiagnosticArgument> extra_arguments) const {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"policy", policy_.policy_name});
  arguments.push_back({"purpose", tag.purpose});
  arguments.push_back({"callsite", tag.callsite});
  arguments.push_back({"category", MemoryCategoryName(tag.category)});
  arguments.push_back({"lifetime", MemoryLifetimeName(tag.lifetime)});
  arguments.push_back({"subsystem", std::to_string(static_cast<unsigned>(tag.subsystem))});
  if (!tag.owner.empty()) {
    arguments.push_back({"owner", tag.owner});
  }
  if (!tag.context_id.empty()) {
    arguments.push_back({"context_id", tag.context_id});
  }
  if (!tag.database_id.empty()) {
    arguments.push_back({"database_id", tag.database_id});
  }
  if (!tag.session_id.empty()) {
    arguments.push_back({"session_id", tag.session_id});
  }
  if (!tag.transaction_id.empty()) {
    arguments.push_back({"transaction_id", tag.transaction_id});
  }
  if (!tag.statement_id.empty()) {
    arguments.push_back({"statement_id", tag.statement_id});
  }
  if (!tag.query_id.empty()) {
    arguments.push_back({"query_id", tag.query_id});
  }
  arguments.push_back({"requested_bytes", std::to_string(bytes)});
  arguments.push_back({"alignment", std::to_string(alignment)});
  arguments.push_back({"current_bytes", std::to_string(accounting_.current_bytes)});
  arguments.push_back({"peak_bytes", std::to_string(accounting_.peak_bytes)});
  arguments.push_back({"hard_limit_bytes", std::to_string(EffectiveHardLimit(policy_))});
  arguments.push_back({"soft_limit_bytes", std::to_string(policy_.soft_limit_bytes)});
  arguments.push_back({"page_buffer_pool_limit_bytes", std::to_string(policy_.page_buffer_pool_limit_bytes)});
  arguments.insert(arguments.end(),
                   std::make_move_iterator(extra_arguments.begin()),
                   std::make_move_iterator(extra_arguments.end()));

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory");
}

DiagnosticRecord BoundedAllocator::MakeFailureInjectionDiagnostic(
    Status status,
    const MemoryTag& tag,
    usize bytes,
    usize alignment,
    const FailureInjectionDecision& decision) const {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"failure_injection_rule_id", decision.rule.rule_id});
  arguments.push_back({"failure_injection_fixture", decision.fixture_name});
  arguments.push_back({"failure_injection_authority_scope", kFailureInjectionAuthorityNote});
  arguments.push_back({"failure_injection_observed_sequence",
                       std::to_string(decision.observed_allocation_sequence)});
  arguments.push_back({"failure_injection_matched_sequence",
                       std::to_string(decision.matched_sequence)});
  arguments.push_back({"failure_injection_fail_on_matched_sequence",
                       std::to_string(decision.rule.fail_on_matched_sequence)});
  arguments.push_back({"failure_injection_rule_callsite", decision.rule.callsite});
  arguments.push_back({"failure_injection_rule_purpose", decision.rule.purpose});
  arguments.push_back({"failure_injection_rule_category", MemoryCategoryName(decision.rule.category)});
  arguments.push_back({"failure_injection_rule_scope_kind",
                       MemoryFailureInjectionScopeKindName(decision.rule.scope_kind)});
  arguments.push_back({"failure_injection_rule_scope_id", decision.rule.scope_id});
  if (!decision.evidence_note.empty()) {
    arguments.push_back({"failure_injection_evidence_note", decision.evidence_note});
  }
  return MakeMemoryDiagnostic(status,
                              "SB-MEMORY-ALLOC-FAILURE-INJECTED",
                              "memory.allocate.failure_injected",
                              tag,
                              bytes,
                              alignment,
                              std::move(arguments));
}

DiagnosticRecord BoundedAllocator::MakeProtectedMemoryDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    MemoryTag tag,
    usize bytes,
    usize alignment,
    const ProtectedMemoryEvidence& evidence,
    std::vector<DiagnosticArgument> extra_arguments) const {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"protected_material_redacted",
                       evidence.protected_material_redacted ? "true" : "false"});
  arguments.push_back({"protected_memory_zero_on_allocate",
                       evidence.zero_on_allocate ? "true" : "false"});
  arguments.push_back({"protected_memory_zero_on_release",
                       evidence.zero_on_release ? "true" : "false"});
  arguments.push_back({"protected_memory_platform_name", evidence.platform_name});
  arguments.push_back({"protected_memory_lock_attempted",
                       evidence.platform_lock_attempted ? "true" : "false"});
  arguments.push_back({"protected_memory_lock_supported",
                       evidence.platform_lock_supported ? "true" : "false"});
  arguments.push_back({"protected_memory_lock_succeeded",
                       evidence.platform_lock_succeeded ? "true" : "false"});
  arguments.push_back({"protected_memory_no_dump_attempted",
                       evidence.no_dump_attempted ? "true" : "false"});
  arguments.push_back({"protected_memory_no_dump_supported",
                       evidence.no_dump_supported ? "true" : "false"});
  arguments.push_back({"protected_memory_no_dump_succeeded",
                       evidence.no_dump_succeeded ? "true" : "false"});
  arguments.push_back({"protected_memory_authority_scope", evidence.authority_scope});
  arguments.insert(arguments.end(),
                   std::make_move_iterator(extra_arguments.begin()),
                   std::make_move_iterator(extra_arguments.end()));
  return MakeMemoryDiagnostic(status,
                              std::move(diagnostic_code),
                              std::move(message_key),
                              std::move(tag),
                              bytes,
                              alignment,
                              std::move(arguments));
}

void BoundedAllocator::RecordFailure(const MemoryTag& tag, bool policy_rejection, bool unknown_pointer) {
  RecordFailure(tag.category, policy_rejection, unknown_pointer);
  for (const auto& key : ContextKeysForTag(tag)) {
    ContextAccounting& context = context_accounting_[key];
    ++context.failure_count;
  }
}

void BoundedAllocator::RecordFailure(MemoryCategory category, bool policy_rejection, bool unknown_pointer) {
  ++accounting_.failure_count;
  if (policy_rejection) {
    ++accounting_.policy_rejection_count;
  }
  if (unknown_pointer) {
    ++accounting_.unknown_pointer_failure_count;
  }
  CategoryAccounting& category_accounting = category_accounting_[category];
  ++category_accounting.failure_count;
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_memory_allocation_failures_total",
      scratchbird::core::metrics::Labels({{"component", "core.memory"},
                                          {"producer", MemoryCategoryName(category)},
                                          {"reason", unknown_pointer ? "unknown_pointer" : (policy_rejection ? "policy_rejection" : "allocation_failure")}}),
      1.0,
      "core_memory");
}

void BoundedAllocator::RecordAllocation(void* pointer, usize bytes, usize alignment, const MemoryTag& tag) {
  AllocationRecord record;
  record.bytes = bytes;
  record.alignment = alignment;
  record.tag = tag;
  if (sharded_accounting_ != nullptr) {
    ShardedMemoryAccountingEvent event;
    event.bytes = bytes;
    event.tag = tag;
    event.scope_ids = ShardedScopeIdsForTag(tag);
    event.page_buffer_bytes = tag.category == MemoryCategory::page_buffer ||
                              tag.lifetime == MemoryLifetime::page_buffer;
    const auto reserved = sharded_accounting_->Reserve(std::move(event));
    if (reserved.ok()) {
      const auto committed = sharded_accounting_->Commit(reserved.token);
      if (committed.ok()) {
        record.sharded_token_id = reserved.token.token_id;
        record.sharded_shard_index = reserved.token.shard_index;
        record.sharded_accounting_committed = true;
      }
    }
  }

  accounting_.current_bytes += bytes;
  accounting_.peak_bytes = std::max(accounting_.peak_bytes, accounting_.current_bytes);
  ++accounting_.allocation_count;
  ++accounting_.active_allocation_count;

  CategoryAccounting& category = category_accounting_[tag.category];
  category.current_bytes += bytes;
  category.peak_bytes = std::max(category.peak_bytes, category.current_bytes);
  ++category.allocation_count;
  ++category.active_allocation_count;

  if (tag.category == MemoryCategory::page_buffer) {
    accounting_.page_buffer_current_bytes += bytes;
    accounting_.page_buffer_peak_bytes = std::max(accounting_.page_buffer_peak_bytes,
                                                  accounting_.page_buffer_current_bytes);
  }
  if (tag.lifetime == MemoryLifetime::arena) {
    accounting_.arena_current_bytes += bytes;
    accounting_.arena_peak_bytes = std::max(accounting_.arena_peak_bytes, accounting_.arena_current_bytes);
  }

  for (const auto& key : ContextKeysForTag(tag)) {
    ContextAccounting& context = context_accounting_[key];
    context.current_bytes += bytes;
    context.peak_bytes = std::max(context.peak_bytes, context.current_bytes);
    ++context.allocation_count;
    ++context.active_allocation_count;
  }

  active_[pointer] = std::move(record);
}

void BoundedAllocator::ApplyAllocationRemovalAccounting(const AllocationRecord& record) {
  if (record.sharded_accounting_committed && sharded_accounting_ != nullptr) {
    ShardedMemoryAccountingToken token;
    token.token_id = record.sharded_token_id;
    token.bytes = record.bytes;
    token.shard_index = record.sharded_shard_index;
    (void)sharded_accounting_->Release(token);
  }

  SubtractCounter(accounting_.current_bytes, record.bytes);
  DecrementCounter(accounting_.active_allocation_count);
  ++accounting_.deallocation_count;

  CategoryAccounting& category = category_accounting_[record.tag.category];
  SubtractCounter(category.current_bytes, record.bytes);
  DecrementCounter(category.active_allocation_count);
  ++category.deallocation_count;

  if (record.tag.category == MemoryCategory::page_buffer) {
    SubtractCounter(accounting_.page_buffer_current_bytes, record.bytes);
  }
  if (record.tag.lifetime == MemoryLifetime::arena) {
    SubtractCounter(accounting_.arena_current_bytes, record.bytes);
  }
  for (const auto& key : ContextKeysForTag(record.tag)) {
    auto it = context_accounting_.find(key);
    if (it == context_accounting_.end()) {
      continue;
    }
    SubtractCounter(it->second.current_bytes, record.bytes);
    DecrementCounter(it->second.active_allocation_count);
    ++it->second.deallocation_count;
  }
}

BoundedAllocator::AllocationRecord BoundedAllocator::RemoveAllocation(void* pointer, bool* found) {
  *found = false;
  AllocationRecord record;
  auto it = active_.find(pointer);
  if (it == active_.end()) {
    return record;
  }
  record = it->second;
  active_.erase(it);
  *found = true;

  ApplyAllocationRemovalAccounting(record);
  return record;
}

ArenaAllocator::ArenaAllocator(BoundedAllocator* allocator, MemoryTag tag)
    : allocator_(allocator), tag_(std::move(tag)) {
  tag_.lifetime = MemoryLifetime::arena;
}

ArenaAllocator::ArenaAllocator(ArenaAllocator&& other) noexcept
    : allocator_(other.allocator_), tag_(std::move(other.tag_)), chunks_(std::move(other.chunks_)) {
  other.allocator_ = nullptr;
  other.chunks_.clear();
}

ArenaAllocator& ArenaAllocator::operator=(ArenaAllocator&& other) noexcept {
  if (this != &other) {
    Reset();
    allocator_ = other.allocator_;
    tag_ = std::move(other.tag_);
    chunks_ = std::move(other.chunks_);
    other.allocator_ = nullptr;
    other.chunks_.clear();
  }
  return *this;
}

ArenaAllocator::~ArenaAllocator() {
  Reset();
}

AllocationResult ArenaAllocator::Allocate(usize bytes, usize alignment) {
  AllocationResult result;
  result.status = OkStatus();
  if (allocator_ == nullptr) {
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    return result;
  }
  if (bytes == 0) {
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    return result;
  }
  if (alignment == 0 || alignment < kDefaultAlignment) {
    alignment = kDefaultAlignment;
  }
  if (!IsPowerOfTwo(alignment)) {
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    return result;
  }

  auto try_allocate_from_chunk = [&](Chunk& chunk) -> void* {
    const auto base = reinterpret_cast<std::uintptr_t>(chunk.pointer);
    if (base > std::numeric_limits<std::uintptr_t>::max() - chunk.used) {
      return nullptr;
    }
    const auto current = base + chunk.used;
    const auto mask = static_cast<std::uintptr_t>(alignment - 1);
    if (current > std::numeric_limits<std::uintptr_t>::max() - mask) {
      return nullptr;
    }
    const auto aligned = (current + mask) & ~mask;
    if (aligned < base) {
      return nullptr;
    }
    const auto offset = static_cast<usize>(aligned - base);
    if (offset > chunk.bytes || bytes > chunk.bytes - offset) {
      return nullptr;
    }
    chunk.used = offset + bytes;
    return reinterpret_cast<void*>(aligned);
  };

  for (auto it = chunks_.rbegin(); it != chunks_.rend(); ++it) {
    if (void* pointer = try_allocate_from_chunk(*it)) {
      result.pointer = pointer;
      result.bytes = bytes;
      result.alignment = alignment;
      return result;
    }
  }

  if (bytes > std::numeric_limits<usize>::max() - (alignment - 1)) {
    result.status = {StatusCode::memory_limit_exceeded, Severity::error, Subsystem::memory};
    return result;
  }
  const usize needed = bytes + alignment - 1;
  MemoryTag tag = tag_;
  tag.lifetime = MemoryLifetime::arena;
  auto allocate_chunk = [&](usize chunk_bytes) {
    return allocator_->Allocate(chunk_bytes, alignment, tag);
  };
  const usize preferred_chunk_bytes = std::max(kDefaultArenaChunkBytes, needed);
  AllocationResult chunk = allocate_chunk(preferred_chunk_bytes);
  if (!chunk.ok() && needed < preferred_chunk_bytes) {
    chunk = allocate_chunk(needed);
  }
  if (!chunk.ok()) {
    return chunk;
  }
  chunks_.push_back({chunk.pointer, chunk.bytes, chunk.alignment, 0});
  result.pointer = try_allocate_from_chunk(chunks_.back());
  if (result.pointer == nullptr) {
    DeallocationResult released = allocator_->Deallocate(chunk.pointer, tag);
    chunks_.pop_back();
    result.status = released.status.ok()
                        ? Status{StatusCode::memory_allocation_failed, Severity::error, Subsystem::memory}
                        : released.status;
    result.diagnostic = released.diagnostic;
    return result;
  }
  result.bytes = bytes;
  result.alignment = alignment;
  return result;
}

DeallocationResult ArenaAllocator::Reset() {
  DeallocationResult last;
  last.status = OkStatus();
  if (allocator_ == nullptr) {
    chunks_.clear();
    return last;
  }
  for (auto it = chunks_.rbegin(); it != chunks_.rend(); ++it) {
    last = allocator_->Deallocate(it->pointer, tag_);
  }
  chunks_.clear();
  return last;
}

MemoryAccountingSnapshot ArenaAllocator::Snapshot() const {
  if (allocator_ == nullptr) {
    return {};
  }
  return allocator_->Snapshot();
}

MemoryManager::MemoryManager(AllocationPolicy policy) : allocator_(std::move(policy)) {}

AllocationResult MemoryManager::Allocate(usize bytes, usize alignment, MemoryTag tag) {
  return allocator_.Allocate(bytes, alignment, std::move(tag));
}

AllocationResult MemoryManager::AllocateZeroed(usize bytes, usize alignment, MemoryTag tag) {
  return allocator_.AllocateZeroed(bytes, alignment, std::move(tag));
}

DeallocationResult MemoryManager::Deallocate(void* pointer, MemoryTag tag) {
  return allocator_.Deallocate(pointer, std::move(tag));
}

PageBufferResult MemoryManager::AllocatePageBuffer(PageBufferRequest request) {
  return allocator_.AllocatePageBuffer(std::move(request));
}

DeallocationResult MemoryManager::ReleasePageBuffer(PageBuffer buffer, MemoryTag tag) {
  return allocator_.ReleasePageBuffer(buffer, std::move(tag));
}

ProtectedBufferResult MemoryManager::AllocateProtected(ProtectedMemoryRequest request) {
  return allocator_.AllocateProtected(std::move(request));
}

ScopedAllocationResult MemoryManager::AllocateScoped(usize bytes, usize alignment, MemoryTag tag) {
  ScopedAllocationResult result;
  AllocationResult allocation = Allocate(bytes, alignment, tag);
  result.status = allocation.status;
  result.diagnostic = allocation.diagnostic;
  if (allocation.ok()) {
    result.allocation = ScopedAllocation(&allocator_, allocation.pointer, allocation.bytes, allocation.alignment, std::move(tag));
  }
  return result;
}

ScopedPageBufferResult MemoryManager::AllocateScopedPageBuffer(PageBufferRequest request) {
  ScopedPageBufferResult result;
  MemoryTag tag = request.tag;
  PageBufferResult page_buffer = AllocatePageBuffer(std::move(request));
  result.status = page_buffer.status;
  result.diagnostic = page_buffer.diagnostic;
  if (page_buffer.ok()) {
    result.buffer = ScopedPageBuffer(&allocator_, page_buffer.buffer, std::move(tag));
  }
  return result;
}

ArenaAllocator MemoryManager::CreateArena(MemoryTag tag) {
  return ArenaAllocator(&allocator_, std::move(tag));
}

MemoryAccountingSnapshot MemoryManager::Snapshot() const {
  return allocator_.Snapshot();
}

const AllocationPolicy& MemoryManager::policy() const {
  return allocator_.policy();
}

MemoryFailureInjectionConfigurationResult MemoryManager::EnableAllocationFailureInjection(
    MemoryFailureInjectionConfiguration configuration) {
  return allocator_.EnableAllocationFailureInjection(std::move(configuration));
}

MemoryFailureInjectionConfigurationResult MemoryManager::DisableAllocationFailureInjection() {
  return allocator_.DisableAllocationFailureInjection();
}

MemoryFailureInjectionSnapshot MemoryManager::FailureInjectionSnapshot() const {
  return allocator_.FailureInjectionSnapshot();
}

BoundedAllocator* MemoryManager::allocator() {
  return &allocator_;
}

DefaultMemoryManagerConfigurationResult ConfigureDefaultMemoryManagerInternal(
    AllocationPolicy policy,
    std::string provenance,
    bool fixture_mode) {
  DefaultMemoryManagerConfigurationResult result;
  result.requested_policy = policy;
  result.fixture_mode = fixture_mode;
  if (provenance.empty()) {
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    result.diagnostic = DefaultManagerDiagnostic(
        result.status,
        "MEMORY.DEFAULT_MANAGER_PROVENANCE_REQUIRED",
        "memory.default_manager_provenance_required",
        {{"policy_name", policy.policy_name},
         {"configuration_mode", fixture_mode ? "fixture" : "production_startup_policy"}});
    return result;
  }
  if (policy.refuse_all_allocations) {
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    result.diagnostic = DefaultManagerDiagnostic(
        result.status,
        "MEMORY.DEFAULT_MANAGER_REFUSAL_POLICY_RESERVED",
        "memory.default_manager_refusal_policy_reserved",
        {{"policy_name", policy.policy_name},
         {"provenance", provenance},
         {"configuration_mode", fixture_mode ? "fixture" : "production_startup_policy"}});
    return result;
  }
  const auto hard_limit = EffectiveHardLimit(policy);
  if (hard_limit == 0) {
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    result.diagnostic = DefaultManagerDiagnostic(
        result.status,
        "MEMORY.DEFAULT_MANAGER_POLICY_UNBOUNDED",
        "memory.default_manager_policy_unbounded",
        {{"policy_name", policy.policy_name}, {"provenance", std::move(provenance)}});
    return result;
  }

  std::lock_guard<std::mutex> lock(DefaultMemoryManagerMutex());
  auto& storage = DefaultMemoryManagerStorage();
  auto& configured_policy = DefaultMemoryManagerConfiguredPolicy();
  auto& runtime_state = DefaultMemoryManagerRuntimeStateStorage();
  if (storage) {
    result.already_initialized = true;
    result.active_policy = storage->policy();
    result.fixture_mode = runtime_state.fixture_mode;
    if (AllocationPoliciesEquivalent(result.active_policy, policy) &&
        runtime_state.fixture_mode == fixture_mode &&
        runtime_state.explicitly_configured) {
      result.status = OkStatus();
      return result;
    }
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    result.diagnostic = DefaultManagerDiagnostic(
        result.status,
        "MEMORY.DEFAULT_MANAGER_ALREADY_INITIALIZED",
        "memory.default_manager_already_initialized",
        {{"active_policy_name", result.active_policy.policy_name},
         {"requested_policy_name", policy.policy_name},
         {"active_mode", runtime_state.fixture_mode ? "fixture" : "production_startup_policy"},
         {"requested_mode", fixture_mode ? "fixture" : "production_startup_policy"},
         {"provenance", std::move(provenance)}});
    return result;
  }

  configured_policy = policy;
  storage = std::make_unique<MemoryManager>(configured_policy);
  runtime_state.explicitly_configured = true;
  runtime_state.fixture_mode = fixture_mode;
  runtime_state.provenance = provenance;
  result.status = OkStatus();
  result.active_policy = configured_policy;
  result.applied = true;
  return result;
}

DefaultMemoryManagerConfigurationResult ConfigureDefaultMemoryManager(
    AllocationPolicy policy,
    std::string provenance) {
  return ConfigureDefaultMemoryManagerInternal(std::move(policy),
                                               std::move(provenance),
                                               false);
}

DefaultMemoryManagerConfigurationResult ConfigureDefaultMemoryManagerForFixture(
    AllocationPolicy policy,
    std::string fixture_name) {
  if (fixture_name.empty()) {
    DefaultMemoryManagerConfigurationResult result;
    result.requested_policy = policy;
    result.fixture_mode = true;
    result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
    result.diagnostic = DefaultManagerDiagnostic(
        result.status,
        "MEMORY.DEFAULT_MANAGER_FIXTURE_NAME_REQUIRED",
        "memory.default_manager_fixture_name_required",
        {{"policy_name", policy.policy_name}});
    return result;
  }
  return ConfigureDefaultMemoryManagerInternal(std::move(policy),
                                               "fixture:" + std::move(fixture_name),
                                               true);
}

DefaultMemoryManagerStateSnapshot DefaultMemoryManagerState() {
  std::lock_guard<std::mutex> lock(DefaultMemoryManagerMutex());
  auto& storage = DefaultMemoryManagerStorage();
  auto& runtime_state = DefaultMemoryManagerRuntimeStateStorage();
  DefaultMemoryManagerStateSnapshot snapshot;
  snapshot.initialized = static_cast<bool>(storage);
  snapshot.explicitly_configured = runtime_state.explicitly_configured;
  snapshot.fixture_mode = runtime_state.fixture_mode;
  snapshot.provenance = runtime_state.provenance;
  snapshot.active_policy = storage ? storage->policy() : UnconfiguredDefaultMemoryManagerPolicy();
  return snapshot;
}

MemoryManager& DefaultMemoryManager() {
  std::lock_guard<std::mutex> lock(DefaultMemoryManagerMutex());
  auto& storage = DefaultMemoryManagerStorage();
  if (!storage) {
    return UnconfiguredDefaultMemoryManager();
  }
  return *storage;
}

}  // namespace scratchbird::core::memory
