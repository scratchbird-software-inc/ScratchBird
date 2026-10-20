// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Proves the transport binding is assembled only from identities the database
// actually committed, and that every incomplete committed state is a refusal
// rather than a partially populated binding. A caller must not be able to
// proceed on a binding whose provider or credential reference was defaulted,
// so no such binding is ever produced.

#include "../../src/server/startup_credential_binding.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace db = scratchbird::storage::database;
using namespace scratchbird::server;
using E = StartupCredentialBindingError;
using Uuid = scratchbird::core::platform::Uuid;

namespace {

int checks = 0;

void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) {
    std::fprintf(stderr, "FAIL startup credential binding: %s\n", what);
    std::exit(1);
  }
}

Uuid Id(std::uint8_t tail) {
  Uuid value{{0x01, 0x8f, 0x7a, 0x10, 0x12, 0x80, 0x70, 0x00, 0x80, 0x00, 0x00,
              0x00, 0x00, 0x00, 0x00, 0x00}};
  value.bytes[15] = tail;
  return value;
}

// A committed state carrying a complete runtime authority binding, as a
// database created with an internal service principal produces.
db::DatabaseBootstrapSecurityCatalogState Committed() {
  db::DatabaseBootstrapSecurityCatalogState state;
  state.present = true;
  state.committed_by_inventory = true;
  state.runtime_authority_binding_present = true;
  state.runtime_binding_uuid = Id(0x40);
  state.service_principal_uuid = Id(0x41);
  state.provider_uuid = Id(0x42);
  state.credential_reference_uuid = Id(0x43);
  state.runtime_authority_generation = 5;
  return state;
}

}  // namespace

int main() {
  const auto database = Id(0x44);

  // 1. A complete committed state yields a binding whose every identity is the
  //    committed one. Asserted field by field: a binding that happened to be
  //    well formed while carrying the wrong provider would still be wrong.
  {
    const auto out = BuildStartupCredentialBinding(Committed(), database);
    Check(out.ok(), "a complete committed state yields a binding");
    Check(out.binding.database == database, "database is the opened database");
    Check(out.binding.principal == Id(0x41),
          "principal is the committed service principal");
    Check(out.binding.provider == Id(0x42), "provider is the committed provider");
    Check(out.binding.credential_reference == Id(0x43),
          "credential reference is the committed one");
    Check(out.binding.generation == 5,
          "generation is the committed binding generation");
    Check(out.binding.custodian_uid.has_value() &&
              *out.binding.custodian_uid ==
                  static_cast<std::uint32_t>(::geteuid()),
          "custodian is this effective user, the one field that is not catalog "
          "state because it is not an authority claim");
    Check(ValidStartupCredentialFrameBinding(out.binding),
          "the produced binding is acceptable to the frame encoder");
  }

  // 2. Every incomplete committed state refuses, and refuses with an empty
  //    binding. Each case removes exactly one thing, so a dropped check cannot
  //    hide behind another.
  {
    struct Case {
      const char* what;
      db::DatabaseBootstrapSecurityCatalogState state;
      E expected;
    };

    auto no_binding = Committed();
    no_binding.runtime_authority_binding_present = false;
    auto no_reference = Committed();
    no_reference.credential_reference_uuid.reset();
    auto no_generation = Committed();
    no_generation.runtime_authority_generation = 0;
    auto not_present = Committed();
    not_present.present = false;
    auto not_committed = Committed();
    not_committed.committed_by_inventory = false;
    // A committed row whose provider is not an engine identity must not be
    // framed; the refusal belongs where the row is read, not at encode time.
    auto bad_provider = Committed();
    bad_provider.provider_uuid = Uuid{};

    const Case cases[] = {
        {"no committed runtime authority binding refuses", no_binding,
         E::binding_absent},
        {"a binding with no credential reference refuses", no_reference,
         E::credential_reference_absent},
        {"a binding with generation zero refuses", no_generation,
         E::generation_absent},
        {"security rows absent refuses", not_present, E::binding_absent},
        {"security rows not proven committed refuses", not_committed,
         E::binding_absent},
        {"a malformed committed provider identity refuses", bad_provider,
         E::binding_absent},
    };

    for (const auto& c : cases) {
      const auto out = BuildStartupCredentialBinding(c.state, database);
      Check(out.error == c.expected, c.what);
      Check(!out.ok(), "a refusal never reports a usable binding");
      Check(!ValidStartupCredentialFrameBinding(out.binding),
            "a refusal leaves no frameable binding behind");
      Check(StartupCredentialBindingDiagnosticCode(out.error)[0] != '\0',
            "a refusal carries a diagnostic code");
    }
  }

  std::printf("PASS startup credential binding checks=%d\n", checks);
  return 0;
}
