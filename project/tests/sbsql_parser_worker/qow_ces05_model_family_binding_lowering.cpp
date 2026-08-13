// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace scratchbird::parser::sbsql {
std::uint64_t Rcp073DocumentFrontdoorProofMaskForTest();
std::uint64_t Rcp074GraphFrontdoorProofMaskForTest();
std::uint64_t Rcp076TimeSeriesFrontdoorProofMaskForTest();
std::uint64_t Rcp079SpatialColumnarFrontdoorProofMaskForTest();
std::uint64_t Rcp080MultimodelWireProofMaskForTest();
}

namespace {

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "00000000-0000-7000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-CES05-DOCUMENT-PARSER: " << detail << '\n';
  return condition;
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages,
                   const std::string_view diagnostic) {
  return std::ranges::any_of(messages.diagnostics, [&](const auto& entry) {
    return entry.code == diagnostic;
  });
}

bool HasOperand(const sbsql::SblrEnvelope& envelope,
                const std::string_view type, const std::string_view name,
                const std::string_view value = {}) {
  return std::ranges::any_of(envelope.operands, [&](const auto& operand) {
    return operand.type == type && operand.name == name &&
           (value.empty() || operand.value == value);
  });
}

std::string Hex(const std::string_view value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    encoded.push_back(kDigits[byte >> 4]);
    encoded.push_back(kDigits[byte & 0x0f]);
  }
  return encoded;
}

bool HasRelationalExpressionFragment(const sbsql::SblrEnvelope& envelope,
                                     const std::string_view fragment) {
  return std::ranges::any_of(envelope.operands, [&](const auto& operand) {
    return operand.type == "relational_expression_v1" &&
           operand.value.find(fragment) != std::string::npos;
  });
}

std::string DiagnosticSummary(const sbsql::MessageVectorSet& messages) {
  std::string summary;
  for (const auto& diagnostic : messages.diagnostics) {
    if (!summary.empty()) summary += ",";
    summary += diagnostic.code + ":" + diagnostic.message;
    for (const auto& field : diagnostic.fields) {
      summary += ":" + field.name + "=" + field.value;
    }
  }
  return summary;
}

sbsql::ParserConfig Config() {
  sbsql::ParserConfig config;
  config.parser_uuid = Uuid(700);
  config.bundle_contract_id = "sbp_sbsql@qow-ces05-document-v1";
  config.build_id = "qow-ces05-document-v1";
  return config;
}

sbsql::SessionContext Session() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = Uuid(701);
  session.connection_uuid = Uuid(702);
  session.database_uuid = Uuid(703);
  session.dialect_profile_uuid = Uuid(704);
  session.catalog_epoch = 7;
  session.security_policy_epoch = 7;
  session.descriptor_epoch = 7;
  return session;
}

void SetEngineAuthority(sbsql::NativeRelationalBindingContext* context) {
  auto& authority = context->engine_statement_authority;
  authority.statement_uuid = context->statement_uuid;
  authority.statement_timestamp = context->statement_timestamp;
  authority.transaction_uuid = context->owning_transaction_uuid;
  authority.statement_snapshot_uuid = context->statement_snapshot_uuid;
  authority.statement_metadata_snapshot_uuid =
      context->statement_metadata_snapshot_uuid;
  authority.catalog_epoch_uuid = context->catalog_epoch_uuid;
  authority.local_transaction_id = context->local_transaction_id;
  authority.snapshot_visible_through_local_transaction_id =
      context->snapshot_visible_through_local_transaction_id;
}

std::uint32_t DescriptorFor(const sbsql::NativeExpressionAstNode& expression) {
  if (expression.literal_kind == sbsql::NativeLiteralAstKind::kString) return 3;
  if (expression.expression_kind ==
          sbsql::NativeExpressionAstKind::kBinary &&
      (expression.operator_name == "=" || expression.operator_name == "<>" ||
       expression.operator_name == "<" || expression.operator_name == "<=" ||
       expression.operator_name == ">" || expression.operator_name == ">=")) {
    return 4;
  }
  return 2;
}

sbsql::NativeRelationalBindingContext ContextFor(
    const sbsql::NativeRelationalAstDocument& ast, const bool unnest) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(710);
  context.catalog_epoch_uuid = Uuid(711);
  context.security_context_uuid = Uuid(712);
  context.statement_uuid = Uuid(713);
  context.owning_transaction_uuid = Uuid(714);
  context.statement_snapshot_uuid = Uuid(715);
  context.statement_metadata_snapshot_uuid = Uuid(716);
  context.local_transaction_id = 17;
  context.snapshot_visible_through_local_transaction_id = 18;
  SetEngineAuthority(&context);
  context.descriptors = {
      {1, Uuid(721), Uuid(731), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {2, Uuid(722), Uuid(732), sbsql::BoundNullability::kNullable,
       std::nullopt, std::nullopt, {}},
      {3, Uuid(723), Uuid(733), sbsql::BoundNullability::kNullable,
       Uuid(741), std::nullopt, {}},
      {4, Uuid(724), Uuid(734), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
  };
  if (unnest) {
    // The document root and element output share the engine JSON type UUID;
    // the path literal and arithmetic children retain text/numeric types.
    context.descriptors = {
        {2, Uuid(722), Uuid(738), sbsql::BoundNullability::kNonNull,
         std::nullopt, std::nullopt, {}},
        {3, Uuid(723), Uuid(738), sbsql::BoundNullability::kNullable,
         std::nullopt, std::nullopt, {}},
        {4, Uuid(724), Uuid(733), sbsql::BoundNullability::kNullable,
         Uuid(741), std::nullopt, {}},
    };
  }

  const bool wildcard = std::ranges::any_of(ast.expressions, [](const auto& e) {
    return e.expression_kind == sbsql::NativeExpressionAstKind::kWildcard;
  });
  const std::size_t wildcard_count = wildcard ? (unnest ? 1 : 3) : 0;
  std::uint32_t next_expression = 101;
  if (wildcard) {
    for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
      const auto descriptor_id = unnest ? 3 : static_cast<std::uint32_t>(ordinal + 1);
      context.expressions.push_back(
          {next_expression++, descriptor_id, std::nullopt,
           unnest ? std::nullopt
                  : std::optional<std::string>(Uuid(750 + ordinal))});
    }
  }
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind == sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput input;
    input.expression_id = next_expression++;
    if (unnest && ast.catalog_relation_sources.front()
                      .model_document_expression_id ==
                      expression.expression_id) {
      input.descriptor_id = 2;
    } else if (unnest &&
               expression.literal_kind ==
                   sbsql::NativeLiteralAstKind::kString) {
      input.descriptor_id = 4;
    } else if (unnest) {
      input.descriptor_id = 5;
    } else {
      input.descriptor_id = DescriptorFor(expression);
    }
    if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kFunctionCall &&
        expression.operator_name != "DOCUMENT_PATH") {
      input.function_uuid = Uuid(770 + expression.expression_id);
    }
    if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kIdentifier &&
        !unnest) {
      const auto& source = ast.catalog_relation_sources.front();
      const bool source_alias =
          expression.qualified_identifier.size() == 1 &&
          source.alias.has_value() &&
          expression.qualified_identifier.front().spelling ==
              source.alias->spelling &&
          expression.qualified_identifier.front().quoted ==
              source.alias->quoted;
      input.bound_name_uuid =
          source_alias ? Uuid(740) : Uuid(780 + expression.expression_id);
    }
    context.expressions.push_back(std::move(input));
  }

  const auto& relation = ast.relations.front();
  const auto& source_ast = ast.catalog_relation_sources.front();
  context.relations.push_back(
      {relation.relation_id,
       source_ast.model_operation_id == "DOCUMENT_UNNEST"
           ? "sblr.model-expand.document-unnest.v1"
           : (source_ast.model_operation_id == "DOCUMENT_PATH"
                  ? "sblr.model-source.document-path.v1"
                  : "sblr.model-source.document-find.v1")});
  if (wildcard) {
    for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
      context.outputs.push_back(
          {static_cast<std::uint32_t>(ordinal + 1),
           static_cast<std::uint32_t>(101 + ordinal),
           unnest ? "element" : std::array<const char*, 3>{"row_uuid", "join_key", "payload"}[ordinal],
           unnest ? 3u : static_cast<std::uint32_t>(ordinal + 1), true,
           static_cast<std::uint32_t>(ordinal), relation.relation_id});
    }
  } else {
    for (std::size_t ordinal = 0;
         ordinal < relation.output_expression_ids.size(); ++ordinal) {
      const auto ast_id = relation.output_expression_ids[ordinal];
      std::size_t preceding = 0;
      for (const auto& expression : ast.expressions) {
        if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kWildcard) continue;
        if (expression.expression_id == ast_id) break;
        ++preceding;
      }
      const auto& binding = context.expressions[wildcard_count + preceding];
      context.outputs.push_back(
          {static_cast<std::uint32_t>(ordinal + 1), binding.expression_id,
           "document_value_" + std::to_string(ordinal + 1),
           binding.descriptor_id, true, static_cast<std::uint32_t>(ordinal),
           relation.relation_id});
    }
  }

  if (!unnest) {
    sbsql::NativeCatalogRelationBindingInput source;
    source.source_id = 1;
    source.resolution_state =
        sbsql::NativeCatalogRelationResolutionState::kBound;
    source.object_uuid = Uuid(740);
    source.resolved_object_type = "document_collection";
    source.resolved_schema_uuid = Uuid(742);
    source.parent_object_uuid = Uuid(743);
    source.catalog_generation_id = 7;
    source.security_epoch = 7;
    source.resource_epoch = 7;
    source.columns = {{0, Uuid(750), 1, "row_uuid"},
                      {1, Uuid(751), 2, "join_key"},
                      {2, Uuid(752), 3, "payload"}};
    context.catalog_relations.push_back(std::move(source));
  }
  return context;
}

sbsql::NativeRelationalBindingContext GraphContextFor(
    const sbsql::NativeRelationalAstDocument& ast) {
  auto context = ContextFor(ast, false);
  context.descriptors = {
      {1, Uuid(721), Uuid(731), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {2, Uuid(722), Uuid(731), sbsql::BoundNullability::kNullable,
       std::nullopt, std::nullopt, {}},
      {3, Uuid(723), Uuid(731), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {4, Uuid(724), Uuid(733), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {5, Uuid(725), Uuid(733), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {6, Uuid(726), Uuid(733), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {7, Uuid(727), Uuid(733), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {8, Uuid(728), Uuid(732), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {9, Uuid(729), Uuid(733), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
  };
  context.expressions.clear();
  context.outputs.clear();
  context.relations.clear();
  context.catalog_relations.clear();
  const bool wildcard = std::ranges::any_of(ast.expressions, [](const auto& e) {
    return e.expression_kind == sbsql::NativeExpressionAstKind::kWildcard;
  });
  std::uint32_t next_expression = 101;
  if (wildcard) {
    static constexpr std::array<const char*, 9> kGraphColumns{
        "vertex_uuid",       "edge_uuid",       "path_uuid",
        "vertex_labels",     "vertex_properties", "edge_properties",
        "direction",         "depth",           "cycle_policy"};
    for (std::uint32_t ordinal = 0; ordinal < kGraphColumns.size(); ++ordinal) {
      context.expressions.push_back(
          {next_expression++, ordinal + 1, std::nullopt,
           Uuid(750 + ordinal)});
      context.outputs.push_back(
          {ordinal + 1, context.expressions.back().expression_id,
           kGraphColumns[ordinal],
           ordinal + 1, true, ordinal, ast.relations.front().relation_id});
    }
  }
  std::unordered_map<std::uint32_t, sbsql::NativeExpressionBindingInput>
      binding_by_ast;
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind == sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput input;
    input.expression_id = next_expression++;
    input.descriptor_id =
        expression.literal_kind == sbsql::NativeLiteralAstKind::kNumeric
            ? 8
            : (expression.literal_kind == sbsql::NativeLiteralAstKind::kString
                   ? 4
                   : 1);
    if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kIdentifier &&
        expression.qualified_identifier.size() == 1) {
      input.bound_name_uuid = Uuid(740);
    }
    context.expressions.push_back(input);
    binding_by_ast.emplace(expression.expression_id, std::move(input));
  }
  if (!wildcard) {
    for (std::size_t ordinal = 0;
         ordinal < ast.relations.front().output_expression_ids.size(); ++ordinal) {
      const auto& input =
          binding_by_ast.at(ast.relations.front().output_expression_ids[ordinal]);
      context.outputs.push_back(
          {static_cast<std::uint32_t>(ordinal + 1), input.expression_id,
           "graph_value_" + std::to_string(ordinal + 1), input.descriptor_id,
           true, static_cast<std::uint32_t>(ordinal),
           ast.relations.front().relation_id});
    }
  }
  const auto& source_ast = ast.catalog_relation_sources.front();
  context.relations.push_back(
      {ast.relations.front().relation_id,
       source_ast.model_operation_id == "GRAPH_EXPAND"
           ? "sblr.model-expand.graph-expand.v1"
           : "sblr.model-source.graph-match.v1"});
  sbsql::NativeCatalogRelationBindingInput source;
  source.source_id = source_ast.source_id;
  source.resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kBound;
  source.object_uuid = Uuid(740);
  source.resolved_object_type = "graph";
  source.resolved_schema_uuid = Uuid(742);
  source.parent_object_uuid = Uuid(743);
  source.catalog_generation_id = 7;
  source.security_epoch = 7;
  source.resource_epoch = 7;
  static constexpr std::array<const char*, 9> kGraphColumns{
      "vertex_uuid",       "edge_uuid",       "path_uuid",
      "vertex_labels",     "vertex_properties", "edge_properties",
      "direction",         "depth",           "cycle_policy"};
  for (std::uint32_t ordinal = 0; ordinal < kGraphColumns.size(); ++ordinal) {
    source.columns.push_back(
        {ordinal, Uuid(750 + ordinal), ordinal + 1, kGraphColumns[ordinal]});
  }
  context.catalog_relations.push_back(std::move(source));
  return context;
}

sbsql::NativeRelationalBindingContext SpatialColumnarContextFor(
    const sbsql::NativeRelationalAstDocument& ast) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(1900);
  context.catalog_epoch_uuid = Uuid(1901);
  context.security_context_uuid = Uuid(1902);
  context.statement_uuid = Uuid(1903);
  context.statement_timestamp = "2026-08-11T02:00:00.123456789Z";
  context.owning_transaction_uuid = Uuid(1904);
  context.statement_snapshot_uuid = Uuid(1905);
  context.statement_metadata_snapshot_uuid = Uuid(1906);
  context.local_transaction_id = 79;
  context.snapshot_visible_through_local_transaction_id = 78;
  SetEngineAuthority(&context);
  context.descriptors = {
      {1, Uuid(1911), Uuid(1921), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "uuid"},
      {2, Uuid(1912), Uuid(1922), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "geometry"},
      {3, Uuid(1913), Uuid(1921), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "uuid"},
      {4, Uuid(1914), Uuid(1924), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "boolean"},
      {5, Uuid(1915), Uuid(1925), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "real64"},
      {6, Uuid(1916), Uuid(1926), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "uint64"},
      {7, Uuid(1917), Uuid(1927), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "int64"},
      {8, Uuid(1918), Uuid(1928), sbsql::BoundNullability::kNullable,
       std::nullopt, std::nullopt, {}, "text"},
      {9, Uuid(1919), Uuid(1927), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}, "int64"},
  };
  const auto& source_ast = ast.catalog_relation_sources.front();
  const bool spatial =
      source_ast.source_kind == sbsql::NativeRelationSourceAstKind::kSpatial;
  sbsql::NativeCatalogRelationBindingInput source;
  source.source_id = source_ast.source_id;
  source.resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kBound;
  source.object_uuid = Uuid(1940);
  source.resolved_object_type =
      spatial ? "spatial_collection" : "logical_relation";
  source.resolved_schema_uuid = Uuid(1941);
  source.parent_object_uuid = Uuid(1942);
  source.catalog_generation_id = 79;
  source.security_epoch = 79;
  source.resource_epoch = 79;
  if (spatial) {
    source.columns = {{0, Uuid(1950), 1, "row_uuid"},
                      {1, Uuid(1951), 2, "spatial_value"},
                      {2, Uuid(1952), 3, "crs_uuid"}};
    if (source_ast.model_operation_ids.size() > 1) {
      source.spatial_crs_uuid = Uuid(1970);
      source.spatial_crs_generation = 79;
    }
    for (std::size_t index = 1;
         index < source_ast.model_operation_ids.size(); ++index) {
      context.spatial_crs_bindings.push_back(
          {source_ast.model_operation_ids[index], Uuid(1970), 79});
    }
  } else {
    source.columns = {{0, Uuid(1950), 1, "row_uuid"},
                      {1, Uuid(1951), 7, "join_key"},
                      {2, Uuid(1952), 8, "payload"},
                      {3, Uuid(1953), 9, "hidden_join_key"}};
  }
  context.catalog_relations.push_back(source);
  context.relations.push_back(
      {ast.relations.front().relation_id,
       spatial ? "sblr.model-source.spatial.v1"
               : "sblr.model-source.columnar.v1"});

  std::vector<const sbsql::NativeCatalogColumnBindingInput*> selected;
  if (!spatial) {
    if (!source_ast.model_columnar_project_names.empty()) {
      for (const auto& name : source_ast.model_columnar_project_names) {
        const auto found = std::ranges::find_if(
            source.columns, [&](const auto& column) {
              return column.canonical_name_key == name.back().spelling;
            });
        selected.push_back(&*found);
      }
    } else {
      for (const auto& column : source.columns) selected.push_back(&column);
    }
  }
  std::vector<std::string> output_names;
  std::vector<std::uint32_t> output_descriptors;
  std::vector<std::string> output_bindings;
  if (spatial) {
    output_names = {"row_uuid", "spatial_value", "crs_uuid"};
    output_descriptors = {1, 2, 3};
    output_bindings = {Uuid(1950), Uuid(1951), Uuid(1952)};
    if (source_ast.model_spatial_match_expression_id.has_value()) {
      output_names.push_back("predicate_truth");
      output_descriptors.push_back(4);
      output_bindings.push_back(Uuid(1940));
    }
    if (source_ast.model_spatial_nearest_expression_id.has_value()) {
      output_names.push_back("distance");
      output_descriptors.push_back(5);
      output_bindings.push_back(Uuid(1940));
    }
  } else {
    for (const auto* column : selected) {
      output_names.push_back(column->canonical_name_key);
      output_descriptors.push_back(column->descriptor_id);
      output_bindings.push_back(column->column_uuid);
    }
  }
  for (std::size_t ordinal = 0; ordinal < output_names.size(); ++ordinal) {
    const auto expression_id = static_cast<std::uint32_t>(1001 + ordinal);
    context.expressions.push_back(
        {expression_id, output_descriptors[ordinal], std::nullopt,
         output_bindings[ordinal]});
    context.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), expression_id,
         output_names[ordinal], output_descriptors[ordinal], true,
         static_cast<std::uint32_t>(ordinal), ast.relations.front().relation_id});
  }
  const auto source_alias = source_ast.model_source_alias.has_value()
                                ? source_ast.model_source_alias
                                : source_ast.alias;
  const auto descriptor_for_column = [&](const std::string& name) {
    const auto found = std::ranges::find_if(
        source.columns, [&](const auto& column) {
          return column.canonical_name_key == name;
        });
    return found == source.columns.end() ? 7u : found->descriptor_id;
  };
  const auto binding_for_column = [&](const std::string& name) {
    const auto found = std::ranges::find_if(
        source.columns, [&](const auto& column) {
          return column.canonical_name_key == name;
        });
    return found == source.columns.end() ? std::optional<std::string>{}
                                         : std::optional<std::string>{
                                               found->column_uuid};
  };
  std::uint32_t next_expression_id = 2001;
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput input;
    input.expression_id = next_expression_id++;
    input.descriptor_id = 7;
    if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kParameter ||
        (expression.expression_kind ==
             sbsql::NativeExpressionAstKind::kFunctionCall &&
         expression.operator_name == "POINT")) {
      input.descriptor_id = 2;
    } else if (expression.operator_name == "SPATIAL_MATCH" ||
               expression.operator_name == "COLUMNAR_FILTER" ||
               expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kBinary) {
      input.descriptor_id = 4;
    } else if (expression.operator_name == "SPATIAL_NEAREST") {
      input.descriptor_id = 5;
    } else if (source_ast.model_spatial_top_k_expression_id ==
               expression.expression_id) {
      input.descriptor_id = 6;
    } else if (expression.literal_kind ==
               sbsql::NativeLiteralAstKind::kString) {
      input.descriptor_id = 8;
    } else if (expression.operator_name == "SPATIAL_SOURCE") {
      input.descriptor_id = 2;
    } else if (expression.operator_name == "COLUMNAR_SOURCE") {
      input.descriptor_id = source.columns.front().descriptor_id;
    } else if (expression.operator_name == "COLUMNAR_PROJECT" &&
               !selected.empty()) {
      input.descriptor_id = selected.front()->descriptor_id;
    }
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kIdentifier) {
      const bool alias = expression.qualified_identifier.size() == 1 &&
                         source_alias.has_value() &&
                         expression.qualified_identifier.front().spelling ==
                             source_alias->spelling;
      if (alias) {
        input.descriptor_id = 1;
        input.bound_name_uuid = Uuid(1940);
      } else if (spatial &&
                 std::ranges::find(
                     source_ast.model_spatial_crs_expression_ids,
                     expression.expression_id) !=
                     source_ast.model_spatial_crs_expression_ids.end()) {
        input.descriptor_id = 3;
        input.bound_name_uuid = Uuid(1970);
      } else if (!spatial && !expression.qualified_identifier.empty()) {
        const auto& name = expression.qualified_identifier.back().spelling;
        input.descriptor_id = descriptor_for_column(name);
        input.bound_name_uuid = binding_for_column(name);
      }
    }
    if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kFunctionCall &&
        expression.operator_name == "POINT") {
      input.function_uuid = Uuid(1980);
    }
    context.expressions.push_back(std::move(input));
  }
  return context;
}

sbsql::NativeRelationalBindingContext MultimodelJoinContextFor(
    const sbsql::NativeRelationalAstDocument& ast,
    const bool carry_timestamp) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(2000);
  context.catalog_epoch_uuid = Uuid(2001);
  context.security_context_uuid = Uuid(2002);
  context.statement_uuid = Uuid(2003);
  context.statement_timestamp =
      carry_timestamp ? "2026-08-12T20:00:00.123456789Z" : "";
  context.owning_transaction_uuid = Uuid(2004);
  context.statement_snapshot_uuid = Uuid(2005);
  context.statement_metadata_snapshot_uuid = Uuid(2006);
  context.local_transaction_id = 80;
  context.snapshot_visible_through_local_transaction_id = 79;
  SetEngineAuthority(&context);
  context.search_analyzer_uuid = Uuid(2007);
  context.search_analyzer_generation = 80;
  const auto source_count = ast.catalog_relation_sources.size();
  std::vector<std::vector<std::uint32_t>> source_projection_expression_ids(
      source_count);
  std::vector<std::vector<std::uint32_t>> source_projection_descriptor_ids(
      source_count);
  std::uint32_t next_descriptor_id = 1;
  std::uint32_t next_expression_id = 1;
  std::uint32_t next_output_id = 1;
  for (std::size_t ordinal = 0; ordinal < source_count; ++ordinal) {
    const auto persisted_descriptor_id = next_descriptor_id++;
    context.descriptors.push_back(
        {persisted_descriptor_id, Uuid(2010 + ordinal), Uuid(2030 + ordinal),
         sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {},
         "uuid"});
    const auto& ast_source = ast.catalog_relation_sources[ordinal];
    sbsql::NativeCatalogRelationBindingInput source;
    source.source_id = ast_source.source_id;
    source.resolution_state =
        sbsql::NativeCatalogRelationResolutionState::kBound;
    source.object_uuid = Uuid(2050 + ordinal);
    source.resolved_schema_uuid = Uuid(2070 + ordinal);
    source.catalog_generation_id = 80;
    source.security_epoch = 80;
    source.resource_epoch = 80;
    switch (ast_source.source_kind) {
      case sbsql::NativeRelationSourceAstKind::kDocument:
        source.resolved_object_type = "document_collection";
        break;
      case sbsql::NativeRelationSourceAstKind::kGraph:
        source.resolved_object_type = "graph";
        break;
      case sbsql::NativeRelationSourceAstKind::kKeyValue:
        source.resolved_object_type = "key_value";
        break;
      case sbsql::NativeRelationSourceAstKind::kTimeSeries:
        source.resolved_object_type = "time_series";
        break;
      case sbsql::NativeRelationSourceAstKind::kVector:
        source.resolved_object_type = "vector";
        break;
      case sbsql::NativeRelationSourceAstKind::kSearch:
        source.resolved_object_type = "search";
        break;
      case sbsql::NativeRelationSourceAstKind::kSpatial:
        source.resolved_object_type = "spatial_collection";
        break;
      case sbsql::NativeRelationSourceAstKind::kColumnar:
        source.resolved_object_type = "logical_relation";
        break;
      default: source.resolved_object_type = "table"; break;
    }
    source.columns.push_back(
        {0, Uuid(2100 + ordinal), persisted_descriptor_id,
         "source_" + std::to_string(ordinal + 1)});
    context.catalog_relations.push_back(std::move(source));
    const bool vector = ast_source.source_kind ==
                        sbsql::NativeRelationSourceAstKind::kVector;
    const bool search = ast_source.source_kind ==
                        sbsql::NativeRelationSourceAstKind::kSearch;
    const std::vector<std::string_view> output_names =
        vector ? std::vector<std::string_view>{"row_uuid", "distance", "score"}
        : search ? std::vector<std::string_view>{
                       "document_uuid", "analyzer_uuid",
                       "analyzer_generation", "score", "rank"}
                 : std::vector<std::string_view>{
                       context.catalog_relations.back()
                           .columns.front()
                           .canonical_name_key};
    for (std::size_t output_ordinal = 0;
         output_ordinal < output_names.size(); ++output_ordinal) {
      auto descriptor_id = persisted_descriptor_id;
      if (vector || search) {
        descriptor_id = next_descriptor_id++;
        context.descriptors.push_back(
            {descriptor_id,
             Uuid(2200 + ordinal * 8 + output_ordinal),
             Uuid(2300 + ordinal * 8 + output_ordinal),
             sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {},
             output_ordinal == 2 && search ? "uint64" : "uuid"});
      }
      const auto expression_id = next_expression_id++;
      const auto bound_name =
          search && (output_ordinal == 1 || output_ordinal == 2)
              ? context.search_analyzer_uuid
              : (vector || search)
                    ? context.catalog_relations.back().object_uuid
                    : context.catalog_relations.back()
                          .columns.front()
                          .column_uuid;
      context.expressions.push_back(
          {expression_id, descriptor_id, std::nullopt, bound_name});
      context.outputs.push_back(
          {next_output_id++, expression_id, std::string(output_names[output_ordinal]),
           descriptor_id, true, static_cast<std::uint32_t>(output_ordinal),
           static_cast<std::uint32_t>(ordinal + 1)});
      source_projection_expression_ids[ordinal].push_back(expression_id);
      source_projection_descriptor_ids[ordinal].push_back(descriptor_id);
    }
  }

  for (std::size_t ordinal = 0; ordinal < source_count; ++ordinal) {
    const auto& source = ast.catalog_relation_sources[ordinal];
    if (source.source_kind ==
        sbsql::NativeRelationSourceAstKind::kCatalogRelation) {
      continue;
    }
    std::unordered_set<std::uint32_t> closure;
    std::vector<std::uint32_t> pending{
        source.model_operation_expression_ids.front()};
    const auto relation = std::ranges::find_if(
        ast.relations, [&](const auto& candidate) {
          return candidate.relation_kind ==
                     sbsql::NativeRelationAstKind::kCatalogSource &&
                 candidate.relation_source_ids ==
                     std::vector<std::uint32_t>{source.source_id};
        });
    pending.insert(pending.end(), relation->predicate_expression_ids.begin(),
                   relation->predicate_expression_ids.end());
    while (!pending.empty()) {
      const auto expression_id = pending.back();
      pending.pop_back();
      if (!closure.insert(expression_id).second) continue;
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      pending.insert(pending.end(), expression->child_expression_ids.begin(),
                     expression->child_expression_ids.end());
    }
    std::vector<std::uint32_t> ordered(closure.begin(), closure.end());
    std::ranges::sort(ordered);
    const auto primary_id = source.model_operation_expression_ids.front();
    const auto primary = std::ranges::find_if(
        ast.expressions, [&](const auto& candidate) {
          return candidate.expression_id == primary_id;
        });
    const auto alias_id = primary->child_expression_ids.empty()
                              ? 0
                              : primary->child_expression_ids.front();
    for (const auto ast_expression_id : ordered) {
      std::optional<std::string> bound_name;
      if (ast_expression_id == alias_id) {
        bound_name = context.catalog_relations[ordinal].object_uuid;
      } else if (source.model_search_analyzer_expression_id ==
                 ast_expression_id) {
        bound_name = context.search_analyzer_uuid;
      } else if (ast_expression_id == primary_id &&
                 (source.source_kind ==
                      sbsql::NativeRelationSourceAstKind::kSpatial ||
                  source.source_kind ==
                      sbsql::NativeRelationSourceAstKind::kColumnar)) {
        bound_name = context.catalog_relations[ordinal].object_uuid;
      }
      context.expressions.push_back(
          {next_expression_id++,
           context.catalog_relations[ordinal].columns.front().descriptor_id,
           std::nullopt, bound_name});
    }
  }

  for (std::size_t join_ordinal = 1; join_ordinal < source_count;
       ++join_ordinal) {
    const auto relation_id =
        static_cast<std::uint32_t>(source_count + join_ordinal);
    context.relations.push_back({relation_id, "join.cross.v1"});
    for (std::size_t ordinal = 0; ordinal <= join_ordinal; ++ordinal) {
      for (std::size_t output_ordinal = 0;
           output_ordinal < source_projection_expression_ids[ordinal].size();
           ++output_ordinal) {
        const auto source_output = std::ranges::find_if(
            context.outputs, [&](const auto& output) {
              return output.relation_id == ordinal + 1 &&
                     output.ordinal == output_ordinal;
            });
        context.outputs.push_back(
            {next_output_id++,
             source_projection_expression_ids[ordinal][output_ordinal],
             source_output->output_name_utf8,
             source_projection_descriptor_ids[ordinal][output_ordinal], true,
             0, relation_id});
      }
    }
    std::uint32_t output_ordinal = 0;
    for (auto& output : context.outputs) {
      if (output.relation_id == relation_id) output.ordinal = output_ordinal++;
    }
  }
  return context;
}

bool MultimodelJoinBindingLowering() {
  const auto timestamp_cst = sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "DOCUMENT_SOURCE(app.docs) AS d CROSS JOIN "
      "SPATIAL_SOURCE(app.spatial) AS s CROSS JOIN "
      "COLUMNAR_SOURCE(app.columnar) AS c;");
  const auto timestamp_ast = sbsql::BuildAst(timestamp_cst);
  bool passed = Require(
      timestamp_ast.native_relational.accepted() &&
          timestamp_ast.native_relational.catalog_relation_sources.size() == 4 &&
          timestamp_ast.native_relational.relations.size() == 7 &&
          timestamp_ast.native_relational.model_object_resolution_requests.size() ==
              3,
      "four-leg multimodel AST was not accepted: " +
          DiagnosticSummary(timestamp_ast.messages));
  if (!timestamp_ast.native_relational.accepted()) return false;
  auto timestamp_context =
      MultimodelJoinContextFor(timestamp_ast.native_relational, true);
  const auto timestamp_bound = sbsql::BindAst(
      timestamp_ast, timestamp_cst, Config(), Session(), {},
      &timestamp_context);
  const auto timestamp_lowered =
      sbsql::LowerToSblr(timestamp_bound, timestamp_cst, Session());
  const auto timestamp_verified = sbsql::VerifySblrEnvelope(timestamp_lowered);
  passed &= Require(
      timestamp_bound.bound && timestamp_bound.native_relational.bound &&
          !timestamp_lowered.messages.has_errors() &&
          HasOperand(timestamp_lowered, "text",
                     "relational_statement_timestamp") &&
          timestamp_verified.admitted,
      "four-leg multimodel binding/lowering failed: " +
          DiagnosticSummary(timestamp_bound.messages) + ":" +
          DiagnosticSummary(timestamp_lowered.messages) + ":" +
          DiagnosticSummary(timestamp_verified.messages));

  const auto nontimestamp_cst = sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "DOCUMENT_SOURCE(app.docs) AS d CROSS JOIN "
      "GRAPH_SOURCE(app.graph_fixture) AS g WHERE "
      "GRAPH_MATCH(g, 'vertex(*)');");
  const auto nontimestamp_ast = sbsql::BuildAst(nontimestamp_cst);
  if (!Require(nontimestamp_ast.native_relational.accepted(),
               "three-leg explicit multimodel AST was not accepted: " +
                   DiagnosticSummary(nontimestamp_ast.messages))) {
    return false;
  }
  auto nontimestamp_context =
      MultimodelJoinContextFor(nontimestamp_ast.native_relational, false);
  const auto nontimestamp_bound = sbsql::BindAst(
      nontimestamp_ast, nontimestamp_cst, Config(), Session(), {},
      &nontimestamp_context);
  const auto nontimestamp_lowered =
      sbsql::LowerToSblr(nontimestamp_bound, nontimestamp_cst, Session());
  const auto nontimestamp_verified =
      sbsql::VerifySblrEnvelope(nontimestamp_lowered);
  passed &= Require(
      nontimestamp_ast.native_relational.accepted() &&
          nontimestamp_bound.bound &&
          !HasOperand(nontimestamp_lowered, "text",
                      "relational_statement_timestamp") &&
          nontimestamp_verified.admitted,
      "document/graph/relational composition minted a timestamp: " +
          DiagnosticSummary(nontimestamp_bound.messages) + ":" +
          DiagnosticSummary(nontimestamp_lowered.messages) + ":" +
          DiagnosticSummary(nontimestamp_verified.messages));

  const auto full_nine_cst = sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "DOCUMENT_SOURCE(app.docs) AS d CROSS JOIN "
      "GRAPH_SOURCE(app.graph_fixture) AS g CROSS JOIN "
      "KEY_VALUE_SOURCE(app.kv) AS kv CROSS JOIN "
      "TIME_SERIES_SOURCE(app.series) AS ts CROSS JOIN "
      "VECTOR_SOURCE(app.vectors) AS v CROSS JOIN "
      "SEARCH_SOURCE(app.search_fixture) AS q CROSS JOIN "
      "SPATIAL_SOURCE(app.spatial) AS s CROSS JOIN "
      "COLUMNAR_SOURCE(app.columnar) AS c WHERE "
      "GRAPH_MATCH(g, 'vertex(*)') AND KV_KEY(kv) = 'alpha' AND "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z') AND "
      "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 2) AND "
      "SEARCH_MATCH(q, SEARCH_TERMS('quick fox'), app.ascii_v1, 3);");
  const auto full_nine_ast = sbsql::BuildAst(full_nine_cst);
  if (!Require(full_nine_ast.native_relational.accepted(),
               "full-nine explicit multimodel AST was not accepted: " +
                   DiagnosticSummary(full_nine_ast.messages))) {
    return false;
  }
  auto full_nine_context =
      MultimodelJoinContextFor(full_nine_ast.native_relational, true);
  const auto full_nine_bound = sbsql::BindAst(
      full_nine_ast, full_nine_cst, Config(), Session(), {},
      &full_nine_context);
  const auto full_nine_lowered =
      sbsql::LowerToSblr(full_nine_bound, full_nine_cst, Session());
  const auto full_nine_verified = sbsql::VerifySblrEnvelope(full_nine_lowered);
  passed &= Require(
      full_nine_ast.native_relational.accepted() &&
          full_nine_ast.native_relational.catalog_relation_sources.size() == 9 &&
          full_nine_ast.native_relational.relations.size() == 17 &&
          full_nine_bound.bound &&
          full_nine_bound.native_relational.relations.size() == 17 &&
          !full_nine_lowered.messages.has_errors() &&
          full_nine_verified.admitted,
      "full-nine multimodel translation was not admitted: " +
          DiagnosticSummary(full_nine_ast.messages) + ":" +
          DiagnosticSummary(full_nine_bound.messages) + ":" +
          DiagnosticSummary(full_nine_lowered.messages) + ":" +
          DiagnosticSummary(full_nine_verified.messages));

  const auto ten_source_ast = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "DOCUMENT_SOURCE(app.docs) AS d CROSS JOIN "
      "GRAPH_SOURCE(app.graph_fixture) AS g CROSS JOIN "
      "KEY_VALUE_SOURCE(app.kv) AS kv CROSS JOIN "
      "TIME_SERIES_SOURCE(app.series) AS ts CROSS JOIN "
      "VECTOR_SOURCE(app.vectors) AS v CROSS JOIN "
      "SEARCH_SOURCE(app.search_fixture) AS q CROSS JOIN "
      "SPATIAL_SOURCE(app.spatial) AS s CROSS JOIN "
      "COLUMNAR_SOURCE(app.columnar) AS c CROSS JOIN app.extra WHERE "
      "GRAPH_MATCH(g, 'vertex(*)') AND KV_KEY(kv) = 'alpha' AND "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z') AND "
      "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 2) AND "
      "SEARCH_MATCH(q, SEARCH_TERMS('quick fox'), app.ascii_v1, 3);"));
  passed &= Require(
      ten_source_ast.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(ten_source_ast.messages,
                        "SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1"),
      "ten-source composition did not fail closed");

  auto unattached_ast = full_nine_ast;
  unattached_ast.native_relational.catalog_relation_sources[1]
      .model_operation_expression_ids.clear();
  auto unattached_context = full_nine_context;
  const auto unattached_bound = sbsql::BindAst(
      unattached_ast, full_nine_cst, Config(), Session(), {},
      &unattached_context);
  passed &= Require(
      !unattached_bound.bound &&
          HasDiagnostic(unattached_bound.messages,
                        "SB_MODEL_BINDING_INCOMPLETE_V1"),
      "unattached multimodel operation root was not refused");

  auto lineage_context = full_nine_context;
  lineage_context.outputs.back().descriptor_id = 1;
  const auto lineage_bound = sbsql::BindAst(
      full_nine_ast, full_nine_cst, Config(), Session(), {},
      &lineage_context);
  passed &= Require(
      !lineage_bound.bound &&
          HasDiagnostic(lineage_bound.messages,
                        "QOW-DIAG-BOUNDAST-OUTPUT"),
      "multimodel descriptor-lineage substitution was not refused");

  auto duplicate_source_ast = full_nine_ast;
  duplicate_source_ast.native_relational.catalog_relation_sources[2].source_id =
      duplicate_source_ast.native_relational.catalog_relation_sources[1].source_id;
  const auto duplicate_source_bound = sbsql::BindAst(
      duplicate_source_ast, full_nine_cst, Config(), Session(), {},
      &full_nine_context);
  passed &= Require(
      !duplicate_source_bound.bound &&
          HasDiagnostic(duplicate_source_bound.messages,
                        "SB_MODEL_BINDING_INCOMPLETE_V1"),
      "duplicate multimodel source identity was not refused");

  auto missing_source_relation_ast = full_nine_ast;
  const auto missing_source_relation = std::ranges::find_if(
      missing_source_relation_ast.native_relational.relations,
      [](const auto& relation) {
        return relation.relation_kind ==
                   sbsql::NativeRelationAstKind::kCatalogSource &&
               relation.relation_source_ids ==
                   std::vector<std::uint32_t>{1};
      });
  if (missing_source_relation !=
      missing_source_relation_ast.native_relational.relations.end()) {
    missing_source_relation_ast.native_relational.relations.erase(
        missing_source_relation);
  }
  const auto missing_source_relation_bound = sbsql::BindAst(
      missing_source_relation_ast, full_nine_cst, Config(), Session(), {},
      &full_nine_context);
  passed &= Require(
      !missing_source_relation_bound.bound &&
          HasDiagnostic(missing_source_relation_bound.messages,
                        "QOW-DIAG-BOUNDAST-RELATION"),
      "missing multimodel source relation was not refused before access");

  auto cyclic_bound = full_nine_bound;
  cyclic_bound.native_relational.relations.back().input_relation_ids.front() =
      cyclic_bound.native_relational.root_relation_id;
  const auto cyclic_lowered =
      sbsql::LowerToSblr(cyclic_bound, full_nine_cst, Session());
  passed &= Require(
      cyclic_lowered.messages.has_errors() &&
          HasDiagnostic(cyclic_lowered.messages,
                        "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "cyclic multimodel relation DAG was not refused");
  return passed;
}

bool SpatialColumnarBindingLowering(
    const std::string_view sql,
    const std::vector<std::string>& expected_operations) {
  const auto cst = sbsql::BuildCst(sql);
  const auto ast = sbsql::BuildAst(cst);
  if (!ast.native_relational.accepted()) {
    return Require(false, "spatial/columnar binding fixture did not parse: " +
                              DiagnosticSummary(ast.messages));
  }
  auto context = SpatialColumnarContextFor(ast.native_relational);
  const auto bound =
      sbsql::BindAst(ast, cst, Config(), Session(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
  const auto operation_count = std::ranges::count_if(
      lowered.operands, [](const auto& operand) {
        if (operand.type != "relational_expression_v1") return false;
        const auto source = Hex("SPATIAL_SOURCE");
        const auto match = Hex("SPATIAL_MATCH");
        const auto nearest = Hex("SPATIAL_NEAREST");
        const auto columnar_source = Hex("COLUMNAR_SOURCE");
        const auto filter = Hex("COLUMNAR_FILTER");
        const auto project = Hex("COLUMNAR_PROJECT");
        return operand.value.find("|" + source + "|") != std::string::npos ||
               operand.value.find("|" + match + "|") != std::string::npos ||
               operand.value.find("|" + nearest + "|") != std::string::npos ||
               operand.value.find("|" + columnar_source + "|") !=
                   std::string::npos ||
               operand.value.find("|" + filter + "|") != std::string::npos ||
               operand.value.find("|" + project + "|") != std::string::npos;
      });
  const auto verified = sbsql::VerifySblrEnvelope(lowered);
  return Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.catalog_relation_sources.size() == 1 &&
          bound.native_relational.catalog_relation_sources.front()
                  .model_operation_ids == expected_operations &&
          bound.native_relational.catalog_relation_sources.front()
                  .model_operation_expression_ids.size() ==
              expected_operations.size() &&
          !lowered.messages.has_errors() &&
          operation_count == expected_operations.size() &&
          verified.admitted,
      "spatial/columnar binding/lowering carriage failed: " +
          DiagnosticSummary(bound.messages) + ":" +
          DiagnosticSummary(lowered.messages) + ":" +
          DiagnosticSummary(verified.messages));
}

sbsql::NativeRelationalBindingContext TimeSeriesContextFor(
    const sbsql::NativeRelationalAstDocument& ast) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(910);
  context.catalog_epoch_uuid = Uuid(911);
  context.security_context_uuid = Uuid(912);
  context.statement_uuid = Uuid(913);
  context.statement_timestamp = "2026-08-10T12:00:00.123456789Z";
  context.owning_transaction_uuid = Uuid(914);
  context.statement_snapshot_uuid = Uuid(915);
  context.statement_metadata_snapshot_uuid = Uuid(916);
  context.local_transaction_id = 76;
  context.snapshot_visible_through_local_transaction_id = 75;
  SetEngineAuthority(&context);
  const auto& source_ast = ast.catalog_relation_sources.front();
  const bool downsample =
      source_ast.model_operation_id == "TIME_SERIES_DOWNSAMPLE";
  const bool bucket = source_ast.model_bucket_expression_id.has_value();
  context.descriptors = {
      {1, Uuid(921), Uuid(931), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {2, Uuid(922), Uuid(931), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {3, Uuid(923), Uuid(931), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {4, Uuid(924), Uuid(932), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::optional<std::string>("UTC"), {}},
      {5, Uuid(925), Uuid(933), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {6, Uuid(926), Uuid(934), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {7, Uuid(927), Uuid(935), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
  };
  if (bucket || downsample) {
    context.descriptors.push_back(
        {8, Uuid(928), Uuid(936), sbsql::BoundNullability::kNonNull,
         std::nullopt, std::nullopt, {}});
  }
  if (downsample) {
    context.descriptors.push_back(
        {9, Uuid(929), Uuid(937), sbsql::BoundNullability::kNonNull,
         std::nullopt, std::nullopt, {}});
    context.descriptors.push_back(
        {10, Uuid(960), Uuid(932), sbsql::BoundNullability::kNonNull,
         std::nullopt, std::optional<std::string>("UTC"), {}});
    if (source_ast.model_time_series_aggregate_id == "COUNT") {
      context.descriptors.push_back(
          {11, Uuid(961), Uuid(937), sbsql::BoundNullability::kNonNull,
           std::nullopt, std::nullopt, {}});
    }
  }
  sbsql::NativeCatalogRelationBindingInput source;
  source.source_id = source_ast.source_id;
  source.resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kBound;
  source.object_uuid = Uuid(940);
  source.resolved_object_type = "time_series";
  source.resolved_schema_uuid = Uuid(941);
  source.parent_object_uuid = Uuid(942);
  source.catalog_generation_id = 76;
  source.security_epoch = 77;
  source.resource_epoch = 78;
  source.columns = {{0, Uuid(950), 1, "row_uuid"},
                    {1, Uuid(951), 2, "series_uuid"},
                    {2, Uuid(952), 3, "metric_uuid"},
                    {3, Uuid(953), 4, "point_timestamp"},
                    {4, Uuid(954), 5, "tags"},
                    {5, Uuid(955), 6, "value"}};
  context.catalog_relations.push_back(std::move(source));
  context.relations.push_back(
      {ast.relations.front().relation_id,
       downsample ? "sblr.model-aggregate.time-series-downsample.v1"
                  : "sblr.model-source.time-series-range-read.v1"});
  const bool wildcard = std::ranges::any_of(ast.expressions, [](const auto& e) {
    return e.expression_kind == sbsql::NativeExpressionAstKind::kWildcard;
  });
  std::uint32_t next_expression = 101;
  if (wildcard) {
    static constexpr std::array<const char*, 6> kNames{
        "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
        "value"};
    for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
      context.expressions.push_back(
          {next_expression++, ordinal + 1, std::nullopt, Uuid(950 + ordinal)});
      context.outputs.push_back(
          {ordinal + 1, context.expressions.back().expression_id,
           kNames[ordinal], ordinal + 1, true, ordinal,
           ast.relations.front().relation_id});
    }
  }
  const std::unordered_map<std::string, std::pair<std::uint32_t, std::string>>
      projected{{"row_uuid", {1, Uuid(950)}},
                 {"series_uuid", {2, Uuid(951)}},
                 {"metric_uuid", {3, Uuid(952)}},
                 {"point_timestamp", {4, Uuid(953)}},
                 {"tags", {5, Uuid(954)}},
                 {"value", {6, Uuid(955)}}};
  std::unordered_map<std::uint32_t, sbsql::NativeExpressionBindingInput>
      binding_by_ast;
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput binding;
    binding.expression_id = next_expression++;
    if (expression.expression_id ==
            source_ast.model_time_series_alias_expression_id.value_or(0)) {
      binding.descriptor_id = 1;
      binding.bound_name_uuid = Uuid(940);
    } else if (expression.expression_id ==
                   source_ast.model_range_start_expression_id.value_or(0) ||
               expression.expression_id ==
                   source_ast.model_range_end_expression_id.value_or(0)) {
      binding.descriptor_id = 4;
    } else if (expression.expression_id ==
                   source_ast.model_interval_expression_id.value_or(0) ||
               (expression.expression_kind ==
                    sbsql::NativeExpressionAstKind::kLiteral &&
                expression.literal_kind ==
                    sbsql::NativeLiteralAstKind::kTemporal)) {
      binding.descriptor_id = 8;
    } else if (expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kIdentifier &&
               !expression.qualified_identifier.empty()) {
      const auto terminal = expression.qualified_identifier.back().spelling;
      const auto found = projected.find(terminal);
      if (found != projected.end()) {
        binding.descriptor_id = found->second.first;
        binding.bound_name_uuid = found->second.second;
      }
    } else if (expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kLiteral &&
               expression.literal_kind ==
                   sbsql::NativeLiteralAstKind::kString) {
      binding.descriptor_id = 5;
    } else if (expression.operator_name == "TIME_RANGE") {
      binding.descriptor_id = 7;
    } else if (expression.operator_name == "TIME_BUCKET") {
      binding.descriptor_id = 4;
    } else if (expression.operator_name == "TIME_DOWNSAMPLE") {
      binding.descriptor_id =
          source_ast.model_time_series_aggregate_id == "COUNT" ? 11 : 6;
    }
    context.expressions.push_back(binding);
    binding_by_ast.emplace(expression.expression_id, std::move(binding));
  }
  if (bucket && !downsample) {
    const std::array<std::uint32_t, 4> descriptors{2, 3, 5, 6};
    const std::array<std::string, 4> names{
        Uuid(951), Uuid(952), Uuid(954), Uuid(955)};
    for (std::size_t ordinal = 0; ordinal < descriptors.size(); ++ordinal) {
      context.expressions.push_back(
          {next_expression++, descriptors[ordinal], std::nullopt,
           names[ordinal]});
    }
  }
  if (downsample) {
    static constexpr std::array<const char*, 7> kNames{
        "series_uuid", "metric_uuid", "bucket_start", "bucket_end", "tags",
        "sample_count", "aggregate_value"};
    const std::array<std::uint32_t, 7> descriptors{
        2, 3, 4, 10, 5, 9,
        source_ast.model_time_series_aggregate_id == "COUNT" ? 11u : 6u};
    const std::array<std::string, 7> names{
        Uuid(951), Uuid(952), Uuid(953), Uuid(953), Uuid(954), Uuid(940),
        source_ast.model_time_series_aggregate_id == "COUNT" ? Uuid(940)
                                                               : Uuid(955)};
    for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
      context.expressions.push_back(
          {next_expression++, descriptors[ordinal], std::nullopt,
           names[ordinal]});
      context.outputs.push_back(
          {ordinal + 1, context.expressions.back().expression_id,
           kNames[ordinal], descriptors[ordinal], true, ordinal,
           ast.relations.front().relation_id});
    }
  } else if (!wildcard) {
    for (std::size_t ordinal = 0;
         ordinal < ast.relations.front().output_expression_ids.size(); ++ordinal) {
      const auto& binding =
          binding_by_ast.at(ast.relations.front().output_expression_ids[ordinal]);
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   ast.relations.front().output_expression_ids[ordinal];
          });
      context.outputs.push_back(
          {static_cast<std::uint32_t>(ordinal + 1), binding.expression_id,
           expression != ast.expressions.end() &&
                   expression->operator_name == "TIME_BUCKET"
               ? "bucket_start"
               : "time_series_value_" + std::to_string(ordinal + 1),
           binding.descriptor_id, true, static_cast<std::uint32_t>(ordinal),
           ast.relations.front().relation_id});
    }
  }
  return context;
}

sbsql::NativeRelationalBindingContext VectorContextFor(
    const sbsql::NativeRelationalAstDocument& ast) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(1010);
  context.catalog_epoch_uuid = Uuid(1011);
  context.security_context_uuid = Uuid(1012);
  context.statement_uuid = Uuid(1013);
  context.statement_timestamp = "2026-08-11T12:00:00.123456789Z";
  context.owning_transaction_uuid = Uuid(1014);
  context.statement_snapshot_uuid = Uuid(1015);
  context.statement_metadata_snapshot_uuid = Uuid(1016);
  context.local_transaction_id = 77;
  context.snapshot_visible_through_local_transaction_id = 76;
  SetEngineAuthority(&context);

  const auto descriptor = [](const std::uint32_t id,
                             const std::uint64_t descriptor_uuid,
                             const std::uint64_t type_uuid,
                             const std::string& canonical_type,
                             const std::string& element_profile = {},
                             const std::optional<std::uint32_t> width =
                                 std::nullopt) {
    sbsql::NativeDescriptorBindingInput input;
    input.descriptor_id = id;
    input.descriptor_uuid = Uuid(descriptor_uuid);
    input.type_uuid = Uuid(type_uuid);
    input.nullability = sbsql::BoundNullability::kNonNull;
    input.width_precision_scale.width = width;
    input.canonical_type_name = canonical_type;
    input.element_profile = element_profile;
    return input;
  };
  context.descriptors = {
      descriptor(1, 1021, 1031, "dense_vector", "real32", 3),
      descriptor(2, 1022, 1032, "text"),
      descriptor(3, 1023, 1033, "uuid"),
      descriptor(4, 1024, 1034, "real64"),
      descriptor(5, 1025, 1035, "uint64"),
      descriptor(6, 1026, 1036, "boolean"),
      descriptor(7, 1027, 1034, "real64"),
  };

  const auto& source_ast = ast.catalog_relation_sources.front();
  sbsql::NativeCatalogRelationBindingInput source;
  source.source_id = source_ast.source_id;
  source.resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kBound;
  source.object_uuid = Uuid(1040);
  source.resolved_object_type = "vector";
  source.resolved_schema_uuid = Uuid(1041);
  source.catalog_generation_id = 77;
  source.security_epoch = 78;
  source.resource_epoch = 79;
  source.columns = {{0, Uuid(1050), 1, "embedding"},
                    {1, Uuid(1051), 2, "metadata"}};
  context.catalog_relations.push_back(std::move(source));
  context.relations.push_back(
      {ast.relations.front().relation_id,
       source_ast.model_operation_id == "VECTOR_FILTERED_SEARCH"
           ? "sblr.model-source.vector-filtered-search.v1"
           : "sblr.model-source.vector-exact-search.v1"});

  const auto maximum_ast_expression = std::ranges::max_element(
      ast.expressions, {}, &sbsql::NativeExpressionAstNode::expression_id);
  std::uint32_t next_expression =
      maximum_ast_expression->expression_id + 1;
  static constexpr std::array<const char*, 3> kNames{
      "row_uuid", "distance", "score"};
  const std::array<std::uint32_t, 3> public_descriptors{3, 4, 7};
  for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    context.expressions.push_back(
        {next_expression, public_descriptors[ordinal], std::nullopt,
         Uuid(1060 + ordinal)});
    context.outputs.push_back(
        {ordinal + 1, next_expression, kNames[ordinal],
         public_descriptors[ordinal], true, ordinal,
         ast.relations.front().relation_id});
    ++next_expression;
  }
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput input;
    input.expression_id = expression.expression_id;
    if (expression.expression_id ==
            source_ast.model_vector_alias_expression_id.value_or(0) ||
        (expression.expression_kind ==
             sbsql::NativeExpressionAstKind::kIdentifier &&
         expression.qualified_identifier.size() == 1 &&
         source_ast.alias.has_value() &&
         expression.qualified_identifier.front().spelling ==
             source_ast.alias->spelling)) {
      input.descriptor_id = 1;
      input.bound_name_uuid = Uuid(1040);
    } else if (expression.expression_id ==
               source_ast.model_vector_query_expression_id.value_or(0)) {
      input.descriptor_id = 1;
    } else if (expression.expression_id ==
               source_ast.model_vector_metric_expression_id.value_or(0)) {
      input.descriptor_id = 2;
    } else if (expression.expression_id ==
               source_ast.model_vector_top_k_expression_id.value_or(0)) {
      input.descriptor_id = 5;
    } else if (expression.expression_id ==
               source_ast.model_vector_metadata_column_expression_id.value_or(
                   0)) {
      input.descriptor_id = 2;
      input.bound_name_uuid = Uuid(1051);
    } else if (expression.expression_id ==
               source_ast.model_vector_metadata_value_expression_id.value_or(
                   0)) {
      input.descriptor_id = 2;
    } else {
      input.descriptor_id = 6;
    }
    context.expressions.push_back(std::move(input));
  }
  return context;
}

sbsql::NativeRelationalBindingContext SearchContextFor(
    const sbsql::NativeRelationalAstDocument& ast) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(1110);
  context.catalog_epoch_uuid = Uuid(1111);
  context.security_context_uuid = Uuid(1112);
  context.statement_uuid = Uuid(1113);
  context.statement_timestamp = "2026-08-11T13:00:00.123456789Z";
  context.owning_transaction_uuid = Uuid(1114);
  context.statement_snapshot_uuid = Uuid(1115);
  context.statement_metadata_snapshot_uuid = Uuid(1116);
  context.local_transaction_id = 87;
  context.snapshot_visible_through_local_transaction_id = 86;
  context.search_analyzer_uuid = Uuid(1142);
  context.search_analyzer_generation = 17;
  SetEngineAuthority(&context);
  const auto descriptor = [](const std::uint32_t id,
                             const std::uint64_t descriptor_uuid,
                             const std::uint64_t type_uuid,
                             const std::string& canonical_type) {
    sbsql::NativeDescriptorBindingInput input;
    input.descriptor_id = id;
    input.descriptor_uuid = Uuid(descriptor_uuid);
    input.type_uuid = Uuid(type_uuid);
    input.nullability = sbsql::BoundNullability::kNonNull;
    input.canonical_type_name = canonical_type;
    return input;
  };
  context.descriptors = {
      descriptor(1, 1121, 1131, "text"),
      descriptor(2, 1122, 1131, "text"),
      descriptor(3, 1123, 1133, "uuid"),
      descriptor(4, 1124, 1134, "uint64"),
      descriptor(5, 1125, 1135, "real64"),
      descriptor(6, 1126, 1136, "boolean"),
      descriptor(7, 1127, 1133, "uuid"),
      descriptor(8, 1128, 1134, "uint64"),
  };
  const auto& source_ast = ast.catalog_relation_sources.front();
  sbsql::NativeCatalogRelationBindingInput source;
  source.source_id = source_ast.source_id;
  source.resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kBound;
  source.object_uuid = Uuid(1140);
  source.resolved_object_type = "search";
  source.resolved_schema_uuid = Uuid(1141);
  source.catalog_generation_id = 87;
  source.security_epoch = 88;
  source.resource_epoch = 89;
  source.columns = {{0, Uuid(1150), 1, "body"},
                    {1, Uuid(1151), 2, "category"}};
  context.catalog_relations.push_back(std::move(source));
  const bool filtered =
      source_ast.model_search_filter_expression_id.has_value();
  context.relations.push_back(
      {ast.relations.front().relation_id,
       std::string("sblr.model-source.search-") +
           (source_ast.model_operation_id == "SEARCH_RANKED_QUERY"
                ? "ranked-query"
                : (source_ast.model_operation_id == "SEARCH_PHRASE_QUERY"
                       ? "phrase-query"
                       : "fuzzy-query")) +
           (filtered ? "-structured-filter.v1" : ".v1")});
  const auto maximum_ast_expression = std::ranges::max_element(
      ast.expressions, {}, &sbsql::NativeExpressionAstNode::expression_id);
  std::uint32_t next_expression = maximum_ast_expression->expression_id + 1;
  static constexpr std::array<const char*, 5> kNames{
      "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
      "rank"};
  const std::array<std::uint32_t, 5> public_descriptors{3, 7, 4, 5, 8};
  for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    context.expressions.push_back(
        {next_expression, public_descriptors[ordinal], std::nullopt,
         ordinal == 0 ? std::optional<std::string>{Uuid(1140)}
                      : (ordinal == 1 || ordinal == 2
                             ? std::optional<std::string>{Uuid(1142)}
                             : std::optional<std::string>{Uuid(1140)})});
    context.outputs.push_back(
        {ordinal + 1, next_expression, kNames[ordinal],
         public_descriptors[ordinal], true, ordinal,
         ast.relations.front().relation_id});
    ++next_expression;
  }
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput input;
    input.expression_id = expression.expression_id;
    if (expression.expression_id ==
        source_ast.model_search_alias_expression_id.value_or(0)) {
      input.descriptor_id = 1;
      input.bound_name_uuid = Uuid(1140);
    } else if (source_ast.alias.has_value() &&
               expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kIdentifier &&
               expression.qualified_identifier.size() == 1 &&
               expression.qualified_identifier.front().quoted ==
                   source_ast.alias->quoted &&
               expression.qualified_identifier.front().spelling ==
                   source_ast.alias->spelling) {
      input.descriptor_id = 1;
      input.bound_name_uuid = Uuid(1140);
    } else if (expression.expression_id ==
               source_ast.model_search_text_expression_id.value_or(0)) {
      input.descriptor_id = 1;
    } else if (expression.expression_id ==
               source_ast.model_search_analyzer_expression_id.value_or(0)) {
      input.descriptor_id = 3;
      input.bound_name_uuid = Uuid(1142);
    } else if (expression.expression_id ==
                   source_ast.model_search_top_k_expression_id.value_or(0) ||
               expression.expression_id ==
                   source_ast.model_search_edit_expression_id.value_or(0)) {
      input.descriptor_id = 4;
    } else if (expression.expression_id ==
               source_ast.model_search_category_column_expression_id.value_or(0)) {
      input.descriptor_id = 2;
      input.bound_name_uuid = Uuid(1151);
    } else if (expression.expression_id ==
               source_ast.model_search_category_value_expression_id.value_or(0)) {
      input.descriptor_id = 2;
    } else {
      input.descriptor_id = 6;
    }
    context.expressions.push_back(std::move(input));
  }
  return context;
}

sbsql::NativeRelationalBindingContext KeyValueContextFor(
    const sbsql::NativeRelationalAstDocument& ast) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(810);
  context.catalog_epoch_uuid = Uuid(811);
  context.security_context_uuid = Uuid(812);
  context.statement_uuid = Uuid(813);
  context.statement_timestamp = "2026-08-10T12:00:00.123456789Z";
  context.owning_transaction_uuid = Uuid(814);
  context.statement_snapshot_uuid = Uuid(815);
  context.statement_metadata_snapshot_uuid = Uuid(816);
  context.local_transaction_id = 75;
  context.snapshot_visible_through_local_transaction_id = 74;
  SetEngineAuthority(&context);
  context.descriptors = {
      {1, Uuid(821), Uuid(831), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {2, Uuid(822), Uuid(832), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {3, Uuid(823), Uuid(832), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {4, Uuid(824), Uuid(834), sbsql::BoundNullability::kNullable,
       std::nullopt, std::nullopt, {}},
  };
  const auto& source_ast = ast.catalog_relation_sources.front();
  sbsql::NativeCatalogRelationBindingInput source;
  source.source_id = source_ast.source_id;
  source.resolution_state =
      sbsql::NativeCatalogRelationResolutionState::kBound;
  source.object_uuid = Uuid(840);
  source.resolved_object_type = "key_value";
  source.resolved_schema_uuid = Uuid(841);
  source.parent_object_uuid = Uuid(842);
  source.catalog_generation_id = 75;
  source.security_epoch = 76;
  source.resource_epoch = 77;
  source.columns = {{0, Uuid(850), 1, "row_uuid"},
                    {1, Uuid(851), 2, "key"},
                    {2, Uuid(852), 3, "value"}};
  context.catalog_relations.push_back(std::move(source));
  context.relations.push_back(
      {ast.relations.front().relation_id,
       source_ast.model_operation_id == "KEY_VALUE_GET"
           ? "sblr.model-source.key-value-get.v1"
           : (source_ast.model_operation_id == "KEY_VALUE_MULTI_GET"
                  ? "sblr.model-source.key-value-multi-get.v1"
                  : "sblr.model-source.key-value-prefix-range.v1")});

  const bool wildcard = std::ranges::any_of(ast.expressions, [](const auto& e) {
    return e.expression_kind == sbsql::NativeExpressionAstKind::kWildcard;
  });
  std::uint32_t next_expression = 101;
  if (wildcard) {
    static constexpr std::array<const char*, 3> kNames{
        "row_uuid", "key", "value"};
    for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
      context.expressions.push_back(
          {next_expression++, ordinal + 1, std::nullopt,
           Uuid(850 + ordinal)});
      context.outputs.push_back(
          {ordinal + 1, context.expressions.back().expression_id,
           kNames[ordinal], ordinal + 1, true, ordinal,
           ast.relations.front().relation_id});
    }
  }

  const std::unordered_map<std::string, std::pair<std::uint32_t, std::string>>
      projected_columns{{"row_uuid", {1, Uuid(850)}},
                        {"key", {2, Uuid(851)}},
                        {"value", {3, Uuid(852)}}};
  const std::unordered_map<std::uint32_t, bool> key_nodes = [&] {
    std::unordered_map<std::uint32_t, bool> result;
    for (const auto id : source_ast.model_key_expression_ids) {
      result.emplace(id, true);
    }
    return result;
  }();
  std::unordered_map<std::uint32_t, sbsql::NativeExpressionBindingInput>
      binding_by_ast;
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput input;
    input.expression_id = next_expression++;
    if (key_nodes.contains(expression.expression_id) ||
        expression.literal_kind == sbsql::NativeLiteralAstKind::kString) {
      input.descriptor_id = 2;
    } else if (expression.expression_kind ==
               sbsql::NativeExpressionAstKind::kIdentifier) {
      input.descriptor_id = 2;
      if (!expression.qualified_identifier.empty()) {
        const auto& terminal = expression.qualified_identifier.back().spelling;
        const auto projected = projected_columns.find(terminal);
        if (projected != projected_columns.end()) {
          input.descriptor_id = projected->second.first;
          input.bound_name_uuid = projected->second.second;
        } else {
          input.bound_name_uuid = Uuid(840);
        }
      }
    } else if (expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kFunctionCall &&
               expression.operator_name == "KV_KEY") {
      input.descriptor_id = 2;
    } else if (expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kFunctionCall ||
               (expression.expression_kind ==
                    sbsql::NativeExpressionAstKind::kBinary &&
                expression.operator_name == "=")) {
      input.descriptor_id = 4;
    } else {
      input.descriptor_id = 2;
    }
    context.expressions.push_back(input);
    binding_by_ast.emplace(expression.expression_id, std::move(input));
  }
  if (!wildcard) {
    for (std::size_t ordinal = 0;
         ordinal < ast.relations.front().output_expression_ids.size();
         ++ordinal) {
      const auto ast_expression_id =
          ast.relations.front().output_expression_ids[ordinal];
      const auto& input = binding_by_ast.at(ast_expression_id);
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id == ast_expression_id;
          });
      const auto name = expression->qualified_identifier.back().spelling;
      context.outputs.push_back(
          {static_cast<std::uint32_t>(ordinal + 1), input.expression_id, name,
           input.descriptor_id, true, static_cast<std::uint32_t>(ordinal),
           ast.relations.front().relation_id});
    }
  }
  return context;
}

sbsql::BoundStatement Bind(const sbsql::CstDocument& cst,
                           const sbsql::AstDocument& ast,
                           const bool unnest) {
  const auto context = ContextFor(ast.native_relational, unnest);
  return sbsql::BindAst(ast, cst, Config(), Session(), {}, &context);
}

bool SourcePathGrammarBindingLowering() {
  const auto cst = sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_SOURCE(app.document_fixture) AS d "
      "WHERE DOCUMENT_PATH(d, '$.join_key') >= 1 + 0;");
  const auto ast = sbsql::BuildAst(cst);
  bool passed = true;
  passed &= Require(ast.native_relational.status ==
                            sbsql::NativeRelationalParseStatus::kAccepted &&
                        ast.native_relational.catalog_relation_sources.size() == 1,
                    "DOCUMENT_SOURCE/DOCUMENT_PATH grammar was not accepted");
  if (!passed) return false;
  const auto& source = ast.native_relational.catalog_relation_sources.front();
  passed &= Require(source.source_kind ==
                            sbsql::NativeRelationSourceAstKind::kDocument &&
                        source.model_family_id == "document" &&
                        source.model_operation_id == "DOCUMENT_PATH" &&
                        source.qualified_name.size() == 2 &&
                        source.alias.has_value() && source.alias_is_explicit &&
                        source.model_path_expression_id.has_value() &&
                        source.model_value_expression_id.has_value() &&
                        source.model_comparison_operator == ">=",
                    "document source AST identity or composed RHS drifted");
  passed &= Require(
      ast.requires_name_resolution && !ast.produces_sblr &&
          ast.native_relational.model_object_resolution_requests.size() == 1 &&
          ast.native_relational.model_object_resolution_requests.front()
                  .source_id == source.source_id &&
          ast.native_relational.model_object_resolution_requests.front()
                  .model_family_id == "document" &&
          ast.native_relational.model_object_resolution_requests.front()
                  .object_class == "document_collection" &&
          ast.native_relational.model_object_resolution_requests.front()
                  .qualified_name.size() == 2,
      "collection source did not publish one exact resolution description");
  auto bound = Bind(cst, ast, false);
  passed &= Require(bound.bound && bound.native_relational.bound &&
                        bound.resolved_object_uuids ==
                            std::vector<std::string>{Uuid(740)} &&
                        bound.native_relational.catalog_relation_sources.front()
                                .object_uuid == Uuid(740),
                    "qualified collection did not bind to its UUID");
  if (!bound.bound) return false;
  passed &= Require(
      bound.native_relational.relations.size() == 1 &&
          bound.native_relational.relations.front().semantic_variant_id ==
              "sblr.model-source.document-path.v1" &&
          std::ranges::any_of(
              bound.native_relational.expressions, [](const auto& expression) {
                return expression.expression_kind ==
                           sbsql::NativeExpressionAstKind::kFunctionCall &&
                       !expression.bound_function_uuid.has_value();
              }),
      "DOCUMENT_PATH operation identity drifted into callable-registry identity");
  const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);
  passed &= Require(!lowered.messages.has_errors() && verified.admitted &&
                        HasOperand(lowered, "relational_node_binding_v1", "1") &&
                        std::ranges::any_of(lowered.operands, [](const auto& op) {
                          return op.type == "relational_node_binding_v1" &&
                                 op.value.find("53424c525f4d4f44454c5f534f555243455f5631") == 0;
                        }),
                    "DOCUMENT_PATH did not lower as SBLR_MODEL_SOURCE_V1");

  auto ambiguous_context = ContextFor(ast.native_relational, false);
  ambiguous_context.catalog_relations.push_back(
      ambiguous_context.catalog_relations.front());
  const auto ambiguous = sbsql::BindAst(ast, cst, Config(), Session(), {},
                                         &ambiguous_context);
  passed &= Require(!ambiguous.bound &&
                        HasDiagnostic(ambiguous.messages,
                                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
                    "ambiguous document source did not fail closed");

  auto semantic_substitution = ContextFor(ast.native_relational, false);
  semantic_substitution.relations.front().semantic_variant_id =
      "catalog.relation-source.v1";
  const auto substituted = sbsql::BindAst(
      ast, cst, Config(), Session(), {}, &semantic_substitution);
  passed &= Require(!substituted.bound &&
                        HasDiagnostic(substituted.messages,
                                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
                    "generic relation semantic substituted for document source");

  auto authority_mismatch = ContextFor(ast.native_relational, false);
  authority_mismatch.engine_statement_authority.statement_snapshot_uuid =
      Uuid(799);
  const auto unauthorized = sbsql::BindAst(
      ast, cst, Config(), Session(), {}, &authority_mismatch);
  passed &= Require(!unauthorized.bound &&
                        HasDiagnostic(unauthorized.messages,
                                      "QOW-DIAG-BOUNDAST-SCOPE"),
                    "mismatched MGA statement authority was accepted");

  auto lossy = bound;
  lossy.native_relational.expressions.back().child_expression_ids.push_back(9999);
  const auto lossy_lowered = sbsql::LowerToSblr(lossy, cst, Session());
  passed &= Require(HasDiagnostic(lossy_lowered.messages,
                                  "SB_MODEL_BINDING_INCOMPLETE_V1"),
                    "lossy expression lineage was lowered");
  return passed;
}

bool UnnestGrammarBindingLowering() {
  const auto cst = sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_UNNEST(DOCUMENT '{\"items\":[3,1,2]}', '$.items[*]') "
      "AS item;");
  const auto ast = sbsql::BuildAst(cst);
  bool passed = Require(
      ast.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kAccepted &&
          ast.native_relational.catalog_relation_sources.size() == 1 &&
          ast.native_relational.catalog_relation_sources.front()
                  .model_operation_id == "DOCUMENT_UNNEST" &&
          ast.native_relational.catalog_relation_sources.front()
                  .model_document_expression_id.has_value() &&
          ast.native_relational.catalog_relation_sources.front()
                  .model_wildcard_path,
      "expression-backed DOCUMENT_UNNEST grammar drifted");
  passed &= Require(!ast.requires_name_resolution && ast.produces_sblr &&
                        ast.native_relational
                            .model_object_resolution_requests.empty(),
                    "DOCUMENT_UNNEST requested or invented catalog resolution");
  if (!passed) return false;
  const auto bound = Bind(cst, ast, true);
  passed &= Require(bound.bound && bound.resolved_object_uuids.empty() &&
                        bound.native_relational.catalog_relation_sources.front()
                                .object_uuid.empty() &&
                        bound.native_relational.catalog_relation_sources.front()
                                .resolved_object_type == "document_expression",
                    "DOCUMENT_UNNEST invented catalog identity");
  if (!bound.bound) return false;
  const auto& bound_source =
      bound.native_relational.catalog_relation_sources.front();
  const auto& bound_relation = bound.native_relational.relations.front();
  const auto bound_expression = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        bound.native_relational.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  const auto root = bound_expression(bound_relation.output_expression_ids.front());
  const auto document =
      bound_expression(*bound_source.model_document_expression_id);
  const auto path = bound_expression(*bound_source.model_path_expression_id);
  const auto descriptor = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(
        bound.native_relational.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == descriptor_id;
        });
  };
  passed &= Require(
      bound_relation.output_expression_ids.size() == 1 &&
          bound_relation.bound_expression_ids ==
              bound_relation.output_expression_ids &&
          root != bound.native_relational.expressions.end() &&
          document != bound.native_relational.expressions.end() &&
          path != bound.native_relational.expressions.end() &&
          root->expression_kind ==
              sbsql::NativeExpressionAstKind::kFunctionCall &&
          root->canonical_operator_name == "DOCUMENT_UNNEST" &&
          !root->bound_function_uuid.has_value() &&
          root->child_expression_ids ==
              std::vector<std::uint32_t>{
                  *bound_source.model_document_expression_id,
                  *bound_source.model_path_expression_id} &&
          path->expression_kind == sbsql::NativeExpressionAstKind::kLiteral &&
          path->literal_kind == sbsql::NativeLiteralAstKind::kString &&
          descriptor(root->result_descriptor_id)->type_uuid ==
              descriptor(document->result_descriptor_id)->type_uuid,
      "DOCUMENT_UNNEST did not preserve its typed root and ordered operands");
  const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);
  passed &= Require(!lowered.messages.has_errors() && verified.admitted &&
                        std::ranges::any_of(lowered.operands, [](const auto& op) {
                          return op.type == "relational_node_binding_v1" &&
                                 op.value.find("53424c525f4d4f44454c5f455850414e445f5631") == 0;
                        }),
                    "DOCUMENT_UNNEST did not lower as SBLR_MODEL_EXPAND_V1");

  auto descriptor_substitution = ContextFor(ast.native_relational, true);
  std::size_t binding_index = 1;
  for (const auto& expression : ast.native_relational.expressions) {
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    if (expression.expression_id ==
        *ast.native_relational.catalog_relation_sources.front()
             .model_document_expression_id) {
      descriptor_substitution.expressions[binding_index].descriptor_id = 4;
      break;
    }
    ++binding_index;
  }
  const auto substituted = sbsql::BindAst(
      ast, cst, Config(), Session(), {}, &descriptor_substitution);
  passed &= Require(
      !substituted.bound &&
          HasDiagnostic(substituted.messages, "SB_MODEL_BINDING_INCOMPLETE_V1"),
      "numeric/text descriptor kind substituted for DOCUMENT_UNNEST JSON root");

  const auto expect_lowering_refusal = [&](auto mutation,
                                            const std::string_view detail) {
    auto invalid = bound;
    mutation(invalid.native_relational);
    const auto invalid_lowered = sbsql::LowerToSblr(invalid, cst, Session());
    return Require(
        HasDiagnostic(invalid_lowered.messages,
                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
        detail);
  };
  const auto root_id = bound_relation.output_expression_ids.front();
  const auto document_id = *bound_source.model_document_expression_id;
  const auto path_id = *bound_source.model_path_expression_id;
  passed &= expect_lowering_refusal(
      [=](auto& native) {
        const auto invalid_root = std::ranges::find_if(
            native.expressions, [&](const auto& expression) {
              return expression.expression_id == root_id;
            });
        std::swap(invalid_root->child_expression_ids[0],
                  invalid_root->child_expression_ids[1]);
      },
      "DOCUMENT_UNNEST reversed operand order was lowered");
  passed &= expect_lowering_refusal(
      [=](auto& native) {
        const auto invalid_root = std::ranges::find_if(
            native.expressions, [&](const auto& expression) {
              return expression.expression_id == root_id;
            });
        invalid_root->child_expression_ids = {document_id, document_id};
      },
      "DOCUMENT_UNNEST duplicate operand identity was lowered");
  passed &= expect_lowering_refusal(
      [=](auto& native) {
        const auto invalid_path = std::ranges::find_if(
            native.expressions, [&](const auto& expression) {
              return expression.expression_id == path_id;
            });
        invalid_path->literal_kind = sbsql::NativeLiteralAstKind::kNumeric;
      },
      "DOCUMENT_UNNEST non-string path kind was lowered");
  passed &= expect_lowering_refusal(
      [](auto& native) {
        auto orphan = native.expressions.front();
        orphan.expression_id = 9999;
        orphan.child_expression_ids.clear();
        native.expressions.push_back(std::move(orphan));
      },
      "DOCUMENT_UNNEST unreachable expression operand was lowered");
  return passed;
}

bool ExactRefusals() {
  const auto donor = sbsql::BuildAst(
      sbsql::BuildCst("SELECT * FROM MONGO_PIPELINE('{ $match: {} }');"));
  const auto write = sbsql::BuildAst(sbsql::BuildCst(
      "UPDATE DOCUMENT_SOURCE(app.document_fixture) SET payload = 'x';"));
  const auto alias = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_SOURCE(app.document_fixture) d "
      "WHERE DOCUMENT_PATH(other, '$.x') = 1;"));
  const auto untyped_path = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_UNNEST(document_parameter, dynamic_path);"));
  bool passed = true;
  passed &= Require(HasDiagnostic(
                        donor.native_relational.messages,
                        "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1"),
                    "opaque donor pipeline was not refused exactly");
  passed &= Require(HasDiagnostic(write.native_relational.messages,
                                  "SB_MODEL_QUERY_WRITE_REFUSED_V1"),
                    "document query write was not refused exactly");
  passed &= Require(alias.native_relational.status ==
                        sbsql::NativeRelationalParseStatus::kRefused,
                    "mismatched document alias was accepted");
  passed &= Require(untyped_path.native_relational.status ==
                        sbsql::NativeRelationalParseStatus::kRefused,
                    "untyped DOCUMENT_UNNEST path was accepted");
  return passed;
}

bool KeyValueGrammarBindingLowering() {
  struct Case {
    std::string sql;
    std::string operation;
    std::string semantic;
    std::size_t key_count;
  };
  const std::array<Case, 3> cases{{
      {"SELECT * FROM KEY_VALUE_SOURCE(app.kv_fixture) AS kv "
       "WHERE KV_KEY(kv) = 'alpha';",
       "KEY_VALUE_GET", "sblr.model-source.key-value-get.v1", 1},
      {"SELECT row_uuid, key, value FROM "
       "KEY_VALUE_SOURCE(app.kv_fixture) AS kv "
       "WHERE KV_MULTI_GET(kv, 'gamma', 'alpha', 'gamma');",
       "KEY_VALUE_MULTI_GET",
       "sblr.model-source.key-value-multi-get.v1", 3},
      {"SELECT * FROM KEY_VALUE_SOURCE(app.kv_fixture) kv "
       "WHERE KV_PREFIX(kv, 'al');",
       "KEY_VALUE_PREFIX_RANGE",
       "sblr.model-source.key-value-prefix-range.v1", 1},
  }};
  bool passed = true;
  std::optional<sbsql::BoundStatement> retained_multi;
  std::optional<sbsql::CstDocument> retained_multi_cst;
  for (const auto& test : cases) {
    const auto cst = sbsql::BuildCst(test.sql);
    const auto ast = sbsql::BuildAst(cst);
    passed &= Require(
        ast.native_relational.accepted() && ast.requires_name_resolution &&
            !ast.produces_sblr &&
            ast.native_relational.catalog_relation_sources.size() == 1 &&
            ast.native_relational.catalog_relation_sources.front().source_kind ==
                sbsql::NativeRelationSourceAstKind::kKeyValue &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_family_id == "key_value" &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_operation_id == test.operation &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_key_expression_ids.size() == test.key_count &&
            ast.native_relational.model_object_resolution_requests.size() == 1 &&
            ast.native_relational.model_object_resolution_requests.front()
                    .object_class == "key_value",
        "ordinary KEY_VALUE_SOURCE grammar/object extraction drifted for " +
            test.operation);
    if (!ast.native_relational.accepted()) continue;
    auto context = KeyValueContextFor(ast.native_relational);
    const auto bound =
        sbsql::BindAst(ast, cst, Config(), Session(), {}, &context);
    passed &= Require(
        bound.bound && bound.native_relational.bound &&
            bound.native_relational.statement_timestamp ==
                context.statement_timestamp &&
            bound.native_relational.catalog_relation_sources.front()
                    .object_uuid == Uuid(840) &&
            bound.native_relational.catalog_relation_sources.front()
                    .model_key_expression_ids.size() == test.key_count &&
            bound.native_relational.relations.front().semantic_variant_id ==
                test.semantic,
        "KEY_VALUE_SOURCE engine-projected binding drifted for " +
            test.operation + ":" + DiagnosticSummary(bound.messages));
    if (!bound.bound) continue;
    const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
    const auto verified = sbsql::VerifySblrEnvelope(lowered);
    if (!verified.admitted) {
      for (const auto& operand : lowered.operands) {
        if (operand.type == "relational_expression_v1") {
          std::cerr << test.operation << " expression " << operand.name
                    << '=' << operand.value << '\n';
        }
      }
    }
    passed &= Require(
        !lowered.messages.has_errors() && verified.admitted &&
            HasOperand(lowered, "text", "relational_statement_timestamp",
                       context.statement_timestamp) &&
            std::ranges::any_of(lowered.operands, [](const auto& operand) {
              return operand.type == "relational_node_binding_v1" &&
                     operand.value.find(
                         "53424c525f4d4f44454c5f534f555243455f5631") == 0;
            }),
        "KEY_VALUE_SOURCE did not lower through SBLR_MODEL_SOURCE_V1 for " +
            test.operation + ":" + DiagnosticSummary(lowered.messages) + "/" +
            DiagnosticSummary(verified.messages));
    if (test.operation == "KEY_VALUE_MULTI_GET") {
      retained_multi = bound;
      retained_multi_cst = cst;
    }
  }

  const auto exact_cst = sbsql::BuildCst(cases.front().sql);
  const auto exact_ast = sbsql::BuildAst(exact_cst);
  auto timestamp_context = KeyValueContextFor(exact_ast.native_relational);
  timestamp_context.engine_statement_authority.statement_timestamp =
      "2026-08-10T12:00:00.123456788Z";
  const auto timestamp_refused = sbsql::BindAst(
      exact_ast, exact_cst, Config(), Session(), {}, &timestamp_context);
  passed &= Require(
      !timestamp_refused.bound &&
          HasDiagnostic(timestamp_refused.messages,
                        "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1"),
      "key/value binding admitted a substituted statement timestamp");
  auto type_context = KeyValueContextFor(exact_ast.native_relational);
  type_context.descriptors[1].nullability =
      sbsql::BoundNullability::kNullable;
  const auto type_refused = sbsql::BindAst(
      exact_ast, exact_cst, Config(), Session(), {}, &type_context);
  passed &= Require(
      !type_refused.bound &&
          HasDiagnostic(type_refused.messages,
                        "SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1"),
      "key/value binding admitted a nullable key expression");

  if (retained_multi.has_value() && retained_multi_cst.has_value()) {
    auto reordered = *retained_multi;
    const auto operation = std::ranges::find_if(
        reordered.native_relational.expressions, [](const auto& expression) {
          return expression.canonical_operator_name == "KV_MULTI_GET";
        });
    if (operation != reordered.native_relational.expressions.end() &&
        operation->child_expression_ids.size() >= 3) {
      std::swap(operation->child_expression_ids[1],
                operation->child_expression_ids[2]);
    }
    const auto refused =
        sbsql::LowerToSblr(reordered, *retained_multi_cst, Session());
    passed &= Require(
        HasDiagnostic(refused.messages,
                      "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        "key/value lowering admitted reordered multi-get child identity");
  }

  const auto wrong_operator = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM KEY_VALUE_SOURCE(app.kv_fixture) kv "
      "WHERE KV_KEY(kv) <> 'alpha';"));
  const auto empty_multi = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM KEY_VALUE_SOURCE(app.kv_fixture) kv "
      "WHERE KV_MULTI_GET(kv);"));
  const auto extra_prefix = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM KEY_VALUE_SOURCE(app.kv_fixture) kv "
      "WHERE KV_PREFIX(kv, 'a', 'b');"));
  passed &= Require(
      HasDiagnostic(wrong_operator.native_relational.messages,
                    "SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1") &&
          HasDiagnostic(empty_multi.native_relational.messages,
                        "SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1") &&
          HasDiagnostic(extra_prefix.native_relational.messages,
                        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
      "key/value parser refusal identities drifted:" +
          DiagnosticSummary(wrong_operator.native_relational.messages) + "/" +
          DiagnosticSummary(empty_multi.native_relational.messages) + "/" +
          DiagnosticSummary(extra_prefix.native_relational.messages));
  return passed;
}

bool TimeSeriesGrammarBindingLowering() {
  struct Case {
    std::string sql;
    std::string operation;
    std::string semantic;
    std::size_t width;
  };
  const std::array<Case, 5> cases{{
      {"SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) AS ts "
       "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
       "TIMESTAMP '2026-08-10T12:02:00Z');",
       "TIME_SERIES_RANGE_READ",
       "sblr.model-source.time-series-range-read.v1", 6},
      {"SELECT TIME_BUCKET(INTERVAL 'PT60S', ts.point_timestamp) FROM "
       "TIME_SERIES_SOURCE(app.series_fixture) AS ts "
       "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
       "TIMESTAMP '2026-08-10T12:02:00Z');",
       "TIME_SERIES_RANGE_READ",
       "sblr.model-source.time-series-range-read.v1", 1},
      {"SELECT TIME_BUCKET(INTERVAL 'P1D', ts.point_timestamp) FROM "
       "TIME_SERIES_SOURCE(app.series_fixture) AS ts "
       "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
       "TIMESTAMP '2026-08-10T12:02:00Z');",
       "TIME_SERIES_RANGE_READ",
       "sblr.model-source.time-series-range-read.v1", 1},
      {"SELECT TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.value) FROM "
       "TIME_SERIES_SOURCE(app.series_fixture) AS ts "
       "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
       "TIMESTAMP '2026-08-10T12:02:00Z');",
       "TIME_SERIES_DOWNSAMPLE",
       "sblr.model-aggregate.time-series-downsample.v1", 7},
      {"SELECT TIME_BUCKET(INTERVAL 'PT1M', ts.point_timestamp), "
       "TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.value) FROM "
       "TIME_SERIES_SOURCE(app.series_fixture) AS ts "
       "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
       "TIMESTAMP '2026-08-10T12:02:00Z');",
       "TIME_SERIES_DOWNSAMPLE",
       "sblr.model-aggregate.time-series-downsample.v1", 7},
  }};
  bool passed = true;
  std::optional<sbsql::BoundStatement> raw_bound;
  std::optional<sbsql::CstDocument> raw_cst;
  std::optional<sbsql::BoundStatement> downsample_bound;
  std::optional<sbsql::CstDocument> downsample_cst;
  for (const auto& test : cases) {
    const auto cst = sbsql::BuildCst(test.sql);
    const auto ast = sbsql::BuildAst(cst);
    passed &= Require(
        ast.native_relational.accepted() && ast.requires_name_resolution &&
            !ast.produces_sblr &&
            ast.native_relational.catalog_relation_sources.size() == 1 &&
            ast.native_relational.catalog_relation_sources.front().source_kind ==
                sbsql::NativeRelationSourceAstKind::kTimeSeries &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_family_id == "time_series" &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_operation_id == test.operation &&
            ast.native_relational.model_object_resolution_requests.size() == 1 &&
            ast.native_relational.model_object_resolution_requests.front()
                    .object_class == "time_series",
        "ordinary TIME_SERIES_SOURCE grammar/object extraction drifted: " +
            test.operation + ":" +
            DiagnosticSummary(ast.native_relational.messages));
    if (!ast.native_relational.accepted()) continue;
    auto context = TimeSeriesContextFor(ast.native_relational);
    const auto bound =
        sbsql::BindAst(ast, cst, Config(), Session(), {}, &context);
    passed &= Require(
        bound.bound && bound.native_relational.bound &&
            bound.native_relational.statement_timestamp ==
                context.statement_timestamp &&
            bound.native_relational.catalog_relation_sources.front()
                    .object_uuid == Uuid(940) &&
            bound.native_relational.relations.front().semantic_variant_id ==
                test.semantic &&
            bound.native_relational.outputs.size() == test.width,
        "TIME_SERIES_SOURCE projected binding drifted: " + test.operation +
            ":" + DiagnosticSummary(bound.messages));
    if (!bound.bound) continue;
    const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
    const auto verified = sbsql::VerifySblrEnvelope(lowered);
    passed &= Require(
        !lowered.messages.has_errors() && verified.admitted &&
            HasOperand(lowered, "text", "relational_statement_timestamp",
                       context.statement_timestamp) &&
            std::ranges::any_of(lowered.operands, [&](const auto& operand) {
              const auto semantic =
                  test.operation == "TIME_SERIES_DOWNSAMPLE"
                      ? "53424c525f4d4f44454c5f4147475245474154455f5631"
                      : "53424c525f4d4f44454c5f534f555243455f5631";
              return operand.type == "relational_node_binding_v1" &&
                     operand.value.find(semantic) == 0;
            }),
        "TIME_SERIES_SOURCE did not lower through the exact model semantic: " +
            test.operation + ":" + DiagnosticSummary(lowered.messages) + "/" +
            DiagnosticSummary(verified.messages));
    if (test.operation == "TIME_SERIES_DOWNSAMPLE") {
      downsample_bound = bound;
      downsample_cst = cst;
    } else if (test.width == 6) {
      raw_bound = bound;
      raw_cst = cst;
    }
  }

  const auto combined_carrier_cst = sbsql::BuildCst(
      "SELECT TIME_BUCKET(INTERVAL 'PT1M', ts.point_timestamp), "
      "TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');");
  auto missing_bucket_carrier_ast = sbsql::BuildAst(combined_carrier_cst);
  if (missing_bucket_carrier_ast.native_relational.accepted()) {
    missing_bucket_carrier_ast.native_relational.catalog_relation_sources
        .front().model_bucket_interval_expression_id.reset();
    auto missing_context =
        TimeSeriesContextFor(missing_bucket_carrier_ast.native_relational);
    const auto missing_bound = sbsql::BindAst(
        missing_bucket_carrier_ast, combined_carrier_cst, Config(), Session(),
        {}, &missing_context);
    passed &= Require(
        !missing_bound.bound &&
            HasDiagnostic(missing_bound.messages,
                          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        "binder admitted a missing TIME_BUCKET interval carrier");
  }
  const auto raw_carrier_cst = sbsql::BuildCst(
      "SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');");
  auto stray_bucket_carrier_ast = sbsql::BuildAst(raw_carrier_cst);
  if (stray_bucket_carrier_ast.native_relational.accepted()) {
    auto& source = stray_bucket_carrier_ast.native_relational
                       .catalog_relation_sources.front();
    source.model_bucket_interval_expression_id =
        source.model_range_start_expression_id;
    source.model_bucket_time_input_expression_id =
        source.model_range_end_expression_id;
    auto stray_context =
        TimeSeriesContextFor(stray_bucket_carrier_ast.native_relational);
    const auto stray_bound = sbsql::BindAst(
        stray_bucket_carrier_ast, raw_carrier_cst, Config(), Session(), {},
        &stray_context);
    passed &= Require(
        !stray_bound.bound &&
            HasDiagnostic(stray_bound.messages,
                          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        "binder admitted stray TIME_BUCKET child carriers");
  }

  const auto mismatched_interval_cst = sbsql::BuildCst(
      "SELECT TIME_BUCKET(INTERVAL 'PT30S', ts.point_timestamp), "
      "TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts "
      "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');");
  const auto mismatched_interval_ast =
      sbsql::BuildAst(mismatched_interval_cst);
  if (mismatched_interval_ast.native_relational.accepted()) {
    auto mismatch_context =
        TimeSeriesContextFor(mismatched_interval_ast.native_relational);
    const auto mismatch_bound = sbsql::BindAst(
        mismatched_interval_ast, mismatched_interval_cst, Config(), Session(),
        {}, &mismatch_context);
    const auto mismatch_lowered =
        sbsql::LowerToSblr(mismatch_bound, mismatched_interval_cst, Session());
    const auto mismatch_verified =
        sbsql::VerifySblrEnvelope(mismatch_lowered);
    passed &= Require(
        (!mismatch_bound.bound &&
         HasDiagnostic(mismatch_bound.messages,
                       "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1")) ||
            (mismatch_bound.bound &&
             HasDiagnostic(mismatch_verified.messages,
                           "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1")),
        "unequal evaluated TIME_BUCKET/TIME_DOWNSAMPLE intervals did not "
        "receive the exact refusal: " +
            DiagnosticSummary(mismatch_bound.messages) + "/" +
            DiagnosticSummary(mismatch_verified.messages));
  } else {
    passed &= Require(false,
                      "combined TIME_BUCKET/TIME_DOWNSAMPLE grammar was "
                      "refused before evaluated-interval validation");
  }
  for (const std::string_view invalid_interval :
       {"0", "-1", "P1M", "PT0.0000000001S", "P106752D"}) {
    const auto invalid_cst = sbsql::BuildCst(
        "SELECT TIME_BUCKET(INTERVAL '" + std::string(invalid_interval) +
        "', ts.point_timestamp) FROM "
        "TIME_SERIES_SOURCE(app.series_fixture) AS ts "
        "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
        "TIMESTAMP '2026-08-10T12:02:00Z');");
    const auto invalid_ast = sbsql::BuildAst(invalid_cst);
    if (!invalid_ast.native_relational.accepted()) {
      passed &= Require(
          HasDiagnostic(invalid_ast.native_relational.messages,
                        "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1"),
          "invalid fixed interval received the wrong parser refusal: " +
              std::string(invalid_interval));
      continue;
    }
    auto invalid_context = TimeSeriesContextFor(invalid_ast.native_relational);
    const auto invalid_bound = sbsql::BindAst(
        invalid_ast, invalid_cst, Config(), Session(), {}, &invalid_context);
    const auto invalid_lowered =
        sbsql::LowerToSblr(invalid_bound, invalid_cst, Session());
    const auto invalid_verified = sbsql::VerifySblrEnvelope(invalid_lowered);
    passed &= Require(
        invalid_bound.bound &&
            HasDiagnostic(invalid_verified.messages,
                          "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1"),
        "invalid fixed interval reached SBLR admission or received the wrong "
        "diagnostic: " +
            std::string(invalid_interval) + ":" +
            DiagnosticSummary(invalid_verified.messages));
  }

  const auto wrong_value_cst = sbsql::BuildCst(
      "SELECT TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.tags) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts "
      "WHERE TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');");
  const auto wrong_value_ast = sbsql::BuildAst(wrong_value_cst);
  if (wrong_value_ast.native_relational.accepted()) {
    auto wrong_value_context =
        TimeSeriesContextFor(wrong_value_ast.native_relational);
    const auto wrong_value_bound = sbsql::BindAst(
        wrong_value_ast, wrong_value_cst, Config(), Session(), {},
        &wrong_value_context);
    passed &= Require(
        !wrong_value_bound.bound &&
            HasDiagnostic(wrong_value_bound.messages,
                          "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1"),
        "TIME_DOWNSAMPLE admitted a non-value aggregate input: " +
            DiagnosticSummary(wrong_value_bound.messages));
  } else {
    passed &= Require(
        HasDiagnostic(wrong_value_ast.native_relational.messages,
                      "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1"),
        "TIME_DOWNSAMPLE wrong-value input received the wrong parser refusal");
  }

  if (downsample_bound.has_value() && downsample_cst.has_value()) {
    auto missing_bucket_carrier = *downsample_bound;
    missing_bucket_carrier.native_relational.catalog_relation_sources.front()
        .model_bucket_time_input_expression_id.reset();
    const auto missing_bucket_carrier_refused = sbsql::LowerToSblr(
        missing_bucket_carrier, *downsample_cst, Session());
    passed &= Require(
        HasDiagnostic(missing_bucket_carrier_refused.messages,
                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
        "lowering admitted a missing TIME_BUCKET input carrier");
    auto reordered = *downsample_bound;
    const auto operation = std::ranges::find_if(
        reordered.native_relational.expressions, [](const auto& expression) {
          return expression.canonical_operator_name == "TIME_DOWNSAMPLE";
        });
    if (operation != reordered.native_relational.expressions.end()) {
      std::swap(operation->child_expression_ids[1],
                operation->child_expression_ids[2]);
    }
    const auto refused =
        sbsql::LowerToSblr(reordered, *downsample_cst, Session());
    passed &= Require(
        HasDiagnostic(refused.messages,
                      "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1"),
        "time-series lowering admitted reordered interval/value children");

    auto duplicate_descriptor = *downsample_bound;
    duplicate_descriptor.native_relational.descriptors[9].descriptor_uuid =
        duplicate_descriptor.native_relational.descriptors[3].descriptor_uuid;
    const auto duplicate_refused = sbsql::LowerToSblr(
        duplicate_descriptor, *downsample_cst, Session());
    passed &= Require(
        HasDiagnostic(duplicate_refused.messages,
                      "SB_MODEL_BINDING_INCOMPLETE_V1") ||
            HasDiagnostic(duplicate_refused.messages,
                          "SBLR.PLAN_TREE.INVALID_HANDLE"),
        "time-series lowering admitted a duplicate derived descriptor UUID:" +
            DiagnosticSummary(duplicate_refused.messages));

    auto substituted_output = *downsample_bound;
    substituted_output.native_relational.outputs[3].descriptor_id =
        substituted_output.native_relational.outputs[2].descriptor_id;
    const auto substitution_refused = sbsql::LowerToSblr(
        substituted_output, *downsample_cst, Session());
    passed &= Require(
        HasDiagnostic(substitution_refused.messages,
                      "SB_MODEL_TYPED_EXCHANGE_INVALID_V1") ||
            HasDiagnostic(substitution_refused.messages,
                          "SB_MODEL_BINDING_INCOMPLETE_V1"),
        "time-series lowering admitted a substituted bucket-end descriptor");
  }
  if (raw_bound.has_value() && raw_cst.has_value()) {
    auto stray_bucket_carrier = *raw_bound;
    auto& source =
        stray_bucket_carrier.native_relational.catalog_relation_sources.front();
    source.model_bucket_interval_expression_id =
        source.model_range_start_expression_id;
    source.model_bucket_time_input_expression_id =
        source.model_range_end_expression_id;
    const auto stray_refused = sbsql::LowerToSblr(
        stray_bucket_carrier, *raw_cst, Session());
    passed &= Require(
        HasDiagnostic(stray_refused.messages,
                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
        "lowering admitted stray TIME_BUCKET child carriers");
  }

  const auto lowercase_aggregate = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT TIME_DOWNSAMPLE(count, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) ts WHERE TIME_RANGE(ts, "
      "TIMESTAMP '2026-08-10T12:00:00Z', TIMESTAMP "
      "'2026-08-10T12:02:00Z');"));
  passed &= Require(
      lowercase_aggregate.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          !lowercase_aggregate.produces_sblr &&
          HasDiagnostic(lowercase_aggregate.native_relational.messages,
                        "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1"),
      "lowercase time-series aggregate alias survived parser admission");

  const std::array<std::string, 7> refused_sql{{
      "SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) ts;",
      "SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) ts WHERE "
      "TIME_RANGE(other, TIMESTAMP '2026-08-10T12:00:00Z', TIMESTAMP "
      "'2026-08-10T12:02:00Z');",
      "SELECT TIME_DOWNSAMPLE(MEDIAN, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) ts WHERE TIME_RANGE(ts, "
      "TIMESTAMP '2026-08-10T12:00:00Z', TIMESTAMP "
      "'2026-08-10T12:02:00Z');",
      "SELECT TIME_DOWNSAMPLE(count, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) ts WHERE TIME_RANGE(ts, "
      "TIMESTAMP '2026-08-10T12:00:00Z', TIMESTAMP "
      "'2026-08-10T12:02:00Z');",
      "SELECT * FROM INFLUX_LINE_PROTOCOL('m,t=v f=1 0');",
      "SELECT * FROM TIME_SERIES_APPEND(app.series_fixture);",
      "TIME_SERIES_APPEND app.series_fixture;",
  }};
  for (const auto& sql : refused_sql) {
    const auto ast = sbsql::BuildAst(sbsql::BuildCst(sql));
    passed &= Require(
        ast.native_relational.status ==
                sbsql::NativeRelationalParseStatus::kRefused &&
            !ast.produces_sblr,
        "time-series refused grammar reached binding: " + sql);
  }
  return passed;
}

bool VectorGrammarBindingLowering() {
  struct Case {
    const char* sql;
    const char* operation;
    const char* semantic;
    bool filtered;
  };
  const std::array<Case, 2> cases{{
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v WHERE "
       "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 2) AS nearest;",
       "VECTOR_EXACT_SEARCH",
       "sblr.model-source.vector-exact-search.v1", false},
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v WHERE "
       "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 2) AND "
       "VECTOR_FILTER(v, v.metadata = '{\"group\":\"a\"}');",
       "VECTOR_FILTERED_SEARCH",
       "sblr.model-source.vector-filtered-search.v1", true},
  }};
  bool passed = true;
  std::optional<sbsql::BoundStatement> retained_bound;
  std::optional<sbsql::CstDocument> retained_cst;
  for (const auto& test : cases) {
    const auto cst = sbsql::BuildCst(test.sql);
    const auto ast = sbsql::BuildAst(cst);
    passed &= Require(
        ast.native_relational.accepted() && ast.requires_name_resolution &&
            !ast.produces_sblr &&
            ast.native_relational.catalog_relation_sources.size() == 1 &&
            ast.native_relational.catalog_relation_sources.front().source_kind ==
                sbsql::NativeRelationSourceAstKind::kVector &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_family_id == "vector" &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_operation_id == test.operation &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_vector_metric_id == "L2_SQUARED" &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_vector_top_k == 2 &&
            ast.native_relational.model_object_resolution_requests.size() == 1 &&
            ast.native_relational.model_object_resolution_requests.front()
                    .object_class == "vector",
        std::string("ordinary vector grammar/object extraction drifted: ") +
            DiagnosticSummary(ast.native_relational.messages));
    if (!ast.native_relational.accepted()) continue;
    const auto& source = ast.native_relational.catalog_relation_sources.front();
    passed &= Require(
        source.model_vector_alias_expression_id.has_value() &&
            source.model_vector_nearest_expression_id.has_value() &&
            source.model_vector_query_expression_id.has_value() &&
            source.model_vector_metric_expression_id.has_value() &&
            source.model_vector_top_k_expression_id.has_value() &&
            (test.filtered ==
             source.model_vector_filter_expression_id.has_value()) &&
            (!source.model_vector_result_alias.has_value() ||
             source.model_vector_result_alias->spelling == "nearest"),
        "vector typed-DAG or result-alias identity is incomplete");

    auto context = VectorContextFor(ast.native_relational);
    const auto bound =
        sbsql::BindAst(ast, cst, Config(), Session(), {}, &context);
    passed &= Require(
        bound.bound && bound.native_relational.bound &&
            bound.native_relational.catalog_relation_sources.size() == 1 &&
            bound.native_relational.catalog_relation_sources.front()
                    .object_uuid == Uuid(1040) &&
            bound.native_relational.catalog_relation_sources.front()
                    .columns.size() == 2 &&
            bound.native_relational.outputs.size() == 3 &&
            bound.native_relational.relations.front().semantic_variant_id ==
                test.semantic,
        std::string("vector engine-projected binding drifted: ") +
            DiagnosticSummary(bound.messages));
    if (!bound.bound) continue;
    const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
    const auto verified = sbsql::VerifySblrEnvelope(lowered);
    passed &= Require(
        !lowered.messages.has_errors() && verified.admitted &&
            HasOperand(lowered, "text", "relational_statement_timestamp",
                       context.statement_timestamp) &&
            std::ranges::any_of(lowered.operands, [](const auto& operand) {
              return operand.type == "relational_node_binding_v1" &&
                     operand.value.find(
                         "53424c525f4d4f44454c5f534f555243455f5631") == 0;
            }),
        std::string("vector source did not lower through SBLR_MODEL_SOURCE_V1: ") +
            DiagnosticSummary(lowered.messages) + "/" +
            DiagnosticSummary(verified.messages));
    if (!test.filtered) {
      retained_bound = bound;
      retained_cst = cst;
    }

    auto duplicate_expression = context;
    duplicate_expression.expressions.back().expression_id =
        duplicate_expression.expressions.front().expression_id;
    const auto duplicate_refused = sbsql::BindAst(
        ast, cst, Config(), Session(), {}, &duplicate_expression);
    passed &= Require(
        !duplicate_refused.bound &&
            HasDiagnostic(duplicate_refused.messages,
                          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        "vector binding admitted a duplicated expression identity");

    auto reordered_expression = context;
    std::swap(reordered_expression.expressions[3],
              reordered_expression.expressions[4]);
    const auto reordered_refused = sbsql::BindAst(
        ast, cst, Config(), Session(), {}, &reordered_expression);
    passed &= Require(
        !reordered_refused.bound &&
            HasDiagnostic(reordered_refused.messages,
                          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        "vector binding admitted reordered expression evidence");

    auto storage_type = context;
    storage_type.descriptors[0].canonical_type_name = "text";
    const auto storage_type_refused = sbsql::BindAst(
        ast, cst, Config(), Session(), {}, &storage_type);
    passed &= Require(
        !storage_type_refused.bound &&
            HasDiagnostic(storage_type_refused.messages,
                          "SB_MODEL_VECTOR_VALUE_REFUSED_V1"),
        "vector binding admitted a substituted storage descriptor type");

    auto output_type = context;
    output_type.descriptors[2].canonical_type_name = "real64";
    const auto output_type_refused = sbsql::BindAst(
        ast, cst, Config(), Session(), {}, &output_type);
    passed &= Require(
        !output_type_refused.bound &&
            HasDiagnostic(output_type_refused.messages,
                          "SB_MODEL_BINDING_INCOMPLETE_V1"),
        "vector binding admitted a substituted row UUID output type");

    auto top_k_type = context;
    top_k_type.descriptors[4].canonical_type_name = "real64";
    const auto top_k_type_refused = sbsql::BindAst(
        ast, cst, Config(), Session(), {}, &top_k_type);
    passed &= Require(
        !top_k_type_refused.bound &&
            HasDiagnostic(top_k_type_refused.messages,
                          "SB_MODEL_VECTOR_VALUE_REFUSED_V1"),
        "vector binding admitted a substituted top-k descriptor type");
  }

  const std::array<std::pair<const char*, const char*>, 8> refusals{{
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v;",
       "SB_MODEL_VECTOR_NEAREST_REFUSED_V1"},
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v WHERE "
       "VECTOR_NEAREST(v, VECTOR '[1,0,0]', MANHATTAN, 2);",
       "SB_MODEL_VECTOR_METRIC_REFUSED_V1"},
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v WHERE "
       "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 0);",
       "SB_MODEL_VECTOR_TOP_K_REFUSED_V1"},
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v WHERE "
       "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 02);",
       "SB_MODEL_VECTOR_TOP_K_REFUSED_V1"},
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v WHERE "
       "VECTOR_NEAREST(other, VECTOR '[1,0,0]', L2_SQUARED, 2);",
       "SB_MODEL_VECTOR_VALUE_REFUSED_V1"},
      {"SELECT * FROM VECTOR_SOURCE(app.vector_fixture) AS v WHERE "
       "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 2) AND "
       "VECTOR_FILTER(v, v.metadata <> '{}');",
       "SB_MODEL_VECTOR_FILTER_REFUSED_V1"},
      {"SELECT * FROM VECTOR_PROVIDER_REQUEST(app.vector_fixture);",
       "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1"},
      {"VECTOR_UPSERT app.vector_fixture;",
       "SB_MODEL_QUERY_WRITE_REFUSED_V1"},
  }};
  for (const auto& [sql, diagnostic] : refusals) {
    const auto ast = sbsql::BuildAst(sbsql::BuildCst(sql));
    passed &= Require(
        ast.native_relational.status ==
                sbsql::NativeRelationalParseStatus::kRefused &&
            HasDiagnostic(ast.native_relational.messages, diagnostic),
        std::string("vector parser refusal identity drifted: ") + diagnostic +
            ":" + DiagnosticSummary(ast.native_relational.messages));
  }

  if (retained_bound.has_value() && retained_cst.has_value()) {
    auto reordered = *retained_bound;
    auto nearest = std::ranges::find_if(
        reordered.native_relational.expressions, [](const auto& expression) {
          return expression.canonical_operator_name == "VECTOR_NEAREST";
        });
    if (nearest != reordered.native_relational.expressions.end() &&
        nearest->child_expression_ids.size() == 4) {
      std::swap(nearest->child_expression_ids[1],
                nearest->child_expression_ids[2]);
    }
    const auto refused =
        sbsql::LowerToSblr(reordered, *retained_cst, Session());
    passed &= Require(
        HasDiagnostic(refused.messages,
                      "SB_MODEL_VECTOR_NEAREST_REFUSED_V1"),
        "vector lowering admitted reordered nearest children");
  }
  return passed;
}

bool SearchGrammarBindingLowering() {
  struct Case {
    const char* sql;
    const char* operation;
    const char* query_kind;
    bool filtered;
  };
  const std::array<Case, 4> cases{{
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_TERMS('quick fox'), app.ascii_v1, 3);",
       "SEARCH_RANKED_QUERY", "SEARCH_TERMS", false},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_PHRASE('quick fox'), app.ascii_v1, 3);",
       "SEARCH_PHRASE_QUERY", "SEARCH_PHRASE", false},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_FUZZY('quik', 1), app.ascii_v1, 3);",
       "SEARCH_FUZZY_QUERY", "SEARCH_FUZZY", false},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_TERMS('quick fox'), app.ascii_v1, 3) AND "
       "SEARCH_FILTER(s, s.category = 'news');",
       "SEARCH_RANKED_QUERY", "SEARCH_TERMS", true},
  }};
  bool passed = true;
  for (const auto& test : cases) {
    const auto cst = sbsql::BuildCst(test.sql);
    const auto ast = sbsql::BuildAst(cst);
    passed &= Require(
        ast.native_relational.accepted() && ast.requires_name_resolution &&
            !ast.produces_sblr &&
            ast.native_relational.catalog_relation_sources.size() == 1 &&
            ast.native_relational.catalog_relation_sources.front().source_kind ==
                sbsql::NativeRelationSourceAstKind::kSearch &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_operation_id == test.operation &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_search_query_kind == test.query_kind &&
            ast.native_relational.catalog_relation_sources.front()
                    .model_search_top_k == 3 &&
            ast.native_relational.model_object_resolution_requests.size() == 2 &&
            ast.native_relational.model_object_resolution_requests[0]
                    .object_class == "search" &&
            ast.native_relational.model_object_resolution_requests[1]
                    .object_class == "search_analyzer",
        std::string("ordinary search grammar/object extraction drifted: ") +
            DiagnosticSummary(ast.native_relational.messages));
    if (!ast.native_relational.accepted()) continue;
    const auto& source = ast.native_relational.catalog_relation_sources.front();
    passed &= Require(
        source.model_search_alias_expression_id.has_value() &&
            source.model_search_match_expression_id.has_value() &&
            source.model_search_query_expression_id.has_value() &&
            source.model_search_text_expression_id.has_value() &&
            source.model_search_analyzer_expression_id.has_value() &&
            source.model_search_top_k_expression_id.has_value() &&
            (test.filtered ==
             source.model_search_filter_expression_id.has_value()) &&
            ((std::string(test.query_kind) == "SEARCH_FUZZY") ==
             source.model_search_edit_expression_id.has_value()),
        "search typed-DAG identity is incomplete");
    auto context = SearchContextFor(ast.native_relational);
    const auto bound =
        sbsql::BindAst(ast, cst, Config(), Session(), {}, &context);
    passed &= Require(
        bound.bound && bound.native_relational.bound &&
            bound.native_relational.catalog_relation_sources.size() == 1 &&
            bound.native_relational.catalog_relation_sources.front()
                    .object_uuid == Uuid(1140) &&
            bound.native_relational.catalog_relation_sources.front()
                    .model_search_analyzer_uuid == Uuid(1142) &&
            bound.native_relational.catalog_relation_sources.front()
                    .model_search_analyzer_generation == 17 &&
            bound.native_relational.outputs.size() == 5,
        std::string("search engine-projected binding drifted: ") +
            DiagnosticSummary(bound.messages));
    if (!bound.bound) continue;
    const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
    const auto verified = sbsql::VerifySblrEnvelope(lowered);
    passed &= Require(
        !lowered.messages.has_errors() && verified.admitted &&
            HasRelationalExpressionFragment(
                lowered, Hex("SEARCH_ANALYZER_BINDING")) &&
            HasRelationalExpressionFragment(lowered, Uuid(1142)) &&
            HasRelationalExpressionFragment(lowered, Hex("17")) &&
            HasRelationalExpressionFragment(
                lowered,
                Hex("9033908d159ddd442f2042467fd49e0a12b47679f7514e9aa6e55488e151d316")) &&
            HasRelationalExpressionFragment(lowered, Hex(test.query_kind)) &&
            HasRelationalExpressionFragment(lowered, Hex("3")) &&
            std::ranges::any_of(lowered.operands, [](const auto& operand) {
              return operand.type == "relational_node_binding_v1" &&
                     operand.value.find(
                         "53424c525f4d4f44454c5f534f555243455f5631") == 0;
            }),
        std::string("search did not lower through SBLR_MODEL_SOURCE_V1: ") +
            DiagnosticSummary(lowered.messages) + "/" +
            DiagnosticSummary(verified.messages));

    auto stale_analyzer = context;
    stale_analyzer.search_analyzer_generation = 0;
    const auto analyzer_refused = sbsql::BindAst(
        ast, cst, Config(), Session(), {}, &stale_analyzer);
    passed &= Require(
        !analyzer_refused.bound &&
            HasDiagnostic(analyzer_refused.messages,
                          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        "search binding admitted an absent analyzer generation");
  }
  const std::array<std::pair<const char*, const char*>, 8> refusals{{
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s;",
       "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_TERMS('fox'), app.ascii_v1, 0);",
       "SB_MODEL_SEARCH_TOP_K_REFUSED_V1"},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_FUZZY('fox', 2), app.ascii_v1, 3);",
       "SB_MODEL_SEARCH_QUERY_TOKEN_LIMIT_REFUSED_V1"},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(other, SEARCH_TERMS('fox'), app.ascii_v1, 3);",
       "SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1"},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_TERMS('fox'), app.ascii_v1, 3) AND "
       "SEARCH_FILTER(s, s.category <> 'news');",
       "SB_MODEL_SEARCH_FILTER_REFUSED_V1"},
      {"SELECT * FROM OPENSEARCH_DSL('{\"query\":{}}');",
       "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1"},
      {"SEARCH_UPSERT app.search_fixture;",
       "SB_MODEL_QUERY_WRITE_REFUSED_V1"},
      {"SELECT * FROM SEARCH_SOURCE(app.search_fixture) AS s WHERE "
       "SEARCH_MATCH(s, SEARCH_UNKNOWN('fox'), app.ascii_v1, 3);",
       "SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1"},
  }};
  for (const auto& [sql, diagnostic] : refusals) {
    const auto ast = sbsql::BuildAst(sbsql::BuildCst(sql));
    passed &= Require(
        ast.native_relational.status ==
                sbsql::NativeRelationalParseStatus::kRefused &&
            HasDiagnostic(ast.native_relational.messages, diagnostic),
        std::string("search parser refusal identity drifted: ") + diagnostic +
            ":" + DiagnosticSummary(ast.native_relational.messages));
  }
  return passed;
}

bool GraphGrammarBindingLowering() {
  const auto match_cst = sbsql::BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g "
      "WHERE GRAPH_MATCH(g, 'vertex(label=Person)');");
  const auto match_ast = sbsql::BuildAst(match_cst);
  bool passed = Require(
      match_ast.native_relational.accepted() &&
          match_ast.requires_name_resolution && !match_ast.produces_sblr &&
          match_ast.native_relational.catalog_relation_sources.size() == 1 &&
          match_ast.native_relational.catalog_relation_sources.front()
                  .source_kind == sbsql::NativeRelationSourceAstKind::kGraph &&
          match_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_id == "GRAPH_MATCH" &&
          match_ast.native_relational.model_object_resolution_requests.size() ==
              1 &&
          match_ast.native_relational.model_object_resolution_requests.front()
                  .object_class == "graph",
      "GRAPH_SOURCE/GRAPH_MATCH grammar or resolution identity drifted");
  if (!passed) return false;
  auto match_context = GraphContextFor(match_ast.native_relational);
  const auto match_bound = sbsql::BindAst(
      match_ast, match_cst, Config(), Session(), {}, &match_context);
  passed &= Require(
      match_bound.bound && match_bound.native_relational.bound &&
          match_bound.native_relational.catalog_relation_sources.front()
                  .object_uuid == Uuid(740) &&
          match_bound.native_relational.relations.front().semantic_variant_id ==
              "sblr.model-source.graph-match.v1",
      "GRAPH_MATCH did not bind the exact graph UUID and semantic");
  if (!match_bound.bound) return false;
  const auto match_lowered =
      sbsql::LowerToSblr(match_bound, match_cst, Session());
  const auto match_verified = sbsql::VerifySblrEnvelope(match_lowered);
  passed &= Require(
      !match_lowered.messages.has_errors() &&
          match_verified.admitted &&
          std::ranges::any_of(match_lowered.operands, [](const auto& operand) {
            return operand.type == "relational_node_binding_v1" &&
                   operand.value.find(
                       "53424c525f4d4f44454c5f534f555243455f5631") == 0;
          }),
      "GRAPH_MATCH did not lower as object-backed SBLR_MODEL_SOURCE_V1: " +
          DiagnosticSummary(match_lowered.messages) + "/" +
          DiagnosticSummary(match_verified.messages));

  const auto implicit_match_cst = sbsql::BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) "
      "WHERE GRAPH_MATCH(graph_fixture, 'vertex(*)');");
  const auto implicit_match_ast = sbsql::BuildAst(implicit_match_cst);
  passed &= Require(
      implicit_match_ast.native_relational.accepted() &&
          implicit_match_ast.native_relational.catalog_relation_sources.front()
              .alias.has_value() &&
          implicit_match_ast.native_relational.catalog_relation_sources.front()
                  .alias->spelling == "graph_fixture" &&
          !implicit_match_ast.native_relational.catalog_relation_sources.front()
               .alias_is_explicit,
      "GRAPH_SOURCE omitted alias did not derive the terminal graph name");
  if (implicit_match_ast.native_relational.accepted()) {
    auto implicit_context = GraphContextFor(implicit_match_ast.native_relational);
    const auto implicit_bound = sbsql::BindAst(
        implicit_match_ast, implicit_match_cst, Config(), Session(), {},
        &implicit_context);
    const auto implicit_lowered =
        sbsql::LowerToSblr(implicit_bound, implicit_match_cst, Session());
    passed &= Require(
        implicit_bound.bound && !implicit_lowered.messages.has_errors() &&
            sbsql::VerifySblrEnvelope(implicit_lowered).admitted,
        "implicit GRAPH_SOURCE alias did not bind and lower exactly");
  }

  const auto malformed_pattern = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g "
      "WHERE GRAPH_MATCH(g, 'opaque donor pattern');"));
  passed &= Require(
      malformed_pattern.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          !malformed_pattern.produces_sblr &&
          HasDiagnostic(malformed_pattern.native_relational.messages,
                        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
      "opaque GRAPH_MATCH pattern survived parser admission");
  const std::array<std::string, 3> unsafe_graph_patterns{
      "vertex(label=Person-Admin)", "vertex(label=Person Admin)",
      std::string("vertex(label=Person") + static_cast<char>(1) + "Admin)"};
  for (const auto& unsafe_pattern : unsafe_graph_patterns) {
    const auto unsafe = sbsql::BuildAst(sbsql::BuildCst(
        "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g WHERE "
        "GRAPH_MATCH(g, '" + unsafe_pattern + "');"));
    passed &= Require(
        unsafe.native_relational.status ==
                sbsql::NativeRelationalParseStatus::kRefused &&
            !unsafe.produces_sblr,
        "unsafe GRAPH_MATCH label pattern survived parser admission");
  }

  const auto expand_cst = sbsql::BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g, "
      "GRAPH_EXPAND(g, BOTH, 0, 3) AS p;");
  const auto expand_ast = sbsql::BuildAst(expand_cst);
  passed &= Require(
      expand_ast.native_relational.accepted() &&
          expand_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_id == "GRAPH_EXPAND" &&
          expand_ast.native_relational.catalog_relation_sources.front()
                  .model_graph_minimum_depth == 0 &&
          expand_ast.native_relational.catalog_relation_sources.front()
                  .model_graph_maximum_depth == 3 &&
          expand_ast.native_relational.catalog_relation_sources.front()
                  .model_graph_cycle_policy == "visited_set",
      "bounded GRAPH_EXPAND grammar or explicit visited-set policy drifted");
  if (!expand_ast.native_relational.accepted()) return false;
  auto expand_context = GraphContextFor(expand_ast.native_relational);
  const auto expand_bound = sbsql::BindAst(
      expand_ast, expand_cst, Config(), Session(), {}, &expand_context);
  passed &= Require(
      expand_bound.bound &&
          expand_bound.native_relational.relations.front().semantic_variant_id ==
              "sblr.model-expand.graph-expand.v1",
      "GRAPH_EXPAND did not bind its object-backed model semantic");
  if (!expand_bound.bound) return false;

  auto missing_source_alias_ast = expand_ast;
  missing_source_alias_ast.native_relational.catalog_relation_sources.front()
      .model_source_alias.reset();
  auto missing_source_alias_context =
      GraphContextFor(missing_source_alias_ast.native_relational);
  const auto missing_source_alias_bound = sbsql::BindAst(
      missing_source_alias_ast, expand_cst, Config(), Session(), {},
      &missing_source_alias_context);
  passed &= Require(
      !missing_source_alias_bound.bound,
      "GRAPH_EXPAND admitted a cleared source-alias binding");

  auto substituted_source_alias_ast = expand_ast;
  substituted_source_alias_ast.native_relational.catalog_relation_sources
      .front()
      .model_source_alias->spelling = "substituted_graph_source";
  auto substituted_source_alias_context =
      GraphContextFor(substituted_source_alias_ast.native_relational);
  const auto substituted_source_alias_bound = sbsql::BindAst(
      substituted_source_alias_ast, expand_cst, Config(), Session(), {},
      &substituted_source_alias_context);
  passed &= Require(
      !substituted_source_alias_bound.bound,
      "GRAPH_EXPAND admitted a source alias that differed from its root operand");

  const auto expand_lowered =
      sbsql::LowerToSblr(expand_bound, expand_cst, Session());
  const auto expand_verified = sbsql::VerifySblrEnvelope(expand_lowered);
  passed &= Require(
      !expand_lowered.messages.has_errors() &&
          expand_verified.admitted &&
          std::ranges::any_of(expand_lowered.operands, [](const auto& operand) {
            return operand.type == "relational_node_binding_v1" &&
                   operand.value.find(
                       "53424c525f4d4f44454c5f455850414e445f5631") == 0 &&
                   operand.value.find(Uuid(740)) != std::string::npos;
          }),
      "GRAPH_EXPAND did not lower as object-backed SBLR_MODEL_EXPAND_V1: " +
          DiagnosticSummary(expand_lowered.messages) + "/" +
          DiagnosticSummary(expand_verified.messages));

  auto missing_lowered_source_alias = expand_bound;
  missing_lowered_source_alias.native_relational.catalog_relation_sources
      .front()
      .model_source_alias.reset();
  passed &= Require(
      HasDiagnostic(
          sbsql::LowerToSblr(missing_lowered_source_alias, expand_cst, Session())
              .messages,
          "SB_MODEL_BINDING_INCOMPLETE_V1"),
      "GRAPH_EXPAND lowering admitted a cleared source-alias binding");

  const auto optional_expand_cst = sbsql::BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g, "
      "GRAPH_EXPAND(g, OUTGOING, 001, 0257);");
  const auto optional_expand_ast = sbsql::BuildAst(optional_expand_cst);
  passed &= Require(
      optional_expand_ast.native_relational.accepted() &&
          optional_expand_ast.native_relational.catalog_relation_sources.front()
                  .model_source_alias->spelling == "g" &&
          optional_expand_ast.native_relational.catalog_relation_sources.front()
                  .alias->spelling == "g" &&
          !optional_expand_ast.native_relational.catalog_relation_sources.front()
               .alias_is_explicit &&
          optional_expand_ast.native_relational.catalog_relation_sources.front()
                  .model_graph_minimum_depth == 1 &&
          optional_expand_ast.native_relational.catalog_relation_sources.front()
                  .model_graph_maximum_depth == 257,
      "optional GRAPH_EXPAND alias or finite leading-zero depth drifted");
  if (optional_expand_ast.native_relational.accepted()) {
    auto optional_context = GraphContextFor(optional_expand_ast.native_relational);
    const auto optional_bound = sbsql::BindAst(
        optional_expand_ast, optional_expand_cst, Config(), Session(), {},
        &optional_context);
    const auto optional_lowered =
        sbsql::LowerToSblr(optional_bound, optional_expand_cst, Session());
    passed &= Require(
        optional_bound.bound && !optional_lowered.messages.has_errors() &&
            sbsql::VerifySblrEnvelope(optional_lowered).admitted,
        "optional GRAPH_EXPAND alias did not preserve source binding/lowering");
  }

  auto allow_cycles = expand_bound;
  allow_cycles.native_relational.catalog_relation_sources.front()
      .model_graph_cycle_policy = "allow_cycles";
  const auto allow_cycles_lowered =
      sbsql::LowerToSblr(allow_cycles, expand_cst, Session());
  passed &= Require(
      HasDiagnostic(allow_cycles_lowered.messages,
                    "SB_MODEL_BINDING_INCOMPLETE_V1"),
      "allow-cycles substitution reached SBLR");
  auto inverted = expand_bound;
  inverted.native_relational.catalog_relation_sources.front()
      .model_graph_minimum_depth = 4;
  const auto inverted_lowered =
      sbsql::LowerToSblr(inverted, expand_cst, Session());
  passed &= Require(
      HasDiagnostic(inverted_lowered.messages,
                    "SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1"),
      "inverted graph bounds did not receive the exact refusal");
  return passed;
}

bool WireFrontdoorProjectionCohort() {
  constexpr std::uint64_t kExpectedMask = (1ull << 22) - 1;
  const auto mask = sbsql::Rcp073DocumentFrontdoorProofMaskForTest();
  return Require(mask == kExpectedMask,
                 "wire front-door projection/refusal mask was incomplete: " +
                     std::to_string(mask));
}

bool GraphWireFrontdoorProjectionCohort() {
  constexpr std::uint64_t kExpectedMask = (1ull << 22) - 1;
  const auto mask = sbsql::Rcp074GraphFrontdoorProofMaskForTest();
  return Require(mask == kExpectedMask,
                 "graph wire front-door/refusal mask was incomplete: " +
                     std::to_string(mask));
}

bool SpatialGrammar() {
  const auto source_ast = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture);"));
  bool passed = Require(
      source_ast.native_relational.accepted() &&
          source_ast.native_relational.catalog_relation_sources.size() == 1 &&
          source_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_ids ==
              std::vector<std::string>{"SPATIAL_SOURCE"} &&
          source_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_expression_ids.size() == 1 &&
          source_ast.native_relational.model_object_resolution_requests.size() ==
              1,
      "SPATIAL_SOURCE grammar/root drifted: " +
          DiagnosticSummary(source_ast.messages));
  const auto match_cst = sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s "
      "WHERE SPATIAL_MATCH(s, INTERSECTS, POINT(0, 0), app.cartesian_crs);");
  const auto match_ast = sbsql::BuildAst(match_cst);
  passed &= Require(
      match_ast.native_relational.accepted() &&
          match_ast.native_relational.catalog_relation_sources.size() == 1,
      "SPATIAL_MATCH grammar was not accepted: " +
          DiagnosticSummary(match_ast.messages));
  if (match_ast.native_relational.accepted()) {
    const auto& match =
        match_ast.native_relational.catalog_relation_sources.front();
    passed &= Require(
        match.source_kind == sbsql::NativeRelationSourceAstKind::kSpatial &&
            match.model_family_id == "spatial" &&
            match.model_operation_id.empty() &&
            match.model_operation_ids ==
                std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_MATCH"} &&
            match.model_operation_expression_ids.size() == 2 &&
            match.model_spatial_predicate_id == "INTERSECTS" &&
            match.model_spatial_crs_names.size() == 1 &&
          match_ast.native_relational.model_object_resolution_requests.size() ==
              2,
        "SPATIAL_MATCH identities or object/CRS requests drifted");
  }
  const auto nearest_cst = sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s, "
      "SPATIAL_NEAREST(s, POINT(0, 0), app.cartesian_crs, 3) AS n;");
  const auto nearest_ast = sbsql::BuildAst(nearest_cst);
  passed &= Require(
      nearest_ast.native_relational.accepted() &&
          nearest_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_ids ==
              std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_NEAREST"} &&
          nearest_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_expression_ids.size() == 2 &&
          nearest_ast.native_relational.catalog_relation_sources.front()
                  .model_spatial_top_k == 3 &&
          nearest_ast.native_relational.catalog_relation_sources.front()
                  .model_source_alias->spelling == "s" &&
          nearest_ast.native_relational.catalog_relation_sources.front()
                  .alias->spelling == "n",
      "SPATIAL_NEAREST grammar/top-k drifted: " +
          DiagnosticSummary(nearest_ast.messages));
  const auto both_ast = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s, "
      "SPATIAL_NEAREST(s, POINT(1, 1), app.cartesian_crs, 7) AS n "
      "WHERE SPATIAL_MATCH(s, CONTAINS, POINT(0, 0), app.cartesian_crs);"));
  passed &= Require(
      both_ast.native_relational.accepted() &&
          both_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_ids ==
              std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_MATCH",
                                       "SPATIAL_NEAREST"} &&
          both_ast.native_relational.catalog_relation_sources.front()
                  .model_operation_expression_ids.size() == 3 &&
          both_ast.native_relational.catalog_relation_sources.front()
                  .model_spatial_query_expression_ids.size() == 2 &&
          both_ast.native_relational.catalog_relation_sources.front()
                  .model_spatial_crs_names.size() == 2 &&
          both_ast.native_relational.model_object_resolution_requests.size() ==
              3,
      "SPATIAL_MATCH+NEAREST semantic order drifted: " +
          DiagnosticSummary(both_ast.messages));
  const auto inferred = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s "
      "WHERE SPATIAL_MATCH(s, INTERSECTS, POINT(0, 0), inferred_crs);"));
  passed &= Require(
      inferred.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(inferred.messages,
                        "SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1"),
      "unqualified/inferred CRS was not refused");
  const auto top_k = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s, "
      "SPATIAL_NEAREST(s, POINT(0, 0), app.cartesian_crs, 4097);"));
  passed &= Require(
      top_k.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(top_k.messages, "SB_MODEL_SPATIAL_TOP_K_REFUSED_V1"),
      "spatial top-k 4097 was not refused");
  const auto nearest_in_where = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s "
      "WHERE SPATIAL_NEAREST(s, POINT(0, 0), app.cartesian_crs, 3);"));
  passed &= Require(
      nearest_in_where.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(nearest_in_where.messages,
                        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
      "SPATIAL_NEAREST in WHERE was not refused");
  const auto missing_alias = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture), "
      "SPATIAL_NEAREST(spatial_fixture, POINT(0, 0), app.cartesian_crs, 3);"));
  passed &= Require(
      missing_alias.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(missing_alias.messages,
                        "SB_MODEL_BINDING_INCOMPLETE_V1"),
      "SPATIAL_NEAREST without explicit source alias was not refused");
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture);",
      {"SPATIAL_SOURCE"});
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s WHERE "
      "SPATIAL_MATCH(s, INTERSECTS, POINT(0, 0), app.cartesian_crs);",
      {"SPATIAL_SOURCE", "SPATIAL_MATCH"});
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s, "
      "SPATIAL_NEAREST(s, POINT(0, 0), app.cartesian_crs, 3) AS n;",
      {"SPATIAL_SOURCE", "SPATIAL_NEAREST"});
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s, "
      "SPATIAL_NEAREST(s, POINT(1, 1), app.cartesian_crs, 7) AS n WHERE "
      "SPATIAL_MATCH(s, CONTAINS, POINT(0, 0), app.cartesian_crs);",
      {"SPATIAL_SOURCE", "SPATIAL_MATCH", "SPATIAL_NEAREST"});
  return passed;
}

bool ColumnarGrammar() {
  const auto source = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c;"));
  bool passed = Require(
      source.native_relational.accepted() &&
          source.native_relational.catalog_relation_sources.size() == 1 &&
          source.native_relational.catalog_relation_sources.front()
                  .source_kind ==
              sbsql::NativeRelationSourceAstKind::kColumnar &&
          source.native_relational.catalog_relation_sources.front()
                  .model_operation_id == "COLUMNAR_SOURCE",
      "COLUMNAR_SOURCE grammar was not accepted: " +
          DiagnosticSummary(source.messages));
  const auto project = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c, "
      "COLUMNAR_PROJECT(c, c.payload, c.row_uuid) AS p;"));
  passed &= Require(
      project.native_relational.accepted() &&
          project.native_relational.catalog_relation_sources.front()
                  .model_operation_ids ==
              std::vector<std::string>{"COLUMNAR_SOURCE",
                                       "COLUMNAR_PROJECT"} &&
          project.native_relational.catalog_relation_sources.front()
                  .model_columnar_project_expression_ids.size() == 2 &&
          project.native_relational.catalog_relation_sources.front()
                  .model_source_alias->spelling == "c" &&
          project.native_relational.catalog_relation_sources.front()
                  .alias->spelling == "p",
      "COLUMNAR_PROJECT grammar/order drifted: " +
          DiagnosticSummary(project.messages));
  const auto filter = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c "
      "WHERE COLUMNAR_FILTER(c, c.join_key > 1);"));
  passed &= Require(
      filter.native_relational.accepted() &&
          filter.native_relational.catalog_relation_sources.front()
                  .model_operation_ids ==
              std::vector<std::string>{"COLUMNAR_SOURCE",
                                       "COLUMNAR_FILTER"} &&
          filter.native_relational.catalog_relation_sources.front()
                  .model_columnar_predicate_expression_id.has_value(),
      "COLUMNAR_FILTER grammar/predicate drifted: " +
          DiagnosticSummary(filter.messages));
  const auto both = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c, "
      "COLUMNAR_PROJECT(c, c.payload, c.row_uuid) AS p "
      "WHERE COLUMNAR_FILTER(c, c.hidden_join_key > 1);"));
  passed &= Require(
      both.native_relational.accepted() &&
          both.native_relational.catalog_relation_sources.front()
                  .model_operation_ids ==
              std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_FILTER",
                                       "COLUMNAR_PROJECT"} &&
          both.native_relational.catalog_relation_sources.front()
                  .model_operation_expression_ids.size() == 3 &&
          both.native_relational.catalog_relation_sources.front()
                  .model_columnar_project_expression_ids.size() == 2 &&
          both.native_relational.catalog_relation_sources.front()
                  .model_columnar_predicate_expression_id.has_value(),
      "COLUMNAR_FILTER+PROJECT semantic order drifted: " +
          DiagnosticSummary(both.messages));
  const auto duplicate = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c, "
      "COLUMNAR_PROJECT(c, c.payload, c.payload);"));
  passed &= Require(
      duplicate.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(duplicate.messages,
                        "SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1"),
      "duplicate columnar projection was not refused");
  const auto select_project = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT COLUMNAR_PROJECT(c, c.payload) "
      "FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c;"));
  passed &= Require(
      select_project.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(select_project.messages,
                        "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
      "COLUMNAR_PROJECT in SELECT was not refused");
  const auto hint = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM COLUMNAR_ENGINE_HINT('segment=1');"));
  passed &= Require(
      hint.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          HasDiagnostic(hint.messages, "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1"),
      "opaque columnar engine hint was not refused");
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture);",
      {"COLUMNAR_SOURCE"});
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c WHERE "
      "COLUMNAR_FILTER(c, c.join_key > 1);",
      {"COLUMNAR_SOURCE", "COLUMNAR_FILTER"});
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c, "
      "COLUMNAR_PROJECT(c, c.payload, c.row_uuid) AS p;",
      {"COLUMNAR_SOURCE", "COLUMNAR_PROJECT"});
  passed &= SpatialColumnarBindingLowering(
      "SELECT * FROM COLUMNAR_SOURCE(app.columnar_fixture) AS c, "
      "COLUMNAR_PROJECT(c, c.payload, c.row_uuid) AS p WHERE "
      "COLUMNAR_FILTER(c, c.hidden_join_key > 1);",
      {"COLUMNAR_SOURCE", "COLUMNAR_FILTER", "COLUMNAR_PROJECT"});
  return passed;
}

bool TimeSeriesWireFrontdoorProjectionCohort() {
  constexpr std::uint64_t kExpectedMask = (1ull << 4) - 1;
  const auto mask = sbsql::Rcp076TimeSeriesFrontdoorProofMaskForTest();
  return Require(mask == kExpectedMask,
                 "time-series pre-resolution refusal mask was incomplete: " +
                     std::to_string(mask));
}

bool SpatialColumnarWireFrontdoorProjectionCohort() {
  const auto sql =
      "SELECT * FROM SPATIAL_SOURCE(app.spatial_fixture) AS s INNER JOIN "
      "COLUMNAR_SOURCE(app.columnar_fixture) AS c ON s.row_uuid = c.row_uuid;";
  const auto cst = sbsql::BuildCst(sql);
  const auto ast = sbsql::BuildAst(cst);
  const bool ast_exact = Require(
      ast.native_relational.accepted() &&
          ast.native_relational.catalog_relation_sources.size() == 2 &&
          ast.native_relational.relations.size() == 3 &&
          ast.native_relational.model_object_resolution_requests.size() == 2,
      "spatial/columnar JOIN did not reach the ordinary typed AST: " +
          DiagnosticSummary(ast.messages));
  constexpr std::uint64_t kExpectedMask = (1ull << 13) - 1;
  const auto mask = sbsql::Rcp079SpatialColumnarFrontdoorProofMaskForTest();
  return ast_exact && Require(mask == kExpectedMask,
                 "spatial/columnar front-door projection mask was incomplete: " +
                     std::to_string(mask));
}

bool MultimodelWireFrontdoorProjectionCohort() {
  constexpr std::uint64_t kExpectedMask = (1ull << 15) - 1;
  const auto mask = sbsql::Rcp080MultimodelWireProofMaskForTest();
  return Require(mask == kExpectedMask,
                 "RCP-080 live 3/4/9 engine-projected wire/refusal mask was "
                 "incomplete: " +
                     std::to_string(mask));
}

}  // namespace

int main() {
  bool passed = true;
  passed &= SourcePathGrammarBindingLowering();
  passed &= UnnestGrammarBindingLowering();
  passed &= ExactRefusals();
  passed &= KeyValueGrammarBindingLowering();
  passed &= TimeSeriesGrammarBindingLowering();
  passed &= VectorGrammarBindingLowering();
  passed &= SearchGrammarBindingLowering();
  passed &= GraphGrammarBindingLowering();
  passed &= SpatialGrammar();
  passed &= ColumnarGrammar();
  passed &= WireFrontdoorProjectionCohort();
  passed &= GraphWireFrontdoorProjectionCohort();
  passed &= TimeSeriesWireFrontdoorProjectionCohort();
  passed &= SpatialColumnarWireFrontdoorProjectionCohort();
  passed &= MultimodelWireFrontdoorProjectionCohort();
  passed &= MultimodelJoinBindingLowering();
  return passed ? 0 : 1;
}
