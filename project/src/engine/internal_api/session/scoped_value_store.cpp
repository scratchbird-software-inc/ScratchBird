// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "scoped_value_store.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace scratchbird::engine::internal_api::session {
namespace {
namespace mem = core::memory;
constexpr Identity kSessionScope{};
constexpr std::size_t kMaximumValueBytes = 16 * 1024;
bool Valid(const Identity& id) { return mem::MemorySystemUuidValid(id); }
bool Valid(const ValueKey& key) {
  return Valid(key.identity) && key.family <= ValueFamily::application_context;
}
mem::MemoryTag Tag(const Identity& database, const Identity& session,
                   const Identity& transaction) {
  mem::MemoryTag tag;
  tag.purpose = "session scoped values";
  tag.callsite = "engine.session.scoped_value_store";
  tag.category = mem::MemoryCategory::core_runtime;
  tag.lifetime = transaction == kSessionScope ? mem::MemoryLifetime::connection
                                              : mem::MemoryLifetime::transaction;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = database;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::session] = session;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = session;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = session;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::transaction] = transaction;
  return tag;
}
}  // namespace

struct ScopedValueStore::Version {
  Version* bucket_next = nullptr;
  Version* next = nullptr;
  Version* previous = nullptr;
  ValueKey key;
  Identity transaction{};
  Identity datatype{};
  Identity security_label{};
  std::uint64_t datatype_generation = 0;
  std::uint64_t sequence = 0;
  std::size_t size = 0;
  bool is_null = false;
  bool inherit = false;
  // Payload is allocated contiguously immediately after this header.
  std::uint8_t* Data() { return reinterpret_cast<std::uint8_t*>(this + 1); }
};
struct ScopedValueStore::Transaction {
  Transaction* next = nullptr;
  Identity identity{};
};
struct ScopedValueStore::Mark {
  Mark* next = nullptr;
  Identity transaction{};
  Identity identity{};
  std::uint64_t sequence = 0;
};

ScopedValueStore::ScopedValueStore(mem::BoundedAllocator& allocator, Identity database,
                                 Identity session, StoreLimits limits)
    : allocator_(allocator), database_(database), session_(session), limits_(limits) {}

ScopedValueStore::Creation ScopedValueStore::Create(mem::BoundedAllocator& allocator,
    const Identity& database, const Identity& session, StoreLimits limits) {
  if (!Valid(database) || !Valid(session) || limits.resident_bytes < sizeof(ScopedValueStore) ||
      !limits.retained_versions || !limits.active_transactions || !limits.savepoints)
    return {};
  try {
    const auto memory = allocator.Allocate(sizeof(ScopedValueStore), alignof(ScopedValueStore),
                                          Tag(database, session, {}));
    if (!memory.ok()) return {{}, StoreStatus::memory_refused};
    return {Owner(new (memory.pointer) ScopedValueStore(allocator, database, session, limits)),
            StoreStatus::ok};
  } catch (const std::bad_alloc&) {
    return {{}, StoreStatus::memory_refused};
  }
}
ScopedValueStore::~ScopedValueStore() { Clear(); }
void ScopedValueStore::Deleter::operator()(ScopedValueStore* store) const noexcept {
  if (!store) return;
  auto& allocator = store->allocator_;
  store->~ScopedValueStore();
  (void)allocator.DeallocateNoAlloc(store);
}
void* ScopedValueStore::Allocate(std::size_t bytes, const Identity& transaction) {
  if (bytes > limits_.resident_bytes - resident_bytes_) return nullptr;
  try {
    const auto memory = allocator_.Allocate(bytes, alignof(std::max_align_t),
                                           Tag(database_, session_, transaction));
    if (!memory.ok()) return nullptr;
    resident_bytes_ += bytes;
    return memory.pointer;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}
void ScopedValueStore::Free(void* pointer, std::size_t bytes) noexcept {
  (void)allocator_.DeallocateNoAlloc(pointer);
  resident_bytes_ -= bytes;
}
StoreStatus ScopedValueStore::Check(const Identity& session, std::uint64_t expected,
                                   bool mutation) const {
  if (session != session_) return StoreStatus::owner_mismatch;
  if (closed_) return StoreStatus::closed;
  if (expected != generation_) return StoreStatus::stale_generation;
  if (mutation && generation_ == std::numeric_limits<std::uint64_t>::max())
    return StoreStatus::generation_exhausted;
  return StoreStatus::ok;
}
StoreStatus ScopedValueStore::CheckScope(ValueScope scope, const Identity& transaction) const {
  if (scope == ValueScope::session)
    return transaction == kSessionScope ? StoreStatus::ok : StoreStatus::invalid_argument;
  if (scope != ValueScope::transaction || !Valid(transaction)) return StoreStatus::invalid_argument;
  return FindTransaction(transaction) ? StoreStatus::ok : StoreStatus::transaction_missing;
}
ScopedValueStore::Transaction* ScopedValueStore::FindTransaction(const Identity& identity) const {
  for (auto* tx = transactions_; tx; tx = tx->next) if (tx->identity == identity) return tx;
  return nullptr;
}
ScopedValueStore::Mark* ScopedValueStore::FindMark(const Identity& transaction,
                                                const Identity& identity) const {
  for (auto* mark = marks_; mark; mark = mark->next)
    if (mark->identity == identity && mark->transaction == transaction) return mark;
  return nullptr;
}
std::size_t ScopedValueStore::Bucket(const ValueKey& key) {
  // Bounded in-session index only, never an equality/authority digest. Always
  // compare the complete binary key after lookup; even collisions stay correct.
  std::uint64_t hash = 14695981039346656037ull;
  for (const auto byte : key.identity) hash = (hash ^ byte) * 1099511628211ull;
  return ((hash ^ static_cast<std::uint8_t>(key.family)) * 1099511628211ull) & 255u;
}
ScopedValueStore::Version* ScopedValueStore::FindVersion(const ValueKey& key,
                                                      const Identity& transaction) const {
  for (auto* node = buckets_[Bucket(key)]; node; node = node->bucket_next)
    if (node->key == key && node->transaction == transaction) return node;
  return nullptr;
}
StoreSnapshot ScopedValueStore::Snapshot(const Identity& session) const {
  std::lock_guard lock(mutex_);
  StoreSnapshot result;
  if (session != session_) { result.status = StoreStatus::owner_mismatch; return result; }
  result.status = closed_ ? StoreStatus::closed : StoreStatus::ok;
  result.generation = generation_;
  result.resident_bytes = resident_bytes_;
  result.retained_versions = version_count_;
  result.active_transactions = transaction_count_;
  result.savepoints = mark_count_;
  return result;
}
StoreResult ScopedValueStore::BeginTransaction(const Identity& session, const Identity& transaction,
                                              std::uint64_t expected) {
  std::lock_guard lock(mutex_);
  auto status = Check(session, expected, true);
  if (status != StoreStatus::ok) return Result(status);
  if (!Valid(transaction)) return Result(StoreStatus::invalid_argument);
  if (FindTransaction(transaction)) return Result(StoreStatus::transaction_exists);
  if (transaction_count_ >= limits_.active_transactions ||
      sizeof(Transaction) > limits_.resident_bytes - resident_bytes_)
    return Result(StoreStatus::quota_exceeded);
  auto* memory = Allocate(sizeof(Transaction), transaction);
  if (!memory) return Result(StoreStatus::memory_refused);
  transactions_ = new (memory) Transaction{transactions_, transaction};
  ++transaction_count_;
  ++generation_;
  return Result(StoreStatus::ok);
}
void ScopedValueStore::Remove(Version* node) noexcept {
  auto** bucket = &buckets_[Bucket(node->key)];
  while (*bucket != node) bucket = &(*bucket)->bucket_next;
  *bucket = node->bucket_next;
  if (node->previous) node->previous->next = node->next;
  else versions_ = node->next;
  if (node->next) node->next->previous = node->previous;
  const auto bytes = sizeof(Version) + node->size;
  node->~Version();
  Free(node, bytes);
  --version_count_;
}
void ScopedValueStore::Remove(Mark* mark) noexcept {
  auto** link = &marks_;
  while (*link != mark) link = &(*link)->next;
  *link = mark->next;
  mark->~Mark();
  Free(mark, sizeof(Mark));
  --mark_count_;
}
void ScopedValueStore::Compact(const Identity& transaction) noexcept {
  // Retain exactly current state plus versions visible to an extant savepoint.
  // A long transaction updating one key must not retain every intermediate
  // value just because an old savepoint exists.
  for (auto* node = versions_; node;) {
    auto* next = node->next;
    if (node->transaction == transaction) {
      Version* predecessor = nullptr;
      for (auto* newer = buckets_[Bucket(node->key)]; newer && newer != node; newer = newer->bucket_next)
        if (newer->key == node->key && newer->transaction == transaction) predecessor = newer;
      if (predecessor) {
        bool needed = false;
        for (auto* mark = marks_; mark; mark = mark->next) {
          if (mark->transaction == transaction && mark->sequence >= node->sequence &&
              mark->sequence < predecessor->sequence) { needed = true; break; }
        }
        if (!needed) Remove(node);
      }
    }
    node = next;
  }
  // Drop inherit markers only after older versions have been discarded, so
  // erasing an override cannot accidentally uncover one of its old versions.
  bool has_marks = false;
  for (auto* mark = marks_; mark; mark = mark->next)
    if (mark->transaction == transaction) { has_marks = true; break; }
  if (!has_marks) {
    for (auto* node = versions_; node;) {
      auto* next = node->next;
      if (node->transaction == transaction && node->inherit) Remove(node);
      node = next;
    }
  }
}
void ScopedValueStore::CompactKey(const ValueKey& key, const Identity& transaction) noexcept {
  bool has_marks = false;
  for (auto* mark = marks_; mark; mark = mark->next)
    if (mark->transaction == transaction) { has_marks = true; break; }
  Version* current = nullptr;
  std::uint64_t predecessor_sequence = 0;
  for (auto* node = buckets_[Bucket(key)]; node;) {
    auto* next = node->bucket_next;
    if (node->key == key && node->transaction == transaction) {
      if (!current) current = node;
      else {
        bool needed = false;
        for (auto* mark = marks_; mark; mark = mark->next) {
          if (mark->transaction == transaction && mark->sequence >= node->sequence &&
              mark->sequence < predecessor_sequence) { needed = true; break; }
        }
        if (!needed) { Remove(node); node = next; continue; }
      }
      predecessor_sequence = node->sequence;
    }
    node = next;
  }
  if (current && current->inherit && !has_marks)
    Remove(current);
}
StoreResult ScopedValueStore::EndTransaction(const Identity& session, const Identity& transaction,
                                            std::uint64_t expected) {
  std::lock_guard lock(mutex_);
  auto status = Check(session, expected, true);
  if (status != StoreStatus::ok) return Result(status);
  auto* tx = FindTransaction(transaction);
  if (!tx) return Result(StoreStatus::transaction_missing);
  ExpireTransaction(tx);
  return Result(StoreStatus::ok);
}
void ScopedValueStore::ExpireTransaction(Transaction* tx) noexcept {
  const auto transaction = tx->identity;
  for (auto* node = versions_; node;) {
    auto* next = node->next;
    if (node->transaction == transaction) Remove(node);
    node = next;
  }
  for (auto* mark = marks_; mark;) {
    auto* next = mark->next;
    if (mark->transaction == transaction) Remove(mark);
    mark = next;
  }
  auto** link = &transactions_;
  while (*link != tx) link = &(*link)->next;
  *link = tx->next;
  tx->~Transaction();
  Free(tx, sizeof(Transaction));
  --transaction_count_;
  ++generation_;
}
ScopedValueStore::TerminalGuard::TerminalGuard(ScopedValueStore& store, const Identity& session,
                                             const Identity& transaction)
    : store_(&store), lock_(store.mutex_), transaction_(transaction) {
  auto status = store.Check(session, store.generation_, true);
  if (status == StoreStatus::ok && !store.FindTransaction(transaction))
    status = StoreStatus::transaction_missing;
  admission_ = store.Result(status);
}
ScopedValueStore::TerminalGuard ScopedValueStore::PrepareTerminal(
    const Identity& session, const Identity& transaction) {
  return TerminalGuard(*this, session, transaction);
}
void ScopedValueStore::TerminalGuard::ApplyKnownTerminal() noexcept {
  if (!lock_.owns_lock() || !admission_.ok() || applied_) return;
  store_->ExpireTransaction(store_->FindTransaction(transaction_));
  applied_ = true;
}
StoreResult ScopedValueStore::Savepoint(const Identity& session, const Identity& transaction,
                                       const Identity& savepoint, std::uint64_t expected) {
  std::lock_guard lock(mutex_);
  auto status = Check(session, expected, true);
  if (status != StoreStatus::ok) return Result(status);
  if (!Valid(savepoint)) return Result(StoreStatus::invalid_argument);
  if (!FindTransaction(transaction)) return Result(StoreStatus::transaction_missing);
  if (FindMark(transaction, savepoint)) return Result(StoreStatus::savepoint_exists);
  if (mark_count_ >= limits_.savepoints || sizeof(Mark) > limits_.resident_bytes - resident_bytes_)
    return Result(StoreStatus::quota_exceeded);
  auto* memory = Allocate(sizeof(Mark), transaction);
  if (!memory) return Result(StoreStatus::memory_refused);
  ++generation_;
  marks_ = new (memory) Mark{marks_, transaction, savepoint, generation_};
  ++mark_count_;
  return Result(StoreStatus::ok);
}
StoreResult ScopedValueStore::RollbackTo(const Identity& session, const Identity& transaction,
                                        const Identity& savepoint, std::uint64_t expected) {
  std::lock_guard lock(mutex_);
  auto status = Check(session, expected, true);
  if (status != StoreStatus::ok) return Result(status);
  if (!FindTransaction(transaction)) return Result(StoreStatus::transaction_missing);
  const auto* target = FindMark(transaction, savepoint);
  if (!target) return Result(StoreStatus::savepoint_missing);
  const auto cutoff = target->sequence;
  for (auto* node = versions_; node;) {
    auto* next = node->next;
    if (node->transaction == transaction && node->sequence > cutoff) Remove(node);
    node = next;
  }
  for (auto* mark = marks_; mark;) {
    auto* next = mark->next;
    if (mark->transaction == transaction && mark->sequence > cutoff) Remove(mark);
    mark = next;
  }
  ++generation_;  // Never restore the old admission generation (ABA).
  return Result(StoreStatus::ok);
}
StoreResult ScopedValueStore::ReleaseSavepoint(const Identity& session, const Identity& transaction,
    const Identity& savepoint, std::uint64_t expected) {
  std::lock_guard lock(mutex_);
  auto status = Check(session, expected, true);
  if (status != StoreStatus::ok) return Result(status);
  if (!FindTransaction(transaction)) return Result(StoreStatus::transaction_missing);
  auto* target = FindMark(transaction, savepoint);
  if (!target) return Result(StoreStatus::savepoint_missing);
  // Release exactly this binary savepoint. Nested-handle invalidation belongs
  // to the MGA coordinator, which supplies the corresponding releases.
  Remove(target);
  Compact(transaction);
  ++generation_;
  return Result(StoreStatus::ok);
}
StoreResult ScopedValueStore::Mutate(const Identity& session, const ValueKey& key, ValueScope scope,
    const Identity& transaction, std::uint64_t expected, const AdmittedValueView* value, bool erase) {
  std::lock_guard lock(mutex_);
  auto status = Check(session, expected, true);
  if (status != StoreStatus::ok) return Result(status);
  if (!Valid(key)) return Result(StoreStatus::invalid_argument);
  status = CheckScope(scope, transaction);
  if (status != StoreStatus::ok) return Result(status);
  if (!erase && (!value || !Valid(value->datatype_uuid) || !value->datatype_generation ||
      !Valid(value->security_label_uuid) || value->bytes.size() > kMaximumValueBytes ||
      (value->is_null && !value->bytes.empty()))) return Result(StoreStatus::invalid_argument);
  auto* previous = FindVersion(key, transaction);
  if (erase && (!previous || previous->inherit)) {
    ++generation_;
    return Result(StoreStatus::ok);
  }
  if (erase && scope == ValueScope::session) {
    Remove(previous);
    ++generation_;
    return Result(StoreStatus::ok);
  }
  bool has_marks = false;
  bool previous_needed = false;
  if (scope == ValueScope::transaction) {
    for (auto* mark = marks_; mark; mark = mark->next) {
      if (mark->transaction != transaction) continue;
      has_marks = true;
      if (previous && mark->sequence >= previous->sequence) previous_needed = true;
    }
  }
  if (erase && !has_marks) {
    Remove(previous);
    ++generation_;
    return Result(StoreStatus::ok);
  }
  const auto size = erase ? 0 : value->bytes.size();
  // A superseded current version is released after replacement allocation.
  // Count retained versions, while admitting ALL transient bytes separately.
  if ((version_count_ >= limits_.retained_versions && (!previous || previous_needed)) ||
      sizeof(Version) + size > limits_.resident_bytes - resident_bytes_)
    return Result(StoreStatus::quota_exceeded);
  auto* memory = Allocate(sizeof(Version) + size, transaction);
  if (!memory) return Result(StoreStatus::memory_refused);
  auto* node = new (memory) Version;
  node->key = key;
  node->transaction = transaction;
  node->sequence = generation_ + 1;
  node->size = size;
  node->inherit = erase;
  if (!erase) {
    node->datatype = value->datatype_uuid;
    node->datatype_generation = value->datatype_generation;
    node->security_label = value->security_label_uuid;
    node->is_null = value->is_null;
    if (size) std::memcpy(node->Data(), value->bytes.data(), size);
  }
  auto& bucket = buckets_[Bucket(key)];
  node->bucket_next = bucket;
  bucket = node;
  node->next = versions_;
  if (versions_) versions_->previous = node;
  versions_ = node;
  ++version_count_;
  if (scope == ValueScope::session) {
    if (previous) Remove(previous);
  } else CompactKey(key, transaction);
  ++generation_;
  return Result(StoreStatus::ok);
}
StoreResult ScopedValueStore::WriteAdmitted(const Identity& session, const ValueKey& key,
    ValueScope scope, const Identity& transaction, std::uint64_t expected, const AdmittedValueView& value) {
  return Mutate(session, key, scope, transaction, expected, &value, false);
}
StoreResult ScopedValueStore::EraseAdmitted(const Identity& session, const ValueKey& key,
    ValueScope scope, const Identity& transaction, std::uint64_t expected) {
  return Mutate(session, key, scope, transaction, expected, nullptr, true);
}
ValueReadResult ScopedValueStore::ReadAdmitted(const Identity& session, const ValueKey& key,
    const Identity& transaction, std::uint64_t expected, std::span<std::uint8_t> output) const {
  std::lock_guard lock(mutex_);
  ValueReadResult result;
  result.status = Check(session, expected, false);
  if (result.status == StoreStatus::owner_mismatch) return result;
  result.generation = generation_;
  if (result.status != StoreStatus::ok) return result;
  if (!Valid(key)) { result.status = StoreStatus::invalid_argument; return result; }
  if (transaction != kSessionScope && !FindTransaction(transaction)) {
    result.status = StoreStatus::transaction_missing; return result;
  }
  auto* node = transaction == kSessionScope ? nullptr : FindVersion(key, transaction);
  if (!node || node->inherit) node = FindVersion(key, kSessionScope);
  if (!node) { result.status = StoreStatus::not_found; return result; }
  result.bytes_required = node->size;
  if (output.size() < node->size) { result.status = StoreStatus::output_too_small; return result; }
  result.datatype_uuid = node->datatype;
  result.datatype_generation = node->datatype_generation;
  result.security_label_uuid = node->security_label;
  result.is_null = node->is_null;
  result.scope = node->transaction == kSessionScope ? ValueScope::session : ValueScope::transaction;
  if (node->size) std::memcpy(output.data(), node->Data(), node->size);
  return result;
}
void ScopedValueStore::Clear() noexcept {
  while (versions_) Remove(versions_);
  while (marks_) Remove(marks_);
  while (transactions_) {
    auto* next = transactions_->next;
    transactions_->~Transaction();
    Free(transactions_, sizeof(Transaction));
    transactions_ = next;
  }
  transaction_count_ = 0;
}
StoreResult ScopedValueStore::Reset(const Identity& session, std::uint64_t expected) {
  std::lock_guard lock(mutex_);
  auto status = Check(session, expected, true);
  if (status != StoreStatus::ok) return Result(status);
  Clear();
  ++generation_;
  return Result(StoreStatus::ok);
}
StoreResult ScopedValueStore::Close(const Identity& session) {
  std::lock_guard lock(mutex_);
  if (session != session_) return Result(StoreStatus::owner_mismatch);
  if (!closed_) {
    Clear();
    closed_ = true;
    if (generation_ != std::numeric_limits<std::uint64_t>::max()) ++generation_;
  }
  return Result(StoreStatus::ok);
}
}  // namespace scratchbird::engine::internal_api::session
