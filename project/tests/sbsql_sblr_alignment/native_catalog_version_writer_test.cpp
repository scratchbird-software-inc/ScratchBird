// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_CATALOG_VERSION_WRITER
// In-place writes W3a (owner Q20 = 1, Q23 = 1): a transaction writes the
// successor version of a catalog row into a catalog leaf in place, on
// databases written by the production create, one page size per run
// (--profile N). Two databases: the production create with the default
// policy pack (indexed relations; the security relation is role 4) and the
// same create primitive with a two-row seed in an unindexed relation (the
// default pack's only unindexed relation is empty).
// The oracle: the relation as read here (catalog root, index walk, leaves),
// the base version and the placement each scenario expects (the
// predecessor's page when the successor fits, by the create's own row-size
// rule, else the lowest-numbered other leaf the head reaches with room), and
// the expected page images built by the production leaf codec from the
// expected rows; every other byte of the file unchanged. Visibility is read
// back through the production fixed-root lease and the catalog visibility
// reader (committed reads), in a fresh open.
//   - Visibility by creator state: in progress, committed, rolled back, dead
//     at restart; a later transaction's successor passes over a dead version
//     (a row_version gap) and becomes visible when committed.
//   - Placement: the predecessor's page (one write) or another leaf (two
//     writes, the successor before the forward link); relation_full when no
//     reached leaf has room, with no write.
//   - The same transaction twice; a drop and the refusal of its successor.
//   - Refusals before any write: malformed requests, a read-only database or
//     transaction, a transaction not in progress, name records, a missing
//     row, write and update conflicts, invalid successors.
//   - Process loss after every write and every write torn at its half-way
//     point over BEGIN, a two-write successor and COMMIT: exactly the write
//     prefix; a torn leaf fails its seal and the relation is refused (owner
//     Q23 = 1); otherwise a new process restarts and writes the next
//     successor.
//   - Every read, write and sync failed in turn over the same script: a
//     failed read writes nothing and the call succeeds again; a failed write
//     or sync leaves exactly the prefix of the writes performed.
// Limit: process death and I/O errors only, not power loss or reordering.
#include "native_database_create.hpp"
#include "native_creation_workspace.hpp"
#include "native_creation_catalog_input.hpp"
#include "native_catalog_version_writer.hpp"
#include "native_construction_audit.hpp"
#include "catalog_construction_intent.hpp"
#include "native_catalog_leaf_page.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_index_btree_page.hpp"
#include "catalog_page.hpp"
#include "catalog_crypto_inline_private_key_policy.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "transaction_inventory_writer.hpp"
#include "transaction_header_page.hpp"
#include "careful_write.hpp"
#include "uuid.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "../support/careful_write_crash_sink.hpp"
#include "../support/owned_temp_directory.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
namespace m = scratchbird::core::memory;
namespace c = scratchbird::core::catalog;
namespace mga = scratchbird::transaction::mga;
namespace u = scratchbird::core::uuid;
namespace f = scratchbird::tests::bootstrap_policy;
namespace fs = std::filesystem;
using namespace scratchbird::core::platform;
using CE = db::CatalogVersionWriteError;
using WE = pg::TransactionWriterError;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
constexpr int kPlannedCrash = 86, kChildCompleted = 70, kChildFailed = 71, kChildException = 73;
constexpr u32 kNoCrash = ~u32{0};

Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
u64 kT = 0;  // ms, one second after the creator's commit
Uuid V7(u64 ms, unsigned salt) {
  Uuid id;
  for (int i = 0; i < 6; ++i) id.bytes[i] = static_cast<byte>(ms >> (8 * (5 - i)));
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[9] = 0x7d;
  id.bytes[13] = static_cast<byte>(salt >> 16);
  id.bytes[14] = static_cast<byte>(salt >> 8);
  id.bytes[15] = static_cast<byte>(salt);
  return id;
}
Uuid TxId(u64 n) { return V7(kT + n, static_cast<unsigned>(n)); }
Uuid VersionId(unsigned k) { return V7(kT + 600000 + k, 0x500000 + k); }
u64 CommitTime(u64 n) { return (kT + 5'000'000 + n) * 1'000'000; }

constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

std::vector<byte> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void WriteAll(const std::string& path, std::span<const byte> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(out.good(), "write a working copy");
}

// A node-issued memory grant (the fixture of native_fixed_root_reader_limits).
struct Grant {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding;
  db::NativeStorageMemory memory;
  static auto Policy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = 1ull << 30;
    p.per_context_limit_bytes = 1ull << 30;
    return p;
  }
  Grant(const Uuid& database, u64 bytes) : manager(Policy()), binding{database, Fixed(61), Fixed(62), Fixed(63)} {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = m::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "storage.catalog_version_writer.test";
    r.purpose = "catalog version writer test";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Fixed(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "catalog version writer test";
    for (const auto& scope : r.scope_chain) {
      m::HierarchicalMemoryBudget b;
      b.scope = scope;
      b.hard_limit_bytes = bytes;
      b.provenance = r.provenance;
      Check(ledger.SetBudget(b).ok(), "parent budget");
    }
    auto grant = m::AcquireReservationBackedMemoryResource(r);
    Check(grant.ok(), "node-issued grant");
    auto adopted = db::AdoptNativeStorageMemory(binding, grant.resource);
    Check(adopted.ok(), "grant adoption");
    memory = std::move(adopted.memory);
  }
};

// ---- The two databases ---------------------------------------------------------
struct Db {
  scratchbird::tests::OwnedTempDirectory dir;
  std::string name;
  u32 size = 0;
  Uuid database, primary, profile;
  d::FilespaceRootReference catalog_root, state_root, map_root, configuration_root, security_root;
  db::NativeCreationWorkspaceReceipt receipt;
  std::vector<byte> bytes;
  mutable u16 role = 0;  // the relation the scenarios change (RoleScope switches it)

  void Roots(const std::string& path) {
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "read-only open");
    const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok(), "page zero");
    for (const auto& root : zero.record->roots) {
      if (root.kind == 2) catalog_root = root;
      if (root.kind == 3) map_root = root;
      if (root.kind == 4) state_root = root;
      if (root.kind == 6) configuration_root = root;
      if (root.kind == 7) security_root = root;
    }
    Check(catalog_root.kind == 2 && state_root.kind == 4 && map_root.kind == 3 && configuration_root.kind == 6 &&
              security_root.kind == 7,
          "page zero names the roots");
    Check(device.Close().ok(), "close");
    bytes = ReadAll(path);
    // Later transactions begin one second after the creator's commit.
    const auto headers = pg::DecodeTransactionHeaderPage(std::span<const byte>(bytes).subspan(
        pg::DecodeTransactionStatePage(std::span<const byte>(bytes).subspan(state_root.page_number * size, size))
                .view->header_chain_page *
            size,
        size));
    kT = std::max(kT, pg::TransactionHeaderAt(*headers.view, 1)->commit_time_utc_ns / 1'000'000 + 1000);
  }
  std::string Copy(std::span<const byte> from, const std::string& file) const {
    const auto p = (dir.path() / file).string();
    WriteAll(p, from);
    return p;
  }
  pg::TransactionWriterBinding TxBinding() const {
    pg::TransactionWriterBinding b;
    b.database_uuid = database;
    b.state_root = state_root;
    return b;
  }
  db::CatalogVersionWriterBinding Binding() const {
    db::CatalogVersionWriterBinding b;
    b.database_uuid = database;
    b.catalog_root = catalog_root;
    b.state_root = state_root;
    b.configuration_root = configuration_root;
    b.security_root = security_root;
    return b;
  }
  db::NativeCatalogRelationBinding Relation() const {
    return {receipt.relation_uuids[role - 1], receipt.index_dependencies[role - 1]};
  }
};

// Switches the relation a database's scenarios change, for one scope.
struct RoleScope {
  const Db& b;
  u16 saved;
  RoleScope(const Db& db, u16 role) : b(db), saved(db.role) { b.role = role; }
  ~RoleScope() { b.role = saved; }
};

// `total_pages`: 0 for the fixture's size (1,024 pages at 8 KiB, else 512).
std::unique_ptr<Db> ProductionDb(unsigned index, u64 total_pages = 0) {
  auto b = std::make_unique<Db>();
  b->name = "production";
  const auto& p = d::kCanonicalFilespacePageProfiles[index];
  b->size = p.page_size_bytes;
  b->profile = p.uuid;
  b->role = 4;
  db::NativeDatabaseCreateRequest r;
  r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  r.database_name = "orders";
  r.database_default_language = scratchbird::core::catalog::kCatalogLanguage_en;/* S2: the language constant, not the tag "en" */
  r.page_size_profile_uuid = p.uuid;
  r.page_size_bytes = p.page_size_bytes;
  r.total_pages = total_pages ? total_pages : (p.page_size_bytes == 8192 ? 1024 : 512);
  r.creation_utc_millis = 1789357072000ULL;
  r.engine_uuid = Fixed(4);
  r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
  r.bootstrap_principal_name = "BootstrapAdmin";
  r.bootstrap_credential_fingerprint = kFingerprint;
  r.service_principal_name = "NodeService";
  r.service_credential_fingerprint = kFingerprint;
  r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
  const auto path = (b->dir.path() / "created").string();
  {
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "node device");
    u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
    auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
    Check(made.ok(), "the production create writes a complete native database");
    b->database = made.receipt->database_uuid;
    b->primary = made.receipt->filespace_uuid;
    b->receipt = made.receipt->workspace;
    Check(device.Close().ok(), "close");
  }
  b->Roots(path);
  return b;
}

// The crypto-inline policy body row of native_crypto_inline_private_key_policy_test,
// alone in the unindexed role 4.
const TypedUuid kSeedCreator{UuidKind::transaction, f::Id(5)};
c::CatalogMetadataVersion SeedMetadata(const Uuid& object, unsigned row, scratchbird::core::catalog::CatalogObjectSubtype subtype, std::string payload) {
  auto r = f::Metadata(0, kSeedCreator, f::Id(60), std::move(payload));
  r.record.header.kind = c::CatalogRecordKind::policy;
  r.record.header.object_uuid = {UuidKind::object, object};
  r.record.header.row_uuid = {UuidKind::row, f::Id(row)};
  r.object_subtype = subtype;
  r.default_name_uuid = {UuidKind::object, f::Id(row + 1000)};
  r.name_vector_uuid = {UuidKind::object, f::Id(row + 2000)};
  return r;
}
std::string SeedBody() {
  f::Golden g(65620);
  g.Identity(1, f::Id(100));
  g.Integer(2, 1);
  g.Identity(3, f::Id(1));
  g.Integer(4, static_cast<u64>(c::CatalogCryptoInlinePrivateKeyPolicy{}.purpose));
  g.Boolean(5, false);
  g.Identity(6, kSeedCreator.value);
  g.Integer(7, 1);
  return g.Finish();
}
std::unique_ptr<Db> SeededDb(unsigned index) {
  auto b = std::make_unique<Db>();
  b->name = "seeded";
  const auto& p = d::kCanonicalFilespacePageProfiles[index];
  b->size = p.page_size_bytes;
  b->profile = p.uuid;
  b->role = 4;
  db::NativeCreationCatalogSeed seed;
  seed[3].push_back({SeedMetadata(f::Id(100), 101, scratchbird::core::catalog::CatalogObjectSubtype::crypto_inline_private_key, SeedBody()), {}});
  db::NativeFilespaceInitializationRequest request;
  u::StandaloneUuidV7Issuer issuer({f::Id(1), f::Id(10)}, {{}, 0, 1000});
  request.bootstrap = {f::Id(1), f::Id(2), p.uuid, d::kNativeBootstrapIntegrityProfile, {}, p.page_size_bytes, 1, 0, 1, 7};
  request.operation_uuid = f::Id(3);
  request.writer_uuid = f::Id(4);
  request.policy_snapshot_uuid = f::Id(10);
  request.creator.transaction_uuid = kSeedCreator;
  request.creator.local_id = mga::MakeLocalTransactionId(1);
  request.creator.scope = mga::TransactionScope::local_node;
  request.total_pages = 64;
  request.creation_utc_millis = 1789357072000ULL;
  request.genesis_graph = db::NativeCreationGenesisGraph::fixed_roots;
  // W3b-2b: the create's construction intent (role 6), as the production
  // create writes it (database_lifecycle), so a later transaction can write
  // its catalog.metadata_change event (WriteCatalogChangeEvent takes its row's
  // owner, schema and parent) and the policy row cites it.
  {
    const auto audit = db::AcquireNativeConstructionIntent(request, f::Id(8), issuer);
    Check(audit.ok(), "the seeded create's construction intent");
    std::array<byte, c::kCatalogConstructionIntentMaxBytes> bytes{};
    const auto encoded = c::EncodeCatalogConstructionIntentInto(*audit.event, bytes);
    Check(encoded.ok(), "the construction intent encodes");
    auto m = SeedMetadata(audit.event->event_uuid, 0, scratchbird::core::catalog::CatalogObjectSubtype::node_construction_intent,
                          std::string(reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size()));
    m.record.header.kind = c::CatalogRecordKind::audit_evidence;
    m.record.header.row_uuid = audit.row_uuid;
    m.record.header.parent_uuid = {UuidKind::object, m.owning_schema_uuid.value};
    m.default_name_uuid = {};
    m.name_vector_uuid = {};
    m.audit_uuid = m.record.header.object_uuid;
    seed[3].front().metadata.audit_uuid = m.audit_uuid;
    seed[5].push_back({std::move(m), {}});
  }
  const auto path = (b->dir.path() / "seeded").string();
  {
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "seeded device");
    const auto made =
        db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device, request, seed, 128 * u64{p.page_size_bytes}, issuer);
    Check(made.ok(), "the create primitive writes the seeded database");
    b->receipt = *made.receipt;
    Check(device.Close().ok(), "close");
  }
  b->database = f::Id(1);
  b->primary = f::Id(2);
  b->Roots(path);
  return b;
}

// ---- The relation as read here (independent of the writer) ----------------------
struct LeafSeen {
  u64 page = 0;
  db::NativeCatalogLeafPage leaf;
  std::map<Uuid, c::CatalogMetadataVersion> metadata;
};
struct Relation {
  bool indexed = false;
  std::vector<LeafSeen> leaves;  // ascending page number
  const LeafSeen& At(u64 page) const {
    for (const auto& l : leaves)
      if (l.page == page) return l;
    throw std::runtime_error("no such leaf");
  }
  // Every version of `row`: (page, row index), newest first.
  std::vector<std::pair<u64, std::size_t>> Versions(const Uuid& row) const {
    std::vector<std::pair<u64, std::size_t>> out;
    for (const auto& l : leaves)
      for (std::size_t i = 0; i < l.leaf.body.rows.size(); ++i)
        if (l.leaf.body.rows[i].row_uuid.value == row) out.emplace_back(l.page, i);
    std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
      return At(a.first).leaf.body.rows[a.second].row_version > At(b.first).leaf.body.rows[b.second].row_version;
    });
    return out;
  }
  const pg::RowDataRecord& Row(const std::pair<u64, std::size_t>& at) const {
    return At(at.first).leaf.body.rows[at.second];
  }
  std::pair<u64, std::size_t> Find(const Uuid& version) const {
    for (const auto& l : leaves)
      for (std::size_t i = 0; i < l.leaf.body.rows.size(); ++i)
        if (l.leaf.body.rows[i].version_uuid == version) return {l.page, i};
    throw std::runtime_error("no such version");
  }
};
std::vector<byte> PageOf(std::span<const byte> file, u64 number, u32 size) {
  return {file.begin() + static_cast<std::ptrdiff_t>(number * size),
          file.begin() + static_cast<std::ptrdiff_t>((number + 1) * size)};
}
Relation ReadRelation(const Db& b, std::span<const byte> file) {
  const auto root = pg::DecodeNativeCatalogRoot(PageOf(file, b.catalog_root.page_number, b.size));
  Check(root.ok(), "the catalog root decodes");
  const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                 [&](const auto& r) { return r.role == b.role; });
  Check(head != root.root->roots.end(), "the relation head");
  Relation out;
  out.indexed = head->page_type == 0x200;
  std::set<u64> bases;
  if (!out.indexed) {
    bases.insert(head->page.page_number);
  } else {
    std::vector<u64> stack{head->page.page_number};
    std::set<u64> seen;
    while (!stack.empty()) {
      const auto n = stack.back();
      stack.pop_back();
      Check(seen.insert(n).second, "the index is a tree");
      const auto page = pg::DecodeNativeBtreePage(PageOf(file, n, b.size));
      Check(page.ok(), "an index page decodes");
      if (page.page->first_child) stack.push_back(page.page->first_child->page_number);
      for (const auto& cell : page.page->cells) {
        if (cell.child) stack.push_back(cell.child->page_number);
        if (cell.base_page) bases.insert(cell.base_page->page_number);
      }
    }
  }
  for (const auto n : bases) {
    auto leaf = db::DecodeNativeCatalogLeaf(PageOf(file, n, b.size));
    Check(leaf.ok(), "a base leaf decodes");
    out.leaves.push_back({n, std::move(*leaf.page), std::move(leaf.metadata)});
  }
  return out;
}
// The create's own row-size rule: the encoded row and its slot.
u64 RowBytes(const c::CatalogMetadataVersion& metadata) {
  const auto bytes = db::NativeCreationCatalogRowBytes({metadata, {}});
  if (!bytes) {
    const auto invalid = c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(metadata));
    std::cerr << "row size: " << (invalid ? invalid->detail : "valid") << '\n';
  }
  Check(bytes.has_value(), "row size");
  return *bytes;
}

// ---- Transactions (the W1 writer) ------------------------------------------------
struct Session {
  const Db* db = nullptr;
  d::FileDevice device;
  std::optional<d::FileDeviceCarefulWriteSink> file;
  std::optional<pg::TransactionInventoryWriter> w;
  Session(const Db& b, const std::string& path, bool restart = true) : db(&b) {
    Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open for writing");
    file.emplace(device);
    w.emplace(b.TxBinding());
    if (restart) Check(w->Restart(*file).ok(), "restart");
  }
  ~Session() {
    file.reset();
    (void)device.Close();
  }
  d::CarefulWriteSink& sink() { return *file; }
};
u64 Begin(pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, u64 n, bool read_only = false) {
  pg::TransactionHeaderRecord h;
  h.transaction_uuid = TxId(n);
  h.read_only = read_only;
  const auto r = w.Begin(sink, h);
  Check(r.ok() && r.transaction_order == n, "BEGIN takes the expected number");
  return n;
}
// SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b): transaction n's catalog.metadata_change
// event, which every definition version it writes cites. Written right after
// its BEGIN (Begin(Session&, n)), before anything a check compares.
Uuid EventId(u64 n) { return V7(kT + 2400000 + n, 0x7b0000 + static_cast<unsigned>(n)); }
scratchbird::core::platform::TypedUuid EventRef(u64 n) { return {UuidKind::object, EventId(n)}; }
db::CatalogVersionWriteResult WriteEvent(const Db& b, d::CarefulWriteSink& sink, u64 n, const Uuid& transaction) {
  db::CatalogChangeEventRequest r;
  r.transaction_order = n;
  r.transaction_uuid = transaction;
  r.event_uuid = EventId(n);
  r.row_uuid = V7(kT + 2400000 + n, 0x7c0000 + static_cast<unsigned>(n));
  r.version_uuid = V7(kT + 2400000 + n, 0x7d0000 + static_cast<unsigned>(n));
  r.engine_uuid = V7(kT + 2500000, 0x7e0000);
  r.event_time_utc_us = CommitTime(n) - 500'000;  // a fixed fixture time, before the commit
  // A new base leaf, and a split up to the root of role 6's tree, should it be full.
  for (unsigned i = 0; i < 6; ++i)
    r.new_pages.push_back({V7(kT + 2600000 + 10 * n + i, 0x7f0000 + static_cast<unsigned>(n)),
                           V7(kT + 2600000 + 10 * n + i, 0x7f8000 + static_cast<unsigned>(n))});
  auto binding = b.Binding();
  binding.allocation_root = b.map_root;
  return db::WriteCatalogChangeEvent(sink, binding, r);
}
void Audit(Session& s, u64 n, const std::optional<Uuid>& transaction = std::nullopt) {
  const auto result = WriteEvent(*s.db, s.sink(), n, transaction ? *transaction : TxId(n));
  if (!result.ok()) std::cerr << "event of " << n << ": " << db::CatalogVersionWriteErrorName(result.error) << '\n';
  Check(result.ok(), "the transaction's catalog.metadata_change event is written");
}
u64 Begin(Session& s, u64 n, bool read_only = false) {
  Begin(*s.w, s.sink(), n, read_only);
  if (!read_only) Audit(s, n);
  return n;
}
void Commit(pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, u64 n) {
  Check(w.Commit(sink, n, CommitTime(n)).ok(), "COMMIT");
}
void Rollback(pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, u64 n) {
  Check(w.Rollback(sink, n, CommitTime(n)).ok(), "ROLLBACK");
}

// ---- Requests and the oracle ----------------------------------------------------
db::CatalogVersionRequest Successor(const Db& b, const Relation& rel, const Uuid& row, const Uuid& predecessor,
                                    u64 n, const Uuid& version, bool drop = false) {
  const auto at = rel.Find(predecessor);
  db::CatalogVersionRequest r;
  r.role = b.role;
  r.transaction_order = n;
  r.transaction_uuid = TxId(n);
  r.row_uuid = row;
  r.predecessor_version_uuid = predecessor;
  r.version_uuid = version;
  r.metadata = rel.At(at.first).metadata.at(predecessor);
  r.metadata.definition_version += 1;
  r.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(n)};
  r.metadata.creator_local_transaction_id = n;
  r.metadata.audit_uuid = EventRef(n);  // W3b-2b: the transaction's event
  if (c::IsCatalogCryptoInlinePrivateKeyPolicyFamily(c::BorrowCatalogMetadataVersion(r.metadata))) {
    // That family binds its body's generation to definition_version.
    auto body = c::DecodeCatalogCryptoInlinePrivateKeyPolicy(r.metadata.record.payload);
    Check(body.ok(), "the policy body decodes");
    body.record->generation = r.metadata.definition_version;
    std::array<byte, c::kCatalogCryptoInlinePrivateKeyPolicyBytes> buffer;
    const auto encoded = c::EncodeCatalogCryptoInlinePrivateKeyPolicyInto(*body.record, buffer);
    Check(encoded.ok(), "the next policy body encodes");
    r.metadata.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size());
  }
  if (drop) {
    r.metadata.record.header.deleted = true;
    r.metadata.lifecycle = c::CatalogObjectLifecycle::dropped;
    r.metadata.status = c::CatalogObjectStatus::retired;
    r.metadata.retired_transaction_uuid = {UuidKind::transaction, TxId(n)};
  }
  return r;
}
struct Expected {
  u64 target = 0, link = 0;
  std::vector<byte> file;
};
// The file after the request, given the base the scenario expects: the
// successor on the predecessor's page when it fits there, else on the
// lowest-numbered other leaf with room; the predecessor's forward link.
std::optional<Expected> Oracle(const Db& b, std::span<const byte> before, const db::CatalogVersionRequest& r) {
  const auto rel = ReadRelation(b, before);
  const auto versions = rel.Versions(r.row_uuid);
  Check(!versions.empty(), "the row exists");
  const u64 newest = rel.Row(versions.front()).row_version;
  const auto base = rel.Find(r.predecessor_version_uuid);
  const auto need = RowBytes(r.metadata);
  std::optional<u64> target;
  if (rel.At(base.first).leaf.body.free_space_bytes >= need) target = base.first;
  for (const auto& l : rel.leaves)
    if (!target && l.page != base.first && l.leaf.body.free_space_bytes >= need) target = l.page;
  if (!target) {
    u64 most = 0;
    for (const auto& l : rel.leaves) most = std::max<u64>(most, l.leaf.body.free_space_bytes);
    std::cerr << "oracle: no room for " << need << " bytes; the roomiest leaf has " << most << '\n';
    return std::nullopt;
  }
  auto link = rel.At(base.first).leaf;
  link.body.rows[base.second].next_version_uuid = r.version_uuid;
  link.body.rows[base.second].next_row_version = newest + 1;
  auto into = *target == base.first ? link : rel.At(*target).leaf;
  pg::RowDataRecord row;
  row.row_uuid = {UuidKind::row, r.row_uuid};
  row.version_uuid = r.version_uuid;
  row.previous_version_uuid = r.predecessor_version_uuid;
  row.transaction_uuid = {UuidKind::transaction, r.transaction_uuid};
  row.local_transaction_id = r.transaction_order;
  row.row_version = newest + 1;
  row.previous_row_version = rel.Row(base).row_version;
  row.storage_generation = into.header.page_generation;
  u32 slot = 0;
  for (const auto& x : into.body.rows) slot = std::max(slot, x.stable_slot_id);
  row.stable_slot_id = slot + 1;
  const auto encoded = c::EncodeCatalogMetadataVersion(r.metadata);
  Check(encoded.ok(), "the successor metadata encodes");
  scratchbird::core::datatypes::DatatypeBinaryValue cell;
  cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
  cell.payload = encoded.bytes;
  row.cells = {{1, std::move(cell), {}}};
  into.body.rows.push_back(std::move(row));
  Expected out;
  out.target = *target;
  out.link = base.first;
  out.file.assign(before.begin(), before.end());
  const auto put = [&](const db::NativeCatalogLeafPage& page) {
    const auto image = db::EncodeNativeCatalogLeaf(page);
    Check(image.ok(), "the expected leaf encodes");
    std::copy(image.bytes.begin(), image.bytes.end(), out.file.begin() + static_cast<std::ptrdiff_t>(page.header.page_number * b.size));
  };
  put(into);
  if (*target != base.first) put(link);
  return out;
}

db::CatalogVersionWriteResult Write(const Db& b, d::CarefulWriteSink& sink, const db::CatalogVersionRequest& r,
                                     bool read_only = false) {
  auto binding = b.Binding();
  binding.read_only_database = read_only;
  return db::WriteCatalogSuccessorVersion(sink, binding, r);
}
// One successor through the writer, checked against the oracle.
db::CatalogVersionWriteResult Expect(const Db& b, Session& s, const std::string& path, const db::CatalogVersionRequest& r,
                                     std::string_view label) {
  const auto before = ReadAll(path);
  const auto expected = Oracle(b, before, r);
  Check(expected.has_value(), std::string(label) + ": the oracle finds room");
  const auto result = Write(b, s.sink(), r);
  if (!result.ok()) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(result.error) << '\n';
  Check(result.ok(), std::string(label) + ": the successor is written");
  Check(result.page_number == expected->target && result.forward_link_page == expected->link,
        std::string(label) + ": placed where the oracle places it");
  Check(result.writes == (expected->target == expected->link ? 1u : 2u), std::string(label) + ": one write per page");
  Check(ReadAll(path) == expected->file, std::string(label) + ": the file is the oracle's");
  return result;
}
// A refused write changes nothing.
void Refused(const Db& b, Session& s, const std::string& path, const db::CatalogVersionRequest& r, CE error,
             std::string_view label, bool read_only = false) {
  const auto before = ReadAll(path);
  const auto result = Write(b, s.sink(), r, read_only);
  if (result.error != error)
    std::cerr << label << ": got " << db::CatalogVersionWriteErrorName(result.error) << '\n';
  Check(result.error == error && result.writes == 0 && result.syncs == 0 && !result.attempted,
        std::string(label) + ": refused as expected with no write");
  Check(ReadAll(path) == before, std::string(label) + ": the file is unchanged");
}

// ---- Reading back -----------------------------------------------------------------
// The committed read's visible version of `row`, through the production
// fixed-root lease and visibility reader in a fresh read-only open; nullopt
// when the relation read is refused.
struct Visible {
  bool read = false;
  std::optional<Uuid> version;
  bool deleted = false;
};
Visible Look(const Db& b, const std::string& path, const Uuid& row) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
  Visible out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    Grant g(b.database, 512u << 20);
    auto fixed = db::AcquireNativeFixedRootMemoryLease(b.database, files, b.primary, {512 * u64{b.size}, 64u << 20},
                                                       g.memory, g.binding);
    Check(fixed.ok(), "the fixed-root lease admits the file");
    const auto pages = [&](std::size_t bytes) { return bytes / b.size; };
    const db::NativeCatalogRelationMemoryLimits limits{b.profile, 1, pages(16u << 20), 4096, 4096,
                                                       {pages(4u << 20), 4u << 20}};
    auto prepared = db::PrepareNativeCatalogVisibilityRead(limits, g.memory, g.binding);
    Check(prepared.ok(), "visibility workspace");
    const auto read = db::NativeCatalogVisibilityLeaseReader::ReadCommitted(*fixed.lease, 2, b.role, b.Relation(),
                                                                            1024 * u64{b.size}, prepared.workspace);
    out.read = read.ok();
    if (read.ok())
      for (const auto& v : read.rows)
        if (v.metadata.record.header.row_uuid.value == row) {
          out.version = v.version_uuid;
          out.deleted = v.metadata.record.header.deleted;
        }
  }
  Check(device.Close().ok(), "close the reader");
  return out;
}
void Sees(const Db& b, const std::string& path, const Uuid& row, const Uuid& version, std::string_view label) {
  const auto seen = Look(b, path, row);
  Check(seen.read && seen.version == version, std::string(label) + ": the committed read sees the expected version");
}

// ---- Choosing rows ----------------------------------------------------------------
struct Rows {
  Uuid other_row, other_version;  // its successor does not fit its page
  Uuid same_row, same_version;    // its successor fits its page
  bool has_other = false, has_same = false;
};
Rows Choose(const Db& b, std::span<const byte> file) {
  // The smallest row of each kind: the relation's room is a few rows at
  // 8 KiB (measured), and every scenario's versions must fit in it.
  const auto rel = ReadRelation(b, file);
  Rows out;
  u64 other_bytes = ~u64{0}, same_bytes = ~u64{0};
  for (const auto& l : rel.leaves)
    for (const auto& row : l.leaf.body.rows) {
      const auto need = RowBytes(l.metadata.at(row.version_uuid));
      const bool fits = l.leaf.body.free_space_bytes >= need;
      if (!fits && need < other_bytes) {
        other_bytes = need;
        out.other_row = row.row_uuid.value;
        out.other_version = row.version_uuid;
        out.has_other = true;
      }
      if (fits && need < same_bytes) {
        same_bytes = need;
        out.same_row = row.row_uuid.value;
        out.same_version = row.version_uuid;
        out.has_same = true;
      }
    }
  return out;
}
// The committed read of a copy of the file as it stands (the writing
// session keeps its device open).
Visible Peek(const Db& b, const std::string& path, const Uuid& row) {
  const auto copy = b.Copy(ReadAll(path), "peek.db");
  const auto seen = Look(b, copy, row);
  fs::remove(copy);
  return seen;
}
void PeekSees(const Db& b, const std::string& path, const Uuid& row, const Uuid& version, std::string_view label) {
  const auto seen = Peek(b, path, row);
  Check(seen.read && seen.version == version, std::string(label) + ": the committed read sees the expected version");
}
db::CatalogVersionRequest Next(const Db& b, const std::string& path, const Uuid& row, const Uuid& predecessor, u64 n,
                               const Uuid& version, bool drop = false) {
  return Successor(b, ReadRelation(b, ReadAll(path)), row, predecessor, n, version, drop);
}
std::pair<Uuid, Uuid> Pick(const Db& b) {
  const auto rows = Choose(b, b.bytes);
  if (rows.has_other) return {rows.other_row, rows.other_version};
  Check(rows.has_same, "a row to change");
  return {rows.same_row, rows.same_version};
}

// ---- Visibility by creator state, conflicts, drop ---------------------------------
// Each case on a fresh copy and at most two new versions: at 8 KiB the
// security relation has room for three versions of its smallest row
// (measured: 3,796 free bytes on its one leaf with room, 961 per version).
void States(const Db& b) {
  const auto [row, v0] = Pick(b);
  const auto path = b.Copy(b.bytes, "states.db");
  {
    Session s(b, path);
    Begin(s, 2);
    const auto first = Expect(b, s, path, Next(b, path, row, v0, 2, VersionId(1)), "in progress");
    Check(first.row_version == ReadRelation(b, b.bytes).Row(ReadRelation(b, b.bytes).Find(v0)).row_version + 1,
          "the successor's row_version follows the newest");
    PeekSees(b, path, row, v0, "in progress");
    Begin(s, 3);
    auto blocked = Next(b, path, row, v0, 3, VersionId(2));
    {
      const auto before = ReadAll(path);
      const auto result = Write(b, s.sink(), blocked);
      Check(result.error == CE::write_conflict && result.blocker_order == 2 && !result.writes && ReadAll(path) == before,
            "a second writer meets the first: write_conflict naming it, no write");
    }
    Commit(*s.w, s.sink(), 2);
    PeekSees(b, path, row, VersionId(1), "committed");
    {
      const auto before = ReadAll(path);
      const auto result = Write(b, s.sink(), blocked);
      Check(result.error == CE::update_conflict && result.base_version_uuid == VersionId(1) && !result.writes &&
                ReadAll(path) == before,
            "the snapshot's version is no longer the newest committed: update_conflict, no write");
    }
    Expect(b, s, path, Next(b, path, row, VersionId(1), 3, VersionId(2)), "after the conflict, on the new base");
    Rollback(*s.w, s.sink(), 3);
    PeekSees(b, path, row, VersionId(1), "rolled back");
  }
  fs::remove(path);
  // A rolled-back version is passed over: a row_version gap.
  const auto gap_path = b.Copy(b.bytes, "gap.db");
  {
    Session s(b, gap_path);
    Begin(s, 2);
    Expect(b, s, gap_path, Next(b, gap_path, row, v0, 2, VersionId(1)), "to be rolled back");
    Rollback(*s.w, s.sink(), 2);
    Begin(s, 3);
    const auto gap = Expect(b, s, gap_path, Next(b, gap_path, row, v0, 3, VersionId(2)), "past a rolled-back version");
    const auto rel = ReadRelation(b, ReadAll(gap_path));
    const auto& written = rel.Row(rel.Find(VersionId(2)));
    Check(gap.row_version == written.row_version && written.previous_version_uuid == v0 &&
              written.previous_row_version + 2 == written.row_version,
          "the successor of a version passed over a rolled-back one: a row_version gap");
    Commit(*s.w, s.sink(), 3);
    PeekSees(b, gap_path, row, VersionId(2), "the version after the gap");
  }
  fs::remove(gap_path);
  // The same transaction twice: the second version chains on its own first.
  const auto twice = b.Copy(b.bytes, "twice.db");
  {
    Session s(b, twice);
    Begin(s, 2);
    Expect(b, s, twice, Next(b, twice, row, v0, 2, VersionId(1)), "the first change");
    Expect(b, s, twice, Next(b, twice, row, VersionId(1), 2, VersionId(2)), "the same transaction again");
    PeekSees(b, twice, row, v0, "two versions in progress");
    Commit(*s.w, s.sink(), 2);
    PeekSees(b, twice, row, VersionId(2), "the transaction's last version");
  }
  fs::remove(twice);
  // The predecessor's own page when it has room, even when another leaf the
  // head reaches has room too: one write. The security relation has no row
  // whose leaf and a lower-numbered leaf both have room; the configuration
  // relation (role 5) has some at 8 to 64 KiB (measured), so it is searched
  // too, for a row whose successor the family rules admit.
  {
    std::optional<std::pair<Uuid, Uuid>> contested;
    u16 role = b.role;
    for (const u16 candidate : {b.role, u16{5}}) {
      if (contested || (candidate != b.role && b.name != "production")) continue;
      RoleScope scope(b, candidate);
      const auto rel = ReadRelation(b, b.bytes);
      for (std::size_t i = 0; i < rel.leaves.size() && !contested; ++i)
        for (const auto& x : rel.leaves[i].leaf.body.rows) {
          const auto& before = rel.leaves[i].metadata.at(x.version_uuid);
          if (before.record.header.kind == c::CatalogRecordKind::localized_name || before.record.header.deleted) continue;
          const auto next = Successor(b, rel, x.row_uuid.value, x.version_uuid, 2, VersionId(1)).metadata;
          const auto need = db::NativeCreationCatalogRowBytes({next, {}});
          if (!need || c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(next)) ||
              !c::CatalogMetadataPreservesFamilyOrigin(before, next) || rel.leaves[i].leaf.body.free_space_bytes < *need)
            continue;
          for (std::size_t j = 0; j < i && !contested; ++j)
            if (rel.leaves[j].leaf.body.free_space_bytes >= *need) contested = {{x.row_uuid.value, x.version_uuid}};
          if (contested) {
            role = candidate;
            break;
          }
        }
    }
    const auto rows = Choose(b, b.bytes);
    Check(rows.has_same, "a row whose leaf has room");
    RoleScope scope(b, role);
    const auto [same_row, same_version] = contested ? *contested : std::pair{rows.same_row, rows.same_version};
    const auto own = b.Copy(b.bytes, "own.db");
    {
      Session s(b, own);
      Begin(s, 2);
      const auto rel = ReadRelation(b, b.bytes);
      const auto home = rel.Find(same_version).first;
      const auto r = Next(b, own, same_row, same_version, 2, VersionId(1));
      unsigned roomy = 0;
      for (const auto& l : rel.leaves)
        if (l.leaf.body.free_space_bytes >= RowBytes(r.metadata)) ++roomy;
      const auto written = Expect(b, s, own, r, "on its own page");
      Check(written.writes == 1 && written.page_number == home, "the successor shares its predecessor's page");
      std::cout << b.name << " own page page_size=" << b.size << " role=" << role << " leaves_with_room=" << roomy
                << " lower_leaf_with_room=" << bool(contested) << '\n';
      Commit(*s.w, s.sink(), 2);
    }
    Sees(b, own, same_row, VersionId(1), "the successor on its own page");
    fs::remove(own);
  }
  // A drop, and its successor refused.
  const auto drop = b.Copy(b.bytes, "drop.db");
  {
    Session s(b, drop);
    Begin(s, 2);
    Expect(b, s, drop, Next(b, drop, row, v0, 2, VersionId(1), true), "a drop");
    Refused(b, s, drop, Next(b, drop, row, VersionId(1), 2, VersionId(2)), CE::row_deleted,
            "the dropping transaction changes the dropped row");
    Commit(*s.w, s.sink(), 2);
    const auto dropped = Peek(b, drop, row);
    Check(dropped.read && dropped.version == VersionId(1) && dropped.deleted, "the committed drop is the visible version");
    Begin(s, 3);
    Refused(b, s, drop, Next(b, drop, row, VersionId(1), 3, VersionId(2)), CE::row_deleted, "after a drop");
  }
  fs::remove(drop);
  // Dead at restart: the next process passes over it.
  const auto dead = b.Copy(b.bytes, "dead.db");
  {
    Session s(b, dead);
    Begin(s, 2);
    Expect(b, s, dead, Next(b, dead, row, v0, 2, VersionId(1)), "left in progress");
  }
  Sees(b, dead, row, v0, "in progress at close");
  {
    Session s(b, dead);  // restart marks 2 dead
    Begin(s, 3);
    const auto passed = Expect(b, s, dead, Next(b, dead, row, v0, 3, VersionId(2)), "past a dead version");
    Check(passed.row_version == ReadRelation(b, b.bytes).Row(ReadRelation(b, b.bytes).Find(v0)).row_version + 2,
          "the dead version keeps its row_version");
    Commit(*s.w, s.sink(), 3);
  }
  Sees(b, dead, row, VersionId(2), "the later transaction's version");
  fs::remove(dead);
  std::cout << b.name << " states page_size=" << b.size << '\n';
}

// ---- Refusals before any write -----------------------------------------------------
void Refusals(const Db& b) {
  const auto [row, v0] = Pick(b);
  const auto path = b.Copy(b.bytes, "refusals.db");
  Session s(b, path);
  Begin(s, 2);
  Begin(s, 3, true);
  Begin(s, 4);
  Commit(*s.w, s.sink(), 4);
  Begin(s, 5);
  Rollback(*s.w, s.sink(), 5);
  const auto good = Next(b, path, row, v0, 2, VersionId(1));
  const auto variant = [&](auto change, CE error, std::string_view label) {
    auto r = good;
    change(r);
    Refused(b, s, path, r, error, label);
  };
  variant([](auto& r) { r.role = 0; }, CE::invalid_request, "role 0");
  // step4: role 7 is now the comments relation; the first role outside the
  // root is 8, and a non-comment successor in role 7 is invalid_role.
  variant([](auto& r) { r.role = 8; }, CE::invalid_request, "role 8");
  variant([](auto& r) { r.role = 7; }, CE::invalid_role, "step4: a non-comment successor in role 7");
  variant([](auto& r) { r.transaction_order = 0; }, CE::invalid_request, "transaction order 0");
  variant([](auto& r) { r.transaction_uuid = {}; }, CE::invalid_request, "nil transaction");
  variant([](auto& r) { r.row_uuid = {}; }, CE::invalid_request, "nil row");
  variant([](auto& r) { r.predecessor_version_uuid = {}; }, CE::invalid_request, "nil predecessor");
  variant([](auto& r) { r.version_uuid = {}; }, CE::invalid_request, "nil version");
  variant([](auto& r) { r.version_uuid = r.predecessor_version_uuid; }, CE::invalid_request, "version = predecessor");
  {
    const auto before = ReadAll(path);
    for (const auto change : {0, 1, 2, 3}) {
      auto binding = b.Binding();
      if (change == 0) binding.catalog_root.kind = 5;
      if (change == 1) binding.state_root.kind = 3;
      if (change == 2) binding.database_uuid = {};
      if (change == 3) binding.maximum_base_pages = 0;
      const auto result = db::WriteCatalogSuccessorVersion(s.sink(), binding, good);
      Check(result.error == CE::invalid_request && !result.writes, "a malformed binding is refused");
    }
    Check(ReadAll(path) == before, "malformed bindings write nothing");
  }
  Refused(b, s, path, good, CE::read_only_database, "read-only database", true);
  // SB_NATIVE_NAME_TREES (N4a): a name record carries its name; one without
  // (or a name on a record that is not one) cannot follow any base.
  variant([](auto& r) { r.metadata.record.header.kind = c::CatalogRecordKind::localized_name; },
          CE::invalid_successor, "a name record without its name");
  variant([](auto& r) { r.transaction_order = 1; }, CE::transaction_not_active, "the creator, committed");
  variant([](auto& r) { r.transaction_order = 99; }, CE::transaction_not_active, "a number never begun");
  variant([](auto& r) { r.transaction_uuid = TxId(7); }, CE::transaction_not_active, "another transaction's id");
  variant(
      [](auto& r) {
        r.transaction_order = 4;
        r.transaction_uuid = TxId(4);
      },
      CE::transaction_not_active, "a committed transaction");
  variant(
      [](auto& r) {
        r.transaction_order = 5;
        r.transaction_uuid = TxId(5);
      },
      CE::transaction_not_active, "a rolled-back transaction");
  {
    auto r = Next(b, path, row, v0, 3, VersionId(1));
    Refused(b, s, path, r, CE::read_only_transaction, "a read-only transaction");
  }
  variant([](auto& r) { r.row_uuid = VersionId(999); }, CE::row_not_found, "no such row");
  {
    const auto rows = Choose(b, b.bytes);
    const Uuid other = rows.has_same && rows.same_version != v0 ? rows.same_version : rows.other_version;
    // The seeded relation has one row: no other row's version to name
    // (update_conflict is also covered in States, on both databases).
    if (!other.is_nil() && other != v0) {
      variant([&](auto& r) { r.predecessor_version_uuid = other; }, CE::update_conflict, "another row's version");
      variant([&](auto& r) { r.version_uuid = other; }, CE::invalid_successor, "an existing version id");
    }
  }
  variant([](auto& r) { r.metadata.definition_version -= 1; }, CE::invalid_successor, "definition_version unchanged");
  variant([](auto& r) { r.metadata.definition_version += 1; }, CE::invalid_successor, "definition_version skips");
  variant([](auto& r) { r.metadata.record.header.object_uuid.value = VersionId(998); }, CE::invalid_successor,
          "another object");
  variant([](auto& r) { r.metadata.record.header.row_uuid.value = VersionId(997); }, CE::invalid_successor,
          "metadata for another row");
  variant([](auto& r) { r.metadata.creator_transaction_uuid.value = TxId(9); }, CE::invalid_successor,
          "another creator");
  variant([](auto& r) { r.metadata.creator_local_transaction_id = 9; }, CE::invalid_successor,
          "another creator number");
  variant([](auto& r) { r.metadata.authority_scope = c::CatalogAuthorityScope::cluster; }, CE::invalid_successor,
          "cluster authority");
  if (good.metadata.catalog_generation > 0)
    variant([](auto& r) { r.metadata.catalog_generation -= 1; }, CE::invalid_successor, "an earlier catalog generation");
  // Never ahead of the catalog root, which only the post-commit root rewrite
  // advances (owner Q24 = 1).
  {
    const auto root = pg::DecodeNativeCatalogRoot(PageOf(b.bytes, b.catalog_root.page_number, b.size));
    Check(root.ok(), "the catalog root decodes");
    const auto& x = *root.root;
    variant([&](auto& r) { r.metadata.catalog_generation = x.catalog_generation + 1; }, CE::ahead_of_catalog_root,
            "a catalog generation above the root's");
    variant([&](auto& r) { r.metadata.schema_epoch = x.schema_epoch + 1; }, CE::ahead_of_catalog_root,
            "a schema epoch above the root's");
    variant([&](auto& r) { r.metadata.security_epoch = x.security_epoch + 1; }, CE::ahead_of_catalog_root,
            "a security epoch above the root's");
    variant([&](auto& r) { r.metadata.resource_epoch = x.resource_epoch + 1; }, CE::ahead_of_catalog_root,
            "a resource epoch above the root's");
  }
  // The base is the newest committed version: a successor that skips over an
  // in-progress one is refused below in States; here the same request then
  // succeeds unchanged.
  Expect(b, s, path, good, "the unchanged request");
  std::cout << b.name << " refusals page_size=" << b.size << '\n';
}

// ---- A runtime authority binding never ahead of the root -----------------------------
// The security reader refuses a binding whose catalog_generation or
// security_epoch is above the root's; a committed one would leave a file it
// cannot serve if the server stops before the post-commit root rewrite.
void BindingAhead(const Db& b) {
  if (b.name != "production") return;
  RoleScope scope(b, 5);
  const auto rel = ReadRelation(b, b.bytes);
  std::optional<std::pair<Uuid, Uuid>> found;
  for (const auto& l : rel.leaves)
    for (const auto& row : l.leaf.body.rows)
      if (!found && c::IsCatalogRuntimeAuthorityBindingPayload(l.metadata.at(row.version_uuid).record.payload))
        found = {{row.row_uuid.value, row.version_uuid}};
  Check(found.has_value(), "the production configuration relation holds a runtime authority binding");
  const auto root = pg::DecodeNativeCatalogRoot(PageOf(b.bytes, b.catalog_root.page_number, b.size));
  Check(root.ok(), "the catalog root decodes");
  const auto path = b.Copy(b.bytes, "binding.db");
  {
    Session s(b, path);
    Begin(s, 2);
    // That family binds its payload's generation to definition_version and its
    // catalog_generation and security_epoch to the metadata's: change both.
    const auto with = [&](auto change) {
      auto r = Successor(b, rel, found->first, found->second, 2, VersionId(1));
      auto binding = c::DecodeCatalogRuntimeAuthorityBinding(r.metadata.record.payload);
      Check(binding.ok(), "the binding decodes");
      binding.record->generation = r.metadata.definition_version;
      change(*binding.record);
      r.metadata.catalog_generation = binding.record->catalog_generation;
      r.metadata.security_epoch = binding.record->security_epoch;
      const auto payload = c::EncodeCatalogRuntimeAuthorityBinding(*binding.record);
      Check(payload.ok(), "the binding encodes");
      r.metadata.record.payload.assign(payload.bytes.begin(), payload.bytes.end());
      return r;
    };
    Refused(b, s, path, with([&](auto& x) { x.catalog_generation = root.root->catalog_generation + 1; }),
            CE::ahead_of_catalog_root, "a binding catalog generation above the root's");
    Refused(b, s, path, with([&](auto& x) { x.security_epoch = root.root->security_epoch + 1; }),
            CE::ahead_of_catalog_root, "a binding security epoch above the root's");
    const auto plain = with([](auto&) {});
    const auto result = Write(b, s.sink(), plain);
    if (!result.ok()) std::cerr << "binding successor: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok(), "the same binding successor within the root's values is written");
  }
  fs::remove(path);
  std::cout << b.name << " binding page_size=" << b.size << '\n';
}

// ---- Filling the relation -------------------------------------------------------------
void Fill(const Db& b) {
  const auto [row, v0] = Pick(b);
  const auto path = b.Copy(b.bytes, "fill.db");
  unsigned written = 0, one = 0, two = 0;
  Uuid last = v0;
  {
    Session s(b, path);
    Begin(s, 2);
    for (;; ++written) {
      Check(written < 100000, "the relation fills");
      const auto r = Next(b, path, row, last, 2, VersionId(1000 + written));
      if (!Oracle(b, ReadAll(path), r)) {
        Refused(b, s, path, r, CE::relation_full, "no reached leaf has room");
        break;
      }
      const auto result = Expect(b, s, path, r, "filling");
      (result.writes == 1 ? one : two) += 1;
      last = r.version_uuid;
    }
    Commit(*s.w, s.sink(), 2);
  }
  Sees(b, path, row, last, "the last version of a full relation");
  fs::remove(path);
  std::cout << b.name << " fill page_size=" << b.size << " successors=" << written << " one_write=" << one
            << " two_writes=" << two << '\n';
}

// ---- Process loss, torn writes, I/O failures ---------------------------------------
class RecordingSink final : public d::CarefulWriteSink {
 public:
  explicit RecordingSink(d::CarefulWriteSink& inner) : inner_(inner) {}
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    const auto r = inner_.WriteAt(offset, bytes);
    if (r.ok()) writes.emplace_back(offset, std::vector<byte>(bytes.begin(), bytes.end()));
    return r;
  }
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }
  std::vector<std::pair<u64, std::vector<byte>>> writes;

 private:
  d::CarefulWriteSink& inner_;
};
using Writes = std::vector<std::pair<u64, std::vector<byte>>>;
std::vector<byte> Apply(std::span<const byte> base, const Writes& w, std::size_t k, std::size_t torn_half = 0) {
  std::vector<byte> out(base.begin(), base.end());
  for (std::size_t i = 0; i < k; ++i)
    std::copy(w[i].second.begin(), w[i].second.end(), out.begin() + static_cast<std::ptrdiff_t>(w[i].first));
  if (torn_half) {
    const auto& t = w[k];
    std::copy(t.second.begin(), t.second.begin() + static_cast<std::ptrdiff_t>(t.second.size() / 2),
              out.begin() + static_cast<std::ptrdiff_t>(t.first));
  }
  return out;
}
// The script: BEGIN 2, a successor of `row` by 2, COMMIT 2.
// BEGIN, the transaction's event (W3b-2b; `event_writes` gets its write
// count), the successor, COMMIT.
bool Script(const Db& b, pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, const Uuid& row, const Uuid& v0,
            const db::CatalogVersionRequest& r, u32* event_writes = nullptr) {
  pg::TransactionHeaderRecord h;
  h.transaction_uuid = TxId(2);
  if (!w.Begin(sink, h).ok()) return false;
  const auto event = WriteEvent(b, sink, 2, TxId(2));
  if (!event.ok()) return false;
  if (event_writes) *event_writes = event.writes;
  if (!Write(b, sink, r).ok()) return false;
  (void)row;
  (void)v0;
  return w.Commit(sink, 2, CommitTime(2)).ok();
}
[[noreturn]] void Child(const Db& b, const std::string& path, const Uuid& row, const Uuid& v0,
                        const db::CatalogVersionRequest& r, u32 limit, bool torn) {
  try {
    d::FileDevice device;
    if (!device.Open(path, d::FileOpenMode::open_existing).ok()) _exit(kChildFailed);
    int code = kChildCompleted;
    {
      d::FileDeviceCarefulWriteSink file(device);
      std::optional<scratchbird::tests::CrashAfterWritesSink> crash;
      std::optional<scratchbird::tests::TornWriteSink> tear;
      d::CarefulWriteSink* sink = torn ? static_cast<d::CarefulWriteSink*>(&tear.emplace(file, limit, kPlannedCrash))
                                       : static_cast<d::CarefulWriteSink*>(&crash.emplace(file, limit, kPlannedCrash));
      pg::TransactionInventoryWriter w(b.TxBinding());
      if (!w.Restart(*sink).ok() || !Script(b, w, *sink, row, v0, r)) code = kChildFailed;
    }
    if (!device.Close().ok()) code = kChildFailed;
    _exit(code);
  } catch (...) {
    _exit(kChildException);
  }
}
bool IsLeafPage(const Db& b, std::span<const byte> file, u64 offset) {
  if (offset % b.size) return false;
  for (const auto& l : ReadRelation(b, file).leaves)
    if (l.page * b.size == offset) return true;
  return false;
}
// After a loss that left the relation readable: a new process restarts and
// writes the next successor on the version the committed read sees.
void Recover(const Db& b, const std::string& path, const Uuid& row, std::string_view label) {
  const auto seen = Look(b, path, row);
  Check(seen.read && seen.version, std::string(label) + ": the relation reads");
  Uuid next_version = VersionId(5000);
  {
    Session s(b, path);
    pg::TransactionHeaderRecord h;
    const auto rel = ReadRelation(b, ReadAll(path));
    // The next number: whatever BEGIN takes after the restart.
    h.transaction_uuid = TxId(40);
    const auto begun = s.w->Begin(s.sink(), h);
    Check(begun.ok(), std::string(label) + ": the next BEGIN");
    const u64 n = *begun.transaction_order;
    Audit(s, n, TxId(40));
    auto r = Successor(b, rel, row, *seen.version, n, next_version);
    r.transaction_uuid = TxId(40);
    r.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(40)};
    r.metadata.audit_uuid = EventRef(n);
    const auto written = Write(b, s.sink(), r);
    if (!written.ok()) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(written.error) << '\n';
    Check(written.ok(), std::string(label) + ": the next successor");
    Check(s.w->Commit(s.sink(), n, CommitTime(40)).ok(), std::string(label) + ": its COMMIT");
  }
  Sees(b, path, row, next_version, std::string(label) + " recovered");
}
void Losses(const Db& b) {
  const auto [row, v0] = Pick(b);
  const auto request = Successor(b, ReadRelation(b, b.bytes), row, v0, 2, VersionId(1));
  u32 event_writes = 0;
  Writes recorded;
  std::vector<byte> final_file;
  {
    const auto path = b.Copy(b.bytes, "record.db");
    {
      Session s(b, path, false);
      RecordingSink rec(s.sink());
      Check(s.w->Restart(rec).ok() && Script(b, *s.w, rec, row, v0, request, &event_writes), "the script runs");
      recorded = rec.writes;
    }
    final_file = ReadAll(path);
    Check(Apply(b.bytes, recorded, recorded.size()) == final_file, "the recorded writes are the file's changes");
    // The successor's writes, independently of the recording: BEGIN writes
    // three units; then the oracle's target page, then (when it is another
    // page) the predecessor's page with the forward link; then COMMIT.
    // W3b-2b: after BEGIN, the transaction's event (its writes counted).
    const std::size_t E = event_writes;
    Check(E >= 1, "the event is written");
    const auto after_begin = Apply(b.bytes, recorded, 3 + E);
    const auto expected = Oracle(b, after_begin, request);
    Check(expected.has_value(), "the oracle places the script's successor");
    const std::size_t pages = expected->target == expected->link ? 1 : 2;
    Check(recorded.size() == 3 + E + pages + 2, "BEGIN, the event, the successor's pages, COMMIT");
    const auto page_write = [&](std::size_t i, u64 page) {
      const auto image = PageOf(expected->file, page, b.size);
      return recorded[i].first == page * b.size && recorded[i].second == image;
    };
    Check(page_write(3 + E, expected->target), "the successor's page is written first");
    if (pages == 2) Check(page_write(4 + E, expected->link), "then the predecessor's forward link");
    Sees(b, path, row, VersionId(1), "the script's result");
    fs::remove(path);
  }
  const auto total = static_cast<u32>(recorded.size());
  unsigned cases = 0, torn_leaves = 0, torn_events = 0;
  for (const bool torn : {false, true}) {
    for (u32 k = torn ? 1 : 0; k <= total; ++k) {
      const bool full = !torn && k == total;
      const std::string label = b.name + (torn ? " torn write " : " crash after ") + std::to_string(k);
      const auto path = b.Copy(b.bytes, "loss.db");
      std::cout.flush();
      std::cerr.flush();
      const pid_t child = fork();
      Check(child >= 0, label + ": fork");
      if (child == 0) Child(b, path, row, v0, request, full ? kNoCrash : k, torn);
      int status = 0;
      Check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == (full ? kChildCompleted : kPlannedCrash),
            label + ": the child stops where planned");
      const auto file = ReadAll(path);
      const auto expected = torn ? Apply(b.bytes, recorded, k - 1, 1) : Apply(b.bytes, recorded, k);
      Check(file == expected, label + ": exactly the write prefix");
      const auto& at = recorded[torn ? k - 1 : 0];
      // W3b-2b: a write of the event torn (not a page of the successor's
      // relation): that page fails its seal, so the next transaction's event
      // is refused with no write.
      const bool torn_event = torn && k - 1 >= 3 && k - 1 < 3 + event_writes && at.second.size() == b.size &&
                              Apply(b.bytes, recorded, k - 1) != file && Apply(b.bytes, recorded, k) != file &&
                              !IsLeafPage(b, b.bytes, at.first);
      if (torn_event) {
        Session s(b, path);
        pg::TransactionHeaderRecord h;
        h.transaction_uuid = TxId(40);
        const auto begun = s.w->Begin(s.sink(), h);
        Check(begun.ok(), label + ": BEGIN still works");
        const auto mid = ReadAll(path);
        const auto refused = WriteEvent(b, s.sink(), *begun.transaction_order, TxId(40));
        if (refused.error != CE::invalid_catalog) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(refused.error) << '\n';
        Check(refused.error == CE::invalid_catalog && !refused.writes && ReadAll(path) == mid,
              label + ": the next event is refused (the torn page fails its seal), no write");
        ++torn_events;
        fs::remove(path);
        ++cases;
        continue;
      }
      const bool torn_leaf = torn && at.second.size() == b.size && IsLeafPage(b, b.bytes, at.first) &&
                             Apply(b.bytes, recorded, k - 1) != file && Apply(b.bytes, recorded, k) != file;
      if (torn_leaf) {
        // Q23 = 1: the seal detects the torn page; the relation is refused.
        const auto page = PageOf(file, at.first / b.size, b.size);
        Check(db::DecodeNativeCatalogLeaf(page).error == db::NativeCatalogLeafError::invalid_integrity,
              label + ": the torn leaf fails its seal");
        Check(!Look(b, path, row).read, label + ": the relation read is refused");
        Session s(b, path);
        const auto before = ReadAll(path);
        pg::TransactionHeaderRecord h;
        h.transaction_uuid = TxId(40);
        const auto begun = s.w->Begin(s.sink(), h);
        Check(begun.ok(), label + ": BEGIN still works");
        auto r = request;
        r.transaction_order = *begun.transaction_order;
        r.transaction_uuid = TxId(40);
        r.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(40)};
        r.metadata.creator_local_transaction_id = r.transaction_order;
        const auto mid = ReadAll(path);
        const auto refused = Write(b, s.sink(), r);
        Check(refused.error == CE::invalid_catalog && refused.leaf_error == db::NativeCatalogLeafError::invalid_integrity &&
                  !refused.writes && ReadAll(path) == mid,
              label + ": the writer refuses the torn leaf with no write");
        (void)before;
        ++torn_leaves;
      } else {
        Recover(b, path, row, label);
      }
      fs::remove(path);
      ++cases;
    }
  }
  Check(torn_leaves > 0, "a torn leaf write was seen");
  std::cout << b.name << " losses page_size=" << b.size << " writes=" << total << " cases=" << cases
            << " torn_leaves=" << torn_leaves << " torn_events=" << torn_events << " event_writes=" << event_writes << '\n';
}

class FaultSink final : public d::CarefulWriteSink {
 public:
  FaultSink(d::CarefulWriteSink& inner, u32 fail_at) : inner_(inner), fail_at_(fail_at) {}
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override {
    if (++calls_ == fail_at_) return Fail(d::BoundedIoOperation::read, offset, bytes.size());
    return inner_.ReadAt(offset, bytes);
  }
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (++calls_ == fail_at_) return Fail(d::BoundedIoOperation::write, offset, bytes.size());
    const auto r = inner_.WriteAt(offset, bytes);
    if (r.ok()) ++writes_;
    return r;
  }
  d::BoundedIoResult Sync() override {
    if (++calls_ == fail_at_) return Fail(d::BoundedIoOperation::sync, 0, 0);
    return inner_.Sync();
  }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }
  u32 writes() const { return writes_; }
  u32 calls() const { return calls_; }
  std::optional<d::BoundedIoOperation> failed() const { return failed_; }

 private:
  d::BoundedIoResult Fail(d::BoundedIoOperation kind, u64 offset, usize bytes) {
    failed_ = kind;
    d::BoundedIoResult r;
    r.operation = kind;
    r.error = d::BoundedIoError::native_failure;
    r.offset = offset;
    r.requested_bytes = bytes;
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  d::CarefulWriteSink& inner_;
  u32 fail_at_;
  u32 calls_ = 0, writes_ = 0;
  std::optional<d::BoundedIoOperation> failed_;
};
void Faults(const Db& b) {
  const auto [row, v0] = Pick(b);
  const auto request = Successor(b, ReadRelation(b, b.bytes), row, v0, 2, VersionId(1));
  Writes recorded;
  std::vector<byte> final_file;
  u32 calls = 0;
  {
    const auto path = b.Copy(b.bytes, "count.db");
    {
      Session s(b, path);
      RecordingSink rec(s.sink());
      FaultSink count(rec, kNoCrash);
      Check(Script(b, *s.w, count, row, v0, request), "the script runs");
      calls = count.calls();
      recorded = rec.writes;
    }
    final_file = ReadAll(path);
    fs::remove(path);
  }
  unsigned reads = 0, writes = 0;
  for (u32 i = 1; i <= calls; ++i) {
    const std::string label = b.name + " fault at call " + std::to_string(i);
    const auto path = b.Copy(b.bytes, "fault.db");
    {
      Session s(b, path);
      FaultSink fault(s.sink(), i);
      pg::TransactionHeaderRecord h;
      h.transaction_uuid = TxId(2);
      // Each step through the fault sink; after a failed read with nothing
      // written the step runs again through the file; after any other failure
      // the run stops.
      enum { begin, event, write, commit, done } step = begin;  // W3b-2b: the event after BEGIN
      bool stopped = false;
      while (step != done && !stopped) {
        const auto before = fault.writes();
        bool ok = false;
        if (step == begin) ok = s.w->Begin(fault, h).ok();
        if (step == event) ok = WriteEvent(b, fault, 2, TxId(2)).ok();
        if (step == write) ok = Write(b, fault, request).ok();
        if (step == commit) ok = s.w->Commit(fault, 2, CommitTime(2)).ok();
        if (ok) {
          step = static_cast<decltype(step)>(step + 1);
          continue;
        }
        Check(fault.failed().has_value(), label + ": a step fails only at the injected fault");
        // A read that fails before the step's first write leaves nothing to
        // undo; one after it (a later unit's preimage) is a failure after a
        // write was attempted, like a failed write or sync.
        if (*fault.failed() == d::BoundedIoOperation::read && fault.writes() == before) {
          Check(fault.writes() == before && ReadAll(path) == Apply(b.bytes, recorded, before),
                label + ": a failed read writes nothing");
          bool again = false;
          if (step == begin) again = s.w->Begin(s.sink(), h).ok();
          if (step == event) again = WriteEvent(b, s.sink(), 2, TxId(2)).ok();
          if (step == write) again = Write(b, s.sink(), request).ok();
          if (step == commit) again = s.w->Commit(s.sink(), 2, CommitTime(2)).ok();
          Check(again, label + ": the step succeeds again");
          ++reads;
          // The rest through the file.
          if (step < event && !WriteEvent(b, s.sink(), 2, TxId(2)).ok()) Check(false, label + ": the event");
          if (step < write && Write(b, s.sink(), request).ok() == false) Check(false, label + ": the successor");
          if (step < commit) Check(s.w->Commit(s.sink(), 2, CommitTime(2)).ok(), label + ": the COMMIT");
          Check(ReadAll(path) == final_file, label + ": the script's result");
          step = done;
        } else {
          Check(ReadAll(path) == Apply(b.bytes, recorded, fault.writes()),
                label + ": a failed write or sync leaves the prefix of the writes performed");
          ++writes;
          stopped = true;
        }
      }
    }
    fs::remove(path);
  }
  Check(reads > 0 && writes > 0, "both read and write faults were injected");
  std::cout << b.name << " faults page_size=" << b.size << " points=" << calls << " reads=" << reads
            << " writes_or_syncs=" << writes << '\n';
}

// ---- The catalog root, rewritten in place after COMMIT (owner Q24 = 1) ------------
pg::NativeCatalogRoot RootOf(const Db& b, std::span<const byte> file) {
  const auto root = pg::DecodeNativeCatalogRoot(PageOf(file, b.catalog_root.page_number, b.size));
  Check(root.ok(), "the catalog root decodes");
  return *root.root;
}
db::CatalogRootAdvanceRequest Advance(const pg::NativeCatalogRoot& now, u64 n, u64 tx) {
  db::CatalogRootAdvanceRequest r;
  r.transaction_order = n;
  r.transaction_uuid = TxId(tx);
  r.catalog_generation = now.catalog_generation + 1;
  r.schema_epoch = now.schema_epoch;
  r.security_epoch = now.security_epoch + 1;
  r.resource_epoch = now.resource_epoch;
  return r;
}
// The file the oracle expects after an advance: the root page re-encoded with
// the new values, no predecessor and no digest; every other byte unchanged.
std::vector<byte> AdvanceOracle(const Db& b, std::span<const byte> before, const db::CatalogRootAdvanceRequest& r) {
  auto root = RootOf(b, before);
  root.catalog_generation = r.catalog_generation;
  root.schema_epoch = r.schema_epoch;
  root.security_epoch = r.security_epoch;
  root.resource_epoch = r.resource_epoch;
  root.predecessor.reset();
  root.predecessor_sha256.fill(0);
  const auto image = pg::EncodeNativeCatalogRoot(root);
  Check(image.ok(), "the expected root encodes");
  std::vector<byte> out(before.begin(), before.end());
  std::copy(image.bytes.begin(), image.bytes.end(),
            out.begin() + static_cast<std::ptrdiff_t>(b.catalog_root.page_number * b.size));
  // The configuration and security route roots, levelled with it.
  for (const auto* at : {&b.configuration_root, &b.security_root}) {
    const auto was = pg::DecodeNativeCatalogRoot(PageOf(before, at->page_number, b.size));
    Check(was.ok() && !was.root->predecessor, "a route root decodes");
    auto level = *was.root;
    level.catalog_generation = r.catalog_generation;
    level.schema_epoch = r.schema_epoch;
    level.security_epoch = r.security_epoch;
    level.resource_epoch = r.resource_epoch;
    const auto route = pg::EncodeNativeCatalogRoot(level);
    Check(route.ok(), "the expected route root encodes");
    std::copy(route.bytes.begin(), route.bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(at->page_number * b.size));
  }
  return out;
}
// A route root's catalog generation as stored.
u64 RouteGeneration(const Db& b, std::span<const byte> file, const d::FilespaceRootReference& at) {
  const auto root = pg::DecodeNativeCatalogRoot(PageOf(file, at.page_number, b.size));
  Check(root.ok(), "a route root decodes");
  return root.root->catalog_generation;
}
void AdvanceRefused(const Db& b, Session& s, const std::string& path, const db::CatalogRootAdvanceRequest& r, CE error,
                    std::string_view label, bool read_only = false) {
  const auto before = ReadAll(path);
  auto binding = b.Binding();
  binding.read_only_database = read_only;
  const auto result = db::AdvanceCatalogRoot(s.sink(), binding, r);
  if (result.error != error) std::cerr << label << ": got " << db::CatalogVersionWriteErrorName(result.error) << '\n';
  Check(result.error == error && !result.writes && !result.syncs && !result.attempted,
        std::string(label) + ": refused as expected with no write");
  Check(ReadAll(path) == before, std::string(label) + ": the file is unchanged");
}
void RootAdvance(const Db& b) {
  const auto [row, v0] = Pick(b);
  const auto created = RootOf(b, b.bytes);
  Check(created.catalog_generation == 1 && !created.predecessor, "the create writes generation 1 with no history");
  const auto path = b.Copy(b.bytes, "root.db");
  {
    Session s(b, path);
    Begin(s, 2);
    Expect(b, s, path, Next(b, path, row, v0, 2, VersionId(1)), "the change the root publishes");
    AdvanceRefused(b, s, path, Advance(created, 2, 2), CE::transaction_not_committed, "before COMMIT");
    Commit(*s.w, s.sink(), 2);
    Begin(s, 3);
    Rollback(*s.w, s.sink(), 3);
    AdvanceRefused(b, s, path, Advance(created, 3, 3), CE::transaction_not_committed, "a rolled-back transaction");
    AdvanceRefused(b, s, path, Advance(created, 9, 9), CE::transaction_not_committed, "a number never begun");
    AdvanceRefused(b, s, path, Advance(created, 2, 7), CE::transaction_not_committed, "another transaction's id");
    AdvanceRefused(b, s, path, Advance(created, 2, 2), CE::read_only_database, "a read-only database", true);
    {
      auto r = Advance(created, 2, 2);
      r.catalog_generation += 1;
      AdvanceRefused(b, s, path, r, CE::root_regression, "a generation that skips");
      r.catalog_generation = created.catalog_generation;
      AdvanceRefused(b, s, path, r, CE::invalid_request, "the current generation");
      r = Advance(created, 2, 2);
      r.security_epoch = created.security_epoch - 1;
      AdvanceRefused(b, s, path, r, CE::root_regression, "a lowered security epoch");
      r = Advance(created, 2, 2);
      r.transaction_order = 0;
      AdvanceRefused(b, s, path, r, CE::invalid_request, "transaction order 0");
    }
    // A route root ahead of the catalog root (forged with the production
    // codec): no state the writer leaves; refused with no write.
    {
      const auto before = ReadAll(path);
      auto forged = pg::DecodeNativeCatalogRoot(PageOf(before, b.security_root.page_number, b.size));
      Check(forged.ok(), "the security route root decodes");
      auto ahead = *forged.root;
      ahead.catalog_generation += 1;
      const auto image = pg::EncodeNativeCatalogRoot(ahead);
      Check(image.ok(), "the forged route root encodes");
      auto file = before;
      std::copy(image.bytes.begin(), image.bytes.end(),
                file.begin() + static_cast<std::ptrdiff_t>(b.security_root.page_number * b.size));
      const auto forged_path = b.Copy(file, "route-ahead.db");
      Session f(b, forged_path);
      AdvanceRefused(b, f, forged_path, Advance(created, 2, 2), CE::invalid_catalog, "a route root ahead of the root");
      fs::remove(forged_path);
    }
    // The advance: the catalog root's unit, then each route root's.
    const auto first = Advance(created, 2, 2);
    const auto expected = AdvanceOracle(b, ReadAll(path), first);
    const auto done = db::AdvanceCatalogRoot(s.sink(), b.Binding(), first);
    if (!done.ok()) std::cerr << "advance: " << db::CatalogVersionWriteErrorName(done.error) << '\n';
    Check(done.ok() && done.writes == 3 && done.previous_generation == created.catalog_generation,
          "the committed change's root advance: three unit writes");
    Check(ReadAll(path) == expected, "the file is the oracle's: the three roots alone changed");
    const auto advanced = RootOf(b, ReadAll(path));
    Check(advanced.catalog_generation == 2 && !advanced.predecessor &&
              advanced.security_epoch == created.security_epoch + 1,
          "generation 2 in place, no predecessor, the security epoch raised");
    // The root now admits versions at its new values; never above them.
    Begin(s, 4);
    auto at_new = Next(b, path, row, VersionId(1), 4, VersionId(2));
    at_new.metadata.catalog_generation = advanced.catalog_generation;
    Refused(b, s, path,
            [&] {
              auto r = at_new;
              r.metadata.catalog_generation = advanced.catalog_generation + 1;
              return r;
            }(),
            CE::ahead_of_catalog_root, "still never above the advanced root");
    Expect(b, s, path, at_new, "a version at the advanced generation");
    Commit(*s.w, s.sink(), 4);
    // A second advance follows the first.
    const auto second = Advance(advanced, 4, 4);
    Check(db::AdvanceCatalogRoot(s.sink(), b.Binding(), second).ok(), "the next advance");
    AdvanceRefused(b, s, path, second, CE::root_regression, "the same advance again");
  }
  Check(RootOf(b, ReadAll(path)).catalog_generation == 3, "generation 3 after two advances");
  Sees(b, path, row, VersionId(2), "the readers admit the in-place root");
  fs::remove(path);
  std::cout << b.name << " root advance page_size=" << b.size << '\n';
}
// Process loss and torn writes over BEGIN, COMMIT and the advance: the write
// prefix. The advance writes the root's first 512-byte unit, which holds every
// changed byte: torn at its half-way point it splits the generation fields
// from the seal, the seal fails and every catalog read is refused. Any earlier
// loss leaves the root as it was and the readers admit it.
bool RootScript(const Db& b, pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink) {
  pg::TransactionHeaderRecord h;
  h.transaction_uuid = TxId(2);
  if (!w.Begin(sink, h).ok() || !w.Commit(sink, 2, CommitTime(2)).ok()) return false;
  auto binding = b.Binding();
  return db::AdvanceCatalogRoot(sink, binding, Advance(RootOf(b, b.bytes), 2, 2)).ok();
}
[[noreturn]] void RootChild(const Db& b, const std::string& path, u32 limit, bool torn) {
  try {
    d::FileDevice device;
    if (!device.Open(path, d::FileOpenMode::open_existing).ok()) _exit(kChildFailed);
    int code = kChildCompleted;
    {
      d::FileDeviceCarefulWriteSink file(device);
      std::optional<scratchbird::tests::CrashAfterWritesSink> crash;
      std::optional<scratchbird::tests::TornWriteSink> tear;
      d::CarefulWriteSink* sink = torn ? static_cast<d::CarefulWriteSink*>(&tear.emplace(file, limit, kPlannedCrash))
                                       : static_cast<d::CarefulWriteSink*>(&crash.emplace(file, limit, kPlannedCrash));
      pg::TransactionInventoryWriter w(b.TxBinding());
      if (!w.Restart(*sink).ok() || !RootScript(b, w, *sink)) code = kChildFailed;
    }
    if (!device.Close().ok()) code = kChildFailed;
    _exit(code);
  } catch (...) {
    _exit(kChildException);
  }
}
void RootLosses(const Db& b) {
  const auto [row, v0] = Pick(b);
  Writes recorded;
  {
    const auto path = b.Copy(b.bytes, "root-record.db");
    {
      Session s(b, path, false);
      RecordingSink rec(s.sink());
      Check(s.w->Restart(rec).ok() && RootScript(b, *s.w, rec), "the root script runs");
      recorded = rec.writes;
    }
    Check(recorded.size() == 8 && recorded[5].first == b.catalog_root.page_number * b.size &&
              recorded[6].first == b.configuration_root.page_number * b.size &&
              recorded[7].first == b.security_root.page_number * b.size &&
              recorded[5].second.size() == 512 && recorded[6].second.size() == 512 && recorded[7].second.size() == 512,
          "BEGIN, COMMIT, then the catalog root's first unit, then each route root's");
    fs::remove(path);
  }
  const auto total = static_cast<u32>(recorded.size());
  unsigned cases = 0, torn_roots = 0;
  for (const bool torn : {false, true}) {
    for (u32 k = torn ? 1 : 0; k <= total; ++k) {
      const bool full = !torn && k == total;
      const std::string label = b.name + (torn ? " root torn write " : " root crash after ") + std::to_string(k);
      const auto path = b.Copy(b.bytes, "root-loss.db");
      std::cout.flush();
      std::cerr.flush();
      const pid_t child = fork();
      Check(child >= 0, label + ": fork");
      if (child == 0) RootChild(b, path, full ? kNoCrash : k, torn);
      int status = 0;
      Check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == (full ? kChildCompleted : kPlannedCrash),
            label + ": the child stops where planned");
      const auto file = ReadAll(path);
      Check(file == (torn ? Apply(b.bytes, recorded, k - 1, 1) : Apply(b.bytes, recorded, k)),
            label + ": exactly the write prefix");
      const bool mixed = torn && Apply(b.bytes, recorded, k - 1) != file && Apply(b.bytes, recorded, k) != file;
      const u64 torn_page = torn ? recorded[k - 1].first / b.size : 0;
      const u64 applied = torn && !mixed && Apply(b.bytes, recorded, k) == file ? k : torn ? k - 1 : k;
      if (mixed && torn_page == b.catalog_root.page_number) {
        Check(pg::DecodeNativeCatalogRoot(PageOf(file, torn_page, b.size)).error ==
                  pg::NativeCatalogRootError::invalid_integrity,
              label + ": the torn catalog root fails its seal");
        Check(!Look(b, path, row).read, label + ": every catalog read is refused");
        ++torn_roots;
      } else if (mixed && (torn_page == b.configuration_root.page_number || torn_page == b.security_root.page_number)) {
        // Only open verification reads a route root: the catalog reads go on.
        Check(pg::DecodeNativeCatalogRoot(PageOf(file, torn_page, b.size)).error ==
                  pg::NativeCatalogRootError::invalid_integrity,
              label + ": the torn route root fails its seal");
        const auto seen = Look(b, path, row);
        Check(seen.read && seen.version == v0, label + ": the catalog reads through the catalog root");
        ++torn_roots;
      } else {
        const auto seen = Look(b, path, row);
        Check(seen.read && seen.version == v0, label + ": the readers admit the roots as they stand");
        Check(RootOf(b, file).catalog_generation == (applied >= 6 ? 2u : 1u),
              label + ": the catalog root advanced only by its own write");
        Check(RouteGeneration(b, file, b.configuration_root) == (applied >= 7 ? 2u : 1u) &&
                  RouteGeneration(b, file, b.security_root) == (applied >= 8 ? 2u : 1u),
              label + ": each route root at most the catalog root, levelled by its own write");
      }
      fs::remove(path);
      ++cases;
    }
  }
  Check(torn_roots == 3, "each torn root unit was seen");
  std::cout << b.name << " root losses page_size=" << b.size << " writes=" << total << " cases=" << cases << '\n';
}
// Every read, write and sync failed in turn over BEGIN, COMMIT and the advance.
void RootFaults(const Db& b) {
  const auto request = Advance(RootOf(b, b.bytes), 2, 2);
  Writes recorded;
  std::vector<byte> final_file;
  u32 calls = 0;
  {
    const auto path = b.Copy(b.bytes, "root-count.db");
    {
      Session s(b, path);
      RecordingSink rec(s.sink());
      FaultSink count(rec, kNoCrash);
      Check(RootScript(b, *s.w, count), "the root script runs");
      calls = count.calls();
      recorded = rec.writes;
    }
    final_file = ReadAll(path);
    fs::remove(path);
  }
  unsigned reads = 0, writes = 0;
  for (u32 i = 1; i <= calls; ++i) {
    const std::string label = b.name + " root fault at call " + std::to_string(i);
    const auto path = b.Copy(b.bytes, "root-fault.db");
    {
      Session s(b, path);
      FaultSink fault(s.sink(), i);
      pg::TransactionHeaderRecord h;
      h.transaction_uuid = TxId(2);
      const auto step_on = [&](int step, d::CarefulWriteSink& sink) {
        if (step == 0) return s.w->Begin(sink, h).ok();
        if (step == 1) return s.w->Commit(sink, 2, CommitTime(2)).ok();
        return db::AdvanceCatalogRoot(sink, b.Binding(), request).ok();
      };
      for (int step = 0; step < 3; ++step) {
        const auto before = fault.writes();
        if (step_on(step, fault)) continue;
        Check(fault.failed().has_value(), label + ": a step fails only at the injected fault");
        if (*fault.failed() == d::BoundedIoOperation::read && fault.writes() == before) {
          Check(ReadAll(path) == Apply(b.bytes, recorded, before), label + ": a failed read writes nothing");
          for (int rest = step; rest < 3; ++rest) Check(step_on(rest, s.sink()), label + ": the rest succeeds");
          Check(ReadAll(path) == final_file, label + ": the script's result");
          ++reads;
        } else {
          Check(ReadAll(path) == Apply(b.bytes, recorded, fault.writes()),
                label + ": a failed write or sync leaves the prefix of the writes performed");
          ++writes;
        }
        break;
      }
    }
    fs::remove(path);
  }
  Check(reads > 0 && writes > 0, "both read and write faults were injected");
  std::cout << b.name << " root faults page_size=" << b.size << " points=" << calls << " reads=" << reads
            << " writes_or_syncs=" << writes << '\n';
}
}  // namespace

// native_catalog_insert_writer_test includes this file for its databases,
// transactions, relation read and committed reads (W3b-1a).
#ifndef SB_CATALOG_VERSION_WRITER_TEST_WITHOUT_MAIN
int main(int argc, char** argv) {
  unsigned profile = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[2]));
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const auto production = ProductionDb(profile);
    const auto seeded = SeededDb(profile);
    const auto rows = Choose(*production, production->bytes);
    Check(rows.has_other && rows.has_same, "the security relation has full leaves and a leaf with room");
    Check(!ReadRelation(*seeded, seeded->bytes).indexed && ReadRelation(*production, production->bytes).indexed,
          "one unindexed and one indexed relation");
    for (const Db* b : {production.get(), seeded.get()}) {
      States(*b);
      Refusals(*b);
      BindingAhead(*b);
      RootAdvance(*b);
      Fill(*b);
      Losses(*b);
      Faults(*b);
      RootLosses(*b);
      RootFaults(*b);
    }
    std::cout << "PASS native catalog version writer page_size=" << production->size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native catalog version writer profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
#endif  // SB_CATALOG_VERSION_WRITER_TEST_WITHOUT_MAIN
