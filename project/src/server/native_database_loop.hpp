// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_NATIVE_DATABASE_LOOP (thin-slice S3b, plan 38 sub-step S3b).
//
// ONE named, engine-owned thread ("sb-db-loop") that constructs, uses and
// destroys everything thread-affine about one open native database: the
// NativeHostedDatabaseRuntime, the startup credential read (custody, the
// retained read operation) and, from S3d, the single TransactionInventoryWriter.
// A wrong-thread destroy of any of them terminates, so every operation on them
// is a REQUEST to this thread, never a call from a session or listener thread.
//
// What the loop is NOT: it is engine infrastructure, not L5. L5 is an agent
// (an internal robot client): it binds through the normal path and gets its own
// session and transactions. The loop only authenticates L5's credential as part
// of the read-only verification and later executes the transaction operations
// the server asks for (owner 2026-11-16, Q236; plan 38).
//
// Lifecycle: opening -> verifying -> serving | failed -> closing -> closed.
//   opening    custody begun, the runtime constructing
//   verifying  the read-only verification sequence (plan 38 section 2c)
//   serving    verified, the transaction-store Restart (S3d) has run (or the
//              file needed none) and the loop's single TransactionInventoryWriter
//              is restarted: the database is serving-ready. Write admission for
//              statements stays fenced until a statement path exists (S3e/S3f).
//              (Was, S3b/S3c: Restart had NOT run, the classification was
//              native_verified_restart_pending.) The loop state stays serving
//              while the DATABASE's serving state (NativeServingState below)
//              is serving | restricted | failed.
//   failed     the open or its verification refused; resources already closed
//              on this thread, the cause reported
//   closing/closed  a submitted close request tore everything down on this
//              thread; the thread is then joined by Close()
//
// Requests: a bounded queue (a mutex and a condition variable; the thread
// sleeps on the variable, nothing polls). Kinds today: open, read_login_material (S4a: a credential read with no
// session, below), close. The Restart
// (S3d) runs inside the open request: once per open, as its last step. Plan 36
// (T1) adds complete_and_begin (the atomic commit + replacement begin) as a new
// method of the SAME TransactionInventoryWriter the loop constructs once per
// open, with the same restarted precondition; the request variant below is
// where it goes. Nothing in S3d begins a transaction (owner Q284: L5's first
// session and transaction are S5).
//
// Ownership: the HostedDatabaseRuntime holds the loop by shared_ptr; the loop
// never holds a shared_ptr to itself (the thread function holds a raw pointer
// and the object outlives its thread because the destructor closes and joins).
// Close() runs the close request, joins the thread and only then returns, so
// the database ownership (the flock) is never released before the loop thread
// has retired every resource that depends on it.

#include "careful_write.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "database_ownership.hpp"
#include "native_hosted_database_runtime.hpp"
#include "process_crypto_owner.hpp"
#include "uuid.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace scratchbird::server {

enum class NativeLoopState : std::uint8_t { opening, verifying, serving, failed, closing, closed };
const char* NativeLoopStateName(NativeLoopState state) noexcept;

// A refusal, as a message vector: a specific code, the step that refused, and
// typed fields (enum names and numbers; never a path, a name or a secret).
struct NativeLoopDiagnostic {
  std::string code;
  std::string step;
  std::vector<std::pair<std::string, std::string>> fields;
};

// SB_NATIVE_RESTART (thin-slice S3d, plan 38; owner Q241 = 1). The serving state
// of the DATABASE after the open, separate from the loop thread's lifecycle state:
//   serving     Restart ran (or the file needed none); the writer is restarted.
//   restricted  Restart found committing / recovering slots (no current writer
//               stores them; the outcome needs the header): NOTHING was written,
//               the writer has not restarted, write admission stays fenced and
//               the database is not served normally. The orders are reported.
//   failed      a write or sync failed inside Restart (the file is in one of the
//               crash states the careful-write order allows); the writer refuses
//               everything until the database is closed and opened again, new
//               statements are fenced. Requires a reopen.
enum class NativeServingState : std::uint8_t { serving, restricted, failed };
const char* NativeServingStateName(NativeServingState state) noexcept;

// What the Restart did, as values (message-vector material, no names): the
// engine-owned TRA_cleanup of the transaction inventory. `horizons` are the
// STORED horizons read back after Restart (Restart never rewrites them).
struct NativeRestartReport {
  bool ran = false;               // Restart completed (marking done; clean file: nothing to mark)
  bool writer_failed = false;     // a write or sync failed; reopen required
  bool read_only_skipped = false; // a read-only database: checked, nothing written
  std::uint64_t next_transaction = 0, scanned_from = 0, marked_dead = 0;
  std::uint64_t writes = 0, syncs = 0;  // sink writes that succeeded / syncs issued by Restart
  // The open verification's count of entries in an UNCERTAIN state (created, preparing, prepared, committing,
  // rolling_back, limbo, recovering), as verified BEFORE Restart. It INCLUDES the committing / recovering slots
  // listed in `unresolved`; prepared / limbo are the ones that are uncertain but left alone, not unresolved.
  std::uint64_t uncertain_transactions = 0;
  std::vector<std::uint64_t> unresolved;    // committing / recovering orders
  std::uint64_t horizon_oit = 0, horizon_oat = 0, horizon_ost = 0, horizon_oct = 0, horizon_oar = 0, horizon_olr = 0;
  std::string writer_error;       // TransactionWriterErrorName of the Restart outcome
  std::string write_error;        // CarefulWriteError detail of a failed write, if any
  std::int32_t native_io_error = 0;
};

struct NativeLoopOpenRequest {
  // The process crypto owner (thread-affine to its constructing thread; the
  // loop thread is made its native-database delegate for the open's lifetime).
  ServerProcessCryptoOwner* crypto = nullptr;
  // The native ownership (data-file flock) the caller already holds. The
  // runtime adopts it; the loop never releases it.
  std::shared_ptr<DatabaseOwnershipLock> ownership;
  // What discovery (S3a) read from the first block and page zero.
  core::platform::Uuid database_uuid;
  core::platform::Uuid filespace_uuid;
  std::uint32_t page_size_bytes = 0;
  std::uint64_t total_pages = 0;
};

#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
// SB_TEST_SEAM (S3d; owner Q295 = gate behind a compile-time test guard, 2026-11-19). Compiled ONLY when a
// test target defines SB_NATIVE_LOOP_TEST_SEAMS and compiles native_database_loop.cpp itself; the production
// sb_server_core does not define it and contains neither symbol (verified with nm). A process-wide wrapper of
// the careful-write sink the Restart writes through, so a test child process can inject crash / torn / failing
// sinks into the real server open path. (Was: an always-compiled NativeLoopOpenRequest::restart_sink_wrapper
// member plus these two functions in sb_server_core, copied in by StartHostedEngine.)
void SetNativeRestartSinkWrapperForTest(
    std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)> wrapper);
std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)>
NativeRestartSinkWrapperForTest();
#endif

// Immutable facts about the verified, serving-ready database. Copied values
// only; no custody, no readiness authority.
struct NativeLoopServingFacts {
  core::platform::Uuid engine_instance_uuid;
  core::platform::Uuid service_principal_uuid;   // L5's principal, from the file
  core::platform::Uuid runtime_binding_uuid;
  std::uint64_t transactions = 0, active_transactions = 0, in_doubt_transactions = 0;
  std::uint64_t filespaces = 0, catalog_pages = 0, wasted_pages = 0;
  std::uint64_t verification_workspace_bytes = 0;
  // True until Restart has completed: false for serving; true for restricted (not run) and failed
  // (did not complete). (Was, S3b/S3c: always true, Restart did not exist.)
  bool restart_pending = true;
  NativeServingState serving_state = NativeServingState::serving;
  NativeRestartReport restart;
  // Specific code for a restricted or failed serving state, empty for serving.
  NativeLoopDiagnostic serving_diagnostic;
  // SEARCH_KEY: SB_NATIVE_SERVED_DATABASE_FACTS (S3e, owner Q238 = 1). The immutable marker the SERVER hands to
  // OpenServedEngine: identity, page profile, page-zero fixed roots, the datatype cohort of the captured database
  // record, the engine instance, the TIP Next/horizons after Restart, the availability table identity and the
  // serving state. Built once by the loop at the end of the open; process-local, never serialized.
  std::shared_ptr<const engine::internal_api::NativeServedDatabaseFacts> served_facts;
};

struct NativeLoopOpenResult {
  bool ok = false;
  NativeLoopState state = NativeLoopState::failed;
  NativeLoopDiagnostic diagnostic;  // when !ok
  NativeLoopServingFacts facts;     // when ok
};

// SEARCH_KEY: SB_NATIVE_LOGIN_MATERIAL (thin-slice S4a, plan 40; owner Q306-Q312). A native credential read with NO
// session: the principal's committed native security rows are read from the served file ON THIS THREAD (the runtime
// is thread-affine) and the supplied password is verified against the stored PBKDF2 verifier. The password arrives
// in protected memory (a ScopedProtectedBuffer the caller allocated; ownership moves into the request) and is zeroed
// and released on the loop thread, whatever the result. Allowed while the database is `serving` or `restricted`
// (Q309); refused with NativeLoginMaterialError::database_failed when it is `failed`, and with database_not_serving
// when the loop has no serving database. Reads take no lock and write nothing; no session or transaction is issued.
// Name resolution (S4b), the authorization adapter (Q307) and EngineAuthenticate (S4c) are NOT here.
struct NativeLoopLoginRequest {
  core::platform::Uuid principal_uuid;                 // binary(16); never a name
  core::memory::ScopedProtectedBuffer password;        // consumed
  std::size_t password_length = 0;                     // bytes of `password` that are the password
};

// SEARCH_KEY: SB_NATIVE_NAME_RESOLVE (thin-slice S4b, plan 40; owner Q306 = 1, Q309). A read-only, resolve-only names
// lookup: the name is DATA (see NativeNameResolveInput) and the answer the object UUID of the committed names
// relation, read from the served file ON THIS THREAD (the runtime is thread-affine), like the login-material read
// and without any lock or write. Allowed while the database is `serving` or `restricted`; refused with
// NativeNameResolveError::database_failed when it is `failed`, and with database_not_serving when the loop has no
// serving database. No session or transaction is issued; the returned result carries binary UUIDs and constants only.
using NativeLoopResolveNameRequest = NativeNameResolveInput;

struct NativeLoopCloseResult {
  bool ok = false;
  NativeLoopState state = NativeLoopState::closed;
  NativeLoopDiagnostic diagnostic;  // when !ok
};

class NativeDatabaseLoop final {
 public:
  // Starts the thread (named sb-db-loop) and returns once it is running.
  static std::shared_ptr<NativeDatabaseLoop> Create();
  NativeDatabaseLoop(const NativeDatabaseLoop&) = delete;
  NativeDatabaseLoop& operator=(const NativeDatabaseLoop&) = delete;
  // Closes (a submitted request) and joins. Never throws.
  ~NativeDatabaseLoop();

  // Queue an open. Must be called on the thread that owns the crypto owner
  // (it names this loop thread as the owner's native-database delegate first).
  // The future is ready when the open reached serving or failed; on failure
  // everything the open acquired is already closed on the loop thread.
  std::future<NativeLoopOpenResult> SubmitOpen(NativeLoopOpenRequest request);

  // S4a: queue a login-material read (see NativeLoopLoginRequest). The future is always satisfied; a refusal is a
  // NativeLoginMaterialResult carrying its specific error. Callable from any thread except the loop thread.
  std::future<NativeLoginMaterialResult> SubmitReadLoginMaterial(NativeLoopLoginRequest request);

  // S4b: queue a name resolution (see NativeLoopResolveNameRequest). The future is always satisfied; a refusal is a
  // NativeNameResolveResult carrying its specific error. Callable from any thread except the loop thread.
  std::future<NativeNameResolveResult> SubmitResolveName(NativeLoopResolveNameRequest request);

  // Queue a close: tear down on the loop thread (read, runtime, credential),
  // then leave the loop. Idempotent.
  std::future<NativeLoopCloseResult> SubmitClose();

  // SubmitClose, wait, join the thread and retire the crypto delegation.
  // Callable from any thread except the loop thread itself (refused).
  NativeLoopCloseResult Close() noexcept;

  NativeLoopState state() const noexcept { return state_.load(std::memory_order_acquire); }
  // The database's serving state (see NativeServingState); meaningful once the open completed.
  NativeServingState serving_state() const noexcept { return serving_state_.load(std::memory_order_acquire); }
  std::thread::id thread_id() const noexcept { return thread_id_; }
  bool on_loop_thread() const noexcept { return std::this_thread::get_id() == thread_id_; }

 private:
  NativeDatabaseLoop() = default;
  struct OpenWork {
    NativeLoopOpenRequest request;
    std::promise<NativeLoopOpenResult> done;
  };
  struct CloseWork {
    std::promise<NativeLoopCloseResult> done;
  };
  struct LoginWork {
    NativeLoopLoginRequest request;
    std::promise<NativeLoginMaterialResult> done;
  };
  struct ResolveNameWork {
    NativeLoopResolveNameRequest request;
    std::promise<NativeNameResolveResult> done;
  };
  // plan 36: struct CompleteAndBeginWork { ... } joins this variant.
  using Work = std::variant<OpenWork, CloseWork, LoginWork, ResolveNameWork>;
  static constexpr std::size_t kQueueCapacity = 8;

  void Run() noexcept;
  void HandleOpen(OpenWork&) noexcept;
  NativeLoopCloseResult HandleClose() noexcept;
  void HandleLogin(LoginWork&) noexcept;
  void HandleResolveName(ResolveNameWork&) noexcept;
  struct Serving;  // loop-thread-only state
  // S3d: the single transaction-store Restart, on this thread, under the custody write operation.
  // Returns false with `refusal` set when the open must be refused (nothing written, or a broken
  // invariant); true when the database reached serving, restricted or failed (facts updated).
  bool RunRestart(const NativeLoopOpenRequest&, const NativeOpenVerificationObservation&,
                  NativeLoopDiagnostic& refusal) noexcept;

  std::thread thread_;
  std::thread::id thread_id_;
  std::atomic<NativeLoopState> state_{NativeLoopState::opening};
  std::atomic<NativeServingState> serving_state_{NativeServingState::serving};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Work> queue_;
  bool accepting_ = true;
  bool joined_ = false;
  ServerProcessCryptoOwner* delegated_crypto_ = nullptr;
  // Created, used and destroyed on the loop thread only.
  std::unique_ptr<Serving> serving_;
};

}  // namespace scratchbird::server
