// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// The repeated unit of the definitions vector that
// NativeInitialPolicySecurityRequest requires. Each identity conjunct is
// asserted separately, because a metadata envelope built from a bad identity
// encodes cleanly and is rejected much later by MatchesMetadata, where it
// presents as a missing object rather than a malformed one.

#include "../../src/storage/database/native_bootstrap_definition_metadata.hpp"

#include <cstdio>
#include <cstdlib>

namespace db = scratchbird::storage::database;
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
using E = db::BootstrapDefinitionMetadataError;

namespace {
int checks = 0;
void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) { std::fprintf(stderr, "FAIL bootstrap definition metadata: %s\n", what); std::exit(1); }
}
p::Uuid Id(std::uint8_t tail) {
  p::Uuid v{{0x01,0x8f,0x7a,0x10,0x12,0x80,0x70,0x00,0x80,0,0,0,0,0,0,0}};
  v.bytes[15] = tail; return v;
}
db::BootstrapDefinitionMetadataInput Good() {
  db::BootstrapDefinitionMetadataInput in;
  in.object_uuid = Id(0x51);
  in.row_uuid = Id(0x57);
  in.kind = c::CatalogRecordKind::config_profile;
  in.object_subtype = "agent_runtime_authority";
  in.owner_principal_uuid = Id(0x52);
  in.audit_object_uuid = Id(0x53);
  in.security_policy_uuid = Id(0x54);
  in.owning_schema_uuid = Id(0x55);
  in.creator_transaction_uuid = {p::UuidKind::transaction, Id(0x56)};
  return in;
}
}  // namespace

int main() {
  // 1. A complete input yields an envelope with every field the strict matcher
  //    reads, under the kind it reads it as.
  {
    const auto out = db::BuildBootstrapDefinitionMetadata(Good());
    Check(out.ok(), "a complete input builds");
    const auto& m = out.metadata;
    Check(m.record.header.kind == c::CatalogRecordKind::config_profile, "kind preserved");
    Check(m.record.header.object_uuid.value == Id(0x51) &&
          m.record.header.object_uuid.kind == p::UuidKind::object, "object identity and kind");
    Check(m.record.header.row_uuid.kind == p::UuidKind::row &&
          m.record.header.row_uuid.value == Id(0x57),
          "row identity is present and distinct; the request refuses duplicates "
          "across every definition, vector and entry identity");
    Check(m.record.header.row_uuid.value != m.record.header.object_uuid.value,
          "row identity differs from object identity");
    Check(m.record.payload.empty(),
          "payload stays empty; the assembler refuses a supplied payload");
    Check(m.object_subtype == "agent_runtime_authority", "subtype preserved verbatim");
    Check(m.owner_uuid.kind == p::UuidKind::principal && m.owner_uuid.value == Id(0x52),
          "owner is a principal identity");
    Check(m.audit_uuid.kind == p::UuidKind::object && m.audit_uuid.value == Id(0x53),
          "audit is an object identity");
    Check(m.security_policy_uuid.kind == p::UuidKind::object &&
          m.security_policy_uuid.value == Id(0x54), "security policy is an object identity");
    Check(m.owning_schema_uuid.kind == p::UuidKind::schema &&
          m.owning_schema_uuid.value == Id(0x55), "owning schema carries schema kind");
    Check(m.record.header.parent_uuid.kind == p::UuidKind::object &&
          m.record.header.parent_uuid.value == Id(0x55),
          "parent carries the same value under object kind");
    Check(m.owning_schema_uuid.value == m.record.header.parent_uuid.value,
          "schema and parent values are equal, as the strict matcher requires");
    Check(m.authority_scope == c::CatalogAuthorityScope::local, "authority scope local");
    Check(m.visibility == c::CatalogVisibilityClass::internal_only, "visibility internal_only");
    Check(m.lifecycle == c::CatalogObjectLifecycle::active, "lifecycle active");
    Check(m.status == c::CatalogObjectStatus::active, "status active");
    Check(m.creator_transaction_uuid.kind == p::UuidKind::transaction,
          "creator transaction carries transaction kind");
  }

  // 2. Each identity is refused on its own. One combined validity check would
  //    let a dropped conjunct hide behind the others.
  {
    struct Case { const char* what; db::BootstrapDefinitionMetadataInput in; E expected; };
    auto nil_object = Good(); nil_object.object_uuid = {};
    auto nil_row = Good(); nil_row.row_uuid = {};
    auto same_row = Good(); same_row.row_uuid = same_row.object_uuid;
    auto no_subtype = Good(); no_subtype.object_subtype.clear();
    auto nil_owner = Good(); nil_owner.owner_principal_uuid = {};
    auto nil_audit = Good(); nil_audit.audit_object_uuid = {};
    auto nil_policy = Good(); nil_policy.security_policy_uuid = {};
    auto nil_schema = Good(); nil_schema.owning_schema_uuid = {};
    auto wrong_tx_kind = Good(); wrong_tx_kind.creator_transaction_uuid = {p::UuidKind::object, Id(0x56)};
    auto zero_gen = Good(); zero_gen.definition_version = 0;

    const Case cases[] = {
      {"a nil object identity refuses", nil_object, E::object_identity_invalid},
      {"a nil row identity refuses", nil_row, E::row_identity_invalid},
      {"a row identity equal to the object identity refuses", same_row, E::row_identity_invalid},
      {"an empty subtype refuses", no_subtype, E::subtype_empty},
      {"a nil owner refuses", nil_owner, E::owner_identity_invalid},
      {"a nil audit identity refuses", nil_audit, E::audit_identity_invalid},
      {"a nil security policy refuses", nil_policy, E::policy_identity_invalid},
      {"a nil owning schema refuses", nil_schema, E::schema_identity_invalid},
      {"a creator transaction of the wrong kind refuses", wrong_tx_kind, E::transaction_identity_invalid},
      {"a zero definition version refuses", zero_gen, E::generation_invalid},
    };
    for (const auto& k : cases) {
      const auto out = db::BuildBootstrapDefinitionMetadata(k.in);
      Check(out.error == k.expected, k.what);
      Check(!out.ok(), "a refusal never reports usable metadata");
      Check(db::BootstrapDefinitionMetadataDiagnosticCode(out.error)[0] != '\0',
            "a refusal carries a diagnostic code");
    }
  }

  std::printf("PASS bootstrap definition metadata checks=%d\n", checks);
  return 0;
}
