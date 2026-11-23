// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_OPEN_BIG_TEST (2026-11-26).
//
// A native database whose names relation is past the old 4096-row / 4 MiB open tier (grown by the PRODUCTION catalog writers
// on a file made by the real create path, native_name_stream_test.cpp's forge) OPENS through the real StartHostedEngine, serves,
// and answers ListNames, the resolver and the login names read exactly as the OFFLINE read of the same file says. Harness:
// native_name_stream_test.cpp (which includes native_name_resolve_test.cpp: create / forge / serve children, the offline
// ground truth). Driver:
//   size <bytes> <state>...    states: open (one file, one open), curve (several opens of one growing file: memory curve),
//                              budget (a tiny governed budget: the specific refusal), small (before/after timing of a fresh file)
//   bigopen <path> <db> <boot> <svc>   the serve child (prints OPENBIG ... lines)

#define SB_OPEN_BIG_TEST 1
#include "native_name_stream_test.cpp"
#include <fcntl.h>
#include <unistd.h>

#include "engine/internal_api/catalog/name_list_api.hpp"

namespace {
std::uint64_t HighWaterKbNow() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmHWM:", 0) == 0) return std::stoull(line.substr(6));
  return 0;
}

eng::EngineListNamesRequest ListRequest(const p::Uuid& scope, const p::Uuid& language, std::uint32_t max, const std::string& token = {}) {
  eng::EngineListNamesRequest r;
  r.context.native_served_facts = env.facts;
  r.context.database_uuid = ToEngine(env.database);
  r.scope_uuid = ToEngine(scope);
  r.language_uuid = ToEngine(language);
  r.continuation = token;
  r.max_entries = max;
  return r;
}

// Every page of one (scope, language); the answer is compared with the OFFLINE read of the same file.
void CheckListNames(const Offline& offline, const p::Uuid& scope, const p::Uuid& language, std::uint32_t page) {
  std::vector<eng::EngineListedName> got;
  std::string token;
  std::uint64_t digest = 0;
  for (std::size_t pages = 0;; ++pages) {
    const auto result = eng::EngineListNames(ListRequest(scope, language, page, token));
    Check(result.ok, "ListNames page: " + result.diagnostic.code);
    if (pages == 0) digest = result.names_digest;
    else Check(result.names_digest == digest, "the snapshot did not change between pages");
    got.insert(got.end(), result.entries.begin(), result.entries.end());
    if (!result.more) break;
    token = result.next_continuation;
  }
  // The offline expectation: per object, the entries in the language; else the object's default entry.
  std::map<p::Uuid, std::vector<const NameFact*>> by_object;
  for (const auto& n : offline.names)
    if (n.scope == scope) by_object[n.object].push_back(&n);
  std::set<std::pair<p::Uuid, std::string>> want, have;
  std::size_t expected = 0;
  for (const auto& [object, entries] : by_object) {
    std::vector<const NameFact*> in_language;
    const NameFact* object_default = nullptr;
    for (const auto* e : entries) {
      if (e->language == language) in_language.push_back(e);
      if (e->default_for_object) object_default = e;
    }
    if (!in_language.empty()) {
      for (const auto* e : in_language) { want.insert({e->entry_uuid, e->text}); ++expected; }
    } else if (object_default != nullptr) {
      want.insert({object_default->entry_uuid, object_default->text});
      ++expected;
    }
  }
  for (const auto& g : got) {
    p::Uuid id;
    std::copy(g.name_entry_uuid.bytes.begin(), g.name_entry_uuid.bytes.end(), id.bytes.begin());
    have.insert({id, g.name});
  }
  Check(got.size() == expected && have == want, "ListNames equals the offline listing (" + std::to_string(got.size()) + " of " + std::to_string(expected) + ")");
}
}  // namespace

namespace {
int OpenBigServeImpl(const std::string& state, const Offline& offline, srv::NativeDatabaseLoop& loop, const fs::path&, const p::Uuid& database,
                     const p::Uuid& bootstrap, const p::Uuid&, double start_ms) {
  const auto hwm_after_open = HighWaterKbNow();
  const auto security_scope = offline.security_schema;
  const auto en = cat::kCatalogLanguage_en;
  // 1. The names source: its rows are the file's, field by field.
  const auto t0 = std::chrono::steady_clock::now();
  const auto snapshot = Rows();
  const double read_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Check(snapshot.ok() && snapshot.database_uuid == database, "the names source reads the served file");
  Check(snapshot.rows.size() == offline.names.size(), "the names source lists exactly the offline live entries (" + std::to_string(snapshot.rows.size()) + ")");
  std::map<p::Uuid, const eng::NativeNameRowValue*> by_entry;
  for (const auto& row : snapshot.rows) by_entry.emplace(row.name_entry_uuid, &row);
  Check(by_entry.size() == snapshot.rows.size(), "every source row has its own entry UUID");
  for (const auto& fact : offline.names) {
    const auto found = by_entry.find(fact.entry_uuid);
    Check(found != by_entry.end(), "every offline entry is a source row");
    const auto& row = *found->second;
    Check(row.object_uuid == fact.object && row.scope_uuid == fact.scope && row.language_uuid == fact.language && row.object_kind == fact.kind &&
              row.was_quoted == fact.quoted && row.raw_name_text == fact.text && row.default_for_object == fact.default_for_object &&
              row.default_for_language == fact.default_for_language && row.name_class == static_cast<std::uint8_t>(fact.name_class),
          "a source row equals the offline entry");
  }
  // 2. ListNames over the principals' scope, every page, against the offline listing.
  const auto t1 = std::chrono::steady_clock::now();
  CheckListNames(offline, security_scope, en, 1000);
  const double list_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  // 3. The resolver (the registry projection of the names source) and name -> UUID: the first, a middle and the last bulk name.
  const auto loaded = eng::LoadNameRegistryState(MakeRequest(Ask{}).context, 0);
  Check(loaded.ok && loaded.state.entries.size() == snapshot.rows.size(), "the registry state is the source's rows, projected");
  std::vector<const NameFact*> bulk;
  for (const auto& fact : offline.names)
    if (fact.text.rfind("bulk_", 0) == 0) bulk.push_back(&fact);
  std::sort(bulk.begin(), bulk.end(), [](const auto* a, const auto* b) { return a->text < b->text; });
  if (!bulk.empty()) {
    for (const auto* fact : {bulk.front(), bulk[bulk.size() / 2], bulk.back()}) {
      Ask a{fact->text, fact->quoted};
      ExpectObject(Resolve(a), fact->object, "a bulk name resolves to the bootstrap principal (" + fact->text + ")");
    }
  }
  // 4. The login names read: the login-material read of the bootstrap principal, with its password.
  {
    PasswordFactory passwords(database);
    srv::NativeLoopLoginRequest login;
    login.principal_uuid = bootstrap;
    login.password = passwords.Make(kBootstrapPassword);
    login.password_length = kBootstrapPassword.size();
    const auto material = loop.SubmitReadLoginMaterial(std::move(login)).get();
    Check(material.ok() && material.material->principal_uuid == bootstrap, "the login-material read of the bootstrap principal works");
  }
  // 5. The login of the LAST principal (SB_OPEN_BIG_LAST: a principal the forge cloned after every other row): the security
  //    relation is past the old 1024-row credential tier, and the login-material read reaches its last row.
  if (const char* last = std::getenv("SB_OPEN_BIG_LAST")) {
    PasswordFactory passwords(database);
    srv::NativeLoopLoginRequest login;
    login.principal_uuid = FromHex(last);
    login.password = passwords.Make(kBootstrapPassword);
    login.password_length = kBootstrapPassword.size();
    const auto material = loop.SubmitReadLoginMaterial(std::move(login)).get();
    Check(material.ok() && material.material->principal_uuid == FromHex(last), "the login-material read of the LAST principal works");
    std::printf("LASTLOGIN ok principals_in_file=%zu security_definitions=%zu\n", offline.principals.size(), offline.security.size());
  }
  // 6. SB_CREDENTIAL_TIER_GROWTH: the credential tier shrunk to 600 rows (the security relation holds more) after the open (standing in for principals added past
  //    its headroom); the next login's credential read runs out, grows the tier from the file and reads once more.
  if (std::getenv("SB_OPEN_BIG_SHRINK")) {
    bool shrunk = false;
    Check(loop.RunOnLoopThreadForTest([&] { shrunk = loop.ShrinkCredentialTierOnLoopThreadForTest(600); }) && shrunk, "the credential tier shrunk on the loop thread");
    PasswordFactory passwords(database);
    srv::NativeLoopLoginRequest login;
    login.principal_uuid = bootstrap;
    login.password = passwords.Make(kBootstrapPassword);
    login.password_length = kBootstrapPassword.size();
    const auto material = loop.SubmitReadLoginMaterial(std::move(login)).get();
    std::printf("GROWTH login_error=%s\n", eng::NativeLoginMaterialErrorName(material.error));
    Check(material.ok() && material.material->principal_uuid == bootstrap, "after the tier ran out, the login grew it from the file and succeeded");
    PasswordFactory again(database);
    srv::NativeLoopLoginRequest second;
    second.principal_uuid = bootstrap;
    second.password = again.Make(kBootstrapPassword);
    second.password_length = kBootstrapPassword.size();
    const auto next = loop.SubmitReadLoginMaterial(std::move(second)).get();
    Check(next.ok(), "the grown tier serves the next login without growing again");
    std::printf("GROWTH ok\n");
  }
  std::optional<srv::NativeLoopServingFacts> facts;
  Check(loop.RunOnLoopThreadForTest([&] { facts = loop.FactsOnLoopThread(); }) && facts.has_value(), "the loop's open facts");
  std::printf("OPENBIG state=%s rows=%zu bulk=%zu open_ms=%.1f snapshot_ms=%.1f list_ms=%.1f hwm_after_open_kb=%llu hwm_end_kb=%llu scale=%llu "
              "catalog_ws=%llu scan_ws=%llu capture_ws=%llu cred_rows=%llu cred_pages=%llu cred_ws=%llu\n",
              state.c_str(), snapshot.rows.size(), bulk.size(), start_ms, read_ms, list_ms, (unsigned long long)hwm_after_open,
              (unsigned long long)HighWaterKbNow(), (unsigned long long)facts->open_catalog_scale, (unsigned long long)facts->open_catalog_workspace_bytes, (unsigned long long)facts->open_catalog_scan_bytes, (unsigned long long)facts->open_capture_bytes,
              (unsigned long long)facts->open_credential_rows, (unsigned long long)facts->open_credential_base_pages,
              (unsigned long long)facts->open_credential_workspace_bytes);
  return 0;
}
}  // namespace

namespace {
int OpenBigServe(const std::string& state, const Offline& offline, srv::NativeDatabaseLoop& loop, const fs::path& directory, const p::Uuid& database,
                 const p::Uuid& bootstrap, const p::Uuid& service, double start_ms) {
  return OpenBigServeImpl(state, offline, loop, directory, database, bootstrap, service, start_ms);
}
}  // namespace

namespace {
// secforge <path> <bootstrap> <count>: <count> new COMMITTED VERSIONS of existing security rows (the principals', roles', groups'
// and grants' definition rows), each a successor written by the PRODUCTION catalog writer in one transaction. The security
// relation's row count is its stored row versions (what the old 1024-row credential tier counted), so this takes the relation
// past the old tier. (New principals would also need a name vector and a default name entry each, which the open's verification
// demands of every definition; a principal clone is not forged here.) Prints the number written.
int SecForgeMode(const fs::path& path, const p::Uuid& bootstrap, int count) {
  const auto started = std::chrono::steady_clock::now();
  const auto before = ReadOffline(path);
  auto w = BeginWriting(path, before);
  w->binding.maximum_chain_pages = w->binding.maximum_index_pages = w->binding.maximum_base_pages = 1u << 20;
  int written = 0;
  std::map<std::string, int> refusals;
  for (const auto& [object, fact] : before.security) {
    if (written >= count) break;
    db::CatalogVersionRequest r;
    r.role = 4;
    r.transaction_order = w->order;
    r.transaction_uuid = w->header.transaction_uuid;
    r.row_uuid = fact.row_uuid;
    r.predecessor_version_uuid = fact.version_uuid;
    r.version_uuid = NewId();
    r.metadata = fact.metadata;
    ++r.metadata.definition_version;
    r.metadata.creator_transaction_uuid = {p::UuidKind::transaction, w->header.transaction_uuid};
    r.metadata.creator_local_transaction_id = w->order;
    r.metadata.audit_uuid = {p::UuidKind::object, w->event};
    r.new_pages = NewPages();
    const auto result = db::WriteCatalogSuccessorVersion(*w->sink, w->binding, r);
    if (result.ok()) ++written;
    else ++refusals[db::CatalogVersionWriteErrorName(result.error)];
  }
  (void)bootstrap;
  for (const auto& [why, n] : refusals) std::cerr << "security successor refused: " << why << " x" << n << '\n';
  Check(written == count, "the production writer wrote the security successors (" + std::to_string(written) + " of " + std::to_string(count) + ")");
  Commit(*w);
  Check(w->device.Close().ok(), "close");
  std::cout << "SECFORGED versions=" << written << " seconds=" << Seconds(started) << std::endl;
  return 0;
}

// One state of one page size. Every database directory is removed as soon as its state finishes (the /tmp footprint).
void OpenBigOneSize(const std::string& self, const fs::path& root, std::uint32_t page_size, const std::vector<std::string>& states) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const std::uint64_t total_pages = std::max<std::uint64_t>(512, (128ull << 20) / page_size);
  for (const auto& state : states) {
    const auto dir = root / (state + std::to_string(page_size));
    fs::create_directory(dir);
    const auto path = dir / "open.sdb";
    int status = 0;
    const auto created = Capture("'" + self + "' bigcreate '" + path.string() + "' " + std::to_string(page_size) + " " + std::to_string(total_pages), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + state + ": create: " + created);
    const auto boot = Field2(created, "bootstrap"), service = Field2(created, "service"), database = Field2(created, "database");
    const auto serve = [&](const char* extra_env) {
      int st = 0;
      const auto out = Capture(std::string(extra_env) + "'" + self + "' serve '" + path.string() + "' serving " + database + " " + boot + " " + service, &st);
      Check(st == 0 && out.find("OPENBIG ") != std::string::npos, tag + state + ": the open and the served checks: " + out.substr(0, 1500));
      return out;
    };
    const auto forge_names = [&](int rows, int start) {
      const auto t0 = std::chrono::steady_clock::now();
      const auto forged = Capture("'" + self + "' bigforge '" + path.string() + "' " + boot + " " + std::to_string(rows) + " 0 250 " + std::to_string(start), &status);
      Check(status == 0 && forged.find("FORGED rows=" + std::to_string(rows)) != std::string::npos, tag + state + ": forge: " + forged.substr(0, 1500));
      const auto seconds = Seconds(t0);
      std::printf("%s%s: forged %d name rows in %.1f s (%.1f rows/s)\n", tag.c_str(), state.c_str(), rows, seconds, rows / seconds);
    };
    if (state == "names" || state == "names10k" || state == "names20k") {
      forge_names(state == "names" ? 3200 : state == "names10k" ? 9000 : 19000, 0);
      const auto old_read = Capture("'" + self + "' oldread '" + path.string() + "'", &status);
      Check(status == 0 && old_read.find("OLDREAD ok=0") != std::string::npos, tag + state + ": the old collect-everything reader refuses the relation (past the old tier): " + old_read.substr(0, 300));
      const auto out = serve("");
      const auto rows = Num(out, "rows");
      Check(rows > db::kNativeCatalogScanRelationMaximumRows, tag + state + ": the names relation is past the old 4096-row tier (" + std::to_string(rows) + ")");
      Check(Num(out, "scale") >= 2, tag + state + ": the open sized itself from the file (scale " + std::to_string(Num(out, "scale")) + ")");
      std::printf("%s%s: OPENED and served; %s\n", tag.c_str(), state.c_str(), out.substr(out.find("OPENBIG ")).c_str());
    } else if (state == "principals") {
      const auto forged = Capture("'" + self + "' secforge '" + path.string() + "' " + boot + " 400", &status);
      Check(status == 0 && forged.find("SECFORGED versions=400") != std::string::npos, tag + state + ": secforge: " + forged.substr(0, 1500));
      const auto out = serve(("SB_OPEN_BIG_LAST=" + boot + " ").c_str());
      Check(out.find("LASTLOGIN ok") != std::string::npos, tag + state + ": the login works: " + out.substr(0, 500));
      Check(Num(out, "cred_rows") > 1024, tag + state + ": the credential tier was sized from the file (" + std::to_string(Num(out, "cred_rows")) + " rows)");
      std::printf("%s%s: OPENED; %s\n", tag.c_str(), state.c_str(), out.substr(out.find("OPENBIG ")).c_str());
    } else if (state == "curve") {
      // The same file opened at growing name counts: governed arena and process high water at each.
      int forged = 0;
      std::uint64_t first_scan = 0;
      for (const int target : {0, 1500, 4900}) {  // + the created ~1100 names: ~1.1k, ~2.6k, ~6k rows
        if (target > forged) { forge_names(target - forged, forged); forged = target; }
        const auto out = serve("");
        std::printf("%scurve: %llu name rows: open %.0f ms, governed catalog workspace %llu bytes (scan %llu, capture %llu) at scale %llu, credential workspace %llu, "
                    "process high water after open %llu KB\n",
                    tag.c_str(), (unsigned long long)Num(out, "rows"), std::stod(Field2(out, "open_ms")), (unsigned long long)Num(out, "catalog_ws"),
                    (unsigned long long)Num(out, "scan_ws"), (unsigned long long)Num(out, "capture_ws"),
                    (unsigned long long)Num(out, "scale"), (unsigned long long)Num(out, "cred_ws"), (unsigned long long)Num(out, "hwm_after_open_kb"));
        if (!first_scan) first_scan = Num(out, "scan_ws");
        // SB_OPEN_COHORT_STREAM: the scan's workspace is a window of leaves (a fixed size) plus per-version keys: it does NOT grow
        // with the base pages (the old collect-everything scan held one decoded leaf workspace per page: 3x at scale 4).
        Check(Num(out, "scan_ws") <= first_scan + first_scan * 6 / 10,
              tag + "curve: the scan workspace stays flat as the file grows (" + std::to_string(Num(out, "scan_ws")) + " against " + std::to_string(first_scan) + " at the first size)");
      }
    } else if (state == "budget") {
      // The governed budget genuinely exceeded: the file needs the workspace the roomy open reported (catalog_ws); half of it is
      // set as the seam's explicit ceiling (SB_OPEN_BIG_BUDGET, bytes of one open's cohort scan + capture + backing).
      forge_names(3200, 0);
      int st = 0;
      const auto roomy = serve("");
      const auto needed = Num(roomy, "catalog_ws");
      Check(needed > 0 && Num(roomy, "scale") >= 2, tag + state + ": the roomy open needed a scale above the first (" + std::to_string(Num(roomy, "scale")) + ")");
      const auto out = Capture("SB_OPEN_BIG_BUDGET=" + std::to_string(needed / 2) + " '" + self + "' serve '" + path.string() + "' serving " + database + " " + boot + " " + service, &st);
      Check(st != 0 && out.find("DATABASE.OPEN_NATIVE_CATALOG_BUDGET_EXCEEDED") != std::string::npos, tag + state + ": a budget the file exceeds refuses with the specific code: " + out.substr(0, 600));
      std::printf("%sbudget: refused with DATABASE.OPEN_NATIVE_CATALOG_BUDGET_EXCEEDED\n", tag.c_str());
    } else if (state == "growth") {
      // The credential tier ran out after the open: the login grows it and reads again (see OpenBigServeImpl step 6).
      const auto out = serve("SB_OPEN_BIG_SHRINK=1 ");
      Check(out.find("GROWTH ok") != std::string::npos, tag + state + ": the login after the tier ran out: " + out.substr(0, 800));
      std::printf("%sgrowth: %s\n", tag.c_str(), out.substr(out.find("GROWTH"), 120).c_str());
    } else if (state == "pagezero") {
      // SB_STREAM_PASS_ANCHOR: page zero verified once per pass; a page zero changed under the pass is refused at its end.
      const auto out = Capture("'" + self + "' pagezero '" + path.string() + "'", &status);
      Check(status == 0 && out.find("PAGEZERO unchanged ok=1") != std::string::npos && out.find("PAGEZERO changed_under_pass ok=0") != std::string::npos &&
                out.find("PAGEZERO restored ok=1") != std::string::npos,
            tag + state + ": the page-zero-under-pass checks: " + out.substr(0, 1500));
      std::printf("%spagezero: %s\n", tag.c_str(), out.substr(0, 700).c_str());
    } else if (state == "small") {
      // The normal small file: the open's time (the suite has many opens).
      double best = 1e18;
      for (int i = 0; i < 5; ++i) {
        const auto out = serve("");
        best = std::min(best, std::stod(Field2(out, "open_ms")));
        if (i == 0) std::printf("%ssmall: scale %llu, %llu rows\n", tag.c_str(), (unsigned long long)Num(out, "scale"), (unsigned long long)Num(out, "rows"));
      }
      std::printf("%ssmall: StartHostedEngine best of 5 = %.1f ms\n", tag.c_str(), best);
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }
}

// SB_STREAM_PASS_ANCHOR: page zero is verified once at the start of a stream pass and compared again after the last leaf. Kept per
// leaf: the file's size unchanged, the page's own header (database, filespace, profile, number, generation, type, flags), the page
// inside the anchored extent. Moved to once per pass: the bootstrap-prefix read, the page-zero read and decode, the first size query.
// This mode reads the names relation (indexed, many leaves) through the stream: once unchanged (must succeed, with fewer preads than
// the three-per-leaf of the old rule), once with a byte of page zero flipped by the test seam after the 3rd leaf read (the pass must
// refuse with page_zero_failure at its end), once more unchanged after the byte is restored.
int PageZeroUnderPassMode(const fs::path& path) {
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(), "open read-only");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  const auto database = zero.record->bootstrap.database_uuid;
  const auto filespace = zero.record->bootstrap.filespace_uuid;
  const auto profile = zero.record->bootstrap.page_size_profile_uuid;
  const auto budget = 8ull * zero.record->total_pages * size;
  db::NativeCatalogStreamLimits limits;
  limits.maximum_profile = profile;
  limits.window_pages = 3;
  limits.history_versions = 4096;
  BoundMemory workspace_memory(database, db::NativeCatalogStreamWorkspaceBytes(limits)), source_memory(database, 32ull * 1024 * 1024);
  auto prepared = db::PrepareNativeCatalogStreamRead(limits, workspace_memory.memory, workspace_memory.binding);
  Check(prepared.ok(), "the stream workspace prepares");
  const std::array<d::NativeFilespaceDevice, 1> devices{{{filespace, profile, &device}}};
  auto source = db::AcquireNativeFixedRootMemoryLease(database, devices, filespace, {budget, 32ull * 1024 * 1024}, source_memory.memory, source_memory.binding);
  Check(source.ok(), "a fixed-root lease over the file");
  const auto run = [&](const char* label) {
    std::uint64_t rows = 0;
    const auto result = db::NativeCatalogVisibilityStreamReader::StreamCommittedSelected(
        *source.lease, 2, 2, budget, prepared.workspace, [&](const db::NativeCatalogVisibleRow&) { ++rows; return true; });
    std::printf("PAGEZERO %s ok=%d error=%s leaf_error=%d leaf_reads=%llu rows=%llu bytes=%llu\n", label, result.ok() ? 1 : 0,
                db::NativeCatalogStreamErrorName(result.error), static_cast<int>(result.leaf_error), (unsigned long long)result.leaf_reads,
                (unsigned long long)rows, (unsigned long long)result.physical_bytes_read);
    return result;
  };
  const auto clean = run("unchanged");
  Check(clean.ok() && clean.leaf_reads > 6, "an unchanged pass succeeds over many leaves (" + std::to_string(clean.leaf_reads) + " leaf reads)");
  // One byte of page zero's unused tail, changed between two leaf reads through a second descriptor.
  const int fd = ::open(path.c_str(), O_RDWR);
  Check(fd >= 0, "a writable descriptor for the test seam");
  unsigned char original = 0;
  const off_t where = static_cast<off_t>(size) - 17;
  Check(::pread(fd, &original, 1, where) == 1, "read the byte the seam will change");
  static int seam_fd = -1;
  static off_t seam_where = 0;
  static unsigned char seam_value = 0;
  seam_fd = fd; seam_where = where; seam_value = static_cast<unsigned char>(original ^ 0x5a);
  db::g_native_stream_leaf_read_test_seam = [](std::uint64_t reads) noexcept {
    if (reads == 3) { (void)::pwrite(seam_fd, &seam_value, 1, seam_where); }
  };
  const auto changed = run("changed_under_pass");
  db::g_native_stream_leaf_read_test_seam = nullptr;
  Check(!changed.ok() && changed.error == db::NativeCatalogStreamError::leaf_failure &&
            changed.leaf_error == db::NativeCatalogLeafMemoryError::page_zero_failure,
        "a page zero changed under the pass is refused with page_zero_failure at its end");
  Check(changed.leaf_reads > 3, "the pass went on past the change (the change is caught at the end, not per leaf): " + std::to_string(changed.leaf_reads));
  (void)::pwrite(fd, &original, 1, where);
  ::close(fd);
  const auto restored = run("restored");
  Check(restored.ok(), "after the byte is restored the pass succeeds again");
  source.lease.Reset();
  Check(device.Close().ok(), "close");
  return 0;
}

bool OpenBigDriver(int argc, char** argv, int* rc) {
  if (argc == 3 && std::string(argv[1]) == "pagezero") { *rc = PageZeroUnderPassMode(argv[2]); return true; }
  if (argc >= 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    std::vector<std::string> states{"names", "principals", "budget"};
    if (argc > 3) states.assign(argv + 3, argv + argc);
    OpenBigOneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), states);
    artifacts.Cleanup();
    std::printf("native_open_big: %d driver checks passed\n", checks);
    *rc = 0;
    return true;
  }
  if (argc == 5 && std::string(argv[1]) == "secforge") {
    *rc = SecForgeMode(argv[2], FromHex(argv[3]), std::stoi(argv[4]));
    return true;
  }
  return false;
}
}  // namespace
