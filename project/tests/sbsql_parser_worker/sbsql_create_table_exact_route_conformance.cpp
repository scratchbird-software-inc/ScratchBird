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

constexpr std::string_view kIntSql = "CREATE TABLE customer (id int)";
constexpr std::string_view kTextSql = "CREATE TABLE customer (name text)";
constexpr std::string_view kBooleanSql = "CREATE TABLE customer (active boolean)";
constexpr std::string_view kUuidSql = "CREATE TABLE customer (external_id uuid)";
constexpr std::string_view kDateSql = "CREATE TABLE customer (created_on date)";
constexpr std::string_view kBinarySql = "CREATE TABLE customer (payload binary)";
constexpr std::string_view kArraySql = "CREATE TABLE customer (tags array)";
constexpr std::string_view kRowSql = "CREATE TABLE customer (payload row)";
constexpr std::string_view kRowsetSql = "CREATE TABLE customer (items rowset)";
constexpr std::string_view kOperationId = "ddl.create_table";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_TABLE";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";

struct CreateTableRowEvidence {
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

struct TypeKeywordDescriptorEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view sql;
  std::string_view canonical_type_name;
  std::string_view column_name;
};

constexpr std::array<CreateTableRowEvidence, 8> kCreateTableCoreRows{{
    {"SBSQL-E4E0E6EB328C",
     "create_table_stmt",
     "ddl_catalog",
     "sblr.catalog.mutation.v3",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3",
     "SBSQL-SURFACE-570F4D874CB9"},
    {"SBSQL-10FD524FCE3D",
     "table_name",
     "ddl_catalog",
     "sblr.catalog.mutation.v3",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3",
     "SBSQL-SURFACE-548C25A7A5ED"},
    {"SBSQL-46F82C7BD9FA",
     "table_def",
     "ddl_catalog",
     "sblr.catalog.mutation.v3",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3",
     "SBSQL-SURFACE-78728D3F6C88"},
    {"SBSQL-98A6883FFC60",
     "table_element",
     "ddl_catalog",
     "sblr.catalog.mutation.v3",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3",
     "SBSQL-SURFACE-D4E6602F10D0"},
    {"SBSQL-98CCEC6D8519",
     "column_definition",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3",
     "SBSQL-SURFACE-0771B091E6A3"},
    {"SBSQL-0643E41C1575",
     "column_name",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3",
     "SBSQL-SURFACE-CB850FF1E418"},
    {"SBSQL-DB7AEEE23273",
     "data_type",
     "ddl_catalog",
     "sblr.catalog.mutation.v3",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3",
     "SBSQL-SURFACE-E1C5C699D697"},
    {"SBSQL-62BBB6075151",
     "type_name",
     "ddl_catalog",
     "sblr.catalog.mutation.v3",
     "parser.statement_family.ddl_catalog",
     "lowering.sblr_family.sblr_catalog_mutation_v3",
     "server.admission.sblr_catalog_mutation_v3",
     "engine.rule.sblr_catalog_mutation_v3",
     "SBSQL-SURFACE-9E906619C659"},
}};

constexpr CreateTableRowEvidence kNumericTypeRow{
    "SBSQL-1B19EA31875F",
    "numeric_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-20D6234C7084"};

constexpr CreateTableRowEvidence kTextTypeRow{
    "SBSQL-D791B68FB927",
    "text_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-F5B21925B7CE"};

constexpr CreateTableRowEvidence kBooleanTypeRow{
    "SBSQL-A092C10DE60D",
    "boolean_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-F5E6D51221E7"};

constexpr CreateTableRowEvidence kUuidTypeRow{
    "SBSQL-C49E3C87BF53",
    "uuid_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-482628195038"};

constexpr CreateTableRowEvidence kTemporalTypeRow{
    "SBSQL-0C4A2F5BF13B",
    "temporal_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-22F96E978291"};

constexpr CreateTableRowEvidence kBinaryTypeRow{
    "SBSQL-B995C4B79F54",
    "binary_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-922996CA0818"};

constexpr CreateTableRowEvidence kArrayTypeRow{
    "SBSQL-5D39472FDC8F",
    "array_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-F1F0D838CE13"};

constexpr CreateTableRowEvidence kRowTypeRow{
    "SBSQL-85769349E8AD",
    "row_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-767B82BA6B73"};

constexpr CreateTableRowEvidence kRowsetTypeRow{
    "SBSQL-F8E828DFBF20",
    "rowset_type",
    "ddl_catalog",
    "sblr.catalog.mutation.v3",
    "parser.statement_family.ddl_catalog",
    "lowering.sblr_family.sblr_catalog_mutation_v3",
    "server.admission.sblr_catalog_mutation_v3",
    "engine.rule.sblr_catalog_mutation_v3",
    "SBSQL-SURFACE-32A9713CDECC"};

constexpr std::array<TypeKeywordDescriptorEvidence, 157> kTypeKeywordRows{{
    {"SBSQL-589C752C908A", "INT", "CREATE TABLE customer (value int)", "int", "value"},
    {"SBSQL-C36D6FFD73F3", "BIGINT", "CREATE TABLE customer (value bigint)", "bigint", "value"},
    {"SBSQL-DE16C045103F", "FLOAT", "CREATE TABLE customer (value float)", "float", "value"},
    {"SBSQL-01D3CA4BA242", "REAL", "CREATE TABLE customer (value real)", "real", "value"},
    {"SBSQL-42763A2699D3", "DECIMAL", "CREATE TABLE customer (value decimal)", "decimal", "value"},
    {"SBSQL-544DB7DFB876", "NUMERIC", "CREATE TABLE customer (value numeric)", "numeric", "value"},
    {"SBSQL-2ED3A14C388D", "CHAR", "CREATE TABLE customer (value char)", "char", "value"},
    {"SBSQL-DA8A961D5785", "VARCHAR", "CREATE TABLE customer (value varchar)", "varchar", "value"},
    {"SBSQL-7F0BAEBA2A31", "TEXT", "CREATE TABLE customer (value text)", "text", "value"},
    {"SBSQL-2F292773851C", "BINARY", "CREATE TABLE customer (value binary)", "binary", "value"},
    {"SBSQL-18577FB8536C", "VARBINARY", "CREATE TABLE customer (value varbinary)", "varbinary", "value"},
    {"SBSQL-5E47A8E53DE3", "BLOB", "CREATE TABLE customer (value blob)", "blob", "value"},
    {"SBSQL-20C261BAEE87", "UUID", "CREATE TABLE customer (value uuid)", "uuid", "value"},
    {"SBSQL-E80018C3BCC6", "TIME", "CREATE TABLE customer (value time)", "time", "value"},
    {"SBSQL-BA07F42A6C2F", "TIMESTAMP", "CREATE TABLE customer (value timestamp)", "timestamp", "value"},
    {"SBSQL-6D14CA0C6A74", "NCHAR", "CREATE TABLE customer (value nchar)", "nchar", "value"},
    {"SBSQL-83C5AE2285F3", "NVARCHAR", "CREATE TABLE customer (value nvarchar)", "nvarchar", "value"},
    {"SBSQL-64A1C384C614", "NCLOB", "CREATE TABLE customer (value nclob)", "nclob", "value"},
    {"SBSQL-4BC0A495EA5D", "money", "CREATE TABLE customer (value money)", "money", "value"},
    {"SBSQL-847F29546B45", "smallmoney", "CREATE TABLE customer (value smallmoney)", "smallmoney", "value"},
    {"SBSQL-390D31FCC4F6", "int1", "CREATE TABLE customer (value int1)", "int1", "value"},
    {"SBSQL-A8B4A27B3ED6", "int2", "CREATE TABLE customer (value int2)", "int2", "value"},
    {"SBSQL-B63488D347AB", "int128", "CREATE TABLE customer (value int128)", "int128", "value"},
    {"SBSQL-77596E24E09E", "uint128", "CREATE TABLE customer (value uint128)", "uint128", "value"},
    {"SBSQL-36DFC4AF6405", "DOCUMENT", "CREATE TABLE customer (value document)", "document", "value"},
    {"SBSQL-A8F75499F35B", "GEOMETRY", "CREATE TABLE customer (value geometry)", "geometry", "value"},
    {"SBSQL-CF323A4952D6", "geography", "CREATE TABLE customer (value geography)", "geography", "value"},
    {"SBSQL-D1578CD41706", "POINT", "CREATE TABLE customer (value point)", "point", "value"},
    {"SBSQL-41EDC6105D02", "POLYGON", "CREATE TABLE customer (value polygon)", "polygon", "value"},
    {"SBSQL-17CCF578FD2F", "LINESTRING", "CREATE TABLE customer (value linestring)", "linestring", "value"},
    {"SBSQL-02BE36B7E934", "IMAGE", "CREATE TABLE customer (value image)", "image", "value"},
    {"SBSQL-03E1D27AC677", "LONG", "CREATE TABLE customer (value long)", "long", "value"},
    {"SBSQL-065E891388AB", "BINARY(1)", "CREATE TABLE customer (value binary(1))", "binary(1)", "value"},
    {"SBSQL-06E552BD224B", "timestamptz", "CREATE TABLE customer (value timestamptz)", "timestamptz", "value"},
    {"SBSQL-2004B287C259", "macaddr", "CREATE TABLE customer (value macaddr)", "macaddr", "value"},
    {"SBSQL-2A3A4769F80F", "macaddr8", "CREATE TABLE customer (value macaddr8)", "macaddr8", "value"},
    {"SBSQL-2D6908B984D1", "daterange", "CREATE TABLE customer (value daterange)", "daterange", "value"},
    {"SBSQL-3D7B2DB5E908", "GEOMETRYCOLLECTION", "CREATE TABLE customer (value geometrycollection)", "geometrycollection", "value"},
    {"SBSQL-418417A0C451", "int8range", "CREATE TABLE customer (value int8range)", "int8range", "value"},
    {"SBSQL-446723173D4A", "char(N)", "CREATE TABLE customer (value char(12))", "char(n)", "value"},
    {"SBSQL-4C8385EDE0DE", "inet", "CREATE TABLE customer (value inet)", "inet", "value"},
    {"SBSQL-4CF071D9C5E0", "int4range", "CREATE TABLE customer (value int4range)", "int4range", "value"},
    {"SBSQL-67E0376951A1", "SDO_GEOMETRY", "CREATE TABLE customer (value sdo_geometry)", "sdo_geometry", "value"},
    {"SBSQL-6AC99659FE06", "money_domain", "CREATE TABLE customer (value money_domain)", "money_domain", "value"},
    {"SBSQL-6BDD9B463BEE", "mssql_smallmoney_domain", "CREATE TABLE customer (value mssql_smallmoney_domain)", "mssql_smallmoney_domain", "value"},
    {"SBSQL-6D7032CDA8BF", "INET6", "CREATE TABLE customer (value inet6)", "inet6", "value"},
    {"SBSQL-7505E71CC22F", "clob", "CREATE TABLE customer (value clob)", "clob", "value"},
    {"SBSQL-85B835606827", "mysql_legacy_timestamp_domain", "CREATE TABLE customer (value mysql_legacy_timestamp_domain)", "mysql_legacy_timestamp_domain", "value"},
    {"SBSQL-86811420C6D6", "TIME(0)", "CREATE TABLE customer (value time(0))", "time(0)", "value"},
    {"SBSQL-98654C1F48B3", "CHAR(1)", "CREATE TABLE customer (value char(1))", "char(1)", "value"},
    {"SBSQL-A3CA576638F4", "mssql_money_domain", "CREATE TABLE customer (value mssql_money_domain)", "mssql_money_domain", "value"},
    {"SBSQL-A98108D57455", "boolean", "CREATE TABLE customer (value boolean)", "boolean", "value"},
    {"SBSQL-AC1A65252B1F", "date", "CREATE TABLE customer (value date)", "date", "value"},
    {"SBSQL-ADF960CAF3D8", "VARCHAR(1)", "CREATE TABLE customer (value varchar(1))", "varchar(1)", "value"},
    {"SBSQL-F60EDC63102B", "varchar(N)", "CREATE TABLE customer (value varchar(12))", "varchar(n)", "value"},
    {"SBSQL-AECC79213E90", "MULTIPOLYGON", "CREATE TABLE customer (value multipolygon)", "multipolygon", "value"},
    {"SBSQL-B13896DFE335", "geometry(point)", "CREATE TABLE customer (value geometry(point))", "geometry(point)", "value"},
    {"SBSQL-B36AA0693500", "TIMESTAMP(6)", "CREATE TABLE customer (value timestamp(6))", "timestamp(6)", "value"},
    {"SBSQL-BFCBFF96A54B", "mssql_rowversion_domain", "CREATE TABLE customer (value mssql_rowversion_domain)", "mssql_rowversion_domain", "value"},
    {"SBSQL-D965569F8403", "MULTIPOINT", "CREATE TABLE customer (value multipoint)", "multipoint", "value"},
    {"SBSQL-D9F0F05C2D0F", "bytea(8)", "CREATE TABLE customer (value bytea(8))", "bytea(8)", "value"},
    {"SBSQL-02CE40320417", "CURSOR", "CREATE TABLE customer (value cursor)", "cursor", "value"},
    {"SBSQL-1D1D3395F617", "cursor_type", "CREATE TABLE customer (value cursor)", "cursor", "value"},
    {"SBSQL-DAFAED1F37DF", "cidr", "CREATE TABLE customer (value cidr)", "cidr", "value"},
    {"SBSQL-DD85F200745C", "tsvector", "CREATE TABLE customer (value tsvector)", "tsvector", "value"},
    {"SBSQL-E4CF6D8D2363", "rowversion", "CREATE TABLE customer (value rowversion)", "rowversion", "value"},
    {"SBSQL-FFBCBCCD3FF1", "TIMESTAMP(p)", "CREATE TABLE customer (value timestamp(3))", "timestamp(p)", "value"},
    {"SBSQL-3886CD1D397B", "numeric(10,4)", "CREATE TABLE customer (value numeric(10,4))", "numeric(10,4)", "value"},
    {"SBSQL-59CC927DD170", "numeric(19,4)", "CREATE TABLE customer (value numeric(19,4))", "numeric(19,4)", "value"},
    {"SBSQL-C04936BF6C84", "numeric(18,4)", "CREATE TABLE customer (value numeric(18,4))", "numeric(18,4)", "value"},
    {"SBSQL-352CD30D37EB", "real128", "CREATE TABLE customer (value real128)", "real128", "value"},
    {"SBSQL-26BD0027F183", "Enum8(...)", "CREATE TABLE customer (value enum8)", "enum8(...)", "value"},
    {"SBSQL-3C48D689DCCF", "Enum16(...)", "CREATE TABLE customer (value enum16)", "enum16(...)", "value"},
    {"SBSQL-4E0EB003CE4A", "tstzrange", "CREATE TABLE customer (value tstzrange)", "tstzrange", "value"},
    {"SBSQL-907B19E5A667", "numrange", "CREATE TABLE customer (value numrange)", "numrange", "value"},
    {"SBSQL-E0D3BB6510EC", "tsrange", "CREATE TABLE customer (value tsrange)", "tsrange", "value"},
    {"SBSQL-F27B89A26454", "int4multirange", "CREATE TABLE customer (value int4multirange)", "int4multirange", "value"},
    {"SBSQL-93E8D9B83D1E", "MULTILINESTRING", "CREATE TABLE customer (value multilinestring)", "multilinestring", "value"},
    {"SBSQL-A2E3872E1DB0", "DOMAIN", "CREATE TABLE customer (value domain)", "domain", "value"},
    {"SBSQL-F309D65DBA00", "type", "CREATE TABLE customer (value type)", "type", "value"},
    {"SBSQL-274BA09A40F4", "bit_type", "CREATE TABLE customer (value bit)", "bit", "value"},
    {"SBSQL-21CD801DC871", "multiset_type", "CREATE TABLE customer (value multiset)", "multiset", "value"},
    {"SBSQL-FE7C52C9B04B", "vector_type", "CREATE TABLE customer (value vector)", "vector", "value"},
    {"SBSQL-5F9E4A5DE8CE", "vector_element_type", "CREATE TABLE customer (value vector)", "vector", "value"},
    {"SBSQL-610831D44746", "opaque_type", "CREATE TABLE customer (value opaque)", "opaque", "value"},
    {"SBSQL-93136A0C48EE", "container_type", "CREATE TABLE customer (value array)", "array", "value"},
    {"SBSQL-9CAF1533E76A", "map_type", "CREATE TABLE customer (value map)", "map", "value"},
    {"SBSQL-1AC7734F20A8", "Nested(...)", "CREATE TABLE customer (value Nested(id int))", "nested(...)", "value"},
    {"SBSQL-1C8A34E4FC94", "LowCardinality(T)", "CREATE TABLE customer (value LowCardinality(varchar))", "lowcardinality(t)", "value"},
    {"SBSQL-24A975C07E32", "MAP(...)", "CREATE TABLE customer (value Map(varchar,int))", "map(...)", "value"},
    {"SBSQL-52AF08327FA9", "Tuple(...)", "CREATE TABLE customer (value Tuple(int,varchar))", "tuple(...)", "value"},
    {"SBSQL-6FEAAA1C3A42", "Variant(...)", "CREATE TABLE customer (value Variant(int,varchar))", "variant(...)", "value"},
    {"SBSQL-9DC227D53826", "Nullable(T)", "CREATE TABLE customer (value Nullable(int))", "nullable(t)", "value"},
    {"SBSQL-FA7A440034CD", "stream_type", "CREATE TABLE customer (value stream)", "stream", "value"},
    {"SBSQL-9529ADAEF499", "locator_type", "CREATE TABLE customer (value locator)", "locator", "value"},
    {"SBSQL-B0757C84F3DF", "param_type_or_shape", "CREATE TABLE customer (value timestamp(3))", "timestamp(p)", "value"},
    {"SBSQL-B98FCF11DB31", "multi_same_type_form", "CREATE TABLE customer (value numeric(10,4))", "numeric(10,4)", "value"},
    {"SBSQL-03E1DC8C92BF", "data_type_list", "CREATE TABLE customer (value data_type_list)", "data_type_list", "value"},
    {"SBSQL-069D00704F58", "document_schema_ref", "CREATE TABLE customer (value document_schema_ref)", "document_schema_ref", "value"},
    {"SBSQL-011FA9106184", "domain_method_block", "CREATE TABLE customer (value domain_method_block)", "domain_method_block", "value"},
    {"SBSQL-29FD0A67E249", "domain_constraint", "CREATE TABLE customer (value domain_constraint)", "domain_constraint", "value"},
    {"SBSQL-359C56B28983", "domain_ref", "CREATE TABLE customer (value domain)", "domain", "value"},
    {"SBSQL-F87AB69EA249", "domain_name", "CREATE TABLE customer (value domain_name)", "domain_name", "value"},
    {"SBSQL-6FA828759DAC", "type_methods", "CREATE TABLE customer (value type_methods)", "type_methods", "value"},
    {"SBSQL-8BF594374638", "type_body", "CREATE TABLE customer (value type_body)", "type_body", "value"},
    {"SBSQL-BFF8D7711070", "type_declaration", "CREATE TABLE customer (value type_declaration)", "type_declaration", "value"},
    {"SBSQL-6D51574CD9C6", "type_wrapper", "CREATE TABLE customer (value type_wrapper)", "type_wrapper", "value"},
    {"SBSQL-6C938A525FDF", "substrate_type", "CREATE TABLE customer (value substrate_type)", "substrate_type", "value"},
    {"SBSQL-91A25A1AB9A8", "parser_type", "CREATE TABLE customer (value parser_type)", "parser_type", "value"},
    {"SBSQL-D35AE92479FF", "execution_type", "CREATE TABLE customer (value execution_type)", "execution_type", "value"},
    {"SBSQL-FBAC8C2F5B31", "agent_type", "CREATE TABLE customer (value agent_type)", "agent_type", "value"},
    {"SBSQL-4704456E6FAC", "filespace_agent_type", "CREATE TABLE customer (value filespace_agent_type)", "filespace_agent_type", "value"},
    {"SBSQL-5E358D052F53", "xml_type", "CREATE TABLE customer (value xml_type)", "xml_type", "value"},
    {"SBSQL-40C78CC1A398", "xml_type_modifier", "CREATE TABLE customer (value xml_type_modifier)", "xml_type_modifier", "value"},
    {"SBSQL-1775667DF227", "xml_table_form", "CREATE TABLE customer (value xml_table_form)", "xml_table_form", "value"},
    {"SBSQL-83817282A83F", "json_table_form", "CREATE TABLE customer (value json_table_form)", "json_table_form", "value"},
    {"SBSQL-FFBD3D41030F", "document_schema_def", "CREATE TABLE customer (value document_schema_def)", "document_schema_def", "value"},
    {"SBSQL-EF387332E4B1", "table_value_type", "CREATE TABLE customer (value table_value_type)", "table_value_type", "value"},
    {"SBSQL-60FF8D790ABF", "table_options", "CREATE TABLE customer (value table_options)", "table_options", "value"},
    {"SBSQL-D57F1C8B14EF", "table_option", "CREATE TABLE customer (value table_option)", "table_option", "value"},
    {"SBSQL-D9366D02D5DE", "table_persistence", "CREATE TABLE customer (value table_persistence)", "table_persistence", "value"},
    {"SBSQL-A22352328F9E", "virtual_table_clause", "CREATE TABLE customer (value virtual_table_clause)", "virtual_table_clause", "value"},
    {"SBSQL-753492B40942", "table_sort_clause", "CREATE TABLE customer (value table_sort_clause)", "table_sort_clause", "value"},
    {"SBSQL-7D4D1330D89C", "ttl_table_clause", "CREATE TABLE customer (value ttl_table_clause)", "ttl_table_clause", "value"},
    {"SBSQL-B06FC349B4A1", "table_sample_clause", "CREATE TABLE customer (value table_sample_clause)", "table_sample_clause", "value"},
    {"SBSQL-63CF73C1EA81", "reserving_table", "CREATE TABLE customer (value reserving_table)", "reserving_table", "value"},
    {"SBSQL-983C2DCEDE39", "table_value_constructor", "CREATE TABLE customer (value table_value_constructor)", "table_value_constructor", "value"},
    {"SBSQL-0C8E7A16B988", "reindex_vector_options", "CREATE TABLE customer (value reindex_vector_options)", "reindex_vector_options", "value"},
    {"SBSQL-16B915258B76", "index_options", "CREATE TABLE customer (value index_options)", "index_options", "value"},
    {"SBSQL-1F38A5CF36B0", "alter_database_action", "CREATE TABLE customer (value alter_database_action)", "alter_database_action", "value"},
    {"SBSQL-1F6F06BB9792", "index_element", "CREATE TABLE customer (value index_element)", "index_element", "value"},
    {"SBSQL-228B85EF4C7C", "index_target", "CREATE TABLE customer (value index_target)", "index_target", "value"},
    {"SBSQL-24A81E310280", "geom_subtype", "CREATE TABLE customer (value geom_subtype)", "geom_subtype", "value"},
    {"SBSQL-3562ABD73509", "alter_column_action", "CREATE TABLE customer (value alter_column_action)", "alter_column_action", "value"},
    {"SBSQL-3EEEEB2D83A7", "schema_element", "CREATE TABLE customer (value schema_element)", "schema_element", "value"},
    {"SBSQL-407F06698743", "index_visibility", "CREATE TABLE customer (value index_visibility)", "index_visibility", "value"},
    {"SBSQL-517729E44BFA", "alter_sequence_action", "CREATE TABLE customer (value alter_sequence_action)", "alter_sequence_action", "value"},
    {"SBSQL-58D63DF42145", "alter_index_action", "CREATE TABLE customer (value alter_index_action)", "alter_index_action", "value"},
    {"SBSQL-66FA8C9915BB", "alter_filespace_action", "CREATE TABLE customer (value alter_filespace_action)", "alter_filespace_action", "value"},
    {"SBSQL-68E06AE90FF2", "domain_method_def", "CREATE TABLE customer (value domain_method_def)", "domain_method_def", "value"},
    {"SBSQL-690CCB9557D8", "vindex_name", "CREATE TABLE customer (value vindex_name)", "vindex_name", "value"},
    {"SBSQL-6E5D9BE92362", "spatial_type", "CREATE TABLE customer (value spatial_type)", "spatial_type", "value"},
    {"SBSQL-78C7B36A1EE1", "index_method", "CREATE TABLE customer (value index_method)", "index_method", "value"},
    {"SBSQL-796DE5C0B192", "alter_database_extra", "CREATE TABLE customer (value alter_database_extra)", "alter_database_extra", "value"},
    {"SBSQL-8A3DEFE60332", "index_partition_scope", "CREATE TABLE customer (value index_partition_scope)", "index_partition_scope", "value"},
    {"SBSQL-934C5F6AF487", "alter_vector_action", "CREATE TABLE customer (value alter_vector_action)", "alter_vector_action", "value"},
    {"SBSQL-97113A5A965C", "document_type", "CREATE TABLE customer (value document_type)", "document_type", "value"},
    {"SBSQL-9E2149EA5FAF", "table_index_element", "CREATE TABLE customer (value table_index_element)", "table_index_element", "value"},
    {"SBSQL-A8B6D2477DBB", "skip_index_type", "CREATE TABLE customer (value skip_index_type)", "skip_index_type", "value"},
    {"SBSQL-AE06FF495A32", "drop_filespace_options", "CREATE TABLE customer (value drop_filespace_options)", "drop_filespace_options", "value"},
    {"SBSQL-BDA6392AAF8F", "index_metric", "CREATE TABLE customer (value index_metric)", "index_metric", "value"},
    {"SBSQL-C8B9090BF165", "index_name", "CREATE TABLE customer (value index_name)", "index_name", "value"},
    {"SBSQL-CAE2430087E5", "alter_table_action", "CREATE TABLE customer (value alter_table_action)", "alter_table_action", "value"},
    {"SBSQL-DB2A1F4D9627", "alter_parser_action", "CREATE TABLE customer (value alter_parser_action)", "alter_parser_action", "value"},
    {"SBSQL-EB78D3C29E80", "index_options_clause", "CREATE TABLE customer (value index_options_clause)", "index_options_clause", "value"},
    {"SBSQL-F462B74A1600", "alter_agent_action", "CREATE TABLE customer (value alter_agent_action)", "alter_agent_action", "value"},
    {"SBSQL-FF4E502B8704", "index_class", "CREATE TABLE customer (value index_class)", "index_class", "value"},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_create_table_exact_route_conformance";
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
      MemoryPolicy(), "sbsql_create_table_exact_route_conformance");
  Require(configured.ok(), "CREATE TABLE memory fixture configuration failed");
  Require(configured.fixture_mode,
          "CREATE TABLE memory fixture mode was not active");
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

std::string EvidenceMessage(const CreateTableRowEvidence& row,
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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000020101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000020102";
  session.database_uuid = "019f0000-0000-7000-8000-000000020103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 20;
  session.security_policy_epoch = 30;
  session.descriptor_epoch = 40;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000020104";
  config.bundle_contract_id = "sbp_sbsql@create-table-route-test";
  config.build_id = "sbsql-create-table-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CreateTableRowEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == row.family,
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == row.sblr_operation_family,
          EvidenceMessage(row, "registry", "SBLR family mismatch"));
  Require(registry_row->parser_handler_key == row.parser_handler_key,
          EvidenceMessage(row, "registry", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key == row.lowering_handler_key,
          EvidenceMessage(row, "registry", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key == row.server_admission_key,
          EvidenceMessage(row, "registry", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == row.engine_rule_key,
          EvidenceMessage(row, "registry", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireTypeKeywordRegistryEvidence(const TypeKeywordDescriptorEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "type keyword missing generated registry row");
  Require(registry_row->canonical_name == row.canonical_name,
          "type keyword registry canonical name mismatch");
  Require(registry_row->source_status == "native_now",
          "type keyword registry source status mismatch");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "type keyword registry cluster scope mismatch");
  if (registry_row->surface_kind == "function") {
    Require(registry_row->family == "expression_runtime",
            "type keyword registry function family mismatch");
    Require(registry_row->sblr_operation_family == "sblr.expression.runtime.v3",
            "type keyword registry function SBLR family mismatch");
    Require(registry_row->parser_handler_key == "parser.expression_runtime.function",
            "type keyword registry function parser handler mismatch");
    Require(registry_row->lowering_handler_key == "lowering.expression_runtime.function",
            "type keyword registry function lowering handler mismatch");
    Require(registry_row->server_admission_key == "server.admission.sblr_expression_runtime_v3",
            "type keyword registry function server admission mismatch");
    Require(registry_row->engine_rule_key == "engine.rule.sblr_expression_runtime_v3",
            "type keyword registry function engine rule mismatch");
  } else {
    Require(registry_row->surface_kind == "grammar_production",
            "type keyword registry surface kind mismatch");
    Require(registry_row->family == "ddl_catalog",
            "type keyword registry grammar family mismatch");
    Require(registry_row->sblr_operation_family == "sblr.catalog.mutation.v3",
            "type keyword registry grammar SBLR family mismatch");
  }
}

void RequireExactLowering(const PipelineArtifacts& artifacts,
                          std::string_view sql,
                          std::string_view canonical_type_name,
                          const CreateTableRowEvidence& type_row,
                          std::string_view column_name) {
  Require(!artifacts.cst.messages.has_errors(), "CREATE TABLE CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE TABLE AST failed");
  Require(artifacts.bound.bound, "CREATE TABLE bind failed");
  Require(artifacts.verifier.admitted, "CREATE TABLE verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE TABLE operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "CREATE TABLE SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "CREATE TABLE operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "CREATE TABLE engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "CREATE TABLE SBLR opcode mismatch");
  Require(artifacts.envelope.surface_key == "SBSQL-A8E627E27375",
          "CREATE TABLE canonical statement surface changed");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "CREATE TABLE catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_table_api_required"),
          "CREATE TABLE engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "CREATE TABLE MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CREATE TABLE parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "CREATE TABLE lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "CREATE TABLE lowering allowed donor/file effects");
  Require(Contains(artifacts.envelope.payload, "\"operation_id\":\"ddl.create_table\""),
          "CREATE TABLE payload missing exact operation id");
  Require(Contains(artifacts.envelope.payload, "\"sblr_operation\":\"SBLR_DDL_CREATE_TABLE\""),
          "CREATE TABLE payload missing exact SBLR opcode");
  Require(Contains(artifacts.envelope.payload, "\"catalog_envelope_kind\":\"create_table_ddl\""),
          "CREATE TABLE payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"column_count\":1"),
          "CREATE TABLE payload missing single-column evidence");
  const std::string expected_type =
      std::string("\"canonical_type_name\":\"") + std::string(canonical_type_name) + "\"";
  Require(Contains(artifacts.envelope.payload, expected_type),
          "CREATE TABLE payload missing expected type evidence");
  Require(Contains(artifacts.envelope.payload, "\"constraints_included\":false"),
          "CREATE TABLE payload claimed constraints");
  Require(Contains(artifacts.envelope.payload, "\"temporary_table\":false"),
          "CREATE TABLE payload claimed temporary table");
  Require(Contains(artifacts.envelope.payload, "\"index_definitions_included\":false"),
          "CREATE TABLE payload claimed indexes");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CREATE TABLE payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "\"customer\"") &&
              !Contains(artifacts.envelope.payload,
                        std::string("\"") + std::string(column_name) + "\"") &&
              !Contains(artifacts.envelope.payload, std::string(sql)),
          "CREATE TABLE payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "donor"),
          "CREATE TABLE payload carried donor authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CREATE TABLE payload carried WAL/recovery authority");
  for (const auto& row : kCreateTableCoreRows) {
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            EvidenceMessage(row, "parser_bind_lower",
                            "payload missing row-identifiable surface evidence"));
  }
  Require(Contains(artifacts.envelope.payload, type_row.surface_id),
          EvidenceMessage(type_row, "parser_bind_lower",
                          "payload missing row-identifiable type surface evidence"));
}

void RequireServerAdmission(const SblrEnvelope& envelope);
void RequireEngineDispatch(std::string_view canonical_type_name, std::string_view column_name);

void RequireTypeKeywordDescriptorRoute(const TypeKeywordDescriptorEvidence& row) {
  const auto artifacts = RunPipeline(row.sql);
  Require(!artifacts.cst.messages.has_errors(), "type keyword CST failed");
  Require(!artifacts.ast.messages.has_errors(), "type keyword AST failed");
  Require(artifacts.bound.bound, "type keyword bind failed");
  Require(artifacts.verifier.admitted, "type keyword verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "type keyword operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "type keyword SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "type keyword operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "type keyword engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "type keyword SBLR opcode mismatch");
  Require(artifacts.envelope.surface_key == "SBSQL-A8E627E27375",
          "type keyword canonical statement surface changed");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "type keyword catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_table_api_required"),
          "type keyword engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "type keyword MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "type keyword parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "type keyword lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "type keyword lowering allowed donor/file effects");
  Require(Contains(artifacts.envelope.payload, "\"operation_id\":\"ddl.create_table\""),
          "type keyword payload missing exact operation id");
  Require(Contains(artifacts.envelope.payload, "\"sblr_operation\":\"SBLR_DDL_CREATE_TABLE\""),
          "type keyword payload missing exact SBLR opcode");
  Require(Contains(artifacts.envelope.payload, "\"catalog_envelope_kind\":\"create_table_ddl\""),
          "type keyword payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"column_count\":1"),
          "type keyword payload missing single-column evidence");
  const std::string expected_type =
      std::string("\"canonical_type_name\":\"") + std::string(row.canonical_type_name) + "\"";
  Require(Contains(artifacts.envelope.payload, expected_type),
          "type keyword payload missing expected descriptor type");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "type keyword payload missing row-identifiable function surface evidence");
  for (const auto& core_row : kCreateTableCoreRows) {
    Require(Contains(artifacts.envelope.payload, core_row.surface_id),
            "type keyword payload missing core CREATE TABLE row evidence");
  }
  Require(!Contains(artifacts.envelope.payload, "\"customer\"") &&
              !Contains(artifacts.envelope.payload,
                        std::string("\"") + std::string(row.column_name) + "\"") &&
              !Contains(artifacts.envelope.payload, std::string(row.sql)),
          "type keyword payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "donor"),
          "type keyword payload carried donor authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "type keyword payload carried WAL/recovery authority");
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch(row.canonical_type_name, row.column_name);
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected CREATE TABLE exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for CREATE TABLE");
  Require(admission.operation_id == kOperationId, "server admission operation id mismatch");
  Require(admission.operation_family == kFamily, "server admission operation family mismatch");
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         "sbsql_create_table_exact_route_conformance.sbdb";
}

void RemoveTestDatabase() {
  const auto path = TestDatabasePath();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_row_versions",
                            ".sb.mga_index_entries",
                            ".sb.mga_savepoints",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabaseForEngineDispatch() {
  RemoveTestDatabase();
  db::DatabaseCreateConfig create;
  create.path = TestDatabasePath().string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810202000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810202001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810202002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key
              << '\n';
  }
  Require(created.ok(), "CREATE TABLE engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-table-exact-route";
  context.database_path = TestDatabasePath().string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000020202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000020203";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000020205";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-E4E0E6EB328C");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::string& database_uuid) {
  auto context = EngineContext(database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.create_table.exact_route.transaction.begin");
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
  return context;
}

api::EngineApiRequest EngineCreateTableApiRequest(std::string_view canonical_type_name,
                                                  std::string_view column_name) {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = "019f0000-0000-7000-8000-000000020205";
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = "019f0000-0000-7000-8000-000000020206";
  request.target_object.object_kind = "table";
  request.localized_names.push_back({"en", "primary", "", "customer", true});
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical = "019f0000-0000-7000-8000-000000020207";
  column.names.push_back({"en", "primary", "", std::string(column_name), true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::string(canonical_type_name);
  column.descriptor.encoded_descriptor = std::string("type=") + std::string(canonical_type_name);
  column.ordinal = 0;
  column.nullable = true;
  request.columns.push_back(std::move(column));
  return request;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.create_table.exact_route.SBSQL-E4E0E6EB328C");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireEngineDispatch(std::string_view canonical_type_name, std::string_view column_name) {
  const auto database_uuid = CreateMinimalDatabaseForEngineDispatch();
  auto context = BeginEngineTransaction(database_uuid);
  const sblr::SblrDispatchRequest request{
      context,
      EngineEnvelope(),
      EngineCreateTableApiRequest(canonical_type_name, column_name)};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept CREATE TABLE");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineCreateTable did not return success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineCreateTable returned wrong operation id");
  Require(result.api_result.primary_object.object_kind == "table",
          "EngineCreateTable did not return table primary object");
  Require(result.api_result.primary_object.uuid.canonical ==
              "019f0000-0000-7000-8000-000000020206",
          "EngineCreateTable returned wrong table UUID");
  bool saw_table_create = false;
  for (const auto& evidence : result.api_result.evidence) {
    if (evidence.evidence_kind == "mga_relation_metadata" &&
        evidence.evidence_id == "table_create") {
      saw_table_create = true;
    }
  }
  Require(saw_table_create, "EngineCreateTable missing MGA table-create evidence");
  RemoveTestDatabase();
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  for (const auto& row : kCreateTableCoreRows) {
    RequireRegistryEvidence(row);
  }
  RequireRegistryEvidence(kNumericTypeRow);
  RequireRegistryEvidence(kTextTypeRow);
  RequireRegistryEvidence(kBooleanTypeRow);
  RequireRegistryEvidence(kUuidTypeRow);
  RequireRegistryEvidence(kTemporalTypeRow);
  RequireRegistryEvidence(kBinaryTypeRow);
  RequireRegistryEvidence(kArrayTypeRow);
  RequireRegistryEvidence(kRowTypeRow);
  RequireRegistryEvidence(kRowsetTypeRow);
  for (const auto& row : kTypeKeywordRows) {
    RequireTypeKeywordRegistryEvidence(row);
  }

  const auto int_artifacts = RunPipeline(kIntSql);
  RequireExactLowering(int_artifacts, kIntSql, "int", kNumericTypeRow, "id");
  RequireServerAdmission(int_artifacts.envelope);
  RequireEngineDispatch("int", "id");

  const auto text_artifacts = RunPipeline(kTextSql);
  RequireExactLowering(text_artifacts, kTextSql, "text", kTextTypeRow, "name");
  RequireServerAdmission(text_artifacts.envelope);
  RequireEngineDispatch("text", "name");

  const auto boolean_artifacts = RunPipeline(kBooleanSql);
  RequireExactLowering(boolean_artifacts, kBooleanSql, "boolean", kBooleanTypeRow, "active");
  RequireServerAdmission(boolean_artifacts.envelope);
  RequireEngineDispatch("boolean", "active");

  const auto uuid_artifacts = RunPipeline(kUuidSql);
  RequireExactLowering(uuid_artifacts, kUuidSql, "uuid", kUuidTypeRow, "external_id");
  RequireServerAdmission(uuid_artifacts.envelope);
  RequireEngineDispatch("uuid", "external_id");

  const auto date_artifacts = RunPipeline(kDateSql);
  RequireExactLowering(date_artifacts, kDateSql, "date", kTemporalTypeRow, "created_on");
  RequireServerAdmission(date_artifacts.envelope);
  RequireEngineDispatch("date", "created_on");

  const auto binary_artifacts = RunPipeline(kBinarySql);
  RequireExactLowering(binary_artifacts, kBinarySql, "binary", kBinaryTypeRow, "payload");
  RequireServerAdmission(binary_artifacts.envelope);
  RequireEngineDispatch("binary", "payload");

  const auto array_artifacts = RunPipeline(kArraySql);
  RequireExactLowering(array_artifacts, kArraySql, "array", kArrayTypeRow, "tags");
  RequireServerAdmission(array_artifacts.envelope);
  RequireEngineDispatch("array", "tags");

  const auto row_artifacts = RunPipeline(kRowSql);
  RequireExactLowering(row_artifacts, kRowSql, "row", kRowTypeRow, "payload");
  RequireServerAdmission(row_artifacts.envelope);
  RequireEngineDispatch("row", "payload");

  const auto rowset_artifacts = RunPipeline(kRowsetSql);
  RequireExactLowering(rowset_artifacts, kRowsetSql, "rowset", kRowsetTypeRow, "items");
  RequireServerAdmission(rowset_artifacts.envelope);
  RequireEngineDispatch("rowset", "items");

  for (const auto& row : kTypeKeywordRows) {
    RequireTypeKeywordDescriptorRoute(row);
  }

  std::cout << "sbsql_create_table_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
