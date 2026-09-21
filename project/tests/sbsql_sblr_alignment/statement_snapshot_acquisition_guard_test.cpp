// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/statement_snapshot_acquisition_guard.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace { long fail_after = -1; unsigned checks = 0; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
namespace platform = scratchbird::core::platform;
using Guard = scratchbird::engine::StatementSnapshotAcquisitionGuard;
static_assert(!std::is_copy_constructible_v<Guard>);
static_assert(!std::is_move_constructible_v<Guard>);
static_assert(std::is_nothrow_constructible_v<Guard, platform::TypedUuid>);
static_assert(std::is_nothrow_destructible_v<Guard>);

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
mga::SnapshotVectorResult Publish() {
  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  for (unsigned i = 0; i < 2; ++i) {
    const auto id = uuid::IssueRuntimeIdentityV7();
    Check(id.has_value(), "transaction identity issuance failed");
    const auto typed = uuid::MakeTypedUuid(platform::UuidKind::transaction, *id);
    Check(typed.ok(), "transaction identity invalid");
    auto begun = mga::BeginLocalTransaction(inventory, typed.value, 1000 + i);
    Check(begun.ok(), "actual MGA transaction begin failed");
    inventory = std::move(begun.inventory);
  }
  auto published = mga::PublishStatementStableSnapshotVector(
      inventory, mga::MakeLocalTransactionId(2), 1003);
  Check(published.ok(), "actual MGA snapshot publication failed");
  return published;
}
bool Unknown(const platform::TypedUuid& id) {
  const auto resolved = mga::ResolvePublishedSnapshotVector(id);
  return !resolved.ok() && resolved.diagnostic.message_key == "transaction.snapshot_vector.unknown";
}
void EarlyReturn(const platform::TypedUuid& id, unsigned stop) {
  Guard guard(id);
  for (unsigned phase = 0; phase < 64; ++phase) {
    if (phase == stop) return;
  }
  throw std::runtime_error("test missed early return");
}
void Cases() {
  for (unsigned phase = 0; phase < 64; ++phase) {
    auto published = Publish();
    const auto id = published.descriptor.snapshot_uuid;
    EarlyReturn(id, phase);
    Check(Unknown(id), "early refusal retained published snapshot allocation");
    Check(!mga::RetainPublishedSnapshotVector(id).valid(), "new snapshot pin after failed acquisition");
  }
  {
    auto published = Publish();
    const auto id = published.descriptor.snapshot_uuid;
    auto pin = mga::RetainPublishedSnapshotVector(id);
    Check(pin.valid() && pin.Resolve().ok(), "pre-failure pin invalid");
    { Guard guard(id); }
    Check(!pin.Resolve().ok(), "failed acquisition left a usable private pin");
    Check(!mga::RetainPublishedSnapshotVector(id).valid(), "revoked publication admitted another pin");
    pin.Release();
    Check(Unknown(id), "last pin release did not remove abandoned publication");
  }
  {
    auto published = Publish();
    const auto id = published.descriptor.snapshot_uuid;
    auto pin = mga::RetainPublishedSnapshotVector(id);
    {
      Guard guard(id);
      fail_after = 0;
      guard.TransferToOwner();
    }
    fail_after = -1;
    Check(mga::ResolvePublishedSnapshotVector(id).ok() && pin.Resolve().ok(),
          "successful receipt transfer revoked actual snapshot authority");
    mga::ReleasePublishedSnapshotVector(id);
    Check(pin.Resolve().ok(), "normal receipt retirement revoked a retained result pin");
    Check(!mga::RetainPublishedSnapshotVector(id).valid(), "retired publication admitted a new pin");
    pin.Release();
    Check(Unknown(id), "normal last-pin release leaked publication");
  }
  for (unsigned pinned = 0; pinned < 2; ++pinned) {
    auto published = Publish();
    const auto id = published.descriptor.snapshot_uuid;
    auto pin = pinned ? mga::RetainPublishedSnapshotVector(id) : mga::PublishedSnapshotPin{};
    {
      Guard guard(id);
      fail_after = 0;
    }
    fail_after = -1;
    Check(!mga::ResolvePublishedSnapshotVector(id).ok(), "allocation-free abort left snapshot live");
    if (pinned) Check(!pin.Resolve().ok(), "allocation-free abort failed to revoke pin");
    pin.Release();
    Check(Unknown(id), "allocation-free abort failed to release publication");
  }
  unsigned injected = 0;
  for (long fault = 0; fault < 128; ++fault) {
    auto published = Publish();
    const auto id = published.descriptor.snapshot_uuid;
    bool transferred = false;
    bool threw = false;
    try {
      Guard guard(id);
      fail_after = fault;
      // Exercise real guard unwinding through successive allocating receipt
      // staging steps. This is ownership-unit coverage, not the full ABI.
      std::vector<std::string> projection;
      for (unsigned stage = 0; stage < 32; ++stage)
        projection.emplace_back(512 + stage, 'x');
      guard.TransferToOwner();
      transferred = true;
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    fail_after = -1;
    if (threw) {
      ++injected;
      Check(!transferred && Unknown(id), "allocation exception stranded snapshot publication");
    } else {
      Check(transferred && mga::ResolvePublishedSnapshotVector(id).ok(),
            "staged acquisition failed to transfer ownership");
      mga::ReleasePublishedSnapshotVector(id);
      Check(Unknown(id), "transferred test owner failed to release");
      break;
    }
  }
  Check(injected >= 32, "allocation sweep did not cover all staging positions");
  { Guard nil({}); }
  std::cout << "snapshot acquisition checks=" << checks
            << " allocation_faults=" << injected << '\n';
}
}  // namespace

int main() {
  try { Cases(); return EXIT_SUCCESS; }
  catch (const std::exception& error) {
    fail_after = -1;
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
