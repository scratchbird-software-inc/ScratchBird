// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-022 model-based reservation ledger and high-churn memory invariant gate.
#include "hierarchical_memory_budget_ledger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;

constexpr mem::u64 kModelHardLimit = 64 * 1024;
constexpr std::string_view kAuthorityBoundary =
    "memory_evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

mem::HierarchicalMemoryBudgetProvenance RuntimeProvenance(
    std::string label = "ceic_022_memory_reservation_model_gate") {
  mem::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = std::move(label);
  return provenance;
}

mem::HierarchicalMemoryScopeRef Scope(mem::HierarchicalMemoryScopeKind kind,
                                      std::string id) {
  return {kind, std::move(id)};
}

std::vector<mem::HierarchicalMemoryScopeRef> Chain(std::string suffix) {
  return {
      Scope(mem::HierarchicalMemoryScopeKind::process, "process-ceic-022"),
      Scope(mem::HierarchicalMemoryScopeKind::database, "database-ceic-022"),
      Scope(mem::HierarchicalMemoryScopeKind::session, "session-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::transaction, "transaction-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::statement, "statement-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::query, "query-" + suffix),
      Scope(mem::HierarchicalMemoryScopeKind::operator_scope, "operator-" + suffix),
  };
}

void SetBudget(mem::HierarchicalMemoryBudgetLedger* ledger,
               mem::HierarchicalMemoryScopeRef scope,
               mem::u64 hard_limit) {
  mem::HierarchicalMemoryBudget budget;
  budget.scope = std::move(scope);
  budget.hard_limit_bytes = hard_limit;
  budget.soft_limit_bytes = 0;
  budget.provenance = RuntimeProvenance();
  Require(ledger->SetBudget(std::move(budget)).ok(), "CEIC-022 SetBudget failed");
}

mem::HierarchicalMemoryReservationRequest Request(std::string suffix,
                                                  std::string owner,
                                                  mem::u64 bytes,
                                                  mem::u64 lease_expires_at_ms = 0) {
  mem::HierarchicalMemoryReservationRequest request;
  request.scope_chain = Chain(std::move(suffix));
  request.category = mem::MemoryCategory::executor_query_reserved;
  request.memory_class = "ceic_022_query_scratch";
  request.requested_bytes = bytes;
  request.owner_id = std::move(owner);
  request.cancelable = true;
  request.spillable = true;
  request.priority = 5;
  request.weight = 2;
  request.lease_expires_at_ms = lease_expires_at_ms;
  request.provenance = RuntimeProvenance();
  return request;
}

std::string DiagnosticArgument(const mem::DiagnosticRecord& diagnostic,
                               std::string_view key) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == key) {
      return argument.value;
    }
  }
  return {};
}

enum class ModelState {
  reserved,
  active
};

struct ModelReservation {
  mem::HierarchicalMemoryReservationToken token;
  mem::u64 bytes = 0;
  std::string owner;
  mem::u64 lease_expires_at_ms = 0;
  ModelState state = ModelState::reserved;
};

struct Model {
  std::map<mem::u64, ModelReservation> reservations;
  mem::u64 reserved_bytes = 0;
  mem::u64 active_bytes = 0;

  void Add(ModelReservation reservation) {
    reserved_bytes += reservation.bytes;
    reservations.emplace(reservation.token.token_id, std::move(reservation));
  }

  void Commit(mem::u64 token_id) {
    auto it = reservations.find(token_id);
    Require(it != reservations.end(), "CEIC-022 model commit missing token");
    Require(it->second.state == ModelState::reserved,
            "CEIC-022 model commit state mismatch");
    reserved_bytes -= it->second.bytes;
    active_bytes += it->second.bytes;
    it->second.state = ModelState::active;
  }

  void Remove(mem::u64 token_id) {
    auto it = reservations.find(token_id);
    Require(it != reservations.end(), "CEIC-022 model remove missing token");
    if (it->second.state == ModelState::reserved) {
      reserved_bytes -= it->second.bytes;
    } else {
      active_bytes -= it->second.bytes;
    }
    reservations.erase(it);
  }

  std::vector<mem::u64> Tokens() const {
    std::vector<mem::u64> tokens;
    tokens.reserve(reservations.size());
    for (const auto& entry : reservations) {
      tokens.push_back(entry.first);
    }
    return tokens;
  }
};

void AssertSnapshot(const mem::HierarchicalMemoryBudgetLedger& ledger,
                    const Model& model,
                    std::string_view label) {
  const auto snapshot = ledger.Snapshot();
  if (snapshot.current_bytes != model.active_bytes ||
      snapshot.reserved_bytes != model.reserved_bytes) {
    std::cerr << "CEIC-022 snapshot mismatch at " << label
              << " model_active=" << model.active_bytes
              << " ledger_active=" << snapshot.current_bytes
              << " model_reserved=" << model.reserved_bytes
              << " ledger_reserved=" << snapshot.reserved_bytes << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(snapshot.current_bytes + snapshot.reserved_bytes <= kModelHardLimit,
          "CEIC-022 active/reserved usage exceeded hard limit");
  Require(snapshot.active_allocation_count + snapshot.active_reservation_count ==
              model.reservations.size(),
          "CEIC-022 active token count mismatch");
}

mem::u64 PickToken(const Model& model, std::mt19937_64* rng) {
  const auto tokens = model.Tokens();
  Require(!tokens.empty(), "CEIC-022 PickToken called with empty model");
  return tokens[static_cast<std::size_t>((*rng)() % tokens.size())];
}

void ReleaseToken(mem::HierarchicalMemoryBudgetLedger* ledger,
                  Model* model,
                  mem::u64 token_id) {
  const auto token = model->reservations.at(token_id).token;
  Require(ledger->Release(token).ok(), "CEIC-022 model release failed");
  model->Remove(token_id);
}

void CleanupOwner(mem::HierarchicalMemoryBudgetLedger* ledger,
                  Model* model,
                  const std::string& owner) {
  mem::u64 expected_count = 0;
  mem::u64 expected_bytes = 0;
  std::vector<mem::u64> remove;
  for (const auto& entry : model->reservations) {
    if (entry.second.owner == owner) {
      ++expected_count;
      expected_bytes += entry.second.bytes;
      remove.push_back(entry.first);
    }
  }
  const auto cleanup = ledger->CleanupOwner(owner);
  Require(cleanup.ok(), "CEIC-022 owner cleanup failed");
  Require(cleanup.cleaned_reservation_count == expected_count,
          "CEIC-022 owner cleanup count mismatch");
  Require(cleanup.cleaned_bytes == expected_bytes,
          "CEIC-022 owner cleanup byte mismatch");
  for (mem::u64 token_id : remove) {
    model->Remove(token_id);
  }
  for (const auto& entry : model->reservations) {
    Require(entry.second.owner != owner, "CEIC-022 owner cleanup left owner token");
  }
}

void CleanupExpired(mem::HierarchicalMemoryBudgetLedger* ledger,
                    Model* model,
                    mem::u64 now_ms) {
  mem::u64 expected_count = 0;
  mem::u64 expected_bytes = 0;
  std::vector<mem::u64> remove;
  for (const auto& entry : model->reservations) {
    const auto lease = entry.second.lease_expires_at_ms;
    if (lease != 0 && lease <= now_ms) {
      ++expected_count;
      expected_bytes += entry.second.bytes;
      remove.push_back(entry.first);
    }
  }
  const auto cleanup = ledger->CleanupExpiredLeases(now_ms);
  Require(cleanup.ok(), "CEIC-022 lease cleanup failed");
  Require(cleanup.cleaned_reservation_count == expected_count,
          "CEIC-022 lease cleanup count mismatch");
  Require(cleanup.cleaned_bytes == expected_bytes,
          "CEIC-022 lease cleanup byte mismatch");
  for (mem::u64 token_id : remove) {
    model->Remove(token_id);
  }
  for (const auto& entry : model->reservations) {
    const auto lease = entry.second.lease_expires_at_ms;
    Require(lease == 0 || lease > now_ms,
            "CEIC-022 lease cleanup removed/kept wrong lease");
  }
}

void DeterministicLifecycleModel() {
  mem::HierarchicalMemoryBudgetLedger ledger(32, 32);
  SetBudget(&ledger, Scope(mem::HierarchicalMemoryScopeKind::process,
                           "process-ceic-022"),
            kModelHardLimit);

  Model model;
  std::mt19937_64 rng(0xCE1C022ULL);
  mem::u64 now_ms = 1000;
  const std::vector<std::string> owners = {
      "owner-query", "owner-session", "owner-disconnect", "owner-shutdown"};

  for (int step = 0; step < 500; ++step) {
    const auto action = static_cast<int>(rng() % 9);
    if (action <= 2 || model.reservations.empty()) {
      const auto bytes = static_cast<mem::u64>(16 + (rng() % 192));
      const auto owner = owners[static_cast<std::size_t>(rng() % owners.size())];
      const auto lease = (rng() % 3 == 0) ? now_ms + static_cast<mem::u64>(25 + (rng() % 160)) : 0;
      auto request = Request("model-" + std::to_string(step), owner, bytes, lease);
      auto reserved = ledger.Reserve(request);
      if (reserved.ok()) {
        ModelReservation reservation;
        reservation.token = reserved.token;
        reservation.bytes = reserved.token.bytes;
        reservation.owner = owner;
        reservation.lease_expires_at_ms = lease;
        model.Add(std::move(reservation));
        if (rng() % 2 == 0) {
          Require(ledger.Commit(reserved.token).ok(), "CEIC-022 model commit failed");
          model.Commit(reserved.token.token_id);
        }
      } else {
        Require(ledger.Snapshot().current_bytes + ledger.Snapshot().reserved_bytes <=
                    kModelHardLimit,
                "CEIC-022 refused reservation overran ledger");
      }
    } else if (action == 3) {
      ReleaseToken(&ledger, &model, PickToken(model, &rng));
    } else if (action == 4) {
      const auto token_id = PickToken(model, &rng);
      const auto token = model.reservations.at(token_id).token;
      Require(ledger.Cancel(token).ok(), "CEIC-022 cancel failed");
      model.Remove(token_id);
    } else if (action == 5) {
      CleanupOwner(&ledger, &model, owners[static_cast<std::size_t>(rng() % owners.size())]);
    } else if (action == 6) {
      now_ms += 50;
      CleanupExpired(&ledger, &model, now_ms);
    } else if (action == 7) {
      const mem::HierarchicalMemoryReservationToken unknown{999999, 64};
      const auto first = ledger.Release(unknown);
      const auto second = ledger.Release(unknown);
      Require(!first.ok() && !second.ok(),
              "CEIC-022 unknown release unexpectedly succeeded");
      Require(first.diagnostic.diagnostic_code == second.diagnostic.diagnostic_code,
              "CEIC-022 unknown release diagnostic was not deterministic");
      Require(DiagnosticArgument(first.diagnostic, "reason") ==
                  DiagnosticArgument(second.diagnostic, "reason"),
              "CEIC-022 unknown release reason was not deterministic");
    } else {
      CleanupOwner(&ledger, &model, "owner-disconnect");
      CleanupOwner(&ledger, &model, "owner-shutdown");
    }

    AssertSnapshot(ledger, model, "deterministic_lifecycle");
  }

  CleanupOwner(&ledger, &model, "owner-query");
  CleanupOwner(&ledger, &model, "owner-session");
  CleanupOwner(&ledger, &model, "owner-disconnect");
  CleanupOwner(&ledger, &model, "owner-shutdown");
  AssertSnapshot(ledger, model, "deterministic_lifecycle_final");
  Require(ledger.Snapshot().current_bytes == 0 && ledger.Snapshot().reserved_bytes == 0,
          "CEIC-022 final model cleanup leaked bytes");
  std::cout << "ceic_022_model_sequence seed=0xCE1C022 ops=500 active=0 reserved=0 authority="
            << kAuthorityBoundary << '\n';
}

void AuthorityRefusalIsNonAuthoritative() {
  mem::HierarchicalMemoryBudgetLedger ledger(8, 8);
  auto request = Request("unsafe", "unsafe-owner", 64);
  request.provenance.parser_authority = true;
  request.provenance.transaction_finality_authority = true;
  const auto refused = ledger.Reserve(request);
  Require(!refused.ok(), "CEIC-022 unsafe authority request was not refused");
  Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-BUDGET-PROVENANCE-REFUSED",
          "CEIC-022 unsafe authority diagnostic mismatch");
  Require(DiagnosticArgument(refused.diagnostic, "reason") ==
              "unsafe_authority_or_relaxed_provenance_refused",
          "CEIC-022 unsafe authority reason mismatch");
  Require(ledger.Snapshot().current_bytes == 0 && ledger.Snapshot().reserved_bytes == 0,
          "CEIC-022 unsafe authority changed ledger accounting");
}

std::uint64_t Percentile(std::vector<std::uint64_t> values, double percentile) {
  Require(!values.empty(), "CEIC-022 percentile called with empty latency set");
  std::sort(values.begin(), values.end());
  const double scaled = percentile * static_cast<double>(values.size() - 1);
  return values[static_cast<std::size_t>(scaled + 0.5)];
}

void WorkerChurnLane(int workers, int operations_per_worker) {
  mem::HierarchicalMemoryBudgetLedger ledger(64, 64);
  SetBudget(&ledger, Scope(mem::HierarchicalMemoryScopeKind::process,
                           "process-ceic-022"),
            32ULL * 1024ULL * 1024ULL);

  std::atomic<bool> failed{false};
  std::mutex latency_mutex;
  std::vector<std::uint64_t> all_latencies;
  all_latencies.reserve(static_cast<std::size_t>(workers * operations_per_worker));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(workers));

  for (int worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker]() {
      std::vector<std::uint64_t> local_latencies;
      local_latencies.reserve(static_cast<std::size_t>(operations_per_worker));
      for (int op = 0; op < operations_per_worker; ++op) {
        const auto begin = std::chrono::steady_clock::now();
        auto request = Request("churn-" + std::to_string(worker) + "-" +
                                   std::to_string(op),
                               "owner-churn-" + std::to_string(worker),
                               static_cast<mem::u64>(32 + ((worker + op) % 16) * 8));
        const auto reservation = ledger.Reserve(request);
        if (!reservation.ok() || !ledger.Commit(reservation.token).ok() ||
            !ledger.Release(reservation.token).ok()) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        const auto end = std::chrono::steady_clock::now();
        local_latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
      }
      std::lock_guard<std::mutex> lock(latency_mutex);
      all_latencies.insert(all_latencies.end(), local_latencies.begin(), local_latencies.end());
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  Require(!failed.load(std::memory_order_relaxed), "CEIC-022 worker churn operation failed");
  const auto cleanup = ledger.CleanupOwner("owner-that-does-not-exist");
  Require(cleanup.ok(), "CEIC-022 empty cleanup failed");
  const auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, "CEIC-022 worker churn leaked active bytes");
  Require(snapshot.reserved_bytes == 0, "CEIC-022 worker churn leaked reserved bytes");
  Require(snapshot.release_count == static_cast<mem::u64>(workers * operations_per_worker),
          "CEIC-022 worker churn release count mismatch");

  const auto p50 = Percentile(all_latencies, 0.50);
  const auto p95 = Percentile(all_latencies, 0.95);
  const auto p99 = Percentile(all_latencies, 0.99);
  Require(p50 <= p95 && p95 <= p99, "CEIC-022 churn percentiles were not ordered");
  std::cout << "ceic_022_churn workers=" << workers
            << " ops=" << all_latencies.size()
            << " p50_ns=" << p50
            << " p95_ns=" << p95
            << " p99_ns=" << p99
            << " cleanup_released=" << snapshot.release_count
            << " authority=" << kAuthorityBoundary << '\n';
}

}  // namespace

int main() {
  DeterministicLifecycleModel();
  AuthorityRefusalIsNonAuthoritative();
  WorkerChurnLane(8, 96);
  WorkerChurnLane(32, 96);
  WorkerChurnLane(64, 80);
  WorkerChurnLane(128, 64);
  std::cout << "ceic_022_memory_reservation_model_gate=pass\n";
  return EXIT_SUCCESS;
}
