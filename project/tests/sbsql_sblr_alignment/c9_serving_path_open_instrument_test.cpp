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
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

#include <dlfcn.h>
#include <fcntl.h>

// Every open of the database file, from any library: libstdc++'s ifstream and
// direct ::open callers are not FileDevice opens, so the FileDevice counter
// alone undercounts. The executable's definitions interpose libc's.
namespace {
std::mutex watched_mutex;
std::string watched_path;
std::atomic<std::uint64_t> watched_opens{0};
// Opens of "<database>.sb.*" sidecar files: still outside the database until
// those stores move into it (owner direction 2026-11-02, everything in the DB).
std::atomic<std::uint64_t> sidecar_opens{0};

// SB_C9_TRACE_OPENS=1 lists every path opened in the measured window, to
// attribute the counts when a baseline changes.
bool tracing = false;

void CountIfWatched(const char* path) {
  if (path == nullptr) return;
  std::lock_guard<std::mutex> guard(watched_mutex);
  if (watched_path.empty()) return;
  if (tracing) std::cerr << "c9_open " << path << '\n';
  if (watched_path == path) {
    watched_opens.fetch_add(1);
  } else if (std::strncmp(path, watched_path.c_str(), watched_path.size()) == 0 &&
             std::strncmp(path + watched_path.size(), ".sb.", 4) == 0) {
    sidecar_opens.fetch_add(1);
  }
}

template <class Fn>
Fn Next(const char* name) {
  return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}
}  // namespace

extern "C" int open(const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args); }
  CountIfWatched(path);
  static const auto real = Next<int (*)(const char*, int, ...)>("open");
  return real(path, flags, mode);
}
extern "C" int open64(const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args); }
  CountIfWatched(path);
  static const auto real = Next<int (*)(const char*, int, ...)>("open64");
  return real(path, flags, mode);
}
extern "C" int openat(int dir, const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args); }
  CountIfWatched(path);
  static const auto real = Next<int (*)(int, const char*, int, ...)>("openat");
  return real(dir, path, flags, mode);
}
extern "C" int openat64(int dir, const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args); }
  CountIfWatched(path);
  static const auto real = Next<int (*)(int, const char*, int, ...)>("openat64");
  return real(dir, path, flags, mode);
}

namespace {

// Path opens observed for kStatementReceipts receipt acquisitions in one
// active transaction. Lower it, with the conversion that removes the opens.
constexpr std::uint64_t kStatementReceipts = 3;
constexpr std::uint64_t kExpectedServingPathOpens = 6;  // measured 2026-11-02: 2 per receipt
// Every open of the database file, including std::ifstream re-reads.
constexpr std::uint64_t kExpectedServingAllOpens = 3;  // measured 2026-11-03: 1 per receipt
// Sidecar opens in the same window, also exact: the sidecar migration lowers it.
constexpr std::uint64_t kExpectedServingSidecarOpens = 177;  // measured 2026-11-03: 59 per receipt

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

  {
    std::lock_guard<std::mutex> guard(watched_mutex);
    watched_path = fixture.database_path.string();
    tracing = std::getenv("SB_C9_TRACE_OPENS") != nullptr;
  }
  const auto before = TotalOpens();
  const auto before_all = watched_opens.load();
  const auto before_sidecar = sidecar_opens.load();
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
  const auto serving_all_opens = watched_opens.load() - before_all;
  const auto serving_sidecar_opens = sidecar_opens.load() - before_sidecar;

  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "C9 instrument: transaction rollback failed");

  std::cout << "c9_serving_path_opens receipts=" << kStatementReceipts
            << " filedevice_observed=" << serving_opens
            << " filedevice_expected=" << kExpectedServingPathOpens
            << " all_opens_observed=" << serving_all_opens
            << " all_opens_expected=" << kExpectedServingAllOpens
            << " sidecar_opens_observed=" << serving_sidecar_opens
            << " sidecar_opens_expected=" << kExpectedServingSidecarOpens << '\n';
  Require(serving_sidecar_opens == kExpectedServingSidecarOpens,
          "C9 instrument: serving-phase sidecar opens changed; if sidecar stores moved "
          "into the database, lower kExpectedServingSidecarOpens with it");
  Require(serving_all_opens == kExpectedServingAllOpens,
          "C9 instrument: serving-phase opens of the database file (any library) "
          "changed; if a C9 conversion removed them, lower kExpectedServingAllOpens with it");
  Require(serving_opens == kExpectedServingPathOpens,
          "C9 instrument: serving-phase path opens changed; if a C9 conversion "
          "removed them, lower kExpectedServingPathOpens with it");
  return 0;
}
