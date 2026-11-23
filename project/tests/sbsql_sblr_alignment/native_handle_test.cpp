// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_HANDLE_TEST (thin-slice S3e, plan 38 sections 1 and 2f; owner Q238 = 1, Q240 = 1,
// Q243 = 1, Q285; 2026-11-19). An ENGINE HANDLE on a native database. The server (here the real
// StartHostedEngine, as a server process runs it) opens a natively created file; the loop publishes the
// immutable NativeServedDatabaseFacts; OpenServedEngine takes them and the handle opens: the header snapshot is
// the facts, no .sb.* file is opened or created, no legacy descriptor recovery runs.
//
// Modes (a child process each: the process gate binds after one open):
//   create <path> <page_size>   native create (as native_serving_test)
//   handle <path>               one StartHostedEngine, then every case below; prints "HANDLE_OK checks=N ..."
// With no argument the driver runs, at each of the five page sizes (8K..128K): create, the C ABI refusal
// (sb_engine_open on a native file still refuses DATABASE.OPEN_NATIVE_RUNTIME_PENDING: embedded stays refused
// until S4, owner Q285), and the handle child.
//
// The handle child checks (all inside one served process):
//   * the facts equal what the loop verified (identity, page size, fixed roots, datatype cohort, TIP Next and
//     the six horizons, engine instance, availability table identity, serving state);
//   * OpenServedEngine with the real facts and the served owner succeeds (normal and validation-only modes);
//   * the same call without facts refuses DATABASE.OPEN_NATIVE_RUNTIME_PENDING; forged facts (database,
//     filespace, page size), facts without an owner and an owner paired with another path refuse with specific
//     codes;
//   * a session opens on the native handle (the prepared-statement registry load is the empty, process-local
//     set); [S4f, owner Q298, 2026-12-02: OLD: the receipt refused ENGINE.STATEMENT_CONTEXT.SESSION_REQUIRED_NATIVE_S4
//     whatever the context held. NEW: a native receipt needs a transaction identity (EXACT_TRANSACTION_REQUIRED without one)
//     and a session the loop APPROVED (an engine session nobody approved refuses NATIVE_SESSION_NOT_OPEN); a receipt WITH an
//     approved session is native_receipt_test's. The marker rule is unchanged: the handle's replaces whatever the caller put
//     in its context] and the receipt still checks the context's database against the header snapshot (= the facts);
//   * every executor-availability entry point answers from the per-build native table: nothing is available
//     (the allow-list is empty), so each refuses SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED with structured fields;
//     Set refuses SBLR.EXECUTOR.NATIVE_AVAILABILITY_IMMUTABLE; the pure table logic (a test allow-list) answers
//     installed;
//   * the legacy side-file registries (diagnostic identity, transaction begin authority, catalog object events,
//     prepared-statement publish) refuse ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED;
//   * the directory listing and the file's bytes are unchanged and holds no .sb.* name.
// The strace half is native_handle_strace.py (0 ".sb." syscalls, 0 write-class syscalls on the data descriptor).

#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include "engine/internal_api/sblr_prepared_statement_registry.hpp"
#include "engine/internal_api/sblr_diagnostic_identity_registry.hpp"
#include "engine/internal_api/sblr_transaction_begin_authority.hpp"
#include "engine/internal_api/catalog/catalog_object_lifecycle.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "scratchbird/engine/engine.h"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include "native_database_open_discovery.hpp"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/process_crypto_owner.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/native_database_create_fixture.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace srv = scratchbird::server;
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace fs = std::filesystem;

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::exit(1);
  }
}

std::string_view View(sb_engine_string_view_t value) {
  return {value.data ? value.data : "", static_cast<std::size_t>(value.size_bytes)};
}

// Releases the result (and nulls the caller's pointer so it cannot be released twice); returns the first
// diagnostic's symbolic code ("" when none). Always called as its own statement, never in the argument list of
// the call that produced the result (argument evaluation order is unspecified).
std::string Code(sb_engine_result_t& result) {
  if (result == nullptr) return {};
  sb_engine_diagnostic_set_view_t diagnostics{};
  diagnostics.struct_size = sizeof(diagnostics);
  diagnostics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::string code;
  if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK && diagnostics.diagnostic_count != 0)
    code = std::string(View(diagnostics.diagnostics[0].symbolic_code));
  (void)sb_engine_result_release(result);
  result = nullptr;
  return code;
}

std::map<std::string, std::string> Snapshot(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    std::string value = "dir";
    if (entry.is_regular_file()) {
      std::ifstream in(entry.path(), std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), {});
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          reinterpret_cast<const p::byte*>(bytes.data()), bytes.size());
      Check(digest.ok(), "sha256");
      value = std::to_string(bytes.size()) + ":" + scratchbird::core::hash::HexLower(digest.digest);
    }
    out[entry.path().filename().string()] = value;
  }
  return out;
}

std::string Hex(const p::Uuid& id) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : id.bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 15]);
  }
  return out;
}

int CreateMode(const fs::path& path, std::uint32_t page_size) {
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "native_handle";
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) {
    std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
    return 1;
  }
  return 0;
}

sb_engine_open_params_v1_t Params(const std::string& path, sb_engine_open_mode_t mode) {
  sb_engine_open_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.database_path_utf8 = path.data();
  params.database_path_size = path.size();
  params.mode = mode;
  return params;
}

api::SblrExecutorAvailabilityRowIdentity LiteralRow() {
  api::SblrExecutorAvailabilityRowIdentity row;
  row.executor_id = api::kSblrLiteralExecutorId;
  row.opcode_code = api::kSblrLiteralOpcodeCode;
  row.opcode_version = api::kSblrLiteralOpcodeVersion;
  row.operand_descriptor_id = api::kSblrLiteralOperandDescriptorId;
  row.result_descriptor_id = api::kSblrLiteralResultDescriptorId;
  row.result_descriptor_version = api::kSblrLiteralResultDescriptorVersion;
  return row;
}
api::SblrExecutorAvailabilityRowIdentity ProjectRow() {
  api::SblrExecutorAvailabilityRowIdentity row;
  row.executor_id = api::kSblrProjectExecutorId;
  row.opcode_code = 1280;
  row.opcode_version = "1.0";
  row.operand_descriptor_id = api::kSblrProjectOperandDescriptorId;
  row.result_descriptor_id = api::kSblrProjectResultDescriptorId;
  row.result_descriptor_version = 1;
  return row;
}

bool HasField(const api::EngineApiDiagnostic& diagnostic, std::string_view key, std::string_view value = {}) {
  return std::any_of(diagnostic.fields.begin(), diagnostic.fields.end(), [&](const auto& field) {
    return field.key == key && (value.empty() || field.value == value);
  });
}
bool HasIdentity(const api::EngineApiDiagnostic& diagnostic, std::string_view key, const p::Uuid& id) {
  return std::any_of(diagnostic.identity_fields.begin(), diagnostic.identity_fields.end(), [&](const auto& field) {
    return field.first == key && field.second == id;
  });
}
// A structured native refusal: a specific code, no prose, the expected fields.
void CheckNotImplemented(const api::EngineApiDiagnostic& diagnostic, const api::SblrExecutorAvailabilityRowIdentity& row,
                         const p::Uuid& database, const std::string& what) {
  Check(diagnostic.code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", what + ": specific code, got " + diagnostic.code);
  Check(diagnostic.detail.empty(), what + ": no prose detail");
  Check(HasField(diagnostic, "opcode_code", std::to_string(row.opcode_code)), what + ": opcode_code field");
  Check(HasField(diagnostic, "row_identity_sha256"), what + ": row identity field");
  Check(HasIdentity(diagnostic, "availability_table_uuid", api::NativeExecutorAvailabilityTableUuid()), what + ": table identity (binary)");
  Check(HasIdentity(diagnostic, "database_uuid", database), what + ": database identity (binary)");
}

int HandleMode(const fs::path& path) {
  const auto directory = path.parent_path();
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !scratchbird::core::memory::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() ||
      !crypto.Start(config, policy).ok()) {
    std::fprintf(stderr, "process setup failed: policy=%d\n", policy.ok() ? 1 : 0);
    return 3;
  }
  config.database_default_path = path;
  // After the memory manager is configured (hashing before it would use the default manager and block it).
  const auto before = Snapshot(directory);
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  if (!started.ok()) {
    std::cout << "REFUSED code=" << started.diagnostics.front().code << std::endl;
    return 2;
  }
  const auto& snapshot = started.state.databases.front();
  const std::string path_text = path.string();

  // ---- the facts the loop published ----
  const auto facts = srv::ServedNativeFactsFor(started.state, path_text);
  const auto owner = srv::ServedRouteOwnerFor(started.state, path_text);
  Check(facts != nullptr, "the loop published facts for the served database");
  Check(owner != nullptr, "a served owner exists");
  Check(srv::ServedNativeFactsFor(started.state, (directory / "other.sdb").string()) == nullptr, "no facts for another path");
  Check(facts->database_uuid == snapshot.database_uuid, "facts: database uuid");
  Check(facts->filespace_uuid == snapshot.filespace_uuid, "facts: filespace uuid");
  Check(facts->page_size_bytes == snapshot.page_size_bytes, "facts: page size");
  Check(facts->engine_instance_uuid == snapshot.native_engine_instance_uuid && !facts->engine_instance_uuid.is_nil(), "facts: engine instance");
  Check(!facts->page_profile_uuid.is_nil() && facts->total_pages != 0, "facts: page profile and total pages");
  bool state_root = false, map_root = false;
  for (const auto& root : facts->fixed_roots) {
    state_root = state_root || root.kind == 4;
    map_root = map_root || root.kind == 3;
  }
  Check(state_root && map_root && facts->fixed_roots.size() >= 4, "facts: page zero's fixed roots (state and allocation among them)");
  Check(facts->datatype_cohort_present && !facts->datatype_catalog_snapshot_uuid.is_nil() &&
            facts->datatype_catalog_generation != 0 && facts->datatype_registry_generation != 0,
        "facts: the datatype cohort of the captured database record");
  Check(facts->tip_next_transaction == snapshot.native_restart_next_transaction && facts->tip_next_transaction != 0,
        "facts: TIP next transaction");
  Check(facts->tip_horizon_oit == snapshot.native_horizon_oit && facts->tip_horizon_oat == snapshot.native_horizon_oat &&
            facts->tip_horizon_ost == snapshot.native_horizon_ost && facts->tip_horizon_oct == snapshot.native_horizon_oct &&
            facts->tip_horizon_oar == snapshot.native_horizon_oar && facts->tip_horizon_olr == snapshot.native_horizon_olr,
        "facts: the six horizons");
  Check(facts->availability_table_uuid == api::NativeExecutorAvailabilityTableUuid(), "facts: availability table identity");
  Check(facts->serving_state == api::NativeServedServingState::serving, "facts: serving state");

  // ---- the handle opens (the server's only way) ----
  const auto normal = Params(path_text, SB_ENGINE_OPEN_NORMAL);
  sb_engine_handle_t engine = nullptr;
  sb_engine_result_t result = nullptr;
  auto status = bridge::OpenServedEngine(&normal, owner, "", facts, &engine, &result);
  Check(status == SB_ENGINE_STATUS_OK && engine != nullptr,
        "OpenServedEngine with the real facts succeeds on a native file (normal mode): " + Code(result));
  Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK, "close");
  engine = nullptr;
  const auto validation = Params(path_text, SB_ENGINE_OPEN_VALIDATION_ONLY);
  status = bridge::OpenServedEngine(&validation, owner, "", facts, &engine, &result);
  Check(status == SB_ENGINE_STATUS_OK && engine != nullptr,
        "OpenServedEngine succeeds in the validation-only mode the session registry uses: " + Code(result));

  // ---- refusals, each with its own specific code ----
  sb_engine_handle_t refused = nullptr;
  status = bridge::OpenServedEngine(&normal, owner, "", &refused, &result);
  Check(status == SB_ENGINE_STATUS_UNSUPPORTED && refused == nullptr, "no facts: the open is still refused");
  Check(Code(result) == "DATABASE.OPEN_NATIVE_RUNTIME_PENDING", "no facts: DATABASE.OPEN_NATIVE_RUNTIME_PENDING (embedded stays refused, Q285)");
  const auto forged = [&](auto change) {
    auto copy = std::make_shared<api::NativeServedDatabaseFacts>(*facts);
    change(*copy);
    return std::shared_ptr<const api::NativeServedDatabaseFacts>(copy);
  };
  const auto expect_refused = [&](const std::shared_ptr<const api::NativeServedDatabaseFacts>& marker,
                                  const std::shared_ptr<scratchbird::storage::disk::RouteOwnershipLease>& with_owner,
                                  std::string_view code, const std::string& what, const std::string& open_path = {}) {
    const auto params = Params(open_path.empty() ? path_text : open_path, SB_ENGINE_OPEN_NORMAL);
    sb_engine_handle_t none = nullptr;
    sb_engine_result_t diagnostic = nullptr;
    const auto refusal_status = bridge::OpenServedEngine(&params, with_owner, "", marker, &none, &diagnostic);
    Check(refusal_status != SB_ENGINE_STATUS_OK && none == nullptr, what + ": refused");
    const auto got = Code(diagnostic);
    Check(got == code, what + ": " + std::string(code) + ", got " + got);
  };
  expect_refused(forged([](auto& f) { f.database_uuid.bytes[15] ^= 1; }), owner, "ENGINE.OPEN.NATIVE_FACTS_MISMATCH", "forged database uuid");
  expect_refused(forged([](auto& f) { f.filespace_uuid.bytes[15] ^= 1; }), owner, "ENGINE.OPEN.NATIVE_FACTS_MISMATCH", "forged filespace uuid");
  expect_refused(forged([](auto& f) { f.page_size_bytes *= 2; }), owner, "ENGINE.OPEN.NATIVE_FACTS_MISMATCH", "forged page size");
  expect_refused(facts, nullptr, "ENGINE.OPEN.NATIVE_FACTS_OWNER_REQUIRED", "facts without a served owner");
  expect_refused(facts, owner, "ENGINE.OPEN.NATIVE_OWNER_DEVICE_REFUSED", "an owner paired with another path (FileDevice::OpenServed checks identity)",
                 (directory / "other.sdb").string());
  {
    sb_engine_open_params_v1_t no_path{};
    no_path.struct_size = sizeof(no_path);
    no_path.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    no_path.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    sb_engine_handle_t none = nullptr;
    sb_engine_result_t diagnostic = nullptr;
    status = bridge::OpenServedEngine(&no_path, owner, "", facts, &none, &diagnostic);
    Check(status != SB_ENGINE_STATUS_OK && none == nullptr, "facts without a database path: refused");
    Check(Code(diagnostic) == "ENGINE.OPEN.NATIVE_FACTS_PATH_REQUIRED", "facts without a path: specific code");
  }

  // ---- a session on the native handle; the receipt refuses until S4 ----
  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  const auto user_id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  const auto session_id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(user_id && session_id, "runtime identities issued");
  std::copy(user_id->bytes.begin(), user_id->bytes.end(), session_params.effective_user_uuid.bytes);
  std::copy(session_id->bytes.begin(), session_id->bytes.end(), session_params.session_uuid.bytes);
  session_params.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;
  sb_engine_session_t session = nullptr;
  status = sb_engine_session_begin(engine, &session_params, &session, &result);
  Check(status == SB_ENGINE_STATUS_OK && session != nullptr,
        "a session begins on the native handle (the prepared-statement registry load is empty and process-local): " + Code(result));

  api::EngineRequestContext context;
  context.database_path = path_text;
  context.database_uuid = facts->database_uuid;
  std::copy_n(session_params.effective_user_uuid.bytes, 16, context.principal_uuid.bytes.begin());
  std::copy_n(session_params.session_uuid.bytes, 16, context.session_uuid.bytes.begin());
  context.trust_mode = api::EngineTrustMode::server_isolated;
  const auto receipt_code = [&](const api::EngineRequestContext& ctx, sb_engine_status_t* status_out = nullptr) {
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &ctx;
    bridge::StatementContextReceiptHandle receipt{};
    bridge::StatementContextReceiptView view{};
    sb_engine_result_t diagnostic = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(session, &request, &receipt, &view, &diagnostic);
    if (status_out) *status_out = status;
    return Code(diagnostic);
  };
  sb_engine_status_t receipt_status = SB_ENGINE_STATUS_OK;
  // S4f (owner Q298): OLD (S3e): the receipt on a native handle refused SESSION_REQUIRED_NATIVE_S4 here. NEW: without a
  // transaction identity it refuses EXACT_TRANSACTION_REQUIRED, and with one but no session the loop approved it refuses
  // NATIVE_SESSION_NOT_OPEN (read only: nothing is written; the approved case is native_receipt_test).
  const auto without_transaction = receipt_code(context, &receipt_status);
  Check(without_transaction == "ENGINE.STATEMENT_CONTEXT.EXACT_TRANSACTION_REQUIRED" &&
            receipt_status == SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        "the receipt on a native handle without a transaction identity refuses EXACT_TRANSACTION_REQUIRED, got " + without_transaction);
  const auto unapproved_transaction = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(unapproved_transaction.has_value(), "a transaction identity");
  context.transaction_uuid = *unapproved_transaction;
  context.local_transaction_id = 9;
  const auto receipt_with_transaction = [&](const api::EngineRequestContext& ctx, sb_engine_status_t* status_out = nullptr) {
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &ctx;
    request.exact_transaction_uuid = *unapproved_transaction;
    bridge::StatementContextReceiptHandle receipt{};
    bridge::StatementContextReceiptView view{};
    sb_engine_result_t diagnostic = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(session, &request, &receipt, &view, &diagnostic);
    if (status_out) *status_out = status;
    return Code(diagnostic);
  };
  Check(receipt_with_transaction(context, &receipt_status) == "ENGINE.STATEMENT_CONTEXT.NATIVE_SESSION_NOT_OPEN" &&
            receipt_status == SB_ENGINE_STATUS_SECURITY_DENIED,
        "a session the loop never approved refuses NATIVE_SESSION_NOT_OPEN (was SESSION_REQUIRED_NATIVE_S4)");
  {
    // The marker has one source: the caller's context here carries NO marker (and no transaction), so without
    // the handle's pointer replacing it the receipt would fall through to EXACT_TRANSACTION_REQUIRED.
    Check(context.native_served_facts == nullptr, "the caller's context carries no marker");
    // A caller-supplied (forged) pointer is replaced as well, never trusted.
    auto liar = context;
    liar.native_served_facts = forged([](auto& f) { f.database_uuid.bytes[0] ^= 0xff; f.serving_state = api::NativeServedServingState::failed; });
    Check(receipt_with_transaction(liar) == "ENGINE.STATEMENT_CONTEXT.NATIVE_SESSION_NOT_OPEN",
          "a caller-supplied facts pointer does not change the outcome (was SESSION_REQUIRED_NATIVE_S4; the handle's facts decide: not failed)");
    auto wrong_database = context;
    wrong_database.database_uuid.bytes[15] ^= 1;
    Check(receipt_code(wrong_database) == "ENGINE.STATEMENT_CONTEXT.SESSION_CONTEXT_MISMATCH",
          "the context's database is still checked against the header snapshot (= the facts)");
  }
  // Every other operation that needs a native session / transaction / statement path refuses, specifically.
  {
    sb_engine_transaction_params_v1_t transaction_params{};
    transaction_params.struct_size = sizeof(transaction_params);
    transaction_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_transaction_t transaction = nullptr;
    status = sb_engine_transaction_begin(session, &transaction_params, &transaction, &result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && transaction == nullptr, "transaction_begin on a native session refuses");
    Check(Code(result) == "ENGINE.NATIVE.OPERATION_NOT_BUILT", "transaction_begin: ENGINE.NATIVE.OPERATION_NOT_BUILT");
    sb_engine_request_context_v1_t request_context{};
    request_context.struct_size = sizeof(request_context);
    request_context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_sblr_dispatch_params_v1_t dispatch_params{};
    dispatch_params.struct_size = sizeof(dispatch_params);
    dispatch_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    status = sb_engine_dispatch_sblr(session, nullptr, &request_context, &dispatch_params, &result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED, "dispatch_sblr on a native session refuses");
    Check(Code(result) == "ENGINE.NATIVE.OPERATION_NOT_BUILT", "dispatch_sblr: ENGINE.NATIVE.OPERATION_NOT_BUILT");
    bridge::StatementParameterCoordinationBeginRequestV1 begin{};
    begin.engine_session = session;
    begin.engine_context = &context;
    begin.mode = bridge::StatementParameterExecutionMode::kPrepared;
    begin.operation_uuid = *scratchbird::core::uuid::IssueRuntimeIdentityV7();
    bridge::StatementParameterCoordinationViewV1 coordination{};
    status = bridge::BeginStatementParameterExecutionCoordinationV1(&begin, &coordination, &result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED, "parameter coordination begin refuses on a native session");
    Check(Code(result) == "ENGINE.NATIVE.OPERATION_NOT_BUILT", "parameter coordination begin: ENGINE.NATIVE.OPERATION_NOT_BUILT");
  }
  status = sb_engine_session_end(session, nullptr, &result);
  Check(status == SB_ENGINE_STATUS_OK, "the session ends on the native handle: " + Code(result));
  Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK, "close the handle");

  // ---- the executor availability table: one native table behind every entry point ----
  api::EngineRequestContext native;
  native.database_path = path_text;
  native.database_uuid = facts->database_uuid;
  native.native_served_facts = facts;
  native.statement_uuid = p::Uuid{};
  native.security_context_present = true;
  const auto literal = LiteralRow();
  const auto project = ProjectRow();
  for (const auto& row : {literal, project}) {
    const auto loaded = api::LoadSblrExecutorAvailabilitySnapshot(native, row);
    Check(!loaded.ok, "Load by identity: not available natively");
    CheckNotImplemented(loaded.diagnostic, row, facts->database_uuid, "Load by identity");
    const auto current = api::LoadCurrentSblrExecutorAvailabilitySnapshot(native, row);
    Check(!current.ok, "LoadCurrent: not available natively");
    CheckNotImplemented(current.diagnostic, row, facts->database_uuid, "LoadCurrent");
    api::SblrExecutorAvailabilitySnapshot admitted;
    const auto revalidated = api::RevalidateSblrExecutorAvailability(native, row, admitted, nullptr);
    CheckNotImplemented(revalidated, row, facts->database_uuid, "Revalidate (exact row)");
  }
  {
    const auto no_row = api::LoadSblrExecutorAvailabilitySnapshot(native);
    // (A default row identity IS the literal row, so the no-row forms ask about the literal.)
    Check(!no_row.ok, "Load without a row: not available natively");
    CheckNotImplemented(no_row.diagnostic, literal, facts->database_uuid, "Load without a row (the literal row)");
    const std::vector<api::SblrExecutorAvailabilityRowIdentity> both{literal, project};
    const auto batch = api::LoadSblrExecutorAvailabilitySnapshots(native, both);
    Check(!batch.ok && batch.rows.size() == 2, "batch: one result per row");
    CheckNotImplemented(batch.diagnostic, literal, facts->database_uuid, "batch (first failure)");
    CheckNotImplemented(batch.rows[1].diagnostic, project, facts->database_uuid, "batch (second row)");
    api::SblrExecutorAvailabilitySnapshot admitted;
    const auto revalidated = api::RevalidateSblrExecutorAvailability(native, admitted, nullptr);
    CheckNotImplemented(revalidated, literal, facts->database_uuid, "Revalidate (no row: the literal row)");
    api::SblrExecutorAvailabilitySetRequest set;
    set.database_uuid = facts->database_uuid;
    set.exact_row_identity = literal;
    set.requested_state = api::SblrExecutorAvailabilityState::installed;
    set.reason_code = "test";
    const auto set_result = api::SetSblrExecutorAvailability(native, set);
    Check(!set_result.ok && set_result.diagnostic.code == "SBLR.EXECUTOR.NATIVE_AVAILABILITY_IMMUTABLE" &&
              HasIdentity(set_result.diagnostic, "availability_table_uuid", api::NativeExecutorAvailabilityTableUuid()),
          "Set refuses on native with a specific code");
    Check(api::NativeExecutorAvailabilityAllowList().empty(), "the native allow-list is empty in this build");
    // A context for another database than the facts names is refused, not answered.
    auto crossed = native;
    crossed.database_uuid.bytes[15] ^= 1;
    Check(api::LoadSblrExecutorAvailabilitySnapshot(crossed, literal).diagnostic.code == "SBLR.EXECUTOR.NATIVE_FACTS_DATABASE_MISMATCH",
          "facts naming another database refuse");
    // The pure table logic: an allow-listed row is installed, generation 1, under the table identity.
    const auto listed = api::NativeExecutorAvailabilityLookup(facts->database_uuid, literal, {literal});
    Check(listed.ok && listed.snapshot.installed && listed.snapshot.generation == 1 &&
              listed.snapshot.availability_state == api::SblrExecutorAvailabilityState::installed &&
              listed.snapshot.snapshot_uuid == api::NativeExecutorAvailabilityTableUuid() &&
              listed.snapshot.database_uuid == facts->database_uuid &&
              listed.snapshot.row_identity_sha256 == api::ComputeSblrExecutorAvailabilityRowIdentitySha256(literal),
          "an allow-listed row answers installed from the table (pure logic)");
    Check(!api::NativeExecutorAvailabilityLookup(facts->database_uuid, project, {literal}).ok, "a row not in that allow-list refuses");
  }

  // ---- the legacy registries behind the receipt: process-local or a specific refusal ----
  api::EngineRequestContext registry = native;
  registry.principal_uuid = context.principal_uuid;
  registry.session_uuid = context.session_uuid;
  registry.statement_metadata_snapshot_engine_owned = true;
  registry.trace_tags.push_back("right:SBLR_PREPARED_STATEMENT_REGISTRY_RECOVERY");
  registry.trace_tags.push_back("private_prepared_statement_registry");
  const auto prepared = api::LoadSblrPreparedStatementRegistryV1(registry);
  Check(prepared.ok && !prepared.found && prepared.snapshot.records.empty(), "prepared-statement registry load: the empty process-local set");
  api::SblrPreparedStatementRegistryRecordV1 record;
  const auto published = api::PublishSblrPreparedStatementV1(registry, record);
  Check(!published.ok && published.diagnostic.code == "ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED" &&
            HasField(published.diagnostic, "registry_family", "prepared_statement_registry"),
        "prepared-statement publish refuses on native");
  const auto diagnostic_registry = api::LoadSblrDiagnosticIdentitySnapshotV1(registry);
  Check(!diagnostic_registry.ok && diagnostic_registry.diagnostic.code == "ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED" &&
            HasField(diagnostic_registry.diagnostic, "registry_family", "diagnostic_identity_registry"),
        "diagnostic identity registry refuses on native");
  const auto begin_authority = api::LoadSblrTransactionBeginAuthorityV1(registry);
  Check(!begin_authority.ok && begin_authority.diagnostic.code == "ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED" &&
            HasField(begin_authority.diagnostic, "registry_family", "transaction_begin_authority"),
        "transaction begin authority refuses on native");
  const auto events = api::LoadCatalogObjectLifecycleState(registry);
  Check(!events.ok && events.diagnostic.code == "ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED" &&
            HasField(events.diagnostic, "registry_family", "catalog_object_events"),
        "catalog object events refuse on native");
  const auto epoch = api::LoadCatalogObjectLifecycleEpochState(registry);
  Check(!epoch.ok && epoch.diagnostic.code == "ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED", "catalog epoch state refuses on native");
  // ---- nothing was created, nothing changed ----
  Check(Snapshot(directory) == before, "the directory (names and sha256) is unchanged");
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* name exists");
  std::cout << "HANDLE_OK checks=" << checks << " roots=" << facts->fixed_roots.size() << " page_size=" << facts->page_size_bytes
            << " cohort=" << Hex(facts->datatype_catalog_snapshot_uuid).substr(0, 8) << std::endl;
  return 0;
}

// ---- the driver ----

struct Run {
  int status = 0;
  std::string out;
};
Run RunChild(const std::string& self, const std::vector<std::string>& args) {
  int fds[2];
  Check(::pipe(fds) == 0, "pipe");
  const auto pid = ::fork();
  Check(pid >= 0, "fork");
  if (pid == 0) {
    ::dup2(fds[1], 1);
    ::close(fds[0]);
    ::close(fds[1]);
    std::vector<std::string> storage = args;
    storage.insert(storage.begin(), self);
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    ::execv(self.c_str(), argv.data());
    ::_exit(72);
  }
  ::close(fds[1]);
  Run run;
  char chunk[512];
  ssize_t got;
  while ((got = ::read(fds[0], chunk, sizeof chunk)) > 0) run.out.append(chunk, static_cast<std::size_t>(got));
  ::close(fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  run.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  return run;
}

int Driver(const std::string& self) {
  scratchbird::tests::OwnedTempDirectory temp;
  for (const std::uint32_t page_size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    const auto directory = temp.path() / ("ps" + std::to_string(page_size));
    fs::create_directories(directory);
    const auto path = directory / "handle.sdb";
    const auto created = RunChild(self, {"create", path.string(), std::to_string(page_size)});
    Check(created.status == 0 && fs::exists(path), "create at " + std::to_string(page_size));
    // The C ABI open (no server, no facts) still refuses a valid native file; embedded stays refused until S4.
    {
      const std::string path_text = path.string();  // Params keeps a pointer into it
      const auto params = Params(path_text, SB_ENGINE_OPEN_NORMAL);
      sb_engine_handle_t engine = nullptr;
      sb_engine_result_t result = nullptr;
      const auto status = sb_engine_open(&params, &engine, &result);
      const auto abi_code = Code(result);
      Check(status == SB_ENGINE_STATUS_UNSUPPORTED && engine == nullptr,
            "sb_engine_open on a native file still refuses (status " + std::to_string(static_cast<int>(status)) + ", " + abi_code + ")");
      Check(abi_code == "DATABASE.OPEN_NATIVE_RUNTIME_PENDING", "sb_engine_open: DATABASE.OPEN_NATIVE_RUNTIME_PENDING (Q285)");
    }
    const auto run = RunChild(self, {"handle", path.string()});
    if (run.status != 0 || run.out.rfind("HANDLE_OK", 0) != 0) {
      std::fprintf(stderr, "handle child at %u exited %d: %s\n", page_size, run.status, run.out.c_str());
      return 1;
    }
    std::cout << "page_size=" << page_size << ' ' << run.out;
  }
  temp.Cleanup();
  std::cout << "native_handle: passed (" << checks << " driver checks)" << std::endl;
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string_view(argv[1]) == "create")
      return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
    if (argc == 3 && std::string_view(argv[1]) == "handle") return HandleMode(argv[2]);
    if (argc == 1) return Driver(fs::read_symlink("/proc/self/exe").string());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "exception: %s\n", error.what());
    return 1;
  }
  std::fprintf(stderr, "usage: native_handle_test [create <path> <page_size> | handle <path>]\n");
  return 64;
}
