// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_NAME_ADMISSION_TEST (Part B step S6, 2026-11-23). NAME ADMISSION in the engine matches the owner's name
// model, on the PRODUCTION create and write path (no durable-format change; that is S7). Owner rulings (2026-11-20..23): "The
// engine stores the name as given. No manipulation."; "an empty name '' IS allowed but not null"; "utf8 only ... no nulls for
// names" (a NUL byte and invalid UTF-8 are refused; NULL is not representable in a std::string, so the wire layers that can
// say "absent" refuse it); the engine's name capacity is 4096 BYTES of UTF-8 (SBsql advertises 128 characters: the parser's);
// every object has a default name; the same (scope, language, exact bytes) is refused; the same text in two languages is not.
//
// Every case runs on a PRODUCTION-created database (EngineCreateLifecycle through the native create fixture), created with the
// EMPTY database name "" (so every path begins with an empty root component). Names are added by the production storage
// writers (BEGIN, change event, a definition touch, WriteCatalogInsertVersions, COMMIT), as native_name_resolve does; this file
// includes that test as its harness (create/forge/serve children, the offline reader, the served facts). Children:
//   createempty <path> <page_size>   engine-API native create with database_name ""
//   admit <path> <b> <s>             the writer matrix: refusals (nothing written: the file is byte-identical across each
//                                    refused call), admissions, capacity probes, duplicates, case variants (pinned as TODAY's
//                                    behaviour, annotated "S7 changes this")
//   serve <path> adm <d> <b> <s>     StartHostedEngine (the open verifiers run on the file with the "" names), then the served
//                                    resolve / ListNames / UuidToName checks; run TWICE per file (restart round trip)
// With "size <bytes>" the driver runs one page size.
//
// What cannot be tested here (and why): the NULL name. std::string and std::string_view cannot carry NULL as distinct from "";
// the SCDD v2 descriptor always has a name_length, so it cannot say "absent" either. The engine refuses a null pointer with a
// nonzero size (AdmitCatalogIdentifierText invalid_text; covered in catalog_identifier_profile_test).

#define SB_NAME_ADMISSION_TEST 1
#define main name_resolve_driver_main
#include "native_name_resolve_test.cpp"
#undef main

#include "engine/internal_api/catalog/name_list_api.hpp"

namespace {
using WE = db::CatalogVersionWriteError;
const auto kProcessStart = std::chrono::steady_clock::now();

std::string Spell(WE e) { return db::CatalogVersionWriteErrorName(e); }
// The two refusals that are STORAGE bounds of the current durable name-tree layout (S7 redesigns the key), not naming rules: a cell
// key longer than a quarter page, or a leaf with no room for the cell. Never the text rule's codes.
bool StorageBound(WE e) { return e == WE::name_key_exceeds_page_capacity || e == WE::name_tree_full; }

// ---- create with the empty database name ----------------------------------------------------------------------------------
int CreateEmptyMode(const fs::path& path, std::uint32_t page_size) {
  std::string verifier;
  if (!scratchbird::core::crypto::DeriveLocalPasswordVerifier(kBootstrapPassword, &verifier)) return 1;
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "";  // S6: "" is a valid database name
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
  if (!scratchbird::tests::NativeFixtureReceiptUuid(made.api, "service_runtime_binding_uuid", &binding)) return 1;
  std::cout << "CREATED database=" << Hex(made.database_uuid) << " bootstrap=" << Hex(made.bootstrap_principal_uuid)
            << " service=" << Hex(made.service_principal_uuid) << " binding=" << Hex(binding) << std::endl;
  return 0;
}

// ---- the writer matrix ----------------------------------------------------------------------------------------------------
// One insert attempt through the production writer; returns its error (none = written). Does NOT Check success.
WE TryInsert(Writing& w, std::vector<db::CatalogInsertRow> rows) {
  db::CatalogInsertRequest r;
  r.transaction_order = w.order;
  r.transaction_uuid = w.header.transaction_uuid;
  r.rows = std::move(rows);
  r.new_pages = NewPages();
  return db::WriteCatalogInsertVersions(*w.sink, w.binding, r).error;
}
WE TryName(Writing& w, const NameFact& base, std::string text, bool quoted = false, std::optional<p::Uuid> language = std::nullopt,
           bool default_for_language = false) {
  std::vector<db::CatalogInsertRow> rows;
  rows.push_back(EntryRow(w, base, {std::move(text), quoted, language, cat::CatalogNameClass::alias, default_for_language}, false));
  return TryInsert(w, std::move(rows));
}
std::string FileBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

std::string Repeat(char c, std::size_t n) { return std::string(n, c); }
std::string Repeat(std::string_view unit, std::size_t n) {
  std::string out;
  out.reserve(unit.size() * n);
  for (std::size_t i = 0; i < n; ++i) out += unit;
  return out;
}

constexpr std::string_view kCaseA = "CaseAlpha", kCaseAUp = "CASEALPHA", kCaseB = "casebeta", kCaseBUp = "CASEBETA",
                           kCaseG = "CaseGamma", kCaseGUp = "CASEGAMMA", kCaseD = "CaseDelta", kSameBytes = "ExactSame";

int AdmitMode(const fs::path& path, const p::Uuid& bootstrap, const p::Uuid& service, std::uint32_t page_size) {
  ProcessMemory();
  const auto before_offline = ReadOffline(path);
  const auto* base_boot = EntryOf(before_offline, bootstrap, std::string(kBootstrapName));
  const auto* base_svc = EntryOf(before_offline, service, std::string(kServiceName));
  Check(base_boot && base_svc, "the create wrote the principals' name entries");
  auto w = BeginWriting(path, before_offline);
  Touch(*w, before_offline.security.at(bootstrap));
  Touch(*w, before_offline.security.at(service));

  // A refused attempt writes NOTHING: the file is byte-identical across the call.
  const auto refused = [&](WE expected, const std::string& what, const NameFact& base, std::string text, bool quoted = false,
                           std::optional<p::Uuid> language = std::nullopt) {
    const auto bytes_before = FileBytes(path);
    const auto got = TryName(*w, base, std::move(text), quoted, language);
    Check(got == expected, what + ": expected " + Spell(expected) + ", got " + Spell(got));
    Check(FileBytes(path) == bytes_before, what + ": nothing written (the file is byte-identical)");
  };
  const auto admitted = [&](const std::string& what, const NameFact& base, std::string text, bool quoted = false,
                            std::optional<p::Uuid> language = std::nullopt, bool default_for_language = false) {
    const auto got = TryName(*w, base, std::move(text), quoted, language, default_for_language);
    Check(got == WE::none, what + ": admitted, got " + Spell(got));
  };

  // 1. NUL and invalid UTF-8: one specific code each, nothing written.
  refused(WE::name_contains_nul, "a NUL inside a name", *base_boot, std::string("a\0b", 3));
  refused(WE::name_contains_nul, "a name that is only NUL", *base_boot, std::string("\0", 1));
  refused(WE::name_contains_nul, "a NUL at the end", *base_boot, std::string("abc\0", 4));
  refused(WE::name_contains_nul, "a quoted name with a NUL", *base_boot, std::string("q\0q", 3), true);
  refused(WE::name_invalid_utf8, "a truncated UTF-8 sequence", *base_boot, std::string("\xC3\x28", 2));
  refused(WE::name_invalid_utf8, "bytes never valid in UTF-8", *base_boot, std::string("\xFF\xFEname", 6));
  refused(WE::name_invalid_utf8, "an encoded UTF-16 surrogate", *base_boot, std::string("\xED\xA0\x80", 3));
  refused(WE::name_invalid_utf8, "an overlong encoding of U+0000", *base_boot, std::string("\xC0\x80", 2));
  refused(WE::name_invalid_utf8, "a scalar above U+10FFFF", *base_boot, std::string("\xF4\x90\x80\x80", 4));
  // 2. Capacity: 4097 bytes is refused with its own code at EVERY page size, whatever the characters.
  refused(WE::name_too_long, "4097 ASCII bytes", *base_boot, Repeat('a', 4097));
  refused(WE::name_too_long, "4097 bytes of 4-byte scalars and an 'a'", *base_boot, Repeat("\xF0\x9F\x90\xA6", 1024) + "a");
  refused(WE::name_too_long, "5000 bytes", *base_boot, Repeat('b', 5000));
  refused(WE::name_too_long, "131041 bytes (past the old 131040 text maximum)", *base_boot, Repeat('c', 131041));

  // 3. The empty name "": admitted (the writer's old invalid_name refusal is gone).
  admitted("the empty name, English alias of the bootstrap principal", *base_boot, "");
  // a second "" in the same scope and language (another object): the duplicate code, nothing written.
  refused(WE::duplicate_name, "a second empty name in the same scope and language", *base_svc, "");
  refused(WE::duplicate_name, "the quoted empty name is the same name", *base_svc, "", true);
  // the same text in a DIFFERENT language on a different object is allowed, and is that language's default name there.
  admitted("the empty name in en-CA on the service principal (default for en-CA)", *base_svc, "", false, cat::kCatalogLanguage_en_CA, true);
  // and in a third language on the bootstrap principal.
  admitted("the empty name in fr-CA on the bootstrap principal", *base_boot, "", false, cat::kCatalogLanguage_fr_CA, false);
  // 4. Multi-byte UTF-8: 2-, 3-, 4-byte scalars and a mixed name are stored as given.
  admitted("a 2-byte scalar name", *base_boot, "\xC3\xA9");
  admitted("a 3-byte scalars name", *base_boot, "\xE6\x97\xA5\xE6\x9C\xAC");
  admitted("a 4-byte scalar name", *base_boot, "\xF0\x9F\x90\xA6");
  admitted("a mixed-width name", *base_boot, "a\xC3\xA9" "b\xE6\x97\xA5" "c\xF0\x9F\x90\xA6");
  // 5. Capacity probes: the largest name the tree cell of THIS page size carries. The engine capacity is 4096 bytes; a cell is a
  //    quarter page (name_key_exceeds_page_capacity, a STORAGE bound of the current durable key layout: S7 redesigns it).
  //    Binary search for the largest admitted size, then the exact-4096 cases.
  {
    std::size_t lo = 0, hi = 4096;  // lo: admitted, hi: candidate
    const auto probe = [&](std::size_t n, int serial) -> WE {
      const char lead = static_cast<char>('d' + serial % 20);  // distinct names: serial picks the lead letter
      const auto text = std::string(1, lead) + Repeat('x', n - 1);
      const auto bytes_before = FileBytes(path);
      const auto got = TryName(*w, *base_boot, text);
      if (got != WE::none) Check(FileBytes(path) == bytes_before, "a capacity probe that is refused writes nothing");
      return got;
    };
    int serial = 0;
    const auto at_max = probe(hi, serial++);
    if (at_max == WE::none) {
      lo = hi;
    } else {
      Check(StorageBound(at_max), "4096 bytes is refused only for a storage bound of this page size: " + Spell(at_max));
      std::size_t low = 1;
      std::size_t high = hi;  // refused
      while (low + 1 < high) {  // low admitted-or-1, high refused
        const auto mid = (low + high) / 2;
        const auto got = probe(mid, serial++);
        if (got == WE::none) low = mid;
        else {
          Check(StorageBound(got), "a probe is admitted or refused for a storage bound of this page size: " + Spell(got));
          high = mid;
        }
      }
      lo = low;
    }
    std::printf("INFO capacity page_size=%u exact_4096=%s largest_admitted_bytes=%zu cell_bound_quarter_page=%u\n", page_size,
                at_max == WE::none ? "admitted" : ("refused_" + Spell(at_max)).c_str(), lo, page_size / 4);
    // 4096 bytes of 4-byte scalars (1024 scalars): the same outcome as 4096 ASCII bytes (the capacity counts BYTES).
    const auto four = TryName(*w, *base_boot, Repeat("\xF0\x9F\x90\xA6", 1024));
    Check(four == at_max, "4096 bytes of 4-byte scalars has the outcome of 4096 ASCII bytes: " + Spell(four) + " vs " + Spell(at_max));
    // 4095 bytes: admitted whenever 4096 is.
    if (at_max == WE::none) {
      const auto got = TryName(*w, *base_boot, "z" + Repeat('x', 4094));
      Check(got == WE::none, "4095 bytes is admitted where 4096 is");
    }
    if (std::getenv("SB_ADMISSION_EXPECT_4096_ADMITTED"))
      Check(at_max == WE::none, "a name of exactly 4096 bytes is admitted at this page size");
  }

  // 6. Uniqueness is by exact (scope, language, bytes): a byte-identical name of another object is refused.
  admitted("an exact name on the bootstrap principal", *base_boot, std::string(kSameBytes));
  refused(WE::duplicate_name, "the same bytes, unquoted, on another object", *base_svc, std::string(kSameBytes));
  // 7. TODAY's case-variant behaviour. S7 CHANGES THIS: the engine still compares DERIVED LANES (the profile's upper-ASCII fold
  //    for unquoted names; a quoted name keeps its case). Pinned so that S7's change is visible and deliberate.
  admitted("S7-lanes a: an unquoted CaseAlpha on the bootstrap principal", *base_boot, std::string(kCaseA));
  refused(WE::duplicate_name, "S7 changes this (a): an unquoted CASEALPHA is the same lane ALPHA today (S7: allowed, bytes differ)",
          *base_svc, std::string(kCaseAUp));
  admitted("S7-lanes b: an unquoted casebeta on the bootstrap principal", *base_boot, std::string(kCaseB));
  refused(WE::duplicate_name, "S7 changes this (b): a QUOTED CASEBETA equals the unquoted name folded today (S7: allowed)", *base_svc,
          std::string(kCaseBUp), true);
  admitted("S7-lanes c: a quoted CaseGamma on the bootstrap principal", *base_boot, std::string(kCaseG), true);
  admitted("S7-lanes c: a quoted CASEGAMMA on the service principal (two quoted case variants are two names today and in S7)", *base_svc,
           std::string(kCaseGUp), true);
  admitted("S7-lanes d: an unquoted CaseDelta on the bootstrap principal", *base_boot, std::string(kCaseD));
  admitted("S7 changes this (d): a QUOTED CaseDelta on the service principal is ADMITTED today (lane ALPHA-fold vs exact); the bytes "
           "are equal, so S7's exact rule REFUSES it", *base_svc, std::string(kCaseD), true);

  Commit(*w);
  Check(w->device.Close().ok(), "close");

  // 8. The committed file reads back (offline visibility reader): every admitted name is exactly as given.
  const auto after = ReadOffline(path);
  const auto has = [&](const p::Uuid& object, std::string_view text, const p::Uuid& language) {
    for (const auto& n : after.names)
      if (n.object == object && n.text == text && n.language == language) return true;
    return false;
  };
  Check(has(bootstrap, "", cat::kCatalogLanguage_en),
        "the committed file holds the empty name of the bootstrap principal in en");
  Check(has(service, "", cat::kCatalogLanguage_en_CA), "the committed file holds the empty name of the service principal in en-CA");
  Check(has(bootstrap, "", cat::kCatalogLanguage_fr_CA), "the committed file holds the empty name of the bootstrap principal in fr-CA");
  Check(has(bootstrap, "\xF0\x9F\x90\xA6", cat::kCatalogLanguage_en), "the 4-byte scalar name is stored byte for byte");
  std::printf("INFO admit page_size=%u names_after=%zu seconds=%.1f\n", page_size, after.names.size(),
              std::chrono::duration<double>(std::chrono::steady_clock::now() - kProcessStart).count());
  std::cout << "ADMITTED ok" << std::endl;
  return 0;
}

// ---- served checks (hook of the harness' ServeMode) ------------------------------------------------------------------------
std::string DisplayHex(const eng::EngineUuid& id) {
  p::Uuid u;
  std::copy(id.bytes.begin(), id.bytes.end(), u.bytes.begin());
  return Hex(u);
}

int AdmissionServe(const std::string& state, const Offline& offline, srv::NativeDatabaseLoop& loop, const fs::path& directory,
                   const std::map<std::string, std::string>& before, const p::Uuid& database, const p::Uuid& bootstrap,
                   const p::Uuid& service) {
  (void)state;
  const auto security_scope = offline.security_schema;
  const p::Uuid en = cat::kCatalogLanguage_en, en_ca = cat::kCatalogLanguage_en_CA, fr_ca = cat::kCatalogLanguage_fr_CA;
  const auto since = [started = std::chrono::steady_clock::now()] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };

  // The open verified the file (startup name-tree, collision, path and binding verifiers all ran with "" names in it).
  // 1. Exact resolve of "": in en it is the bootstrap principal's, in en-CA the service principal's, in fr-CA the bootstrap's, and
  //    nobody has it in de-DE. The quoted flag takes no part.
  {
    const auto o = ResolveText("");
    ExpectObject(o, bootstrap, "S6: the empty name resolves exactly (en)");
    const auto& m = o.result.matched_name;
    Check(m.present && m.object_kind == kUserKind && Same(m.language_uuid, en) && Same(m.scope_uuid, security_scope) &&
              m.name_class == static_cast<std::uint8_t>(cat::CatalogNameClass::alias) && !m.default_for_object,
          "the matched entry of \"\" is the alias entry as stored (scope, language, kind, class)");
    ExpectObject(ResolveText("", true), bootstrap, "S6: the quoted empty name is the same lookup");
    Ask ca{""};
    ca.language = TagFor(en_ca);
    ExpectObject(Resolve(ca), service, "S6: the empty name in en-CA is the service principal's");
    Ask fr{""};
    fr.language = TagFor(fr_ca);
    ExpectObject(Resolve(fr), bootstrap, "S6: the empty name in fr-CA is the bootstrap principal's");
    Ask de{""};
    de.language = "de-DE";
    ExpectRefusal(Resolve(de), kNotFound, "S6: nobody holds the empty name in de-DE");
    ExpectRefusal(ResolveText(" "), kNotFound, "a blank is not the empty name");
    ExpectRefusal(ResolveText(std::string_view("\0", 1)), kNotFound, "a NUL is not the empty name");
  }
  // 2. Every name the admit child stored is found by its exact bytes, whatever its length or encoding. Where two objects hold the
  //    SAME bytes in one scope and language (today's lanes admit an unquoted CaseDelta beside a quoted CaseDelta: S7 changes this)
  //    the exact matcher says AMBIGUOUS: an honest answer for a state S7 forbids.
  std::size_t found = 0, longest = 0, ambiguous = 0;
  std::map<std::pair<std::string, p::Uuid>, std::set<p::Uuid>> holders;
  for (const auto& fact : offline.names)
    if (fact.scope == security_scope && fact.kind == kUserKind) holders[{fact.text, fact.language}].insert(fact.object);
  for (const auto& fact : offline.names) {
    if (fact.scope != security_scope || fact.kind != kUserKind || (fact.object != bootstrap && fact.object != service)) continue;
    Ask ask{fact.text, fact.quoted};
    ask.language = TagFor(fact.language);
    const auto o = Resolve(ask);
    if (holders[{fact.text, fact.language}].size() > 1) {
      ExpectRefusal(o, "CATALOG.NAME.AMBIGUOUS", "two objects hold these bytes in one scope and language");
      ++ambiguous;
    } else {
      Check(o.ok() && Same(o.result.primary_object.uuid, fact.object),
            "S6: a stored name is found by its exact bytes: " + std::to_string(fact.text.size()) + " bytes");
    }
    ++found;
    longest = std::max(longest, fact.text.size());
  }
  Check(found >= 12 && ambiguous == 2, "the served file resolves every principal name it holds (found " + std::to_string(found) +
                                           ", ambiguous " + std::to_string(ambiguous) + ")");
  // 3. ListNames: "" is listed (the bootstrap's en alias with the default; the service's en-CA default), as stored.
  {
    eng::EngineListNamesRequest r;
    r.context.native_served_facts = env.facts;
    r.context.database_uuid = ToEngine(env.database);
    r.scope_uuid = ToEngine(security_scope);
    r.language_uuid = ToEngine(en);
    r.max_entries = 1024;
    const auto listed = eng::EngineListNames(r);
    Check(listed.ok, "ListNames reads the scope");
    bool empty_boot = false;
    for (const auto& e : listed.entries)
      if (e.name.empty() && Same(e.object_uuid, bootstrap) && Same(e.language_uuid, en) && !e.is_fallback) empty_boot = true;
    Check(empty_boot, "S6 (S4 ListNames): the empty name appears in the English listing, as stored, not as a fallback");
    r.language_uuid = ToEngine(en_ca);
    const auto listed_ca = eng::EngineListNames(r);
    Check(listed_ca.ok, "ListNames reads the scope in en-CA");
    bool empty_svc = false;
    for (const auto& e : listed_ca.entries)
      if (e.name.empty() && Same(e.object_uuid, service) && Same(e.language_uuid, en_ca) && !e.is_fallback) empty_svc = true;
    Check(empty_svc, "S6: the en-CA listing holds the service principal's empty name, not as a fallback");
  }
  // 4. UuidToName: the path begins with the empty DATABASE root component and may end with an empty leaf.
  {
    eng::EngineUuidToNameRequest r;
    r.context.native_served_facts = env.facts;
    r.context.database_uuid = ToEngine(env.database);
    r.object_uuid = ToEngine(bootstrap);
    r.language_uuid = ToEngine(en);
    const auto path = eng::EngineUuidToName(r);
    Check(path.ok && path.components.size() >= 2, "UuidToName of the bootstrap principal answers");
    Check(path.components.front().is_database_root && path.components.front().name.empty(),
          "S6: the path's root component is the database's name \"\" (a component of zero bytes)");
    r.object_uuid = ToEngine(service);
    r.language_uuid = ToEngine(en_ca);
    const auto svc = eng::EngineUuidToName(r);
    Check(svc.ok && svc.components.size() >= 2, "UuidToName of the service principal in en-CA answers");
    Check(svc.components.back().name.empty() && !svc.components.back().is_fallback && Same(svc.components.back().language_uuid, en_ca),
          "S6: the leaf component is the service principal's en-CA name \"\" (no fallback)");
    Check(svc.components.front().is_database_root && svc.components.front().name.empty(),
          "S6: and the root component is the empty database name");
  }
  // Reads write nothing.
  Check(Snapshot(directory) == before, "the directory and every file's bytes are unchanged after every call");
  Check(loop.Close().ok, "the loop closes");
  std::printf("INFO serve found=%zu longest_name=%zu seconds=%.1f\n", found, longest, since());
  std::cout << "NAME_RESOLVE_OK checks=" << checks << " state=adm" << std::endl;
  return 0;
}

// ---- driver ---------------------------------------------------------------------------------------------------------------
void OneAdmissionSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto dir = root / ("adm" + std::to_string(page_size));
  fs::create_directory(dir);
  const auto path = dir / "names.sdb";
  int status = 0;
  const auto started = std::chrono::steady_clock::now();
  const auto created = Capture("'" + self + "' createempty '" + path.string() + "' " + std::to_string(page_size), &status);
  Check(status == 0 && created.starts_with("CREATED "), tag + "create with the empty name: " + created);
  const auto admit = Capture("'" + self + "' admit '" + path.string() + "' " + Field(created, "bootstrap") + " " + Field(created, "service") + " " +
                                 std::to_string(page_size), &status);
  Check(status == 0 && admit.find("ADMITTED ok") != std::string::npos, tag + "admit child: " + admit.substr(0, 3000));
  for (std::size_t at = 0; (at = admit.find("INFO ", at)) != std::string::npos; at += 5)
    std::printf("%s%s\n", tag.c_str(), admit.substr(at, admit.find('\n', at) - at).c_str());
  for (int round = 1; round <= 2; ++round) {  // restart round trip: the second serve is a cold open of the same file
    const auto served = Capture("'" + self + "' serve '" + path.string() + "' adm " + Field(created, "database") + " " + Field(created, "bootstrap") +
                                    " " + Field(created, "service"), &status);
    Check(status == 0 && served.find("NAME_RESOLVE_OK") != std::string::npos, tag + "serve round " + std::to_string(round) + ": " + served.substr(0, 3000));
    Check(served.find(std::string(kBootstrapPassword)) == std::string::npos, tag + "no test password in any child output");
    for (std::size_t at = 0; (at = served.find("INFO ", at)) != std::string::npos; at += 5)
      std::printf("%sround %d %s\n", tag.c_str(), round, served.substr(at, served.find('\n', at) - at).c_str());
  }
  for (const auto& entry : fs::directory_iterator(dir))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, tag + "no .sb.* side file exists");
  std::printf("%stotal %.1f s\n", tag.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  fflush(stdout);
  std::error_code ignored;
  fs::remove_all(dir, ignored);  // the /tmp footprint
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "createempty") return CreateEmptyMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc == 6 && std::string(argv[1]) == "admit")
    return AdmitMode(argv[2], FromHex(argv[3]), FromHex(argv[4]), static_cast<std::uint32_t>(std::stoul(argv[5])));
  if (argc == 7 && std::string(argv[1]) == "serve") return ServeMode(argv[2], argv[3], FromHex(argv[4]), FromHex(argv[5]), FromHex(argv[6]));
  if (argc == 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    OneAdmissionSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])));
    artifacts.Cleanup();
    std::printf("native_name_admission: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
