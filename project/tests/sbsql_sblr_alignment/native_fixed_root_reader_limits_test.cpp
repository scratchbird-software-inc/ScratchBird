// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// Memory, image-budget and digest-provider limits of the lease readers on the
// fixed-root source, over a database written by the production create. Drift
// removal R4b: these purposes were checked only on the selected-checkpoint
// source (the drift layer's guarded modes). Here, at every page size:
//   - each reader workspace is admitted at its exact size and refused one byte
//     short, with nothing retained from the refusal;
//   - each catalog relation reads within exactly the image budget it uses and
//     is refused, with no rows, one byte short of it;
//   - a read under the lease makes no heap allocation;
// and, on one page size, every digest-provider call a relation read makes is
// failed in turn, each failure mode in turn: every one refuses, and the read
// succeeds again afterwards (ctest shards per catalog relation and digest
// site: --digest-relation N SHARD SHARDS sweeps the sites s with
// s % SHARDS == SHARD). Read failures and corruption of every page a reader reads
// are swept by native_fixed_root_readers_test.
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_allocation_lease_reader.hpp"
#include "native_catalog_roots_lease_reader.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// Heap: count allocations while armed.
bool counting = false;
unsigned long allocations = 0;
// Digest provider: count sites, or fail one phase at the Nth site.
unsigned hash_fault = 0, hash_target = 1, hash_seen = 0;
bool hash_active = false, hash_counting = false;
}  // namespace

void* operator new(std::size_t n) {
  if (counting) ++allocations;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t alignment) {
  if (counting) ++allocations;
  const auto a = static_cast<std::size_t>(alignment);
  n = n ? n : 1;
  if (auto* p = std::aligned_alloc(a, (n + a - 1) / a * a)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

// Failure modes: 1 context allocation, 2 init, 3 update, 4 final, 5 short digest.
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new() {
  hash_active = (hash_fault || hash_counting) && ++hash_seen == hash_target && hash_fault;
  if (hash_active && hash_fault == 1) { hash_fault = 0; return nullptr; }
  return __real_EVP_MD_CTX_new();
}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*, const EVP_MD*, ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c, const EVP_MD* m, ENGINE* e) {
  if (hash_active && hash_fault == 2) { hash_fault = 0; return 0; }
  return __real_EVP_DigestInit_ex(c, m, e);
}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*, const void*, size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c, const void* b, size_t n) {
  if (hash_active && hash_fault == 3) { hash_fault = 0; return 0; }
  return __real_EVP_DigestUpdate(c, b, n);
}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*, unsigned char*, unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c, unsigned char* b, unsigned int* n) {
  if (hash_active && hash_fault == 4) { hash_fault = 0; return 0; }
  const int r = __real_EVP_DigestFinal_ex(c, b, n);
  if (hash_active && hash_fault == 5) { hash_fault = 0; *n = 31; }
  return r;
}
extern "C" int __real_EVP_Digest(const void*, size_t, unsigned char*, unsigned int*, const EVP_MD*, ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data, size_t bytes, unsigned char* out, unsigned int* size, const EVP_MD* md,
                                 ENGINE* engine) {
  const bool selected = (hash_fault || hash_counting) && ++hash_seen == hash_target && hash_fault;
  const auto mode = selected ? hash_fault : 0;
  if (selected) hash_fault = 0;
  if (mode && mode != 5) return 0;
  const auto result = __real_EVP_Digest(data, bytes, out, size, md, engine);
  if (mode == 5) *size = 31;
  return result;
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

// A node-issued memory grant of exactly `bytes` (the fixture of
// native_fixed_root_readers_test).
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
    r.route_label = "storage.fixed_root.reader_limits";
    r.purpose = "fixed-root reader limits";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Fixed(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "fixed-root reader limits";
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
    Check(device.Close().ok() && device.Open(path, d::FileOpenMode::open_existing,d::DatabaseFileOwnership::native_data_file).ok(), "independent reopen");
  }
  std::vector<d::NativeFilespaceDevice> Devices() { return {{receipt.filespace_uuid, profile, &device}}; }
};

db::NativeCatalogRelationMemoryLimits Limits(const Database& n) {
  const auto pages = [&](std::size_t bytes) { return bytes / static_cast<std::size_t>(n.size); };
  return {n.profile, 1, pages(16u << 20), 4096, 4096, {pages(4u << 20), 4u << 20}};
}
db::NativeCatalogRelationBinding Binding(const Database& n, unsigned i) {
  return {n.receipt.workspace.relation_uuids[i], n.receipt.workspace.index_dependencies[i]};
}

// The smallest grant that admits `prepare`, found by bisection between a size
// that is refused and one that is admitted. Every probe uses a fresh grant.
template <class Prepare>
u64 SmallestGrant(const Uuid& database, u64 low, u64 high, Prepare&& prepare) {
  Check(!prepare(database, low) && prepare(database, high), "a refused and an admitted grant bracket the size");
  while (high - low > 1) {
    const auto mid = low + (high - low) / 2;
    (prepare(database, mid) ? high : low) = mid;
  }
  return high;
}

// The workspaces of the visibility (relation and B-tree) and catalog-roots
// readers: admitted at their declared size, refused one byte short.
void Workspaces(Database& n) {
  const auto l = Limits(n);
  const auto database = n.receipt.database_uuid;
  const auto declared = db::NativeCatalogVisibilityWorkspaceBytes(l);
  Check(declared > 0, "the visibility workspace declares its size");
  const auto visibility = [&](const Uuid& id, u64 bytes) {
    Grant g(id, bytes);
    auto p = db::PrepareNativeCatalogVisibilityRead(l, g.memory, g.binding);
    if (!p.ok())
      Check(p.error == db::NativeCatalogRelationLeaseError::memory_failure,
            "a short grant is refused as a memory failure, not a partial workspace");
    return p.ok();
  };
  const auto smallest = SmallestGrant(database, declared / 2, declared * 2, visibility);
  Check(smallest == declared, "the visibility workspace needs exactly its declared size");
  Check(visibility(database, declared) && !visibility(database, declared - 1),
        "the declared size is admitted and one byte short is refused");
  const auto roots_declared = db::NativeCatalogRootsWorkspaceBytes(n.profile);
  const auto roots = [&](const Uuid& id, u64 bytes) {
    Grant g(id, bytes);
    return db::PrepareNativeCatalogRootsRead(n.profile, g.memory, g.binding).ok();
  };
  Check(roots_declared > 0 && SmallestGrant(database, roots_declared / 2, roots_declared * 2, roots) == roots_declared,
        "the catalog-roots workspace needs exactly its declared size");
}

// Image budget and heap: every relation reads within exactly the image bytes
// it retains, is refused one byte short with no rows, and allocates nothing.
void ImageBudgets(Database& n) {
  const auto devices = n.Devices();
  const Uuid database = n.receipt.database_uuid, primary = n.receipt.filespace_uuid;
  Grant g(database, 512u << 20);
  auto fixed = db::AcquireNativeFixedRootMemoryLease(database, devices, primary, {256 * n.size, 64u << 20}, g.memory,
                                                     g.binding);
  Check(fixed.ok(), "fixed-root lease");
  auto prepared = db::PrepareNativeCatalogVisibilityRead(Limits(n), g.memory, g.binding);
  Check(prepared.ok(), "visibility workspace");
  for (unsigned i = 0; i < 6; ++i) {
    const auto read = [&](u64 budget) {
      return db::NativeCatalogVisibilityLeaseReader::ReadCommitted(*fixed.lease, 2, static_cast<u16>(i + 1), Binding(n, i),
                                                                   budget, prepared.workspace);
    };
    counting = true;
    allocations = 0;
    const auto baseline = read(512 * n.size);
    counting = false;
    if (!baseline.ok())
      std::cerr << "relation " << i + 1 << " error=" << int(baseline.error) << " source=" << int(baseline.source.error)
                << '\n';
    Check(baseline.ok(), "the relation reads committed");
    Check(allocations == 0, "a read under the fixed-root lease makes no heap allocation");
    const auto used = baseline.source.retained_image_bytes;
    const auto rows = baseline.rows.size();
    Check(used > 0 && used <= 512 * n.size, "the read retains page images within its budget");
    const auto exact = read(used);
    Check(exact.ok() && exact.rows.size() == rows, "exactly the retained image bytes are enough");
    const auto short_read = read(used - 1);
    Check(!short_read.ok() && short_read.rows.empty(), "one byte short of the retained image bytes is refused, no rows");
    Check(read(used).ok(), "the relation reads again after a refusal");
  }
  // The catalog roots and the primary allocation, under the same lease.
  auto roots = db::PrepareNativeCatalogRootsRead(n.profile, g.memory, g.binding);
  Check(roots.ok(), "roots workspace");
  counting = true;
  allocations = 0;
  const auto r = db::NativeCatalogRootsLeaseReader::Read(*fixed.lease, 64 * n.size, roots.workspace);
  counting = false;
  Check(r.ok() && allocations == 0, "the catalog roots read makes no heap allocation");
  auto allocation = db::PrepareNativeAllocationRead(16u << 20, g.memory, g.binding);
  Check(allocation.ok(), "allocation workspace");
  counting = true;
  allocations = 0;
  const auto a = db::NativeAllocationLeaseReader::Read(*fixed.lease, primary, 512 * n.size, allocation.workspace);
  counting = false;
  Check(a.ok() && allocations == 0, "the allocation read makes no heap allocation");
}

// Every digest-provider call a relation read makes, failed in turn in each
// failure mode: each is refused, and the relation reads again afterwards.
void DigestSweep(unsigned relation, unsigned shard, unsigned shards) {
  Database n(0);
  const auto devices = n.Devices();
  const Uuid database = n.receipt.database_uuid, primary = n.receipt.filespace_uuid;
  Grant g(database, 512u << 20);
  auto fixed = db::AcquireNativeFixedRootMemoryLease(database, devices, primary, {256 * n.size, 64u << 20}, g.memory,
                                                     g.binding);
  Check(fixed.ok(), "fixed-root lease for the digest sweep");
  auto prepared = db::PrepareNativeCatalogVisibilityRead(Limits(n), g.memory, g.binding);
  Check(prepared.ok(), "digest sweep workspace");
  unsigned total_sites = 0;
  for (unsigned i = relation - 1; i < relation; ++i) {
    const auto run = [&] {
      return db::NativeCatalogVisibilityLeaseReader::ReadCommitted(*fixed.lease, 2, static_cast<u16>(i + 1),
                                                                   Binding(n, i), 512 * n.size, prepared.workspace)
          .ok();
    };
    hash_counting = true;
    hash_seen = 0;
    Check(run(), "digest sweep baseline");
    hash_counting = false;
    const unsigned sites = hash_seen;
    Check(sites > 0, "the relation read verifies page seals");
    total_sites += sites;
    for (unsigned site = 1; site <= sites; ++site)
      for (unsigned mode = 1; mode <= 5 && site % shards == shard; ++mode) {
        hash_seen = 0;
        hash_target = site;
        hash_fault = mode;
        const bool ok = run();
        const bool consumed = hash_fault == 0;
        hash_fault = 0;
        hash_active = false;
        if (!consumed) continue;  // mode 1-4 at an EVP_Digest site and modes 2-5 after a failed context are not reached
        if (ok)
          std::cerr << "relation " << i + 1 << ": digest site " << site << " of " << sites << " mode " << mode
                    << " accepted\n";
        Check(!ok, "every failed digest refuses the read");
      }
    hash_target = 1;
    Check(run(), "the relation reads again after the digest sweep");
  }
  std::cout << "digest sites swept: " << total_sites << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 5 && std::string_view(argv[1]) == "--digest-relation") {
      const auto relation = std::stoi(argv[2]), shard = std::stoi(argv[3]), shards = std::stoi(argv[4]);
      Check(relation >= 1 && relation <= 6 && shards >= 1 && shard >= 0 && shard < shards,
            "digest shard: relation 1-6, shard in [0, shards)");
      DigestSweep(static_cast<unsigned>(relation), static_cast<unsigned>(shard), static_cast<unsigned>(shards));
    } else {
      Check(argc == 1, "arguments: none, or --digest-relation N SHARD SHARDS");
      for (unsigned profile = 0; profile < 5; ++profile) {
        Database n(profile);
        Workspaces(n);
        ImageBudgets(n);
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "native fixed root reader limits FAIL " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "native fixed root reader limits: " << checks << " checks passed\n";
  return 0;
}
