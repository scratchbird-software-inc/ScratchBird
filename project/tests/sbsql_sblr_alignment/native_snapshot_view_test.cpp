// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SNAPSHOT_POINT_VIEW_TEST (owner 2026-11-21: the true snapshot-view reader entry, before S4e).
// The snapshot-point scope of the database loop captures the TIP (Next, the active and in-doubt slots) as a visibility
// snapshot VALUE, and the credential read and the names read made inside the scope decide every row version's
// visibility from that value (the visibility reader's ...At entries), NOT from the inventory at read time. Every case
// runs on a PRODUCTION-created database served by StartHostedEngine; the changes between a scope's capture and its read
// are made on the loop thread through the loop's own writer (BEGIN, change event, WriteCatalogSuccessorVersion /
// WriteCatalogInsertVersions, COMMIT / ROLLBACK) via the guarded test seam RunWithTransactionSinkOnLoopThreadForTest.
// Modes (a child process each):
//   create <path> <page_size>              engine-API native create (bootstrap credential = kBootstrapPassword)
//   serve <path> <database> <bootstrap> <service> [restricted]
//                                          serve and run every case; prints SNAPVIEW_OK; the strace half brackets each
//                                          region with "MARK <name>" on stderr (change_* regions write, read_* never do)
// With no argument the driver runs every size; "size <bytes>" one page size. The strace half is native_snapshot_view_strace.py.

#include "engine/internal_api/catalog/name_registry.hpp"
#include "engine/internal_api/catalog/name_resolution_api.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "catalog_identifier_text_rule.hpp"
#include "engine/internal_api/security/agent_password_derivation.hpp"
#include "hash_digest.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "local_password_verifier.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "filespace_page_zero.hpp"
#include "catalog_authorization_policy.hpp"
#include "catalog_identifier_lookup_key.hpp"
#include "catalog_identifier_profile.hpp"
#include "catalog_language.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_native_security_record.hpp"
#include "catalog_record_codec.hpp"
#include "native_catalog_version_writer.hpp"
#include "transaction_inventory_writer.hpp"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/native_database_loop.hpp"
#include "server/process_crypto_owner.hpp"
#include "uuid.hpp"
#include "../support/catalog_change_event_fixture.hpp"
#include "../support/native_database_create_fixture.hpp"
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
#include <thread>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace db = scratchbird::storage::database;
namespace srv = scratchbird::server;
namespace mem = scratchbird::core::memory;
namespace cat = scratchbird::core::catalog;
namespace fs = std::filesystem;

namespace tip = scratchbird::storage::page;
namespace api = scratchbird::engine::internal_api;

constexpr std::string_view kBootstrapPassword = "S4f-Bootstrap-Pw-3c88d10e6b";
constexpr std::string_view kWrongPassword = "S4f-Wrong-Pw-b0a4e9d217";

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

p::Uuid NewId() {
  const auto id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(id.has_value(), "an identity");
  return *id;
}

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
  BoundMemory(const p::Uuid& database, std::uint64_t bytes)
      : manager(Policy(bytes)), binding{database, NewId(), NewId(), NewId()} {
    mem::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = mem::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = mem::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "native name resolve test read";
    r.purpose = "the test's own offline names read";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{mem::HierarchicalMemoryScopeKind::process, {}, NewId().bytes},
                     {mem::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "native name resolve test";
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

void ProcessMemory() {
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  Check(policy.ok() && mem::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok(), "process memory");
}

// ---- the offline read: the ground truth the served resolution is compared with ----------------------------------
// One visible name entry of the file, read by the production visibility reader as an independent read-only reader
// (no server). The text is the test's own copy, used to ask the question and never printed.
struct NameFact {
  p::Uuid object, scope, language, entry_uuid, row_uuid, version_uuid;
  std::uint16_t kind = 0;
  bool quoted = false, default_for_object = false, default_for_language = false;
  cat::CatalogNameClass name_class = cat::CatalogNameClass::primary;
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
  p::Uuid security_schema;                 // the authorization policy body's user schema
  std::vector<NameFact> names;             // live entries only
  std::map<p::Uuid, DefinitionFact> security;  // role 4 rows by object
  std::vector<p::Uuid> principals;         // kind user_account
  std::vector<d::FilespaceRootReference> roots;
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
    const auto config = db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(lease, 2, 5, budget, prepared.workspace);
    Check(config.ok(), "the configuration relation reads");
    for (const auto& row : config.rows) {
      const auto& m = row.metadata;
      if (row.metadata.record.header.deleted || !cat::IsCatalogAuthorizationPolicyPayload(m.record.payload)) continue;
      const auto body = cat::DecodeCatalogAuthorizationPolicy(m.record.payload);
      Check(body.record.has_value(), "the authorization policy body decodes");
      out.security_schema = body.record->user_schema_uuid;
    }
    Check(!out.security_schema.is_nil(), "the file has an authorization policy body naming the security schema");
  }
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
      if (row.metadata.record.header.kind == cat::CatalogRecordKind::user_account) out.principals.push_back(f.object);
      out.security.emplace(f.object, std::move(f));
    }
  }
  {
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
      f.entry_uuid = e->name_entry_uuid.value;
      f.row_uuid = row.metadata.record.header.row_uuid.value;
      f.version_uuid = row.version_uuid;
      f.kind = static_cast<std::uint16_t>(e->object_kind);
      f.quoted = e->was_quoted;
      f.default_for_object = e->default_for_object;
      f.default_for_language = e->default_for_language;
      f.name_class = e->name_class;
      f.text = std::string(e->raw_name_text);
      f.entry = cat::MaterializeCatalogNameEntry(*e);
      f.metadata = cat::MaterializeCatalogMetadataVersion(row.metadata);
      out.names.push_back(std::move(f));
    }
  }
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return out;
}

const NameFact* EntryOf(const Offline& o, const p::Uuid& object, const std::string& text) {
  for (const auto& n : o.names)
    if (n.object == object && n.text == text) return &n;
  return nullptr;
}
const NameFact* FirstEntryOf(const Offline& o, const p::Uuid& object) {
  for (const auto& n : o.names)
    if (n.object == object) return &n;
  return nullptr;
}

// ---- create -----------------------------------------------------------------------------------------------------
constexpr std::string_view kBootstrapName = "fixture_owner";
constexpr std::string_view kServiceName = "fixture_service";

int CreateMode(const fs::path& path, std::uint32_t page_size) {
  std::string verifier;
  if (!scratchbird::core::crypto::DeriveLocalPasswordVerifier(kBootstrapPassword, &verifier)) return 1;
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "native_name_resolve";
  request.bootstrap_principal_name = std::string(kBootstrapName);
  request.service_principal_name = std::string(kServiceName);
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  request.bootstrap_credential_fingerprint = verifier;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) {
    std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
    return 1;
  }
  p::Uuid binding;
  if (!scratchbird::tests::NativeFixtureReceiptUuid(made.api, "service_runtime_binding_uuid", &binding)) {
    std::cerr << "the create receipt has no service_runtime_binding_uuid\n";
    return 1;
  }
  std::cout << "CREATED database=" << Hex(made.database_uuid) << " bootstrap=" << Hex(made.bootstrap_principal_uuid)
            << " service=" << Hex(made.service_principal_uuid) << " binding=" << Hex(binding) << std::endl;
  return 0;
}

// ---- the loop's own writer, through the guarded seam ----------------------------------------------------------------
struct Writing {
  d::CarefulWriteSink* sink = nullptr;
  db::CatalogVersionWriterBinding binding;
  tip::TransactionHeaderRecord header;
  std::uint64_t order = 0;
  p::Uuid event;
};
std::vector<db::CatalogNewPageIdentity> NewPages() {
  std::vector<db::CatalogNewPageIdentity> pages;
  for (int i = 0; i < 8; ++i) pages.push_back({NewId(), NewId()});
  return pages;
}
struct NewName {
  std::string text;
  bool quoted = false;
  std::optional<p::Uuid> language;  // absent: the base entry's
  cat::CatalogNameClass name_class = cat::CatalogNameClass::alias;
  bool default_for_language = false;
};
db::CatalogInsertRow EntryRow(Writing& w, const NameFact& base, const NewName& n) {
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
  row.metadata.creator_transaction_uuid = {p::UuidKind::transaction, w.header.transaction_uuid};
  row.metadata.creator_local_transaction_id = w.order;
  row.metadata.definition_version = 1;
  auto e = *base.entry;
  e.raw_name_text = n.text;
  e.was_quoted = n.quoted;
  e.quote_style = n.quoted ? cat::CatalogNameQuoteStyle::double_quote : cat::CatalogNameQuoteStyle::none;
  e.requires_exact_match = n.quoted;
  e.name_entry_uuid = id;
  e.name_class = n.name_class;
  if (n.language) e.language_uuid = cat::CatalogLanguageIdentity(*n.language);
  e.default_for_object = false;
  e.default_for_language = n.default_for_language;
  e.created_transaction_uuid = row.metadata.creator_transaction_uuid;
  e.catalog_generation_id = row.metadata.catalog_generation;
  e.lifecycle_state = cat::CatalogNameLifecycle::active;
  e.dropped_transaction_uuid.reset();
  row.name = cat::CatalogNamePayload{e};
  Check(cat::CatalogNamePayloadMatchesMetadata(*row.name, row.metadata), "the new entry agrees with its metadata");
  return row;
}
void Insert(Writing& w, std::vector<db::CatalogInsertRow> rows) {
  db::CatalogInsertRequest r;
  r.transaction_order = w.order;
  r.transaction_uuid = w.header.transaction_uuid;
  r.rows = std::move(rows);
  r.new_pages = NewPages();
  const auto written = db::WriteCatalogInsertVersions(*w.sink, w.binding, r);
  if (!written.ok()) std::cerr << "insert refused: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
  Check(written.ok(), "the name rows are inserted in place");
}
// ---- the protected password (only for the cross-check with the S4a login-material read) ---------------------------
struct PasswordFactory {
  mem::MemoryManager manager{mem::DefaultLocalEngineMemoryPolicy()};
  mem::MemoryTag tag;
  explicit PasswordFactory(const p::Uuid& database) {
    tag.category = mem::MemoryCategory::core_runtime;
    tag.purpose = "native name resolve test password";
    tag.callsite = "native_name_resolve_test";
    tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = database.bytes;
  }
  mem::ScopedProtectedBuffer Make(std::string_view bytes) {
    mem::ProtectedMemoryRequest request;
    request.bytes = std::max<std::size_t>(bytes.size(), 1);
    request.tag = tag;
    request.material_class = "native name resolve test password";
    request.platform_policy = mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto allocated = manager.AllocateProtected(request);
    Check(allocated.ok(), "protected memory for the test password");
    std::memcpy(allocated.buffer.data(), bytes.data(), bytes.size());
    return std::move(allocated.buffer);
  }
};


// ---- the case helpers ---------------------------------------------------------------------------------------------
using LoginError = srv::NativeLoginMaterialError;
constexpr std::uint8_t kReadCommittedStatement = 2;  // IsolationProfileKind::read_committed_statement

constexpr std::string_view kNameAfterCapture = "Snap_Name_Committed_After_Capture";
constexpr std::string_view kNameActiveAtCapture = "Snap_Name_Active_At_Capture";
constexpr std::string_view kNameRolledBack = "Snap_Name_Rolled_Back_Before_Capture";
constexpr std::string_view kNameRolledBackLater = "Snap_Name_Rolled_Back_After_Capture";

// The strace half splits the run at these marks (stderr, unbuffered): change_* regions write, read_* regions never do.
void Mark(const std::string& name) {
  const std::string line = "MARK " + name + "\n";
  (void)!::write(2, line.data(), line.size());
}
std::string ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

struct Ctx {
  srv::NativeDatabaseLoop* loop = nullptr;
  PasswordFactory* passwords = nullptr;
  p::Uuid database, bootstrap;
  fs::path path;
};

// What the two reads of a scope (or of the live entries) saw.
struct Seen {
  LoginError login = LoginError::invalid_request;
  bool names_ok = false;
  std::set<std::string> names;
};
Seen Observe(Ctx& c, std::string_view password = kBootstrapPassword) {
  Seen out;
  srv::NativeLoopLoginRequest request;
  request.principal_uuid = c.bootstrap;
  request.password = c.passwords->Make(password);
  request.password_length = password.size();
  out.login = c.loop->ReadLoginMaterialOnLoopThread(std::move(request)).error;
  const auto names = c.loop->ReadNamesSnapshotOnLoopThread();
  out.names_ok = names.ok();
  for (const auto& row : names.rows) out.names.insert(row.raw_name_text);
  return out;
}
bool Has(const Seen& s, std::string_view name) { return s.names.count(std::string(name)) != 0; }

// One loop transaction that changes the catalog: BEGIN by the loop's own writer, its change event, then the writes.
struct Change {
  p::Uuid key;
  Writing w;
  bool begun = false;
};
db::CatalogVersionWriterBinding BindingOf(const Offline& off) {
  std::optional<d::FilespaceRootReference> catalog_root, state_root, allocation_root, configuration_root, security_root;
  for (const auto& root : off.roots) {
    if (root.kind == 2) catalog_root = root;
    if (root.kind == 4) state_root = root;
    if (root.kind == 3) allocation_root = root;
    if (root.kind == 6) configuration_root = root;
    if (root.kind == 7) security_root = root;
  }
  Check(catalog_root && state_root && allocation_root && configuration_root && security_root, "page zero names the roots");
  db::CatalogVersionWriterBinding b;
  b.database_uuid = off.database;
  b.catalog_root = *catalog_root;
  b.state_root = *state_root;
  b.configuration_root = *configuration_root;
  b.security_root = *security_root;
  b.allocation_root = *allocation_root;
  return b;
}
template <class Body>
void WithSink(Ctx& c, Change& ch, Body&& body) {
  Check(c.loop->RunWithTransactionSinkOnLoopThreadForTest([&](d::CarefulWriteSink& sink) {
          ch.w.sink = &sink;
          body(ch.w);
          ch.w.sink = nullptr;
        }),
        "the loop's write door opened for the test");
}
Change BeginChange(Ctx& c, const Offline& off) {
  Change ch;
  ch.key = NewId();
  const auto begun = c.loop->BeginWorkOnLoopThread({ch.key, false, kReadCommittedStatement, {}});
  Check(begun.ok(), "BEGIN of a change transaction by the loop's writer");
  ch.begun = true;
  ch.w.header.transaction_uuid = begun.begun_uuid;
  ch.w.order = begun.begun_order;
  ch.w.binding = BindingOf(off);
  ch.w.event = NewId();
  WithSink(c, ch, [&](Writing& w) {
    const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto written = scratchbird::tests::catalog_change_event::WriteEvent(*w.sink, w.binding, w.order, w.header.transaction_uuid,
                                                                            w.event, NewId(), NewId(), NewId(), now_us);
    Check(written.ok(), "the transaction's catalog.metadata_change event");
  });
  return ch;
}
// A successor of the bootstrap principal's security row; `active` set = the same row with that active flag, absent =
// the "touch" (nothing else changes).
DefinitionFact Successor(Ctx& c, Change& ch, const DefinitionFact& f, std::optional<bool> active) {
  DefinitionFact next;
  WithSink(c, ch, [&](Writing& w) {
    db::CatalogVersionRequest r;
    r.role = 4;
    r.transaction_order = w.order;
    r.transaction_uuid = w.header.transaction_uuid;
    r.row_uuid = f.row_uuid;
    r.predecessor_version_uuid = f.version_uuid;
    r.version_uuid = NewId();
    r.metadata = f.metadata;
    if (active) {
      auto record = cat::DecodeNativeSecurityRecord(f.metadata.record.header.kind, f.metadata.record.payload);
      Check(record.ok() && record.record->principal(), "the principal row decodes");
      auto principal = *record.record->principal();
      principal.active = *active;
      const auto payload = cat::EncodeNativeSecurityPrincipal(principal);
      Check(payload.ok(), "the successor payload encodes under the production codec");
      r.metadata.record.payload.assign(payload.bytes.begin(), payload.bytes.end());
    }
    ++r.metadata.definition_version;
    r.metadata.creator_transaction_uuid = {p::UuidKind::transaction, w.header.transaction_uuid};
    r.metadata.creator_local_transaction_id = w.order;
    r.metadata.audit_uuid = {p::UuidKind::object, w.event};
    const auto written = db::WriteCatalogSuccessorVersion(*w.sink, w.binding, r);
    if (!written.ok()) std::cerr << "successor refused: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
    Check(written.ok(), "the catalog version writer writes the successor in place");
    next = f;
    next.version_uuid = r.version_uuid;
    next.metadata = r.metadata;
  });
  return next;
}
void AddName(Ctx& c, Change& ch, const NameFact& base, std::string_view text) {
  WithSink(c, ch, [&](Writing& w) {
    std::vector<db::CatalogInsertRow> rows;
    rows.push_back(EntryRow(w, base, {std::string(text), false, std::nullopt, cat::CatalogNameClass::alias, false}));
    Insert(w, std::move(rows));
  });
}
void Complete(Ctx& c, Change& ch, bool commit) {
  const auto done = c.loop->CompleteWorkOnLoopThread(
      {ch.key, ch.w.order, commit ? srv::NativeLoopCompletion::commit : srv::NativeLoopCompletion::rollback});
  Check(done.ok() && done.committed == commit, commit ? "COMMIT of the change transaction" : "ROLLBACK of the change transaction");
}
srv::NativeLoopReadScopeResult Scope(Ctx& c, const std::function<void(const srv::NativeLoopReadTransaction&)>& work) {
  const auto result = c.loop->RunInEngineReadTransaction(work, srv::NativeLoopReadScopeMode::snapshot_point);
  Check(result.ok() && result.mode == srv::NativeLoopReadScopeMode::snapshot_point, "the snapshot-point scope completed");
  return result;
}
const DefinitionFact& PrincipalRow(const Offline& off, const p::Uuid& principal) {
  const auto found = off.security.find(principal);
  Check(found != off.security.end(), "the principal's security row is in the file");
  return found->second;
}
const NameFact& BaseName(const Offline& off, const p::Uuid& principal) {
  const auto* base = EntryOf(off, principal, std::string(kBootstrapName));
  Check(base != nullptr, "the principal's create-time name entry is visible");
  return *base;
}

// ---- the cases ------------------------------------------------------------------------------------------------------
// The file cannot be opened a second time while it is served, so the principal's current COMMITTED row is tracked here
// (the successor each committed change wrote), starting from the offline read made before the open.
void Cases(Ctx& c, const Offline& off, DefinitionFact cur) {
  const auto login_ok = [](const Seen& s) { return s.login == LoginError::none; };
  const auto not_active = [](const Seen& s) { return s.login == LoginError::principal_not_active; };

  // 0. The baseline: nothing changed; the scope's reads answer as the live ones do.
  {
    Mark("read_0");
    const auto image = ReadAll(c.path);
    Seen in_scope;
    const auto scope = Scope(c, [&](const srv::NativeLoopReadTransaction& t) {
      Check(t.mode == srv::NativeLoopReadScopeMode::snapshot_point && t.order == 0, "a snapshot point, no transaction");
      Check(t.snapshot_visible_through + 1 == t.snapshot_next_transaction, "the view is visible through Next - 1");
      Check(t.snapshot_active_count == 0 && t.snapshot_in_doubt_count == 0, "a clean file captures no active or in-doubt slot");
      in_scope = Observe(c);
    });
    Check(login_ok(in_scope) && in_scope.names_ok && Has(in_scope, kBootstrapName), "baseline: the scope authenticates and lists the create's name");
    Check(!Has(in_scope, kNameAfterCapture), "baseline: no forged name");
    Check(ReadAll(c.path) == image, "baseline: the reads wrote nothing");
    (void)scope;
  }

  // 1. STABILITY (credential): a commit that disables the principal BETWEEN the capture and the read. The scope still
  //    authenticates; the no-view entry and a scope captured AFTER the commit see the change.
  {
    Seen in_scope;
    std::string image;
    Scope(c, [&](const srv::NativeLoopReadTransaction& t) {
      Check(t.snapshot_active_count == 0, "case 1: nothing active at the capture");
      Mark("change_1");
      auto ch = BeginChange(c, off);
      const auto next = Successor(c, ch, cur, false);
      Complete(c, ch, true);
      cur = next;
      image = ReadAll(c.path);
      Mark("read_1");
      in_scope = Observe(c);
      Check(ReadAll(c.path) == image, "case 1: the scope's reads wrote nothing (page by page, the whole file)");
    });
    Check(login_ok(in_scope), "case 1: the capture predates the disabling commit: the scope still authenticates");
    Mark("read_1_live");
    const auto live = Observe(c);
    Check(not_active(live), "case 1: the NO-VIEW entry (read-time inventory) sees the principal disabled");
    Mark("read_1_after");
    Seen after;
    Scope(c, [&](const srv::NativeLoopReadTransaction&) { after = Observe(c); });
    Check(not_active(after), "case 1: a scope captured AFTER the commit sees the principal disabled");
  }

  // 2. STABILITY (names): a name row committed between the capture and the read is not seen by the scope.
  {
    Seen in_scope;
    Scope(c, [&](const srv::NativeLoopReadTransaction&) {
      Mark("change_2");
      auto ch = BeginChange(c, off);
      const auto next = Successor(c, ch, cur, std::nullopt);
      AddName(c, ch, BaseName(off, c.bootstrap), kNameAfterCapture);
      Complete(c, ch, true);
      cur = next;
      Mark("read_2");
      in_scope = Observe(c);
    });
    Check(in_scope.names_ok && Has(in_scope, kBootstrapName) && !Has(in_scope, kNameAfterCapture),
          "case 2: the scope does not see the name committed after its capture");
    Check(not_active(in_scope), "case 2: and still sees the principal as it was at the capture (disabled by case 1)");
    Mark("read_2_live");
    const auto live = Observe(c);
    Check(Has(live, kNameAfterCapture), "case 2: the no-view names read sees the new name");
    Mark("read_2_after");
    Seen after;
    Scope(c, [&](const srv::NativeLoopReadTransaction&) { after = Observe(c); });
    Check(Has(after, kNameAfterCapture), "case 2: a scope captured AFTER the commit sees the new name");
  }

  // 3. A transaction ACTIVE at the capture that commits before the read stays invisible: it re-enables the principal and
  //    adds a name. (Its slot is in the active list; it is committed by the time the read runs.)
  {
    Mark("change_3");
    auto ch = BeginChange(c, off);
    const auto next3 = Successor(c, ch, cur, true);
    AddName(c, ch, BaseName(off, c.bootstrap), kNameActiveAtCapture);
    Seen in_scope;
    Scope(c, [&](const srv::NativeLoopReadTransaction& t) {
      Check(t.snapshot_active_count == 1, "case 3: the capture lists the transaction active at the capture");
      Mark("change_3_commit");
      Complete(c, ch, true);
      cur = next3;
      Mark("read_3");
      in_scope = Observe(c);
    });
    Check(not_active(in_scope), "case 3: the committed-later transaction's re-enabling is invisible to the scope");
    Check(in_scope.names_ok && !Has(in_scope, kNameActiveAtCapture), "case 3: and its name is invisible");
    Mark("read_3_live");
    const auto live = Observe(c);
    Check(login_ok(live) && Has(live, kNameActiveAtCapture), "case 3: the no-view entry sees both");
    // (c) committed BEFORE the capture: visible.
    Mark("read_3_after");
    Seen after;
    Scope(c, [&](const srv::NativeLoopReadTransaction& t) {
      Check(t.snapshot_active_count == 0, "case 3: nothing active at the later capture");
      after = Observe(c);
    });
    Check(login_ok(after) && Has(after, kNameActiveAtCapture) && Has(after, kNameAfterCapture),
          "case 3: row versions of transactions committed before the capture are visible");
  }

  // 4. Dead versions: a transaction rolled back BEFORE the capture never shows; one rolled back after the capture
  //    (it was active at it) never shows either.
  {
    Mark("change_4");
    auto dead = BeginChange(c, off);
    (void)Successor(c, dead, cur, false);
    AddName(c, dead, BaseName(off, c.bootstrap), kNameRolledBack);
    Complete(c, dead, false);
    Seen before_capture;
    Scope(c, [&](const srv::NativeLoopReadTransaction& t) {
      Check(t.snapshot_active_count == 0, "case 4: the rolled-back transaction is not active at the capture");
      Mark("read_4");
      before_capture = Observe(c);
    });
    Check(login_ok(before_capture) && !Has(before_capture, kNameRolledBack),
          "case 4: a version written by a transaction rolled back before the capture is never visible");
    Mark("change_4_late");
    auto late = BeginChange(c, off);
    (void)Successor(c, late, cur, false);
    AddName(c, late, BaseName(off, c.bootstrap), kNameRolledBackLater);
    Seen after_rollback;
    Scope(c, [&](const srv::NativeLoopReadTransaction& t) {
      Check(t.snapshot_active_count == 1, "case 4: the second transaction is active at the capture");
      Mark("change_4_rollback");
      Complete(c, late, false);
      Mark("read_4_late");
      after_rollback = Observe(c);
    });
    Check(login_ok(after_rollback) && !Has(after_rollback, kNameRolledBackLater),
          "case 4: a version of a transaction active at the capture and rolled back before the read is never visible");
    Mark("read_4_live");
    const auto live = Observe(c);
    Check(login_ok(live) && !Has(live, kNameRolledBack) && !Has(live, kNameRolledBackLater),
          "case 4: the no-view entry never shows the dead versions either");
  }
  Mark("end");
}

// ---- the served child -----------------------------------------------------------------------------------------------
int ServeMode(const fs::path& path, const p::Uuid& database, const p::Uuid& bootstrap, const p::Uuid& service,
              bool restricted) {
  const auto directory = path.parent_path();
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !mem::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() ||
      !crypto.Start(config, policy).ok()) {
    std::fprintf(stderr, "process setup failed\n");
    return 3;
  }
  config.database_default_path = path;
  const auto offline = ReadOffline(path);  // before the server owns the file (a served file cannot be opened again)
  Check(offline.database == database, "the file's database UUID is the create's");
  const auto before = Snapshot(directory);
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  Check(started.ok(), "the server serves the file: " + (started.diagnostics.empty() ? std::string() : started.diagnostics.front().code));
  Check(!started.state.database_runtimes.empty() && started.state.database_runtimes.front() &&
            started.state.database_runtimes.front()->native_loop,
        "the hosted state holds the native loop");
  auto& loop = *started.state.database_runtimes.front()->native_loop;
  Check(std::string(srv::NativeServingStateName(loop.serving_state())) == (restricted ? "restricted" : "serving"),
        "the database's serving state");
  Check(!loop.on_loop_thread(), "the test is not on the loop thread");
  PasswordFactory passwords(database);
  Ctx ctx;
  ctx.loop = &loop;
  ctx.passwords = &passwords;
  ctx.database = database;
  ctx.bootstrap = bootstrap;
  ctx.path = path;
  (void)service;
  if (restricted) {
    // A committing slot (left by the native_serving writer) is IN DOUBT at the capture: the capture lists it, and the
    // credential read still works at the snapshot point (restricted allows login; only begin is refused).
    Seen seen;
    const auto scope = loop.RunInEngineReadTransaction([&](const srv::NativeLoopReadTransaction& t) {
      Check(t.snapshot_in_doubt_count == 1 && t.snapshot_active_count == 0, "the committing slot is the one in-doubt entry of the capture");
      seen = Observe(ctx);
    }, srv::NativeLoopReadScopeMode::snapshot_point);
    Check(scope.ok(), "the restricted snapshot scope completed");
    Check(seen.login == LoginError::none && seen.names_ok, "restricted: the scope authenticates and reads names at the view");
  } else {
    Check(loop.RunOnLoopThreadForTest([&] { Cases(ctx, offline, PrincipalRow(offline, bootstrap)); }), "the loop ran the cases");
  }
  Check(loop.Close().ok, "the loop closes");
  if (restricted) Check(Snapshot(directory) == before, "restricted: the directory and every file's bytes are unchanged");
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  std::cout << "SNAPVIEW_OK checks=" << checks << " mode=" << (restricted ? "restricted" : "serving") << std::endl;
  return 0;
}

// ---- driver ---------------------------------------------------------------------------------------------------------
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

void OneSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  for (const bool restricted : {false, true}) {
    const std::string state = restricted ? "restricted" : "serving";
    const auto dir = root / (state + std::to_string(page_size));
    fs::create_directory(dir);
    const auto path = dir / "snap.sdb";
    int status = 0;
    const auto created = Capture("'" + self + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
    if (restricted)
      Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' committing", &status).find("FIXTURE ok") != std::string::npos && status == 0,
            tag + "the native_serving writer left a committing slot");
    const auto start = std::chrono::steady_clock::now();
    const auto served = Capture("'" + self + "' serve '" + path.string() + "' " + Field(created, "database") + " " +
                                    Field(created, "bootstrap") + " " + Field(created, "service") + (restricted ? " restricted" : ""),
                                &status);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(status == 0 && served.find("SNAPVIEW_OK") != std::string::npos, tag + state + ": serve child: " + served.substr(0, 3000));
    for (const auto& output : {created, served})
      Check(output.find(kBootstrapPassword) == std::string::npos && output.find(kWrongPassword) == std::string::npos,
            tag + state + ": no test password in any child output");
    std::printf("%s%s: %.1f s\n", tag.c_str(), state.c_str(), seconds);
    fflush(stdout);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc >= 6 && std::string(argv[1]) == "serve")
    return ServeMode(argv[2], FromHex(argv[3]), FromHex(argv[4]), FromHex(argv[5]), argc == 7 && std::string(argv[6]) == "restricted");
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (argc == 3) OneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])));
    else
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u}) OneSize(self, artifacts.path(), size);
    artifacts.Cleanup();
    std::printf("native_snapshot_view: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
