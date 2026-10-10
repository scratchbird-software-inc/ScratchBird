// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/session/scoped_value_store.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <random>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <vector>

namespace s = scratchbird::engine::internal_api::session;
namespace m = scratchbird::core::memory;
using Status = s::StoreStatus;
void Check(bool condition, std::source_location loc = std::source_location::current()) {
  if (!condition) throw std::runtime_error("scoped value store failure at line " + std::to_string(loc.line()));
}
s::Identity Id(unsigned n) {
  s::Identity id{1, 144, 10, 9, 0, 124, 112, 0, 128, 0, 0, 0, 0, 0, 0, 0};
  for (unsigned i = 0; i < 4; ++i) id[15 - i] = static_cast<std::uint8_t>(n >> (i * 8));
  return id;
}
s::ValueKey Key(unsigned n, s::ValueFamily family = s::ValueFamily::application_context) {
  return {family, Id(n)};
}
m::AllocationPolicy Policy() {
  m::AllocationPolicy policy;
  policy.byte_limit = 64 * 1024 * 1024;
  policy.zero_memory_on_release = true;
  return policy;
}
struct Fixture {
  m::BoundedAllocator allocator{Policy()};
  s::Identity database = Id(100), session = Id(101);
  s::ScopedValueStore::Owner store;
  explicit Fixture(s::StoreLimits limits = {}) {
    auto created = s::ScopedValueStore::Create(allocator, database, session, limits);
    Check(created.status == Status::ok && created.owner);
    store = std::move(created.owner);
    Check(allocator.Snapshot().current_bytes == store->Snapshot(session).resident_bytes);
    for (const auto& context : allocator.Snapshot().contexts) {
      Check(context.binary_scope.has_value() && context.scope_id.empty());
      Check(context.binary_scope->uuid == database || context.binary_scope->uuid == session);
    }
  }
  std::uint64_t Gen() const { return store->Snapshot(session).generation; }
  s::AdmittedValueView Value(std::span<const std::uint8_t> bytes, bool null = false) const {
    return {Id(110), 17, Id(111), null, bytes};
  }
  void Write(s::ValueKey key, unsigned value, s::Identity tx = {}) {
    const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 24)};
    Check(store->WriteAdmitted(session, key, tx == s::Identity{} ? s::ValueScope::session :
        s::ValueScope::transaction, tx, Gen(), Value(bytes)).ok());
  }
  void Expect(s::ValueKey key, unsigned value, s::Identity tx = {},
              s::ValueScope scope = s::ValueScope::session) {
    std::array<std::uint8_t, 4> output{};
    const auto result = store->ReadAdmitted(session, key, tx, Gen(), output);
    Check(result.ok() && result.bytes_required == 4 && !result.is_null && result.scope == scope);
    Check(result.datatype_uuid == Id(110) && result.datatype_generation == 17 &&
          result.security_label_uuid == Id(111));
    unsigned actual = 0;
    for (unsigned i = 0; i < 4; ++i) actual |= static_cast<unsigned>(output[i]) << (8 * i);
    Check(actual == value);
  }
  void Missing(s::ValueKey key, s::Identity tx = {}) {
    std::array<std::uint8_t, 32> untouched;
    untouched.fill(0xa5);
    Check(store->ReadAdmitted(session, key, tx, Gen(), untouched).status == Status::not_found);
    Check(std::all_of(untouched.begin(), untouched.end(), [](auto b) { return b == 0xa5; }));
  }
};

void LifetimeRules() {
  // Both commit and rollback invoke the same scope expiry AFTER confirmed MGA
  // finality. This test is a retention test, not proof of an MGA executor route.
  for (auto family : {s::ValueFamily::setting, s::ValueFamily::qualifier, s::ValueFamily::application_context}) {
    Fixture f;
    const auto key = Key(1, family), other = Key(2, family);
    const auto a = Id(200), b = Id(201), outer = Id(210), inner = Id(211);
    f.Write(key, 1);
    Check(f.store->BeginTransaction(f.session, a, f.Gen()).ok());
    Check(f.store->BeginTransaction(f.session, b, f.Gen()).ok());
    f.Write(key, 2, a); f.Write(key, 3, b);
    Check(f.store->Savepoint(f.session, a, outer, f.Gen()).ok());
    f.Write(key, 4, a); f.Write(other, 5, a);
    Check(f.store->Savepoint(f.session, a, inner, f.Gen()).ok());
    f.Write(key, 6, a); f.Write(key, 7);  // Session update beneath both overrides.
    const auto before = f.Gen();
    Check(f.store->RollbackTo(f.session, a, inner, before).ok());
    Check(f.Gen() > before);
    f.Expect(key, 4, a, s::ValueScope::transaction); f.Expect(key, 7);
    f.Expect(key, 3, b, s::ValueScope::transaction);
    Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::transaction, a, before,
          f.Value({})).status == Status::stale_generation);
    Check(f.store->RollbackTo(f.session, a, outer, f.Gen()).ok());
    f.Expect(key, 2, a, s::ValueScope::transaction); f.Missing(other, a); f.Expect(key, 7);
    Check(f.store->RollbackTo(f.session, a, inner, f.Gen()).status == Status::savepoint_missing);
    f.Write(key, 8, a);
    Check(f.store->RollbackTo(f.session, a, outer, f.Gen()).ok());
    f.Expect(key, 2, a, s::ValueScope::transaction);
    Check(f.store->ReleaseSavepoint(f.session, a, outer, f.Gen()).ok());
    Check(f.store->Snapshot(f.session).retained_versions == 3);
    Check(f.store->EndTransaction(f.session, a, f.Gen()).ok());
    f.Expect(key, 7);
    Check(f.store->EndTransaction(f.session, b, f.Gen()).ok());
    f.Expect(key, 7);
    Check(f.store->Snapshot(f.session).retained_versions == 1);
    Check(f.store->BeginTransaction(f.session, Id(202), f.Gen()).ok());
    Check(f.store->Savepoint(f.session, Id(202), outer, f.Gen()).ok());
    f.Write(other, 9);
    Check(f.store->RollbackTo(f.session, Id(202), outer, f.Gen()).ok());
    f.Expect(other, 9);  // Session-only write survives savepoint rollback.
    Check(f.store->EndTransaction(f.session, Id(202), f.Gen()).ok());
    f.Expect(other, 9);  // And terminal rollback/commit expiry.
  }
}

void EraseAndCompaction() {
  Fixture f;
  auto key = Key(1); const auto tx = Id(200), a = Id(210), b = Id(211);
  f.Write(key, 1);
  Check(f.store->BeginTransaction(f.session, tx, f.Gen()).ok());
  f.Write(key, 2, tx);
  Check(f.store->Savepoint(f.session, tx, a, f.Gen()).ok());
  Check(f.store->EraseAdmitted(f.session, key, s::ValueScope::transaction, tx, f.Gen()).ok());
  f.Expect(key, 1, tx);
  Check(f.store->Savepoint(f.session, tx, b, f.Gen()).ok());
  f.Write(key, 3, tx);
  Check(f.store->ReleaseSavepoint(f.session, tx, a, f.Gen()).ok());
  Check(f.store->RollbackTo(f.session, tx, b, f.Gen()).ok());
  f.Expect(key, 1, tx);
  Check(f.store->ReleaseSavepoint(f.session, tx, b, f.Gen()).ok());
  Check(f.store->Snapshot(f.session).retained_versions == 1);
  for (unsigned i = 0; i < 1000; ++i) f.Write(key, i, tx);
  Check(f.store->Snapshot(f.session).retained_versions == 2);
  Check(f.store->EraseAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen()).ok());
  f.Missing(key); f.Expect(key, 999, tx, s::ValueScope::transaction);
  Check(f.store->EraseAdmitted(f.session, key, s::ValueScope::transaction, tx, f.Gen()).ok());
  f.Missing(key, tx);
  Check(f.store->Snapshot(f.session).retained_versions == 0);
}

void ExactBytesAndAdmission() {
  Fixture f;
  const auto key = Key(1);
  std::vector<std::uint8_t> payload(16 * 1024);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i);
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), f.Value(payload)).ok());
  payload.assign(payload.size(), 0xff); // The store owns an immutable copy.
  auto read = f.store->ReadAdmitted(f.session, key, {}, f.Gen(), payload);
  Check(read.ok() && read.bytes_required == payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i) Check(payload[i] == static_cast<std::uint8_t>(i));
  auto too_small = std::span(payload).first(16);
  std::fill(too_small.begin(), too_small.end(), 0xa5);
  read = f.store->ReadAdmitted(f.session, key, {}, f.Gen(), too_small);
  Check(read.status == Status::output_too_small && read.bytes_required == payload.size());
  Check(std::all_of(too_small.begin(), too_small.end(), [](auto b) { return b == 0xa5; }));
  payload.push_back(0);
  const auto unchanged = f.Gen();
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), f.Value(payload)).status == Status::invalid_argument);
  Check(f.Gen() == unchanged);
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), f.Value({}, true)).ok());
  read = f.store->ReadAdmitted(f.session, key, {}, f.Gen(), {});
  Check(read.ok() && read.is_null && read.bytes_required == 0 && read.datatype_generation == 17);
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), f.Value({})).ok());
  read = f.store->ReadAdmitted(f.session, key, {}, f.Gen(), {});
  Check(read.ok() && !read.is_null && read.bytes_required == 0);
  const std::array<std::uint8_t, 1> bytes{0};
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), f.Value(bytes, true)).status == Status::invalid_argument);
  auto invalid = f.Value(bytes); invalid.datatype_uuid = {};
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), invalid).status == Status::invalid_argument);
  invalid = f.Value(bytes); invalid.datatype_generation = 0;
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), invalid).status == Status::invalid_argument);
  invalid = f.Value(bytes); invalid.security_label_uuid = {};
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, {}, f.Gen(), invalid).status == Status::invalid_argument);
  Check(f.store->WriteAdmitted(f.session, key, static_cast<s::ValueScope>(255), {}, f.Gen(), f.Value(bytes)).status == Status::invalid_argument);
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::session, Id(200), f.Gen(), f.Value(bytes)).status == Status::invalid_argument);
  Check(f.store->WriteAdmitted(f.session, key, s::ValueScope::transaction, Id(200), f.Gen(), f.Value(bytes)).status == Status::transaction_missing);
  Check(f.store->ReadAdmitted(f.session, key, Id(200), f.Gen(), {}).status == Status::transaction_missing);
  Check(f.store->WriteAdmitted(Id(999), key, s::ValueScope::session, {}, f.Gen(), f.Value(bytes)).status == Status::owner_mismatch);
  Check(f.store->ReadAdmitted(Id(999), key, {}, f.Gen(), {}).status == Status::owner_mismatch);
  Check(f.store->Reset(Id(999), f.Gen()).status == Status::owner_mismatch);
  Check(f.store->Close(Id(999)).status == Status::owner_mismatch);
  Check(f.store->BeginTransaction(f.session, {}, f.Gen()).status == Status::invalid_argument);
  Check(f.store->BeginTransaction(f.session, Id(200), f.Gen()).ok());
  Check(f.store->BeginTransaction(f.session, Id(200), f.Gen()).status == Status::transaction_exists);
  Check(f.store->Savepoint(f.session, Id(200), Id(201), f.Gen()).ok());
  Check(f.store->Savepoint(f.session, Id(200), Id(201), f.Gen()).status == Status::savepoint_exists);
  const auto before_reset = f.Gen();
  Check(f.store->Reset(f.session, before_reset).ok());
  Check(f.store->ReadAdmitted(f.session, key, {}, before_reset, {}).status == Status::stale_generation);
  Check(f.store->EndTransaction(f.session, Id(200), f.Gen()).status == Status::transaction_missing);
  Check(f.store->Snapshot(f.session).savepoints == 0);
  Check(f.store->Snapshot(f.session).active_transactions == 0);
  f.Missing(key);
  Check(f.store->Close(f.session).ok());
  Check(f.store->ReadAdmitted(f.session, key, {}, f.Gen(), {}).status == Status::closed);
  Check(f.store->BeginTransaction(f.session, Id(200), f.Gen()).status == Status::closed);
  Check(f.store->Close(f.session).ok());
  f.store.reset();
  Check(f.allocator.Snapshot().current_bytes == 0 && f.allocator.Snapshot().active_allocation_count == 0);
}

void Inject(m::BoundedAllocator& allocator) {
  m::MemoryFailureInjectionConfiguration config;
  config.test_guard = m::MakeMemoryFailureInjectionTestGuard();
  config.fixture_enabled = true; config.fixture_name = "scoped-session-values";
  config.evidence_note = "retention mutation must be atomic on allocation refusal";
  m::MemoryFailureInjectionRule rule;
  rule.rule_id = "next-retention-allocation";
  rule.callsite = "engine.session.scoped_value_store";
  config.rules.push_back(rule);
  Check(allocator.EnableAllocationFailureInjection(std::move(config)).ok());
}
void AllocationAndQuotaFailures() {
  m::BoundedAllocator allocator(Policy());
  Inject(allocator);
  auto failed = s::ScopedValueStore::Create(allocator, Id(1), Id(2));
  Check(!failed.owner && failed.status == Status::memory_refused);
  Check(allocator.Snapshot().current_bytes == 0);
  Fixture f;
  const auto key = Key(1), txkey = Key(2);
  const auto tx = Id(200), mark = Id(210);
  f.Write(key, 1);
  auto before = f.store->Snapshot(f.session);
  Inject(f.allocator);
  Check(f.store->BeginTransaction(f.session, tx, f.Gen()).status == Status::memory_refused);
  Check(f.Gen() == before.generation && f.allocator.Snapshot().current_bytes == before.resident_bytes);
  Check(f.allocator.DisableAllocationFailureInjection().ok());
  Check(f.store->BeginTransaction(f.session, tx, f.Gen()).ok());
  f.Write(txkey, 2, tx);
  for (unsigned kind = 0; kind < 3; ++kind) {
    before = f.store->Snapshot(f.session); Inject(f.allocator);
    auto result = kind == 0 ? f.store->Savepoint(f.session, tx, mark, f.Gen()) :
        f.store->WriteAdmitted(f.session, kind == 1 ? key : txkey,
          kind == 1 ? s::ValueScope::session : s::ValueScope::transaction,
          kind == 1 ? s::Identity{} : tx, f.Gen(), f.Value({}));
    Check(result.status == Status::memory_refused);
    Check(f.Gen() == before.generation && f.allocator.Snapshot().current_bytes == before.resident_bytes);
    f.Expect(key, 1); f.Expect(txkey, 2, tx, s::ValueScope::transaction);
    Check(f.allocator.DisableAllocationFailureInjection().ok());
  }
  Check(f.store->Savepoint(f.session, tx, mark, f.Gen()).ok());
  before = f.store->Snapshot(f.session);
  Inject(f.allocator);
  Check(f.store->EraseAdmitted(f.session, txkey, s::ValueScope::transaction, tx,
        f.Gen()).status == Status::memory_refused);
  Check(f.Gen() == before.generation && f.allocator.Snapshot().current_bytes == before.resident_bytes);
  f.Expect(txkey, 2, tx, s::ValueScope::transaction);
  Check(f.allocator.DisableAllocationFailureInjection().ok());
  f.Write(txkey, 3, tx);
  Inject(f.allocator); // Rollback, release, expiry and destruction need no allocation.
  Check(f.store->RollbackTo(f.session, tx, mark, f.Gen()).ok());
  f.Expect(txkey, 2, tx, s::ValueScope::transaction);
  Check(f.store->ReleaseSavepoint(f.session, tx, mark, f.Gen()).ok());
  Check(f.store->EndTransaction(f.session, tx, f.Gen()).ok());
  f.Expect(key, 1); f.store.reset();
  Check(f.allocator.Snapshot().current_bytes == 0);
  s::StoreLimits limits; limits.retained_versions = 2; limits.active_transactions = 1; limits.savepoints = 1;
  Fixture bounded(limits);
  Check(bounded.store->BeginTransaction(bounded.session, tx, bounded.Gen()).ok());
  Check(bounded.store->BeginTransaction(bounded.session, Id(201), bounded.Gen()).status == Status::quota_exceeded);
  bounded.Write(key, 1, tx);
  Check(bounded.store->Savepoint(bounded.session, tx, mark, bounded.Gen()).ok());
  Check(bounded.store->Savepoint(bounded.session, tx, Id(211), bounded.Gen()).status == Status::quota_exceeded);
  bounded.Write(key, 2, tx);
  // Replacing the current, unreferenced version at full retention quota works;
  // adding a different key still requires another retained version.
  bounded.Write(key, 3, tx);
  before = bounded.store->Snapshot(bounded.session);
  Check(bounded.store->WriteAdmitted(bounded.session, txkey, s::ValueScope::transaction, tx,
        bounded.Gen(), bounded.Value({})).status == Status::quota_exceeded);
  Check(bounded.Gen() == before.generation);
  bounded.Expect(key, 3, tx, s::ValueScope::transaction);
  Check(bounded.store->RollbackTo(bounded.session, tx, mark, bounded.Gen()).ok());
  bounded.Expect(key, 1, tx, s::ValueScope::transaction);
  Check(bounded.store->ReleaseSavepoint(bounded.session, tx, mark, bounded.Gen()).ok());
  bounded.Write(txkey, 4, tx);
  Inject(bounded.allocator);  // Erase at full quota must not allocate.
  Check(bounded.store->EraseAdmitted(bounded.session, key, s::ValueScope::transaction, tx,
        bounded.Gen()).ok());
  bounded.Missing(key, tx);
  Check(bounded.allocator.FailureInjectionSnapshot().rules.front().matched_sequence == 0);
  s::StoreLimits no_room; no_room.resident_bytes = sizeof(s::ScopedValueStore) + 1;
  Fixture tight(no_room);
  Check(tight.store->WriteAdmitted(tight.session, key, s::ValueScope::session, {}, tight.Gen(),
        tight.Value({})).status == Status::quota_exceeded);
  Check(tight.Gen() == 1 && tight.store->Snapshot(tight.session).retained_versions == 0);
  auto policy = Policy(); policy.byte_limit = sizeof(s::ScopedValueStore) + 1;
  m::BoundedAllocator physically_bounded(policy);
  auto admitted = s::ScopedValueStore::Create(physically_bounded, Id(1), Id(2));
  Check(admitted.status == Status::ok);
  Check(admitted.owner->WriteAdmitted(Id(2), key, s::ValueScope::session, {}, 1,
        tight.Value({})).status == Status::memory_refused);
  Check(admitted.owner->Snapshot(Id(2)).generation == 1);
  admitted.owner.reset();
  Check(physically_bounded.Snapshot().current_bytes == 0);
  Check(s::ScopedValueStore::Create(physically_bounded, {}, Id(2)).status == Status::invalid_argument);
  Check(s::ScopedValueStore::Create(physically_bounded, Id(1), {}).status == Status::invalid_argument);
}

void ConcurrentWriters() {
  Fixture f;
  std::atomic<bool> failed = false;
  std::vector<std::thread> threads;
  for (unsigned index = 0; index < 4; ++index) threads.emplace_back([&, index] {
    for (unsigned value = 0; value < 250; ++value) {
      const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(value), 0, 0, 0};
      for (;;) {
        const auto result = f.store->WriteAdmitted(f.session, Key(index + 1), s::ValueScope::session,
                                                  {}, f.Gen(), f.Value(bytes));
        if (result.ok()) break;
        if (result.status != Status::stale_generation) { failed = true; return; }
      }
    }
  });
  for (auto& thread : threads) thread.join();
  Check(!failed);
  Check(f.Gen() == 1001 && f.store->Snapshot(f.session).retained_versions == 4);
  for (unsigned i = 0; i < 4; ++i) f.Expect(Key(i + 1), 249);
  Check(f.allocator.Snapshot().current_bytes == f.store->Snapshot(f.session).resident_bytes);
  const auto same_generation = f.Gen();
  std::atomic<unsigned> accepted = 0, stale = 0;
  threads.clear();
  for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&, i] {
    const auto result = f.store->WriteAdmitted(f.session, Key(90 + i), s::ValueScope::session,
                                              {}, same_generation, f.Value({}));
    if (result.ok()) ++accepted;
    else if (result.status == Status::stale_generation) ++stale;
    else failed = true;
  });
  for (auto& thread : threads) thread.join();
  Check(!failed && accepted == 1 && stale == 7 && f.Gen() == same_generation + 1);
}

void SavepointReferenceModel() {
  // Independent snapshot-copy oracle; the implementation uses no such copies.
  // Checks compaction when marks from separate transactions are interleaved,
  // outer marks are released and an inherit marker is itself rolled back.
  Fixture f;
  using Values = std::map<unsigned, unsigned>;
  struct Mark { s::Identity id; Values values; };
  struct Transaction { s::Identity id; Values values; std::vector<Mark> marks; };
  Values session;
  std::array<Transaction, 2> transactions{{{Id(200), {}, {}}, {Id(201), {}, {}}}};
  for (auto& tx : transactions) Check(f.store->BeginTransaction(f.session, tx.id, f.Gen()).ok());
  std::mt19937 random(0x53425354);
  unsigned identity = 10000;
  for (unsigned step = 0; step < 5000; ++step) {
    auto& tx = transactions[random() % transactions.size()];
    const unsigned key = random() % 12 + 1, value = random();
    switch (random() % 9) {
      case 0: session[key] = value; f.Write(Key(key), value); break;
      case 1: case 2: tx.values[key] = value; f.Write(Key(key), value, tx.id); break;
      case 3:
        session.erase(key);
        Check(f.store->EraseAdmitted(f.session, Key(key), s::ValueScope::session, {}, f.Gen()).ok());
        break;
      case 4:
        tx.values.erase(key);
        Check(f.store->EraseAdmitted(f.session, Key(key), s::ValueScope::transaction, tx.id, f.Gen()).ok());
        break;
      case 5:
        if (tx.marks.size() < 8) {
          auto mark = Id(identity++);
          Check(f.store->Savepoint(f.session, tx.id, mark, f.Gen()).ok());
          tx.marks.push_back({mark, tx.values});
        }
        break;
      case 6:
        if (!tx.marks.empty()) {
          const auto index = random() % tx.marks.size();
          Check(f.store->RollbackTo(f.session, tx.id, tx.marks[index].id, f.Gen()).ok());
          tx.values = tx.marks[index].values;
          tx.marks.resize(index + 1);
        }
        break;
      case 7:
        if (!tx.marks.empty()) {
          const auto index = random() % tx.marks.size();
          Check(f.store->ReleaseSavepoint(f.session, tx.id, tx.marks[index].id, f.Gen()).ok());
          tx.marks.erase(tx.marks.begin() + static_cast<std::ptrdiff_t>(index));
        }
        break;
      case 8:
        Check(f.store->EndTransaction(f.session, tx.id, f.Gen()).ok());
        tx = {Id(identity++), {}, {}};
        Check(f.store->BeginTransaction(f.session, tx.id, f.Gen()).ok());
        break;
    }
    for (unsigned k = 1; k <= 12; ++k) {
      if (session.contains(k)) f.Expect(Key(k), session.at(k)); else f.Missing(Key(k));
      for (const auto& owner : transactions) {
        if (owner.values.contains(k)) f.Expect(Key(k), owner.values.at(k), owner.id, s::ValueScope::transaction);
        else if (session.contains(k)) f.Expect(Key(k), session.at(k), owner.id);
        else f.Missing(Key(k), owner.id);
      }
    }
    Check(f.store->Snapshot(f.session).resident_bytes == f.allocator.Snapshot().current_bytes);
  }
  // A single old mark should not retain thousands of intermediate updates.
  auto& tx = transactions.front();
  Check(f.store->EndTransaction(f.session, tx.id, f.Gen()).ok());
  tx.id = Id(identity++);
  Check(f.store->BeginTransaction(f.session, tx.id, f.Gen()).ok());
  f.Write(Key(100), 1, tx.id);
  const auto mark = Id(identity++);
  Check(f.store->Savepoint(f.session, tx.id, mark, f.Gen()).ok());
  f.Write(Key(100), 2, tx.id);
  const auto retained = f.store->Snapshot(f.session).retained_versions;
  for (unsigned i = 0; i < 10000; ++i) f.Write(Key(100), i, tx.id);
  Check(f.store->Snapshot(f.session).retained_versions == retained);
  Check(f.store->RollbackTo(f.session, tx.id, mark, f.Gen()).ok());
  f.Expect(Key(100), 1, tx.id, s::ValueScope::transaction);
}

void KnownTerminalBoundary() {
  Fixture f;
  const auto key = Key(1);
  const auto tx = Id(200);
  Check(f.store->BoundTo(f.database, f.session));
  Check(!f.store->BoundTo(Id(999), f.session) && !f.store->BoundTo(f.database, Id(999)));
  Check(f.store->BeginTransaction(f.session, tx, f.Gen()).ok());
  f.Write(key, 7); f.Write(key, 9, tx); f.Write(key, 11);
  const auto before = f.Gen();
  {
    auto guard = f.store->PrepareTerminal(f.session, tx);
    Check(guard.admission().ok());
    // Destructor on refusal/exception/unknown outcome cannot expire values.
  }
  Check(f.Gen() == before); f.Expect(key, 9, tx, s::ValueScope::transaction); f.Expect(key, 11);
  {
    auto guard = f.store->PrepareTerminal(Id(999), tx);
    Check(guard.admission().status == Status::owner_mismatch);
    guard.ApplyKnownTerminal();
  }
  {
    auto guard = f.store->PrepareTerminal(f.session, Id(999));
    Check(guard.admission().status == Status::transaction_missing);
    guard.ApplyKnownTerminal();
  }
  Inject(f.allocator);
  try {
    auto guard = f.store->PrepareTerminal(f.session, tx);
    Check(guard.admission().ok());
    auto moved = std::move(guard);
    guard.ApplyKnownTerminal(); // Moved-from guard holds no lock or permission.
    moved.ApplyKnownTerminal(); moved.ApplyKnownTerminal();
    throw std::bad_alloc();  // Result/diagnostic failure cannot restore values.
  } catch (const std::bad_alloc&) {}
  Check(f.Gen() == before + 1 && f.store->Snapshot(f.session).active_transactions == 0);
  f.Expect(key, 11);
  Check(f.allocator.FailureInjectionSnapshot().rules.front().matched_sequence == 0);
}

void PreparedMutationBoundary() {
  Fixture f;
  const auto key = Key(1);
  const auto tx = Id(200), mark = Id(210);
  f.Write(key, 1);
  auto before = f.store->Snapshot(f.session);
  std::array<std::uint8_t, 4> bytes{2, 0, 0, 0};
  {
    auto prepared = f.store->PrepareWriteAdmitted(f.session, key, s::ValueScope::session,
                                                 {}, before.generation, f.Value(bytes));
    Check(prepared.admission().ok() && prepared.admission().generation == before.generation);
    Check(f.allocator.Snapshot().current_bytes > before.resident_bytes);
    auto moved = std::move(prepared);
    Check(!prepared.admission().ok() && !prepared.PublishAdmitted().ok());
    Check(moved.admission().ok());
    // Audit refusal/exception: dropping the prepared owner must not publish.
  }
  Check(f.Gen() == before.generation &&
        f.allocator.Snapshot().current_bytes == before.resident_bytes);
  f.Expect(key, 1);
  {
    auto prepared = f.store->PrepareWriteAdmitted(f.session, key, s::ValueScope::session,
                                                 {}, before.generation, f.Value(bytes));
    Check(prepared.admission().ok());
    bytes[0] = 99; // Prepared value owns a copy before audit/caller-buffer reuse.
    Inject(f.allocator);
    const auto first = prepared.PublishAdmitted();
    Check(first.ok() && first.generation == before.generation + 1);
    auto moved = std::move(prepared);
    Check(!prepared.PublishAdmitted().ok());
    Check(moved.PublishAdmitted().generation == first.generation);
  }
  Check(f.allocator.FailureInjectionSnapshot().rules.front().matched_sequence == 0);
  Check(f.allocator.DisableAllocationFailureInjection().ok());
  Check(f.Gen() == before.generation + 1);
  f.Expect(key, 2);
  before = f.store->Snapshot(f.session);
  Inject(f.allocator);
  {
    auto refused = f.store->PrepareWriteAdmitted(f.session, key, s::ValueScope::session,
                                                {}, before.generation, f.Value(bytes));
    Check(refused.admission().status == Status::memory_refused);
    Check(refused.PublishAdmitted().status == Status::memory_refused);
  }
  Check(f.Gen() == before.generation &&
        f.allocator.Snapshot().current_bytes == before.resident_bytes);
  Check(f.allocator.DisableAllocationFailureInjection().ok());
  f.Expect(key, 2);
  for (bool publish : {false, true}) {
    const auto generation = f.Gen();
    {
      auto prepared = f.store->PrepareEraseAdmitted(f.session, key, s::ValueScope::session,
                                                   {}, generation);
      Check(prepared.admission().ok());
      if (publish) {
        Check(prepared.PublishAdmitted().generation == generation + 1);
        Check(prepared.PublishAdmitted().generation == generation + 1);
      }
    }
    if (publish) f.Missing(key); else f.Expect(key, 2);
    Check(f.Gen() == generation + (publish ? 1 : 0));
  }
  const auto missing_generation = f.Gen();
  {
    auto noop = f.store->PrepareEraseAdmitted(f.session, key, s::ValueScope::session,
                                             {}, missing_generation);
    Check(noop.PublishAdmitted().generation == missing_generation + 1);
    Check(noop.PublishAdmitted().generation == missing_generation + 1);
  }
  Check(f.Gen() == missing_generation + 1);
  Check(f.store->BeginTransaction(f.session, tx, f.Gen()).ok());
  f.Write(key, 3); f.Write(key, 4, tx);
  Check(f.store->Savepoint(f.session, tx, mark, f.Gen()).ok());
  for (bool publish : {false, true}) {
    before = f.store->Snapshot(f.session);
    {
      auto erased = f.store->PrepareEraseAdmitted(f.session, key, s::ValueScope::transaction,
                                                 tx, before.generation);
      Check(erased.admission().ok());
      Check(f.allocator.Snapshot().current_bytes > before.resident_bytes);
      if (publish) Check(erased.PublishAdmitted().ok());
    }
    if (!publish) {
      Check(f.Gen() == before.generation &&
            f.allocator.Snapshot().current_bytes == before.resident_bytes);
      f.Expect(key, 4, tx, s::ValueScope::transaction);
    } else f.Expect(key, 3, tx);
  }
  Check(f.store->RollbackTo(f.session, tx, mark, f.Gen()).ok());
  f.Expect(key, 4, tx, s::ValueScope::transaction);
  Check(f.store->ReleaseSavepoint(f.session, tx, mark, f.Gen()).ok());
  const auto generation = f.Gen();
  Inject(f.allocator);
  {
    auto erased = f.store->PrepareEraseAdmitted(f.session, key, s::ValueScope::transaction,
                                               tx, generation);
    Check(erased.admission().ok() && erased.PublishAdmitted().ok());
  }
  Check(f.allocator.FailureInjectionSnapshot().rules.front().matched_sequence == 0);
  f.Expect(key, 3, tx);
}

void PreparedMutationSerialization() {
  for (bool publish : {false, true}) {
    Fixture f;
    const auto key = Key(1);
    f.Write(key, 1);
    const auto generation = f.Gen();
    const std::array<std::uint8_t, 4> prepared_bytes{2, 0, 0, 0}, competing_bytes{3, 0, 0, 0};
    std::atomic<bool> entered = false, finished = false;
    s::StoreResult competing;
    std::thread writer;
    {
      auto prepared = f.store->PrepareWriteAdmitted(f.session, key, s::ValueScope::session,
                                                   {}, generation, f.Value(prepared_bytes));
      Check(prepared.admission().ok());
      writer = std::thread([&] {
        entered = true;
        competing = f.store->WriteAdmitted(f.session, key, s::ValueScope::session,
                                           {}, generation, f.Value(competing_bytes));
        finished = true;
      });
      while (!entered.load()) std::this_thread::yield();
      // No store operation or inventory acquisition may reenter on this thread.
      // Hold the guard through publication/abandonment, then join after unlock.
      if (publish) (void)prepared.PublishAdmitted();
    }
    writer.join();
    Check(finished && f.Gen() == generation + 1);
    Check(competing.status == (publish ? Status::stale_generation : Status::ok));
    f.Expect(key, publish ? 2 : 3);
  }
}

void CollisionAndSessionIsolation() {
  Fixture f;
  auto second = s::ScopedValueStore::Create(f.allocator, f.database, Id(102));
  Check(second.status == Status::ok);
  // More full binary keys than buckets: collision equality must remain exact.
  for (unsigned i = 0; i < 1100; ++i) f.Write(Key(i + 1), i);
  for (unsigned i = 0; i < 1100; ++i) f.Expect(Key(i + 1), i);
  for (const auto family : {s::ValueFamily::setting, s::ValueFamily::qualifier}) {
    f.Write(Key(1, family), 9999); f.Expect(Key(1), 0); f.Expect(Key(1, family), 9999);
  }
  Check(second.owner->ReadAdmitted(Id(102), Key(1), {}, 1, {}).status == Status::not_found);
  Check(second.owner->ReadAdmitted(f.session, Key(1), {}, 1, {}).status == Status::owner_mismatch);
  f.store.reset(); second.owner.reset();
  Check(f.allocator.Snapshot().current_bytes == 0);
}

int main() {
  try {
    LifetimeRules(); EraseAndCompaction(); ExactBytesAndAdmission();
    AllocationAndQuotaFailures(); ConcurrentWriters(); CollisionAndSessionIsolation(); SavepointReferenceModel(); KnownTerminalBoundary();
    PreparedMutationBoundary(); PreparedMutationSerialization();
    std::cout << "scoped session retention: lifecycle, binary values, quotas, allocation failures, concurrency passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
