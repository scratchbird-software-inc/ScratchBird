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
  // Lookup absence is nil, not a generated identity or an exception. The
  // production proof builder below must refuse that absence before any proof.
  Check(d::DirectConstraintUuid({},table,"key","unique_key").is_nil());
  Check(d::DirectConstraintUuid(fields,table,"key","primary_key").is_nil());
  Check(d::DirectConstraintUuid(fields,table,"key","unique_key").is_nil());
  auto candidate_fields=fields;
  candidate_fields.identities["candidate_key_constraint_uuid"]=Id(5);
  Check(d::DirectConstraintUuid(candidate_fields,table,"key","unique_key")==Id(5));
  Check(d::DirectConstraintUuid(candidate_fields,table,"key","primary_key")==Id(5));
  Check(d::DirectConstraintUuid(candidate_fields,table,"key","foreign_key")==identity);
  for (auto invalid : {api::EngineUuid{},Id(6)}) {
    if (!invalid.is_nil()) invalid.bytes[6]=0x40;
    auto malformed=candidate_fields;
    malformed.identities["candidate_key_constraint_uuid"]=invalid;
    Check(Refused([&] { d::DirectConstraintUuid(malformed,table,"key","unique_key"); }));
  }
  auto text_candidate=candidate_fields;
  text_candidate["candidate_key_constraint_uuid"]="019f0000-0000-7000-8000-000000000005";
  Check(Refused([&] { d::DirectConstraintUuid(text_candidate,table,"key","unique_key"); }));
  api::CatalogColumnMetadata missing_candidate;
  missing_candidate.text={{"unique","true"}};
  missing_candidate.identities={{"support_uuid",Id(7)},{"constraint_uuid",identity}};
  std::string missing_descriptor;
  Check(api::EncodeCatalogColumnMetadata(missing_candidate,&missing_descriptor));
  auto unbound_table=table;
  unbound_table.columns={{"key",missing_descriptor}};
  api::CrudIndexRecord support;
  support.index_uuid=Id(7); support.table_uuid=table.table_uuid;
  support.column_name="key"; support.unique=true;
  d::DirectPhysicalBulkAppendRequest request;
  request.context.database_uuid=Id(8);
  request.context.transaction_uuid=Id(9);
  request.context.local_transaction_id=1;
  request.target_table.uuid=table.table_uuid;
  api::MgaRelationReadView state;
  const auto refused_proof=d::BuildDirectBulkConstraintProof(request,state,unbound_table,
      {support},{},{},false,nullptr,nullptr);
  Check(!refused_proof.ok && refused_proof.diagnostic.error &&
      refused_proof.failure_reason=="bulk_constraint_identity_required" &&
      refused_proof.evidence.empty());
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

  // Packet tags describe inputs. Only equal source/destination carriers may
  // bypass the converted retained-row batch; column order alone is insufficient.
  api::InsertRowEncoderPlan encoder;
  encoder.columns = {{"key", "int64"}, {"label", "text"}};
  api::EngineNativeRowPacketFrame frame;
  frame.present = true;
  frame.field_order = {"key", "label"};
  frame.column_type_tags = {2, 1};
  request.lane_operation = "native_bulk";
  request.native_row_packet = &frame;
  Check(d::DirectBulkSourceCarriersMatchDestination(request, encoder));
  for (const auto source_tag : {1, 3, 4, 5, 6, 7, 0}) {
    frame.column_type_tags[0] = source_tag;
    Check(!d::DirectBulkSourceCarriersMatchDestination(request, encoder));
  }
  frame.column_type_tags = {2, 1};
  frame.field_order = {"label", "key"};
  Check(!d::DirectBulkSourceCarriersMatchDestination(request, encoder));
  frame.field_order = {"key", "label"};
  frame.column_type_tags.pop_back();
  Check(!d::DirectBulkSourceCarriersMatchDestination(request, encoder));
  Check(!d::DirectBulkSourceCarriersMatchDestination(request, {}));

  // The fallback retains destination-width bytes, including sign extension;
  // no query reader is permitted to reinterpret the old source text/width.
  namespace dt = scratchbird::core::datatypes;
  frame.packet_bytes = {'4', '2'};
  auto stored = d::DirectNativePacketStoredValue(frame, {1, false, 0, 2},
                                                dt::CanonicalTypeId::int64);
  Check(stored.isPresent() && stored.bytes == std::string("\x2a\0\0\0\0\0\0\0", 8));
  frame.packet_bytes = {0xff, 0xff, 0xff, 0xff};
  stored = d::DirectNativePacketStoredValue(frame, {4, false, 0, 4},
                                           dt::CanonicalTypeId::int64);
  Check(stored.isPresent() && stored.bytes == std::string(8, static_cast<char>(0xff)));
  frame.packet_bytes.assign(8, 0xff);
  stored = d::DirectNativePacketStoredValue(frame, {5, false, 0, 8},
                                           dt::CanonicalTypeId::uint64);
  Check(stored.isPresent() && stored.bytes == std::string(8, static_cast<char>(0xff)));
  frame.packet_bytes.assign(8, 0);
  frame.packet_bytes.back() = 0x80; // REAL64 negative zero must retain its bits.
  stored = d::DirectNativePacketStoredValue(frame, {6, false, 0, 8},
                                           dt::CanonicalTypeId::real64);
  Check(stored.isPresent() && stored.bytes == std::string("\0\0\0\0\0\0\0\x80", 8));
  Check(d::DirectNativePacketStoredValue(frame, {6, true, 0, 0},
                                        dt::CanonicalTypeId::real64).isSqlNull());
  frame.packet_bytes = {'9', '2', '2', '3', '3', '7', '2', '0', '3', '6',
                        '8', '5', '4', '7', '7', '5', '8', '0', '8'};
  Check(Refused([&] { d::DirectNativePacketStoredValue(frame,
      {1, false, 0, frame.packet_bytes.size()}, dt::CanonicalTypeId::int64); }));

  request.native_row_packet = nullptr;
  request.shared_row_field_order = frame.field_order;
  api::EngineTypedValue integer, label;
  integer.descriptor.canonical_type_name = "int64";
  label.descriptor.canonical_type_name = "text";
  std::vector<api::EngineRowValue> rows(2);
  for (auto& row : rows) row.fields = {{"key", integer}, {"label", label}};
  request.borrowed_input_rows = rows;
  Check(d::DirectBulkSourceCarriersMatchDestination(request, encoder));
  rows.back().fields[0].second.descriptor.canonical_type_name = "int32";
  Check(!d::DirectBulkSourceCarriersMatchDestination(request, encoder));
  rows.back().fields[0].second.descriptor.canonical_type_name = "unknown";
  Check(!d::DirectBulkSourceCarriersMatchDestination(request, encoder));
}
