// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SEARCH_KEY: SB_CATALOG_IDENTIFIER_CASE_FOLD (owner Q177, Q180-Q183,
// 2026-11-14). The legacy name-event registry (the live SQL resolver) matches
// and collides names by one rule: an entry's own-lane key (exact when quoted,
// folded when unquoted) against the lookup key (folded when unquoted, the text
// when quoted). Under the native profile (sbsql_v3) the fold is upper case,
// ASCII letters only. The entries here are built by MakeNameRegistryEntry,
// the constructor the registry's writers use, so their keys are the
// registry's own. Resolution itself (ResolveNameRegistryPrivate and the
// parser server's leaf lookup) applies NameRegistryEntryMatchesLookupKey to
// each candidate; that loop is not run here (it needs a database).
#include "catalog/name_registry.hpp"

#include <iostream>
#include <string>

namespace api = scratchbird::engine::internal_api;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL " << message << '\n'; }
}
api::NameRegistryEntry Entry(const std::string& text, bool quoted, const std::string& profile = "sbsql_v3") {
  api::EngineRequestContext context;
  context.identifier_profile_uuid = profile;
  api::EngineLocalizedName name;
  name.name = name.raw_name_text = name.display_name = text;
  name.was_quoted = quoted;
  name.default_name = true;
  return api::MakeNameRegistryEntry(context, {}, "table", {}, name, text);
}
// What the resolver computes for a presented name.
std::string Lookup(const std::string& text, bool quoted, const std::string& profile = "sbsql_v3") {
  return api::NameRegistryLookupKey(text, profile, quoted);
}
bool Finds(const std::string& text, bool quoted, const api::NameRegistryEntry& entry) {
  return api::NameRegistryEntryMatchesLookupKey(entry, Lookup(text, quoted, entry.identifier_profile_uuid));
}
}  // namespace

int main() {
  // Folding: native upper ASCII; donor labels keep their own.
  Check(Lookup("hELLo", false) == "HELLO", "native: an unquoted name folds to upper case");
  Check(Lookup("hELLo", true) == "hELLo", "native: a quoted name keeps its spelling");
  Check(Lookup("caf\xc3\xa9", false) == "CAF\xc3\xa9",
        "Q182: only ASCII letters fold; a UTF-8 letter is kept byte for byte");
  Check(Lookup("Hello", false, "firebird_v5") == "HELLO", "Firebird folds to upper case like the native profile");
  Check(Lookup("Hello", false, "postgresql") == "hello", "PostgreSQL folds to lower case (donor difference kept)");
  Check(Lookup("Hello", false, "") == "HELLO", "the empty label is the native profile");

  const auto unquoted_Hello = Entry("Hello", false);
  const auto unquoted_hello = Entry("hello", false);
  const auto quoted_HELLO = Entry("HELLO", true);
  const auto quoted_Hello = Entry("Hello", true);

  // Resolution.
  Check(Finds("hello", false, unquoted_Hello) && Finds("HELLO", false, unquoted_Hello) &&
            Finds("hELLO", false, unquoted_Hello),
        "Q177: unquoted hello, HELLO and hELLO all find an unquoted Hello");
  Check(Finds("hello", false, quoted_HELLO),
        "Q177: unquoted hello finds a quoted \"HELLO\" (was: an unquoted lookup skipped every quoted entry)");
  Check(!Finds("hello", false, quoted_Hello), "Q180: unquoted hello does not find a quoted-only \"Hello\"");
  Check(Finds("HELLO", true, unquoted_Hello),
        "Q181: quoted \"HELLO\" finds an unquoted-created Hello (was: not found, its exact key was Hello)");
  Check(!Finds("Hello", true, unquoted_Hello),
        "Q177: quoted \"Hello\" does not find an unquoted-created Hello (was: found)");
  Check(Finds("Hello", true, quoted_Hello) && !Finds("Hello", true, quoted_HELLO),
        "Q177: a quoted lookup finds only its own spelling among quoted names");

  // Collisions (the shared key lane).
  Check(api::NameRegistryLookupKeysCollide(unquoted_hello, unquoted_Hello),
        "Q177: unquoted hello and unquoted Hello are the same name");
  Check(!api::NameRegistryLookupKeysCollide(quoted_HELLO, quoted_Hello) &&
            !api::NameRegistryLookupKeysCollide(quoted_Hello, quoted_HELLO),
        "Q177: quoted \"HELLO\" and quoted \"Hello\" coexist");
  Check(api::NameRegistryLookupKeysCollide(unquoted_hello, quoted_HELLO) &&
            api::NameRegistryLookupKeysCollide(quoted_HELLO, unquoted_hello),
        "Q177: unquoted hello and quoted \"HELLO\" collide, in either order (was: never)");
  Check(!api::NameRegistryLookupKeysCollide(unquoted_hello, quoted_Hello) &&
            !api::NameRegistryLookupKeysCollide(quoted_Hello, unquoted_hello),
        "Q177: unquoted hello and quoted \"Hello\" coexist, in either order (was: a quoted \"Hello\" collided with "
        "an unquoted Hello by both exact keys)");
  Check(!api::NameRegistryLookupKeysCollide(Entry("\xc3\x89", false), Entry("\xc3\xa9", false)),
        "Q182: unquoted \xc3\x89 and \xc3\xa9 stay distinct (ASCII fold only)");

  // A donor profile keeps its own fold under the same rule.
  Check(api::NameRegistryLookupKeysCollide(Entry("Hello", false, "postgresql"), Entry("hello", true, "postgresql")) &&
            !api::NameRegistryLookupKeysCollide(Entry("Hello", false, "postgresql"), Entry("HELLO", true, "postgresql")),
        "PostgreSQL label: unquoted Hello is \"hello\", not \"HELLO\"");

  if (failures) { std::cerr << "FAIL name registry identifier case: " << failures << " of " << checks << '\n'; return 1; }
  std::cout << "PASS name registry identifier case " << checks << " checks\n";
  return 0;
}
