// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_PARSER_NAME_RESOLUTION (Part B step S1). Unit tests of the
// parser-side name resolver (parsers/shared/name_resolution). Every case
// encodes what the ENGINE'S resolver does today with the same inputs
// (name_registry.cpp), because S1 must not change a result; the same table runs
// against the real engine on production-created native databases in
// native_name_resolve_test (the differential half). Cases marked TODAY record
// a behaviour the owner's rulings do not describe (listed for S2 in the S1
// report); they are asserted as they are, not as they should be.

#include "name_resolution/name_resolution.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace nr = scratchbird::parsers::name_resolution;

namespace {
int checks = 0;
int failures = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::cerr << "FAIL " << message << '\n';
  }
}

nr::Uuid16 Id(std::uint8_t n) {
  nr::Uuid16 id{};
  id[15] = n;
  return id;
}

// An in-memory source: the stored names, listed by (scope, language).
class FixedSource final : public nr::NameSource {
 public:
  std::vector<nr::NameEntry> entries;
  std::size_t lists = 0;
  std::vector<nr::NameEntry> List(const nr::Uuid16& scope, std::string_view language) override {
    ++lists;
    std::vector<nr::NameEntry> out;
    for (const auto& entry : entries)
      if (entry.scope == scope && entry.language == language) out.push_back(entry);
    return out;
  }
  void Add(nr::Uuid16 scope, nr::Uuid16 object, std::string language, std::string object_class, std::string text,
           bool quoted) {
    entries.push_back({scope, object, std::move(language), std::move(object_class), std::move(text), quoted});
  }
};

nr::ResolveRequest Typed(std::vector<nr::NameComponent> path, std::string cls = {}) {
  nr::ResolveRequest request;
  request.path = std::move(path);
  if (!cls.empty()) request.accepts_class = [cls](const std::string& c) { return c == cls; };
  return request;
}
nr::NameComponent U(std::string text) { return {std::move(text), false}; }
nr::NameComponent Q(std::string text) { return {std::move(text), true}; }

const nr::NameFoldRule& Sbsql() { return nr::SbsqlNameFoldRule(); }

// A lower-case rule: what a PostgreSQL-style parser would supply. It exists to
// show the rule is an interface; no SBsql path uses it.
class LowerFold final : public nr::NameFoldRule {
 public:
  std::string_view Label() const noexcept override { return "test_lower"; }
  std::string Key(std::string_view spelling, bool quoted) const override {
    std::string key(spelling);
    if (quoted) return key;
    for (auto& ch : key)
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return key;
  }
};

nr::ResolveOutcome Run(FixedSource& source, const nr::ResolutionContext& context, const nr::ResolveRequest& request,
                       const nr::NameFoldRule& rule = Sbsql()) {
  nr::NameListCache cache(source);
  return nr::Resolve(rule, cache, context, request);
}

nr::ResolutionContext In(nr::Uuid16 scope, std::string language = "en") {
  nr::ResolutionContext context;
  context.target_schema = scope;
  context.session_language = language;
  context.default_language = language;
  return context;
}
}  // namespace

int main() {
  const auto s1 = Id(1);
  const auto s2 = Id(2);

  // ---- 1. The fold: unquoted -> UPPER ASCII, quoted exact -----------------------------------------------------------
  Check(nr::SbsqlIdentifierKey("hELLo", false) == "HELLO", "unquoted folds to upper case");
  Check(nr::SbsqlIdentifierKey("hELLo", true) == "hELLo", "quoted keeps its spelling");
  Check(nr::SbsqlIdentifierKey("caf\xc3\xa9", false) == "CAF\xc3\xa9", "Q182: only ASCII letters fold");
  Check(nr::SbsqlIdentifierKey("\xc3\xa9", false) != nr::SbsqlIdentifierKey("\xc3\x89", false),
        "Q182: e-acute and E-acute stay distinct");
  Check(nr::SbsqlIdentifierKey("", false).empty() && nr::SbsqlIdentifierKey("", true).empty(), "the empty name has the empty key");
  Check(Sbsql().Key("a_1$", false) == "A_1$" && Sbsql().Label() == "sbsql_v3", "the rule object is the same fold");
  Check(LowerFold().Key("HeLLo", false) == "hello", "another dialect supplies its own rule");

  // ---- 2. Matching: the key against the stored name's own lane ---------------------------------------------------------
  {
    FixedSource source;
    source.Add(s1, Id(10), "en", "table", "Hello", false);  // created unquoted: lane HELLO
    const auto ctx = In(s1);
    for (const char* spelling : {"hello", "HELLO", "hELLO", "Hello"}) {
      const auto o = Run(source, ctx, Typed({U(spelling)}));
      Check(o.status == nr::ResolveStatus::found && o.entry.object == Id(10), "unquoted spellings find an unquoted-created Hello");
    }
    Check(Run(source, ctx, Typed({Q("HELLO")})).status == nr::ResolveStatus::found,
          "Q181: quoted HELLO finds an unquoted-created Hello");
    Check(Run(source, ctx, Typed({Q("Hello")})).status == nr::ResolveStatus::not_found,
          "Q177: quoted Hello does not find an unquoted-created Hello");
    Check(Run(source, ctx, Typed({Q("hello")})).status == nr::ResolveStatus::not_found, "quoted hello does not find it either");
  }
  {
    FixedSource source;
    source.Add(s1, Id(11), "en", "table", "Hello", true);  // created quoted: lane Hello
    const auto ctx = In(s1);
    Check(Run(source, ctx, Typed({U("hello")})).status == nr::ResolveStatus::not_found,
          "Q180: unquoted hello does not find a quoted-only Hello");
    Check(Run(source, ctx, Typed({U("HELLO")})).status == nr::ResolveStatus::not_found,
          "Q180: unquoted HELLO does not find a quoted-only Hello");
    Check(Run(source, ctx, Typed({Q("Hello")})).status == nr::ResolveStatus::found, "quoted Hello finds it");
    Check(Run(source, ctx, Typed({Q("HELLO")})).status == nr::ResolveStatus::not_found, "quoted HELLO does not find it");
  }
  {
    FixedSource source;
    source.Add(s1, Id(12), "en", "table", "HELLO", true);  // created quoted upper: lane HELLO
    const auto ctx = In(s1);
    Check(Run(source, ctx, Typed({U("hello")})).status == nr::ResolveStatus::found && Run(source, ctx, Typed({U("hello")})).entry.object == Id(12),
          "Q177: unquoted hello finds a quoted HELLO");
  }

  // ---- 3. Twins: the shared key lane ------------------------------------------------------------------------------------
  {
    const auto entry = [](const char* text, bool quoted) { return nr::NameEntry{{}, {}, "en", "table", text, quoted}; };
    Check(nr::NamesCollide(Sbsql(), entry("hello", false), entry("Hello", false)), "unquoted hello and Hello are one name");
    Check(!nr::NamesCollide(Sbsql(), entry("HELLO", true), entry("Hello", true)), "quoted HELLO and Hello coexist");
    Check(nr::NamesCollide(Sbsql(), entry("hello", false), entry("HELLO", true)) &&
              nr::NamesCollide(Sbsql(), entry("HELLO", true), entry("hello", false)),
          "unquoted hello and quoted HELLO collide, in either order");
    Check(!nr::NamesCollide(Sbsql(), entry("hello", false), entry("Hello", true)) &&
              !nr::NamesCollide(Sbsql(), entry("Hello", true), entry("hello", false)),
          "unquoted hello and quoted Hello coexist, in either order");
    Check(!nr::NamesCollide(Sbsql(), entry("\xc3\x89", false), entry("\xc3\xa9", false)), "ASCII fold only");
    Check(nr::OwnLane(Sbsql(), entry("Hello", false)) == "HELLO" && nr::OwnLane(Sbsql(), entry("Hello", true)) == "Hello",
          "the own lane: the fold when created unquoted, the spelling when created quoted");
  }
  {
    // Two twins that coexist (both quoted) each find their own object, quoted; unquoted finds only the upper twin.
    FixedSource source;
    source.Add(s1, Id(20), "en", "table", "TwinName", true);
    source.Add(s1, Id(21), "en", "table", "TWINNAME", true);
    const auto ctx = In(s1);
    Check(Run(source, ctx, Typed({Q("TwinName")})).entry.object == Id(20), "the quoted mixed twin");
    Check(Run(source, ctx, Typed({Q("TWINNAME")})).entry.object == Id(21), "the quoted upper twin");
    Check(Run(source, ctx, Typed({Q("twinname")})).status == nr::ResolveStatus::not_found, "a third spelling, quoted");
    const auto unquoted = Run(source, ctx, Typed({U("twinname")}));
    Check(unquoted.status == nr::ResolveStatus::found && unquoted.entry.object == Id(21), "unquoted folds to the upper twin");
  }

  // ---- 4. Scope candidates and search-path order ------------------------------------------------------------------------
  {
    nr::ResolutionContext ctx;
    ctx.target_schema = s1;
    ctx.current_schema = s1;
    ctx.search_path = {s2, s1, Id(3)};
    ctx.default_root = Id(9);
    const auto scopes = nr::ScopeCandidates(ctx);
    Check(scopes.size() == 5 && scopes[0] == s1 && scopes[1] == s2 && scopes[2] == Id(3) && scopes[3] == Id(9) &&
              nr::IsNil(scopes[4]),
          "scopes: target, current, search path, default root, nil; duplicates dropped");
    Check(nr::ScopeCandidates(nr::ResolutionContext{}).size() == 1,
          "no scope at all is the nil scope only (what the wire gives the engine today: it sends no scope UUIDs)");
  }
  {
    // The one name in the second search-path schema is found; the same name in two schemas of the same language is an
    // ambiguity (TODAY: the engine has no quoted-first stage; the owner's quoted-first rule is listed for S2).
    FixedSource source;
    source.Add(s2, Id(30), "en", "table", "orders", false);
    nr::ResolutionContext ctx = In(s1);
    ctx.search_path = {s2};
    const auto found = Run(source, ctx, Typed({U("Orders")}));
    Check(found.status == nr::ResolveStatus::found && found.entry.object == Id(30), "a name in a later search-path schema is found");
    Check(Run(source, In(s1), Typed({U("orders")})).status == nr::ResolveStatus::not_found,
          "a schema that is not on the path is not searched");
    source.Add(s1, Id(31), "en", "table", "ORDERS", false);
    const auto twice = Run(source, ctx, Typed({U("orders")}));
    Check(twice.status == nr::ResolveStatus::ambiguous && twice.objects.size() == 2 && twice.objects[0] == Id(31) &&
              twice.objects[1] == Id(30),
          "TODAY: the same name in two scopes is an ambiguity (objects in scope order), not the first scope's name");
    Check(Run(source, ctx, Typed({Q("ORDERS")})).status == nr::ResolveStatus::ambiguous,
          "TODAY: quoted ORDERS is equally ambiguous: no quoted-first stage exists in the engine");
    source.Add(s1, Id(32), "en", "table", "Orders", true);  // quoted-only Mixed: lane Orders
    Check(Run(source, ctx, Typed({Q("Orders")})).entry.object == Id(32), "a quoted spelling finds only its own lane");
  }
  {
    // The nil scope is searched last.
    FixedSource source;
    source.Add(nr::Uuid16{}, Id(33), "en", "table", "rootname", false);
    Check(Run(source, In(s1), Typed({U("ROOTNAME")})).entry.object == Id(33), "the nil scope is a candidate");
  }

  // ---- 5. Language fallback: session, database default, "und"; the first language with a match wins ------------------
  {
    const auto langs = [](std::string session, std::string def) {
      nr::ResolutionContext c;
      c.session_language = std::move(session);
      c.default_language = std::move(def);
      return nr::LanguageCandidates(c);
    };
    Check((langs("fr-CA", "en") == std::vector<std::string>{"fr-CA", "en", "und"}), "session, default, und");
    Check((langs("en", "en") == std::vector<std::string>{"en", "und"}), "a session in the default language is not tried twice");
    Check((langs("", "") == std::vector<std::string>{"en", "und"}), "no languages at all: the engine default en, then und");
    Check((langs("", "de-DE") == std::vector<std::string>{"de-DE", "und"}), "no session language: the default");
    Check((langs("fr-CA", "und") == std::vector<std::string>{"fr-CA", "und"}), "und is not repeated");

    FixedSource source;
    source.Add(s1, Id(40), "en", "table", "livre", false);
    source.Add(s1, Id(41), "fr-CA", "table", "livre", false);
    source.Add(s1, Id(42), "und", "table", "neutral", false);
    source.Add(s1, Id(43), "en", "table", "anglais", false);
    nr::ResolutionContext ctx = In(s1);
    ctx.session_language = "fr-CA";
    ctx.default_language = "en";
    const auto livre = Run(source, ctx, Typed({U("livre")}));
    Check(livre.status == nr::ResolveStatus::found && livre.entry.object == Id(41),
          "the first language with any match wins: the session language's name, not an ambiguity with the default's");
    Check(Run(source, ctx, Typed({U("anglais")})).entry.object == Id(43), "falls back to the database default language");
    Check(Run(source, ctx, Typed({U("neutral")})).entry.object == Id(42), "falls back to und");
    ctx.session_language = "de-DE";
    ctx.default_language = "de-DE";
    Check(Run(source, ctx, Typed({U("anglais")})).status == nr::ResolveStatus::not_found,
          "a language that is neither the session's, the default or und is never searched");
  }

  // ---- 6. Qualified paths ----------------------------------------------------------------------------------------------
  {
    FixedSource source;
    const auto root = Id(50);
    source.Add(root, s1, "en", "schema", "app", false);
    source.Add(root, s2, "en", "schema", "Mixed", true);
    source.Add(s1, Id(51), "en", "table", "t", false);
    source.Add(s2, Id(52), "en", "table", "t", false);
    nr::ResolutionContext ctx = In(root);
    Check(Run(source, ctx, Typed({U("APP"), U("T")})).entry.object == Id(51), "schema.table, both unquoted");
    Check(Run(source, ctx, Typed({Q("Mixed"), U("t")})).entry.object == Id(52), "a quoted schema step is exact");
    Check(Run(source, ctx, Typed({U("mixed"), U("t")})).status == nr::ResolveStatus::not_found,
          "an unquoted schema step does not find a quoted-only Mixed");
    Check(Run(source, ctx, Typed({U("nosuch"), U("t")})).status == nr::ResolveStatus::not_found, "an unknown schema");
    Check(Run(source, ctx, Typed({U("app"), U("nosuch")})).status == nr::ResolveStatus::not_found, "an unknown leaf");
    // A qualifier step looks at schema entries only.
    source.Add(root, Id(53), "en", "table", "tbl_not_a_schema", false);
    Check(Run(source, ctx, Typed({U("tbl_not_a_schema"), U("t")})).status == nr::ResolveStatus::not_found,
          "a qualifier step that is not a schema finds nothing");
    // The leaf class filter.
    Check(Run(source, ctx, Typed({U("app"), U("t")}, "view")).status == nr::ResolveStatus::not_found, "the class filter");
    Check(Run(source, ctx, Typed({U("app"), U("t")}, "table")).entry.object == Id(51), "the class filter accepts");
    // The keys sent per step.
    const auto o = Run(source, ctx, Typed({U("app"), Q("T")}));
    Check(o.keys.size() == 2 && o.keys[0] == "APP" && o.keys[1] == "T", "the key of each step: folded when unquoted, exact when quoted");
  }

  // ---- 7. Empty names: passed through, nothing invented ----------------------------------------------------------------
  {
    FixedSource source;
    source.Add(s1, Id(60), "en", "table", "x", false);
    Check(Run(source, In(s1), Typed({})).status == nr::ResolveStatus::invalid_request, "no path at all: the engine's name_required");
    Check(Run(source, In(s1), Typed({U("")})).status == nr::ResolveStatus::invalid_request, "an empty leaf: the engine's name_required");
    Check(Run(source, In(s1), Typed({U(""), U("x")})).status == nr::ResolveStatus::not_found,
          "an empty qualifier step finds nothing (the engine's scope walk gives no scope)");
    Check(Run(source, In(s1), Typed({U("a"), U("")})).status == nr::ResolveStatus::invalid_request, "an empty leaf under a qualifier");
    Check(!nr::SplitPresentedName("", false) && !nr::SplitPresentedName("a..b", false) && !nr::SplitPresentedName(".a", false) &&
              !nr::SplitPresentedName("a.", false) && !nr::SplitPresentedName("\"a", false),
          "the text splitter refuses an empty component and an open quote, as the server's does today");
    // The empty-quoted name "" (TODAY the server's splitter refuses it as an empty component).
    Check(!nr::SplitPresentedName("\"\"", false), "TODAY: the quoted empty name is refused by the splitter (empty component)");
    const auto empty = nr::PrepareSbsqlWireName("", false, "relation");
    Check(!empty.rewritten && empty.presented_name.empty() && !empty.quoted, "the empty name is sent unchanged: the server decides, as today");
    const auto bad = nr::PrepareSbsqlWireName("a..b", false, "relation");
    Check(!bad.rewritten && bad.presented_name == "a..b", "text that does not split is sent unchanged");
    const auto open = nr::PrepareSbsqlWireName("\"abc", false, "relation");
    Check(!open.rewritten && open.presented_name == "\"abc", "an open quote is sent unchanged");
  }

  // ---- 8. Dotted text: quoting, dots inside quotes, what is put on the wire ---------------------------------------------
  {
    auto split = nr::SplitPresentedName("\"a.b\".c", false);
    Check(split && split->size() == 2 && (*split)[0] == nr::NameComponent{"a.b", true} && (*split)[1] == nr::NameComponent{"c", false},
          "a dot inside quotes belongs to the name");
    split = nr::SplitPresentedName("\"x\"\"y\"", false);
    Check(split && split->size() == 1 && (*split)[0] == nr::NameComponent{"x\"y", true}, "a doubled quote is one quote");
    split = nr::SplitPresentedName("a.b", true);
    Check(split && (*split)[0].quoted && (*split)[1].quoted, "a name marked quoted makes every component quoted");
    split = nr::SplitPresentedName("s.\"T\"", false);
    Check(split && !(*split)[0].quoted && (*split)[1].quoted, "quoting is per component");
    Check(nr::RenderPresentedName({{"a.b", true}, {"c", false}}) == "\"a.b\".c", "render: quoted components in quotes");
    Check(nr::RenderPresentedName({{"x\"y", true}}) == "\"x\"\"y\"", "render: an inner quote is doubled");

    // What goes on the wire.
    const auto plain = nr::PrepareSbsqlWireName("orders", false, "relation");
    Check(plain.presented_name == "ORDERS" && !plain.quoted && plain.rewritten && plain.original == "orders",
          "an unquoted name goes on the wire folded");
    const auto sys = nr::PrepareSbsqlWireName("sys.Version", false, "relation");
    Check(sys.presented_name == "SYS.VERSION", "every unquoted component is folded");
    const auto mixed = nr::PrepareSbsqlWireName("app.\"Orders\"", false, "relation");
    Check(mixed.presented_name == "APP.\"Orders\"", "a quoted component stays exact, an unquoted one folds");
    const auto dotted = nr::PrepareSbsqlWireName("\"a.b\".c", false, "relation");
    Check(dotted.presented_name == "\"a.b\".C", "a quoted component with a dot keeps its quotes");
    const auto marked = nr::PrepareSbsqlWireName("abc", true, "relation");
    Check(marked.presented_name == "\"abc\"" && marked.quoted, "a name marked quoted is exact; the mark is passed through");
    const auto already = nr::PrepareSbsqlWireName("ORDERS", false, "relation");
    Check(!already.rewritten && already.presented_name == "ORDERS", "a folded name is not rewritten (idempotent)");
    Check(nr::PrepareSbsqlWireName(already.presented_name, false, "relation").presented_name == already.presented_name,
          "preparing a prepared name changes nothing");
    const auto charset = nr::PrepareSbsqlWireName("utf8", false, "charset");
    const auto collation = nr::PrepareSbsqlWireName("unicode_ci", false, "Collation");
    Check(!charset.rewritten && charset.presented_name == "utf8" && !collation.rewritten,
          "resource names (charset, collation) are the engine's resource catalogue's: sent as typed");
    Check(plain.RestoreEcho("ORDERS") == "orders" && plain.RestoreEcho("public.Orders") == "public.Orders" &&
              charset.RestoreEcho("utf8") == "utf8",
          "the echo of the text sent is mapped back to the typed text; an engine-owned name is kept");

    // The prepared text, split again, gives the same keys the typed text gives.
    for (const char* typed : {"orders", "Orders", "a.b", "\"Mixed\".t", "s.\"x.y\".Z", "\"q\"\"q\""}) {
      const auto typed_split = nr::SplitPresentedName(typed, false);
      const auto sent = nr::PrepareSbsqlWireName(typed, false, "relation");
      const auto sent_split = nr::SplitPresentedName(sent.presented_name, sent.quoted);
      bool same = typed_split && sent_split && typed_split->size() == sent_split->size();
      for (std::size_t i = 0; same && i < typed_split->size(); ++i)
        same = Sbsql().Key((*typed_split)[i].spelling, (*typed_split)[i].quoted) ==
               Sbsql().Key((*sent_split)[i].spelling, (*sent_split)[i].quoted);
      Check(same, "the wire text has the same component keys as the typed text");
    }
  }

  // ---- 9. The list cache and its refresh hooks ----------------------------------------------------------------------------
  {
    FixedSource source;
    source.Add(s1, Id(70), "en", "table", "cached", false);
    nr::NameListCache cache(source);
    const auto ctx = In(s1);
    const auto request = Typed({U("cached")});
    Check(nr::Resolve(Sbsql(), cache, ctx, request).status == nr::ResolveStatus::found, "found through the cache");
    const auto reads = cache.source_reads();
    Check(nr::Resolve(Sbsql(), cache, ctx, request).status == nr::ResolveStatus::found && cache.source_reads() == reads,
          "the second lookup reads no page");
    source.Add(s1, Id(71), "en", "table", "later", false);
    Check(nr::Resolve(Sbsql(), cache, ctx, Typed({U("later")})).status == nr::ResolveStatus::not_found,
          "a name created after the page was read is not seen until the cache refreshes (the snapshot)");
    cache.OnTransactionEnd();
    Check(cache.cached_pages() == 0 && nr::Resolve(Sbsql(), cache, ctx, Typed({U("later")})).status == nr::ResolveStatus::found,
          "after a commit or rollback the pages are read again");
    cache.ObserveGeneration(5);
    Check(cache.cached_pages() > 0, "the first generation seen keeps the pages");
    cache.ObserveGeneration(5);
    Check(cache.cached_pages() > 0, "the same generation keeps the pages");
    cache.ObserveGeneration(6);
    Check(cache.cached_pages() == 0, "a changed generation drops the pages");
  }

  // ---- 10. Another dialect's rule, through the same resolver -----------------------------------------------------------
  {
    FixedSource source;
    source.Add(s1, Id(80), "en", "table", "hello", true);  // created quoted: lane hello under either rule
    const LowerFold lower;
    Check(Run(source, In(s1), Typed({U("HELLO")}), lower).status == nr::ResolveStatus::found,
          "the lower rule finds a quoted hello from unquoted HELLO");
    Check(Run(source, In(s1), Typed({U("HELLO")}), Sbsql()).status == nr::ResolveStatus::not_found,
          "the SBsql rule does not: its key HELLO is not the lane hello");
  }

  if (failures != 0) {
    std::cerr << "FAIL sbsql name resolution: " << failures << " of " << checks << '\n';
    return 1;
  }
  std::cout << "PASS sbsql name resolution " << checks << " checks\n";
  return 0;
}
