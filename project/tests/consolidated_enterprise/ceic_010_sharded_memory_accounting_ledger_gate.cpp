// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-010 focused validation for ShardedMemoryAccountingLedger.
#include "sharded_memory_accounting_ledger.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

mem::ShardedMemoryAccountingEvent Event(std::string_view context,
                                        std::string_view owner,
                                        mem::MemoryCategory category,
                                        mem::u64 bytes,
                                        bool page_buffer = false) {
  mem::ShardedMemoryAccountingEvent event;
  event.bytes = bytes;
  event.tag.purpose = "ceic_010_sharded_memory_accounting_ledger_gate";
  event.tag.context_id = std::string(context);
  event.tag.owner = std::string(owner);
  event.tag.category = category;
  event.tag.lifetime = page_buffer ? mem::MemoryLifetime::page_buffer : mem::MemoryLifetime::statement;
  event.page_buffer_bytes = page_buffer;
  return event;
}

mem::ShardedMemoryAccountingEvent EventForDifferentShard(
    const mem::ShardedMemoryAccountingLedger& ledger,
    std::size_t baseline_shard,
    mem::u64 bytes = 64,
    mem::MemoryCategory category = mem::MemoryCategory::test_probe,
    bool page_buffer = false) {
  for (int index = 0; index < 10000; ++index) {
    auto event = Event("ceic-010-shard-" + std::to_string(index),
                       "owner-sharded",
                       category,
                       bytes,
                       page_buffer);
    if (ledger.ShardIndexForEvent(event) != baseline_shard) {
      return event;
    }
  }
  Fail("CEIC-010 could not find a second shard input");
}

const mem::ShardedMemoryAccountingCategorySnapshot* FindCategory(
    const mem::ShardedMemoryAccountingSnapshot& snapshot,
    mem::MemoryCategory category) {
  for (const auto& entry : snapshot.categories) {
    if (entry.category == category) {
      return &entry;
    }
  }
  return nullptr;
}

const mem::ShardedMemoryAccountingScopeSnapshot* FindScope(
    const std::vector<mem::ShardedMemoryAccountingScopeSnapshot>& scopes,
    std::string_view scope_id) {
  for (const auto& entry : scopes) {
    if (entry.scope_id == scope_id) {
      return &entry;
    }
  }
  return nullptr;
}

std::string Serialize(const mem::ShardedMemoryAccountingSnapshot& snapshot) {
  std::ostringstream out;
  out << "context_filter=" << snapshot.context_filter
      << ";shards=" << snapshot.shard_count
      << ";reserved=" << snapshot.reserved_bytes
      << ";current=" << snapshot.current_bytes
      << ";peak=" << snapshot.peak_bytes
      << ";reservations=" << snapshot.reservation_count
      << ";commits=" << snapshot.commit_count
      << ";releases=" << snapshot.release_count
      << ";failures=" << snapshot.failed_release_count
      << ";page_current=" << snapshot.page_buffer_current_bytes
      << ";page_peak=" << snapshot.page_buffer_peak_bytes;
  for (const auto& shard : snapshot.shards) {
    out << "|shard:" << shard.shard_index << ':' << shard.current_bytes << ':'
        << shard.peak_bytes << ':' << shard.commit_count << ':' << shard.release_count;
  }
  for (const auto& category : snapshot.categories) {
    out << "|category:" << mem::MemoryCategoryName(category.category) << ':'
        << category.current_bytes << ':' << category.peak_bytes << ':'
        << category.allocation_count << ':' << category.release_count;
  }
  for (const auto& context : snapshot.contexts) {
    out << "|context:" << context.scope_id << ':' << context.current_bytes << ':'
        << context.peak_bytes << ':' << context.allocation_count << ':'
        << context.release_count;
  }
  for (const auto& owner : snapshot.owners) {
    out << "|owner:" << owner.scope_id << ':' << owner.current_bytes << ':'
        << owner.peak_bytes << ':' << owner.allocation_count << ':'
        << owner.release_count;
  }
  return out.str();
}

void MultiShardReserveCommitRelease() {
  mem::ShardedMemoryAccountingLedger ledger(8);
  const auto first_event = Event("ceic-010-context-a", "owner-a", mem::MemoryCategory::test_probe, 128);
  const auto first_shard = ledger.ShardIndexForEvent(first_event);
  const auto second_event = EventForDifferentShard(ledger, first_shard);

  const auto first = ledger.Reserve(first_event);
  Require(first.ok(), "CEIC-010 first reserve failed");
  const auto second = ledger.Reserve(second_event);
  Require(second.ok(), "CEIC-010 second reserve failed");
  Require(first.token.shard_index != second.token.shard_index, "CEIC-010 did not exercise multiple shards");
  Require(ledger.Commit(first.token).ok(), "CEIC-010 first commit failed");
  Require(ledger.Commit(second.token).ok(), "CEIC-010 second commit failed");

  auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 192, "CEIC-010 merged current bytes mismatch");
  Require(snapshot.commit_count == 2, "CEIC-010 commit count mismatch");
  Require(snapshot.active_allocation_count == 2, "CEIC-010 active allocation count mismatch");

  Require(ledger.Release(first.token).ok(), "CEIC-010 first release failed");
  Require(ledger.Release(second.token).ok(), "CEIC-010 second release failed");
  snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, "CEIC-010 release did not zero merged current bytes");
  Require(snapshot.peak_bytes == 192, "CEIC-010 merged peak bytes mismatch");
  Require(snapshot.release_count == 2, "CEIC-010 release count mismatch");
}

void PerContextPeakCategoryOwnerAndPageBuffer() {
  mem::ShardedMemoryAccountingLedger ledger(8);
  const auto first = ledger.Reserve(Event("ceic-010-context-peak", "owner-peak",
                                         mem::MemoryCategory::test_probe, 100));
  const auto second = ledger.Reserve(Event("ceic-010-context-peak", "owner-peak",
                                          mem::MemoryCategory::diagnostics, 50));
  const auto page = ledger.Reserve(Event("ceic-010-page-context", "owner-page",
                                        mem::MemoryCategory::page_buffer, 4096, true));
  Require(first.ok() && second.ok() && page.ok(), "CEIC-010 setup reserve failed");
  Require(ledger.Commit(first.token).ok(), "CEIC-010 first context commit failed");
  Require(ledger.Commit(second.token).ok(), "CEIC-010 second context commit failed");
  Require(ledger.Commit(page.token).ok(), "CEIC-010 page commit failed");
  Require(ledger.Release(second.token).ok(), "CEIC-010 second context release failed");

  const auto context = ledger.SnapshotForContext("ceic-010-context-peak");
  const auto* context_scope = FindScope(context.contexts, "ceic-010-context-peak");
  Require(context_scope != nullptr, "CEIC-010 context snapshot missing");
  Require(context_scope->current_bytes == 100, "CEIC-010 context current mismatch");
  Require(context_scope->peak_bytes == 150, "CEIC-010 context peak mismatch");
  Require(context.categories.size() == 2, "CEIC-010 per-context category output mismatch");
  Require(FindCategory(context, mem::MemoryCategory::diagnostics) != nullptr,
          "CEIC-010 per-context diagnostics category missing");
  const auto* owner = FindScope(context.owners, "owner-peak");
  Require(owner != nullptr, "CEIC-010 per-context owner output missing");
  Require(owner->peak_bytes == 150, "CEIC-010 owner peak mismatch");

  const auto snapshot = ledger.Snapshot();
  const auto* test_category = FindCategory(snapshot, mem::MemoryCategory::test_probe);
  Require(test_category != nullptr, "CEIC-010 test category missing");
  Require(test_category->current_bytes == 100, "CEIC-010 category current mismatch");
  Require(test_category->peak_bytes == 100, "CEIC-010 category peak mismatch");
  Require(snapshot.page_buffer_current_bytes == 4096, "CEIC-010 page buffer current mismatch");
  Require(snapshot.page_buffer_peak_bytes == 4096, "CEIC-010 page buffer peak mismatch");

  Require(ledger.Release(first.token).ok(), "CEIC-010 first context release failed");
  Require(ledger.Release(page.token).ok(), "CEIC-010 page release failed");
}

void GlobalPeakIsExactNotShardPeakSum() {
  mem::ShardedMemoryAccountingLedger ledger(8);
  const auto first_event = Event("ceic-010-stagger-a", "owner-stagger",
                                mem::MemoryCategory::metrics, 100);
  const auto first_shard = ledger.ShardIndexForEvent(first_event);
  const auto second_event = EventForDifferentShard(ledger,
                                                   first_shard,
                                                   200,
                                                   mem::MemoryCategory::diagnostics);
  const auto first = ledger.Reserve(first_event);
  const auto second = ledger.Reserve(second_event);
  Require(first.ok() && second.ok(), "CEIC-010 stagger setup reserve failed");
  Require(ledger.Commit(first.token).ok(), "CEIC-010 stagger first commit failed");
  Require(ledger.Release(first.token).ok(), "CEIC-010 stagger first release failed");
  Require(ledger.Commit(second.token).ok(), "CEIC-010 stagger second commit failed");
  Require(ledger.Release(second.token).ok(), "CEIC-010 stagger second release failed");

  const auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, "CEIC-010 stagger current bytes leaked");
  Require(snapshot.peak_bytes == 200, "CEIC-010 global peak summed stale shard peaks");

  const auto page_a = ledger.Reserve(Event("ceic-010-page-stagger-a",
                                          "owner-page-stagger",
                                          mem::MemoryCategory::test_probe,
                                          4096,
                                          true));
  const auto page_b = ledger.Reserve(EventForDifferentShard(ledger,
                                                            ledger.ShardIndexForEvent(
                                                                Event("ceic-010-page-stagger-a",
                                                                      "owner-page-stagger",
                                                                      mem::MemoryCategory::test_probe,
                                                                      4096,
                                                                      true)),
                                                            2048,
                                                            mem::MemoryCategory::test_probe,
                                                            true));
  Require(page_a.ok() && page_b.ok(), "CEIC-010 stagger page reserve failed");
  Require(ledger.Commit(page_a.token).ok(), "CEIC-010 stagger page first commit failed");
  auto page_context = ledger.SnapshotForContext("ceic-010-page-stagger-a");
  Require(page_context.page_buffer_current_bytes == 4096,
          "CEIC-010 per-context page current missing page-buffer flag");
  Require(ledger.Release(page_a.token).ok(), "CEIC-010 stagger page first release failed");
  Require(ledger.Commit(page_b.token).ok(), "CEIC-010 stagger page second commit failed");
  Require(ledger.Release(page_b.token).ok(), "CEIC-010 stagger page second release failed");
  const auto page_snapshot = ledger.Snapshot();
  Require(page_snapshot.page_buffer_peak_bytes == 4096,
          "CEIC-010 global page peak summed stale shard peaks");
}

void DeterministicSnapshotAndUnderflowDiagnostic() {
  mem::ShardedMemoryAccountingLedger ledger(8);
  const auto first = ledger.Reserve(Event("ceic-010-det-b", "owner-b",
                                         mem::MemoryCategory::metrics, 72));
  const auto second = ledger.Reserve(Event("ceic-010-det-a", "owner-a",
                                          mem::MemoryCategory::cleanup, 36));
  Require(first.ok() && second.ok(), "CEIC-010 deterministic setup reserve failed");
  Require(ledger.Commit(first.token).ok(), "CEIC-010 deterministic first commit failed");
  Require(ledger.Commit(second.token).ok(), "CEIC-010 deterministic second commit failed");

  const auto serialized_a = Serialize(ledger.Snapshot());
  const auto serialized_b = Serialize(ledger.Snapshot());
  Require(serialized_a == serialized_b, "CEIC-010 merged snapshot is not deterministic");

  auto corrupted = first.token;
  corrupted.bytes += 1;
  const auto refused = ledger.Release(corrupted);
  Require(!refused.ok(), "CEIC-010 corrupted release was accepted");
  Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-LEDGER-RELEASE-UNDERFLOW-REFUSED",
          "CEIC-010 release underflow diagnostic code mismatch");
  Require(ledger.Snapshot().current_bytes == 108, "CEIC-010 underflow changed accounting state");

  Require(ledger.Release(first.token).ok(), "CEIC-010 valid first release failed after refusal");
  Require(ledger.Release(second.token).ok(), "CEIC-010 valid second release failed after refusal");
  Require(ledger.Snapshot().failed_release_count == 1, "CEIC-010 failed release count mismatch");
}

void ConcurrentAccounting() {
  mem::ShardedMemoryAccountingLedger ledger(16);
  constexpr int kThreads = 8;
  constexpr int kIterations = 500;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([thread_index, &ledger]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const auto event = Event("ceic-010-concurrent-" + std::to_string(thread_index),
                                 "owner-concurrent-" + std::to_string(thread_index % 3),
                                 iteration % 2 == 0 ? mem::MemoryCategory::metrics
                                                    : mem::MemoryCategory::diagnostics,
                                 8 + static_cast<mem::u64>(thread_index));
        const auto reservation = ledger.Reserve(event);
        Require(reservation.ok(), "CEIC-010 concurrent reserve failed");
        Require(ledger.Commit(reservation.token).ok(), "CEIC-010 concurrent commit failed");
        Require(ledger.Release(reservation.token).ok(), "CEIC-010 concurrent release failed");
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, "CEIC-010 concurrent current bytes leaked");
  Require(snapshot.reserved_bytes == 0, "CEIC-010 concurrent reserved bytes leaked");
  Require(snapshot.commit_count == static_cast<mem::u64>(kThreads * kIterations),
          "CEIC-010 concurrent commit count mismatch");
  Require(snapshot.release_count == static_cast<mem::u64>(kThreads * kIterations),
          "CEIC-010 concurrent release count mismatch");
  Require(snapshot.failed_release_count == 0, "CEIC-010 concurrent release failure count mismatch");
}

}  // namespace

int main() {
  MultiShardReserveCommitRelease();
  PerContextPeakCategoryOwnerAndPageBuffer();
  GlobalPeakIsExactNotShardPeakSum();
  DeterministicSnapshotAndUnderflowDiagnostic();
  ConcurrentAccounting();
  return EXIT_SUCCESS;
}
