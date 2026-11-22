// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_NAME_RESOLVE_TEST (thin-slice S4b, plan 40; owner Q306 = 1, Q309, 2026-11-19/20; re-pointed
// 2026-11-20/21, "Part A" of the name-resolution rework). The engine's EXISTING resolver (EngineResolveName ->
// ResolveNameRegistryPublic -> LoadNameRegistryState) resolves a name given AS DATA to the object UUID of the committed
// names relation of a served native database, or refuses with a specific code. (Until Part A this file drove S4b's
// private NativeDatabaseLoop::SubmitResolveName, a duplicate of that resolver, now deleted.) The native read is the
// names source the loop publishes in the served facts (NativeNamesSource): a native context never reads a legacy side
// file. No session is needed at this level; the session's own uncommitted names arrive with S4e
// (SB_NAME_SNAPSHOT_OWN_TXN). Assertions that depend on the engine's fold / language fallback / "" refusal are marked
// "CHANGES WITH PART B".
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
//
// SB_PARSER_NAME_RESOLUTION differential (Part B step S1, 2026-11-24): every state also runs a table of typed names through the
// parser-side resolver (parsers/shared/name_resolution) and asserts that what the module sends to the CURRENT engine, and what the
// module itself resolves from the engine's own entries, equal the engine's answer to the raw typed spelling. The engine's fold is
// still there in S1 and the module's output is already folded, so the two agree; S2 removes the engine's fold and this block
// then proves the module alone carries the rule.

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
#if defined(SB_NAME_STREAM_TEST)
#include "native_catalog_visibility_stream_reader.hpp"
#endif
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
#include "name_resolution/name_resolution.hpp"
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
#if defined(SB_NAME_STREAM_TEST)
    // SB_NATIVE_CATALOG_STREAM: a names relation past the collect-everything scan tier (4096 rows / 4 MiB) is read by the
    // streaming reader; the stream test proves it against the collect-everything reader on files the old reader reads.
    db::NativeCatalogStreamLimits stream_limits;
    stream_limits.maximum_profile = profile;
    BoundMemory stream_memory(out.database, db::NativeCatalogStreamWorkspaceBytes(stream_limits));
    auto stream_prepared = db::PrepareNativeCatalogStreamRead(stream_limits, stream_memory.memory, stream_memory.binding);
    Check(stream_prepared.ok(), "the stream workspace prepares");
    const auto visit = [&](const db::NativeCatalogVisibleRow& row) {
      if (!row.name_payload || row.metadata.record.header.deleted || row.provisional) return true;
      const auto* e = std::get_if<cat::CatalogNameEntryView>(&*row.name_payload);
      if (!e) return true;
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
      return true;
    };
    const auto streamed = db::NativeCatalogVisibilityStreamReader::StreamCommittedSelected(lease, 2, 2, budget, stream_prepared.workspace, visit);
    Check(streamed.ok(), "the names relation streams");
    if (false)
#endif
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
// S4c (native login) forge kind "login": names the login test needs, written by the same production writers.
constexpr std::string_view kLoginAlias = "owner_login_alias";        // an alias-class name on the bootstrap principal
constexpr std::string_view kLoginHidden = "owner_hidden_login";      // a hidden-class name on the bootstrap principal
constexpr std::string_view kLoginHiddenOnly = "hidden_only_login";   // a hidden name that nothing else shares

// Forge kinds (each is cumulative on the file as it stands):
//   names        one committed transaction: an English alias, a French name, a quoted name and a quoted case twin on the
//                bootstrap principal, the other twin on L5, and an alias that the next kind drops
//   dead         names, then a second committed transaction that drops the alias
//   uncommitted  names, then a transaction that inserts a name and is left ACTIVE (the served open's Restart kills it)
int ForgeMode(const fs::path& path, const std::string& kind, const p::Uuid& bootstrap, const p::Uuid& service) {
  ProcessMemory();
  if (kind != "names" && kind != "dead" && kind != "uncommitted" && kind != "login") return 2;
  if (kind == "login") {
    // SB_NATIVE_LOGIN_TEST (S4c): one committed transaction with the names the login cases need. The hidden class
    // (CatalogNameClass::hidden) is written through the engine's own entry writer; nothing is hand-patched.
    const auto before = ReadOffline(path);
    const auto* base_boot = EntryOf(before, bootstrap, std::string(kBootstrapName));
    const auto* base_svc = EntryOf(before, service, std::string(kServiceName));
    Check(base_boot && base_svc, "the create wrote the principals' name entries");
    auto w = BeginWriting(path, before);
    Touch(*w, before.security.at(bootstrap));
    Touch(*w, before.security.at(service));
    std::vector<db::CatalogInsertRow> rows;
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kLoginAlias), false, std::nullopt, cat::CatalogNameClass::alias, false}));
    rows.push_back(EntryRow(*w, *base_boot, {std::string(kLoginHidden), false, std::nullopt, cat::CatalogNameClass::hidden, false}));
    rows.push_back(EntryRow(*w, *base_svc, {std::string(kLoginHiddenOnly), false, std::nullopt, cat::CatalogNameClass::hidden, false}));
    Insert(*w, std::move(rows));
    Commit(*w);
    Check(w->device.Close().ok(), "close");
    std::cout << "FORGED kind=login" << std::endl;
    return 0;
  }
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

// ---- the EXISTING resolver on a native context -------------------------------------------------------------------------
// Part A (2026-11-20): the name -> UUID step is EngineResolveName (and, for the entry itself, ResolveNameRegistryPublic),
// the engine's existing resolver, on a context that carries the served facts a native handle's contexts carry
// (ServedNativeFactsFor: the facts the loop published). It needs NO session: the registry load on a native context reads
// the committed names through the facts' names source. What a session (S4) will add is the transaction: the names of the
// session's own uncommitted work (SB_NAME_SNAPSHOT_OWN_TXN).
// Assertions marked "CHANGES WITH PART B" depend on what the engine's resolver does today (it folds, falls back across
// languages, refuses "", takes no language filter) and are expected to change when Part B removes that.
namespace eng = scratchbird::engine::internal_api;

struct Env {
  std::shared_ptr<const eng::NativeServedDatabaseFacts> facts;
  p::Uuid database, security_scope;
};
Env env;

constexpr const char* kNotFound = "CATALOG.NAME.NOT_FOUND";

eng::EngineUuid ToEngine(const p::Uuid& id) {
  eng::EngineUuid out;
  std::copy(id.bytes.begin(), id.bytes.end(), out.bytes.begin());
  return out;
}
bool Same(const eng::EngineUuid& a, const p::Uuid& b) { return std::equal(a.bytes.begin(), a.bytes.end(), b.bytes.begin()); }
// The language tag the registry projection gives a known language constant (the boundary adapter's spelling).
std::string TagFor(const p::Uuid& language) {
  for (const auto& known : cat::kCatalogLanguages)
    if (known.uuid == language) return std::string(known.tag);
  Check(false, "a language constant");
  return {};
}
// The class text the projection gives a record kind (name_registry.cpp NativeKindClassText: user and role are the
// legacy classes, every other kind its catalog token).
std::string ClassFor(std::uint16_t kind) {
  const auto k = static_cast<cat::CatalogRecordKind>(kind);
  if (k == cat::CatalogRecordKind::user_account) return "user";
  if (k == cat::CatalogRecordKind::role_account) return "role";
  return cat::CatalogRecordKindName(k);
}

struct Ask {
  std::string text;
  bool quoted = false;
  std::string quote_style;                // "" : double_quote when quoted, none otherwise
  std::string language = "en";            // the session language tag (the existing resolver falls back from it)
  std::optional<p::Uuid> scope;           // default: the file's security schema (the principals' scope)
  std::string object_class;               // "" : any
};

eng::EngineResolveNameRequest MakeRequest(const Ask& ask) {
  eng::EngineResolveNameRequest request;
  request.context.native_served_facts = env.facts;
  request.context.database_uuid = ToEngine(env.database);
  request.context.language_context.language_tag = ask.language;
  request.context.language_context.default_language_tag = ask.language;
  request.context.identifier_profile_uuid = "sbsql_v3";
  request.target_schema.uuid = ToEngine(ask.scope ? *ask.scope : env.security_scope);
  request.sql_object_reference.expected_object_type = ask.object_class;
  auto& name = request.sql_object_reference.object_name;
  name.raw_text = ask.text;
  name.was_quoted = ask.quoted;
  name.requires_exact_match = ask.quoted;
  name.quote_style = !ask.quote_style.empty() ? ask.quote_style : ask.quoted ? "double_quote" : "none";
  name.identifier_profile_uuid = "sbsql_v3";
  return request;
}

struct Outcome {
  eng::EngineResolveNameResult result;
  std::string code;     // the first diagnostic's code when the resolution is refused
  std::string summary;  // everything the test says about the result: structured only
  double milliseconds = 0;
  bool ok() const { return result.ok; }
};
std::vector<std::string> all_summaries;
std::set<std::string> asked_texts;  // every name the test asked about (to prove none appears in a summary)
std::vector<double> resolve_ms;

Outcome Resolve(const Ask& ask) {
  if (ask.text.size() < 200) asked_texts.insert(ask.text);
  const auto request = MakeRequest(ask);
  const auto started = std::chrono::steady_clock::now();
  Outcome out;
  out.result = eng::EngineResolveName(request);
  out.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  if (!out.result.ok && !out.result.diagnostics.empty()) out.code = out.result.diagnostics.front().code;
  std::ostringstream summary;
  summary << "ok=" << out.result.ok << " code=" << out.code;
  if (out.result.ok)
    summary << " object=" << Hex(out.result.primary_object.uuid) << " class=" << out.result.primary_object.object_kind
            << " schema=" << Hex(out.result.bound_object_identity.resolved_schema_uuid)
            << " generation=" << out.result.bound_object_identity.catalog_generation_id;
  out.summary = summary.str();
  all_summaries.push_back(out.summary);
  resolve_ms.push_back(out.milliseconds);
  return out;
}
Outcome ResolveText(std::string_view text, bool quoted = false) { return Resolve(Ask{std::string(text), quoted}); }

// The registry-level matches of the same question: the entries (entry UUID, class text, language tag, scope) behind a
// resolution. ResolveNameRegistryPublic is the lookup EngineResolveName calls.
std::vector<eng::NameRegistryEntry> Entries(const Ask& ask) {
  const auto matches = eng::ResolveNameRegistryPublic(MakeRequest(ask), ask.object_class);
  Check(matches.ok, "the registry-level lookup of the same question: " + matches.diagnostic.code);
  return matches.matches;
}

void ExpectRefusal(const Outcome& o, const std::string& code, const std::string& what) {
  Check(!o.ok(), what + ": no resolution: " + o.summary);
  Check(o.code == code, what + ": expected " + code + ", got " + o.summary);
}
void ExpectObject(const Outcome& o, const p::Uuid& object, const std::string& what) {
  Check(o.ok(), what + ": resolved: " + o.summary);
  Check(Same(o.result.primary_object.uuid, object), what + ": the expected object UUID: " + o.summary);
}

constexpr std::uint16_t kUserKind = static_cast<std::uint16_t>(cat::CatalogRecordKind::user_account);

// The names source's own rows (the committed role-2 entries as values): the evidence that the projection's input is the
// file's, field by field, and where the default flags (not part of the legacy entry) are checked.
eng::NativeNamesSnapshot Rows() {
  Check(env.facts && env.facts->names_source, "the served facts carry a names source");
  return env.facts->names_source->Read();
}
const eng::NativeNameRowValue* RowOf(const eng::NativeNamesSnapshot& snapshot, const p::Uuid& entry) {
  for (const auto& row : snapshot.rows)
    if (row.name_entry_uuid == entry) return &row;
  return nullptr;
}

// ---- SB_PARSER_NAME_RESOLUTION differential: the parser-side module against the CURRENT engine ----------------------------------
// For each typed name the engine's answer to the RAW spelling (the call every parser makes today) is compared with
//  (W) what the SBsql parser puts on the wire after the module (PrepareSbsqlWireName: unquoted components folded, quoted
//      exact), split as the server splits it, through the same engine call (the engine still folds: idempotent), and
//  (E) the module's key for each step sent as an EXACT spelling (what the engine will receive after S2 removes its fold), and
//  (M) the module's own walk over the engine's own name entries (language fallback, scope walk, own lanes, ambiguity).
// A module that does not fold, or folds wrongly, fails (E) and (M) here and the unit test there.
namespace nr = scratchbird::parsers::name_resolution;

nr::Uuid16 Uuid16Of(const eng::EngineUuid& id) {
  nr::Uuid16 out{};
  std::copy(id.bytes.begin(), id.bytes.begin() + 16, out.begin());
  return out;
}
std::string HexOf(const nr::Uuid16& id) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : id) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 15]);
  }
  return out;
}

// The names the engine's registry holds for the served database, as the module's source: nothing but the engine's current
// projection (LoadNameRegistryState), filtered by the profile equality the engine's matcher applies today.
class RegistryNameSource final : public nr::NameSource {
 public:
  RegistryNameSource() {
    const auto loaded = eng::LoadNameRegistryState(MakeRequest(Ask{}).context, 0);
    Check(loaded.ok, "the registry state loads for the differential");
    entries_ = loaded.state.entries;
  }
  std::vector<nr::NameEntry> List(const nr::Uuid16& scope, std::string_view language) override {
    std::vector<nr::NameEntry> out;
    for (const auto& entry : entries_) {
      if (entry.identifier_profile_uuid != "sbsql_v3") continue;
      if (Uuid16Of(entry.scope_uuid) != scope || entry.language_tag != language) continue;
      out.push_back({scope, Uuid16Of(entry.object_uuid), entry.language_tag, entry.object_class, entry.raw_name_text,
                     entry.requires_exact_match});
    }
    return out;
  }

 private:
  std::vector<eng::NameRegistryEntry> entries_;
};

std::string VerdictOf(const eng::EngineResolveNameResult& result) {
  if (result.ok) return "found:" + HexOf(Uuid16Of(result.primary_object.uuid));
  return "refused:" + (result.diagnostics.empty() ? std::string() : result.diagnostics.front().code);
}
std::string VerdictOf(const nr::ResolveOutcome& outcome) {
  switch (outcome.status) {
    case nr::ResolveStatus::found: return "found:" + HexOf(outcome.entry.object);
    case nr::ResolveStatus::not_found: return "refused:CATALOG.NAME.NOT_FOUND";
    case nr::ResolveStatus::ambiguous: return "refused:CATALOG.NAME.AMBIGUOUS";
    case nr::ResolveStatus::invalid_request: return "refused:SB_ENGINE_API_INVALID_REQUEST";
  }
  return "?";
}

// The engine call for a typed path (qualifier steps, then the leaf), each step as (text, quoted, exact).
struct EngineStep {
  std::string text;
  bool quoted = false;
  bool exact = false;
};
eng::EngineResolveNameResult AskEngine(const std::vector<EngineStep>& steps, const std::string& language, const p::Uuid& scope,
                                       const std::string& object_class) {
  Ask ask{steps.back().text, steps.back().quoted};
  ask.language = language;
  ask.scope = scope;
  ask.object_class = object_class;
  auto request = MakeRequest(ask);
  request.sql_object_reference.object_name.requires_exact_match = steps.back().exact || steps.back().quoted;
  for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
    eng::EngineIdentifierAtom atom;
    atom.raw_text = steps[i].text;
    atom.was_quoted = steps[i].quoted;
    atom.requires_exact_match = steps[i].exact || steps[i].quoted;
    atom.quote_style = steps[i].quoted ? "double_quote" : "none";
    atom.identifier_profile_uuid = "sbsql_v3";
    request.sql_object_reference.path_components.push_back(atom);
  }
  if (steps.size() > 1) request.sql_object_reference.no_search_path = true;
  return eng::EngineResolveName(request);
}

struct DifferentialCounts {
  std::size_t cases = 0, engine_calls = 0, found = 0, not_found = 0, invalid = 0, rewritten = 0, stored_sent = 0;
};
// SB_PARSER_NAME_LIST (Part B S4): a cache shared by every case of one run (the snapshot does not change during the run)
// and the check that the engine is also asked the STORED spelling of what the module matched. Null: the S1 behaviour.
nr::NameListCache* shared_name_cache = nullptr;
bool send_stored_spelling = false;

void DifferentialCase(nr::NameSource& source, DifferentialCounts* counts, const std::vector<nr::NameComponent>& typed,
                      const std::string& language, const p::Uuid& scope, const std::string& object_class,
                      const std::string& what) {
  std::vector<EngineStep> raw, wire, exact;
  for (const auto& step : typed) raw.push_back({step.spelling, step.quoted, step.quoted});
  // (W): the text the SBsql parser sends, split the way the server splits it.
  const auto prepared = nr::PrepareSbsqlWireName(nr::RenderPresentedName(typed), false, "relation");
  const auto split = nr::SplitPresentedName(prepared.presented_name, prepared.quoted);
  if (prepared.rewritten) ++counts->rewritten;
  if (split) {
    for (const auto& step : *split) wire.push_back({step.spelling, step.quoted, step.quoted});
  } else {
    wire = raw;  // text that does not split is sent unchanged
  }
  // (E): the module's key for each step, exact.
  const auto& rule = nr::SbsqlNameFoldRule();
  for (const auto& step : typed) exact.push_back({rule.Key(step.spelling, step.quoted), true, true});

  const auto verdict_raw = VerdictOf(AskEngine(raw, language, scope, object_class));
  const auto verdict_wire = VerdictOf(AskEngine(wire, language, scope, object_class));
  const auto verdict_exact = VerdictOf(AskEngine(exact, language, scope, object_class));
  counts->engine_calls += 3;
  Check(verdict_wire == verdict_raw, "differential (W) the wire text the module prepares: " + what + ": " + verdict_wire + " vs raw " + verdict_raw);
  Check(verdict_exact == verdict_raw, "differential (E) the module's keys sent exact: " + what + ": " + verdict_exact + " vs raw " + verdict_raw);

  // (M): the module's own walk over the engine's entries.
  nr::NameListCache local_cache(source);
  nr::NameListCache& cache = shared_name_cache != nullptr ? *shared_name_cache : local_cache;
  nr::ResolutionContext context;
  context.target_schema = Uuid16Of(scope);
  context.session_language = context.default_language = language;
  nr::ResolveRequest request;
  request.path = typed;
  if (!object_class.empty()) request.accepts_class = [object_class](const std::string& c) { return c == object_class; };
  const auto walked = nr::Resolve(rule, cache, context, request);
  const auto verdict_walk = VerdictOf(walked);
  Check(verdict_walk == verdict_raw, "differential (M) the module's own walk: " + what + ": " + verdict_walk + " vs raw " + verdict_raw);
  // (S): the engine asked the STORED spelling of each matched entry (each step with the quoted datum it was stored with):
  // the answer is the object the module chose, which is what an exact matcher (S2) will answer too.
  if (send_stored_spelling) {
    if (const auto stored = nr::StoredPresentedName(walked)) {
      const auto parts = nr::SplitPresentedName(*stored, false);
      Check(parts.has_value(), "the stored spelling splits: " + what);
      std::vector<EngineStep> sent;
      for (const auto& step : *parts) sent.push_back({step.spelling, step.quoted, step.quoted});
      const auto verdict_stored = VerdictOf(AskEngine(sent, language, scope, object_class));
      ++counts->engine_calls;
      ++counts->stored_sent;
      Check(verdict_stored == verdict_raw, "differential (S) the stored spelling of the matched entries: " + what + ": " + verdict_stored + " vs raw " + verdict_raw);
    }
  }

  ++counts->cases;
  if (verdict_raw.rfind("found:", 0) == 0) ++counts->found;
  else if (verdict_raw == "refused:CATALOG.NAME.NOT_FOUND") ++counts->not_found;
  else ++counts->invalid;
}

std::string AsciiCase(std::string text, int mode) {  // 0 upper, 1 lower, 2 alternating
  for (std::size_t i = 0; i < text.size(); ++i) {
    char& ch = text[i];
    const bool up = mode == 0 || (mode == 2 && i % 2 == 0);
    if (up && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (!up && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return text;
}

DifferentialCounts DifferentialAgainstModule(const Offline& offline, const p::Uuid& bootstrap, const p::Uuid& service,
                                             nr::NameSource* external_source = nullptr) {
  DifferentialCounts counts;
  std::optional<RegistryNameSource> registry_source;
  if (external_source == nullptr) registry_source.emplace();
  nr::NameSource& source = external_source != nullptr ? *external_source : *registry_source;
  const auto& database = offline.database;
  const auto& security_scope = offline.security_schema;
  const auto language_of = [](const NameFact& f) { return TagFor(f.language); };

  // Unqualified names of the principals' scope: every name of the bootstrap and the L5 principal (the forged names are
  // among them) and every thirty-second other principal name, each in seven spellings, in its own language and in en.
  std::size_t other = 0;
  for (const auto& fact : offline.names) {
    if (fact.scope != security_scope || fact.kind != kUserKind) continue;
    if (fact.object != bootstrap && fact.object != service && (other++ % 32) != 0) continue;
    std::set<std::pair<std::string, bool>> spellings{
        {fact.text, false},      {AsciiCase(fact.text, 0), false}, {AsciiCase(fact.text, 1), false},
        {AsciiCase(fact.text, 2), false}, {fact.text, true},       {AsciiCase(fact.text, 0), true},
        {AsciiCase(fact.text, 1), true}};
    std::set<std::string> languages{language_of(fact), "en"};
    for (const auto& language : languages)
      for (const auto& [text, quoted] : spellings)
        DifferentialCase(source, &counts, {{text, quoted}}, language, security_scope, "",
                         std::string(quoted ? "quoted " : "unquoted ") + "name in " + language);
  }
  // Names that are not names, and the empty name (a plain refusal today: name_required).
  for (const char* text : {"", "fixture_owne", "fixture_ownerx", " fixture_owner", "no_such_principal_anywhere", "f"})
    DifferentialCase(source, &counts, {{text, false}}, "en", security_scope, "", std::string("a non-name: ") + text);
  DifferentialCase(source, &counts, {{"", true}}, "en", security_scope, "", "the quoted empty name");
  // A kind filter and another scope.
  DifferentialCase(source, &counts, {{std::string(kBootstrapName), false}}, "en", security_scope, "user", "the user class");
  DifferentialCase(source, &counts, {{std::string(kBootstrapName), false}}, "en", security_scope, "role", "the role class");
  DifferentialCase(source, &counts, {{std::string(kBootstrapName), false}}, "en", database, "", "a scope that does not hold it");
  DifferentialCase(source, &counts, {{std::string(kBootstrapName), false}}, "de-DE", security_scope, "", "another language");

  // Qualified: schema.name, from the root. The schema's own name is whatever the database scope calls the principals' scope.
  for (const auto& schema : offline.names) {
    if (schema.object != security_scope || schema.scope != database) continue;
    for (const auto& leaf : offline.names) {
      if (leaf.scope != security_scope || leaf.kind != kUserKind || (leaf.object != bootstrap && leaf.object != service) ||
          !leaf.default_for_object)
        continue;
      for (int mode : {0, 1}) {
        for (bool quoted_schema : {false, true}) {
          DifferentialCase(source, &counts,
                           {{AsciiCase(schema.text, mode), quoted_schema}, {leaf.text, leaf.quoted}},
                           TagFor(leaf.language), database, "",
                           std::string("a qualified name, schema ") + (quoted_schema ? "quoted" : "unquoted"));
        }
      }
      DifferentialCase(source, &counts, {{schema.text, schema.quoted}, {AsciiCase(leaf.text, 0), false}},
                       TagFor(leaf.language), database, "", "a qualified name, unquoted leaf");
      DifferentialCase(source, &counts, {{"no_such_schema", false}, {leaf.text, leaf.quoted}},
                       TagFor(leaf.language), database, "", "a qualified name, unknown schema");
      DifferentialCase(source, &counts, {{"", false}, {leaf.text, leaf.quoted}}, TagFor(leaf.language), database, "",
                       "a qualified name, empty schema step");
    }
  }
  // The database scope's own names (the schemas), by class.
  std::size_t in_database = 0;
  for (const auto& fact : offline.names) {
    if (fact.scope != database || (in_database++ % 3) != 0) continue;
    DifferentialCase(source, &counts, {{fact.text, fact.quoted}}, TagFor(fact.language), database, ClassFor(fact.kind),
                     "a name of the database scope");
    DifferentialCase(source, &counts, {{AsciiCase(fact.text, 1), false}}, TagFor(fact.language), database, "",
                     "a name of the database scope, lower unquoted");
  }
  Check(counts.cases >= 40 && counts.found >= 10 && counts.not_found >= 10 && counts.invalid >= 1 && counts.rewritten >= 10,
        "the differential table covers found, not-found and refused cases, and the module rewrote names");
  return counts;
}

#ifdef SB_OPEN_BIG_TEST
// SB_NATIVE_OPEN_BIG_TEST (2026-11-26): native_open_big_test.cpp includes this file as its harness and supplies the served
// checks of a file past the old open tiers through this hook (defined in its own anonymous namespace).
int OpenBigServe(const std::string& state, const Offline& offline, srv::NativeDatabaseLoop& loop, const fs::path& directory,
                 const p::Uuid& database, const p::Uuid& bootstrap, const p::Uuid& service, double start_ms);
#endif
#ifdef SB_NAME_LIST_TEST
// SB_ENGINE_NAME_LIST_API (Part B S4): native_name_list_test.cpp includes this file as its harness (create, forge, serve,
// strace driver) and supplies the served checks of ListNames / UuidToName through this hook, defined in its own
// translation unit's anonymous namespace.
int NameListServe(const std::string& state, const Offline& offline, srv::NativeDatabaseLoop& loop, const fs::path& directory,
                  const std::map<std::string, std::string>& before, const p::Uuid& database, const p::Uuid& bootstrap,
                  const p::Uuid& service);
#endif

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
  const bool forged = state == "names" || state == "dead" || state == "uncommitted" || state == "many" || state == "flow";
  Check(std::set<p::Uuid>(offline.principals.begin(), offline.principals.end()).size() == offline.principals.size() &&
            std::count(offline.principals.begin(), offline.principals.end(), bootstrap) == 1 &&
            std::count(offline.principals.begin(), offline.principals.end(), service) == 1,
        "the offline read lists the bootstrap and the L5 principal");
  if (state == "failed")
    srv::SetNativeRestartSinkWrapperForTest([](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
      return std::make_unique<FailFirstWriteSink>(inner);
    });
  const auto before_start = Snapshot(directory);
#ifdef SB_OPEN_BIG_TEST
  if (const char* budget = std::getenv("SB_OPEN_BIG_BUDGET")) srv::SetNativeCatalogSizingBudgetForTest(std::stoull(budget));
#endif
  const auto start_clock = std::chrono::steady_clock::now();
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  const double start_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_clock).count();
  if (std::getenv("SB_OPEN_TIMING")) std::fprintf(stderr, "OPEN_MS %.1f\n", start_ms);
  if (state == "failed") {
    Check(!started.ok() && started.diagnostics.front().code == "DATABASE.RESTART_WRITE_FAILED",
          "the seam produced a REAL failed Restart: " + (started.diagnostics.empty() ? std::string("no diagnostic") : started.diagnostics.front().code));
  } else {
#ifdef SB_OPEN_BIG_TEST
    if (!started.ok())
      for (const auto& field : started.diagnostics.front().fields) std::fprintf(stderr, "  diagnostic field %s=%s\n", field.key.c_str(), field.value.c_str());
#endif
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

  // The facts a native handle's contexts carry: the ones the loop published, found by the database's path (the lookup
  // the session layer uses). No session is involved.
  env.facts = srv::ServedNativeFactsFor(started.state, path.string());
  env.database = database;
  env.security_scope = offline.security_schema;
  Check(env.facts != nullptr && env.facts->names_source != nullptr, "the loop published served facts with a names source");
#ifdef SB_OPEN_BIG_TEST
  return OpenBigServe(state, offline, loop, directory, database, bootstrap, service, start_ms);
#endif
#ifdef SB_NAME_LIST_TEST
  return NameListServe(state, offline, loop, directory, before, database, bootstrap, service);
#endif
  const auto security_scope = offline.security_schema;
  std::size_t agents_resolved = 0, names_resolved = 0, database_scope_resolved = 0;

  if (state == "failed") {
    const auto o = ResolveText(kBootstrapName);
    ExpectRefusal(o, eng::NativeNamesSnapshotErrorCode(eng::NativeNamesSnapshotError::database_failed), "failed state");
    Check(std::string(eng::NativeNamesSnapshotErrorCode(eng::NativeNamesSnapshotError::database_failed)) !=
              eng::NativeNamesSnapshotErrorCode(eng::NativeNamesSnapshotError::database_not_serving),
          "failed and not-serving are distinct codes");
  } else {
    // 0. The names source itself (A1): its rows are the file's, field by field, and it names the principals' scope.
    //    (New in Part A: replaces S4b's per-result field checks; the default flags live only here.)
    {
      const auto snapshot = Rows();
      Check(snapshot.ok() && snapshot.database_uuid == database, "the names source reads the served file");
      Check(snapshot.principal_schemas.size() == 1 && snapshot.principal_schemas.front() == security_scope,
            "the names source reports the authorization policy's user schema as the principals' scope");
      Check(!snapshot.profiles.empty(), "the names source reports the identifier profiles");
      Check(snapshot.rows.size() == offline.names.size(), "the names source lists exactly the offline live entries");
      for (const auto& fact : offline.names) {
        const auto* row = RowOf(snapshot, fact.entry_uuid);
        Check(row != nullptr, "every offline entry is a source row");
        Check(row->object_uuid == fact.object && row->scope_uuid == fact.scope && row->language_uuid == fact.language &&
                  row->object_kind == fact.kind && row->was_quoted == fact.quoted && row->raw_name_text == fact.text &&
                  row->default_for_object == fact.default_for_object && row->default_for_language == fact.default_for_language &&
                  row->name_class == static_cast<std::uint8_t>(fact.name_class),
              "a source row equals the offline entry (identities, kind, text, flags, class)");
        Check(row->catalog_generation_id != 0, "a source row carries its catalog generation");
      }
      // The registry projection (what the existing resolver matches): one entry per source row, nothing from any
      // legacy store.
      const auto loaded = eng::LoadNameRegistryState(MakeRequest(Ask{}).context, 0);
      Check(loaded.ok && loaded.state.entries.size() == snapshot.rows.size(), "the registry state is the source's rows, projected");
    }
    // 1. The bootstrap principal's own name resolves to the bootstrap principal UUID; the S4a login-material read of the
    //    same file for that UUID reports the same principal (the cross-check the owner asked for).
    {
      const auto o = ResolveText(kBootstrapName);
      ExpectObject(o, bootstrap, "the bootstrap principal's name");
      const auto& x = o.result;
      Check(Same(x.bound_object_identity.resolved_schema_uuid, security_scope), "the security schema scope is reported");
      Check(x.primary_object.object_kind == "user", "the object's kind is the user account kind (projected class text)");
      const auto* fact = EntryOf(offline, bootstrap, std::string(kBootstrapName));
      Check(fact != nullptr, "the create's entry");
      const auto entries = Entries(Ask{std::string(kBootstrapName)});
      Check(entries.size() == 1, "the name matches one entry (one distinct object)");
      Check(Same(entries.front().name_entry_uuid, fact->entry_uuid) && entries.front().language_tag == TagFor(fact->language),
            "the language and the entry UUID are the file's");
      Check(entries.front().name_class == "primary", "the create's name is a primary name");
      const auto snapshot = Rows();
      const auto* row = RowOf(snapshot, fact->entry_uuid);
      Check(row != nullptr && row->default_for_object && row->default_for_language, "and the object's and its language's default (source row)");
      // The generation and the epochs the SBLR bind/execute checks read come from the row.
      Check(x.bound_object_identity.catalog_generation_id == row->catalog_generation_id &&
                x.bound_object_identity.object_descriptor_generation == row->catalog_generation_id &&
                x.bound_object_identity.resource_epoch == row->resource_epoch,
            "the resolution carries the row's catalog generation and resource epoch");
      PasswordFactory passwords(database);
      srv::NativeLoopLoginRequest login;
      login.principal_uuid = [&] { p::Uuid id; std::copy(x.primary_object.uuid.bytes.begin(), x.primary_object.uuid.bytes.end(), id.bytes.begin()); return id; }();
      login.password = passwords.Make(kBootstrapPassword);
      login.password_length = kBootstrapPassword.size();
      const auto material = loop.SubmitReadLoginMaterial(std::move(login)).get();
      Check(material.ok() && material.material->principal_uuid == bootstrap && material.material->database_uuid == database,
            "the S4a login-material read of the resolved UUID reports the same principal");
    }
    // 2. Unquoted names fold ASCII upper-case and match case-insensitively (created case is kept).
    //    CHANGES WITH PART B (the engine's fold).
    for (const char* spelling : {"FIXTURE_OWNER", "Fixture_Owner", "fIxTuRe_oWnEr"})
      ExpectObject(ResolveText(spelling), bootstrap, std::string("unquoted ") + spelling);
    // 3. Quoted names match exactly: the unquoted-created name's own lane is its upper-case fold (Q181), so a quoted
    //    "FIXTURE_OWNER" finds it and any other spelling inside quotes does not. CHANGES WITH PART B.
    ExpectObject(ResolveText("FIXTURE_OWNER", true), bootstrap, "quoted exact fold spelling");
    ExpectRefusal(ResolveText("fixture_owner", true), kNotFound, "quoted created spelling");
    ExpectRefusal(ResolveText("Fixture_Owner", true), kNotFound, "quoted mixed spelling");
    // 4. A name that is a prefix or an extension of a real one, an unknown name, and a name with surrounding blanks.
    for (const char* text : {"fixture_owne", "fixture_ownerx", "xfixture_owner", "fixture_owner ", " fixture_owner", "f", "fixture",
                             "no_such_principal_anywhere", "fixture_ownerfixture_owner"})
      ExpectRefusal(ResolveText(text), kNotFound, std::string("not a name of the file: ") + text);
    // 5. Inputs S4b refused as "cannot be an identifier". The existing resolver has no such rules: the engine stores
    //    names as given and the parser owns them (owner 2026-11-20). CHANGES WITH PART B for the empty name (a valid
    //    name, refused today as a missing name); every other one is a plain not-found now (was name_invalid).
    {
      const auto empty = ResolveText("");
      ExpectRefusal(empty, "SB_ENGINE_API_INVALID_REQUEST", "empty name (refused today as name_required; \"\" is a valid name)");
      ExpectRefusal(ResolveText(std::string_view("fixture\0owner", 13)), kNotFound, "a NUL in the name");
      ExpectRefusal(ResolveText(std::string_view("\xC3\x28", 2)), kNotFound, "invalid UTF-8 (a truncated sequence)");
      ExpectRefusal(ResolveText(std::string_view("\xFF\xFEname", 6)), kNotFound, "invalid UTF-8 (bytes never valid)");
      ExpectRefusal(ResolveText(std::string_view("\xED\xA0\x80", 3)), kNotFound, "a UTF-16 surrogate encoded in UTF-8");
      ExpectRefusal(ResolveText(std::string(cat::kCatalogIdentifierTextMaximumBytes + 1, 'a')), kNotFound, "over the old text limit");
      ExpectRefusal(ResolveText(std::string(513, 'a')), kNotFound, "over the old profile length rule");
      ExpectRefusal(ResolveText(std::string(512, 'a')), kNotFound, "a 512-character name is simply absent");
      // A quote style the profile did not admit (backtick): the quoted spelling is compared exactly, as given.
      Ask backtick{std::string(kBootstrapName), true, "backtick"};
      ExpectRefusal(Resolve(backtick), kNotFound, "a backtick-quoted created spelling (exact lane)");
      // Malformed-request refusals of S4b with no counterpart in the existing request: a quoted name with no quote
      // style is just a quoted name; an unquoted name with a quote style ignores the style; a nil scope is an unknown
      // scope. (S4b's kind filter of zero and nil language filter have no field in the existing request: not testable.)
      Ask quoted_without_style{std::string(kBootstrapName), true, "none"};
      ExpectRefusal(Resolve(quoted_without_style), kNotFound, "quoted with no quote style (exact lane)");
      Ask unquoted_with_style{std::string(kBootstrapName), false, "double_quote"};
      ExpectObject(Resolve(unquoted_with_style), bootstrap, "unquoted with a quote style (the style is not used for an unquoted name)");
      Ask nil_scope{std::string(kBootstrapName)};
      nil_scope.scope = p::Uuid{};
      ExpectRefusal(Resolve(nil_scope), kNotFound, "a nil scope");
    }
    // 6. Scope: an explicit scope equal to the file's security schema gives the answer; another scope does not hold the
    //    name; names under the database scope resolve under it.
    {
      Ask explicit_scope{std::string(kBootstrapName)};
      explicit_scope.scope = security_scope;
      ExpectObject(Resolve(explicit_scope), bootstrap, "the explicit security schema scope");
      explicit_scope.scope = database;
      ExpectRefusal(Resolve(explicit_scope), kNotFound, "the database scope does not hold a principal's name");
      explicit_scope.scope = NewId();
      ExpectRefusal(Resolve(explicit_scope), kNotFound, "an unknown scope");
      std::size_t n = 0;
      for (const auto& fact : offline.names) {
        if (fact.scope != database) continue;
        Ask a{fact.text, fact.quoted};
        a.language = TagFor(fact.language);
        a.scope = database;
        a.object_class = ClassFor(fact.kind);
        const auto o = Resolve(a);
        ExpectObject(o, fact.object, "a name under the database scope");
        Check(Same(o.result.bound_object_identity.resolved_schema_uuid, database) && o.result.primary_object.object_kind == ClassFor(fact.kind),
              "its scope and kind are reported");
        ++n;
      }
      Check(n >= 1, "the database scope holds names");
      database_scope_resolved = n;
    }
    // 7. Language and kind. The existing resolver has no language FILTER: the session language (then the database
    //    default, then "und") is tried in order. The entry's own language finds it; another language does not.
    {
      Ask by_language{std::string(kBootstrapName)};
      const auto* fact = EntryOf(offline, bootstrap, std::string(kBootstrapName));
      Check(fact != nullptr, "the create's entry");
      by_language.language = TagFor(fact->language);
      ExpectObject(Resolve(by_language), bootstrap, "the entry's own language");
      by_language.language = cat::kCatalogLanguage_fr_CA == fact->language ? "de-DE" : "fr-CA";
      ExpectRefusal(Resolve(by_language), kNotFound, "another language");
      Ask by_kind{std::string(kBootstrapName)};
      by_kind.object_class = "user";
      ExpectObject(Resolve(by_kind), bootstrap, "the user class");
      by_kind.object_class = "role";
      ExpectRefusal(Resolve(by_kind), kNotFound, "the role class");
    }
    // 8. Service (agent) principals resolve: resolution is a lookup; refusing an agent for external login is the login's
    //    job. L5 by its name, then every principal name of the file (the L5 principal included).
    {
      const auto l5 = ResolveText(kServiceName);
      ExpectObject(l5, service, "the L5 service principal's name");
      Check(l5.result.primary_object.object_kind == "user", "a service principal is the same record kind as a user");
      // A resolve loads the committed name rows (about 50-70 ms); the bootstrap principal, L5 and every fourth other
      // principal name of the file are resolved (each name is checked against the file's own list, not assumed).
      std::set<p::Uuid> distinct;
      std::size_t seen = 0;
      for (const auto& fact : offline.names) {
        if (fact.scope != security_scope || fact.kind != kUserKind) continue;
        if (fact.object != bootstrap && fact.object != service && (seen++ % 4) != 0) continue;
        Ask a{fact.text, fact.quoted};
        a.language = TagFor(fact.language);
        const auto o = Resolve(a);
        ExpectObject(o, fact.object, "a principal name of the file");
        distinct.insert(fact.object);
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
    const auto snapshot = Rows();
    // A second name in the same language: the same UUID, an alias, not the default.
    for (const char* spelling : {"Owner_Alias_Keep", "owner_alias_keep", "OWNER_ALIAS_KEEP"}) {
      const auto o = ResolveText(spelling);
      ExpectObject(o, bootstrap, std::string("the alias ") + spelling);
      const auto entries = Entries(Ask{spelling});
      const auto* row = RowOf(snapshot, alias->entry_uuid);
      Check(entries.size() == 1 && entries.front().name_class == "alias" && Same(entries.front().name_entry_uuid, alias->entry_uuid) &&
                row != nullptr && !row->default_for_object,
            "the alias is reported as an alias that is not the default name, with its own entry UUID");
    }
    // A name in a second language. S4b found it with no language filter; the existing resolver tries the session
    // language only (then the default, then "und"): CHANGES WITH PART B (the language fallback). In French and not in
    // another language, as before.
    {
      ExpectRefusal(ResolveText(kAliasFrench), kNotFound, "the French name, asked in the English session (no any-language search)");
      Ask in_french{std::string(kAliasFrench)};
      in_french.language = "fr-CA";
      const auto o = Resolve(in_french);
      ExpectObject(o, bootstrap, "the French name in French");
      const auto entries = Entries(in_french);
      const auto* row = RowOf(snapshot, french->entry_uuid);
      Check(entries.size() == 1 && entries.front().language_tag == "fr-CA" && Same(entries.front().name_entry_uuid, french->entry_uuid) &&
                row != nullptr && row->default_for_language && !row->default_for_object,
            "the French name reports its language and that it is its language's default (source row)");
      in_french.language = french->language == cat::kCatalogLanguage_fr_CA ? "en" : TagFor(french->language);
      ExpectRefusal(Resolve(in_french), kNotFound, "the French name asked in the other language");
    }
    // A quoted name matches exactly; unquoted spellings fold to upper-case and do not find a quoted-only mixed-case name.
    {
      ExpectObject(ResolveText(kQuotedMixed, true), bootstrap, "the quoted name, quoted");
      ExpectRefusal(ResolveText("mixed_q", true), kNotFound, "the quoted name in another case, quoted");
      ExpectRefusal(ResolveText(kQuotedMixed), kNotFound, "a quoted-only mixed-case name asked unquoted (Q180)");
      ExpectRefusal(ResolveText("MIXED_Q"), kNotFound, "the quoted name's fold asked unquoted (Q180)");
      ExpectRefusal(ResolveText("MIXED_Q", true), kNotFound, "the quoted name's fold asked quoted");
    }
    // A quoted case-only twin: both exist (each quoted), each finds its own object, quoted; unquoted finds only the
    // upper-case twin (the quoted upper-case name's own lane is its exact upper-case text).
    {
      ExpectObject(ResolveText(kTwinBootstrap, true), bootstrap, "the quoted twin on the bootstrap principal");
      ExpectObject(ResolveText(kTwinService, true), service, "the quoted twin on L5");
      ExpectRefusal(ResolveText("twinname", true), kNotFound, "a third spelling, quoted");
      ExpectObject(ResolveText("twinname"), service, "unquoted twin spelling folds to the upper-case twin");
      ExpectObject(ResolveText("TwinName"), service, "unquoted twin spelling, mixed case");
    }
    // Every live name of the bootstrap principal (the file's own list) resolves to the one UUID.
    {
      std::size_t count = 0;
      for (const auto& fact : offline.names) {
        if (fact.object != bootstrap) continue;
        Ask a{fact.text, fact.quoted};
        a.language = TagFor(fact.language);
        ExpectObject(Resolve(a), bootstrap, "every name the bootstrap principal has");
        ++count;
      }
      Check(count == (state == "dead" ? 5u : 6u), "the bootstrap principal carries the create's name and the forged live ones");
    }
    // The alias the dead state drops is a live name until then; once dropped (a committed delete-marker successor) it
    // does not resolve, whatever its case.
    if (state == "dead") {
      Check(EntryOf(offline, bootstrap, std::string(kAliasDrop)) == nullptr, "the dropped alias is not listed offline either");
      ExpectRefusal(ResolveText(kAliasDrop), kNotFound, "the dropped alias");
      ExpectRefusal(ResolveText("drop_alias"), kNotFound, "the dropped alias, other case");
      ExpectRefusal(ResolveText("DROP_ALIAS", true), kNotFound, "the dropped alias, quoted");
    } else {
      ExpectObject(ResolveText(kAliasDrop), bootstrap, "the alias that a later transaction drops, still live");
    }
    if (state == "uncommitted") {
      // The never-committed name's transaction was killed by the open's Restart: it must not resolve.
      ExpectRefusal(ResolveText(kNeverCommitted), kNotFound, "a name written by a transaction that never committed");
    }
  }

  if (state != "failed") {
    // SB_PARSER_NAME_RESOLUTION differential (Part B step S1): see DifferentialAgainstModule.
    {
      const auto started_at = std::chrono::steady_clock::now();
      const auto counts = DifferentialAgainstModule(offline, bootstrap, service);
      std::cout << "INFO name_module_differential cases=" << counts.cases << " engine_calls=" << counts.engine_calls
                << " found=" << counts.found << " not_found=" << counts.not_found << " refused_other=" << counts.invalid
                << " rewritten=" << counts.rewritten << " seconds="
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count() << std::endl;
    }
    // The direct in-loop path (A1; S4c's login runs as loop work). The names source called ON the loop thread reads
    // directly and returns the same rows as the cross-thread call; the loop-thread-only entry refuses elsewhere; the
    // cross-thread submit refuses from the loop thread (the loop never waits on itself). New in Part A.
    {
      const auto off_loop = Rows();
      Check(off_loop.ok(), "the cross-thread read");
      const auto refused_here = loop.ReadNamesSnapshotOnLoopThread();
      Check(refused_here.error == eng::NativeNamesSnapshotError::invalid_request, "the loop-thread entry refuses a caller off the loop thread");
      eng::NativeNamesSnapshot in_loop, direct, submit_from_loop;
      const bool ran = loop.RunOnLoopThreadForTest([&] {
        in_loop = env.facts->names_source->Read();             // the source, on the loop thread: the direct path
        direct = loop.ReadNamesSnapshotOnLoopThread();         // the loop-thread entry itself
        submit_from_loop = loop.SubmitReadNamesSnapshot().get();  // refused, never deadlocks
        // And the whole existing resolver from loop work: S4c's login will do exactly this.
        const auto resolved = eng::EngineResolveName(MakeRequest(Ask{std::string(kBootstrapName)}));
        if (!resolved.ok || !Same(resolved.primary_object.uuid, bootstrap)) in_loop.error = eng::NativeNamesSnapshotError::names_source_failure;
      });
      Check(ran, "work ran on the loop thread");
      Check(in_loop.ok() && direct.ok() && in_loop.rows.size() == off_loop.rows.size() && direct.rows.size() == off_loop.rows.size(),
            "the in-loop path returns the same rows (and EngineResolveName works from loop work)");
      Check(submit_from_loop.error == eng::NativeNamesSnapshotError::invalid_request, "the cross-thread submit refuses from the loop thread");
    }
    // Concurrent callers: the existing resolver from three threads; every load is a request on the loop's queue,
    // serialized there; each gets its own correct answer.
    std::vector<std::thread> callers;
    std::vector<std::string> failures(3);
    for (int t = 0; t < 3; ++t)
      callers.emplace_back([&, t] {
        for (int i = 0; i < 4; ++i) {
          const auto a = eng::EngineResolveName(MakeRequest(Ask{std::string(kBootstrapName)}));
          const auto b = eng::EngineResolveName(MakeRequest(Ask{"no_such_principal_anywhere"}));
          if (!a.ok || !Same(a.primary_object.uuid, bootstrap) || b.ok || b.diagnostics.empty() || b.diagnostics.front().code != kNotFound)
            failures[t] = "wrong answer";
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
  // A source that outlives its loop answers `closed` (the binding is cleared when the loop starts to close).
  ExpectRefusal(ResolveText(kBootstrapName), eng::NativeNamesSnapshotErrorCode(eng::NativeNamesSnapshotError::closed),
                "a resolution after the loop closed");
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
