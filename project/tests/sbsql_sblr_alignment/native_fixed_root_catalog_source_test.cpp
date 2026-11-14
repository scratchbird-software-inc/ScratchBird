// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_FIXED_ROOT_READ
// A native catalog read that starts from page zero's fixed roots (drift
// removal R3b1). On a database written by the production create, which still
// carries its genesis checkpoint until R3b3, a committed catalog read from page
// zero returned exactly what the same read from the checkpoint returned (R3b1);
// since R3b3 the create writes no checkpoint and the read is checked against
// independent reads of the same file. Then every read the page-zero read makes is failed in turn and
// corrupted in turn, the image allowance is held to the byte, and page zero or
// catalog roots that do not bind are refused.
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "physical_mga_cow_store.hpp"
#include "filespace_page_zero.hpp"
#include "catalog_page.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
bool track = false;
unsigned reads = 0, fail_at = 0, corrupt_at = 0;
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
namespace pg = scratchbird::storage::page;
namespace u = scratchbird::core::uuid;
using namespace scratchbird::core::platform;
using RE = db::NativeCheckpointCatalogRelationError;
unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " line=" + std::to_string(at.line()));
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
scratchbird::tests::OwnedTempDirectory* artifacts = nullptr;

struct Created {
  std::string path;
  db::NativeDatabaseCreateReceipt receipt;
  u64 size = 0;
  Uuid profile;
};
const Created& Production(unsigned profile_index) {
  static std::map<unsigned, Created> made;
  if (const auto at = made.find(profile_index); at != made.end()) return at->second;
  const auto& p = d::kCanonicalFilespacePageProfiles[profile_index];
  Created out;
  out.size = p.page_size_bytes;
  out.profile = p.uuid;
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
  out.path = (artifacts->path() / ("created-" + std::to_string(profile_index))).string();
  d::FileDevice device;
  Check(device.Open(out.path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "production create device");
  u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
  auto result = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
  Check(result.ok(), "the production create writes a complete native database");
  out.receipt = *result.receipt;
  Check(device.Close().ok(), "release the creating handle");
  return made.emplace(profile_index, std::move(out)).first->second;
}

// A read-only open of a database file (or of a copy changed for a negative).
struct Open {
  d::FileDevice device;
  std::vector<d::NativeFilespaceDevice> devices;
  Open(const Created& made, const std::string& path) {
    Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "open the database file read-only");
    devices = {{made.receipt.filespace_uuid, made.profile, &device}};
  }
};
db::NativeCatalogRelationBinding Binding(const Created& made, unsigned role) {
  return {made.receipt.workspace.relation_uuids[role - 1], made.receipt.workspace.index_dependencies[role - 1]};
}
// The whole file: the largest relation (2,118 rows at 8 KiB) needs more than
// 256 pages of images.
u64 Budget(const Created& made) { return (made.size == 8192 ? 1024 : 512) * made.size; }
db::NativeCommittedCatalogReadResult Read(const Created& made, Open& o, const db::NativeCatalogSource& source,
                                          u16 selector, unsigned role, u64 budget) {
  return db::ReadNativeCommittedCatalogVersionsFromOpenDevices(made.receipt.database_uuid, o.devices, source, selector,
                                                               static_cast<u16>(role), Binding(made, role), budget);
}
db::NativeCatalogSource PageZero(const Created& made) { return db::NativeCatalogSource::PageZero(made.receipt.filespace_uuid); }

// Drift removal R3b3: the production create writes no checkpoint, so the
// page-zero read is checked against independent reads of the same file. Every
// relation of both catalogs reads exactly the receipt's committed rows; the
// feature catalog (the catalog root at create) reads what the catalog reads;
// every retained leaf image is byte-identical to that page read straight from
// the file; and every row was created by the construction transaction, the
// only one in the inventory. (Until R3b3 this compared against the genesis
// checkpoint; all 12 relations agreed byte for byte at every page size.)
void PageZeroReads(unsigned profile) {
  const auto& made = Production(profile);
  // W4d: the receipt no longer has a checkpoint field at all (the checkpoint
  // graph was deleted), so "a production create carries no checkpoint" is now
  // a property of the type, checked at compile time.
  static_assert(![]<class R>() { return requires(const R& r) { r.checkpoint; }; }
                     .template operator()<db::NativeCreationWorkspaceReceipt>(),
                "a production create carries no checkpoint");
  Open o(made, made.path);
  unsigned compared = 0;
  for (unsigned role = 1; role <= 6; ++role) {
    const auto catalog = Read(made, o, PageZero(made), 2, role, Budget(made));
    const auto feature = Read(made, o, PageZero(made), 8, role, Budget(made));
    if (!catalog.ok())
      std::cerr << "profile " << profile << " role " << role << " not read: error=" << int(catalog.error)
                << " source=" << int(catalog.source.error) << " relation=" << int(catalog.source.relation.error) << '\n';
    Check(catalog.ok() && feature.ok() && catalog.source.page_zero_source && feature.source.page_zero_source,
          "every relation of both catalogs reads from page zero");
    compared += 2;
    Check(catalog.rows.size() == made.receipt.workspace.catalog_rows[role - 1] && feature.rows.size() == catalog.rows.size(),
          "exactly the receipt's committed rows, through either catalog root");
    std::set<Uuid> versions;
    for (std::size_t i = 0; i < catalog.rows.size(); ++i) {
      Check(versions.insert(catalog.rows[i].version_uuid).second, "every committed version is distinct");
      Check(catalog.rows[i].version_uuid == feature.rows[i].version_uuid &&
                catalog.rows[i].metadata.record.payload == feature.rows[i].metadata.record.payload &&
                !catalog.rows[i].provisional,
            "the feature catalog reads the catalog's committed rows");
    }
    Check(catalog.source.relation.catalogs.size() == feature.source.relation.catalogs.size(), "the same relation images");
    for (const auto& image : catalog.source.relation.catalogs) {
      std::vector<byte> page(made.size);
      const auto io = o.device.ReadAt(image.page->header.page_number * made.size, page.data(), page.size());
      Check(io.ok() && io.bytes_transferred == page.size() && page == image.bytes,
            "every retained leaf image is the page as stored");
    }
    const auto& inventory = catalog.source.inventory();
    Check(inventory.next_local_transaction_id == 2 && inventory.entries.size() == 1 &&
              inventory.entries[0].identity.local_id.value == 1 &&
              inventory.entries[0].identity.transaction_uuid.value == made.receipt.construction_transaction.transaction_uuid.value &&
              !catalog.source.cluster_authority_required(),
          "the inventory holds only the construction transaction; no cluster authority");
    for (const auto& creator : catalog.source.row_creators)
      Check(creator.inventory_entry_index == 0, "every row was created by the construction transaction");
  }
  Check(compared == 12, "every relation of both catalogs was read, none skipped");
}

// Every read of a page-zero committed read, failed then corrupted.
void Sweeps(unsigned profile) {
  const auto& made = Production(profile);
  Open o(made, made.path);
  reads = 0;
  track = true;
  const auto clean = Read(made, o, PageZero(made), 2, 1, Budget(made));
  track = false;
  const unsigned total = reads;
  Check(clean.ok() && total > 0, "a clean page-zero read, counted");
  for (unsigned at = 1; at <= total; ++at)
    for (bool corrupt : {false, true}) {
      reads = 0;
      (corrupt ? corrupt_at : fail_at) = at;
      track = true;
      const auto r = Read(made, o, PageZero(made), 2, 1, Budget(made));
      track = false;
      fail_at = corrupt_at = 0;
      Check(!r.ok() && r.rows.empty() && reads >= at,
            corrupt ? "every corrupted page-zero read is refused with no rows" : "every failed page-zero read is refused with no rows");
    }
  Check(Read(made, o, PageZero(made), 2, 1, Budget(made)).ok(), "a clean read after the sweep");
  std::cout << "profile " << profile << ": page-zero catalog read " << total << " reads failed and corrupted\n";
}

// The image allowance holds to the byte.
void AllowanceToTheByte(unsigned profile) {
  const auto& made = Production(profile);
  Open o(made, made.path);
  const auto full = Read(made, o, PageZero(made), 2, 1, Budget(made));
  Check(full.ok() && full.source.retained_image_bytes > 0, "the page-zero read reports its retained images");
  const auto needed = full.source.retained_image_bytes;
  const auto short_one = Read(made, o, PageZero(made), 2, 1, needed - 1);
  Check(!short_one.ok() && short_one.rows.empty(), "an allowance one byte short of the retained images is refused");
}

// A copy of the database with page zero or a catalog root changed.
std::string Copy(const Created& made, std::string_view name) {
  const auto path = (artifacts->path() / (std::string(name) + "-" + std::to_string(made.size))).string();
  std::filesystem::copy_file(made.path, path, std::filesystem::copy_options::overwrite_existing);
  return path;
}
d::FilespacePageZero ReadZero(const std::string& path) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "open for page zero");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "decode page zero");
  auto out = *zero.record;
  Check(device.Close().ok(), "close");
  return out;
}
void WriteAt(const std::string& path, u64 offset, const std::vector<byte>& bytes) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open for a negative");
  const auto io = device.WriteAt(offset, bytes.data(), bytes.size());
  Check(io.ok() && io.bytes_transferred == bytes.size() && device.Sync().ok() && device.Close().ok(), "write the change");
}
void WriteZero(const std::string& path, const d::FilespacePageZero& zero) {
  const auto encoded = d::EncodeFilespacePageZero(zero);
  Check(encoded.ok(), "re-encode page zero (resealed)");
  WriteAt(path, 0, *encoded.bytes);
}
void Negatives(unsigned profile) {
  const auto& made = Production(profile);
  const auto refused = [&](const std::string& path, std::string_view why) {
    Open o(made, path);
    const auto r = Read(made, o, PageZero(made), 2, 1, Budget(made));
    Check(!r.ok() && r.rows.empty() && r.source.page_zero_source, why);
    return r;
  };
  {
    // An online page zero must name root kinds 1-8 (owner ruling 2026-11-07),
    // so a page zero without this root cannot even be written: the codec
    // refuses it before any file exists.
    auto zero = ReadZero(made.path);
    std::erase_if(zero.roots, [](const auto& r) { return r.kind == 8; });
    const auto encoded = d::EncodeFilespacePageZero(zero);
    Check(!encoded.ok() && encoded.error == d::FilespacePageZeroError::required_root_missing,
          "page zero without the feature catalog root is refused: an online page zero cannot omit it");
  }
  {
    // An online page zero must name root kinds 1-8 (owner ruling 2026-11-07),
    // so a page zero without this root cannot even be written: the codec
    // refuses it before any file exists.
    auto zero = ReadZero(made.path);
    std::erase_if(zero.roots, [](const auto& r) { return r.kind == 2; });
    const auto encoded = d::EncodeFilespacePageZero(zero);
    Check(!encoded.ok() && encoded.error == d::FilespacePageZeroError::required_root_missing,
          "page zero without the catalog root is refused: an online page zero cannot omit it");
  }
  {
    auto zero = ReadZero(made.path);
    for (auto& r : zero.roots)
      if (r.kind == 2 || r.kind == 8) ++r.page_generation;
    const auto path = Copy(made, "generation");
    WriteZero(path, zero);
    const auto r = refused(path, "a catalog root reference with the wrong generation is refused");
    Check(r.source.error == RE::fixed_root_failure || r.source.error == RE::io_failure ||
              r.source.error == RE::creator_mismatch,
          "the generation finding is typed");
  }
  {
    auto zero = ReadZero(made.path);
    for (auto& r : zero.roots)
      if (r.kind == 2 || r.kind == 8) r.object_uuid = Fixed(4001);
    const auto path = Copy(made, "owner");
    WriteZero(path, zero);
    const auto r = refused(path, "a catalog root reference naming another owner is refused");
    Check(r.source.error == RE::fixed_root_failure && r.source.fixed_root_kind &&
              (*r.source.fixed_root_kind == 2 || *r.source.fixed_root_kind == 8),
          "the owner finding names the catalog root");
  }
  {
    // A well-sealed catalog root image whose page identity is not the one the
    // allocation map records for that page.
    const auto zero = ReadZero(made.path);
    const auto root = std::find_if(zero.roots.begin(), zero.roots.end(), [](const auto& r) { return r.kind == 2; });
    Check(root != zero.roots.end(), "the catalog root reference");
    d::FileDevice device;
    Check(device.Open(made.path, d::FileOpenMode::open_existing_read_only).ok(), "open for the catalog root");
    const auto image = pg::ReadNativeCatalogRootFromOpenDevice(device, made.receipt.database_uuid, *root);
    Check(image.ok() && device.Close().ok(), "the stored catalog root");
    auto changed = *image.root;
    changed.header.page_uuid = Fixed(4002);
    const auto encoded = pg::EncodeNativeCatalogRoot(changed);
    Check(encoded.ok(), "re-encode the catalog root with another page identity (resealed)");
    const auto path = Copy(made, "page-uuid");
    WriteAt(path, root->page_number * made.size, encoded.bytes);
    const auto r = refused(path, "a catalog root whose page identity differs from its allocation record is refused");
    Check(r.source.error == RE::fixed_root_failure && r.source.fixed_root_kind && *r.source.fixed_root_kind == 2,
          "the page identity finding names the catalog root");
  }
}
}  // namespace

int main() {
  try {
    scratchbird::tests::OwnedTempDirectory owned;
    artifacts = &owned;
    for (unsigned p = 0; p < 5; ++p) {
      PageZeroReads(p);
      Sweeps(p);
      AllowanceToTheByte(p);
      Negatives(p);
    }
    owned.Cleanup();
    artifacts = nullptr;
    std::cout << "native fixed root catalog source PASS checks=" << checks << " production_create=true\n";
    return 0;
  } catch (const std::exception& e) {
    track = false;
    fail_at = corrupt_at = 0;
    std::cerr << "native fixed root catalog source FAIL " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
