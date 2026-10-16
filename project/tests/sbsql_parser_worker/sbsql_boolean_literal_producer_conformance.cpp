// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual parser producers and frozen codecs with explicit structural fixtures.
// No fixture projection is represented as a live engine receipt/admission.
#include "wire/sbsql_test_wire.hpp"
#include "lowering/lowering.hpp"
#include "lowering/relational_identity_operand.hpp"
#include "binder/descriptor_authority.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace scratchbird::parser::sbsql {
std::optional<std::vector<std::uint8_t>> EncodeBooleanLiteralPrebindForTest(
    const NativeRelationalAstDocument&, const ipc::ParserStatementContext&,
    const ScalarProjectionLiteralDemandDescriptionV1*);
bool ConsumeBooleanLiteralPrebindForTest(const NativeRelationalAstDocument&,
    ipc::ParserStatementContext*, const std::vector<std::uint8_t>&,
    const ScalarProjectionLiteralDemandDescriptionV1*);
std::optional<std::vector<std::uint8_t>> EncodeBooleanLiteralTableForTest(
    const BoundStatement&, const SblrEnvelope&, const ipc::ParserStatementContext&);
std::optional<std::vector<std::uint8_t>> CaptureBooleanLiteralFinalizeForTest(
    const NativeRelationalAstDocument&, const BoundStatement&, const SblrEnvelope&,
    const ipc::ParserStatementContext&, const std::vector<std::uint8_t>&,
    const ScalarProjectionLiteralDemandDescriptionV1*);
}
namespace p = scratchbird::parser::sbsql;
namespace ipc = scratchbird::parser::ipc;
namespace rt = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
using Bytes = std::vector<std::uint8_t>;
using Uuid = scratchbird::core::platform::Uuid;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
Uuid Id(unsigned value) {
  Uuid id{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,0}};
  id.bytes[14] = static_cast<std::uint8_t>(value >> 8);
  id.bytes[15] = static_cast<std::uint8_t>(value);
  return id;
}
ipc::ParserStatementContext Context() {
  ipc::ParserStatementContext context;
  context.literal_preliminary_receipt_uuid = Id(1);
  context.literal_catalog_snapshot_uuid = dt::kDatatypeCohortV11;
  context.literal_catalog_generation = 11;
  context.literal_security_epoch = 3;
  context.literal_resource_epoch = 5;
  context.literal_mga_snapshot_uuid = Id(2);
  for (unsigned slot = 0; slot != 4; ++slot) {
    ipc::ParserStatementContext::DescriptorProfile profile;
    profile.profile_kind = 1; profile.slot = static_cast<std::uint16_t>(slot);
    profile.descriptor_uuid = Id(10 + slot); profile.type_uuid = Id(20);
    context.descriptor_profiles.push_back(profile);
  }
  return context;
}
const dt::DatatypeTypeCodecIdentityRowV1& Identity(unsigned lexical_class) {
  const auto wanted = lexical_class == 4 ? dt::CanonicalTypeId::boolean : dt::CanonicalTypeId::int64;
  for (const auto& candidate : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = candidate.legacy_fields;
    if (row.catalog_snapshot_uuid == dt::kDatatypeCohortV11 && row.catalog_generation == 11 &&
        row.registry_generation == 11 && row.canonical_binary_type_code == static_cast<unsigned>(wanted))
      return row;
  }
  throw std::runtime_error("frozen structural fixture cohort missing");
}
Bytes Response(const rt::SblrLiteralPrebindRequestV1& request) {
  rt::SblrLiteralPrebindResultV1 response;
  response.preliminary_receipt_uuid = request.preliminary_receipt_uuid;
  response.catalog_snapshot_uuid = request.catalog_snapshot_uuid;
  response.catalog_generation = request.catalog_generation;
  response.security_epoch = request.security_epoch;
  response.resource_epoch = request.resource_epoch;
  response.mga_snapshot_uuid = request.mga_snapshot_uuid;
  response.demand_sha256 = request.demand_sha256;
  for (const auto& demand : request.demands) {
    const auto& identity = Identity(demand.lexical_class);
    rt::SblrLiteralStatementDescriptorProfileV1 profile;
    profile.profile_uuid = Id(100 + static_cast<unsigned>(demand.occurrence_id)).bytes;
    profile.statement_receipt_uuid = request.preliminary_receipt_uuid;
    profile.catalog_snapshot_uuid = request.catalog_snapshot_uuid;
    profile.catalog_generation = request.catalog_generation;
    profile.descriptor_uuid = identity.descriptor_uuid.bytes;
    profile.descriptor_generation = identity.descriptor_generation;
    profile.type_uuid = identity.type_uuid.bytes;
    profile.codec_id = identity.codec_id; profile.codec_version = identity.codec_version;
    profile.codec_generation = identity.codec_generation; profile.nullable = demand.nullable;
    profile.profile_binding_sha256 = rt::ComputeSblrLiteralDescriptorProfileBindingV1(
        profile, request.security_epoch, request.resource_epoch);
    auto bytes = rt::EncodeSblrLiteralDescriptorProfileV1(profile);
    Check(!bytes.empty(), "structural SBLP encoding failed");
    response.mappings.push_back({demand.occurrence_id, std::move(bytes)});
  }
  response.ordered_profile_sha256 = rt::ComputeSblrLiteralOrderedProfilesSha256V1(response.mappings);
  return rt::EncodeSblrLiteralPrebindResultV1(response);
}
struct BoundFixture { p::BoundStatement bound; p::SblrEnvelope lowered; };
BoundFixture BindFixture(const p::NativeRelationalAstDocument& ast,
                         const ipc::ParserStatementContext& context,
                         const rt::SblrLiteralPrebindRequestV1& request) {
  BoundFixture fixture;
  fixture.lowered.descriptor_epoch = context.literal_catalog_generation;
  for (std::size_t index = 0; index != request.demands.size(); ++index) {
    const auto& demand = request.demands[index];
    const auto& profile = context.literal_statement_descriptor_profiles[index];
    const auto source = std::ranges::find_if(ast.expressions, [&](const auto& expression) {
      return expression.structural_literal_occurrence_id == demand.occurrence_id;
    });
    Check(source != ast.expressions.end(), "native fixture occurrence disappeared");
    p::NativeDescriptorBindingInput descriptor;
    descriptor.descriptor_id = static_cast<std::uint32_t>(index + 1);
    descriptor.descriptor_uuid = profile.binding_descriptor_uuid; descriptor.type_uuid = profile.type_uuid;
    descriptor.canonical_type_name = demand.lexical_class == 4 ? "boolean" : "int64";
    descriptor.nullability = profile.nullable ? p::BoundNullability::kNullable : p::BoundNullability::kNonNull;
    Check(p::PreserveNativeDescriptorAuthority(&descriptor, context), "exact fixture projection refused");
    p::BoundDescriptorAstRecord bound;
    bound.descriptor_id = descriptor.descriptor_id; bound.descriptor_uuid = descriptor.descriptor_uuid;
    bound.type_uuid = descriptor.type_uuid; bound.nullability = descriptor.nullability;
    bound.canonical_type_name = descriptor.canonical_type_name;
    bound.descriptor_generation = descriptor.descriptor_generation; bound.type_generation = descriptor.type_generation;
    bound.codec_id = descriptor.codec_id; bound.codec_version = descriptor.codec_version;
    bound.codec_generation = descriptor.codec_generation; bound.statement_receipt_uuid = descriptor.statement_receipt_uuid;
    bound.datatype_catalog_snapshot_uuid = descriptor.datatype_catalog_snapshot_uuid;
    bound.datatype_catalog_generation = descriptor.datatype_catalog_generation;
    bound.datatype_registry_generation = descriptor.datatype_registry_generation;
    fixture.bound.native_relational.descriptors.push_back(bound);
    api::RelationalTypeDescriptor wire;
    wire.descriptor_id = bound.descriptor_id; wire.descriptor_uuid = bound.descriptor_uuid; wire.type_uuid = bound.type_uuid;
    wire.nullability = profile.nullable ? api::RelationalNullability::kNullable : api::RelationalNullability::kNonNull;
    wire.datatype_identity_authoritative = true;
    wire.descriptor_generation = bound.descriptor_generation; wire.type_generation = bound.type_generation;
    wire.codec_id = bound.codec_id; wire.codec_version = bound.codec_version; wire.codec_generation = bound.codec_generation;
    wire.statement_receipt_uuid = bound.statement_receipt_uuid;
    wire.datatype_catalog_snapshot_uuid = bound.datatype_catalog_snapshot_uuid;
    wire.datatype_catalog_generation = bound.datatype_catalog_generation;
    wire.datatype_registry_generation = bound.datatype_registry_generation;
    p::SblrOperand operand;
    operand.type = "relational_descriptor_v3"; operand.name = "slot_" + std::to_string(wire.descriptor_id);
    operand.canonical_value_kind = static_cast<std::uint16_t>(rt::SblrValueKind::relational_type_descriptor);
    Check(rt::EncodeRelationalTypeDescriptorV1(wire, &operand.canonical_value_body), "fixture descriptor encoding failed");
    fixture.lowered.operands.push_back(operand);
    p::BoundExpressionAstRecord expression;
    expression.expression_id = source->expression_id;
    expression.expression_kind = p::NativeExpressionAstKind::kLiteral; expression.literal_kind = source->literal_kind;
    expression.result_descriptor_id = wire.descriptor_id; expression.literal_or_parameter_ref = source->spelling;
    expression.structural_literal_occurrence_id = source->structural_literal_occurrence_id;
    fixture.bound.native_relational.expressions.push_back(expression);
    auto encoded = p::MakeRelationalExpressionOperand(expression);
    Check(encoded.has_value(), "fixture expression encoding failed");
    fixture.lowered.operands.push_back(std::move(*encoded));
  }
  return fixture;
}
void CheckCase(const char* sql, bool has_null) {
  const auto cst = p::BuildCst(sql);
  const auto ast = p::BuildAst(cst).native_relational;
  Check(!ast.expressions.empty(), "actual parser omitted literal occurrences");
  std::optional<p::ScalarProjectionLiteralDemandDescriptionV1> described;
  if (has_null) {
    described = p::DescribeScalarProjectionLiteralDemandsV1(cst, ast, {4096, 256});
    Check(described->applicable && described->valid, "actual scalar NULL context inference failed");
  }
  const auto* description = described ? &*described : nullptr;
  auto context = Context();
  const auto encoded = p::EncodeBooleanLiteralPrebindForTest(ast, context, description);
  Check(encoded.has_value(), "native Boolean prebind unavailable");
  const auto decoded = rt::DecodeSblrLiteralPrebindRequestV1(encoded->data(), encoded->size());
  Check(decoded.ok && !decoded.request.demands.empty(), "actual SBLN producer invalid");
  auto reordered = ast; std::ranges::reverse(reordered.expressions);
  Check(p::EncodeBooleanLiteralPrebindForTest(reordered, context, description) == encoded,
      "source occurrence ordering changed under AST record reorder");
  auto duplicate = ast; duplicate.expressions.push_back(ast.expressions.front());
  Check(!p::EncodeBooleanLiteralPrebindForTest(duplicate, context, description), "duplicate occurrence admitted");
  bool saw_null = false;
  for (const auto& demand : decoded.request.demands) {
    const auto source = std::ranges::find_if(ast.expressions, [&](const auto& expression) {
      return expression.structural_literal_occurrence_id == demand.occurrence_id;
    });
    Check(source != ast.expressions.end(), "demand lost native source occurrence");
    if (source->literal_kind == p::NativeLiteralAstKind::kNumeric) {
      Check(demand.lexical_class == 1 && !demand.nullable, "numeric profile shifted by Boolean occurrence");
      continue;
    }
    const bool is_null = source->literal_kind == p::NativeLiteralAstKind::kNull;
    saw_null |= is_null;
    Bytes material{'S','B','B','L',1,0,static_cast<std::uint8_t>(is_null ? 1 : 0),
        static_cast<std::uint8_t>(is_null ? 0 : 1)};
    if (!is_null) material.push_back(source->spelling.front() == 'F' || source->spelling.front() == 'f' ? 0 : 1);
    const auto hash = scratchbird::core::hash::ComputeSha256Digest(material);
    Check(hash.ok() && demand.lexical_class == 4 && demand.context_class == 1 &&
        demand.nullable == is_null && demand.lexical_sha256 == hash.digest, "Boolean state/hash demand changed");
  }
  Check(saw_null == has_null, "Boolean NULL context omitted or invented");
  const auto response = Response(decoded.request);
  Check(!response.empty(), "SBLQ fixture encoding failed");
  for (std::size_t index = 0; index != ast.expressions.size(); ++index) {
    const auto& source = ast.expressions[index];
    const bool selected = std::ranges::any_of(decoded.request.demands, [&](const auto& demand) {
      return source.expression_kind == p::NativeExpressionAstKind::kLiteral &&
          source.structural_literal_occurrence_id == demand.occurrence_id;
    });
    if (!selected) continue;
    auto malformed = ast;
    malformed.expressions[index].spelling.clear();
    auto unchanged = context;
    Check(!p::EncodeBooleanLiteralPrebindForTest(malformed, unchanged, description),
        "empty selected spelling published a partial demand beside valid literals");
    Check(!p::ConsumeBooleanLiteralPrebindForTest(malformed, &unchanged, response, description) &&
        unchanged.literal_statement_descriptor_profiles.empty() &&
        unchanged.descriptor_profiles.size() == context.descriptor_profiles.size() &&
        std::ranges::equal(unchanged.descriptor_profiles, context.descriptor_profiles,
            [](const auto& left, const auto& right) {
              return left.descriptor_uuid == right.descriptor_uuid && left.type_uuid == right.type_uuid;
            }),
        "empty selected spelling mutated context through a partial demand");
  }
  auto wrong_nullability = decoded.request;
  wrong_nullability.demands.front().nullable = !wrong_nullability.demands.front().nullable;
  auto refused_context = context;
  Check(!p::ConsumeBooleanLiteralPrebindForTest(ast, &refused_context, Response(wrong_nullability), description) &&
      refused_context.literal_statement_descriptor_profiles.empty(),
      "profile nullability contradicting the exact demand was published");
  for (std::size_t size = 0; size < response.size(); ++size) {
    auto unchanged = context;
    const Bytes truncated(response.begin(), response.begin() + size);
    Check(!p::ConsumeBooleanLiteralPrebindForTest(ast, &unchanged, truncated, description) &&
        unchanged.literal_statement_descriptor_profiles.empty() &&
        unchanged.descriptor_profiles.front().descriptor_uuid == context.descriptor_profiles.front().descriptor_uuid,
        "truncated mapping published a descriptor prefix");
  }
  Check(p::ConsumeBooleanLiteralPrebindForTest(ast, &context, response, description), "complete mapping refused");
  const auto fixture = BindFixture(ast, context, decoded.request);
  const auto table = p::EncodeBooleanLiteralTableForTest(fixture.bound, fixture.lowered, context);
  Check(table.has_value(), "bound Boolean SBXN unavailable");
  const auto parsed = rt::DecodeSblrStatefulExpressionNodeTableV2(table->data(), table->size());
  Check(parsed.ok && parsed.table.wire_version == (has_null ? 2 : 1) &&
      parsed.table.nodes.size() == decoded.request.demands.size(), "SBXN state/version/occurrence mismatch");
  for (const auto& node : parsed.table.nodes) {
    const auto source = std::ranges::find_if(ast.expressions, [&](const auto& expression) {
      return expression.expression_id == node.node_id;
    });
    Check(source != ast.expressions.end(), "SBXN acquired a fabricated native node ID");
    if (source->literal_kind == p::NativeLiteralAstKind::kBoolean)
      Check(node.value_state == 0 && node.literal_body == Bytes{
          static_cast<std::uint8_t>(source->spelling.front() == 'F' || source->spelling.front() == 'f' ? 0 : 1)},
          "Boolean SBXN payload is not exactly one native byte");
    if (source->literal_kind == p::NativeLiteralAstKind::kNull)
      Check(node.value_state == 1 && node.literal_body.empty(), "NULL SBXN retained a value payload");
  }
  const auto finalize = p::CaptureBooleanLiteralFinalizeForTest(ast, fixture.bound, fixture.lowered,
      Context(), response, description);
  Check(finalize.has_value(), "actual finalize producer never submitted complete material");
  rt::SblrLiteralFinalizeRequestV1 finalized;
  Check((has_null ? rt::DecodeSblrStatefulLiteralFinalizeRequestV2(finalize->data(), finalize->size(), &finalized)
                  : rt::DecodeSblrLiteralFinalizeRequestV1(finalize->data(), finalize->size(), &finalized)),
      "actual finalized SBLF/SBBA/SBXN framing invalid");
  rt::SblrLiteralBoundAstV1 sbba;
  Check(rt::DecodeSblrLiteralBoundAstV1(finalized.canonical_sbba.data(), finalized.canonical_sbba.size(), &sbba) &&
      sbba.nodes.size() == decoded.request.demands.size(), "complete SBBA occurrence mapping invalid");
  for (std::size_t index = 0; index < sbba.nodes.size(); ++index)
    Check(sbba.nodes[index].nullable == decoded.request.demands[index].nullable &&
        sbba.nodes[index].occurrence_id == decoded.request.demands[index].occurrence_id,
        "SBBA lost occurrence or nullable profile");
  auto wrong = fixture;
  wrong.bound.native_relational.descriptors.front().type_generation++;
  Check(!p::EncodeBooleanLiteralTableForTest(wrong.bound, wrong.lowered, context), "crossed bound descriptor admitted");
  wrong = fixture;
  wrong.bound.native_relational.descriptors.front().statement_receipt_uuid = Id(999);
  Check(!p::EncodeBooleanLiteralTableForTest(wrong.bound, wrong.lowered, context), "foreign bound receipt admitted");
  wrong = fixture;
  wrong.bound.native_relational.expressions.front().literal_or_parameter_ref = "2";
  const auto invalid_expression = p::MakeRelationalExpressionOperand(wrong.bound.native_relational.expressions.front());
  Check(invalid_expression.has_value(), "malformed value fixture framing failed");
  wrong.lowered.operands[1] = *invalid_expression;
  Check(!p::EncodeBooleanLiteralTableForTest(wrong.bound, wrong.lowered, context), "noncanonical Boolean body admitted");
  if (described) {
    auto invalid = *described;
    const auto null = std::ranges::find_if(invalid.entries, [](const auto& entry) { return entry.is_null; });
    Check(null != invalid.entries.end(), "NULL description missing");
    null->range.offset++;
    Check(!p::EncodeBooleanLiteralPrebindForTest(ast, Context(), &invalid), "crossed source range admitted");
  }
}
}
int main() try {
  CheckCase("VALUES (TRUE, 7, FALSE, TRUE)", false);
  CheckCase("VALUES (false,TRUE)", false);
  CheckCase("SELECT IIF(NULL, TRUE, FALSE) AS value", true);
  const auto bare = p::BuildAst(p::BuildCst("VALUES (NULL)")).native_relational;
  Check(!p::EncodeBooleanLiteralPrebindForTest(bare, Context(), nullptr), "contextless NULL acquired Boolean profile");
  std::cout << "boolean_literal_producer_component_checks=" << checks << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
