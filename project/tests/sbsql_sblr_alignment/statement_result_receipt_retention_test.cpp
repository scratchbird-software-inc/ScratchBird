// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"
#include "engine/statement_context_receipt_retention.hpp"
#include "engine/internal_api/dml/update_resource_authority_provider.hpp"
#include "transaction_snapshot.hpp"
#include <barrier>
#include <thread>

namespace mga = scratchbird::transaction::mga;
using Owners = api::RetainedStatementResultAuthoritiesV1;
using Reason = api::TypedResultProducerReleaseReasonV1;
static std::atomic<unsigned> checks{0};
static void Check(bool value, const char* message) {
  ++checks;
  Require(value, message);
}

struct Acquired {
  bridge::StatementContextReceiptHandle handle;
  bridge::StatementContextReceiptView view;
  api::EngineRequestContext context;
  std::shared_ptr<api::EngineDmlUpdateResourceReceiptV1> resources;
  platform::TypedUuid snapshot;
  std::array<std::uint8_t, 16> owner{}, receipt_id{}, snapshot_id{};
  Acquired(PublicSession& session, api::EngineRequestContext& transaction) {
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &transaction;
    request.exact_transaction_uuid = transaction.transaction_uuid.canonical;
    Check(bridge::AcquireStatementContextReceipt(
              session.session, &request, &handle, &view, nullptr) ==
              SB_ENGINE_STATUS_OK, "real receipt acquisition failed");
    Check(bridge::CopyStatementContextEngineContextV1(handle, &context, nullptr) ==
              SB_ENGINE_STATUS_OK, "real owner context copy failed");
    const auto parsed = uuid::ParseUuid(view.statement_snapshot_uuid);
    Check(parsed.ok(), "published snapshot UUID is invalid");
    const auto typed = uuid::MakeTypedUuid(platform::UuidKind::object, parsed.value);
    Check(typed.ok(), "snapshot UUID typing failed");
    snapshot = typed.value;
    owner = RawUuid(context.session_uuid.canonical);
    receipt_id = RawUuid(view.receipt_uuid);
    snapshot_id = parsed.value.bytes;
    resources = context.dml_update_resource_receipt.lock();
    Check(resources && !resources->IsRevoked(),
          "actual private resource receipt was not live");
  }
  Owners Retain() {
    Owners out;
    Check(api::RetainStatementContextForResultV1(handle, owner, receipt_id, &out) ==
              SB_ENGINE_STATUS_OK && out.statement_receipt && out.snapshot,
          "real result owners could not be retained");
    return out;
  }
  void Live(Owners& out) const {
    Check(out.statement_receipt->ObserveOwner(owner) ==
              api::TypedResultProducerOwnerObservationV1::authorized,
          "retained session identity lost");
    Check(out.statement_receipt->ObserveReceipt(receipt_id) ==
              api::TypedResultProducerReceiptObservationV1::live,
          "retained receipt authority lost");
    Check(out.snapshot->ObserveSnapshot(snapshot_id, snapshot_id) ==
              api::TypedResultProducerMgaObservationV1::live_and_equal,
          "retained actual snapshot lost");
    Check(!resources->IsRevoked(),
          "private resource receipt revoked before last result owner");
  }
  void RetiredPublic() const {
    api::EngineRequestContext forbidden;
    Check(bridge::CopyStatementContextEngineContextV1(handle, &forbidden, nullptr) ==
              SB_ENGINE_STATUS_INVALID_HANDLE,
          "retired public receipt still admitted context copy");
    Owners late;
    Check(api::RetainStatementContextForResultV1(handle, owner, receipt_id, &late) ==
              SB_ENGINE_STATUS_INVALID_HANDLE && !late.statement_receipt && !late.snapshot,
          "retired public receipt admitted a new private owner");
  }
};

static void SnapshotBaseline(PublicSession& session, api::EngineRequestContext& context) {
  Acquired acquired(session, context);
  auto pin = mga::RetainPublishedSnapshotVector(acquired.snapshot);
  Check(pin.valid() && pin.Resolve().ok(), "real result snapshot pin failed");
  Check(bridge::ReleaseStatementContextReceipt(acquired.handle) == SB_ENGINE_STATUS_OK,
        "ordinary receipt release failed");
  Check(pin.Resolve().ok(), "ordinary receipt release revoked a retained result snapshot");
  Check(!mga::ResolvePublishedSnapshotVector(acquired.snapshot).ok(),
        "ordinary release retained public snapshot authority");
  Check(!mga::RetainPublishedSnapshotVector(acquired.snapshot).valid(),
        "retired snapshot allowed a new owner");
  acquired.RetiredPublic();
  Check(acquired.resources->IsRevoked(),
        "unretained receipt resources survived ordinary release");
  pin.Release();
}

static void IdentityMatrix(PublicSession& session, api::EngineRequestContext& context) {
  Acquired acquired(session, context);
  auto retained = acquired.Retain();
  Owners empty;
  Check(api::RetainStatementContextForResultV1({}, acquired.owner, acquired.receipt_id,
        &empty) == SB_ENGINE_STATUS_INVALID_ARGUMENT, "nil handle admitted");
  Check(api::RetainStatementContextForResultV1(acquired.handle, acquired.owner,
        acquired.receipt_id, nullptr) == SB_ENGINE_STATUS_INVALID_ARGUMENT, "null output admitted");
  Check(api::RetainStatementContextForResultV1(acquired.handle, acquired.owner,
        acquired.receipt_id, &retained) == SB_ENGINE_STATUS_INVALID_ARGUMENT,
        "nonempty owners overwritten");
  // All 255 alternate values at every position of each binary16 authority.
  // This includes valid UUIDs differing only in one byte, not just malformed UUIDs.
  for (std::size_t i = 0; i != 16; ++i) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      if (octet != acquired.owner[i]) {
        auto wrong = acquired.owner; wrong[i] = static_cast<std::uint8_t>(octet);
        Check(retained.statement_receipt->ObserveOwner(wrong) ==
                  api::TypedResultProducerOwnerObservationV1::denied, "foreign owner admitted");
        Check(api::RetainStatementContextForResultV1(acquired.handle, wrong,
              acquired.receipt_id, &empty) == SB_ENGINE_STATUS_SECURITY_DENIED,
              "foreign owner could retain receipt");
      }
      if (octet != acquired.receipt_id[i]) {
        auto wrong = acquired.receipt_id; wrong[i] = static_cast<std::uint8_t>(octet);
        Check(retained.statement_receipt->ObserveReceipt(wrong) ==
                  api::TypedResultProducerReceiptObservationV1::stale, "foreign receipt admitted");
        Check(api::RetainStatementContextForResultV1(acquired.handle, acquired.owner,
              wrong, &empty) == SB_ENGINE_STATUS_INVALID_HANDLE,
              "foreign receipt identity could retain receipt");
      }
      if (octet != acquired.snapshot_id[i]) {
        auto wrong = acquired.snapshot_id; wrong[i] = static_cast<std::uint8_t>(octet);
        Check(retained.snapshot->ObserveSnapshot(wrong, acquired.snapshot_id) ==
                  api::TypedResultProducerMgaObservationV1::stale_or_unequal,
              "foreign statement snapshot admitted");
        Check(retained.snapshot->ObserveSnapshot(acquired.snapshot_id, wrong) ==
                  api::TypedResultProducerMgaObservationV1::stale_or_unequal,
              "foreign result snapshot admitted");
      }
    }
  }
  acquired.Live(retained);
  retained = {};
  Check(bridge::ReleaseStatementContextReceipt(acquired.handle) == SB_ENGINE_STATUS_OK,
        "identity matrix receipt cleanup failed");
}

static void ReleaseMatrix(PublicSession& session, api::EngineRequestContext& context) {
  for (unsigned reason = 0; reason <= static_cast<unsigned>(Reason::open_refused); ++reason) {
    for (bool public_first : {false, true}) {
      for (bool snapshot_first : {false, true}) {
        Acquired acquired(session, context);
        auto first = acquired.Retain();
        auto second = acquired.Retain();
        if (public_first) {
          Check(bridge::ReleaseStatementContextReceipt(acquired.handle) == SB_ENGINE_STATUS_OK,
                "public retirement with result owners failed");
          acquired.RetiredPublic();
          Check(bridge::ReleaseStatementContextReceipt(acquired.handle) ==
                    SB_ENGINE_STATUS_ALREADY_RELEASED, "public retirement replay changed");
        }
        acquired.Live(first);
        acquired.Live(second);
        first = {};  // Actual destructor-driven release, not a test counter.
        acquired.Live(second);
        if (!public_first) {
          Check(bridge::ReleaseStatementContextReceipt(acquired.handle) == SB_ENGINE_STATUS_OK,
                "public retirement after first result close failed");
          acquired.RetiredPublic();
        }
        const auto why = static_cast<Reason>(reason);
        if (snapshot_first) second.snapshot->Release(why);
        second.statement_receipt->Release(why);
        second.statement_receipt->Release(why);
        Check(acquired.resources->IsRevoked(),
              "last result did not revoke actual private resources");
        Check(second.statement_receipt->ObserveReceipt(acquired.receipt_id) ==
                  api::TypedResultProducerReceiptObservationV1::stale,
              "released private receipt was still live");
        Check(!mga::ResolvePublishedSnapshotVector(acquired.snapshot).ok(),
              "last result failed to retire snapshot publication");
        if (!snapshot_first) {
          Check(second.snapshot->ObserveSnapshot(acquired.snapshot_id, acquired.snapshot_id) ==
                    api::TypedResultProducerMgaObservationV1::live_and_equal,
                "receipt close revoked separately held MGA pin");
        }
        second.snapshot->Release(why);
        second.snapshot->Release(why);
        Check(second.snapshot->ObserveSnapshot(acquired.snapshot_id, acquired.snapshot_id) ==
                  api::TypedResultProducerMgaObservationV1::stale_or_unequal,
              "released snapshot adapter remained live");
        Check(bridge::ReleaseStatementContextReceipt(acquired.handle) ==
                  SB_ENGINE_STATUS_ALREADY_RELEASED, "final receipt release replay changed");
      }
    }
  }
}

static void ForcedSession(bool retired) {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  Acquired acquired(session, context);
  auto first = acquired.Retain();
  auto second = acquired.Retain();
  if (retired)
    Check(bridge::ReleaseStatementContextReceipt(acquired.handle) == SB_ENGINE_STATUS_OK,
          "pre-shutdown public retirement failed");
  Check(session.End() == SB_ENGINE_STATUS_OK, "forced session shutdown failed");
  Check(sb_engine_close(session.engine, nullptr) == SB_ENGINE_STATUS_OK, "engine close failed");
  session.engine = nullptr;
  for (auto* out : {&first, &second}) {
    Check(out->statement_receipt->ObserveOwner(acquired.owner) ==
              api::TypedResultProducerOwnerObservationV1::denied,
          "deleted session still authorized a result");
    Check(out->statement_receipt->ObserveReceipt(acquired.receipt_id) ==
              api::TypedResultProducerReceiptObservationV1::stale,
          "forced session teardown left receipt live");
    Check(out->snapshot->ObserveSnapshot(acquired.snapshot_id, acquired.snapshot_id) ==
              api::TypedResultProducerMgaObservationV1::stale_or_unequal,
          "forced session teardown left MGA pin live");
  }
  Check(acquired.resources->IsRevoked(),
        "forced session teardown retained private resources");
  first = {}; second = {};  // Must be safe after both raw session and engine deletion.
  acquired.RetiredPublic();
}

static void TransactionRevocation() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  Acquired acquired(session, context);
  auto retained = acquired.Retain();
  const auto snapshot = mga::ResolvePublishedSnapshotVector(acquired.snapshot);
  Check(snapshot.ok(), "published snapshot disappeared");
  mga::RevokePublishedSnapshotVectorsForTransaction(
      snapshot.descriptor.owning_transaction_uuid, snapshot.descriptor.owning_transaction);
  Check(retained.snapshot->ObserveSnapshot(acquired.snapshot_id, acquired.snapshot_id) ==
            api::TypedResultProducerMgaObservationV1::stale_or_unequal,
        "actual transaction revocation failed to invalidate retained MGA");
  Owners late;
  Check(api::RetainStatementContextForResultV1(acquired.handle, acquired.owner,
        acquired.receipt_id, &late) == SB_ENGINE_STATUS_INVALID_HANDLE,
        "revoked transaction snapshot allowed a new result owner");
  Check(session.End() == SB_ENGINE_STATUS_OK, "revoked transaction session cleanup failed");
}

// Bounded competing histories: a single actual session shutdown races public
// retirement, private final release and observation. No sleeps or retry passes.
static void ConcurrentShutdown() {
  for (unsigned history = 0; history != 32; ++history) {
    auto fixture = CreateFixture();
    PublicSession session(fixture);
    std::atomic<unsigned> probes{0};
    auto context = BeginTransaction(fixture, &probes);
    Acquired acquired(session, context);
    auto retained = acquired.Retain();
    std::barrier start(4);
    std::thread close([&] {
      start.arrive_and_wait();
      Check(session.End() == SB_ENGINE_STATUS_OK, "racing actual session shutdown failed");
    });
    std::thread retire([&] {
      start.arrive_and_wait();
      const auto status = bridge::ReleaseStatementContextReceipt(acquired.handle);
      Check(status == SB_ENGINE_STATUS_OK || status == SB_ENGINE_STATUS_ALREADY_RELEASED,
            "racing public retirement failed");
    });
    std::thread release([&] {
      start.arrive_and_wait();
      retained.statement_receipt->Release(Reason::disconnect);
      retained.snapshot->Release(Reason::disconnect);
    });
    start.arrive_and_wait();
    (void)retained.statement_receipt->ObserveReceipt(acquired.receipt_id);
    (void)retained.snapshot->ObserveSnapshot(acquired.snapshot_id, acquired.snapshot_id);
    close.join(); retire.join(); release.join();
    Check(retained.statement_receipt->ObserveReceipt(acquired.receipt_id) ==
              api::TypedResultProducerReceiptObservationV1::stale,
          "racing shutdown retained receipt authority");
    Check(acquired.resources->IsRevoked(),
          "racing shutdown leaked real receipt resources");
  }
}

int main() {
  {
    auto fixture = CreateFixture();
    PublicSession session(fixture);
    std::atomic<unsigned> probes{0};
    auto context = BeginTransaction(fixture, &probes);
    SnapshotBaseline(session, context);
    IdentityMatrix(session, context);
    ReleaseMatrix(session, context);
    Check(session.End() == SB_ENGINE_STATUS_OK, "ordinary session cleanup failed");
  }
  ForcedSession(false);
  ForcedSession(true);
  TransactionRevocation();
  ConcurrentShutdown();
  std::cout << "real statement/result retention " << checks.load()
            << " checks PASS; binary identity matrix;40 release histories;32 competing shutdown histories\n";
}
