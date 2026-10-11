// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "session_registry.hpp"
#include "sblr_dispatch_server.hpp"
#include "ipc_server.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "transaction/transaction_api.hpp"
#include "wire/binary_status_packet.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace s = scratchbird::server;
namespace m = scratchbird::core::memory;
namespace u = scratchbird::core::uuid;
using scratchbird::core::platform::Uuid;
namespace bridge = scratchbird::server_engine_bridge;
namespace api = scratchbird::engine::internal_api;
std::string_view mode;
bool inject = true;
unsigned ends = 0, closes = 0;
sb_engine_result_t injected_result = nullptr;
bridge::StatementContextReceiptHandle injected_receipt;
s::ServerSessionRegistry* cancellation_registry = nullptr;
unsigned cancellation_release_calls = 0;
bool cancellation_all_fenced = true;
bool inject_cancellation_reporting = false;
void* operator new(std::size_t bytes) {
  if (inject_cancellation_reporting && cancellation_registry) {
    bool fenced = !cancellation_registry->cursors_by_uuid.empty();
    for (const auto& [_, cursor] : cancellation_registry->cursors_by_uuid)
      fenced &= cursor.closed && cursor.exhausted;
    if (fenced) { inject_cancellation_reporting = false; throw std::bad_alloc(); }
  }
  if (auto* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
extern "C" sb_engine_status_t real_release_receipt(bridge::StatementContextReceiptHandle)
    asm("__real__ZN11scratchbird20server_engine_bridge30ReleaseStatementContextReceiptENS0_29StatementContextReceiptHandleE");
extern "C" sb_engine_status_t wrapped_release_receipt(bridge::StatementContextReceiptHandle)
    asm("__wrap__ZN11scratchbird20server_engine_bridge30ReleaseStatementContextReceiptENS0_29StatementContextReceiptHandleE");
extern "C" sb_engine_status_t wrapped_release_receipt(bridge::StatementContextReceiptHandle receipt) {
  if (inject && injected_receipt && receipt == injected_receipt) {
    inject = false; return SB_ENGINE_STATUS_RESOURCE_EXHAUSTED;
  }
  return real_release_receipt(receipt);
}
extern "C" sb_engine_status_t __real_sb_engine_result_release(sb_engine_result_t);
extern "C" sb_engine_status_t __wrap_sb_engine_result_release(sb_engine_result_t result) {
  if (cancellation_registry && result) {
    ++cancellation_release_calls;
    for (const auto& [_, cursor] : cancellation_registry->cursors_by_uuid)
      cancellation_all_fenced &= cursor.closed && cursor.exhausted;
  }
  if (inject && result && result == injected_result) {
    inject = false;
    if (mode == "cancel_prepared_exception") throw std::bad_alloc();
    return SB_ENGINE_STATUS_RESOURCE_EXHAUSTED;
  }
  return __real_sb_engine_result_release(result);
}
extern "C" sb_engine_status_t __real_sb_engine_close(sb_engine_handle_t, sb_engine_result_t*);
extern "C" sb_engine_status_t __wrap_sb_engine_close(sb_engine_handle_t engine, sb_engine_result_t* out) {
  ++closes;
  if (inject && (mode == "engine" || mode == "authority" || mode == "begin")) {
    inject = false;
    // Fault injection asks the real ABI to generate its complete native
    // invalid-handle vector; the real owned handle is deliberately untouched.
    return __real_sb_engine_close(nullptr, out);
  }
  return __real_sb_engine_close(engine, out);
}
extern "C" sb_engine_status_t __real_sb_engine_session_end(sb_engine_session_t,
    const sb_engine_session_end_params_v1_t*, sb_engine_result_t*);
extern "C" sb_engine_status_t __wrap_sb_engine_session_end(sb_engine_session_t session,
    const sb_engine_session_end_params_v1_t* params, sb_engine_result_t* out) {
  ++ends;
  if (inject && mode == "session") {
    inject = false;
    return __real_sb_engine_session_end(nullptr, params, out);
  }
  return __real_sb_engine_session_end(session, params, out);
}
extern "C" sb_engine_status_t __real_sb_engine_session_begin(sb_engine_handle_t,
    const sb_engine_session_params_v1_t*, sb_engine_session_t*, sb_engine_result_t*);
extern "C" sb_engine_status_t __wrap_sb_engine_session_begin(sb_engine_handle_t engine,
    const sb_engine_session_params_v1_t* params, sb_engine_session_t* session, sb_engine_result_t* out) {
  if (inject && mode == "begin") return __real_sb_engine_session_begin(nullptr, params, session, out);
  return __real_sb_engine_session_begin(engine, params, session, out);
}
void Check(bool ok, const char* reason) {if (!ok) throw std::runtime_error(reason);}
Uuid Id() {auto id = u::IssueRuntimeIdentityV7(); Check(bool(id), "native identity"); return *id;}
void CancelPreparedCustody() {
  s::ServerSessionRegistry registry;
  s::ServerSessionRecord session;
  session.session_uuid = Id().bytes; session.effective_user_uuid = Id().bytes;
  session.connection_uuid = Id().bytes; session.embedded_in_process = true;
  struct Cleanup {
    s::ServerSessionRegistry& registry;
    std::array<std::uint8_t, 16> session;
    ~Cleanup() {
      inject = false; inject_cancellation_reporting = false; cancellation_registry = nullptr;
      if (!s::CloseServerPublicAbiSessionForSession(&registry, session).completed) std::terminate();
    }
  } cleanup{registry, session.session_uuid};
  std::string detail;
  const auto* owner = s::EnsureServerPublicAbiSessionForContext(&registry, session, &detail);
  Check(owner && owner->engine, "real engine owns cancellation result cohort");
  const auto prepared = Id();
  std::array<Uuid, 4> requests, cursors;
  for (unsigned i = 0; i < requests.size(); ++i) {
    requests[i] = Id(); cursors[i] = Id();
    s::ServerCursorRecord cursor;
    cursor.session_uuid = session.session_uuid;
    cursor.cursor_uuid = cursors[i].bytes;
    cursor.prepared_statement_uuid = prepared.bytes;
    if (i < 3) Check(sb_engine_describe_capabilities(owner->engine, nullptr, &cursor.engine_result) == SB_ENGINE_STATUS_OK &&
          cursor.engine_result, "independent real retained result");
    cursor.row_packet = "retained_cancel_payload";
    if (i == 1) injected_result = cursor.engine_result;
    registry.cursors_by_uuid.emplace(cursors[i], std::move(cursor));
    s::ServerRequestRecord request;
    request.request_uuid = requests[i].bytes;
    request.session_uuid = session.session_uuid;
    request.prepared_statement_uuid = prepared.bytes;
    request.cursor_uuid = cursors[i].bytes;
    request.engine_result_retained = i < 3;
    request.operation_id = "query.select";
    request.state = s::ServerRequestLifecycleState::kActive;
    registry.requests_by_uuid.emplace(requests[i], std::move(request));
  }
  // The close command adds another lifecycle record for an existing cursor.
  // Its known request state must not resolve the older execution's unknown
  // outcome, nor cause an implicit second release attempt after a refusal.
  auto alias = registry.requests_by_uuid.at(requests[1]);
  alias.request_uuid = Id().bytes;
  alias.engine_result_retained = false;
  const Uuid alias_key{alias.request_uuid};
  registry.requests_by_uuid.emplace(alias_key, alias);
  const std::string target(reinterpret_cast<const char*>(prepared.bytes.data()), prepared.bytes.size());
  cancellation_registry = &registry;
  if (mode == "cancel_prepared_unauthorized") {
    // Foreign first record and a separate foreign member: map order must not
    // affect either admission or disclosure of private request identities.
    registry.requests_by_uuid.at(requests[1]).session_uuid = Id().bytes;
    const auto first_key = registry.requests_by_uuid.begin()->first;
    registry.requests_by_uuid.at(first_key).session_uuid = Id().bytes;
    const auto refused = s::CancelServerRequestLifecycle(&registry, target, session, false, 1000);
    scratchbird::wire::binary_status::Stream empty;
    empty << "[]";
    Check(refused.error && !refused.accepted && refused.records_json == empty.str() &&
          cancellation_release_calls == 0, "whole selection authorization precedes effects and disclosure");
    Check(refused.diagnostics.size() == 1 && refused.diagnostics.front().identity_fields.size() == 1 &&
          refused.diagnostics.front().identity_fields.front().second == prepared,
          "denial exposes only the supplied target, never a discovered request identity");
    const auto all = s::CancelServerRequestLifecycle(&registry, {}, session, false, 1000);
    Check(all.error && all.records_json == empty.str() && all.diagnostics.size() == 1 &&
          all.diagnostics.front().identity_fields.empty() && cancellation_release_calls == 0,
          "empty-target denial exposes no foreign identities");
    for (const auto& [_, cursor] : registry.cursors_by_uuid)
      Check(!cursor.closed, "authorization refusal has no partial cancellation");
    registry.requests_by_uuid.at(requests[1]).session_uuid = session.session_uuid;
    registry.requests_by_uuid.at(first_key).session_uuid = session.session_uuid;
  }
  if (mode == "cancel_prepared_allocation") { inject = false; inject_cancellation_reporting = true; }
  bool threw = false;
  try {
    const auto cancelled = s::CancelServerRequestLifecycle(&registry, target, session, false, 1000);
    Check(cancelled.accepted && cancelled.unknown_outcome && !cancelled.error,
          "cancellation admitted without claiming unknown finality resolved");
  } catch (const std::bad_alloc&) { threw = true; }
  Check(threw == (mode == "cancel_prepared_exception" || mode == "cancel_prepared_allocation") &&
        !inject && !inject_cancellation_reporting && cancellation_all_fenced,
        "all selected cursors fenced before actual release or exception");
  if (mode == "cancel_prepared_allocation")
    Check(cancellation_release_calls == 0, "reporting allocation fault precedes every native release");
  for (const auto& [_, cursor] : registry.cursors_by_uuid)
    Check(cursor.closed && cursor.exhausted, "every cursor remains fenced after interrupted reporting or release");
  Check(registry.cursors_by_uuid.at(cursors[1]).engine_result == injected_result &&
        registry.requests_by_uuid.at(requests[1]).engine_result_retained &&
        registry.cursors_by_uuid.at(cursors[1]).row_packet == "retained_cancel_payload",
        "failed member retains actual handle, bytes and request accounting");
  if (!threw) {
    Check(cancellation_release_calls == 3, "ordinary refusal does not skip sibling releases");
    for (const unsigned i : {0u, 2u})
      Check(!registry.cursors_by_uuid.at(cursors[i]).engine_result &&
            !registry.requests_by_uuid.at(requests[i]).engine_result_retained,
            "every successful sibling publishes actual release");
  }
  const auto retry = s::CancelServerRequestLifecycle(&registry, target, session, false, 1000);
  Check(retry.accepted && retry.unknown_outcome, "retry preserves unknown finality after physical release");
  for (unsigned i = 0; i < requests.size(); ++i) {
    const auto& cursor = registry.cursors_by_uuid.at(cursors[i]);
    const auto& request = registry.requests_by_uuid.at(requests[i]);
    Check(cursor.closed && cursor.exhausted && !cursor.engine_result && cursor.row_packet.empty() &&
          !request.engine_result_retained && request.state == (i < 3 ? s::ServerRequestLifecycleState::kUnknownOutcome :
                                                                     s::ServerRequestLifecycleState::kCancelled),
          "full retry drains every member without manufacturing transaction success");
    Check(cursor.finality_state == (i < 3 ? "cancelled_unknown_outcome" : "cancelled"),
          "mixed cohort preserves per-cursor finality classification");
  }
  const auto calls = cancellation_release_calls;
  Check(registry.requests_by_uuid.at(alias_key).state == s::ServerRequestLifecycleState::kCancelled &&
        registry.cursors_by_uuid.at(cursors[1]).finality_state == "cancelled_unknown_outcome",
        "duplicate cursor requests retain independent finality and aggregate uncertainty");
  Check(s::CancelServerRequestLifecycle(&registry, target, session, false, 1000).unknown_outcome &&
        cancellation_release_calls == calls, "completed physical retry is idempotent");
  const std::string alias_target(reinterpret_cast<const char*>(alias_key.bytes.data()), alias_key.bytes.size());
  const auto alias_retry = s::CancelServerRequestLifecycle(&registry, alias_target, session, false, 1000);
  Check(alias_retry.accepted && !alias_retry.unknown_outcome && cancellation_release_calls == calls &&
        registry.cursors_by_uuid.at(cursors[1]).finality_state == "cancelled_unknown_outcome" &&
        registry.requests_by_uuid.at(requests[1]).state == s::ServerRequestLifecycleState::kUnknownOutcome,
        "narrow known-request retry cannot resolve prior shared-cursor uncertainty");
}
void ReceiptCustody() {
  namespace db = scratchbird::storage::database;
  namespace fixtures = scratchbird::tests;
  fixtures::OwnedTempDirectory artifacts;
  {
  db::DatabaseCreateConfig create;
  create.path = (artifacts.path()/"receipt.sbdb").string();
  create.database_uuid = u::MakeTypedUuid(scratchbird::core::platform::UuidKind::database, Id()).value;
  create.filespace_uuid = u::MakeTypedUuid(scratchbird::core::platform::UuidKind::filespace, Id()).value;
  create.page_size = 16384;
  fixtures::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  Check(created.ok(), "credentialed real receipt database");
  s::HostedEngineState hosted;
  s::DatabaseOwnershipRequest ownership_request;
  ownership_request.database_path = create.path;
  ownership_request.owner_kind = "server";
  auto ownership = s::AcquireDatabaseOwnership(ownership_request);
  Check(ownership.acquired, "actual database ownership lock");
  hosted.database_ownership_locks.push_back(std::move(ownership.lock));
  s::ServerIpcEndpointOwner endpoint_owner(hosted);
  hosted.database_ownership_locks.clear();
  auto context = fixtures::BootstrapFixtureOwnerContext(create);
  api::EngineBeginTransactionRequest begin;
  begin.context = context; begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  Check(begun.ok, "actual MGA transaction");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  struct Rollback {
    api::EngineRequestContext context;
    ~Rollback() {
      api::EngineRollbackTransactionRequest rollback; rollback.context = context;
      if (!api::EngineRollbackTransaction(rollback).ok) std::terminate();
    }
  } transaction{context};
  auto& registry = endpoint_owner.sessions;
  s::ServerSessionRecord session;
  session.session_uuid = context.session_uuid.bytes;
  session.effective_user_uuid = context.principal_uuid.bytes;
  session.database_path = context.database_path;
  session.database_uuid = context.database_uuid;
  struct Cleanup {
    s::ServerSessionRegistry& registry; std::array<std::uint8_t,16> session;
    ~Cleanup() {inject = false; if (!s::CloseServerPublicAbiSessionForSession(&registry, session).completed) std::terminate();}
  } cleanup{registry, session.session_uuid};
  std::string detail;
  auto* owner = s::EnsureServerPublicAbiSessionForContext(&registry, session, &detail);
  Check(owner && owner->engine_session, "actual credentialed ABI session");
  s::ServerStatementContextRecord record;
  record.session_uuid = session.session_uuid;
  record.statement_uuid = Id();
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context; request.exact_transaction_uuid = context.transaction_uuid;
  // Install the empty owner before acquiring the actual engine receipt.
  auto& pending = registry.statement_contexts_by_statement_uuid.emplace(record.statement_uuid, record).first->second;
  Check(bridge::AcquireStatementContextReceipt(owner->engine_session, &request,
        &pending.receipt, &pending.view, nullptr) == SB_ENGINE_STATUS_OK, "real engine-issued receipt");
  // Rekey the preallocated owner to the actual engine-issued identity. Node
  // extraction/reinsertion does not allocate a replacement receipt owner.
  auto node = registry.statement_contexts_by_statement_uuid.extract(record.statement_uuid);
  node.key() = node.mapped().view.statement_uuid;
  node.mapped().statement_uuid = node.key();
  node.mapped().owning_local_transaction_id = context.local_transaction_id;
  node.mapped().owning_transaction_uuid = context.transaction_uuid;
  auto& stored = registry.statement_contexts_by_statement_uuid.insert(std::move(node)).position->second;
  Check(stored.statement_uuid == stored.view.statement_uuid && !stored.statement_uuid.is_nil(),
        "server owner uses actual engine-issued statement identity");
  injected_receipt = stored.receipt;
  s::ServerPreparedStatementRecord prepared;
  prepared.session_uuid = session.session_uuid; prepared.prepared_statement_uuid = Id().bytes;
  registry.prepared_by_uuid.emplace(Uuid{prepared.prepared_statement_uuid}, prepared);
  s::ServerCursorRecord cursor;
  cursor.cursor_uuid = Id().bytes; cursor.session_uuid = session.session_uuid;
  cursor.prepared_statement_uuid = prepared.prepared_statement_uuid;
  cursor.statement_context_statement_uuid = stored.statement_uuid;
  registry.cursors_by_uuid.emplace(Uuid{cursor.cursor_uuid}, cursor);
  if (mode == "receipt_owner") {
    const auto refused = s::DrainServerIpcEndpoint(endpoint_owner);
    Check(!refused.complete && !refused.sessions_complete && refused.agents_complete && refused.listeners_complete,
          "embedded drain retains actual failed native ownership");
    Check(!s::AcquireDatabaseOwnership(ownership_request).acquired &&
          endpoint_owner.engine_state.database_ownership_locks.front()->valid(),
          "pending embedded owner retains actual database exclusion");
  } else if (mode == "receipt_session") {
    const auto refused = s::CloseServerPublicAbiSessionForSession(&registry, session.session_uuid);
    Check(!refused.completed && refused.stage == s::ServerPublicAbiCloseStage::statement_receipts &&
          refused.status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED && ends == 0 && closes == 0,
          "failed real receipt release prevents native session destruction");
  } else {
    const auto refused = s::CloseServerPreparedStatement(&registry, session.session_uuid, prepared.prepared_statement_uuid);
    Check(!refused.completed && refused.cleanup_status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
          "prepared close retains failed actual receipt");
  }
  Check(!inject && stored.receipt == injected_receipt && stored.released && !stored.release_in_progress &&
        stored.last_release_status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
        "exact receipt retained but server execution revoked");
  api::EngineRequestContext copied;
  Check(bridge::CopyStatementContextEngineContextV1(injected_receipt, &copied, nullptr) == SB_ENGINE_STATUS_OK,
        "injected failure left native receipt alive");
  const auto retry = s::CloseServerPreparedStatement(&registry, session.session_uuid, prepared.prepared_statement_uuid);
  Check(retry.completed && registry.statement_contexts_by_statement_uuid.empty(),
        "successful real receipt retry removes only completed owner");
  Check(bridge::CopyStatementContextEngineContextV1(injected_receipt, &copied, nullptr) == SB_ENGINE_STATUS_INVALID_HANDLE,
        "successful release revokes actual engine receipt");
  Check(s::CloseServerPublicAbiSessionForSession(&registry, session.session_uuid).completed,
        "native session drains only after receipt release");
  Check(s::DrainServerIpcEndpoint(endpoint_owner).complete, "embedded host can retry to actual resource completion");
  }
  artifacts.Cleanup();
}
int main(int argc, char** argv) try {
  mode = argc == 2 ? argv[1] : "engine";
  Check(m::ConfigureDefaultMemoryManagerForFixture(m::DefaultLocalEngineMemoryPolicy(),
        "server_abi_close_custody").ok(), "real fixture manager");
  if (mode == "receipt_session" || mode == "receipt_prepared" || mode == "receipt_owner") {ReceiptCustody(); return 0;}
  if (mode.starts_with("cancel_prepared")) {CancelPreparedCustody(); return 0;}
  s::ServerSessionRegistry registry;
  s::ServerSessionRecord session;
  session.session_uuid = Id().bytes;
  session.effective_user_uuid = Id().bytes;
  session.connection_uuid = Id().bytes;
  session.embedded_in_process = true;
  const Uuid key{session.session_uuid};
  // Every failure path retries while registry ownership is still alive.
  struct Cleanup {
    s::ServerSessionRegistry& registry; std::array<std::uint8_t,16> session;
    ~Cleanup() {
      inject = false;
      if (!s::CloseServerPublicAbiSessionForSession(&registry, session).completed) std::terminate();
    }
  } cleanup{registry, session.session_uuid};
  std::string detail;
  auto invalid_session = session;
  invalid_session.session_uuid = {};
  Check(!s::EnsureServerPublicAbiSessionForContext(&registry, invalid_session, &detail) &&
        detail == "session_identity_invalid" && registry.public_abi_sessions_by_session_uuid.empty() &&
        ends == 0 && closes == 0, "invalid binary identity refused before native acquisition");
  auto* owned = s::EnsureServerPublicAbiSessionForContext(&registry, session, &detail);
  const bool cursor_route = mode == "cursor_route" || mode == "retired_cursor_route" || mode == "cancel_route";
  const bool close_route = mode == "prepared_route" || cursor_route;
  if (mode == "cursor" || mode == "prepared" || close_route) {
    Check(owned && owned->engine_session, "actual session for result custody");
    Check(sb_engine_describe_capabilities(owned->engine, nullptr, &injected_result) == SB_ENGINE_STATUS_OK &&
          injected_result, "real ABI result allocation");
    s::ServerPreparedStatementRecord prepared;
    prepared.session_uuid = session.session_uuid;
    prepared.prepared_statement_uuid = Id().bytes;
    const Uuid prepared_key{prepared.prepared_statement_uuid};
    registry.prepared_by_uuid.emplace(prepared_key, prepared);
    s::ServerCursorRecord cursor;
    cursor.session_uuid = session.session_uuid;
    cursor.cursor_uuid = Id().bytes;
    cursor.prepared_statement_uuid = prepared.prepared_statement_uuid;
    cursor.engine_result = injected_result;
    cursor.row_packet = "retained_result_evidence";
    if (mode == "retired_cursor_route") {
      s::ServerTransactionState transaction;
      transaction.local_transaction_id = 7;
      transaction.transaction_uuid = Id();
      transaction.snapshot_visible_through_local_transaction_id = 6;
      session.transactions_by_local_id.emplace(transaction.local_transaction_id, transaction);
      cursor.owning_local_transaction_id = transaction.local_transaction_id;
      cursor.owning_transaction_uuid = transaction.transaction_uuid;
      cursor.owning_snapshot_visible_through_local_transaction_id = transaction.snapshot_visible_through_local_transaction_id;
    }
    const Uuid cursor_key{cursor.cursor_uuid};
    registry.cursors_by_uuid.emplace(cursor_key, cursor);
    if (mode == "cursor") {
      s::ServerCursorRecord sibling;
      sibling.session_uuid = session.session_uuid;
      sibling.cursor_uuid = Id().bytes;
      registry.cursors_by_uuid.emplace(Uuid{sibling.cursor_uuid}, sibling);
      auto sibling_prepared = prepared;
      sibling_prepared.prepared_statement_uuid = Id().bytes;
      sibling_prepared.prepared_metadata_transferable = true;
      registry.prepared_by_uuid.emplace(Uuid{sibling_prepared.prepared_statement_uuid}, sibling_prepared);
    }
    s::ServerRequestRecord request;
    request.request_uuid = Id().bytes;
    request.session_uuid = session.session_uuid;
    request.cursor_uuid = cursor.cursor_uuid;
    request.state = s::ServerRequestLifecycleState::kActive;
    request.engine_result_retained = true;
    const Uuid request_key{request.request_uuid};
    registry.requests_by_uuid.emplace(request_key, request);
    registry.sessions_by_uuid.emplace(key, session);
    const auto route = [&] {
      s::sbps::Frame frame;
      frame.header.request_uuid = Id().bytes;
      frame.header.session_uuid = session.session_uuid;
      frame.header.connection_uuid = session.connection_uuid;
      if (mode == "prepared_route") {
        frame.header.payload_schema_id = 4013;
        frame.payload = s::EncodeClosePreparedSblrPayloadForTest(session.session_uuid, prepared.prepared_statement_uuid);
        return s::HandleClosePreparedSblr(&registry, frame);
      }
      frame.payload = mode == "cancel_route"
          ? s::EncodeCancelCursorPayloadForTest(session.session_uuid, cursor.cursor_uuid)
          : s::EncodeCloseCursorPayloadForTest(session.session_uuid, cursor.cursor_uuid);
      return s::HandleCloseCursor(&registry, frame);
    };
    if (close_route) {
      const auto refused = route();
      Check(!refused.accepted && (refused.frame_flags & s::sbps::kFlagError) &&
            !refused.diagnostics.empty() && refused.diagnostics.front().code == "SBLR.EXECUTION_FAILED" && !inject,
            "actual server close route refuses injected resource failure");
      bool exact_status = false;
      for (const auto& field : refused.diagnostics.front().fields)
        exact_status |= field.key == "native_cleanup_status" && field.value == std::to_string(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED);
      Check(exact_status, "close refusal carries exact native cleanup status");
    } else if (mode == "prepared") {
      const auto pending = s::CloseServerPreparedStatement(&registry, session.session_uuid, prepared.prepared_statement_uuid);
      Check(pending.found && !pending.completed && pending.engine_results_released == 0 &&
            pending.cleanup_status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED &&
            registry.prepared_by_uuid.at(prepared_key).cleanup_pending,
            "logical prepared retirement is not physical resource completion");
    } else {
      const auto pending = s::CloseServerPublicAbiSessionForSession(&registry, session.session_uuid);
      Check(!pending.completed && pending.stage == s::ServerPublicAbiCloseStage::cursor_results &&
            pending.status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED && ends == 0 && closes == 0,
            "retained cursor prevents session and engine close");
      for (const auto& [_, dependent] : registry.cursors_by_uuid)
        Check(dependent.closed && dependent.exhausted, "all cursors fenced before a release failure");
      for (const auto& [_, dependent] : registry.prepared_by_uuid)
        Check(dependent.closed && dependent.cleanup_pending && !dependent.prepared_metadata_transferable,
              "all prepared statements fenced before a release failure");
    }
    const auto& retained = registry.cursors_by_uuid.at(cursor_key);
    Check(retained.engine_result == injected_result && retained.row_packet == "retained_result_evidence" &&
          registry.requests_by_uuid.at(request_key).engine_result_retained &&
          registry.requests_by_uuid.at(request_key).state == (mode == "cancel_route"
              ? s::ServerRequestLifecycleState::kUnknownOutcome : s::ServerRequestLifecycleState::kActive),
          "failed release retains actual result, request accounting and private payload");
    sb_engine_result_class_t result_class{};
    Check(sb_engine_result_class(injected_result, &result_class) == SB_ENGINE_STATUS_OK,
          "retained native result remains valid until successful retry");
    if (mode == "retired_cursor_route") {
      // Model the server selector's removal after MGA retirement; this is a
      // resource-custody fixture, not evidence of transaction execution.
      registry.sessions_by_uuid.at(key).transactions_by_local_id.clear();
    }
    if (close_route) {
      s::sbps::Frame other;
      other.header.request_uuid = Id().bytes;
      other.header.session_uuid = Id().bytes;
      other.payload = s::EncodeCloseCursorPayloadForTest(other.header.session_uuid, cursor.cursor_uuid);
      Check(!s::HandleCloseCursor(&registry, other).accepted &&
            registry.cursors_by_uuid.at(cursor_key).engine_result == injected_result,
            "pending cleanup remains bound to the original session");
      const auto retried = route();
      Check(retried.accepted && !(retried.frame_flags & s::sbps::kFlagError),
            "actual close route succeeds after exact retained resource release");
    }
    const auto retry = s::CloseServerPreparedStatement(&registry, session.session_uuid, prepared.prepared_statement_uuid);
    Check(retry.completed && retry.engine_results_released == (close_route ? 0u : 1u) &&
          !registry.prepared_by_uuid.at(prepared_key).cleanup_pending &&
          !registry.cursors_by_uuid.at(cursor_key).engine_result &&
          registry.cursors_by_uuid.at(cursor_key).row_packet.empty() &&
          !registry.requests_by_uuid.at(request_key).engine_result_retained,
          "retry publishes completion only after real release");
    if (mode == "cancel_route") {
      Check(registry.requests_by_uuid.at(request_key).state == s::ServerRequestLifecycleState::kUnknownOutcome,
            "physical cleanup cannot turn unknown MGA finality into success");
    }
    Check(s::CloseServerPublicAbiSessionForSession(&registry, session.session_uuid).completed,
          "session drains after native result release");
    return 0;
  }
  if (mode == "begin") {
    Check(!owned && detail == "engine_session_begin_failed", "begin failure remains refused");
    Check(registry.public_abi_sessions_by_session_uuid.size() == 1,
          "failed begin close retains preallocated registry owner");
    owned = &registry.public_abi_sessions_by_session_uuid.at(key);
    Check(owned->engine && !owned->engine_session && owned->closing, "only actual surviving handles retained");
  } else {
    Check(owned && owned->engine && owned->engine_session, "real engine and session opened");
    auto* engine = owned->engine;
    auto* engine_session = owned->engine_session;
    if (mode == "authority") {
      auto changed = session;
      changed.effective_user_uuid = Id().bytes;
      Check(!s::EnsureServerPublicAbiSessionForContext(&registry, changed, &detail) &&
            detail == "public_abi_context_cleanup_pending", "authority change cannot overwrite failed close");
    } else {
      const auto failed = s::CloseServerPublicAbiSessionForSession(&registry, session.session_uuid);
      Check(failed.found && !failed.completed && failed.status == SB_ENGINE_STATUS_INVALID_HANDLE,
            "exact native failure status returned, never a success count");
    }
    owned = &registry.public_abi_sessions_by_session_uuid.at(key);
    Check(owned->engine == engine && owned->closing, "same engine remains in revoked custody");
    Check(owned->engine_session == (mode == "session" ? engine_session : nullptr),
          "session is cleared only after actual successful end");
    Check(closes == (mode == "session" ? 0u : 1u), "failed session end must not close its engine");
  }
  Check(owned->close_diagnostics && owned->last_close.status == SB_ENGINE_STATUS_INVALID_HANDLE,
        "retain real engine diagnostic result with exact native failure");
  scratchbird::server_engine_bridge::EngineDiagnosticSnapshot snapshot;
  Check(scratchbird::server_engine_bridge::CopyEngineDiagnosticSnapshot(
        owned->close_diagnostics.get(), 0, &snapshot) && snapshot.code == "ENGINE.ABI.INVALID_HANDLE" &&
        u::IsEngineIdentityUuid(Uuid{snapshot.occurrence_uuid}), "complete binary native diagnostic preserved");
  const auto close_count = closes, end_count = ends;
  Check(!s::EnsureServerPublicAbiSessionForContext(&registry, session, &detail) &&
        detail == "public_abi_context_cleanup_pending" && closes == close_count && ends == end_count,
        "revoked cache cannot be reused or silently replaced");
  const auto retried = s::CloseServerPublicAbiSessionForSession(&registry, session.session_uuid);
  Check(retried.found && retried.completed && registry.public_abi_sessions_by_session_uuid.empty(),
        "real retry releases exactly the retained handles");
  Check(ends == (mode == "session" ? 2u : mode == "begin" ? 0u : 1u),
        "already-ended session is never ended twice");
  Check(s::CloseServerPublicAbiSessionForSession(&registry, session.session_uuid).completed,
        "empty retry is idempotent");
  return 0;
} catch (const std::exception& error) {std::cerr << error.what() << '\n'; return 1;}
