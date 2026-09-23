// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/dml/direct_physical_bulk_append.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace api = scratchbird::engine::internal_api;
namespace d = api::dml;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(unsigned n) { return scratchbird::tests::FixtureUuid(1183,n); }
template<class F> bool Refused(F f) {
  try { f(); return false; } catch (const std::invalid_argument&) { return true; }
}
int main() {
  auto identity=Id(1); identity.bytes[10]=0; identity.bytes[15]=255;
  const auto bytes=api::MetadataUuidBytes(identity);
  const auto bound=d::ParseDirectOptionIdentity(scratchbird::core::platform::UuidKind::filespace,bytes);
  Check(bound.valid() && bound.value==identity && d::TypedUuidIdentity(bound)==identity);
  Check(!d::ParseDirectOptionIdentity(scratchbird::core::platform::UuidKind::filespace,bytes.substr(1)).valid());
  Check(!d::ParseDirectOptionIdentity(scratchbird::core::platform::UuidKind::filespace,
      "019f0000-0000-7000-8000-000000000001").valid());
  api::CatalogColumnMetadata metadata;
  metadata.identities={{"constraint_uuid",identity},{"referenced_table_uuid",Id(2)}};
  metadata.text={{"referenced_column","parent_key"},{"constraint_mutation_batch_state","sealed"}};
  std::string descriptor;
  Check(api::EncodeCatalogColumnMetadata(metadata,&descriptor));
  const auto fields=d::DirectDescriptorFields(descriptor);
  api::CrudTableRecord table; table.table_uuid=Id(3);
  Check(d::DirectConstraintUuid(fields,table,"child_key","foreign_key")==identity);
  Check(d::DirectDescriptorDeclaresForeignKey(fields));
  const auto reference=d::DirectParseForeignKeyReference(fields);
  Check(reference && reference->parent_table_uuid==Id(2) && reference->parent_column=="parent_key");
  Check(Refused([&] { d::DirectDescriptorFields("constraint_uuid=019f0000-0000-7000-8000-000000000001"); }));
  Check(Refused([&] { d::DirectConstraintUuid({},table,"key","unique_key"); }));
  auto conflict=fields; conflict.identities["foreign_table_uuid"]=Id(4);
  Check(Refused([&] { d::DirectParseForeignKeyReference(conflict); }));
  std::vector<api::EngineEvidenceReference> evidence={{"row_page_allocation",identity},{"count","4"}};
  Check(d::FirstEvidenceIdentity(evidence,"row_page_allocation")==identity);
  Check(d::FirstEvidenceIdentity(evidence,"count").is_nil());
  Check(d::HasEvidence(evidence,"count","4"));
  Check(!d::HasEvidence(evidence,"row_page_allocation",bytes));
  api::DmlPageAllocationRuntimeResult allocation;
  allocation.evidence={{"count",identity}};
  Check(d::DirectAllocationEvidenceU64(allocation,"count")==0);
  allocation.evidence={{"count","42"}};
  Check(d::DirectAllocationEvidenceU64(allocation,"count")==42);
}
