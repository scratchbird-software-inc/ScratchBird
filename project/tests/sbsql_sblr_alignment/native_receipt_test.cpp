// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_RECEIPT_TEST (thin-slice S4f; owner Q298: "the receipt forks: cohort from the facts, inventory from the
// native TIP reader, are built with S4's session and transaction"). The NATIVE STATEMENT-CONTEXT RECEIPT on a PRODUCTION-created
// database served by StartHostedEngine. The user's session and transaction come from the REAL approval (HandleAuthHandoff,
// S4e); the public-ABI engine session over them is made exactly as the server's EnsureServerPublicAbiSessionForContext makes
// it (OpenServedEngine with the record's facts and served owner, then sb_engine_session_begin).
//
// Asserted, per page size (a child process each; the file is read back OFFLINE as ground truth):
//   * a receipt is acquired for an approved session's ACTIVE transaction; its fields equal the TIP header of that transaction
//     (+48 engine instance, +64 principal, +80 attachment, +96 session, the UUID, the order), the served facts (datatype
//     cohort), the offline TIP (Next, the active and committed slots) and the catalog generation read offline from the names;
//   * a second session gets its OWN receipt and its OWN snapshot (its begin saw the first session's transaction in progress);
//   * the snapshot is STABLE: a catalog change committed after a transaction began is invisible to its receipt (its snapshot,
//     its high-water mark and the generation it sees), and a session approved after the commit sees it; a read_committed
//     transaction's receipt takes its snapshot at acquire and sees it; a profile the native receipt does not know refuses;
//   * every refusal carries its own code and writes NOTHING (the file's bytes are identical): no session, a session that
//     ended, a transaction that ended, another session's transaction, a stale generation (context or authorization), a
//     disagreeing order, an absent transaction identity, the failed database, the restricted database;
//   * the receipt is READ ONLY: the file's bytes are identical before and after every acquire (the strace half counts
//     the syscalls);
//   * the SBLR name-resolve BIND with a native receipt refuses with the availability table's OWN code
//     (SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED: NAME_RESOLVE is not on the native allow-list, owner Q302), dispatching a native
//     receipt refuses ENGINE.NATIVE.OPERATION_NOT_BUILT (S4g), and the name-resolve JOURNAL refuses a native receipt's context
//     with ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED (owner Part A Q3: moves into the single file later).
// Modes (a child process each):
//   serve <path> <state> <database_hex> <bootstrap_hex> <service_hex>   states: receipt restricted trace
//        prints "RECEIPT_OK checks=N"; `trace` marks regions on stderr for native_receipt_strace.py
//   pages <path> <page_size>                                            the chains' page numbers (for the strace half)
// With no argument the driver runs everything; "size <bytes>" one page size. create / fixture are the sibling binaries' modes.

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
constexpr std::string_view kNameCommitted = "Receipt_Name_Committed_After_Begin";
constexpr std::uint8_t kSnapshotStable = 4, kReadCommittedStatement = 2, kRepeatableRead = 5;  // IsolationProfileKind

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
  PutString(&out, "native_receipt_test");
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
const NameFact& BaseName(const Offline& o, const p::Uuid& object) {
  for (const auto& n : o.names)
    if (n.object == object && n.text == kBootstrapName) return n;
  Check(false, "the bootstrap principal's create-time name entry is visible");
  return o.names.front();
}

// ---- a catalog change made on the loop thread by the loop's own writer (BEGIN, name row, COMMIT) ------------------------
db::CatalogVersionWriterBinding BindingOf(const Offline& o) {
  std::optional<d::FilespaceRootReference> catalog_root, state_root, allocation_root, configuration_root, security_root;
  for (const auto& root : o.roots) {
    if (root.kind == 2) catalog_root = root;
    if (root.kind == 4) state_root = root;
    if (root.kind == 3) allocation_root = root;
    if (root.kind == 6) configuration_root = root;
    if (root.kind == 7) security_root = root;
  }
  Check(catalog_root && state_root && allocation_root && configuration_root && security_root, "page zero names the roots");
  db::CatalogVersionWriterBinding b;
  b.database_uuid = o.database;
  b.catalog_root = *catalog_root;
  b.state_root = *state_root;
  b.configuration_root = *configuration_root;
  b.security_root = *security_root;
  b.allocation_root = *allocation_root;
  return b;
}
std::set<std::uint64_t> CommittedOrders(const off::FileView& f);
struct Change {
  std::uint64_t order = 0, generation = 0;
};
// Commits ONE catalog change the way the native catalog publishes one: the catalog root's generation is advanced by the loop's
// writer (AdvanceCatalogRoot, by one, naming an earlier COMMITTED transaction), then a transaction inserts a new alias name of the
// bootstrap principal at the new generation and commits. Runs on the loop thread (the writer is thread-affine).
Change CommitNameChange(const Offline& o, const p::Uuid& principal) {
  Change change;
  std::uint64_t& order = change.order;
  std::uint64_t& generation = change.generation;
  Check(served.loop->RunOnLoopThreadForTest([&] {
          // the root as the file has it now, and a committed transaction to name
          const auto file = off::ReadFile(served.path, served.facts->page_size_bytes);
          std::optional<d::FilespaceRootReference> catalog_root;
          for (const auto& root : file.zero.record->roots)
            if (root.kind == 2) catalog_root = root;
          Check(catalog_root.has_value(), "page zero names the catalog root");
          const auto root_begin = file.image.begin() + static_cast<std::ptrdiff_t>(catalog_root->page_number * file.page_size);
          const auto decoded_root = pgs::DecodeNativeCatalogRoot(std::vector<p::byte>(root_begin, root_begin + file.page_size));
          Check(decoded_root.ok(), "the catalog root decodes");
          const auto committed = CommittedOrders(file);
          Check(!committed.empty(), "the created file has a committed transaction");
          const auto named_order = *committed.begin();
          generation = decoded_root.root->catalog_generation + 1;
          Check(served.loop->RunWithTransactionSinkOnLoopThreadForTest([&](d::CarefulWriteSink& sink) {
                  auto binding = BindingOf(o);
                  db::CatalogRootAdvanceRequest advance;
                  advance.transaction_order = named_order;
                  advance.transaction_uuid = file.headers.at(named_order).transaction_uuid;
                  advance.catalog_generation = generation;
                  advance.schema_epoch = decoded_root.root->schema_epoch;
                  advance.security_epoch = decoded_root.root->security_epoch;
                  advance.resource_epoch = decoded_root.root->resource_epoch;
                  const auto advanced = db::AdvanceCatalogRoot(sink, binding, advance);
                  if (!advanced.ok()) std::cerr << "root advance refused: " << db::CatalogVersionWriteErrorName(advanced.error) << '\n';
                  Check(advanced.ok(), "the catalog root advances to the next generation");
                }),
                "the loop's write door opened for the root advance");
          const auto key = NewId();
          const auto begun = served.loop->BeginWorkOnLoopThread({key, false, kReadCommittedStatement, {}});
          Check(begun.ok(), "BEGIN of the catalog change by the loop's writer");
          order = begun.begun_order;
          Check(served.loop->RunWithTransactionSinkOnLoopThreadForTest([&](d::CarefulWriteSink& sink) {
                  auto binding = BindingOf(o);
                  const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count());
                  const auto event_uuid = NewId();
                  const auto written_event = scratchbird::tests::catalog_change_event::WriteEvent(
                      sink, binding, order, begun.begun_uuid, event_uuid, NewId(), NewId(), NewId(), now_us);
                  Check(written_event.ok(), "the transaction's catalog.metadata_change event");
                  // A name row is inserted under its object's definition: the transaction first writes a successor ("touch")
                  // of the principal's security row, which takes the definition's write lock (as a real change does).
                  {
                    const auto found = o.security.find(principal);
                    Check(found != o.security.end(), "the principal's security row is in the file");
                    const auto& f = found->second;
                    db::CatalogVersionRequest r;
                    r.role = 4;
                    r.transaction_order = order;
                    r.transaction_uuid = begun.begun_uuid;
                    r.row_uuid = f.row_uuid;
                    r.predecessor_version_uuid = f.version_uuid;
                    r.version_uuid = NewId();
                    r.metadata = f.metadata;
                    ++r.metadata.definition_version;
                    r.metadata.creator_transaction_uuid = {p::UuidKind::transaction, begun.begun_uuid};
                    r.metadata.creator_local_transaction_id = order;
                    r.metadata.audit_uuid = {p::UuidKind::object, event_uuid};
                    const auto touched = db::WriteCatalogSuccessorVersion(sink, binding, r);
                    if (!touched.ok()) std::cerr << "successor refused: " << db::CatalogVersionWriteErrorName(touched.error) << '\n';
                    Check(touched.ok(), "the catalog version writer writes the definition's successor in place");
                  }
                  const auto& base = BaseName(o, principal);
                  db::CatalogInsertRow row;
                  row.role = 2;
                  row.row_uuid = NewId();
                  row.version_uuid = NewId();
                  row.metadata = *base.metadata;
                  const p::TypedUuid id{p::UuidKind::object, NewId()};
                  row.metadata.record.header.object_uuid = id;
                  row.metadata.record.header.row_uuid = {p::UuidKind::row, row.row_uuid};
                  row.metadata.record.header.deleted = false;
                  row.metadata.record.payload.clear();
                  row.metadata.creator_transaction_uuid = {p::UuidKind::transaction, begun.begun_uuid};
                  row.metadata.creator_local_transaction_id = order;
                  row.metadata.definition_version = 1;
                  row.metadata.catalog_generation = generation;
                  auto e = *base.entry;
                  e.raw_name_text = std::string(kNameCommitted);
                  e.was_quoted = false;
                  e.quote_style = cat::CatalogNameQuoteStyle::none;
                  e.requires_exact_match = false;
                  e.name_entry_uuid = id;
                  e.name_class = cat::CatalogNameClass::alias;
                  e.default_for_object = false;
                  e.default_for_language = false;
                  e.created_transaction_uuid = row.metadata.creator_transaction_uuid;
                  e.catalog_generation_id = generation;
                  e.lifecycle_state = cat::CatalogNameLifecycle::active;
                  e.dropped_transaction_uuid.reset();
                  row.name = cat::CatalogNamePayload{e};
                  Check(cat::CatalogNamePayloadMatchesMetadata(*row.name, row.metadata), "the new entry agrees with its metadata");
                  db::CatalogInsertRequest request;
                  request.transaction_order = order;
                  request.transaction_uuid = begun.begun_uuid;
                  request.rows.push_back(std::move(row));
                  for (int i = 0; i < 8; ++i) request.new_pages.push_back({NewId(), NewId()});
                  const auto written = db::WriteCatalogInsertVersions(sink, binding, request);
                  if (!written.ok()) std::cerr << "insert refused: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
                  Check(written.ok(), "the name row is inserted in place");
                }),
                "the loop's write door opened for the test");
          const auto done = served.loop->CompleteWorkOnLoopThread({key, order, srv::NativeLoopCompletion::commit});
          Check(done.ok() && done.committed, "COMMIT of the catalog change");
        }),
        "the loop ran the catalog change");
  return change;
}

// ---- the receipt, checked against the file --------------------------------------------------------------------------------
std::set<std::uint64_t> ActiveOrders(const off::FileView& f) {
  std::set<std::uint64_t> out;
  for (const auto& [order, word] : f.words)
    if (word.state == pgs::TipStateCode::active || word.state == pgs::TipStateCode::read_only_active) out.insert(order);
  return out;
}
std::set<std::uint64_t> CommittedOrders(const off::FileView& f) {
  std::set<std::uint64_t> out;
  for (const auto& [order, word] : f.words)
    if (word.state == pgs::TipStateCode::committed) out.insert(order);
  return out;
}
std::set<std::uint64_t> AsSet(const std::vector<std::uint64_t>& v) { return {v.begin(), v.end()}; }

// What the receipt of `a` must say given the file (read now) and what the test KNOWS began before it: `expected_excluded` are the
// orders active when the transaction began (plus itself), `boundary` is the begin's Next - 1 (the transaction's order - 1).
void CheckReceiptAgainstFile(const std::string& what, const Approved& a, const Acquired& r, const off::FileView& f,
                             const std::set<std::uint64_t>& expected_excluded, std::uint64_t generation) {
  const auto order = a.approval.transaction_order;
  const auto header = f.headers.find(order);
  Check(header != f.headers.end(), what + ": the transaction's header is in the file");
  const auto& h = header->second;
  const auto& v = r.view;
  Check(v.owning_local_transaction_id == order && v.owning_transaction_uuid.bytes == h.transaction_uuid.bytes,
        what + ": the receipt's owning transaction is the TIP header's (order and UUID)");
  Check(h.engine_instance_uuid == served.facts->engine_instance_uuid, what + ": header +48 is the served engine instance");
  Check(h.principal_or_agent_uuid == p::Uuid{a.record->effective_user_uuid} && h.principal_or_agent_uuid == served.bootstrap,
        what + ": header +64 is the authenticated principal (the bootstrap user)");
  Check(h.session_uuid == a.session() && h.attachment_uuid == p::Uuid{a.approval.attachment_uuid},
        what + ": header +80 / +96 are the approval's attachment and session");
  Check(h.isolation_profile == kSnapshotStable && f.words.at(order).state == pgs::TipStateCode::active,
        what + ": the header's isolation is snapshot_stable and its slot is active");
  const auto context = CopyContext(r);
  Check(context.principal_uuid.bytes == h.principal_or_agent_uuid.bytes && context.session_uuid.bytes == h.session_uuid.bytes &&
            context.transaction_uuid.bytes == h.transaction_uuid.bytes && context.local_transaction_id == order,
        what + ": the receipt's context carries the header's principal, session and transaction");
  Check(context.native_served_facts == served.facts, what + ": the receipt's marker is the handle's served facts (process-local)");
  Check(context.catalog_generation_id == generation && r.view.catalog_generation_id == generation,
        what + ": the catalog generation is " + std::to_string(generation));
  // The cohort comes from the served facts, and those are what the loop captured from the database record.
  Check(v.literal_catalog_snapshot_uuid.bytes == served.facts->datatype_catalog_snapshot_uuid.bytes &&
            v.literal_catalog_generation == served.facts->datatype_catalog_generation &&
            v.literal_registry_generation == served.facts->datatype_registry_generation && served.facts->datatype_cohort_present,
        what + ": the datatype cohort is the served facts'");
  // The inventory is the live TIP: Next as the file says; the snapshot is the BEGIN snapshot.
  Check(v.publication_inventory_next_local_transaction_id == f.counters.next_transaction,
        what + ": the publication's Next is the file's TIP Next (" + std::to_string(f.counters.next_transaction) + ")");
  Check(AsSet(v.active_excluded_local_transaction_ids) == expected_excluded, what + ": the snapshot's active exclusions are the begin's");
  Check(v.in_doubt_excluded_local_transaction_ids.empty(), what + ": nothing was in doubt");
  std::uint64_t high = 0;
  for (const auto committed : CommittedOrders(f))
    if (committed < order && expected_excluded.count(committed) == 0) high = std::max(high, committed);
  Check(v.visible_committed_high_watermark == high, what + ": the high-water mark is the newest committed order below the begin that the snapshot may see");
  const auto active = ActiveOrders(f);
  Check(!active.empty() && v.oldest_active_local_transaction_id == *active.begin(), what + ": the oldest active is the file's");
  Check(r.view.statement_snapshot_uuid != r.view.receipt_uuid && !r.view.statement_snapshot_uuid.is_nil() && !r.view.receipt_uuid.is_nil(),
        what + ": the receipt and its snapshot have their own identities");
  Check(r.view.active_transaction_handle_bytes.empty() && r.view.name_resolve_executor_availability_generation == 0,
        what + ": no TXBH handle and no executor generation natively (no native begin authority, empty allow-list)");
}

// ---- the cases ---------------------------------------------------------------------------------------------------------------
int ReceiptState(const std::string& state, const Offline& offline) {
  const bool trace = state == "trace";
  const auto page_size = served.facts->page_size_bytes;
  const auto read = [&] { return off::ReadFile(served.path, page_size); };
  const std::uint64_t g0 = offline.catalog_generation;
  Check(g0 != 0, "the created file's names carry a catalog generation (" + std::to_string(g0) + ")");
  Check(served.facts->receipt_source != nullptr, "the loop published the receipt source in the facts");

  if (trace) Mark("approve");
  auto s1 = Approve();
  auto ps1 = OpenPublicFor(*s1);
  auto c1 = ContextFor(*s1, g0);
  std::vector<p::byte> image_before;
  if (trace) {
    // The test's own reads of the data path sit in their own regions (prepare, verify); the receipt region holds ONLY the engine.
    Mark("prepare");
    image_before = Image(served.path);
    Mark("receipt");
  }
  auto r1 = Acquire(*ps1, c1, s1->approval.transaction_uuid, !trace);
  Check(r1.ok(), "a receipt is acquired for an approved session's active transaction: " + r1.code);
  const auto order1 = s1->approval.transaction_order;
  if (trace) {
    // The strace half: a second acquire and one refusal, still in the read-only region.
    auto again = Acquire(*ps1, c1, s1->approval.transaction_uuid, false);
    Check(again.ok(), "a second receipt for the same transaction");
    auto stale = ContextFor(*s1, g0 + 7);
    const auto refused = Acquire(*ps1, stale, s1->approval.transaction_uuid, false);
    Check(refused.code == "ENGINE.STATEMENT_CONTEXT.NATIVE_CATALOG_GENERATION_STALE", "a stale generation refuses (traced)");
    Release(again);
    Release(r1);
    Mark("verify");
    Check(Image(served.path) == image_before, "the receipts and the refusal wrote nothing: the file's bytes are identical");
    Mark("disconnect");
    ps1.reset();
    Disconnect(*s1);
    return 0;
  }
  CheckReceiptAgainstFile("session 1", *s1, r1, read(), {order1}, g0);

  // A second session: its own receipt, its own snapshot (its begin saw session 1's transaction in progress).
  auto s2 = Approve();
  auto ps2 = OpenPublicFor(*s2);
  auto c2 = ContextFor(*s2, g0);
  auto r2 = Acquire(*ps2, c2, s2->approval.transaction_uuid);
  Check(r2.ok(), "the second session gets a receipt: " + r2.code);
  const auto order2 = s2->approval.transaction_order;
  Check(order2 > order1 && s2->session() != s1->session(), "a second session, a later transaction");
  CheckReceiptAgainstFile("session 2", *s2, r2, read(), {order1, order2}, g0);
  Check(r2.view.receipt_uuid != r1.view.receipt_uuid && r2.view.statement_snapshot_uuid != r1.view.statement_snapshot_uuid &&
            r2.view.owning_transaction_uuid != r1.view.owning_transaction_uuid && r2.view.statement_uuid != r1.view.statement_uuid,
        "the two sessions' receipts and snapshots are distinct");
  // Session 1's receipt, taken AGAIN after session 2 began: its snapshot is still its BEGIN snapshot (session 2 is not in it).
  {
    auto again = Acquire(*ps1, c1, s1->approval.transaction_uuid);
    Check(again.ok(), "session 1 acquires again after session 2 began: " + again.code);
    CheckReceiptAgainstFile("session 1 again", *s1, again, read(), {order1}, g0);
    Check(again.view.receipt_uuid != r1.view.receipt_uuid && again.view.statement_snapshot_uuid != r1.view.statement_snapshot_uuid,
          "a new receipt, a new published snapshot");
    Release(again);
  }

  // STABILITY: a catalog change committed AFTER both began (generation g0 + 1).
  const auto change = CommitNameChange(offline, served.bootstrap);
  const auto change_order = change.order;
  const auto g1 = change.generation;
  Check(change_order > order2 && g1 > g0, "the change is a later transaction than both sessions' and a later generation");
  {
    const auto f = read();
    Check(f.words.at(change_order).state == pgs::TipStateCode::committed, "the change is committed in the file");
    auto r1b = Acquire(*ps1, c1, s1->approval.transaction_uuid);
    Check(r1b.ok(), "session 1's receipt after the change: " + r1b.code);
    CheckReceiptAgainstFile("session 1 after the change", *s1, r1b, f, {order1}, g0);
    Check(r1b.view.visible_committed_high_watermark < change_order && r1b.view.publication_inventory_next_local_transaction_id > change_order,
          "the change is below the publication's Next but invisible to session 1's snapshot (high-water mark below it)");
    Release(r1b);
    // a claim of the NEW generation is stale for session 1 (its snapshot sees the old one), in the context or the authorization
    auto claim_new = ContextFor(*s1, g1);
    Check(Acquire(*ps1, claim_new, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_CATALOG_GENERATION_STALE",
          "session 1 claiming the new generation: stale (its snapshot predates the commit)");
    auto claim_auth = c1;
    claim_auth.authorization_context.catalog_generation_id = g1;
    Check(Acquire(*ps1, claim_auth, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_CATALOG_GENERATION_STALE",
          "a stale generation in the materialized AUTHORIZATION refuses too");
    auto claim_zero = c1;
    claim_zero.catalog_generation_id = 0;
    claim_zero.authorization_context.catalog_generation_id = 0;
    Check(Acquire(*ps1, claim_zero, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_CATALOG_GENERATION_STALE",
          "a context that states no generation is not a current one: refused, never filled in");
  }
  // A session approved AFTER the commit sees it.
  auto s3 = Approve();
  auto ps3 = OpenPublicFor(*s3);
  auto c3 = ContextFor(*s3, g1);
  auto r3 = Acquire(*ps3, c3, s3->approval.transaction_uuid);
  Check(r3.ok(), "a session approved after the change acquires with the new generation: " + r3.code);
  const auto order3 = s3->approval.transaction_order;
  {
    const auto f = read();
    CheckReceiptAgainstFile("session 3", *s3, r3, f, {order1, order2, order3}, g1);
    Check(r3.view.visible_committed_high_watermark == change_order, "its snapshot sees the change (high-water mark = the change's order)");
    auto claim_old = ContextFor(*s3, g0);
    Check(Acquire(*ps3, claim_old, s3->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_CATALOG_GENERATION_STALE",
          "session 3 claiming the OLD generation: stale");
  }

  // ISOLATION: the receipt's snapshot follows the transaction's isolation profile. A read_committed_statement transaction (owned by
  // session 1 through the loop's own begin) takes its snapshot AT ACQUIRE: it sees the change that session 1's snapshot_stable
  // transaction does not. A profile the native receipt does not know refuses.
  {
    const auto begin_owned = [&](std::uint8_t isolation) {
      srv::NativeLoopBeginRequest begin;
      begin.owner_key = s1->session();
      begin.isolation_profile = isolation;
      begin.identity.principal_uuid = served.bootstrap;
      begin.identity.attachment_uuid = p::Uuid{s1->approval.attachment_uuid};
      begin.identity.session_uuid = s1->session();
      const auto begun = served.loop->SubmitBeginWork(begin).get();
      Check(begun.ok(), "the loop begins a second transaction owned by session 1");
      return begun;
    };
    const auto rc = begin_owned(kReadCommittedStatement);
    auto crc = ContextFor(s1->record->database_path, s1->record->effective_user_uuid, s1->record->session_uuid, rc.begun_uuid, rc.begun_order, g1);
    auto got = Acquire(*ps1, crc, rc.begun_uuid);
    Check(got.ok(), "a read_committed_statement transaction's receipt: " + got.code);
    Check(got.view.visible_committed_high_watermark == change_order && CopyContext(got).catalog_generation_id == g1,
          "taken at acquire: it sees the change (high-water mark and generation)");
    Release(got);
    const auto rr = begin_owned(kRepeatableRead);
    auto crr = ContextFor(s1->record->database_path, s1->record->effective_user_uuid, s1->record->session_uuid, rr.begun_uuid, rr.begun_order, g1);
    Check(Acquire(*ps1, crr, rr.begun_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_ISOLATION_UNSUPPORTED",
          "an isolation profile the native receipt does not know refuses specifically");
    for (const auto& begun : {rc, rr}) {
      const auto done = served.loop->SubmitCompleteWork({s1->session(), begun.begun_order, srv::NativeLoopCompletion::rollback}).get();
      Check(done.ok(), "the extra transaction is rolled back");
    }
  }

  // REFUSALS, each with its own code, each writing nothing (Acquire checks the bytes).
  {
    // no session: a session UUID the loop never approved, with a real transaction's identity
    const auto stranger = NewId();
    auto pss = OpenPublic(s1->record->served_route_owner, s1->record->database_path, s1->record->native_served_facts, s1->record->effective_user_uuid, stranger.bytes);
    auto cs = ContextFor(s1->record->database_path, s1->record->effective_user_uuid, stranger.bytes, s1->approval.transaction_uuid, order1, g0);
    Check(Acquire(*pss, cs, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_SESSION_NOT_OPEN",
          "no session (never approved): NATIVE_SESSION_NOT_OPEN");
    // another session's transaction
    auto foreign = ContextFor(s2->record->database_path, s2->record->effective_user_uuid, s2->record->session_uuid, s1->approval.transaction_uuid, order1, g0);
    Check(Acquire(*ps2, foreign, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_TRANSACTION_NOT_OWNED",
          "session 2 asking for session 1's transaction: NATIVE_TRANSACTION_NOT_OWNED");
    // a transaction that is not in the table
    const auto absent = NewId();
    auto ca = ContextFor(s1->record->database_path, s1->record->effective_user_uuid, s1->record->session_uuid, absent, order1 + 1000, g0);
    Check(Acquire(*ps1, ca, absent).code == "ENGINE.STATEMENT_CONTEXT.TRANSACTION_NOT_ACTIVE", "an unknown transaction: TRANSACTION_NOT_ACTIVE");
    // the caller's order disagrees with the loop's table for that UUID
    auto wrong_order = ContextFor(*s1, g0);
    wrong_order.local_transaction_id = order1 + 1;
    Check(Acquire(*ps1, wrong_order, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_TRANSACTION_ORDER_MISMATCH",
          "a disagreeing local transaction id: NATIVE_TRANSACTION_ORDER_MISMATCH");
    // a different principal in the context
    auto wrong_principal = ContextFor(*s1, g0);
    wrong_principal.principal_uuid = served.service;
    wrong_principal.authorization_context.principal_uuid = served.service;
    Check(Acquire(*ps1, wrong_principal, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.SESSION_CONTEXT_MISMATCH",
          "a context for another principal: SESSION_CONTEXT_MISMATCH");
    // no transaction identity at all
    Check(Acquire(*ps1, c1, p::Uuid{}).code == "ENGINE.STATEMENT_CONTEXT.EXACT_TRANSACTION_REQUIRED", "no transaction identity: EXACT_TRANSACTION_REQUIRED");
    // the caller's facts marker is never trusted: a forged pointer (here a failed-state copy) changes nothing
    {
      auto forged = std::make_shared<api::NativeServedDatabaseFacts>(*served.facts);
      forged->serving_state = api::NativeServedServingState::failed;
      forged->serving_state_cell.reset();
      auto liar = c1;
      liar.native_served_facts = forged;
      auto ok = Acquire(*ps1, liar, s1->approval.transaction_uuid);
      Check(ok.ok(), "a caller-supplied facts pointer is replaced by the handle's: " + ok.code);
      Release(ok);
    }
    // a transaction that ENDED (commit through the loop, no replacement) while its session is still open
    {
      const auto done = served.loop->SubmitCompleteWork({s3->session(), order3, srv::NativeLoopCompletion::commit}).get();
      Check(done.ok() && done.committed, "the loop commits session 3's transaction (no replacement)");
      Check(Acquire(*ps3, c3, s3->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.TRANSACTION_NOT_ACTIVE",
            "a transaction that ended: TRANSACTION_NOT_ACTIVE");
    }
    // a session that ENDED (disconnect: the loop rolls its transaction back and ends the session)
    {
      Disconnect(*s2);
      Check(Acquire(*ps2, c2, s2->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_SESSION_NOT_OPEN",
            "a session that ended: NATIVE_SESSION_NOT_OPEN");
    }
  }

  // BIND / DISPATCH / JOURNAL with a native receipt (session 1's, still live).
  {
    // BIND: the receipt's NAME_RESOLVE executor generation is 0 (not on the native allow-list), and the bind says so with the
    // availability table's OWN code, not as a stale epoch.
    bind::NameResolveBindRequestV1 public_bind;
    public_bind.authenticated_receipt_uuid = r1.view.receipt_uuid.bytes;
    public_bind.occurrence = 1;
    public_bind.resolution_mode = 2;
    public_bind.object_class = 2;
    public_bind.target_name_atoms = {{"definitely_absent_name", false}};
    std::vector<std::uint8_t> exact_bind;
    std::string detail;
    Check(bind::EncodeNameResolveBindRequestV1(public_bind, &exact_bind, &detail), "the canonical bind request encodes: " + detail);
    bind::NameResolveBindRequestV1 decoded;
    Check(bind::DecodeNameResolveBindRequestV1(exact_bind.data(), exact_bind.size(), &decoded, &detail), "and decodes: " + detail);
    bridge::StatementNameResolveBindRequestV1 request;
    request.authenticated_receipt_uuid = r1.view.receipt_uuid;
    request.occurrence = decoded.occurrence;
    request.resolution_mode = decoded.resolution_mode;
    request.object_class = decoded.object_class;
    for (const auto& source : decoded.target_name_atoms) request.target_name_atoms.push_back({source.raw_utf8, source.quoted});
    request.target_name_atoms_sha256 = decoded.target_name_atoms_sha256;
    request.namespace_name_atoms_sha256 = decoded.namespace_name_atoms_sha256;
    request.request_evidence_sha256 = decoded.request_evidence_sha256;
    request.exact_bind_request_bytes = exact_bind;
    bridge::StatementNameResolveBindAckV1 ack;
    sb_engine_result_t result = nullptr;
    const auto before = Image(served.path);
    const auto status = bridge::BindStatementNameResolveAuthorityV1(r1.receipt, &request, &ack, &result);
    const auto code = Code(result);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && ack.failure_code == code,
          "the name-resolve bind with a native receipt refuses with the availability table's own code: " + code);
    Check(Image(served.path) == before, "and wrote nothing");
    // DISPATCH: executing is S4g
    bridge::StatementContextDispatchRequest dispatch;
    dispatch.receipt = r1.receipt;
    result = nullptr;
    const auto dispatched = bridge::DispatchStatementContextReceipt(&dispatch, &result);
    Check(dispatched == SB_ENGINE_STATUS_UNSUPPORTED && Code(result) == "ENGINE.NATIVE.OPERATION_NOT_BUILT",
          "dispatching a native receipt refuses ENGINE.NATIVE.OPERATION_NOT_BUILT (S4g)");
    Check(Image(served.path) == before, "and wrote nothing");
    // JOURNAL: the resolve journal refuses a native receipt's context (Part A Q3: it moves into the single file later)
    auto journal_context = CopyContext(r1);
    journal_context.statement_metadata_snapshot_engine_owned = true;
    journal_context.trace_tags.push_back("private_name_resolve_journal");
    api::SblrNameResolveJournalKeyV1 key;
    key.database_uuid = served.facts->database_uuid.bytes;
    key.statement_receipt_uuid = r1.view.receipt_uuid.bytes;
    key.resolution_uuid = NewId().bytes;
    key.catalog_generation = g0;
    key.security_epoch = 1;
    key.resource_epoch = 1;
    for (const auto& looked : {api::LookupSblrNameResolveJournalV1(journal_context, key), api::EnsureSblrNameResolveJournalV1(journal_context, key)}) {
      Check(!looked.ok && looked.diagnostic.code == "ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED", "the name-resolve journal refuses a native receipt's context");
      Check(std::any_of(looked.diagnostic.fields.begin(), looked.diagnostic.fields.end(),
                        [](const auto& f) { return f.key == "registry_family" && f.value == "sblr_name_resolve_journal"; }),
            "with the registry family as a structured field");
    }
    Check(Image(served.path) == before, "and the journal wrote nothing");
  }
  Release(r2);
  Release(r3);
  Release(r1);

  // FAILED: the loop's writer fails on the next write; the database becomes `failed`; a receipt is refused (nothing written).
  {
    class FailFirstWriteSink final : public d::CarefulWriteSink {
     public:
      using D = d::BoundedIoResult;
      explicit FailFirstWriteSink(d::CarefulWriteSink& inner) : inner_(inner) {}
      D WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
        D r;
        r.operation = d::BoundedIoOperation::write;
        r.error = d::BoundedIoError::native_failure;
        r.offset = offset;
        r.requested_bytes = bytes.size();
        r.native_error = EIO;
        r.native_attempted = true;
        return r;
      }
      D ReadAt(p::u64 offset, std::span<p::byte> bytes) override { return inner_.ReadAt(offset, bytes); }
      D Sync() override { return inner_.Sync(); }
      bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

     private:
      d::CarefulWriteSink& inner_;
    };
    Check(served.facts->CurrentServingState() == api::NativeServedServingState::serving, "serving before the failure");
    srv::SetNativeTransactionSinkWrapperForTest([](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
      return std::make_unique<FailFirstWriteSink>(inner);
    });
    {
      // The approval whose first write fails is refused (and the database is failed afterwards).
      srv::ServerSessionRegistry registry;
      const auto connection = sbps::MakeUuidV7Bytes();
      const auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(connection, kBootstrapName, kBootstrapPassword), connection);
      srv::NativeLoginThrottle().Clear();
      const auto refused = srv::HandleAuthHandoff(&registry, served.started.state, frame);
      Check(!refused.accepted && !refused.native_approval.present && registry.sessions_by_uuid.empty(), "the next approval's write failed: refused");
    }
    Check(served.facts->CurrentServingState() == api::NativeServedServingState::failed, "the live state is failed");
    Check(Acquire(*ps1, c1, s1->approval.transaction_uuid).code == "ENGINE.STATEMENT_CONTEXT.NATIVE_DATABASE_FAILED",
          "a receipt on the failed database: NATIVE_DATABASE_FAILED");
    srv::SetNativeTransactionSinkWrapperForTest(nullptr);
  }
  ps3.reset();
  ps2.reset();
  ps1.reset();
  Disconnect(*s3);
  std::printf("RECEIPT g0=%llu change_order=%llu order1=%llu order2=%llu order3=%llu\n", static_cast<unsigned long long>(g0),
              static_cast<unsigned long long>(change_order), static_cast<unsigned long long>(order1), static_cast<unsigned long long>(order2),
              static_cast<unsigned long long>(order3));
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
  // The ground truth is read OFFLINE, before the server owns the file.
  std::optional<Offline> offline;
  if (state != "restricted") offline = ReadOffline(path);
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
  int rc = 0;
  if (state == "restricted") {
    // A RESTRICTED database (a committing slot at open): login is allowed (Q309) and no session or transaction is, so no receipt.
    Check(served.facts->CurrentServingState() == api::NativeServedServingState::restricted, "the database is restricted");
    const auto before = Image(path);
    const auto connection = sbps::MakeUuidV7Bytes();
    srv::ServerSessionRegistry registry;
    const auto frame = MakeFrame(sbps::MessageType::kAuthHandoff, AuthPayload(connection, kBootstrapName, kBootstrapPassword), connection);
    const auto handoff = srv::HandleAuthHandoff(&registry, served.started.state, frame);
    Check(!handoff.accepted && !handoff.diagnostics.empty() && handoff.diagnostics.front().code == "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY" &&
              registry.sessions_by_uuid.empty(),
          "the login is allowed but the session is refused with the Q299 code: no session exists");
    const auto owner = srv::ServedRouteOwnerFor(served.started.state, path.string());
    Check(owner != nullptr, "the served owner exists");
    const auto user = NewId(), session = NewId(), transaction = NewId();
    auto ps = OpenPublic(owner, path.string(), served.facts, user.bytes, session.bytes);
    auto context = ContextFor(path.string(), user.bytes, session.bytes, transaction, 5, 1);
    const auto got = Acquire(*ps, context, transaction);
    Check(!got.ok() && got.status == SB_ENGINE_STATUS_UNSUPPORTED && got.code == "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY",
          "the receipt on a restricted database refuses the Q299 code (before the transaction identity is even looked up): " + got.code);
    Check(Image(path) == before, "and nothing was written");
    ps.reset();
  } else {
    rc = ReceiptState(state, *offline);
  }
  if (state == "trace") Mark("end");
  (void)served.loop->Close();
  served.facts.reset();
  served.started = {};
  if (rc == 0) std::cout << "RECEIPT_OK checks=" << checks << " state=" << state << std::endl;
  return rc;
}

std::string Capture(const std::string& command, int* status) {
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

void RunState(const std::string& self, const fs::path& root, std::uint32_t page_size, const std::string& state) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto dir = root / (state + std::to_string(page_size));
  fs::create_directory(dir);
  const auto path = dir / "receipt.sdb";
  int status = 0;
  const std::string material = SB_NATIVE_LOGIN_MATERIAL_TEST_BIN;
  const auto created = Capture("'" + material + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
  Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
  if (state == "restricted")
    Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' committing", &status).find("FIXTURE ok") != std::string::npos && status == 0,
          tag + "the native_serving writer left a committing slot");
  const auto start = std::chrono::steady_clock::now();
  const auto served_out = Capture("'" + self + "' serve '" + path.string() + "' " + state + " " + Field(created, "database") + " " + Field(created, "bootstrap") + " " +
                                      Field(created, "service"),
                                  &status);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  Check(status == 0 && served_out.find("RECEIPT_OK") != std::string::npos, tag + state + ": serve child: " + served_out.substr(0, 3000));
  Check(served_out.find(kBootstrapPassword) == std::string::npos && created.find(kBootstrapPassword) == std::string::npos, tag + "no test password in any child output");
  std::printf("%s%s: %.1f s\n", tag.c_str(), state.c_str(), seconds);
  fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if (argc == 4 && std::string(argv[1]) == "pages") {
    const auto file = off::ReadFile(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
    const auto list = [](const std::vector<p::u64>& pages) {
      std::string out;
      for (std::size_t i = 0; i < pages.size(); ++i) out += (i ? "," : "") + std::to_string(pages[i]);
      return out;
    };
    std::printf("PAGES state=%s header=%s ledger=%s next=%llu\n", list(file.state_pages).c_str(), list(file.header_pages).c_str(),
                list(file.ledger_pages).c_str(), static_cast<unsigned long long>(file.counters.next_transaction));
    return 0;
  }
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    const auto run_size = [&](std::uint32_t size) {
      for (const char* state : {"receipt", "restricted"}) RunState(self, artifacts.path(), size, state);
    };
    if (argc == 3) run_size(static_cast<std::uint32_t>(std::stoul(argv[2])));
    else
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u}) run_size(size);
    artifacts.Cleanup();
    std::printf("native_receipt: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
