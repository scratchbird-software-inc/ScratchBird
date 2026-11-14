// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// The lease readers on the fixed-root source (R2a of the drift removal). A
// database written by the production create is read through both sources:
// the selected-checkpoint lease (the drift layer, removed in R4) and the
// fixed-root lease. On an in-place database both must yield the same pages
// and rows. On the fixed-root path, every reader read is then failed in turn
// and corrupted in turn: each must refuse. With no checkpoint digest on that
// path, this is what shows every content page a reader accepts is sealed.
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_allocation_lease_reader.hpp"
#include "native_catalog_roots_lease_reader.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
thread_local bool track = false;
thread_local unsigned reads = 0, fail_at = 0, corrupt_at = 0;
}  // namespace
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* b, size_t n, off_t offset) {
  if (!track) return __real_pread(fd, b, n, offset);
  ++reads;
  if (fail_at && reads == fail_at) {
    errno = EIO;
    return -1;
  }
  const auto r = __real_pread(fd, b, n, offset);
  if (corrupt_at && reads == corrupt_at && r > 0) static_cast<unsigned char*>(b)[r - 1] ^= 1;
  return r;
}

namespace {
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace m = scratchbird::core::memory;
namespace u = scratchbird::core::uuid;
using namespace scratchbird::core::platform;
unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

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
    r.route_label = "storage.fixed_root.readers";
    r.purpose = "fixed-root reader conformance";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Fixed(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "fixed-root reader conformance";
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

struct Database {
  scratchbird::tests::OwnedTempDirectory artifacts;
  std::string path;
  d::FileDevice device;
  db::NativeDatabaseCreateReceipt receipt;
  u64 size = 0;
  Uuid profile;
  explicit Database(unsigned profile_index) {
    const auto& p = d::kCanonicalFilespacePageProfiles[profile_index];
    size = p.page_size_bytes;
    profile = p.uuid;
    db::NativeDatabaseCreateRequest r;
    r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
    r.database_name = "orders";
    r.database_default_language = scratchbird::core::catalog::kCatalogLanguage_en;/* S2: the language constant, not the tag "en" */
    r.page_size_profile_uuid = p.uuid;
    r.page_size_bytes = p.page_size_bytes;
    r.total_pages = p.page_size_bytes == 8192 ? 1024 : 512;
    r.creation_utc_millis = 1789357072000ULL;
    r.engine_uuid = Fixed(4);
    r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
    r.bootstrap_principal_name = "BootstrapAdmin";
    r.bootstrap_credential_fingerprint = kFingerprint;
    r.service_principal_name = "NodeService";
    r.service_credential_fingerprint = kFingerprint;
    r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
    path = (artifacts.path() / "node").string();
    Check(device.Open(path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "node device");
    u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
    auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
    Check(made.ok(), "the production create writes a complete native database");
    receipt = *made.receipt;
    Check(device.Close().ok() && device.Open(path, d::FileOpenMode::open_existing).ok(), "independent reopen");
  }
  std::vector<d::NativeFilespaceDevice> Devices() { return {{receipt.filespace_uuid, profile, &device}}; }
};

db::NativeCatalogRelationMemoryLimits Limits(const Database& n) {
  // A constant byte budget at every page size: 16 MiB of base leaves, 4 MiB
  // of navigation.
  const auto pages = [&](std::size_t bytes) { return bytes / static_cast<std::size_t>(n.size); };
  return {n.profile, 1, pages(16u << 20), 4096, 4096, {pages(4u << 20), 4u << 20}};
}

bool SameBytes(std::span<const byte> a, std::span<const byte> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// Each relation, committed, through a given lease; rows as (object, payload).
template <class Lease>
std::vector<std::vector<std::pair<Uuid, std::string>>> Relations(const Lease& lease, Database& n, Grant& g,
                                                                 bool bound) {
  std::vector<std::vector<std::pair<Uuid, std::string>>> out(6);
  for (unsigned i = 0; i < 6; ++i) {
    auto prepared = db::PrepareNativeCatalogVisibilityRead(Limits(n), g.memory, g.binding);
    if (!prepared.ok())
      std::cerr << "visibility workspace error=" << int(prepared.error) << " memory=" << int(prepared.memory_error)
                << " page=" << n.size << " relation=" << i + 1 << '\n';
    Check(prepared.ok(), "visibility workspace");
    const db::NativeCatalogRelationBinding binding{n.receipt.workspace.relation_uuids[i],
                                                   n.receipt.workspace.index_dependencies[i]};
    const auto read = bound ? db::NativeCatalogVisibilityLeaseReader::ReadCommitted(
                                  lease, 2, static_cast<u16>(i + 1), binding, 512 * n.size, prepared.workspace)
                            : db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(
                                  lease, 2, static_cast<u16>(i + 1), 512 * n.size, prepared.workspace);
    Check(read.ok(), "every catalog relation reads committed");
    for (const auto& row : read.rows)
      out[i].push_back({row.metadata.record.header.object_uuid.value, std::string(row.metadata.record.payload)});
  }
  return out;
}

// Drift removal R3b3: the production create writes no checkpoint, so every
// reader on the fixed-root lease is checked against the file read
// independently: the catalog root and allocation map images against the pages
// read straight from the file, and every relation against the open-device
// committed read from page zero (itself compared byte for byte with the
// checkpoint read before R3b3, native_fixed_root_catalog_source). Until R3b3
// this compared each reader on a selected-checkpoint lease and on the
// fixed-root lease; every output agreed.
void Independent(unsigned profile) {
  Database n(profile);
  const auto devices = n.Devices();
  const Uuid database = n.receipt.database_uuid, primary = n.receipt.filespace_uuid;
  Grant gf(database, 512u << 20);
  auto fixed = db::AcquireNativeFixedRootMemoryLease(database, devices, primary, {256 * n.size, 64u << 20}, gf.memory,
                                                     gf.binding);
  Check(fixed.ok(), "fixed-root lease");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(n.device);
  Check(zero.ok(), "page zero read straight from the file");
  const auto stored_page = [&](u64 number) {
    std::vector<byte> bytes(n.size);
    const auto io = n.device.ReadAt(number * n.size, bytes.data(), bytes.size());
    Check(io.ok() && io.bytes_transferred == bytes.size(), "a page read straight from the file");
    return bytes;
  };
  const auto root_page = [&](u16 kind) {
    const auto r = std::find_if(zero.record->roots.begin(), zero.record->roots.end(),
                                [&](const auto& x) { return x.kind == kind; });
    Check(r != zero.record->roots.end(), "page zero names the root");
    return r->page_number;
  };
  // Catalog roots.
  {
    auto wf = db::PrepareNativeCatalogRootsRead(n.profile, gf.memory, gf.binding);
    Check(wf.ok(), "roots workspace");
    const auto b = db::NativeCatalogRootsLeaseReader::Read(*fixed.lease, 64 * n.size, wf.workspace);
    Check(b.ok() && !b.catalogs.empty() && b.feature_root_index < b.catalogs.size(), "catalog roots through the fixed root");
    const bool same_page = root_page(2) == root_page(8);
    Check(b.feature_root_index == (same_page ? 0u : 1u), "the feature catalog is the catalog root at create");
    Check(SameBytes(b.catalogs[0].image, stored_page(root_page(2))) && b.catalogs[0].inventory_entry_index == 0,
          "the catalog root image is the page as stored, created by the construction transaction");
  }
  // Allocation of the primary file.
  {
    auto wf = db::PrepareNativeAllocationRead(16u << 20, gf.memory, gf.binding);
    Check(wf.ok(), "allocation workspace");
    const auto b = db::NativeAllocationLeaseReader::Read(*fixed.lease, primary, 512 * n.size, wf.workspace);
    Check(b.ok() && b.chain.pages.size() == n.receipt.workspace.map_pages && b.root && b.root->page.page_number == root_page(3),
          "the allocation chain is the created maps, from page zero's head");
    for (std::size_t i = 0; i < b.chain.pages.size(); ++i)
      Check(SameBytes(b.chain.pages[i].image, stored_page(1 + i)), "every map image is the page as stored");
  }
  // Every relation, bound and discovered, against the open-device read.
  std::vector<std::vector<std::pair<Uuid, std::string>>> expected(6);
  for (unsigned i = 0; i < 6; ++i) {
    const auto read = db::ReadNativeCommittedCatalogVersionsFromOpenDevices(
        database, devices, db::NativeCatalogSource::PageZero(primary), 2, static_cast<u16>(i + 1),
        {n.receipt.workspace.relation_uuids[i], n.receipt.workspace.index_dependencies[i]},
        (n.size == 8192 ? 1024 : 512) * n.size);
    Check(read.ok(), "every relation reads committed through the open-device page-zero source");
    for (const auto& row : read.rows)
      expected[i].push_back({row.metadata.record.header.object_uuid.value, std::string(row.metadata.record.payload)});
  }
  for (const bool bound : {true, false}) {
    const auto b = Relations(*fixed.lease, n, gf, bound);
    Check(b == expected, "every relation yields, through the fixed-root lease, the rows the open-device read returns");
    std::size_t rows = 0;
    for (const auto& r : b) rows += r.size();
    Check(rows > 100, "a populated production catalog");
  }
}

// Every read a reader makes on the fixed-root path, failed and corrupted in turn.
void Sweeps() {
  Database n(0);
  const auto devices = n.Devices();
  const Uuid database = n.receipt.database_uuid, primary = n.receipt.filespace_uuid;
  Grant g(database, 512u << 20);
  auto fixed = db::AcquireNativeFixedRootMemoryLease(database, devices, primary, {256 * n.size, 64u << 20}, g.memory,
                                                     g.binding);
  Check(fixed.ok(), "fixed-root lease for the sweeps");
  for (unsigned i = 0; i < 6; ++i) {
    const db::NativeCatalogRelationBinding binding{n.receipt.workspace.relation_uuids[i],
                                                   n.receipt.workspace.index_dependencies[i]};
    // One workspace for the whole sweep: reuse invalidates only the previous
    // result, which each run checks before the next.
    auto prepared = db::PrepareNativeCatalogVisibilityRead(Limits(n), g.memory, g.binding);
    Check(prepared.ok(), "sweep workspace");
    const auto run = [&] {
      track = true;
      reads = 0;
      const auto read = db::NativeCatalogVisibilityLeaseReader::ReadCommitted(
          *fixed.lease, 2, static_cast<u16>(i + 1), binding, 512 * n.size, prepared.workspace);
      track = false;
      return read.ok();
    };
    Check(run(), "sweep baseline");
    const unsigned total = reads;
    Check(total >= 3, "the relation read reads pages");
    for (unsigned at = 1; at <= total; ++at) {
      fail_at = at;
      const bool ok = run();
      fail_at = 0;
      Check(!ok, "every failed reader read refuses");
      corrupt_at = at;
      const bool accepted = run();
      corrupt_at = 0;
      if (accepted) std::cerr << "relation " << i + 1 << ": corrupted read " << at << " of " << total << " accepted\n";
      Check(!accepted, "every corrupted reader read refuses");
    }
    Check(run(), "the relation reads again after the sweep");
  }
  // Allocation and catalog roots, the same way.
  {
    const auto run = [&] {
      auto w = db::PrepareNativeAllocationRead(16u << 20, g.memory, g.binding);
      Check(w.ok(), "allocation sweep workspace");
      track = true;
      reads = 0;
      const auto r = db::NativeAllocationLeaseReader::Read(*fixed.lease, primary, 512 * n.size, w.workspace);
      track = false;
      return r.ok();
    };
    Check(run(), "allocation baseline");
    // The primary chain is retained by the lease: no further reads.
    Check(reads == 0, "the primary allocation is served from the lease");
  }
  {
    const auto run = [&] {
      auto w = db::PrepareNativeCatalogRootsRead(n.profile, g.memory, g.binding);
      Check(w.ok(), "roots sweep workspace");
      track = true;
      reads = 0;
      const auto r = db::NativeCatalogRootsLeaseReader::Read(*fixed.lease, 64 * n.size, w.workspace);
      track = false;
      return r.ok();
    };
    Check(run(), "roots baseline");
    const unsigned total = reads;
    for (unsigned at = 1; at <= total; ++at) {
      fail_at = at;
      const bool ok = run();
      fail_at = 0;
      Check(!ok, "every failed roots read refuses");
      corrupt_at = at;
      const bool accepted = run();
      corrupt_at = 0;
      if (accepted) std::cerr << "roots: corrupted read " << at << " of " << total << " accepted\n";
      Check(!accepted, "every corrupted roots read refuses");
    }
  }
}
}  // namespace

int main() {
  try {
    for (unsigned profile = 0; profile < 5; ++profile) Independent(profile);
    Sweeps();
  } catch (const std::exception& e) {
    std::cerr << "native fixed root readers FAIL " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "native fixed root readers: " << checks << " checks passed\n";
  return 0;
}
