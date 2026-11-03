// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_FILE_DEVICE_PATH_OPEN_OBSERVATION (register C9/C15)
// Embedded-phase instrument: counts database path opens made while the engine
// is already serving (engine open, session begun, transaction active). Under
// the single-owner model those threads should use the open device instead of
// re-opening the path, so each C9 conversion lowers this count. The expected
// count is exact so a regression and an unrecorded improvement both fail.
#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"

#include "storage/disk/disk_device.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>

namespace {

// Path opens observed for kStatementReceipts receipt acquisitions in one
// active transaction. Lower it, with the conversion that removes the opens.
constexpr std::uint64_t kStatementReceipts = 3;
constexpr std::uint64_t kExpectedServingPathOpens = 6;  // measured 2026-11-02: 2 per receipt

std::uint64_t TotalOpens() {
  const auto observed = scratchbird::storage::disk::ObserveFileDevicePathOpens();
  return observed.opens_without_route_owner + observed.opens_with_route_owner;
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = {};

  const auto before = TotalOpens();
  for (std::uint64_t i = 0; i < kStatementReceipts; ++i) {
    bridge::StatementContextAcquireRequest acquire;
    acquire.engine_context = &context;
    acquire.exact_transaction_uuid = context.transaction_uuid;
    bridge::StatementContextReceiptHandle receipt{};
    bridge::StatementContextReceiptView view{};
    sb_engine_result_t result = nullptr;
    Require(bridge::AcquireStatementContextReceipt(session.session, &acquire, &receipt,
                                                   &view, &result) == SB_ENGINE_STATUS_OK,
            "C9 instrument: statement receipt acquisition failed");
    if (result != nullptr) (void)sb_engine_result_release(result);
    Require(bridge::ReleaseStatementContextReceipt(receipt) == SB_ENGINE_STATUS_OK,
            "C9 instrument: statement receipt release failed");
  }
  const auto serving_opens = TotalOpens() - before;

  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "C9 instrument: transaction rollback failed");

  std::cout << "c9_serving_path_opens receipts=" << kStatementReceipts
            << " observed=" << serving_opens
            << " expected=" << kExpectedServingPathOpens << '\n';
  Require(serving_opens == kExpectedServingPathOpens,
          "C9 instrument: serving-phase path opens changed; if a C9 conversion "
          "removed them, lower kExpectedServingPathOpens with it");
  return 0;
}
