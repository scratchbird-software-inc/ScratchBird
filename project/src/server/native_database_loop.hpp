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
// session, below), read_names, begin / complete / complete_and_begin work and the engine read-transaction scope
// (S4d, SB_NATIVE_LOOP_TRANSACTIONS below), close. The Restart
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

#include "session_ledger_chain.hpp"
#include "careful_write.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "database_ownership.hpp"
#include "native_hosted_database_runtime.hpp"
#include "process_crypto_owner.hpp"
#include "row_version.hpp"
#include "uuid.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
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
// SB_TEST_SEAM (S4d): the same, for the sink every loop transaction request (begin, complete, complete-and-begin)
// writes through; nothing set = the real sink. Lets a test child make a real transaction write or sync fail.
void SetNativeTransactionSinkWrapperForTest(
    std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)> wrapper);
std::function<std::unique_ptr<storage::disk::CarefulWriteSink>(storage::disk::CarefulWriteSink&)>
NativeTransactionSinkWrapperForTest();
#endif

// Immutable facts about the verified, serving-ready database. Copied values
// only; no custody, no readiness authority.
struct NativeLoopServingFacts {
  core::platform::Uuid engine_instance_uuid;
  core::platform::Uuid service_principal_uuid;   // L5's principal, from the file
  core::platform::Uuid runtime_binding_uuid;
  std::uint64_t transactions = 0, active_transactions = 0, in_doubt_transactions = 0;
  std::uint64_t filespaces = 0, catalog_pages = 0, wasted_pages = 0;
  // SB_SESSION_LEDGER (plan 36 T3/T4): what the open's ledger stage read (records and pages of the chain; a torn
  // last record was found and ignored).
  std::uint64_t ledger_records = 0, ledger_pages = 0;
  bool ledger_torn_tail = false;
  // S4e: sessions the ledger shows as never ended when the file was opened (a crash or kill of an earlier engine
  // instance left them; this open's Restart already marked their transactions dead). Reported, never repaired here.
  std::uint64_t ledger_orphaned_sessions = 0;
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

// SEARCH_KEY: SB_NATIVE_LOOP_TRANSACTIONS (thin-slice S4d, plan 40; owner Q305, Q309, Q315, 2026-11-19/21).
// Begin, complete (commit or rollback) and complete-and-begin of a native MGA transaction, as REQUESTS to the loop
// thread, which owns the thread-affine TransactionInventoryWriter (plan 36 T1). Every transaction has a durable
// header (owner 2026-11-05). Allowed only while the database is `serving`: `restricted` refuses with the Q299 code
// (ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY) until the S5 resolver, `failed` and not-serving refuse specifically.
// OWNERSHIP (Q315): the writer has no session notion, so the loop keeps an in-memory table, transaction order ->
// opaque owner key (a binary(16) the caller chooses; S4e passes the session UUID), and checks it before every
// complete / complete-and-begin; the replacement transaction of a complete-and-begin belongs to the same owner. A
// completion by a non-owner, or of an order the table does not hold, is refused before anything is written. The
// table is memory only (a restart empties it; the file's dead slots are the record): no session records exist yet
// (S4e) and the header's attachment (+80) and session (+96) stay zero unless the caller supplies them.
enum class NativeLoopTransactionError : std::uint8_t {
  none,
  invalid_request,        // nil owner key, nil/unknown completion kind, a loop-thread caller of a cross-thread submit
  database_not_serving,   // no serving database, the loop is closing / closed, or the work cannot be queued
  database_failed,        // the database is `failed` (or became so): requires reopen
  database_restricted,    // Q299: ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY
  busy,                   // the bounded queue is full
  unknown_transaction,    // the order is not in the loop's table
  not_owner,              // the owner key is not the transaction's owner
  identity_failure,       // no transaction UUID could be issued
  writer_refused,         // the writer refused BEFORE any write; nothing changed, the writer is usable
  writer_failed,          // a write or sync failed: the file is in a crash state, the database is now `failed`
};
const char* NativeLoopTransactionErrorName(NativeLoopTransactionError error) noexcept;
const char* NativeLoopTransactionErrorCode(NativeLoopTransactionError error) noexcept;

enum class NativeLoopCompletion : std::uint8_t { commit, rollback };

// What a begin states about the transaction. Identities are as the caller gives them (all zero = not yet bound; the
// header codec admits zero). The engine instance (+48) is the loop's own, never the caller's.
struct NativeLoopTransactionIdentity {
  core::platform::Uuid principal_uuid;   // header +64: the AUTHENTICATED principal, supplied explicitly by the caller (S4e, at approval)
  core::platform::Uuid attachment_uuid;  // header +80 (zero until S4e / T2-T4)
  core::platform::Uuid session_uuid;     // header +96 (zero until S4e / T2-T4)
};
struct NativeLoopBeginRequest {
  core::platform::Uuid owner_key;            // binary(16), not nil
  bool read_only = false;
  std::uint8_t isolation_profile = 0;        // IsolationProfileKind as the header's u8; 0 = not stated
  NativeLoopTransactionIdentity identity;
};
struct NativeLoopCompleteRequest {
  core::platform::Uuid owner_key;
  std::uint64_t transaction_order = 0;
  NativeLoopCompletion completion = NativeLoopCompletion::commit;
};
struct NativeLoopCompleteAndBeginRequest {
  core::platform::Uuid owner_key;            // owns n, and becomes the owner of the replacement m
  std::uint64_t transaction_order = 0;       // n
  NativeLoopCompletion completion = NativeLoopCompletion::commit;
  bool read_only = false;                    // the replacement's
  std::uint8_t isolation_profile = 0;
  NativeLoopTransactionIdentity identity;
};
// One result for the three requests. Begin fills begun_*; complete fills completed_*; complete-and-begin fills both.
struct NativeLoopTransactionResult {
  NativeLoopTransactionError error = NativeLoopTransactionError::invalid_request;
  std::uint64_t begun_order = 0;
  core::platform::Uuid begun_uuid;
  std::uint64_t completed_order = 0;
  bool committed = false;                    // the completed transaction's final state
  std::uint64_t completed_at_utc_ns = 0;
  // From the writer (message-vector material): its own error name, the plan's successful writes and issued syncs
  // (chain growth, when it was needed, is counted apart), and whether the completion's outcome is decided only by
  // the file at the next open.
  std::string writer_error;
  std::uint32_t writes = 0, syncs = 0, growth_writes = 0, growth_syncs = 0;
  bool outcome_uncertain = false;
  bool ok() const noexcept { return error == NativeLoopTransactionError::none; }
};

// SEARCH_KEY: SB_NATIVE_LOOP_LEDGER (plan 36 T4, owner Q165 = 1, Q308). Appends of session-ledger records as REQUESTS
// to the loop thread, which owns the thread-affine SessionLedger (and the TransactionInventoryWriter whose Begin and
// CompleteAndBegin check the ledger: a header's attachment (+80) and session (+96) must already be COMPLETE ledger
// records, "durable before cited"). Admission is exactly the transaction requests' (NativeLoopTransactionError): only
// while the database is `serving`; `restricted` refuses with the Q299 code (ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY),
// `failed` and not-serving refuse specifically. One custody operation per request. A refusal of the ledger itself
// (codec, references, duplicate, bound) is `writer_refused` with ledger_error naming the cause: nothing was written and
// the loop goes on. A write or sync failure is `writer_failed` and the database becomes `failed` (reopen). No session
// semantics: a record is only appended (S4e creates real sessions); the record UUID is the caller's (a v7 engine id).
struct NativeLoopLedgerResult {
  NativeLoopTransactionError error = NativeLoopTransactionError::invalid_request;
  std::uint64_t ordinal = 0;                // the appended record's ordinal (the refused one's, on a refusal)
  core::platform::Uuid record_uuid;
  std::string ledger_error;                 // the ledger's own error name (a refusal or a failure)
  std::string record_error;                 // the codec's cause when the record itself was refused
  std::uint32_t writes = 0, syncs = 0, growth_writes = 0, growth_syncs = 0;
  bool outcome_uncertain = false;           // writer_failed: the record may or may not be on disk (a reopen decides)
  bool ok() const noexcept { return error == NativeLoopTransactionError::none; }
};

// SEARCH_KEY: SB_NATIVE_LOOP_SESSIONS (S4e; owner 2026-11-21: "the user's transaction begins only on approval", 2026-11-22:
// session end = a NEW ledger record with the ended state; S4d answer 3: close rolls back owned transactions).
// A session's APPROVAL and its END are each ONE request to the loop thread, composed of the same on-loop steps the ledger
// and transaction requests use (AppendLedgerNow, BeginNow, CompleteNow), so the order is fixed and a half-made session is
// ended in the same request. APPROVAL: append the CONNECTION record, then the SESSION record, then the ATTACHMENT record,
// each durable (written, then synced) before the next, then BEGIN the user's transaction with header +48 = the loop's engine
// instance, +64 = the session record's principal, +80 = the attachment, +96 = the session, the owner key = the session UUID
// (the durable-before-cited check of Begin refuses anything else). If any step after the session record fails, an ended
// record (reason setup_failed) is appended before the request answers. The loop fills the database UUID and the engine
// instance of the records; the caller supplies every other identity (v7, server-issued) and the principal, which is the
// AUTHENTICATED one (the server takes it from the authentication result, never from a request field). END: roll back every
// transaction the session owns (CompleteWork rolled_back, no replacement), then append the session's ended record.
enum class NativeLoopSessionStep : std::uint8_t { none, connection, session, attachment, begin, ended };
const char* NativeLoopSessionStepName(NativeLoopSessionStep step) noexcept;
struct NativeLoopSessionOpenRequest {
  core::platform::Uuid connection_uuid;   // the CONNECTION record's UUID: the server-issued channel UUID (v7)
  storage::page::LedgerConnectionRecord connection;
  core::platform::Uuid session_uuid;      // the SESSION record's UUID, the transactions' owner key (v7)
  storage::page::LedgerSessionRecord session;  // open state; engine_instance_uuid is filled by the loop; settings give
                                               // the first transaction's isolation profile and read-only flag
  core::platform::Uuid attachment_uuid;   // the ATTACHMENT record's UUID, header +80 (v7)
};
struct NativeLoopSessionOpenResult {
  NativeLoopTransactionError error = NativeLoopTransactionError::invalid_request;
  NativeLoopSessionStep failed_step = NativeLoopSessionStep::none;  // the step that refused or failed (error != none)
  std::uint64_t connection_ordinal = 0, session_ordinal = 0, attachment_ordinal = 0;  // 0 = not appended
  std::uint64_t begun_order = 0;
  core::platform::Uuid begun_uuid;
  // After a failure past the session record: the ended record (setup_failed) was appended (true) or could not be.
  bool setup_failed_recorded = false;
  std::string detail;                     // the failing step's ledger / writer error name
  std::uint32_t writes = 0, syncs = 0;    // every successful write and every sync of the whole request
  bool ok() const noexcept { return error == NativeLoopTransactionError::none; }
};
struct NativeLoopSessionEndRequest {
  core::platform::Uuid session_uuid;
  storage::page::LedgerEndReason reason = storage::page::LedgerEndReason::none;  // 1..6
};
struct NativeLoopSessionEndResult {
  NativeLoopTransactionError error = NativeLoopTransactionError::invalid_request;
  bool session_unknown = false;           // this loop holds no open session by that UUID (never opened here, or ended)
  std::uint32_t transactions_rolled_back = 0, rollbacks_failed = 0;
  bool ended_recorded = false;            // the ended record is durable
  std::string detail;
  std::uint32_t writes = 0, syncs = 0;
  bool ok() const noexcept { return error == NativeLoopTransactionError::none && ended_recorded; }
};

// SB_NATIVE_ENGINE_READ_TRANSACTION (owner 2026-11-21: the credential read and the names read of a native login run
// in an engine-owned SHORT READ-ONLY SNAPSHOT TRANSACTION, as Firebird's server reads its security database). The
// work runs ON THE LOOP THREAD between a begin and a completion; the completion is ALWAYS made (commit, as Firebird
// does for a read-only transaction) even when the work throws.
// The two bodies of the scope. Owner ruling 2026-11-21: login reads run at an in-memory snapshot point and the user's
// transaction begins only on approval, so snapshot_point is the DEFAULT; durable_transaction stays as an optional path:
//   durable_transaction  a durable read-only transaction (header, TIP slot, 5 writes / 5 syncs per scope, one number)
//   snapshot_point       NO header, NO write, NO number: the TIP's Next and stored horizons captured in memory at the
//                        start (like Firebird's in-memory system transaction 0). The user's own transaction would begin
//                        only on approval. Allowed on `restricted` too (a read of the file, no write).
enum class NativeLoopReadScopeMode : std::uint8_t { durable_transaction, snapshot_point };
struct NativeLoopReadTransaction {
  NativeLoopReadScopeMode mode = NativeLoopReadScopeMode::durable_transaction;
  // snapshot_point: the inventory as it stood at the capture (order stays 0, transaction_uuid nil).
  std::uint64_t snapshot_next_transaction = 0, snapshot_oit = 0, snapshot_oat = 0, snapshot_ost = 0;
  // snapshot_point: the snapshot value the scope's reads decide visibility from (SB_SNAPSHOT_POINT_VIEW): rows whose
  // creator is numbered above visible_through, or listed active / in doubt at the capture, are invisible; the counts are
  // the lengths of those two lists.
  std::uint64_t snapshot_visible_through = 0, snapshot_active_count = 0, snapshot_in_doubt_count = 0;
  std::uint64_t order = 0;
  core::platform::Uuid transaction_uuid;
  core::platform::Uuid owner_key;            // the scope's own key, issued per scope
};
struct NativeLoopReadScopeResult {
  NativeLoopReadScopeMode mode = NativeLoopReadScopeMode::durable_transaction;
  NativeLoopTransactionResult begin;         // the work did not run unless begin.ok(); snapshot_point: the capture
  std::uint64_t snapshot_next_transaction = 0;
  bool work_ran = false;
  bool work_threw = false;                   // the work threw; the exception was absorbed
  NativeLoopTransactionResult complete;      // meaningful when begin.ok()
  bool ok() const noexcept { return begin.ok() && work_ran && !work_threw && complete.ok(); }
};

struct NativeLoopCloseResult {
  bool ok = false;
  NativeLoopState state = NativeLoopState::closed;
  NativeLoopDiagnostic diagnostic;  // when !ok
};

class NativeDatabaseLoop;
// SB_NATIVE_NAMES_SOURCE: the link between the names source in the served facts and its loop. The loop clears `loop`
// (under the lock) when it starts to close, so a source that outlives the loop answers `closed`.
struct NativeNamesBinding {
  std::mutex mutex;
  NativeDatabaseLoop* loop = nullptr;
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

  // SEARCH_KEY: SB_NATIVE_NAMES_SOURCE (S4b rework "Part A"). A read-only read of the committed names (role 2) and
  // identifier profiles (role 5) of the served file, as values, ON THIS THREAD (the runtime is thread-affine); no
  // lock, no write, no session or transaction (SB_NAME_SNAPSHOT_OWN_TXN seam: committed snapshot until S4e).
  // Allowed while the database is `serving` or `restricted` (Q309); refused when `failed` or not serving.
  // The cross-thread submit: the future is always satisfied; a refusal is a snapshot carrying its specific error.
  // Callable from any thread EXCEPT the loop thread (the loop never waits on itself; refused with invalid_request).
  std::future<engine::internal_api::NativeNamesSnapshot> SubmitReadNamesSnapshot();
  // The direct in-loop path: the same read, run now. Callable ONLY on the loop thread (S4c's login runs as loop
  // work); refused with invalid_request elsewhere. It must not be called while another runtime read is in progress
  // on this thread (the runtime answers `busy`): a loop-work step reads, ends its read, then asks for names.
  engine::internal_api::NativeNamesSnapshot ReadNamesSnapshotOnLoopThread() noexcept;
  // SB_NATIVE_NAMES_SOURCE TEST SEAM: run `work` on the loop thread and wait (refused, false, from the loop thread or
  // when the loop is not accepting). It exists so the direct in-loop names path has a real caller before S4c's login
  // (the first real loop work that needs it) exists; S4c's own loop work replaces its use. `work` must not throw.
  bool RunOnLoopThreadForTest(std::function<void()> work);
#if defined(SB_NATIVE_LOOP_TEST_SEAMS)
  // SB_TEST_SEAM (snapshot view): from the loop thread only, run `body` with the careful-write sink every loop
  // transaction request writes through (the same write door and writer), so a test can make a real catalog change
  // between a scope's capture and its read. False (body not run) off the loop thread, when not serving, or when failed.
  // Compiled only when a test target defines SB_NATIVE_LOOP_TEST_SEAMS; sb_server_core has no such symbol.
  bool RunWithTransactionSinkOnLoopThreadForTest(const std::function<void(storage::disk::CarefulWriteSink&)>& body);
#endif

  // SEARCH_KEY: SB_NATIVE_LOOP_TRANSACTIONS (S4d). The direct in-loop login read: the S4a credential read (the same
  // code the submit runs), run now. Callable ONLY on the loop thread (refused with invalid_request elsewhere); it
  // consumes the password as the submit does. Inside RunInEngineReadTransaction it and ReadNamesSnapshotOnLoopThread
  // run under ONE transaction. NOTE the PBKDF2 compare is part of this call (S4a), so it runs inside the scope.
  NativeLoginMaterialResult ReadLoginMaterialOnLoopThread(NativeLoopLoginRequest request) noexcept;

  // Transactions (see SB_NATIVE_LOOP_TRANSACTIONS above). The cross-thread submits queue the work and the future is
  // always satisfied (a refusal is a result carrying its specific error); they are refused with invalid_request on
  // the loop thread (the loop never waits on itself). The OnLoopThread forms run the same work now and are refused
  // with invalid_request anywhere else: login and later loop work use them.
  std::future<NativeLoopTransactionResult> SubmitBeginWork(NativeLoopBeginRequest request);
  std::future<NativeLoopTransactionResult> SubmitCompleteWork(NativeLoopCompleteRequest request);
  std::future<NativeLoopTransactionResult> SubmitCompleteAndBeginWork(NativeLoopCompleteAndBeginRequest request);
  NativeLoopTransactionResult BeginWorkOnLoopThread(const NativeLoopBeginRequest& request) noexcept;
  NativeLoopTransactionResult CompleteWorkOnLoopThread(const NativeLoopCompleteRequest& request) noexcept;
  NativeLoopTransactionResult CompleteAndBeginWorkOnLoopThread(const NativeLoopCompleteAndBeginRequest& request) noexcept;
  // SB_NATIVE_LOOP_LEDGER: append one session-ledger record (see above). The cross-thread submit is refused with
  // invalid_request on the loop thread; the OnLoopThread form is refused with invalid_request anywhere else.
  std::future<NativeLoopLedgerResult> SubmitAppendLedgerRecord(storage::page::SessionLedgerRecord record);
  NativeLoopLedgerResult AppendLedgerRecordOnLoopThread(const storage::page::SessionLedgerRecord& record) noexcept;
  // SB_NATIVE_LOOP_SESSIONS: approval and end of a session (see above). Cross-thread submits; refused with invalid_request on
  // the loop thread. Admission is the transaction requests' (serving only; restricted refuses with the Q299 code BEFORE
  // anything is written; failed and not-serving refuse specifically).
  std::future<NativeLoopSessionOpenResult> SubmitOpenSession(NativeLoopSessionOpenRequest request);
  std::future<NativeLoopSessionEndResult> SubmitEndSession(NativeLoopSessionEndRequest request);
  // Loop thread only: the sessions this loop has open (approved and not yet ended), ascending by UUID.
  std::vector<core::platform::Uuid> OpenSessionsOnLoopThread() const;
  // Loop thread only (empty / zero elsewhere): the ledger's current entry for a record UUID, and its sizes.
  std::optional<storage::page::SessionLedgerEntry> FindLedgerEntryOnLoopThread(const core::platform::Uuid& uuid) const;
  std::uint64_t LedgerRecordCountOnLoopThread() const noexcept;
  // Loop thread only: the verified open's facts as the loop holds them now (wasted_pages, ledger_records, ...).
  std::optional<NativeLoopServingFacts> FactsOnLoopThread() const;
  // Orders of the transactions the loop's ownership table holds, ascending. Loop thread only (empty elsewhere).
  std::vector<std::uint64_t> OpenTransactionOrdersOnLoopThread() const;

  // SB_NATIVE_ENGINE_READ_TRANSACTION: begin an engine-owned read-only transaction, run `work` on the loop thread,
  // complete it (commit) whatever `work` did, report both. From a non-loop thread this queues a scope request and
  // waits; ON the loop thread it runs the scope directly (login is loop work). `work` may throw (absorbed, reported)
  // and may call the OnLoopThread reads above. Refused (the work does not run, begin.error says why) exactly as the
  // transactions are: not serving, failed, restricted (Q299), queue full.
  // SB_SNAPSHOT_POINT_VIEW (closes the former SB_READ_TXN_SNAPSHOT_SEAM for snapshot_point): inside a snapshot_point scope
  // the OnLoopThread reads below (login material, names) decide row-version visibility from the snapshot VALUE captured
  // at the scope's start, not from the inventory at read time. durable_transaction mode (an optional mode) still reads
  // committed state at read time: it has no captured value (see the .cpp for what remains).
  NativeLoopReadScopeResult RunInEngineReadTransaction(std::function<void(const NativeLoopReadTransaction&)> work,
                                                       NativeLoopReadScopeMode mode = NativeLoopReadScopeMode::snapshot_point);
  // SB_NATIVE_LOGIN_SOURCE (S4c): the queueing half of RunInEngineReadTransaction, without the wait: the future is
  // always satisfied (a refusal is a result whose begin.error says why). Refused (invalid_request is not a scope
  // error, so database_not_serving is used) on the loop thread: the loop never waits on itself.
  std::future<NativeLoopReadScopeResult> SubmitReadScope(std::function<void(const NativeLoopReadTransaction&)> work,
                                                         NativeLoopReadScopeMode mode = NativeLoopReadScopeMode::snapshot_point);

  // SEARCH_KEY: SB_NATIVE_LOGIN_SOURCE (S4c). One native login ATTEMPT = one snapshot-point scope whose work runs on
  // the loop thread: `select` (name -> principal, supplied by the engine) then the S4a credential read for the
  // selected principal, the password copied into protected memory on this thread and the caller's bytes scrubbed.
  // For a selection that fails as a CREDENTIAL cause (unknown, hidden or ambiguous name) the credential read still
  // runs, for a well-formed identity that is guaranteed absent, so a name that does not exist costs the same PBKDF2
  // as a wrong password (a timing equaliser; its verdict is discarded). Infrastructure failures skip the burn.
  // Never throws. `Submit...` is the queueing form (non-loop threads, completed by `Finish...`); the direct form runs
  // on the loop thread (login as loop work). The password string is zeroed in every form.
  using LoginSelect = std::function<engine::internal_api::NativeLoginSelection()>;
  std::future<NativeLoopReadScopeResult> SubmitLoginAttempt(const LoginSelect& select, std::string& password,
                                                            engine::internal_api::NativeLoginAttemptResult* out);
  static void FinishLoginAttempt(const NativeLoopReadScopeResult& scope, std::string& password,
                                 engine::internal_api::NativeLoginAttemptResult* out) noexcept;
  engine::internal_api::NativeLoginAttemptResult AttemptLoginOnLoopThread(const LoginSelect& select,
                                                                         std::string& password) noexcept;

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
  struct NamesWork {
    std::promise<engine::internal_api::NativeNamesSnapshot> done;
  };
  struct TestWork {
    std::function<void()> work;
    std::promise<void> done;
  };
  struct BeginWork {
    NativeLoopBeginRequest request;
    std::promise<NativeLoopTransactionResult> done;
  };
  struct CompleteWork {
    NativeLoopCompleteRequest request;
    std::promise<NativeLoopTransactionResult> done;
  };
  struct CompleteAndBeginWork {
    NativeLoopCompleteAndBeginRequest request;
    std::promise<NativeLoopTransactionResult> done;
  };
  struct LedgerWork {
    storage::page::SessionLedgerRecord record;
    std::promise<NativeLoopLedgerResult> done;
  };
  struct SessionOpenWork {
    NativeLoopSessionOpenRequest request;
    std::promise<NativeLoopSessionOpenResult> done;
  };
  struct SessionEndWork {
    NativeLoopSessionEndRequest request;
    std::promise<NativeLoopSessionEndResult> done;
  };
  struct ReadScopeWork {
    std::function<void(const NativeLoopReadTransaction&)> work;
    NativeLoopReadScopeMode mode = NativeLoopReadScopeMode::durable_transaction;
    std::promise<NativeLoopReadScopeResult> done;
  };
  using Work = std::variant<OpenWork, CloseWork, LoginWork, NamesWork, TestWork, BeginWork, CompleteWork,
                            CompleteAndBeginWork, ReadScopeWork, LedgerWork, SessionOpenWork, SessionEndWork>;
  static constexpr std::size_t kQueueCapacity = 8;

  void Run() noexcept;
  void HandleOpen(OpenWork&) noexcept;
  NativeLoopCloseResult HandleClose() noexcept;
  void HandleLogin(LoginWork&) noexcept;
  void HandleNames(NamesWork&) noexcept;
  NativeLoginMaterialResult ReadLoginMaterialNow(NativeLoopLoginRequest&) noexcept;  // loop thread only
  void LoginInScope(const LoginSelect& select, std::string& password,
                    engine::internal_api::NativeLoginAttemptResult* out) noexcept;  // loop thread, inside a scope
  // S4d: admission shared by every transaction request (loop thread only; checked again when the work runs).
  NativeLoopTransactionError AdmitTransactionWork() noexcept;
  // Admission at the queue (any thread, lock held): the refusal of a request that must not be queued, or none.
  NativeLoopTransactionError AdmitQueuedTransaction() const noexcept;
  NativeLoopTransactionResult BeginNow(const NativeLoopBeginRequest&) noexcept;
  NativeLoopTransactionResult CompleteNow(const NativeLoopCompleteRequest&) noexcept;
  NativeLoopTransactionResult CompleteAndBeginNow(const NativeLoopCompleteAndBeginRequest&) noexcept;
  NativeLoopLedgerResult AppendLedgerNow(const storage::page::SessionLedgerRecord&) noexcept;
  NativeLoopReadScopeResult RunReadScopeNow(const std::function<void(const NativeLoopReadTransaction&)>&,
                                            NativeLoopReadScopeMode) noexcept;
  NativeLoopReadScopeResult RunSnapshotScopeNow(const std::function<void(const NativeLoopReadTransaction&)>&) noexcept;
  void HandleTransaction(BeginWork&) noexcept;
  void HandleTransaction(CompleteWork&) noexcept;
  void HandleTransaction(CompleteAndBeginWork&) noexcept;
  void HandleReadScope(ReadScopeWork&) noexcept;
  void HandleLedger(LedgerWork&) noexcept;
  NativeLoopSessionOpenResult OpenSessionNow(const NativeLoopSessionOpenRequest&) noexcept;
  NativeLoopSessionEndResult EndSessionNow(const core::platform::Uuid& session_uuid,
                                           storage::page::LedgerEndReason reason) noexcept;
  void HandleSession(SessionOpenWork&) noexcept;
  void HandleSession(SessionEndWork&) noexcept;
  // The graceful close's first act (S4d answer 3, S4e): end every session this loop holds open (reason server_shutdown) and
  // roll back every transaction still in the ownership table, while the database is still serving.
  void EndAllSessionsAtClose() noexcept;
  // The one cross-thread submit of the three transaction requests (defined and only instantiated in the .cpp).
  template <class WorkT, class Request>
  std::future<NativeLoopTransactionResult> SubmitTransaction(Request request);
  // A writer failure (a write or sync failed): the database is now `failed` (reopen), as after a failed Restart.
  void NoteTransactionWriterFailure() noexcept;
  engine::internal_api::NativeNamesSnapshot ReadNamesSnapshotNow() noexcept;  // loop thread only
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
  // S4e Part 4: the cell shared with the published facts (see NativeServedServingStateCell); set when the facts are built.
  std::shared_ptr<engine::internal_api::NativeServedServingStateCell> served_state_cell_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Work> queue_;
  bool accepting_ = true;
  bool joined_ = false;
  ServerProcessCryptoOwner* delegated_crypto_ = nullptr;
  // Created, used and destroyed on the loop thread only.
  std::unique_ptr<Serving> serving_;
  // SB_SNAPSHOT_POINT_VIEW: the snapshot value of the snapshot-point scope that is running its work right now (loop
  // thread only; empty outside a scope). ReadLoginMaterialNow and ReadNamesSnapshotNow pass it to the runtime reads.
  std::optional<transaction::mga::VisibilitySnapshot> scope_view_;
  // SB_NATIVE_NAMES_SOURCE: what the NativeNamesSource handed out in the served facts points at.
  std::shared_ptr<NativeNamesBinding> names_binding_ = std::make_shared<NativeNamesBinding>();
};

}  // namespace scratchbird::server
