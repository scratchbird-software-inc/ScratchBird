// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Component API regression for exact UUID bindings, identity casts and the
// explicit UUID/BINARY boundary and bound canonical ordering; not SQL/IPC.
#include "executor/descriptor_value_runtime.hpp"
#include "internal_api/query/expression_api.hpp"
#include "internal_api/catalog/datatype_bootstrap_identity.hpp"
#include "internal_api/catalog/column_metadata_codec.hpp"
#include "sblr/canonical_query_descriptor_support.hpp"
#include "sblr/canonical_query_object_free_composition_support.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
// Measure the warmed key operation, then fail its final allocation (key
// reserve). Do not assume a particular standard library's string capacity.
// One-shot failure permits allocation of diagnostics.
namespace {
thread_local bool track_uuid_key_allocations = false;
thread_local std::size_t uuid_key_allocation_count = 0;
thread_local std::size_t fail_uuid_key_allocation = 0;
}
void* operator new(std::size_t bytes) {
  if (track_uuid_key_allocations &&
      ++uuid_key_allocation_count == fail_uuid_key_allocation) {
    track_uuid_key_allocations = false;
    fail_uuid_key_allocation = 0;
    throw std::bad_alloc();
  }
  if (void* allocation = std::malloc(bytes ? bytes : 1)) return allocation;
  throw std::bad_alloc();
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
namespace {
unsigned checks = 0;
void Check(bool passed, const char* detail) {
  ++checks;
  if (!passed) throw std::runtime_error(detail);
}
api::EngineDescriptor Descriptor(const char* type, unsigned ordinal) {
  auto descriptor = exec::MakeExecutorDescriptor(type);
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2086, ordinal);
  descriptor.descriptor_kind = "scalar";
  descriptor.encoded_descriptor = "nullability=nullable";
  return descriptor;
}
void Preserved(const api::EngineTypedValue& result,
               const api::EngineTypedValue& source,
               const api::EngineDescriptor& target) {
  Check(result.descriptor == target && result.state == source.state &&
            result.is_null == source.is_null &&
            result.binary_value == source.binary_value && result.encoded_value.empty(),
        "cast changed descriptor, native bytes or SQL NULL state");
}
void Both(const api::EngineTypedValue& source, const api::EngineDescriptor& target) {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  const auto direct = exec::CastDescriptorValue(source, target, &diagnostic);
  if (!diagnostic.ok) std::cerr << diagnostic.diagnostic_code << ':' << diagnostic.detail << '\n';
  Check(diagnostic.ok, "bound executor cast refused");
  Preserved(direct, source, target);
  api::EngineTypedValue coerced;
  std::string category, detail;
  Check(api::QowApplyCanonicalDescriptorCoercionV1(
            source, target, true, &coerced, &category, &detail),
        "bound QOW explicit cast refused");
  Preserved(coerced, source, target);
  Check(!category.empty(), "successful QOW cast lost its category");
}
void Reject(const api::EngineTypedValue& source,
            const api::EngineDescriptor& target) {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  const auto direct = exec::CastDescriptorValue(source, target, &diagnostic);
  Check(!diagnostic.ok && !diagnostic.diagnostic_code.empty() &&
            direct.binary_value.empty() && direct.encoded_value.empty(),
        "executor admitted invalid binding or published a partial payload");
  api::EngineTypedValue coerced = source;
  std::string category = "stale", detail;
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
            source, target, true, &coerced, &category, &detail) &&
            !detail.empty() && category.empty() &&
            coerced.state == api::EngineValueState::error &&
            coerced.binary_value.empty() && coerced.encoded_value.empty(),
        "QOW admitted invalid binding or retained stale output");
}
void CoreIdentity(const api::EngineTypedValue& source,const api::EngineDescriptor& target) {
  namespace dt=scratchbird::core::datatypes;
  dt::DatatypeCastRequest request;
  request.value.type_id=request.target_type_id=dt::CanonicalTypeId::uuid;
  request.value.encoded_value.assign(source.binary_value.begin(),source.binary_value.end());
  std::string detail;
  Check(exec::BuildBoundExecutionTypeDescriptor(source.descriptor,dt::CanonicalTypeId::uuid,
      &request.value.descriptor,&detail)&&exec::BuildBoundExecutionTypeDescriptor(target,
      dt::CanonicalTypeId::uuid,&request.target_descriptor,&detail),"Core fixture keeps bound descriptor shapes");
  for(auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,
      dt::DatatypeCastContext::explicit_cast})for(bool convenience:{false,true}) {
    request.context=context;request.explicit_cast=convenience;
    const auto cast=dt::CastDatatypeValue(request);
    Check(cast.ok()&&cast.category==dt::DatatypeCastCategory::identity&&
        cast.value.type_id==dt::CanonicalTypeId::uuid&&!cast.value.is_null&&
        cast.value.encoded_value==request.value.encoded_value&&
        cast.value.descriptor.nullable_allowed==request.target_descriptor.nullable_allowed,
        "Core identity preserves every data bit and target nullability in each cast context");
  }
  request.context=static_cast<dt::DatatypeCastContext>(255);
  const auto invalid=dt::CastDatatypeValue(request);
  Check(!invalid.ok()&&invalid.value.type_id==dt::CanonicalTypeId::unknown&&
      invalid.value.encoded_value.empty(),"identity cannot bypass invalid cast context");
}
void LiteralBindings() {
  namespace s = scratchbird::engine::sblr;
  namespace dt = scratchbird::core::datatypes;
  const auto builtin=exec::MakeExecutorDescriptor("uuid");
  const auto identity=dt::LookupDatatypeTypeCodecIdentityV1(
      api::kBootstrapDatatypeCatalogUuid,api::kBootstrapDatatypeCatalogGeneration,
      api::kBootstrapDatatypeRegistryGeneration,builtin.datatype_descriptor_uuid,
      builtin.datatype_descriptor_generation);
  Check(identity.ok,"fixture resolves actual published UUID codec row");
  const auto& row=identity.row;
  api::RelationalTypeDescriptor source;
  source.descriptor_id=1;source.descriptor_uuid=row.descriptor_uuid;source.type_uuid=row.type_uuid;
  source.nullability=api::RelationalNullability::kNonNull;
  source.datatype_identity_authoritative=true;source.descriptor_generation=row.descriptor_generation;
  source.type_generation=row.type_generation;source.codec_id=row.codec_id;
  source.codec_version=row.codec_version;source.codec_generation=row.codec_generation;
  source.statement_receipt_uuid=scratchbird::tests::FixtureUuid(2086,10);
  source.datatype_catalog_snapshot_uuid=row.catalog_snapshot_uuid;
  source.datatype_catalog_generation=row.catalog_generation;
  source.datatype_registry_generation=row.registry_generation;
  api::TypedRelationalDag dag;dag.descriptors.push_back(source);
  api::RelationalDagNode node;node.node_id=1;node.node_kind=api::RelationalDagNodeKind::kValues;
  node.output_descriptor_ids={1};node.semantic_variant_id="values.literal-table.v1";
  dag.outputs.push_back({1,1,1,"uuid_value",1,true,0});
  for(unsigned pattern=0;pattern<130;++pattern) {
    std::string bytes(16,'\0');if(pattern==1)bytes.assign(16,static_cast<char>(0xff));
    if(pattern>=2)bytes[(pattern-2)/8]=static_cast<char>(1u<<((pattern-2)%8));
    api::RelationalExpressionRecord expression;expression.expression_id=pattern+1;
    expression.expression_kind=api::RelationalExpressionKind::kLiteral;
    expression.result_descriptor_id=1;expression.literal_kind=api::RelationalLiteralKind::kUuid;
    expression.literal_or_parameter_ref=bytes;dag.expressions.push_back(expression);
    dag.values_rows.push_back({pattern+1,{pattern+1}});node.values_row_ids.push_back(pattern+1);
  }
  dag.nodes.push_back(node);
  s::plan::CanonicalLogicalRelationalNode logical;logical.logical_node_id=1;
  auto values=s::MaterializeValues(dag,logical,{});
  if(!values.ok)std::cerr<<values.detail<<'\n';
  Check(values.ok&&values.batch.rows.size()==130,"bound UUID VALUES executes every binary pattern");
  api::EngineDescriptor expected;
  Check(s::BuildExactCanonicalUuidRuntimeDescriptorV1(source,&expected),"exact UUID runtime binding");
  Check(values.batch.columns.front().descriptor==expected,"column retains complete exact datatype binding");
  const auto binary=Descriptor("binary",20);
  for(unsigned pattern=0;pattern<130;++pattern) {
    const auto& value=values.batch.rows[pattern].values.front();
    const auto& input=*dag.expressions[pattern].literal_or_parameter_ref;
    Check(value.descriptor==expected&&value.encoded_value.empty()&&
        value.binary_value==std::vector<std::uint8_t>(input.begin(),input.end()),
        "VALUES preserves exact descriptor and all128 data bits without text");
    Both(value,binary);Both(value,expected);CoreIdentity(value,expected);
    auto nullable=expected;nullable.encoded_descriptor="nullability=nullable";Both(value,nullable);
    CoreIdentity(value,nullable);
    auto nullable_value=value;nullable_value.descriptor=nullable;
    Both(nullable_value,expected);CoreIdentity(nullable_value,expected);
    const auto& next=values.batch.rows[(pattern+1)%130].values.front();
    const int byte_order=std::memcmp(value.binary_value.data(),next.binary_value.data(),16);
    int comparison=99;std::string detail;
    Check(api::QowCompareCanonicalNonCollatedScalarsV1(value,next,&comparison,&detail)&&
        comparison==(byte_order<0?-1:byte_order>0?1:0)&&detail.empty(),
        "actual VALUES output retains sufficient binding for canonical UUID ordering");
  }
  for(unsigned mutation=0;mutation<18;++mutation) {
    auto invalid=source;
    switch(mutation) {
      case 0:invalid.descriptor_uuid={};break;
      case 1:++invalid.descriptor_generation;break;
      case 2:invalid.type_uuid={};break;
      case 3:++invalid.type_generation;break;
      case 4:invalid.codec_id+="invalid";break;
      case 5:++invalid.codec_version;break;
      case 6:++invalid.codec_generation;break;
      case 7:invalid.datatype_catalog_snapshot_uuid={};break;
      case 8:++invalid.datatype_catalog_generation;break;
      case 9:++invalid.datatype_registry_generation;break;
      case 10:invalid.statement_receipt_uuid={};break;
      case 11:invalid.nullability=api::RelationalNullability::kUnknown;break;
      case 12:invalid.collation_uuid=scratchbird::tests::FixtureUuid(2086,21);break;
      case 13:invalid.timezone_profile_id="UTC";break;
      case 14:invalid.width=16;break;
      case 15:invalid.precision=16;break;
      case 16:invalid.scale=0;break;
      case 17:invalid.statement_receipt_uuid.bytes[6]=0x40;break;
    }
    auto refused=expected;
    Check(!s::BuildExactCanonicalUuidRuntimeDescriptorV1(invalid,&refused)&&
        refused==api::EngineDescriptor{},"invalid exact UUID binding clears previous output");
    dag.descriptors.front()=invalid;const auto failure=s::MaterializeValues(dag,logical,{});
    Check(!failure.ok&&failure.batch.rows.empty()&&failure.batch.columns.empty()&&
        failure.result_bindings.empty(),"invalid UUID cohort publishes no VALUES prefix");
  }
  auto unadmitted=source;unadmitted.datatype_identity_authoritative=false;
  auto refused=expected;
  Check(!s::BuildExactCanonicalUuidRuntimeDescriptorV1(unadmitted,&refused)&&
      refused==api::EngineDescriptor{},"bare registry fields are not an admitted literal binding");
  dag.descriptors.front()=source;
  auto second=source;second.descriptor_id=2;++second.descriptor_generation;
  auto two_columns=dag;two_columns.descriptors.push_back(second);
  two_columns.nodes.front().output_descriptor_ids.push_back(2);
  two_columns.outputs.push_back({2,1,2,"second_uuid",2,true,1});
  auto second_literal=dag.expressions.front();second_literal.expression_id=131;
  second_literal.result_descriptor_id=2;two_columns.expressions.push_back(second_literal);
  for(auto& row:two_columns.values_rows)row.expression_ids.push_back(131);
  const auto partial_schema=s::MaterializeValues(two_columns,logical,{});
  Check(!partial_schema.ok&&partial_schema.batch.columns.empty()&&partial_schema.batch.rows.empty()&&
      partial_schema.result_bindings.empty(),"late invalid descriptor clears earlier column bindings");
  for(unsigned width:{0u,15u,17u,36u}) {
    auto malformed=dag;malformed.expressions.back().literal_or_parameter_ref=std::string(width,'x');
    const auto partial_rows=s::MaterializeValues(malformed,logical,{});
    Check(!partial_rows.ok&&partial_rows.batch.rows.empty()&&partial_rows.batch.columns.empty()&&
        partial_rows.result_bindings.empty(),"last malformed UUID literal publishes no prior row prefix");
  }
}

void CanonicalOrdering() {
  namespace dt = scratchbird::core::datatypes;
  const auto descriptor = Descriptor("uuid", 3);
  std::vector<api::EngineTypedValue> values;
  std::vector<dt::DatatypeOperationValue> operands;
  std::vector<std::string> keys;
  dt::DatatypeUuidOrderingProfileV1 profile;
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    auto value = exec::MakeExecutorValue(descriptor, {}, false);
    value.binary_value.assign(16, pattern == 1 ? 0xff : 0);
    if (pattern >= 2) value.binary_value[(pattern - 2) / 8] =
        static_cast<std::uint8_t>(1u << ((pattern - 2) % 8));
    dt::DatatypeOperationValue operand;
    operand.type_id = dt::CanonicalTypeId::uuid;
    operand.encoded_value.assign(value.binary_value.begin(), value.binary_value.end());
    std::string detail;
    Check(exec::BuildBoundExecutionTypeDescriptor(descriptor, operand.type_id,
        &operand.descriptor, &detail), "ordering fixture requires actual supplied descriptor binding");
    Check(dt::ResolveCanonicalUuidOrderingProfileV1(operand.descriptor, &profile),
        "canonical base UUID descriptor resolves default ordering");
    dt::DatatypeSortKeyRequest key_request{operand};
    key_request.uuid_ordering = profile;
    const auto key = dt::MakeDatatypeSortKey(key_request);
    Check(key.ok() && key.sort_key.size() == 66 &&
        key.sort_key.substr(50) == operand.encoded_value,
        "bound UUID key retains all16 canonical bytes and full binary provenance");
    Check(std::memcmp(key.sort_key.data(), profile.profile_uuid.bytes.data(), 16) == 0 &&
        std::memcmp(key.sort_key.data() + 24, operand.descriptor.descriptor_uuid.bytes, 16) == 0 &&
        key.sort_key[23] == 1 && key.sort_key[48] == 0 && key.sort_key[49] == 1,
        "key prefix has exact binary profile, descriptor, generation and state framing");
    values.push_back(value);
    operands.push_back(operand);
    keys.push_back(key.sort_key);
  }
  const auto sign = [](int x) { return x < 0 ? -1 : x > 0 ? 1 : 0; };
  for (std::size_t i = 0; i < values.size(); ++i) {
    for (std::size_t j = 0; j < values.size(); ++j) {
      const int expected = sign(std::memcmp(values[i].binary_value.data(),
                                           values[j].binary_value.data(), 16));
      dt::DatatypeComparisonRequest request{operands[i], operands[j]};
      request.uuid_ordering = profile;
      const auto compared = dt::CompareDatatypeValues(request);
      Check(compared.ok() && compared.comparison == expected,
          "Core comparison differs from unsigned canonical-byte oracle");
      int comparison = 99;
      std::string detail;
      Check(api::QowCompareCanonicalNonCollatedScalarsV1(values[i], values[j],
          &comparison, &detail) && comparison == expected && detail.empty(),
          "bound QOW UUID comparison differs from Core/oracle");
      Check(sign(keys[i].compare(keys[j])) == expected,
          "UUID sort key disagrees with comparison");
      request.right.descriptor.nullable_allowed = false;
      const auto nonnull = dt::CompareDatatypeValues(request);
      Check(nonnull.ok() && nonnull.comparison == expected,
          "PRESENT nullability must not alter UUID order");
    }
  }
  auto null = operands.front();
  null.is_null = true;
  null.encoded_value.clear();
  for (auto ordering : {dt::DatatypeNullOrdering::nulls_first,
                        dt::DatatypeNullOrdering::nulls_last}) {
    dt::DatatypeSortKeyRequest null_key_request{null};
    null_key_request.uuid_ordering = profile;
    null_key_request.null_ordering = ordering;
    const auto null_key = dt::MakeDatatypeSortKey(null_key_request);
    Check(null_key.ok() && null_key.sort_key.size() == 50,
        "typed NULL key is framed without a UUID payload");
    for (const auto& operand : operands) {
      dt::DatatypeComparisonRequest request{null, operand};
      request.uuid_ordering = profile;
      request.null_ordering = ordering;
      const int expected = ordering == dt::DatatypeNullOrdering::nulls_first ? -1 : 1;
      auto compared = dt::CompareDatatypeValues(request);
      Check(compared.ok() && compared.comparison == expected, "NULL ordering is independent of UUID value order");
      std::swap(request.left, request.right);
      compared = dt::CompareDatatypeValues(request);
      Check(compared.ok() && compared.comparison == -expected, "NULL order is antisymmetric");
      dt::DatatypeSortKeyRequest present_key_request{operand};
      present_key_request.uuid_ordering = profile;
      present_key_request.null_ordering = ordering;
      const auto present_key = dt::MakeDatatypeSortKey(present_key_request);
      Check(present_key.ok() && sign(null_key.sort_key.compare(present_key.sort_key)) == expected,
          "NULL key agrees with comparison for every bit pattern");
    }
    dt::DatatypeComparisonRequest both_null{null, null};
    both_null.uuid_ordering = profile;
    both_null.null_ordering = ordering;
    const auto equal = dt::CompareDatatypeValues(both_null);
    Check(equal.ok() && equal.comparison == 0, "two admitted NULLs sort equally");
  }
  const auto refuse_operand = [&](const dt::DatatypeOperationValue& invalid) {
    for (bool swap : {false, true}) {
      dt::DatatypeComparisonRequest request{invalid, operands.front()};
      if (swap) std::swap(request.left, request.right);
      request.uuid_ordering = profile;
      const auto result = dt::CompareDatatypeValues(request);
      Check(!result.ok() && result.comparison == 0 && !result.diagnostic.diagnostic_code.empty(),
          "invalid UUID ordering operand must not publish comparison");
    }
    dt::DatatypeSortKeyRequest request{invalid};
    request.uuid_ordering = profile;
    const auto result = dt::MakeDatatypeSortKey(request);
    Check(!result.ok() && result.sort_key.empty() && !result.diagnostic.diagnostic_code.empty(),
        "invalid UUID ordering operand must not publish key prefix");
  };
  for (unsigned width : {0u, 15u, 17u, 36u}) {
    auto invalid = operands.front(); invalid.encoded_value.assign(width, 'x');
    refuse_operand(invalid);
  }
  auto invalid = null; invalid.encoded_value.assign(16, '\0'); refuse_operand(invalid);
  invalid = null; invalid.descriptor.nullable_allowed = false; refuse_operand(invalid);
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    invalid = operands.front();
    switch (mutation) {
      case 0: invalid.descriptor = {}; break;
      case 1: ++invalid.descriptor.descriptor_epoch; break;
      case 2: {
        const auto domain = scratchbird::tests::FixtureUuid(2086, 99);
        std::copy(domain.bytes.begin(), domain.bytes.end(), invalid.descriptor.domain_uuid.bytes);
        break;
      }
      case 3: invalid.descriptor.bit_width = 64; break;
      case 4: invalid.descriptor.descriptor_authoritative = false; break;
      case 5: invalid.descriptor.parser_independent = false; break;
      case 6: invalid.descriptor.descriptor_uuid.bytes[15] ^= 1; break;
    }
    auto resolved = profile;
    Check(!dt::ResolveCanonicalUuidOrderingProfileV1(invalid.descriptor, &resolved) &&
        resolved.profile_uuid == dt::DatatypeUuidOrderingProfileV1{}.profile_uuid &&
        resolved.generation == 0, "resolver clears stale output for invalid descriptor/domain");
    refuse_operand(invalid);
  }
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    for (const auto& operand : {operands.front(), null}) {
      dt::DatatypeComparisonRequest request{operand, operand};
      request.uuid_ordering = profile;
      switch (mutation) {
        case 0: request.uuid_ordering = {}; break;
        case 1: ++request.uuid_ordering.generation; break;
        case 2: request.uuid_ordering.generation = 0; break;
        case 3: request.uuid_ordering.profile_uuid.bytes[15] ^= 1; break;
        case 4: request.null_ordering = static_cast<dt::DatatypeNullOrdering>(99); break;
        case 5: request.case_insensitive_character_compare = true; break;
      }
      const auto compared = dt::CompareDatatypeValues(request);
      dt::DatatypeSortKeyRequest key_request{operand};
      key_request.uuid_ordering = request.uuid_ordering;
      key_request.null_ordering = request.null_ordering;
      key_request.case_insensitive_character_compare = request.case_insensitive_character_compare;
      const auto key = dt::MakeDatatypeSortKey(key_request);
      Check(!compared.ok() && compared.comparison == 0 && !key.ok() && key.sort_key.empty(),
          "unknown, absent or stale profile and invalid options fail even for two NULLs");
    }
  }
  dt::DatatypeComparisonRequest mixed{operands.front(), operands.front()};
  mixed.uuid_ordering = profile;
  mixed.right.type_id = dt::CanonicalTypeId::uint128;
  Check(!dt::CompareDatatypeValues(mixed).ok(), "UUID bytes cannot infer numeric operand conversion");
  for (unsigned mutation = 0; mutation < 12; ++mutation) {
    auto bad = values.front();
    switch (mutation) {
      case 0: bad.descriptor.datatype_descriptor_uuid = {}; break;
      case 1: ++bad.descriptor.datatype_descriptor_generation; break;
      case 2: bad.encoded_value = "text"; break;
      case 3: bad.binary_value.resize(15); break;
      case 4: bad.state = api::EngineValueState::missing; break;
      case 5: bad.is_null = true; break;
      case 6: bad.descriptor.encoded_descriptor += ";ordering=guid"; break;
      case 7: bad.descriptor.encoded_descriptor += ";ordering=timeuuid"; break;
      case 8: bad.descriptor.encoded_descriptor += ";width=64"; break;
      case 9: bad.descriptor.encoded_descriptor += ";precision=0"; break;
      case 10: bad.descriptor.encoded_descriptor += ";scale=0"; break;
      case 11: bad.descriptor.encoded_descriptor += ";timezone_profile_id=1"; break;
    }
    for (bool swap : {false, true}) {
      int comparison = 99; std::string detail;
      Check(!api::QowCompareCanonicalNonCollatedScalarsV1(
          swap ? values.front() : bad, swap ? bad : values.front(), &comparison, &detail) &&
          comparison == 0 && !detail.empty(), "QOW preserves binding/carrier/state rejection");
    }
  }
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    api::CatalogColumnMetadata fields;
    fields.text["nullability"] = "nullable";
    if (mutation == 1) fields.text["ordering"] = "guid";
    if (mutation == 2) fields.identities["ordering_profile_uuid"] =
        scratchbird::tests::FixtureUuid(2086, 100);
    if (mutation == 3) fields.text["width"] = "128";
    auto framed = values.front();
    Check(api::EncodeCatalogColumnMetadata(fields, &framed.descriptor.encoded_descriptor),
        "framed UUID metadata fixture must encode");
    for (bool swap : {false, true}) {
      int comparison = 99; std::string detail;
      const bool accepted = api::QowCompareCanonicalNonCollatedScalarsV1(
          swap ? values.front() : framed, swap ? framed : values.front(), &comparison, &detail);
      Check(accepted == (mutation == 0) && comparison == 0 &&
          detail.empty() == accepted, "framed UUID metadata preserves exact admission and rejects ignored fields");
    }
  }
  for (const auto& operand : {operands.front(), null}) {
    dt::DatatypeSortKeyRequest request{operand};
    request.uuid_ordering = profile;
    uuid_key_allocation_count = fail_uuid_key_allocation = 0;
    track_uuid_key_allocations = true;
    const auto warmed = dt::MakeDatatypeSortKey(request);
    track_uuid_key_allocations = false;
    const auto allocation_count = uuid_key_allocation_count;
    Check(warmed.ok() && allocation_count > 0, "key allocation fault must target a warmed allocating operation");
    Check(allocation_count <= 64,
        "warmed UUID key must not allocate whole-manifest copies per descriptor validation");
    std::cout << "uuid_key_allocations=" << allocation_count
              << " null=" << operand.is_null << '\n';
    fail_uuid_key_allocation = allocation_count;
    uuid_key_allocation_count = 0;
    track_uuid_key_allocations = true;
    const auto failed = dt::MakeDatatypeSortKey(request);
    const bool injected = fail_uuid_key_allocation == 0;
    track_uuid_key_allocations = false;
    fail_uuid_key_allocation = 0;
    Check(injected && !failed.ok() && failed.sort_key.empty() &&
        failed.status.code == scratchbird::core::platform::StatusCode::memory_allocation_failed,
        "UUID key allocation fault returns resource failure without prefix");
    Check(dt::MakeDatatypeSortKey(request).ok(), "UUID key operation recovers after allocation failure");
  }
}
}

int main() try {
  const auto uuid = Descriptor("uuid", 1), binary = Descriptor("binary", 2);
  auto stale = uuid;
  ++stale.datatype_descriptor_generation;
  auto unbound = uuid;
  unbound.datatype_descriptor_uuid = {};
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    auto value = exec::MakeExecutorValue(uuid, {}, false);
    value.binary_value.assign(16, 0);
    if (pattern == 1) value.binary_value.assign(16, 0xff);
    if (pattern >= 2) value.binary_value[(pattern - 2) / 8] =
        static_cast<std::uint8_t>(1u << ((pattern - 2) % 8));
    Both(value, binary);
    Both(value, uuid);
    CoreIdentity(value,uuid);
    auto bytes = value;
    bytes.descriptor = binary;
    Both(bytes, uuid);
    Reject(bytes, stale);
    Reject(bytes, unbound);
    auto invalid = value;
    invalid.descriptor = stale;
    Reject(invalid, binary);
    invalid.descriptor = unbound;
    Reject(invalid, binary);
    Reject(value, stale);Reject(value, unbound);
    Reject(invalid, uuid);
    api::EngineTypedValue result;
    std::string category, detail;
    Check(!api::QowApplyCanonicalDescriptorCoercionV1(
              value, binary, false, &result, &category, &detail) &&
              result.state == api::EngineValueState::error &&
              result.binary_value.empty() && result.encoded_value.empty(),
          "implicit UUID/BINARY cast accepted or published data");
  }
  for (unsigned width : {0u, 15u, 17u}) {
    auto invalid = exec::MakeExecutorValue(uuid, {}, false);
    invalid.binary_value.assign(width, 0);
    Reject(invalid, binary);
    Reject(invalid, uuid);
    invalid.descriptor = binary;
    Reject(invalid, uuid);
  }
  auto present=exec::MakeExecutorValue(uuid,{},false);
  present.binary_value.assign(16,0x80);
  auto mixed=present;mixed.encoded_value="text cannot be a second UUID carrier";
  Reject(mixed,uuid);Reject(mixed,binary);
  for(auto state:{api::EngineValueState::sql_null,api::EngineValueState::missing,
      api::EngineValueState::default_requested,api::EngineValueState::unknown,
      api::EngineValueState::error,api::EngineValueState::lob_handle,
      api::EngineValueState::protected_value}) {
    auto invalid=present;invalid.state=state;
    Reject(invalid,uuid);Reject(invalid,binary);
  }
  Both(present,uuid);
  auto null = exec::MakeExecutorValue(uuid, {}, true);
  Both(null, binary);
  Both(null, uuid);
  Reject(null, stale);
  Reject(null, unbound);
  null.binary_value.push_back(0);
  Reject(null, binary);
  LiteralBindings();
  CanonicalOrdering();
  std::cout << "native_uuid_cast_binding checks=" << checks << " patterns=130 uuid_pairs=16900 failures=0\n";
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << "FAIL check=" << checks << ' ' << error.what() << '\n';
  return EXIT_FAILURE;
}
