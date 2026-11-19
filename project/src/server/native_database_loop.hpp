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
//   serving    verified and serving-ready; the writer is adopted; Restart (the
//              TIP) has NOT run (S3d), so nothing may write yet
//   failed     the open or its verification refused; resources already closed
//              on this thread, the cause reported
//   closing/closed  a submitted close request tore everything down on this
//              thread; the thread is then joined by Close()
//
// Requests: a bounded queue (a mutex and a condition variable; the thread
// sleeps on the variable, nothing polls). Kinds today: open, close. Plan 36
// adds complete_and_begin (the atomic commit + replacement begin); the request
// variant below is where it goes.
//
// Ownership: the HostedDatabaseRuntime holds the loop by shared_ptr; the loop
// never holds a shared_ptr to itself (the thread function holds a raw pointer
// and the object outlives its thread because the destructor closes and joins).
// Close() runs the close request, joins the thread and only then returns, so
// the database ownership (the flock) is never released before the loop thread
// has retired every resource that depends on it.

#include "database_ownership.hpp"
#include "native_hosted_database_runtime.hpp"
#include "process_crypto_owner.hpp"
#include "uuid.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
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

// Immutable facts about the verified, serving-ready database. Copied values
// only; no custody, no readiness authority.
struct NativeLoopServingFacts {
  core::platform::Uuid engine_instance_uuid;
  core::platform::Uuid service_principal_uuid;   // L5's principal, from the file
  core::platform::Uuid runtime_binding_uuid;
  std::uint64_t transactions = 0, active_transactions = 0, in_doubt_transactions = 0;
  std::uint64_t filespaces = 0, catalog_pages = 0, wasted_pages = 0;
  std::uint64_t verification_workspace_bytes = 0;
  // Restart (the TIP) is S3d: this open ends before it.
  bool restart_pending = true;
};

struct NativeLoopOpenResult {
  bool ok = false;
  NativeLoopState state = NativeLoopState::failed;
  NativeLoopDiagnostic diagnostic;  // when !ok
  NativeLoopServingFacts facts;     // when ok
};

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

  // Queue a close: tear down on the loop thread (read, runtime, credential),
  // then leave the loop. Idempotent.
  std::future<NativeLoopCloseResult> SubmitClose();

  // SubmitClose, wait, join the thread and retire the crypto delegation.
  // Callable from any thread except the loop thread itself (refused).
  NativeLoopCloseResult Close() noexcept;

  NativeLoopState state() const noexcept { return state_.load(std::memory_order_acquire); }
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
  // plan 36: struct CompleteAndBeginWork { ... } joins this variant.
  using Work = std::variant<OpenWork, CloseWork>;
  static constexpr std::size_t kQueueCapacity = 8;

  void Run() noexcept;
  void HandleOpen(OpenWork&) noexcept;
  NativeLoopCloseResult HandleClose() noexcept;
  struct Serving;  // loop-thread-only state

  std::thread thread_;
  std::thread::id thread_id_;
  std::atomic<NativeLoopState> state_{NativeLoopState::opening};
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
