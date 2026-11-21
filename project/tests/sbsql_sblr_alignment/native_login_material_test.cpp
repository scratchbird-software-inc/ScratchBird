// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_LOGIN_MATERIAL_TEST (thin-slice S4a, plan 40; owner Q306-Q312, 2026-11-19/20). A native
// credential read with NO session: NativeDatabaseLoop::SubmitReadLoginMaterial reads the principal's committed
// native security rows from the served file on the database loop thread, verifies the supplied password (protected
// memory) against the stored PBKDF2 verifier, and returns the principal's facts or a specific refusal.
//
// Every case runs against a PRODUCTION-created database (EngineCreateLifecycle through the native create fixture):
// the bootstrap principal's verifier is the real PBKDF2 of a known test password, so the plaintext is known and can
// be searched for in every output. One served child per (page size, state): one StartHostedEngine gives one process
// gate bind. Modes (a child process each):
//   create <path> <page_size>     engine-API native create; the bootstrap credential derived from kBootstrapPassword
//   serve <path> <state>          state: serving | restricted | failed. One StartHostedEngine (restricted: the file
//                                 carries a committing slot written by the native_serving binary's own writer;
//                                 failed: an active slot and the Restart seam failing its first write, the loop's
//                                 own SB_TEST_SEAM), then the login cases for that state; prints "LOGIN_MATERIAL_OK"
//   forge <path> <kind>           a committed successor version of the bootstrap principal's security row, written
//                                 by the engine's own in-place writers (BEGIN, change event, successor, COMMIT):
//                                 kind disabled | nocredential | (S4e Part 1) role_connect | role_noconnect | group_connect |
//                                 transitive | role_revoked | role_deny: successors of the bootstrap principal's membership row
//                                 (another parent) and of the policy pack's CONNECT grant (retired / deny)
// With no argument the driver runs every size and state; "size <bytes>" runs one page size; "common" the cases that
// need no page size. The strace half is native_login_material_strace.py.

#include "engine/internal_api/security/agent_password_derivation.hpp"
#include "engine/internal_api/security/authentication_api.hpp"
#include "engine/internal_api/security/native_runtime_security_observation.hpp"
#include "hash_digest.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "local_password_verifier.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "filespace_page_zero.hpp"
#include "catalog_native_security_record.hpp"
#include "catalog_page.hpp"
#include "native_catalog_leaf_page.hpp"
#include "native_catalog_version_writer.hpp"
#include "native_index_btree_page.hpp"
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
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace db = scratchbird::storage::database;
namespace srv = scratchbird::server;
namespace api = scratchbird::engine::internal_api;
namespace mem = scratchbird::core::memory;
namespace cat = scratchbird::core::catalog;
namespace fs = std::filesystem;
using E = srv::NativeLoginMaterialError;

// Test passwords: distinctive, so a leak is a byte-exact search hit. Never put in an argv (strace's execve line).
constexpr std::string_view kBootstrapPassword = "S4a-Bootstrap-Pw-7f3a91c2d8";
constexpr std::string_view kWrongPassword = "S4a-Wrong-Pw-0c2d55e1b9aa";

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

// ---- create --------------------------------------------------------------------------------------------------
int CreateMode(const fs::path& path, std::uint32_t page_size) {
  std::string verifier;
  if (!scratchbird::core::crypto::DeriveLocalPasswordVerifier(kBootstrapPassword, &verifier)) return 1;
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "native_login_material";
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

// ---- the offline read of the principals (the production observation reader over a fixed-root lease) ------------
struct PrincipalFact {
  p::Uuid uuid;
  cat::CatalogSecurityPrincipalKind kind = cat::CatalogSecurityPrincipalKind::none;
  bool active = false, bootstrap = false, has_credential = false, has_seed = false;
  std::array<unsigned char, 16> derived{};  // database UUID XOR seed (agents); a secret, never printed
};
struct OfflineRead {
  std::vector<PrincipalFact> principals;
  std::vector<p::Uuid> roles, groups;
};

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
  static p::Uuid Fresh() {
    const auto id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    Check(id.has_value(), "an identity for the offline read");
    return *id;
  }
  BoundMemory(const p::Uuid& database, std::uint64_t bytes)
      : manager(Policy(bytes)), binding{database, Fresh(), Fresh(), Fresh()} {
    mem::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = mem::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = mem::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "native login material test read";
    r.purpose = "the test's own offline security read";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[mem::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{mem::HierarchicalMemoryScopeKind::process, {}, Fresh().bytes},
                     {mem::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "native login material test";
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

// Reads every committed principal, role and group of the file as an independent read-only reader (no server).
// The runtime binding is the one the create receipt names: found through the configuration relation by the same
// production reader the server uses (ReadStartupServiceIdentity is the server's; here the binding UUID is read
// from the create's own output).
OfflineRead ReadOffline(const fs::path& path, const p::Uuid& binding_uuid) {
  OfflineRead out;
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "the offline read opens the file read-only");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  const auto database = zero.record->bootstrap.database_uuid;
  const auto filespace = zero.record->bootstrap.filespace_uuid;
  const auto profile = zero.record->bootstrap.page_size_profile_uuid;
  const auto budget = db::NativeHostedSourceBackingBytes(size);
  constexpr auto rows = db::kNativeHostedCatalogRelationMaximumRows;
  const db::NativeCatalogRelationMemoryLimits limits{profile, 1, db::NativeHostedCatalogBasePages(size), rows, rows,
                                                     db::NativeHostedCatalogNavigationLimits(size)};
  const auto bytes = api::NativeRuntimeSecurityWorkspaceBytes(limits);
  Check(bytes != 0, "a security read workspace size");
  BoundMemory workspace_memory(database, bytes), source_memory(database, 32ull * 1024 * 1024);
  auto prepared = api::PrepareNativeRuntimeSecurityRead(limits, workspace_memory.memory, workspace_memory.binding);
  Check(prepared.ok(), "the security read workspace prepares");
  const std::array<d::NativeFilespaceDevice, 1> devices{{{filespace, profile, &device}}};
  auto source = db::AcquireNativeFixedRootMemoryLease(database, devices, filespace, {budget, 32ull * 1024 * 1024},
                                                      source_memory.memory, source_memory.binding);
  Check(source.ok(), "a fixed-root lease over the file");
  api::NativeRuntimeSecurityReadRequest request;
  request.database_uuid = database;
  request.runtime_binding_uuid = binding_uuid;
  request.runtime_binding_generation = 1;
  request.maximum_retained_image_bytes = budget;
  request.catalog_selection = api::NativeRuntimeCatalogSelection::discover_selected;
  const auto observed = api::ReadCommittedNativeRuntimeSecurityOnLease(*source.lease, request, prepared.workspace);
  Check(observed.ok(), "the committed security rows read through the production observer");
  for (const auto& record : observed.records) {
    if (const auto* principal = record.record.principal()) {
      PrincipalFact fact;
      fact.uuid = record.object_uuid;
      fact.kind = principal->principal_kind;
      fact.active = principal->active;
      fact.bootstrap = principal->bootstrap_principal;
      fact.has_credential = principal->credential.has_value();
      fact.has_seed = principal->password_seed.has_value();
      if (principal->password_seed)
        fact.derived = api::DeriveAgentPassword(database, std::span<const p::byte, 16>(*principal->password_seed));
      out.principals.push_back(fact);
    }
    if (record.record.role()) out.roles.push_back(record.object_uuid);
    if (record.record.group()) out.groups.push_back(record.object_uuid);
  }
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return out;
}

// ---- the protected password ------------------------------------------------------------------------------------
struct PasswordFactory {
  mem::MemoryManager manager{mem::DefaultLocalEngineMemoryPolicy()};
  mem::MemoryTag tag;
  explicit PasswordFactory(const p::Uuid& database) {
    tag.category = mem::MemoryCategory::core_runtime;
    tag.purpose = "native login material test password";
    tag.callsite = "native_login_material_test";
    tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = database.bytes;
  }
  // `capacity` >= bytes.size(); the password is the first bytes.size() bytes.
  mem::ScopedProtectedBuffer Make(std::string_view bytes, std::size_t capacity = 0) {
    mem::ProtectedMemoryRequest request;
    request.bytes = std::max<std::size_t>(capacity, std::max<std::size_t>(bytes.size(), 1));
    request.tag = tag;
    request.material_class = "native login material test password";
    request.platform_policy = mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto allocated = manager.AllocateProtected(request);
    Check(allocated.ok(), "protected memory for the test password");
    if (!bytes.empty()) std::memcpy(allocated.buffer.data(), bytes.data(), bytes.size());
    return std::move(allocated.buffer);
  }
};

std::mutex g_password_mutex;
mem::ScopedProtectedBuffer passwords_mutex_make(PasswordFactory& passwords, std::string_view bytes) {
  std::lock_guard<std::mutex> guard(g_password_mutex);
  return passwords.Make(bytes);
}

struct Outcome {
  srv::NativeLoginMaterialResult result;
  std::string summary;  // everything the test says about the result: structured only
  double milliseconds = 0;
};
std::string Summary(const srv::NativeLoginMaterialResult& r) {
  std::ostringstream out;
  out << "error=" << srv::NativeLoginMaterialErrorName(r.error) << " code=" << srv::NativeLoginMaterialErrorCode(r.error)
      << " security_error=" << static_cast<unsigned>(r.security_error) << " source_error=" << static_cast<unsigned>(r.source_error)
      << " custody_error=" << static_cast<unsigned>(r.custody_error) << " verifier_error=" << static_cast<unsigned>(r.verifier_error);
  if (r.material) {
    const auto& m = *r.material;
    out << " principal=" << Hex(m.principal_uuid) << " roles=" << m.role_uuids.size() << " groups=" << m.group_uuids.size()
        << " edges=" << m.edges.size() << " catalog_generation=" << m.catalog_generation;
  }
  return out.str();
}

std::vector<std::string> all_summaries;

Outcome Login(srv::NativeDatabaseLoop& loop, PasswordFactory& passwords, const p::Uuid& principal, std::string_view password,
              std::size_t capacity = 0, std::size_t length_override = SIZE_MAX) {
  srv::NativeLoopLoginRequest request;
  request.principal_uuid = principal;
  request.password = passwords.Make(password, capacity);
  request.password_length = length_override == SIZE_MAX ? password.size() : length_override;
  const auto started = std::chrono::steady_clock::now();
  Outcome out;
  out.result = loop.SubmitReadLoginMaterial(std::move(request)).get();
  out.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  out.summary = Summary(out.result);
  all_summaries.push_back(out.summary);
  return out;
}

void ExpectRefusal(const Outcome& o, E expected, const std::string& what) {
  Check(!o.result.ok() && !o.result.material.has_value(), what + ": not authenticated, no material: " + o.summary);
  Check(o.result.error == expected, what + ": expected " + srv::NativeLoginMaterialErrorName(expected) + ", got " + o.summary);
  Check(std::string(srv::NativeLoginMaterialErrorCode(o.result.error)).starts_with("SECURITY."), what + ": a stable code");
}

// ---- forge: a committed successor version of the bootstrap principal's security row ------------------------------
// The engine has no statement that disables a principal or drops its verifier yet, so the change is made the way the
// engine's own in-place writers make any catalog change (the same BEGIN / change event / successor version / COMMIT
// the writers' tests use): a SECOND, COMMITTED version of the bootstrap principal's row, carrying active=false or no
// credential. Nothing is hand-patched. Kind: disabled | nocredential.
int ForgeMode(const fs::path& path, const std::string& kind, const p::Uuid& principal_uuid) {
  namespace tip = scratchbird::storage::page;
  d::FileDevice device;
  if (!device.Open(path.string(), d::FileOpenMode::open_existing, d::DatabaseFileOwnership::native_data_file).ok()) return 1;
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const std::uint64_t size = zero.record->bootstrap.page_size_bytes;
  const auto database = zero.record->bootstrap.database_uuid;
  std::optional<d::FilespaceRootReference> catalog_root, state_root, allocation_root, configuration_root, security_root;
  for (const auto& root : zero.record->roots) {
    if (root.kind == 2) catalog_root = root;
    if (root.kind == 4) state_root = root;
    if (root.kind == 3) allocation_root = root;
    if (root.kind == 6) configuration_root = root;
    if (root.kind == 7) security_root = root;
  }
  Check(catalog_root && state_root && allocation_root && configuration_root && security_root, "page zero names the roots");
  const auto policy = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(policy.has_value(), "an issuer identity");
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({database, *policy}, scratchbird::core::uuid::kStandardLocalClockUuidV7Policy);
  tip::TransactionWriterBinding binding;
  binding.database_uuid = database;
  binding.state_root = *state_root;
  binding.allocation_root = *allocation_root;
  binding.issuer = &issuer;
  d::FileDeviceCarefulWriteSink sink(device);
  tip::TransactionInventoryWriter writer(binding);
  Check(writer.Restart(sink).ok(), "the writer restarts the created file");
  const auto id = [&] {
    const auto v = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    Check(v.has_value(), "an identity");
    return *v;
  };
  tip::TransactionHeaderRecord header;
  header.transaction_uuid = id();
  const auto begun = writer.Begin(sink, header);
  Check(begun.ok() && begun.transaction_order.has_value(), "BEGIN of the change transaction");
  const std::uint64_t order = *begun.transaction_order;
  db::CatalogVersionWriterBinding cv;
  cv.database_uuid = database;
  cv.catalog_root = *catalog_root;
  cv.state_root = *state_root;
  cv.configuration_root = *configuration_root;
  cv.security_root = *security_root;
  cv.allocation_root = *allocation_root;   // S4e: a successor that finds no room on its page goes on a new base leaf (needs the allocation map)
  const auto event = id(), event_row = id(), event_version = id(), engine = id();
  const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto written_event = scratchbird::tests::catalog_change_event::WriteEvent(
      sink, cv, order, header.transaction_uuid, event, event_row, event_version, engine, now_us);
  Check(written_event.ok(), "the transaction's catalog.metadata_change event");
  // Find the principal's row in the security relation (role 4): the catalog root, the relation's index, its base leaves.
  const auto page = [&](std::uint64_t number) {
    std::vector<p::byte> bytes(size);
    const auto io = sink.ReadAt(number * size, bytes);
    Check(io.ok() && io.bytes_transferred == size, "read a catalog page");
    return bytes;
  };
  const auto root = tip::DecodeNativeCatalogRoot(page(catalog_root->page_number));
  Check(root.ok(), "the catalog root decodes");
  const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(), [](const auto& r) { return r.role == 4; });
  Check(head != root.root->roots.end(), "the security relation has a head");
  std::set<std::uint64_t> bases, seen;
  std::vector<std::uint64_t> stack{head->page.page_number};
  if (head->page_type != 0x200) bases.insert(head->page.page_number);
  else
    while (!stack.empty()) {
      const auto number = stack.back();
      stack.pop_back();
      if (!seen.insert(number).second) continue;
      const auto node = tip::DecodeNativeBtreePage(page(number));
      Check(node.ok(), "a security index page decodes");
      if (node.page->first_child) stack.push_back(node.page->first_child->page_number);
      for (const auto& cell : node.page->cells) {
        if (cell.child) stack.push_back(cell.child->page_number);
        if (cell.base_page) bases.insert(cell.base_page->page_number);
      }
    }
  // ---- what to change: one or two committed successor versions in the one transaction ----
  struct Planned {
    cat::CatalogMetadataVersion meta;
    p::Uuid row, version;
    std::string payload;
    bool retire = false;   // the successor also RETIRES the row (a revoked grant): retired_transaction_uuid = this transaction
  };
  std::vector<Planned> planned;
  struct Found {
    cat::CatalogMetadataVersion meta;
    p::Uuid row, version;
  };
  // The newest version of the first row that satisfies `want` (rows are visited in leaf order; a fresh file has one version each).
  const auto find = [&](const auto& want) {
    std::optional<Found> out;
    for (const auto number : bases) {
      const auto leaf = db::DecodeNativeCatalogLeaf(page(number));
      Check(leaf.ok(), "a security leaf decodes");
      for (const auto& row : leaf.page->body.rows) {
        const auto& m = leaf.metadata.at(row.version_uuid);
        if (want(m)) out = Found{m, row.row_uuid.value, row.version_uuid};
      }
    }
    return out;
  };
  const auto grant_of = [&](const Found& f) {
    auto record = cat::DecodeNativeSecurityRecord(f.meta.record.header.kind, f.meta.record.payload);
    Check(record.ok() && record.record->grant(), "the grant row decodes");
    return *record.record->grant();
  };
  // S4e Part 1 (NEW) forge kinds: the role / group privilege cases need NO new row (the writer has no insert): the
  // bootstrap principal's own membership row (member = principal, parent = the engine-owned SYSARCH role) gets a successor
  // with another PARENT, and the default policy pack's own grant rows get successors. Pack identities (resources/
  // policy-packs/default-local-password): role ...0100 holds CONNECT (grant ...0300), role ...0101 holds no CONNECT, group
  // ...0200 holds CONNECT (grant ...030a), group ...0201 is a member of role ...0100 (pack membership).
  const auto role_0100 = FromHex("018f7a10128070008000000000000100"), role_0101 = FromHex("018f7a10128070008000000000000101"),
             group_0200 = FromHex("018f7a10128070008000000000000200"), group_0201 = FromHex("018f7a10128070008000000000000201"),
             grant_0300 = FromHex("018f7a10128070008000000000000300");
  if (kind == "role_connect" || kind == "role_noconnect" || kind == "group_connect" || kind == "transitive" ||
      kind == "role_revoked" || kind == "role_deny") {
    const auto membership = find([&](const cat::CatalogMetadataVersion& m) {
      if (m.record.header.kind != cat::CatalogRecordKind::grant_record) return false;
      const auto r = cat::DecodeNativeSecurityRecord(m.record.header.kind, m.record.payload);
      if (!r.ok() || !r.record->grant()) return false;
      const auto& g = *r.record->grant();
      return g.grant_class == cat::CatalogSecurityGrantClass::role_membership && g.member_uuid && *g.member_uuid == principal_uuid;
    });
    Check(membership.has_value(), "the bootstrap principal's membership row is in the relation");
    auto grant = grant_of(*membership);
    p::Uuid parent = role_0100;
    cat::CatalogSecuritySubjectKind parent_kind = cat::CatalogSecuritySubjectKind::role;
    if (kind == "role_noconnect") parent = role_0101;
    if (kind == "group_connect") { parent = group_0200; parent_kind = cat::CatalogSecuritySubjectKind::group; }
    if (kind == "transitive") { parent = group_0201; parent_kind = cat::CatalogSecuritySubjectKind::group; }
    grant.parent_uuid = parent;
    grant.parent_kind = parent_kind;
    grant.role_uuid = parent_kind == cat::CatalogSecuritySubjectKind::role ? std::optional<p::Uuid>(parent) : std::nullopt;
    const auto payload = cat::EncodeNativeSecurityGrant(grant);
    Check(payload.ok(), "the successor membership encodes under the production codec");
    planned.push_back({membership->meta, membership->row, membership->version,
                       std::string(payload.bytes.begin(), payload.bytes.end()), false});
    if (kind == "role_revoked" || kind == "role_deny") {
      const auto connect = find([&](const cat::CatalogMetadataVersion& m) { return m.record.header.object_uuid.value == grant_0300; });
      Check(connect.has_value(), "the pack's CONNECT grant of role ...0100 is in the relation");
      auto cg = grant_of(*connect);
      Check(cg.right == cat::CatalogSecurityRight::CONNECT && cg.effect == cat::CatalogSecurityEffect::allow, "it is an allow CONNECT");
      if (kind == "role_deny") cg.effect = cat::CatalogSecurityEffect::deny;
      const auto cpayload = cat::EncodeNativeSecurityGrant(cg);
      Check(cpayload.ok(), "the successor grant encodes under the production codec");
      planned.push_back({connect->meta, connect->row, connect->version,
                         std::string(cpayload.bytes.begin(), cpayload.bytes.end()), kind == "role_revoked"});
    }
  } else {
    const auto principal_row = find([&](const cat::CatalogMetadataVersion& m) {
      return m.record.header.object_uuid.value == principal_uuid && m.record.header.kind == cat::CatalogRecordKind::user_account;
    });
    Check(principal_row.has_value(), "the bootstrap principal's security row is in the relation");
    auto record = cat::DecodeNativeSecurityRecord(principal_row->meta.record.header.kind, principal_row->meta.record.payload);
    Check(record.ok() && record.record->principal(), "the principal row decodes");
    auto principal = *record.record->principal();
    if (kind == "disabled") principal.active = false;
    else if (kind == "nocredential") principal.credential.reset();
    else return 2;
    const auto payload = cat::EncodeNativeSecurityPrincipal(principal);
    Check(payload.ok(), "the successor payload encodes under the production codec");
    planned.push_back({principal_row->meta, principal_row->row, principal_row->version,
                       std::string(payload.bytes.begin(), payload.bytes.end()), false});
  }
  for (const auto& plan : planned) {
    db::CatalogVersionRequest request;
    request.role = 4;
    request.transaction_order = order;
    request.transaction_uuid = header.transaction_uuid;
    request.row_uuid = plan.row;
    request.predecessor_version_uuid = plan.version;
    request.version_uuid = id();
    request.metadata = plan.meta;
    request.metadata.record.payload.assign(plan.payload.begin(), plan.payload.end());
    ++request.metadata.definition_version;
    request.metadata.creator_transaction_uuid = {p::UuidKind::transaction, header.transaction_uuid};
    request.metadata.creator_local_transaction_id = order;
    request.metadata.audit_uuid = {p::UuidKind::object, event};
    if (plan.retire) {
      // A revoked grant is a DROPPED row: the shape the version writer's own drop uses (header deleted, lifecycle dropped,
      // status retired, retired by this transaction, payload unchanged); the writer refuses any other retirement.
      request.metadata.record.header.deleted = true;
      request.metadata.lifecycle = cat::CatalogObjectLifecycle::dropped;
      request.metadata.status = cat::CatalogObjectStatus::retired;
      request.metadata.retired_transaction_uuid = {p::UuidKind::transaction, header.transaction_uuid};
      // A definition drop also drops its fully dependent rows (live comments): their identities, caller-issued; unused ones
      // are not consumed.
      for (int i = 0; i < 8; ++i) request.dependent_versions.push_back(id());
      for (int i = 0; i < 8; ++i) request.new_pages.push_back({id(), id()});
    }
    const auto written = db::WriteCatalogSuccessorVersion(sink, cv, request);
    if (!written.ok()) std::cerr << "successor refused: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
    Check(written.ok(), "the catalog version writer writes the successor in place");
  }
  const auto done_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  Check(writer.Commit(sink, order, done_ns).ok(), "COMMIT of the change transaction");
  Check(!writer.failed(), "the writer is sound");
  Check(device.Close().ok(), "close");
  std::cout << "FORGED kind=" << kind << " order=" << order << std::endl;
  return 0;
}

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

// serve <path> <state> <database_hex> <bootstrap_hex> <service_hex> <binding_hex>: the identities come from the create
// child's output (the driver passes them on); the binding UUID is read from the file here.
int ServeMode(const fs::path& path, const std::string& state, const p::Uuid& database, const p::Uuid& bootstrap,
              const p::Uuid& service, const p::Uuid& binding, const std::string& expectation) {
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
  // (The process crypto owner is started before anything touches OpenSSL, the digest in Snapshot included.)
  const auto before = Snapshot(directory);
  // The offline read happens BEFORE the server owns the file (after the process memory setup, which must come
  // first): it gives the agents' derived passwords (database UUID XOR the stored seed) and the full principal list,
  // the ground truth the served read is compared with.
  const auto offline = ReadOffline(path, binding);
  if (state == "failed")
    srv::SetNativeRestartSinkWrapperForTest([](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
      return std::make_unique<FailFirstWriteSink>(inner);
    });
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  // A failed Restart makes StartHostedEngine report a diagnostic by design (Q292: the server exits); the state is
  // still held for the caller. Every other state starts clean.
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
  const bool forged = state == "disabled" || state == "nocredential";
  const std::string expected_state = forged ? "serving" : state;
  Check(std::string(srv::NativeServingStateName(loop.serving_state())) == expected_state,
        "the database's serving state is " + expected_state);
  Check(!loop.on_loop_thread(), "the test is not on the loop thread");

  PasswordFactory passwords(database);
  std::vector<Outcome> timings;

  if (state == "failed") {
    // `failed` is refused before anything is queued; the password is consumed.
    const auto o = Login(loop, passwords, bootstrap, kBootstrapPassword);
    ExpectRefusal(o, E::database_failed, "failed state");
    Check(o.result.error != E::none && std::string(srv::NativeLoginMaterialErrorCode(E::database_failed)) !=
                                           srv::NativeLoginMaterialErrorCode(E::database_not_serving),
          "failed and not-serving are distinct codes");
  } else if (forged) {
    // The bootstrap principal's committed successor row (made by the engine's writers, see ForgeMode) is disabled /
    // has no verifier. The right and the wrong password are refused with the SAME specific code: the principal's
    // state decides, not the password. Other principals are untouched.
    const E expected = state == "disabled" ? E::principal_not_active : E::credential_absent;
    const auto right = Login(loop, passwords, bootstrap, kBootstrapPassword);
    ExpectRefusal(right, expected, state + " principal, the password that was right before the change");
    const auto wrong = Login(loop, passwords, bootstrap, kWrongPassword);
    ExpectRefusal(wrong, expected, state + " principal, a wrong password");
    Check(right.summary == wrong.summary, "the refusal does not depend on the password: identical vectors");
    // A principal of the same file the change did not touch: L5 is still refused as an agent, an absent one is still absent.
    ExpectRefusal(Login(loop, passwords, service, kWrongPassword), E::external_binding_refused, "L5 after the change");
    const auto absent_id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    Check(absent_id.has_value(), "an identity for the absent principal");
    ExpectRefusal(Login(loop, passwords, *absent_id, kBootstrapPassword), E::principal_absent, "an absent principal after the change");
    timings.push_back(right);
    timings.push_back(wrong);
  } else {
    // 1. The bootstrap user, the right password: authenticated, with its facts.
    const auto good = Login(loop, passwords, bootstrap, kBootstrapPassword);
    Check(good.result.ok(), "bootstrap + right password: authenticated: " + good.summary);
    const auto& m = *good.result.material;
    Check(m.principal_uuid == bootstrap && m.database_uuid == database, "the principal UUID and the database UUID are the create's");
    Check(m.principal_kind == cat::CatalogSecurityPrincipalKind::user && m.bootstrap_principal,
          "the bootstrap principal is a user principal flagged bootstrap");
    Check(m.security_context_authority_version == 1 && m.security_context_generation && m.security_generation &&
              m.policy_generation && m.definition_generation && m.catalog_generation && m.schema_epoch && m.security_epoch,
          "the generations and epochs the later adapter fences on are present");
    Check(!m.principal_version_uuid.is_nil(), "the principal row's version identity is reported");
    Check(!m.role_uuids.empty(), "the bootstrap principal has at least one direct role (the create's membership grant)");
    for (const auto& role : m.role_uuids)
      Check(std::find(offline.roles.begin(), offline.roles.end(), role) != offline.roles.end(),
            "every reported role UUID is a role row of the file");
    Check(!m.edges.empty(), "the principal's direct grant-family rows are reported");
    for (const auto& edge : m.edges)
      Check(edge.member_uuid == bootstrap || edge.subject_uuid == bootstrap,
            "every reported edge names the authenticated principal");
    Check(m.group_uuids.empty() || std::all_of(m.group_uuids.begin(), m.group_uuids.end(), [&](const auto& g) {
            return std::find(offline.groups.begin(), offline.groups.end(), g) != offline.groups.end();
          }), "every reported group UUID is a group row of the file");
    timings.push_back(good);
    std::cout << "INFO seeded=" << std::count_if(offline.principals.begin(), offline.principals.end(), [](const auto& f) { return f.has_seed; })
              << " with_credential=" << std::count_if(offline.principals.begin(), offline.principals.end(), [](const auto& f) { return f.has_credential; })
              << " inactive=" << std::count_if(offline.principals.begin(), offline.principals.end(), [](const auto& f) { return !f.active; })
              << " principals=" << offline.principals.size() << " roles_in_file=" << offline.roles.size()
              << " groups_in_file=" << offline.groups.size() << " bootstrap_roles=" << m.role_uuids.size()
              << " bootstrap_groups=" << m.group_uuids.size() << " bootstrap_edges=" << m.edges.size()
              << " effective_roles=" << m.effective_role_uuids.size() << " effective_groups=" << m.effective_group_uuids.size()  // S4e Part 1 (NEW)
              << " generations=" << m.security_generation << "/" << m.policy_generation << "/" << m.security_context_generation
              << std::endl;

    // 2. The wrong password: the mismatch code, and only that.
    const auto wrong = Login(loop, passwords, bootstrap, kWrongPassword);
    ExpectRefusal(wrong, E::password_mismatch, "wrong password");
    Check(wrong.result.verifier_error == api::LocalPasswordCredentialError::mismatch, "the verifier said mismatch");
    timings.push_back(wrong);

    // 3. An absent principal UUID (a fresh, valid identity that names no row).
    const auto absent_id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    Check(absent_id.has_value(), "an identity for the absent principal");
    const auto absent = Login(loop, passwords, *absent_id, kBootstrapPassword);
    ExpectRefusal(absent, E::principal_absent, "absent principal");
    timings.push_back(absent);

    // 4. A row that is not a principal.
    Check(!offline.roles.empty(), "the file holds a role row");
    ExpectRefusal(Login(loop, passwords, offline.roles.front(), kBootstrapPassword), E::principal_wrong_family,
                  "a role UUID as the principal");

    // 5. Every agent / service principal is refused for external login, with the right password, with a wrong one,
    //    and the two refusals are the same code (no oracle). L5's correct derived password is known (offline read).
    //    The create writes ~160 agent principals; a login costs a PBKDF2 (~0.2 s), so the login is exercised for L5
    //    and an evenly spaced sample of ten of the rest. The KIND of every one is
    //    checked from the offline read (no login).
    std::size_t agents = 0, with_password = 0, index = 0;
    std::size_t other_agents = 0;
    for (const auto& fact : offline.principals)
      if (fact.uuid != bootstrap && fact.uuid != service) ++other_agents;
    const std::size_t stride = std::max<std::size_t>(1, other_agents / 10);
    for (const auto& fact : offline.principals) {
      if (fact.uuid == bootstrap) continue;
      Check(fact.kind == cat::CatalogSecurityPrincipalKind::service,
            "every principal other than the bootstrap user is a service (agent) principal");
      ++agents;
      const bool sampled = fact.uuid == service || (index++ % stride == 0);
      if (!sampled) continue;
      const auto wrong_agent = Login(loop, passwords, fact.uuid, kWrongPassword);
      ExpectRefusal(wrong_agent, E::external_binding_refused, "agent principal, wrong password");
      Check(std::string(srv::NativeLoginMaterialErrorCode(E::external_binding_refused)) ==
                api::CheckExternalAuthenticationBinding("service").code,
            "the refusal code is the one CheckExternalAuthenticationBinding returns for a service principal (Q274)");
      if (fact.has_seed) {
        ++with_password;
        const std::string correct(reinterpret_cast<const char*>(fact.derived.data()), fact.derived.size());
        const auto right_agent = Login(loop, passwords, fact.uuid, correct);
        ExpectRefusal(right_agent, E::external_binding_refused, "agent principal, its CORRECT derived password");
        Check(right_agent.result.verifier_error == wrong_agent.result.verifier_error &&
                  right_agent.summary == wrong_agent.summary,
              "an agent's refusal is identical whether or not the password matched (no oracle)");
        if (fact.uuid == service) timings.push_back(right_agent);
      }
    }
    Check(agents >= 2 && with_password >= 1, "the create made the L5 principal and the other agent principals");
    Check(std::any_of(offline.principals.begin(), offline.principals.end(), [&](const auto& f) { return f.uuid == service && f.has_seed; }),
          "L5's row carries the password seed");

    // 6. Inputs that cannot be local passwords, decided before the file is read.
    ExpectRefusal(Login(loop, passwords, bootstrap, std::string_view("a\0b-password-xx", 15)), E::password_inadmissible, "a NUL in the password");
    ExpectRefusal(Login(loop, passwords, bootstrap, "", 16, 0), E::password_inadmissible, "an empty password");
    ExpectRefusal(Login(loop, passwords, bootstrap, std::string(1025, 'x')), E::password_inadmissible, "a 1025-byte password");
    ExpectRefusal(Login(loop, passwords, bootstrap, "short-but-fine-123", 32, 99), E::invalid_request, "a length beyond the protected buffer");
    ExpectRefusal(Login(loop, passwords, p::Uuid{}, kBootstrapPassword), E::invalid_request, "a nil principal UUID");
    // A password that is a prefix / extension of the real one never matches (the length is part of the secret).
    ExpectRefusal(Login(loop, passwords, bootstrap, kBootstrapPassword.substr(0, kBootstrapPassword.size() - 1)), E::password_mismatch, "a prefix of the password");
    ExpectRefusal(Login(loop, passwords, bootstrap, std::string(kBootstrapPassword) + "x"), E::password_mismatch, "the password plus one byte");
    // Spare capacity beyond the password length is not part of the password: the protected buffer is larger.
    Check(Login(loop, passwords, bootstrap, kBootstrapPassword, 256).result.ok(), "a larger protected buffer with the right length authenticates");

    // 7. Repeated reads: the same answer every time, no growth that fails (governed memory is returned per call).
    for (int i = 0; i < 6; ++i) {
      const auto again = Login(loop, passwords, bootstrap, i % 2 ? kBootstrapPassword : kWrongPassword);
      Check(i % 2 ? again.result.ok() : again.result.error == E::password_mismatch, "repeated read " + std::to_string(i) + ": " + again.summary);
    }
  }

  if (state == "serving" || state == "restricted") {
    // Concurrent callers: the requests are serialized on the loop thread (the runtime is thread-affine); every caller
    // gets its own correct answer. Three threads, one right and one wrong password each.
    std::vector<std::thread> callers;
    std::vector<std::string> failures(3);
    for (int t = 0; t < 3; ++t)
      callers.emplace_back([&, t] {
        for (const bool right : {true, false}) {
          srv::NativeLoopLoginRequest request;
          request.principal_uuid = bootstrap;
          const std::string_view pw = right ? kBootstrapPassword : kWrongPassword;
          request.password = passwords_mutex_make(passwords, pw);
          request.password_length = pw.size();
          const auto result = loop.SubmitReadLoginMaterial(std::move(request)).get();
          if (right != result.ok() || (!right && result.error != E::password_mismatch)) failures[t] = Summary(result);
        }
      });
    for (auto& c : callers) c.join();
    for (const auto& f : failures) Check(f.empty(), "a concurrent caller got a wrong answer: " + f);
  }

  // The refusal vectors and every summary are structured only: no password byte in any of them.
  for (const auto& s : all_summaries)
    Check(s.find(kBootstrapPassword) == std::string::npos && s.find(kWrongPassword) == std::string::npos &&
              s.find("verifier=") == std::string::npos,
          "no password or verifier in a result summary");

  // Timing (informational; the PBKDF2 runs on the database loop thread).
  double sum = 0, worst = 0;
  for (const auto& t : timings) { sum += t.milliseconds; worst = std::max(worst, t.milliseconds); }
  std::cout << "TIMING logins=" << timings.size() << " mean_ms=" << (timings.empty() ? 0 : sum / timings.size()) << " max_ms=" << worst << std::endl;

  Check(Snapshot(directory) == before, "the directory and every file's bytes are unchanged after every call" +
                                           std::string(state == "failed" ? " (the failed fixture's Restart wrote nothing: its first write failed)" : ""));
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");

  // After the loop is closed (a submitted request, on its own thread) a request is refused specifically and its
  // password is consumed.
  Check(loop.Close().ok, "the loop closes");
  ExpectRefusal(Login(loop, passwords, bootstrap, kBootstrapPassword), E::database_not_serving, "a login after the loop closed");
  (void)expectation;
  std::cout << "LOGIN_MATERIAL_OK checks=" << checks << " state=" << state << std::endl;
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


void OneSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const std::string forbidden_a(kBootstrapPassword), forbidden_b(kWrongPassword);
  const auto run_state = [&](const std::string& state) {
    const auto dir = root / (state + std::to_string(page_size));
    fs::create_directory(dir);
    const auto path = dir / "login.sdb";
    int status = 0;
    const auto created = Capture("'" + self + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
    if (state == "restricted") {
      Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' committing", &status).find("FIXTURE ok") != std::string::npos && status == 0,
            tag + "the native_serving writer left a committing slot");
    } else if (state == "disabled" || state == "nocredential") {
      const auto forged = Capture("'" + self + "' forge '" + path.string() + "' " + state + " " + Field(created, "bootstrap"), &status);
      Check(status == 0 && forged.find("FORGED kind=" + state) != std::string::npos, tag + state + ": the engine's writers changed the row: " + forged.substr(0, 1500));
    } else if (state == "failed") {
      Check(Capture(std::string("'") + SB_NATIVE_SERVING_TEST_BIN + "' fixture '" + path.string() + "' active", &status).find("FIXTURE ok") != std::string::npos && status == 0,
            tag + "the native_serving writer left an active slot (the Restart will write; the seam fails it)");
    }
    const auto start = std::chrono::steady_clock::now();
    const auto served = Capture("'" + self + "' serve '" + path.string() + "' " + state + " " + Field(created, "database") + " " +
                                    Field(created, "bootstrap") + " " + Field(created, "service") + " " + Field(created, "binding"),
                                &status);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(status == 0 && served.find("LOGIN_MATERIAL_OK") != std::string::npos, tag + state + ": serve child: " + served.substr(0, 2000));
    // No password byte, in any form, in anything a child printed (create included).
    for (const auto& output : {created, served})
      Check(output.find(forbidden_a) == std::string::npos && output.find(forbidden_b) == std::string::npos,
            tag + state + ": no test password in any child output");
    if (const auto info = served.find("INFO "); info != std::string::npos)
      std::printf("%s%s: %s\n", tag.c_str(), state.c_str(), served.substr(info, served.find('\n', info) - info).c_str());
    std::printf("%s%s: %.1f s; %s", tag.c_str(), state.c_str(), seconds, served.substr(served.find("TIMING")).c_str());
    fflush(stdout);
  };
  run_state("serving");
  run_state("restricted");
  run_state("failed");
  run_state("disabled");
  run_state("nocredential");
}
}  // namespace

// derive <path> <binding_hex>: (S4c, used by native_login_test) prints "DERIVED <uuid_hex>:<password_hex>..." for every
// principal that carries a password seed (the agents and L5): database UUID XOR the stored seed, read offline through the
// production observer BEFORE any server owns the file. The password is a test secret: it goes to the parent's capture
// (never argv, never a log) and the parent hands it to its served child through the environment.
int DeriveMode(const fs::path& path, const p::Uuid& binding) {
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
  const auto offline = ReadOffline(path, binding);
  std::string out = "DERIVED";
  for (const auto& fact : offline.principals) {
    if (!fact.has_seed) continue;
    std::string pw;
    static constexpr char kHex[] = "0123456789abcdef";
    for (const auto byte : fact.derived) {
      pw.push_back(kHex[byte >> 4]);
      pw.push_back(kHex[byte & 15]);
    }
    out += " " + Hex(fact.uuid) + ":" + pw;
  }
  std::cout << out << std::endl;
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "derive") return DeriveMode(argv[2], FromHex(argv[3]));
  if (argc == 4 && std::string(argv[1]) == "create") return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc == 5 && std::string(argv[1]) == "forge") return ForgeMode(argv[2], argv[3], FromHex(argv[4]));
  if (argc == 8 && std::string(argv[1]) == "serve")
    return ServeMode(argv[2], argv[3], FromHex(argv[4]), FromHex(argv[5]), FromHex(argv[6]), FromHex(argv[7]), "");
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (argc == 3) OneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])));
    else
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u}) OneSize(self, artifacts.path(), size);
    artifacts.Cleanup();
    std::printf("native_login_material: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
