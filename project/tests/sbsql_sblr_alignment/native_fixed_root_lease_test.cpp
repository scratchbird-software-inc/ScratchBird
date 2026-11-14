// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// The fixed-root read source (R1 of the drift removal): a real native create,
// then the lease read straight from page zero's roots.
#include "native_fixed_root_memory_lease.hpp"
#include "native_creation_workspace.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
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
namespace mga = scratchbird::transaction::mga;
using namespace scratchbird::core::platform;
using E = db::NativeFixedRootMemoryError;
unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
Uuid Id(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}

struct MemoryFixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding{Id(1), Id(61), Id(62), Id(63)};
  db::NativeStorageMemory memory;
  static auto Policy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = 64u << 20;
    p.per_context_limit_bytes = 64u << 20;
    return p;
  }
  explicit MemoryFixture(u64 bytes) : manager(Policy()) {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = m::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "storage.fixed_root.conformance";
    r.purpose = "fixed-root read";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Id(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "fixed-root conformance";
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

struct Node {
  std::filesystem::path directory, path;
  d::FileDevice device;
  db::NativeFilespaceInitializationRequest request;
  u64 size = 0;
  explicit Node(unsigned profile) {
    char name[] = "/tmp/sb-fixed-root-XXXXXX";
    if (!mkdtemp(name)) throw std::runtime_error("mkdtemp");
    directory = name;
    path = directory / "node";
    const auto& p = d::kCanonicalFilespacePageProfiles[profile];
    size = p.page_size_bytes;
    request.bootstrap = {Id(1), Id(2), p.uuid, d::kNativeBootstrapIntegrityProfile, {}, p.page_size_bytes, 1, 0, 1, 7};
    request.operation_uuid = Id(3);
    request.writer_uuid = Id(4);
    request.policy_snapshot_uuid = Id(10);
    request.creator.transaction_uuid = {UuidKind::transaction, Id(5)};
    request.creator.local_id = mga::MakeLocalTransactionId(1);
    request.creator.scope = mga::TransactionScope::local_node;
    request.creation_utc_millis = 1789357072000ULL;
    request.total_pages = 64;
    Check(device.Open(path.string(), d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "node device");
    scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1), Id(10)}, {{}, 0, 1000});
    Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device, request, 256 * size, issuer).ok(), "real native create");
  }
  ~Node() {
    (void)device.Close();
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
  }
  std::vector<d::NativeFilespaceDevice> Devices() { return {{Id(2), request.bootstrap.page_size_profile_uuid, &device}}; }
};

// W4d (2026-11-08): no writer produces the legacy SBTINV01 inventory any more,
// and W4d-3 removed its decoder. A FORGERY over a production-created file: the
// in-place inventory page's family magic is overwritten with another family's
// (the removed SBTINV01 magic, the likeliest stale bytes), so the fixed-root
// lease must refuse an unknown inventory family; nothing decodes it.
void ForgeLegacyInventoryFamily(Node& n) {
  namespace pg = scratchbird::storage::page;
  constexpr std::array<byte, 8> legacy{{'S', 'B', 'T', 'I', 'N', 'V', '0', '1'}};
  std::vector<byte> bytes(n.size);
  unsigned forged = 0;
  for (u64 page = 1; page < n.request.total_pages; ++page) {
    const auto read = n.device.ReadAt(page * n.size, bytes.data(), bytes.size());
    Check(read.ok() && read.bytes_transferred == bytes.size(), "read a created page");
    if (!std::equal(pg::kTransactionStatePageMagic.begin(), pg::kTransactionStatePageMagic.end(),
                    bytes.begin() + pg::kTransactionStateFamilyOffset)) continue;
    std::copy(legacy.begin(), legacy.end(), bytes.begin() + pg::kTransactionStateFamilyOffset);
    const auto write = n.device.WriteAt(page * n.size, bytes.data(), bytes.size());
    Check(write.ok() && write.bytes_transferred == bytes.size(), "forge the legacy inventory family");
    ++forged;
  }
  Check(forged == 1 && n.device.Sync().ok(), "the create wrote exactly one in-place inventory page");
}

db::NativeFixedRootMemoryResult Acquire(Node& n, MemoryFixture& f, db::NativeFixedRootMemoryLimits limits,
                                       Uuid database = Id(1), Uuid primary = Id(2)) {
  const auto devices = n.Devices();
  return db::AcquireNativeFixedRootMemoryLease(database, devices, primary, limits, f.memory, f.binding);
}

// FORGERIES over a production-created file (W4d-3): one page rewritten, read,
// then restored byte for byte.
std::vector<byte> ReadPage(Node& n, u64 page) {
  std::vector<byte> bytes(n.size);
  const auto read = n.device.ReadAt(page * n.size, bytes.data(), bytes.size());
  Check(read.ok() && read.bytes_transferred == bytes.size(), "read a page to forge");
  return bytes;
}
void WritePage(Node& n, u64 page, const std::vector<byte>& bytes) {
  const auto write = n.device.WriteAt(page * n.size, bytes.data(), bytes.size());
  Check(write.ok() && write.bytes_transferred == bytes.size() && n.device.Sync().ok(), "write a forged or restored page");
}
// The page's common header with the payload-encrypted flag set; every other
// byte as the create wrote it.
std::vector<byte> EncryptedFlag(std::vector<byte> bytes) {
  const auto header = d::DecodeNativeCommonPageHeader(bytes.data(), d::kNativeCommonPageHeaderBytes);
  Check(header.ok() && !(header.header->flags & 1u), "a plaintext page as created");
  auto flagged = *header.header;
  flagged.flags |= 1u;
  const auto encoded = d::EncodeNativeCommonPageHeader(flagged);
  Check(encoded.ok(), "the flagged header encodes");
  std::copy(encoded.bytes->begin(), encoded.bytes->end(), bytes.begin());
  return bytes;
}
// Page zero with its kind-4 (inventory) root changed by `change`.
void ForgeInventoryRoot(Node& n, const auto& change) {
  auto zero = d::ReadFilespacePageZeroFromOpenDevice(n.device);
  Check(zero.ok(), "page zero for the inventory root forgery");
  auto bad = *zero.record;
  const auto at = std::find_if(bad.roots.begin(), bad.roots.end(), [](const auto& r) { return r.kind == 4; });
  Check(at != bad.roots.end(), "inventory root");
  change(*at);
  const auto encoded = d::EncodeFilespacePageZero(bad);
  Check(encoded.ok(), "forged page zero encodes");
  Check(n.device.WriteAt(0, encoded.bytes->data(), encoded.bytes->size()).ok() && n.device.Sync().ok(),
        "write the forged inventory root");
}

void TestProfile(unsigned profile) {
  Node n(profile);
  MemoryFixture f(32u << 20);
  const db::NativeFixedRootMemoryLimits limits{256 * n.size, 8u << 20};
  auto r = Acquire(n, f, limits);
  Check(r.ok(), "fixed-root lease on a freshly created database");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(n.device);
  Check(zero.ok(), "page zero");
  const auto& z = *zero.record;
  const auto& lease = *r.lease;
  const auto& id = lease.identity();
  Check(id.database_uuid == Id(1) && id.filespace_uuid == Id(2) && id.page_zero_uuid == z.page_uuid &&
            id.page_generation == z.page_generation && id.root_set_generation == z.root_set_generation &&
            id.total_pages == z.total_pages && id.creation_utc_millis == z.creation_utc_millis &&
            id.creation_operation_uuid == z.creation_operation_uuid &&
            id.page_size_bytes == z.bootstrap.page_size_bytes && !id.cluster_authority_required(),
        "the identity is page zero's own");
  const auto same = [](const d::FilespaceRootReference& a, const d::FilespaceRootReference& b) {
    return a.kind == b.kind && a.page_type == b.page_type && a.filespace_uuid == b.filespace_uuid &&
           a.page_number == b.page_number && a.page_generation == b.page_generation &&
           a.page_size_profile_uuid == b.page_size_profile_uuid && a.object_uuid == b.object_uuid;
  };
  Check(lease.roots().size() == z.roots.size() &&
            std::equal(lease.roots().begin(), lease.roots().end(), z.roots.begin(), same),
        "the roots are page zero's, as stored");
  for (const u16 kind : {u16{1}, u16{2}, u16{3}, u16{4}, u16{5}, u16{6}, u16{7}, u16{8}, u16{10}})
    Check(lease.Root(kind) && lease.Root(kind)->kind == kind, "every fixed root is present");
  const auto& inv = lease.inventory();
  Check(inv.next_local_transaction_id == 2 && inv.entries.size() == 1 && inv.entries[0].identity.local_id.value == 1 &&
            inv.entries[0].identity.transaction_uuid.value == Id(5) && mga::HasCommittedInventoryOutcome(inv.entries[0]),
        "the in-place inventory: transaction 1 committed, Next 2");
  Check(lease.inventory_pages().size() == 2 && lease.inventory_pages()[0].header.page_number == lease.Root(4)->page_number &&
            lease.inventory_pages()[0].object_uuid == lease.Root(4)->object_uuid &&
            lease.inventory_pages()[1].header.page_type == 0x301,
        "state page then header page, both of the inventory object");
  const u64 maps = profile ? 1 : 2;  // 64 pages: 60 per 8 KiB map
  Check(lease.allocation().pages.size() == maps && lease.directory().pages.size() == 1, "allocation and directory chains");
  Check(lease.retained_image_bytes() == (1 + 2 + maps + 1) * n.size && lease.backing_bytes_used() > 0,
        "page zero, two inventory pages, the maps and the directory are retained");
  // Two leases on one database see the same identity.
  MemoryFixture g(32u << 20);
  auto again = Acquire(n, g, limits);
  Check(again.ok() && again.lease->identity() == id, "the identity is stable across reads");
  // The exact backing boundary.
  const auto used = lease.backing_bytes_used();
  r.lease.Reset();
  again.lease.Reset();
  MemoryFixture exact(32u << 20);
  Check(Acquire(n, exact, {256 * n.size, used}).ok(), "exactly the backing used");
  MemoryFixture short_one(32u << 20);
  const auto refused = Acquire(n, short_one, {256 * n.size, used - 1});
  Check(!refused.ok() && refused.error == E::resource_exhausted && !refused.lease, "one byte less is refused");
}

void TestRefusals() {
  {
    Node legacy(0);
    ForgeLegacyInventoryFamily(legacy);
    MemoryFixture f(32u << 20);
    const auto r = Acquire(legacy, f, {256 * legacy.size, 8u << 20});
    Check(!r.ok() && r.error == E::inventory_failure && r.inventory_error == scratchbird::storage::page::NativeInventoryError::invalid_family &&
              !r.lease,
          "an unknown inventory family (the removed SBTINV01 magic) is refused, not decoded");
  }
  Node n(0);
  // The inventory's two pages, from a clean lease.
  u64 state_page = 0, header_page = 0;
  {
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    Check(r.ok() && r.lease->inventory_pages().size() == 2, "the created inventory's pages");
    state_page = r.lease->inventory_pages()[0].header.page_number;
    header_page = r.lease->inventory_pages()[1].header.page_number;
  }
  // An encrypted inventory page is never read as plaintext. The state page at
  // the root is refused on the probe as needing crypto authority. The header
  // page's decoder refuses any page flag as an invalid header before the
  // reader's own encryption check is reached, so that refusal reports an
  // invalid inventory instead (observed 2026-11-08; reported, not changed).
  for (const u64 page : {state_page, header_page}) {
    const auto original = ReadPage(n, page);
    WritePage(n, page, EncryptedFlag(original));
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    using IE = scratchbird::storage::page::NativeInventoryError;
    if (page == state_page)
      Check(!r.ok() && !r.lease && r.error == E::encrypted_requires_authority &&
                r.inventory_error == IE::encrypted_requires_crypto_authority,
            "an encrypted inventory state page is refused as needing crypto authority, not read as plaintext");
    else
      Check(!r.ok() && !r.lease && r.error == E::inventory_failure && r.inventory_error == IE::invalid_inventory,
            "an encrypted inventory header page is refused by its decoder, not read as plaintext");
    WritePage(n, page, original);
    MemoryFixture g(32u << 20);
    Check(Acquire(n, g, {256 * n.size, 8u << 20}).ok(), "the restored inventory reads again");
  }
  // The inventory root must name the page as created: its generation and its
  // object (the inventory pages' allocation owner).
  {
    const auto original = ReadPage(n, 0);
    ForgeInventoryRoot(n, [](d::FilespaceRootReference& root) { ++root.page_generation; });
    MemoryFixture f(32u << 20);
    auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    Check(!r.ok() && r.error == E::inventory_failure && !r.lease,
          "an inventory root naming another generation is refused");
    WritePage(n, 0, original);
    ForgeInventoryRoot(n, [](d::FilespaceRootReference& root) { root.object_uuid = Id(901); });
    MemoryFixture g(32u << 20);
    r = Acquire(n, g, {256 * n.size, 8u << 20});
    Check(!r.ok() && r.error == E::root_mismatch && r.root_kind == u16{4} && !r.lease,
          "an inventory root naming another object is refused, naming kind 4");
    WritePage(n, 0, original);
    MemoryFixture h(32u << 20);
    Check(Acquire(n, h, {256 * n.size, 8u << 20}).ok(), "restored page zero reads again");
  }
  // The device cohort: one device supplied twice, and a device whose page-size
  // profile is not the one page zero records.
  {
    MemoryFixture f(32u << 20);
    auto devices = n.Devices();
    devices.push_back(devices.front());
    const auto r = db::AcquireNativeFixedRootMemoryLease(Id(1), devices, Id(2), {256 * n.size, 8u << 20}, f.memory, f.binding);
    Check(!r.ok() && r.error == E::invalid_device && !r.lease, "the same device supplied twice is refused");
  }
  {
    MemoryFixture f(32u << 20);
    auto devices = n.Devices();
    for (const auto& other : d::kCanonicalFilespacePageProfiles)
      if (other.uuid != devices.front().page_size_profile_uuid) {
        devices.front().page_size_profile_uuid = other.uuid;
        break;
      }
    Check(devices.front().page_size_profile_uuid != n.request.bootstrap.page_size_profile_uuid, "another profile");
    const auto r = db::AcquireNativeFixedRootMemoryLease(Id(1), devices, Id(2), {256 * n.size, 8u << 20}, f.memory, f.binding);
    // Page zero is read with the supplied device's profile, so the refusal is
    // page zero's.
    Check(!r.ok() && !r.lease && r.error == E::page_zero_failure,
          "a device whose page-size profile is not page zero's is refused");
  }
  {
    MemoryFixture f(32u << 20);
    Check(Acquire(n, f, {256 * n.size, 8u << 20}, Id(1), Id(9)).error == E::invalid_device, "the primary must be supplied");
  }
  {
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20}, Id(7));
    Check(!r.ok() && r.error == E::memory_binding_failure, "memory bound to another database");
  }
  {
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {n.size, 8u << 20});
    Check(!r.ok() && r.error == E::resource_exhausted, "a one-page image budget is refused");
  }
  {
    MemoryFixture f(32u << 20);
    Check(Acquire(n, f, {0, 8u << 20}).error == E::invalid_request && Acquire(n, f, {256 * n.size, 0}).error == E::invalid_request,
          "zero limits are refused");
  }
  // Every read fails in turn; every corrupted read in turn.
  MemoryFixture base(32u << 20);
  track = true;
  reads = 0;
  auto ok = Acquire(n, base, {256 * n.size, 8u << 20});
  track = false;
  const unsigned total = reads;
  Check(ok.ok() && total > 4, "fault sweep baseline");
  ok.lease.Reset();
  for (unsigned at = 1; at <= total; ++at) {
    MemoryFixture f(32u << 20);
    track = true;
    reads = 0;
    fail_at = at;
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    track = false;
    fail_at = 0;
    Check(!r.ok() && !r.lease, "every failed read refuses without a lease");
  }
  for (unsigned at = 1; at <= total; ++at) {
    MemoryFixture f(32u << 20);
    track = true;
    reads = 0;
    corrupt_at = at;
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    track = false;
    corrupt_at = 0;
    if (r.ok()) std::cerr << "corrupted read " << at << " of " << total << " was accepted\n";
    Check(!r.ok() && !r.lease, "every corrupted read refuses without a lease");
  }
  // A root whose object is not the page's allocation owner.
  {
    auto zero = d::ReadFilespacePageZeroFromOpenDevice(n.device);
    Check(zero.ok(), "page zero for the misbound root");
    auto bad = *zero.record;
    const auto at = std::find_if(bad.roots.begin(), bad.roots.end(), [](const auto& r) { return r.kind == 6; });
    Check(at != bad.roots.end(), "configuration root");
    at->object_uuid = Id(900);
    const auto encoded = d::EncodeFilespacePageZero(bad);
    Check(encoded.ok(), "misbound page zero encodes");
    std::vector<byte> original(n.size);
    Check(n.device.ReadAt(0, original.data(), original.size()).ok(), "keep page zero");
    Check(n.device.WriteAt(0, encoded.bytes->data(), encoded.bytes->size()).ok() && n.device.Sync().ok(), "write misbound root");
    MemoryFixture f(32u << 20);
    const auto r = Acquire(n, f, {256 * n.size, 8u << 20});
    Check(!r.ok() && r.error == E::root_mismatch && r.root_kind == u16{6}, "a misbound root is refused, naming its kind");
    Check(n.device.WriteAt(0, original.data(), original.size()).ok() && n.device.Sync().ok(), "restore page zero");
    MemoryFixture g(32u << 20);
    Check(Acquire(n, g, {256 * n.size, 8u << 20}).ok(), "restored page zero reads again");
  }
}
}  // namespace

// The backed lease (no memory grant; the create's self-check) is the same
// acquisition over a caller backing: it describes the file exactly as the
// governed lease does, uses only the backing it is given, refuses a backing one
// byte short, and refuses every failed or corrupted read the same way.
void TestBacked(unsigned profile) {
  Node n(profile);
  MemoryFixture f(32u << 20);
  const db::NativeFixedRootMemoryLimits limits{256 * n.size, 8u << 20};
  auto governed = Acquire(n, f, limits);
  Check(governed.ok(), "governed lease for the backed comparison");
  const auto& g = *governed.lease;
  const auto devices = n.Devices();
  std::vector<byte> backing(limits.maximum_backing_bytes);
  auto backed = db::AcquireNativeFixedRootBackedLease(Id(1), devices, Id(2), limits, backing);
  Check(backed.ok(), "backed lease over the same database");
  const auto& b = *backed.lease;
  const auto same = [](const d::FilespaceRootReference& x, const d::FilespaceRootReference& y) {
    return x.kind == y.kind && x.page_type == y.page_type && x.filespace_uuid == y.filespace_uuid &&
           x.page_number == y.page_number && x.page_generation == y.page_generation &&
           x.page_size_profile_uuid == y.page_size_profile_uuid && x.object_uuid == y.object_uuid;
  };
  Check(b.identity() == g.identity() && b.roots().size() == g.roots().size() &&
            std::equal(b.roots().begin(), b.roots().end(), g.roots().begin(), same),
        "the backed lease has the governed lease's identity and roots");
  Check(b.inventory().next_local_transaction_id == g.inventory().next_local_transaction_id &&
            b.inventory().next_commit_sequence == g.inventory().next_commit_sequence &&
            b.inventory().entries.size() == g.inventory().entries.size() &&
            b.inventory_pages().size() == g.inventory_pages().size() &&
            b.allocation().pages.size() == g.allocation().pages.size() &&
            b.directory().pages.size() == g.directory().pages.size() &&
            b.retained_image_bytes() == g.retained_image_bytes() && b.backing_bytes_used() == g.backing_bytes_used(),
        "the backed lease reads the same inventory, allocation and directory with the same bytes");
  const auto used = b.backing_bytes_used();
  const auto* first = backing.data();
  Check(used <= backing.size() && b.allocation().pages.front().image.data() >= first &&
            b.allocation().pages.front().image.data() < first + backing.size(),
        "the backed lease keeps its images in the caller's backing");
  governed.lease.Reset();
  backed.lease.Reset();
  {
    std::vector<byte> exact(used);
    Check(db::AcquireNativeFixedRootBackedLease(Id(1), devices, Id(2), {256 * n.size, used}, exact).ok(),
          "exactly the backing used");
    std::vector<byte> short_one(used - 1);
    reads = 0;
    track = true;
    const auto refused = db::AcquireNativeFixedRootBackedLease(Id(1), devices, Id(2), {256 * n.size, used}, short_one);
    track = false;
    Check(!refused.ok() && refused.error == E::resource_exhausted && !refused.lease && !reads,
          "a backing one byte short of the limit is refused before any read");
  }
  // Every read failed, then every read corrupted, through the backed lease.
  reads = 0;
  track = true;
  Check(db::AcquireNativeFixedRootBackedLease(Id(1), devices, Id(2), limits, backing).ok(), "count the backed reads");
  track = false;
  const unsigned total = reads;
  Check(total > 0, "the backed lease reads the file");
  for (unsigned at = 1; at <= total; ++at)
    for (bool corrupt : {false, true}) {
      reads = 0;
      (corrupt ? corrupt_at : fail_at) = at;
      track = true;
      const auto r = db::AcquireNativeFixedRootBackedLease(Id(1), devices, Id(2), limits, backing);
      track = false;
      fail_at = corrupt_at = 0;
      Check(!r.ok() && !r.lease && reads >= at, corrupt ? "every corrupted backed read is refused"
                                                        : "every failed backed read is refused");
      if (!corrupt) Check(r.error == E::io_failure, "a failed read is an io_failure");
    }
  Check(db::AcquireNativeFixedRootBackedLease(Id(1), devices, Id(2), limits, backing).ok(),
        "a clean backed read after the sweep");
}

int main() {
  try {
    for (unsigned profile = 0; profile < 5; ++profile) TestProfile(profile);
    for (unsigned profile = 0; profile < 5; ++profile) TestBacked(profile);
    TestRefusals();
  } catch (const std::exception& e) {
    std::cerr << "native fixed root FAIL " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "native fixed root: " << checks << " checks passed\n";
  return 0;
}
