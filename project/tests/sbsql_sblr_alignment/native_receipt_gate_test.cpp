// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_RECEIPT_GATE_TEST (S4f exposure close; owner 2026-10-19 / 2026-11-16: the database is ONE file, "no other
// files should be created", legacy side files are drift). A NATIVE statement-context receipt exists since S4f, and about 65
// receipt-consuming entry points of the server-engine bridge reach legacy ".sb.*" side-file families. The engine refuses a native
// receipt at ONE gate (native_receipt_consumer_gate in engine/public_abi.cpp: an explicit allow-list, today only what works natively).
// This test calls EVERY gated entry point on a native session's receipt (production-created database, the real create path, the
// real HandleAuthHandoff session and transaction as the S4f native_receipt test does) with a default-constructed request (the gate
// runs BEFORE request validation, so no request needs to be valid) and asserts, per entry point:
//   * the exact code ENGINE.NATIVE.OPERATION_NOT_BUILT with the diagnostic fields operation = the entry point and requires = the
//     family's token (read through the bridge's private diagnostic snapshot);
//   * the data file's bytes are identical before and after (so its Next is unchanged: also read offline around the whole table);
//   * the directory holds exactly the data file before and after (no file, no directory entry) and no file named ".sb." exists
//     anywhere under the test's temp tree.
// Allow-listed entries still work: the receipt's acquire, the copy of its context and its release; Dispatch and the name-resolve
// bind keep their OWN gate (the gate passes them through: a default request reaches their own validation / refusal), and the
// parameter-coordination begin, the C ABI's dispatch and transaction begin keep theirs. A session (not a receipt) reaching the
// prepared-metadata binding refuses at the same gate. The list of entry points is the table below (kept in step with
// statement_context.hpp by native_receipt_gate_coverage_test.py, which fails for a consumer that is not in it).
// Modes (a child process each):
//   serve <path> <state> <database_hex> <bootstrap_hex> <service_hex>   states: gate trace
//        prints "GATE_OK checks=N"; `trace` marks regions on stderr for native_receipt_gate_strace.py
// With no argument the driver runs everything; "size <bytes>" one page size.

#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/native_login_material.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "engine/internal_api/sblr_name_resolve_journal.hpp"
#include "security/authentication_api.hpp"
#include "catalog_language.hpp"
#include "catalog_records.hpp"
#include "catalog_authorization_policy.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_native_security_record.hpp"
#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "hash_digest.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "native_catalog_version_writer.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "filespace_page_zero.hpp"
#include "uuid.hpp"
#include "scratchbird/engine/engine.h"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/login_throttle.hpp"
#include "server/native_database_loop.hpp"
#include "server/process_crypto_owner.hpp"
#include "engine/sblr/sblr_session_setting_set_runtime.hpp"
#include "server_engine_bridge/diagnostic_fields.hpp"
#include "server_engine_bridge/prepared_metadata_binding.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "session_registry.hpp"
#include "ipc_server.hpp"
#include "wire/parser_server_ipc/sbps_statement_management_bind_codec.hpp"
#include "../support/catalog_change_event_fixture.hpp"
#include "../support/native_offline_file_reader.hpp"
#include "../support/owned_temp_directory.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace db = scratchbird::storage::database;
namespace pgs = scratchbird::storage::page;
namespace srv = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace bind = scratchbird::wire::sbps_statement_management;
namespace mem = scratchbird::core::memory;
namespace cat = scratchbird::core::catalog;
namespace off = scratchbird::tests::offline;
namespace fs = std::filesystem;

constexpr std::string_view kBootstrapPassword = "S4a-Bootstrap-Pw-7f3a91c2d8";
constexpr std::string_view kBootstrapName = "fixture_owner";

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::exit(1);
  }
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
p::Uuid FromHex(std::string_view hex) {
  p::Uuid id;
  Check(hex.size() == 32, "a 32-digit hex identity");
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<p::byte>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  return id;
}
p::Uuid NewId() {
  const auto id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(id.has_value(), "an identity");
  return *id;
}
void Mark(const std::string& name) {
  const std::string line = "MARK " + name + "\n";
  (void)!::write(2, line.data(), line.size());
}
std::vector<p::byte> Image(const fs::path& path) { return off::ReadAll(path); }

std::string_view View(sb_engine_string_view_t value) {
  return {value.data ? value.data : "", static_cast<std::size_t>(value.size_bytes)};
}
// Releases the result and returns the first diagnostic's symbolic code ("" when none).
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

// ---- the handoff (the same frames the server's attach path runs) ---------------------------------------------------
void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}
void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}
std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid, std::string_view name, std::string_view password) {
  std::vector<std::uint8_t> out(connection_uuid.begin(), connection_uuid.end());
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, name);
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, password);
  PutString(&out, "native_receipt_gate_test");
  PutString(&out, "");
  return out;
}
sbps::Frame MakeFrame(sbps::MessageType type, std::vector<std::uint8_t> payload, const std::array<std::uint8_t, 16>& connection) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection;
  frame.header.payload_schema_id = 3001;
  frame.payload = std::move(payload);
  return frame;
}

struct Served {
  srv::HostedEngineResult started;
  srv::NativeDatabaseLoop* loop = nullptr;
  std::shared_ptr<const api::NativeServedDatabaseFacts> facts;
  fs::path path, directory;
  p::Uuid database, bootstrap, service;
} served;

struct Approved {
  std::unique_ptr<srv::ServerSessionRegistry> registry = std::make_unique<srv::ServerSessionRegistry>();
  srv::SessionOperationResult result;
  std::array<std::uint8_t, 16> connection{};
  srv::ServerNativeApproval approval;
  const srv::ServerSessionRecord* record = nullptr;
  p::Uuid session() const { return p::Uuid{approval.session_uuid}; }
};
std::unique_ptr<Approved> Approve() {
  auto out = std::make_unique<Approved>();
  out->connection = sbps::MakeUuidV7Bytes();
  const auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(out->connection, kBootstrapName, kBootstrapPassword), out->connection);
  srv::NativeLoginThrottle().Clear();
  out->result = srv::HandleAuthHandoff(out->registry.get(), served.started.state, frame);
  Check(out->result.accepted && out->result.native_approval.present, "the login is approved by the real handoff");
  out->approval = out->result.native_approval;
  const auto found = out->registry->sessions_by_uuid.find(p::Uuid{out->approval.session_uuid});
  Check(found != out->registry->sessions_by_uuid.end(), "the session is recorded in the registry");
  out->record = &found->second;
  const auto& rec = *out->record;
  Check(rec.native_session_approved && rec.native_served_facts == served.facts && rec.served_route_owner != nullptr &&
            rec.local_transaction_id == out->approval.transaction_order && rec.transaction_uuid == out->approval.transaction_uuid,
        "the record carries the facts, the served owner and the transaction the loop began");
  return out;
}
void Disconnect(Approved& a) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = a.connection;
  frame.header.session_uuid = a.approval.session_uuid;
  frame.payload.assign(a.approval.session_uuid.begin(), a.approval.session_uuid.end());
  PutString(&frame.payload, "parser_disconnect_notice");
  (void)srv::HandleDisconnectNotice(a.registry.get(), frame);
}

// ---- the public-ABI engine session: as the server's EnsureServerPublicAbiSessionForContext makes it ---------------------
struct PublicSession {
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  PublicSession() = default;
  PublicSession(const PublicSession&) = delete;
  PublicSession& operator=(const PublicSession&) = delete;
  ~PublicSession() {
    sb_engine_result_t ignored = nullptr;
    if (session) {
      sb_engine_session_end_params_v1_t end{};
      end.struct_size = sizeof(end);
      end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      end.rollback_active_transactions = 1;  // S4g part 1: a native receipt now publishes the TXBH transaction handle, as the server's own session end does
      (void)sb_engine_session_end(session, &end, &ignored);
      if (ignored) (void)sb_engine_result_release(ignored);
    }
    ignored = nullptr;
    if (engine) {
      (void)sb_engine_close(engine, &ignored);
      if (ignored) (void)sb_engine_result_release(ignored);
    }
  }
};
std::unique_ptr<PublicSession> OpenPublic(const std::shared_ptr<d::RouteOwnershipLease>& owner, const std::string& path_text,
                                          const std::shared_ptr<const api::NativeServedDatabaseFacts>& facts,
                                          const std::array<std::uint8_t, 16>& user, const std::array<std::uint8_t, 16>& session_uuid) {
  auto out = std::make_unique<PublicSession>();
  sb_engine_open_params_v1_t open{};
  open.struct_size = sizeof(open);
  open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open.database_path_utf8 = path_text.data();
  open.database_path_size = path_text.size();
  open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_result_t result = nullptr;
  auto status = bridge::OpenServedEngine(&open, owner, SB_DEFAULT_POLICY_PACK_ROOT, facts, &out->engine, &result);
  Check(status == SB_ENGINE_STATUS_OK && out->engine != nullptr, "the engine handle opens on the served native database: " + Code(result));
  sb_engine_session_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::memcpy(params.effective_user_uuid.bytes, user.data(), 16);
  std::memcpy(params.session_uuid.bytes, session_uuid.data(), 16);
  params.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  params.default_language_utf8 = "en";
  params.default_language_size = 2;
  result = nullptr;
  status = sb_engine_session_begin(out->engine, &params, &out->session, &result);
  Check(status == SB_ENGINE_STATUS_OK && out->session != nullptr, "the engine session begins: " + Code(result));
  return out;
}
std::unique_ptr<PublicSession> OpenPublicFor(const Approved& a) {
  const auto& rec = *a.record;
  return OpenPublic(rec.served_route_owner, rec.database_path, rec.native_served_facts, rec.effective_user_uuid, rec.session_uuid);
}

// The context the server materializes for a native session (EngineContextForSessionProjection), as values: the identities of
// the session record and the generation the caller claims the catalog is at.
api::EngineRequestContext ContextFor(const std::string& path_text, const std::array<std::uint8_t, 16>& user,
                                     const std::array<std::uint8_t, 16>& session_uuid, const p::Uuid& transaction,
                                     std::uint64_t order, std::uint64_t generation) {
  api::EngineRequestContext context;
  context.database_path = path_text;
  context.database_uuid = served.facts->database_uuid;
  context.principal_uuid = api::EngineUuid{user};
  context.session_uuid = api::EngineUuid{session_uuid};
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.transaction_uuid = transaction;
  context.local_transaction_id = order;
  context.security_context_present = true;
  context.catalog_generation_id = generation;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid = NewId();
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = 1;
  context.authorization_context.policy_epoch = 1;
  context.authorization_context.catalog_generation_id = generation;
  context.current_package_uuid = NewId();
  return context;
}
api::EngineRequestContext ContextFor(const Approved& a, std::uint64_t generation) {
  const auto& rec = *a.record;
  return ContextFor(rec.database_path, rec.effective_user_uuid, rec.session_uuid, a.approval.transaction_uuid, a.approval.transaction_order, generation);
}

struct Acquired {
  sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
  std::string code;
  bridge::StatementContextReceiptHandle receipt{};
  bridge::StatementContextReceiptView view{};
  bool ok() const { return status == SB_ENGINE_STATUS_OK; }
};
// One acquire. The file's bytes are identical before and after it (the receipt is read only), success or refusal.
// `compare_bytes` false is for the strace state only: the test's own reads of the data path must not sit inside the traced
// region (they would hide an engine-side open of the data path); it compares once around the whole region instead.
Acquired Acquire(PublicSession& ps, const api::EngineRequestContext& context, const p::Uuid& transaction, bool compare_bytes = true) {
  const auto before = compare_bytes ? Image(served.path) : std::vector<p::byte>{};
  Acquired out;
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = transaction;
  sb_engine_result_t result = nullptr;
  out.status = bridge::AcquireStatementContextReceipt(ps.session, &request, &out.receipt, &out.view, &result);
  out.code = Code(result);
  if (compare_bytes)
    Check(Image(served.path) == before, "the acquire (" + (out.ok() ? std::string("ok") : out.code) + ") wrote nothing: the file's bytes are identical");
  return out;
}
void Release(Acquired& a) {
  if (a.receipt) Check(bridge::ReleaseStatementContextReceipt(a.receipt) == SB_ENGINE_STATUS_OK, "the receipt releases");
  a.receipt = {};
}
api::EngineRequestContext CopyContext(const Acquired& a) {
  api::EngineRequestContext copy;
  sb_engine_result_t result = nullptr;
  Check(bridge::CopyStatementContextEngineContextV1(a.receipt, &copy, &result) == SB_ENGINE_STATUS_OK, "the receipt's context copies: " + Code(result));
  return copy;
}

// ---- the offline ground truth ------------------------------------------------------------------------------------------
struct BoundMemory {
  mem::MemoryManager manager;
  mem::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding;
  db::NativeStorageMemory memory;
  static auto Policy(std::uint64_t bytes) {
    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = policy.per_context_limit_bytes = 4 * bytes;
    policy.soft_limit_bytes = 0;
    return policy;
  }
  BoundMemory(const p::Uuid& database, std::uint64_t bytes) : manager(Policy(bytes)), binding{database, NewId(), NewId(), NewId()} {
    mem::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = mem::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = mem::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "native receipt test read";
    r.purpose = "the test's own offline names read";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{mem::HierarchicalMemoryScopeKind::process, {}, NewId().bytes},
                     {mem::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "native receipt test";
    for (const auto& scope : r.scope_chain) {
      mem::HierarchicalMemoryBudget b;
      b.scope = scope;
      b.hard_limit_bytes = bytes;
      b.provenance = r.provenance;
      Check(ledger.SetBudget(b).ok(), "offline read parent budget");
    }
    auto grant = mem::AcquireReservationBackedMemoryResource(r);
    Check(grant.ok(), "offline read memory grant");
    auto adopted = db::AdoptNativeStorageMemory(binding, grant.resource);
    Check(adopted.ok(), "offline read grant adoption");
    memory = std::move(adopted.memory);
  }
};
struct NameFact {
  p::Uuid object, scope, language;
  std::string text;
  std::optional<cat::CatalogNameEntry> entry;
  std::optional<cat::CatalogMetadataVersion> metadata;
};
struct DefinitionFact {
  p::Uuid object, row_uuid, version_uuid;
  cat::CatalogMetadataVersion metadata;
};
struct Offline {
  p::Uuid database, filespace;
  std::uint32_t page_size = 0;
  std::map<p::Uuid, DefinitionFact> security;  // role 4 rows by object
  std::vector<NameFact> names;  // live entries only
  std::vector<d::FilespaceRootReference> roots;
  std::uint64_t catalog_generation = 0;  // the maximum over the live entries (what the names source reports)
};
Offline ReadOffline(const fs::path& path) {
  Offline out;
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "the offline read opens the file read-only");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  out.page_size = size;
  out.database = zero.record->bootstrap.database_uuid;
  out.filespace = zero.record->bootstrap.filespace_uuid;
  out.roots = zero.record->roots;
  const auto profile = zero.record->bootstrap.page_size_profile_uuid;
  const auto budget = db::NativeHostedSourceBackingBytes(size);
  const db::NativeCatalogRelationMemoryLimits limits{profile, 1, db::NativeCatalogScanBasePages(size), db::kNativeCatalogScanRelationMaximumRows,
      db::kNativeCatalogScanRelationMaximumRows, db::NativeCatalogScanNavigationLimits(size),
      db::NativeNameTreeNavigationLimits(size, db::kNativeCatalogScanRelationMaximumRows)};
  BoundMemory workspace_memory(out.database, db::NativeCatalogVisibilityWorkspaceBytes(limits)), source_memory(out.database, 32ull * 1024 * 1024);
  auto prepared = db::PrepareNativeCatalogVisibilityRead(limits, workspace_memory.memory, workspace_memory.binding);
  Check(prepared.ok(), "the visibility workspace prepares");
  const std::array<d::NativeFilespaceDevice, 1> devices{{{out.filespace, profile, &device}}};
  auto source = db::AcquireNativeFixedRootMemoryLease(out.database, devices, out.filespace, {budget, 32ull * 1024 * 1024},
                                                      source_memory.memory, source_memory.binding);
  Check(source.ok(), "a fixed-root lease over the file");
  const auto& lease = *source.lease;
  {
    const auto security = db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(lease, 2, 4, budget, prepared.workspace);
    Check(security.ok(), "the security relation reads");
    for (const auto& row : security.rows) {
      if (row.metadata.record.header.deleted) continue;
      DefinitionFact f;
      f.object = row.metadata.record.header.object_uuid.value;
      f.row_uuid = row.metadata.record.header.row_uuid.value;
      f.version_uuid = row.version_uuid;
      f.metadata = cat::MaterializeCatalogMetadataVersion(row.metadata);
      out.security.emplace(f.object, std::move(f));
    }
  }
  const auto names = db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(lease, 2, 2, budget, prepared.workspace);
  Check(names.ok(), "the names relation reads");
  for (const auto& row : names.rows) {
    if (!row.name_payload || row.metadata.record.header.deleted || row.provisional) continue;
    const auto* e = std::get_if<cat::CatalogNameEntryView>(&*row.name_payload);
    if (!e) continue;
    NameFact f;
    f.object = e->object_uuid.value;
    f.scope = e->scope_uuid.value;
    f.language = e->language_uuid.value;
    f.text = std::string(e->raw_name_text);
    f.entry = cat::MaterializeCatalogNameEntry(*e);
    f.metadata = cat::MaterializeCatalogMetadataVersion(row.metadata);
    out.catalog_generation = std::max<std::uint64_t>(out.catalog_generation, f.entry->catalog_generation_id);
    out.names.push_back(std::move(f));
  }
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return out;
}

// ---- the gate ---------------------------------------------------------------------------------------------------------------
struct Probe {
  sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
  std::string code, operation, requires_token;
};
// Reads the first diagnostic (code and the operation / requires fields, through the bridge's private snapshot) and releases the result.
Probe Observe(sb_engine_status_t status, sb_engine_result_t& result) {
  Probe out;
  out.status = status;
  if (result != nullptr) {
    bridge::EngineDiagnosticSnapshot snapshot;
    if (bridge::CopyEngineDiagnosticSnapshot(result, 0, &snapshot)) {
      out.code = snapshot.code;
      for (const auto& field : snapshot.fields) {
        if (field.key == "operation") out.operation = field.value;
        if (field.key == "requires") out.requires_token = field.value;
      }
    }
    (void)sb_engine_result_release(result);
    result = nullptr;
  }
  return out;
}

struct Gated {
  const char* name;
  const char* family;
  const char* requires_token;
  std::function<sb_engine_status_t(bridge::StatementContextReceiptHandle, sb_engine_result_t*)> call;
};
// EVERY gated receipt consumer of statement_context.hpp (generated once from the declarations; the source-policy test
// native_receipt_gate_coverage_test.py fails for a declared consumer that is missing here or from the engine's table).
// The family and token columns are this test's own pin of the engine's allow-list data.
const std::vector<Gated>& GatedTable() {
  using namespace scratchbird::server_engine_bridge;
  static const std::vector<Gated> table = {
    {"CoordinateSessionValueDescriptorV1", "session_value_descriptor", "native_parameter_and_variable_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::array<std::uint8_t, 16> a1{}; scratchbird::engine::internal_api::SessionValueOperation a2{}; scratchbird::engine::sblr::SblrSessionSettingSetRequestV1 a3{}; scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1 a4{}; return bridge::CoordinateSessionValueDescriptorV1(receipt, a1, a2, &a3, &a4, out_result); }},
    {"AttachStatementRelationOccurrenceMappingsV1", "relation_occurrence_mappings", "native_statement_occurrence_mappings", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<StatementRelationOccurrenceMappingV1> a1{}; return bridge::AttachStatementRelationOccurrenceMappingsV1(receipt, a1, out_result); }},
    {"CopyStatementRelationOccurrenceMappingsV1", "relation_occurrence_mappings", "native_statement_occurrence_mappings", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<StatementRelationOccurrenceMappingV1> a1{}; return bridge::CopyStatementRelationOccurrenceMappingsV1(receipt, &a1, out_result); }},
    {"BindStatementTableTruncateTargetV1", "table_truncate", "native_relation_store", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementTableTruncateBindRequestV1 a1{}; StatementTableTruncateBindAckV1 a2{}; return bridge::BindStatementTableTruncateTargetV1(receipt, &a1, &a2, out_result); }},
    {"BindStatementBulkImportAuthorityV1", "bulk_import", "native_bulk_import_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementBulkImportBindRequestV1 a1{}; StatementBulkImportBindAckV1 a2{}; return bridge::BindStatementBulkImportAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementBulkImportAuthorityV1", "bulk_import", "native_bulk_import_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementBulkImportAuthorityV1 a3{}; return bridge::CopyStatementBulkImportAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementPrepareAuthorityV1", "prepared_statement", "native_prepared_statement_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementPrepareBindRequestV1 a1{}; StatementPrepareBindAckV1 a2{}; return bridge::BindStatementPrepareAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementPrepareAuthorityV1", "prepared_statement", "native_prepared_statement_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementPrepareAuthorityV1 a2{}; return bridge::CopyStatementPrepareAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementExecuteAuthorityV1", "prepared_statement", "native_prepared_statement_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementExecuteBindRequestV1 a1{}; std::vector<std::uint8_t> a2{}; return bridge::BindStatementExecuteAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementExecuteAuthorityV1", "prepared_statement", "native_prepared_statement_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementExecuteAuthorityV1 a2{}; return bridge::CopyStatementExecuteAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementFreeAuthorityV1", "prepared_statement", "native_prepared_statement_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementFreeBindRequestV1 a1{}; StatementFreeBindAckV1 a2{}; return bridge::BindStatementFreeAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementFreeAuthorityV1", "prepared_statement", "native_prepared_statement_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementFreeAuthorityV1 a2{}; return bridge::CopyStatementFreeAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementExecuteDirectAuthorityV1", "execute_direct", "native_executor_availability_s4g", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementExecuteDirectBindRequestV1 a1{}; StatementExecuteDirectBindAckV1 a2{}; return bridge::BindStatementExecuteDirectAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementExecuteDirectAuthorityV1", "execute_direct", "native_executor_availability_s4g", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementExecuteDirectAuthorityV1 a2{}; return bridge::CopyStatementExecuteDirectAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementQueryExplainAuthorityV1", "query_explain", "native_explain_message_vectors", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementQueryExplainBindRequestV1 a1{}; StatementQueryExplainBindAckV1 a2{}; return bridge::BindStatementQueryExplainAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementQueryExplainAuthorityV1", "query_explain", "native_explain_message_vectors", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementQueryExplainAuthorityV1 a2{}; return bridge::CopyStatementQueryExplainAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementDdlCreateSchemaAuthorityV1", "ddl_create_schema", "native_ddl_execution_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlCreateSchemaBindRequestV1 a1{}; StatementDdlCreateSchemaAuthorityV1 a2{}; return bridge::BindStatementDdlCreateSchemaAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementDdlCreateSchemaAuthorityV1", "ddl_create_schema", "native_ddl_execution_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlCreateSchemaAuthorityV1 a3{}; return bridge::CopyStatementDdlCreateSchemaAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementSecurityAlterPolicyAuthorityV1", "security_policy", "native_security_catalog_writes", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementSecurityAlterPolicyBindRequestV1 a1{}; StatementSecurityAlterPolicyAuthorityV1 a2{}; return bridge::BindStatementSecurityAlterPolicyAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementSecurityAlterPolicyAuthorityV1", "security_policy", "native_security_catalog_writes", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementSecurityAlterPolicyAuthorityV1 a3{}; return bridge::CopyStatementSecurityAlterPolicyAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementSecurityCreatePrivilegeTemplateAuthorityV1", "security_policy", "native_security_catalog_writes", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementSecurityCreatePrivilegeTemplateBindRequestV1 a1{}; StatementSecurityCreatePrivilegeTemplateAuthorityV1 a2{}; return bridge::BindStatementSecurityCreatePrivilegeTemplateAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementSecurityCreatePrivilegeTemplateAuthorityV1", "security_policy", "native_security_catalog_writes", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementSecurityCreatePrivilegeTemplateAuthorityV1 a3{}; return bridge::CopyStatementSecurityCreatePrivilegeTemplateAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementDdlCreateTriggerAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlCreateTriggerBindRequestV1 a1{}; StatementDdlCreateTriggerAuthorityV1 a2{}; return bridge::BindStatementDdlCreateTriggerAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementDdlCreateTriggerAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlCreateTriggerAuthorityV1 a3{}; return bridge::CopyStatementDdlCreateTriggerAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementDdlAlterTriggerAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlAlterTriggerBindRequestV1 a1{}; StatementDdlAlterTriggerAuthorityV1 a2{}; return bridge::BindStatementDdlAlterTriggerAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementDdlAlterTriggerAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlAlterTriggerAuthorityV1 a3{}; return bridge::CopyStatementDdlAlterTriggerAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementDdlDropTriggerAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlDropTriggerBindRequestV1 a1{}; StatementDdlDropTriggerAuthorityV1 a2{}; return bridge::BindStatementDdlDropTriggerAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementDdlDropTriggerAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlDropTriggerAuthorityV1 a3{}; return bridge::CopyStatementDdlDropTriggerAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementDdlCreateProcedureAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlCreateProcedureBindRequestV2 a1{}; StatementDdlCreateProcedureAuthorityV1 a2{}; return bridge::BindStatementDdlCreateProcedureAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementDdlCreateProcedureAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDdlCreateProcedureAuthorityV1 a3{}; return bridge::CopyStatementDdlCreateProcedureAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementProcedureInvokeAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementProcedureInvokeBindRequestV2 a1{}; StatementProcedureInvokeAuthorityV1 a2{}; return bridge::BindStatementProcedureInvokeAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementProcedureInvokeAuthorityV1", "executable_objects", "native_executable_object_catalog", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementProcedureInvokeAuthorityV1 a3{}; return bridge::CopyStatementProcedureInvokeAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementCatalogIntrospectAuthorityV1", "catalog_introspect", "native_catalog_introspection", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementCatalogIntrospectAuthorityV1 a3{}; return bridge::BindStatementCatalogIntrospectAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"CopyStatementCatalogIntrospectAuthorityV1", "catalog_introspect", "native_catalog_introspection", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementCatalogIntrospectAuthorityV1 a3{}; return bridge::CopyStatementCatalogIntrospectAuthorityV1(receipt, 1, 1, &a3, out_result); }},
    {"BindStatementCatalogEpochCheckAuthorityV1", "catalog_epoch_check", "native_catalog_epoch_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementCatalogEpochCheckBindRequestV1 a1{}; StatementCatalogEpochCheckBindAckV1 a2{}; return bridge::BindStatementCatalogEpochCheckAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementCatalogEpochCheckAuthorityV1", "catalog_epoch_check", "native_catalog_epoch_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementCatalogEpochCheckAuthorityV1 a2{}; return bridge::CopyStatementCatalogEpochCheckAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementParseTextAuthorityV1", "parse_text", "native_parse_text_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementParseTextBindRequestV1 a1{}; StatementParseTextBindAckV1 a2{}; return bridge::BindStatementParseTextAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementParseTextAuthorityV1", "parse_text", "native_parse_text_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementParseTextAuthorityV1 a2{}; return bridge::CopyStatementParseTextAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementDatabaseAttachAuthorityV1", "database_attach", "native_database_attach_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDatabaseAttachBindRequestV1 a1{}; StatementDatabaseAttachBindAckV1 a2{}; return bridge::BindStatementDatabaseAttachAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementDatabaseAttachAuthorityV1", "database_attach", "native_database_attach_journal", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementDatabaseAttachAuthorityV1 a2{}; return bridge::CopyStatementDatabaseAttachAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementOptimizerStatsReadAuthorityV1", "optimizer_stats", "native_optimizer_stats_store", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementOptimizerStatsReadAuthorityV1 a2{}; return bridge::BindStatementOptimizerStatsReadAuthorityV1(receipt, 1, &a2, out_result); }},
    {"CopyStatementOptimizerStatsReadAuthorityV1", "optimizer_stats", "native_optimizer_stats_store", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementOptimizerStatsReadAuthorityV1 a2{}; return bridge::CopyStatementOptimizerStatsReadAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementOptimizerStatsDropAuthorityV1", "optimizer_stats", "native_optimizer_stats_store", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementOptimizerStatsDropAuthorityV1 a2{}; return bridge::BindStatementOptimizerStatsDropAuthorityV1(receipt, 1, &a2, out_result); }},
    {"CopyStatementOptimizerStatsDropAuthorityV1", "optimizer_stats", "native_optimizer_stats_store", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementOptimizerStatsDropAuthorityV1 a2{}; return bridge::CopyStatementOptimizerStatsDropAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementCancelAuthorityV1", "statement_cancel", "native_statement_cancellation_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementCancelBindRequestV1 a1{}; StatementCancelBindAckV1 a2{}; return bridge::BindStatementCancelAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementCancelAuthorityV1", "statement_cancel", "native_statement_cancellation_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementCancelAuthorityV1 a2{}; return bridge::CopyStatementCancelAuthorityV1(receipt, 1, &a2, out_result); }},
    {"BindStatementParameterBindAuthorityV1", "parameter_binding", "native_parameter_and_variable_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementParameterBindRequestV1 a1{}; std::vector<std::uint8_t> a2{}; return bridge::BindStatementParameterBindAuthorityV1(receipt, &a1, &a2, out_result); }},
    {"CopyStatementParameterBindAuthorityV1", "parameter_binding", "native_parameter_and_variable_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementParameterBindAuthorityV1 a2{}; return bridge::CopyStatementParameterBindAuthorityV1(receipt, 1, &a2, out_result); }},
    {"NegotiateStatementParameterDescriptorsV1", "parameter_binding", "native_parameter_and_variable_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::NegotiateStatementParameterDescriptorsV1(receipt, a1, &a2, out_result); }},
    {"FinalizeStatementParameterBindingV1", "parameter_binding", "native_parameter_and_variable_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::FinalizeStatementParameterBindingV1(receipt, a1, &a2, out_result); }},
    {"FinalizeStatementVariableBindingV1", "parameter_binding", "native_parameter_and_variable_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::FinalizeStatementVariableBindingV1(receipt, a1, &a2, out_result); }},
    {"AssignStatementVariableValuesV1", "parameter_binding", "native_parameter_and_variable_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::AssignStatementVariableValuesV1(receipt, a1, &a2, out_result); }},
    {"NegotiateStatementLiteralDescriptorsV1", "literal_descriptors", "native_literal_descriptor_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::NegotiateStatementLiteralDescriptorsV1(receipt, a1, &a2, out_result); }},
    {"FinalizeStatementLiteralBindingV1", "literal_descriptors", "native_literal_descriptor_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::FinalizeStatementLiteralBindingV1(receipt, a1, &a2, out_result); }},
    {"IssueStatementContextualTextLiteralProfilesV2", "literal_descriptors", "native_literal_descriptor_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::IssueStatementContextualTextLiteralProfilesV2(receipt, a1, &a2, out_result); }},
    {"PrepareStatementContextualTextLiteralExecutionV2", "literal_descriptors", "native_literal_descriptor_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementContextualTextLiteralPrepareRequestV2 a0{}; a0.receipt = receipt; scratchbird::engine::internal_api::PreparedContextualTextLiteralSetV2 a1{}; return bridge::PrepareStatementContextualTextLiteralExecutionV2(&a0, &a1, out_result); }},
    {"JointConsumeStatementContextualTextLiteralExecutionV2", "literal_descriptors", "native_literal_descriptor_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementContextualTextLiteralJointConsumeRequestV2 a0{}; a0.receipt = receipt; scratchbird::engine::internal_api:: ContextualTextExecutionAuthorityLeaseV2 a1{}; return bridge::JointConsumeStatementContextualTextLiteralExecutionV2(&a0, &a1, out_result); }},
    {"RevokeStatementContextualTextLiteralProfilesV2", "literal_descriptors", "native_literal_descriptor_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) {  return bridge::RevokeStatementContextualTextLiteralProfilesV2(receipt, "x", out_result); }},
    {"RetainStatementSourceArtifactV1", "source_artifact", "native_source_artifact_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementSourceArtifactRetentionV1 a3{}; return bridge::RetainStatementSourceArtifactV1(receipt, nullptr, 1, &a3, out_result); }},
    {"ResolveStatementSourceArtifactV1", "source_artifact", "native_source_artifact_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementSourceArtifactReferenceV1 a1{}; StatementSourceArtifactRetentionV1 a2{}; return bridge::ResolveStatementSourceArtifactV1(receipt, &a1, &a2, out_result); }},
    {"IssueStatementSourceMapDescriptorV1", "sblr_descriptor_registries", "native_sblr_descriptor_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::IssueStatementSourceMapDescriptorV1(receipt, a1, &a2, out_result); }},
    {"IssueStatementErrorVectorDescriptorV1", "sblr_descriptor_registries", "native_sblr_descriptor_registries", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { std::vector<std::uint8_t> a1{}; std::vector<std::uint8_t> a2{}; return bridge::IssueStatementErrorVectorDescriptorV1(receipt, a1, &a2, out_result); }},
    {"AcquireStatementPackageAdmissionReservation", "package_admission", "native_package_admission_registry", [](bridge::StatementContextReceiptHandle receipt, sb_engine_result_t* out_result) { StatementPackageAdmissionReservationRequest a0{}; a0.receipt = receipt; StatementPackageAdmissionReservationHandle a1{}; StatementPackageAdmissionReservationView a2{}; return bridge::AcquireStatementPackageAdmissionReservation(&a0, &a1, &a2, out_result); }},
  };
  return table;
}

// A recursive listing of a directory: every entry's relative path and kind (files carry their size).
std::map<std::string, std::string> Listing(const fs::path& root) {
  std::map<std::string, std::string> out;
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
    const auto rel = fs::relative(it->path(), root).string();
    out[rel] = it->is_directory(error) ? "dir" : "file:" + std::to_string(it->file_size(error));
  }
  return out;
}
bool AnySideFile(const fs::path& root) {
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
    if (it->path().filename().string().find(".sb.") != std::string::npos) return true;
  return false;
}

struct Observation {
  std::vector<p::byte> image;
  std::map<std::string, std::string> listing;
};
Observation Before(bool compare) {
  Observation o;
  if (compare) {
    o.image = Image(served.path);
    o.listing = Listing(served.directory);
  }
  return o;
}
// One refused call: the exact code, the fields, nothing written, nothing created.
void ExpectRefusal(const std::string& what, const Probe& got, const std::string& code, const std::string& operation, const std::string& requires_token,
                   const Observation& before, bool compare) {
  Check(got.status == SB_ENGINE_STATUS_UNSUPPORTED, what + ": status UNSUPPORTED (" + std::to_string(static_cast<int>(got.status)) + ")");
  Check(got.code == code, what + ": code " + code + " (got " + got.code + ")");
  Check(got.operation == operation, what + ": field operation = " + operation + " (got " + got.operation + ")");
  Check(got.requires_token == requires_token, what + ": field requires = " + requires_token + " (got " + got.requires_token + ")");
  if (!compare) return;
  Check(Image(served.path) == before.image, what + ": the file's bytes are identical");
  Check(Listing(served.directory) == before.listing, what + ": no file or directory entry appeared or changed");
}

int GateState(const std::string& state, const Offline& offline) {
  const bool trace = state == "trace";
  const bool compare = !trace;  // the strace state compares once around the whole region (the test's own reads would hide an engine-side open)
  const std::uint64_t g0 = offline.catalog_generation;
  Check(g0 != 0, "the created file's names carry a catalog generation");
  Check(Listing(served.directory).size() == 1 && Listing(served.directory).count(served.path.filename().string()) == 1,
        "the database directory holds only the data file before the gate runs");

  if (trace) Mark("approve");
  auto s1 = Approve();
  auto ps1 = OpenPublicFor(*s1);
  auto c1 = ContextFor(*s1, g0);
  std::vector<p::byte> image_before;
  if (trace) {
    Mark("prepare");
    image_before = Image(served.path);
  }
  const auto listing_before = Listing(served.directory);
  // Next as the file shows it AFTER the approval (the approval's own transaction was begun) and BEFORE the gate region.
  const auto next_before = off::ReadFile(served.path, served.facts->page_size_bytes).counters.next_transaction;
  if (trace) Mark("gate");
  auto r1 = Acquire(*ps1, c1, s1->approval.transaction_uuid, compare);
  Check(r1.ok(), "a receipt is acquired for an approved session's active transaction: " + r1.code);
  const auto receipt = r1.receipt;

  // ---- the allow-list: what works natively still works ------------------------------------------------------------------------
  {
    api::EngineRequestContext copy;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::CopyStatementContextEngineContextV1(receipt, &copy, &result);
    Check(status == SB_ENGINE_STATUS_OK && Code(result).empty() && copy.native_served_facts == served.facts,
          "CopyStatementContextEngineContextV1 (allowed): the receipt's native context still copies");
  }
  // S4g part 1: CopyStatementNameResolveAuthorityV1 is allowed natively (it reads the receipt's in-memory bind authority); no occurrence
  // was bound on this receipt, so it passes the gate to its own answer (authority hidden), not the native refusal.
  {
    bridge::StatementNameResolveAuthorityV1 authority;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::CopyStatementNameResolveAuthorityV1(receipt, 1, &authority, &result);
    const auto code = Code(result);
    Check(status != SB_ENGINE_STATUS_OK && code == "SECURITY.ACCESS_DENIED",
          "CopyStatementNameResolveAuthorityV1 (allowed): passes the gate to its own 'authority hidden' answer: " + code);
  }

  // ---- every refused consumer, on the native receipt ---------------------------------------------------------------------------
  std::size_t count = 0;
  std::set<std::string> families;
  for (const auto& row : GatedTable()) {
    const auto before = Before(compare);
    sb_engine_result_t result = nullptr;
    const auto status = row.call(receipt, &result);
    const auto got = Observe(status, result);
    ExpectRefusal(row.name, got, "ENGINE.NATIVE.OPERATION_NOT_BUILT", row.name, row.requires_token, before, compare);
    families.insert(row.family);
    ++count;
  }
  // The session-keyed consumer (a native SESSION, not a receipt) of the prepared-metadata binding.
  {
    sb_engine_request_context_v1_t request_context{};
    request_context.struct_size = sizeof(request_context);
    request_context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_sblr_dispatch_params_v1_t params{};
    params.struct_size = sizeof(params);
    params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    {
      const auto before = Before(compare);
      bridge::PreparedMetadataBindingHandle binding = nullptr;
      sb_engine_result_t result = nullptr;
      const auto status = bridge::CreatePreparedMetadataBinding(ps1->session, &request_context, NewId(), &params, &binding, &result);
      ExpectRefusal("CreatePreparedMetadataBinding", Observe(status, result), "ENGINE.NATIVE.OPERATION_NOT_BUILT", "CreatePreparedMetadataBinding",
                    "native_prepared_statement_registry", before, compare);
      Check(binding == nullptr, "CreatePreparedMetadataBinding: no binding was made");
      ++count;
    }
    {
      const auto before = Before(compare);
      int marker = 0;  // a non-null binding: the gate answers before the binding is read
      sb_engine_result_t result = nullptr;
      const auto status = bridge::DispatchWithPreparedMetadataBinding(
          ps1->session, nullptr, &request_context, &params, reinterpret_cast<bridge::PreparedMetadataBindingHandle>(&marker), &result);
      ExpectRefusal("DispatchWithPreparedMetadataBinding", Observe(status, result), "ENGINE.NATIVE.OPERATION_NOT_BUILT", "DispatchWithPreparedMetadataBinding",
                    "native_executor_availability_s4g", before, compare);
      ++count;
    }
  }

  // ---- the entries that keep their OWN gate (the consumer gate passes them through unchanged) ------------------------------
  {
    const auto before = Before(compare);
    bridge::StatementContextDispatchRequest dispatch;
    dispatch.receipt = receipt;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
    ExpectRefusal("DispatchStatementContextReceipt", Observe(status, result), "ENGINE.NATIVE.OPERATION_NOT_BUILT", "dispatch_statement_context",
                  "native_executor_availability_s4g", before, compare);
  }
  {
    const auto before = Before(compare);
    api::EngineRequestContext context = c1;
    bridge::StatementParameterCoordinationBeginRequestV1 begin{};
    begin.engine_session = ps1->session;
    begin.engine_context = &context;
    begin.mode = bridge::StatementParameterExecutionMode::kPrepared;
    begin.operation_uuid = NewId();
    bridge::StatementParameterCoordinationViewV1 view{};
    sb_engine_result_t result = nullptr;
    const auto status = bridge::BeginStatementParameterExecutionCoordinationV1(&begin, &view, &result);
    ExpectRefusal("BeginStatementParameterExecutionCoordinationV1", Observe(status, result), "ENGINE.NATIVE.OPERATION_NOT_BUILT", "parameter_coordination_begin",
                  "native_session_and_transaction_s4", before, compare);
  }
  {
    // The name-resolve BIND keeps its own gate: a default request is NOT answered by the consumer gate (the allow-list passes it through to
    // its own request validation); its real native refusal (SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED) is asserted by the S4f native_receipt test.
    const auto before = Before(compare);
    bridge::StatementNameResolveBindRequestV1 request;
    bridge::StatementNameResolveBindAckV1 ack;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::BindStatementNameResolveAuthorityV1(receipt, &request, &ack, &result);
    const auto got = Observe(status, result);
    Check(got.code == "SBLR.OPERAND.INVALID" && got.operation.empty(),
          "BindStatementNameResolveAuthorityV1 (own gate): passes through the consumer gate to its own validation (" + got.code + ")");
    if (compare) Check(Image(served.path) == before.image && Listing(served.directory) == before.listing, "and wrote and created nothing");
  }
  {
    const auto before = Before(compare);
    sb_engine_sblr_dispatch_params_v1_t params{};
    params.struct_size = sizeof(params);
    params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_request_context_v1_t request_context{};
    request_context.struct_size = sizeof(request_context);
    request_context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_result_t result = nullptr;
    auto status = sb_engine_dispatch_sblr(ps1->session, nullptr, &request_context, &params, &result);
    ExpectRefusal("sb_engine_dispatch_sblr", Observe(status, result), "ENGINE.NATIVE.OPERATION_NOT_BUILT", "dispatch_sblr", "native_session_and_transaction_s4",
                  before, compare);
    sb_engine_transaction_params_v1_t transaction_params{};
    transaction_params.struct_size = sizeof(transaction_params);
    transaction_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    sb_engine_transaction_t transaction = nullptr;
    status = sb_engine_transaction_begin(ps1->session, &transaction_params, &transaction, &result);
    ExpectRefusal("sb_engine_transaction_begin", Observe(status, result), "ENGINE.NATIVE.OPERATION_NOT_BUILT", "transaction_begin", "native_session_and_transaction_s4",
                  before, compare);
    Check(transaction == nullptr, "no engine transaction was made");
  }

  // ---- the receipt is still live and releases (the gate does not consume or invalidate it) --------------------------------------
  Release(r1);
  if (trace) Mark("verify");
  if (trace) Check(Image(served.path) == image_before, "the file's bytes are identical after the whole table");
  Check(Listing(served.directory) == listing_before, "no file or directory entry appeared during the whole table");
  Check(!AnySideFile(served.directory.parent_path()), "no file named .sb. exists anywhere under the test's temp tree");
  Check(off::ReadFile(served.path, served.facts->page_size_bytes).counters.next_transaction == next_before,
        "the file's Next transaction is unchanged by the whole table");
  if (trace) Mark("disconnect");
  ps1.reset();
  Disconnect(*s1);
  std::printf("GATE entries=%zu families=%zu\n", count, families.size());
  return 0;
}

int ServeMode(const fs::path& path, const std::string& state) {
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !mem::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() || !crypto.Start(config, policy).ok()) {
    std::fprintf(stderr, "process setup failed\n");
    return 3;
  }
  config.database_default_path = path;
  served.path = path;
  served.directory = path.parent_path();
  const auto offline = ReadOffline(path);
  served.started = srv::StartHostedEngine(config, nullptr, &crypto);
  Check(served.started.ok(), "the server serves the file: " + (served.started.diagnostics.empty() ? std::string() : served.started.diagnostics.front().code));
  Check(!served.started.state.database_runtimes.empty() && served.started.state.database_runtimes.front() &&
            served.started.state.database_runtimes.front()->native_loop,
        "the hosted state holds the native loop");
  served.loop = served.started.state.database_runtimes.front()->native_loop.get();
  served.facts = srv::ServedNativeFactsFor(served.started.state, path.string());
  Check(served.facts != nullptr && served.facts->receipt_source != nullptr, "the loop published the facts with a receipt source");
  served.database = served.facts->database_uuid;
  srv::NativeLoginThrottle().Clear();
  const int rc = GateState(state, offline);
  if (state == "trace") Mark("end");
  (void)served.loop->Close();
  served.facts.reset();
  served.started = {};
  if (rc == 0) std::cout << "GATE_OK checks=" << checks << " state=" << state << std::endl;
  return rc;
}

std::string CaptureOutput(const std::string& command, int* status) {
  std::string output;
  FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
  Check(pipe != nullptr, "popen");
  char chunk[512];
  std::size_t got;
  while ((got = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) output.append(chunk, got);
  const int raw = ::pclose(pipe);
  *status = raw == -1 ? -1 : (WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw));
  return output;
}
std::string Field(const std::string& text, std::string_view key) {
  const auto at = text.find(std::string(key) + "=");
  if (at == std::string::npos) return {};
  const auto from = at + key.size() + 1;
  const auto to = text.find_first_of(" \n", from);
  return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

void RunSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto dir = root / ("gate" + std::to_string(page_size));
  fs::create_directory(dir);
  const auto path = dir / "gate.sdb";
  int status = 0;
  const std::string material = SB_NATIVE_LOGIN_MATERIAL_TEST_BIN;
  const auto created = CaptureOutput("'" + material + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
  Check(status == 0 && created.starts_with("CREATED "), tag + "create: " + created);
  const auto start = std::chrono::steady_clock::now();
  const auto served_out = CaptureOutput("'" + self + "' serve '" + path.string() + "' gate " + Field(created, "database") + " " + Field(created, "bootstrap") + " " +
                                            Field(created, "service"),
                                        &status);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  Check(status == 0 && served_out.find("GATE_OK") != std::string::npos, tag + "serve child: " + served_out.substr(0, 3000));
  Check(served_out.find(kBootstrapPassword) == std::string::npos && created.find(kBootstrapPassword) == std::string::npos, tag + "no test password in any child output");
  Check(!AnySideFile(root), tag + "no file named .sb. under the driver's temp tree");
  const auto summary = served_out.find("GATE entries");
  std::printf("%sgate: %.1f s: %s", tag.c_str(), seconds, summary == std::string::npos ? "\n" : served_out.substr(summary, served_out.find('\n', summary) - summary + 1).c_str());
  fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (argc == 3) RunSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])));
    else
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u}) RunSize(self, artifacts.path(), size);
    artifacts.Cleanup();
    std::printf("native_receipt_gate: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
