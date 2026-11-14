// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_ALLOCATION_MAP_IN_PLACE
// Allocation from the in-place allocation map by careful write, on a database
// written by the production create, one page size per run (--profile N).
// Every map state is compared with an independent oracle that decodes the map
// pages itself (states and every record, including crash leftovers) rather
// than through the codec under test, and every file is read back by the chain
// reader and the production fixed-root lease.
//   - Rules: refused requests write nothing; one allocation per plan; a map
//     root of any other family is refused for allocation (a forgery: W4d-3b
//     removed the sealed family, the only other one).
//   - Exhaustion: every free slot is allocated, lowest first; the next
//     allocation is refused filespace_full with no write; every allocation is
//     two writes, record then state, even when they share a unit (owner Q76 =
//     1, 2026-11-13: that unit is written twice, the record first and
//     durable); page zero's free count is not kept or compared (owner Q22 =
//     1).
//   - Q76: at every page size with slots whose record shares the state's unit
//     and lies past its half-way point, both writes of such an allocation torn
//     at their half-way point leave a valid prefix that the readers open.
//   - Process loss after every write of a scripted sequence (fork + _exit),
//     and every write torn at its half-way point: the file is exactly the
//     oracle's write prefix (a torn unit is old or new); the readers admit it;
//     a new process allocates the lowest free slot, overwriting a leftover
//     record; nothing outside the map pages changes.
//   - Every read, write and sync failed in turn: a failed read writes
//     nothing; a failed write or sync leaves exactly the prefix of the writes
//     performed; the next allocation proceeds from that state.
// Limit: process death and I/O errors only, not power loss or reordering.
#include "native_database_create.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_creation_workspace.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "allocation_map_writer.hpp"
#include "careful_write.hpp"
#include "uuid.hpp"
#include "../support/careful_write_crash_sink.hpp"
#include "../support/owned_temp_directory.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
namespace mga = scratchbird::transaction::mga;
namespace u = scratchbird::core::uuid;
namespace fs = std::filesystem;
using namespace scratchbird::core::platform;
using AW = pg::AllocationWriteError;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
constexpr int kPlannedCrash = 86, kChildCompleted = 70, kChildFailed = 71, kChildException = 73;
constexpr u32 kNoCrash = ~u32{0};

Uuid V7(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[13] = static_cast<byte>(n >> 16);
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

std::vector<byte> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  Check(bool(in), "open the file to read it back");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void WriteAll(const std::string& path, std::span<const byte> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(bool(out.flush()), "write the file");
}

// ---- The independent map model ------------------------------------------------
// Layout, written out here: a map page's states start at 512, 4 bits per
// slot; records start at roundup128(512 + ceil(N/2)), 128 bytes per slot; the
// family header carries N at +12 and the first page at +48 (from offset 128);
// the next map page is the u64 at 256.
struct Slot {
  byte state = 0;
  std::array<byte, 128> record{};  // all zero = empty
  bool operator==(const Slot&) const = default;
};
u64 Load64(const byte* p) {
  u64 v = 0;
  for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
  return v;
}
u32 Load32(const byte* p) {
  u32 v = 0;
  for (int i = 3; i >= 0; --i) v = v << 8 | p[i];
  return v;
}
struct MapModel {
  std::map<u64, Slot> slots;   // by page number
  std::vector<u64> map_pages;  // in chain order
  bool operator==(const MapModel& o) const { return slots == o.slots && map_pages == o.map_pages; }
};
MapModel ModelOf(std::span<const byte> file, u32 size, u64 first_map_page) {
  MapModel m;
  for (u64 page = first_map_page; page;) {
    const auto* p = file.data() + page * size;
    m.map_pages.push_back(page);
    const u32 n = Load32(p + 128 + 12);
    const u64 first = Load64(p + 128 + 48);
    const u64 records_at = (512 + (n + 1) / 2 + 127) / 128 * 128;
    for (u32 i = 0; i < n; ++i) {
      Slot s;
      s.state = static_cast<byte>((p[512 + i / 2] >> (4 * (i % 2))) & 15);
      std::copy_n(p + records_at + 128 * i, 128, s.record.begin());
      m.slots[first + i] = s;
    }
    page = Load64(p + 256);
    Check(m.map_pages.size() < 100000, "the map chain ends");
  }
  return m;
}
std::array<byte, 128> RecordBytes(const pg::NativeAllocationRecord& r) {
  std::array<byte, 128> b{};
  const auto put64 = [&](std::size_t at, u64 v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<byte>(v >> (8 * i));
  };
  const auto put_uuid = [&](std::size_t at, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), b.begin() + at); };
  put64(0, r.page_number);
  put_uuid(8, r.allocation_uuid);
  put_uuid(24, r.page_uuid);
  put_uuid(40, r.owner_uuid);
  put_uuid(56, r.creator_transaction_uuid);
  put64(72, r.creator_local_transaction_id);
  put64(80, r.page_generation);
  put64(88, r.reuse_horizon);
  for (int i = 0; i < 4; ++i) b[96 + i] = static_cast<byte>(r.page_type >> (8 * i));
  put_uuid(100, r.creator_operation_uuid);
  return b;
}
// Is a slot's record in the same 512-byte unit as its state?
bool SameUnit(u64 page, const MapModel& m, std::span<const byte> file, u32 size) {
  for (const auto map_page : m.map_pages) {
    const auto* p = file.data() + map_page * size;
    const u32 n = Load32(p + 128 + 12);
    const u64 first = Load64(p + 128 + 48);
    if (page < first || page >= first + n) continue;
    const u64 i = page - first, records_at = (512 + (n + 1) / 2 + 127) / 128 * 128;
    return (records_at + 128 * i) / 512 == (512 + i / 2) / 512;
  }
  Check(false, "page covered by the map");
  return false;
}

// One allocation of the script, and the oracle's steps for it.
struct Alloc {
  u32 type = 6;
  unsigned id = 0;  // identities V7(id), V7(id + 1), V7(id + 2)
  bool system = false;
};
pg::PageAllocationRequest RequestOf(const Alloc& a) {
  pg::PageAllocationRequest r;
  r.page_type = a.type;
  r.owner_uuid = V7(a.id);
  r.page_uuid = V7(a.id + 1);
  r.allocation_uuid = V7(a.id + 2);
  if (!a.system) {
    r.creator_transaction_uuid = V7(a.id + 3);
    r.creator_local_transaction_id = 2;
  }
  return r;
}
// The lowest free slot gets the record (step 1) and then the state (step 2),
// two steps even when they share a unit (owner Q76 = 1).
using Step = std::function<void(MapModel&)>;
void AddSteps(std::vector<Step>& out, const Alloc& a, const MapModel& model, std::span<const byte> file, u32 size) {
  u64 page = 0;
  for (const auto& [n, s] : model.slots)
    if (s.state == 0) {
      page = n;
      break;
    }
  Check(page != 0, "the oracle finds a free slot");
  const auto request = RequestOf(a);
  pg::NativeAllocationRecord r;
  r.page_number = page;
  r.allocation_uuid = request.allocation_uuid;
  r.page_uuid = request.page_uuid;
  r.owner_uuid = request.owner_uuid;
  r.creator_transaction_uuid = request.creator_transaction_uuid;
  r.creator_local_transaction_id = request.creator_local_transaction_id;
  r.page_generation = 1;
  r.page_type = request.page_type;
  const auto bytes = RecordBytes(r);
  (void)file;
  (void)size;
  // Q76: was one combined step when SameUnit; now always record, then state.
  out.push_back([page, bytes](MapModel& m) { m.slots[page].record = bytes; });
  out.push_back([page](MapModel& m) { m.slots[page].state = 2; });
}
u64 LowestFree(const MapModel& model) {
  for (const auto& [n, s] : model.slots)
    if (s.state == 0) return n;
  return 0;
}
// Steps for a sequence of allocations from a starting model.
std::vector<Step> Steps(std::span<const Alloc> allocs, MapModel model, std::span<const byte> file, u32 size) {
  std::vector<Step> out;
  for (const auto& a : allocs) {
    const auto before = out.size();
    AddSteps(out, a, model, file, size);
    for (auto i = before; i < out.size(); ++i) out[i](model);
  }
  return out;
}
MapModel Prefix(MapModel m, const std::vector<Step>& steps, std::size_t k) {
  for (std::size_t i = 0; i < k; ++i) steps[i](m);
  return m;
}
std::vector<Alloc> Script() {
  return {{6, 1000, false}, {0x301, 1010, true}, {6, 1020, false}, {5, 1030, false}, {0x301, 1040, true},
          {6, 1050, false}};
}

// ---- The production file ---------------------------------------------------
struct Base {
  scratchbird::tests::OwnedTempDirectory dir;
  std::string path;
  u32 size = 0;
  Uuid database, primary, profile;
  d::FilespaceRootReference root;  // kind 3
  std::vector<byte> bytes;
  explicit Base(unsigned index) {
    const auto& p = d::kCanonicalFilespacePageProfiles[index];
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
    r.engine_uuid = V7(4);
    r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
    r.bootstrap_principal_name = "BootstrapAdmin";
    r.bootstrap_credential_fingerprint = kFingerprint;
    r.service_principal_name = "NodeService";
    r.service_credential_fingerprint = kFingerprint;
    r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
    path = (dir.path() / "created").string();
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "node device");
      u::StandaloneUuidV7Issuer issuer({V7(1), V7(10)}, {{}, 0, 1000});
      auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
      Check(made.ok(), "the production create writes a complete native database");
      database = made.receipt->database_uuid;
      primary = made.receipt->filespace_uuid;
      Check(device.Close().ok(), "close");
    }
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "read-only open");
    const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok(), "page zero");
    const auto at = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [](const auto& x) { return x.kind == 3; });
    Check(at != zero.record->roots.end(), "page zero names the allocation map");
    root = *at;
    Check(device.Close().ok(), "close");
    bytes = ReadAll(path);
  }
  pg::AllocationMapBinding Binding() const { return {database, root}; }
  std::string Fresh(const std::string& name) const {
    const auto p = (dir.path() / name).string();
    WriteAll(p, bytes);
    return p;
  }
  MapModel Model(std::span<const byte> file) const { return ModelOf(file, size, root.page_number); }
  // Every byte outside the map pages is the created file's.
  void Untouched(std::span<const byte> file, const MapModel& m, std::string_view label) const {
    Check(file.size() == bytes.size(), std::string(label) + ": the file keeps its size");
    for (u64 page = 0; page < file.size() / size; ++page) {
      if (std::find(m.map_pages.begin(), m.map_pages.end(), page) != m.map_pages.end()) continue;
      if (!std::equal(file.begin() + page * size, file.begin() + (page + 1) * size, bytes.begin() + page * size))
        Check(false, std::string(label) + ": page " + std::to_string(page) + " outside the map changed");
    }
  }
  // The chain reader and the fixed-root lease read the file; page zero's free
  // count is the create's, not compared.
  void Readers(const std::string& file, std::string_view label) const {
    d::FileDevice device;
    Check(device.Open(file, d::FileOpenMode::open_existing_read_only).ok(), std::string(label) + ": read-only open");
    const auto chain = pg::ReadNativeAllocationChainFromOpenDevice(device, {database, primary, profile}, 64u << 20);
    if (!chain.ok()) std::cerr << label << " chain error=" << int(chain.error) << '\n';
    Check(chain.ok(), std::string(label) + ": the chain reader admits the map");
    {
      const std::vector<d::NativeFilespaceDevice> files{{primary, profile, &device}};
      static std::vector<byte> backing(64u << 20);
      auto fixed = db::AcquireNativeFixedRootBackedLease(database, files, primary, {256ull * size, backing.size()}, backing);
      Check(fixed.ok(), std::string(label) + ": the fixed-root lease admits the file");
      fixed.lease.Reset();
    }
    Check(device.Close().ok(), "close");
  }
};

pg::PageAllocationResult Allocate(d::CarefulWriteSink& sink, const Base& b, const Alloc& a) {
  return pg::AllocatePage(sink, b.Binding(), RequestOf(a));
}

// ---- Rules -----------------------------------------------------------------------
void Rules(const Base& b) {
  const auto path = b.Fresh("rules.db");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
  {
    d::FileDeviceCarefulWriteSink sink(device);
    const auto before = ReadAll(path);
    auto bad = RequestOf({6, 2000, false});
    bad.page_type = 0;
    Check(pg::AllocatePage(sink, b.Binding(), bad).error == AW::invalid_request, "page type 0 refused");
    bad = RequestOf({6, 2000, false});
    bad.page_uuid.bytes[6] = 0x40;
    Check(pg::AllocatePage(sink, b.Binding(), bad).error == AW::invalid_request, "a page id that is not a UUIDv7 refused");
    bad = RequestOf({6, 2000, true});
    bad.creator_local_transaction_id = 3;
    Check(pg::AllocatePage(sink, b.Binding(), bad).error == AW::invalid_request, "a local number without a transaction refused");
    auto binding = b.Binding();
    binding.allocation_root.kind = 4;
    Check(pg::AllocatePage(sink, binding, RequestOf({6, 2000, false})).error == AW::invalid_request, "a non-map root refused");
    Check(ReadAll(path) == before, "no refused request writes anything");
    // One allocation per plan.
    auto loaded = pg::AllocationMapChain::Load(sink, b.Binding());
    Check(loaded.ok(), "load the chain");
    d::CarefulWritePlan plan;
    const auto first = loaded.chain->PlanAllocate(plan, RequestOf({6, 2010, false}));
    const auto second = loaded.chain->PlanAllocate(plan, RequestOf({6, 2020, false}));
    Check(first.ok() && first.map_writes == 2 && !second.ok(), "one allocation per plan, two map writes (Q76)");
    Check(ReadAll(path) == before, "planning writes nothing");
  }
  Check(device.Close().ok(), "close");
  fs::remove(path);
  // A map root of any other family is never allocated from in place. W4d-3b
  // removed the sealed family, the only other one, so this is a FORGERY over a
  // production-created file: the map root's family magic is overwritten with
  // the removed sealed family's (the likeliest stale bytes); nothing else
  // changes.
  {
    const auto forged_path = b.Fresh("unknown-family.db");
    d::FileDevice forged;
    Check(forged.Open(forged_path, d::FileOpenMode::open_existing).ok(), "forgery device");
    std::vector<byte> image(b.size);
    const auto read = forged.ReadAt(b.root.page_number * b.size, image.data(), image.size());
    Check(read.ok() && read.bytes_transferred == image.size() && pg::IsAllocationMapInplaceImage(image),
          "the created map root is in place");
    constexpr std::array<byte, 8> removed{{'S', 'B', 'A', 'B', 'M', '0', '0', '2'}};
    std::copy(removed.begin(), removed.end(), image.begin() + 128);
    const auto io = forged.WriteAt(b.root.page_number * b.size, image.data(), image.size());
    Check(io.ok() && io.bytes_transferred == image.size() && forged.Sync().ok(), "forge the map root's family");
    const auto before = ReadAll(forged_path);
    {
      d::FileDeviceCarefulWriteSink sink(forged);
      const auto r = pg::AllocatePage(sink, b.Binding(), RequestOf({6, 2030, false}));
      Check(r.error == AW::invalid_map && r.map_error == pg::NativeAllocationError::invalid_family && r.writes == 0,
            "a map root of an unknown family is refused for in-place allocation");
    }
    Check(forged.Close().ok() && ReadAll(forged_path) == before, "and nothing is written");
    fs::remove(forged_path);
  }
}

// ---- Exhaustion ----------------------------------------------------------------------
void Exhaustion(const Base& b) {
  const auto path = b.Fresh("full.db");
  const MapModel base = b.Model(b.bytes);
  u64 free = 0;
  for (const auto& [n, s] : base.slots) free += s.state == 0;
  Check(free > 0, "the create leaves free pages");
  MapModel model = base;
  u64 shared = 0, split = 0;  // Q76: was merged (one write) vs split
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
  {
    d::FileDeviceCarefulWriteSink sink(device);
    const bool forced = device.forced_ordered_writes();
    for (u64 i = 0; i < free; ++i) {
      const Alloc a{i % 2 ? 6u : 0x301u, 10000 + static_cast<unsigned>(i) * 4, i % 3 == 0};
      std::vector<Step> steps;
      const bool same = SameUnit(LowestFree(model), model, b.bytes, b.size);
      AddSteps(steps, a, model, b.bytes, b.size);
      for (const auto& s : steps) s(model);
      const auto r = Allocate(sink, b, a);
      // Q76: a shared unit's record is its own plan, so its two writes each
      // end with a Sync whatever the sink's ordering.
      Check(r.ok() && steps.size() == 2 && r.writes == 2 && r.syncs == (same ? 2u : forced ? 1u : 2u),
            "every free slot is allocated, with a barrier between its two writes");
      (same ? shared : split)++;
    }
    const auto before = ReadAll(path);
    const auto full = Allocate(sink, b, {6, 900000, false});
    Check(full.error == AW::filespace_full && full.writes == 0 && full.syncs == 0 && ReadAll(path) == before,
          "with no free slot the allocation is refused with no write");
  }
  Check(device.Close().ok(), "close");
  const auto file = ReadAll(path);
  Check(b.Model(file) == model, "the map is the oracle's, slot for slot");
  b.Untouched(file, model, "exhaustion");
  b.Readers(path, "exhaustion");
  Check(split > 0 && (b.size == 131072 || shared > 0),
        "both kinds occurred: record and state in two units, and (Q76; where a record shares the state unit) "
        "the shared unit written twice");
  std::cout << "exhaustion page_size=" << b.size << " allocated=" << free << " shared_unit=" << shared
            << " two_units=" << split << '\n';
  fs::remove(path);
}

// ---- Process loss and torn writes ---------------------------------------------
[[noreturn]] void Child(const Base& b, const std::string& path, u32 limit, bool torn) {
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
      for (const auto& a : Script())
        if (code == kChildCompleted && !Allocate(*sink, b, a).ok()) code = kChildFailed;
    }
    if (!device.Close().ok()) code = kChildFailed;
    _exit(code);
  } catch (...) {
    _exit(kChildException);
  }
}

void ProcessLoss(const Base& b, bool torn) {
  const auto script = Script();
  const MapModel base = b.Model(b.bytes);
  const auto steps = Steps(script, base, b.bytes, b.size);
  const u32 total = static_cast<u32>(steps.size());
  Check(total >= script.size(), "every allocation has at least one write");
  unsigned old_units = 0, new_units = 0, leftovers_reused = 0;
  for (u32 k = torn ? 1 : 0; k <= total + (torn ? 0 : 1); ++k) {
    const bool full = !torn && k == total + 1;
    const std::string label = std::string(torn ? "torn write " : "crash after ") + std::to_string(k);
    const auto path = b.Fresh("loss.db");
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = fork();
    Check(child >= 0, label + ": fork");
    if (child == 0) Child(b, path, full ? kNoCrash : k, torn);
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == (full ? kChildCompleted : kPlannedCrash),
          label + ": the child stops where planned");
    auto file = ReadAll(path);
    MapModel expected = Prefix(base, steps, full ? total : torn ? k - 1 : k);
    const MapModel seen = b.Model(file);
    if (torn && seen != expected) {
      expected = Prefix(base, steps, k);
      ++new_units;
    } else if (torn) {
      ++old_units;
    }
    Check(seen == expected, label + ": the map is the oracle's write prefix");
    b.Untouched(file, seen, label);
    b.Readers(path, label);
    // A new process allocates the lowest free slot, a leftover record's too.
    MapModel after = expected;
    std::vector<Step> next;
    const Alloc extra{6, 50000 + k * 4, false};
    AddSteps(next, extra, after, b.bytes, b.size);
    for (const auto& s : next) s(after);
    bool leftover = false;
    for (const auto& [n, s] : expected.slots)
      if (s.state == 0 && s.record != std::array<byte, 128>{}) leftover = true;
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": reopen");
      {
        d::FileDeviceCarefulWriteSink sink(device);
        Check(Allocate(sink, b, extra).ok(), label + ": the next allocation succeeds");
      }
      Check(device.Close().ok(), label + ": close");
    }
    file = ReadAll(path);
    Check(b.Model(file) == after, label + ": it took the lowest free slot");
    if (leftover) {
      ++leftovers_reused;
      for (const auto& [n, s] : b.Model(file).slots)
        Check(s.state != 0 || s.record == std::array<byte, 128>{}, label + ": the leftover record was overwritten");
    }
    b.Readers(path, label + " after");
    fs::remove(path);
  }
  if (torn) Check(old_units > 0 && new_units > 0, "torn writes left both old and new units");
  else Check(leftovers_reused > 0, "a crash between an allocation's writes left a leftover, reused next");
  std::cout << (torn ? "torn writes " : "process loss ") << "page_size=" << b.size << " writes=" << total
            << " leftovers_reused=" << leftovers_reused << '\n';
}

// ---- Q76: the shared unit torn at its half-way point ----------------------------
// A slot is exposed when its record shares the state's unit and lies past the
// unit's half-way point (state written, record not, by a half write). In the
// first map page as create lays it out: 1-2 at 8/16/32 KiB, 0-1 at 64 KiB and
// at 128 KiB (its one map page covers 512 pages, so records start at 768).
bool Exposed(u64 page, const MapModel& m, std::span<const byte> file, u32 size) {
  for (const auto map_page : m.map_pages) {
    const auto* p = file.data() + map_page * size;
    const u32 n = Load32(p + 128 + 12);
    const u64 first = Load64(p + 128 + 48);
    if (page < first || page >= first + n) continue;
    const u64 i = page - first, records_at = (512 + (n + 1) / 2 + 127) / 128 * 128;
    const u64 record = records_at + 128 * i, state = 512 + i / 2, unit = state / 512 * 512;
    return record / 512 == state / 512 && state < unit + 256 && record >= unit + 256;
  }
  return false;
}
void ExposedTear(const Base& b) {
  // The expected exposed indices, from the format's own layout.
  const std::map<u32, std::vector<u32>> expected{
      {8192, {1, 2}}, {16384, {1, 2}}, {32768, {1, 2}}, {65536, {0, 1}}, {131072, {0, 1}}};
  const MapModel base = b.Model(b.bytes);
  std::vector<u32> indices;
  {
    const auto* p = b.bytes.data() + base.map_pages.front() * b.size;
    const u64 first = Load64(p + 128 + 48);
    for (const auto& [n, s] : base.slots)
      if (n >= first && n < first + Load32(p + 128 + 12) && Exposed(n, base, b.bytes, b.size))
        indices.push_back(static_cast<u32>(n - first));
  }
  if (!expected.contains(b.size) || indices != expected.at(b.size)) {
    std::cerr << "exposed indices page_size=" << b.size << ":";
    for (const auto i : indices) std::cerr << ' ' << i;
    std::cerr << '\n';
  }
  Check(expected.contains(b.size) && indices == expected.at(b.size), "the exposed slot indices are the layout's");

  // Fill, in a fresh copy, every free slot below the first free exposed slot,
  // so the next allocation takes an exposed slot; then tear each of its two
  // writes at the half-way point.
  u64 target = 0;
  for (const auto& [n, s] : base.slots)
    if (s.state == 0 && Exposed(n, base, b.bytes, b.size)) {
      target = n;
      break;
    }
  if (target == 0) {
    // 128 KiB: the exposed slots cover pages 0 and 1, which create allocates;
    // no allocation can reach them.
    for (const auto& [n, s] : base.slots)
      if (Exposed(n, base, b.bytes, b.size)) Check(s.state != 0, "an exposed slot with no free one is allocated");
    std::cout << "exposed tear page_size=" << b.size << " no free exposed slot (create allocated them)\n";
    return;
  }
  const auto prepared = b.Fresh("exposed.db");
  MapModel model = base;
  {
    d::FileDevice device;
    Check(device.Open(prepared, d::FileOpenMode::open_existing).ok(), "open");
    {
      d::FileDeviceCarefulWriteSink sink(device);
      unsigned id = 70000;
      while (LowestFree(model) != target) {
        const Alloc a{6, id, false};
        id += 4;
        std::vector<Step> steps;
        AddSteps(steps, a, model, b.bytes, b.size);
        for (const auto& s : steps) s(model);
        Check(Allocate(sink, b, a).ok(), "fill below the exposed slot");
      }
    }
    Check(device.Close().ok(), "close");
  }
  Check(b.Model(ReadAll(prepared)) == model, "filled to the exposed slot");
  const auto prepared_bytes = ReadAll(prepared);
  const Alloc exposed{6, 90000, false};
  std::vector<Step> steps;
  AddSteps(steps, exposed, model, b.bytes, b.size);
  Check(steps.size() == 2, "the exposed allocation is two writes");
  for (u32 k = 1; k <= 2; ++k) {
    const std::string label = "exposed tear write " + std::to_string(k);
    const auto path = (b.dir.path() / "exposed-torn.db").string();
    WriteAll(path, prepared_bytes);
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = fork();
    Check(child >= 0, label + ": fork");
    if (child == 0) {
      try {
        d::FileDevice device;
        if (!device.Open(path, d::FileOpenMode::open_existing).ok()) _exit(kChildFailed);
        {
          d::FileDeviceCarefulWriteSink file(device);
          scratchbird::tests::TornWriteSink tear(file, k, kPlannedCrash);
          (void)Allocate(tear, b, exposed);
        }
        _exit(kChildCompleted);
      } catch (...) {
        _exit(kChildException);
      }
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == kPlannedCrash,
          label + ": the child stops at the torn write");
    const auto file = ReadAll(path);
    const auto seen = b.Model(file);
    Check(seen == Prefix(model, steps, k - 1) || seen == Prefix(model, steps, k),
          label + ": the map is the old or the new prefix, never a state without its record");
    Check(seen.slots.at(target).state == 0 || seen.slots.at(target).record != std::array<byte, 128>{},
          label + ": an allocated slot has its record");
    b.Untouched(file, seen, label);
    b.Readers(path, label);  // the file opens
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": reopen");
      {
        d::FileDeviceCarefulWriteSink sink(device);
        Check(Allocate(sink, b, {6, 95000 + 4 * k, false}).ok(), label + ": the next allocation succeeds");
      }
      Check(device.Close().ok(), label + ": close");
    }
    b.Readers(path, label + " after");
    fs::remove(path);
  }
  fs::remove(prepared);
  std::cout << "exposed tear page_size=" << b.size << " page=" << target << " both writes torn, file opens\n";
}

// ---- I/O failures -----------------------------------------------------------------
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
  bool reached() const { return calls_ >= fail_at_; }

 private:
  static d::BoundedIoResult Fail(d::BoundedIoOperation kind, u64 offset, usize bytes) {
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
};

void Faults(const Base& b) {
  const auto script = Script();
  const MapModel base = b.Model(b.bytes);
  const auto steps = Steps(script, base, b.bytes, b.size);
  unsigned points = 0, refused_reads = 0, failed_writes = 0;
  for (u32 at = 1;; ++at) {
    const std::string label = "fault at call " + std::to_string(at);
    const auto path = b.Fresh("faults.db");
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": open");
    bool reached = false;
    u32 performed = 0;
    {
      d::FileDeviceCarefulWriteSink file(device);
      FaultSink sink(file, at);
      for (const auto& a : script) {
        auto r = Allocate(sink, b, a);
        if (r.ok()) continue;
        if (!r.attempted) {
          Check(r.error == AW::io_failure && r.writes == 0, label + ": a failed read writes nothing");
          ++refused_reads;
          r = Allocate(sink, b, a);
          Check(r.ok(), label + ": and the same allocation succeeds again");
          continue;
        }
        Check(r.error == AW::write_failed, label + ": a failed write or sync is reported");
        ++failed_writes;
        break;
      }
      reached = sink.reached();
      performed = sink.writes();
    }
    Check(device.Close().ok(), label + ": close");
    const auto file = ReadAll(path);
    Check(b.Model(file) == Prefix(base, steps, performed), label + ": the map is the prefix of the writes performed");
    b.Readers(path, label);
    fs::remove(path);
    if (!reached) break;
    ++points;
  }
  Check(points > 10 && refused_reads > 0 && failed_writes > 0, "reads, writes and syncs were each failed");
  std::cout << "faults page_size=" << b.size << " points=" << points << " failed_reads=" << refused_reads
            << " failed_writes=" << failed_writes << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  unsigned profile = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[2]));
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const Base b(profile);
    Rules(b);
    Exhaustion(b);
    ProcessLoss(b, false);
    ProcessLoss(b, true);
    ExposedTear(b);
    Faults(b);
    std::cout << "PASS native allocation writer page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native allocation writer profile=" << profile << ": " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
