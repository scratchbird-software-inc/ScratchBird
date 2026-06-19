// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000002101";
constexpr std::string_view kRelatedUuid = "019f0000-0000-7000-8000-000000002102";
constexpr std::string_view kThirdRelationUuid = "019f0000-0000-7000-8000-000000002103";
constexpr std::string_view kBoundedOrderedSelectSql =
    "SELECT * FROM customer ORDER BY id DESC LIMIT 2 OFFSET 1";
constexpr std::string_view kBoundedTopSelectSql = "SELECT TOP 2 * FROM customer";
constexpr std::string_view kBoundedFetchFirstSelectSql =
    "SELECT * FROM customer FETCH FIRST 2 ROWS ONLY";
constexpr std::string_view kBoundedFetchNextSelectSql =
    "SELECT * FROM customer FETCH NEXT 2 ROW ONLY";
constexpr std::string_view kBoundedWhereEqualitySelectSql =
    "SELECT * FROM customer WHERE id = 1";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000002130";
constexpr std::string_view kColumnIdUuid = "019f0000-0000-7000-8000-000000002131";
constexpr std::string_view kColumnNameUuid = "019f0000-0000-7000-8000-000000002132";

api::EngineRequestContext g_engine_context;
bool g_engine_context_ready = false;

struct DmlSurfaceEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view family;
  std::string_view sblr_operation_family;
  std::string_view parser_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view validation_fixture_id;
};

struct OrderedSelectClauseEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view family;
  std::string_view sblr_operation_family;
  std::string_view validation_fixture_id;
  std::string_view primary_payload_marker;
  std::string_view secondary_payload_marker;
  std::string_view descriptor_payload_marker;
};

struct GroupByGrammarEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view grammar_production;
};

struct HavingClauseEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view sql_fixture;
};

struct CteGrammarEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view family;
  std::string_view canonical_surface_family;
  std::string_view parser_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view validation_fixture_id;
  std::string_view grammar_production;
};

struct WindowClauseGrammarEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view sql_fixture;
  std::string_view route_marker;
  std::string_view clause_marker;
};

struct WhereClauseGrammarEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view family;
  std::string_view sblr_operation_family;
  std::string_view parser_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view validation_fixture_id;
};

struct FetchClauseGrammarEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
};

struct TopClauseGrammarEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
};

struct SimpleSelectSkeletonEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view family;
  std::string_view sblr_operation_family;
  std::string_view parser_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view validation_fixture_id;
  std::string_view grammar_role;
};

struct DmlContextualKeywordEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view payload_marker;
};

constexpr std::array<DmlContextualKeywordEvidence, 14> kDmlContextualKeywordRows{{
    {"SBSQL-2C97BBAE2A81", "select", "SBSQL-2C97BBAE2A81"},
    {"SBSQL-92A32408C70E", "from", "SBSQL-92A32408C70E"},
    {"SBSQL-DC1B5EC1A284", "where", "SBSQL-DC1B5EC1A284"},
    {"SBSQL-540C91206EA6", "ORDER", "SBSQL-540C91206EA6"},
    {"SBSQL-356228FF8746", "LIMIT", "SBSQL-356228FF8746"},
    {"SBSQL-D9EDA8F8E4E5", "OFFSET", "SBSQL-D9EDA8F8E4E5"},
    {"SBSQL-C287FF004431", "GROUP", "SBSQL-C287FF004431"},
    {"SBSQL-35ABB1D5392D", "FETCH", "SBSQL-35ABB1D5392D"},
    {"SBSQL-91BFC12AD088", "HAVING", "SBSQL-91BFC12AD088"},
    {"SBSQL-B18D3DA70CBB", "JOIN", "SBSQL-B18D3DA70CBB"},
    {"SBSQL-510838831CA8", "INSERT", "SBSQL-510838831CA8"},
    {"SBSQL-C13B63A81BCC", "UPDATE", "SBSQL-C13B63A81BCC"},
    {"SBSQL-5B33579F1F80", "DELETE", "SBSQL-5B33579F1F80"},
    {"SBSQL-B4135CC511C3", "MERGE", "SBSQL-B4135CC511C3"},
}};

constexpr std::array<DmlSurfaceEvidence, 20> kDmlSurfaceRows{{
    {"SBSQL-267688D774AF",
     "select",
     "canonical_surface",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-07971D942DFF"},
    {"SBSQL-ABCDC5A9E27A",
     "select_statement",
     "grammar_production",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-7B465418A763"},
    {"SBSQL-3ED0C0C1F6C7",
     "subquery_expression",
     "grammar_production",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-9807DC715DD5"},
    {"SBSQL-7A82912DB96E",
     "order_by_clause",
     "grammar_production",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-32D0F634B7C7"},
    {"SBSQL-39BB51C9872F",
     "limit_offset_clause",
     "grammar_production",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-7131DBB5F1D6"},
    {"SBSQL-BDF73B0A3735",
     "join_op",
     "grammar_production",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-468486B3700F"},
    {"SBSQL-FE045D17DF14",
     "join_condition",
     "grammar_production",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-DF998C7675E5"},
    {"SBSQL-BC2625A7F9D0",
     "insert",
     "canonical_surface",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-35160C9B863A"},
    {"SBSQL-8BF484ED5D20",
     "insert_statement",
     "grammar_production",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-ECB149557A44"},
    {"SBSQL-FC67CA158753",
     "insert_source",
     "grammar_production",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-3E477F2FEB79"},
    {"SBSQL-CCC63596E92A",
     "update",
     "canonical_surface",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-E97A73516C39"},
    {"SBSQL-63C1855AC02B",
     "update_statement",
     "grammar_production",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-996C54248275"},
    {"SBSQL-258F31C7EF63",
     "delete",
     "canonical_surface",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-7D06786BBE09"},
    {"SBSQL-DE3D1122AC55",
     "delete_statement",
     "grammar_production",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-86D79593C245"},
    {"SBSQL-6C4B02DAE3FF",
     "merge",
     "canonical_surface",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-9817D90E9773"},
    {"SBSQL-CC62E72012F0",
     "merge_statement",
     "grammar_production",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-7B081C1DEC0F"},
    {"SBSQL-02DAB8CFB3C0",
     "upsert",
     "canonical_surface",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-DDFA9F6A8BE3"},
    {"SBSQL-755DEDA239C0",
     "upsert_statement",
     "grammar_production",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-C5C052AFD834"},
    {"SBSQL-465931ED7427",
     "copy_import_export",
     "canonical_surface",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-768FBB82C44C"},
    {"SBSQL-4F912014EA85",
     "copy_statement",
     "grammar_production",
     "dml",
     "sblr.dml.operation.v3",
     "parser.statement_family.dml",
     "lowering.sblr_family.sblr_dml_operation_v3",
     "server.admission.sblr_dml_operation_v3",
     "engine.rule.sblr_dml_operation_v3",
     "SBSQL-SURFACE-37F06D1A7B77"},
}};

constexpr DmlSurfaceEvidence kBoundedCopySourceRow{
    "SBSQL-D19FE1151601",
    "copy_source",
    "grammar_production",
    "dml",
    "sblr.dml.operation.v3",
    "parser.statement_family.dml",
    "lowering.sblr_family.sblr_dml_operation_v3",
    "server.admission.sblr_dml_operation_v3",
    "engine.rule.sblr_dml_operation_v3",
    "SBSQL-SURFACE-E6D2FBF98472"};

constexpr DmlSurfaceEvidence kBoundedCopyFormatRow{
    "SBSQL-2DDA6BFD9B65",
    "copy_format",
    "grammar_production",
    "dml",
    "sblr.dml.operation.v3",
    "parser.statement_family.dml",
    "lowering.sblr_family.sblr_dml_operation_v3",
    "server.admission.sblr_dml_operation_v3",
    "engine.rule.sblr_dml_operation_v3",
    "SBSQL-SURFACE-B2541690AE45"};

constexpr DmlSurfaceEvidence kBoundedCopyOptionsRow{
    "SBSQL-4369855D2FC4",
    "copy_options",
    "grammar_production",
    "dml",
    "sblr.dml.operation.v3",
    "parser.statement_family.dml",
    "lowering.sblr_family.sblr_dml_operation_v3",
    "server.admission.sblr_dml_operation_v3",
    "engine.rule.sblr_dml_operation_v3",
    "SBSQL-SURFACE-C6DEAB5F3512"};

constexpr DmlSurfaceEvidence kBoundedCopyEndpointRow{
    "SBSQL-BDC2B64DA2A9",
    "copy_endpoint",
    "grammar_production",
    "dml",
    "sblr.dml.operation.v3",
    "parser.statement_family.dml",
    "lowering.sblr_family.sblr_dml_operation_v3",
    "server.admission.sblr_dml_operation_v3",
    "engine.rule.sblr_dml_operation_v3",
    "SBSQL-SURFACE-3E4A9E8BADBF"};

constexpr std::array<OrderedSelectClauseEvidence, 3> kBoundedOrderedSelectClauseRows{{
    {"SBSQL-7A82912DB96E",
     "order_by_clause",
     "query",
     "sblr.query.relational.v3",
     "SBSQL-SURFACE-32D0F634B7C7",
     "\"order_by\":\"id\"",
     "\"order_direction\":\"desc\"",
     "\"ordering_binding_model\":\"engine_row_descriptor_field\""},
    {"SBSQL-7A97866422E3",
     "sort_spec",
     "general",
     "sblr.general.operation.v3",
     "SBSQL-SURFACE-8D5E0FF8E35E",
     "\"order_by\":\"id\"",
     "\"order_direction\":\"desc\"",
     "\"ordering_binding_model\":\"engine_row_descriptor_field\""},
    {"SBSQL-39BB51C9872F",
     "limit_offset_clause",
     "query",
     "sblr.query.relational.v3",
     "SBSQL-SURFACE-7131DBB5F1D6",
     "\"limit\":\"2\"",
     "\"offset\":\"1\"",
     "\"ordering_binding_model\":\"engine_row_descriptor_field\""},
}};

constexpr TopClauseGrammarEvidence kBoundedTopClauseRow{
    "SBSQL-BDFA93BADC7C",
    "top_clause",
    "SBSQL-SURFACE-9E74D9A05B5C"};

constexpr std::array<GroupByGrammarEvidence, 3> kBoundedGroupByGrammarRows{{
    {"SBSQL-7A9BAFF959F7",
     "group_by_clause",
     "SBSQL-SURFACE-C2FAB960D6D1",
     "group_by_clause"},
    {"SBSQL-516ED93EC9EF",
     "group_by_list",
     "SBSQL-SURFACE-8366653DBCD1",
     "group_by_list"},
    {"SBSQL-E60597ED7F40",
     "group_by_item",
     "SBSQL-SURFACE-2CB0B287DC92",
     "group_by_item"},
}};

constexpr HavingClauseEvidence kBoundedHavingClauseRow{
    "SBSQL-6769CF2BD71C",
    "having_clause",
    "SBSQL-SURFACE-CC22626E33FD",
    "SELECT id, SUM(total) FROM customer GROUP BY id HAVING SUM(total) > 1"};

constexpr std::array<CteGrammarEvidence, 2> kBoundedMaterializedCteGrammarRows{{
    {"SBSQL-4B6E0DFBB334",
     "with_clause",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-E484A658F166",
     "with_clause"},
    {"SBSQL-7B9D9B2C828B",
     "cte_def",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3",
     "SBSQL-SURFACE-86F042A32EDF",
     "cte_def"},
}};

constexpr std::array<WindowClauseGrammarEvidence, 3> kBoundedWindowClauseGrammarRows{{
    {"SBSQL-1026B4E68CA7",
     "over_clause",
     "SBSQL-SURFACE-B36CE3C8A992",
     "SELECT row_number() OVER (ORDER BY id) FROM customer",
     "\"query_envelope_kind\":\"table_row_number_window\"",
     "\"order_by\":\"id\""},
    {"SBSQL-120A1B113995",
     "window_partition_clause",
     "SBSQL-SURFACE-1988CB5CE3A5",
     "SELECT count(*) OVER (PARTITION BY dept) FROM sales",
     "\"query_envelope_kind\":\"table_partition_count_window\"",
     "\"partition_by\":\"dept\""},
    {"SBSQL-479F12CDBDD7",
     "window_spec",
     "SBSQL-SURFACE-E4AE11394C04",
     "SELECT row_number() OVER (ORDER BY id) FROM customer",
     "\"query_envelope_kind\":\"table_row_number_window\"",
     "\"window_binding_model\":\"engine_row_descriptor_field_int64_current_route\""},
}};

constexpr std::array<WhereClauseGrammarEvidence, 4> kBoundedWhereEqualityPredicateRows{{
    {"SBSQL-FDDEDDE219FF",
     "where_clause",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-DC5ABDE2B3A2"},
    {"SBSQL-C599A11E2743",
     "comparison_expr",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3",
     "SBSQL-SURFACE-B51453AA5423"},
    {"SBSQL-BD4C48607AA5",
     "comparison_op",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3",
     "SBSQL-SURFACE-974A4CAD4A35"},
    {"SBSQL-75C314C8812A",
     "column_ref",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3",
     "SBSQL-SURFACE-36FDE4CB9CA8"},
}};

constexpr FetchClauseGrammarEvidence kBoundedFetchClauseRow{
    "SBSQL-E3899FC39487",
    "fetch_clause",
    "SBSQL-SURFACE-D743853A9555"};

constexpr std::array<SimpleSelectSkeletonEvidence, 9> kBoundedSimpleSelectSkeletonRows{{
    {"SBSQL-B088E6B465FB",
     "query_dml_stmt",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-C84E43D80132",
     "query_dml_stmt"},
    {"SBSQL-D229BF16AB18",
     "query_expression",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-76979B964EAA",
     "query_expression"},
    {"SBSQL-69FF4A7219CE",
     "query_contract",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-FFD8D0D316ED",
     "query_contract"},
    {"SBSQL-1CEA5265AEBF",
     "query_term",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-E60FF0C0CCE0",
     "query_term"},
    {"SBSQL-3A43CC647BE1",
     "select_list",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-DFCD9AC4379D",
     "select_list"},
    {"SBSQL-287ABE0E2158",
     "select_item",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-6B0041676E29",
     "select_item"},
    {"SBSQL-F63161605D67",
     "from_clause",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3",
     "SBSQL-SURFACE-F50C9DDA81F2",
     "from_clause"},
    {"SBSQL-E8662E29C028",
     "table_primary",
     "query",
     "sblr.query.relational.v3",
     "parser.statement_family.query",
     "lowering.sblr_family.sblr_query_relational_v3",
     "server.admission.sblr_query_relational_v3",
     "engine.rule.sblr_query_relational_v3",
     "SBSQL-SURFACE-E48BA5909F1E",
     "table_primary"},
    {"SBSQL-41BE7F0451CF",
     "table_reference",
     "ddl_catalog",
     "sblr.catalog.mutation.v3",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3",
     "SBSQL-SURFACE-A3925B282ABD",
     "table_reference"},
}};

std::string EvidenceMessage(const DmlSurfaceEvidence& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string OrderedSelectEvidenceMessage(const OrderedSelectClauseEvidence& row,
                                         std::string_view phase,
                                         std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string GroupByEvidenceMessage(const GroupByGrammarEvidence& row,
                                   std::string_view phase,
                                   std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string HavingClauseEvidenceMessage(const HavingClauseEvidence& row,
                                        std::string_view phase,
                                        std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string CteEvidenceMessage(const CteGrammarEvidence& row,
                               std::string_view phase,
                               std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string WindowClauseEvidenceMessage(const WindowClauseGrammarEvidence& row,
                                        std::string_view phase,
                                        std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string WhereClauseEvidenceMessage(const WhereClauseGrammarEvidence& row,
                                       std::string_view phase,
                                       std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string FetchClauseEvidenceMessage(const FetchClauseGrammarEvidence& row,
                                       std::string_view phase,
                                       std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string TopClauseEvidenceMessage(const TopClauseGrammarEvidence& row,
                                     std::string_view phase,
                                     std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string SimpleSelectSkeletonEvidenceMessage(const SimpleSelectSkeletonEvidence& row,
                                                std::string_view phase,
                                                std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_dml_exact_route_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_dml_exact_route_conformance");
  Require(configured.ok(), "DML exact-route memory fixture configuration failed");
  Require(configured.fixture_mode,
          "DML exact-route memory fixture mode was not active");
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

std::string_view ExpectedServerAdmissionFamily(std::string_view operation_id,
                                               std::string_view envelope_family) {
  if (operation_id == "dml.insert_rows") return "sblr.dml.insert.v3";
  if (operation_id == "dml.update_rows") return "sblr.dml.update.v3";
  if (operation_id == "dml.delete_rows") return "sblr.dml.delete.v3";
  if (operation_id == "dml.merge_rows") return "sblr.dml.merge.v3";
  if (operation_id == "dml.plan_import_rows" ||
      operation_id == "dml.execute_import_rows" ||
      operation_id == "dml.execute_native_bulk_ingest" ||
      operation_id == "dml.normalize_import_checkpoint_model" ||
      operation_id == "dml.normalize_import_reject_model") {
    return "sblr.bulk.import.v3";
  }
  return envelope_family;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool DiagnosticsContain(const MessageVectorSet& messages, std::string_view needle) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (Contains(diagnostic.code, needle) ||
        Contains(diagnostic.message, needle)) {
      return true;
    }
    for (const auto& field : diagnostic.fields) {
      if (Contains(field.name, needle) || Contains(field.value, needle)) {
        return true;
      }
    }
  }
  return false;
}

void PrintMessageSet(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.name << '=' << field.value << '\n';
    }
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000002111";
  session.connection_uuid = "019f0000-0000-7000-8000-000000002112";
  session.database_uuid = "019f0000-0000-7000-8000-000000002113";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000002114";
  config.bundle_contract_id = "sbp_sbsql@dml-route-test";
  config.build_id = "sbsql-dml-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql, std::vector<std::string> resolved = {}) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, resolved);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  static const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("sbsql_dml_exact_route_conformance_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
  return path;
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock",
                            ".sb.mga_row_versions",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_index_entries",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_large_values",
                            ".sb.mga_savepoints"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabaseForEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779821600000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779821600001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779821600002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DML exact-route engine dispatch database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContextForDatabase(const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-dml-exact-route";
  context.database_path = TestDatabasePath().string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000002122";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000002123";
  context.security_context_present = true;
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:DML_ROUTE_TEST");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::string& database_uuid) {
  auto context = EngineContextForDatabase(database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.dml.exact_route.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "transaction begin envelope did not validate");
  Require(result.accepted, "transaction begin dispatch did not accept");
  Require(result.api_result.ok, "transaction begin did not return success");
  Require(result.api_result.local_transaction_id != 0,
          "transaction begin did not return local transaction id");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  return context;
}

api::EngineApiRequest EngineCreateCustomerTableApiRequest() {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "table";
  request.localized_names.push_back({"en", "primary", "", "customer", true});

  api::EngineColumnDefinition id_column;
  id_column.requested_column_uuid.canonical = std::string(kColumnIdUuid);
  id_column.names.push_back({"en", "primary", "", "id", true});
  id_column.descriptor.descriptor_kind = "scalar";
  id_column.descriptor.canonical_type_name = "text";
  id_column.descriptor.encoded_descriptor = "type=text";
  id_column.ordinal = 0;
  id_column.nullable = true;
  request.columns.push_back(std::move(id_column));

  api::EngineColumnDefinition name_column;
  name_column.requested_column_uuid.canonical = std::string(kColumnNameUuid);
  name_column.names.push_back({"en", "primary", "", "name", true});
  name_column.descriptor.descriptor_kind = "scalar";
  name_column.descriptor.canonical_type_name = "text";
  name_column.descriptor.encoded_descriptor = "type=text";
  name_column.ordinal = 1;
  name_column.nullable = true;
  request.columns.push_back(std::move(name_column));
  return request;
}

api::EngineApiRequest EngineCreateSchemaApiRequest() {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kSchemaUuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back({"en", "primary", "", "dml_exact_route", true});
  return request;
}

void PrepareEngineDispatchContext() {
  const auto database_uuid = CreateMinimalDatabaseForEngineDispatch();
  auto context = BeginEngineTransaction(database_uuid);
  auto schema_envelope = sblr::MakeSblrEnvelope("ddl.create_schema",
                                                "SBLR_DDL_CREATE_SCHEMA",
                                                "trace.dml.exact_route.seed_schema");
  schema_envelope.requires_security_context = true;
  schema_envelope.requires_transaction_context = true;
  schema_envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest schema_request{
      context,
      schema_envelope,
      EngineCreateSchemaApiRequest()};
  const auto schema_result = sblr::DispatchSblrOperation(schema_request);
  for (const auto& diagnostic : schema_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : schema_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(schema_result.envelope_validated, "seed schema envelope did not validate");
  Require(schema_result.accepted, "seed schema dispatch did not accept");
  Require(schema_result.dispatched_to_api, "seed schema dispatch did not route to engine API");
  Require(schema_result.api_result.ok, "seed schema create did not return success");

  auto envelope = sblr::MakeSblrEnvelope("ddl.create_table",
                                         "SBLR_DDL_CREATE_TABLE",
                                         "trace.dml.exact_route.seed_table");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{
      context,
      envelope,
      EngineCreateCustomerTableApiRequest()};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "seed table envelope did not validate");
  Require(result.accepted, "seed table dispatch did not accept");
  Require(result.dispatched_to_api, "seed table dispatch did not route to engine API");
  Require(result.api_result.ok, "seed table create did not return success");
  g_engine_context = context;
  g_engine_context_ready = true;
}

void RequireRegistryEvidence(const DmlSurfaceEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == row.surface_kind,
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == row.family,
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == row.sblr_operation_family,
          EvidenceMessage(row, "registry", "SBLR operation family mismatch"));
  Require(registry_row->parser_handler_key == row.parser_handler_key,
          EvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key == row.lowering_handler_key,
          EvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key == row.server_admission_key,
          EvidenceMessage(row, "server_admission", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == row.engine_rule_key,
          EvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireRegistryEvidence() {
  for (const auto& row : kDmlSurfaceRows) {
    RequireRegistryEvidence(row);
  }
}

void RequireContextualKeywordRegistryEvidence() {
  for (const auto& row : kDmlContextualKeywordRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "DML contextual keyword registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "DML contextual keyword canonical name drift");
    Require(registry_row->surface_kind == "function",
            "DML contextual keyword surface kind drift");
    Require(registry_row->source_status == "native_now",
            "DML contextual keyword source status drift");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "DML contextual keyword cluster scope drift");
    Require(registry_row->sblr_operation_family == "sblr.expression.runtime.v3",
            "DML contextual keyword SBLR family drift");
  }
}

void RequireGroupByGrammarRegistryEvidence(const GroupByGrammarEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          GroupByEvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          GroupByEvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          GroupByEvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "query",
          GroupByEvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          GroupByEvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          GroupByEvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.query.relational.v3",
          GroupByEvidenceMessage(row, "registry", "SBLR operation family mismatch"));
  Require(registry_row->parser_handler_key == "parser.statement_family.query",
          GroupByEvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key == "lowering.sblr_family.sblr_query_relational_v3",
          GroupByEvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key == "server.admission.sblr_query_relational_v3",
          GroupByEvidenceMessage(row, "server_admission", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_query_relational_v3",
          GroupByEvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          GroupByEvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireHavingClauseRegistryEvidence(const HavingClauseEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          HavingClauseEvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          HavingClauseEvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          HavingClauseEvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "general",
          HavingClauseEvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          HavingClauseEvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          HavingClauseEvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          HavingClauseEvidenceMessage(row, "registry", "canonical SBLR family mismatch"));
  Require(registry_row->parser_handler_key == "parser.grammar_ast",
          HavingClauseEvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          HavingClauseEvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key == "server.admission.sblr_general_operation_v3",
          HavingClauseEvidenceMessage(row, "server_admission", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          HavingClauseEvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          HavingClauseEvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireWindowClauseGrammarRegistryEvidence(const WindowClauseGrammarEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          WindowClauseEvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          WindowClauseEvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          WindowClauseEvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "general",
          WindowClauseEvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          WindowClauseEvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          WindowClauseEvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          WindowClauseEvidenceMessage(row, "registry", "canonical SBLR family mismatch"));
  Require(registry_row->parser_handler_key == "parser.grammar_ast",
          WindowClauseEvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          WindowClauseEvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key == "server.admission.sblr_general_operation_v3",
          WindowClauseEvidenceMessage(row, "server_admission", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          WindowClauseEvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          WindowClauseEvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireExactLowering(std::string_view sql,
                          std::string_view operation_family,
                          std::string_view operation_id,
                          std::string_view opcode,
                          std::string_view required_right,
                          std::string_view surface_variant) {
  std::vector<std::string> resolved{std::string(kTargetUuid)};
  if (operation_id == "dml.merge_rows") {
    resolved.push_back(std::string(kRelatedUuid));
  }
  const auto artifacts = RunPipeline(sql, resolved);
  Require(artifacts.bound.bound, "DML/query statement did not bind after UUID resolution");
  Require(artifacts.verifier.admitted, "DML/query SBLR verifier rejected exact route");
  Require(artifacts.envelope.operation_family == operation_family,
          "DML/query operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == operation_family,
          "DML/query SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == operation_id, "DML/query operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == operation_id,
          "DML/query engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == opcode, "DML/query opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, required_right),
          "DML/query required right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.dml_api_required"),
          "engine DML authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          "DML/query transaction authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_security_authorization"),
          "parser no-security-authorization authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "parser no-storage/finality authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.catalog.object_descriptor"),
          "DML/query object descriptor ref missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.storage.row_descriptor"),
          "DML/query row descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, "\"dml_envelope_kind\""),
          "DML/query payload missing DML envelope marker");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "DML/query target UUID missing from payload");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"dml_surface_variant\":\"") + std::string(surface_variant) + "\""),
          "DML/query surface variant missing from payload");
  if (operation_id == "dml.plan_import_rows") {
    Require(Contains(artifacts.envelope.payload, "\"import_execution_deferred\":true"),
            "COPY/import planning payload did not defer execution");
    Require(Contains(artifacts.envelope.payload,
                     "\"mga_access_model\":\"import_plan_no_row_write\""),
            "COPY/import planning payload claimed row-write MGA access");
    Require(Contains(artifacts.envelope.payload,
                     "\"source_kind\":\"native_sbsql_import\""),
            "COPY/import planning payload source kind mismatch");
    Require(Contains(artifacts.envelope.payload, "\"format_family\":\"csv\""),
            "COPY/import planning payload format family mismatch");
    Require(Contains(artifacts.envelope.payload, "\"source_handle_included\":false"),
            "COPY/import planning payload included a source handle");
    Require(Contains(artifacts.envelope.payload, "\"row_persistence_claimed\":false"),
            "COPY/import planning payload claimed row persistence");
    Require(Contains(artifacts.envelope.payload, "\"parser_decodes_bytes\":false"),
            "COPY/import planning payload claimed parser byte decoding");
  }
  Require(!Contains(artifacts.envelope.payload, sql),
          "DML/query envelope embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "DML/query envelope embedded target name text");
  Require(!Contains(artifacts.envelope.payload, "staging"),
          "DML/query envelope embedded source name text");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "DML/query envelope embedded source_text");
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":true"),
          "DML/query envelope marked SQL text present");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << field.key << '=' << field.value << '\n';
    }
  }
  Require(admission.admitted, "server admission rejected exact DML/query route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require engine public ABI dispatch for DML/query");
  Require(admission.operation_id == operation_id, "server admission operation id mismatch");
  const auto expected_admission_family =
      ExpectedServerAdmissionFamily(operation_id, operation_family);
  if (admission.operation_family != expected_admission_family) {
    std::cerr << "server admission operation family mismatch sql=" << sql
              << " expected=" << expected_admission_family
              << " actual=" << admission.operation_family
              << " operation_id=" << operation_id << '\n';
  }
  Require(admission.operation_family == expected_admission_family,
          "server admission operation family mismatch");
}

void RequireCopySourceExactRouteEvidence() {
  RequireRegistryEvidence(kBoundedCopySourceRow);

  constexpr std::string_view kCopySourceSql = "COPY customer FROM STDIN";
  const auto artifacts = RunPipeline(kCopySourceSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound,
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source route did not bind after UUID resolution"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source SBLR verifier rejected import planning route"));
  Require(artifacts.envelope.operation_family == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_PLAN_IMPORT_ROWS",
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_rights, "right.write"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source required write right missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.import_planning_api_required"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source import planning authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source parser no-storage/finality authority step missing"));
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source target UUID missing from payload"));
  Require(Contains(artifacts.envelope.payload,
                   "\"dml_surface_variant\":\"copy_import_export\""),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source surface variant mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"import_execution_deferred\":true"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source did not defer import execution"));
  Require(Contains(artifacts.envelope.payload,
                   "\"mga_access_model\":\"import_plan_no_row_write\""),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source claimed row-write MGA access"));
  Require(Contains(artifacts.envelope.payload,
                   "\"source_kind\":\"native_sbsql_import\""),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source source_kind mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"format_family\":\"csv\""),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source format_family mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"source_handle_included\":false"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source included source handle authority"));
  Require(Contains(artifacts.envelope.payload, "\"parser_decodes_bytes\":false"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source claimed parser byte decoding"));
  Require(Contains(artifacts.envelope.payload, "\"row_persistence_claimed\":false"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source claimed row persistence"));
  Require(Contains(artifacts.envelope.payload, "\"parser_authorizes\":false"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source claimed parser authorization"));
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source claimed object-name text authority"));
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source claimed SQL text authority"));
  Require(!Contains(artifacts.envelope.payload, kCopySourceSql),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source embedded source SQL text"));
  Require(!Contains(artifacts.envelope.payload, "customer"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source embedded target object-name text"));
  Require(!Contains(artifacts.envelope.payload, "STDIN"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source embedded source endpoint text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source embedded source_text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_object_name\""),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source embedded source object-name text"));
  Require(!Contains(artifacts.envelope.payload, "\"sql\":"),
          EvidenceMessage(kBoundedCopySourceRow, "parser_bind_lower",
                          "COPY source embedded SQL text field"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << field.key << '=' << field.value << '\n';
    }
  }
  Require(admission.admitted,
          EvidenceMessage(kBoundedCopySourceRow, "server_admission",
                          "server admission rejected COPY source route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(kBoundedCopySourceRow, "server_admission",
                          "server admission did not require public ABI dispatch"));
  Require(admission.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopySourceRow, "server_admission",
                          "server admission operation id mismatch"));
  Require(admission.operation_family ==
              ExpectedServerAdmissionFamily(admission.operation_id,
                                            artifacts.envelope.operation_family),
          EvidenceMessage(kBoundedCopySourceRow, "server_admission",
                          "server admission operation family mismatch"));
}

void RequireCopyFormatExactRouteEvidence() {
  RequireRegistryEvidence(kBoundedCopyFormatRow);

  constexpr std::string_view kCopyFormatSql = "COPY customer FROM STDIN JSONL";
  const auto artifacts = RunPipeline(kCopyFormatSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound,
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format route did not bind after UUID resolution"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format SBLR verifier rejected import planning route"));
  Require(artifacts.envelope.operation_family == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_PLAN_IMPORT_ROWS",
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_rights, "right.write"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format required write right missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.import_planning_api_required"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format import planning authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format parser no-storage/finality authority step missing"));
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format target UUID missing from payload"));
  Require(Contains(artifacts.envelope.payload,
                   "\"dml_surface_variant\":\"copy_import_export\""),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format surface variant mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"import_execution_deferred\":true"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format did not defer import execution"));
  Require(Contains(artifacts.envelope.payload,
                   "\"mga_access_model\":\"import_plan_no_row_write\""),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format claimed row-write MGA access"));
  Require(Contains(artifacts.envelope.payload,
                   "\"source_kind\":\"native_sbsql_import\""),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format source_kind mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"format_family\":\"jsonl\""),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format_family mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"source_handle_included\":false"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format included source handle authority"));
  Require(Contains(artifacts.envelope.payload, "\"parser_decodes_bytes\":false"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format claimed parser byte decoding"));
  Require(Contains(artifacts.envelope.payload, "\"row_persistence_claimed\":false"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format claimed row persistence"));
  Require(Contains(artifacts.envelope.payload, "\"parser_authorizes\":false"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format claimed parser authorization"));
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format claimed object-name text authority"));
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format claimed SQL text authority"));
  Require(!Contains(artifacts.envelope.payload, kCopyFormatSql),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format embedded source SQL text"));
  Require(!Contains(artifacts.envelope.payload, "customer"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format embedded target object-name text"));
  Require(!Contains(artifacts.envelope.payload, "STDIN"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format embedded source endpoint text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format embedded source_text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_object_name\""),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format embedded source object-name text"));
  Require(!Contains(artifacts.envelope.payload, "\"sql\":"),
          EvidenceMessage(kBoundedCopyFormatRow, "parser_bind_lower",
                          "COPY format embedded SQL text field"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << field.key << '=' << field.value << '\n';
    }
  }
  Require(admission.admitted,
          EvidenceMessage(kBoundedCopyFormatRow, "server_admission",
                          "server admission rejected COPY format route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(kBoundedCopyFormatRow, "server_admission",
                          "server admission did not require public ABI dispatch"));
  Require(admission.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyFormatRow, "server_admission",
                          "server admission operation id mismatch"));
  Require(admission.operation_family ==
              ExpectedServerAdmissionFamily(admission.operation_id,
                                            artifacts.envelope.operation_family),
          EvidenceMessage(kBoundedCopyFormatRow, "server_admission",
                          "server admission operation family mismatch"));
}

void RequireCopyOptionsExactRouteEvidence() {
  RequireRegistryEvidence(kBoundedCopyOptionsRow);

  constexpr std::string_view kCopyOptionsSql = "COPY customer FROM STDIN WITH HEADER";
  const auto artifacts = RunPipeline(kCopyOptionsSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound,
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options route did not bind after UUID resolution"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options SBLR verifier rejected import planning route"));
  Require(artifacts.envelope.operation_family == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_PLAN_IMPORT_ROWS",
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_rights, "right.write"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options required write right missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.import_planning_api_required"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options import planning authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options parser no-storage/finality authority step missing"));
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options target UUID missing from payload"));
  Require(Contains(artifacts.envelope.payload,
                   "\"dml_surface_variant\":\"copy_import_export\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options surface variant mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"import_execution_deferred\":true"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options did not defer import execution"));
  Require(Contains(artifacts.envelope.payload,
                   "\"mga_access_model\":\"import_plan_no_row_write\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options claimed row-write MGA access"));
  Require(Contains(artifacts.envelope.payload,
                   "\"source_kind\":\"native_sbsql_import\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options source_kind mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"format_family\":\"csv\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options format_family mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"copy_options_present\":true"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options marker missing"));
  Require(Contains(artifacts.envelope.payload, "\"copy_header_option\":true"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY HEADER option marker missing"));
  Require(Contains(artifacts.envelope.payload, "\"source_handle_included\":false"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options included source handle authority"));
  Require(Contains(artifacts.envelope.payload, "\"parser_decodes_bytes\":false"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options claimed parser byte decoding"));
  Require(Contains(artifacts.envelope.payload, "\"row_persistence_claimed\":false"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options claimed row persistence"));
  Require(Contains(artifacts.envelope.payload, "\"parser_authorizes\":false"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options claimed parser authorization"));
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options claimed object-name text authority"));
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options claimed SQL text authority"));
  Require(!Contains(artifacts.envelope.payload, kCopyOptionsSql),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded source SQL text"));
  Require(!Contains(artifacts.envelope.payload, "customer"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded target object-name text"));
  Require(!Contains(artifacts.envelope.payload, "STDIN"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded source endpoint text"));
  Require(!Contains(artifacts.envelope.payload, "HEADER"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded source option text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded source_text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_handle\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded source handle authority"));
  Require(!Contains(artifacts.envelope.payload, "\"source_object_name\""),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded source object-name text"));
  Require(!Contains(artifacts.envelope.payload, "\"sql\":"),
          EvidenceMessage(kBoundedCopyOptionsRow, "parser_bind_lower",
                          "COPY options embedded SQL text field"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << field.key << '=' << field.value << '\n';
    }
  }
  Require(admission.admitted,
          EvidenceMessage(kBoundedCopyOptionsRow, "server_admission",
                          "server admission rejected COPY options route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(kBoundedCopyOptionsRow, "server_admission",
                          "server admission did not require public ABI dispatch"));
  Require(admission.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyOptionsRow, "server_admission",
                          "server admission operation id mismatch"));
  Require(admission.operation_family ==
              ExpectedServerAdmissionFamily(admission.operation_id,
                                            artifacts.envelope.operation_family),
          EvidenceMessage(kBoundedCopyOptionsRow, "server_admission",
                          "server admission operation family mismatch"));
}

void RequireCopyEndpointExactRouteEvidence() {
  RequireRegistryEvidence(kBoundedCopyEndpointRow);

  constexpr std::string_view kCopyEndpointSql = "COPY customer FROM STDIN";
  const auto artifacts = RunPipeline(kCopyEndpointSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound,
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint route did not bind after UUID resolution"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint SBLR verifier rejected import planning route"));
  Require(artifacts.envelope.operation_family == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.dml.operation.v3",
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_PLAN_IMPORT_ROWS",
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_rights, "right.write"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint required write right missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.import_planning_api_required"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint import planning authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint parser no-storage/finality authority step missing"));
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint target UUID missing from payload"));
  Require(Contains(artifacts.envelope.payload,
                   "\"dml_surface_variant\":\"copy_import_export\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint surface variant mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"import_execution_deferred\":true"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint did not defer import execution"));
  Require(Contains(artifacts.envelope.payload,
                   "\"mga_access_model\":\"import_plan_no_row_write\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint claimed row-write MGA access"));
  Require(Contains(artifacts.envelope.payload,
                   "\"source_kind\":\"native_sbsql_import\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint source_kind mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"format_family\":\"csv\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint format_family mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"source_handle_included\":false"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint included source handle authority"));
  Require(Contains(artifacts.envelope.payload, "\"parser_decodes_bytes\":false"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint claimed parser byte decoding"));
  Require(Contains(artifacts.envelope.payload, "\"row_persistence_claimed\":false"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint claimed row persistence"));
  Require(Contains(artifacts.envelope.payload, "\"parser_authorizes\":false"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint claimed parser authorization"));
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint claimed object-name text authority"));
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint claimed SQL text authority"));
  Require(!Contains(artifacts.envelope.payload, kCopyEndpointSql),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint embedded source SQL text"));
  Require(!Contains(artifacts.envelope.payload, "customer"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint embedded target object-name text"));
  Require(!Contains(artifacts.envelope.payload, "STDIN"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint embedded source endpoint text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint embedded source_text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_handle\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint embedded source handle authority"));
  Require(!Contains(artifacts.envelope.payload, "\"source_object_name\""),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint embedded source object-name text"));
  Require(!Contains(artifacts.envelope.payload, "\"sql\":"),
          EvidenceMessage(kBoundedCopyEndpointRow, "parser_bind_lower",
                          "COPY endpoint embedded SQL text field"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << field.key << '=' << field.value << '\n';
    }
  }
  Require(admission.admitted,
          EvidenceMessage(kBoundedCopyEndpointRow, "server_admission",
                          "server admission rejected COPY endpoint route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(kBoundedCopyEndpointRow, "server_admission",
                          "server admission did not require public ABI dispatch"));
  Require(admission.operation_id == "dml.plan_import_rows",
          EvidenceMessage(kBoundedCopyEndpointRow, "server_admission",
                          "server admission operation id mismatch"));
  Require(admission.operation_family ==
              ExpectedServerAdmissionFamily(admission.operation_id,
                                            artifacts.envelope.operation_family),
          EvidenceMessage(kBoundedCopyEndpointRow, "server_admission",
                          "server admission operation family mismatch"));
}

api::EngineRequestContext EngineContext() {
  Require(g_engine_context_ready, "engine dispatch context was not prepared");
  return g_engine_context;
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string operation_id, std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode),
                                         "trace.dml.exact_route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "table"});
  return envelope;
}

api::EngineApiRequest EngineApiRequestForDml(std::string_view operation_id) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "table";
  if (operation_id == "dml.select_rows") {
    request.predicate.predicate_kind = "column_equals";
    request.predicate.canonical_predicate_envelope = "id";
    api::EngineTypedValue value;
    value.descriptor.descriptor_kind = "scalar";
    value.descriptor.canonical_type_name = "text";
    value.descriptor.encoded_descriptor = "type=text";
    value.encoded_value = "1";
    request.predicate.bound_values.push_back(std::move(value));
  }
  if (operation_id == "dml.insert_rows" || operation_id == "dml.merge_rows") {
    api::EngineRowValue row;
    row.requested_row_uuid.canonical = "019f0000-0000-7000-8000-000000002125";
    api::EngineTypedValue value;
    value.encoded_value = "1";
    row.fields.push_back({"id", value});
    request.rows.push_back(row);
  }
  if (operation_id == "dml.merge_rows") {
    request.predicate.predicate_kind = "row_uuid_match";
    request.predicate.canonical_predicate_envelope = "019f0000-0000-7000-8000-000000002125";
  }
  return request;
}

bool ApiDiagnosticContains(const api::EngineApiResult& result, std::string_view needle) {
  for (const auto& diagnostic : result.diagnostics) {
    if (Contains(diagnostic.code, needle) ||
        Contains(diagnostic.message_key, needle) ||
        Contains(diagnostic.detail, needle)) {
      return true;
    }
  }
  return false;
}

void RequireEngineDispatch(std::string operation_id, std::string opcode) {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelope(operation_id, opcode),
      EngineApiRequestForDml(operation_id)};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept DML/query operation");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to a DML API");
  Require(result.api_result.operation_id == operation_id,
          "engine SBLR dispatch returned wrong DML/query operation id");
  if (operation_id == "dml.plan_import_rows") {
    Require(result.api_result.ok, "engine import planning API did not accept canonical COPY plan");
  }
  Require(!ApiDiagnosticContains(result.api_result, "target_table_uuid_required"),
          "engine DML dispatch lost the UUID-bound target table");
  Require(!ApiDiagnosticContains(result.api_result, "source_table_uuid_required"),
          "engine query dispatch lost the UUID-bound source table");
}

void RequireInsertSourceExactRouteEvidence() {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById("SBSQL-FC67CA158753");
  Require(registry_row != nullptr,
          "SBSQL-FC67CA158753 insert_source generated registry row missing");
  Require(registry_row->canonical_name == "insert_source",
          "SBSQL-FC67CA158753 canonical name drift");
  Require(registry_row->surface_kind == "grammar_production",
          "SBSQL-FC67CA158753 surface kind drift");
  Require(registry_row->family == "dml", "SBSQL-FC67CA158753 family drift");
  Require(registry_row->sblr_operation_family == "sblr.dml.operation.v3",
          "SBSQL-FC67CA158753 SBLR family drift");

  const auto artifacts = RunPipeline("INSERT INTO customer VALUES (1)", {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "insert_source route did not bind after UUID resolution");
  Require(artifacts.verifier.admitted, "insert_source SBLR verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.dml.operation.v3",
          "insert_source operation family mismatch");
  Require(artifacts.envelope.operation_id == "dml.insert_rows",
          "insert_source operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_INSERT_ROWS",
          "insert_source opcode mismatch");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "insert_source target UUID missing from payload");
  Require(Contains(artifacts.envelope.payload, "\"dml_surface_variant\":\"insert\""),
          "insert_source payload missing bounded insert route marker");
  Require(!Contains(artifacts.envelope.payload, "INSERT INTO customer VALUES (1)"),
          "insert_source envelope embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "insert_source envelope embedded object-name text");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "insert_source envelope embedded source_text");
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":true"),
          "insert_source envelope marked SQL text present");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected insert_source exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require engine public ABI dispatch for insert_source");
  Require(admission.operation_id == "dml.insert_rows",
          "server admission insert_source operation id mismatch");

  RequireEngineDispatch("dml.insert_rows", "SBLR_DML_INSERT_ROWS");
}

void RequireInsertValuesKeywordStringLiteralEvidence() {
  const auto grants = RunPipeline(
      "INSERT INTO customer VALUES "
      "('grant_direct_select', 'user_direct', 'principal', 'object_direct', 'SELECT', FALSE, TRUE, 11), "
      "('grant_role_update', 'role_reporting', 'role', 'object_role', 'UPDATE', FALSE, TRUE, 11), "
      "('grant_security_reader', 'role_security_reader', 'role', 'object_security_catalog', 'VISIBLE', FALSE, TRUE, 11)",
      {std::string(kTargetUuid)});
  Require(grants.bound.bound, "keyword-string INSERT route did not bind");
  Require(grants.verifier.admitted, "keyword-string INSERT verifier rejected exact route");
  Require(grants.envelope.operation_id == "dml.insert_rows",
          std::string("keyword-string INSERT operation id mismatch: ") +
              grants.envelope.operation_id);
  Require(Contains(grants.envelope.payload, "\"insert_values_row_count\":\"3\""),
          "keyword-string INSERT payload missing row count");
  Require(Contains(grants.envelope.payload, "\"insert_values_column_count\":\"8\""),
          "keyword-string INSERT payload missing column count");
  Require(Contains(grants.envelope.payload, "\"insert_values_0_4_value\":\"SELECT\""),
          "keyword-string INSERT lost SELECT literal");
  Require(Contains(grants.envelope.payload, "\"insert_values_1_4_value\":\"UPDATE\""),
          "keyword-string INSERT lost UPDATE literal");
  Require(Contains(grants.envelope.payload, "\"insert_values_2_4_value\":\"VISIBLE\""),
          "keyword-string INSERT lost VISIBLE literal");
  Require(Contains(grants.envelope.payload, "\"insert_values_0_5_type\":\"boolean\""),
          "keyword-string INSERT lost boolean literal type");
  Require(Contains(grants.envelope.payload, "\"insert_values_0_7_type\":\"bigint\""),
          "keyword-string INSERT lost numeric literal type");

  const auto replay = RunPipeline(
      "INSERT INTO customer VALUES "
      "('case_replay_cross_user', 'user_nested', 'user_none', 'object_nested', 'SELECT', 'SECURITY.AUTHORIZATION.PRINCIPAL_MISMATCH'), "
      "('case_replay_role_change', 'user_role_nested', 'user_direct', 'object_role', 'UPDATE', 'SECURITY.AUTHORIZATION.PRINCIPAL_MISMATCH')",
      {std::string(kTargetUuid)});
  Require(replay.bound.bound, "diagnostic-string INSERT route did not bind");
  Require(replay.verifier.admitted, "diagnostic-string INSERT verifier rejected exact route");
  Require(replay.envelope.operation_id == "dml.insert_rows",
          "diagnostic-string INSERT operation id mismatch");
  Require(Contains(replay.envelope.payload, "\"insert_values_row_count\":\"2\""),
          "diagnostic-string INSERT payload missing row count");
  Require(Contains(replay.envelope.payload,
                   "\"insert_values_0_5_value\":\"SECURITY.AUTHORIZATION.PRINCIPAL_MISMATCH\""),
          "diagnostic-string INSERT lost refusal diagnostic literal");

  const auto replay_command_literal = RunPipeline(
      "INSERT INTO customer VALUES "
      "('SBSQL-SURFACE-671B00230945', 'SBSQL-E57785E2BD95', "
      "'SBSQL_SURFACE_REPLAY SBSQL-E57785E2BD95')",
      {std::string(kTargetUuid)});
  Require(replay_command_literal.bound.bound,
          "surface-replay command literal INSERT route did not bind");
  Require(replay_command_literal.verifier.admitted,
          "surface-replay command literal INSERT verifier rejected exact route");
  Require(replay_command_literal.envelope.operation_id == "dml.insert_rows",
          "surface-replay command literal INSERT operation id mismatch");
  Require(Contains(replay_command_literal.envelope.payload,
                   "\"insert_values_0_2_value\":\"SBSQL_SURFACE_REPLAY SBSQL-E57785E2BD95\""),
          "surface-replay command literal INSERT lost replay command literal");
  const auto replay_command_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{
          replay_command_literal.envelope.payload, false});
  for (const auto& diagnostic : replay_command_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(replay_command_admission.admitted,
          "server admission rejected surface-replay command literal INSERT");

  const auto quoted_system_variable_literal = RunPipeline(
      "INSERT INTO customer VALUES "
      "('SBSQL-SURFACE-F63A40BE0271', 'SBSQL-4798C99894E7', '@@tx_isolation')",
      {std::string(kTargetUuid)});
  Require(quoted_system_variable_literal.bound.bound,
          "quoted system-variable literal INSERT route did not bind");
  Require(quoted_system_variable_literal.verifier.admitted,
          "quoted system-variable literal INSERT verifier rejected exact route");
  Require(quoted_system_variable_literal.envelope.operation_id == "dml.insert_rows",
          "quoted system-variable literal INSERT operation id mismatch");
  Require(Contains(quoted_system_variable_literal.envelope.payload,
                   "\"insert_values_0_2_type\":\"text\""),
          "quoted system-variable literal INSERT lost text literal type");
  Require(Contains(quoted_system_variable_literal.envelope.payload,
                   "\"insert_values_0_2_value\":\"@@tx_isolation\""),
          "quoted system-variable literal INSERT lost @@tx_isolation string payload");

  const auto generated_surface_manifest_literal = RunPipeline(
      "INSERT INTO customer VALUES "
      "('SBSQL-SURFACE-F63A40BE0271', 'SBSQL-4798C99894E7', 'BATCH-0032', "
      "'@@tx_isolation', 'expression_runtime', 'operator', 'native_now', "
      "'sblr.expression.runtime.v3', 'parser_parse_only', "
      "'parser_parse_only;parser_bind_lower;diagnostic;server_admission;udr_sql_to_sblr;engine_behavior;full_route', "
      "'SBSQL_SURFACE_REPLAY SBSQL-4798C99894E7', "
      "'accepted-or-exact-canonical-refusal;admit_revalidate_route_stream_cancel_and_return_message_vector', "
      "'execute-sblr-internal-procedure-only-no-sql-text', "
      "'project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl#SBSQL-SURFACE-F63A40BE0271')",
      {std::string(kTargetUuid)});
  Require(generated_surface_manifest_literal.bound.bound,
          "generated surface manifest literal INSERT route did not bind");
  Require(generated_surface_manifest_literal.verifier.admitted,
          "generated surface manifest literal INSERT verifier rejected exact route");
  Require(generated_surface_manifest_literal.envelope.operation_id == "dml.insert_rows",
          "generated surface manifest literal INSERT operation id mismatch");
  Require(Contains(generated_surface_manifest_literal.envelope.payload,
                   "\"insert_values_column_count\":\"14\""),
          "generated surface manifest literal INSERT lost generated row column count");
  Require(Contains(generated_surface_manifest_literal.envelope.payload,
                   "\"insert_values_0_3_value\":\"@@tx_isolation\""),
          "generated surface manifest literal INSERT lost generated @@tx_isolation field");
  Require(Contains(generated_surface_manifest_literal.envelope.payload,
                   "\"insert_values_0_13_value\":\"project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl#SBSQL-SURFACE-F63A40BE0271\""),
          "generated surface manifest literal INSERT lost payload-ref field");

  const auto generated_cast_manifest_literal = RunPipeline(
      "INSERT INTO customer VALUES "
      "('SBSQL-SURFACE-5091F71AEE3D', 'SBSQL-C6EDE941F4E9', 'BATCH-0029', "
      "'CAST', 'expression_runtime', 'function', 'native_now', "
      "'sblr.expression.runtime.v3', 'parser_parse_only', "
      "'parser_parse_only;parser_bind_lower;diagnostic;server_admission;udr_sql_to_sblr;engine_behavior;full_route', "
      "'SBSQL_SURFACE_REPLAY SBSQL-C6EDE941F4E9', "
      "'accepted-or-exact-canonical-refusal;admit_revalidate_route_stream_cancel_and_return_message_vector', "
      "'execute-sblr-internal-procedure-only-no-sql-text', "
      "'project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl#SBSQL-SURFACE-5091F71AEE3D')",
      {std::string(kTargetUuid)});
  Require(generated_cast_manifest_literal.bound.bound,
          "generated CAST manifest literal INSERT route did not bind");
  Require(generated_cast_manifest_literal.verifier.admitted,
          "generated CAST manifest literal INSERT verifier rejected exact route");
  Require(generated_cast_manifest_literal.envelope.operation_id == "dml.insert_rows",
          "generated CAST manifest literal INSERT operation id mismatch");
  Require(Contains(generated_cast_manifest_literal.envelope.payload,
                   "\"insert_values_0_3_value\":\"CAST\""),
          "generated CAST manifest literal INSERT lost quoted CAST literal");
}

void RequireUnresolvedNamesFailClosed() {
  const auto artifacts = RunPipeline("INSERT INTO customer VALUES (1)");
  Require(!artifacts.bound.bound || artifacts.envelope.messages.has_errors() ||
              artifacts.verifier.messages.has_errors(),
          "DML without UUID resolution did not fail closed");
}

void RequireSelectOrderLimitLowering() {
  const auto artifacts = RunPipeline(kBoundedOrderedSelectSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "SELECT ORDER/LIMIT statement did not bind");
  Require(artifacts.verifier.admitted, "SELECT ORDER/LIMIT verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "SELECT ORDER/LIMIT operation family mismatch");
  Require(artifacts.envelope.operation_id == "dml.select_rows",
          "SELECT ORDER/LIMIT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "SELECT ORDER/LIMIT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"order_by\":\"id\""),
          "SELECT ORDER/LIMIT payload missing order_by");
  Require(Contains(artifacts.envelope.payload, "\"order_direction\":\"desc\""),
          "SELECT ORDER/LIMIT payload missing descending order");
  Require(Contains(artifacts.envelope.payload, "\"limit\":\"2\""),
          "SELECT ORDER/LIMIT payload missing limit");
  Require(Contains(artifacts.envelope.payload, "\"offset\":\"1\""),
          "SELECT ORDER/LIMIT payload missing offset");
  Require(Contains(artifacts.envelope.payload, "\"ordering_binding_model\":\"engine_row_descriptor_field\""),
          "SELECT ORDER/LIMIT payload missing descriptor ordering binding");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "SELECT ORDER/LIMIT envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT *"),
          "SELECT ORDER/LIMIT envelope embedded SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected SELECT ORDER/LIMIT route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require engine public ABI dispatch for SELECT ORDER/LIMIT");

  for (const auto& row : kBoundedOrderedSelectClauseRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr,
            OrderedSelectEvidenceMessage(row, "registry", "missing generated registry row"));
    Require(registry_row->canonical_name == row.canonical_name,
            OrderedSelectEvidenceMessage(row, "registry", "canonical name mismatch"));
    Require(registry_row->surface_kind == "grammar_production",
            OrderedSelectEvidenceMessage(row, "registry", "surface kind mismatch"));
    Require(registry_row->family == row.family,
            OrderedSelectEvidenceMessage(row, "registry", "family mismatch"));
    Require(registry_row->source_status == "native_now",
            OrderedSelectEvidenceMessage(row, "registry", "source status mismatch"));
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            OrderedSelectEvidenceMessage(row, "registry", "cluster scope mismatch"));
    Require(registry_row->sblr_operation_family == row.sblr_operation_family,
            OrderedSelectEvidenceMessage(row, "registry", "SBLR operation family mismatch"));
    Require(registry_row->validation_fixture_id == row.validation_fixture_id,
            OrderedSelectEvidenceMessage(row, "registry", "validation fixture id mismatch"));
    Require(artifacts.envelope.operation_id == "dml.select_rows",
            OrderedSelectEvidenceMessage(row, "lowering", "operation id mismatch"));
    Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
            OrderedSelectEvidenceMessage(row, "lowering", "opcode mismatch"));
    Require(Contains(artifacts.envelope.payload, row.primary_payload_marker),
            OrderedSelectEvidenceMessage(row, "fixture", "primary payload marker missing"));
    Require(Contains(artifacts.envelope.payload, row.secondary_payload_marker),
            OrderedSelectEvidenceMessage(row, "fixture", "secondary payload marker missing"));
    Require(Contains(artifacts.envelope.payload, row.descriptor_payload_marker),
            OrderedSelectEvidenceMessage(row, "fixture", "descriptor payload marker missing"));
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
            OrderedSelectEvidenceMessage(row, "fixture", "target UUID missing"));
    Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
            OrderedSelectEvidenceMessage(row, "fixture", "payload did not prove no name text"));
    Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
            OrderedSelectEvidenceMessage(row, "fixture", "payload did not prove no SQL text"));
    Require(!Contains(artifacts.envelope.payload, "customer"),
            OrderedSelectEvidenceMessage(row, "fixture", "payload embedded source table name"));
    Require(!Contains(artifacts.envelope.payload, "SELECT *"),
            OrderedSelectEvidenceMessage(row, "fixture", "payload embedded SQL text"));
    Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
            OrderedSelectEvidenceMessage(row, "fixture", "payload embedded source_text"));
  }
}

void RequireContextualKeywordExactRouteEvidence() {
  RequireContextualKeywordRegistryEvidence();

  const auto simple_select = RunPipeline("SELECT * FROM customer", {std::string(kTargetUuid)});
  Require(simple_select.bound.bound, "contextual SELECT/FROM keyword route did not bind");
  Require(simple_select.verifier.admitted,
          "contextual SELECT/FROM keyword verifier rejected exact route");
  Require(Contains(simple_select.envelope.payload, "\"dml_keyword_surface_ids\""),
          "contextual SELECT/FROM keyword payload missing keyword surface list");
  Require(Contains(simple_select.envelope.payload, "SBSQL-2C97BBAE2A81"),
          "contextual SELECT keyword payload marker missing");
  Require(Contains(simple_select.envelope.payload, "SBSQL-92A32408C70E"),
          "contextual FROM keyword payload marker missing");

  const auto where_select =
      RunPipeline(kBoundedWhereEqualitySelectSql, {std::string(kTargetUuid)});
  Require(where_select.bound.bound, "contextual WHERE keyword route did not bind");
  Require(where_select.verifier.admitted,
          "contextual WHERE keyword verifier rejected exact route");
  Require(Contains(where_select.envelope.payload, "SBSQL-DC1B5EC1A284"),
          "contextual WHERE keyword payload marker missing");
  Require(Contains(where_select.envelope.payload, "\"predicate_kind\":\"column_equals\""),
          "contextual WHERE keyword payload missing predicate proof");

  const auto ordered_select =
      RunPipeline(kBoundedOrderedSelectSql, {std::string(kTargetUuid)});
  Require(ordered_select.bound.bound, "contextual ORDER/LIMIT/OFFSET route did not bind");
  Require(ordered_select.verifier.admitted,
          "contextual ORDER/LIMIT/OFFSET verifier rejected exact route");
  Require(Contains(ordered_select.envelope.payload, "SBSQL-540C91206EA6"),
          "contextual ORDER keyword payload marker missing");
  Require(Contains(ordered_select.envelope.payload, "SBSQL-356228FF8746"),
          "contextual LIMIT keyword payload marker missing");
  Require(Contains(ordered_select.envelope.payload, "SBSQL-D9EDA8F8E4E5"),
          "contextual OFFSET keyword payload marker missing");
  Require(Contains(ordered_select.envelope.payload, "\"order_by\":\"id\""),
          "contextual ORDER keyword payload missing order proof");
  Require(Contains(ordered_select.envelope.payload, "\"limit\":\"2\""),
          "contextual LIMIT keyword payload missing limit proof");
  Require(Contains(ordered_select.envelope.payload, "\"offset\":\"1\""),
          "contextual OFFSET keyword payload missing offset proof");

  const auto group_select = RunPipeline(
      "SELECT id, SUM(amount) FROM sales GROUP BY id",
      {std::string(kTargetUuid)});
  Require(group_select.bound.bound, "contextual GROUP keyword route did not bind");
  Require(group_select.verifier.admitted,
          "contextual GROUP keyword verifier rejected exact route");
  Require(Contains(group_select.envelope.payload, "\"query_keyword_surface_ids\""),
          "contextual GROUP keyword payload missing keyword surface list");
  Require(Contains(group_select.envelope.payload, "SBSQL-C287FF004431"),
          "contextual GROUP keyword payload marker missing");
  Require(Contains(group_select.envelope.payload, "\"query_operation\":\"group_by\""),
          "contextual GROUP keyword payload missing group route proof");

  const auto fetch_select =
      RunPipeline(kBoundedFetchFirstSelectSql, {std::string(kTargetUuid)});
  Require(fetch_select.bound.bound, "contextual FETCH keyword route did not bind");
  Require(fetch_select.verifier.admitted,
          "contextual FETCH keyword verifier rejected exact route");
  Require(Contains(fetch_select.envelope.payload, "SBSQL-35ABB1D5392D"),
          "contextual FETCH keyword payload marker missing");
  Require(Contains(fetch_select.envelope.payload, "\"limit\":\"2\""),
          "contextual FETCH keyword payload missing limit proof");

  const auto having_select = RunPipeline(
      "SELECT id, SUM(total) FROM customer GROUP BY id HAVING SUM(total) > 1",
      {std::string(kTargetUuid)});
  Require(having_select.bound.bound, "contextual HAVING keyword route did not bind");
  Require(having_select.verifier.admitted,
          "contextual HAVING keyword verifier rejected exact route");
  Require(Contains(having_select.envelope.payload, "SBSQL-91BFC12AD088"),
          "contextual HAVING keyword payload marker missing");
  Require(Contains(having_select.envelope.payload, "\"having_predicate\":\"aggregate_gt\""),
          "contextual HAVING keyword payload missing predicate proof");

  const auto join_select = RunPipeline(
      "SELECT * FROM customer JOIN orders ON customer.id = orders.id",
      {std::string(kTargetUuid), std::string(kRelatedUuid)});
  Require(join_select.bound.bound, "contextual JOIN keyword route did not bind");
  Require(join_select.verifier.admitted,
          "contextual JOIN keyword verifier rejected exact route");
  Require(Contains(join_select.envelope.payload, "SBSQL-B18D3DA70CBB"),
          "contextual JOIN keyword payload marker missing");
  Require(Contains(join_select.envelope.payload, "\"query_operation\":\"inner_join\""),
          "contextual JOIN keyword payload missing join route proof");

  const auto insert_route =
      RunPipeline("INSERT INTO customer VALUES (1)", {std::string(kTargetUuid)});
  Require(insert_route.bound.bound, "contextual INSERT keyword route did not bind");
  Require(insert_route.verifier.admitted,
          "contextual INSERT keyword verifier rejected exact route");
  Require(Contains(insert_route.envelope.payload, "SBSQL-510838831CA8"),
          "contextual INSERT keyword payload marker missing");
  Require(Contains(insert_route.envelope.payload, "\"dml_surface_variant\":\"insert\""),
          "contextual INSERT keyword payload missing insert variant proof");

  const auto update_route =
      RunPipeline("UPDATE customer SET name = 'x'", {std::string(kTargetUuid)});
  Require(update_route.bound.bound, "contextual UPDATE keyword route did not bind");
  Require(update_route.verifier.admitted,
          "contextual UPDATE keyword verifier rejected exact route");
  Require(Contains(update_route.envelope.payload, "SBSQL-C13B63A81BCC"),
          "contextual UPDATE keyword payload marker missing");
  Require(Contains(update_route.envelope.payload, "\"dml_surface_variant\":\"update\""),
          "contextual UPDATE keyword payload missing update variant proof");

  const auto delete_route =
      RunPipeline("DELETE FROM customer", {std::string(kTargetUuid)});
  Require(delete_route.bound.bound, "contextual DELETE keyword route did not bind");
  Require(delete_route.verifier.admitted,
          "contextual DELETE keyword verifier rejected exact route");
  Require(Contains(delete_route.envelope.payload, "SBSQL-5B33579F1F80"),
          "contextual DELETE keyword payload marker missing");
  Require(Contains(delete_route.envelope.payload, "\"dml_surface_variant\":\"delete\""),
          "contextual DELETE keyword payload missing delete variant proof");

  const auto merge_route = RunPipeline(
      "MERGE INTO customer USING staging ON customer.id = staging.id WHEN MATCHED THEN UPDATE SET name = staging.name",
      {std::string(kTargetUuid), std::string(kRelatedUuid)});
  Require(merge_route.bound.bound, "contextual MERGE keyword route did not bind");
  Require(merge_route.verifier.admitted,
          "contextual MERGE keyword verifier rejected exact route");
  Require(Contains(merge_route.envelope.payload, "SBSQL-B4135CC511C3"),
          "contextual MERGE keyword payload marker missing");
  Require(Contains(merge_route.envelope.payload, "\"dml_surface_variant\":\"merge\""),
          "contextual MERGE keyword payload missing merge variant proof");

  for (const auto* payload : {&simple_select.envelope.payload,
                             &where_select.envelope.payload,
                             &ordered_select.envelope.payload,
                             &group_select.envelope.payload,
                             &fetch_select.envelope.payload,
                             &having_select.envelope.payload,
                             &join_select.envelope.payload,
                             &insert_route.envelope.payload,
                             &update_route.envelope.payload,
                             &delete_route.envelope.payload,
                             &merge_route.envelope.payload}) {
    Require(Contains(*payload, "\"sql_text_included\":false"),
            "contextual keyword payload did not prove no SQL text authority");
    Require(!Contains(*payload, "WAL") && !Contains(*payload, "wal"),
            "contextual keyword payload carried WAL authority");
  }
}

void RequireTopClauseLowering() {
  const auto artifacts = RunPipeline(kBoundedTopSelectSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "SELECT TOP statement did not bind");
  Require(artifacts.verifier.admitted, "SELECT TOP verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "SELECT TOP operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.relational.v3",
          "SELECT TOP SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == "dml.select_rows",
          "SELECT TOP operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "dml.select_rows",
          "SELECT TOP engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "SELECT TOP opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"limit\":\"2\""),
          "SELECT TOP payload missing limit proof");
  Require(Contains(artifacts.envelope.payload, "\"bounded_top_clause\":true"),
          "SELECT TOP payload missing bounded top marker");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "SELECT TOP payload missing target UUID");
  Require(Contains(artifacts.envelope.payload, "\"dml_surface_variant\":\"select\""),
          "SELECT TOP payload missing SELECT surface variant");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_uuid_resolution\":\"server_name_registry_required\""),
          "SELECT TOP payload missing UUID resolution authority");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "SELECT TOP envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT TOP"),
          "SELECT TOP envelope embedded SQL text");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "SELECT TOP envelope embedded source_text");

  const auto* registry_row =
      FindGeneratedSurfaceRegistryRowById(kBoundedTopClauseRow.surface_id);
  Require(registry_row != nullptr,
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "missing generated registry row"));
  Require(registry_row->canonical_name == kBoundedTopClauseRow.canonical_name,
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "surface kind mismatch"));
  Require(registry_row->family == "general",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "family mismatch"));
  Require(registry_row->source_status == "native_now",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "canonical SBLR family mismatch"));
  Require(registry_row->parser_handler_key == "parser.grammar_ast",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "parser_bind_lower",
                                   "parser handler mismatch"));
  Require(registry_row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "parser_bind_lower",
                                   "lowering handler mismatch"));
  Require(registry_row->server_admission_key == "server.admission.sblr_general_operation_v3",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "server_admission",
                                   "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "engine_dispatch",
                                   "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == kBoundedTopClauseRow.validation_fixture_id,
          TopClauseEvidenceMessage(kBoundedTopClauseRow,
                                   "registry",
                                   "validation fixture id mismatch"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected SELECT TOP route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT TOP");
  Require(admission.operation_id == "dml.select_rows",
          "server admission SELECT TOP operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission SELECT TOP route family mismatch");

  constexpr std::string_view kUnsupportedTopSql[] = {
      "SELECT TOP 2 PERCENT * FROM customer",
      "SELECT TOP 2 WITH TIES * FROM customer",
      "SELECT TOP 2 * FROM customer WITH TIES"};
  for (const auto sql : kUnsupportedTopSql) {
    const auto unsupported = RunPipeline(sql, {std::string(kTargetUuid)});
    Require(!unsupported.bound.bound || unsupported.envelope.messages.has_errors() ||
                unsupported.verifier.messages.has_errors(),
            "unsupported TOP variant did not fail closed");
  }
}

void RequireFetchClauseLowering() {
  const auto artifacts = RunPipeline(kBoundedFetchFirstSelectSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "SELECT FETCH FIRST statement did not bind");
  Require(artifacts.verifier.admitted, "SELECT FETCH FIRST verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "SELECT FETCH FIRST operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.relational.v3",
          "SELECT FETCH FIRST SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == "dml.select_rows",
          "SELECT FETCH FIRST operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "dml.select_rows",
          "SELECT FETCH FIRST engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "SELECT FETCH FIRST opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"limit\":\"2\""),
          "SELECT FETCH FIRST payload missing limit proof");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "SELECT FETCH FIRST payload missing target UUID");
  Require(Contains(artifacts.envelope.payload, "\"dml_surface_variant\":\"select\""),
          "SELECT FETCH FIRST payload missing SELECT surface variant");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_uuid_resolution\":\"server_name_registry_required\""),
          "SELECT FETCH FIRST payload missing UUID resolution authority");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "SELECT FETCH FIRST envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "FETCH FIRST"),
          "SELECT FETCH FIRST envelope embedded SQL text");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "SELECT FETCH FIRST envelope embedded source_text");

  const auto next_artifacts = RunPipeline(kBoundedFetchNextSelectSql, {std::string(kTargetUuid)});
  Require(next_artifacts.bound.bound, "SELECT FETCH NEXT statement did not bind");
  Require(next_artifacts.verifier.admitted, "SELECT FETCH NEXT verifier rejected exact route");
  Require(next_artifacts.envelope.operation_id == "dml.select_rows",
          "SELECT FETCH NEXT operation id mismatch");
  Require(next_artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "SELECT FETCH NEXT opcode mismatch");
  Require(Contains(next_artifacts.envelope.payload, "\"limit\":\"2\""),
          "SELECT FETCH NEXT payload missing limit proof");
  Require(!Contains(next_artifacts.envelope.payload, "FETCH NEXT"),
          "SELECT FETCH NEXT envelope embedded SQL text");

  const auto* registry_row =
      FindGeneratedSurfaceRegistryRowById(kBoundedFetchClauseRow.surface_id);
  Require(registry_row != nullptr,
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "missing generated registry row"));
  Require(registry_row->canonical_name == kBoundedFetchClauseRow.canonical_name,
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "surface kind mismatch"));
  Require(registry_row->family == "general",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "family mismatch"));
  Require(registry_row->source_status == "native_now",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "canonical SBLR family mismatch"));
  Require(registry_row->parser_handler_key == "parser.grammar_ast",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "parser_bind_lower",
                                     "parser handler mismatch"));
  Require(registry_row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "parser_bind_lower",
                                     "lowering handler mismatch"));
  Require(registry_row->server_admission_key == "server.admission.sblr_general_operation_v3",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "server_admission",
                                     "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "engine_dispatch",
                                     "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == kBoundedFetchClauseRow.validation_fixture_id,
          FetchClauseEvidenceMessage(kBoundedFetchClauseRow,
                                     "registry",
                                     "validation fixture id mismatch"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected SELECT FETCH FIRST route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT FETCH FIRST");
  Require(admission.operation_id == "dml.select_rows",
          "server admission SELECT FETCH FIRST operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission SELECT FETCH FIRST route family mismatch");

  constexpr std::string_view kUnsupportedFetchSql[] = {
      "SELECT * FROM customer FETCH FIRST 2 ROWS WITH TIES",
      "SELECT * FROM customer FETCH FIRST 2 PERCENT ROWS ONLY"};
  for (const auto sql : kUnsupportedFetchSql) {
    const auto unsupported = RunPipeline(sql, {std::string(kTargetUuid)});
    Require(!unsupported.bound.bound || unsupported.envelope.messages.has_errors() ||
                unsupported.verifier.messages.has_errors(),
            "unsupported FETCH variant did not fail closed");
  }
}

void RequireWhereEqualityPredicateLowering() {
  const auto artifacts =
      RunPipeline(kBoundedWhereEqualitySelectSql, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "SELECT WHERE equality statement did not bind");
  Require(artifacts.verifier.admitted,
          "SELECT WHERE equality verifier rejected bounded exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "SELECT WHERE equality operation family mismatch");
  Require(artifacts.envelope.operation_id == "dml.select_rows",
          "SELECT WHERE equality operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "SELECT WHERE equality opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"predicate_kind\":\"column_equals\""),
          "SELECT WHERE equality payload missing column_equals predicate");
  Require(Contains(artifacts.envelope.payload, "\"predicate_column\":\"id\""),
          "SELECT WHERE equality payload missing predicate column");
  Require(Contains(artifacts.envelope.payload, "\"predicate_value\":\"1\""),
          "SELECT WHERE equality payload missing predicate value");
  Require(Contains(artifacts.envelope.payload, "\"predicate_value_type\":\"integer\""),
          "SELECT WHERE equality payload missing predicate value type");
  Require(Contains(artifacts.envelope.payload,
                   "\"predicate_binding_model\":\"engine_row_descriptor_field\""),
          "SELECT WHERE equality payload missing descriptor predicate binding");
  Require(Contains(artifacts.envelope.payload, "\"predicate_descriptor_bound\":true"),
          "SELECT WHERE equality payload missing descriptor-bound predicate marker");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "SELECT WHERE equality payload missing target UUID");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "SELECT WHERE equality envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT *"),
          "SELECT WHERE equality envelope embedded SQL text");

  for (const auto& row : kBoundedWhereEqualityPredicateRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr,
            WhereClauseEvidenceMessage(row, "registry", "missing generated registry row"));
    Require(registry_row->canonical_name == row.canonical_name,
            WhereClauseEvidenceMessage(row, "registry", "canonical name mismatch"));
    Require(registry_row->surface_kind == "grammar_production",
            WhereClauseEvidenceMessage(row, "registry", "surface kind mismatch"));
    Require(registry_row->family == row.family,
            WhereClauseEvidenceMessage(row, "registry", "family mismatch"));
    Require(registry_row->source_status == "native_now",
            WhereClauseEvidenceMessage(row, "registry", "source status mismatch"));
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            WhereClauseEvidenceMessage(row, "registry", "cluster scope mismatch"));
    Require(registry_row->sblr_operation_family == row.sblr_operation_family,
            WhereClauseEvidenceMessage(row, "registry", "SBLR operation family mismatch"));
    Require(registry_row->parser_handler_key == row.parser_handler_key,
            WhereClauseEvidenceMessage(row, "parser_bind_lower", "parser handler mismatch"));
    Require(registry_row->lowering_handler_key == row.lowering_handler_key,
            WhereClauseEvidenceMessage(row, "parser_bind_lower", "lowering handler mismatch"));
    Require(registry_row->server_admission_key == row.server_admission_key,
            WhereClauseEvidenceMessage(row, "server_admission", "server admission key mismatch"));
    Require(registry_row->engine_rule_key == row.engine_rule_key,
            WhereClauseEvidenceMessage(row, "engine_dispatch", "engine rule mismatch"));
    Require(registry_row->validation_fixture_id == row.validation_fixture_id,
            WhereClauseEvidenceMessage(row, "registry", "validation fixture id mismatch"));
  }

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected SELECT WHERE equality route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require engine public ABI dispatch for SELECT WHERE equality");
  Require(admission.operation_id == "dml.select_rows",
          "server admission SELECT WHERE equality operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission SELECT WHERE equality family mismatch");

  const auto like_artifacts =
      RunPipeline("SELECT * FROM customer WHERE note LIKE 'a%'",
                  {std::string(kTargetUuid)});
  Require(like_artifacts.bound.bound, "SELECT WHERE LIKE statement did not bind");
  Require(like_artifacts.verifier.admitted,
          "SELECT WHERE LIKE verifier rejected bounded exact route");
  Require(like_artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "SELECT WHERE LIKE operation family mismatch");
  Require(like_artifacts.envelope.operation_id == "dml.select_rows",
          "SELECT WHERE LIKE operation id mismatch");
  Require(like_artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "SELECT WHERE LIKE opcode mismatch");
  Require(Contains(like_artifacts.envelope.payload, "\"predicate_kind\":\"column_like\""),
          "SELECT WHERE LIKE payload missing column_like predicate");
  Require(Contains(like_artifacts.envelope.payload, "\"predicate_column\":\"note\""),
          "SELECT WHERE LIKE payload missing predicate column");
  Require(Contains(like_artifacts.envelope.payload, "\"predicate_value\":\"a%\""),
          "SELECT WHERE LIKE payload missing predicate value");
  Require(Contains(like_artifacts.envelope.payload, "\"predicate_value_type\":\"text\""),
          "SELECT WHERE LIKE payload missing predicate value type");
  Require(Contains(like_artifacts.envelope.payload,
                   "\"predicate_binding_model\":\"engine_row_descriptor_field\""),
          "SELECT WHERE LIKE payload missing descriptor predicate binding");
  Require(Contains(like_artifacts.envelope.payload, "\"predicate_descriptor_bound\":true"),
          "SELECT WHERE LIKE payload missing descriptor-bound predicate marker");
  Require(!Contains(like_artifacts.envelope.payload, "customer"),
          "SELECT WHERE LIKE envelope embedded source table name");
  Require(!Contains(like_artifacts.envelope.payload, "LIKE"),
          "SELECT WHERE LIKE envelope embedded SQL syntax text");

  const auto like_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{like_artifacts.envelope.payload, false});
  Require(like_admission.admitted, "server admission rejected SELECT WHERE LIKE route");
  Require(like_admission.requires_public_abi_dispatch,
          "server admission did not require engine public ABI dispatch for SELECT WHERE LIKE");
  Require(like_admission.operation_id == "dml.select_rows",
          "server admission SELECT WHERE LIKE operation id mismatch");
  Require(like_admission.operation_family == "sblr.query.relational.v3",
          "server admission SELECT WHERE LIKE family mismatch");
}

void RequireSimpleSelectSkeletonLowering() {
  const auto artifacts = RunPipeline("SELECT * FROM customer", {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "simple SELECT skeleton statement did not bind");
  Require(artifacts.verifier.admitted, "simple SELECT skeleton verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "simple SELECT skeleton operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.relational.v3",
          "simple SELECT skeleton route operation key mismatch");
  Require(artifacts.envelope.operation_id == "dml.select_rows",
          "simple SELECT skeleton operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "dml.select_rows",
          "simple SELECT skeleton engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "simple SELECT skeleton opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"dml_envelope_kind\""),
          "simple SELECT skeleton payload missing DML envelope marker");
  Require(Contains(artifacts.envelope.payload, "\"dml_surface_variant\":\"select\""),
          "simple SELECT skeleton payload missing select variant");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "simple SELECT skeleton payload missing target UUID");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "simple SELECT skeleton envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT *"),
          "simple SELECT skeleton envelope embedded SQL text");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "simple SELECT skeleton envelope embedded source_text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected simple SELECT skeleton route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for simple SELECT skeleton");
  Require(admission.operation_id == "dml.select_rows",
          "server admission simple SELECT skeleton operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission simple SELECT skeleton route family mismatch");

  for (const auto& row : kBoundedSimpleSelectSkeletonRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr,
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "missing generated registry row"));
    Require(registry_row->canonical_name == row.canonical_name,
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "canonical name mismatch"));
    Require(registry_row->surface_kind == "grammar_production",
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "surface kind mismatch"));
    Require(registry_row->family == row.family,
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "family mismatch"));
    Require(registry_row->source_status == "native_now",
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "source status mismatch"));
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "cluster scope mismatch"));
    Require(registry_row->sblr_operation_family == row.sblr_operation_family,
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "canonical SBLR family mismatch"));
    Require(registry_row->parser_handler_key == row.parser_handler_key,
            SimpleSelectSkeletonEvidenceMessage(row, "parser_bind_lower", "parser handler mismatch"));
    Require(registry_row->lowering_handler_key == row.lowering_handler_key,
            SimpleSelectSkeletonEvidenceMessage(row, "parser_bind_lower", "lowering handler mismatch"));
    Require(registry_row->server_admission_key == row.server_admission_key,
            SimpleSelectSkeletonEvidenceMessage(row, "server_admission", "server admission key mismatch"));
    Require(registry_row->engine_rule_key == row.engine_rule_key,
            SimpleSelectSkeletonEvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
    Require(registry_row->validation_fixture_id == row.validation_fixture_id,
            SimpleSelectSkeletonEvidenceMessage(row, "registry", "validation fixture id mismatch"));
    Require(artifacts.envelope.operation_id == "dml.select_rows",
            SimpleSelectSkeletonEvidenceMessage(row, "lowering", "operation id mismatch"));
    Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
            SimpleSelectSkeletonEvidenceMessage(row, "lowering", "opcode mismatch"));
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            SimpleSelectSkeletonEvidenceMessage(row, "lowering",
                                                "payload missing row-identifiable surface id"));
    Require(!Contains(artifacts.envelope.payload, "customer"),
            SimpleSelectSkeletonEvidenceMessage(row, "fixture", "payload embedded object-name text"));
    Require(!Contains(artifacts.envelope.payload, "SELECT *"),
            SimpleSelectSkeletonEvidenceMessage(row, "fixture", "payload embedded SQL text"));
  }
}

void RequireTableJoinLowering() {
  const auto artifacts = RunPipeline(
      "SELECT * FROM customer JOIN orders ON customer.id = orders.id",
      {std::string(kTargetUuid), std::string(kRelatedUuid)});
  Require(artifacts.bound.bound, "table join statement did not bind");
  Require(artifacts.verifier.admitted, "table join verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "table join operation family mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "table join operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "table join opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "table join query plan authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          "table join MGA visibility authority step missing");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_inner_join\""),
          "table join payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"query_operation\":\"inner_join\""),
          "table join payload missing query operation");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "table join payload missing left UUID");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"related_object_0_uuid\":\"") + std::string(kRelatedUuid) + "\""),
          "table join payload missing right UUID");
  Require(Contains(artifacts.envelope.payload, "\"left_key_field\":\"id\""),
          "table join payload missing left key field");
  Require(Contains(artifacts.envelope.payload, "\"right_key_field\":\"id\""),
          "table join payload missing right key field");
  Require(Contains(artifacts.envelope.payload, "\"join_binding_model\":\"engine_row_descriptor_field\""),
          "table join payload missing descriptor-field binding model");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":true"),
          "table join did not claim source relation requirement");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":true"),
          "table join did not declare row storage access");
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          "table join claimed object name text");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "table join envelope embedded left table name");
  Require(!Contains(artifacts.envelope.payload, "orders"),
          "table join envelope embedded right table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT *"),
          "table join envelope embedded SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected table join route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for table join");
  Require(admission.operation_id == "query.plan_operation",
          "server admission table join operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission table join family mismatch");

  const auto count_assertion = RunPipeline(
      "SELECT 'join_count' AS assertion_id, COUNT(*) AS actual_count, "
      "2 AS expected_count FROM customer JOIN orders ON customer.id = orders.id",
      {std::string(kTargetUuid), std::string(kRelatedUuid)});
  Require(count_assertion.bound.bound, "table join count assertion did not bind");
  Require(count_assertion.verifier.admitted,
          "table join count assertion verifier rejected exact route");
  Require(count_assertion.envelope.operation_id == "query.plan_operation",
          "table join count assertion operation id mismatch");
  Require(!DiagnosticsContain(count_assertion.envelope.messages,
                              "SBSQL.QUERY.COUNT_ROUTE_UNSUPPORTED"),
          "table join count assertion was also routed through generic count analysis");
  Require(Contains(count_assertion.envelope.payload,
                   "\"query_envelope_kind\":\"table_inner_join\""),
          "table join count assertion did not stay on join route");
  Require(!Contains(count_assertion.envelope.payload,
                    "\"query_envelope_kind\":\"table_count\""),
          "table join count assertion emitted table_count payload");
}

void RequireTableSetOperationLowering() {
  struct Case {
    std::string_view sql;
    std::string_view operation;
    bool by_name;
  };
  constexpr Case kCases[] = {
      {"SELECT * FROM customer UNION SELECT * FROM customer_archive", "union_distinct", false},
      {"SELECT * FROM customer UNION ALL SELECT * FROM customer_archive", "union_all", false},
      {"SELECT * FROM customer UNION BY NAME SELECT * FROM customer_archive", "union_distinct", true},
      {"SELECT * FROM customer UNION ALL BY NAME SELECT * FROM customer_archive", "union_all", true},
      {"SELECT * FROM customer INTERSECT DISTINCT SELECT * FROM customer_archive", "intersect_distinct", false},
      {"SELECT * FROM customer INTERSECT ALL SELECT * FROM customer_archive", "intersect_all", false},
      {"SELECT * FROM customer INTERSECT BY NAME SELECT * FROM customer_archive", "intersect_distinct", true},
      {"SELECT * FROM customer EXCEPT SELECT * FROM customer_archive", "except_distinct", false},
      {"SELECT * FROM customer EXCEPT ALL SELECT * FROM customer_archive", "except_all", false},
      {"SELECT * FROM customer EXCEPT BY NAME SELECT * FROM customer_archive", "except_distinct", true}};
  for (const auto& test : kCases) {
    const auto artifacts = RunPipeline(
        test.sql,
        {std::string(kTargetUuid), std::string(kRelatedUuid)});
    Require(artifacts.bound.bound, "table set operation statement did not bind");
    Require(artifacts.verifier.admitted, "table set operation verifier rejected exact route");
    Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
            "table set operation family mismatch");
    Require(artifacts.envelope.operation_id == "query.plan_operation",
            "table set operation id mismatch");
    Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            "table set operation opcode mismatch");
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.query_plan_api_required"),
            "table set operation query plan authority step missing");
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.mga_snapshot_visibility_required"),
            "table set operation MGA visibility authority step missing");
    Require(HasValue(artifacts.envelope.descriptor_refs,
                     "sys.query.table_set_operation_descriptor"),
            "table set operation descriptor ref missing");
    Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_set_operation\""),
            "table set operation payload marker missing");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"set_operation\":\"") + std::string(test.operation) + "\""),
            "table set operation payload missing operation");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
            "table set operation payload missing left UUID");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"related_object_0_uuid\":\"") + std::string(kRelatedUuid) + "\""),
            "table set operation payload missing right UUID");
    Require(Contains(artifacts.envelope.payload,
                     test.by_name ? "\"set_by_name\":true" : "\"set_by_name\":false"),
            "table set operation payload missing BY NAME flag");
    Require(Contains(artifacts.envelope.payload,
                     test.by_name
                         ? "\"set_binding_model\":\"engine_row_descriptor_name_int64_current_route\""
                         : "\"set_binding_model\":\"engine_row_descriptor_ordinal_int64_current_route\""),
            "table set operation payload missing binding model");
    Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":true"),
            "table set operation did not claim source relation requirement");
    Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":true"),
            "table set operation did not declare row storage access");
    Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
            "table set operation claimed object name text");
    Require(!Contains(artifacts.envelope.payload, "customer"),
            "table set operation envelope embedded left table name");
    Require(!Contains(artifacts.envelope.payload, "customer_archive"),
            "table set operation envelope embedded right table name");
    Require(!Contains(artifacts.envelope.payload, "SELECT *"),
            "table set operation envelope embedded SQL text");

    const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
    Require(admission.admitted, "server admission rejected table set operation route");
    Require(admission.requires_public_abi_dispatch,
            "server admission did not require public ABI dispatch for table set operation");
    Require(admission.operation_id == "query.plan_operation",
            "server admission table set operation id mismatch");
    Require(admission.operation_family == "sblr.query.relational.v3",
            "server admission table set operation family mismatch");
  }

  const auto count_union = RunPipeline(
      "SELECT 'SBDFS-085-010' AS assertion_id, "
      "COUNT(*) AS actual_security_matrix_cases, "
      "13 AS expected_security_matrix_cases "
      "FROM ("
      "SELECT case_id FROM security_authorization_cases "
      "UNION ALL "
      "SELECT case_id FROM security_sblr_replay_cases "
      "UNION ALL "
      "SELECT case_id FROM security_uuid_resolution_cases"
      ") AS security_case_union",
      {std::string(kTargetUuid),
       std::string(kRelatedUuid),
       std::string(kThirdRelationUuid)});
  Require(count_union.bound.bound, "set-operation count assertion did not bind");
  Require(count_union.verifier.admitted,
          "set-operation count assertion verifier rejected exact route");
  Require(count_union.envelope.operation_family == "sblr.query.relational.v3",
          "set-operation count assertion family mismatch");
  Require(count_union.envelope.operation_id == "query.plan_operation",
          "set-operation count assertion operation id mismatch");
  Require(Contains(count_union.envelope.payload,
                   "\"query_envelope_kind\":\"table_set_operation\""),
          "set-operation count assertion payload marker missing");
  Require(Contains(count_union.envelope.payload, "\"set_operation\":\"union_all\""),
          "set-operation count assertion payload missing union_all");
  Require(Contains(count_union.envelope.payload, "\"relation_count\":\"3\""),
          "set-operation count assertion payload missing relation count");
  Require(Contains(count_union.envelope.payload,
                   std::string("\"target_object_uuid\":\"") +
                       std::string(kTargetUuid) + "\""),
          "set-operation count assertion payload missing first relation UUID");
  Require(Contains(count_union.envelope.payload,
                   std::string("\"related_object_0_uuid\":\"") +
                       std::string(kRelatedUuid) + "\""),
          "set-operation count assertion payload missing second relation UUID");
  Require(Contains(count_union.envelope.payload,
                   std::string("\"related_object_1_uuid\":\"") +
                       std::string(kThirdRelationUuid) + "\""),
          "set-operation count assertion payload missing third relation UUID");
  Require(Contains(count_union.envelope.payload, "\"result_projection\":\"count_assertion\""),
          "set-operation count assertion payload missing count projection");
  Require(Contains(count_union.envelope.payload, "\"expected_count\":\"13\""),
          "set-operation count assertion payload missing expected count");
  Require(!Contains(count_union.envelope.payload, "security_authorization_cases"),
          "set-operation count assertion payload embedded source relation name");
  Require(!Contains(count_union.envelope.payload, "UNION ALL"),
          "set-operation count assertion payload embedded SQL text");

  const auto count_union_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{count_union.envelope.payload, false});
  Require(count_union_admission.admitted,
          "server admission rejected set-operation count assertion route");
  Require(count_union_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for set-operation count assertion");
}

void RequireRowNumberWindowLowering() {
  for (const auto& row : kBoundedWindowClauseGrammarRows) {
    RequireWindowClauseGrammarRegistryEvidence(row);
  }

  const auto artifacts = RunPipeline(
      "SELECT row_number() OVER (ORDER BY id) FROM customer",
      {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "row_number window statement did not bind");
  Require(artifacts.verifier.admitted, "row_number window verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "row_number window operation family mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "row_number window operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "row_number window opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "row_number window query plan authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          "row_number window MGA visibility authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.query.window_descriptor"),
          "row_number window descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_row_number_window\""),
          "row_number window payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"query_operation\":\"row_number_window\""),
          "row_number window payload missing query operation");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "row_number window payload missing table UUID");
  Require(Contains(artifacts.envelope.payload, "\"order_by\":\"id\""),
          "row_number window payload missing order field");
  Require(Contains(artifacts.envelope.payload, "\"order_column\":\"0\""),
          "row_number window payload missing order column");
  Require(Contains(artifacts.envelope.payload, "\"window_binding_model\":\"engine_row_descriptor_field_int64_current_route\""),
          "row_number window payload missing binding model");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":true"),
          "row_number window did not claim source relation requirement");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":true"),
          "row_number window did not declare row storage access");
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          "row_number window claimed object name text");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "row_number window envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT row_number"),
          "row_number window envelope embedded SQL text");
  for (const auto& row : kBoundedWindowClauseGrammarRows) {
    if (row.sql_fixture != "SELECT row_number() OVER (ORDER BY id) FROM customer") {
      continue;
    }
    Require(artifacts.envelope.operation_id == "query.plan_operation",
            WindowClauseEvidenceMessage(row, "lowering", "operation id mismatch"));
    Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            WindowClauseEvidenceMessage(row, "lowering", "opcode mismatch"));
    Require(Contains(artifacts.envelope.payload, row.route_marker),
            WindowClauseEvidenceMessage(row, "lowering", "route marker missing"));
    Require(Contains(artifacts.envelope.payload, row.clause_marker),
            WindowClauseEvidenceMessage(row, "lowering", "clause marker missing"));
    Require(!Contains(artifacts.envelope.payload, "customer"),
            WindowClauseEvidenceMessage(row, "fixture", "payload embedded source table name"));
    Require(!Contains(artifacts.envelope.payload, "SELECT row_number"),
            WindowClauseEvidenceMessage(row, "fixture", "payload embedded SQL text"));
  }

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected row_number window route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for row_number window");
  Require(admission.operation_id == "query.plan_operation",
          "server admission row_number window operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission row_number window family mismatch");

  const auto count_partition = RunPipeline(
      "SELECT count(*) OVER (PARTITION BY dept) FROM sales",
      {std::string(kTargetUuid)});
  Require(count_partition.bound.bound,
          "count partition window statement did not bind");
  Require(count_partition.verifier.admitted,
          "count partition window verifier rejected exact route");
  Require(count_partition.envelope.operation_family == "sblr.query.relational.v3",
          "count partition window operation family mismatch");
  Require(count_partition.envelope.operation_id == "query.plan_operation",
          "count partition window operation id mismatch");
  Require(count_partition.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "count partition window opcode mismatch");
  Require(HasValue(count_partition.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "count partition window query plan authority step missing");
  Require(HasValue(count_partition.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          "count partition window MGA visibility authority step missing");
  Require(HasValue(count_partition.envelope.descriptor_refs,
                   "sys.query.window_descriptor"),
          "count partition window descriptor ref missing");
  Require(Contains(count_partition.envelope.payload,
                   "\"query_envelope_kind\":\"table_partition_count_window\""),
          "count partition window payload marker missing");
  Require(Contains(count_partition.envelope.payload,
                   "\"query_operation\":\"partition_count_window\""),
          "count partition window payload missing operation");
  Require(Contains(count_partition.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "count partition window payload missing table UUID");
  Require(Contains(count_partition.envelope.payload, "\"partition_by\":\"dept\""),
          "count partition window payload missing partition field");
  Require(Contains(count_partition.envelope.payload, "\"partition_column\":\"0\""),
          "count partition window payload missing partition column");
  Require(Contains(count_partition.envelope.payload,
                   "\"window_function\":\"count_star_partition\""),
          "count partition window payload missing canonical function");
  Require(Contains(count_partition.envelope.payload,
                   "\"aggregate_function\":\"sb.aggregate.count\""),
          "count partition window payload missing canonical aggregate");
  Require(Contains(count_partition.envelope.payload,
                   "\"window_binding_model\":\"engine_row_descriptor_field_int64_partition_count_route\""),
          "count partition window payload missing partition-count binding model");
  Require(Contains(count_partition.envelope.payload, "\"source_relation_required\":true"),
          "count partition window did not claim source relation requirement");
  Require(Contains(count_partition.envelope.payload, "\"row_storage_touched\":true"),
          "count partition window did not declare row storage access");
  Require(Contains(count_partition.envelope.payload, "\"object_name_text_included\":false"),
          "count partition window claimed object name text");
  Require(!Contains(count_partition.envelope.payload, "sales"),
          "count partition window envelope embedded source table name");
  Require(!Contains(count_partition.envelope.payload, "SELECT count"),
          "count partition window envelope embedded SQL text");
  for (const auto& row : kBoundedWindowClauseGrammarRows) {
    if (row.sql_fixture != "SELECT count(*) OVER (PARTITION BY dept) FROM sales") {
      continue;
    }
    Require(count_partition.envelope.operation_id == "query.plan_operation",
            WindowClauseEvidenceMessage(row, "lowering", "operation id mismatch"));
    Require(count_partition.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            WindowClauseEvidenceMessage(row, "lowering", "opcode mismatch"));
    Require(Contains(count_partition.envelope.payload, row.route_marker),
            WindowClauseEvidenceMessage(row, "lowering", "route marker missing"));
    Require(Contains(count_partition.envelope.payload, row.clause_marker),
            WindowClauseEvidenceMessage(row, "lowering", "clause marker missing"));
    Require(!Contains(count_partition.envelope.payload, "sales"),
            WindowClauseEvidenceMessage(row, "fixture", "payload embedded source table name"));
    Require(!Contains(count_partition.envelope.payload, "SELECT count"),
            WindowClauseEvidenceMessage(row, "fixture", "payload embedded SQL text"));
  }

  const auto count_partition_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{count_partition.envelope.payload, false});
  Require(count_partition_admission.admitted,
          "server admission rejected count partition window route");
  Require(count_partition_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for count partition window");
  Require(count_partition_admission.operation_id == "query.plan_operation",
          "server admission count partition window operation id mismatch");
  Require(count_partition_admission.operation_family == "sblr.query.relational.v3",
          "server admission count partition window family mismatch");

  const auto qualified_row_number = RunPipeline(
      "SELECT sb.window.row_number() OVER (ORDER BY id) FROM customer",
      {std::string(kTargetUuid)});
  Require(qualified_row_number.bound.bound,
          "qualified canonical row_number window statement did not bind");
  Require(qualified_row_number.verifier.admitted,
          "qualified canonical row_number window verifier rejected exact route");
  Require(Contains(qualified_row_number.envelope.payload,
                   "\"query_envelope_kind\":\"table_row_number_window\""),
          "qualified canonical row_number payload marker missing");
  Require(Contains(qualified_row_number.envelope.payload,
                   "\"window_function\":\"row_number\""),
          "qualified canonical row_number payload missing function");
  Require(!Contains(qualified_row_number.envelope.payload, "sb.window.row_number"),
          "qualified canonical row_number envelope embedded source function name");

  struct Case {
    std::string_view sql;
    std::string_view function;
    bool has_value_field;
    bool has_window_n;
    std::string_view binding_model;
  };
  constexpr Case kCases[] = {
      {"SELECT rank() OVER (ORDER BY id) FROM customer", "rank", false, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT dense_rank() OVER (ORDER BY id) FROM customer", "dense_rank", false, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT percent_rank() OVER (ORDER BY id) FROM customer", "percent_rank", false, false,
       "engine_row_descriptor_field_typed_nullable_route"},
      {"SELECT cume_dist() OVER (ORDER BY id) FROM customer", "cume_dist", false, false,
       "engine_row_descriptor_field_typed_nullable_route"},
      {"SELECT ntile(2) OVER (ORDER BY id) FROM customer", "ntile", false, true,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT lag(id) OVER (ORDER BY id) FROM customer", "lag", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT lead(id) OVER (ORDER BY id) FROM customer", "lead", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT first_value(id) OVER (ORDER BY id) FROM customer", "first_value", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT last_value(id) OVER (ORDER BY id) FROM customer", "last_value", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT nth_value(id, 2) OVER (ORDER BY id) FROM customer", "nth_value", true, true,
       "engine_row_descriptor_field_typed_nullable_route"},
      {"SELECT sb.window.rank() OVER (ORDER BY id) FROM customer", "rank", false, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT sb.window.dense_rank() OVER (ORDER BY id) FROM customer", "dense_rank", false, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT sb.window.percent_rank() OVER (ORDER BY id) FROM customer", "percent_rank", false, false,
       "engine_row_descriptor_field_typed_nullable_route"},
      {"SELECT sb.window.cume_dist() OVER (ORDER BY id) FROM customer", "cume_dist", false, false,
       "engine_row_descriptor_field_typed_nullable_route"},
      {"SELECT sb.window.ntile(2) OVER (ORDER BY id) FROM customer", "ntile", false, true,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT sb.window.lag(id) OVER (ORDER BY id) FROM customer", "lag", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT sb.window.lead(id) OVER (ORDER BY id) FROM customer", "lead", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT sb.window.first_value(id) OVER (ORDER BY id) FROM customer", "first_value", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT sb.window.last_value(id) OVER (ORDER BY id) FROM customer", "last_value", true, false,
       "engine_row_descriptor_field_int64_current_route"},
      {"SELECT sb.window.nth_value(id, 2) OVER (ORDER BY id) FROM customer", "nth_value", true, true,
       "engine_row_descriptor_field_typed_nullable_route"}};
  for (const auto& test : kCases) {
    const auto window_artifacts = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(window_artifacts.bound.bound, "navigation window statement did not bind");
    Require(window_artifacts.verifier.admitted,
            "navigation window verifier rejected exact route");
    Require(window_artifacts.envelope.operation_id == "query.plan_operation",
            "navigation window operation id mismatch");
    Require(Contains(window_artifacts.envelope.payload,
                     "\"query_envelope_kind\":\"table_window\""),
            "navigation window payload marker missing");
    Require(Contains(window_artifacts.envelope.payload, "\"query_operation\":\"window\""),
            "navigation window payload missing generic window operation");
    Require(Contains(window_artifacts.envelope.payload,
                     std::string("\"window_function\":\"") + std::string(test.function) + "\""),
            "navigation window payload missing expected function");
    if (test.has_value_field) {
      Require(Contains(window_artifacts.envelope.payload, "\"window_value_field\":\"id\""),
              "navigation window payload missing value field");
    } else {
      Require(!Contains(window_artifacts.envelope.payload, "\"window_value_field\""),
              "nullary ranking window payload should not carry value field");
    }
    if (test.has_window_n) {
      Require(Contains(window_artifacts.envelope.payload, "\"window_n\":\"2\""),
              "window payload missing numeric window operand");
    }
    Require(Contains(window_artifacts.envelope.payload,
                     std::string("\"window_binding_model\":\"") + std::string(test.binding_model) + "\""),
            "navigation window payload missing expected binding model");
    Require(Contains(window_artifacts.envelope.payload, "\"order_by\":\"id\""),
            "navigation window payload missing order field");
    Require(!Contains(window_artifacts.envelope.payload, "SELECT "),
            "navigation window envelope embedded SQL text");
  }
}

void RequireGroupByAggregateLowering() {
  const auto artifacts = RunPipeline(
      "SELECT id, SUM(amount) FROM sales GROUP BY id",
      {std::string(kTargetUuid)});
  for (const auto& row : kBoundedGroupByGrammarRows) {
    RequireGroupByGrammarRegistryEvidence(row);
  }
  Require(artifacts.bound.bound, "grouped aggregate statement did not bind");
  Require(artifacts.verifier.admitted, "grouped aggregate verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "grouped aggregate operation family mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "grouped aggregate operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "grouped aggregate opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "grouped aggregate query plan authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          "grouped aggregate MGA visibility authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.query.aggregate_descriptor"),
          "grouped aggregate descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_group_sum\""),
          "grouped aggregate payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"query_operation\":\"group_by\""),
          "grouped aggregate payload missing query operation");
  for (const auto& row : kBoundedGroupByGrammarRows) {
    Require(Contains(artifacts.envelope.payload, "\"query_operation\":\"group_by\""),
            GroupByEvidenceMessage(row, "lowering", "group_by query operation missing"));
    Require(Contains(artifacts.envelope.payload, "\"group_key_field\":\"id\""),
            GroupByEvidenceMessage(row, "lowering", "group key id missing"));
    Require(Contains(artifacts.envelope.payload,
                     "\"aggregate_function\":\"sb.aggregate.sum\""),
            GroupByEvidenceMessage(row, "lowering", "canonical SUM aggregate missing"));
  }
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "grouped aggregate payload missing table UUID");
  Require(Contains(artifacts.envelope.payload, "\"group_key_field\":\"id\""),
          "grouped aggregate payload missing group key field");
  Require(Contains(artifacts.envelope.payload, "\"aggregate_function\":\"sb.aggregate.sum\""),
          "grouped aggregate payload missing canonical sum aggregate");
  Require(Contains(artifacts.envelope.payload, "\"aggregate_value_field\":\"amount\""),
          "grouped aggregate payload missing aggregate value field");
  Require(Contains(artifacts.envelope.payload, "\"group_key_column\":\"0\""),
          "grouped aggregate payload missing key column");
  Require(Contains(artifacts.envelope.payload, "\"aggregate_value_column\":\"1\""),
          "grouped aggregate payload missing aggregate column");
  Require(Contains(artifacts.envelope.payload, "\"aggregate_binding_model\":\"engine_row_descriptor_field_int64_current_route\""),
          "grouped aggregate payload missing binding model");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":true"),
          "grouped aggregate did not claim source relation requirement");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":true"),
          "grouped aggregate did not declare row storage access");
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          "grouped aggregate claimed object name text");
  Require(!Contains(artifacts.envelope.payload, "sales"),
          "grouped aggregate envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT id"),
          "grouped aggregate envelope embedded SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected grouped aggregate route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for grouped aggregate");
  Require(admission.operation_id == "query.plan_operation",
          "server admission grouped aggregate operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission grouped aggregate family mismatch");

  const auto field_artifacts = RunPipeline(
      "SELECT dept, SUM(cost) FROM sales GROUP BY dept",
      {std::string(kTargetUuid)});
  Require(field_artifacts.bound.bound,
          "descriptor-field grouped aggregate statement did not bind");
  Require(field_artifacts.verifier.admitted,
          "descriptor-field grouped aggregate verifier rejected exact route");
  Require(Contains(field_artifacts.envelope.payload, "\"group_key_field\":\"dept\""),
          "descriptor-field grouped aggregate payload missing group key field");
  Require(Contains(field_artifacts.envelope.payload, "\"aggregate_value_field\":\"cost\""),
          "descriptor-field grouped aggregate payload missing aggregate value field");
  Require(Contains(field_artifacts.envelope.payload, "\"aggregate_function\":\"sb.aggregate.sum\""),
          "descriptor-field grouped aggregate payload missing canonical sum aggregate");
  Require(Contains(field_artifacts.envelope.payload,
                   "\"aggregate_binding_model\":\"engine_row_descriptor_field_int64_current_route\""),
          "descriptor-field grouped aggregate payload missing descriptor binding model");

  const auto canonical_sum_artifacts = RunPipeline(
      "SELECT dept, sb.aggregate.sum(cost) FROM sales GROUP BY dept",
      {std::string(kTargetUuid)});
  Require(canonical_sum_artifacts.bound.bound,
          "canonical sb.aggregate.sum grouped aggregate statement did not bind");
  Require(canonical_sum_artifacts.verifier.admitted,
          "canonical sb.aggregate.sum grouped aggregate verifier rejected exact route");
  Require(Contains(canonical_sum_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"table_group_sum\""),
          "canonical sb.aggregate.sum payload marker missing");
  Require(Contains(canonical_sum_artifacts.envelope.payload,
                   "\"aggregate_function\":\"sb.aggregate.sum\""),
          "canonical sb.aggregate.sum payload missing canonical function id");
  Require(!Contains(canonical_sum_artifacts.envelope.payload, "sb.aggregate.sum("),
          "canonical sb.aggregate.sum envelope carried source spelling");

  const auto sum_distinct_artifacts = RunPipeline(
      "SELECT dept, SUM(DISTINCT cost) FROM sales GROUP BY dept",
      {std::string(kTargetUuid)});
  Require(sum_distinct_artifacts.bound.bound,
          "SUM(DISTINCT) fail-closed proof did not bind before route refusal");
  Require(sum_distinct_artifacts.envelope.messages.has_errors() ||
              sum_distinct_artifacts.verifier.messages.has_errors(),
          "SUM(DISTINCT) exact route did not fail closed");
  Require(DiagnosticsContain(sum_distinct_artifacts.envelope.messages,
                             "SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED"),
          "SUM(DISTINCT) fail-closed proof missing grouped route diagnostic");
  Require(DiagnosticsContain(sum_distinct_artifacts.envelope.messages,
                             "sum_distinct_requires_distinct_aggregate_execution_route"),
          "SUM(DISTINCT) fail-closed proof missing precise DISTINCT route reason");
  Require(!sum_distinct_artifacts.verifier.admitted,
          "SUM(DISTINCT) exact route admitted unsupported DISTINCT aggregate");
  Require(!Contains(sum_distinct_artifacts.envelope.payload, "\"aggregate_function\""),
          "SUM(DISTINCT) fail-closed route emitted aggregate payload authority");

  struct TypedAggregateCase {
    std::string_view sql;
    std::string_view function_id;
    std::string_view value_field;
    std::string_view binding_model;
    std::string_view pair_field{};
    std::string_view option_field{};
    std::string_view option_value{};
  };
  const TypedAggregateCase typed_cases[] = {
      {"SELECT dept, STDDEV(cost) FROM sales GROUP BY dept",
       "sb.aggregate.stddev",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, STDDEV_SAMP(cost) FROM sales GROUP BY dept",
       "sb.aggregate.stddev_samp",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, STDDEV_POP(cost) FROM sales GROUP BY dept",
       "sb.aggregate.stddev_pop",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, VARIANCE(cost) FROM sales GROUP BY dept",
       "sb.aggregate.variance",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, VARIANCE_SAMP(cost) FROM sales GROUP BY dept",
       "sb.aggregate.variance_samp",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, VARIANCE_POP(cost) FROM sales GROUP BY dept",
       "sb.aggregate.variance_pop",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, sb.aggregate.stddev(cost) FROM sales GROUP BY dept",
       "sb.aggregate.stddev",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, sb.aggregate.stddev_pop(cost) FROM sales GROUP BY dept",
       "sb.aggregate.stddev_pop",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, sb.aggregate.variance_pop(cost) FROM sales GROUP BY dept",
       "sb.aggregate.variance_pop",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, COUNT(*) FROM sales GROUP BY dept",
       "sb.aggregate.count",
       "dept",
       "engine_row_descriptor_field_int64_result_route"},
      {"SELECT dept, COUNT(cost) FROM sales GROUP BY dept",
       "sb.aggregate.count",
       "cost",
       "engine_row_descriptor_field_int64_result_route"},
      {"SELECT dept, sb.aggregate.count(cost) FROM sales GROUP BY dept",
       "sb.aggregate.count",
       "cost",
       "engine_row_descriptor_field_int64_result_route"},
      {"SELECT dept, AVG(cost) FROM sales GROUP BY dept",
       "sb.aggregate.avg",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, sb.aggregate.avg(cost) FROM sales GROUP BY dept",
       "sb.aggregate.avg",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, MIN(cost) FROM sales GROUP BY dept",
       "sb.aggregate.min",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, sb.aggregate.min(cost) FROM sales GROUP BY dept",
       "sb.aggregate.min",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, MAX(cost) FROM sales GROUP BY dept",
       "sb.aggregate.max",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, sb.aggregate.max(cost) FROM sales GROUP BY dept",
       "sb.aggregate.max",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, BOOL_AND(flag) FROM sales GROUP BY dept",
       "sb.aggregate.bool_and",
       "flag",
       "engine_row_descriptor_field_boolean_nullable_route"},
      {"SELECT dept, sb.aggregate.bool_and(flag) FROM sales GROUP BY dept",
       "sb.aggregate.bool_and",
       "flag",
       "engine_row_descriptor_field_boolean_nullable_route"},
      {"SELECT dept, BOOL_OR(flag) FROM sales GROUP BY dept",
       "sb.aggregate.bool_or",
       "flag",
       "engine_row_descriptor_field_boolean_nullable_route"},
      {"SELECT dept, sb.aggregate.bool_or(flag) FROM sales GROUP BY dept",
       "sb.aggregate.bool_or",
       "flag",
       "engine_row_descriptor_field_boolean_nullable_route"},
      {"SELECT dept, EVERY(flag) FROM sales GROUP BY dept",
       "sb.aggregate.every",
       "flag",
       "engine_row_descriptor_field_boolean_nullable_route"},
      {"SELECT dept, sb.aggregate.every(flag) FROM sales GROUP BY dept",
       "sb.aggregate.every",
       "flag",
       "engine_row_descriptor_field_boolean_nullable_route"},
      {"SELECT dept, APPROX_COUNT_DISTINCT(cost) FROM sales GROUP BY dept",
       "sb.aggregate.approx_count_distinct",
       "cost",
       "engine_row_descriptor_field_int64_result_route"},
      {"SELECT dept, APPROX_MEDIAN(cost) FROM sales GROUP BY dept",
       "sb.aggregate.approx_median",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, CORR(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.corr",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, COVAR_POP(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.covar_pop",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, COVAR_SAMP(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.covar_samp",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_COUNT(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_count",
       "cost",
       "engine_row_descriptor_field_int64_result_route",
       "id"},
      {"SELECT dept, REGR_AVGX(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_avgx",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_AVGY(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_avgy",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_INTERCEPT(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_intercept",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_R2(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_r2",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_SLOPE(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_slope",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_SXX(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_sxx",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_SXY(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_sxy",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, REGR_SYY(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.regr_syy",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, sb.aggregate.corr(cost, id) FROM sales GROUP BY dept",
       "sb.aggregate.corr",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "id"},
      {"SELECT dept, MODE(cost) FROM sales GROUP BY dept",
       "sb.aggregate.mode",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route"},
      {"SELECT dept, APPROX_TOP_K(cost, 1) FROM sales GROUP BY dept",
       "sb.aggregate.approx_top_k",
       "cost",
       "engine_row_descriptor_field_json_nullable_route",
       "",
       "aggregate_limit",
       "1"},
      {"SELECT dept, PERCENTILE_CONT(cost, 0.5) FROM sales GROUP BY dept",
       "sb.aggregate.percentile_cont",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "",
       "aggregate_fraction",
       "0.5"},
      {"SELECT dept, PERCENTILE_DISC(cost, 0.5) FROM sales GROUP BY dept",
       "sb.aggregate.percentile_disc",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "",
       "aggregate_fraction",
       "0.5"},
      {"SELECT dept, APPROX_PERCENTILE_CONT(cost, 0.5) FROM sales GROUP BY dept",
       "sb.aggregate.approx_percentile_cont",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "",
       "aggregate_fraction",
       "0.5"},
      {"SELECT dept, APPROX_PERCENTILE_DISC(cost, 0.5) FROM sales GROUP BY dept",
       "sb.aggregate.approx_percentile_disc",
       "cost",
       "engine_row_descriptor_field_real64_nullable_route",
       "",
       "aggregate_fraction",
       "0.5"},
  };
  for (const auto& test : typed_cases) {
    const auto stat_artifacts = RunPipeline(std::string(test.sql),
                                            {std::string(kTargetUuid)});
    Require(stat_artifacts.bound.bound,
            "typed grouped aggregate statement did not bind");
    Require(stat_artifacts.verifier.admitted,
            "typed grouped aggregate verifier rejected exact route");
    Require(stat_artifacts.envelope.operation_family == "sblr.query.relational.v3",
            "typed grouped aggregate operation family mismatch");
    Require(stat_artifacts.envelope.operation_id == "query.plan_operation",
            "typed grouped aggregate operation id mismatch");
    Require(stat_artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            "typed grouped aggregate opcode mismatch");
    Require(Contains(stat_artifacts.envelope.payload,
                     "\"query_envelope_kind\":\"table_group_aggregate\""),
            "typed grouped aggregate payload marker missing");
    Require(Contains(stat_artifacts.envelope.payload,
                     std::string("\"aggregate_function\":\"") +
                         std::string(test.function_id) + "\""),
            "typed grouped aggregate payload missing canonical function id");
    Require(Contains(stat_artifacts.envelope.payload,
                     std::string("\"aggregate_value_field\":\"") +
                         std::string(test.value_field) + "\""),
            "typed grouped aggregate payload missing aggregate value field");
    Require(Contains(stat_artifacts.envelope.payload,
                     std::string("\"aggregate_binding_model\":\"") +
                         std::string(test.binding_model) + "\""),
            "typed grouped aggregate payload missing expected binding model");
    if (!test.pair_field.empty()) {
      Require(Contains(stat_artifacts.envelope.payload,
                       std::string("\"aggregate_pair_value_field\":\"") +
                           std::string(test.pair_field) + "\""),
              "typed grouped aggregate payload missing pair value field");
      Require(Contains(stat_artifacts.envelope.payload,
                       "\"aggregate_pair_value_column\":\"2\""),
              "typed grouped aggregate payload missing pair value column");
    }
    if (!test.option_field.empty()) {
      Require(Contains(stat_artifacts.envelope.payload,
                       std::string("\"") + std::string(test.option_field) + "\":\"" +
                           std::string(test.option_value) + "\""),
              "typed grouped aggregate payload missing aggregate option");
    }
    Require(!Contains(stat_artifacts.envelope.payload, "sb.aggregate.stddev("),
            "typed grouped aggregate envelope carried source spelling");
    Require(!Contains(stat_artifacts.envelope.payload, "sb.aggregate.stddev_pop("),
            "typed grouped aggregate envelope carried population stddev source spelling");
    Require(!Contains(stat_artifacts.envelope.payload, "sb.aggregate.variance_pop("),
            "typed grouped aggregate envelope carried population variance source spelling");

    const auto stat_admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{stat_artifacts.envelope.payload, false});
    Require(stat_admission.admitted,
            "server admission rejected typed grouped aggregate route");
    Require(stat_admission.requires_public_abi_dispatch,
            "server admission did not require public ABI dispatch for typed grouped aggregate");
    Require(stat_admission.operation_id == "query.plan_operation",
            "server admission typed aggregate operation id mismatch");
  }

  struct DistinctAggregateRefusalCase {
    std::string_view sql;
    std::string_view reason;
  };
  const DistinctAggregateRefusalCase distinct_refusals[] = {
      {"SELECT dept, COUNT(DISTINCT cost) FROM sales GROUP BY dept",
       "count_distinct_requires_distinct_aggregate_execution_route"},
      {"SELECT dept, AVG(DISTINCT cost) FROM sales GROUP BY dept",
       "avg_distinct_requires_distinct_aggregate_execution_route"}};
  for (const auto& test : distinct_refusals) {
    const auto distinct_artifacts = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(distinct_artifacts.bound.bound,
            "DISTINCT aggregate fail-closed proof did not bind before route refusal");
    Require(distinct_artifacts.envelope.messages.has_errors() ||
                distinct_artifacts.verifier.messages.has_errors(),
            "DISTINCT aggregate exact route did not fail closed");
    Require(DiagnosticsContain(distinct_artifacts.envelope.messages,
                               "SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED"),
            "DISTINCT aggregate fail-closed proof missing grouped route diagnostic");
    Require(DiagnosticsContain(distinct_artifacts.envelope.messages, test.reason),
            "DISTINCT aggregate fail-closed proof missing precise route reason");
    Require(!distinct_artifacts.verifier.admitted,
            "DISTINCT aggregate exact route admitted unsupported DISTINCT aggregate");
    Require(!Contains(distinct_artifacts.envelope.payload, "\"aggregate_function\""),
            "DISTINCT aggregate fail-closed route emitted aggregate payload authority");
  }

  const auto listagg_artifacts = RunPipeline(
      "SELECT dept, LISTAGG(flag, '|' ON OVERFLOW TRUNCATE '...' WITHOUT COUNT) "
      "WITHIN GROUP (ORDER BY id) FROM sales GROUP BY dept",
      {std::string(kTargetUuid)});
  Require(listagg_artifacts.bound.bound,
          "LISTAGG WITHIN GROUP statement did not bind");
  Require(listagg_artifacts.verifier.admitted,
          "LISTAGG WITHIN GROUP verifier rejected exact route");
  Require(listagg_artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "LISTAGG WITHIN GROUP operation family mismatch");
  Require(listagg_artifacts.envelope.operation_id == "query.plan_operation",
          "LISTAGG WITHIN GROUP operation id mismatch");
  Require(listagg_artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "LISTAGG WITHIN GROUP opcode mismatch");
  Require(Contains(listagg_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"table_group_aggregate\""),
          "LISTAGG WITHIN GROUP payload marker missing");
  Require(Contains(listagg_artifacts.envelope.payload,
                   "\"aggregate_function\":\"sb.aggregate.listagg\""),
          "LISTAGG WITHIN GROUP payload missing canonical function id");
  Require(Contains(listagg_artifacts.envelope.payload,
                   "\"aggregate_value_field\":\"flag\""),
          "LISTAGG WITHIN GROUP payload missing aggregate value field");
  Require(Contains(listagg_artifacts.envelope.payload,
                   "\"aggregate_binding_model\":\"engine_row_descriptor_field_text_nullable_route\""),
          "LISTAGG WITHIN GROUP payload missing text nullable binding model");
  Require(Contains(listagg_artifacts.envelope.payload, "\"order_by\":\"id\""),
          "LISTAGG WITHIN GROUP payload missing order field");
  Require(Contains(listagg_artifacts.envelope.payload, "\"order_column\":\"0\""),
          "LISTAGG WITHIN GROUP payload missing order column");
  Require(Contains(listagg_artifacts.envelope.payload, "\"listagg_separator\":\"|\""),
          "LISTAGG WITHIN GROUP payload missing separator option");
  Require(Contains(listagg_artifacts.envelope.payload, "\"listagg_overflow_mode\":\"truncate\""),
          "LISTAGG WITHIN GROUP payload missing overflow mode");
  Require(Contains(listagg_artifacts.envelope.payload,
                   "\"listagg_truncation_indicator\":\"...\""),
          "LISTAGG WITHIN GROUP payload missing truncation indicator");
  Require(Contains(listagg_artifacts.envelope.payload, "\"listagg_with_count\":\"false\""),
          "LISTAGG WITHIN GROUP payload missing WITHOUT COUNT option");
  Require(!Contains(listagg_artifacts.envelope.payload, "LISTAGG("),
          "LISTAGG WITHIN GROUP envelope carried source spelling");
  Require(!Contains(listagg_artifacts.envelope.payload, "WITHIN GROUP"),
          "LISTAGG WITHIN GROUP envelope embedded SQL syntax text");

  const auto listagg_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{listagg_artifacts.envelope.payload, false});
  Require(listagg_admission.admitted,
          "server admission rejected LISTAGG WITHIN GROUP route");
  Require(listagg_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for LISTAGG WITHIN GROUP");
  Require(listagg_admission.operation_id == "query.plan_operation",
          "server admission LISTAGG operation id mismatch");

  struct StringAggRouteCase {
    std::string_view sql;
    std::string_view source_spelling;
  };
  const StringAggRouteCase string_agg_routes[] = {
      {"SELECT dept, STRING_AGG(flag, '|' ORDER BY id) FROM sales GROUP BY dept",
       "STRING_AGG("},
      {"SELECT dept, sb.aggregate.string_agg(flag, '|' ORDER BY id) FROM sales GROUP BY dept",
       "sb.aggregate.string_agg("},
  };
  for (const auto& test : string_agg_routes) {
    const auto string_agg_artifacts = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(string_agg_artifacts.bound.bound,
            "STRING_AGG grouped route statement did not bind");
    Require(string_agg_artifacts.verifier.admitted,
            "STRING_AGG grouped route verifier rejected exact route");
    Require(string_agg_artifacts.envelope.operation_family == "sblr.query.relational.v3",
            "STRING_AGG grouped route operation family mismatch");
    Require(string_agg_artifacts.envelope.operation_id == "query.plan_operation",
            "STRING_AGG grouped route operation id mismatch");
    Require(string_agg_artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            "STRING_AGG grouped route opcode mismatch");
    Require(Contains(string_agg_artifacts.envelope.payload,
                     "\"query_envelope_kind\":\"table_group_aggregate\""),
            "STRING_AGG grouped route payload marker missing");
    Require(Contains(string_agg_artifacts.envelope.payload,
                     "\"aggregate_function\":\"sb.aggregate.string_agg\""),
            "STRING_AGG grouped route payload missing canonical function id");
    Require(Contains(string_agg_artifacts.envelope.payload,
                     "\"aggregate_value_field\":\"flag\""),
            "STRING_AGG grouped route payload missing aggregate value field");
    Require(Contains(string_agg_artifacts.envelope.payload,
                     "\"aggregate_binding_model\":\"engine_row_descriptor_field_text_nullable_route\""),
            "STRING_AGG grouped route payload missing text nullable binding model");
    Require(Contains(string_agg_artifacts.envelope.payload, "\"order_by\":\"id\""),
            "STRING_AGG grouped route payload missing order field");
    Require(Contains(string_agg_artifacts.envelope.payload, "\"order_column\":\"0\""),
            "STRING_AGG grouped route payload missing order column");
    Require(Contains(string_agg_artifacts.envelope.payload, "\"listagg_separator\":\"|\""),
            "STRING_AGG grouped route payload missing delimiter option");
    Require(!Contains(string_agg_artifacts.envelope.payload, std::string(test.source_spelling)),
            "STRING_AGG grouped route envelope carried source spelling");
    Require(!Contains(string_agg_artifacts.envelope.payload, "ORDER BY"),
            "STRING_AGG grouped route envelope embedded SQL syntax text");

    const auto string_agg_admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{string_agg_artifacts.envelope.payload, false});
    Require(string_agg_admission.admitted,
            "server admission rejected STRING_AGG grouped route");
    Require(string_agg_admission.requires_public_abi_dispatch,
            "server admission did not require public ABI dispatch for STRING_AGG grouped route");
    Require(string_agg_admission.operation_id == "query.plan_operation",
            "server admission STRING_AGG operation id mismatch");
  }

  struct StringAggRefusalCase {
    std::string_view sql;
    std::string_view reason;
  };
  const StringAggRefusalCase string_agg_refusals[] = {
      {"SELECT dept, STRING_AGG(flag) FROM sales GROUP BY dept",
       "string_agg_delimiter_literal_required"},
      {"SELECT dept, STRING_AGG(flag, '|') FROM sales GROUP BY dept",
       "string_agg_requires_order_by_current_route"},
      {"SELECT dept, STRING_AGG(flag, '|' ON OVERFLOW TRUNCATE '...') FROM sales GROUP BY dept",
       "string_agg_overflow_clause_not_supported_current_route"},
  };
  for (const auto& test : string_agg_refusals) {
    const auto refused = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(refused.bound.bound,
            "STRING_AGG fail-closed proof did not bind before route refusal");
    Require(refused.envelope.messages.has_errors() ||
                refused.verifier.messages.has_errors(),
            "STRING_AGG unsupported form did not fail closed");
    Require(DiagnosticsContain(refused.envelope.messages,
                               "SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED"),
            "STRING_AGG fail-closed proof missing grouped route diagnostic");
    Require(DiagnosticsContain(refused.envelope.messages, test.reason),
            "STRING_AGG fail-closed proof missing precise route reason");
    Require(!refused.verifier.admitted,
            "STRING_AGG unsupported form admitted exact route");
    Require(!Contains(refused.envelope.payload, "\"aggregate_function\""),
            "STRING_AGG fail-closed route emitted aggregate payload authority");
  }

  struct JsonAggRouteCase {
    std::string_view sql;
    std::string_view source_spelling;
  };
  const JsonAggRouteCase json_agg_routes[] = {
      {"SELECT dept, JSON_AGG(flag ORDER BY id) FROM sales GROUP BY dept",
       "JSON_AGG("},
      {"SELECT dept, sb.aggregate.json_agg(flag ORDER BY id) FROM sales GROUP BY dept",
       "sb.aggregate.json_agg("},
  };
  for (const auto& test : json_agg_routes) {
    const auto json_agg_artifacts = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(json_agg_artifacts.bound.bound,
            "JSON_AGG grouped route statement did not bind");
    Require(json_agg_artifacts.verifier.admitted,
            "JSON_AGG grouped route verifier rejected exact route");
    Require(json_agg_artifacts.envelope.operation_family == "sblr.query.relational.v3",
            "JSON_AGG grouped route operation family mismatch");
    Require(json_agg_artifacts.envelope.operation_id == "query.plan_operation",
            "JSON_AGG grouped route operation id mismatch");
    Require(json_agg_artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            "JSON_AGG grouped route opcode mismatch");
    Require(Contains(json_agg_artifacts.envelope.payload,
                     "\"query_envelope_kind\":\"table_group_aggregate\""),
            "JSON_AGG grouped route payload marker missing");
    Require(Contains(json_agg_artifacts.envelope.payload,
                     "\"aggregate_function\":\"sb.aggregate.json_agg\""),
            "JSON_AGG grouped route payload missing canonical function id");
    Require(Contains(json_agg_artifacts.envelope.payload,
                     "\"aggregate_value_field\":\"flag\""),
            "JSON_AGG grouped route payload missing aggregate value field");
    Require(Contains(json_agg_artifacts.envelope.payload,
                     "\"aggregate_binding_model\":\"engine_row_descriptor_field_json_nullable_route\""),
            "JSON_AGG grouped route payload missing json nullable binding model");
    Require(Contains(json_agg_artifacts.envelope.payload, "\"order_by\":\"id\""),
            "JSON_AGG grouped route payload missing order field");
    Require(Contains(json_agg_artifacts.envelope.payload, "\"order_column\":\"0\""),
            "JSON_AGG grouped route payload missing order column");
    Require(!Contains(json_agg_artifacts.envelope.payload, "\"listagg_separator\""),
            "JSON_AGG grouped route carried ordered text delimiter option");
    Require(!Contains(json_agg_artifacts.envelope.payload, std::string(test.source_spelling)),
            "JSON_AGG grouped route envelope carried source spelling");
    Require(!Contains(json_agg_artifacts.envelope.payload, "ORDER BY"),
            "JSON_AGG grouped route envelope embedded SQL syntax text");

    const auto json_agg_admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{json_agg_artifacts.envelope.payload, false});
    Require(json_agg_admission.admitted,
            "server admission rejected JSON_AGG grouped route");
    Require(json_agg_admission.requires_public_abi_dispatch,
            "server admission did not require public ABI dispatch for JSON_AGG grouped route");
    Require(json_agg_admission.operation_id == "query.plan_operation",
            "server admission JSON_AGG operation id mismatch");
  }

  struct JsonAggRefusalCase {
    std::string_view sql;
    std::string_view reason;
  };
  const JsonAggRefusalCase json_agg_refusals[] = {
      {"SELECT dept, JSON_AGG(flag) FROM sales GROUP BY dept",
       "json_agg_requires_order_by_current_route"},
      {"SELECT dept, JSON_AGG(DISTINCT flag ORDER BY id) FROM sales GROUP BY dept",
       "json_agg_distinct_requires_distinct_aggregate_execution_route"},
      {"SELECT dept, JSON_AGG(flag ORDER BY id) FILTER (WHERE flag) FROM sales GROUP BY dept",
       "json_agg_filter_clause_not_supported_current_route"},
      {"SELECT dept, JSON_AGG(flag ORDER BY id) OVER (PARTITION BY dept) FROM sales GROUP BY dept",
       "json_agg_window_bridge_not_supported_current_route"},
      {"SELECT dept, JSON_AGG(flag ORDER BY id) WITHIN GROUP (ORDER BY id) FROM sales GROUP BY dept",
       "json_agg_within_group_not_supported_current_route"},
  };
  for (const auto& test : json_agg_refusals) {
    const auto refused = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(refused.bound.bound,
            "JSON_AGG fail-closed proof did not bind before route refusal");
    Require(refused.envelope.messages.has_errors() ||
                refused.verifier.messages.has_errors(),
            "JSON_AGG unsupported form did not fail closed");
    Require(DiagnosticsContain(refused.envelope.messages,
                               "SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED"),
            "JSON_AGG fail-closed proof missing grouped route diagnostic");
    Require(DiagnosticsContain(refused.envelope.messages, test.reason),
            "JSON_AGG fail-closed proof missing precise route reason");
    Require(!refused.verifier.admitted,
            "JSON_AGG unsupported form admitted exact route");
    Require(!Contains(refused.envelope.payload, "\"aggregate_function\""),
            "JSON_AGG fail-closed route emitted aggregate payload authority");
  }

  struct JsonObjectAggRouteCase {
    std::string_view sql;
    std::string_view source_spelling;
  };
  const JsonObjectAggRouteCase json_object_agg_routes[] = {
      {"SELECT dept, JSON_OBJECT_AGG(id, flag ORDER BY id) FROM sales GROUP BY dept",
       "JSON_OBJECT_AGG("},
      {"SELECT dept, sb.aggregate.json_object_agg(id, flag ORDER BY id) FROM sales GROUP BY dept",
       "sb.aggregate.json_object_agg("},
  };
  for (const auto& test : json_object_agg_routes) {
    const auto artifacts = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(artifacts.bound.bound,
            "JSON_OBJECT_AGG grouped route statement did not bind");
    Require(artifacts.verifier.admitted,
            "JSON_OBJECT_AGG grouped route verifier rejected exact route");
    Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
            "JSON_OBJECT_AGG grouped route operation family mismatch");
    Require(artifacts.envelope.operation_id == "query.plan_operation",
            "JSON_OBJECT_AGG grouped route operation id mismatch");
    Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            "JSON_OBJECT_AGG grouped route opcode mismatch");
    Require(Contains(artifacts.envelope.payload,
                     "\"query_envelope_kind\":\"table_group_aggregate\""),
            "JSON_OBJECT_AGG grouped route payload marker missing");
    Require(Contains(artifacts.envelope.payload,
                     "\"aggregate_function\":\"sb.aggregate.json_object_agg\""),
            "JSON_OBJECT_AGG grouped route payload missing canonical function id");
    Require(Contains(artifacts.envelope.payload,
                     "\"aggregate_value_field\":\"id\""),
            "JSON_OBJECT_AGG grouped route payload missing key field");
    Require(Contains(artifacts.envelope.payload,
                     "\"aggregate_pair_value_field\":\"flag\""),
            "JSON_OBJECT_AGG grouped route payload missing value field");
    Require(Contains(artifacts.envelope.payload,
                     "\"aggregate_binding_model\":\"engine_row_descriptor_field_json_nullable_route\""),
            "JSON_OBJECT_AGG grouped route payload missing json nullable binding model");
    Require(Contains(artifacts.envelope.payload, "\"order_by\":\"id\""),
            "JSON_OBJECT_AGG grouped route payload missing order field");
    Require(Contains(artifacts.envelope.payload, "\"order_column\":\"0\""),
            "JSON_OBJECT_AGG grouped route payload missing order column");
    Require(Contains(artifacts.envelope.payload, "\"aggregate_pair_value_column\":\"2\""),
            "JSON_OBJECT_AGG grouped route payload missing value column");
    Require(!Contains(artifacts.envelope.payload, "\"listagg_separator\""),
            "JSON_OBJECT_AGG grouped route carried ordered text delimiter option");
    Require(!Contains(artifacts.envelope.payload, std::string(test.source_spelling)),
            "JSON_OBJECT_AGG grouped route envelope carried source spelling");
    Require(!Contains(artifacts.envelope.payload, "ORDER BY"),
            "JSON_OBJECT_AGG grouped route envelope embedded SQL syntax text");

    const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
    Require(admission.admitted,
            "server admission rejected JSON_OBJECT_AGG grouped route");
    Require(admission.requires_public_abi_dispatch,
            "server admission did not require public ABI dispatch for JSON_OBJECT_AGG grouped route");
    Require(admission.operation_id == "query.plan_operation",
            "server admission JSON_OBJECT_AGG operation id mismatch");
  }

  struct JsonObjectAggRefusalCase {
    std::string_view sql;
    std::string_view reason;
  };
  const JsonObjectAggRefusalCase json_object_agg_refusals[] = {
      {"SELECT dept, JSON_OBJECT_AGG(id, flag) FROM sales GROUP BY dept",
       "json_object_agg_requires_order_by_current_route"},
      {"SELECT dept, JSON_OBJECT_AGG(DISTINCT id, flag ORDER BY id) FROM sales GROUP BY dept",
       "json_object_agg_distinct_requires_distinct_aggregate_execution_route"},
      {"SELECT dept, JSON_OBJECT_AGG(id, flag ORDER BY id) FILTER (WHERE flag) FROM sales GROUP BY dept",
       "json_object_agg_filter_clause_not_supported_current_route"},
      {"SELECT dept, JSON_OBJECT_AGG(id, flag ORDER BY id) OVER (PARTITION BY dept) FROM sales GROUP BY dept",
       "json_object_agg_window_bridge_not_supported_current_route"},
      {"SELECT dept, JSON_OBJECT_AGG(id, flag ORDER BY id) WITHIN GROUP (ORDER BY id) FROM sales GROUP BY dept",
       "json_object_agg_within_group_not_supported_current_route"},
  };
  for (const auto& test : json_object_agg_refusals) {
    const auto refused = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(refused.bound.bound,
            "JSON_OBJECT_AGG fail-closed proof did not bind before route refusal");
    Require(refused.envelope.messages.has_errors() ||
                refused.verifier.messages.has_errors(),
            "JSON_OBJECT_AGG unsupported form did not fail closed");
    Require(DiagnosticsContain(refused.envelope.messages,
                               "SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED"),
            "JSON_OBJECT_AGG fail-closed proof missing grouped route diagnostic");
    Require(DiagnosticsContain(refused.envelope.messages, test.reason),
            "JSON_OBJECT_AGG fail-closed proof missing precise route reason");
    Require(!refused.verifier.admitted,
            "JSON_OBJECT_AGG unsupported form admitted exact route");
    Require(!Contains(refused.envelope.payload, "\"aggregate_function\""),
            "JSON_OBJECT_AGG fail-closed route emitted aggregate payload authority");
  }

  struct ArrayAggRouteCase {
    std::string_view sql;
    std::string_view source_spelling;
  };
  const ArrayAggRouteCase array_agg_routes[] = {
      {"SELECT dept, ARRAY_AGG(flag ORDER BY id) FROM sales GROUP BY dept",
       "ARRAY_AGG("},
      {"SELECT dept, sb.aggregate.array_agg(flag ORDER BY id) FROM sales GROUP BY dept",
       "sb.aggregate.array_agg("},
  };
  for (const auto& test : array_agg_routes) {
    const auto array_agg_artifacts = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(array_agg_artifacts.bound.bound,
            "ARRAY_AGG grouped route statement did not bind");
    Require(array_agg_artifacts.verifier.admitted,
            "ARRAY_AGG grouped route verifier rejected exact route");
    Require(array_agg_artifacts.envelope.operation_family == "sblr.query.relational.v3",
            "ARRAY_AGG grouped route operation family mismatch");
    Require(array_agg_artifacts.envelope.operation_id == "query.plan_operation",
            "ARRAY_AGG grouped route operation id mismatch");
    Require(array_agg_artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            "ARRAY_AGG grouped route opcode mismatch");
    Require(Contains(array_agg_artifacts.envelope.payload,
                     "\"query_envelope_kind\":\"table_group_aggregate\""),
            "ARRAY_AGG grouped route payload marker missing");
    Require(Contains(array_agg_artifacts.envelope.payload,
                     "\"aggregate_function\":\"sb.aggregate.array_agg\""),
            "ARRAY_AGG grouped route payload missing canonical function id");
    Require(Contains(array_agg_artifacts.envelope.payload,
                     "\"aggregate_value_field\":\"flag\""),
            "ARRAY_AGG grouped route payload missing aggregate value field");
    Require(Contains(array_agg_artifacts.envelope.payload,
                     "\"aggregate_binding_model\":\"engine_row_descriptor_field_list_nullable_route\""),
            "ARRAY_AGG grouped route payload missing list nullable binding model");
    Require(Contains(array_agg_artifacts.envelope.payload, "\"order_by\":\"id\""),
            "ARRAY_AGG grouped route payload missing order field");
    Require(Contains(array_agg_artifacts.envelope.payload, "\"order_column\":\"0\""),
            "ARRAY_AGG grouped route payload missing order column");
    Require(!Contains(array_agg_artifacts.envelope.payload, "\"listagg_separator\""),
            "ARRAY_AGG grouped route carried ordered text delimiter option");
    Require(!Contains(array_agg_artifacts.envelope.payload, std::string(test.source_spelling)),
            "ARRAY_AGG grouped route envelope carried source spelling");
    Require(!Contains(array_agg_artifacts.envelope.payload, "ORDER BY"),
            "ARRAY_AGG grouped route envelope embedded SQL syntax text");

    const auto array_agg_admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{array_agg_artifacts.envelope.payload, false});
    Require(array_agg_admission.admitted,
            "server admission rejected ARRAY_AGG grouped route");
    Require(array_agg_admission.requires_public_abi_dispatch,
            "server admission did not require public ABI dispatch for ARRAY_AGG grouped route");
    Require(array_agg_admission.operation_id == "query.plan_operation",
            "server admission ARRAY_AGG operation id mismatch");
  }

  struct ArrayAggRefusalCase {
    std::string_view sql;
    std::string_view reason;
  };
  const ArrayAggRefusalCase array_agg_refusals[] = {
      {"SELECT dept, ARRAY_AGG(flag) FROM sales GROUP BY dept",
       "array_agg_requires_order_by_current_route"},
      {"SELECT dept, ARRAY_AGG(DISTINCT flag ORDER BY id) FROM sales GROUP BY dept",
       "array_agg_distinct_requires_distinct_aggregate_execution_route"},
      {"SELECT dept, ARRAY_AGG(flag ORDER BY id) FILTER (WHERE flag) FROM sales GROUP BY dept",
       "array_agg_filter_clause_not_supported_current_route"},
      {"SELECT dept, ARRAY_AGG(flag ORDER BY id) OVER (PARTITION BY dept) FROM sales GROUP BY dept",
       "array_agg_window_bridge_not_supported_current_route"},
      {"SELECT dept, ARRAY_AGG(flag ORDER BY id) WITHIN GROUP (ORDER BY id) FROM sales GROUP BY dept",
       "array_agg_within_group_not_supported_current_route"},
  };
  for (const auto& test : array_agg_refusals) {
    const auto refused = RunPipeline(test.sql, {std::string(kTargetUuid)});
    Require(refused.bound.bound,
            "ARRAY_AGG fail-closed proof did not bind before route refusal");
    Require(refused.envelope.messages.has_errors() ||
                refused.verifier.messages.has_errors(),
            "ARRAY_AGG unsupported form did not fail closed");
    Require(DiagnosticsContain(refused.envelope.messages,
                               "SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED"),
            "ARRAY_AGG fail-closed proof missing grouped route diagnostic");
    Require(DiagnosticsContain(refused.envelope.messages, test.reason),
            "ARRAY_AGG fail-closed proof missing precise route reason");
    Require(!refused.verifier.admitted,
            "ARRAY_AGG unsupported form admitted exact route");
    Require(!Contains(refused.envelope.payload, "\"aggregate_function\""),
            "ARRAY_AGG fail-closed route emitted aggregate payload authority");
  }
}

void RequireTableCountLowering() {
  const auto count_all = RunPipeline("SELECT COUNT(*) FROM customer",
                                     {std::string(kTargetUuid)});
  Require(count_all.bound.bound, "COUNT(*) statement did not bind");
  Require(count_all.verifier.admitted, "COUNT(*) verifier rejected exact route");
  Require(count_all.envelope.operation_family == "sblr.query.relational.v3",
          "COUNT(*) operation family mismatch");
  Require(count_all.envelope.operation_id == "query.plan_operation",
          "COUNT(*) operation id mismatch");
  Require(count_all.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "COUNT(*) opcode mismatch");
  Require(HasValue(count_all.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "COUNT(*) query plan authority step missing");
  Require(HasValue(count_all.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          "COUNT(*) MGA visibility authority step missing");
  Require(Contains(count_all.envelope.payload,
                   "\"query_envelope_kind\":\"table_count\""),
          "COUNT(*) payload missing table_count route marker");
  Require(Contains(count_all.envelope.payload,
                   "\"query_operation\":\"count_all\""),
          "COUNT(*) payload missing count operation");
  Require(Contains(count_all.envelope.payload,
                   "\"aggregate_function\":\"sb.aggregate.count\""),
          "COUNT(*) payload missing canonical aggregate function");
  Require(Contains(count_all.envelope.payload,
                   "\"target_object_uuid\":\"" + std::string(kTargetUuid) + "\""),
          "COUNT(*) payload missing target UUID");
  Require(!Contains(count_all.envelope.payload, "customer"),
          "COUNT(*) payload embedded source table name");
  Require(!Contains(count_all.envelope.payload, "SELECT COUNT"),
          "COUNT(*) payload embedded SQL text");

  const auto count_field = RunPipeline("SELECT COUNT(id) FROM customer",
                                       {std::string(kTargetUuid)});
  Require(count_field.bound.bound, "COUNT(field) statement did not bind");
  Require(count_field.verifier.admitted, "COUNT(field) verifier rejected exact route");
  Require(Contains(count_field.envelope.payload,
                   "\"query_envelope_kind\":\"table_count\""),
          "COUNT(field) payload missing table_count route marker");
  Require(Contains(count_field.envelope.payload,
                   "\"aggregate_value_field\":\"id\""),
          "COUNT(field) payload missing aggregate field binding");
  Require(Contains(count_field.envelope.payload,
                   "\"count_all\":false"),
          "COUNT(field) payload did not mark non-star count");

  const auto count_like = RunPipeline(
      "SELECT 'SBDFS-100-002' AS assertion_id, COUNT(*) AS actual_full_route_rows, "
      "2560 AS expected_full_route_rows FROM customer WHERE route_set LIKE '%full_route%'",
      {std::string(kTargetUuid)});
  Require(count_like.bound.bound, "COUNT LIKE assertion statement did not bind");
  Require(count_like.verifier.admitted, "COUNT LIKE assertion verifier rejected exact route");
  Require(Contains(count_like.envelope.payload,
                   "\"query_envelope_kind\":\"table_count\""),
          "COUNT LIKE assertion payload missing table_count route marker");
  Require(Contains(count_like.envelope.payload,
                   "\"result_projection\":\"count_assertion\""),
          "COUNT LIKE assertion payload missing assertion projection marker");
  Require(Contains(count_like.envelope.payload, "\"predicate_kind\":\"column_like\""),
          "COUNT LIKE assertion payload missing LIKE predicate kind");
  Require(Contains(count_like.envelope.payload, "\"predicate_column\":\"route_set\""),
          "COUNT LIKE assertion payload missing predicate column");
  Require(Contains(count_like.envelope.payload, "\"predicate_value\":\"%full_route%\""),
          "COUNT LIKE assertion payload missing predicate value");
  Require(!Contains(count_like.envelope.payload, "SELECT 'SBDFS-100-002'"),
          "COUNT LIKE assertion payload embedded SQL text");

  const auto count_not_in = RunPipeline(
      "SELECT 'SBDFS-100-004' AS assertion_id, COUNT(*) AS actual_statement_surface_rows, "
      "1083 AS expected_statement_surface_rows FROM customer "
      "WHERE surface_kind NOT IN ('function', 'operator', 'variable')",
      {std::string(kTargetUuid)});
  Require(count_not_in.bound.bound, "COUNT NOT IN assertion statement did not bind");
  Require(count_not_in.verifier.admitted,
          "COUNT NOT IN assertion verifier rejected exact route");
  Require(Contains(count_not_in.envelope.payload,
                   "\"query_envelope_kind\":\"table_count\""),
          "COUNT NOT IN assertion payload missing table_count route marker");
  Require(Contains(count_not_in.envelope.payload,
                   "\"result_projection\":\"count_assertion\""),
          "COUNT NOT IN assertion payload missing assertion projection marker");
  Require(Contains(count_not_in.envelope.payload,
                   "\"predicate_kind\":\"column_not_in_list\""),
          "COUNT NOT IN assertion payload missing NOT IN predicate kind");
  Require(Contains(count_not_in.envelope.payload, "\"predicate_column\":\"surface_kind\""),
          "COUNT NOT IN assertion payload missing predicate column");
  Require(Contains(count_not_in.envelope.payload,
                   "\"predicate_value\":\"function,operator,variable\""),
          "COUNT NOT IN assertion payload missing predicate values");
  Require(!Contains(count_not_in.envelope.payload, "SELECT 'SBDFS-100-004'"),
          "COUNT NOT IN assertion payload embedded SQL text");

  const auto catalog_count = RunPipeline(
      "SELECT 'SBDFS-056-007' AS assertion_id, "
      "COUNT(*) AS actual_remaining_proc_tables, "
      "0 AS expected_remaining_proc_tables "
      "FROM sys.tables "
      "WHERE table_name = 'proc_tasks' "
      "AND schema_id IN (SELECT schema_id FROM sys.schemas "
      "WHERE schema_name = 'users.public.p5_proc')");
  if (!catalog_count.verifier.admitted) {
    PrintMessageSet(catalog_count.bound.messages);
    PrintMessageSet(catalog_count.envelope.messages);
    PrintMessageSet(catalog_count.verifier.messages);
  }
  Require(catalog_count.verifier.admitted,
          "catalog COUNT subquery verifier rejected exact route");
  Require(catalog_count.envelope.operation_family == "sblr.observability.inspect.v3",
          "catalog COUNT subquery operation family mismatch");
  Require(catalog_count.envelope.operation_id == "observability.show_catalog",
          "catalog COUNT subquery operation id mismatch");
  Require(Contains(catalog_count.envelope.payload,
                   "\"observability_envelope_kind\":\"catalog_projection_count\""),
          "catalog COUNT subquery payload missing catalog count marker");
  Require(Contains(catalog_count.envelope.payload,
                   "\"catalog_projection\":\"sys.tables\""),
          "catalog COUNT subquery payload missing sys.tables projection");
  Require(Contains(catalog_count.envelope.payload,
                   "\"predicate_kind\":\"column_in_projection\""),
          "catalog COUNT subquery payload missing column_in_projection predicate");
  Require(Contains(catalog_count.envelope.payload,
                   "\"predicate_column\":\"schema_id\""),
          "catalog COUNT subquery payload missing outer predicate column");
  Require(Contains(catalog_count.envelope.payload,
                   "\"additional_predicate_kind\":\"column_equals\""),
          "catalog COUNT subquery payload missing additional equality predicate kind");
  Require(Contains(catalog_count.envelope.payload,
                   "\"additional_predicate_column\":\"table_name\""),
          "catalog COUNT subquery payload missing additional equality predicate column");
  Require(Contains(catalog_count.envelope.payload,
                   "\"additional_predicate_value\":\"proc_tasks\""),
          "catalog COUNT subquery payload missing additional equality predicate value");
  Require(Contains(catalog_count.envelope.payload,
                   "\"subquery_projection\":\"sys.schemas\""),
          "catalog COUNT subquery payload missing sys.schemas subquery projection");
  Require(Contains(catalog_count.envelope.payload,
                   "\"subquery_predicate_column\":\"schema_name\""),
          "catalog COUNT subquery payload missing subquery predicate column");
  Require(Contains(catalog_count.envelope.payload,
                   "\"subquery_predicate_value\":\"users.public.p5_proc\""),
          "catalog COUNT subquery payload missing subquery predicate value");
  Require(!Contains(catalog_count.envelope.payload, "SELECT 'SBDFS-056-007'"),
          "catalog COUNT subquery payload embedded SQL text");
}

void RequireMaterializedCteLowering() {
  const auto artifacts = RunPipeline(
      "WITH c AS (SELECT * FROM customer) SELECT * FROM c",
      {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "materialized CTE statement did not bind");
  Require(artifacts.verifier.admitted, "materialized CTE verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "materialized CTE operation family mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "materialized CTE operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "materialized CTE opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "materialized CTE query plan authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          "materialized CTE MGA visibility authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.query.cte_descriptor"),
          "materialized CTE descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_materialized_cte\""),
          "materialized CTE payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"query_operation\":\"materialized_cte\""),
          "materialized CTE payload missing query operation");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "materialized CTE payload missing source table UUID");
  Require(Contains(artifacts.envelope.payload, "\"cte_strategy\":\"materialized\""),
          "materialized CTE payload missing strategy");
  Require(Contains(artifacts.envelope.payload, "\"cte_binding_model\":\"single_nonrecursive_cte_uuid_source_current_route\""),
          "materialized CTE payload missing binding model");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":true"),
          "materialized CTE did not claim source relation requirement");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":true"),
          "materialized CTE did not declare row storage access");
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          "materialized CTE claimed object name text");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "materialized CTE envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "WITH c"),
          "materialized CTE envelope embedded SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected materialized CTE route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for materialized CTE");
  Require(admission.operation_id == "query.plan_operation",
          "server admission materialized CTE operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission materialized CTE family mismatch");

  for (const auto& row : kBoundedMaterializedCteGrammarRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr,
            CteEvidenceMessage(row, "registry", "missing generated registry row"));
    Require(registry_row->canonical_name == row.canonical_name,
            CteEvidenceMessage(row, "registry", "canonical name mismatch"));
    Require(registry_row->surface_kind == "grammar_production",
            CteEvidenceMessage(row, "registry", "surface kind mismatch"));
    Require(registry_row->family == row.family,
            CteEvidenceMessage(row, "registry", "family mismatch"));
    Require(registry_row->source_status == "native_now",
            CteEvidenceMessage(row, "registry", "source status mismatch"));
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            CteEvidenceMessage(row, "registry", "cluster scope mismatch"));
    Require(registry_row->sblr_operation_family == row.canonical_surface_family,
            CteEvidenceMessage(row, "registry", "canonical SBLR family mismatch"));
    Require(registry_row->parser_handler_key == row.parser_handler_key,
            CteEvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
    Require(registry_row->lowering_handler_key == row.lowering_handler_key,
            CteEvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
    Require(registry_row->server_admission_key == row.server_admission_key,
            CteEvidenceMessage(row, "server_admission", "server admission key mismatch"));
    Require(registry_row->engine_rule_key == row.engine_rule_key,
            CteEvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
    Require(registry_row->validation_fixture_id == row.validation_fixture_id,
            CteEvidenceMessage(row, "registry", "validation fixture id mismatch"));
    Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
            CteEvidenceMessage(row, "lowering", "route family mismatch"));
    Require(artifacts.envelope.operation_id == "query.plan_operation",
            CteEvidenceMessage(row, "lowering", "operation id mismatch"));
    Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
            CteEvidenceMessage(row, "lowering", "opcode mismatch"));
    Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_materialized_cte\""),
            CteEvidenceMessage(row, "fixture", "materialized CTE payload marker missing"));
    Require(Contains(artifacts.envelope.payload, "\"cte_strategy\":\"materialized\""),
            CteEvidenceMessage(row, "fixture", "materialized CTE strategy missing"));
    Require(!Contains(artifacts.envelope.payload, "customer"),
            CteEvidenceMessage(row, "fixture", "payload embedded source table name"));
    Require(!Contains(artifacts.envelope.payload, "WITH c"),
            CteEvidenceMessage(row, "fixture", "payload embedded SQL text"));
  }
}

void RequireExplainWithCteAliasLowering() {
  const auto artifacts = RunPipeline(
      "EXPLAIN WITH recent AS (VALUES (1)) SELECT * FROM recent");
  Require(artifacts.bound.bound, "EXPLAIN WITH local CTE alias did not bind");
  Require(artifacts.verifier.admitted,
          "EXPLAIN WITH local CTE alias verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.observability.inspect.v3",
          "EXPLAIN WITH operation family mismatch");
  Require(artifacts.envelope.operation_id == "observability.explain_operation",
          "EXPLAIN WITH operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_OBSERVABILITY_EXPLAIN_OPERATION",
          "EXPLAIN WITH opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "EXPLAIN WITH parser SQL execution boundary missing");
  Require(!DiagnosticsContain(artifacts.envelope.messages,
                              "SBSQL.NAME_RESOLUTION"),
          "EXPLAIN WITH local CTE alias produced name-resolution diagnostic");
  Require(!Contains(artifacts.envelope.payload, "recent"),
          "EXPLAIN WITH envelope embedded local CTE alias text");
}

void RequireRecursiveCteLowering() {
  const auto artifacts = RunPipeline(
      "WITH RECURSIVE c(n) AS (VALUES (1) UNION VALUES (2), (3)) SELECT * FROM c");
  Require(artifacts.bound.bound, "recursive CTE statement did not bind");
  Require(artifacts.verifier.admitted, "recursive CTE verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "recursive CTE operation family mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "recursive CTE operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "recursive CTE opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "recursive CTE query plan authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "recursive CTE parser SQL execution boundary missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "recursive CTE parser finality boundary missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.query.recursive_cte_descriptor"),
          "recursive CTE descriptor ref missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.query.values_rowset_descriptor"),
          "recursive CTE values descriptor ref missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"values_recursive_cte\""),
          "recursive CTE payload marker missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"query_operation\":\"recursive_cte\""),
          "recursive CTE payload missing query operation");
  Require(Contains(artifacts.envelope.payload,
                   "\"cte_strategy\":\"recursive_fixed_point_materialized\""),
          "recursive CTE payload missing fixed-point strategy");
  Require(Contains(artifacts.envelope.payload,
                   "\"cte_binding_model\":\"values_anchor_recursive_step_fixed_point_current_route\""),
          "recursive CTE payload missing binding model");
  Require(Contains(artifacts.envelope.payload, "\"relation_count\":\"2\""),
          "recursive CTE payload missing relation count");
  Require(Contains(artifacts.envelope.payload, "\"relation_0_row_count\":\"1\""),
          "recursive CTE payload missing anchor row count");
  Require(Contains(artifacts.envelope.payload, "\"relation_1_row_count\":\"2\""),
          "recursive CTE payload missing recursive row count");
  Require(Contains(artifacts.envelope.payload, "\"values_column_count\":\"1\""),
          "recursive CTE payload missing column count");
  Require(Contains(artifacts.envelope.payload, "\"relation_0_0_0_name\":\"n\""),
          "recursive CTE payload missing column alias on anchor relation");
  Require(Contains(artifacts.envelope.payload, "\"relation_0_0_0_value\":\"1\""),
          "recursive CTE payload missing anchor value");
  Require(Contains(artifacts.envelope.payload, "\"relation_1_1_0_value\":\"3\""),
          "recursive CTE payload missing recursive value");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":false"),
          "recursive CTE claimed source relation requirement");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "recursive CTE claimed row storage access");
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          "recursive CTE claimed object name text");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "recursive CTE claimed SQL text");
  Require(!Contains(artifacts.envelope.payload, "WITH RECURSIVE"),
          "recursive CTE envelope embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected recursive CTE route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for recursive CTE");
  Require(admission.operation_id == "query.plan_operation",
          "server admission recursive CTE operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission recursive CTE family mismatch");

  const auto duplicate_preserving = RunPipeline(
      "WITH RECURSIVE c(n) AS (VALUES (1) UNION ALL VALUES (1)) SELECT * FROM c");
  Require(!duplicate_preserving.verifier.admitted,
          "recursive CTE UNION ALL duplicate-preserving route did not fail closed");
  Require(duplicate_preserving.envelope.messages.has_errors(),
          "recursive CTE UNION ALL route did not report parser message vector");
}

void RequireScalarSubqueryLowering() {
  const auto artifacts = RunPipeline(
      "SELECT (SELECT id FROM customer)",
      {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "scalar subquery statement did not bind");
  if (!artifacts.verifier.admitted) {
    for (const auto& diagnostic : artifacts.verifier.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.name << '=' << field.value << '\n';
      }
    }
    std::cerr << artifacts.envelope.payload << '\n';
  }
  Require(artifacts.verifier.admitted, "scalar subquery verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "scalar subquery operation family mismatch");
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          "scalar subquery operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          "scalar subquery opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          "scalar subquery query plan authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          "scalar subquery MGA visibility authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.query.subquery_descriptor"),
          "scalar subquery descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_scalar_subquery\""),
          "scalar subquery payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"query_operation\":\"scalar_subquery\""),
          "scalar subquery payload missing query operation");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "scalar subquery payload missing source table UUID");
  Require(Contains(artifacts.envelope.payload, "\"projected_field\":\"id\""),
          "scalar subquery payload missing projected field");
  Require(Contains(artifacts.envelope.payload, "\"project_columns\":\"0\""),
          "scalar subquery payload missing projected column");
  Require(Contains(artifacts.envelope.payload, "\"subquery_cardinality_model\":\"first_value_current_route\""),
          "scalar subquery payload missing cardinality model");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":true"),
          "scalar subquery did not claim source relation requirement");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":true"),
          "scalar subquery did not declare row storage access");
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          "scalar subquery claimed object name text");
  Require(!Contains(artifacts.envelope.payload, "customer"),
          "scalar subquery envelope embedded source table name");
  Require(!Contains(artifacts.envelope.payload, "SELECT ("),
          "scalar subquery envelope embedded SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected scalar subquery route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for scalar subquery");
  Require(admission.operation_id == "query.plan_operation",
          "server admission scalar subquery operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission scalar subquery family mismatch");
}

void RequireHavingClauseLowering() {
  const auto& row = kBoundedHavingClauseRow;
  RequireHavingClauseRegistryEvidence(row);

  const auto artifacts = RunPipeline(row.sql_fixture, {std::string(kTargetUuid)});
  Require(artifacts.bound.bound,
          HavingClauseEvidenceMessage(row, "binder", "HAVING statement did not bind"));
  Require(artifacts.verifier.admitted,
          HavingClauseEvidenceMessage(row, "lowering", "HAVING verifier rejected exact route"));
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          HavingClauseEvidenceMessage(row, "lowering", "HAVING operation family mismatch"));
  Require(artifacts.envelope.operation_id == "query.plan_operation",
          HavingClauseEvidenceMessage(row, "lowering", "HAVING operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_PLAN_OPERATION",
          HavingClauseEvidenceMessage(row, "lowering", "HAVING opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_plan_api_required"),
          HavingClauseEvidenceMessage(row, "authority", "HAVING query plan authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_visibility_required"),
          HavingClauseEvidenceMessage(row, "authority", "HAVING MGA visibility authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          HavingClauseEvidenceMessage(row, "authority", "HAVING parser SQL execution boundary missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.query.aggregate_descriptor"),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING aggregate descriptor ref missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.storage.row_descriptor"),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING row descriptor ref missing"));
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"table_group_sum\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING group sum payload marker missing"));
  Require(Contains(artifacts.envelope.payload, "\"query_operation\":\"group_by\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING query operation missing"));
  Require(Contains(artifacts.envelope.payload, "\"group_key_field\":\"id\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING group key missing"));
  Require(Contains(artifacts.envelope.payload, "\"aggregate_function\":\"sb.aggregate.sum\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING aggregate function missing"));
  Require(Contains(artifacts.envelope.payload, "\"aggregate_value_field\":\"total\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING aggregate value field missing"));
  Require(Contains(artifacts.envelope.payload, "\"having_predicate\":\"aggregate_gt\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING predicate metadata missing"));
  Require(Contains(artifacts.envelope.payload, "\"having_threshold\":\"1\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING threshold metadata missing"));
  Require(Contains(artifacts.envelope.payload, "\"having_aggregate_function\":\"sb.aggregate.sum\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING aggregate function metadata missing"));
  Require(Contains(artifacts.envelope.payload, "\"having_value_field\":\"total\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING value field metadata missing"));
  Require(Contains(artifacts.envelope.payload, "\"having_value_column\":\"1\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING value column metadata missing"));
  Require(Contains(artifacts.envelope.payload, "\"having_group_key_field\":\"id\""),
          HavingClauseEvidenceMessage(row, "lowering", "HAVING group key metadata missing"));
  Require(Contains(artifacts.envelope.payload, "\"object_name_text_included\":false"),
          HavingClauseEvidenceMessage(row, "fixture", "HAVING claimed object name text"));
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          HavingClauseEvidenceMessage(row, "fixture", "HAVING claimed SQL text"));
  Require(!Contains(artifacts.envelope.payload, "customer"),
          HavingClauseEvidenceMessage(row, "fixture", "HAVING envelope embedded source table name"));
  Require(!Contains(artifacts.envelope.payload, "SELECT id"),
          HavingClauseEvidenceMessage(row, "fixture", "HAVING envelope embedded source SQL text"));
  Require(!Contains(artifacts.envelope.payload, "HAVING"),
          HavingClauseEvidenceMessage(row, "fixture", "HAVING envelope embedded clause text"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          HavingClauseEvidenceMessage(row, "server_admission", "server admission rejected HAVING route"));
  Require(admission.requires_public_abi_dispatch,
          HavingClauseEvidenceMessage(row, "server_admission", "HAVING did not require public ABI dispatch"));
  Require(admission.operation_id == "query.plan_operation",
          HavingClauseEvidenceMessage(row, "server_admission", "HAVING operation id mismatch"));
  Require(admission.operation_family == "sblr.query.relational.v3",
          HavingClauseEvidenceMessage(row, "server_admission", "HAVING operation family mismatch"));
}

void RequireUnsupportedQueryFamiliesFailClosed() {
  constexpr std::string_view kUnsupportedSql[] = {
      "SELECT * FROM customer LEFT JOIN orders ON customer.id = orders.id",
      "SELECT * FROM customer JOIN orders ON customer.note = orders.note",
      "SELECT id, SUM(DISTINCT total) FROM customer GROUP BY id",
      "SELECT id, SUM(total) FROM customer GROUP BY note",
      "SELECT * FROM customer UNION BY ORDINAL SELECT * FROM customer_archive",
      "SELECT row_number() OVER (PARTITION BY note ORDER BY id) FROM customer",
      "SELECT lag(id, 2) OVER (ORDER BY id) FROM customer",
      "SELECT ntile(0) OVER (ORDER BY id) FROM customer",
      "SELECT nth_value(id, 0) OVER (ORDER BY id) FROM customer",
      "WITH RECURSIVE c AS (SELECT * FROM customer) SELECT * FROM c",
      "WITH c(id) AS (SELECT * FROM customer) SELECT * FROM c",
      "WITH c AS (SELECT id FROM customer) SELECT * FROM c",
      "WITH c AS (SELECT * FROM customer) SELECT id FROM c",
      "SELECT (SELECT note FROM customer)",
      "SELECT (SELECT id FROM customer WHERE id = 1)",
      "SELECT (SELECT id FROM customer), 1",
      "SELECT * FROM customer WHERE id > 1",
      "SELECT * FROM customer WHERE id = 1 AND note = 'x'",
      "SELECT * FROM customer WHERE EXISTS (SELECT id FROM customer)"};
  for (const auto sql : kUnsupportedSql) {
    const auto artifacts = RunPipeline(sql, {std::string(kTargetUuid)});
    Require(!artifacts.bound.bound || artifacts.envelope.messages.has_errors() ||
                artifacts.verifier.messages.has_errors(),
            std::string("unsupported query row family did not fail closed: ") +
                std::string(sql));
  }
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireRegistryEvidence();
  RequireExactLowering("SELECT * FROM customer",
                       "sblr.query.relational.v3",
                       "dml.select_rows",
                       "SBLR_DML_SELECT_ROWS",
                       "right.read",
                       "select");
  RequireExactLowering("INSERT INTO customer VALUES (1)",
                       "sblr.dml.operation.v3",
                       "dml.insert_rows",
                       "SBLR_DML_INSERT_ROWS",
                       "right.write",
                       "insert");
  PrepareEngineDispatchContext();
  RequireInsertSourceExactRouteEvidence();
  RequireInsertValuesKeywordStringLiteralEvidence();
  RequireExactLowering("UPDATE customer SET name = 'x'",
                       "sblr.dml.operation.v3",
                       "dml.update_rows",
                       "SBLR_DML_UPDATE_ROWS",
                       "right.write",
                       "update");
  RequireExactLowering("DELETE FROM customer",
                       "sblr.dml.operation.v3",
                       "dml.delete_rows",
                       "SBLR_DML_DELETE_ROWS",
                       "right.write",
                       "delete");
  RequireExactLowering("MERGE INTO customer USING staging ON customer.id = staging.id WHEN MATCHED THEN UPDATE SET name = staging.name",
                       "sblr.dml.operation.v3",
                       "dml.merge_rows",
                       "SBLR_DML_MERGE_ROWS",
                       "right.write",
                       "merge");
  RequireExactLowering("UPSERT INTO customer VALUES (1)",
                       "sblr.dml.operation.v3",
                       "dml.insert_rows",
                       "SBLR_DML_INSERT_ROWS",
                       "right.write",
                       "upsert");
  RequireExactLowering("COPY customer FROM STDIN",
                       "sblr.dml.operation.v3",
                       "dml.plan_import_rows",
                       "SBLR_DML_PLAN_IMPORT_ROWS",
                       "right.write",
                       "copy_import_export");
  RequireCopySourceExactRouteEvidence();
  RequireCopyOptionsExactRouteEvidence();
  RequireCopyFormatExactRouteEvidence();
  RequireCopyEndpointExactRouteEvidence();
  RequireSimpleSelectSkeletonLowering();
  RequireSelectOrderLimitLowering();
  RequireContextualKeywordExactRouteEvidence();
  RequireTopClauseLowering();
  RequireFetchClauseLowering();
  RequireWhereEqualityPredicateLowering();
  RequireTableJoinLowering();
  RequireTableSetOperationLowering();
  RequireRowNumberWindowLowering();
  RequireGroupByAggregateLowering();
  RequireTableCountLowering();
  RequireMaterializedCteLowering();
  RequireExplainWithCteAliasLowering();
  RequireRecursiveCteLowering();
  RequireScalarSubqueryLowering();
  RequireHavingClauseLowering();
  RequireUnsupportedQueryFamiliesFailClosed();
  RequireUnresolvedNamesFailClosed();

  RequireEngineDispatch("dml.select_rows", "SBLR_DML_SELECT_ROWS");
  RequireEngineDispatch("dml.insert_rows", "SBLR_DML_INSERT_ROWS");
  RequireEngineDispatch("dml.update_rows", "SBLR_DML_UPDATE_ROWS");
  RequireEngineDispatch("dml.delete_rows", "SBLR_DML_DELETE_ROWS");
  RequireEngineDispatch("dml.merge_rows", "SBLR_DML_MERGE_ROWS");
  RequireEngineDispatch("dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS");
  RemoveDatabaseArtifacts(TestDatabasePath());
  std::cout << "sbsql_dml_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
