// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_NAME_RESOLVE_TEST (thin-slice S4b, plan 40; owner Q306 = 1, Q309, 2026-11-19/20). The minimal,
// read-only, resolve-only names reader: NativeDatabaseLoop::SubmitResolveName resolves a name given AS DATA to the
// object UUID of the committed names relation of a served native database, or refuses with a specific code.
//
// Every case runs against a PRODUCTION-created database (EngineCreateLifecycle through the native create fixture).
// Extra names (a second language, a quoted name, a case twin, a dropped name, an uncommitted name) are written by the
// production storage writers (BEGIN, change event, a definition touch, WriteCatalogInsertVersions /
// WriteCatalogSuccessorVersion, COMMIT): the engine has no statement that adds a name yet. Reopening such a file runs
// the startup name-tree verification, which is the evidence the forged rows are real. One served child per (page size,
// state); one StartHostedEngine gives one process gate bind. Modes (a child process each):
//   create <path> <page_size>        engine-API native create (the bootstrap credential is a known test password)
//   forge <path> <kind> <b> <s>      kind names | dead | uncommitted: names the bootstrap (<b>) and the L5 principal (<s>)
//   serve <path> <state> ...         state: serving | restricted | failed | names | dead | uncommitted
// With no argument the driver runs every size and state; "size <bytes>" runs one page size. The strace half is
// native_name_resolve_strace.py.

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
using E = srv::NativeNameResolveError;

constexpr std::string_view kBootstrapPassword = "S4b-Bootstrap-Pw-5e1d77a0c3";

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

// ---- forge: names written by the production storage writers --------------------------------------------------------
namespace tip = scratchbird::storage::page;

struct Writing {
  d::FileDevice device;
  std::optional<d::FileDeviceCarefulWriteSink> sink;
  std::unique_ptr<scratchbird::core::uuid::StandaloneUuidV7Issuer> issuer;
  std::optional<tip::TransactionInventoryWriter> writer;
  db::CatalogVersionWriterBinding binding;
  tip::TransactionHeaderRecord header;
  std::uint64_t order = 0;
  p::Uuid event;
};

// Opens the file for writing, restarts the transaction writer, BEGINs a transaction and writes its change event.
std::unique_ptr<Writing> BeginWriting(const fs::path& path, const Offline& offline) {
  auto w = std::make_unique<Writing>();
  Check(w->device.Open(path.string(), d::FileOpenMode::open_existing, d::DatabaseFileOwnership::native_data_file).ok(), "open for writing");
  std::optional<d::FilespaceRootReference> catalog_root, state_root, allocation_root, configuration_root, security_root;
  for (const auto& root : offline.roots) {
    if (root.kind == 2) catalog_root = root;
    if (root.kind == 4) state_root = root;
    if (root.kind == 3) allocation_root = root;
    if (root.kind == 6) configuration_root = root;
    if (root.kind == 7) security_root = root;
  }
  Check(catalog_root && state_root && allocation_root && configuration_root && security_root, "page zero names the roots");
  w->issuer = std::make_unique<scratchbird::core::uuid::StandaloneUuidV7Issuer>(
      scratchbird::core::uuid::StandaloneUuidV7Binding{offline.database, NewId()},
      scratchbird::core::uuid::kStandardLocalClockUuidV7Policy);
  tip::TransactionWriterBinding tb;
  tb.database_uuid = offline.database;
  tb.state_root = *state_root;
  tb.allocation_root = *allocation_root;
  tb.issuer = w->issuer.get();
  w->sink.emplace(w->device);
  w->writer.emplace(tb);
  Check(w->writer->Restart(*w->sink).ok(), "the writer restarts the file");
  w->header.transaction_uuid = NewId();
  const auto begun = w->writer->Begin(*w->sink, w->header);
  Check(begun.ok() && begun.transaction_order.has_value(), "BEGIN of the forging transaction");
  w->order = *begun.transaction_order;
  w->binding.database_uuid = offline.database;
  w->binding.catalog_root = *catalog_root;
  w->binding.state_root = *state_root;
  w->binding.configuration_root = *configuration_root;
  w->binding.security_root = *security_root;
  w->binding.allocation_root = *allocation_root;
  w->event = NewId();
  const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto written = scratchbird::tests::catalog_change_event::WriteEvent(*w->sink, w->binding, w->order, w->header.transaction_uuid,
                                                                          w->event, NewId(), NewId(), NewId(), now_us);
  Check(written.ok(), "the transaction's catalog.metadata_change event");
  return w;
}
std::vector<db::CatalogNewPageIdentity> NewPages() {
  std::vector<db::CatalogNewPageIdentity> pages;
  for (int i = 0; i < 8; ++i) pages.push_back({NewId(), NewId()});
  return pages;
}
void Commit(Writing& w) {
  const auto done_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  Check(w.writer->Commit(*w.sink, w.order, done_ns).ok(), "COMMIT of the forging transaction");
  Check(!w.writer->failed(), "the writer is sound");
}
// The definition's "touch" (owner Q247: a name write needs this transaction's version of the object's definition
// row): a successor that changes nothing else.
void Touch(Writing& w, const DefinitionFact& f) {
  db::CatalogVersionRequest r;
  r.role = 4;
  r.transaction_order = w.order;
  r.transaction_uuid = w.header.transaction_uuid;
  r.row_uuid = f.row_uuid;
  r.predecessor_version_uuid = f.version_uuid;
  r.version_uuid = NewId();
  r.metadata = f.metadata;
  ++r.metadata.definition_version;
  r.metadata.creator_transaction_uuid = {p::UuidKind::transaction, w.header.transaction_uuid};
  r.metadata.creator_local_transaction_id = w.order;
  r.metadata.audit_uuid = {p::UuidKind::object, w.event};
  const auto written = db::WriteCatalogSuccessorVersion(*w.sink, w.binding, r);
  if (!written.ok()) std::cerr << "touch refused: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
  Check(written.ok(), "the definition touch is written in place");
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
// A drop: the entry's delete-marker successor (lifecycle dropped, retired by this transaction).
void DropEntry(Writing& w, const NameFact& f) {
  db::CatalogVersionRequest r;
  r.role = 2;
  r.transaction_order = w.order;
  r.transaction_uuid = w.header.transaction_uuid;
  r.row_uuid = f.row_uuid;
  r.predecessor_version_uuid = f.version_uuid;
  r.version_uuid = NewId();
  r.metadata = *f.metadata;
  r.metadata.record.payload.clear();
  ++r.metadata.definition_version;
  r.metadata.creator_transaction_uuid = {p::UuidKind::transaction, w.header.transaction_uuid};
  r.metadata.creator_local_transaction_id = w.order;
  r.metadata.record.header.deleted = true;
  r.metadata.lifecycle = cat::CatalogObjectLifecycle::dropped;
  r.metadata.status = cat::CatalogObjectStatus::retired;
  r.metadata.retired_transaction_uuid = {p::UuidKind::transaction, w.header.transaction_uuid};
  auto e = *f.entry;
  e.lifecycle_state = cat::CatalogNameLifecycle::dropped;
  e.dropped_transaction_uuid = p::TypedUuid{p::UuidKind::transaction, w.header.transaction_uuid};
  r.name = cat::CatalogNamePayload{e};
  r.new_pages = NewPages();
  const auto written = db::WriteCatalogSuccessorVersion(*w.sink, w.binding, r);
  if (!written.ok()) std::cerr << "drop refused: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
  Check(written.ok(), "the name drop is written in place");
}

constexpr std::string_view kAliasKeep = "Owner_Alias_Keep";
constexpr std::string_view kAliasFrench = "proprietaire_fr";
constexpr std::string_view kQuotedMixed = "Mixed_Q";
constexpr std::string_view kTwinBootstrap = "TwinName";
constexpr std::string_view kTwinService = "TWINNAME";
constexpr std::string_view kAliasDrop = "Drop_Alias";
constexpr std::string_view kNeverCommitted = "Never_Committed";

// Forge kinds (each is cumulative on the file as it stands):
//   names        one committed transaction: an English alias, a French name, a quoted name and a quoted case twin on the
//                bootstrap principal, the other twin on L5, and an alias that the next kind drops
//   dead         names, then a second committed transaction that drops the alias
//   uncommitted  names, then a transaction that inserts a name and is left ACTIVE (the served open's Restart kills it)
int ForgeMode(const fs::path& path, const std::string& kind, const p::Uuid& bootstrap, const p::Uuid& service) {
  ProcessMemory();
  if (kind != "names" && kind != "dead" && kind != "uncommitted") return 2;
  {
    const auto before = ReadOffline(path);
    const auto* base_boot = EntryOf(before, bootstrap, std::string(kBootstrapName));
    const auto* base_svc = EntryOf(before, service, std::string(kServiceName));
    Check(base_boot && base_svc, "the create wrote the principals' name entries");
    Check(before.security.count(bootstrap) && before.security.count(service), "the principals' security rows exist");
    auto w = BeginWriting(path, before);
    Touch(*w, before.security.at(bootstrap));
    Touch(*w, before.security.at(service));
    std::vector<db::CatalogInsertRow> rows;
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kAliasKeep), false, std::nullopt, cat::CatalogNameClass::alias, false}));
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kAliasFrench), false, cat::kCatalogLanguage_fr_CA, cat::CatalogNameClass::alias, true}));
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kQuotedMixed), true, std::nullopt, cat::CatalogNameClass::alias, false}));
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kTwinBootstrap), true, std::nullopt, cat::CatalogNameClass::alias, false}));
    rows.push_back(EntryRow(*w, *base_svc, {std::string(kTwinService), true, std::nullopt, cat::CatalogNameClass::alias, false}));
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kAliasDrop), false, std::nullopt, cat::CatalogNameClass::alias, false}));
    Insert(*w, std::move(rows));
    Commit(*w);
    Check(w->device.Close().ok(), "close");
  }
  if (kind == "dead") {
    const auto mid = ReadOffline(path);
    const auto* target = EntryOf(mid, bootstrap, std::string(kAliasDrop));
    Check(target != nullptr, "the alias to drop is visible and committed before its drop");
    auto w = BeginWriting(path, mid);
    Touch(*w, mid.security.at(bootstrap));
    DropEntry(*w, *target);
    Commit(*w);
    Check(w->device.Close().ok(), "close");
    const auto after = ReadOffline(path);
    Check(EntryOf(after, bootstrap, std::string(kAliasDrop)) == nullptr, "after the drop the offline read no longer lists the alias");
    Check(EntryOf(after, bootstrap, std::string(kAliasKeep)) != nullptr, "the other alias is still listed");
  }
  if (kind == "uncommitted") {
    const auto mid = ReadOffline(path);
    const auto* base_boot = EntryOf(mid, bootstrap, std::string(kBootstrapName));
    Check(base_boot != nullptr, "the bootstrap entry");
    auto w = BeginWriting(path, mid);
    Touch(*w, mid.security.at(bootstrap));
    std::vector<db::CatalogInsertRow> rows;
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kNeverCommitted), false, std::nullopt, cat::CatalogNameClass::alias, false}));
    Insert(*w, std::move(rows));
    // No COMMIT: the transaction stays active in the file.
    Check(w->device.Close().ok(), "close");
    // The row really is in the file (so that "does not resolve" is the visibility rule's doing, not an absent row).
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    Check(bytes.find(kNeverCommitted) != std::string::npos, "the uncommitted name's row is physically in the file");
  }
  std::cout << "FORGED kind=" << kind << std::endl;
  return 0;
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

// ---- the served child ---------------------------------------------------------------------------------------------
class FailFirstWriteSink final : public d::CarefulWriteSink {
 public:
  explicit FailFirstWriteSink(d::CarefulWriteSink& inner) : inner_(inner) {}
  d::BoundedIoResult WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
    d::BoundedIoResult r;
    r.operation = d::BoundedIoOperation::write;
    r.error = d::BoundedIoError::native_failure;
    r.offset = offset;
    r.requested_bytes = bytes.size();
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  d::BoundedIoResult ReadAt(p::u64 offset, std::span<p::byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  d::CarefulWriteSink& inner_;
};

struct Outcome {
  srv::NativeNameResolveResult result;
  std::string summary;  // everything the test says about the result: structured only
  double milliseconds = 0;
};
std::vector<std::string> all_summaries;
std::set<std::string> asked_texts;  // every name the test asked about (to prove none appears in a summary)
std::vector<double> resolve_ms;

std::string Summary(const srv::NativeNameResolveResult& r) {
  std::ostringstream out;
  out << "error=" << srv::NativeNameResolveErrorName(r.error) << " code=" << srv::NativeNameResolveErrorCode(r.error)
      << " key_error=" << static_cast<unsigned>(r.key_error) << " source_error=" << static_cast<unsigned>(r.source_error)
      << " relation_error=" << static_cast<unsigned>(r.relation_error) << " custody_error=" << static_cast<unsigned>(r.custody_error);
  if (r.resolved) {
    const auto& x = *r.resolved;
    out << " object=" << Hex(x.object_uuid) << " scope=" << Hex(x.scope_uuid) << " language=" << Hex(x.language_uuid)
        << " entry=" << Hex(x.name_entry_uuid) << " kind=" << x.object_kind << " class=" << static_cast<unsigned>(x.name_class)
        << " default_object=" << x.default_for_object << " default_language=" << x.default_for_language
        << " matched=" << x.matched_entries;
  }
  return out.str();
}

struct Ask {
  std::string text;
  bool quoted = false;
  cat::CatalogNameQuoteStyle style = cat::CatalogNameQuoteStyle::none;  // none with quoted: double_quote
  std::optional<p::Uuid> language;
  srv::NativeNameScopeKind scope_kind = srv::NativeNameScopeKind::principal_namespace;
  p::Uuid scope;
  std::optional<std::uint16_t> kind;
  bool raw_style = false;  // use `style` as given (to test the refusal of a bad combination)
};

Outcome Resolve(srv::NativeDatabaseLoop& loop, const Ask& ask) {
  srv::NativeLoopResolveNameRequest request;
  request.text = ask.text;
  request.quoted = ask.quoted;
  request.quote_style = ask.raw_style ? ask.style
                        : ask.quoted ? (ask.style == cat::CatalogNameQuoteStyle::none ? cat::CatalogNameQuoteStyle::double_quote : ask.style)
                                     : cat::CatalogNameQuoteStyle::none;
  request.language = ask.language;
  request.scope_kind = ask.scope_kind;
  request.scope_uuid = ask.scope;
  request.object_kind = ask.kind;
  if (ask.text.size() < 200) asked_texts.insert(ask.text);
  const auto started = std::chrono::steady_clock::now();
  Outcome out;
  out.result = loop.SubmitResolveName(std::move(request)).get();
  out.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  out.summary = Summary(out.result);
  all_summaries.push_back(out.summary);
  if (out.result.error != E::database_failed && out.result.error != E::database_not_serving) resolve_ms.push_back(out.milliseconds);
  return out;
}
Outcome ResolveText(srv::NativeDatabaseLoop& loop, std::string_view text, bool quoted = false) {
  return Resolve(loop, Ask{std::string(text), quoted});
}

void ExpectRefusal(const Outcome& o, E expected, const std::string& what) {
  Check(!o.result.ok() && !o.result.resolved.has_value(), what + ": no resolution: " + o.summary);
  Check(o.result.error == expected, what + ": expected " + srv::NativeNameResolveErrorName(expected) + ", got " + o.summary);
  Check(std::string(srv::NativeNameResolveErrorCode(o.result.error)).starts_with("CATALOG.NAME_RESOLVE_NATIVE_"), what + ": a stable code");
}
void ExpectObject(const Outcome& o, const p::Uuid& object, const std::string& what) {
  Check(o.result.ok(), what + ": resolved: " + o.summary);
  Check(o.result.resolved->object_uuid == object, what + ": the expected object UUID: " + o.summary);
}

constexpr std::uint16_t kUserKind = static_cast<std::uint16_t>(cat::CatalogRecordKind::user_account);

int ServeMode(const fs::path& path, const std::string& state, const p::Uuid& database, const p::Uuid& bootstrap,
              const p::Uuid& service) {
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
  // The offline read happens BEFORE the server owns the file: the ground truth the served read is compared with.
  const auto offline = ReadOffline(path);
  Check(offline.database == database, "the file's database UUID is the create's");
  const bool forged = state == "names" || state == "dead" || state == "uncommitted";
  Check(std::set<p::Uuid>(offline.principals.begin(), offline.principals.end()).size() == offline.principals.size() &&
            std::count(offline.principals.begin(), offline.principals.end(), bootstrap) == 1 &&
            std::count(offline.principals.begin(), offline.principals.end(), service) == 1,
        "the offline read lists the bootstrap and the L5 principal");
  if (state == "failed")
    srv::SetNativeRestartSinkWrapperForTest([](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
      return std::make_unique<FailFirstWriteSink>(inner);
    });
  const auto before_start = Snapshot(directory);
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  if (state == "failed") {
    Check(!started.ok() && started.diagnostics.front().code == "DATABASE.RESTART_WRITE_FAILED",
          "the seam produced a REAL failed Restart: " + (started.diagnostics.empty() ? std::string("no diagnostic") : started.diagnostics.front().code));
  } else {
    Check(started.ok(), "the server serves the file: " + (started.diagnostics.empty() ? std::string() : started.diagnostics.front().code));
  }
  Check(!started.state.database_runtimes.empty() && started.state.database_runtimes.front() &&
            started.state.database_runtimes.front()->native_loop,
        "the hosted state holds the native loop");
  auto& loop = *started.state.database_runtimes.front()->native_loop;
  const std::string expected_state = (forged || state == "serving") ? "serving" : state;
  Check(std::string(srv::NativeServingStateName(loop.serving_state())) == expected_state, "the database's serving state is " + expected_state);
  Check(!loop.on_loop_thread(), "the test is not on the loop thread");
  // An open that Restarts a dead transaction writes by construction (the uncommitted file): the no-change claim is
  // about the calls, so the snapshot is taken after the open there.
  const auto before = state == "uncommitted" ? Snapshot(directory) : before_start;
  (void)before_start;

  const auto security_scope = offline.security_schema;
  std::size_t agents_resolved = 0, names_resolved = 0, database_scope_resolved = 0;

  if (state == "failed") {
    const auto o = ResolveText(loop, kBootstrapName);
    ExpectRefusal(o, E::database_failed, "failed state");
    Check(std::string(srv::NativeNameResolveErrorCode(E::database_failed)) != srv::NativeNameResolveErrorCode(E::database_not_serving),
          "failed and not-serving are distinct codes");
  } else {
    // 1. The bootstrap principal's own name resolves to the bootstrap principal UUID; the S4a login-material read of the
    //    same file for that UUID reports the same principal (the cross-check the owner asked for).
    {
      const auto o = ResolveText(loop, kBootstrapName);
      ExpectObject(o, bootstrap, "the bootstrap principal's name");
      const auto& x = *o.result.resolved;
      Check(x.database_uuid == database && x.scope_uuid == security_scope, "the database and the security schema scope are reported");
      Check(x.object_kind == kUserKind, "the object's kind is the user account kind");
      Check(x.default_for_object && x.default_for_language && x.name_class == cat::CatalogNameClass::primary && x.matched_entries == 1,
            "the create's name is the primary default name, matched once");
      const auto* fact = EntryOf(offline, bootstrap, std::string(kBootstrapName));
      Check(fact != nullptr && x.language_uuid == fact->language && x.name_entry_uuid == fact->entry_uuid,
            "the language and the entry UUID are the file's");
      PasswordFactory passwords(database);
      srv::NativeLoopLoginRequest login;
      login.principal_uuid = x.object_uuid;
      login.password = passwords.Make(kBootstrapPassword);
      login.password_length = kBootstrapPassword.size();
      const auto material = loop.SubmitReadLoginMaterial(std::move(login)).get();
      Check(material.ok() && material.material->principal_uuid == x.object_uuid && material.material->database_uuid == database,
            "the S4a login-material read of the resolved UUID reports the same principal");
    }
    // 2. Unquoted names fold ASCII upper-case and match case-insensitively (created case is kept).
    for (const char* spelling : {"FIXTURE_OWNER", "Fixture_Owner", "fIxTuRe_oWnEr"})
      ExpectObject(ResolveText(loop, spelling), bootstrap, std::string("unquoted ") + spelling);
    // 3. Quoted names match exactly: the unquoted-created name's own lane is its upper-case fold (Q181), so a quoted
    //    "FIXTURE_OWNER" finds it and any other spelling inside quotes does not.
    ExpectObject(ResolveText(loop, "FIXTURE_OWNER", true), bootstrap, "quoted exact fold spelling");
    ExpectRefusal(ResolveText(loop, "fixture_owner", true), E::name_not_found, "quoted created spelling");
    ExpectRefusal(ResolveText(loop, "Fixture_Owner", true), E::name_not_found, "quoted mixed spelling");
    // 4. A name that is a prefix or an extension of a real one, an unknown name, and a name with surrounding blanks.
    for (const char* text : {"fixture_owne", "fixture_ownerx", "xfixture_owner", "fixture_owner ", " fixture_owner", "f", "fixture",
                             "no_such_principal_anywhere", "fixture_ownerfixture_owner"})
      ExpectRefusal(ResolveText(loop, text), E::name_not_found, std::string("not a name of the file: ") + text);
    // 5. Inputs that cannot be an identifier: decided before the file is read, each with its own key error.
    {
      const auto empty = ResolveText(loop, "");
      ExpectRefusal(empty, E::name_invalid, "empty name");
      const auto nul = ResolveText(loop, std::string_view("fixture\0owner", 13));
      ExpectRefusal(nul, E::name_invalid, "a NUL in the name");
      ExpectRefusal(ResolveText(loop, std::string_view("\xC3\x28", 2)), E::name_invalid, "invalid UTF-8 (a truncated sequence)");
      ExpectRefusal(ResolveText(loop, std::string_view("\xFF\xFEname", 6)), E::name_invalid, "invalid UTF-8 (bytes never valid)");
      ExpectRefusal(ResolveText(loop, std::string_view("\xED\xA0\x80", 3)), E::name_invalid, "a UTF-16 surrogate encoded in UTF-8");
      const auto too_long = ResolveText(loop, std::string(cat::kCatalogIdentifierTextMaximumBytes + 1, 'a'));
      ExpectRefusal(too_long, E::name_invalid, "over the text limit");
      Check(empty.result.key_error == cat::CatalogIdentifierKeyError::invalid_text && too_long.result.key_error == cat::CatalogIdentifierKeyError::invalid_text,
            "the text rule's own key error is carried");
      // Over the identifier profile's length rule (512 scalar values), under the text limit: the profile refuses it.
      const auto profile_long = ResolveText(loop, std::string(513, 'a'));
      ExpectRefusal(profile_long, E::name_invalid, "over the profile's maximum identifier length");
      Check(profile_long.result.key_error == cat::CatalogIdentifierKeyError::identifier_too_long, "the length rule's key error is carried");
      ExpectRefusal(ResolveText(loop, std::string(512, 'a')), E::name_not_found, "a 512-character name is admissible and simply absent");
      // A quote style the profile does not admit.
      Ask backtick{std::string(kBootstrapName), true, cat::CatalogNameQuoteStyle::backtick};
      const auto bt = Resolve(loop, backtick);
      ExpectRefusal(bt, E::name_invalid, "a quote style the profile does not admit");
      Check(bt.result.key_error == cat::CatalogIdentifierKeyError::invalid_quote_mode, "invalid_quote_mode is carried");
      // Requests that are malformed in themselves.
      Ask quoted_without_style{std::string(kBootstrapName), true, cat::CatalogNameQuoteStyle::none};
      quoted_without_style.raw_style = true;
      ExpectRefusal(Resolve(loop, quoted_without_style), E::invalid_request, "quoted with no quote style");
      Ask unquoted_with_style{std::string(kBootstrapName), false, cat::CatalogNameQuoteStyle::double_quote};
      unquoted_with_style.raw_style = true;
      ExpectRefusal(Resolve(loop, unquoted_with_style), E::invalid_request, "unquoted with a quote style");
      Ask nil_scope{std::string(kBootstrapName)};
      nil_scope.scope_kind = srv::NativeNameScopeKind::explicit_uuid;
      ExpectRefusal(Resolve(loop, nil_scope), E::invalid_request, "an explicit scope that is nil");
      Ask zero_kind{std::string(kBootstrapName)};
      zero_kind.kind = 0;
      ExpectRefusal(Resolve(loop, zero_kind), E::invalid_request, "a record-kind filter of zero");
      Ask nil_language{std::string(kBootstrapName)};
      nil_language.language = p::Uuid{};
      ExpectRefusal(Resolve(loop, nil_language), E::invalid_request, "a language filter that is nil");
    }
    // 6. Scope: an explicit scope UUID equal to the file's security schema gives the same answer as the principal
    //    namespace; another scope does not hold the name; names under the database scope resolve under it.
    {
      Ask explicit_scope{std::string(kBootstrapName)};
      explicit_scope.scope_kind = srv::NativeNameScopeKind::explicit_uuid;
      explicit_scope.scope = security_scope;
      ExpectObject(Resolve(loop, explicit_scope), bootstrap, "the explicit security schema scope");
      explicit_scope.scope = database;
      ExpectRefusal(Resolve(loop, explicit_scope), E::name_not_found, "the database scope does not hold a principal's name");
      explicit_scope.scope = NewId();
      ExpectRefusal(Resolve(loop, explicit_scope), E::name_not_found, "an unknown scope");
      std::size_t n = 0;
      for (const auto& fact : offline.names) {
        if (fact.scope != database) continue;
        Ask a{fact.text, fact.quoted};
        a.scope_kind = srv::NativeNameScopeKind::explicit_uuid;
        a.scope = database;
        a.kind = fact.kind;
        const auto o = Resolve(loop, a);
        ExpectObject(o, fact.object, "a name under the database scope");
        Check(o.result.resolved->scope_uuid == database && o.result.resolved->object_kind == fact.kind, "its scope and kind are reported");
        ++n;
      }
      Check(n >= 1, "the database scope holds names");
      database_scope_resolved = n;
    }
    // 7. Language and kind filters.
    {
      Ask by_language{std::string(kBootstrapName)};
      const auto* fact = EntryOf(offline, bootstrap, std::string(kBootstrapName));
      Check(fact != nullptr, "the create's entry");
      by_language.language = fact->language;
      ExpectObject(Resolve(loop, by_language), bootstrap, "the entry's own language");
      by_language.language = cat::kCatalogLanguage_fr_CA == fact->language ? cat::kCatalogLanguage_de_DE : cat::kCatalogLanguage_fr_CA;
      ExpectRefusal(Resolve(loop, by_language), E::name_not_found, "another language");
      Ask by_kind{std::string(kBootstrapName)};
      by_kind.kind = kUserKind;
      ExpectObject(Resolve(loop, by_kind), bootstrap, "the user kind");
      by_kind.kind = static_cast<std::uint16_t>(cat::CatalogRecordKind::role_account);
      ExpectRefusal(Resolve(loop, by_kind), E::name_not_found, "the role kind");
    }
    // 8. Service (agent) principals resolve: resolution is a lookup; refusing an agent for external login is the login's
    //    job. L5 by its name, then every principal name of the file (the L5 principal included).
    {
      const auto l5 = ResolveText(loop, kServiceName);
      ExpectObject(l5, service, "the L5 service principal's name");
      Check(l5.result.resolved->object_kind == kUserKind, "a service principal is the same record kind as a user");
      // A resolve is a scan of the visible name rows (about 50 ms); the bootstrap principal, L5 and every fourth other
      // principal name of the file are resolved (each name is checked against the file's own list, not assumed).
      std::set<p::Uuid> distinct;
      std::size_t seen = 0;
      for (const auto& fact : offline.names) {
        if (fact.scope != security_scope || fact.kind != kUserKind) continue;
        if (fact.object != bootstrap && fact.object != service && (seen++ % 4) != 0) continue;
        const auto o = Resolve(loop, Ask{fact.text, fact.quoted});
        ExpectObject(o, fact.object, "a principal name of the file");
        distinct.insert(o.result.resolved->object_uuid);
        if (fact.object != bootstrap) ++agents_resolved;
        ++names_resolved;
      }
      Check(agents_resolved >= 10 && distinct.size() >= 10, "the agent principals resolve, each name to its own UUID");
    }
  }

  if (forged) {
    // The forged names (see ForgeMode), all through the real writers; each of them reachable.
    const auto* alias = EntryOf(offline, bootstrap, std::string(kAliasKeep));
    const auto* french = EntryOf(offline, bootstrap, std::string(kAliasFrench));
    Check(state != "uncommitted" || EntryOf(offline, bootstrap, std::string(kNeverCommitted)) == nullptr,
          "the offline committed read does not list the uncommitted name");
    Check(alias != nullptr && french != nullptr, "the offline read lists the forged names");
    // A second name in the same language: the same UUID, an alias, not the default.
    for (const char* spelling : {"Owner_Alias_Keep", "owner_alias_keep", "OWNER_ALIAS_KEEP"}) {
      const auto o = ResolveText(loop, spelling);
      ExpectObject(o, bootstrap, std::string("the alias ") + spelling);
      Check(o.result.resolved->name_class == cat::CatalogNameClass::alias && !o.result.resolved->default_for_object &&
                o.result.resolved->name_entry_uuid == alias->entry_uuid,
            "the alias is reported as an alias that is not the default name, with its own entry UUID");
    }
    // A name in a second language: reachable without a language, with its language, and not with another.
    {
      const auto any = ResolveText(loop, kAliasFrench);
      ExpectObject(any, bootstrap, "the French name, any language");
      Check(any.result.resolved->language_uuid == cat::kCatalogLanguage_fr_CA && any.result.resolved->default_for_language &&
                !any.result.resolved->default_for_object,
            "the French name reports its language and that it is its language's default");
      Ask in_french{std::string(kAliasFrench)};
      in_french.language = cat::kCatalogLanguage_fr_CA;
      ExpectObject(Resolve(loop, in_french), bootstrap, "the French name in French");
      in_french.language = french->language == cat::kCatalogLanguage_fr_CA ? cat::kCatalogLanguage_en : french->language;
      ExpectRefusal(Resolve(loop, in_french), E::name_not_found, "the French name asked in the other language");
    }
    // A quoted name matches exactly; unquoted spellings fold to upper-case and do not find a quoted-only mixed-case name.
    {
      ExpectObject(ResolveText(loop, kQuotedMixed, true), bootstrap, "the quoted name, quoted");
      ExpectRefusal(ResolveText(loop, "mixed_q", true), E::name_not_found, "the quoted name in another case, quoted");
      ExpectRefusal(ResolveText(loop, kQuotedMixed), E::name_not_found, "a quoted-only mixed-case name asked unquoted (Q180)");
      ExpectRefusal(ResolveText(loop, "MIXED_Q"), E::name_not_found, "the quoted name's fold asked unquoted (Q180)");
      ExpectRefusal(ResolveText(loop, "MIXED_Q", true), E::name_not_found, "the quoted name's fold asked quoted");
    }
    // A quoted case-only twin: both exist (each quoted), each finds its own object, quoted; unquoted finds only the
    // upper-case twin (the quoted upper-case name's own lane is its exact upper-case text).
    {
      ExpectObject(ResolveText(loop, kTwinBootstrap, true), bootstrap, "the quoted twin on the bootstrap principal");
      ExpectObject(ResolveText(loop, kTwinService, true), service, "the quoted twin on L5");
      ExpectRefusal(ResolveText(loop, "twinname", true), E::name_not_found, "a third spelling, quoted");
      ExpectObject(ResolveText(loop, "twinname"), service, "unquoted twin spelling folds to the upper-case twin");
      ExpectObject(ResolveText(loop, "TwinName"), service, "unquoted twin spelling, mixed case");
    }
    // Every live name of the bootstrap principal (the file's own list) resolves to the one UUID.
    {
      std::size_t count = 0;
      for (const auto& fact : offline.names) {
        if (fact.object != bootstrap) continue;
        Ask a{fact.text, fact.quoted};
        a.language = fact.language;
        ExpectObject(Resolve(loop, a), bootstrap, "every name the bootstrap principal has");
        ++count;
      }
      Check(count == (state == "dead" ? 5u : 6u), "the bootstrap principal carries the create's name and the forged live ones");
    }
    // The alias the dead state drops is a live name until then; once dropped (a committed delete-marker successor) it
    // does not resolve, whatever its case.
    if (state == "dead") {
      Check(EntryOf(offline, bootstrap, std::string(kAliasDrop)) == nullptr, "the dropped alias is not listed offline either");
      ExpectRefusal(ResolveText(loop, kAliasDrop), E::name_not_found, "the dropped alias");
      ExpectRefusal(ResolveText(loop, "drop_alias"), E::name_not_found, "the dropped alias, other case");
      ExpectRefusal(ResolveText(loop, "DROP_ALIAS", true), E::name_not_found, "the dropped alias, quoted");
    } else {
      ExpectObject(ResolveText(loop, kAliasDrop), bootstrap, "the alias that a later transaction drops, still live");
    }
    if (state == "uncommitted") {
      // The never-committed name's transaction was killed by the open's Restart: it must not resolve.
      ExpectRefusal(ResolveText(loop, kNeverCommitted), E::name_not_found, "a name written by a transaction that never committed");
    }
  }

  // Concurrent callers: serialized on the loop thread; each gets its own correct answer.
  if (state != "failed") {
    std::vector<std::thread> callers;
    std::vector<std::string> failures(3);
    for (int t = 0; t < 3; ++t)
      callers.emplace_back([&, t] {
        for (int i = 0; i < 4; ++i) {
          srv::NativeLoopResolveNameRequest known, unknown;
          known.text = std::string(kBootstrapName);
          unknown.text = "no_such_principal_anywhere";
          const auto a = loop.SubmitResolveName(std::move(known)).get();
          const auto b = loop.SubmitResolveName(std::move(unknown)).get();
          if (!a.ok() || a.resolved->object_uuid != bootstrap || b.error != E::name_not_found) failures[t] = Summary(a) + " / " + Summary(b);
        }
      });
    for (auto& c : callers) c.join();
    for (const auto& f : failures) Check(f.empty(), "a concurrent caller got a wrong answer: " + f);
  }

  // The results and every summary are structured only: no asked name appears in any of them.
  for (const auto& s : all_summaries)
    for (const auto& text : asked_texts)
      Check(text.size() < 6 || s.find(text) == std::string::npos, "no name text in a result summary");

  double sum = 0, worst = 0;
  for (const auto t : resolve_ms) { sum += t; worst = std::max(worst, t); }
  std::cout << "INFO principal_names=" << names_resolved << " agents=" << agents_resolved << " database_scope=" << database_scope_resolved
            << " entries_in_file=" << offline.names.size() << " security_scope_known=" << !security_scope.is_nil() << std::endl;
  std::cout << "TIMING resolves=" << resolve_ms.size() << " mean_ms=" << (resolve_ms.empty() ? 0 : sum / resolve_ms.size())
            << " max_ms=" << worst << std::endl;

  Check(Snapshot(directory) == before, "the directory and every file's bytes are unchanged after every call");
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");

  Check(loop.Close().ok, "the loop closes");
  ExpectRefusal(ResolveText(loop, kBootstrapName), E::database_not_serving, "a resolution after the loop closed");
  std::cout << "NAME_RESOLVE_OK checks=" << checks << " state=" << state << std::endl;
  return 0;
}

// ---- driver --------------------------------------------------------------------------------------------------------
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

void OneSize(const std::string& self, const fs::path& root, std::uint32_t page_size, const std::vector<std::string>& states) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  for (const auto& state : states) {
    const auto dir = root / (state + std::to_string(page_size));
    fs::create_directory(dir);
    const auto path = dir / "names.sdb";
    int status = 0;
    const auto created = Capture("'" + self + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
    if (state == "restricted") {
      Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' committing", &status).find("FIXTURE ok") != std::string::npos && status == 0,
            tag + "the native_serving writer left a committing slot");
    } else if (state == "names" || state == "dead" || state == "uncommitted") {
      const auto forged = Capture("'" + self + "' forge '" + path.string() + "' " + state + " " + Field(created, "bootstrap") + " " + Field(created, "service"), &status);
      Check(status == 0 && forged.find("FORGED kind=" + state) != std::string::npos, tag + state + ": the engine's writers wrote the names: " + forged.substr(0, 1500));
    } else if (state == "failed") {
      Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' active", &status).find("FIXTURE ok") != std::string::npos && status == 0,
            tag + "the native_serving writer left an active slot (the Restart will write; the seam fails it)");
    }
    const auto start = std::chrono::steady_clock::now();
    const auto served = Capture("'" + self + "' serve '" + path.string() + "' " + state + " " + Field(created, "database") + " " +
                                    Field(created, "bootstrap") + " " + Field(created, "service"),
                                &status);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(status == 0 && served.find("NAME_RESOLVE_OK") != std::string::npos, tag + state + ": serve child: " + served.substr(0, 2500));
    Check(served.find(std::string(kBootstrapPassword)) == std::string::npos && created.find(std::string(kBootstrapPassword)) == std::string::npos,
          tag + state + ": no test password in any child output");
    if (const auto info = served.find("INFO "); info != std::string::npos)
      std::printf("%s%s: %s\n", tag.c_str(), state.c_str(), served.substr(info, served.find('\n', info) - info).c_str());
    std::printf("%s%s: %.1f s; %s", tag.c_str(), state.c_str(), seconds, served.substr(served.find("TIMING")).c_str());
    fflush(stdout);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc == 6 && std::string(argv[1]) == "forge") return ForgeMode(argv[2], argv[3], FromHex(argv[4]), FromHex(argv[5]));
  if (argc == 7 && std::string(argv[1]) == "serve") return ServeMode(argv[2], argv[3], FromHex(argv[4]), FromHex(argv[5]), FromHex(argv[6]));
  // "size <bytes> [state...]": one page size, the given states (default: all).
  if (argc >= 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    std::vector<std::string> states{"serving", "restricted", "failed", "names", "dead", "uncommitted"};
    if (argc > 3) states.assign(argv + 3, argv + argc);
    OneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), states);
    artifacts.Cleanup();
    std::printf("native_name_resolve: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
