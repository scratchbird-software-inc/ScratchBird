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
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000003101";

struct ScalarLiteralGrammarRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view fixture_sql;
  std::string_view projection_name;
  std::string_view type_name;
  std::string_view value;
  bool is_null{false};
  std::string_view family{"general"};
  std::string_view sblr_operation_family{"sblr.general.operation.v3"};
  std::string_view parser_handler_key{"parser.grammar_ast"};
  std::string_view lowering_handler_key{"lowering.sblr_family.sblr_general_operation_v3"};
  std::string_view server_admission_key{"server.admission.sblr_general_operation_v3"};
  std::string_view engine_rule_key{"engine.rule.sblr_general_operation_v3"};
};

constexpr ScalarLiteralGrammarRowEvidence kScalarLiteralGrammarRows[] = {
    {"SBSQL-1FC7D47D036C", "literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "one", "bigint", "1", false},
    {"SBSQL-8C106AFE4529", "integer_literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "one", "bigint", "1", false},
    {"SBSQL-C50A00C8E3DD", "string_literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "two", "text", "two", false},
    {"SBSQL-CAEF92407B2D", "null_literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "empty_value", "null", "", true},
    {"SBSQL-701AC25E2EB8", "boolean_literal",
     "SELECT TRUE AS truth", "truth", "boolean", "true", false},
    {"SBSQL-E61ADCFD4F93", "numeric_literal",
     "SELECT 12.34 AS decimal_value", "decimal_value", "numeric", "12.34", false},
    {"SBSQL-43CAD9DA7AEE", "uint_literal",
     "SELECT 42U AS uint_value", "uint_value", "uint64", "42", false},
    {"SBSQL-224B7A099AA6", "int128_literal",
     "SELECT 123I128 AS int128_value", "int128_value", "int128", "123", false},
    {"SBSQL-477DFABD507C", "uint128_literal",
     "SELECT 123U128 AS uint128_value", "uint128_value", "uint128", "123", false},
    {"SBSQL-0403B44C8B4C", "real128_literal",
     "SELECT 1.25R128 AS real128_value", "real128_value", "real128", "1.25", false},
    {"SBSQL-4F7D60F01353", "decimal_literal",
     "SELECT 12.34 AS decimal_value", "decimal_value", "numeric", "12.34", false},
    {"SBSQL-ACF69057CA9A", "float_literal",
     "SELECT 1e2 AS float_value", "float_value", "numeric", "1e2", false},
    {"SBSQL-E94BF9993D60", "binary_literal",
     "SELECT X'00ff10' AS binary_value", "binary_value", "binary", "00ff10", false},
    {"SBSQL-252811043F4E", "uuid_literal",
     "SELECT UUID '550e8400-e29b-41d4-a716-446655440000' AS uuid_value",
     "uuid_value", "uuid", "550e8400-e29b-41d4-a716-446655440000", false},
    {"SBSQL-0515D767787D", "date_literal",
     "SELECT DATE '2026-05-14' AS date_value",
     "date_value", "date", "2026-05-14", false},
    {"SBSQL-E3F8F121508E", "time_literal",
     "SELECT TIME '14:23:45' AS time_value",
     "time_value", "time", "14:23:45", false},
    {"SBSQL-6A6A28B04C3D", "timestamp_literal",
     "SELECT TIMESTAMP '2026-05-14T14:23:45' AS timestamp_value",
     "timestamp_value", "timestamp", "2026-05-14T14:23:45", false},
    {"SBSQL-72BA1A6D5BF8", "interval_literal",
     "SELECT INTERVAL '1 day' AS interval_value",
     "interval_value", "interval", "1 day", false},
    {"SBSQL-7C37BF0C7569", "interval_qualifier",
     "SELECT INTERVAL '1' DAY AS interval_value",
     "interval_value", "interval", "1 day", false},
    {"SBSQL-CAF62B31FE35", "document_literal",
     "SELECT DOCUMENT '{\"a\":1}' AS document_value",
     "document_value", "document", "{\"a\":1}", false,
     "multi_model", "sblr.query.multimodel_or_ddl.v3",
     "parser.statement_family.multi_model",
     "lowering.sblr_family.sblr_query_multimodel_or_ddl_v3",
     "server.admission.sblr_query_multimodel_or_ddl_v3",
     "engine.rule.sblr_query_multimodel_or_ddl_v3"},
    {"SBSQL-94498009F62D", "json_literal_inline",
     "SELECT JSON '{\"a\":1}' AS json_value",
     "json_value", "json_document", "{\"a\":1}", false},
    {"SBSQL-04D6EA33D8AF", "vector_literal",
     "SELECT VECTOR '[1,2,3]' AS vector_value",
     "vector_value", "dense_vector", "[1,2,3]", false,
     "multi_model", "sblr.query.multimodel_or_ddl.v3",
     "parser.statement_family.multi_model",
     "lowering.sblr_family.sblr_query_multimodel_or_ddl_v3",
     "server.admission.sblr_query_multimodel_or_ddl_v3",
     "engine.rule.sblr_query_multimodel_or_ddl_v3"},
};

constexpr ScalarLiteralGrammarRowEvidence kScalarLiteralFunctionRows[] = {
    {"SBSQL-BF53B71555D7", "integer_literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "one", "bigint", "1", false},
    {"SBSQL-FABD8DA5368B", "string_literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "two", "text", "two", false},
    {"SBSQL-E91BCF5291EB", "null_literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "empty_value", "null", "", true},
    {"SBSQL-838155BF3490", "expr.null_literal",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "empty_value", "null", "", true},
    {"SBSQL-D8158299C619", "NULL",
     "SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth",
     "empty_value", "null", "", true},
    {"SBSQL-4BFA84D04527", "boolean_literal",
     "SELECT TRUE AS truth", "truth", "boolean", "true", false},
    {"SBSQL-D52E75B538C5", "TRUE",
     "SELECT TRUE AS truth", "truth", "boolean", "true", false},
    {"SBSQL-CE809B0F48C9", "FALSE",
     "SELECT FALSE AS falsehood", "falsehood", "boolean", "false", false},
    {"SBSQL-EE95253C80B3", "decimal_literal",
     "SELECT 12.34 AS decimal_value", "decimal_value", "numeric", "12.34", false},
    {"SBSQL-2E1743A87369", "uint_literal",
     "SELECT 42U AS uint_value", "uint_value", "uint64", "42", false},
    {"SBSQL-FBAE83B3BD03", "int128_literal",
     "SELECT 123I128 AS int128_value", "int128_value", "int128", "123", false},
    {"SBSQL-F65637AB40BF", "uint128_literal",
     "SELECT 123U128 AS uint128_value", "uint128_value", "uint128", "123", false},
    {"SBSQL-8E719BFB5277", "real128_literal",
     "SELECT 1.25R128 AS real128_value", "real128_value", "real128", "1.25", false},
    {"SBSQL-FD2E26018B58", "float_literal",
     "SELECT 1e2 AS float_value", "float_value", "numeric", "1e2", false},
    {"SBSQL-3BDB82ED4BDB", "binary_literal",
     "SELECT X'00ff10' AS binary_value", "binary_value", "binary", "00ff10", false},
    {"SBSQL-6D258F5FD8E5", "uuid_literal",
     "SELECT UUID '550e8400-e29b-41d4-a716-446655440000' AS uuid_value",
     "uuid_value", "uuid", "550e8400-e29b-41d4-a716-446655440000", false},
    {"SBSQL-A360BCCACD0C", "date_literal",
     "SELECT DATE '2026-05-14' AS date_value",
     "date_value", "date", "2026-05-14", false},
    {"SBSQL-7526C65D8BBC", "time_literal",
     "SELECT TIME '14:23:45' AS time_value",
     "time_value", "time", "14:23:45", false},
    {"SBSQL-CE3C4B01B1C2", "timestamp_literal",
     "SELECT TIMESTAMP '2026-05-14T14:23:45' AS timestamp_value",
     "timestamp_value", "timestamp", "2026-05-14T14:23:45", false},
    {"SBSQL-C1C98762EFB2", "interval_literal",
     "SELECT INTERVAL '1 day' AS interval_value",
     "interval_value", "interval", "1 day", false},
    {"SBSQL-F968D91E6167", "INTERVAL",
     "SELECT INTERVAL '1 day' AS interval_value",
     "interval_value", "interval", "1 day", false},
    {"SBSQL-97C8879B6EAF", "document_literal",
     "SELECT DOCUMENT '{\"a\":1}' AS document_value",
     "document_value", "document", "{\"a\":1}", false},
    {"SBSQL-C32CC43E49B1", "vector_literal",
     "SELECT VECTOR '[1,2,3]' AS vector_value",
     "vector_value", "dense_vector", "[1,2,3]", false},
};

struct Sbsfc016LanguagePolicyRow {
  const char* surface_id;
  const char* function_name;
  const char* function_id;
  const char* result_type;
  const char* expected_value;
};

constexpr Sbsfc016LanguagePolicyRow kSbsfc016LanguagePolicyRows[] = {
    {"SBSQL-0A96CD6009B0", "numeric_division_by_zero", "sb.scalar.numeric_division_by_zero", "character", "error"},
    {"SBSQL-1076274FA39A", "localized_label_max_length_bytes", "sb.scalar.localized_label_max_length_bytes", "uint64", "1024"},
    {"SBSQL-1DD19CC14460", "default_schema_resolution", "sb.scalar.default_schema_resolution", "character", "session_search_path_ambiguous_context_refusal"},
    {"SBSQL-38F7E6CE50C4", "result_set_max_rows_in_response", "sb.scalar.result_set_max_rows_in_response", "uint64", "0"},
    {"SBSQL-3B0F02549442", "identifier_max_length_bytes", "sb.scalar.identifier_max_length_bytes", "uint64", "255"},
    {"SBSQL-566F25C6A1DC", "statement_timeout", "sb.scalar.statement_timeout", "uint64", "0"},
    {"SBSQL-63F49DEC1864", "statement_timeout_default", "sb.scalar.statement_timeout_default", "uint64", "0"},
    {"SBSQL-48F297900CCF", "statement_timeout_ms", "sb.scalar.statement_timeout_ms", "uint64", "0"},
    {"SBSQL-61AA83D5AF3B", "client_min_messages_default", "sb.scalar.client_min_messages_default", "character", "NOTICE"},
    {"SBSQL-62293C2E8E6C", "numeric_overflow_behavior", "sb.scalar.numeric_overflow_behavior", "character", "error"},
    {"SBSQL-6A71D5F290FE", "null_ordering_default_for_asc", "sb.scalar.null_ordering_default_for_asc", "character", "NULLS LAST"},
    {"SBSQL-6DFFFC9C7143", "statement_max_length_bytes", "sb.scalar.statement_max_length_bytes", "uint64", "33554432"},
    {"SBSQL-76AE668F5B32", "null_concat_returns_null", "sb.scalar.null_concat_returns_null", "boolean", "1"},
    {"SBSQL-885A9826C368", "recursion_max_depth", "sb.scalar.recursion_max_depth", "uint64", "1024"},
    {"SBSQL-8F4843478BD9", "parameter_marker_max_count", "sb.scalar.parameter_marker_max_count", "uint64", "262144"},
    {"SBSQL-94568295C808", "delimited_identifier_max_length_bytes", "sb.scalar.delimited_identifier_max_length_bytes", "uint64", "255"},
    {"SBSQL-A0D8F5B44E98", "case_resolution_for_quoted_identifiers", "sb.scalar.case_resolution_for_quoted_identifiers", "character", "exact_spelling_match_not_identity"},
    {"SBSQL-A727EFD5293F", "temporal_default_precision", "sb.scalar.temporal_default_precision", "uint64", "6"},
    {"SBSQL-AB0BCEEA8CAD", "string_truncation_behavior", "sb.scalar.string_truncation_behavior", "character", "error"},
    {"SBSQL-ADF52458FA79", "timezone_resolution", "sb.scalar.timezone_resolution", "character", "session_timezone_explicit_zone_conversion"},
    {"SBSQL-B2ACB81B5943", "null_in_aggregate_skipped", "sb.scalar.null_in_aggregate_skipped", "boolean", "1"},
    {"SBSQL-C8724AE0B69E", "null_ordering_default_for_desc", "sb.scalar.null_ordering_default_for_desc", "character", "NULLS FIRST"},
    {"SBSQL-C9B937A0E6A4", "interval_default_precision", "sb.scalar.interval_default_precision", "character", "qualifier_driven"},
    {"SBSQL-CB465EA2790A", "name_resolution", "sb.scalar.name_resolution", "character", "uuid_bound_at_bind_time"},
    {"SBSQL-D231D788EACF", "recursive_schema_path_separator", "sb.scalar.recursive_schema_path_separator", "character", "."},
    {"SBSQL-DBEE94C525A5", "identifier_max_length_chars", "sb.scalar.identifier_max_length_chars", "uint64", "63"},
    {"SBSQL-04636D3CDEF5", "lock_timeout", "sb.scalar.lock_timeout", "uint64", "0"},
    {"SBSQL-0080B3C2968B", "lock_timeout_default", "sb.scalar.lock_timeout_default", "uint64", "0"},
    {"SBSQL-E6D97E527535", "lock_timeout_ms", "sb.scalar.lock_timeout_ms", "uint64", "0"},
    {"SBSQL-678A3FA8960F", "idle_in_transaction_session_timeout", "sb.scalar.idle_in_transaction_session_timeout", "uint64", "0"},
    {"SBSQL-EF6F8A3F935F", "idle_in_transaction_timeout_default", "sb.scalar.idle_in_transaction_timeout_default", "uint64", "0"},
    {"SBSQL-ED07CA49F7D2", "idle_in_transaction_session_timeout_ms", "sb.scalar.idle_in_transaction_session_timeout_ms", "uint64", "0"},
    {"SBSQL-902AB93C9666", "transaction_timeout", "sb.scalar.transaction_timeout", "uint64", "0"},
    {"SBSQL-9842FF657243", "transaction_timeout_default", "sb.scalar.transaction_timeout_default", "uint64", "0"},
    {"SBSQL-6D203D956A66", "client_addr", "sb.scalar.client_addr", "character", "session.client_address"},
    {"SBSQL-040616412F95", "client_port", "sb.scalar.client_port", "uint64", "0"},
    {"SBSQL-876E669FEA8F", "default_decimal_division_scale", "sb.scalar.default_decimal_division_scale", "uint64", "6"},
    {"SBSQL-920D2DD526FC", "temp_buffer_size_default", "sb.scalar.temp_buffer_size_default", "uint64", "8388608"},
    {"SBSQL-EF1565632553", "null_in_unique_constraint", "sb.scalar.null_in_unique_constraint", "character", "multiple_nulls_allowed"},
    {"SBSQL-FFCEFC0CEF1E", "qualified_name_max_segments", "sb.scalar.qualified_name_max_segments", "uint64", "16"},
    {"SBSQL-F53140C3E231", "empty_string_equals_null", "sb.scalar.empty_string_equals_null", "boolean", "0"},
    {"SBSQL-E37283B0B5BD", "count_distinct_includes_null", "sb.scalar.count_distinct_includes_null", "boolean", "0"},
};

constexpr Sbsfc016LanguagePolicyRow kSbsfc016MetadataRows[] = {
    {"SBSQL-00A225C7BC09", "operation_evidence_required", "sb.scalar.operation_evidence_required", "boolean", "1"},
    {"SBSQL-5E9BF65CABA9", "decision_proof_required", "sb.scalar.decision_proof_required", "boolean", "1"},
    {"SBSQL-EA1055FD778D", "current_capability_set", "sb.scalar.current_capability_set", "character", "public_noncluster_alpha"},
    {"SBSQL-96AC6D338681", "current_engine_version", "sb.scalar.current_engine_version", "character", "ScratchBird 0.1.0"},
    {"SBSQL-C69205E8B7F5", "application_name", "sb.scalar.application_name", "character", "sbsql_conformance"},
    {"SBSQL-52A87230C51F", "current_locale", "sb.scalar.current_locale", "character", "en-US"},
    {"SBSQL-9F79AF739250", "client_protocol", "sb.scalar.client_protocol", "uuid", "019e1600-0000-7000-8000-0000000000ee"},
    {"SBSQL-8EF55DAAC17F", "private_profile_active", "sb.scalar.private_profile_active", "boolean", "0"},
    {"SBSQL-C41110D1C709", "built_in_function_shadow_rule", "sb.scalar.built_in_function_shadow_rule", "character", "deny_builtin_shadowing"},
    {"SBSQL-3D23D96C580E", "current_isolation_level", "sb.scalar.current_isolation_level", "character", "snapshot"},
    {"SBSQL-0D3AF5689337", "mga_isolation_profile", "sb.scalar.mga_isolation_profile", "character", "snapshot_transaction"},
    {"SBSQL-33ABD23561B2", "tx_read_only", "sb.scalar.tx_read_only", "boolean", "0"},
    {"SBSQL-6E6DF1F1D3F5", "read_only_session", "sb.scalar.read_only_session", "boolean", "0"},
    {"SBSQL-48630666FF4B", "request_key_required", "sb.scalar.request_key_required", "boolean", "1"},
    {"SBSQL-493876D2FFDB", "sbsql_v3", "sb.scalar.sbsql_v3", "character", "sbsql.v3"},
    {"SBSQL-95EFD6F591B0", "sqlstate", "sb.scalar.sqlstate", "character", "00000"},
    {"SBSQL-B5B0AC8A7813", "sqlcode", "sb.scalar.sqlcode", "int64", "0"},
    {"SBSQL-9487E723F6DB", "sqlerrm", "sb.scalar.sqlerrm", "character", "OK"},
    {"SBSQL-26096EC6FBAF", "not_found", "sb.scalar.not_found", "boolean", "0"},
    {"SBSQL-14D576F7019D", "deprecated_keyword", "sb.scalar.deprecated_keyword", "character", "keyword_class.deprecated"},
    {"SBSQL-21B3D26B555C", "donor_contextual_keyword", "sb.scalar.donor_contextual_keyword", "character", "keyword_class.donor_contextual"},
    {"SBSQL-7036F89856D2", "reserved_native_keyword", "sb.scalar.reserved_native_keyword", "character", "keyword_class.reserved_native"},
    {"SBSQL-FA8B706E49D0", "contextual_native_keyword", "sb.scalar.contextual_native_keyword", "character", "keyword_class.contextual_native"},
    {"SBSQL-813817A7EDFD", "donor_reserved_keyword", "sb.scalar.donor_reserved_keyword", "character", "keyword_class.donor_reserved"},
    {"SBSQL-C1E6BF629293", "meta_command_keyword", "sb.scalar.meta_command_keyword", "character", "keyword_class.meta_command"},
    {"SBSQL-92C51F4C0F42", "private_only_keyword", "sb.scalar.private_only_keyword", "character", "keyword_class.private_only"},
    {"SBSQL-2150B810CBA5", "refusal_only_keyword", "sb.scalar.refusal_only_keyword", "character", "keyword_class.refusal_only"},
    {"SBSQL-D8C32B223686", "statement_terminator", "sb.scalar.statement_terminator", "character", "lexeme.statement_terminator"},
    {"SBSQL-9FB7E7E0066C", "comment_line", "sb.scalar.comment_line", "character", "lexeme.comment_line"},
    {"SBSQL-BB0BB989E8B2", "comment_block", "sb.scalar.comment_block", "character", "lexeme.comment_block"},
    {"SBSQL-80864EB79EEB", "current_request_uuid", "sb.scalar.current_request_uuid", "uuid", "019f0000-0000-7000-8000-000000003125"},
    {"SBSQL-5CDBD2168B18", "current_dialect_version", "sb.scalar.current_dialect_version", "character", "sbsql.v3"},
    {"SBSQL-26BCBFF1FEED", "cardinality_violation", "sb.scalar.cardinality_violation", "character", "21000"},
    {"SBSQL-0032FE107FA7", "currency", "sb.scalar.currency", "character", "datatype.currency"},
    {"SBSQL-0382B5F327AA", "client_min_messages", "sb.scalar.client_min_messages", "character", "NOTICE"},
    {"SBSQL-03FC991DBA65", "merge_action", "sb.scalar.merge_action", "character", "dml.merge_action"},
    {"SBSQL-0D36181A09C4", "colocation", "sb.scalar.colocation", "character", "physical_layout.colocation"},
    {"SBSQL-1FF40A008189", "identifier_bare", "sb.scalar.identifier_bare", "character", "identifier_class.bare"},
    {"SBSQL-2477F077886D", "search_path", "sb.scalar.search_path", "character", "users.public"},
    {"SBSQL-27CC3A88C9CA", "sbsql_psql", "sb.scalar.sbsql_psql", "character", "sbsql.psql"},
    {"SBSQL-416D56ED915F", "sql_variant", "sb.scalar.sql_variant", "character", "datatype.sql_variant"},
    {"SBSQL-460F18A49506", "random_seed", "sb.scalar.random_seed", "uint64", "0"},
    {"SBSQL-461B56193B9B", "recursion_limit", "sb.scalar.recursion_limit", "uint64", "1024"},
    {"SBSQL-6DE72BA55723", "performance", "sb.scalar.performance", "character", "management.performance"},
    {"SBSQL-72FA26BECAC5", "parser_only", "sb.scalar.parser_only", "character", "scope.parser_only"},
    {"SBSQL-73BA9BC6005B", "deprecated", "sb.scalar.deprecated", "character", "keyword_class.deprecated"},
    {"SBSQL-7D66C2236A2B", "filesystem", "sb.scalar.filesystem", "character", "scope.filesystem_public_refused"},
    {"SBSQL-80B16048C80F", "refuse", "sb.scalar.refuse", "character", "decision.refuse"},
    {"SBSQL-823D51652BD0", "metrics", "sb.scalar.metrics", "character", "management.metrics"},
    {"SBSQL-85D62227A882", "catalog_read", "sb.scalar.catalog_read", "character", "authority.catalog_read"},
    {"SBSQL-87F8BED4D9EE", "donor_log_compatibility", "sb.scalar.donor_log_compatibility", "character", "compatibility.donor_log_non_authority"},
    {"SBSQL-92C226CF5A7A", "fail_closed", "sb.scalar.fail_closed", "character", "decision.fail_closed"},
    {"SBSQL-980131EAA57E", "requires_new_function", "sb.scalar.requires_new_function", "character", "implementation.requires_new_function"},
    {"SBSQL-A069D1ED14C0", "random_seed_control", "sb.scalar.random_seed_control", "character", "policy.random_seed_control"},
    {"SBSQL-B777E985366D", "evidence", "sb.scalar.evidence", "character", "evidence.required"},
    {"SBSQL-B845A701EF3C", "private_profile_read", "sb.scalar.private_profile_read", "character", "authority.private_profile_read"},
    {"SBSQL-C9883DF74D82", "evidence_chain_uuid", "sb.scalar.evidence_chain_uuid", "uuid", "019f0000-0000-7000-8000-000000003125"},
    {"SBSQL-CE7F2EE0D34E", "parameter_marker", "sb.scalar.parameter_marker", "character", "token.parameter_marker"},
    {"SBSQL-D437EC74B872", "security", "sb.scalar.security", "character", "management.security"},
    {"SBSQL-DC3ADB63538F", "localized_label", "sb.scalar.localized_label", "character", "label.localized"},
    {"SBSQL-E302317C73E2", "policy_blocked", "sb.scalar.policy_blocked", "character", "decision.policy_blocked"},
    {"SBSQL-E9EC607BA6D8", "notice", "sb.scalar.notice", "character", "NOTICE"},
    {"SBSQL-F1C822127E64", "dictionary_encoded", "sb.scalar.dictionary_encoded", "character", "encoding.dictionary"},
    {"SBSQL-06DAC31C3A89", "unresolved", "sb.scalar.unresolved", "character", "decision.unresolved"},
    {"SBSQL-0FEC8BF7CD54", "public", "sb.scalar.public", "character", "schema.public"},
    {"SBSQL-112E5B307AE1", "tablegroup", "sb.scalar.tablegroup", "character", "physical_layout.tablegroup"},
    {"SBSQL-131758ECDAEC", "none", "sb.scalar.none", "character", "value.none"},
    {"SBSQL-15828625DD4A", "descriptor", "sb.scalar.descriptor", "character", "metadata.descriptor"},
    {"SBSQL-18E2D41E637B", "engine", "sb.scalar.engine", "character", "authority.engine"},
    {"SBSQL-3456DC1DC1E6", "catalog", "sb.scalar.catalog", "character", "authority.catalog"},
    {"SBSQL-456D4BF70496", "unsupported", "sb.scalar.unsupported", "character", "decision.unsupported"},
    {"SBSQL-46D0BFB6ED61", "mergetree", "sb.scalar.mergetree", "character", "physical_layout.mergetree"},
    {"SBSQL-6AA173CC38F1", "refused", "sb.scalar.refused", "character", "decision.refused"},
    {"SBSQL-70117DFD73D9", "innodb", "sb.scalar.innodb", "character", "donor_storage.innodb"},
    {"SBSQL-902E2EF680C8", "hnsw", "sb.scalar.hnsw", "character", "index_method.hnsw"},
    {"SBSQL-98F7C17D42D9", "sessions", "sb.scalar.sessions", "character", "management.sessions"},
    {"SBSQL-9B1E3F7A4C5F", "asof", "sb.scalar.asof", "character", "temporal.asof"},
    {"SBSQL-A19CEADCF8F5", "post_event", "sb.scalar.post_event", "character", "event.post"},
    {"SBSQL-A770228DEC74", "regional", "sb.scalar.regional", "character", "locality.regional"},
    {"SBSQL-B6E1C5B44AAC", "ivf_flat", "sb.scalar.ivf_flat", "character", "index_method.ivf_flat"},
    {"SBSQL-BB09AA368A93", "tsquery", "sb.scalar.tsquery", "character", "datatype.tsquery"},
    {"SBSQL-C549A9F3CF89", "client_address", "sb.scalar.client_address", "character", "session.client_address"},
    {"SBSQL-CD402A856EE1", "txn", "sb.scalar.txn", "character", "transaction.alias"},
    {"SBSQL-D7D779AE16BC", "hierarchyid", "sb.scalar.hierarchyid", "character", "datatype.hierarchyid"},
    {"SBSQL-E217738F653C", "locality", "sb.scalar.locality", "character", "physical_layout.locality"},
    {"SBSQL-FD4C6500CBC7", "unknown", "sb.scalar.unknown", "character", "value.unknown"},
    {"SBSQL-FE93CA721F82", "sortop", "sb.scalar.sortop", "character", "operator.sortop"},
    {"SBSQL-0E3B7964D1F9", "customer_id", "sb.scalar.customer_id", "character", "fixture.identifier.customer_id"},
    {"SBSQL-5EF1F41AA6DE", "customers", "sb.scalar.customers", "character", "fixture.identifier.customers"},
    {"SBSQL-64EA78126DC3", "sep", "sb.scalar.sep", "character", "fixture.identifier.sep"},
    {"SBSQL-976C8ECD388C", "part", "sb.scalar.part", "character", "fixture.identifier.part"},
    {"SBSQL-9EF349862A64", "value", "sb.scalar.value", "character", "fixture.identifier.value"},
    {"SBSQL-C6DE029570B1", "expr", "sb.scalar.expr", "character", "fixture.identifier.expr"},
    {"SBSQL-132BACF604F3", "private_surface_refused", "sb.scalar.private_surface_refused", "character", "diagnostic.private_surface_refused"},
    {"SBSQL-2E1DD952FDAF", "no_request", "sb.scalar.no_request", "character", "diagnostic.no_request"},
    {"SBSQL-38565DFB4027", "no_statement", "sb.scalar.no_statement", "character", "diagnostic.no_statement"},
    {"SBSQL-3AA8E65D9971", "unsupported_refused_by_design", "sb.scalar.unsupported_refused_by_design", "character", "decision.unsupported_refused_by_design"},
    {"SBSQL-406B1DB66B8A", "operator_overload_unresolved", "sb.scalar.operator_overload_unresolved", "character", "diagnostic.operator_overload_unresolved"},
    {"SBSQL-4762703600F0", "no_transaction", "sb.scalar.no_transaction", "character", "diagnostic.no_transaction"},
    {"SBSQL-6ED46344E647", "requires_function_authoring", "sb.scalar.requires_function_authoring", "character", "implementation.requires_function_authoring"},
    {"SBSQL-7A84E49081E9", "event_trigger_authority_unavailable", "sb.scalar.event_trigger_authority_unavailable", "character", "authority.event_trigger_unavailable"},
    {"SBSQL-7F00278E0723", "capability_required", "sb.scalar.capability_required", "character", "diagnostic.capability_required"},
    {"SBSQL-882F0BF84BCC", "psql_case_not_found", "sb.scalar.psql_case_not_found", "character", "psql.case_not_found"},
    {"SBSQL-95878BFEDF43", "syntax_parser_only", "sb.scalar.syntax_parser_only", "character", "syntax.parser_only"},
    {"SBSQL-A371FE7C3BAA", "donor_only_rewrite", "sb.scalar.donor_only_rewrite", "character", "donor.rewrite_only"},
    {"SBSQL-A57396612A09", "object_resolution_failed", "sb.scalar.object_resolution_failed", "character", "diagnostic.object_resolution_failed"},
    {"SBSQL-B8E49C049ECB", "error_diagnostic_uuid", "sb.scalar.error_diagnostic_uuid", "uuid", "019f0000-0000-7000-8000-000000003128"},
    {"SBSQL-91F466E96DE4", "transaction", "sb.scalar.transaction", "character", "fixture.identifier.transaction"},
    {"SBSQL-BB49C3D09E24", "context_ambiguous", "sb.scalar.context_ambiguous", "character", "diagnostic.context_ambiguous"},
    {"SBSQL-CE3790BA0486", "policy_blocked_diagnostic", "sb.scalar.policy_blocked_diagnostic", "character", "decision.policy_blocked"},
    {"SBSQL-CB2705E35D88", "diag_sqlstate", "sb.scalar.diag_sqlstate", "character", "00000"},
    {"SBSQL-D2A2D11E9991", "canonical_function_idempotency_requirement", "sb.scalar.canonical_function_idempotency_requirement", "character", "metadata.idempotency_requirement"},
    {"SBSQL-D4C7802D088A", "deprecation_warning", "sb.scalar.deprecation_warning", "character", "warning.deprecation"},
    {"SBSQL-E01305A870F7", "syntax_unsupported", "sb.scalar.syntax_unsupported", "character", "syntax.unsupported"},
    {"SBSQL-E41591430FF1", "dynamic_sql_untrusted", "sb.scalar.dynamic_sql_untrusted", "character", "sblr.dynamic_sql.untrusted"},
    {"SBSQL-F4443D288A35", "udr_admission_denied", "sb.scalar.udr_admission_denied", "character", "sblr.udr.admission_denied"},
    {"SBSQL-279F22AD9A6E", "statement", "sb.scalar.statement", "character", "fixture.identifier.statement"},
    {"SBSQL-4555F205B04B", "table", "sb.scalar.table", "character", "fixture.identifier.table"},
    {"SBSQL-4A468A1D2EF5", "a", "sb.scalar.a", "character", "fixture.identifier.a"},
    {"SBSQL-4E297DAEBA5B", "x", "sb.scalar.x", "character", "fixture.identifier.x"},
    {"SBSQL-ADBBF56B1E71", "t", "sb.scalar.t", "character", "fixture.identifier.t"},
    {"SBSQL-C643D29F39E1", "name", "sb.scalar.name", "character", "fixture.identifier.name"},
    {"SBSQL-B4D68D87A882", "customer", "sb.scalar.customer", "character", "fixture.identifier.customer"},
    {"SBSQL-F81E6A7D24D3", "session", "sb.scalar.session", "character", "fixture.identifier.session"},
    {"SBSQL-6B594AD9CCAB", "alter", "sb.scalar.alter", "character", "keyword.contextual.alter"},
    {"SBSQL-0F2568295099", "as", "sb.scalar.as", "character", "keyword.contextual.as"},
    {"SBSQL-054E79ED816E", "begin", "sb.scalar.begin", "character", "keyword.contextual.begin"},
    {"SBSQL-52AD0BC41D43", "create", "sb.scalar.create", "character", "keyword.contextual.create"},
    {"SBSQL-3E7F642D05FF", "cross", "sb.scalar.cross", "character", "keyword.contextual.cross"},
    {"SBSQL-AA7E9547F110", "distinct", "sb.scalar.distinct", "character", "keyword.contextual.distinct"},
    {"SBSQL-D94ED65E33DB", "drop", "sb.scalar.drop", "character", "keyword.contextual.drop"},
    {"SBSQL-6FA47CF68727", "else", "sb.scalar.else", "character", "keyword.contextual.else"},
    {"SBSQL-DAFA3EAF3212", "end", "sb.scalar.end", "character", "keyword.contextual.end"},
    {"SBSQL-09C5727E3E1B", "events", "sb.scalar.events", "character", "keyword.contextual.events"},
    {"SBSQL-169A3A38AFD4", "exists", "sb.scalar.exists", "character", "keyword.contextual.exists"},
    {"SBSQL-003C1703274A", "full", "sb.scalar.full", "character", "keyword.contextual.full"},
    {"SBSQL-DE3A507DF9C4", "index", "sb.scalar.index", "character", "keyword.contextual.index"},
    {"SBSQL-D7351B1C962A", "inner", "sb.scalar.inner", "character", "keyword.contextual.inner"},
    {"SBSQL-E2F9B4091D9B", "is", "sb.scalar.is", "character", "keyword.contextual.is"},
    {"SBSQL-999C058E0160", "lateral", "sb.scalar.lateral", "character", "keyword.contextual.lateral"},
    {"SBSQL-ACA52E0AA80F", "natural", "sb.scalar.natural", "character", "keyword.contextual.natural"},
    {"SBSQL-A1491C2B87CE", "on", "sb.scalar.on", "character", "keyword.contextual.on"},
    {"SBSQL-11D5D2852576", "outer", "sb.scalar.outer", "character", "keyword.contextual.outer"},
    {"SBSQL-907D2DCB4E7A", "sequence", "sb.scalar.sequence", "character", "keyword.contextual.sequence"},
    {"SBSQL-94CD1F009C96", "show", "sb.scalar.show", "character", "keyword.contextual.show"},
    {"SBSQL-05EAA3104951", "similar", "sb.scalar.similar", "character", "keyword.contextual.similar"},
    {"SBSQL-0B5204549537", "then", "sb.scalar.then", "character", "keyword.contextual.then"},
    {"SBSQL-540CB1A2A056", "upsert", "sb.scalar.upsert", "character", "keyword.contextual.upsert"},
    {"SBSQL-00E6C0F7766E", "using", "sb.scalar.using", "character", "keyword.contextual.using"},
    {"SBSQL-AE7A4534FAF9", "view", "sb.scalar.view", "character", "keyword.contextual.view"},
    {"SBSQL-A5E97200C0D9", "wait", "sb.scalar.wait", "character", "keyword.contextual.wait"},
    {"SBSQL-3666F7699C5E", "when", "sb.scalar.when", "character", "keyword.contextual.when"},
    {"SBSQL-CC9235354362", "with", "sb.scalar.with", "character", "keyword.contextual.with"},
    {"SBSQL-853009230194", "stmt.null", "sb.scalar.stmt_null", "character", "statement.null"},
};

struct Sbsfc016PolicyRefusalRow {
  const char* surface_id;
  const char* sql_expression;
  const char* function_id;
};

struct Sbsfc016ProceduralDiagnosticRow {
  const char* surface_id;
  const char* function_name;
  const char* function_id;
  const char* diagnostic_id;
  const char* diagnostic_detail;
};

constexpr Sbsfc016ProceduralDiagnosticRow kSbsfc016ProceduralDiagnosticRows[] = {
    {"SBSQL-B7E4638E5F7C", "signal", "sb.scalar.signal",
     "SB_DIAG_PROCEDURAL_SIGNAL", "SIGNAL emitted SBsql procedural diagnostic"},
    {"SBSQL-22A8D96173B3", "raise", "sb.scalar.raise",
     "SB_DIAG_PROCEDURAL_RAISE", "RAISE emitted SBsql procedural diagnostic"},
    {"SBSQL-7390531C3071", "resignal", "sb.scalar.resignal",
     "SB_DIAG_PROCEDURAL_RESIGNAL", "RESIGNAL emitted SBsql procedural diagnostic"},
};

constexpr Sbsfc016PolicyRefusalRow kSbsfc016PolicyRefusalRows[] = {
    {"SBSQL-2DB83873C621", "@@autocommit", "sb.scalar.refusal_system_variable_autocommit"},
    {"SBSQL-5FBC168DF633", "@@session.autocommit", "sb.scalar.refusal_system_variable_session_autocommit"},
    {"SBSQL-7269DD8C7658", "@@session.var", "sb.scalar.refusal_system_variable_session_var"},
    {"SBSQL-9637EA2DFC5A", "@@global.var", "sb.scalar.refusal_system_variable_global_var"},
    {"SBSQL-7697E4BCE46F", "@@hostname", "sb.scalar.refusal_system_variable_hostname"},
    {"SBSQL-EED4041BEB12", "@@SERVERNAME", "sb.scalar.refusal_system_variable_servername"},
    {"SBSQL-F89DC25F25BC", "current_query()", "sb.scalar.refusal_current_query"},
    {"SBSQL-243B7D1E55C5", "pg_read_binary_file('/tmp/sb_refusal.bin')", "sb.scalar.refusal_pg_read_binary_file"},
    {"SBSQL-4C8128BF2BAA", "pg_read_file('/tmp/sb_refusal.txt')", "sb.scalar.refusal_pg_read_file"},
    {"SBSQL-C57AB769021B", "pg_read_server_files('/tmp/sb_refusal.txt')", "sb.scalar.refusal_pg_read_server_files"},
    {"SBSQL-ED59B532C7C6", "pg_ls_dir('/tmp')", "sb.scalar.refusal_pg_ls_dir"},
    {"SBSQL-AAAAAA49C991", "lo_import('/tmp/sb_refusal.bin')", "sb.scalar.refusal_lo_import"},
    {"SBSQL-45B0362E386F", "lo_export(1, '/tmp/sb_refusal.bin')", "sb.scalar.refusal_lo_export"},
    {"SBSQL-CEA7A25D6025", "pg_reload_conf()", "sb.scalar.refusal_pg_reload_conf"},
    {"SBSQL-4F59D2264C2C", "pg_rotate_logfile()", "sb.scalar.refusal_pg_rotate_logfile"},
    {"SBSQL-4189F1BE1A5A", "WAL()", "sb.scalar.refusal_wal"},
    {"SBSQL-1E628CF3F25A", "pg_sleep(1)", "sb.scalar.refusal_pg_sleep"},
    {"SBSQL-0EEF9D0588DE", "pg_terminate_session(42)", "sb.scalar.refusal_pg_terminate_session"},
    {"SBSQL-1D7526609BC7", "pg_cron()", "sb.scalar.refusal_pg_cron"},
    {"SBSQL-E0DDB9379496", "pg_backend_pid()", "sb.scalar.refusal_pg_backend_pid"},
    {"SBSQL-3E730331C624", "sql.bulk_exceptions()", "sb.scalar.refusal_sql_bulk_exceptions"},
    {"SBSQL-366A50FD36A0", "IDEMPOTENCY('request-key')", "sb.scalar.refusal_idempotency"},
    {"SBSQL-1C4864BC8A40", "EXISTSNODE('<x/>', '/x')", "sb.scalar.refusal_existsnode"},
    {"SBSQL-D2F4FCC62B8D", "UPDATEXML('<x/>', '/x', 'y')", "sb.scalar.refusal_updatexml"},
    {"SBSQL-24B221BB6B46", "XMLELEMENT('x')", "sb.scalar.refusal_xmlelement"},
    {"SBSQL-219F078BDF7C", "MODEL()", "sb.scalar.refusal_model"},
    {"SBSQL-1EC5E3273B4D", "EVENT()", "sb.scalar.refusal_event"},
    {"SBSQL-EBD034715D41", "OPERATOR()", "sb.scalar.refusal_operator"},
    {"SBSQL-82082E76658A", "SYS_CONTEXT('USERENV','CLIENT_INFO')", "sb.scalar.refusal_sys_context_client_info"},
    {"SBSQL-93B790433D9D", "RDB$GET_CONTEXT('USER_SESSION','CLIENT_PID')", "sb.scalar.refusal_rdb_get_context_client_pid"},
    {"SBSQL-18E22DA52C20", "WITH(READPAST)", "sb.scalar.refusal_with_readpast"},
    {"SBSQL-1CB047ADD7B5", "WITH(NOLOCK)", "sb.scalar.refusal_with_nolock"},
    {"SBSQL-F9144106E9F2", "EXPLAIN(WALTRUE)", "sb.scalar.refusal_explain_waltrue"},
    {"SBSQL-E9D1FF70EC0D", "EXPLAIN(DONOR_LOG_COMPATIBILITYTRUE)", "sb.scalar.refusal_explain_donor_log_compatibilitytrue"},
    {"SBSQL-BEB64FF33809", "EXPLAIN(EVIDENCETRUE)", "sb.scalar.refusal_explain_evidencetrue"},
    {"SBSQL-5D9C952A3697", "@@TRANCOUNT", "sb.scalar.refusal_system_variable_trancount"},
    {"SBSQL-511BC12EEF82", "ATOMIC", "sb.scalar.refusal_atomic"},
    {"SBSQL-75CF9FACACE2", "CALL", "sb.scalar.refusal_call"},
    {"SBSQL-7B58373BC23A", "FORALL", "sb.scalar.refusal_forall"},
    {"SBSQL-83401CA819BA", "ATTACH", "sb.scalar.refusal_attach"},
    {"SBSQL-88DF354F5A82", "DESCRIBE", "sb.scalar.refusal_describe"},
    {"SBSQL-A43A427BA70A", "EXECUTE_DYNAMIC_SQL", "sb.scalar.refusal_execute_dynamic_sql"},
    {"SBSQL-B200BD7425CA", "OPERATOR_MANIFEST.csv", "sb.scalar.refusal_operator_manifest_csv"},
    {"SBSQL-B9BFDB8979B1", "DETACH", "sb.scalar.refusal_detach"},
    {"SBSQL-C10FBE98B475", "INTO", "sb.scalar.refusal_into"},
    {"SBSQL-CDDDEF1DE9AB", "SUSPEND", "sb.scalar.refusal_suspend"},
    {"SBSQL-D3442C734EE8", "RETURNING", "sb.scalar.refusal_returning"},
    {"SBSQL-D6ACEF2C5FE1", "OVER", "sb.scalar.refusal_over"},
    {"SBSQL-DE49BA68D87E", "EXCLUDE", "sb.scalar.refusal_exclude"},
    {"SBSQL-E7A6869DB4C1", "COLLATE", "sb.scalar.refusal_collate"},
    {"SBSQL-EE837B9B5297", "DESC", "sb.scalar.refusal_desc"},
    {"SBSQL-73AAF62A5CC3", "SYS_CONTEXT('USERENV','NAME')", "sb.scalar.refusal_sys_context_userenv_name"},
    {"SBSQL-B26CC22AE57C", "RDB$GET_CONTEXT('USER_SESSION','NAME')", "sb.scalar.refusal_rdb_get_context_user_session_name"},
    {"SBSQL-33A632BC9375", "OVERLAPS('2026-01-01','2026-02-01','2026-01-15','2026-02-15')", "sb.scalar.refusal_overlaps"},
    {"SBSQL-F89F3F1D7CBB", "IDENTITY()", "sb.scalar.refusal_identity"},
};

constexpr Sbsfc016PolicyRefusalRow kSbsfc027PolicyRefusalRows[] = {
    {"SBSQL-8EA33E3F97F0", "ARRAY(SELECT 1)", "sb.scalar.refusal_array_subquery"},
    {"SBSQL-93C5DE08981B", "customer.table", "sb.scalar.refusal_customer_table"},
    {"SBSQL-DCA62654CB0F", "@@", "sb.scalar.refusal_at_at"},
    {"SBSQL-6860D73D6667", "f(name=>value)", "sb.scalar.refusal_named_argument"},
    {"SBSQL-971C709406A0", "@", "sb.scalar.refusal_at"},
};

constexpr Sbsfc016PolicyRefusalRow kSbsfc028AntiWalPolicyRefusalRows[] = {
    {"SBSQL-6637546ABDF0", "donor_log_mode", "sb.scalar.refusal_donor_log_mode"},
    {"SBSQL-17068E518638", "checkpoint_donor_log", "sb.scalar.refusal_checkpoint_donor_log"},
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::size_t FindJsonStringEnd(std::string_view text, std::size_t start) {
  bool escaped = false;
  for (std::size_t index = start; index < text.size(); ++index) {
    const char ch = text[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return index;
  }
  return std::string_view::npos;
}

std::string UnescapeJsonString(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool escaped = false;
  for (const char ch : text) {
    if (escaped) {
      switch (ch) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        default: out.push_back(ch); break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    out.push_back(ch);
  }
  if (escaped) out.push_back('\\');
  return out;
}

sblr::SblrOperationEnvelope EngineEnvelopeFromParserEnvelope(const SblrEnvelope& parser_envelope) {
  auto envelope = sblr::MakeSblrEnvelope(
      parser_envelope.engine_api_operation_id.empty() ? parser_envelope.operation_id
                                                      : parser_envelope.engine_api_operation_id,
      parser_envelope.sblr_opcode,
      parser_envelope.trace_key);
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;

  std::size_t index = 0;
  while (index < parser_envelope.payload.size()) {
    const std::size_t key_start_quote = parser_envelope.payload.find('"', index);
    if (key_start_quote == std::string_view::npos) break;
    const std::size_t key_end_quote = FindJsonStringEnd(parser_envelope.payload, key_start_quote + 1);
    if (key_end_quote == std::string_view::npos) break;
    std::size_t cursor = key_end_quote + 1;
    while (cursor < parser_envelope.payload.size() &&
           std::isspace(static_cast<unsigned char>(parser_envelope.payload[cursor]))) {
      ++cursor;
    }
    if (cursor >= parser_envelope.payload.size() || parser_envelope.payload[cursor] != ':') {
      index = key_end_quote + 1;
      continue;
    }
    ++cursor;
    while (cursor < parser_envelope.payload.size() &&
           std::isspace(static_cast<unsigned char>(parser_envelope.payload[cursor]))) {
      ++cursor;
    }
    if (cursor >= parser_envelope.payload.size() || parser_envelope.payload[cursor] != '"') {
      index = cursor + 1;
      continue;
    }
    const std::size_t value_end_quote = FindJsonStringEnd(parser_envelope.payload, cursor + 1);
    if (value_end_quote == std::string_view::npos) break;
    envelope.operands.push_back(
        {"text",
         UnescapeJsonString(parser_envelope.payload.substr(key_start_quote + 1,
                                                           key_end_quote - key_start_quote - 1)),
         UnescapeJsonString(parser_envelope.payload.substr(cursor + 1,
                                                           value_end_quote - cursor - 1))});
    index = value_end_quote + 1;
  }
  return envelope;
}

bool HasDiagnosticCode(const MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasNumericLiteralFamily(const CstDocument& cst, std::string_view text,
                             std::string_view literal_family) {
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kNumericLiteral && token.text == text &&
        token.literal_family == literal_family) {
      return true;
    }
  }
  return false;
}

std::string FirstBinaryLiteralText(const CstDocument& cst,
                                   std::string_view literal_family) {
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kBinaryLiteral &&
        token.literal_family == literal_family) {
      return token.text;
    }
  }
  return {};
}

std::string FirstUuidLiteralText(const CstDocument& cst) {
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kUuidLiteral && token.literal_family == "uuid") {
      return token.text;
    }
  }
  return {};
}

std::string FirstTemporalLiteralText(const CstDocument& cst,
                                     std::string_view literal_family) {
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kTemporalLiteral &&
        token.literal_family == literal_family) {
      return token.text;
    }
  }
  return {};
}

std::string FirstDocumentLiteralText(const CstDocument& cst,
                                     std::string_view literal_family) {
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kDocumentLiteral &&
        token.literal_family == literal_family) {
      return token.text;
    }
  }
  return {};
}

std::string FirstVectorLiteralText(const CstDocument& cst,
                                   std::string_view literal_family) {
  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kVectorLiteral &&
        token.literal_family == literal_family) {
      return token.text;
    }
  }
  return {};
}

std::string EvidenceMessage(const ScalarLiteralGrammarRowEvidence& row,
                            std::string_view stage,
                            std::string_view detail) {
  std::string message;
  message.reserve(192);
  message.append(row.surface_id);
  message.append(" ");
  message.append(row.canonical_name);
  message.append(" ");
  message.append(stage);
  message.append(": ");
  message.append(detail);
  return message;
}

void RequireScalarLiteralRegistryEvidence(const ScalarLiteralGrammarRowEvidence& evidence) {
  const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
  Require(row != nullptr, EvidenceMessage(evidence, "registry", "missing generated registry row"));
  Require(row->canonical_name == evidence.canonical_name,
          EvidenceMessage(evidence, "registry", "canonical name mismatch"));
  Require(row->surface_kind == "grammar_production",
          EvidenceMessage(evidence, "registry", "surface kind mismatch"));
  Require(row->family == evidence.family,
          EvidenceMessage(evidence, "registry", "family mismatch"));
  Require(row->source_status == "native_now",
          EvidenceMessage(evidence, "registry", "source status mismatch"));
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(evidence, "registry", "cluster scope mismatch"));
  Require(row->sblr_operation_family == evidence.sblr_operation_family,
          EvidenceMessage(evidence, "registry", "SBLR operation family mismatch"));
  Require(row->parser_handler_key == evidence.parser_handler_key,
          EvidenceMessage(evidence, "registry", "parser handler key mismatch"));
  Require(row->lowering_handler_key == evidence.lowering_handler_key,
          EvidenceMessage(evidence, "registry", "lowering handler key mismatch"));
  Require(row->server_admission_key == evidence.server_admission_key,
          EvidenceMessage(evidence, "registry", "server admission key mismatch"));
  Require(row->engine_rule_key == evidence.engine_rule_key,
          EvidenceMessage(evidence, "registry", "engine rule key mismatch"));
}

void RequireScalarLiteralFunctionRegistryEvidence(const ScalarLiteralGrammarRowEvidence& evidence) {
  const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
  Require(row != nullptr, EvidenceMessage(evidence, "registry", "missing generated registry row"));
  Require(row->canonical_name == evidence.canonical_name,
          EvidenceMessage(evidence, "registry", "canonical name mismatch"));
  Require(row->surface_kind == "function",
          EvidenceMessage(evidence, "registry", "surface kind mismatch"));
  Require(row->family == "expression_runtime",
          EvidenceMessage(evidence, "registry", "family mismatch"));
  Require(row->source_status == "native_now",
          EvidenceMessage(evidence, "registry", "source status mismatch"));
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(evidence, "registry", "cluster scope mismatch"));
  Require(row->sblr_operation_family == "sblr.expression.runtime.v3",
          EvidenceMessage(evidence, "registry", "SBLR operation family mismatch"));
  Require(row->parser_handler_key == "parser.expression_runtime.function",
          EvidenceMessage(evidence, "registry", "parser handler key mismatch"));
  Require(row->lowering_handler_key == "lowering.expression_runtime.function",
          EvidenceMessage(evidence, "registry", "lowering handler key mismatch"));
  Require(row->server_admission_key == "server.admission.sblr_expression_runtime_v3",
          EvidenceMessage(evidence, "registry", "server admission key mismatch"));
  Require(row->engine_rule_key == "engine.rule.sblr_expression_runtime_v3",
          EvidenceMessage(evidence, "registry", "engine rule key mismatch"));
}

void RequireConcatExpressionRegistryEvidence() {
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-D69AF9916294");
  Require(row != nullptr, "concat_expr registry row missing");
  Require(row->canonical_name == "concat_expr", "concat_expr registry canonical name mismatch");
  Require(row->surface_kind == "grammar_production", "concat_expr registry surface kind mismatch");
  Require(row->family == "general", "concat_expr registry family mismatch");
  Require(row->source_status == "native_now", "concat_expr registry status mismatch");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "concat_expr registry cluster scope mismatch");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "concat_expr registry SBLR family mismatch");
  Require(row->parser_handler_key == "parser.grammar_ast",
          "concat_expr parser handler mismatch");
  Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          "concat_expr lowering handler mismatch");
  Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
          "concat_expr server admission key mismatch");
  Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          "concat_expr engine rule mismatch");
}

void RequireFunctionCallGrammarRegistryEvidence() {
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-17B72695FA1A");
  Require(row != nullptr, "function_call registry row missing");
  Require(row->canonical_name == "function_call",
          "function_call registry canonical name mismatch");
  Require(row->surface_kind == "grammar_production",
          "function_call registry surface kind mismatch");
  Require(row->family == "general", "function_call registry family mismatch");
  Require(row->source_status == "native_now", "function_call registry status mismatch");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "function_call registry cluster scope mismatch");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "function_call registry SBLR family mismatch");
  Require(row->parser_handler_key == "parser.grammar_ast",
          "function_call parser handler mismatch");
  Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          "function_call lowering handler mismatch");
  Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
          "function_call server admission key mismatch");
  Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          "function_call engine rule mismatch");
}

void RequireCurrentValueFormRegistryEvidence() {
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-1FF37754C949");
  Require(row != nullptr, "current_value_form registry row missing");
  Require(row->canonical_name == "current_value_form",
          "current_value_form registry canonical name mismatch");
  Require(row->surface_kind == "grammar_production",
          "current_value_form registry surface kind mismatch");
  Require(row->family == "general", "current_value_form registry family mismatch");
  Require(row->source_status == "native_now",
          "current_value_form registry status mismatch");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "current_value_form registry cluster scope mismatch");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "current_value_form registry SBLR family mismatch");
  Require(row->parser_handler_key == "parser.grammar_ast",
          "current_value_form parser handler mismatch");
  Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          "current_value_form lowering handler mismatch");
  Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
          "current_value_form server admission key mismatch");
  Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          "current_value_form engine rule mismatch");
}

void RequireExtractFormRegistryEvidence() {
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-A9F195402FC3");
  Require(row != nullptr, "extract_form registry row missing");
  Require(row->canonical_name == "extract_form",
          "extract_form registry canonical name mismatch");
  Require(row->surface_kind == "grammar_production",
          "extract_form registry surface kind mismatch");
  Require(row->family == "general", "extract_form registry family mismatch");
  Require(row->source_status == "native_now",
          "extract_form registry status mismatch");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "extract_form registry cluster scope mismatch");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "extract_form registry SBLR family mismatch");
  Require(row->parser_handler_key == "parser.grammar_ast",
          "extract_form parser handler mismatch");
  Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          "extract_form lowering handler mismatch");
  Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
          "extract_form server admission key mismatch");
  Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          "extract_form engine rule mismatch");
}

void RequireSpecialKeywordTextRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-240551B3294D", "sb.special.substring_keyword"},
      {"SBSQL-053A6453B4A8", "sb.special.trim_keyword"},
  };
  for (const auto& evidence : rows) {
    const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
    Require(row != nullptr, "special keyword text registry row missing");
    Require(row->canonical_name == evidence.canonical_name,
            "special keyword text registry canonical name mismatch");
    Require(row->surface_kind == "function",
            "special keyword text registry surface kind mismatch");
    Require(row->family == "expression_runtime",
            "special keyword text registry family mismatch");
    Require(row->source_status == "native_now",
            "special keyword text registry status mismatch");
    Require(row->cluster_scope == "noncluster_or_profile_scoped",
            "special keyword text registry cluster scope mismatch");
    Require(row->sblr_operation_family == "sblr.expression.runtime.v3",
            "special keyword text registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.expression_runtime.function",
            "special keyword text parser handler mismatch");
    Require(row->lowering_handler_key == "lowering.expression_runtime.function",
            "special keyword text lowering handler mismatch");
    Require(row->server_admission_key == "server.admission.sblr_expression_runtime_v3",
            "special keyword text server admission key mismatch");
    Require(row->engine_rule_key == "engine.rule.sblr_expression_runtime_v3",
            "special keyword text engine rule mismatch");
  }
}

void RequireSqlKeywordTextFormRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-271EC4F56C42", "substring_form"},
      {"SBSQL-24DC9B10C2A7", "trim_form"},
      {"SBSQL-51401468C798", "position_form"},
      {"SBSQL-74FCAE5A5A7D", "overlay_form"},
  };
  for (const auto& evidence : rows) {
    const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
    Require(row != nullptr, "SQL keyword text grammar-form registry row missing");
    Require(row->canonical_name == evidence.canonical_name,
            "SQL keyword text grammar-form registry canonical name mismatch");
    Require(row->surface_kind == "grammar_production",
            "SQL keyword text grammar-form registry surface kind mismatch");
    Require(row->family == "general",
            "SQL keyword text grammar-form registry family mismatch");
    Require(row->source_status == "native_now",
            "SQL keyword text grammar-form registry status mismatch");
    Require(row->cluster_scope == "noncluster_or_profile_scoped",
            "SQL keyword text grammar-form registry cluster scope mismatch");
    Require(row->sblr_operation_family == "sblr.general.operation.v3",
            "SQL keyword text grammar-form registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.grammar_ast",
            "SQL keyword text grammar-form parser handler mismatch");
    Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
            "SQL keyword text grammar-form lowering handler mismatch");
    Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
            "SQL keyword text grammar-form server admission key mismatch");
    Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
            "SQL keyword text grammar-form engine rule mismatch");
  }
}

void RequireGreatestLeastFormRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-05E96B0F3F00", "greatest_form"},
      {"SBSQL-0DB23C2EED07", "least_form"},
  };
  for (const auto& evidence : rows) {
    const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
    Require(row != nullptr, "greatest/least grammar-form registry row missing");
    Require(row->canonical_name == evidence.canonical_name,
            "greatest/least grammar-form registry canonical name mismatch");
    Require(row->surface_kind == "grammar_production",
            "greatest/least grammar-form registry surface kind mismatch");
    Require(row->family == "general",
            "greatest/least grammar-form registry family mismatch");
    Require(row->source_status == "native_now",
            "greatest/least grammar-form registry status mismatch");
    Require(row->cluster_scope == "noncluster_or_profile_scoped",
            "greatest/least grammar-form registry cluster scope mismatch");
    Require(row->sblr_operation_family == "sblr.general.operation.v3",
            "greatest/least grammar-form registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.grammar_ast",
            "greatest/least grammar-form parser handler mismatch");
    Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
            "greatest/least grammar-form lowering handler mismatch");
    Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
            "greatest/least grammar-form server admission key mismatch");
    Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
            "greatest/least grammar-form engine rule mismatch");
  }
}

void RequireConditionalFormRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-A398437381A5", "coalesce_form"},
      {"SBSQL-6F8636E3AA55", "nullif_form"},
  };
  for (const auto& evidence : rows) {
    const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
    Require(row != nullptr, "conditional grammar-form registry row missing");
    Require(row->canonical_name == evidence.canonical_name,
            "conditional grammar-form registry canonical name mismatch");
    Require(row->surface_kind == "grammar_production",
            "conditional grammar-form registry surface kind mismatch");
    Require(row->family == "general",
            "conditional grammar-form registry family mismatch");
    Require(row->source_status == "native_now",
            "conditional grammar-form registry status mismatch");
    Require(row->cluster_scope == "noncluster_or_profile_scoped",
            "conditional grammar-form registry cluster scope mismatch");
    Require(row->sblr_operation_family == "sblr.general.operation.v3",
            "conditional grammar-form registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.grammar_ast",
            "conditional grammar-form parser handler mismatch");
    Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
            "conditional grammar-form lowering handler mismatch");
    Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
            "conditional grammar-form server admission key mismatch");
    Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
            "conditional grammar-form engine rule mismatch");
  }
}

void RequireLikeGrammarRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-263734C6F29A", "like_clause"},
      {"SBSQL-1D6043AA4146", "like_option"},
      {"SBSQL-93F23B0F097E", "like_options"},
  };
  for (const auto& evidence : rows) {
    const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
    Require(row != nullptr, "LIKE grammar registry row missing");
    Require(row->canonical_name == evidence.canonical_name,
            "LIKE grammar registry canonical name mismatch");
    Require(row->surface_kind == "grammar_production",
            "LIKE grammar registry surface kind mismatch");
    Require(row->family == "general",
            "LIKE grammar registry family mismatch");
    Require(row->source_status == "native_now",
            "LIKE grammar registry status mismatch");
    Require(row->cluster_scope == "noncluster_or_profile_scoped",
            "LIKE grammar registry cluster scope mismatch");
    Require(row->sblr_operation_family == "sblr.general.operation.v3",
            "LIKE grammar registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.grammar_ast",
            "LIKE grammar parser handler mismatch");
    Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
            "LIKE grammar lowering handler mismatch");
    Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
            "LIKE grammar server admission key mismatch");
    Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
            "LIKE grammar engine rule mismatch");
  }
}

void RequireLogicalExpressionGrammarRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-AB137C592B9D", "and_expr"},
      {"SBSQL-16A72B09C540", "or_expr"},
      {"SBSQL-516CDBE6991C", "not_expr"},
  };
  for (const auto& evidence : rows) {
    const auto* row = FindGeneratedSurfaceRegistryRowById(evidence.surface_id);
    Require(row != nullptr, "logical expression grammar registry row missing");
    Require(row->canonical_name == evidence.canonical_name,
            "logical expression grammar registry canonical name mismatch");
    Require(row->surface_kind == "grammar_production",
            "logical expression grammar registry surface kind mismatch");
    Require(row->family == "general",
            "logical expression grammar registry family mismatch");
    Require(row->source_status == "native_now",
            "logical expression grammar registry status mismatch");
    Require(row->cluster_scope == "noncluster_or_profile_scoped",
            "logical expression grammar registry cluster scope mismatch");
    Require(row->sblr_operation_family == "sblr.general.operation.v3",
            "logical expression grammar registry SBLR family mismatch");
    Require(row->parser_handler_key == "parser.grammar_ast",
            "logical expression grammar parser handler mismatch");
    Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
            "logical expression grammar lowering handler mismatch");
    Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
            "logical expression grammar server admission key mismatch");
    Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
            "logical expression grammar engine rule mismatch");
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000003111";
  session.connection_uuid = "019f0000-0000-7000-8000-000000003112";
  session.database_uuid = "019f0000-0000-7000-8000-000000003113";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000003114";
  config.bundle_contract_id = "sbp_sbsql@scalar-projection-route-test";
  config.build_id = "sbsql-scalar-projection-route-test";
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

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-query-scalar-projection";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000003121";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000003120";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000003122";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000003123";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000003124";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000003125";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000003128";
  context.transaction_isolation_level = "snapshot";
  context.current_sqlstate = "00000";
  context.client_protocol_uuid = "019e1600-0000-7000-8000-0000000000ee";
  context.application_name = "sbsql_conformance";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000003126";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000003127";
  context.local_transaction_id = 42;
  context.last_row_count = 7;
  context.last_row_count_present = true;
  context.statement_timestamp = "2026-05-12T14:23:45Z";
  context.transaction_timestamp = "2026-05-12T13:00:00Z";
  context.current_timestamp = "2026-05-12T14:23:46Z";
  context.current_monotonic_ns = "123456789";
  context.deterministic_random_u64 = 1ull << 63;
  context.deterministic_random_u64_present = true;
  context.deterministic_uuid_text = "550e8400-e29b-41d4-a716-446655440000";
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "4"});
  envelope.operands.push_back({"text", "projection_0_name", "one"});
  envelope.operands.push_back({"text", "projection_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_value", "1"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_1_name", "two"});
  envelope.operands.push_back({"text", "projection_1_type", "text"});
  envelope.operands.push_back({"text", "projection_1_value", "two"});
  envelope.operands.push_back({"text", "projection_1_is_null", "false"});
  envelope.operands.push_back({"text", "projection_2_name", "empty_value"});
  envelope.operands.push_back({"text", "projection_2_type", "null"});
  envelope.operands.push_back({"text", "projection_2_value", ""});
  envelope.operands.push_back({"text", "projection_2_is_null", "true"});
  envelope.operands.push_back({"text", "projection_3_name", "truth"});
  envelope.operands.push_back({"text", "projection_3_type", "boolean"});
  envelope.operands.push_back({"text", "projection_3_value", "true"});
  envelope.operands.push_back({"text", "projection_3_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope FalseLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.false_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "falsehood"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "boolean"});
  envelope.operands.push_back({"text", "projection_0_value", "false"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope DecimalLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.decimal_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "decimal_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "numeric"});
  envelope.operands.push_back({"text", "projection_0_value", "12.34"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope UintLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.uint_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "uint_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "uint64"});
  envelope.operands.push_back({"text", "projection_0_value", "42"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope Int128LiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.int128_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "int128_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "int128"});
  envelope.operands.push_back({"text", "projection_0_value", "123"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope Uint128LiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.uint128_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "uint128_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "uint128"});
  envelope.operands.push_back({"text", "projection_0_value", "123"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope Real128LiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.real128_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "real128_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "real128"});
  envelope.operands.push_back({"text", "projection_0_value", "1.25"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope FloatLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.float_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "float_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "numeric"});
  envelope.operands.push_back({"text", "projection_0_value", "1e2"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope BinaryLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.binary_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "binary_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "binary"});
  envelope.operands.push_back({"text", "projection_0_value", "00ff10"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope UuidLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.uuid_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "uuid_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "uuid"});
  envelope.operands.push_back({"text", "projection_0_value",
                               "550e8400-e29b-41d4-a716-446655440000"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope DateLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.date_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "date_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "date"});
  envelope.operands.push_back({"text", "projection_0_value", "2026-05-14"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope TimeLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.time_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "time_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "time"});
  envelope.operands.push_back({"text", "projection_0_value", "14:23:45"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope TimestampLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.timestamp_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "timestamp_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "timestamp"});
  envelope.operands.push_back({"text", "projection_0_value", "2026-05-14T14:23:45"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope IntervalLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.interval_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "interval_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "interval"});
  envelope.operands.push_back({"text", "projection_0_value", "1 day"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope DocumentLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.document_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "document_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "document"});
  envelope.operands.push_back({"text", "projection_0_value", "{\"a\":1}"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope JsonLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.json_literal_inline");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "json_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "json_document"});
  envelope.operands.push_back({"text", "projection_0_value", "{\"a\":1}"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope VectorLiteralEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.vector_literal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "vector_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "dense_vector"});
  envelope.operands.push_back({"text", "projection_0_value", "[1,2,3]"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

struct FunctionProjectionArg {
  std::string type;
  std::string value;
  bool is_null{false};
};

void AppendFunctionProjectionOperand(sblr::SblrOperationEnvelope& envelope,
                                     std::size_t projection_index,
                                     std::string_view name,
                                     std::string_view result_type,
                                     std::string_view function_id,
                                     std::initializer_list<FunctionProjectionArg> args) {
  const std::string prefix = "projection_" + std::to_string(projection_index);
  envelope.operands.push_back({"text", prefix + "_name", std::string(name)});
  envelope.operands.push_back({"text", prefix + "_expr_kind", "function"});
  envelope.operands.push_back({"text", prefix + "_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", prefix + "_type", std::string(result_type)});
  envelope.operands.push_back({"text", prefix + "_value", ""});
  envelope.operands.push_back({"text", prefix + "_is_null", "false"});
  envelope.operands.push_back({"text", prefix + "_function_id", std::string(function_id)});
  envelope.operands.push_back({"text", prefix + "_function_arg_count", std::to_string(args.size())});
  std::size_t arg_index = 0;
  for (const auto& arg : args) {
    const std::string arg_prefix = prefix + "_arg_" + std::to_string(arg_index);
    envelope.operands.push_back({"text", arg_prefix + "_type", arg.type});
    envelope.operands.push_back({"text", arg_prefix + "_value", arg.value});
    envelope.operands.push_back({"text", arg_prefix + "_is_null", arg.is_null ? "true" : "false"});
    ++arg_index;
  }
}

void AppendLiteralProjectionExpression(sblr::SblrOperationEnvelope& envelope,
                                       std::string_view prefix,
                                       std::string_view type,
                                       std::string_view value,
                                       bool is_null = false) {
  envelope.operands.push_back({"text", std::string(prefix) + "expr_kind", "literal"});
  envelope.operands.push_back({"text", std::string(prefix) + "type", std::string(type)});
  envelope.operands.push_back({"text", std::string(prefix) + "value", std::string(value)});
  envelope.operands.push_back({"text", std::string(prefix) + "is_null", is_null ? "true" : "false"});
}

void AppendLikeProjectionExpression(sblr::SblrOperationEnvelope& envelope,
                                    std::string_view prefix,
                                    FunctionProjectionArg left,
                                    FunctionProjectionArg right,
                                    std::string_view operator_id = "op_like",
                                    std::string_view canonical_operator_id = "sb.operator.like") {
  envelope.operands.push_back({"text", std::string(prefix) + "expr_kind", "operator"});
  envelope.operands.push_back({"text", std::string(prefix) + "expr_opcode", "SBLR_OPERATOR_CALL"});
  envelope.operands.push_back({"text", std::string(prefix) + "type", "boolean"});
  envelope.operands.push_back({"text", std::string(prefix) + "value", ""});
  envelope.operands.push_back({"text", std::string(prefix) + "is_null", "false"});
  envelope.operands.push_back({"text", std::string(prefix) + "operator_id", std::string(operator_id)});
  envelope.operands.push_back({"text", std::string(prefix) + "canonical_operator_id", std::string(canonical_operator_id)});
  envelope.operands.push_back({"text", std::string(prefix) + "operator_arg_count", "2"});
  AppendLiteralProjectionExpression(envelope, std::string(prefix) + "arg_0_",
                                    left.type, left.value, left.is_null);
  AppendLiteralProjectionExpression(envelope, std::string(prefix) + "arg_1_",
                                    right.type, right.value, right.is_null);
}

void AppendNotProjectionExpression(sblr::SblrOperationEnvelope& envelope,
                                   std::string_view prefix,
                                   std::string_view child_prefix,
                                   bool child_is_like = true) {
  envelope.operands.push_back({"text", std::string(prefix) + "expr_kind", "operator"});
  envelope.operands.push_back({"text", std::string(prefix) + "expr_opcode", "SBLR_OPERATOR_CALL"});
  envelope.operands.push_back({"text", std::string(prefix) + "type", "boolean"});
  envelope.operands.push_back({"text", std::string(prefix) + "value", ""});
  envelope.operands.push_back({"text", std::string(prefix) + "is_null", "false"});
  envelope.operands.push_back({"text", std::string(prefix) + "operator_id", "op_not"});
  envelope.operands.push_back({"text", std::string(prefix) + "canonical_operator_id", "sb.operator.not"});
  envelope.operands.push_back({"text", std::string(prefix) + "operator_arg_count", "1"});
  if (child_is_like) {
    AppendLikeProjectionExpression(envelope, std::string(prefix) + "arg_0_",
                                   {"text", "sparrow"}, {"text", "duck%"});
  } else {
    AppendLiteralProjectionExpression(envelope, std::string(prefix) + "arg_0_", "boolean", child_prefix);
  }
}

void AppendOperatorProjectionExpression(sblr::SblrOperationEnvelope& envelope,
                                        std::string_view prefix,
                                        std::string_view result_type,
                                        std::string_view operator_id,
                                        std::string_view canonical_operator_id,
                                        std::vector<FunctionProjectionArg> arguments) {
  envelope.operands.push_back({"text", std::string(prefix) + "expr_kind", "operator"});
  envelope.operands.push_back({"text", std::string(prefix) + "expr_opcode", "SBLR_OPERATOR_CALL"});
  envelope.operands.push_back({"text", std::string(prefix) + "type", std::string(result_type)});
  envelope.operands.push_back({"text", std::string(prefix) + "value", ""});
  envelope.operands.push_back({"text", std::string(prefix) + "is_null", "false"});
  envelope.operands.push_back({"text", std::string(prefix) + "operator_id", std::string(operator_id)});
  envelope.operands.push_back({"text", std::string(prefix) + "canonical_operator_id", std::string(canonical_operator_id)});
  envelope.operands.push_back({"text", std::string(prefix) + "operator_arg_count", std::to_string(arguments.size())});
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    AppendLiteralProjectionExpression(envelope,
                                      std::string(prefix) + "arg_" + std::to_string(index) + "_",
                                      arguments[index].type,
                                      arguments[index].value,
                                      arguments[index].is_null);
  }
}

sblr::SblrOperationEnvelope FunctionProjectionEngineEnvelope(std::string arg_type = "real64",
                                                             std::string arg_value = "0.7853981633974483") {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.function");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "cot_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_0_type", "real64"});
  envelope.operands.push_back({"text", "projection_0_value", ""});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_function_id", "sb.scalar.cot"});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", std::move(arg_type)});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", std::move(arg_value)});
  envelope.operands.push_back({"text", "projection_0_arg_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope MultiArgumentFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.multi_argument_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "25"});
  AppendFunctionProjectionOperand(envelope, 0, "joined", "character", "sb.scalar.concat",
                                  {{"text", "alpha"}, {"text", "-"}, {"text", "beta"}});
  AppendFunctionProjectionOperand(envelope, 1, "replaced", "character", "sb.scalar.replace",
                                  {{"text", "alpha beta beta"}, {"text", "beta"}, {"text", "B"}});
  AppendFunctionProjectionOperand(envelope, 2, "regex_ok", "boolean", "sb.regex.match",
                                  {{"text", "Alpha"}, {"text", "^a"}, {"text", "i"}});
  AppendFunctionProjectionOperand(envelope, 3, "year_part", "int64", "sb.temporal.date_part",
                                  {{"text", "year"}, {"text", "2026-05-11T14:23:45"}});
  AppendFunctionProjectionOperand(envelope, 4, "day_trunc", "timestamp", "sb.temporal.date_trunc",
                                  {{"text", "day"}, {"text", "2026-05-11T14:23:45"}});
  AppendFunctionProjectionOperand(envelope, 5, "json_len", "uint64", "sb.json.array_length",
                                  {{"text", "[1,2,3]"}});
  AppendFunctionProjectionOperand(envelope, 6, "sound_code", "character", "sb.scalar.soundex",
                                  {{"text", "Robert"}});
  AppendFunctionProjectionOperand(envelope, 7, "edit_distance", "int64", "sb.scalar.levenshtein",
                                  {{"text", "kitten"}, {"text", "sitting"}});
  AppendFunctionProjectionOperand(envelope, 8, "cardinality_value", "int64", "sb.scalar.cardinality",
                                  {{"array", "[1,2,3]"}});
  AppendFunctionProjectionOperand(envelope, 9, "stuff_value", "character", "sb.scalar.stuff",
                                  {{"character", "abcdef"},
                                   {"int64", "2"},
                                   {"int64", "3"},
                                   {"character", "XY"}});
  AppendFunctionProjectionOperand(envelope, 10, "regex_count", "int64", "sb.scalar.regexp_count",
                                  {{"text", "abcabc"}, {"text", "a."}});
  AppendFunctionProjectionOperand(envelope, 11, "regex_capture", "array", "sb.scalar.regexp_match",
                                  {{"text", "abc-123"}, {"text", "([a-z]+)-([0-9]+)"}});
  AppendFunctionProjectionOperand(envelope, 12, "regex_replaced", "character", "sb.scalar.regexp_replace",
                                  {{"text", "abc123abc"}, {"text", "abc"}, {"text", "X"}, {"text", "g"}});
  AppendFunctionProjectionOperand(envelope, 13, "regex_parts", "array", "sb.scalar.regexp_split_to_array",
                                  {{"text", "a,b;c"}, {"text", "[,;]"}});
  AppendFunctionProjectionOperand(envelope, 14, "regex_substr", "character", "sb.scalar.regexp_substr",
                                  {{"text", "abc-123-def"}, {"text", "[0-9]+"}});
  AppendFunctionProjectionOperand(envelope, 15, "occurrences_regex_count", "int64", "sb.scalar.occurrences_regex",
                                  {{"text", "a."}, {"text", "abcabc"}});
  AppendFunctionProjectionOperand(envelope, 16, "position_regex_after", "int64", "sb.scalar.position_regex",
                                  {{"text", "a"},
                                   {"text", "A-a"},
                                   {"int64", "2"},
                                   {"text", "i"},
                                   {"text", "after"}});
  AppendFunctionProjectionOperand(envelope, 17, "substring_regex_group", "character", "sb.scalar.substring_regex",
                                  {{"text", "([a-z])([0-9]+)"},
                                   {"text", "a1 b22 c333"},
                                   {"int64", "2"},
                                   {"int64", "2"},
                                   {"text", ""}});
  AppendFunctionProjectionOperand(envelope, 18, "translate_regex_all", "character", "sb.scalar.translate_regex",
                                  {{"text", "abc"},
                                   {"text", "abc123abc"},
                                   {"text", "X"},
                                   {"text", "all"},
                                   {"text", ""}});
  AppendFunctionProjectionOperand(envelope, 19, "regex_matches", "array", "sb.scalar.regexp_matches",
                                  {{"text", "abc-123"}, {"text", "([a-z]+)-([0-9]+)"}});
  AppendFunctionProjectionOperand(envelope, 20, "regex_split_rows", "array", "sb.scalar.regexp_split_to_table",
                                  {{"text", "a,b;c"}, {"text", "[,;]"}});
  AppendFunctionProjectionOperand(envelope, 21, "converted", "character", "sb.scalar.convert",
                                  {{"text", "deja vu"}, {"text", "UTF8"}});
  AppendFunctionProjectionOperand(envelope, 22, "array_joined", "character", "sb.scalar.array_to_string",
                                  {{"text", "[\"a\",null,\"c\"]"}, {"text", "|"}});
  AppendFunctionProjectionOperand(envelope, 23, "ascii_text", "character", "sb.scalar.to_ascii",
                                  {{"text", "d\xc3\xa9j\xc3\xa0 vu"}});
  AppendFunctionProjectionOperand(envelope, 24, "formatted", "character", "sb.scalar.format",
                                  {{"text", "SELECT %I = %L %%"}, {"text", "col name"}, {"text", "O'Reilly"}});
  return envelope;
}

sblr::SblrOperationEnvelope ConcatExpressionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.concat_expr");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope, 0, "joined", "character", "sb.scalar.concat",
                                  {{"text", "alpha"}, {"text", "-"}, {"text", "beta"}});
  return envelope;
}

sblr::SblrOperationEnvelope OperatorProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.operators");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "7"});
  envelope.operands.push_back({"text", "projection_0_name", "like_true"});
  AppendLikeProjectionExpression(envelope, "projection_0_",
                                 {"text", "ScratchBird"}, {"text", "Scratch%"});
  envelope.operands.push_back({"text", "projection_1_name", "like_false"});
  AppendLikeProjectionExpression(envelope, "projection_1_",
                                 {"text", "sparrow"}, {"text", "duck%"});
  envelope.operands.push_back({"text", "projection_2_name", "like_unknown"});
  AppendLikeProjectionExpression(envelope, "projection_2_",
                                 {"text", "", true}, {"text", "bird%"});
  envelope.operands.push_back({"text", "projection_3_name", "not_like_true"});
  AppendNotProjectionExpression(envelope, "projection_3_", "");
  envelope.operands.push_back({"text", "projection_4_name", "not_true"});
  AppendNotProjectionExpression(envelope, "projection_4_", "true", false);
  envelope.operands.push_back({"text", "projection_5_name", "ilike_true"});
  AppendLikeProjectionExpression(envelope, "projection_5_",
                                 {"text", "ScratchBird"},
                                 {"text", "scratch%"},
                                 "op_ilike",
                                 "sb.operator.ilike");
  envelope.operands.push_back({"text", "projection_6_name", "like_escape_true"});
  AppendLikeProjectionExpression(envelope, "projection_6_",
                                 {"text", "a_%"},
                                 {"text", "a\\_%"});
  return envelope;
}

sblr::SblrOperationEnvelope InvalidLikeOperatorProjectionEngineEnvelope(FunctionProjectionArg left,
                                                                       FunctionProjectionArg right) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.operator_refusal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "like_refusal"});
  AppendLikeProjectionExpression(envelope, "projection_0_", std::move(left), std::move(right));
  return envelope;
}

sblr::SblrOperationEnvelope ExtendedOperatorProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.extended_operators");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "9"});
  envelope.operands.push_back({"text", "projection_0_name", "and_false"});
  AppendOperatorProjectionExpression(envelope, "projection_0_", "boolean",
                                     "op_and", "sb.operator.and",
                                     {{"boolean", "true"}, {"boolean", "false"}});
  envelope.operands.push_back({"text", "projection_1_name", "or_true"});
  AppendOperatorProjectionExpression(envelope, "projection_1_", "boolean",
                                     "op_or", "sb.operator.or",
                                     {{"boolean", "false"}, {"boolean", "true"}});
  envelope.operands.push_back({"text", "projection_2_name", "negated"});
  AppendOperatorProjectionExpression(envelope, "projection_2_", "int64",
                                     "op_unary_minus", "sb.operator.unary_minus",
                                     {{"bigint", "7"}});
  envelope.operands.push_back({"text", "projection_3_name", "distinct_null"});
  AppendOperatorProjectionExpression(envelope, "projection_3_", "boolean",
                                     "op_is_distinct", "sb.operator.is_distinct_from",
                                     {{"bigint", "1"}, {"null", "", true}});
  envelope.operands.push_back({"text", "projection_4_name", "regex_true"});
  AppendOperatorProjectionExpression(envelope, "projection_4_", "boolean",
                                     "op_regex_match", "sb.operator.regex_match",
                                     {{"text", "Alpha"}, {"text", "^A"}});
  envelope.operands.push_back({"text", "projection_5_name", "json_value"});
  AppendOperatorProjectionExpression(envelope, "projection_5_", "json_document",
                                     "op_json_get", "sb.operator.json_get",
                                     {{"json_document", "{\"a\":1}"}, {"text", "$.a"}});
  envelope.operands.push_back({"text", "projection_6_name", "json_text"});
  AppendOperatorProjectionExpression(envelope, "projection_6_", "text",
                                     "op_json_get_text", "sb.operator.json_get_text",
                                     {{"json_document", "{\"a\":\"bird\"}"}, {"text", "$.a"}});
  envelope.operands.push_back({"text", "projection_7_name", "array_contains"});
  AppendOperatorProjectionExpression(envelope, "projection_7_", "boolean",
                                     "op_array_contains", "sb.operator.array_contains",
                                     {{"array", "[1,2]"}, {"bigint", "2"}});
  envelope.operands.push_back({"text", "projection_8_name", "xor_true"});
  AppendOperatorProjectionExpression(envelope, "projection_8_", "boolean",
                                     "op_xor", "sb.operator.xor",
                                     {{"boolean", "true"}, {"boolean", "false"}});
  return envelope;
}

sblr::SblrOperationEnvelope NumericFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.numeric_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "11"});
  AppendFunctionProjectionOperand(envelope, 0, "pi_value", "real64", "sb.scalar.pi", {});
  AppendFunctionProjectionOperand(envelope, 1, "cbrt_value", "real64", "sb.scalar.cbrt",
                                  {{"bigint", "27"}});
  AppendFunctionProjectionOperand(envelope, 2, "deg_value", "real64", "sb.scalar.degrees",
                                  {{"real64", "3.141592653589793"}});
  AppendFunctionProjectionOperand(envelope, 3, "rad_value", "real64", "sb.scalar.radians",
                                  {{"bigint", "180"}});
  AppendFunctionProjectionOperand(envelope, 4, "cotd_value", "real64", "sb.scalar.cotd",
                                  {{"bigint", "45"}});
  AppendFunctionProjectionOperand(envelope, 5, "div_value", "int64", "sb.scalar.div",
                                  {{"bigint", "7"}, {"bigint", "3"}});
  AppendFunctionProjectionOperand(envelope, 6, "fact_value", "int64", "sb.scalar.factorial",
                                  {{"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 7, "gcd_value", "int64", "sb.scalar.gcd",
                                  {{"bigint", "48"}, {"bigint", "18"}});
  AppendFunctionProjectionOperand(envelope, 8, "lcm_value", "int64", "sb.scalar.lcm",
                                  {{"bigint", "6"}, {"bigint", "8"}});
  AppendFunctionProjectionOperand(envelope, 9, "bucket_value", "int64", "sb.scalar.width_bucket",
                                  {{"bigint", "5"}, {"bigint", "0"}, {"bigint", "10"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 10, "bit_count_value", "int64", "sb.scalar.bit_count",
                                  {{"bigint", "7"}});
  return envelope;
}

sblr::SblrOperationEnvelope AdditionalNumericFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.additional_numeric_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "22"});
  AppendFunctionProjectionOperand(envelope, 0, "abs_value", "int64", "sb.scalar.abs",
                                  {{"int64", "-5"}});
  AppendFunctionProjectionOperand(envelope, 1, "ceil_value", "real64", "sb.scalar.ceil",
                                  {{"real64", "1.4"}});
  AppendFunctionProjectionOperand(envelope, 2, "floor_value", "real64", "sb.scalar.floor",
                                  {{"real64", "1.6"}});
  AppendFunctionProjectionOperand(envelope, 3, "round_value", "real64", "sb.scalar.round",
                                  {{"real64", "1.5"}});
  AppendFunctionProjectionOperand(envelope, 4, "sqrt_value", "real64", "sb.scalar.sqrt",
                                  {{"real64", "4.0"}});
  AppendFunctionProjectionOperand(envelope, 5, "power_value", "real64", "sb.scalar.power",
                                  {{"real64", "2.0"}, {"real64", "10.0"}});
  AppendFunctionProjectionOperand(envelope, 6, "sin_value", "real64", "sb.scalar.sin",
                                  {{"real64", "0.0"}});
  AppendFunctionProjectionOperand(envelope, 7, "cos_value", "real64", "sb.scalar.cos",
                                  {{"real64", "0.0"}});
  AppendFunctionProjectionOperand(envelope, 8, "tan_value", "real64", "sb.scalar.tan",
                                  {{"real64", "0.0"}});
  AppendFunctionProjectionOperand(envelope, 9, "exp_value", "real64", "sb.scalar.exp",
                                  {{"real64", "0.0"}});
  AppendFunctionProjectionOperand(envelope, 10, "ln_value", "real64", "sb.scalar.ln",
                                  {{"real64", "1.0"}});
  AppendFunctionProjectionOperand(envelope, 11, "log10_value", "real64", "sb.scalar.log10",
                                  {{"real64", "100.0"}});
  AppendFunctionProjectionOperand(envelope, 12, "log_value", "real64", "sb.scalar.log",
                                  {{"real64", "2.0"}, {"real64", "8.0"}});
  AppendFunctionProjectionOperand(envelope, 13, "trunc_value", "real64", "sb.scalar.trunc",
                                  {{"real64", "1.7"}});
  AppendFunctionProjectionOperand(envelope, 14, "truncate_value", "real64", "sb.scalar.truncate",
                                  {{"real64", "1.7"}});
  AppendFunctionProjectionOperand(envelope, 15, "mod_value", "real64", "sb.scalar.mod",
                                  {{"real64", "10.0"}, {"real64", "3.0"}});
  AppendFunctionProjectionOperand(envelope, 16, "sign_value", "int64", "sb.scalar.sign",
                                  {{"real64", "5.0"}});
  AppendFunctionProjectionOperand(envelope, 17, "bit_and_value", "int64", "sb.scalar.bit_and",
                                  {{"int64", "240"}, {"int64", "15"}});
  AppendFunctionProjectionOperand(envelope, 18, "bit_or_value", "int64", "sb.scalar.bit_or",
                                  {{"int64", "240"}, {"int64", "15"}});
  AppendFunctionProjectionOperand(envelope, 19, "bit_xor_value", "int64", "sb.scalar.bit_xor",
                                  {{"int64", "42"}, {"int64", "42"}});
  AppendFunctionProjectionOperand(envelope, 20, "bit_shift_left_value", "int64",
                                  "sb.scalar.bit_shift_left", {{"int64", "1"}, {"int64", "8"}});
  AppendFunctionProjectionOperand(envelope, 21, "bit_shift_right_value", "int64",
                                  "sb.scalar.bit_shift_right", {{"int64", "256"}, {"int64", "8"}});
  return envelope;
}

sblr::SblrOperationEnvelope ExtendedNumericFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.extended_numeric_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "17"});
  AppendFunctionProjectionOperand(envelope, 0, "asin_value", "real64", "sb.scalar.asin",
                                  {{"real64", "0.5"}});
  AppendFunctionProjectionOperand(envelope, 1, "acos_value", "real64", "sb.scalar.acos",
                                  {{"real64", "0.5"}});
  AppendFunctionProjectionOperand(envelope, 2, "atan_value", "real64", "sb.scalar.atan",
                                  {{"real64", "1.0"}});
  AppendFunctionProjectionOperand(envelope, 3, "sinh_value", "real64", "sb.scalar.sinh",
                                  {{"real64", "0.0"}});
  AppendFunctionProjectionOperand(envelope, 4, "cosh_value", "real64", "sb.scalar.cosh",
                                  {{"real64", "0.0"}});
  AppendFunctionProjectionOperand(envelope, 5, "tanh_value", "real64", "sb.scalar.tanh",
                                  {{"real64", "0.0"}});
  AppendFunctionProjectionOperand(envelope, 6, "asinh_value", "real64", "sb.scalar.asinh",
                                  {{"real64", "1.0"}});
  AppendFunctionProjectionOperand(envelope, 7, "acosh_value", "real64", "sb.scalar.acosh",
                                  {{"real64", "1.0"}});
  AppendFunctionProjectionOperand(envelope, 8, "atanh_value", "real64", "sb.scalar.atanh",
                                  {{"real64", "0.5"}});
  AppendFunctionProjectionOperand(envelope, 9, "log2_value", "real64", "sb.scalar.log2",
                                  {{"real64", "8.0"}});
  AppendFunctionProjectionOperand(envelope, 10, "atan2_value", "real64", "sb.scalar.atan2",
                                  {{"real64", "1.0"}, {"real64", "1.0"}});
  AppendFunctionProjectionOperand(envelope, 11, "sind_value", "real64", "sb.scalar.sind",
                                  {{"real64", "30.0"}});
  AppendFunctionProjectionOperand(envelope, 12, "cosd_value", "real64", "sb.scalar.cosd",
                                  {{"real64", "60.0"}});
  AppendFunctionProjectionOperand(envelope, 13, "tand_value", "real64", "sb.scalar.tand",
                                  {{"real64", "45.0"}});
  AppendFunctionProjectionOperand(envelope, 14, "asind_value", "real64", "sb.scalar.asind",
                                  {{"real64", "0.5"}});
  AppendFunctionProjectionOperand(envelope, 15, "acosd_value", "real64", "sb.scalar.acosd",
                                  {{"real64", "0.5"}});
  AppendFunctionProjectionOperand(envelope, 16, "atand_value", "real64", "sb.scalar.atand",
                                  {{"real64", "1.0"}});
  return envelope;
}

sblr::SblrOperationEnvelope TextJsonFuzzyFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.text_json_fuzzy_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "33"});
  AppendFunctionProjectionOperand(envelope, 0, "char_len", "int64", "sb.scalar.char_length",
                                  {{"text", "surface"}});
  AppendFunctionProjectionOperand(envelope, 1, "left_value", "character", "sb.scalar.left",
                                  {{"text", "abcdef"}, {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 2, "right_value", "character", "sb.scalar.right",
                                  {{"text", "abcdef"}, {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 3, "uuid_value", "uuid", "sb.scalar.uuid_from_string",
                                  {{"text", "550E8400-E29B-41D4-A716-446655440000"}});
  AppendFunctionProjectionOperand(envelope, 4, "uuid_text", "character", "sb.scalar.uuid_to_string",
                                  {{"uuid", "550E8400-E29B-41D4-A716-446655440000"}});
  AppendFunctionProjectionOperand(envelope, 5, "digest_value", "uint64", "sb.scalar.digest",
                                  {{"text", "hello"}, {"text", "fnv64"}});
  AppendFunctionProjectionOperand(envelope, 6, "jsonb_len", "uint64", "sb.json.jsonb_array_length",
                                  {{"text", "[1,2,3]"}});
  AppendFunctionProjectionOperand(envelope, 7, "json_set_value", "json_document", "sb.json.set",
                                  {{"text", R"({"a":1})"}, {"text", "$.b"}, {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 8, "json_removed", "json_document", "sb.json.remove",
                                  {{"text", R"({"a":1,"b":2})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 9, "json_replaced", "json_document", "sb.json.replace",
                                  {{"text", R"({"a":1})"}, {"text", "$.a"}, {"int64", "4"}});
  AppendFunctionProjectionOperand(envelope, 10, "json_inserted", "json_document", "sb.json.insert",
                                  {{"text", R"({"a":1})"}, {"text", "$.b"}, {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 11, "json_array", "json_document", "sb.json.build_array",
                                  {{"int64", "1"}, {"text", "bird"}, {"null", "", true}});
  AppendFunctionProjectionOperand(envelope, 12, "json_object", "json_document", "sb.json.build_object",
                                  {{"text", "a"}, {"int64", "1"}, {"text", "b"}, {"text", "bird"}});
  AppendFunctionProjectionOperand(envelope, 13, "json_text", "json_document", "sb.json.to_json",
                                  {{"text", "bird"}});
  AppendFunctionProjectionOperand(envelope, 14, "jsonb_text", "json_document", "sb.json.to_jsonb",
                                  {{"int64", "7"}});
  AppendFunctionProjectionOperand(envelope, 15, "jsonb_type", "character", "sb.json.jsonb_typeof",
                                  {{"text", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 16, "json_type", "character", "sb.json.typeof",
                                  {{"text", "[1,2]"}});
  AppendFunctionProjectionOperand(envelope, 17, "json_extracted", "json_document", "sb.json.extract",
                                  {{"text", R"({"a":42})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 18, "json_exists_value", "boolean", "sb.json.exists",
                                  {{"text", R"({"a":1})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 19, "json_value_value", "json_document", "sb.json.value",
                                  {{"text", R"({"a":"bird"})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 20, "json_query_value", "json_document", "sb.json.query",
                                  {{"text", R"({"a":[1,2]})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 21, "jsonb_set_value", "json_document", "sb.json.jsonb_set",
                                  {{"text", R"({"a":1})"}, {"text", "$.a"}, {"int64", "5"}});
  AppendFunctionProjectionOperand(envelope, 22, "jsonb_array", "json_document",
                                  "sb.json.jsonb_build_array",
                                  {{"int64", "1"}, {"text", "bird"}, {"null", "", true}});
  AppendFunctionProjectionOperand(envelope, 23, "jsonb_object", "json_document",
                                  "sb.json.jsonb_build_object",
                                  {{"text", "a"}, {"int64", "1"}});
  AppendFunctionProjectionOperand(envelope, 24, "metaphone_value", "character", "sb.scalar.metaphone",
                                  {{"text", "Smith"}});
  AppendFunctionProjectionOperand(envelope, 25, "dmetaphone_value", "character", "sb.scalar.dmetaphone",
                                  {{"text", "gumbo"}});
  AppendFunctionProjectionOperand(envelope, 26, "dmetaphone_alt_value", "character",
                                  "sb.scalar.dmetaphone_alt", {{"text", "Smith"}});
  AppendFunctionProjectionOperand(envelope, 27, "levenshtein_le_value", "int64",
                                  "sb.scalar.levenshtein_le",
                                  {{"text", "kitten"}, {"text", "sitten"}, {"int64", "1"}});
  AppendFunctionProjectionOperand(envelope, 28, "damerau_value", "int64",
                                  "sb.scalar.damerau_levenshtein",
                                  {{"text", "CA"}, {"text", "AC"}});
  AppendFunctionProjectionOperand(envelope, 29, "jaro_value", "real64", "sb.scalar.jaro_similarity",
                                  {{"text", "MARTHA"}, {"text", "MARHTA"}});
  AppendFunctionProjectionOperand(envelope, 30, "jaro_winkler_value", "real64",
                                  "sb.scalar.jaro_winkler_similarity",
                                  {{"text", "MARTHA"}, {"text", "MARHTA"}});
  AppendFunctionProjectionOperand(envelope, 31, "similarity_value", "real64", "sb.scalar.similarity",
                                  {{"text", "hello"}, {"text", "hello"}});
  AppendFunctionProjectionOperand(envelope, 32, "word_similarity_value", "real64",
                                  "sb.scalar.word_similarity",
                                  {{"text", "hello"}, {"text", "say hello today"}});
  return envelope;
}

sblr::SblrOperationEnvelope Sbsfc013DocumentCollectionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.sbsfc013_document_collection");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "65"});
  AppendFunctionProjectionOperand(envelope, 0, "SBSQL-FE519C1C20F6", "character",
                                  "sb.json.typeof", {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 1, "SBSQL-4324775DBCA0", "character",
                                  "sb.json.typeof", {{"json_document", "[1,2]"}});
  AppendFunctionProjectionOperand(envelope, 2, "SBSQL-83995B2BC266", "json_document",
                                  "sb.json.extract",
                                  {{"json_document", R"({"a":42})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 3, "SBSQL-35F0E9FF7755", "json_document",
                                  "sb.json.extract",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.missing"}});
  AppendFunctionProjectionOperand(envelope, 4, "SBSQL-7EE8FAD14C5A", "boolean",
                                  "sb.json.exists",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 5, "SBSQL-3E3BA120C541", "boolean",
                                  "sb.json.exists",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.missing"}});
  AppendFunctionProjectionOperand(envelope, 6, "SBSQL-1342A8B02022", "boolean",
                                  "sb.json.exists",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 7, "SBSQL-5E705E2E1462", "json_document",
                                  "sb.json.value",
                                  {{"json_document", R"({"a":7})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 8, "SBSQL-5B765753ADEC", "json_document",
                                  "sb.json.value",
                                  {{"json_document", R"({"a":"bird"})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 9, "SBSQL-68134CF09B70", "json_document",
                                  "sb.json.value",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.missing"}});
  AppendFunctionProjectionOperand(envelope, 10, "SBSQL-09BA0A3A71DB", "json_document",
                                  "sb.json.query",
                                  {{"json_document", R"({"a":{"b":2}})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 11, "SBSQL-DC8507F9B9C5", "json_document",
                                  "sb.json.query",
                                  {{"json_document", R"({"a":[1,2]})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 12, "SBSQL-03D2E8D0B9AE", "json_document",
                                  "sb.json.set",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.b"},
                                   {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 13, "SBSQL-7AA44FB9077C", "json_document",
                                  "sb.json.set",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"},
                                   {"int64", "3"}});
  AppendFunctionProjectionOperand(envelope, 14, "SBSQL-E062D64D0C8F", "json_document",
                                  "sb.json.remove",
                                  {{"json_document", R"({"a":1,"b":2})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 15, "SBSQL-B4CF3C70B1D3", "json_document",
                                  "sb.json.remove",
                                  {{"json_document", R"({"a":1,"b":2})"},
                                   {"text", "$.missing"}});
  AppendFunctionProjectionOperand(envelope, 16, "SBSQL-14C23DAA9C77", "json_document",
                                  "sb.json.replace",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"},
                                   {"int64", "4"}});
  AppendFunctionProjectionOperand(envelope, 17, "SBSQL-23EEDCBB8140", "json_document",
                                  "sb.json.replace",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.missing"},
                                   {"int64", "4"}});
  AppendFunctionProjectionOperand(envelope, 18, "SBSQL-AF045C026980", "json_document",
                                  "sb.json.insert",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.b"},
                                   {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 19, "SBSQL-2E230404921F", "json_document",
                                  "sb.json.insert",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"},
                                   {"int64", "9"}});
  AppendFunctionProjectionOperand(envelope, 20, "SBSQL-C55F7ADBD13B", "json_document",
                                  "sb.json.jsonb_set",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"},
                                   {"int64", "5"}});
  AppendFunctionProjectionOperand(envelope, 21, "SBSQL-723E5CADA519", "json_document",
                                  "sb.json.jsonb_set",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.missing"},
                                   {"int64", "5"}, {"int64", "0"}});
  AppendFunctionProjectionOperand(envelope, 22, "SBSQL-0A060E6427B3", "uint64",
                                  "sb.json.array_length",
                                  {{"json_document", R"([1,2,{"a":3}])"}});
  AppendFunctionProjectionOperand(envelope, 23, "SBSQL-DEDF07FAB7F3", "uint64",
                                  "sb.json.array_length", {{"json_document", "[]"}});
  AppendFunctionProjectionOperand(envelope, 24, "SBSQL-5BCF9869AA4C", "uint64",
                                  "sb.json.jsonb_array_length",
                                  {{"json_document", "[1,2,3]"}});
  AppendFunctionProjectionOperand(envelope, 25, "SBSQL-3CEB816A1165", "uint64",
                                  "sb.json.jsonb_array_length", {{"json_document", "[]"}});
  AppendFunctionProjectionOperand(envelope, 26, "SBSQL-7B99FF977C66", "json_document",
                                  "sb.json.build_array",
                                  {{"int64", "1"}, {"text", "bird"}, {"null", "", true}});
  AppendFunctionProjectionOperand(envelope, 27, "SBSQL-4640811DBAC8", "json_document",
                                  "sb.json.build_array", {});
  AppendFunctionProjectionOperand(envelope, 28, "SBSQL-E2DFF93CA59C", "json_document",
                                  "sb.json.build_object",
                                  {{"text", "a"}, {"int64", "1"}, {"text", "b"},
                                   {"text", "bird"}});
  AppendFunctionProjectionOperand(envelope, 29, "SBSQL-36FBFED38C80", "json_document",
                                  "sb.json.jsonb_build_array",
                                  {{"int64", "1"}, {"text", "bird"}, {"null", "", true}});
  AppendFunctionProjectionOperand(envelope, 30, "SBSQL-34E68EB56EDC", "json_document",
                                  "sb.json.jsonb_build_object",
                                  {{"text", "a"}, {"int64", "1"}});
  AppendFunctionProjectionOperand(envelope, 31, "SBSQL-CB837AAEBEAD", "json_document",
                                  "sb.json.to_json", {{"text", "bird"}});
  AppendFunctionProjectionOperand(envelope, 32, "SBSQL-F0AB18F7417B", "json_document",
                                  "sb.json.to_json", {{"null", "", true}});
  AppendFunctionProjectionOperand(envelope, 33, "SBSQL-4119D041403C", "json_document",
                                  "sb.json.to_jsonb",
                                  {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 34, "SBSQL-88E66066EBC7", "json_document",
                                  "sb.json.to_jsonb", {{"int64", "7"}});
  AppendFunctionProjectionOperand(envelope, 35, "SBSQL-048498BB9A7F", "character",
                                  "sb.json.jsonb_typeof",
                                  {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 36, "SBSQL-2F78C18D9292", "character",
                                  "sb.json.jsonb_typeof", {{"json_document", "[1,2]"}});
  AppendFunctionProjectionOperand(envelope, 37, "SBSQL-36FF2B0254C0", "character",
                                  "sb.json.typeof", {{"json_document", "true"}});
  AppendFunctionProjectionOperand(envelope, 38, "SBSQL-0926D8F4ABD5", "json_document",
                                  "sb.json.object", {{"text", "a"}, {"int64", "1"}});
  AppendFunctionProjectionOperand(envelope, 39, "SBSQL-551339ECEE75", "json_document",
                                  "sb.json.jsonb_object", {{"text", "a"}, {"int64", "1"}});
  AppendFunctionProjectionOperand(envelope, 40, "SBSQL-90E1BC86D62F", "json_document",
                                  "sb.json.object_keys",
                                  {{"json_document", R"({"a":1,"b":2})"}});
  AppendFunctionProjectionOperand(envelope, 41, "SBSQL-A05313B740CC", "json_document",
                                  "sb.json.object_keys", {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 42, "SBSQL-03447BA4EB25", "json_document",
                                  "sb.json.jsonb_object_keys",
                                  {{"json_document", R"({"a":1,"b":2})"}});
  AppendFunctionProjectionOperand(envelope, 43, "SBSQL-EB982B4F95B3", "json_document",
                                  "sb.json.jsonb_object_keys",
                                  {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 44, "SBSQL-0390232C7296", "json_document",
                                  "sb.json.array_elements",
                                  {{"json_document", "[1,2]"}});
  AppendFunctionProjectionOperand(envelope, 45, "SBSQL-7819B29C7AB5", "json_document",
                                  "sb.json.array_elements",
                                  {{"json_document", "[1,2]"}});
  AppendFunctionProjectionOperand(envelope, 46, "SBSQL-4D97B9EA482B", "json_document",
                                  "sb.json.array_elements_text",
                                  {{"json_document", "[1,2]"}});
  AppendFunctionProjectionOperand(envelope, 47, "SBSQL-76883ECD3648", "json_document",
                                  "sb.json.each", {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 48, "SBSQL-18521C5D03B8", "json_document",
                                  "sb.json.each", {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 49, "SBSQL-4B7CDEB23364", "json_document",
                                  "sb.json.each_text", {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 50, "SBSQL-6AF2FB9EDEB9", "json_document",
                                  "sb.json.each_text", {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 51, "SBSQL-CE5BD771D075", "json_document",
                                  "sb.json.jsonb_insert",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.b"},
                                   {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 52, "SBSQL-429DB32D5CC2", "json_document",
                                  "sb.json.jsonb_insert",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"},
                                   {"int64", "9"}, {"int64", "1"}});
  AppendFunctionProjectionOperand(envelope, 53, "SBSQL-58F6D7F43DA6", "boolean",
                                  "sb.json.jsonb_path_exists",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 54, "SBSQL-D4C29991D99B", "boolean",
                                  "sb.json.jsonb_path_exists",
                                  {{"json_document", R"({"a":1})"}, {"text", "$.missing"}});
  AppendFunctionProjectionOperand(envelope, 55, "SBSQL-9A4AB48B76FD", "boolean",
                                  "sb.json.jsonb_path_match",
                                  {{"json_document", R"({"ok":true})"}, {"text", "$.ok"}});
  AppendFunctionProjectionOperand(envelope, 56, "SBSQL-7C4821112F94", "boolean",
                                  "sb.json.jsonb_path_match",
                                  {{"json_document", R"({"ok":false})"}, {"text", "$.ok"}});
  AppendFunctionProjectionOperand(envelope, 57, "SBSQL-436880E1F3F7", "json_document",
                                  "sb.json.jsonb_path_query",
                                  {{"json_document", R"({"a":{"b":2}})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 58, "SBSQL-EA5E00825D4D", "json_document",
                                  "sb.json.jsonb_path_query",
                                  {{"json_document", R"({"a":7})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 59, "SBSQL-A1C65D80CE68", "json_document",
                                  "sb.json.jsonb_path_query_array",
                                  {{"json_document", R"({"a":7})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 60, "SBSQL-B64295F1B742", "json_document",
                                  "sb.json.jsonb_path_query_first",
                                  {{"json_document", R"({"a":"bird"})"}, {"text", "$.a"}});
  AppendFunctionProjectionOperand(envelope, 61, "SBSQL-6910DED90537", "character",
                                  "sb.json.jsonb_pretty",
                                  {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 62, "SBSQL-4CFCAC326BFB", "character",
                                  "sb.json.jsonb_pretty",
                                  {{"json_document", R"({"a":1})"}});
  AppendFunctionProjectionOperand(envelope, 63, "SBSQL-5157364BCB20", "json_document",
                                  "sb.json.jsonb_strip_nulls",
                                  {{"json_document", R"({"a":null,"b":2})"}});
  AppendFunctionProjectionOperand(envelope, 64, "SBSQL-98D9B54A7630", "json_document",
                                  "sb.json.jsonb_strip_nulls",
                                  {{"json_document", R"({"a":1,"b":null})"}});
  return envelope;
}

sblr::SblrOperationEnvelope Sbsfc013DocumentCollectionProjectionFailureEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.sbsfc013_document_collection_failure");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope, 0, "SBSQL-3217FFB2F3BD", "json_document",
                                  "sb.json.build_object",
                                  {{"text", "a"}, {"int64", "1"}, {"text", "b"}});
  return envelope;
}

sblr::SblrOperationEnvelope VectorFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.vector_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "14"});
  AppendFunctionProjectionOperand(envelope, 0, "vector_value", "dense_vector", "sb.vector.vector",
                                  {{"int64", "1"}, {"real64", "2.5"}, {"int64", "-3"}});
  AppendFunctionProjectionOperand(envelope, 1, "vector_dims_value", "int64",
                                  "sb.vector.vector_dims", {{"dense_vector", "[1, 2, 3]"}});
  AppendFunctionProjectionOperand(envelope, 2, "vector_norm_value", "real64",
                                  "sb.vector.vector_norm", {{"dense_vector", "[3,4]"}});
  AppendFunctionProjectionOperand(envelope, 3, "l2_distance_value", "real64",
                                  "sb.vector.l2_distance",
                                  {{"dense_vector", "[1,2]"}, {"dense_vector", "[4,6]"}});
  AppendFunctionProjectionOperand(envelope, 4, "cosine_distance_value", "real64",
                                  "sb.vector.cosine_distance",
                                  {{"dense_vector", "[1,0]"}, {"dense_vector", "[0,1]"}});
  AppendFunctionProjectionOperand(envelope, 5, "inner_product_value", "real64",
                                  "sb.vector.inner_product",
                                  {{"dense_vector", "[1,2,3]"}, {"dense_vector", "[4,5,6]"}});
  AppendFunctionProjectionOperand(envelope, 6, "negative_inner_product_value", "real64",
                                  "sb.vector.negative_inner_product",
                                  {{"dense_vector", "[1,2,3]"}, {"dense_vector", "[4,5,6]"}});
  AppendFunctionProjectionOperand(envelope, 7, "hamming_distance_value", "int64",
                                  "sb.vector.hamming_distance",
                                  {{"bit_vector", "10101"}, {"bit_vector", "10011"}});
  AppendFunctionProjectionOperand(envelope, 8, "normalized_vector", "dense_vector",
                                  "sb.vector.vector_l2_normalize", {{"dense_vector", "[3,4]"}});
  AppendFunctionProjectionOperand(envelope, 9, "subvector_value", "dense_vector",
                                  "sb.vector.subvector",
                                  {{"dense_vector", "[9,8,7,6]"}, {"int64", "2"}, {"int64", "2"}});
  AppendFunctionProjectionOperand(envelope, 10, "int8_vector_value", "int8_vector",
                                  "sb.vector.vector_cast_int8",
                                  {{"dense_vector", "[-129.2,-1.5,0.49,2.5,300]"}});
  AppendFunctionProjectionOperand(envelope, 11, "float16_vector_value", "float16_vector",
                                  "sb.vector.vector_cast_float16",
                                  {{"dense_vector", "[1,0.3333,65504]"}});
  AppendFunctionProjectionOperand(envelope, 12, "vector_sum_value", "dense_vector",
                                  "sb.vector.vector_sum",
                                  {{"dense_vector", "[1,2,3]"}, {"dense_vector", "[4,5,6]"}});
  AppendFunctionProjectionOperand(envelope, 13, "vector_avg_value", "dense_vector",
                                  "sb.vector.vector_avg",
                                  {{"dense_vector", "[1,2,3]"}, {"dense_vector", "[4,5,6]"}});
  return envelope;
}

sblr::SblrOperationEnvelope BinaryCryptoFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.binary_crypto_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "5"});
  AppendFunctionProjectionOperand(envelope, 0, "encoded_hex", "character",
                                  "sb.scalar.encode", {{"binary", "00ff10"}, {"text", "hex"}});
  AppendFunctionProjectionOperand(envelope, 1, "decoded_hex", "binary",
                                  "sb.scalar.decode", {{"text", "00ff10"}, {"text", "hex"}});
  AppendFunctionProjectionOperand(envelope, 2, "oracle_decode_value", "character",
                                  "sb.scalar.oracle_decode",
                                  {{"int64", "2"}, {"int64", "1"}, {"text", "one"},
                                   {"int64", "2"}, {"text", "two"}, {"text", "fallback"}});
  AppendFunctionProjectionOperand(envelope, 3, "random_value", "real64",
                                  "sb.scalar.random", {});
  AppendFunctionProjectionOperand(envelope, 4, "timezone_value", "character",
                                  "sb.scalar.current_setting_timezone", {});
  return envelope;
}

sblr::SblrOperationEnvelope CryptoDependencyRefusalEngineEnvelope(std::string_view function_id) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.crypto_dependency_refusal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope, 0, "crypto_digest", "character", function_id,
                                  {{"text", "hello"}});
  return envelope;
}

sblr::SblrOperationEnvelope TemporalSessionProviderProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.temporal_session_provider");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "16"});
  AppendFunctionProjectionOperand(envelope, 0, "stmt_ts", "timestamp_tz",
                                  "sb.temporal.statement_timestamp", {});
  AppendFunctionProjectionOperand(envelope, 1, "tx_ts", "timestamp_tz",
                                  "sb.temporal.transaction_timestamp", {});
  AppendFunctionProjectionOperand(envelope, 2, "clock_ts", "timestamp_tz",
                                  "sb.temporal.clock_timestamp", {});
  AppendFunctionProjectionOperand(envelope, 3, "timeofday_value", "character",
                                  "sb.temporal.timeofday", {});
  AppendFunctionProjectionOperand(envelope, 4, "localtime_value", "time",
                                  "sb.temporal.localtime", {});
  AppendFunctionProjectionOperand(envelope, 5, "localtimestamp_value", "timestamp",
                                  "sb.temporal.localtimestamp", {});
  AppendFunctionProjectionOperand(envelope, 6, "current_ts", "timestamp_tz",
                                  "sb.temporal.current_timestamp", {});
  AppendFunctionProjectionOperand(envelope, 7, "current_date_value", "date",
                                  "sb.temporal.current_date", {});
  AppendFunctionProjectionOperand(envelope, 8, "current_time_value", "time",
                                  "sb.temporal.current_time", {});
  AppendFunctionProjectionOperand(envelope, 9, "now_value", "timestamp_tz",
                                  "sb.temporal.now", {});
  AppendFunctionProjectionOperand(envelope, 10, "uuid1", "uuid", "sb.uuid.v1", {});
  AppendFunctionProjectionOperand(envelope, 11, "uuid4", "uuid", "sb.uuid.v4", {});
  AppendFunctionProjectionOperand(envelope, 12, "uuid7", "uuid", "sb.uuid.v7", {});
  AppendFunctionProjectionOperand(envelope, 13, "uuid1_canonical", "uuid", "sb.uuid.v1", {});
  AppendFunctionProjectionOperand(envelope, 14, "uuid4_canonical", "uuid", "sb.uuid.v4", {});
  AppendFunctionProjectionOperand(envelope, 15, "uuid7_canonical", "uuid", "sb.uuid.v7", {});
  return envelope;
}

sblr::SblrOperationEnvelope SpecialCurrentTimestampKeywordProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.evaluate_projection",
      "SBLR_QUERY_EVALUATE_PROJECTION",
      "trace.query.scalar_projection.special_current_timestamp_keyword");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope,
                                  0,
                                  "current_ts",
                                  "timestamp_tz",
                                  "sb.special.current_timestamp_keyword",
                                  {});
  return envelope;
}

sblr::SblrOperationEnvelope CurrentValueFormProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.evaluate_projection",
      "SBLR_QUERY_EVALUATE_PROJECTION",
      "trace.query.scalar_projection.current_value_form");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope,
                                  0,
                                  "current_ts",
                                  "timestamp_tz",
                                  "sb.temporal.current_timestamp",
                                  {});
  return envelope;
}

sblr::SblrOperationEnvelope TemporalConstructorProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.temporal_constructors");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "6"});
  AppendFunctionProjectionOperand(envelope, 0, "made_date", "date",
                                  "sb.temporal.make_date",
                                  {{"bigint", "2026"}, {"bigint", "5"}, {"bigint", "11"}});
  AppendFunctionProjectionOperand(envelope, 1, "made_time", "time",
                                  "sb.temporal.make_time",
                                  {{"bigint", "14"}, {"bigint", "23"}, {"bigint", "45"}});
  AppendFunctionProjectionOperand(envelope, 2, "made_timestamp", "timestamp",
                                  "sb.temporal.make_timestamp",
                                  {{"text", "2026-05-11"}, {"text", "14:23:45"}});
  AppendFunctionProjectionOperand(envelope, 3, "made_timestamp_6", "timestamp",
                                  "sb.temporal.make_timestamp",
                                  {{"bigint", "2026"}, {"bigint", "5"}, {"bigint", "11"},
                                   {"bigint", "14"}, {"bigint", "23"}, {"bigint", "45"}});
  AppendFunctionProjectionOperand(envelope, 4, "made_timestamptz", "timestamp_tz",
                                  "sb.temporal.make_timestamptz",
                                  {{"text", "2026-05-11"}, {"text", "14:23:45"}});
  AppendFunctionProjectionOperand(envelope, 5, "made_timestamptz_offset", "timestamp_tz",
                                  "sb.temporal.make_timestamptz",
                                  {{"text", "2026-05-11"}, {"text", "14:23:45"},
                                   {"text", "+05:30"}});
  return envelope;
}

sblr::SblrOperationEnvelope TemporalFieldArithmeticProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.temporal_field_arithmetic");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "9"});
  AppendFunctionProjectionOperand(envelope, 0, "dow_value", "int64", "sb.temporal.dow",
                                  {{"text", "2026-05-11"}});
  AppendFunctionProjectionOperand(envelope, 1, "doy_value", "int64", "sb.temporal.doy",
                                  {{"text", "2026-05-11"}});
  AppendFunctionProjectionOperand(envelope, 2, "quarter_value", "int64", "sb.temporal.quarter",
                                  {{"text", "2026-05-11"}});
  AppendFunctionProjectionOperand(envelope, 3, "isodow_value", "int64", "sb.temporal.isodow",
                                  {{"text", "2026-05-11"}});
  AppendFunctionProjectionOperand(envelope, 4, "week_value", "int64", "sb.temporal.week",
                                  {{"text", "2026-05-11"}});
  AppendFunctionProjectionOperand(envelope, 5, "add_months_value", "date",
                                  "sb.temporal.add_months",
                                  {{"text", "2026-05-11"}, {"bigint", "3"}});
  AppendFunctionProjectionOperand(envelope, 6, "add_months_clamp", "date",
                                  "sb.temporal.add_months",
                                  {{"text", "2026-01-31"}, {"bigint", "1"}});
  AppendFunctionProjectionOperand(envelope, 7, "last_day_value", "date",
                                  "sb.temporal.last_day", {{"text", "2026-05-11"}});
  AppendFunctionProjectionOperand(envelope, 8, "last_day_leap", "date",
                                  "sb.temporal.last_day", {{"text", "2024-02-15"}});
  return envelope;
}

sblr::SblrOperationEnvelope TemporalDateTimeBatchProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.temporal_datetime_batch");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "11"});
  AppendFunctionProjectionOperand(envelope, 0, "age_value", "interval", "sb.temporal.age",
                                  {{"text", "2026-05-12T01:02:03"}, {"text", "2026-05-10T00:00:00"}});
  AppendFunctionProjectionOperand(envelope, 1, "age_days", "int64", "sb.temporal.age_in_days",
                                  {{"text", "2026-05-12"}, {"text", "2026-05-10"}});
  AppendFunctionProjectionOperand(envelope, 2, "age_months", "int64", "sb.temporal.age_in_months",
                                  {{"text", "2026-04-01"}, {"text", "2026-01-01"}});
  AppendFunctionProjectionOperand(envelope, 3, "age_years", "int64", "sb.temporal.age_in_years",
                                  {{"text", "2026-01-01"}, {"text", "2024-01-01"}});
  AppendFunctionProjectionOperand(envelope, 4, "date_add_value", "timestamp", "sb.temporal.date_add",
                                  {{"text", "2026-05-10T00:00:00"}, {"text", "P1DT2H"}});
  AppendFunctionProjectionOperand(envelope, 5, "date_sub_value", "timestamp", "sb.temporal.date_sub",
                                  {{"text", "2026-05-10T00:00:00"}, {"text", "P2D"}});
  AppendFunctionProjectionOperand(envelope, 6, "date_diff_value", "int64", "sb.temporal.date_diff",
                                  {{"text", "day"}, {"text", "2026-05-10"}, {"text", "2026-05-15"}});
  AppendFunctionProjectionOperand(envelope, 7, "date_bin_value", "timestamp", "sb.temporal.date_bin",
                                  {{"text", "PT15M"}, {"text", "2026-05-10T10:07:00"},
                                   {"text", "2026-05-10T10:00:00"}});
  AppendFunctionProjectionOperand(envelope, 8, "interval_value", "interval", "sb.temporal.make_interval",
                                  {{"bigint", "1"}, {"bigint", "2"}, {"bigint", "0"}, {"bigint", "3"},
                                   {"bigint", "4"}, {"bigint", "5"}, {"bigint", "6"}});
  AppendFunctionProjectionOperand(envelope, 9, "timezone_value", "timestamp_tz", "sb.temporal.timezone",
                                  {{"text", "UTC"}, {"text", "2026-05-10T10:00:00"}});
  AppendFunctionProjectionOperand(envelope, 10, "timezone_offset_value", "timestamp_tz", "sb.temporal.timezone",
                                  {{"text", "+02:30"}, {"text", "2026-05-10T10:00:00"}});
  return envelope;
}

sblr::SblrOperationEnvelope ProceduralContextProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.procedural_context");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "38"});
  AppendFunctionProjectionOperand(envelope, 0, "session_id_value", "uuid",
                                  "sb.session.session_id", {});
  AppendFunctionProjectionOperand(envelope, 1, "transaction_id_value", "uint64",
                                  "sb.session.transaction_id", {});
  AppendFunctionProjectionOperand(envelope, 2, "transaction_uuid_value", "uuid",
                                  "sb.session.transaction_uuid", {});
  AppendFunctionProjectionOperand(envelope, 3, "current_user_value", "uuid",
                                  "sb.session.current_user", {});
  AppendFunctionProjectionOperand(envelope, 4, "session_user_value", "uuid",
                                  "sb.session.session_user", {});
  AppendFunctionProjectionOperand(envelope, 5, "system_user_value", "uuid",
                                  "sb.session.system_user", {});
  AppendFunctionProjectionOperand(envelope, 6, "user_value", "uuid",
                                  "sb.session.user", {});
  AppendFunctionProjectionOperand(envelope, 7, "current_catalog_value", "uuid",
                                  "sb.session.current_catalog", {});
  AppendFunctionProjectionOperand(envelope, 8, "current_schema_value", "uuid",
                                  "sb.session.current_schema", {});
  AppendFunctionProjectionOperand(envelope, 9, "current_database_value", "uuid",
                                  "sb.session.current_database", {});
  AppendFunctionProjectionOperand(envelope, 10, "current_role_value", "uuid",
                                  "sb.session.current_role", {});
  AppendFunctionProjectionOperand(envelope, 11, "current_server_value", "uuid",
                                  "sb.session.current_server", {});
  AppendFunctionProjectionOperand(envelope, 12, "server_version_value", "character",
                                  "sb.scalar.server_version", {});
  AppendFunctionProjectionOperand(envelope, 13, "server_version_num_value", "uint64",
                                  "sb.scalar.server_version_num", {});
  AppendFunctionProjectionOperand(envelope, 14, "timezone_value", "character",
                                  "sb.scalar.current_setting_timezone", {});
  AppendFunctionProjectionOperand(envelope, 15, "array_rank_limit", "uint64",
                                  "sb.scalar.array_max_dimension", {});
  AppendFunctionProjectionOperand(envelope, 16, "array_element_limit", "uint64",
                                  "sb.scalar.array_max_element_count", {});
  AppendFunctionProjectionOperand(envelope, 17, "case_branch_limit", "uint64",
                                  "sb.scalar.case_when_max_branches", {});
  AppendFunctionProjectionOperand(envelope, 18, "cte_count_limit", "uint64",
                                  "sb.scalar.cte_max_count_per_statement", {});
  AppendFunctionProjectionOperand(envelope, 19, "subquery_depth_limit", "uint64",
                                  "sb.scalar.nested_subquery_max_depth", {});
  AppendFunctionProjectionOperand(envelope, 20, "recursive_cte_depth_limit", "uint64",
                                  "sb.scalar.recursive_cte_max_depth", {});
  AppendFunctionProjectionOperand(envelope, 21, "result_column_limit", "uint64",
                                  "sb.scalar.result_set_max_columns", {});
  AppendFunctionProjectionOperand(envelope, 22, "union_arm_limit", "uint64",
                                  "sb.scalar.union_max_arms", {});
  AppendFunctionProjectionOperand(envelope, 23, "default_charset_value", "character",
                                  "sb.scalar.default_charset", {});
  AppendFunctionProjectionOperand(envelope, 24, "default_collation_value", "character",
                                  "sb.scalar.default_collation", {});
  AppendFunctionProjectionOperand(envelope, 25, "comparison_collation_rule", "character",
                                  "sb.scalar.comparison_collation_resolution", {});
  AppendFunctionProjectionOperand(envelope, 26, "keyword_case_rule_value", "character",
                                  "sb.scalar.keyword_case_rule", {});
  AppendFunctionProjectionOperand(envelope, 27, "quoted_identifier_case_rule_value",
                                  "character", "sb.scalar.quoted_identifier_case_rule", {});
  AppendFunctionProjectionOperand(envelope, 28, "unquoted_identifier_case_rule_value",
                                  "character", "sb.scalar.unquoted_identifier_case_rule", {});
  AppendFunctionProjectionOperand(envelope, 29, "unicode_root_value", "character",
                                  "sb.scalar.unicode_root", {});
  AppendFunctionProjectionOperand(envelope, 30, "current_session_id_alias", "uuid",
                                  "sb.session.session_id", {});
  AppendFunctionProjectionOperand(envelope, 31, "current_session_uuid_alias", "uuid",
                                  "sb.session.session_id", {});
  AppendFunctionProjectionOperand(envelope, 32, "current_transaction_id_alias", "uint64",
                                  "sb.session.transaction_id", {});
  AppendFunctionProjectionOperand(envelope, 33, "current_statement_uuid_value", "uuid",
                                  "sb.session.current_statement_uuid", {});
  AppendFunctionProjectionOperand(envelope, 34, "current_timezone_alias", "character",
                                  "sb.scalar.current_setting_timezone", {});
  AppendFunctionProjectionOperand(envelope, 35, "row_count_value", "uint64",
                                  "sb.fn.diagnostic.row_count", {});
  AppendFunctionProjectionOperand(envelope, 36, "application_name_value", "character",
                                  "sb.scalar.application_name", {});
  AppendFunctionProjectionOperand(envelope, 37, "mga_isolation_profile_value", "character",
                                  "sb.scalar.mga_isolation_profile", {});
  return envelope;
}

sblr::SblrOperationEnvelope Sbsfc016FixedPolicyLimitProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.sbsfc016_fixed_policy_limits");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "8"});
  AppendFunctionProjectionOperand(envelope, 0, "SBSQL-F824C47A36A5", "uint64",
                                  "sb.scalar.array_max_dimension", {});
  AppendFunctionProjectionOperand(envelope, 1, "SBSQL-BCAC432F4C75", "uint64",
                                  "sb.scalar.array_max_element_count", {});
  AppendFunctionProjectionOperand(envelope, 2, "SBSQL-6C698B54A7CB", "uint64",
                                  "sb.scalar.case_when_max_branches", {});
  AppendFunctionProjectionOperand(envelope, 3, "SBSQL-8EAA8898DBEB", "uint64",
                                  "sb.scalar.cte_max_count_per_statement", {});
  AppendFunctionProjectionOperand(envelope, 4, "SBSQL-2CF7F4318343", "uint64",
                                  "sb.scalar.nested_subquery_max_depth", {});
  AppendFunctionProjectionOperand(envelope, 5, "SBSQL-8A442BFCB429", "uint64",
                                  "sb.scalar.recursive_cte_max_depth", {});
  AppendFunctionProjectionOperand(envelope, 6, "SBSQL-189B5E58953A", "uint64",
                                  "sb.scalar.result_set_max_columns", {});
  AppendFunctionProjectionOperand(envelope, 7, "SBSQL-7998B79486A5", "uint64",
                                  "sb.scalar.union_max_arms", {});
  return envelope;
}

sblr::SblrOperationEnvelope Sbsfc016LanguagePolicyProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.sbsfc016_language_policy");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  const std::size_t expected_count =
      sizeof(kSbsfc016LanguagePolicyRows) / sizeof(kSbsfc016LanguagePolicyRows[0]);
  envelope.operands.push_back({"text", "projection_count", std::to_string(expected_count)});
  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& expected = kSbsfc016LanguagePolicyRows[index];
    AppendFunctionProjectionOperand(envelope,
                                    index,
                                    expected.surface_id,
                                    expected.result_type,
                                    expected.function_id,
                                    {});
  }
  return envelope;
}

sblr::SblrOperationEnvelope Sbsfc016MetadataProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.sbsfc016_metadata");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  const std::size_t expected_count =
      sizeof(kSbsfc016MetadataRows) / sizeof(kSbsfc016MetadataRows[0]);
  envelope.operands.push_back({"text", "projection_count", std::to_string(expected_count)});
  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& expected = kSbsfc016MetadataRows[index];
    AppendFunctionProjectionOperand(envelope,
                                    index,
                                    expected.surface_id,
                                    expected.result_type,
                                    expected.function_id,
                                    {});
  }
  return envelope;
}

sblr::SblrOperationEnvelope Sbsfc016ProceduralDiagnosticEngineEnvelope(
    const Sbsfc016ProceduralDiagnosticRow& expected) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.evaluate_projection",
      "SBLR_QUERY_EVALUATE_PROJECTION",
      "trace.query.scalar_projection.sbsfc016_procedural_diagnostic");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope,
                                  0,
                                  expected.surface_id,
                                  "character",
                                  expected.function_id,
                                  {});
  return envelope;
}

sblr::SblrOperationEnvelope CurrentSettingLiteralRefusalEngineEnvelope(std::string_view setting_name) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.current_setting_refusal");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope, 0, "current_setting_refusal", "character",
                                  "sb.scalar.current_setting", {{"text", std::string(setting_name)}});
  return envelope;
}

sblr::SblrOperationEnvelope TextFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.text_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "3"});

  envelope.operands.push_back({"text", "projection_0_name", "lowered"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_0_type", "character"});
  envelope.operands.push_back({"text", "projection_0_value", ""});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_function_id", "sb.scalar.lower"});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", "ALPHA"});
  envelope.operands.push_back({"text", "projection_0_arg_0_is_null", "false"});

  envelope.operands.push_back({"text", "projection_1_name", "uppered"});
  envelope.operands.push_back({"text", "projection_1_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_1_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_1_type", "character"});
  envelope.operands.push_back({"text", "projection_1_value", ""});
  envelope.operands.push_back({"text", "projection_1_is_null", "false"});
  envelope.operands.push_back({"text", "projection_1_function_id", "sb.scalar.upper"});
  envelope.operands.push_back({"text", "projection_1_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_1_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_1_arg_0_value", "beta"});
  envelope.operands.push_back({"text", "projection_1_arg_0_is_null", "false"});

  envelope.operands.push_back({"text", "projection_2_name", "len_value"});
  envelope.operands.push_back({"text", "projection_2_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_2_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_2_type", "int64"});
  envelope.operands.push_back({"text", "projection_2_value", ""});
  envelope.operands.push_back({"text", "projection_2_is_null", "false"});
  envelope.operands.push_back({"text", "projection_2_function_id", "sb.scalar.length"});
  envelope.operands.push_back({"text", "projection_2_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_2_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_2_arg_0_value", "surface"});
  envelope.operands.push_back({"text", "projection_2_arg_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope MoreTextFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.more_text_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "5"});

  envelope.operands.push_back({"text", "projection_0_name", "octets"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_0_type", "int64"});
  envelope.operands.push_back({"text", "projection_0_value", ""});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_function_id", "sb.scalar.octet_length"});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", "hello"});
  envelope.operands.push_back({"text", "projection_0_arg_0_is_null", "false"});

  envelope.operands.push_back({"text", "projection_1_name", "bits"});
  envelope.operands.push_back({"text", "projection_1_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_1_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_1_type", "int64"});
  envelope.operands.push_back({"text", "projection_1_value", ""});
  envelope.operands.push_back({"text", "projection_1_is_null", "false"});
  envelope.operands.push_back({"text", "projection_1_function_id", "sb.scalar.bit_length"});
  envelope.operands.push_back({"text", "projection_1_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_1_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_1_arg_0_value", "hello"});
  envelope.operands.push_back({"text", "projection_1_arg_0_is_null", "false"});

  envelope.operands.push_back({"text", "projection_2_name", "reversed"});
  envelope.operands.push_back({"text", "projection_2_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_2_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_2_type", "character"});
  envelope.operands.push_back({"text", "projection_2_value", ""});
  envelope.operands.push_back({"text", "projection_2_is_null", "false"});
  envelope.operands.push_back({"text", "projection_2_function_id", "sb.scalar.reverse"});
  envelope.operands.push_back({"text", "projection_2_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_2_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_2_arg_0_value", "abc"});
  envelope.operands.push_back({"text", "projection_2_arg_0_is_null", "false"});

  envelope.operands.push_back({"text", "projection_3_name", "code_value"});
  envelope.operands.push_back({"text", "projection_3_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_3_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_3_type", "int64"});
  envelope.operands.push_back({"text", "projection_3_value", ""});
  envelope.operands.push_back({"text", "projection_3_is_null", "false"});
  envelope.operands.push_back({"text", "projection_3_function_id", "sb.scalar.ascii"});
  envelope.operands.push_back({"text", "projection_3_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_3_arg_0_type", "text"});
  envelope.operands.push_back({"text", "projection_3_arg_0_value", "Z"});
  envelope.operands.push_back({"text", "projection_3_arg_0_is_null", "false"});

  envelope.operands.push_back({"text", "projection_4_name", "char_value"});
  envelope.operands.push_back({"text", "projection_4_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_4_expr_opcode", "SBLR_FUNCTION_CALL"});
  envelope.operands.push_back({"text", "projection_4_type", "character"});
  envelope.operands.push_back({"text", "projection_4_value", ""});
  envelope.operands.push_back({"text", "projection_4_is_null", "false"});
  envelope.operands.push_back({"text", "projection_4_function_id", "sb.scalar.chr"});
  envelope.operands.push_back({"text", "projection_4_function_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_4_arg_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_4_arg_0_value", "90"});
  envelope.operands.push_back({"text", "projection_4_arg_0_is_null", "false"});
  return envelope;
}

sblr::SblrOperationEnvelope TrimEncodingFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.trim_encoding_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "8"});
  AppendFunctionProjectionOperand(envelope, 0, "trimmed", "character", "sb.scalar.trim",
                                  {{"text", "  hello  "}});
  AppendFunctionProjectionOperand(envelope, 1, "trimmed_chars", "character", "sb.scalar.trim",
                                  {{"text", "xxhelloxx"}, {"text", "x"}});
  AppendFunctionProjectionOperand(envelope, 2, "btrimmed", "character", "sb.scalar.btrim",
                                  {{"text", "abcabcfooabcabc"}, {"text", "abc"}});
  AppendFunctionProjectionOperand(envelope, 3, "ltrimmed", "character", "sb.scalar.ltrim",
                                  {{"text", "xyzhelloxyz"}, {"text", "xyz"}});
  AppendFunctionProjectionOperand(envelope, 4, "rtrimmed", "character", "sb.scalar.rtrim",
                                  {{"text", "xyzhelloxyz"}, {"text", "xyz"}});
  AppendFunctionProjectionOperand(envelope, 5, "left_space_trimmed", "character",
                                  "sb.scalar.ltrim", {{"text", "  hello"}});
  AppendFunctionProjectionOperand(envelope, 6, "right_space_trimmed", "character",
                                  "sb.scalar.rtrim", {{"text", "hello  "}});
  AppendFunctionProjectionOperand(envelope, 7, "hex_value", "character",
                                  "sb.scalar.to_hex", {{"bigint", "255"}});
  return envelope;
}

sblr::SblrOperationEnvelope TextConditionalFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.text_conditional_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "22"});
  AppendFunctionProjectionOperand(envelope, 0, "sub_part", "character",
                                  "sb.scalar.substring",
                                  {{"text", "hello world"}, {"bigint", "7"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 1, "sub_tail", "character",
                                  "sb.scalar.substring",
                                  {{"text", "hello world"}, {"bigint", "7"}});
  AppendFunctionProjectionOperand(envelope, 2, "substr_part", "character",
                                  "sb.scalar.substr",
                                  {{"text", "hello world"}, {"bigint", "7"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 3, "pos_value", "int64",
                                  "sb.scalar.position",
                                  {{"text", "world"}, {"text", "hello world"}});
  AppendFunctionProjectionOperand(envelope, 4, "left_padded", "character",
                                  "sb.scalar.lpad",
                                  {{"text", "bird"}, {"bigint", "7"}, {"text", "*"}});
  AppendFunctionProjectionOperand(envelope, 5, "right_padded", "character",
                                  "sb.scalar.rpad",
                                  {{"text", "bird"}, {"bigint", "7"}, {"text", "*"}});
  AppendFunctionProjectionOperand(envelope, 6, "repeated", "character",
                                  "sb.scalar.repeat",
                                  {{"text", "ha"}, {"bigint", "3"}});
  AppendFunctionProjectionOperand(envelope, 7, "overlay_short", "character",
                                  "sb.scalar.overlay",
                                  {{"text", "hello world"}, {"text", "SBSQL"}, {"bigint", "7"}});
  AppendFunctionProjectionOperand(envelope, 8, "overlay_for", "character",
                                  "sb.scalar.overlay",
                                  {{"text", "hello world"}, {"text", "new"}, {"bigint", "7"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 9, "instr_pos", "int64",
                                  "sb.scalar.instr",
                                  {{"text", "banana"}, {"text", "na"}, {"bigint", "3"}, {"bigint", "2"}});
  AppendFunctionProjectionOperand(envelope, 10, "strpos_pos", "int64",
                                  "sb.scalar.strpos",
                                  {{"text", "hello world"}, {"text", "world"}});
  AppendFunctionProjectionOperand(envelope, 11, "ifnull_value", "character",
                                  "sb.scalar.ifnull",
                                  {{"text", "", true}, {"text", "fallback"}});
  AppendFunctionProjectionOperand(envelope, 12, "coalesce_value", "character",
                                  "sb.scalar.coalesce",
                                  {{"text", "", true}, {"text", "first"}, {"text", "second"}});
  AppendFunctionProjectionOperand(envelope, 13, "coalesce_strict_value", "character",
                                  "sb.scalar.coalesce_strict",
                                  {{"text", "", true}, {"text", "strict"}, {"text", "second"}});
  AppendFunctionProjectionOperand(envelope, 14, "nullif_value", "character",
                                  "sb.scalar.nullif",
                                  {{"text", "same"}, {"text", "same"}});
  AppendFunctionProjectionOperand(envelope, 15, "nvl2_value", "character",
                                  "sb.scalar.nvl2",
                                  {{"text", "set"}, {"text", "yes"}, {"text", "no"}});
  AppendFunctionProjectionOperand(envelope, 16, "greatest_value", "character",
                                  "sb.scalar.greatest",
                                  {{"bigint", "9"}, {"bigint", "3"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 17, "least_value", "character",
                                  "sb.scalar.least",
                                  {{"bigint", "9"}, {"bigint", "3"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 18, "iif_value", "character",
                                  "sb.scalar.iif",
                                  {{"boolean", "true"}, {"text", "yes"}, {"text", "no"}});
  AppendFunctionProjectionOperand(envelope, 19, "initcap_value", "character",
                                  "sb.scalar.initcap",
                                  {{"text", "hello WORLD-from sb"}});
  AppendFunctionProjectionOperand(envelope, 20, "translate_value", "character",
                                  "sb.scalar.translate",
                                  {{"text", "banana"}, {"text", "an"}, {"text", "ox"}});
  AppendFunctionProjectionOperand(envelope, 21, "unicode_value", "int64",
                                  "sb.scalar.unicode",
                                  {{"text", "A"}});
  return envelope;
}

sblr::SblrOperationEnvelope CoalesceStrictInvalidProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.coalesce_strict_invalid");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  AppendFunctionProjectionOperand(envelope, 0, "coalesce_strict_invalid", "character",
                                  "sb.scalar.coalesce_strict", {});
  return envelope;
}

sblr::SblrOperationEnvelope SqlKeywordTextFunctionProjectionEngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "trace.query.scalar_projection.sql_keyword_text_functions");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "9"});
  AppendFunctionProjectionOperand(envelope, 0, "position_keyword", "int64",
                                  "sb.scalar.position",
                                  {{"text", "lo"}, {"text", "hello"}});
  AppendFunctionProjectionOperand(envelope, 1, "substring_keyword", "character",
                                  "sb.special.substring_keyword",
                                  {{"text", "hello world"}, {"bigint", "7"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 2, "substring_tail_keyword", "character",
                                  "sb.special.substring_keyword",
                                  {{"text", "hello world"}, {"bigint", "7"}});
  AppendFunctionProjectionOperand(envelope, 3, "trim_both_keyword", "character",
                                  "sb.special.trim_keyword",
                                  {{"text", "xxhelloxx"}, {"text", "x"}});
  AppendFunctionProjectionOperand(envelope, 4, "trim_spaces_keyword", "character",
                                  "sb.special.trim_keyword",
                                  {{"text", "  hello  "}});
  AppendFunctionProjectionOperand(envelope, 5, "trim_leading_keyword", "character",
                                  "sb.scalar.ltrim",
                                  {{"text", "xxhello"}, {"text", "x"}});
  AppendFunctionProjectionOperand(envelope, 6, "trim_trailing_keyword", "character",
                                  "sb.scalar.rtrim",
                                  {{"text", "helloxx"}, {"text", "x"}});
  AppendFunctionProjectionOperand(envelope, 7, "overlay_for_keyword", "character",
                                  "sb.scalar.overlay",
                                  {{"text", "hello world"}, {"text", "SBSQL"}, {"bigint", "7"}, {"bigint", "5"}});
  AppendFunctionProjectionOperand(envelope, 8, "overlay_keyword", "character",
                                  "sb.scalar.overlay",
                                  {{"text", "hello world"}, {"text", "SBSQL"}, {"bigint", "7"}});
  return envelope;
}

void RequireScalarLowering() {
  for (const auto& row : kScalarLiteralGrammarRows) {
    RequireScalarLiteralRegistryEvidence(row);
  }
  for (const auto& row : kScalarLiteralFunctionRows) {
    RequireScalarLiteralFunctionRegistryEvidence(row);
  }

  const auto artifacts =
      RunPipeline("SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth");
  Require(artifacts.bound.bound, "scalar SELECT did not bind");
  Require(artifacts.verifier.admitted, "scalar SELECT SBLR verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.relational.v3",
          "scalar SELECT operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.relational.v3",
          "scalar SELECT SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "scalar SELECT operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "scalar SELECT engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "scalar SELECT opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_projection_api_required"),
          "engine query projection authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          "scalar SELECT transaction authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_security_authorization"),
          "parser no-security-authorization authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "parser no-storage/finality authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "parser no-SQL-execution authority step missing");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.query.scalar_projection_descriptor"),
          "scalar projection descriptor ref missing");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"scalar_projection\""),
          "scalar projection payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"4\""),
          "scalar projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_name\":\"one\""),
          "scalar projection first alias missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "scalar projection first expression kind missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_type\":\"bigint\""),
          "scalar projection first type missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_value\":\"1\""),
          "scalar projection first value missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_name\":\"two\""),
          "scalar projection second alias missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_expr_kind\":\"literal\""),
          "scalar projection second expression kind missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_type\":\"text\""),
          "scalar projection second type missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_value\":\"two\""),
          "scalar projection second value missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_expr_kind\":\"literal\""),
          "scalar projection NULL expression kind missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_type\":\"null\""),
          "scalar projection NULL type missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_is_null\":\"true\""),
          "scalar projection NULL marker missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_name\":\"truth\""),
          "scalar projection boolean alias missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_expr_kind\":\"literal\""),
          "scalar projection boolean expression kind missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_type\":\"boolean\""),
          "scalar projection boolean type missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_value\":\"true\""),
          "scalar projection boolean value missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_is_null\":\"false\""),
          "scalar projection boolean null marker mismatch");
  Require(Contains(artifacts.envelope.payload, "\"source_relation_required\":false"),
          "scalar projection claimed a source relation");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "scalar projection claimed row storage");
  Require(!Contains(artifacts.envelope.payload, "SELECT 1"),
          "scalar projection payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "\"target_object_uuid\""),
          "scalar projection payload carried a target object UUID");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "scalar projection payload embedded source_text");

  const auto boolean_artifacts = RunPipeline("SELECT TRUE AS truth");
  Require(boolean_artifacts.bound.bound, "SELECT TRUE scalar literal did not bind");
  Require(boolean_artifacts.verifier.admitted,
          "SELECT TRUE scalar literal verifier rejected exact route");
  Require(boolean_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT TRUE scalar literal operation id mismatch");
  Require(boolean_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT TRUE scalar literal engine API operation id mismatch");
  Require(boolean_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT TRUE scalar literal opcode mismatch");
  Require(Contains(boolean_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT TRUE scalar projection marker missing");
  Require(Contains(boolean_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT TRUE scalar projection count missing");
  Require(Contains(boolean_artifacts.envelope.payload, "\"projection_0_name\":\"truth\""),
          "SELECT TRUE scalar projection alias missing");
  Require(Contains(boolean_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT TRUE scalar projection expression kind missing");
  Require(Contains(boolean_artifacts.envelope.payload, "\"projection_0_type\":\"boolean\""),
          "SELECT TRUE scalar projection boolean type missing");
  Require(Contains(boolean_artifacts.envelope.payload, "\"projection_0_value\":\"true\""),
          "SELECT TRUE scalar projection boolean value missing");
  Require(!Contains(boolean_artifacts.envelope.payload, "SELECT TRUE"),
          "SELECT TRUE scalar projection payload embedded source SQL text");
  Require(!Contains(boolean_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT TRUE scalar projection payload carried a target object UUID");
  Require(Contains(boolean_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT TRUE scalar projection claimed row storage");

  const auto false_artifacts = RunPipeline("SELECT FALSE AS falsehood");
  Require(false_artifacts.bound.bound, "SELECT FALSE scalar literal did not bind");
  Require(false_artifacts.verifier.admitted,
          "SELECT FALSE scalar literal verifier rejected exact route");
  Require(false_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT FALSE scalar literal operation id mismatch");
  Require(false_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT FALSE scalar literal engine API operation id mismatch");
  Require(false_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT FALSE scalar literal opcode mismatch");
  Require(Contains(false_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT FALSE scalar projection marker missing");
  Require(Contains(false_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT FALSE scalar projection count missing");
  Require(Contains(false_artifacts.envelope.payload, "\"projection_0_name\":\"falsehood\""),
          "SELECT FALSE scalar projection alias missing");
  Require(Contains(false_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT FALSE scalar projection expression kind missing");
  Require(Contains(false_artifacts.envelope.payload, "\"projection_0_type\":\"boolean\""),
          "SELECT FALSE scalar projection boolean type missing");
  Require(Contains(false_artifacts.envelope.payload, "\"projection_0_value\":\"false\""),
          "SELECT FALSE scalar projection boolean value missing");
  Require(!Contains(false_artifacts.envelope.payload, "SELECT FALSE"),
          "SELECT FALSE scalar projection payload embedded source SQL text");
  Require(!Contains(false_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT FALSE scalar projection payload carried a target object UUID");
  Require(Contains(false_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT FALSE scalar projection claimed row storage");

  const auto decimal_artifacts = RunPipeline("SELECT 12.34 AS decimal_value");
  Require(decimal_artifacts.bound.bound, "SELECT decimal scalar literal did not bind");
  Require(decimal_artifacts.verifier.admitted,
          "SELECT decimal scalar literal verifier rejected exact route");
  Require(HasNumericLiteralFamily(decimal_artifacts.cst, "12.34", "decimal"),
          "SELECT decimal scalar projection token literal_family mismatch");
  Require(decimal_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT decimal scalar literal operation id mismatch");
  Require(decimal_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT decimal scalar literal engine API operation id mismatch");
  Require(decimal_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT decimal scalar literal opcode mismatch");
  Require(Contains(decimal_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT decimal scalar projection marker missing");
  Require(Contains(decimal_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT decimal scalar projection count missing");
  Require(Contains(decimal_artifacts.envelope.payload, "\"projection_0_name\":\"decimal_value\""),
          "SELECT decimal scalar projection alias missing");
  Require(Contains(decimal_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT decimal scalar projection expression kind missing");
  Require(Contains(decimal_artifacts.envelope.payload, "\"projection_0_type\":\"numeric\""),
          "SELECT decimal scalar projection numeric type missing");
  Require(Contains(decimal_artifacts.envelope.payload, "\"projection_0_value\":\"12.34\""),
          "SELECT decimal scalar projection value missing");
  Require(Contains(decimal_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT decimal scalar projection null marker mismatch");
  Require(Contains(decimal_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT decimal scalar projection claimed a source relation");
  Require(Contains(decimal_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT decimal scalar projection claimed row storage");
  Require(!Contains(decimal_artifacts.envelope.payload, "SELECT 12.34"),
          "SELECT decimal scalar projection payload embedded source SQL text");
  Require(!Contains(decimal_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT decimal scalar projection payload carried a target object UUID");
  Require(!Contains(decimal_artifacts.envelope.payload, "\"source_text\""),
          "SELECT decimal scalar projection payload embedded source_text");

  const auto uint_artifacts = RunPipeline("SELECT 42U AS uint_value");
  Require(uint_artifacts.bound.bound, "SELECT uint scalar literal did not bind");
  Require(uint_artifacts.verifier.admitted,
          "SELECT uint scalar literal verifier rejected exact route");
  Require(HasNumericLiteralFamily(uint_artifacts.cst, "42U", "uint"),
          "SELECT uint scalar projection token literal_family mismatch");
  Require(uint_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT uint scalar literal operation id mismatch");
  Require(uint_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT uint scalar literal engine API operation id mismatch");
  Require(uint_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT uint scalar literal opcode mismatch");
  Require(Contains(uint_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT uint scalar projection marker missing");
  Require(Contains(uint_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT uint scalar projection count missing");
  Require(Contains(uint_artifacts.envelope.payload, "\"projection_0_name\":\"uint_value\""),
          "SELECT uint scalar projection alias missing");
  Require(Contains(uint_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT uint scalar projection expression kind missing");
  Require(Contains(uint_artifacts.envelope.payload, "\"projection_0_type\":\"uint64\""),
          "SELECT uint scalar projection uint64 type missing");
  Require(Contains(uint_artifacts.envelope.payload, "\"projection_0_value\":\"42\""),
          "SELECT uint scalar projection value missing");
  Require(Contains(uint_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT uint scalar projection null marker mismatch");
  Require(Contains(uint_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT uint scalar projection claimed a source relation");
  Require(Contains(uint_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT uint scalar projection claimed row storage");
  Require(!Contains(uint_artifacts.envelope.payload, "SELECT 42U"),
          "SELECT uint scalar projection payload embedded source SQL text");
  Require(!Contains(uint_artifacts.envelope.payload, "42U"),
          "SELECT uint scalar projection payload retained the numeric suffix");
  Require(!Contains(uint_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT uint scalar projection payload carried a target object UUID");
  Require(!Contains(uint_artifacts.envelope.payload, "\"source_text\""),
          "SELECT uint scalar projection payload embedded source_text");

  const auto int128_artifacts = RunPipeline("SELECT 123I128 AS int128_value");
  Require(int128_artifacts.bound.bound, "SELECT int128 scalar literal did not bind");
  Require(int128_artifacts.verifier.admitted,
          "SELECT int128 scalar literal verifier rejected exact route");
  Require(HasNumericLiteralFamily(int128_artifacts.cst, "123I128", "int128"),
          "SELECT int128 scalar projection token literal_family mismatch");
  Require(int128_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT int128 scalar literal operation id mismatch");
  Require(int128_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT int128 scalar literal engine API operation id mismatch");
  Require(int128_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT int128 scalar literal opcode mismatch");
  Require(Contains(int128_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT int128 scalar projection marker missing");
  Require(Contains(int128_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT int128 scalar projection count missing");
  Require(Contains(int128_artifacts.envelope.payload,
                   "\"projection_0_name\":\"int128_value\""),
          "SELECT int128 scalar projection alias missing");
  Require(Contains(int128_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT int128 scalar projection expression kind missing");
  Require(Contains(int128_artifacts.envelope.payload, "\"projection_0_type\":\"int128\""),
          "SELECT int128 scalar projection int128 type missing");
  Require(Contains(int128_artifacts.envelope.payload, "\"projection_0_value\":\"123\""),
          "SELECT int128 scalar projection value missing");
  Require(Contains(int128_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT int128 scalar projection null marker mismatch");
  Require(Contains(int128_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT int128 scalar projection claimed a source relation");
  Require(Contains(int128_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT int128 scalar projection claimed row storage");
  Require(!Contains(int128_artifacts.envelope.payload, "SELECT 123I128"),
          "SELECT int128 scalar projection payload embedded source SQL text");
  Require(!Contains(int128_artifacts.envelope.payload, "123I128"),
          "SELECT int128 scalar projection payload retained the numeric suffix");
  Require(!Contains(int128_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT int128 scalar projection payload carried a target object UUID");
  Require(!Contains(int128_artifacts.envelope.payload, "\"source_text\""),
          "SELECT int128 scalar projection payload embedded source_text");

  const auto uint128_artifacts = RunPipeline("SELECT 123U128 AS uint128_value");
  Require(uint128_artifacts.bound.bound, "SELECT uint128 scalar literal did not bind");
  Require(uint128_artifacts.verifier.admitted,
          "SELECT uint128 scalar literal verifier rejected exact route");
  Require(HasNumericLiteralFamily(uint128_artifacts.cst, "123U128", "uint128"),
          "SELECT uint128 scalar projection token literal_family mismatch");
  Require(uint128_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT uint128 scalar literal operation id mismatch");
  Require(uint128_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT uint128 scalar literal engine API operation id mismatch");
  Require(uint128_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT uint128 scalar literal opcode mismatch");
  Require(Contains(uint128_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT uint128 scalar projection marker missing");
  Require(Contains(uint128_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT uint128 scalar projection count missing");
  Require(Contains(uint128_artifacts.envelope.payload,
                   "\"projection_0_name\":\"uint128_value\""),
          "SELECT uint128 scalar projection alias missing");
  Require(Contains(uint128_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT uint128 scalar projection expression kind missing");
  Require(Contains(uint128_artifacts.envelope.payload, "\"projection_0_type\":\"uint128\""),
          "SELECT uint128 scalar projection uint128 type missing");
  Require(Contains(uint128_artifacts.envelope.payload, "\"projection_0_value\":\"123\""),
          "SELECT uint128 scalar projection value missing");
  Require(Contains(uint128_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT uint128 scalar projection null marker mismatch");
  Require(Contains(uint128_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT uint128 scalar projection claimed a source relation");
  Require(Contains(uint128_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT uint128 scalar projection claimed row storage");
  Require(!Contains(uint128_artifacts.envelope.payload, "SELECT 123U128"),
          "SELECT uint128 scalar projection payload embedded source SQL text");
  Require(!Contains(uint128_artifacts.envelope.payload, "123U128"),
          "SELECT uint128 scalar projection payload retained the numeric suffix");
  Require(!Contains(uint128_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT uint128 scalar projection payload carried a target object UUID");
  Require(!Contains(uint128_artifacts.envelope.payload, "\"source_text\""),
          "SELECT uint128 scalar projection payload embedded source_text");

  const auto real128_artifacts = RunPipeline("SELECT 1.25R128 AS real128_value");
  Require(real128_artifacts.bound.bound, "SELECT real128 scalar literal did not bind");
  Require(real128_artifacts.verifier.admitted,
          "SELECT real128 scalar literal verifier rejected exact route");
  Require(HasNumericLiteralFamily(real128_artifacts.cst, "1.25R128", "real128"),
          "SELECT real128 scalar projection token literal_family mismatch");
  Require(real128_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT real128 scalar literal operation id mismatch");
  Require(real128_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT real128 scalar literal engine API operation id mismatch");
  Require(real128_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT real128 scalar literal opcode mismatch");
  Require(Contains(real128_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT real128 scalar projection marker missing");
  Require(Contains(real128_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT real128 scalar projection count missing");
  Require(Contains(real128_artifacts.envelope.payload,
                   "\"projection_0_name\":\"real128_value\""),
          "SELECT real128 scalar projection alias missing");
  Require(Contains(real128_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT real128 scalar projection expression kind missing");
  Require(Contains(real128_artifacts.envelope.payload, "\"projection_0_type\":\"real128\""),
          "SELECT real128 scalar projection real128 type missing");
  Require(Contains(real128_artifacts.envelope.payload, "\"projection_0_value\":\"1.25\""),
          "SELECT real128 scalar projection value missing");
  Require(Contains(real128_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT real128 scalar projection null marker mismatch");
  Require(Contains(real128_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT real128 scalar projection claimed a source relation");
  Require(Contains(real128_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT real128 scalar projection claimed row storage");
  Require(!Contains(real128_artifacts.envelope.payload, "SELECT 1.25R128"),
          "SELECT real128 scalar projection payload embedded source SQL text");
  Require(!Contains(real128_artifacts.envelope.payload, "1.25R128"),
          "SELECT real128 scalar projection payload retained the numeric suffix");
  Require(!Contains(real128_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT real128 scalar projection payload carried a target object UUID");
  Require(!Contains(real128_artifacts.envelope.payload, "\"source_text\""),
          "SELECT real128 scalar projection payload embedded source_text");

  const auto float_artifacts = RunPipeline("SELECT 1e2 AS float_value");
  Require(float_artifacts.bound.bound, "SELECT float scalar literal did not bind");
  Require(float_artifacts.verifier.admitted,
          "SELECT float scalar literal verifier rejected exact route");
  Require(HasNumericLiteralFamily(float_artifacts.cst, "1e2", "float"),
          "SELECT float scalar projection token literal_family mismatch");
  Require(float_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT float scalar literal operation id mismatch");
  Require(float_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT float scalar literal engine API operation id mismatch");
  Require(float_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT float scalar literal opcode mismatch");
  Require(Contains(float_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT float scalar projection marker missing");
  Require(Contains(float_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT float scalar projection count missing");
  Require(Contains(float_artifacts.envelope.payload, "\"projection_0_name\":\"float_value\""),
          "SELECT float scalar projection alias missing");
  Require(Contains(float_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT float scalar projection expression kind missing");
  Require(Contains(float_artifacts.envelope.payload, "\"projection_0_type\":\"numeric\""),
          "SELECT float scalar projection numeric type missing");
  Require(Contains(float_artifacts.envelope.payload, "\"projection_0_value\":\"1e2\""),
          "SELECT float scalar projection value missing");
  Require(Contains(float_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT float scalar projection null marker mismatch");
  Require(Contains(float_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT float scalar projection claimed a source relation");
  Require(Contains(float_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT float scalar projection claimed row storage");
  Require(!Contains(float_artifacts.envelope.payload, "SELECT 1e2"),
          "SELECT float scalar projection payload embedded source SQL text");
  Require(!Contains(float_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT float scalar projection payload carried a target object UUID");
  Require(!Contains(float_artifacts.envelope.payload, "\"source_text\""),
          "SELECT float scalar projection payload embedded source_text");

  const auto binary_artifacts = RunPipeline("SELECT X'00ff10' AS binary_value");
  Require(binary_artifacts.bound.bound, "SELECT binary scalar literal did not bind");
  Require(binary_artifacts.verifier.admitted,
          "SELECT binary scalar literal verifier rejected exact route");
  const std::string binary_token_value =
      FirstBinaryLiteralText(binary_artifacts.cst, "hex_binary");
  Require(binary_token_value == "00ff10",
          "SELECT binary scalar projection token literal_family/value mismatch");
  Require(binary_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT binary scalar literal operation id mismatch");
  Require(binary_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT binary scalar literal engine API operation id mismatch");
  Require(binary_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT binary scalar literal opcode mismatch");
  Require(Contains(binary_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT binary scalar projection marker missing");
  Require(Contains(binary_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT binary scalar projection count missing");
  Require(Contains(binary_artifacts.envelope.payload, "\"projection_0_name\":\"binary_value\""),
          "SELECT binary scalar projection alias missing");
  Require(Contains(binary_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT binary scalar projection expression kind missing");
  Require(Contains(binary_artifacts.envelope.payload, "\"projection_0_type\":\"binary\""),
          "SELECT binary scalar projection binary type missing");
  Require(Contains(binary_artifacts.envelope.payload,
                   std::string("\"projection_0_value\":\"") + binary_token_value + "\""),
          "SELECT binary scalar projection value did not match lexer payload");
  Require(Contains(binary_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT binary scalar projection null marker mismatch");
  Require(Contains(binary_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT binary scalar projection claimed a source relation");
  Require(Contains(binary_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT binary scalar projection claimed row storage");
  Require(!Contains(binary_artifacts.envelope.payload, "SELECT X"),
          "SELECT binary scalar projection payload embedded source SQL text");
  Require(!Contains(binary_artifacts.envelope.payload, "X'00ff10'"),
          "SELECT binary scalar projection payload embedded source literal text");
  Require(!Contains(binary_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT binary scalar projection payload carried a target object UUID");
  Require(!Contains(binary_artifacts.envelope.payload, "\"source_text\""),
          "SELECT binary scalar projection payload embedded source_text");

  const auto uuid_artifacts =
      RunPipeline("SELECT UUID '550e8400-e29b-41d4-a716-446655440000' AS uuid_value");
  Require(uuid_artifacts.bound.bound, "SELECT UUID scalar literal did not bind");
  Require(uuid_artifacts.verifier.admitted,
          "SELECT UUID scalar literal verifier rejected exact route");
  const std::string uuid_token_value = FirstUuidLiteralText(uuid_artifacts.cst);
  Require(uuid_token_value == "550e8400-e29b-41d4-a716-446655440000",
          "SELECT UUID scalar projection token literal_family/value mismatch");
  Require(uuid_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT UUID scalar literal operation id mismatch");
  Require(uuid_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT UUID scalar literal engine API operation id mismatch");
  Require(uuid_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT UUID scalar literal opcode mismatch");
  Require(Contains(uuid_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT UUID scalar projection marker missing");
  Require(Contains(uuid_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT UUID scalar projection count missing");
  Require(Contains(uuid_artifacts.envelope.payload, "\"projection_0_name\":\"uuid_value\""),
          "SELECT UUID scalar projection alias missing");
  Require(Contains(uuid_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT UUID scalar projection expression kind missing");
  Require(Contains(uuid_artifacts.envelope.payload, "\"projection_0_type\":\"uuid\""),
          "SELECT UUID scalar projection UUID type missing");
  Require(Contains(uuid_artifacts.envelope.payload,
                   std::string("\"projection_0_value\":\"") + uuid_token_value + "\""),
          "SELECT UUID scalar projection value did not match lexer payload");
  Require(Contains(uuid_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT UUID scalar projection null marker mismatch");
  Require(Contains(uuid_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT UUID scalar projection claimed a source relation");
  Require(Contains(uuid_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT UUID scalar projection claimed row storage");
  Require(!Contains(uuid_artifacts.envelope.payload, "SELECT UUID"),
          "SELECT UUID scalar projection payload embedded source SQL text");
  Require(!Contains(uuid_artifacts.envelope.payload, "UUID '550e8400"),
          "SELECT UUID scalar projection payload embedded source literal text");
  Require(!Contains(uuid_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT UUID scalar projection payload carried a target object UUID");
  Require(!Contains(uuid_artifacts.envelope.payload, "\"source_text\""),
          "SELECT UUID scalar projection payload embedded source_text");

  const auto date_artifacts = RunPipeline("SELECT DATE '2026-05-14' AS date_value");
  Require(date_artifacts.bound.bound, "SELECT DATE scalar literal did not bind");
  Require(date_artifacts.verifier.admitted,
          "SELECT DATE scalar literal verifier rejected exact route");
  const std::string date_token_value = FirstTemporalLiteralText(date_artifacts.cst, "DATE");
  Require(date_token_value == "2026-05-14",
          "SELECT DATE scalar projection token literal_family/value mismatch");
  Require(date_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT DATE scalar literal operation id mismatch");
  Require(date_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT DATE scalar literal engine API operation id mismatch");
  Require(date_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT DATE scalar literal opcode mismatch");
  Require(Contains(date_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT DATE scalar projection marker missing");
  Require(Contains(date_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT DATE scalar projection count missing");
  Require(Contains(date_artifacts.envelope.payload, "\"projection_0_name\":\"date_value\""),
          "SELECT DATE scalar projection alias missing");
  Require(Contains(date_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT DATE scalar projection expression kind missing");
  Require(Contains(date_artifacts.envelope.payload, "\"projection_0_type\":\"date\""),
          "SELECT DATE scalar projection date type missing");
  Require(Contains(date_artifacts.envelope.payload,
                   std::string("\"projection_0_value\":\"") + date_token_value + "\""),
          "SELECT DATE scalar projection value did not match lexer payload");
  Require(Contains(date_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT DATE scalar projection null marker mismatch");
  Require(Contains(date_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT DATE scalar projection claimed a source relation");
  Require(Contains(date_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT DATE scalar projection claimed row storage");
  Require(!Contains(date_artifacts.envelope.payload, "SELECT DATE"),
          "SELECT DATE scalar projection payload embedded source SQL text");
  Require(!Contains(date_artifacts.envelope.payload, "DATE '2026"),
          "SELECT DATE scalar projection payload embedded source literal text");
  Require(!Contains(date_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT DATE scalar projection payload carried a target object UUID");
  Require(!Contains(date_artifacts.envelope.payload, "\"source_text\""),
          "SELECT DATE scalar projection payload embedded source_text");

  const auto time_artifacts = RunPipeline("SELECT TIME '14:23:45' AS time_value");
  Require(time_artifacts.bound.bound, "SELECT TIME scalar literal did not bind");
  Require(time_artifacts.verifier.admitted,
          "SELECT TIME scalar literal verifier rejected exact route");
  const std::string time_token_value = FirstTemporalLiteralText(time_artifacts.cst, "TIME");
  Require(time_token_value == "14:23:45",
          "SELECT TIME scalar projection token literal_family/value mismatch");
  Require(time_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT TIME scalar literal operation id mismatch");
  Require(time_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT TIME scalar literal engine API operation id mismatch");
  Require(time_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT TIME scalar literal opcode mismatch");
  Require(Contains(time_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT TIME scalar projection marker missing");
  Require(Contains(time_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT TIME scalar projection count missing");
  Require(Contains(time_artifacts.envelope.payload, "\"projection_0_name\":\"time_value\""),
          "SELECT TIME scalar projection alias missing");
  Require(Contains(time_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT TIME scalar projection expression kind missing");
  Require(Contains(time_artifacts.envelope.payload, "\"projection_0_type\":\"time\""),
          "SELECT TIME scalar projection time type missing");
  Require(Contains(time_artifacts.envelope.payload,
                   std::string("\"projection_0_value\":\"") + time_token_value + "\""),
          "SELECT TIME scalar projection value did not match lexer payload");
  Require(Contains(time_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT TIME scalar projection null marker mismatch");
  Require(Contains(time_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT TIME scalar projection claimed a source relation");
  Require(Contains(time_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT TIME scalar projection claimed row storage");
  Require(!Contains(time_artifacts.envelope.payload, "SELECT TIME"),
          "SELECT TIME scalar projection payload embedded source SQL text");
  Require(!Contains(time_artifacts.envelope.payload, "TIME '14:23"),
          "SELECT TIME scalar projection payload embedded source literal text");
  Require(!Contains(time_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT TIME scalar projection payload carried a target object UUID");
  Require(!Contains(time_artifacts.envelope.payload, "\"source_text\""),
          "SELECT TIME scalar projection payload embedded source_text");

  const auto timestamp_artifacts =
      RunPipeline("SELECT TIMESTAMP '2026-05-14T14:23:45' AS timestamp_value");
  Require(timestamp_artifacts.bound.bound, "SELECT TIMESTAMP scalar literal did not bind");
  Require(timestamp_artifacts.verifier.admitted,
          "SELECT TIMESTAMP scalar literal verifier rejected exact route");
  const std::string timestamp_token_value =
      FirstTemporalLiteralText(timestamp_artifacts.cst, "TIMESTAMP");
  Require(timestamp_token_value == "2026-05-14T14:23:45",
          "SELECT TIMESTAMP scalar projection token literal_family/value mismatch");
  Require(timestamp_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT TIMESTAMP scalar literal operation id mismatch");
  Require(timestamp_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT TIMESTAMP scalar literal engine API operation id mismatch");
  Require(timestamp_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT TIMESTAMP scalar literal opcode mismatch");
  Require(Contains(timestamp_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT TIMESTAMP scalar projection marker missing");
  Require(Contains(timestamp_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT TIMESTAMP scalar projection count missing");
  Require(Contains(timestamp_artifacts.envelope.payload,
                   "\"projection_0_name\":\"timestamp_value\""),
          "SELECT TIMESTAMP scalar projection alias missing");
  Require(Contains(timestamp_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT TIMESTAMP scalar projection expression kind missing");
  Require(Contains(timestamp_artifacts.envelope.payload, "\"projection_0_type\":\"timestamp\""),
          "SELECT TIMESTAMP scalar projection timestamp type missing");
  Require(Contains(timestamp_artifacts.envelope.payload,
                   std::string("\"projection_0_value\":\"") + timestamp_token_value + "\""),
          "SELECT TIMESTAMP scalar projection value did not match lexer payload");
  Require(Contains(timestamp_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT TIMESTAMP scalar projection null marker mismatch");
  Require(Contains(timestamp_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT TIMESTAMP scalar projection claimed a source relation");
  Require(Contains(timestamp_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT TIMESTAMP scalar projection claimed row storage");
  Require(!Contains(timestamp_artifacts.envelope.payload, "SELECT TIMESTAMP"),
          "SELECT TIMESTAMP scalar projection payload embedded source SQL text");
  Require(!Contains(timestamp_artifacts.envelope.payload, "TIMESTAMP '2026"),
          "SELECT TIMESTAMP scalar projection payload embedded source literal text");
  Require(!Contains(timestamp_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT TIMESTAMP scalar projection payload carried a target object UUID");
  Require(!Contains(timestamp_artifacts.envelope.payload, "\"source_text\""),
          "SELECT TIMESTAMP scalar projection payload embedded source_text");

  const auto interval_artifacts =
      RunPipeline("SELECT INTERVAL '1 day' AS interval_value");
  Require(interval_artifacts.bound.bound, "SELECT INTERVAL scalar literal did not bind");
  Require(interval_artifacts.verifier.admitted,
          "SELECT INTERVAL scalar literal verifier rejected exact route");
  const std::string interval_token_value =
      FirstTemporalLiteralText(interval_artifacts.cst, "INTERVAL");
  Require(interval_token_value == "1 day",
          "SELECT INTERVAL scalar projection token literal_family/value mismatch");
  Require(interval_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT INTERVAL scalar literal operation id mismatch");
  Require(interval_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT INTERVAL scalar literal engine API operation id mismatch");
  Require(interval_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT INTERVAL scalar literal opcode mismatch");
  Require(Contains(interval_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT INTERVAL scalar projection marker missing");
  Require(Contains(interval_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT INTERVAL scalar projection count missing");
  Require(Contains(interval_artifacts.envelope.payload,
                   "\"projection_0_name\":\"interval_value\""),
          "SELECT INTERVAL scalar projection alias missing");
  Require(Contains(interval_artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "SELECT INTERVAL scalar projection expression kind missing");
  Require(Contains(interval_artifacts.envelope.payload, "\"projection_0_type\":\"interval\""),
          "SELECT INTERVAL scalar projection interval type missing");
  Require(Contains(interval_artifacts.envelope.payload,
                   std::string("\"projection_0_value\":\"") + interval_token_value + "\""),
          "SELECT INTERVAL scalar projection value did not match lexer payload");
  Require(Contains(interval_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT INTERVAL scalar projection null marker mismatch");
  Require(Contains(interval_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT INTERVAL scalar projection claimed a source relation");
  Require(Contains(interval_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT INTERVAL scalar projection claimed row storage");
  Require(!Contains(interval_artifacts.envelope.payload, "SELECT INTERVAL"),
          "SELECT INTERVAL scalar projection payload embedded source SQL text");
  Require(!Contains(interval_artifacts.envelope.payload, "INTERVAL '1 day"),
          "SELECT INTERVAL scalar projection payload embedded source literal text");
  Require(!Contains(interval_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT INTERVAL scalar projection payload carried a target object UUID");
  Require(!Contains(interval_artifacts.envelope.payload, "\"source_text\""),
          "SELECT INTERVAL scalar projection payload embedded source_text");

  const auto interval_qualified_artifacts =
      RunPipeline("SELECT INTERVAL '1' DAY AS interval_value");
  Require(interval_qualified_artifacts.bound.bound,
          "SELECT INTERVAL qualified scalar literal did not bind");
  Require(interval_qualified_artifacts.verifier.admitted,
          "SELECT INTERVAL qualified scalar literal verifier rejected exact route");
  const std::string interval_qualified_token_value =
      FirstTemporalLiteralText(interval_qualified_artifacts.cst, "INTERVAL");
  Require(interval_qualified_token_value == "1",
          "SELECT INTERVAL qualified token literal_family/value mismatch");
  Require(interval_qualified_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT INTERVAL qualified scalar literal operation id mismatch");
  Require(interval_qualified_artifacts.envelope.sblr_opcode ==
              "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT INTERVAL qualified scalar literal opcode mismatch");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT INTERVAL qualified scalar projection marker missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_name\":\"interval_value\""),
          "SELECT INTERVAL qualified scalar projection alias missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_expr_kind\":\"literal\""),
          "SELECT INTERVAL qualified scalar projection expression kind missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_type\":\"interval\""),
          "SELECT INTERVAL qualified scalar projection interval type missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_value\":\"1 day\""),
          "SELECT INTERVAL qualified scalar projection normalized value mismatch");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_literal_family\":\"INTERVAL\""),
          "SELECT INTERVAL qualified scalar projection literal family missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_interval_qualifier\":\"DAY\""),
          "SELECT INTERVAL qualified scalar projection qualifier missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_interval_unit\":\"DAY\""),
          "SELECT INTERVAL qualified scalar projection unit missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_interval_literal_payload\":\"1\""),
          "SELECT INTERVAL qualified scalar projection raw interval payload missing");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"projection_0_is_null\":\"false\""),
          "SELECT INTERVAL qualified scalar projection null marker mismatch");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"source_relation_required\":false"),
          "SELECT INTERVAL qualified scalar projection claimed a source relation");
  Require(Contains(interval_qualified_artifacts.envelope.payload,
                   "\"row_storage_touched\":false"),
          "SELECT INTERVAL qualified scalar projection claimed row storage");
  Require(!Contains(interval_qualified_artifacts.envelope.payload, "SELECT INTERVAL"),
          "SELECT INTERVAL qualified scalar projection payload embedded source SQL text");
  Require(!Contains(interval_qualified_artifacts.envelope.payload, "INTERVAL '1"),
          "SELECT INTERVAL qualified scalar projection payload embedded source literal text");
  Require(!Contains(interval_qualified_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT INTERVAL qualified scalar projection payload carried a target object UUID");
  Require(!Contains(interval_qualified_artifacts.envelope.payload, "\"source_text\""),
          "SELECT INTERVAL qualified scalar projection payload embedded source_text");

  const auto callable_interval_literal =
      RunPipeline("SELECT interval_literal(INTERVAL '1 day') AS interval_value");
  Require(callable_interval_literal.bound.bound,
          "interval_literal callable negative route did not reach binder");
  Require(!callable_interval_literal.verifier.admitted,
          "interval_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_interval_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "interval_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_interval_literal.envelope.payload,
                    "\"function_id\":\"interval_literal\""),
          "interval_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_interval_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.interval_literal\""),
          "interval_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_interval_literal.envelope.payload, "SELECT interval_literal"),
          "interval_literal callable negative route embedded source SQL text");

  const auto document_artifacts =
      RunPipeline("SELECT DOCUMENT '{\"a\":1}' AS document_value");
  Require(document_artifacts.bound.bound,
          "SELECT DOCUMENT scalar literal did not bind");
  Require(document_artifacts.verifier.admitted,
          "SELECT DOCUMENT scalar literal verifier rejected exact route");
  const std::string document_token_value =
      FirstDocumentLiteralText(document_artifacts.cst, "DOCUMENT");
  Require(document_token_value == "{\"a\":1}",
          "SELECT DOCUMENT scalar projection token literal_family/value mismatch");
  Require(document_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT DOCUMENT scalar literal operation id mismatch");
  Require(document_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT DOCUMENT scalar literal engine API operation id mismatch");
  Require(document_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT DOCUMENT scalar literal opcode mismatch");
  Require(Contains(document_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT DOCUMENT scalar projection marker missing");
  Require(Contains(document_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT DOCUMENT scalar projection count missing");
  Require(Contains(document_artifacts.envelope.payload,
                   "\"projection_0_name\":\"document_value\""),
          "SELECT DOCUMENT scalar projection alias missing");
  Require(Contains(document_artifacts.envelope.payload,
                   "\"projection_0_expr_kind\":\"literal\""),
          "SELECT DOCUMENT scalar projection expression kind missing");
  Require(Contains(document_artifacts.envelope.payload,
                   "\"projection_0_type\":\"document\""),
          "SELECT DOCUMENT scalar projection document type missing");
  Require(Contains(document_artifacts.envelope.payload,
                   "\"projection_0_value\":\"{\\\"a\\\":1}\""),
          "SELECT DOCUMENT scalar projection value did not match lexer payload");
  Require(Contains(document_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT DOCUMENT scalar projection null marker mismatch");
  Require(Contains(document_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT DOCUMENT scalar projection claimed a source relation");
  Require(Contains(document_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT DOCUMENT scalar projection claimed row storage");
  Require(!Contains(document_artifacts.envelope.payload, "SELECT DOCUMENT"),
          "SELECT DOCUMENT scalar projection payload embedded source SQL text");
  Require(!Contains(document_artifacts.envelope.payload, "DOCUMENT '{"),
          "SELECT DOCUMENT scalar projection payload embedded source literal text");
  Require(!Contains(document_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT DOCUMENT scalar projection payload carried a target object UUID");
  Require(!Contains(document_artifacts.envelope.payload, "\"source_text\""),
          "SELECT DOCUMENT scalar projection payload embedded source_text");

  const auto callable_document_literal =
      RunPipeline("SELECT document_literal(DOCUMENT '{\"a\":1}') AS document_value");
  Require(callable_document_literal.bound.bound,
          "document_literal callable negative route did not reach binder");
  Require(!callable_document_literal.verifier.admitted,
          "document_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_document_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "document_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_document_literal.envelope.payload,
                    "\"function_id\":\"document_literal\""),
          "document_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_document_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.document_literal\""),
          "document_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_document_literal.envelope.payload, "SELECT document_literal"),
          "document_literal callable negative route embedded source SQL text");

  const auto json_artifacts =
      RunPipeline("SELECT JSON '{\"a\":1}' AS json_value");
  Require(json_artifacts.bound.bound,
          "SELECT JSON scalar literal did not bind");
  Require(json_artifacts.verifier.admitted,
          "SELECT JSON scalar literal verifier rejected exact route");
  const std::string json_token_value =
      FirstDocumentLiteralText(json_artifacts.cst, "JSON");
  Require(json_token_value == "{\"a\":1}",
          "SELECT JSON scalar projection token literal_family/value mismatch");
  Require(json_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT JSON scalar literal operation id mismatch");
  Require(json_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT JSON scalar literal engine API operation id mismatch");
  Require(json_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT JSON scalar literal opcode mismatch");
  Require(Contains(json_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT JSON scalar projection marker missing");
  Require(Contains(json_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT JSON scalar projection count missing");
  Require(Contains(json_artifacts.envelope.payload,
                   "\"projection_0_name\":\"json_value\""),
          "SELECT JSON scalar projection alias missing");
  Require(Contains(json_artifacts.envelope.payload,
                   "\"projection_0_expr_kind\":\"literal\""),
          "SELECT JSON scalar projection expression kind missing");
  Require(Contains(json_artifacts.envelope.payload,
                   "\"projection_0_type\":\"json_document\""),
          "SELECT JSON scalar projection json_document type missing");
  Require(Contains(json_artifacts.envelope.payload,
                   "\"projection_0_value\":\"{\\\"a\\\":1}\""),
          "SELECT JSON scalar projection value did not match lexer payload");
  Require(Contains(json_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT JSON scalar projection null marker mismatch");
  Require(Contains(json_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT JSON scalar projection claimed a source relation");
  Require(Contains(json_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT JSON scalar projection claimed row storage");
  Require(!Contains(json_artifacts.envelope.payload, "SELECT JSON"),
          "SELECT JSON scalar projection payload embedded source SQL text");
  Require(!Contains(json_artifacts.envelope.payload, "JSON '{"),
          "SELECT JSON scalar projection payload embedded source literal text");
  Require(!Contains(json_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT JSON scalar projection payload carried a target object UUID");
  Require(!Contains(json_artifacts.envelope.payload, "\"source_text\""),
          "SELECT JSON scalar projection payload embedded source_text");

  const auto vector_artifacts =
      RunPipeline("SELECT VECTOR '[1,2,3]' AS vector_value");
  Require(vector_artifacts.bound.bound,
          "SELECT VECTOR scalar literal did not bind");
  Require(vector_artifacts.verifier.admitted,
          "SELECT VECTOR scalar literal verifier rejected exact route");
  const std::string vector_token_value =
      FirstVectorLiteralText(vector_artifacts.cst, "VECTOR");
  Require(vector_token_value == "[1,2,3]",
          "SELECT VECTOR scalar projection token literal_family/value mismatch");
  Require(vector_artifacts.envelope.operation_id == "query.evaluate_projection",
          "SELECT VECTOR scalar literal operation id mismatch");
  Require(vector_artifacts.envelope.engine_api_operation_id == "query.evaluate_projection",
          "SELECT VECTOR scalar literal engine API operation id mismatch");
  Require(vector_artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SELECT VECTOR scalar literal opcode mismatch");
  Require(Contains(vector_artifacts.envelope.payload,
                   "\"query_envelope_kind\":\"scalar_projection\""),
          "SELECT VECTOR scalar projection marker missing");
  Require(Contains(vector_artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "SELECT VECTOR scalar projection count missing");
  Require(Contains(vector_artifacts.envelope.payload,
                   "\"projection_0_name\":\"vector_value\""),
          "SELECT VECTOR scalar projection alias missing");
  Require(Contains(vector_artifacts.envelope.payload,
                   "\"projection_0_expr_kind\":\"literal\""),
          "SELECT VECTOR scalar projection expression kind missing");
  Require(Contains(vector_artifacts.envelope.payload,
                   "\"projection_0_type\":\"dense_vector\""),
          "SELECT VECTOR scalar projection dense_vector type missing");
  Require(Contains(vector_artifacts.envelope.payload,
                   "\"projection_0_value\":\"[1,2,3]\""),
          "SELECT VECTOR scalar projection value did not match lexer payload");
  Require(Contains(vector_artifacts.envelope.payload, "\"projection_0_is_null\":\"false\""),
          "SELECT VECTOR scalar projection null marker mismatch");
  Require(Contains(vector_artifacts.envelope.payload, "\"source_relation_required\":false"),
          "SELECT VECTOR scalar projection claimed a source relation");
  Require(Contains(vector_artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SELECT VECTOR scalar projection claimed row storage");
  Require(!Contains(vector_artifacts.envelope.payload, "SELECT VECTOR"),
          "SELECT VECTOR scalar projection payload embedded source SQL text");
  Require(!Contains(vector_artifacts.envelope.payload, "VECTOR '["),
          "SELECT VECTOR scalar projection payload embedded source literal text");
  Require(!Contains(vector_artifacts.envelope.payload, "\"target_object_uuid\""),
          "SELECT VECTOR scalar projection payload carried a target object UUID");
  Require(!Contains(vector_artifacts.envelope.payload, "\"source_text\""),
          "SELECT VECTOR scalar projection payload embedded source_text");

  const auto callable_vector_literal =
      RunPipeline("SELECT vector_literal(VECTOR '[1,2,3]') AS vector_value");
  Require(callable_vector_literal.bound.bound,
          "vector_literal callable negative route did not reach binder");
  Require(!callable_vector_literal.verifier.admitted,
          "vector_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_vector_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "vector_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_vector_literal.envelope.payload,
                    "\"function_id\":\"vector_literal\""),
          "vector_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_vector_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.vector_literal\""),
          "vector_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_vector_literal.envelope.payload, "SELECT vector_literal"),
          "vector_literal callable negative route embedded source SQL text");

  const auto callable_uint_literal = RunPipeline("SELECT uint_literal(42U) AS uint_value");
  Require(callable_uint_literal.bound.bound,
          "uint_literal callable negative route did not reach binder");
  Require(!callable_uint_literal.verifier.admitted,
          "uint_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_uint_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "uint_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_uint_literal.envelope.payload, "\"function_id\":\"uint_literal\""),
          "uint_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_uint_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.uint_literal\""),
          "uint_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_uint_literal.envelope.payload, "SELECT uint_literal"),
          "uint_literal callable negative route embedded source SQL text");

  const auto callable_int128_literal =
      RunPipeline("SELECT int128_literal(123I128) AS int128_value");
  Require(callable_int128_literal.bound.bound,
          "int128_literal callable negative route did not reach binder");
  Require(!callable_int128_literal.verifier.admitted,
          "int128_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_int128_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "int128_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_int128_literal.envelope.payload,
                    "\"function_id\":\"int128_literal\""),
          "int128_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_int128_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.int128_literal\""),
          "int128_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_int128_literal.envelope.payload, "SELECT int128_literal"),
          "int128_literal callable negative route embedded source SQL text");

  const auto callable_uint128_literal =
      RunPipeline("SELECT uint128_literal(123U128) AS uint128_value");
  Require(callable_uint128_literal.bound.bound,
          "uint128_literal callable negative route did not reach binder");
  Require(!callable_uint128_literal.verifier.admitted,
          "uint128_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_uint128_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "uint128_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_uint128_literal.envelope.payload,
                    "\"function_id\":\"uint128_literal\""),
          "uint128_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_uint128_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.uint128_literal\""),
          "uint128_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_uint128_literal.envelope.payload, "SELECT uint128_literal"),
          "uint128_literal callable negative route embedded source SQL text");

  const auto callable_real128_literal =
      RunPipeline("SELECT real128_literal(1.25R128) AS real128_value");
  Require(callable_real128_literal.bound.bound,
          "real128_literal callable negative route did not reach binder");
  Require(!callable_real128_literal.verifier.admitted,
          "real128_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_real128_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "real128_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_real128_literal.envelope.payload,
                    "\"function_id\":\"real128_literal\""),
          "real128_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_real128_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.real128_literal\""),
          "real128_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_real128_literal.envelope.payload, "SELECT real128_literal"),
          "real128_literal callable negative route embedded source SQL text");

  const auto callable_float_literal = RunPipeline("SELECT float_literal(1e2) AS float_value");
  Require(callable_float_literal.bound.bound,
          "float_literal callable negative route did not reach binder");
  Require(!callable_float_literal.verifier.admitted,
          "float_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_float_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "float_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_float_literal.envelope.payload, "\"function_id\":\"float_literal\""),
          "float_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_float_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.float_literal\""),
          "float_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_float_literal.envelope.payload, "SELECT float_literal"),
          "float_literal callable negative route embedded source SQL text");

  const auto callable_date_literal =
      RunPipeline("SELECT date_literal(DATE '2026-05-14') AS date_value");
  Require(callable_date_literal.bound.bound,
          "date_literal callable negative route did not reach binder");
  Require(!callable_date_literal.verifier.admitted,
          "date_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_date_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "date_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_date_literal.envelope.payload, "\"function_id\":\"date_literal\""),
          "date_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_date_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.date_literal\""),
          "date_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_date_literal.envelope.payload, "SELECT date_literal"),
          "date_literal callable negative route embedded source SQL text");

  const auto callable_time_literal =
      RunPipeline("SELECT time_literal(TIME '14:23:45') AS time_value");
  Require(callable_time_literal.bound.bound,
          "time_literal callable negative route did not reach binder");
  Require(!callable_time_literal.verifier.admitted,
          "time_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_time_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "time_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_time_literal.envelope.payload, "\"function_id\":\"time_literal\""),
          "time_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_time_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.time_literal\""),
          "time_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_time_literal.envelope.payload, "SELECT time_literal"),
          "time_literal callable negative route embedded source SQL text");

  const auto callable_timestamp_literal =
      RunPipeline("SELECT timestamp_literal(TIMESTAMP '2026-05-14T14:23:45') AS timestamp_value");
  Require(callable_timestamp_literal.bound.bound,
          "timestamp_literal callable negative route did not reach binder");
  Require(!callable_timestamp_literal.verifier.admitted,
          "timestamp_literal was admitted as a callable SQL function");
  Require(HasDiagnosticCode(callable_timestamp_literal.envelope.messages,
                            "SBSQL.QUERY.PROJECTION_INVALID"),
          "timestamp_literal callable negative route did not fail as an invalid projection");
  Require(!Contains(callable_timestamp_literal.envelope.payload,
                    "\"function_id\":\"timestamp_literal\""),
          "timestamp_literal callable negative route emitted a function_id payload");
  Require(!Contains(callable_timestamp_literal.envelope.payload,
                    "\"function_id\":\"sb.scalar.timestamp_literal\""),
          "timestamp_literal callable negative route emitted a scalar function payload");
  Require(!Contains(callable_timestamp_literal.envelope.payload, "SELECT timestamp_literal"),
          "timestamp_literal callable negative route embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected scalar projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require engine public ABI dispatch");
  Require(admission.operation_id == "query.evaluate_projection",
          "server admission operation id mismatch");
  Require(admission.operation_family == "sblr.query.relational.v3",
          "server admission operation family mismatch");

  const auto boolean_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{boolean_artifacts.envelope.payload, false});
  for (const auto& diagnostic : boolean_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(boolean_admission.admitted, "server admission rejected SELECT TRUE scalar route");
  Require(boolean_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT TRUE");
  Require(boolean_admission.operation_id == "query.evaluate_projection",
          "SELECT TRUE server admission operation id mismatch");

  const auto decimal_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{decimal_artifacts.envelope.payload, false});
  for (const auto& diagnostic : decimal_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(decimal_admission.admitted, "server admission rejected SELECT decimal scalar route");
  Require(decimal_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT decimal");
  Require(decimal_admission.operation_id == "query.evaluate_projection",
          "SELECT decimal server admission operation id mismatch");

  const auto float_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{float_artifacts.envelope.payload, false});
  for (const auto& diagnostic : float_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(float_admission.admitted, "server admission rejected SELECT float scalar route");
  Require(float_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT float");
  Require(float_admission.operation_id == "query.evaluate_projection",
          "SELECT float server admission operation id mismatch");

  const auto binary_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{binary_artifacts.envelope.payload, false});
  for (const auto& diagnostic : binary_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(binary_admission.admitted, "server admission rejected SELECT binary scalar route");
  Require(binary_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT binary");
  Require(binary_admission.operation_id == "query.evaluate_projection",
          "SELECT binary server admission operation id mismatch");

  const auto uuid_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{uuid_artifacts.envelope.payload, false});
  for (const auto& diagnostic : uuid_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(uuid_admission.admitted, "server admission rejected SELECT UUID scalar route");
  Require(uuid_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT UUID");
  Require(uuid_admission.operation_id == "query.evaluate_projection",
          "SELECT UUID server admission operation id mismatch");

  const auto date_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{date_artifacts.envelope.payload, false});
  for (const auto& diagnostic : date_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(date_admission.admitted, "server admission rejected SELECT DATE scalar route");
  Require(date_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT DATE");
  Require(date_admission.operation_id == "query.evaluate_projection",
          "SELECT DATE server admission operation id mismatch");

  const auto time_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{time_artifacts.envelope.payload, false});
  for (const auto& diagnostic : time_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(time_admission.admitted, "server admission rejected SELECT TIME scalar route");
  Require(time_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT TIME");
  Require(time_admission.operation_id == "query.evaluate_projection",
          "SELECT TIME server admission operation id mismatch");

  const auto timestamp_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{timestamp_artifacts.envelope.payload, false});
  for (const auto& diagnostic : timestamp_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(timestamp_admission.admitted,
          "server admission rejected SELECT TIMESTAMP scalar route");
  Require(timestamp_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SELECT TIMESTAMP");
  Require(timestamp_admission.operation_id == "query.evaluate_projection",
          "SELECT TIMESTAMP server admission operation id mismatch");
}

void RequireFunctionProjectionLowering() {
  RequireFunctionCallGrammarRegistryEvidence();

  const auto artifacts = RunPipeline("SELECT cot(0.7853981633974483) AS cot_value");
  Require(artifacts.bound.bound, "function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted, "function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"scalar_projection\""),
          "function scalar projection marker missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"function\""),
          "function scalar projection expression kind missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_opcode\":\"SBLR_FUNCTION_CALL\""),
          "function scalar projection SBLR function-call opcode missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "function_call row-identifiable payload marker missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.scalar.cot\""),
          "function scalar projection canonical function id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_type\":\"numeric\""),
          "function scalar projection argument descriptor missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"0.7853981633974483\""),
          "function scalar projection argument value missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT cot"),
          "function scalar projection payload embedded source SQL text");

  const auto invalid = RunPipeline("SELECT cot('not_numeric') AS cot_value");
  Require(invalid.bound.bound, "invalid-input function scalar SELECT did not bind");
  Require(invalid.verifier.admitted,
          "invalid-input function scalar SELECT should lower to engine diagnostic route");
  Require(Contains(invalid.envelope.payload, "\"projection_0_arg_0_type\":\"text\""),
          "invalid-input function scalar projection text argument descriptor missing");
  Require(Contains(invalid.envelope.payload, "\"projection_0_arg_0_value\":\"not_numeric\""),
          "invalid-input function scalar projection text argument value missing");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected function scalar projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for function scalar projection");
}

void RequireNumericFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT pi() AS pi_value, cbrt(27) AS cbrt_value, "
      "degrees(3.141592653589793) AS deg_value, radians(180) AS rad_value, "
      "cotd(45) AS cotd_value, div(7, 3) AS div_value, factorial(5) AS fact_value, "
      "gcd(48, 18) AS gcd_value, lcm(6, 8) AS lcm_value, "
      "width_bucket(5, 0, 10, 5) AS bucket_value, bit_count(7) AS bit_count_value");
  Require(artifacts.bound.bound, "numeric function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "numeric function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "numeric function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "numeric function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"11\""),
          "numeric function projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.scalar.pi\""),
          "pi canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"0\""),
          "pi zero-argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.scalar.cbrt\""),
          "cbrt canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_id\":\"sb.scalar.degrees\""),
          "degrees canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_id\":\"sb.scalar.radians\""),
          "radians canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_function_id\":\"sb.scalar.cotd\""),
          "cotd canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_5_function_id\":\"sb.scalar.div\""),
          "div canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_6_function_id\":\"sb.scalar.factorial\""),
          "factorial canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_7_function_id\":\"sb.scalar.gcd\""),
          "gcd canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_8_function_id\":\"sb.scalar.lcm\""),
          "lcm canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_9_function_id\":\"sb.scalar.width_bucket\""),
          "width_bucket canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_9_function_arg_count\":\"4\""),
          "width_bucket argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_10_function_id\":\"sb.scalar.bit_count\""),
          "bit_count canonical id missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT pi"),
          "numeric function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected numeric function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for numeric function projection");
}

void RequireAdditionalNumericFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT abs(-5) AS abs_value, ceil(1.4) AS ceil_value, floor(1.6) AS floor_value, "
      "round(1.5) AS round_value, sqrt(4.0) AS sqrt_value, power(2.0, 10.0) AS power_value, "
      "sin(0.0) AS sin_value, cos(0.0) AS cos_value, tan(0.0) AS tan_value, "
      "exp(0.0) AS exp_value, ln(1.0) AS ln_value, log10(100.0) AS log10_value, "
      "log(2.0, 8.0) AS log_value, trunc(1.7) AS trunc_value, "
      "truncate(1.7) AS truncate_value, mod(10.0, 3.0) AS mod_value, "
      "sign(5.0) AS sign_value, bit_and(240, 15) AS bit_and_value, "
      "bit_or(240, 15) AS bit_or_value, bit_xor(42, 42) AS bit_xor_value, "
      "bit_shift_left(1, 8) AS bit_shift_left_value, "
      "bit_shift_right(256, 8) AS bit_shift_right_value");
  Require(artifacts.bound.bound, "additional numeric function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "additional numeric function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "additional numeric function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "additional numeric function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"22\""),
          "additional numeric function projection count missing");
  for (const auto* function_id : {
           "sb.scalar.abs",
           "sb.scalar.ceil",
           "sb.scalar.floor",
           "sb.scalar.round",
           "sb.scalar.sqrt",
           "sb.scalar.power",
           "sb.scalar.sin",
           "sb.scalar.cos",
           "sb.scalar.tan",
           "sb.scalar.exp",
           "sb.scalar.ln",
           "sb.scalar.log10",
           "sb.scalar.log",
           "sb.scalar.trunc",
           "sb.scalar.truncate",
           "sb.scalar.mod",
           "sb.scalar.sign",
           "sb.scalar.bit_and",
           "sb.scalar.bit_or",
           "sb.scalar.bit_xor",
           "sb.scalar.bit_shift_left",
           "sb.scalar.bit_shift_right",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "additional numeric canonical id missing");
  }
  Require(!Contains(artifacts.envelope.payload, "SELECT abs"),
          "additional numeric function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected additional numeric function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for additional numeric function projection");
}

void RequireQualifiedCanonicalNumericFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT sb.scalar.abs(-5) AS abs_value, sb.scalar.sqrt(4.0) AS sqrt_value, "
      "sb.scalar.floor(1.6) AS floor_value, sb.scalar.power(2.0, 10.0) AS power_value, "
      "sb.scalar.random() AS random_value");
  Require(artifacts.bound.bound, "qualified canonical numeric scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "qualified canonical numeric scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "qualified canonical numeric scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "qualified canonical numeric scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"5\""),
          "qualified canonical numeric projection count missing");
  for (const auto* function_id : {
           "sb.scalar.abs",
           "sb.scalar.sqrt",
           "sb.scalar.floor",
           "sb.scalar.power",
           "sb.scalar.random",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "qualified canonical numeric function id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_type\":\"bigint\""),
          "qualified canonical abs argument descriptor missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"-5\""),
          "qualified canonical abs argument value missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_arg_count\":\"2\""),
          "qualified canonical power argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_function_arg_count\":\"0\""),
          "qualified canonical random zero-argument count missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT sb.scalar"),
          "qualified canonical numeric function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted,
          "server admission rejected qualified canonical numeric function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for qualified canonical numeric projection");
}

void RequireExtendedNumericFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT asin(0.5) AS asin_value, acos(0.5) AS acos_value, atan(1.0) AS atan_value, "
      "sinh(0.0) AS sinh_value, cosh(0.0) AS cosh_value, tanh(0.0) AS tanh_value, "
      "asinh(1.0) AS asinh_value, acosh(1.0) AS acosh_value, atanh(0.5) AS atanh_value, "
      "log2(8.0) AS log2_value, atan2(1.0, 1.0) AS atan2_value, "
      "sind(30.0) AS sind_value, cosd(60.0) AS cosd_value, tand(45.0) AS tand_value, "
      "asind(0.5) AS asind_value, acosd(0.5) AS acosd_value, atand(1.0) AS atand_value");
  Require(artifacts.bound.bound, "extended numeric function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "extended numeric function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "extended numeric function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "extended numeric function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"17\""),
          "extended numeric function projection count missing");
  for (const auto* function_id : {
           "sb.scalar.asin",
           "sb.scalar.acos",
           "sb.scalar.atan",
           "sb.scalar.sinh",
           "sb.scalar.cosh",
           "sb.scalar.tanh",
           "sb.scalar.asinh",
           "sb.scalar.acosh",
           "sb.scalar.atanh",
           "sb.scalar.log2",
           "sb.scalar.atan2",
           "sb.scalar.sind",
           "sb.scalar.cosd",
           "sb.scalar.tand",
           "sb.scalar.asind",
           "sb.scalar.acosd",
           "sb.scalar.atand",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "extended numeric canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_10_function_arg_count\":\"2\""),
          "atan2 argument count missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT asin"),
          "extended numeric function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected extended numeric function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for extended numeric function projection");
}

void RequireTextJsonFuzzyFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"SQL(SELECT char_length('surface') AS char_len, left('abcdef', 2) AS left_value, right('abcdef', 2) AS right_value, uuid_from_string('550E8400-E29B-41D4-A716-446655440000') AS uuid_value, uuid_to_string('550E8400-E29B-41D4-A716-446655440000') AS uuid_text, digest('hello', 'fnv64') AS digest_value, jsonb_array_length('[1,2,3]') AS jsonb_len, json_set('{"a":1}', '$.b', 2) AS json_set_value, json_remove('{"a":1,"b":2}', '$.a') AS json_removed, json_replace('{"a":1}', '$.a', 4) AS json_replaced, json_insert('{"a":1}', '$.b', 2) AS json_inserted, json_build_array(1, 'bird', NULL) AS json_array, json_build_object('a', 1, 'b', 'bird') AS json_object, to_json('bird') AS json_text, to_jsonb(7) AS jsonb_text, jsonb_typeof('{"a":1}') AS jsonb_type, json_typeof('[1,2]') AS json_type, json_extract('{"a":42}', '$.a') AS json_extracted, json_exists('{"a":1}', '$.a') AS json_exists_value, json_value('{"a":"bird"}', '$.a') AS json_value_value, json_query('{"a":[1,2]}', '$.a') AS json_query_value, jsonb_set('{"a":1}', '$.a', 5) AS jsonb_set_value, jsonb_build_array(1, 'bird', NULL) AS jsonb_array, jsonb_build_object('a', 1) AS jsonb_object, metaphone('Smith') AS metaphone_value, dmetaphone('gumbo') AS dmetaphone_value, dmetaphone_alt('Smith') AS dmetaphone_alt_value, levenshtein_le('kitten', 'sitten', 1) AS levenshtein_le_value, damerau_levenshtein('CA', 'AC') AS damerau_value, jaro_similarity('MARTHA', 'MARHTA') AS jaro_value, jaro_winkler_similarity('MARTHA', 'MARHTA') AS jaro_winkler_value, similarity('hello', 'hello') AS similarity_value, word_similarity('hello', 'say hello today') AS word_similarity_value)SQL");
  Require(artifacts.bound.bound, "text/json/fuzzy function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "text/json/fuzzy function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "text/json/fuzzy function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "text/json/fuzzy function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"33\""),
          "text/json/fuzzy function projection count missing");
  for (const auto* function_id : {
           "sb.scalar.char_length",
           "sb.scalar.left",
           "sb.scalar.right",
           "sb.scalar.uuid_from_string",
           "sb.scalar.uuid_to_string",
           "sb.scalar.digest",
           "sb.json.jsonb_array_length",
           "sb.json.set",
           "sb.json.remove",
           "sb.json.replace",
           "sb.json.insert",
           "sb.json.build_array",
           "sb.json.build_object",
           "sb.json.to_json",
           "sb.json.to_jsonb",
           "sb.json.jsonb_typeof",
           "sb.json.typeof",
           "sb.json.extract",
           "sb.json.exists",
           "sb.json.value",
           "sb.json.query",
           "sb.json.jsonb_set",
           "sb.json.jsonb_build_array",
           "sb.json.jsonb_build_object",
           "sb.scalar.metaphone",
           "sb.scalar.dmetaphone",
           "sb.scalar.dmetaphone_alt",
           "sb.scalar.levenshtein_le",
           "sb.scalar.damerau_levenshtein",
           "sb.scalar.jaro_similarity",
           "sb.scalar.jaro_winkler_similarity",
           "sb.scalar.similarity",
           "sb.scalar.word_similarity",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "text/json/fuzzy canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.scalar.left\""),
          "left(text,n) signature-backed route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_id\":\"sb.scalar.right\""),
          "right(text,n) signature-backed route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_11_arg_2_is_null\":\"true\""),
          "json_build_array NULL argument marker missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT char_length"),
          "text/json/fuzzy function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected text/json/fuzzy function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for text/json/fuzzy function projection");
}

void RequireVectorFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"SQL(SELECT vector(1, 2.5, -3) AS vector_value, vector_dims('[1, 2, 3]') AS vector_dims_value, vector_norm('[3,4]') AS vector_norm_value, l2_distance('[1,2]', '[4,6]') AS l2_distance_value, cosine_distance('[1,0]', '[0,1]') AS cosine_distance_value, inner_product('[1,2,3]', '[4,5,6]') AS inner_product_value, negative_inner_product('[1,2,3]', '[4,5,6]') AS negative_inner_product_value, hamming_distance('10101', '10011') AS hamming_distance_value, vector_l2_normalize('[3,4]') AS normalized_vector, subvector('[9,8,7,6]', 2, 2) AS subvector_value, vector_cast_int8('[-129.2,-1.5,0.49,2.5,300]') AS int8_vector_value, vector_cast_float16('[1,0.3333,65504]') AS float16_vector_value, vector_sum('[1,2,3]', '[4,5,6]') AS vector_sum_value, vector_avg('[1,2,3]', '[4,5,6]') AS vector_avg_value)SQL");
  Require(artifacts.bound.bound, "vector function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "vector function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "vector function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "vector function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"14\""),
          "vector function projection count missing");
  for (const auto* function_id : {
           "sb.vector.vector",
           "sb.vector.vector_dims",
           "sb.vector.vector_norm",
           "sb.vector.vector_sum",
           "sb.vector.vector_avg",
           "sb.vector.l2_distance",
           "sb.vector.cosine_distance",
           "sb.vector.inner_product",
           "sb.vector.negative_inner_product",
           "sb.vector.hamming_distance",
           "sb.vector.vector_l2_normalize",
           "sb.vector.subvector",
           "sb.vector.vector_cast_int8",
           "sb.vector.vector_cast_float16",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "vector canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"3\""),
          "vector constructor scalar argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_9_function_arg_count\":\"3\""),
          "subvector argument count missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT vector"),
          "vector function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected vector function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for vector function projection");
}

void RequireBinaryCryptoFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"SQL(SELECT encode(X'00ff10', 'hex') AS encoded_hex, decode('00ff10', 'hex') AS decoded_hex, decode(2, 1, 'one', 2, 'two', 'fallback') AS oracle_decode_value, md5('hello') AS md5_value, sha1('hello') AS sha1_value, sha224('hello') AS sha224_value, sha256('hello') AS sha256_value, sha384('hello') AS sha384_value, sha512('hello') AS sha512_value, random() AS random_value, current_setting('timezone') AS timezone_value, position(B'11110000' IN B'0000111111110000') AS bit_pos, substring(B'0000111111110000' FROM 4 FOR 8) AS bit_slice)SQL");
  Require(artifacts.bound.bound, "binary/crypto scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "binary/crypto scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "binary/crypto scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "binary/crypto scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"13\""),
          "binary/crypto function projection count missing");
  for (const auto* function_id : {
           "sb.scalar.encode",
           "sb.scalar.decode",
           "sb.scalar.oracle_decode",
           "sb.scalar.md5",
           "sb.scalar.sha1",
           "sb.scalar.sha224",
           "sb.scalar.sha256",
           "sb.scalar.sha384",
           "sb.scalar.sha512",
           "sb.scalar.random",
           "sb.scalar.current_setting_timezone",
           "sb.scalar.position",
           "sb.special.substring_keyword",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "binary/crypto canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_type\":\"binary\""),
          "binary literal argument type missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_arg_count\":\"6\""),
          "Oracle-style decode argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_10_function_arg_count\":\"0\""),
          "current_setting('timezone') special row should lower as nullary canonical function");
  Require(Contains(artifacts.envelope.payload, "\"projection_11_arg_0_type\":\"bit_string\"") &&
              Contains(artifacts.envelope.payload, "\"projection_11_arg_1_type\":\"bit_string\""),
          "bit-string position argument descriptors missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_12_type\":\"bit_string\"") &&
              Contains(artifacts.envelope.payload, "\"projection_12_arg_0_type\":\"bit_string\""),
          "bit-string substring result/input descriptors missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_12_function_id\":\"sb.special.substring_keyword\""),
          "bit-string SUBSTRING keyword special-form id missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT encode"),
          "binary/crypto function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected binary/crypto function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for binary/crypto function projection");
}

void RequireTemporalSessionProviderProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT statement_timestamp AS stmt_ts, transaction_timestamp AS tx_ts, "
      "clock_timestamp() AS clock_ts, timeofday() AS timeofday_value, "
      "localtime AS localtime_value, localtimestamp AS localtimestamp_value, "
      "current_timestamp AS current_ts, current_date AS current_date_value, "
      "current_time AS current_time_value, now() AS now_value, "
      "uuid_v1() AS uuid1, uuid_v4() AS uuid4, uuid_v7() AS uuid7, "
      "sb.uuid.v1() AS uuid1_canonical, sb.uuid.v4() AS uuid4_canonical, "
      "sb.uuid.v7() AS uuid7_canonical");
  Require(artifacts.bound.bound, "temporal/session provider scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "temporal/session provider scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "temporal/session provider scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "temporal/session provider scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"16\""),
          "temporal/session provider projection count missing");
  for (const auto* function_id : {
           "sb.temporal.statement_timestamp",
           "sb.temporal.transaction_timestamp",
           "sb.temporal.clock_timestamp",
           "sb.temporal.timeofday",
           "sb.temporal.localtime",
           "sb.temporal.localtimestamp",
           "sb.temporal.current_timestamp",
           "sb.temporal.current_date",
           "sb.temporal.current_time",
           "sb.temporal.now",
           "sb.uuid.v1",
           "sb.uuid.v4",
           "sb.uuid.v7",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "temporal/session provider canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"0\""),
          "bare statement_timestamp zero-argument route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_function_id\":\"sb.temporal.localtime\""),
          "bare localtime keyword route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_7_function_id\":\"sb.temporal.current_date\""),
          "bare current_date keyword route missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT statement_timestamp"),
          "temporal/session provider payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected temporal/session provider route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for temporal/session provider route");
}

void RequireTemporalConstructorProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT make_date(2026, 5, 11) AS made_date, "
      "make_time(14, 23, 45) AS made_time, "
      "make_timestamp('2026-05-11', '14:23:45') AS made_timestamp, "
      "make_timestamp(2026, 5, 11, 14, 23, 45) AS made_timestamp_6, "
      "make_timestamptz('2026-05-11', '14:23:45') AS made_timestamptz, "
      "make_timestamptz('2026-05-11', '14:23:45', '+05:30') AS made_timestamptz_offset");
  Require(artifacts.bound.bound, "temporal constructor scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "temporal constructor scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "temporal constructor scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "temporal constructor scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"6\""),
          "temporal constructor projection count missing");
  for (const auto* function_id : {
           "sb.temporal.make_date",
           "sb.temporal.make_time",
           "sb.temporal.make_timestamp",
           "sb.temporal.make_timestamptz",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "temporal constructor canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_arg_count\":\"6\""),
          "six-int make_timestamp constructor argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_5_arg_2_value\":\"+05:30\""),
          "make_timestamptz explicit timezone argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT make_date"),
          "temporal constructor payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected temporal constructor projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for temporal constructor projection route");
}

void RequireTemporalFieldArithmeticProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT dow('2026-05-11') AS dow_value, doy('2026-05-11') AS doy_value, "
      "quarter('2026-05-11') AS quarter_value, isodow('2026-05-11') AS isodow_value, "
      "week('2026-05-11') AS week_value, "
      "add_months('2026-05-11', 3) AS add_months_value, "
      "add_months('2026-01-31', 1) AS add_months_clamp, "
      "last_day('2026-05-11') AS last_day_value, "
      "last_day('2024-02-15') AS last_day_leap");
  Require(artifacts.bound.bound, "temporal field/arithmetic scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "temporal field/arithmetic scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "temporal field/arithmetic scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "temporal field/arithmetic scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"9\""),
          "temporal field/arithmetic projection count missing");
  for (const auto* function_id : {
           "sb.temporal.dow",
           "sb.temporal.doy",
           "sb.temporal.quarter",
           "sb.temporal.isodow",
           "sb.temporal.week",
           "sb.temporal.add_months",
           "sb.temporal.last_day",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "temporal field/arithmetic canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_5_function_arg_count\":\"2\""),
          "add_months argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_6_arg_0_value\":\"2026-01-31\""),
          "add_months clamp input missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_8_arg_0_value\":\"2024-02-15\""),
          "last_day leap-year input missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT dow"),
          "temporal field/arithmetic payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected temporal field/arithmetic projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for temporal field/arithmetic route");
}

void RequireTemporalDateTimeBatchProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT age('2026-05-12T01:02:03', '2026-05-10T00:00:00') AS age_value, "
      "age_in_days('2026-05-12', '2026-05-10') AS age_days, "
      "age_in_months('2026-04-01', '2026-01-01') AS age_months, "
      "age_in_years('2026-01-01', '2024-01-01') AS age_years, "
      "date_add('2026-05-10T00:00:00', 'P1DT2H') AS date_add_value, "
      "date_sub('2026-05-10T00:00:00', 'P2D') AS date_sub_value, "
      "date_diff('day', '2026-05-10', '2026-05-15') AS date_diff_value, "
      "date_bin('PT15M', '2026-05-10T10:07:00', '2026-05-10T10:00:00') AS date_bin_value, "
      "make_interval(1, 2, 0, 3, 4, 5, 6) AS interval_value, "
      "timezone('UTC', '2026-05-10T10:00:00') AS timezone_value");
  Require(artifacts.bound.bound, "temporal date/time batch scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "temporal date/time batch scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "temporal date/time batch scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "temporal date/time batch scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"10\""),
          "temporal date/time batch projection count missing");
  for (const auto* function_id : {
           "sb.temporal.age",
           "sb.temporal.age_in_days",
           "sb.temporal.age_in_months",
           "sb.temporal.age_in_years",
           "sb.temporal.date_add",
           "sb.temporal.date_sub",
           "sb.temporal.date_diff",
           "sb.temporal.date_bin",
           "sb.temporal.make_interval",
           "sb.temporal.timezone",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "temporal date/time batch canonical id missing");
  }
  for (const auto* surface_id : {
           "SBSQL-2D1538908FB4",
           "SBSQL-190A9409E3F9",
           "SBSQL-6A44A39395D3",
           "SBSQL-89C364139695",
           "SBSQL-352D7A25CBF2",
           "SBSQL-20441CF0D96A",
           "SBSQL-83DF52C71DB1",
           "SBSQL-3DF9A31D7101",
           "SBSQL-5ECBB4B91523",
           "SBSQL-94D34F1E05AF",
           "SBSQL-B18EB4D81617",
           "SBSQL-FB4C56854614",
           "SBSQL-ED8C540CF5B1",
           "SBSQL-E4A673B0FADE",
           "SBSQL-BF05630BC377",
           "SBSQL-6421F1CDC60B",
           "SBSQL-2B4C5FFFF451",
       }) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            "temporal date/time batch surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_8_function_arg_count\":\"7\""),
          "make_interval seven-argument route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_9_arg_0_value\":\"UTC\""),
          "timezone UTC argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT age"),
          "temporal date/time batch payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected temporal date/time batch projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for temporal date/time batch route");
}

void RequireProceduralContextProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT session_id() AS session_id_value, transaction_id() AS transaction_id_value, "
      "transaction_uuid() AS transaction_uuid_value, current_user AS current_user_value, "
      "session_user AS session_user_value, system_user AS system_user_value, "
      "user AS user_value, current_catalog AS current_catalog_value, "
      "current_schema AS current_schema_value, current_database AS current_database_value, "
      "current_role AS current_role_value, current_server AS current_server_value, "
      "server_version() AS server_version_value, server_version_num() AS server_version_num_value, "
      "current_setting('timezone') AS timezone_value, "
      "array_max_dimension() AS array_rank_limit, array_max_element_count() AS array_element_limit, "
      "case_when_max_branches() AS case_branch_limit, "
      "cte_max_count_per_statement() AS cte_count_limit, "
      "nested_subquery_max_depth() AS subquery_depth_limit, "
      "recursive_cte_max_depth() AS recursive_cte_depth_limit, "
      "result_set_max_columns() AS result_column_limit, union_max_arms() AS union_arm_limit, "
      "default_charset() AS default_charset_value, "
      "default_collation() AS default_collation_value, "
      "comparison_collation_resolution() AS comparison_collation_rule, "
      "keyword_case_rule() AS keyword_case_rule_value, "
      "quoted_identifier_case_rule() AS quoted_identifier_case_rule_value, "
      "unquoted_identifier_case_rule() AS unquoted_identifier_case_rule_value, "
      "unicode_root() AS unicode_root_value, "
      "current_session_id() AS current_session_id_alias, "
      "current_session_uuid() AS current_session_uuid_alias, "
      "current_transaction_id() AS current_transaction_id_alias, "
      "current_statement_uuid() AS current_statement_uuid_value, "
      "current_timezone() AS current_timezone_alias, "
      "row_count() AS row_count_value, "
      "application_name AS application_name_value, "
      "mga_isolation_profile AS mga_isolation_profile_value");
  Require(artifacts.bound.bound, "procedural/context scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "procedural/context scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "procedural/context scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "procedural/context scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"38\""),
          "procedural/context projection count missing");
  for (const auto* function_id : {
           "sb.session.session_id",
           "sb.session.transaction_id",
           "sb.session.transaction_uuid",
           "sb.session.current_user",
           "sb.session.session_user",
           "sb.session.system_user",
           "sb.session.user",
           "sb.session.current_catalog",
           "sb.session.current_schema",
           "sb.session.current_database",
           "sb.session.current_role",
           "sb.session.current_server",
           "sb.scalar.server_version",
           "sb.scalar.server_version_num",
           "sb.scalar.application_name",
           "sb.scalar.current_setting_timezone",
           "sb.scalar.array_max_dimension",
           "sb.scalar.array_max_element_count",
           "sb.scalar.case_when_max_branches",
           "sb.scalar.cte_max_count_per_statement",
           "sb.scalar.nested_subquery_max_depth",
           "sb.scalar.recursive_cte_max_depth",
           "sb.scalar.result_set_max_columns",
           "sb.scalar.union_max_arms",
           "sb.scalar.default_charset",
           "sb.scalar.default_collation",
           "sb.scalar.comparison_collation_resolution",
           "sb.scalar.keyword_case_rule",
           "sb.scalar.quoted_identifier_case_rule",
           "sb.scalar.unquoted_identifier_case_rule",
           "sb.scalar.unicode_root",
           "sb.session.current_statement_uuid",
           "sb.fn.diagnostic.row_count",
           "sb.scalar.mga_isolation_profile",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "procedural/context canonical id missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_arg_count\":\"0\""),
          "bare current_user zero-argument route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_14_function_arg_count\":\"0\""),
          "current_setting('timezone') nullary special route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_23_function_arg_count\":\"0\""),
          "default_charset zero-argument route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_29_function_arg_count\":\"0\""),
          "unicode_root zero-argument route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_30_name\":\"current_session_id_alias\""),
          "current_session_id alias projection missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_31_name\":\"current_session_uuid_alias\""),
          "current_session_uuid alias projection missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_32_name\":\"current_transaction_id_alias\""),
          "current_transaction_id alias projection missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_33_function_id\":\"sb.session.current_statement_uuid\""),
          "current_statement_uuid context route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_34_function_id\":\"sb.scalar.current_setting_timezone\""),
          "current_timezone fixed-policy alias route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_35_function_id\":\"sb.fn.diagnostic.row_count\""),
          "row_count diagnostic context route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_35_function_arg_count\":\"0\""),
          "row_count diagnostic context route must be nullary");
  Require(Contains(artifacts.envelope.payload, "\"projection_36_function_id\":\"sb.scalar.application_name\""),
          "application_name context route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_37_function_id\":\"sb.scalar.mga_isolation_profile\""),
          "mga_isolation_profile context route missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT session_id"),
          "procedural/context payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected procedural/context provider route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for procedural/context route");
}

void RequireSbsfc016DonorSystemVariableProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT @@ROWCOUNT AS rowcount_value, @@SPID AS spid_value, "
      "@@VERSION AS version_value, @@global.version AS global_version_value, "
      "@@session.version AS session_version_value, "
      "@@session.time_zone AS session_timezone_value, @@time_zone AS timezone_value, "
      "@@transaction_isolation AS transaction_isolation_value, "
      "@@tx_isolation AS tx_isolation_value");
  Require(artifacts.bound.bound, "SBSFC-016 donor system variable SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-016 donor system variable SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-016 donor system variable operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-016 donor system variable opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"9\""),
          "SBSFC-016 donor system variable projection count missing");
  for (const auto* function_id : {
           "sb.fn.diagnostic.row_count",
           "sb.session.session_id",
           "sb.scalar.current_engine_version",
           "sb.scalar.current_setting_timezone",
           "sb.scalar.current_isolation_level",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "SBSFC-016 donor system variable canonical id missing");
  }
  for (const auto* surface_id : {
           "SBSQL-DE3C8D86F7F4",
           "SBSQL-463DCD391130",
           "SBSQL-C9CD649263EC",
           "SBSQL-35078B674F78",
           "SBSQL-4BA3FC03F99E",
           "SBSQL-F000B704E26B",
           "SBSQL-57121B14D3D2",
           "SBSQL-F06055E58BA0",
           "SBSQL-4798C99894E7",
           "SBSQL-61F9B45870E1",
           "SBSQL-7DA173DB6A22",
           "SBSQL-FD4E4EFCCC17",
           "SBSQL-B9D595C41C23",
           "SBSQL-07BDB01F1458",
       }) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            "SBSFC-016 donor system variable surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"0\""),
          "SBSFC-016 @@ROWCOUNT route must be nullary");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_8_function_id\":\"sb.scalar.current_isolation_level\""),
          "SBSFC-016 @@tx_isolation alias route missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT @@ROWCOUNT"),
          "SBSFC-016 donor system variable payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-016 donor system variable route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for donor system variable route");

  const sblr::SblrDispatchRequest request{
      EngineContext(), EngineEnvelopeFromParserEnvelope(artifacts.envelope),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR donor system variable envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept donor system variable route");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route donor system variable projection");
  Require(result.api_result.ok, "engine donor system variable scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine donor system variable projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 9,
          "engine donor system variable projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine donor system variable scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "rowcount_value", "uint64", "7");
  expect_text(1, "spid_value", "uuid", "019f0000-0000-7000-8000-000000003122");
  expect_text(2, "version_value", "character", "ScratchBird 0.1.0");
  expect_text(3, "global_version_value", "character", "ScratchBird 0.1.0");
  expect_text(4, "session_version_value", "character", "ScratchBird 0.1.0");
  expect_text(5, "session_timezone_value", "character", "UTC");
  expect_text(6, "timezone_value", "character", "UTC");
  expect_text(7, "transaction_isolation_value", "character", "snapshot");
  expect_text(8, "tx_isolation_value", "character", "snapshot");
}

void RequireSbsfc016DonorContextProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT RDB$GET_CONTEXT('SYSTEM','CURRENT_USER') AS rdb_current_user, "
      "RDB$GET_CONTEXT('SYSTEM','ENGINE_VERSION') AS rdb_engine_version, "
      "RDB$GET_CONTEXT('SYSTEM','TRANSACTION_ID') AS rdb_transaction_id, "
      "SYSTEM_VAR('timezone') AS system_var_timezone, "
      "SYS_CONTEXT('USERENV','CURRENT_USER') AS sys_current_user, "
      "SYS_CONTEXT('USERENV','SESSIONID') AS sys_sessionid, "
      "SYS_CONTEXT('USERENV','SESSION_USER') AS sys_session_user");
  Require(artifacts.bound.bound, "SBSFC-016 donor context SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-016 donor context SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-016 donor context operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-016 donor context opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"7\""),
          "SBSFC-016 donor context projection count missing");
  for (const auto* function_id : {
           "sb.session.current_user",
           "sb.scalar.current_engine_version",
           "sb.session.transaction_id",
           "sb.scalar.current_setting",
           "sb.session.session_id",
           "sb.session.session_user",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "SBSFC-016 donor context canonical id missing");
  }
  for (const auto* surface_id : {
           "SBSQL-11D5ED7A686F",
           "SBSQL-594209C32142",
           "SBSQL-C25F2B374483",
           "SBSQL-6BD2088A414A",
           "SBSQL-B0DCA7477008",
           "SBSQL-B8F2EF583846",
           "SBSQL-20BB356E693A",
           "SBSQL-8395558E18E8",
           "SBSQL-7FF120A84845",
           "SBSQL-FD4E4EFCCC17",
       }) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            "SBSFC-016 donor context surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_arg_count\":\"1\""),
          "SBSFC-016 SYSTEM_VAR recognized setting argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_arg_0_value\":\"timezone\""),
          "SBSFC-016 SYSTEM_VAR recognized timezone argument missing");
  Require(!Contains(artifacts.envelope.payload, "RDB$GET_CONTEXT"),
          "SBSFC-016 donor context payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-016 donor context route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for donor context route");

  const sblr::SblrDispatchRequest request{
      EngineContext(), EngineEnvelopeFromParserEnvelope(artifacts.envelope),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR donor context envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept donor context route");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route donor context projection");
  Require(result.api_result.ok, "engine donor context scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine donor context projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 7,
          "engine donor context projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine donor context scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "rdb_current_user", "uuid", "019f0000-0000-7000-8000-000000003123");
  expect_text(1, "rdb_engine_version", "character", "ScratchBird 0.1.0");
  expect_text(2, "rdb_transaction_id", "uint64", "42");
  expect_text(3, "system_var_timezone", "character", "UTC");
  expect_text(4, "sys_current_user", "uuid", "019f0000-0000-7000-8000-000000003123");
  expect_text(5, "sys_sessionid", "uuid", "019f0000-0000-7000-8000-000000003122");
  expect_text(6, "sys_session_user", "uuid", "019f0000-0000-7000-8000-000000003123");
}

void RequireSbsfc016PolicyRefusalProjectionLowering() {
  std::string sql = "SELECT ";
  const std::size_t row_count =
      sizeof(kSbsfc016PolicyRefusalRows) / sizeof(kSbsfc016PolicyRefusalRows[0]);
  for (std::size_t index = 0; index < row_count; ++index) {
    if (index != 0) sql += ", ";
    sql += kSbsfc016PolicyRefusalRows[index].sql_expression;
    sql += " AS \"";
    sql += kSbsfc016PolicyRefusalRows[index].surface_id;
    sql += "\"";
  }

  const auto artifacts = RunPipeline(sql);
  Require(artifacts.bound.bound, "SBSFC-016 policy-refusal SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-016 policy-refusal SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-016 policy-refusal operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-016 policy-refusal opcode mismatch");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_count\":\"" + std::to_string(row_count) + "\""),
          "SBSFC-016 policy-refusal projection count mismatch");
  for (std::size_t index = 0; index < row_count; ++index) {
    const auto& expected = kSbsfc016PolicyRefusalRows[index];
    const std::string projection_prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload,
                     projection_prefix + "_function_id\":\"" + expected.function_id + "\""),
            std::string(expected.surface_id) + " policy-refusal function id missing");
    Require(Contains(artifacts.envelope.payload, expected.surface_id),
            std::string(expected.surface_id) + " policy-refusal surface evidence missing");
  }
  for (const auto* generic_surface_id : {
           "SBSQL-61F9B45870E1",
           "SBSQL-7DA173DB6A22",
           "SBSQL-FD4E4EFCCC17",
           "SBSQL-8395558E18E8",
           "SBSQL-17B72695FA1A",
       }) {
    Require(Contains(artifacts.envelope.payload, generic_surface_id),
            "SBSFC-016 policy-refusal generic grammar surface evidence missing");
  }
  Require(!Contains(artifacts.envelope.payload, "SELECT @@autocommit"),
          "SBSFC-016 policy-refusal payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "/tmp/sb_refusal"),
          "SBSFC-016 policy-refusal payload leaked filesystem argument text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-016 policy-refusal route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for policy-refusal route");
}

void RequireSbsfc027PolicyRefusalProjectionLowering() {
  for (const auto& expected : kSbsfc027PolicyRefusalRows) {
    std::string sql = "SELECT ";
    sql += expected.sql_expression;
    sql += " AS \"";
    sql += expected.surface_id;
    sql += "\"";

    const auto artifacts = RunPipeline(sql);
    for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    Require(artifacts.bound.bound,
            std::string(expected.surface_id) + " SBSFC-027 policy-refusal SELECT did not bind");
    for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    Require(artifacts.verifier.admitted,
            std::string(expected.surface_id) + " SBSFC-027 SBLR verifier rejected route");
    Require(artifacts.envelope.operation_id == "query.evaluate_projection",
            std::string(expected.surface_id) + " SBSFC-027 operation id mismatch");
    Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
            std::string(expected.surface_id) + " SBSFC-027 opcode mismatch");
    Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"1\""),
            std::string(expected.surface_id) + " SBSFC-027 projection count mismatch");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"projection_0_function_id\":\"") + expected.function_id + "\""),
            std::string(expected.surface_id) + " SBSFC-027 function id missing");
    Require(Contains(artifacts.envelope.payload, expected.surface_id),
            std::string(expected.surface_id) + " SBSFC-027 surface evidence missing");
    Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
            std::string(expected.surface_id) + " SBSFC-027 function-call grammar evidence missing");
    Require(!Contains(artifacts.envelope.payload, expected.sql_expression),
            std::string(expected.surface_id) + " SBSFC-027 payload embedded source SQL text");

    const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Require(admission.admitted,
            std::string(expected.surface_id) + " server admission rejected SBSFC-027 route");
    Require(admission.requires_public_abi_dispatch,
            std::string(expected.surface_id) + " server admission did not require public ABI");
  }
}

void RequireSbsfc028UuidCompatHelperProjectionLowering() {
  const std::string sql =
      "SELECT uuid_generate_v1() AS \"SBSQL-FC9B0CD1F5DB\", "
      "uuid_generate_v3('6ba7b810-9dad-11d1-80b4-00c04fd430c8', 'www.example.com') "
      "AS \"SBSQL-E49B5155B2B1\", "
      "uuid_generate_v4() AS \"SBSQL-241B0D52C153\", "
      "uuid_generate_v5('6ba7b810-9dad-11d1-80b4-00c04fd430c8', 'www.example.com') "
      "AS \"SBSQL-8BCDDF8247E8\", "
      "uuid_nil() AS \"SBSQL-3DE58F410E3D\", "
      "uuid_version('550e8400-e29b-41d4-a716-446655440000') AS \"SBSQL-F7C73033C9AF\", "
      "uuid_timestamp('019e176c-2968-7abc-8def-0123456789ab') AS \"SBSQL-D8D48231FE55\"";
  const auto artifacts = RunPipeline(sql);
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-028 UUID compatibility helper SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted, "SBSFC-028 UUID compatibility helper verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-028 UUID compatibility helper operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-028 UUID compatibility helper opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"7\""),
          "SBSFC-028 UUID compatibility helper projection count mismatch");

  struct ExpectedProjection {
    const char* surface_id;
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"SBSQL-FC9B0CD1F5DB", "sb.uuid.generate_v1", "uuid", "0"},
      {"SBSQL-E49B5155B2B1", "sb.uuid.generate_v3", "uuid", "2"},
      {"SBSQL-241B0D52C153", "sb.uuid.generate_v4", "uuid", "0"},
      {"SBSQL-8BCDDF8247E8", "sb.uuid.generate_v5", "uuid", "2"},
      {"SBSQL-3DE58F410E3D", "sb.uuid.nil", "uuid", "0"},
      {"SBSQL-F7C73033C9AF", "sb.uuid.version", "int64", "1"},
      {"SBSQL-D8D48231FE55", "sb.uuid.timestamp", "timestamp_tz", "1"},
  };
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_name\":\"" + row.surface_id + "\""),
            std::string(row.surface_id) + " SBSFC-028 row label missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.surface_id) + " SBSFC-028 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.surface_id) + " SBSFC-028 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.surface_id) + " SBSFC-028 function arg count missing");
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            std::string(row.surface_id) + " SBSFC-028 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-2B04304A3564"),
          "SBSFC-028 uuid_version(uuid) signature surface evidence missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-917F20E39F2A"),
          "SBSFC-028 uuid_timestamp bare-name surface evidence missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-8DB105A26016"),
          "SBSFC-028 uuid_generate_v3 surface evidence missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-E01F11B7628F"),
          "SBSFC-028 uuid_generate_v5 surface evidence missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-028 function-call grammar evidence missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT uuid_generate_v1"),
          "SBSFC-028 payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-028 UUID compatibility helper route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-028 UUID compatibility helper admission did not require public ABI");
}

void RequireSbsfc031TxidSurfaceProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT txid_current() AS txid_current_value, "
      "txid_status() AS txid_status_value, "
      "txid_status(31031) AS txid_status_arg_value");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-031 txid surface SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted, "SBSFC-031 txid surface verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-031 txid surface operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-031 txid surface opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"3\""),
          "SBSFC-031 txid surface projection count mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.session.transaction_id\""),
          "SBSFC-031 txid_current canonical transaction_id function id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_type\":\"uint64\""),
          "SBSFC-031 txid_current result descriptor missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-8A70982AB170"),
          "SBSFC-031 txid_current surface evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.session.txid_status\""),
          "SBSFC-031 txid_status canonical function id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_type\":\"character\""),
          "SBSFC-031 txid_status result descriptor missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_arg_count\":\"0\""),
          "SBSFC-031 txid_status no-arg arity missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-F239E35B53FE"),
          "SBSFC-031 txid_status no-arg surface evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_id\":\"sb.session.txid_status\""),
          "SBSFC-031 txid_status(bigint) canonical function id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_arg_0_type\":\"bigint\""),
          "SBSFC-031 txid_status(bigint) argument descriptor missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_arg_0_value\":\"31031\""),
          "SBSFC-031 txid_status(bigint) argument value missing");
  Require(Contains(artifacts.envelope.payload, "SBSQL-2D1AB3761105"),
          "SBSFC-031 txid_status(bigint) surface evidence missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT txid_current"),
          "SBSFC-031 txid surface payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-031 txid route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-031 txid admission did not require public ABI");
}

void RequireSbsfc032ScalarUtilityConversionProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT atan2d(1, 0) AS atan2d_value, "
      "collation_for AS collation_for_bare, collation_for('abc') AS collation_for_text, "
      "descriptor_of AS descriptor_of_bare, descriptor_of(42) AS descriptor_of_value, "
      "pg_typeof AS pg_typeof_bare, pg_typeof(NULL) AS pg_typeof_null, "
      "safe_cast AS safe_cast_bare, safe_cast('123', 'int64') AS safe_cast_value, "
      "try_cast AS try_cast_bare, try_cast('bad', 'int64') AS try_cast_value, "
      "similar_to_escape AS similar_to_escape_bare, "
      "similar_to_escape('a%b') AS similar_to_escape_text, "
      "value_state AS value_state_bare, value_state(42) AS value_state_value");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-032 scalar utility/conversion SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-032 scalar utility/conversion verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-032 scalar utility/conversion operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-032 scalar utility/conversion opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"15\""),
          "SBSFC-032 scalar utility/conversion projection count mismatch");

  struct ExpectedProjection {
    const char* surface_id;
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"SBSQL-23E118659719", "sb.scalar.atan2d", "real64", "2"},
      {"SBSQL-6DFCF52729AD", "sb.scalar.collation_for", "character", "0"},
      {"SBSQL-D989C17E1878", "sb.scalar.collation_for", "character", "1"},
      {"SBSQL-99C52D5953AD", "sb.scalar.descriptor_of", "json_document", "0"},
      {"SBSQL-7A5C9806BEF5", "sb.scalar.descriptor_of", "json_document", "1"},
      {"SBSQL-CC379798CF3D", "sb.scalar.pg_typeof", "character", "0"},
      {"SBSQL-3E3527E30D5F", "sb.scalar.pg_typeof", "character", "1"},
      {"SBSQL-D6FBF57E26FC", "sb.scalar.safe_cast", "character", "0"},
      {"SBSQL-6A962F180717", "sb.scalar.safe_cast", "character", "2"},
      {"SBSQL-78EE8FA84A8F", "sb.scalar.try_cast", "character", "0"},
      {"SBSQL-77A5EAFF0CD5", "sb.scalar.try_cast", "character", "2"},
      {"SBSQL-BC1A67FBC111", "sb.scalar.similar_to_escape", "character", "0"},
      {"SBSQL-254D0D3E1F58", "sb.scalar.similar_to_escape", "character", "1"},
      {"SBSQL-161D7B3339E9", "sb.scalar.value_state", "character", "0"},
      {"SBSQL-A442EACDA177", "sb.scalar.value_state", "character", "1"},
  };
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.surface_id) + " SBSFC-032 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.surface_id) + " SBSFC-032 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.surface_id) + " SBSFC-032 function arg count missing");
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            std::string(row.surface_id) + " SBSFC-032 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-032 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_8_arg_1_value\":\"int64\""),
          "SBSFC-032 safe_cast target descriptor argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_10_arg_0_value\":\"bad\""),
          "SBSFC-032 try_cast input argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT atan2d"),
          "SBSFC-032 scalar utility/conversion payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-032 scalar utility/conversion route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-032 scalar utility/conversion admission did not require public ABI");
}

void RequireSbsfc033CatalogDescriptorDiagnosticProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT catalog_object_owner AS owner_bare, "
      "catalog_object_owner('019f0000-0000-7000-8000-000000003113') AS owner_by_uuid, "
      "catalog_object_uuid AS uuid_bare, "
      "catalog_object_uuid('current_schema', 'schema') AS uuid_by_name, "
      "catalog_object_name AS name_bare, "
      "catalog_object_name('019f0000-0000-7000-8000-000000003126') AS name_by_uuid, "
      "catalog_object_class AS class_bare, "
      "catalog_object_class('019f0000-0000-7000-8000-000000003127') AS class_by_uuid, "
      "descriptor_snapshot_id AS descriptor_snapshot_id_value, "
      "ExecutionTypeDescriptor AS execution_type_descriptor_value, "
      "column_descriptor('019f0000-0000-7000-8000-000000003113', 'database_uuid') "
      "AS column_descriptor_value, "
      "column_descriptor AS column_descriptor_bare, "
      "index_descriptor AS index_descriptor_bare, "
      "index_descriptor('019f0000-0000-7000-8000-000000003399') AS index_descriptor_unknown, "
      "diagnostic_field AS diagnostic_field_bare, "
      "diagnostic_field('sqlstate') AS diagnostic_field_sqlstate, "
      "diagnostic_count AS diagnostic_count_value, "
      "gdscode AS gdscode_value, "
      "last_error_position AS last_error_position_value, "
      "error_class AS error_class_value");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-033 catalog/descriptor/diagnostic SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-033 catalog/descriptor/diagnostic verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-033 catalog/descriptor/diagnostic operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-033 catalog/descriptor/diagnostic opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"20\""),
          "SBSFC-033 catalog/descriptor/diagnostic projection count mismatch");

  struct ExpectedProjection {
    const char* surface_id;
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"SBSQL-06BFD87D6529", "sb.scalar.catalog_object_owner", "uuid", "0"},
      {"SBSQL-D26A0353E396", "sb.scalar.catalog_object_owner", "uuid", "1"},
      {"SBSQL-1C8329808D20", "sb.scalar.catalog_object_uuid", "uuid", "0"},
      {"SBSQL-400622501328", "sb.scalar.catalog_object_uuid", "uuid", "2"},
      {"SBSQL-99BB305208AD", "sb.scalar.catalog_object_name", "character", "0"},
      {"SBSQL-61072961FEDB", "sb.scalar.catalog_object_name", "character", "1"},
      {"SBSQL-E1CB2F1D2656", "sb.scalar.catalog_object_class", "character", "0"},
      {"SBSQL-A9F113392815", "sb.scalar.catalog_object_class", "character", "1"},
      {"SBSQL-352707AC1CAE", "sb.scalar.descriptor_snapshot_id", "uuid", "0"},
      {"SBSQL-BE412B3728C3", "sb.scalar.execution_type_descriptor", "json_document", "0"},
      {"SBSQL-62343F602D38", "sb.scalar.column_descriptor", "json_document", "2"},
      {"SBSQL-67431DA0E42F", "sb.scalar.column_descriptor", "json_document", "0"},
      {"SBSQL-95EA30EEFDEB", "sb.scalar.index_descriptor", "json_document", "0"},
      {"SBSQL-A209C2B5CDDD", "sb.scalar.index_descriptor", "json_document", "1"},
      {"SBSQL-780AF496F174", "sb.scalar.diagnostic_field", "character", "0"},
      {"SBSQL-6A13011127CF", "sb.scalar.diagnostic_field", "character", "1"},
      {"SBSQL-78B6ABBE922C", "sb.scalar.diagnostic_count", "uint64", "0"},
      {"SBSQL-0D860B4A13B7", "sb.scalar.gdscode", "int64", "0"},
      {"SBSQL-E00EAE7EDC3C", "sb.scalar.last_error_position", "int64", "0"},
      {"SBSQL-E36B1B028CC2", "sb.scalar.error_class", "character", "0"},
  };
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.surface_id) + " SBSFC-033 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.surface_id) + " SBSFC-033 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.surface_id) + " SBSFC-033 function arg count missing");
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            std::string(row.surface_id) + " SBSFC-033 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-033 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_arg_0_value\":\"current_schema\""),
          "SBSFC-033 catalog_object_uuid name argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_10_arg_1_value\":\"database_uuid\""),
          "SBSFC-033 column_descriptor column-name argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_15_arg_0_value\":\"sqlstate\""),
          "SBSFC-033 diagnostic_field name argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT catalog_object_owner"),
          "SBSFC-033 catalog/descriptor/diagnostic payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-033 catalog/descriptor/diagnostic route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-033 catalog/descriptor/diagnostic admission did not require public ABI");
}

void RequireSbsfc034TextTrigramBitStringProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT bit_string_position(B'11110000', B'0000111111110000') AS bit_pos, "
      "bit_string_substring(B'0000111111110000', 5, 8) AS bit_slice, "
      "show_trgm AS show_trgm_bare, "
      "show_trgm('cat') AS trigrams, "
      "pg_trgm AS pg_trgm_capability");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-034 text/trigram/bit-string SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-034 text/trigram/bit-string verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-034 text/trigram/bit-string operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-034 text/trigram/bit-string opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"5\""),
          "SBSFC-034 text/trigram/bit-string projection count mismatch");

  struct ExpectedProjection {
    const char* surface_id;
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"SBSQL-D7C03289F1AA", "sb.scalar.bit_string_position", "int64", "2"},
      {"SBSQL-4CE572F88F79", "sb.scalar.bit_string_substring", "bit_string", "3"},
      {"SBSQL-38CE72403078", "sb.scalar.show_trgm", "array", "0"},
      {"SBSQL-A2A770275F65", "sb.scalar.show_trgm", "array", "1"},
      {"SBSQL-F01F3112C706", "sb.scalar.pg_trgm", "json_document", "0"},
  };
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.surface_id) + " SBSFC-034 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.surface_id) + " SBSFC-034 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.surface_id) + " SBSFC-034 function arg count missing");
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            std::string(row.surface_id) + " SBSFC-034 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-034 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_type\":\"bit_string\"") &&
              Contains(artifacts.envelope.payload, "\"projection_0_arg_1_type\":\"bit_string\""),
          "SBSFC-034 bit_string_position argument descriptors missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_type\":\"bit_string\"") &&
              Contains(artifacts.envelope.payload, "\"projection_1_arg_0_type\":\"bit_string\""),
          "SBSFC-034 bit_string_substring descriptors missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_arg_0_value\":\"cat\""),
          "SBSFC-034 show_trgm text argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT bit_string_position"),
          "SBSFC-034 text/trigram/bit-string payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-034 text/trigram/bit-string route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-034 text/trigram/bit-string admission did not require public ABI");
}

void RequireSbsfc035RangeScalarHelperProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT range_contains('[1,5]', '[2,5)') AS range_contains_result, "
      "range_contains_element('[1,5)', 3) AS range_contains_element_result, "
      "range_lower('[1,5)') AS range_lower_result, "
      "range_lower_inc('[1,5)') AS range_lower_inc_result, "
      "range_overlaps('[1,5]', '[5,8)') AS range_overlaps_result, "
      "range_strictly_left('[1,5)', '[5,8)') AS range_left_result, "
      "range_strictly_right('[5,8)', '[1,5)') AS range_right_result, "
      "range_upper('[1,5)') AS range_upper_result, "
      "range_upper_inc('[1,5)') AS range_upper_inc_result");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-035 range scalar helper SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-035 range scalar helper verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-035 range scalar helper operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-035 range scalar helper opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"9\""),
          "SBSFC-035 range scalar helper projection count mismatch");

  struct ExpectedProjection {
    const char* surface_id;
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"SBSQL-DBA02300598B", "sb.scalar.range_contains", "boolean", "2"},
      {"SBSQL-33FC6A422D2A", "sb.scalar.range_contains_element", "boolean", "2"},
      {"SBSQL-E67AFDCD6017", "sb.scalar.range_lower", "character", "1"},
      {"SBSQL-23C8E30D9502", "sb.scalar.range_lower_inc", "boolean", "1"},
      {"SBSQL-68F1659E06B7", "sb.scalar.range_overlaps", "boolean", "2"},
      {"SBSQL-98C59707CA44", "sb.scalar.range_strictly_left", "boolean", "2"},
      {"SBSQL-866D51FDCD73", "sb.scalar.range_strictly_right", "boolean", "2"},
      {"SBSQL-7547BF6B8187", "sb.scalar.range_upper", "character", "1"},
      {"SBSQL-A383B9185803", "sb.scalar.range_upper_inc", "boolean", "1"},
  };
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.surface_id) + " SBSFC-035 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.surface_id) + " SBSFC-035 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.surface_id) + " SBSFC-035 function arg count missing");
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            std::string(row.surface_id) + " SBSFC-035 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-035 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"[1,5]\"") &&
              Contains(artifacts.envelope.payload, "\"projection_0_arg_1_value\":\"[2,5)\""),
          "SBSFC-035 range_contains descriptor arguments missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_arg_1_type\":\"bigint\"") &&
              Contains(artifacts.envelope.payload, "\"projection_1_arg_1_value\":\"3\""),
          "SBSFC-035 range_contains_element element argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_6_function_id\":\"sb.scalar.range_strictly_right\""),
          "SBSFC-035 range_strictly_right route missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT range_contains"),
          "SBSFC-035 range scalar helper payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-035 range scalar helper route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-035 range scalar helper admission did not require public ABI");
}

void RequireSbsfc036SpatialGeometryScalarHelperProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"(SELECT
      st_x('POINT(3 4)') AS st_x_result,
      st_makepoint(3, 4) AS st_makepoint_result,
      st_crosses('LINESTRING(0 0,2 2)', 'LINESTRING(0 2,2 0)') AS st_crosses_result,
      st_simplify('LINESTRING(0 0,1 1)', 0.1) AS st_simplify_result,
      st_geometrytype('POINT(1 2)') AS st_geometrytype_result,
      st_geogfromtext('POINT(1 2)') AS st_geogfromtext_result,
      st_contains('POLYGON((0 0,4 0,4 4,0 4,0 0))', 'POINT(2 2)') AS st_contains_result,
      geom_extent('LINESTRING(0 1,2 3)') AS geom_extent_result,
      st_y('POINT(3 4)') AS st_y_result,
      st_disjoint('POINT(0 0)', 'POINT(5 5)') AS st_disjoint_result,
      st_transform('POINT(1 2)', 4326) AS st_transform_result,
      st_numpoints('LINESTRING(0 0,1 1,2 2)') AS st_numpoints_result,
      st_asbinary('POINT(1 2)') AS st_asbinary_result,
      st_assvg('POINT(1 2)') AS st_assvg_result,
      geom_union('POINT(1 2)') AS geom_union_result,
      st_perimeter('POLYGON((0 0,4 0,4 3,0 3,0 0))') AS st_perimeter_result,
      st_envelope('LINESTRING(0 1,2 3)') AS st_envelope_result,
      st_distance('POINT(0 0)', 'POINT(3 4)') AS st_distance_result,
      st_astext('POINT(1 2)') AS st_astext_result,
      st_touches('POLYGON((0 0,1 0,1 1,0 1,0 0))','POLYGON((1 0,2 0,2 1,1 1,1 0))') AS st_touches_result,
      st_setsrid('POINT(1 2)', 4326) AS st_setsrid_result,
      st_buffer('POINT(1 1)', 2) AS st_buffer_result,
      st_npoints('LINESTRING(0 0,1 1,2 2)') AS st_npoints_result,
      st_overlaps('POLYGON((0 0,2 0,2 2,0 2,0 0))','POLYGON((1 1,3 1,3 3,1 3,1 1))') AS st_overlaps_result,
      st_intersects('POLYGON((0 0,2 0,2 2,0 2,0 0))','POLYGON((1 1,3 1,3 3,1 3,1 1))') AS st_intersects_result,
      st_geomfromwkb('0101000000000000000000F03F0000000000000040') AS st_geomfromwkb_result,
      st_equals('POINT(1 2)','POINT(1 2)') AS st_equals_result,
      st_makepolygon('LINESTRING(0 0,1 0,1 1,0 0)') AS st_makepolygon_result,
      st_srid('SRID=3857;POINT(1 2)') AS st_srid_result,
      st_geomfromgeojson('{"type":"Point","coordinates":[3,4]}') AS st_geomfromgeojson_result,
      st_centroid('POLYGON((0 0,4 0,4 4,0 4,0 0))') AS st_centroid_result,
      st_intersection('POLYGON((0 0,2 0,2 2,0 2,0 0))','POLYGON((1 1,3 1,3 3,1 3,1 1))') AS st_intersection_result,
      st_area('POLYGON((0 0,4 0,4 3,0 3,0 0))') AS st_area_result,
      st_within('POINT(2 2)', 'POLYGON((0 0,4 0,4 4,0 4,0 0))') AS st_within_result,
      geom_collect('POINT(1 2)', 'POINT(3 4)') AS geom_collect_result,
      st_asgeojson('POINT(1 2)') AS st_asgeojson_result)");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-036 spatial geometry scalar helper SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-036 spatial geometry scalar helper verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-036 spatial geometry scalar helper operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-036 spatial geometry scalar helper opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"36\""),
          "SBSFC-036 spatial geometry scalar helper projection count mismatch");

  struct ExpectedProjection {
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"sb.scalar.st_x", "real64", "1"},
      {"sb.scalar.st_makepoint", "geometry", "2"},
      {"sb.scalar.st_crosses", "boolean", "2"},
      {"sb.scalar.st_simplify", "geometry", "2"},
      {"sb.scalar.st_geometrytype_geometry", "character", "1"},
      {"sb.scalar.st_geogfromtext", "geometry", "1"},
      {"sb.scalar.st_contains", "boolean", "2"},
      {"sb.scalar.geom_extent_geometry", "geometry", "1"},
      {"sb.scalar.st_y", "real64", "1"},
      {"sb.scalar.st_disjoint", "boolean", "2"},
      {"sb.scalar.st_transform_geometry_target_srid", "geometry", "2"},
      {"sb.scalar.st_numpoints", "int64", "1"},
      {"sb.scalar.st_asbinary", "bytea", "1"},
      {"sb.scalar.st_assvg", "character", "1"},
      {"sb.scalar.geom_union_geometry", "geometry", "1"},
      {"sb.scalar.st_perimeter", "real64", "1"},
      {"sb.scalar.st_envelope", "geometry", "1"},
      {"sb.scalar.st_distance", "real64", "2"},
      {"sb.scalar.st_astext", "character", "1"},
      {"sb.scalar.st_touches_g1_g2", "boolean", "2"},
      {"sb.scalar.st_setsrid_geometry_srid", "geometry", "2"},
      {"sb.scalar.st_buffer", "geometry", "2"},
      {"sb.scalar.st_npoints", "int64", "1"},
      {"sb.scalar.st_overlaps", "boolean", "2"},
      {"sb.scalar.st_intersects", "boolean", "2"},
      {"sb.scalar.st_geomfromwkb", "geometry", "1"},
      {"sb.scalar.st_equals_g1_g2", "boolean", "2"},
      {"sb.scalar.st_makepolygon_linestring_holesarray", "geometry", "1"},
      {"sb.scalar.st_srid_geometry", "int64", "1"},
      {"sb.scalar.st_geomfromgeojson_text", "geometry", "1"},
      {"sb.scalar.st_centroid", "geometry", "1"},
      {"sb.scalar.st_intersection", "geometry", "2"},
      {"sb.scalar.st_area_geometry", "real64", "1"},
      {"sb.scalar.st_within_g1_g2", "boolean", "2"},
      {"sb.scalar.geom_collect", "geometry", "2"},
      {"sb.scalar.st_asgeojson_geometry_maxdecimaldigits", "json_document", "1"},
  };
  static_assert(sizeof(expected) / sizeof(expected[0]) == 36);
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.function_id) + " SBSFC-036 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.function_id) + " SBSFC-036 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.function_id) + " SBSFC-036 function arg count missing");
  }

  for (const auto* surface_id : {
           "SBSQL-007BD17BDF55", "SBSQL-01C6BF2303B1", "SBSQL-064CC33574E2",
           "SBSQL-14816C2A7E33", "SBSQL-14CD6B2AA8E3", "SBSQL-16705FD6AD8C",
           "SBSQL-1817177FD841", "SBSQL-19B4EE69BD6A", "SBSQL-19C49EFCE56D",
           "SBSQL-20DB96AF4D98", "SBSQL-2ED82920C391", "SBSQL-34B32A5FF887",
           "SBSQL-37FDE5D4CA38", "SBSQL-39EC9401DD7F", "SBSQL-3B96D65453D5",
           "SBSQL-3E576350E9B0", "SBSQL-3F84A2CEBD71", "SBSQL-4016AFBC31B8",
           "SBSQL-48C47B22CD64", "SBSQL-4976BE206EC9", "SBSQL-53EF6CC1B84B",
           "SBSQL-549915041FC5", "SBSQL-56C21F337176", "SBSQL-577953487165",
           "SBSQL-581DB27EE2F3", "SBSQL-5C008F9218F5", "SBSQL-610EB642822F",
           "SBSQL-63555A174F42", "SBSQL-65C7DA8D048B", "SBSQL-6B3AC153575D",
           "SBSQL-6CC46392ADAC", "SBSQL-6E042C3D9DA7", "SBSQL-6EE712BB2CA2",
           "SBSQL-71E8B706D3F5", "SBSQL-7387E0B53393", "SBSQL-774E359ADAF4",
           "SBSQL-779284739DB2", "SBSQL-78846923611D", "SBSQL-7B1A7ED9A65B",
           "SBSQL-7C81986B79D9", "SBSQL-7E7F908D3782", "SBSQL-7F1AA7BC1C1B",
           "SBSQL-81134D15580F", "SBSQL-8126547CB199", "SBSQL-82098A2E3A54",
           "SBSQL-83D324A5BD04", "SBSQL-8A2191CBD1FF", "SBSQL-918FBB8B8F9A",
           "SBSQL-9589706D80E8", "SBSQL-9639CB5F0B9A"}) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string(surface_id) + " SBSFC-036 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-036 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"POINT(3 4)\""),
          "SBSFC-036 st_x geometry argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_arg_0_value\":\"3\"") &&
              Contains(artifacts.envelope.payload, "\"projection_1_arg_1_value\":\"4\""),
          "SBSFC-036 st_makepoint numeric arguments missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_29_arg_0_value\":\"{\\\"type\\\":\\\"Point\\\",\\\"coordinates\\\":[3,4]}\""),
          "SBSFC-036 st_geomfromgeojson JSON argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT st_x"),
          "SBSFC-036 spatial geometry scalar helper payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-036 spatial geometry route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-036 spatial geometry admission did not require public ABI");
}

void RequireSbsfc037XmlMultimodelScalarHelperProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"(SELECT
      XMLFOREST('bird' AS item) AS xmlforest_result,
      XMLCAST('<item>bird</item>' AS xml_document) AS xmlcast_result,
      XMLEXISTS('item', '<item>bird</item>') AS xmlexists_result,
      XMLATTRIBUTES('bird' AS kind) AS xmlattributes_result,
      XMLCONCAT('<a/>','<b/>') AS xmlconcat_result,
      XMLCOMMENT('ok') AS xmlcomment_result,
      XMLPI(NAME stylesheet, 'href=a') AS xmlpi_result,
      XMLROOT('<r/>', VERSION '1.1', STANDALONE yes) AS xmlroot_result,
      XMLELEMENT(NAME item, 'bird') AS xmlelement_result,
      XMLAGG('<a/>','<b/>') AS xmlagg_result,
      XMLTABLE('/item','<item/>') AS xmltable_result)");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-037 XML/multimodel scalar helper SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-037 XML/multimodel scalar helper verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-037 XML/multimodel scalar helper operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-037 XML/multimodel scalar helper opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"11\""),
          "SBSFC-037 XML/multimodel scalar helper projection count mismatch");

  struct ExpectedProjection {
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"sb.xml.forest", "xml_document", "1"},
      {"sb.xml.cast", "xml_document", "2"},
      {"sb.xml.exists", "boolean", "2"},
      {"sb.xml.attributes", "xml", "1"},
      {"sb.xml.concat", "xml_document", "2"},
      {"sb.xml.comment", "xml", "1"},
      {"sb.xml.pi", "xml", "2"},
      {"sb.xml.root", "xml_document", "3"},
      {"sb.xml.element", "xml_document", "2"},
      {"sb.xml.agg", "xml_document", "2"},
      {"sb.xml.table", "json_document", "2"},
  };
  static_assert(sizeof(expected) / sizeof(expected[0]) == 11);
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.function_id) + " SBSFC-037 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.function_id) + " SBSFC-037 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.function_id) + " SBSFC-037 function arg count missing");
  }

  for (const auto* surface_id : {
           "SBSQL-0C16676374C8", "SBSQL-6C89436D2254", "SBSQL-2BBA1DA50B23",
           "SBSQL-0C8A8486F751", "SBSQL-104DD993AED4", "SBSQL-EEA4907830CB",
           "SBSQL-1FD7CBD0921F", "SBSQL-E2022718464C", "SBSQL-934D2E7C0508",
           "SBSQL-2B38A69D425B", "SBSQL-4F494D9A6610", "SBSQL-7881C81BBBE8",
           "SBSQL-DC75730A32EA", "SBSQL-51E09D00A979", "SBSQL-52CC2FA7719D",
           "SBSQL-A31D3F4A9E77", "SBSQL-54EBF8EDE58A", "SBSQL-5702FA6BF536",
           "SBSQL-94785A48EF57", "SBSQL-F0C5F1661298", "SBSQL-796CAD6CD56E"}) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string(surface_id) + " SBSFC-037 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-037 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_name\":\"item\""),
          "SBSFC-037 XMLFOREST AS-name argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_arg_0_name\":\"kind\""),
          "SBSFC-037 XMLATTRIBUTES AS-name argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_6_arg_0_value\":\"stylesheet\"") &&
              Contains(artifacts.envelope.payload, "\"projection_6_arg_1_value\":\"href=a\""),
          "SBSFC-037 XMLPI arguments missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_8_arg_0_value\":\"item\"") &&
              Contains(artifacts.envelope.payload, "\"projection_8_arg_1_value\":\"bird\""),
          "SBSFC-037 XMLELEMENT arguments missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT XMLFOREST"),
          "SBSFC-037 XML/multimodel scalar helper payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-037 XML/multimodel route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-037 XML/multimodel admission did not require public ABI");
}

void RequireSbsfc038SpatialTailScalarHelperProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"(SELECT
      st_setsrid('POINT(1 2)', 4326) AS st_setsrid_result,
      st_dwithin('POINT(0 0)', 'POINT(3 4)', 5) AS st_dwithin_signature_result,
      st_m('POINT M(1 2 7)') AS st_m_result,
      st_overlaps('POLYGON((0 0,2 0,2 2,0 2,0 0))','POLYGON((1 1,3 1,3 3,1 3,1 1))') AS st_overlaps_signature_result,
      st_difference('POINT(1 2)', 'POINT(3 4)') AS st_difference_signature_result,
      st_z('POINT Z(1 2 9)') AS st_z_result,
      st_area('POLYGON((0 0,2 0,2 2,0 2,0 0))') AS st_area_result,
      st_asmvtgeom('POINT(5 5)', '0 0 10 10') AS st_asmvtgeom_result,
      st_difference('POINT(1 2)', 'POINT(3 4)') AS st_difference_result,
      st_length('LINESTRING(0 0,3 4)') AS st_length_result,
      geom_union('POINT(1 2)') AS geom_union_result,
      geom_collect('POINT(1 2)') AS geom_collect_geometry_result,
      st_makepoint(1, 2, 3, 4) AS st_makepoint_xyzm_result,
      st_equals('POINT(1 2)', 'POINT(1 2)') AS st_equals_result,
      st_intersection('POLYGON((0 0,2 0,2 2,0 2,0 0))','POLYGON((1 1,3 1,3 3,1 3,1 1))') AS st_intersection_signature_result,
      st_centroid('POLYGON((0 0,2 0,2 2,0 2,0 0))') AS st_centroid_geometry_result,
      st_geometrytype('POINT(1 2)') AS st_geometrytype_result,
      st_geomfromgeojson('{"type":"Point","coordinates":[3,4]}') AS st_geomfromgeojson_result,
      st_makeline('POINT(0 0)', 'POINT(1 1)') AS st_makeline_signature_result,
      st_geomfromtext('POINT(1 2)', 4326) AS st_geomfromtext_signature_result,
      geom_extent('LINESTRING(0 0,2 3)') AS geom_extent_result,
      st_symdifference('POINT(1 2)', 'POINT(3 4)') AS st_symdifference_signature_result,
      st_asgeojson('POINT(1 2)') AS st_asgeojson_result,
      st_dwithin('POINT(0 0)', 'POINT(3 4)', 5) AS st_dwithin_result,
      st_touches('POLYGON((0 0,1 0,1 1,0 1,0 0))','POLYGON((1 0,2 0,2 1,1 1,1 0))') AS st_touches_result,
      st_transform('POINT(1 2)', 3857) AS st_transform_result,
      st_covers('POLYGON((0 0,4 0,4 4,0 4,0 0))','POINT(2 2)') AS st_covers_signature_result,
      st_srid('SRID=4326;POINT(1 2)') AS st_srid_result,
      st_disjoint('POINT(0 0)', 'POINT(5 5)') AS st_disjoint_signature_result,
      st_convexhull('LINESTRING(0 0,2 3)') AS st_convexhull_geometry_result,
      st_length('LINESTRING(0 0,3 4)') AS st_length_geometry_result,
      st_convexhull('LINESTRING(0 0,2 3)') AS st_convexhull_result,
      st_npoints('LINESTRING(0 0,1 1,2 1)') AS st_npoints_geometry_result,
      st_makeline('[[0,0],[1,1]]') AS st_makeline_result,
      st_makepolygon('LINESTRING(0 0,1 0,1 1,0 0)') AS st_makepolygon_result,
      st_geomfromtext('POINT(1 2)') AS st_geomfromtext_result,
      st_within('POINT(2 2)', 'POLYGON((0 0,4 0,4 4,0 4,0 0))') AS st_within_result,
      st_symdifference('POINT(1 2)', 'POINT(3 4)') AS st_symdifference_result,
      st_covers('POLYGON((0 0,4 0,4 4,0 4,0 0))','POINT(2 2)') AS st_covers_result,
      st_union('POINT(1 2)', 'POINT(3 4)') AS st_union_result,
      st_geogfromtext('POINT(1 2)') AS st_geogfromtext_wkt_result,
      st_union('POINT(1 2)', 'POINT(3 4)') AS st_union_signature_result,
      st_asmvtgeom('POINT(5 5)', '0 0 10 10', 10, 0, TRUE) AS st_asmvtgeom_signature_result)");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-038 spatial tail scalar helper SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-038 spatial tail scalar helper verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-038 spatial tail scalar helper operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-038 spatial tail scalar helper opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"43\""),
          "SBSFC-038 spatial tail scalar helper projection count mismatch");

  struct ExpectedProjection {
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"sb.scalar.st_setsrid_geometry_srid", "geometry", "2"},
      {"sb.scalar.st_dwithin", "boolean", "3"},
      {"sb.scalar.st_m", "real64", "1"},
      {"sb.scalar.st_overlaps", "boolean", "2"},
      {"sb.scalar.st_difference", "geometry", "2"},
      {"sb.scalar.st_z", "real64", "1"},
      {"sb.scalar.st_area_geometry", "real64", "1"},
      {"sb.scalar.st_asmvtgeom", "geometry", "2"},
      {"sb.scalar.st_difference", "geometry", "2"},
      {"sb.scalar.st_length", "real64", "1"},
      {"sb.scalar.geom_union_geometry", "geometry", "1"},
      {"sb.scalar.geom_collect", "geometry", "1"},
      {"sb.scalar.st_makepoint", "geometry", "4"},
      {"sb.scalar.st_equals_g1_g2", "boolean", "2"},
      {"sb.scalar.st_intersection", "geometry", "2"},
      {"sb.scalar.st_centroid", "geometry", "1"},
      {"sb.scalar.st_geometrytype_geometry", "character", "1"},
      {"sb.scalar.st_geomfromgeojson_text", "geometry", "1"},
      {"sb.scalar.st_makeline", "geometry", "2"},
      {"sb.scalar.st_geomfromtext", "geometry", "2"},
      {"sb.scalar.geom_extent_geometry", "geometry", "1"},
      {"sb.scalar.st_symdifference", "geometry", "2"},
      {"sb.scalar.st_asgeojson_geometry_maxdecimaldigits", "json_document", "1"},
      {"sb.scalar.st_dwithin", "boolean", "3"},
      {"sb.scalar.st_touches_g1_g2", "boolean", "2"},
      {"sb.scalar.st_transform_geometry_target_srid", "geometry", "2"},
      {"sb.scalar.st_covers", "boolean", "2"},
      {"sb.scalar.st_srid_geometry", "int64", "1"},
      {"sb.scalar.st_disjoint", "boolean", "2"},
      {"sb.scalar.st_convexhull", "geometry", "1"},
      {"sb.scalar.st_length", "real64", "1"},
      {"sb.scalar.st_convexhull", "geometry", "1"},
      {"sb.scalar.st_npoints", "int64", "1"},
      {"sb.scalar.st_makeline", "geometry", "1"},
      {"sb.scalar.st_makepolygon_linestring_holesarray", "geometry", "1"},
      {"sb.scalar.st_geomfromtext", "geometry", "1"},
      {"sb.scalar.st_within_g1_g2", "boolean", "2"},
      {"sb.scalar.st_symdifference", "geometry", "2"},
      {"sb.scalar.st_covers", "boolean", "2"},
      {"sb.scalar.st_union", "geometry", "2"},
      {"sb.scalar.st_geogfromtext", "geometry", "1"},
      {"sb.scalar.st_union", "geometry", "2"},
      {"sb.scalar.st_asmvtgeom", "geometry", "5"},
  };
  static_assert(sizeof(expected) / sizeof(expected[0]) == 43);
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.function_id) + " SBSFC-038 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.function_id) + " SBSFC-038 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.function_id) + " SBSFC-038 function arg count missing");
  }

  for (const auto* surface_id : {
           "SBSQL-9689873CEFCA", "SBSQL-A01836D957A0", "SBSQL-A0BCD0E4C3DC",
           "SBSQL-A57555BEE95E", "SBSQL-A5BDCC976DD0", "SBSQL-A5D10A16CCFA",
           "SBSQL-A8D99D74565F", "SBSQL-AD4F92702329", "SBSQL-AEFECB9626BB",
           "SBSQL-B1718AA4E4B6", "SBSQL-B26EC3DF7AFB", "SBSQL-B288AFD4ECE5",
           "SBSQL-B5825D1638CA", "SBSQL-BA4115A6DBA5", "SBSQL-BD9DD4BBECA7",
           "SBSQL-C03FDC7E09D0", "SBSQL-C44E7F61A475", "SBSQL-C557FC25C1DF",
           "SBSQL-C5B5E28021D3", "SBSQL-C6D14CCCA2D1", "SBSQL-CBD9B6358B34",
           "SBSQL-CBE14326BD0B", "SBSQL-CF31B52FAA1F", "SBSQL-CFE56EE1BAC3",
           "SBSQL-D3C5EA9765BE", "SBSQL-D5BEA7309046", "SBSQL-DB22C5B8D6E6",
           "SBSQL-E211ACCD957F", "SBSQL-E43632706687", "SBSQL-E4EB3BEDAA0A",
           "SBSQL-E73C186D5991", "SBSQL-E8E12B064114", "SBSQL-F053EEAC95CD",
           "SBSQL-F1B58755A174", "SBSQL-F21F901FC2AF", "SBSQL-F3C89846D91C",
           "SBSQL-F4AE1FA62237", "SBSQL-F763191B3241", "SBSQL-F7D5231CA0E4",
           "SBSQL-F8050BCAF06D", "SBSQL-F985930BDD2F", "SBSQL-FB46F964CAA5",
           "SBSQL-FF57FEDF9747"}) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string(surface_id) + " SBSFC-038 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-038 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_12_arg_2_value\":\"3\"") &&
              Contains(artifacts.envelope.payload, "\"projection_12_arg_3_value\":\"4\""),
          "SBSFC-038 st_makepoint optional Z/M arguments missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_17_arg_0_value\":\"{\\\"type\\\":\\\"Point\\\",\\\"coordinates\\\":[3,4]}\""),
          "SBSFC-038 st_geomfromgeojson JSON argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_42_arg_4_type\":\"boolean\""),
          "SBSFC-038 st_asmvtgeom clip boolean argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT st_setsrid"),
          "SBSFC-038 spatial tail scalar helper payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-038 spatial tail route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-038 spatial tail admission did not require public ABI");
}

void RequireSbsfc039XmlDocumentQueryScalarHelperProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"(SELECT
      XMLDOCUMENT('<r><item>bird</item></r>') AS xmldocument_result,
      XMLNAMESPACES('p','urn:test') AS xmlnamespaces_result,
      XMLPARSE('document','<r/>') AS xmlparse_result,
      XMLQUERY('/r','<r/>') AS xmlquery_result,
      XMLSERIALIZE('document','<r/>','character') AS xmlserialize_result,
      XMLTEXT('a < b') AS xmltext_result,
      XMLVALIDATE('document','<r/>') AS xmlvalidate_result,
      xml('<r/>') AS xml_result,
      xml.attrs('id','42') AS xml_attrs_result,
      xml.ns('p','urn:test') AS xml_ns_result)");
  for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.bound.bound, "SBSFC-039 XML document/query scalar helper SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-039 XML document/query scalar helper verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-039 XML document/query scalar helper operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-039 XML document/query scalar helper opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"10\""),
          "SBSFC-039 XML document/query scalar helper projection count mismatch");

  struct ExpectedProjection {
    const char* function_id;
    const char* result_type;
    const char* arg_count;
  };
  const ExpectedProjection expected[] = {
      {"sb.xml.document", "xml_document", "1"},
      {"sb.xml.namespaces", "xml", "2"},
      {"sb.xml.parse", "xml_document", "2"},
      {"sb.xml.query", "xml_document", "2"},
      {"sb.xml.serialize", "character", "3"},
      {"sb.xml.text", "xml", "1"},
      {"sb.xml.validate", "xml_document", "2"},
      {"sb.xml.document", "xml_document", "1"},
      {"sb.xml.attrs", "xml", "2"},
      {"sb.xml.ns", "xml", "2"},
  };
  static_assert(sizeof(expected) / sizeof(expected[0]) == 10);
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const auto& row = expected[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload, prefix + "_function_id\":\"" + row.function_id + "\""),
            std::string(row.function_id) + " SBSFC-039 function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"" + row.result_type + "\""),
            std::string(row.function_id) + " SBSFC-039 result descriptor missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_arg_count\":\"" + row.arg_count + "\""),
            std::string(row.function_id) + " SBSFC-039 function arg count missing");
  }

  for (const auto* surface_id : {
           "SBSQL-253585ABE51D", "SBSQL-5753A90D2A1C", "SBSQL-9D96355276FC",
           "SBSQL-4F9AE84DDF5A", "SBSQL-965B96256EB3", "SBSQL-F48761720168",
           "SBSQL-B9BD61883168", "SBSQL-04FE00443530", "SBSQL-24C067DA97B0",
           "SBSQL-C9809EF23816", "SBSQL-82BBA556D880", "SBSQL-D53A57E7DD0B",
           "SBSQL-666EAE033CFC", "SBSQL-B4880446510E", "SBSQL-663D565ADA02",
           "SBSQL-5F496C39F6E8", "SBSQL-2ABE2825F6A1"}) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string(surface_id) + " SBSFC-039 surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
          "SBSFC-039 function-call grammar evidence missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_arg_0_value\":\"document\"") &&
              Contains(artifacts.envelope.payload, "\"projection_2_arg_1_value\":\"<r/>\""),
          "SBSFC-039 XMLPARSE special-form arguments missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_arg_2_value\":\"character\""),
          "SBSFC-039 XMLSERIALIZE target type argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_8_arg_0_value\":\"id\"") &&
              Contains(artifacts.envelope.payload, "\"projection_9_arg_0_value\":\"p\""),
          "SBSFC-039 xml attrs/ns arguments missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT XMLDOCUMENT"),
          "SBSFC-039 XML document/query scalar helper payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-039 XML document/query route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-039 XML document/query admission did not require public ABI");
}

void RequireSbsfc028AntiWalPolicyRefusalProjectionLowering() {
  for (const auto& expected : kSbsfc028AntiWalPolicyRefusalRows) {
    std::string sql = "SELECT ";
    sql += expected.sql_expression;
    sql += " AS \"";
    sql += expected.surface_id;
    sql += "\"";

    const auto artifacts = RunPipeline(sql);
    for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    Require(artifacts.bound.bound,
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL SELECT did not bind");
    for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    Require(artifacts.verifier.admitted,
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL verifier rejected route");
    Require(artifacts.envelope.operation_id == "query.evaluate_projection",
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL operation id mismatch");
    Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL opcode mismatch");
    Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"1\""),
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL projection count mismatch");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"projection_0_function_id\":\"") + expected.function_id + "\""),
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL function id missing");
    Require(Contains(artifacts.envelope.payload, expected.surface_id),
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL surface evidence missing");
    Require(Contains(artifacts.envelope.payload, "SBSQL-17B72695FA1A"),
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL function-call evidence missing");
    Require(!Contains(artifacts.envelope.payload, "SELECT "),
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL payload embedded source SQL text");

    const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Require(admission.admitted,
            std::string(expected.surface_id) + " server admission rejected SBSFC-028 Anti-WAL route");
    Require(admission.requires_public_abi_dispatch,
            std::string(expected.surface_id) + " SBSFC-028 Anti-WAL admission did not require public ABI");
  }
}

void RequireSbsfc016DonorAliasFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT ceiling(1.2) AS ceiling_value, NVL(NULL, 'fallback') AS nvl_value, "
      "IIF(TRUE, 'yes', 'no') AS iif_value, SYSDATE AS sysdate_value, "
      "gettransactionid AS gettransactionid_value");
  Require(artifacts.bound.bound, "SBSFC-016 donor alias function SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-016 donor alias function SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-016 donor alias function operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-016 donor alias function opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"5\""),
          "SBSFC-016 donor alias function projection count missing");
  for (const auto* function_id : {
           "sb.scalar.ceil",
           "sb.scalar.ifnull",
           "sb.scalar.iif",
           "sb.temporal.current_timestamp",
           "sb.session.transaction_id",
       }) {
    Require(Contains(artifacts.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "SBSFC-016 donor alias canonical id missing");
  }
  for (const auto* surface_id : {
           "SBSQL-869658452A5F",
           "SBSQL-0AF9A7B98768",
           "SBSQL-A6B9214AEE67",
           "SBSQL-96E89F3A2DAD",
           "SBSQL-1D8ADC63D435",
           "SBSQL-67D1D3830219",
           "SBSQL-17B72695FA1A",
       }) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            "SBSFC-016 donor alias function surface evidence missing");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_arg_count\":\"3\""),
          "SBSFC-016 IIF argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_arg_count\":\"0\""),
          "SBSFC-016 SYSDATE nullary route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_function_arg_count\":\"0\""),
          "SBSFC-016 gettransactionid nullary route missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT ceiling"),
          "SBSFC-016 donor alias payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-016 donor alias function route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for donor alias function route");

  const sblr::SblrDispatchRequest request{
      EngineContext(), EngineEnvelopeFromParserEnvelope(artifacts.envelope),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR donor alias function envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept donor alias function route");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route donor alias function projection");
  Require(result.api_result.ok, "engine donor alias function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine donor alias function projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 5,
          "engine donor alias function projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine donor alias function scalar projection mismatch: ") +
                std::string(name));
  };
  Require(row.fields[0].first == "ceiling_value" &&
              row.fields[0].second.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(row.fields[0].second.encoded_value) - 2.0) < 1e-6,
          "engine donor alias function scalar projection mismatch: ceiling_value");
  expect_text(1, "nvl_value", "text", "fallback");
  expect_text(2, "iif_value", "text", "yes");
  expect_text(3, "sysdate_value", "timestamp_tz", "2026-05-12T14:23:46Z");
  expect_text(4, "gettransactionid_value", "uint64", "42");
}

void RequireSbsfc016FixedPolicyLimitProjectionLowering() {
  const auto artifacts = RunPipeline(
      R"SQL(SELECT array_max_dimension() AS "SBSQL-F824C47A36A5", array_max_element_count() AS "SBSQL-BCAC432F4C75", case_when_max_branches() AS "SBSQL-6C698B54A7CB", cte_max_count_per_statement() AS "SBSQL-8EAA8898DBEB", nested_subquery_max_depth() AS "SBSQL-2CF7F4318343", recursive_cte_max_depth() AS "SBSQL-8A442BFCB429", result_set_max_columns() AS "SBSQL-189B5E58953A", union_max_arms() AS "SBSQL-7998B79486A5")SQL");
  Require(artifacts.bound.bound, "SBSFC-016 fixed policy limit scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-016 fixed policy limit scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-016 fixed policy limit scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-016 fixed policy limit scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"8\""),
          "SBSFC-016 fixed policy limit projection count missing");

  struct ExpectedProjection {
    const char* surface_id;
    const char* function_id;
  };
  const ExpectedProjection expected[] = {
      {"SBSQL-F824C47A36A5", "sb.scalar.array_max_dimension"},
      {"SBSQL-BCAC432F4C75", "sb.scalar.array_max_element_count"},
      {"SBSQL-6C698B54A7CB", "sb.scalar.case_when_max_branches"},
      {"SBSQL-8EAA8898DBEB", "sb.scalar.cte_max_count_per_statement"},
      {"SBSQL-2CF7F4318343", "sb.scalar.nested_subquery_max_depth"},
      {"SBSQL-8A442BFCB429", "sb.scalar.recursive_cte_max_depth"},
      {"SBSQL-189B5E58953A", "sb.scalar.result_set_max_columns"},
      {"SBSQL-7998B79486A5", "sb.scalar.union_max_arms"},
  };
  for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_name\":\"" + expected[index].surface_id + "\""),
            std::string(expected[index].surface_id) + " row label missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_id\":\"" + expected[index].function_id + "\""),
            std::string(expected[index].surface_id) + " canonical function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_function_arg_count\":\"0\""),
            std::string(expected[index].surface_id) + " nullary function route missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_type\":\"uint64\""),
            std::string(expected[index].surface_id) + " uint64 descriptor missing");
  }
  Require(!Contains(artifacts.envelope.payload, "SELECT array_max_dimension"),
          "SBSFC-016 fixed policy limit payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-016 fixed policy limit route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for SBSFC-016 fixed policy limit route");
}

void RequireSbsfc016LanguagePolicyProjectionLowering() {
  const std::size_t expected_count =
      sizeof(kSbsfc016LanguagePolicyRows) / sizeof(kSbsfc016LanguagePolicyRows[0]);
  std::string sql = "SELECT ";
  for (std::size_t index = 0; index < expected_count; ++index) {
    if (index != 0) sql += ", ";
    sql += kSbsfc016LanguagePolicyRows[index].function_name;
    sql += "() AS \"";
    sql += kSbsfc016LanguagePolicyRows[index].surface_id;
    sql += "\"";
  }

  const auto artifacts = RunPipeline(sql);
  Require(artifacts.bound.bound, "SBSFC-016 language policy scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-016 language policy scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-016 language policy scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-016 language policy scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_count\":\"" + std::to_string(expected_count) + "\""),
          "SBSFC-016 language policy projection count missing");

  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& expected = kSbsfc016LanguagePolicyRows[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_name\":\"" + expected.surface_id + "\""),
            std::string(expected.surface_id) + " language policy row label missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_id\":\"" + expected.function_id + "\""),
            std::string(expected.surface_id) + " language policy canonical function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_function_arg_count\":\"0\""),
            std::string(expected.surface_id) + " language policy nullary function route missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_type\":\"" + expected.result_type + "\""),
            std::string(expected.surface_id) + " language policy descriptor missing");
  }
  Require(!Contains(artifacts.envelope.payload, "SELECT numeric_division_by_zero"),
          "SBSFC-016 language policy payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-016 language policy route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for SBSFC-016 language policy route");
}

void RequireSbsfc016MetadataProjectionLowering() {
  const std::size_t expected_count =
      sizeof(kSbsfc016MetadataRows) / sizeof(kSbsfc016MetadataRows[0]);
  for (std::size_t probe = 0; probe < expected_count; ++probe) {
    const auto& expected = kSbsfc016MetadataRows[probe];
    std::string one_sql = "SELECT ";
    one_sql += expected.function_name;
    one_sql += "() AS \"";
    one_sql += expected.surface_id;
    one_sql += "\"";
    const auto one = RunPipeline(one_sql);
    if (!one.verifier.admitted) {
      std::cerr << "metadata row failed: " << expected.surface_id << " "
                << expected.function_name << '\n';
      for (const auto& diagnostic : one.envelope.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
    }
    Require(one.verifier.admitted, "single SBSFC-016 metadata scalar route failed");
  }
  std::string sql = "SELECT ";
  for (std::size_t index = 0; index < expected_count; ++index) {
    if (index != 0) sql += ", ";
    sql += kSbsfc016MetadataRows[index].function_name;
    sql += "() AS \"";
    sql += kSbsfc016MetadataRows[index].surface_id;
    sql += "\"";
  }

  const auto artifacts = RunPipeline(sql);
  Require(artifacts.bound.bound, "SBSFC-016 metadata scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  if (!artifacts.verifier.admitted) {
    std::cerr << artifacts.envelope.payload << '\n';
  }
  Require(artifacts.verifier.admitted,
          "SBSFC-016 metadata scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-016 metadata scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-016 metadata scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_count\":\"" + std::to_string(expected_count) + "\""),
          "SBSFC-016 metadata projection count missing");

  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& expected = kSbsfc016MetadataRows[index];
    const std::string prefix = "\"projection_" + std::to_string(index);
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_name\":\"" + expected.surface_id + "\""),
            std::string(expected.surface_id) + " metadata row label missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_function_id\":\"" + expected.function_id + "\""),
            std::string(expected.surface_id) + " metadata canonical function id missing");
    Require(Contains(artifacts.envelope.payload, prefix + "_function_arg_count\":\"0\""),
            std::string(expected.surface_id) + " metadata nullary function route missing");
    Require(Contains(artifacts.envelope.payload,
                     prefix + "_type\":\"" + expected.result_type + "\""),
            std::string(expected.surface_id) + " metadata descriptor missing");
  }
  Require(!Contains(artifacts.envelope.payload, "SELECT operation_evidence_required"),
          "SBSFC-016 metadata payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected SBSFC-016 metadata route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for SBSFC-016 metadata route");
}

void RequireSbsfc016ProceduralDiagnosticProjectionLowering() {
  constexpr std::size_t expected_count =
      sizeof(kSbsfc016ProceduralDiagnosticRows) / sizeof(kSbsfc016ProceduralDiagnosticRows[0]);
  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& expected = kSbsfc016ProceduralDiagnosticRows[index];
    std::string sql = "SELECT ";
    sql += expected.function_name;
    sql += "() AS \"";
    sql += expected.surface_id;
    sql += "\"";
    const auto artifacts = RunPipeline(sql);
    if (!artifacts.verifier.admitted) {
      std::cerr << "procedural diagnostic row failed: " << expected.surface_id << " "
                << expected.function_name << '\n';
      for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      std::cerr << artifacts.envelope.payload << '\n';
    }
    Require(artifacts.bound.bound, "SBSFC-016 procedural diagnostic scalar SELECT did not bind");
    Require(artifacts.verifier.admitted,
            "SBSFC-016 procedural diagnostic scalar SELECT SBLR verifier rejected route");
    Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"1\""),
            "SBSFC-016 procedural diagnostic projection count missing");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"projection_0_name\":\"") + expected.surface_id + "\""),
            std::string(expected.surface_id) + " procedural diagnostic row label missing");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"projection_0_function_id\":\"") + expected.function_id + "\""),
            std::string(expected.surface_id) + " procedural diagnostic function id missing");
    Require(!Contains(artifacts.envelope.payload, "SELECT "),
            "SBSFC-016 procedural diagnostic payload embedded source SQL text");

    const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
        scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Require(admission.admitted, "server admission rejected SBSFC-016 procedural diagnostic route");
    Require(admission.requires_public_abi_dispatch,
            "server admission did not require public ABI for SBSFC-016 procedural diagnostic route");
  }
}

void RequireCurrentSettingLiteralRefusalLowering() {
  const auto artifacts = RunPipeline(
      "SELECT current_setting('var') AS var_setting, "
      "current_setting('autocommit') AS autocommit_setting");
  Require(artifacts.bound.bound, "current_setting literal refusal SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "current_setting literal refusal SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "current_setting literal refusal SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "current_setting literal refusal SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"2\""),
          "current_setting literal refusal projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.scalar.current_setting\""),
          "current_setting('var') generic runtime route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"1\""),
          "current_setting('var') argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"var\""),
          "current_setting('var') literal payload missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.scalar.current_setting\""),
          "current_setting('autocommit') generic runtime route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_arg_count\":\"1\""),
          "current_setting('autocommit') argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_arg_0_value\":\"autocommit\""),
          "current_setting('autocommit') literal payload missing");
  Require(!Contains(artifacts.envelope.payload, "sb.scalar.current_setting_timezone"),
          "current_setting literal refusal route used timezone fast path");
  Require(!Contains(artifacts.envelope.payload, "SELECT current_setting"),
          "current_setting literal refusal payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected current_setting literal refusal route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for current_setting literal refusal route");
}

void RequireTextFunctionProjectionLowering() {
  RequireSpecialKeywordTextRegistryEvidence();
  RequireSqlKeywordTextFormRegistryEvidence();

  const auto artifacts = RunPipeline(
      "SELECT lower('ALPHA') AS lowered, upper('beta') AS uppered, length('surface') AS len_value");
  Require(artifacts.bound.bound, "text function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted, "text function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "text function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "text function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"3\""),
          "text function scalar projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_opcode\":\"SBLR_FUNCTION_CALL\""),
          "lower function scalar projection opcode missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.scalar.lower\""),
          "lower function canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"ALPHA\""),
          "lower function argument value missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_expr_opcode\":\"SBLR_FUNCTION_CALL\""),
          "upper function scalar projection opcode missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.scalar.upper\""),
          "upper function canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_arg_0_value\":\"beta\""),
          "upper function argument value missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_expr_opcode\":\"SBLR_FUNCTION_CALL\""),
          "length function scalar projection opcode missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_id\":\"sb.scalar.length\""),
          "length function canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_arg_0_value\":\"surface\""),
          "length function argument value missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT lower"),
          "text function scalar projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected text function scalar projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for text function scalar projection");

  const auto more = RunPipeline(
      "SELECT octet_length('hello') AS octets, bit_length('hello') AS bits, "
      "reverse('abc') AS reversed, ascii('Z') AS code_value, chr(90) AS char_value");
  Require(more.bound.bound, "additional text function scalar SELECT did not bind");
  for (const auto& diagnostic : more.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(more.verifier.admitted,
          "additional text function scalar SELECT SBLR verifier rejected route");
  Require(more.envelope.operation_id == "query.evaluate_projection",
          "additional text function scalar SELECT operation id mismatch");
  Require(more.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "additional text function scalar SELECT opcode mismatch");
  Require(Contains(more.envelope.payload, "\"projection_count\":\"5\""),
          "additional text function scalar projection count missing");
  Require(Contains(more.envelope.payload, "\"projection_0_function_id\":\"sb.scalar.octet_length\""),
          "octet_length canonical id missing");
  Require(Contains(more.envelope.payload, "\"projection_1_function_id\":\"sb.scalar.bit_length\""),
          "bit_length canonical id missing");
  Require(Contains(more.envelope.payload, "\"projection_2_function_id\":\"sb.scalar.reverse\""),
          "reverse canonical id missing");
  Require(Contains(more.envelope.payload, "\"projection_3_function_id\":\"sb.scalar.ascii\""),
          "ascii canonical id missing");
  Require(Contains(more.envelope.payload, "\"projection_4_function_id\":\"sb.scalar.chr\""),
          "chr canonical id missing");
  Require(Contains(more.envelope.payload, "\"projection_4_arg_0_type\":\"bigint\""),
          "chr bigint argument descriptor missing");
  Require(Contains(more.envelope.payload, "\"projection_4_arg_0_value\":\"90\""),
          "chr argument value missing");
  Require(!Contains(more.envelope.payload, "SELECT octet_length"),
          "additional text function projection payload embedded source SQL text");

  const auto trim_encoding = RunPipeline(
      "SELECT trim('  hello  ') AS trimmed, trim('xxhelloxx', 'x') AS trimmed_chars, "
      "btrim('abcabcfooabcabc', 'abc') AS btrimmed, "
      "ltrim('xyzhelloxyz', 'xyz') AS ltrimmed, "
      "rtrim('xyzhelloxyz', 'xyz') AS rtrimmed, "
      "ltrim('  hello') AS left_space_trimmed, "
      "rtrim('hello  ') AS right_space_trimmed, to_hex(255) AS hex_value");
  Require(trim_encoding.bound.bound, "trim/encoding text function scalar SELECT did not bind");
  for (const auto& diagnostic : trim_encoding.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(trim_encoding.verifier.admitted,
          "trim/encoding text function scalar SELECT SBLR verifier rejected route");
  Require(trim_encoding.envelope.operation_id == "query.evaluate_projection",
          "trim/encoding text function scalar SELECT operation id mismatch");
  Require(trim_encoding.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "trim/encoding text function scalar SELECT opcode mismatch");
  Require(Contains(trim_encoding.envelope.payload, "\"projection_count\":\"8\""),
          "trim/encoding text function scalar projection count missing");
  for (const auto* function_id : {
           "sb.scalar.trim",
           "sb.scalar.btrim",
           "sb.scalar.ltrim",
           "sb.scalar.rtrim",
           "sb.scalar.to_hex",
       }) {
    Require(Contains(trim_encoding.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "trim/encoding text canonical id missing");
  }
  Require(Contains(trim_encoding.envelope.payload, "\"projection_1_function_arg_count\":\"2\""),
          "two-argument trim projection argument count missing");
  Require(Contains(trim_encoding.envelope.payload, "\"projection_7_arg_0_type\":\"bigint\""),
          "to_hex bigint argument descriptor missing");
  Require(Contains(trim_encoding.envelope.payload, "\"projection_7_arg_0_value\":\"255\""),
          "to_hex argument value missing");
  Require(!Contains(trim_encoding.envelope.payload, "SELECT trim"),
          "trim/encoding function projection payload embedded source SQL text");

  const auto trim_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{trim_encoding.envelope.payload, false});
  for (const auto& diagnostic : trim_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(trim_admission.admitted, "server admission rejected trim/encoding function route");
  Require(trim_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for trim/encoding function route");

  const auto sbsfc026rj = RunPipeline(
      "SELECT to_numeric('42.5') AS to_numeric_value, "
      "to_number('7.25') AS to_number_value, "
      "to_integer('-42') AS to_integer_value, "
      "to_bigint('9223372036854775807') AS to_bigint_value, "
      "to_double('2.5') AS to_double_value, "
      "to_real('1.25') AS to_real_value, "
      "to_boolean(0) AS to_boolean_value, "
      "is_ascii('abc123') AS is_ascii_value, "
      "is_alphanumeric('abc123') AS is_alphanumeric_value, "
      "is_space('  ') AS is_space_value, "
      "is_digit('12345') AS is_digit_value, "
      "quote_ident('needs quote') AS quote_ident_value, "
      "quote_literal('O''Reilly') AS quote_literal_value, "
      "quote_nullable(NULL) AS quote_nullable_value, "
      "bit_set(8, 1) AS bit_set_value, "
      "bit_test(8, 3) AS bit_test_value, "
      "bit_clear(10, 1) AS bit_clear_value, "
      "bit_toggle(8, 1) AS bit_toggle_value, "
      "crc32('123456789') AS crc32_value, "
      "crc32c('123456789') AS crc32c_value, "
      "to_bytes('AZ', 'utf8') AS to_bytes_value");
  Require(sbsfc026rj.bound.bound, "SBSFC-026R-J function scalar SELECT did not bind");
  for (const auto& diagnostic : sbsfc026rj.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(sbsfc026rj.verifier.admitted,
          "SBSFC-026R-J function scalar SELECT SBLR verifier rejected route");
  Require(sbsfc026rj.envelope.operation_id == "query.evaluate_projection",
          "SBSFC-026R-J function scalar SELECT operation id mismatch");
  Require(sbsfc026rj.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SBSFC-026R-J function scalar SELECT opcode mismatch");
  Require(Contains(sbsfc026rj.envelope.payload, "\"projection_count\":\"21\""),
          "SBSFC-026R-J function projection count missing");
  for (const auto* function_id : {
           "sb.scalar.to_numeric",
           "sb.scalar.to_number",
           "sb.scalar.to_integer",
           "sb.scalar.to_bigint",
           "sb.scalar.to_double",
           "sb.scalar.to_real",
           "sb.scalar.to_boolean",
           "sb.scalar.is_ascii",
           "sb.scalar.is_alphanumeric",
           "sb.scalar.is_space",
           "sb.scalar.is_digit",
           "sb.scalar.quote_ident",
           "sb.scalar.quote_literal",
           "sb.scalar.quote_nullable",
           "sb.scalar.bit_set",
           "sb.scalar.bit_test",
           "sb.scalar.bit_clear",
           "sb.scalar.bit_toggle",
           "sb.scalar.crc32",
           "sb.scalar.crc32c",
           "sb.scalar.to_bytes",
       }) {
    Require(Contains(sbsfc026rj.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "SBSFC-026R-J canonical function id missing");
  }
  Require(Contains(sbsfc026rj.envelope.payload, "\"projection_20_function_arg_count\":\"2\""),
          "SBSFC-026R-J to_bytes argument count missing");
  Require(!Contains(sbsfc026rj.envelope.payload, "SELECT to_numeric"),
          "SBSFC-026R-J function projection payload embedded source SQL text");

  const auto sbsfc026rj_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{sbsfc026rj.envelope.payload, false});
  for (const auto& diagnostic : sbsfc026rj_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(sbsfc026rj_admission.admitted,
          "server admission rejected SBSFC-026R-J function route");
  Require(sbsfc026rj_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for SBSFC-026R-J function route");

  RequireGreatestLeastFormRegistryEvidence();
  RequireConditionalFormRegistryEvidence();
  const auto text_conditional = RunPipeline(
      "SELECT substring('hello world', 7, 5) AS sub_part, "
      "substring('hello world', 7) AS sub_tail, "
      "substr('hello world', 7, 5) AS substr_part, "
      "position('world', 'hello world') AS pos_value, "
      "lpad('bird', 7, '*') AS left_padded, "
      "rpad('bird', 7, '*') AS right_padded, "
      "repeat('ha', 3) AS repeated, "
      "overlay('hello world', 'SBSQL', 7) AS overlay_short, "
      "overlay('hello world', 'new', 7, 5) AS overlay_for, "
      "instr('banana', 'na', 3, 2) AS instr_pos, "
      "strpos('hello world', 'world') AS strpos_pos, "
      "ifnull(NULL, 'fallback') AS ifnull_value, "
      "coalesce(NULL, 'first', 'second') AS coalesce_value, "
      "coalesce_strict(NULL, 'strict', 'second') AS coalesce_strict_value, "
      "nullif('same', 'same') AS nullif_value, "
      "nvl2('set', 'yes', 'no') AS nvl2_value, "
      "greatest(9, 3, 5) AS greatest_value, "
      "least(9, 3, 5) AS least_value, "
      "IIF(TRUE, 'yes', 'no') AS iif_value, "
      "initcap('hello WORLD-from sb') AS initcap_value, "
      "translate('banana', 'an', 'ox') AS translate_value, "
      "unicode('A') AS unicode_value");
  Require(text_conditional.bound.bound,
          "text/conditional function scalar SELECT did not bind");
  for (const auto& diagnostic : text_conditional.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(text_conditional.verifier.admitted,
          "text/conditional function scalar SELECT SBLR verifier rejected route");
  Require(text_conditional.envelope.operation_id == "query.evaluate_projection",
          "text/conditional function scalar SELECT operation id mismatch");
  Require(text_conditional.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "text/conditional function scalar SELECT opcode mismatch");
  Require(Contains(text_conditional.envelope.payload, "\"projection_count\":\"22\""),
          "text/conditional function scalar projection count missing");
  for (const auto* function_id : {
           "sb.scalar.substring",
           "sb.scalar.substr",
           "sb.scalar.position",
           "sb.scalar.lpad",
           "sb.scalar.rpad",
           "sb.scalar.repeat",
           "sb.scalar.overlay",
           "sb.scalar.instr",
           "sb.scalar.strpos",
           "sb.scalar.ifnull",
           "sb.scalar.coalesce",
           "sb.scalar.coalesce_strict",
           "sb.scalar.nullif",
           "sb.scalar.nvl2",
           "sb.scalar.greatest",
           "sb.scalar.least",
           "sb.scalar.iif",
           "sb.scalar.initcap",
           "sb.scalar.translate",
           "sb.scalar.unicode",
       }) {
    Require(Contains(text_conditional.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "text/conditional canonical id missing");
  }
  Require(Contains(text_conditional.envelope.payload, "\"projection_0_function_arg_count\":\"3\""),
          "substring three-argument count missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_1_function_arg_count\":\"2\""),
          "substring two-argument count missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_8_function_arg_count\":\"4\""),
          "overlay four-argument count missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_11_arg_0_is_null\":\"true\""),
          "ifnull NULL argument marker missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_12_arg_0_is_null\":\"true\""),
          "coalesce NULL argument marker missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_12_function_id\":\"sb.scalar.coalesce\""),
          "coalesce_form canonical function id missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_12_function_arg_count\":\"3\""),
          "coalesce_form argument count missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_13_arg_0_is_null\":\"true\""),
          "coalesce_strict NULL argument marker missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_13_function_id\":\"sb.scalar.coalesce_strict\""),
          "coalesce_strict canonical function id missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_13_function_arg_count\":\"3\""),
          "coalesce_strict argument count missing");
  Require(Contains(text_conditional.envelope.payload, "SBSQL-07D714C91850"),
          "coalesce_strict bare surface evidence missing");
  Require(Contains(text_conditional.envelope.payload, "SBSQL-6841156E0F21"),
          "coalesce_strict args surface evidence missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_14_function_id\":\"sb.scalar.nullif\""),
          "nullif_form canonical function id missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_14_function_arg_count\":\"2\""),
          "nullif_form argument count missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_16_function_id\":\"sb.scalar.greatest\""),
          "greatest_form canonical function id missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_16_function_arg_count\":\"3\""),
          "greatest_form argument count missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_17_function_id\":\"sb.scalar.least\""),
          "least_form canonical function id missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_17_function_arg_count\":\"3\""),
          "least_form argument count missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_18_function_id\":\"sb.scalar.iif\""),
          "IIF canonical function id missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_18_function_arg_count\":\"3\""),
          "IIF argument count missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_19_function_id\":\"sb.scalar.initcap\""),
          "initcap canonical function id missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_20_function_id\":\"sb.scalar.translate\""),
          "translate canonical function id missing");
  Require(Contains(text_conditional.envelope.payload, "\"projection_20_function_arg_count\":\"3\""),
          "translate argument count missing");
  Require(Contains(text_conditional.envelope.payload,
                   "\"projection_21_function_id\":\"sb.scalar.unicode\""),
          "unicode canonical function id missing");
  Require(!Contains(text_conditional.envelope.payload, "SELECT substring"),
          "text/conditional function projection payload embedded source SQL text");

  const auto text_conditional_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{text_conditional.envelope.payload, false});
  for (const auto& diagnostic : text_conditional_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(text_conditional_admission.admitted,
          "server admission rejected text/conditional function route");
  Require(text_conditional_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for text/conditional function route");

  auto require_alias_projection =
      [](std::string_view sql,
         std::string_view label,
         std::string_view projection_count,
         std::initializer_list<std::string_view> function_ids) {
        const auto aliases = RunPipeline(sql);
        Require(aliases.bound.bound, std::string(label) + " scalar SELECT did not bind");
        for (const auto& diagnostic : aliases.envelope.messages.diagnostics) {
          std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        }
        if (!aliases.verifier.admitted) {
          std::cerr << label << " payload: " << aliases.envelope.payload << '\n';
        }
        Require(aliases.verifier.admitted,
                std::string(label) + " SELECT SBLR verifier rejected route");
        Require(aliases.envelope.operation_id == "query.evaluate_projection",
                std::string(label) + " SELECT operation id mismatch");
        Require(aliases.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
                std::string(label) + " SELECT opcode mismatch");
        Require(Contains(aliases.envelope.payload,
                         std::string("\"projection_count\":\"") + std::string(projection_count) + "\""),
                std::string(label) + " projection count missing");
        for (const auto function_id : function_ids) {
          Require(Contains(aliases.envelope.payload,
                           std::string("function_id\":\"") + std::string(function_id) + "\""),
                  std::string(label) + " canonical alias id missing");
        }

        const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
            scratchbird::server::ServerSblrAdmissionRequest{aliases.envelope.payload, false});
        for (const auto& diagnostic : admission.diagnostics) {
          std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        }
        Require(admission.admitted, std::string("server admission rejected ") + std::string(label));
        Require(admission.requires_public_abi_dispatch,
                std::string("server admission did not require public ABI for ") + std::string(label));
        return aliases.envelope.payload;
      };

  require_alias_projection(
      "SELECT sb.scalar.length('surface') AS dotted_len",
      "dotted text alias",
      "1",
      {
          "sb.scalar.length",
      });
  require_alias_projection(
      "SELECT sb.scalar.substring('hello world', 7, 5) AS dotted_substring",
      "dotted substring alias",
      "1",
      {"sb.scalar.substring"});
  require_alias_projection(
      "SELECT sb.scalar.trim('  trim  ') AS dotted_trim",
      "dotted trim alias",
      "1",
      {"sb.scalar.trim"});
  require_alias_projection(
      "SELECT sb.scalar.overlay('hello world', 'SBSQL', 7, 5) AS dotted_overlay",
      "dotted overlay alias",
      "1",
      {"sb.scalar.overlay"});
  require_alias_projection(
      "SELECT sb.scalar.concat('left', 'right') AS dotted_concat",
      "dotted concat alias",
      "1",
      {"sb.scalar.concat"});
  require_alias_projection(
      "SELECT sb.operator.concat('left', 'right') AS dotted_operator_concat",
      "dotted operator concat alias",
      "1",
      {"sb.scalar.concat"});
  require_alias_projection(
      "SELECT sb.scalar.replace('one two', 'two', '2') AS dotted_replace",
      "dotted replace alias",
      "1",
      {"sb.scalar.replace"});
  require_alias_projection(
      "SELECT sb.scalar.octet_length('abc') AS dotted_octets",
      "dotted octet_length alias",
      "1",
      {"sb.scalar.octet_length"});
  const auto sbsfc026_native_payload = require_alias_projection(
      "SELECT cardinality('[1,2,3]') AS multiset_cardinality, "
      "STUFF('abcdef', 2, 3, 'XY') AS stuffed",
      "SBSFC-026 native function alias",
      "2",
      {
          "sb.scalar.cardinality",
          "sb.scalar.stuff",
      });
  Require(Contains(sbsfc026_native_payload, "SBSQL-77E9B5B7AE98"),
          "SBSFC-026 CARDINALITY surface id missing from parser payload");
  Require(Contains(sbsfc026_native_payload, "SBSQL-512B2D237E65"),
          "SBSFC-026 CARDINALITY bare surface id missing from parser payload");
  Require(Contains(sbsfc026_native_payload, "SBSQL-630C9B6D7C13"),
          "SBSFC-026 CARDINALITY collection signature surface id missing from parser payload");
  Require(Contains(sbsfc026_native_payload, "SBSQL-54361A1F6365"),
          "SBSFC-026 STUFF surface id missing from parser payload");
  require_alias_projection(
      "SELECT currval() AS seq_curr_missing, "
      "currval('proj_seq') AS seq_curr_named, "
      "gen_id() AS seq_gen_missing, "
      "gen_id('proj_gen', 1) AS seq_gen_inc, "
      "lastval() AS seq_last, "
      "nextval() AS seq_next_missing, "
      "nextval('proj_seq') AS seq_next_named, "
      "setval() AS seq_set_missing, "
      "setval('proj_seq', 7, 0) AS seq_set_named",
      "SBSFC-029 sequence scalar alias",
      "9",
      {
          "sb.scalar.currval",
          "sb.scalar.currval",
          "sb.scalar.gen_id",
          "sb.scalar.gen_id",
          "sb.scalar.lastval",
          "sb.scalar.nextval",
          "sb.scalar.nextval",
          "sb.scalar.setval",
          "sb.scalar.setval",
      });
  require_alias_projection(
      "SELECT LEFT('abcdef', 2) AS uppercase_left",
      "uppercase LEFT alias",
      "1",
      {"sb.scalar.left"});
  require_alias_projection(
      "SELECT RIGHT('abcdef', 2) AS uppercase_right",
      "uppercase RIGHT alias",
      "1",
      {"sb.scalar.right"});
  require_alias_projection(
      "SELECT sb.scalar.ifnull(NULL, 'fallback') AS dotted_ifnull, "
      "sb.scalar.coalesce(NULL, 'first') AS dotted_coalesce, "
      "sb.scalar.nullif('same', 'same') AS dotted_nullif",
      "dotted conditional alias",
      "3",
      {
          "sb.scalar.ifnull",
          "sb.scalar.coalesce",
          "sb.scalar.nullif",
      });
  const auto dotted_session_bare_payload = require_alias_projection(
      "SELECT sb.session.current_user AS dotted_current_user, "
      "sb.session.current_catalog AS dotted_current_catalog, "
      "sb.session.current_schema AS dotted_current_schema",
      "dotted session bare alias",
      "3",
      {
          "sb.session.current_user",
          "sb.session.current_catalog",
          "sb.session.current_schema",
      });
  Require(Contains(dotted_session_bare_payload, "\"projection_0_function_arg_count\":\"0\""),
          "dotted bare current_user zero-argument route missing");
  Require(Contains(dotted_session_bare_payload, "\"projection_1_function_arg_count\":\"0\""),
          "dotted bare current_catalog zero-argument route missing");
  Require(Contains(dotted_session_bare_payload, "\"projection_2_function_arg_count\":\"0\""),
          "dotted bare current_schema zero-argument route missing");
  require_alias_projection(
      "SELECT sb.session.current_user() AS dotted_current_user_call, "
      "sb.session.current_catalog() AS dotted_current_catalog_call, "
      "sb.session.current_schema() AS dotted_current_schema_call",
      "dotted session function-call alias",
      "3",
      {
          "sb.session.current_user",
          "sb.session.current_catalog",
          "sb.session.current_schema",
      });
  const auto special_alias_payload = require_alias_projection(
      "SELECT sb.special_form.coalesce(NULL, 'first') AS special_coalesce",
      "special-form coalesce alias",
      "1",
      {"sb.scalar.coalesce"});
  Require(Contains(special_alias_payload, "\"projection_0_function_id\":\"sb.scalar.coalesce\""),
          "sb.special_form.coalesce did not lower to canonical coalesce function id");
  Require(Contains(special_alias_payload, "\"projection_0_arg_0_is_null\":\"true\""),
          "sb.special_form.coalesce NULL argument marker missing");
  Require(!Contains(special_alias_payload, "sb.special_form.coalesce"),
          "special-form alias leaked source surface spelling into SBLR payload");

  const auto keyword_text = RunPipeline(
      "SELECT POSITION('lo' IN 'hello') AS position_keyword, "
      "SUBSTRING('hello world' FROM 7 FOR 5) AS substring_keyword, "
      "SUBSTRING('hello world' FROM 7) AS substring_tail_keyword, "
      "TRIM(BOTH 'x' FROM 'xxhelloxx') AS trim_both_keyword, "
      "TRIM(FROM '  hello  ') AS trim_spaces_keyword, "
      "TRIM(LEADING 'x' FROM 'xxhello') AS trim_leading_keyword, "
      "TRIM(TRAILING 'x' FROM 'helloxx') AS trim_trailing_keyword, "
      "OVERLAY('hello world' PLACING 'SBSQL' FROM 7 FOR 5) AS overlay_for_keyword, "
      "OVERLAY('hello world' PLACING 'SBSQL' FROM 7) AS overlay_keyword");
  Require(keyword_text.bound.bound,
          "SQL keyword text function scalar SELECT did not bind");
  for (const auto& diagnostic : keyword_text.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(keyword_text.verifier.admitted,
          "SQL keyword text function scalar SELECT SBLR verifier rejected route");
  Require(keyword_text.envelope.operation_id == "query.evaluate_projection",
          "SQL keyword text function scalar SELECT operation id mismatch");
  Require(keyword_text.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "SQL keyword text function scalar SELECT opcode mismatch");
  Require(Contains(keyword_text.envelope.payload, "\"projection_count\":\"9\""),
          "SQL keyword text function scalar projection count missing");
  for (const auto* function_id : {
           "sb.scalar.position",
           "sb.special.substring_keyword",
           "sb.special.trim_keyword",
           "sb.scalar.ltrim",
           "sb.scalar.rtrim",
           "sb.scalar.overlay",
       }) {
    Require(Contains(keyword_text.envelope.payload,
                     std::string("function_id\":\"") + function_id + "\""),
            "SQL keyword text canonical id missing");
  }
  Require(Contains(keyword_text.envelope.payload, "\"projection_0_function_arg_count\":\"2\""),
          "POSITION keyword argument count missing");
  Require(Contains(keyword_text.envelope.payload,
                   "\"projection_0_function_id\":\"sb.scalar.position\""),
          "POSITION keyword scalar function id missing");
  Require(Contains(keyword_text.envelope.payload,
                   "\"projection_1_function_id\":\"sb.special.substring_keyword\""),
          "SUBSTRING keyword special-form function id missing");
  Require(Contains(keyword_text.envelope.payload, "\"projection_1_function_arg_count\":\"3\""),
          "SUBSTRING keyword FOR argument count missing");
  Require(Contains(keyword_text.envelope.payload,
                   "\"projection_2_function_id\":\"sb.special.substring_keyword\""),
          "SUBSTRING keyword tail special-form function id missing");
  Require(Contains(keyword_text.envelope.payload, "\"projection_2_function_arg_count\":\"2\""),
          "SUBSTRING keyword tail argument count missing");
  Require(Contains(keyword_text.envelope.payload,
                   "\"projection_3_function_id\":\"sb.special.trim_keyword\""),
          "TRIM keyword BOTH special-form function id missing");
  Require(Contains(keyword_text.envelope.payload, "\"projection_3_function_arg_count\":\"2\""),
          "TRIM keyword char argument count missing");
  Require(Contains(keyword_text.envelope.payload,
                   "\"projection_4_function_id\":\"sb.special.trim_keyword\""),
          "TRIM keyword default-space special-form function id missing");
  Require(Contains(keyword_text.envelope.payload, "\"projection_4_function_arg_count\":\"1\""),
          "TRIM keyword default-space argument count missing");
  Require(Contains(keyword_text.envelope.payload, "\"projection_7_function_arg_count\":\"4\""),
          "OVERLAY keyword FOR argument count missing");
  Require(Contains(keyword_text.envelope.payload,
                   "\"projection_7_function_id\":\"sb.scalar.overlay\""),
          "OVERLAY keyword FOR scalar function id missing");
  Require(Contains(keyword_text.envelope.payload, "\"projection_8_function_arg_count\":\"3\""),
          "OVERLAY keyword default-length argument count missing");
  Require(Contains(keyword_text.envelope.payload,
                   "\"projection_8_function_id\":\"sb.scalar.overlay\""),
          "OVERLAY keyword default-length scalar function id missing");
  Require(!Contains(keyword_text.envelope.payload, "SELECT POSITION"),
          "SQL keyword text function projection payload embedded source SQL text");

  const auto keyword_text_admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{keyword_text.envelope.payload, false});
  for (const auto& diagnostic : keyword_text_admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(keyword_text_admission.admitted,
          "server admission rejected SQL keyword text function route");
  Require(keyword_text_admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for SQL keyword text function route");
}

void RequireMultiArgumentFunctionProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT concat('alpha', '-', 'beta') AS joined, "
      "replace('alpha beta beta', 'beta', 'B') AS replaced, "
      "regexp_like('Alpha', '^a', 'i') AS regex_ok, "
      "date_part('year', '2026-05-11T14:23:45') AS year_part, "
      "date_trunc('day', '2026-05-11T14:23:45') AS day_trunc, "
      "json_array_length('[1,2,3]') AS json_len, "
      "soundex('Robert') AS sound_code, "
      "levenshtein('kitten', 'sitting') AS edit_distance, "
      "regexp_count('abcabc', 'a.') AS regex_count, "
      "regexp_match('abc-123', '([a-z]+)-([0-9]+)') AS regex_capture, "
      "regexp_replace('abc123abc', 'abc', 'X', 'g') AS regex_replaced, "
      "regexp_split_to_array('a,b;c', '[,;]') AS regex_parts, "
      "regexp_substr('abc-123-def', '[0-9]+') AS regex_substr, "
      "OCCURRENCES_REGEX('a.' IN 'abcabc') AS occurrences_regex_count, "
      "POSITION_REGEX(AFTER 'a' IN 'A-a' OCCURRENCE 2 FLAG 'i') AS position_regex_after, "
      "SUBSTRING_REGEX('([a-z])([0-9]+)' IN 'a1 b22 c333' OCCURRENCE 2 GROUP 2 FLAG '') AS substring_regex_group, "
      "TRANSLATE_REGEX('abc' IN 'abc123abc' WITH 'X' ALL FLAG '') AS translate_regex_all, "
      "regexp_matches('abc-123', '([a-z]+)-([0-9]+)') AS regex_matches, "
      "regexp_split_to_table('a,b;c', '[,;]') AS regex_split_rows, "
      "convert('deja vu', 'UTF8') AS converted, "
      "array_to_string('[\"a\",null,\"c\"]', '|') AS array_joined, "
      "to_ascii('deja vu') AS ascii_text, "
      "format('SELECT %I = %L %%', 'col name', 'O''Reilly') AS formatted");
  Require(artifacts.bound.bound, "multi-argument function scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "multi-argument function scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "multi-argument function scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "multi-argument function scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"23\""),
          "multi-argument function projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.scalar.concat\""),
          "concat canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"3\""),
          "concat argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.scalar.replace\""),
          "replace canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_id\":\"sb.regex.match\""),
          "regexp_like canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_id\":\"sb.temporal.date_part\""),
          "date_part canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_function_id\":\"sb.temporal.date_trunc\""),
          "date_trunc canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_5_function_id\":\"sb.json.array_length\""),
          "json_array_length canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_6_function_id\":\"sb.scalar.soundex\""),
          "soundex canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_7_function_id\":\"sb.scalar.levenshtein\""),
          "levenshtein canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_7_arg_1_value\":\"sitting\""),
          "levenshtein second argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_8_function_id\":\"sb.scalar.regexp_count\""),
          "regexp_count canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_9_function_id\":\"sb.scalar.regexp_match\""),
          "regexp_match canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_10_function_id\":\"sb.scalar.regexp_replace\""),
          "regexp_replace canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_11_function_id\":\"sb.scalar.regexp_split_to_array\""),
          "regexp_split_to_array canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_12_function_id\":\"sb.scalar.regexp_substr\""),
          "regexp_substr canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_13_function_id\":\"sb.scalar.occurrences_regex\""),
          "occurrences_regex canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_14_function_id\":\"sb.scalar.position_regex\""),
          "position_regex canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_15_function_id\":\"sb.scalar.substring_regex\""),
          "substring_regex canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_16_function_id\":\"sb.scalar.translate_regex\""),
          "translate_regex canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_17_function_id\":\"sb.scalar.regexp_matches\""),
          "regexp_matches canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_18_function_id\":\"sb.scalar.regexp_split_to_table\""),
          "regexp_split_to_table canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_19_function_id\":\"sb.scalar.convert\""),
          "convert canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_20_function_id\":\"sb.scalar.array_to_string\""),
          "array_to_string canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_21_function_id\":\"sb.scalar.to_ascii\""),
          "to_ascii canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_22_function_id\":\"sb.scalar.format\""),
          "format canonical id missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT concat"),
          "multi-argument function projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected multi-argument function projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for multi-argument function projection");
}

void RequireConcatExpressionProjectionLowering() {
  RequireConcatExpressionRegistryEvidence();
  const auto artifacts = RunPipeline("SELECT 'alpha' || '-' || 'beta' AS joined");
  Require(artifacts.bound.bound, "concat_expr scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "concat_expr scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "concat_expr scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "concat_expr scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "concat_expr scalar projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_name\":\"joined\""),
          "concat_expr projection alias missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"function\""),
          "concat_expr did not lower as function-call expression");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_opcode\":\"SBLR_FUNCTION_CALL\""),
          "concat_expr function-call opcode missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.scalar.concat\""),
          "concat_expr canonical concat function id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"3\""),
          "concat_expr argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"alpha\""),
          "concat_expr first argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_1_value\":\"-\""),
          "concat_expr separator argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_2_value\":\"beta\""),
          "concat_expr third argument missing");
  Require(!Contains(artifacts.envelope.payload, "||"),
          "concat_expr payload retained source concat operator text");
  Require(!Contains(artifacts.envelope.payload, "SELECT 'alpha'"),
          "concat_expr payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected concat_expr projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for concat_expr projection");
  Require(admission.operation_id == "query.evaluate_projection",
          "concat_expr server admission operation id mismatch");
}

void RequireQualifiedRegexAliasProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT sb.func.regexp_like('Alpha', '^a', 'i') AS regex_ok");
  Require(artifacts.bound.bound, "qualified sb.func.regexp_like scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "qualified sb.func.regexp_like scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "qualified sb.func.regexp_like operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "qualified sb.func.regexp_like opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "qualified sb.func.regexp_like projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.regex.match\""),
          "qualified sb.func.regexp_like canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"3\""),
          "qualified sb.func.regexp_like argument count missing");
  Require(!Contains(artifacts.envelope.payload, "sb.func.regexp_like"),
          "qualified sb.func.regexp_like payload retained alias as execution authority");
  Require(!Contains(artifacts.envelope.payload, "SELECT sb.func.regexp_like"),
          "qualified sb.func.regexp_like payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected qualified sb.func.regexp_like projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for qualified sb.func.regexp_like projection");
}

void RequireQualifiedTemporalAliasProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT sb.temporal.date_part('year', '2026-05-11T14:23:45') AS year_part, "
      "sb.temporal.date_trunc('day', '2026-05-11T14:23:45') AS day_trunc");
  Require(artifacts.bound.bound,
          "qualified sb.temporal date_part/date_trunc scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "qualified sb.temporal date_part/date_trunc scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "qualified sb.temporal date_part/date_trunc operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "qualified sb.temporal date_part/date_trunc opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"2\""),
          "qualified sb.temporal date_part/date_trunc projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.temporal.date_part\""),
          "qualified sb.temporal.date_part canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"2\""),
          "qualified sb.temporal.date_part argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.temporal.date_trunc\""),
          "qualified sb.temporal.date_trunc canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_arg_count\":\"2\""),
          "qualified sb.temporal.date_trunc argument count missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT sb.temporal"),
          "qualified sb.temporal alias payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected qualified sb.temporal alias projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for qualified sb.temporal alias projection");
}

void RequireBareTemporalDateAliasProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT date_part('year', '2026-05-11T14:23:45') AS year_part, "
      "date_trunc('day', '2026-05-11T14:23:45') AS day_trunc");
  Require(artifacts.bound.bound,
          "bare date_part/date_trunc scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "bare date_part/date_trunc scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "bare date_part/date_trunc operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "bare date_part/date_trunc opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"2\""),
          "bare date_part/date_trunc projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.temporal.date_part\""),
          "bare date_part canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"2\""),
          "bare date_part argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.temporal.date_trunc\""),
          "bare date_trunc canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_arg_count\":\"2\""),
          "bare date_trunc argument count missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT date_part"),
          "bare date_part/date_trunc payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected bare date_part/date_trunc projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for bare date_part/date_trunc projection");
}

void RequireExtractSpecialAndCanonicalJsonProjectionLowering() {
  RequireExtractFormRegistryEvidence();
  const auto artifacts = RunPipeline(
      R"SQL(SELECT EXTRACT(YEAR FROM '2026-05-11T14:23:45') AS extract_year, EXTRACT(EPOCH FROM '2026-05-11T14:23:45') AS extract_epoch, sb.special.extract('month', '2026-05-11T14:23:45') AS special_extract, sb.json.extract('{"a":42}', '$.a') AS json_extract_canonical)SQL");
  Require(artifacts.bound.bound,
          "EXTRACT/sb.special.extract/sb.json.extract scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "EXTRACT/sb.special.extract/sb.json.extract scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "EXTRACT/sb.special.extract/sb.json.extract operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "EXTRACT/sb.special.extract/sb.json.extract opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"4\""),
          "EXTRACT/sb.special.extract/sb.json.extract projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.temporal.date_part\""),
          "EXTRACT generic canonical date_part id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"year\""),
          "EXTRACT generic part argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.temporal.date_part\""),
          "EXTRACT epoch canonical date_part id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_arg_0_value\":\"epoch\""),
          "EXTRACT epoch part argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_id\":\"sb.temporal.date_part\""),
          "sb.special.extract canonical date_part id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_arg_0_value\":\"month\""),
          "sb.special.extract part argument missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_id\":\"sb.json.extract\""),
          "canonical sb.json.extract id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_arg_1_value\":\"$.a\""),
          "canonical sb.json.extract path argument missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT EXTRACT"),
          "EXTRACT projection payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "sb.special.extract"),
          "sb.special.extract payload retained alias as execution authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted,
          "server admission rejected EXTRACT/sb.special.extract/sb.json.extract projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for EXTRACT/sb.special.extract/sb.json.extract projection");
}

void RequireQualifiedTemporalProviderAliasProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT sb.temporal.now() AS now_value, "
      "sb.temporal.current_timestamp AS current_ts, "
      "sb.temporal.current_date AS current_date_value, "
      "sb.temporal.current_time AS current_time_value");
  Require(artifacts.bound.bound,
          "qualified sb.temporal provider scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "qualified sb.temporal provider scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "qualified sb.temporal provider operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "qualified sb.temporal provider opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"4\""),
          "qualified sb.temporal provider projection count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.temporal.now\""),
          "qualified sb.temporal.now canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_arg_count\":\"0\""),
          "qualified sb.temporal.now argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_function_id\":\"sb.temporal.current_timestamp\""),
          "qualified sb.temporal.current_timestamp canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_function_id\":\"sb.temporal.current_date\""),
          "qualified sb.temporal.current_date canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_function_id\":\"sb.temporal.current_time\""),
          "qualified sb.temporal.current_time canonical id missing");
  for (int index = 1; index < 4; ++index) {
    Require(Contains(artifacts.envelope.payload,
                     "\"projection_" + std::to_string(index) + "_function_arg_count\":\"0\""),
            "qualified sb.temporal provider zero-argument route missing");
  }
  Require(!Contains(artifacts.envelope.payload, "SELECT sb.temporal"),
          "qualified sb.temporal provider payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected qualified sb.temporal provider route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for qualified sb.temporal provider route");
}

void RequireSpecialCurrentTimestampKeywordProjectionLowering() {
  const auto artifacts =
      RunPipeline("SELECT sb.special.current_timestamp_keyword AS current_ts");
  Require(artifacts.bound.bound,
          "sb.special.current_timestamp_keyword scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "sb.special.current_timestamp_keyword scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "sb.special.current_timestamp_keyword operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "sb.special.current_timestamp_keyword opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "sb.special.current_timestamp_keyword projection count missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_function_id\":\"sb.special.current_timestamp_keyword\""),
          "sb.special.current_timestamp_keyword canonical function id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_function_arg_count\":\"0\""),
          "sb.special.current_timestamp_keyword zero-argument route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_type\":\"timestamp_tz\""),
          "sb.special.current_timestamp_keyword timestamp_tz descriptor missing");
  Require(!Contains(artifacts.envelope.payload, "sb.temporal.current_timestamp"),
          "sb.special.current_timestamp_keyword lowered through temporal alias instead of special-form id");
  Require(!Contains(artifacts.envelope.payload,
                    "SELECT sb.special.current_timestamp_keyword"),
          "sb.special.current_timestamp_keyword payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted,
          "server admission rejected sb.special.current_timestamp_keyword projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for sb.special.current_timestamp_keyword route");
}

void RequireCurrentValueFormProjectionLowering() {
  RequireCurrentValueFormRegistryEvidence();
  const auto artifacts = RunPipeline("SELECT current_timestamp AS current_ts");
  Require(artifacts.bound.bound,
          "current_value_form/current_timestamp scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted,
          "current_value_form/current_timestamp scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "current_value_form/current_timestamp operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "current_value_form/current_timestamp opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"1\""),
          "current_value_form/current_timestamp projection count missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_function_id\":\"sb.temporal.current_timestamp\""),
          "current_value_form/current_timestamp canonical function id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_function_arg_count\":\"0\""),
          "current_value_form/current_timestamp zero-argument route missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_type\":\"timestamp_tz\""),
          "current_value_form/current_timestamp timestamp_tz descriptor missing");
  Require(!Contains(artifacts.envelope.payload, "sb.special.current_timestamp_keyword"),
          "current_value_form/current_timestamp leaked special-form alias into execution id");
  Require(!Contains(artifacts.envelope.payload, "SELECT current_timestamp"),
          "current_value_form/current_timestamp payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted,
          "server admission rejected current_value_form/current_timestamp projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for current_value_form/current_timestamp route");
}

void RequireTableSelectStillUsesDml() {
  const auto artifacts = RunPipeline("SELECT * FROM customer", {std::string(kTargetUuid)});
  Require(artifacts.bound.bound, "table SELECT did not bind after UUID resolution");
  Require(artifacts.verifier.admitted, "table SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "dml.select_rows",
          "table SELECT no longer routes to DML select rows");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_SELECT_ROWS",
          "table SELECT DML opcode mismatch");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
          "table SELECT target UUID missing");
}

void RequireOperatorProjectionLowering() {
  RequireLogicalExpressionGrammarRegistryEvidence();
  RequireLikeGrammarRegistryEvidence();
  const auto artifacts = RunPipeline(
      "SELECT NOT TRUE AS not_true, 'ScratchBird' LIKE 'Scratch%' AS like_match, "
      "NOT ('sparrow' LIKE 'duck%') AS not_like_match, 'bird' NOT LIKE 'b%' AS not_like_composed, "
      "'a_%' LIKE 'a!_%' ESCAPE '!' AS like_escape, "
      "'Scratch_Bird' ILIKE 'scratch!_bird' ESCAPE '!' AS ilike_escape");
  Require(artifacts.bound.bound, "operator scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted, "operator scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "operator scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "operator scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"6\""),
          "operator scalar projection count mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_id\":\"op_not\""),
          "prefix NOT operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_canonical_operator_id\":\"sb.operator.not\""),
          "prefix NOT canonical operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_arg_count\":\"1\""),
          "prefix NOT operator argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_type\":\"boolean\""),
          "prefix NOT boolean operand descriptor missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"true\""),
          "prefix NOT boolean operand value missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_operator_id\":\"op_like\""),
          "LIKE operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_1_canonical_operator_id\":\"sb.operator.like\""),
          "LIKE canonical operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_operator_id\":\"op_not\""),
          "composed NOT operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_arg_0_operator_id\":\"op_like\""),
          "composed NOT(LIKE) child operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_operator_id\":\"op_not\""),
          "NOT LIKE composition operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_arg_0_operator_id\":\"op_like\""),
          "NOT LIKE child LIKE operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_operator_id\":\"op_like\""),
          "LIKE ESCAPE operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_4_arg_1_value\":\"a\\\\_%\""),
          "LIKE ESCAPE normalized pattern missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_5_operator_id\":\"op_ilike\""),
          "ILIKE ESCAPE operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_5_arg_1_value\":\"scratch\\\\_bird\""),
          "ILIKE ESCAPE normalized pattern missing");
  Require(!Contains(artifacts.envelope.payload, "op_not_like"),
          "operator scalar projection emitted forbidden op_not_like");
  Require(!Contains(artifacts.envelope.payload, "SELECT NOT"),
          "operator scalar projection payload embedded source SQL text");

  const auto invalid = RunPipeline("SELECT 'abc' LIKE 'abc\\\\' AS bad_like");
  Require(invalid.bound.bound, "invalid LIKE pattern scalar SELECT did not bind");
  Require(invalid.verifier.admitted,
          "invalid LIKE pattern should lower to engine diagnostic route");
  Require(Contains(invalid.envelope.payload, "\"projection_0_operator_id\":\"op_like\""),
          "invalid LIKE operator route missing");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected operator scalar projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for operator scalar projection");
}

void RequireIlikeProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT 'ScratchBird' ILIKE 'scratch%' AS ilike_match, "
      "'sparrow' ILIKE 'DUCK%' AS ilike_false");
  Require(artifacts.bound.bound, "ILIKE scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted, "ILIKE scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "ILIKE scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "ILIKE scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"2\""),
          "ILIKE scalar projection count mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_id\":\"op_ilike\""),
          "ILIKE operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_canonical_operator_id\":\"sb.operator.ilike\""),
          "ILIKE canonical operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_operator_id\":\"op_ilike\""),
          "ILIKE false operator id missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT 'ScratchBird'"),
          "ILIKE scalar projection payload embedded source SQL text");

  const auto invalid = RunPipeline("SELECT 'abc' ILIKE 'abc\\\\' AS bad_ilike");
  Require(invalid.bound.bound, "invalid ILIKE pattern scalar SELECT did not bind");
  Require(invalid.verifier.admitted,
          "invalid ILIKE pattern should lower to engine diagnostic route");
  Require(Contains(invalid.envelope.payload, "\"projection_0_operator_id\":\"op_ilike\""),
          "invalid ILIKE operator route missing");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected ILIKE scalar projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for ILIKE scalar projection");
}

void RequireExtendedOperatorProjectionLowering() {
  const auto artifacts = RunPipeline(
      "SELECT TRUE AND FALSE AS and_infix, FALSE OR TRUE AS or_infix, "
      "sb.operator.and(TRUE, FALSE) AS and_call, sb.operator.or(FALSE, TRUE) AS or_call, "
      "sb.operator.unary_minus(7) AS neg_call, 1 IS DISTINCT FROM NULL AS distinct_infix, "
      "sb.operator.is_distinct_from(1, 1) AS distinct_call, MATCH('Alpha', '^A') AS match_call, "
      "sb.regex.match('Alpha', '^A') AS regex_match, "
      "sb.operator.regex_match('Alpha', '^A') AS regex_op, "
      "sb.operator.json_get('{\"a\":1}', '$.a') AS json_get, "
      "sb.operator.json_get_text('{\"a\":\"bird\"}', '$.a') AS json_text, "
      "sb.operator.array_contains('[1,2]', 2) AS array_contains, "
      "TRUE XOR FALSE AS xor_infix, sb.operator.xor(TRUE, FALSE) AS xor_call");
  if (!artifacts.bound.bound) {
    for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
  }
  Require(artifacts.bound.bound, "extended operator scalar SELECT did not bind");
  for (const auto& diagnostic : artifacts.envelope.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(artifacts.verifier.admitted, "extended operator scalar SELECT SBLR verifier rejected route");
  Require(artifacts.envelope.operation_id == "query.evaluate_projection",
          "extended operator scalar SELECT operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
          "extended operator scalar SELECT opcode mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_count\":\"15\""),
          "extended operator scalar projection count mismatch");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_id\":\"op_and\""),
          "AND infix operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_canonical_operator_id\":\"sb.operator.and\""),
          "AND infix canonical operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_arg_count\":\"2\""),
          "AND infix operator argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"true\""),
          "AND infix left operand missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_1_value\":\"false\""),
          "AND infix right operand missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_operator_id\":\"op_or\""),
          "OR infix operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_1_canonical_operator_id\":\"sb.operator.or\""),
          "OR infix canonical operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_operator_arg_count\":\"2\""),
          "OR infix operator argument count missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_arg_0_value\":\"false\""),
          "OR infix left operand missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_1_arg_1_value\":\"true\""),
          "OR infix right operand missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_2_operator_id\":\"op_and\""),
          "sb.operator.and call operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_3_operator_id\":\"op_or\""),
          "sb.operator.or call operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_4_operator_id\":\"op_unary_minus\""),
          "unary minus operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_5_operator_id\":\"op_is_distinct\""),
          "IS DISTINCT FROM operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_6_operator_id\":\"op_is_distinct\""),
          "sb.operator.is_distinct_from call operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_7_function_id\":\"sb.regex.match\""),
          "MATCH function canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_8_function_id\":\"sb.regex.match\""),
          "sb.regex.match function canonical id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_9_operator_id\":\"op_regex_match\""),
          "regex operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_10_operator_id\":\"op_json_get\""),
          "JSON get operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_11_operator_id\":\"op_json_get_text\""),
          "JSON get text operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_12_operator_id\":\"op_array_contains\""),
          "array contains operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_13_operator_id\":\"op_xor\""),
          "XOR infix operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_13_canonical_operator_id\":\"sb.operator.xor\""),
          "XOR infix canonical operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_13_expression_surface_ids\":[\"SBSQL-ACBDFA33C8D8\""),
          "XOR infix surface id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_14_operator_id\":\"op_xor\""),
          "sb.operator.xor call operator id missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_14_canonical_operator_id\":\"sb.operator.xor\""),
          "sb.operator.xor canonical operator id missing");
  Require(Contains(artifacts.envelope.payload, "\"projection_14_expression_surface_ids\":[\"SBSQL-ACBDFA33C8D8\""),
          "sb.operator.xor surface id missing");
  Require(!Contains(artifacts.envelope.payload, "SELECT TRUE AND"),
          "extended operator scalar projection payload embedded source SQL text");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admission.admitted, "server admission rejected extended operator scalar projection route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI for extended operator scalar projection");
}

void RequireEngineDispatch() {
  const sblr::SblrDispatchRequest request{EngineContext(), EngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept scalar projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to projection API");
  Require(result.api_result.ok, "engine scalar projection API failed");
  Require(result.api_result.operation_id == "query.evaluate_projection",
          "engine scalar projection operation id mismatch");
  Require(result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine scalar projection result kind mismatch");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 4, "engine scalar projection column count mismatch");
  Require(row.fields[0].first == "one" &&
              row.fields[0].second.descriptor.canonical_type_name == "bigint" &&
              row.fields[0].second.encoded_value == "1",
          "engine scalar projection first field mismatch");
  Require(row.fields[1].first == "two" &&
              row.fields[1].second.descriptor.canonical_type_name == "text" &&
              row.fields[1].second.encoded_value == "two",
          "engine scalar projection second field mismatch");
  Require(row.fields[2].first == "empty_value" &&
              row.fields[2].second.descriptor.canonical_type_name == "null" &&
              row.fields[2].second.is_null,
          "engine scalar projection NULL field mismatch");
  Require(row.fields[3].first == "truth" &&
              row.fields[3].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[3].second.encoded_value == "true" &&
              !row.fields[3].second.is_null,
          "engine scalar projection boolean field mismatch");

  const sblr::SblrDispatchRequest false_request{
      EngineContext(), FalseLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto false_result = sblr::DispatchSblrOperation(false_request);
  for (const auto& diagnostic : false_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : false_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(false_result.envelope_validated,
          "engine SBLR FALSE literal envelope did not validate");
  Require(false_result.accepted, "engine SBLR dispatch did not accept FALSE literal");
  Require(false_result.dispatched_to_api,
          "engine SBLR dispatch did not route FALSE literal to projection API");
  Require(false_result.api_result.ok, "engine FALSE literal scalar projection API failed");
  Require(false_result.api_result.result_shape.rows.size() == 1,
          "engine FALSE literal scalar projection did not produce one row");
  const auto& false_row = false_result.api_result.result_shape.rows.front();
  Require(false_row.fields.size() == 1,
          "engine FALSE literal scalar projection column count mismatch");
  Require(false_row.fields[0].first == "falsehood" &&
              false_row.fields[0].second.descriptor.canonical_type_name == "boolean" &&
              false_row.fields[0].second.encoded_value == "false" &&
              !false_row.fields[0].second.is_null,
          "engine FALSE literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest decimal_request{
      EngineContext(), DecimalLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto decimal_result = sblr::DispatchSblrOperation(decimal_request);
  for (const auto& diagnostic : decimal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : decimal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(decimal_result.envelope_validated,
          "engine decimal scalar projection envelope did not validate");
  Require(decimal_result.accepted, "engine decimal scalar projection dispatch rejected");
  Require(decimal_result.dispatched_to_api,
          "engine decimal scalar projection did not route to projection API");
  Require(decimal_result.api_result.ok, "engine decimal scalar projection API failed");
  Require(decimal_result.api_result.operation_id == "query.evaluate_projection",
          "engine decimal scalar projection operation id mismatch");
  Require(decimal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine decimal scalar projection result kind mismatch");
  Require(decimal_result.api_result.result_shape.rows.size() == 1,
          "engine decimal scalar projection did not produce one row");
  const auto& decimal_row = decimal_result.api_result.result_shape.rows.front();
  Require(decimal_row.fields.size() == 1,
          "engine decimal scalar projection column count mismatch");
  Require(decimal_row.fields[0].first == "decimal_value" &&
              decimal_row.fields[0].second.descriptor.canonical_type_name == "numeric" &&
              decimal_row.fields[0].second.encoded_value == "12.34" &&
              !decimal_row.fields[0].second.is_null,
          "engine decimal scalar projection field mismatch");

  const sblr::SblrDispatchRequest uint_literal_request{
      EngineContext(), UintLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto uint_literal_result = sblr::DispatchSblrOperation(uint_literal_request);
  for (const auto& diagnostic : uint_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : uint_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(uint_literal_result.envelope_validated,
          "engine uint literal scalar projection envelope did not validate");
  Require(uint_literal_result.accepted,
          "engine uint literal scalar projection dispatch rejected");
  Require(uint_literal_result.dispatched_to_api,
          "engine uint literal scalar projection did not route to projection API");
  Require(uint_literal_result.api_result.ok,
          "engine uint literal scalar projection API failed");
  Require(uint_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine uint literal scalar projection operation id mismatch");
  Require(uint_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine uint literal scalar projection result kind mismatch");
  Require(uint_literal_result.api_result.result_shape.rows.size() == 1,
          "engine uint literal scalar projection did not produce one row");
  const auto& uint_literal_row = uint_literal_result.api_result.result_shape.rows.front();
  Require(uint_literal_row.fields.size() == 1,
          "engine uint literal scalar projection column count mismatch");
  Require(uint_literal_row.fields[0].first == "uint_value" &&
              uint_literal_row.fields[0].second.descriptor.canonical_type_name == "uint64" &&
              uint_literal_row.fields[0].second.encoded_value == "42" &&
              !uint_literal_row.fields[0].second.is_null,
          "engine uint literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest int128_literal_request{
      EngineContext(), Int128LiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto int128_literal_result = sblr::DispatchSblrOperation(int128_literal_request);
  for (const auto& diagnostic : int128_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : int128_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(int128_literal_result.envelope_validated,
          "engine int128 literal scalar projection envelope did not validate");
  Require(int128_literal_result.accepted,
          "engine int128 literal scalar projection dispatch rejected");
  Require(int128_literal_result.dispatched_to_api,
          "engine int128 literal scalar projection did not route to projection API");
  Require(int128_literal_result.api_result.ok,
          "engine int128 literal scalar projection API failed");
  Require(int128_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine int128 literal scalar projection operation id mismatch");
  Require(int128_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine int128 literal scalar projection result kind mismatch");
  Require(int128_literal_result.api_result.result_shape.rows.size() == 1,
          "engine int128 literal scalar projection did not produce one row");
  const auto& int128_literal_row =
      int128_literal_result.api_result.result_shape.rows.front();
  Require(int128_literal_row.fields.size() == 1,
          "engine int128 literal scalar projection column count mismatch");
  Require(int128_literal_row.fields[0].first == "int128_value" &&
              int128_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "int128" &&
              int128_literal_row.fields[0].second.encoded_value == "123" &&
              !int128_literal_row.fields[0].second.is_null,
          "engine int128 literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest uint128_literal_request{
      EngineContext(), Uint128LiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto uint128_literal_result = sblr::DispatchSblrOperation(uint128_literal_request);
  for (const auto& diagnostic : uint128_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : uint128_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(uint128_literal_result.envelope_validated,
          "engine uint128 literal scalar projection envelope did not validate");
  Require(uint128_literal_result.accepted,
          "engine uint128 literal scalar projection dispatch rejected");
  Require(uint128_literal_result.dispatched_to_api,
          "engine uint128 literal scalar projection did not route to projection API");
  Require(uint128_literal_result.api_result.ok,
          "engine uint128 literal scalar projection API failed");
  Require(uint128_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine uint128 literal scalar projection operation id mismatch");
  Require(uint128_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine uint128 literal scalar projection result kind mismatch");
  Require(uint128_literal_result.api_result.result_shape.rows.size() == 1,
          "engine uint128 literal scalar projection did not produce one row");
  const auto& uint128_literal_row =
      uint128_literal_result.api_result.result_shape.rows.front();
  Require(uint128_literal_row.fields.size() == 1,
          "engine uint128 literal scalar projection column count mismatch");
  Require(uint128_literal_row.fields[0].first == "uint128_value" &&
              uint128_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "uint128" &&
              uint128_literal_row.fields[0].second.encoded_value == "123" &&
              !uint128_literal_row.fields[0].second.is_null,
          "engine uint128 literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest real128_literal_request{
      EngineContext(), Real128LiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto real128_literal_result = sblr::DispatchSblrOperation(real128_literal_request);
  for (const auto& diagnostic : real128_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : real128_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(real128_literal_result.envelope_validated,
          "engine real128 literal scalar projection envelope did not validate");
  Require(real128_literal_result.accepted,
          "engine real128 literal scalar projection dispatch rejected");
  Require(real128_literal_result.dispatched_to_api,
          "engine real128 literal scalar projection did not route to projection API");
  Require(real128_literal_result.api_result.ok,
          "engine real128 literal scalar projection API failed");
  Require(real128_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine real128 literal scalar projection operation id mismatch");
  Require(real128_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine real128 literal scalar projection result kind mismatch");
  Require(real128_literal_result.api_result.result_shape.rows.size() == 1,
          "engine real128 literal scalar projection did not produce one row");
  const auto& real128_literal_row =
      real128_literal_result.api_result.result_shape.rows.front();
  Require(real128_literal_row.fields.size() == 1,
          "engine real128 literal scalar projection column count mismatch");
  Require(real128_literal_row.fields[0].first == "real128_value" &&
              real128_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "real128" &&
              real128_literal_row.fields[0].second.encoded_value == "1.25" &&
              !real128_literal_row.fields[0].second.is_null,
          "engine real128 literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest float_literal_request{
      EngineContext(), FloatLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto float_literal_result = sblr::DispatchSblrOperation(float_literal_request);
  for (const auto& diagnostic : float_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : float_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(float_literal_result.envelope_validated,
          "engine float literal scalar projection envelope did not validate");
  Require(float_literal_result.accepted,
          "engine float literal scalar projection dispatch rejected");
  Require(float_literal_result.dispatched_to_api,
          "engine float literal scalar projection did not route to projection API");
  Require(float_literal_result.api_result.ok,
          "engine float literal scalar projection API failed");
  Require(float_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine float literal scalar projection operation id mismatch");
  Require(float_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine float literal scalar projection result kind mismatch");
  Require(float_literal_result.api_result.result_shape.rows.size() == 1,
          "engine float literal scalar projection did not produce one row");
  const auto& float_literal_row = float_literal_result.api_result.result_shape.rows.front();
  Require(float_literal_row.fields.size() == 1,
          "engine float literal scalar projection column count mismatch");
  Require(float_literal_row.fields[0].first == "float_value" &&
              float_literal_row.fields[0].second.descriptor.canonical_type_name == "numeric" &&
              float_literal_row.fields[0].second.encoded_value == "1e2" &&
              !float_literal_row.fields[0].second.is_null,
          "engine float literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest binary_literal_request{
      EngineContext(), BinaryLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto binary_literal_result = sblr::DispatchSblrOperation(binary_literal_request);
  for (const auto& diagnostic : binary_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : binary_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(binary_literal_result.envelope_validated,
          "engine binary literal scalar projection envelope did not validate");
  Require(binary_literal_result.accepted,
          "engine binary literal scalar projection dispatch rejected");
  Require(binary_literal_result.dispatched_to_api,
          "engine binary literal scalar projection did not route to projection API");
  Require(binary_literal_result.api_result.ok,
          "engine binary literal scalar projection API failed");
  Require(binary_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine binary literal scalar projection operation id mismatch");
  Require(binary_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine binary literal scalar projection result kind mismatch");
  Require(binary_literal_result.api_result.result_shape.rows.size() == 1,
          "engine binary literal scalar projection did not produce one row");
  const auto& binary_literal_row = binary_literal_result.api_result.result_shape.rows.front();
  Require(binary_literal_row.fields.size() == 1,
          "engine binary literal scalar projection column count mismatch");
  Require(binary_literal_row.fields[0].first == "binary_value" &&
              binary_literal_row.fields[0].second.descriptor.canonical_type_name == "binary" &&
              binary_literal_row.fields[0].second.encoded_value == "00ff10" &&
              !binary_literal_row.fields[0].second.is_null,
          "engine binary literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest uuid_literal_request{
      EngineContext(), UuidLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto uuid_literal_result = sblr::DispatchSblrOperation(uuid_literal_request);
  for (const auto& diagnostic : uuid_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : uuid_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(uuid_literal_result.envelope_validated,
          "engine UUID literal scalar projection envelope did not validate");
  Require(uuid_literal_result.accepted,
          "engine UUID literal scalar projection dispatch rejected");
  Require(uuid_literal_result.dispatched_to_api,
          "engine UUID literal scalar projection did not route to projection API");
  Require(uuid_literal_result.api_result.ok,
          "engine UUID literal scalar projection API failed");
  Require(uuid_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine UUID literal scalar projection operation id mismatch");
  Require(uuid_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine UUID literal scalar projection result kind mismatch");
  Require(uuid_literal_result.api_result.result_shape.rows.size() == 1,
          "engine UUID literal scalar projection did not produce one row");
  const auto& uuid_literal_row = uuid_literal_result.api_result.result_shape.rows.front();
  Require(uuid_literal_row.fields.size() == 1,
          "engine UUID literal scalar projection column count mismatch");
  Require(uuid_literal_row.fields[0].first == "uuid_value" &&
              uuid_literal_row.fields[0].second.descriptor.canonical_type_name == "uuid" &&
              uuid_literal_row.fields[0].second.encoded_value ==
                  "550e8400-e29b-41d4-a716-446655440000" &&
              !uuid_literal_row.fields[0].second.is_null,
          "engine UUID literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest date_literal_request{
      EngineContext(), DateLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto date_literal_result = sblr::DispatchSblrOperation(date_literal_request);
  for (const auto& diagnostic : date_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : date_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(date_literal_result.envelope_validated,
          "engine DATE literal scalar projection envelope did not validate");
  Require(date_literal_result.accepted,
          "engine DATE literal scalar projection dispatch rejected");
  Require(date_literal_result.dispatched_to_api,
          "engine DATE literal scalar projection did not route to projection API");
  Require(date_literal_result.api_result.ok,
          "engine DATE literal scalar projection API failed");
  Require(date_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine DATE literal scalar projection operation id mismatch");
  Require(date_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine DATE literal scalar projection result kind mismatch");
  Require(date_literal_result.api_result.result_shape.rows.size() == 1,
          "engine DATE literal scalar projection did not produce one row");
  const auto& date_literal_row = date_literal_result.api_result.result_shape.rows.front();
  Require(date_literal_row.fields.size() == 1,
          "engine DATE literal scalar projection column count mismatch");
  Require(date_literal_row.fields[0].first == "date_value" &&
              date_literal_row.fields[0].second.descriptor.canonical_type_name == "date" &&
              date_literal_row.fields[0].second.encoded_value == "2026-05-14" &&
              !date_literal_row.fields[0].second.is_null,
          "engine DATE literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest time_literal_request{
      EngineContext(), TimeLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto time_literal_result = sblr::DispatchSblrOperation(time_literal_request);
  for (const auto& diagnostic : time_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : time_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(time_literal_result.envelope_validated,
          "engine TIME literal scalar projection envelope did not validate");
  Require(time_literal_result.accepted,
          "engine TIME literal scalar projection dispatch rejected");
  Require(time_literal_result.dispatched_to_api,
          "engine TIME literal scalar projection did not route to projection API");
  Require(time_literal_result.api_result.ok,
          "engine TIME literal scalar projection API failed");
  Require(time_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine TIME literal scalar projection operation id mismatch");
  Require(time_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine TIME literal scalar projection result kind mismatch");
  Require(time_literal_result.api_result.result_shape.rows.size() == 1,
          "engine TIME literal scalar projection did not produce one row");
  const auto& time_literal_row = time_literal_result.api_result.result_shape.rows.front();
  Require(time_literal_row.fields.size() == 1,
          "engine TIME literal scalar projection column count mismatch");
  Require(time_literal_row.fields[0].first == "time_value" &&
              time_literal_row.fields[0].second.descriptor.canonical_type_name == "time" &&
              time_literal_row.fields[0].second.encoded_value == "14:23:45" &&
              !time_literal_row.fields[0].second.is_null,
          "engine TIME literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest timestamp_literal_request{
      EngineContext(), TimestampLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto timestamp_literal_result = sblr::DispatchSblrOperation(timestamp_literal_request);
  for (const auto& diagnostic : timestamp_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : timestamp_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(timestamp_literal_result.envelope_validated,
          "engine TIMESTAMP literal scalar projection envelope did not validate");
  Require(timestamp_literal_result.accepted,
          "engine TIMESTAMP literal scalar projection dispatch rejected");
  Require(timestamp_literal_result.dispatched_to_api,
          "engine TIMESTAMP literal scalar projection did not route to projection API");
  Require(timestamp_literal_result.api_result.ok,
          "engine TIMESTAMP literal scalar projection API failed");
  Require(timestamp_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine TIMESTAMP literal scalar projection operation id mismatch");
  Require(timestamp_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine TIMESTAMP literal scalar projection result kind mismatch");
  Require(timestamp_literal_result.api_result.result_shape.rows.size() == 1,
          "engine TIMESTAMP literal scalar projection did not produce one row");
  const auto& timestamp_literal_row =
      timestamp_literal_result.api_result.result_shape.rows.front();
  Require(timestamp_literal_row.fields.size() == 1,
          "engine TIMESTAMP literal scalar projection column count mismatch");
  Require(timestamp_literal_row.fields[0].first == "timestamp_value" &&
              timestamp_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "timestamp" &&
              timestamp_literal_row.fields[0].second.encoded_value ==
                  "2026-05-14T14:23:45" &&
              !timestamp_literal_row.fields[0].second.is_null,
          "engine TIMESTAMP literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest interval_literal_request{
      EngineContext(), IntervalLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto interval_literal_result = sblr::DispatchSblrOperation(interval_literal_request);
  for (const auto& diagnostic : interval_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : interval_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(interval_literal_result.envelope_validated,
          "engine INTERVAL literal scalar projection envelope did not validate");
  Require(interval_literal_result.accepted,
          "engine INTERVAL literal scalar projection dispatch rejected");
  Require(interval_literal_result.dispatched_to_api,
          "engine INTERVAL literal scalar projection did not route to projection API");
  Require(interval_literal_result.api_result.ok,
          "engine INTERVAL literal scalar projection API failed");
  Require(interval_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine INTERVAL literal scalar projection operation id mismatch");
  Require(interval_literal_result.api_result.result_shape.result_kind == "scalar_projection_rows",
          "engine INTERVAL literal scalar projection result kind mismatch");
  Require(interval_literal_result.api_result.result_shape.rows.size() == 1,
          "engine INTERVAL literal scalar projection did not produce one row");
  const auto& interval_literal_row =
      interval_literal_result.api_result.result_shape.rows.front();
  Require(interval_literal_row.fields.size() == 1,
          "engine INTERVAL literal scalar projection column count mismatch");
  Require(interval_literal_row.fields[0].first == "interval_value" &&
              interval_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "interval" &&
              interval_literal_row.fields[0].second.encoded_value == "1 day" &&
              !interval_literal_row.fields[0].second.is_null,
          "engine INTERVAL literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest document_literal_request{
      EngineContext(), DocumentLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto document_literal_result =
      sblr::DispatchSblrOperation(document_literal_request);
  for (const auto& diagnostic : document_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : document_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(document_literal_result.envelope_validated,
          "engine DOCUMENT literal scalar projection envelope did not validate");
  Require(document_literal_result.accepted,
          "engine DOCUMENT literal scalar projection dispatch rejected");
  Require(document_literal_result.dispatched_to_api,
          "engine DOCUMENT literal scalar projection did not route to projection API");
  Require(document_literal_result.api_result.ok,
          "engine DOCUMENT literal scalar projection API failed");
  Require(document_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine DOCUMENT literal scalar projection operation id mismatch");
  Require(document_literal_result.api_result.result_shape.result_kind ==
              "scalar_projection_rows",
          "engine DOCUMENT literal scalar projection result kind mismatch");
  Require(document_literal_result.api_result.result_shape.rows.size() == 1,
          "engine DOCUMENT literal scalar projection did not produce one row");
  const auto& document_literal_row =
      document_literal_result.api_result.result_shape.rows.front();
  Require(document_literal_row.fields.size() == 1,
          "engine DOCUMENT literal scalar projection column count mismatch");
  Require(document_literal_row.fields[0].first == "document_value" &&
              document_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "document" &&
              document_literal_row.fields[0].second.encoded_value == "{\"a\":1}" &&
              !document_literal_row.fields[0].second.is_null,
          "engine DOCUMENT literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest json_literal_request{
      EngineContext(), JsonLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto json_literal_result =
      sblr::DispatchSblrOperation(json_literal_request);
  for (const auto& diagnostic : json_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : json_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(json_literal_result.envelope_validated,
          "engine JSON literal scalar projection envelope did not validate");
  Require(json_literal_result.accepted,
          "engine JSON literal scalar projection dispatch rejected");
  Require(json_literal_result.dispatched_to_api,
          "engine JSON literal scalar projection did not route to projection API");
  Require(json_literal_result.api_result.ok,
          "engine JSON literal scalar projection API failed");
  Require(json_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine JSON literal scalar projection operation id mismatch");
  Require(json_literal_result.api_result.result_shape.result_kind ==
              "scalar_projection_rows",
          "engine JSON literal scalar projection result kind mismatch");
  Require(json_literal_result.api_result.result_shape.rows.size() == 1,
          "engine JSON literal scalar projection did not produce one row");
  const auto& json_literal_row =
      json_literal_result.api_result.result_shape.rows.front();
  Require(json_literal_row.fields.size() == 1,
          "engine JSON literal scalar projection column count mismatch");
  Require(json_literal_row.fields[0].first == "json_value" &&
              json_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "json_document" &&
              json_literal_row.fields[0].second.encoded_value == "{\"a\":1}" &&
              !json_literal_row.fields[0].second.is_null,
          "engine JSON literal scalar projection field mismatch");

  const sblr::SblrDispatchRequest vector_literal_request{
      EngineContext(), VectorLiteralEngineEnvelope(), api::EngineApiRequest{}};
  const auto vector_literal_result =
      sblr::DispatchSblrOperation(vector_literal_request);
  for (const auto& diagnostic : vector_literal_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : vector_literal_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(vector_literal_result.envelope_validated,
          "engine VECTOR literal scalar projection envelope did not validate");
  Require(vector_literal_result.accepted,
          "engine VECTOR literal scalar projection dispatch rejected");
  Require(vector_literal_result.dispatched_to_api,
          "engine VECTOR literal scalar projection did not route to projection API");
  Require(vector_literal_result.api_result.ok,
          "engine VECTOR literal scalar projection API failed");
  Require(vector_literal_result.api_result.operation_id == "query.evaluate_projection",
          "engine VECTOR literal scalar projection operation id mismatch");
  Require(vector_literal_result.api_result.result_shape.result_kind ==
              "scalar_projection_rows",
          "engine VECTOR literal scalar projection result kind mismatch");
  Require(vector_literal_result.api_result.result_shape.rows.size() == 1,
          "engine VECTOR literal scalar projection did not produce one row");
  const auto& vector_literal_row =
      vector_literal_result.api_result.result_shape.rows.front();
  Require(vector_literal_row.fields.size() == 1,
          "engine VECTOR literal scalar projection column count mismatch");
  Require(vector_literal_row.fields[0].first == "vector_value" &&
              vector_literal_row.fields[0].second.descriptor.canonical_type_name ==
                  "dense_vector" &&
              vector_literal_row.fields[0].second.encoded_value == "[1,2,3]" &&
              !vector_literal_row.fields[0].second.is_null,
          "engine VECTOR literal scalar projection field mismatch");

}

bool ApiHasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool ApiHasDiagnosticDetail(const api::EngineApiResult& result,
                            std::string_view code,
                            std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code && diagnostic.detail == detail) return true;
  }
  return false;
}

void RequireEngineSbsfc013DocumentCollectionProjectionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), Sbsfc013DocumentCollectionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "SBSFC-013 document collection projection envelope did not validate");
  Require(result.accepted,
          "SBSFC-013 document collection projection route was not accepted");
  Require(result.dispatched_to_api,
          "SBSFC-013 document collection projection did not route to projection API");
  Require(result.api_result.ok,
          "SBSFC-013 document collection projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-013 document collection projection did not produce one row");

  struct ExpectedField {
    const char* surface_id;
    const char* descriptor;
    const char* encoded_value;
    bool is_null;
  };
  const ExpectedField expected[] = {
      {"SBSQL-FE519C1C20F6", "character", "object", false},
      {"SBSQL-4324775DBCA0", "character", "array", false},
      {"SBSQL-83995B2BC266", "json_document", "42", false},
      {"SBSQL-35F0E9FF7755", "json_document", "", true},
      {"SBSQL-7EE8FAD14C5A", "boolean", "1", false},
      {"SBSQL-3E3BA120C541", "boolean", "0", false},
      {"SBSQL-1342A8B02022", "boolean", "1", false},
      {"SBSQL-5E705E2E1462", "json_document", "7", false},
      {"SBSQL-5B765753ADEC", "json_document", R"("bird")", false},
      {"SBSQL-68134CF09B70", "json_document", "", true},
      {"SBSQL-09BA0A3A71DB", "json_document", R"({"b":2})", false},
      {"SBSQL-DC8507F9B9C5", "json_document", "[1,2]", false},
      {"SBSQL-03D2E8D0B9AE", "json_document", R"({"a":1,"b":2})", false},
      {"SBSQL-7AA44FB9077C", "json_document", R"({"a":3})", false},
      {"SBSQL-E062D64D0C8F", "json_document", R"({"b":2})", false},
      {"SBSQL-B4CF3C70B1D3", "json_document", R"({"a":1,"b":2})", false},
      {"SBSQL-14C23DAA9C77", "json_document", R"({"a":4})", false},
      {"SBSQL-23EEDCBB8140", "json_document", R"({"a":1})", false},
      {"SBSQL-AF045C026980", "json_document", R"({"a":1,"b":2})", false},
      {"SBSQL-2E230404921F", "json_document", R"({"a":1})", false},
      {"SBSQL-C55F7ADBD13B", "json_document", R"({"a":5})", false},
      {"SBSQL-723E5CADA519", "json_document", R"({"a":1})", false},
      {"SBSQL-0A060E6427B3", "uint64", "3", false},
      {"SBSQL-DEDF07FAB7F3", "uint64", "0", false},
      {"SBSQL-5BCF9869AA4C", "uint64", "3", false},
      {"SBSQL-3CEB816A1165", "uint64", "0", false},
      {"SBSQL-7B99FF977C66", "json_document", R"([1,"bird",null])", false},
      {"SBSQL-4640811DBAC8", "json_document", "[]", false},
      {"SBSQL-E2DFF93CA59C", "json_document", R"({"a":1,"b":"bird"})", false},
      {"SBSQL-36FBFED38C80", "json_document", R"([1,"bird",null])", false},
      {"SBSQL-34E68EB56EDC", "json_document", R"({"a":1})", false},
      {"SBSQL-CB837AAEBEAD", "json_document", R"("bird")", false},
      {"SBSQL-F0AB18F7417B", "json_document", "null", false},
      {"SBSQL-4119D041403C", "json_document", R"({"a":1})", false},
      {"SBSQL-88E66066EBC7", "json_document", "7", false},
      {"SBSQL-048498BB9A7F", "character", "object", false},
      {"SBSQL-2F78C18D9292", "character", "array", false},
      {"SBSQL-36FF2B0254C0", "character", "boolean", false},
      {"SBSQL-0926D8F4ABD5", "json_document", R"({"a":1})", false},
      {"SBSQL-551339ECEE75", "json_document", R"({"a":1})", false},
      {"SBSQL-90E1BC86D62F", "json_document", R"(["a","b"])", false},
      {"SBSQL-A05313B740CC", "json_document", R"(["a"])", false},
      {"SBSQL-03447BA4EB25", "json_document", R"(["a","b"])", false},
      {"SBSQL-EB982B4F95B3", "json_document", R"(["a"])", false},
      {"SBSQL-0390232C7296", "json_document", "[1,2]", false},
      {"SBSQL-7819B29C7AB5", "json_document", "[1,2]", false},
      {"SBSQL-4D97B9EA482B", "json_document", R"(["1","2"])", false},
      {"SBSQL-76883ECD3648", "json_document", R"([{"key":"a","value":1}])", false},
      {"SBSQL-18521C5D03B8", "json_document", R"([{"key":"a","value":1}])", false},
      {"SBSQL-4B7CDEB23364", "json_document", R"([{"key":"a","value":"1"}])", false},
      {"SBSQL-6AF2FB9EDEB9", "json_document", R"([{"key":"a","value":"1"}])", false},
      {"SBSQL-CE5BD771D075", "json_document", R"({"a":1,"b":2})", false},
      {"SBSQL-429DB32D5CC2", "json_document", R"({"a":1})", false},
      {"SBSQL-58F6D7F43DA6", "boolean", "1", false},
      {"SBSQL-D4C29991D99B", "boolean", "0", false},
      {"SBSQL-9A4AB48B76FD", "boolean", "1", false},
      {"SBSQL-7C4821112F94", "boolean", "0", false},
      {"SBSQL-436880E1F3F7", "json_document", R"({"b":2})", false},
      {"SBSQL-EA5E00825D4D", "json_document", "7", false},
      {"SBSQL-A1C65D80CE68", "json_document", "[7]", false},
      {"SBSQL-B64295F1B742", "json_document", R"("bird")", false},
      {"SBSQL-6910DED90537", "character", "{\n  \"a\": 1\n}", false},
      {"SBSQL-4CFCAC326BFB", "character", "{\n  \"a\": 1\n}", false},
      {"SBSQL-5157364BCB20", "json_document", R"({"b":2})", false},
      {"SBSQL-98D9B54A7630", "json_document", R"({"a":1})", false},
  };

  const auto& row = result.api_result.result_shape.rows.front();
  const std::size_t expected_count = sizeof(expected) / sizeof(expected[0]);
  Require(row.fields.size() == expected_count,
          "SBSFC-013 document collection projection column count mismatch");
  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& actual = row.fields[index];
    const auto& wanted = expected[index];
    const std::string label = wanted.surface_id;
    Require(actual.first == wanted.surface_id,
            label + " SBSFC-013 projection label mismatch");
    Require(actual.second.descriptor.canonical_type_name == wanted.descriptor,
            label + " SBSFC-013 projection descriptor mismatch");
    Require(actual.second.is_null == wanted.is_null,
            label + " SBSFC-013 projection nullability mismatch");
    if (!wanted.is_null) {
      Require(actual.second.encoded_value == wanted.encoded_value,
              label + " SBSFC-013 projection value mismatch");
    }
  }

  const sblr::SblrDispatchRequest failure_request{
      EngineContext(), Sbsfc013DocumentCollectionProjectionFailureEngineEnvelope(),
      api::EngineApiRequest{}};
  const auto failure = sblr::DispatchSblrOperation(failure_request);
  for (const auto& diagnostic : failure.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : failure.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(failure.envelope_validated,
          "SBSQL-3217FFB2F3BD SBSFC-013 projection failure envelope did not validate");
  Require(failure.accepted,
          "SBSQL-3217FFB2F3BD SBSFC-013 projection failure route was not accepted");
  Require(failure.dispatched_to_api,
          "SBSQL-3217FFB2F3BD SBSFC-013 projection failure did not route to API");
  Require(!failure.api_result.ok,
          "SBSQL-3217FFB2F3BD SBSFC-013 projection failure unexpectedly succeeded");
  Require(ApiHasDiagnostic(failure.api_result, "SB_DIAG_FUNCTION_INVALID_INPUT"),
          "SBSQL-3217FFB2F3BD SBSFC-013 projection diagnostic mismatch");
}

void RequireEngineFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), FunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR function envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept function scalar projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route function projection");
  Require(result.api_result.ok, "engine function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 1, "engine function scalar projection column count mismatch");
  Require(row.fields.front().first == "cot_value", "engine function scalar projection alias mismatch");
  const auto& value = row.fields.front().second;
  Require(value.descriptor.canonical_type_name == "real64",
          "engine function scalar projection descriptor mismatch");
  Require(!value.is_null, "engine function scalar projection unexpectedly returned NULL");
  Require(std::fabs(std::stod(value.encoded_value) - 1.0) < 1e-6,
          "engine function scalar projection value mismatch");

  const sblr::SblrDispatchRequest invalid_request{
      EngineContext(), FunctionProjectionEngineEnvelope("text", "not_numeric"), api::EngineApiRequest{}};
  const auto invalid_result = sblr::DispatchSblrOperation(invalid_request);
  Require(invalid_result.envelope_validated,
          "engine SBLR invalid-input function envelope did not validate");
  Require(invalid_result.accepted,
          "engine SBLR dispatch did not accept invalid-input function projection for engine diagnostic");
  Require(!invalid_result.api_result.ok,
          "engine invalid-input function scalar projection unexpectedly succeeded");
  Require(ApiHasDiagnostic(invalid_result.api_result, "SB_DIAG_FUNCTION_INVALID_INPUT"),
          "engine invalid-input function scalar projection diagnostic mismatch");
}

void RequireEngineNumericFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), NumericFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR numeric function envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept numeric function projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route numeric function projection");
  Require(result.api_result.ok, "engine numeric function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine numeric function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 11, "engine numeric function scalar projection column count mismatch");
  Require(row.fields[0].first == "pi_value" &&
              row.fields[0].second.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(row.fields[0].second.encoded_value) - std::acos(-1.0)) < 1e-6,
          "engine pi function scalar projection mismatch");
  Require(row.fields[1].first == "cbrt_value" &&
              row.fields[1].second.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(row.fields[1].second.encoded_value) - 3.0) < 1e-6,
          "engine cbrt function scalar projection mismatch");
  Require(row.fields[2].first == "deg_value" &&
              row.fields[2].second.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(row.fields[2].second.encoded_value) - 180.0) < 1e-6,
          "engine degrees function scalar projection mismatch");
  Require(row.fields[3].first == "rad_value" &&
              row.fields[3].second.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(row.fields[3].second.encoded_value) - std::acos(-1.0)) < 1e-6,
          "engine radians function scalar projection mismatch");
  Require(row.fields[4].first == "cotd_value" &&
              row.fields[4].second.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(row.fields[4].second.encoded_value) - 1.0) < 1e-6,
          "engine cotd function scalar projection mismatch");
  Require(row.fields[5].first == "div_value" &&
              row.fields[5].second.descriptor.canonical_type_name == "int64" &&
              row.fields[5].second.encoded_value == "2",
          "engine div function scalar projection mismatch");
  Require(row.fields[6].first == "fact_value" &&
              row.fields[6].second.descriptor.canonical_type_name == "int64" &&
              row.fields[6].second.encoded_value == "120",
          "engine factorial function scalar projection mismatch");
  Require(row.fields[7].first == "gcd_value" &&
              row.fields[7].second.descriptor.canonical_type_name == "int64" &&
              row.fields[7].second.encoded_value == "6",
          "engine gcd function scalar projection mismatch");
  Require(row.fields[8].first == "lcm_value" &&
              row.fields[8].second.descriptor.canonical_type_name == "int64" &&
              row.fields[8].second.encoded_value == "24",
          "engine lcm function scalar projection mismatch");
  Require(row.fields[9].first == "bucket_value" &&
              row.fields[9].second.descriptor.canonical_type_name == "int64" &&
              row.fields[9].second.encoded_value == "3",
          "engine width_bucket function scalar projection mismatch");
  Require(row.fields[10].first == "bit_count_value" &&
              row.fields[10].second.descriptor.canonical_type_name == "int64" &&
              row.fields[10].second.encoded_value == "3",
          "engine bit_count function scalar projection mismatch");
}

void RequireEngineAdditionalNumericFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), AdditionalNumericFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR additional numeric function envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept additional numeric function projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route additional numeric function projection");
  Require(result.api_result.ok, "engine additional numeric function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine additional numeric function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 22,
          std::string("engine additional numeric function scalar projection column count mismatch observed=") +
              std::to_string(row.fields.size()));
  Require(row.fields[0].first == "abs_value" &&
              row.fields[0].second.descriptor.canonical_type_name == "int64" &&
              row.fields[0].second.encoded_value == "5",
          "engine abs function scalar projection mismatch");
  Require(row.fields[1].first == "ceil_value" &&
              std::fabs(std::stod(row.fields[1].second.encoded_value) - 2.0) < 1e-6,
          "engine ceil function scalar projection mismatch");
  Require(row.fields[2].first == "floor_value" &&
              std::fabs(std::stod(row.fields[2].second.encoded_value) - 1.0) < 1e-6,
          "engine floor function scalar projection mismatch");
  Require(row.fields[3].first == "round_value" &&
              std::fabs(std::stod(row.fields[3].second.encoded_value) - 2.0) < 1e-6,
          "engine round function scalar projection mismatch");
  Require(row.fields[4].first == "sqrt_value" &&
              std::fabs(std::stod(row.fields[4].second.encoded_value) - 2.0) < 1e-6,
          "engine sqrt function scalar projection mismatch");
  Require(row.fields[5].first == "power_value" &&
              std::fabs(std::stod(row.fields[5].second.encoded_value) - 1024.0) < 1e-6,
          "engine power function scalar projection mismatch");
  Require(row.fields[6].first == "sin_value" &&
              std::fabs(std::stod(row.fields[6].second.encoded_value) - 0.0) < 1e-6,
          "engine sin function scalar projection mismatch");
  Require(row.fields[7].first == "cos_value" &&
              std::fabs(std::stod(row.fields[7].second.encoded_value) - 1.0) < 1e-6,
          "engine cos function scalar projection mismatch");
  Require(row.fields[8].first == "tan_value" &&
              std::fabs(std::stod(row.fields[8].second.encoded_value) - 0.0) < 1e-6,
          "engine tan function scalar projection mismatch");
  Require(row.fields[9].first == "exp_value" &&
              std::fabs(std::stod(row.fields[9].second.encoded_value) - 1.0) < 1e-6,
          "engine exp function scalar projection mismatch");
  Require(row.fields[10].first == "ln_value" &&
              std::fabs(std::stod(row.fields[10].second.encoded_value) - 0.0) < 1e-6,
          "engine ln function scalar projection mismatch");
  Require(row.fields[11].first == "log10_value" &&
              std::fabs(std::stod(row.fields[11].second.encoded_value) - 2.0) < 1e-6,
          "engine log10 function scalar projection mismatch");
  Require(row.fields[12].first == "log_value" &&
              std::fabs(std::stod(row.fields[12].second.encoded_value) - 3.0) < 1e-6,
          "engine log function scalar projection mismatch");
  Require(row.fields[13].first == "trunc_value" &&
              std::fabs(std::stod(row.fields[13].second.encoded_value) - 1.0) < 1e-6,
          "engine trunc function scalar projection mismatch");
  Require(row.fields[14].first == "truncate_value" &&
              std::fabs(std::stod(row.fields[14].second.encoded_value) - 1.0) < 1e-6,
          "engine truncate function scalar projection mismatch");
  Require(row.fields[15].first == "mod_value" &&
              std::fabs(std::stod(row.fields[15].second.encoded_value) - 1.0) < 1e-6,
          "engine mod function scalar projection mismatch");
  Require(row.fields[16].first == "sign_value" &&
              row.fields[16].second.descriptor.canonical_type_name == "int64" &&
              row.fields[16].second.encoded_value == "1",
          "engine sign function scalar projection mismatch");
  Require(row.fields[17].first == "bit_and_value" &&
              row.fields[17].second.encoded_value == "0",
          "engine bit_and function scalar projection mismatch");
  Require(row.fields[18].first == "bit_or_value" &&
              row.fields[18].second.encoded_value == "255",
          "engine bit_or function scalar projection mismatch");
  Require(row.fields[19].first == "bit_xor_value" &&
              row.fields[19].second.encoded_value == "0",
          "engine bit_xor function scalar projection mismatch");
  Require(row.fields[20].first == "bit_shift_left_value" &&
              row.fields[20].second.encoded_value == "256",
          "engine bit_shift_left function scalar projection mismatch");
  Require(row.fields[21].first == "bit_shift_right_value" &&
              row.fields[21].second.encoded_value == "1",
          "engine bit_shift_right function scalar projection mismatch");
}

void RequireEngineExtendedNumericFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), ExtendedNumericFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR extended numeric function envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept extended numeric function projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route extended numeric function projection");
  Require(result.api_result.ok, "engine extended numeric function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine extended numeric function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 17,
          "engine extended numeric function scalar projection column count mismatch");
  const auto expect_real = [&](std::size_t index,
                               std::string_view name,
                               double expected,
                               double epsilon = 1e-6) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == "real64" &&
                std::fabs(std::stod(row.fields[index].second.encoded_value) - expected) < epsilon,
            std::string("engine extended numeric function scalar projection mismatch: ") +
                std::string(name));
  };
  expect_real(0, "asin_value", std::asin(0.5));
  expect_real(1, "acos_value", std::acos(0.5));
  expect_real(2, "atan_value", std::atan(1.0));
  expect_real(3, "sinh_value", 0.0);
  expect_real(4, "cosh_value", 1.0);
  expect_real(5, "tanh_value", 0.0);
  expect_real(6, "asinh_value", std::asinh(1.0));
  expect_real(7, "acosh_value", 0.0);
  expect_real(8, "atanh_value", std::atanh(0.5));
  expect_real(9, "log2_value", 3.0);
  expect_real(10, "atan2_value", std::atan2(1.0, 1.0));
  expect_real(11, "sind_value", 0.5);
  expect_real(12, "cosd_value", 0.5);
  expect_real(13, "tand_value", 1.0);
  expect_real(14, "asind_value", 30.0);
  expect_real(15, "acosd_value", 60.0);
  expect_real(16, "atand_value", 45.0);
}

void RequireEngineTextJsonFuzzyFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), TextJsonFuzzyFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR text/json/fuzzy function envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept text/json/fuzzy function projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route text/json/fuzzy function projection");
  Require(result.api_result.ok, "engine text/json/fuzzy function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine text/json/fuzzy function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 33,
          "engine text/json/fuzzy function scalar projection column count mismatch");
  Require(row.fields[0].first == "char_len" && row.fields[0].second.encoded_value == "7",
          "engine char_length function scalar projection mismatch");
  Require(row.fields[1].first == "left_value" && row.fields[1].second.encoded_value == "ab",
          "engine left function scalar projection mismatch");
  Require(row.fields[2].first == "right_value" && row.fields[2].second.encoded_value == "ef",
          "engine right function scalar projection mismatch");
  Require(row.fields[3].first == "uuid_value" &&
              row.fields[3].second.descriptor.canonical_type_name == "uuid" &&
              row.fields[3].second.encoded_value == "550e8400-e29b-41d4-a716-446655440000",
          "engine uuid_from_string function scalar projection mismatch");
  Require(row.fields[4].first == "uuid_text" &&
              row.fields[4].second.encoded_value == "550e8400-e29b-41d4-a716-446655440000",
          "engine uuid_to_string function scalar projection mismatch");
  Require(row.fields[5].first == "digest_value" &&
              row.fields[5].second.descriptor.canonical_type_name == "uint64" &&
              row.fields[5].second.encoded_value == "25347132070217633",
          "engine digest function scalar projection mismatch");
  Require(row.fields[6].first == "jsonb_len" && row.fields[6].second.encoded_value == "3",
          "engine jsonb_array_length function scalar projection mismatch");
  Require(row.fields[7].first == "json_set_value" &&
              row.fields[7].second.encoded_value == R"({"a":1,"b":2})",
          "engine json_set function scalar projection mismatch");
  Require(row.fields[8].first == "json_removed" &&
              row.fields[8].second.encoded_value == R"({"b":2})",
          "engine json_remove function scalar projection mismatch");
  Require(row.fields[9].first == "json_replaced" &&
              row.fields[9].second.encoded_value == R"({"a":4})",
          "engine json_replace function scalar projection mismatch");
  Require(row.fields[10].first == "json_inserted" &&
              row.fields[10].second.encoded_value == R"({"a":1,"b":2})",
          "engine json_insert function scalar projection mismatch");
  Require(row.fields[11].first == "json_array" &&
              row.fields[11].second.encoded_value == R"([1,"bird",null])",
          "engine json_build_array function scalar projection mismatch");
  Require(row.fields[12].first == "json_object" &&
              row.fields[12].second.encoded_value == R"({"a":1,"b":"bird"})",
          "engine json_build_object function scalar projection mismatch");
  Require(row.fields[13].first == "json_text" &&
              row.fields[13].second.encoded_value == R"("bird")",
          "engine to_json function scalar projection mismatch");
  Require(row.fields[14].first == "jsonb_text" && row.fields[14].second.encoded_value == "7",
          "engine to_jsonb function scalar projection mismatch");
  Require(row.fields[15].first == "jsonb_type" && row.fields[15].second.encoded_value == "object",
          "engine jsonb_typeof function scalar projection mismatch");
  Require(row.fields[16].first == "json_type" && row.fields[16].second.encoded_value == "array",
          "engine json_typeof function scalar projection mismatch");
  Require(row.fields[17].first == "json_extracted" &&
              row.fields[17].second.encoded_value == "42",
          "engine json_extract function scalar projection mismatch");
  Require(row.fields[18].first == "json_exists_value" &&
              row.fields[18].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[18].second.encoded_value == "1",
          "engine json_exists function scalar projection mismatch");
  Require(row.fields[19].first == "json_value_value" &&
              row.fields[19].second.encoded_value == R"("bird")",
          "engine json_value function scalar projection mismatch");
  Require(row.fields[20].first == "json_query_value" &&
              row.fields[20].second.encoded_value == "[1,2]",
          "engine json_query function scalar projection mismatch");
  Require(row.fields[21].first == "jsonb_set_value" &&
              row.fields[21].second.encoded_value == R"({"a":5})",
          "engine jsonb_set function scalar projection mismatch");
  Require(row.fields[22].first == "jsonb_array" &&
              row.fields[22].second.encoded_value == R"([1,"bird",null])",
          "engine jsonb_build_array function scalar projection mismatch");
  Require(row.fields[23].first == "jsonb_object" &&
              row.fields[23].second.encoded_value == R"({"a":1})",
          "engine jsonb_build_object function scalar projection mismatch");
  Require(row.fields[24].first == "metaphone_value" && row.fields[24].second.encoded_value == "SM0",
          "engine metaphone function scalar projection mismatch");
  Require(row.fields[25].first == "dmetaphone_value" && row.fields[25].second.encoded_value == "KMP",
          "engine dmetaphone function scalar projection mismatch");
  Require(row.fields[26].first == "dmetaphone_alt_value" &&
              row.fields[26].second.encoded_value == "SMT",
          "engine dmetaphone_alt function scalar projection mismatch");
  Require(row.fields[27].first == "levenshtein_le_value" &&
              row.fields[27].second.encoded_value == "1",
          "engine levenshtein_le function scalar projection mismatch");
  Require(row.fields[28].first == "damerau_value" && row.fields[28].second.encoded_value == "1",
          "engine damerau_levenshtein function scalar projection mismatch");
  Require(row.fields[29].first == "jaro_value" &&
              std::fabs(std::stod(row.fields[29].second.encoded_value) - 0.9444444444444445) < 1e-6,
          "engine jaro_similarity function scalar projection mismatch");
  Require(row.fields[30].first == "jaro_winkler_value" &&
              std::fabs(std::stod(row.fields[30].second.encoded_value) - 0.9611111111111111) < 1e-6,
          "engine jaro_winkler_similarity function scalar projection mismatch");
  Require(row.fields[31].first == "similarity_value" &&
              std::fabs(std::stod(row.fields[31].second.encoded_value) - 1.0) < 1e-6,
          "engine similarity function scalar projection mismatch");
  Require(row.fields[32].first == "word_similarity_value" &&
              std::fabs(std::stod(row.fields[32].second.encoded_value) - 1.0) < 1e-6,
          "engine word_similarity function scalar projection mismatch");
}

void RequireEngineVectorFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), VectorFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR vector function envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept vector function projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route vector function projection");
  Require(result.api_result.ok, "engine vector function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine vector function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 14,
          "engine vector function scalar projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine vector scalar projection mismatch: ") +
                std::string(name));
  };
  const auto expect_real = [&](std::size_t index,
                               std::string_view name,
                               double expected,
                               double epsilon = 1e-12) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == "real64" &&
                std::fabs(std::stod(row.fields[index].second.encoded_value) - expected) < epsilon,
            std::string("engine vector scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "vector_value", "dense_vector", "[1,2.5,-3]");
  expect_text(1, "vector_dims_value", "int64", "3");
  expect_real(2, "vector_norm_value", 5.0);
  expect_real(3, "l2_distance_value", 5.0);
  expect_real(4, "cosine_distance_value", 1.0);
  expect_real(5, "inner_product_value", 32.0);
  expect_real(6, "negative_inner_product_value", -32.0);
  expect_text(7, "hamming_distance_value", "int64", "2");
  expect_text(8, "normalized_vector", "dense_vector", "[0.6,0.8]");
  expect_text(9, "subvector_value", "dense_vector", "[8,7]");
  expect_text(10, "int8_vector_value", "int8_vector", "[-128,-2,0,3,127]");
  expect_text(11, "float16_vector_value", "float16_vector", "[1,0.333251953125,65504]");
  expect_text(12, "vector_sum_value", "dense_vector", "[5,7,9]");
  expect_text(13, "vector_avg_value", "dense_vector", "[2.5,3.5,4.5]");
}

void RequireEngineBinaryCryptoFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), BinaryCryptoFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR binary/crypto function envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept binary/crypto function projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route binary/crypto function projection");
  Require(result.api_result.ok, "engine binary/crypto scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine binary/crypto scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 5,
          "engine binary/crypto scalar projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine binary/crypto scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "encoded_hex", "character", "00ff10");
  expect_text(1, "decoded_hex", "binary", "00ff10");
  expect_text(2, "oracle_decode_value", "text", "two");
  Require(row.fields[3].first == "random_value" &&
              row.fields[3].second.descriptor.canonical_type_name == "real64" &&
              std::fabs(std::stod(row.fields[3].second.encoded_value) - 0.5) < 1e-12,
          "engine deterministic random function scalar projection mismatch");
  expect_text(4, "timezone_value", "character", "UTC");

  for (const auto* function_id : {
           "sb.scalar.md5",
           "sb.scalar.sha1",
           "sb.scalar.sha224",
           "sb.scalar.sha256",
           "sb.scalar.sha384",
           "sb.scalar.sha512",
       }) {
    const sblr::SblrDispatchRequest refusal_request{
        EngineContext(), CryptoDependencyRefusalEngineEnvelope(function_id), api::EngineApiRequest{}};
    const auto refusal = sblr::DispatchSblrOperation(refusal_request);
    Require(refusal.envelope_validated && refusal.accepted && refusal.dispatched_to_api,
            "engine crypto dependency refusal did not reach projection API");
    Require(!refusal.api_result.ok,
            "engine crypto dependency refusal unexpectedly succeeded");
    Require(!refusal.api_result.diagnostics.empty() &&
                refusal.api_result.diagnostics.front().code ==
                    "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE",
            "engine crypto dependency refusal diagnostic mismatch");
  }
}

void RequireEngineTemporalSessionProviderFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), TemporalSessionProviderProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR temporal/session provider envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept temporal/session provider projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route temporal/session provider projection");
  Require(result.api_result.ok, "engine temporal/session provider scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine temporal/session provider scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 16,
          "engine temporal/session provider scalar projection column count mismatch");
  Require(row.fields[0].first == "stmt_ts" &&
              row.fields[0].second.descriptor.canonical_type_name == "timestamp_tz" &&
              row.fields[0].second.encoded_value == "2026-05-12T14:23:45Z",
          "engine statement_timestamp provider scalar projection mismatch");
  Require(row.fields[1].first == "tx_ts" &&
              row.fields[1].second.descriptor.canonical_type_name == "timestamp_tz" &&
              row.fields[1].second.encoded_value == "2026-05-12T13:00:00Z",
          "engine transaction_timestamp provider scalar projection mismatch");
  Require(row.fields[2].first == "clock_ts" &&
              row.fields[2].second.encoded_value == "2026-05-12T14:23:46Z",
          "engine clock_timestamp provider scalar projection mismatch");
  Require(row.fields[3].first == "timeofday_value" &&
              row.fields[3].second.descriptor.canonical_type_name == "character" &&
              row.fields[3].second.encoded_value == "2026-05-12T14:23:46Z",
          "engine timeofday provider scalar projection mismatch");
  Require(row.fields[4].first == "localtime_value" &&
              row.fields[4].second.descriptor.canonical_type_name == "time" &&
              row.fields[4].second.encoded_value == "14:23:46",
          "engine localtime provider scalar projection mismatch");
  Require(row.fields[5].first == "localtimestamp_value" &&
              row.fields[5].second.descriptor.canonical_type_name == "timestamp" &&
              row.fields[5].second.encoded_value == "2026-05-12T14:23:46",
          "engine localtimestamp provider scalar projection mismatch");
  Require(row.fields[6].first == "current_ts" &&
              row.fields[6].second.encoded_value == "2026-05-12T14:23:46Z",
          "engine current_timestamp provider scalar projection mismatch");
  Require(row.fields[7].first == "current_date_value" &&
              row.fields[7].second.descriptor.canonical_type_name == "date" &&
              row.fields[7].second.encoded_value == "2026-05-12",
          "engine current_date provider scalar projection mismatch");
  Require(row.fields[8].first == "current_time_value" &&
              row.fields[8].second.descriptor.canonical_type_name == "time" &&
              row.fields[8].second.encoded_value == "14:23:46",
          "engine current_time provider scalar projection mismatch");
  Require(row.fields[9].first == "now_value" &&
              row.fields[9].second.encoded_value == "2026-05-12T14:23:46Z",
          "engine now provider scalar projection mismatch");
  for (std::size_t index = 10; index < 16; ++index) {
    Require(row.fields[index].second.descriptor.canonical_type_name == "uuid" &&
                row.fields[index].second.encoded_value == "550e8400-e29b-41d4-a716-446655440000",
            "engine deterministic uuid provider scalar projection mismatch");
  }
}

void RequireEngineSpecialCurrentTimestampKeywordDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), SpecialCurrentTimestampKeywordProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine special current timestamp keyword envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept special current timestamp keyword projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route special current timestamp keyword projection");
  Require(result.api_result.ok,
          "engine special current timestamp keyword scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine special current timestamp keyword projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 1,
          "engine special current timestamp keyword projection column count mismatch");
  Require(row.fields[0].first == "current_ts" &&
              row.fields[0].second.descriptor.canonical_type_name == "timestamp_tz" &&
              row.fields[0].second.encoded_value == "2026-05-12T14:23:46Z" &&
              !row.fields[0].second.is_null,
          "engine special current timestamp keyword provider scalar projection mismatch");
}

void RequireEngineCurrentValueFormDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), CurrentValueFormProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine current_value_form envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept current_value_form projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route current_value_form projection");
  Require(result.api_result.ok,
          "engine current_value_form scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine current_value_form projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 1,
          "engine current_value_form projection column count mismatch");
  Require(row.fields[0].first == "current_ts" &&
              row.fields[0].second.descriptor.canonical_type_name == "timestamp_tz" &&
              row.fields[0].second.encoded_value == "2026-05-12T14:23:46Z" &&
              !row.fields[0].second.is_null,
          "engine current_value_form provider scalar projection mismatch");
}

void RequireEngineTemporalConstructorFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), TemporalConstructorProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR temporal constructor envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept temporal constructor projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route temporal constructor projection");
  Require(result.api_result.ok, "engine temporal constructor scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine temporal constructor scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 6,
          "engine temporal constructor scalar projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine temporal constructor scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "made_date", "date", "2026-05-11");
  expect_text(1, "made_time", "time", "14:23:45");
  expect_text(2, "made_timestamp", "timestamp", "2026-05-11T14:23:45");
  expect_text(3, "made_timestamp_6", "timestamp", "2026-05-11T14:23:45");
  expect_text(4, "made_timestamptz", "timestamp_tz", "2026-05-11T14:23:45Z");
  expect_text(5, "made_timestamptz_offset", "timestamp_tz", "2026-05-11T14:23:45+05:30");
}

void RequireEngineTemporalFieldArithmeticFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), TemporalFieldArithmeticProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR temporal field/arithmetic envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept temporal field/arithmetic projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route temporal field/arithmetic projection");
  Require(result.api_result.ok, "engine temporal field/arithmetic scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine temporal field/arithmetic scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 9,
          "engine temporal field/arithmetic scalar projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine temporal field/arithmetic scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "dow_value", "int64", "1");
  expect_text(1, "doy_value", "int64", "131");
  expect_text(2, "quarter_value", "int64", "2");
  expect_text(3, "isodow_value", "int64", "1");
  expect_text(4, "week_value", "int64", "20");
  expect_text(5, "add_months_value", "date", "2026-08-11");
  expect_text(6, "add_months_clamp", "date", "2026-02-28");
  expect_text(7, "last_day_value", "date", "2026-05-31");
  expect_text(8, "last_day_leap", "date", "2024-02-29");
}

void RequireEngineTemporalDateTimeBatchFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), TemporalDateTimeBatchProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR temporal date/time batch envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept temporal date/time batch projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route temporal date/time batch projection");
  Require(result.api_result.ok, "engine temporal date/time batch scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine temporal date/time batch scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 11,
          "engine temporal date/time batch scalar projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine temporal date/time batch scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "age_value", "interval", "PT176523S");
  expect_text(1, "age_days", "int64", "2");
  expect_text(2, "age_months", "int64", "3");
  expect_text(3, "age_years", "int64", "2");
  expect_text(4, "date_add_value", "timestamp", "2026-05-11T02:00:00");
  expect_text(5, "date_sub_value", "timestamp", "2026-05-08T00:00:00");
  expect_text(6, "date_diff_value", "int64", "5");
  expect_text(7, "date_bin_value", "timestamp", "2026-05-10T10:00:00");
  expect_text(8, "interval_value", "interval", "P1Y2M3DT4H5M6S");
  expect_text(9, "timezone_value", "timestamp_tz", "2026-05-10T10:00:00Z");
  expect_text(10, "timezone_offset_value", "timestamp_tz", "2026-05-10T07:30:00Z");
}

void RequireEngineProceduralContextFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), ProceduralContextProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR procedural/context provider envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept procedural/context provider projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route procedural/context provider projection");
  Require(result.api_result.ok, "engine procedural/context provider scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine procedural/context provider scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 38,
          "engine procedural/context provider scalar projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(row.fields[index].first == name &&
                row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                row.fields[index].second.encoded_value == expected,
            std::string("engine procedural/context scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "session_id_value", "uuid", "019f0000-0000-7000-8000-000000003122");
  expect_text(1, "transaction_id_value", "uint64", "42");
  expect_text(2, "transaction_uuid_value", "uuid", "019f0000-0000-7000-8000-000000003124");
  expect_text(3, "current_user_value", "uuid", "019f0000-0000-7000-8000-000000003123");
  expect_text(4, "session_user_value", "uuid", "019f0000-0000-7000-8000-000000003123");
  expect_text(5, "system_user_value", "uuid", "019f0000-0000-7000-8000-000000003123");
  expect_text(6, "user_value", "uuid", "019f0000-0000-7000-8000-000000003123");
  expect_text(7, "current_catalog_value", "uuid", "019f0000-0000-7000-8000-000000003121");
  expect_text(8, "current_schema_value", "uuid", "019f0000-0000-7000-8000-000000003126");
  expect_text(9, "current_database_value", "uuid", "019f0000-0000-7000-8000-000000003121");
  expect_text(10, "current_role_value", "uuid", "019f0000-0000-7000-8000-000000003127");
  expect_text(11, "current_server_value", "uuid", "019f0000-0000-7000-8000-000000003120");
  expect_text(12, "server_version_value", "character", "ScratchBird 0.1.0");
  expect_text(13, "server_version_num_value", "uint64", "100");
  expect_text(14, "timezone_value", "character", "UTC");
  expect_text(15, "array_rank_limit", "uint64", "6");
  expect_text(16, "array_element_limit", "uint64", "1048576");
  expect_text(17, "case_branch_limit", "uint64", "1024");
  expect_text(18, "cte_count_limit", "uint64", "1024");
  expect_text(19, "subquery_depth_limit", "uint64", "256");
  expect_text(20, "recursive_cte_depth_limit", "uint64", "1024");
  expect_text(21, "result_column_limit", "uint64", "4096");
  expect_text(22, "union_arm_limit", "uint64", "1024");
  expect_text(23, "default_charset_value", "character", "UTF-8");
  expect_text(24, "default_collation_value", "character", "unicode_root");
  expect_text(25,
              "comparison_collation_rule",
              "character",
              "descriptor_collation_explicit_collate_override");
  expect_text(26, "keyword_case_rule_value", "character", "case_insensitive");
  expect_text(27,
              "quoted_identifier_case_rule_value",
              "character",
              "spelling_preserving_not_identity");
  expect_text(28,
              "unquoted_identifier_case_rule_value",
              "character",
              "case_preserving_case_insensitive");
  expect_text(29, "unicode_root_value", "character", "unicode_root");
  expect_text(30, "current_session_id_alias", "uuid", "019f0000-0000-7000-8000-000000003122");
  expect_text(31, "current_session_uuid_alias", "uuid", "019f0000-0000-7000-8000-000000003122");
  expect_text(32, "current_transaction_id_alias", "uint64", "42");
  expect_text(33, "current_statement_uuid_value", "uuid", "019f0000-0000-7000-8000-000000003125");
  expect_text(34, "current_timezone_alias", "character", "UTC");
  expect_text(35, "row_count_value", "uint64", "7");
  expect_text(36, "application_name_value", "character", "sbsql_conformance");
  expect_text(37, "mga_isolation_profile_value", "character", "snapshot_transaction");
}

void RequireEngineSbsfc016FixedPolicyLimitDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), Sbsfc016FixedPolicyLimitProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "SBSFC-016 fixed policy limit projection envelope did not validate");
  Require(result.accepted,
          "SBSFC-016 fixed policy limit projection route was not accepted");
  Require(result.dispatched_to_api,
          "SBSFC-016 fixed policy limit projection did not route to projection API");
  Require(result.api_result.ok,
          "SBSFC-016 fixed policy limit projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-016 fixed policy limit projection did not produce one row");

  struct ExpectedField {
    const char* surface_id;
    const char* encoded_value;
  };
  const ExpectedField expected[] = {
      {"SBSQL-F824C47A36A5", "6"},
      {"SBSQL-BCAC432F4C75", "1048576"},
      {"SBSQL-6C698B54A7CB", "1024"},
      {"SBSQL-8EAA8898DBEB", "1024"},
      {"SBSQL-2CF7F4318343", "256"},
      {"SBSQL-8A442BFCB429", "1024"},
      {"SBSQL-189B5E58953A", "4096"},
      {"SBSQL-7998B79486A5", "1024"},
  };

  const auto& row = result.api_result.result_shape.rows.front();
  const std::size_t expected_count = sizeof(expected) / sizeof(expected[0]);
  Require(row.fields.size() == expected_count,
          "SBSFC-016 fixed policy limit projection column count mismatch");
  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& actual = row.fields[index];
    const auto& wanted = expected[index];
    const std::string label = wanted.surface_id;
    Require(actual.first == wanted.surface_id,
            label + " SBSFC-016 fixed policy limit projection label mismatch");
    Require(actual.second.descriptor.canonical_type_name == "uint64",
            label + " SBSFC-016 fixed policy limit descriptor mismatch");
    Require(!actual.second.is_null,
            label + " SBSFC-016 fixed policy limit unexpectedly returned null");
    Require(actual.second.encoded_value == wanted.encoded_value,
            label + " SBSFC-016 fixed policy limit value mismatch");
  }
}

void RequireEngineSbsfc016LanguagePolicyDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), Sbsfc016LanguagePolicyProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "SBSFC-016 language policy projection envelope did not validate");
  Require(result.accepted,
          "SBSFC-016 language policy projection route was not accepted");
  Require(result.dispatched_to_api,
          "SBSFC-016 language policy projection did not route to projection API");
  Require(result.api_result.ok,
          "SBSFC-016 language policy projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-016 language policy projection did not produce one row");

  const auto& row = result.api_result.result_shape.rows.front();
  const std::size_t expected_count =
      sizeof(kSbsfc016LanguagePolicyRows) / sizeof(kSbsfc016LanguagePolicyRows[0]);
  Require(row.fields.size() == expected_count,
          "SBSFC-016 language policy projection column count mismatch");
  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& actual = row.fields[index];
    const auto& expected = kSbsfc016LanguagePolicyRows[index];
    const std::string label = expected.surface_id;
    Require(actual.first == expected.surface_id,
            label + " SBSFC-016 language policy projection label mismatch");
    Require(actual.second.descriptor.canonical_type_name == expected.result_type,
            label + " SBSFC-016 language policy descriptor mismatch");
    Require(!actual.second.is_null,
            label + " SBSFC-016 language policy unexpectedly returned null");
    Require(actual.second.encoded_value == expected.expected_value,
            label + " SBSFC-016 language policy value mismatch");
  }
}

void RequireEngineSbsfc016MetadataDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), Sbsfc016MetadataProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "SBSFC-016 metadata projection envelope did not validate");
  Require(result.accepted,
          "SBSFC-016 metadata projection route was not accepted");
  Require(result.dispatched_to_api,
          "SBSFC-016 metadata projection did not route to projection API");
  Require(result.api_result.ok,
          "SBSFC-016 metadata projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-016 metadata projection did not produce one row");

  const auto& row = result.api_result.result_shape.rows.front();
  const std::size_t expected_count =
      sizeof(kSbsfc016MetadataRows) / sizeof(kSbsfc016MetadataRows[0]);
  Require(row.fields.size() == expected_count,
          "SBSFC-016 metadata projection column count mismatch");
  for (std::size_t index = 0; index < expected_count; ++index) {
    const auto& actual = row.fields[index];
    const auto& expected = kSbsfc016MetadataRows[index];
    const std::string label = expected.surface_id;
    Require(actual.first == expected.surface_id,
            label + " SBSFC-016 metadata projection label mismatch");
    Require(actual.second.descriptor.canonical_type_name == expected.result_type,
            label + " SBSFC-016 metadata descriptor mismatch");
    Require(!actual.second.is_null,
            label + " SBSFC-016 metadata unexpectedly returned null");
    Require(actual.second.encoded_value == expected.expected_value,
            label + " SBSFC-016 metadata value mismatch");
  }
}

void RequireEngineSbsfc016ProceduralDiagnosticDispatch() {
  for (const auto& expected : kSbsfc016ProceduralDiagnosticRows) {
    const sblr::SblrDispatchRequest request{
        EngineContext(), Sbsfc016ProceduralDiagnosticEngineEnvelope(expected), api::EngineApiRequest{}};
    const auto result = sblr::DispatchSblrOperation(request);
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Require(result.envelope_validated,
            "SBSFC-016 procedural diagnostic projection envelope did not validate");
    Require(result.accepted,
            "SBSFC-016 procedural diagnostic projection route was not accepted");
    Require(result.dispatched_to_api,
            "SBSFC-016 procedural diagnostic projection did not route to projection API");
    Require(!result.api_result.ok,
            "SBSFC-016 procedural diagnostic projection unexpectedly succeeded");
    Require(ApiHasDiagnosticDetail(result.api_result,
                                   expected.diagnostic_id,
                                   expected.diagnostic_detail),
            std::string(expected.surface_id) + " procedural diagnostic mismatch");
  }
}

void RequireEngineCurrentSettingLiteralRefusalDispatch() {
  for (const auto* setting_name : {"var", "autocommit"}) {
    const sblr::SblrDispatchRequest request{
        EngineContext(), CurrentSettingLiteralRefusalEngineEnvelope(setting_name), api::EngineApiRequest{}};
    const auto result = sblr::DispatchSblrOperation(request);
    Require(result.envelope_validated,
            "engine SBLR current_setting literal refusal envelope did not validate");
    Require(result.accepted,
            "engine SBLR dispatch did not accept current_setting literal refusal route");
    Require(result.dispatched_to_api,
            "engine SBLR dispatch did not route current_setting literal refusal projection");
    Require(!result.api_result.ok,
            "engine current_setting literal refusal unexpectedly succeeded");
    Require(ApiHasDiagnosticDetail(result.api_result,
                                   "SB_DIAG_FUNCTION_INVALID_INPUT",
                                   "current_setting setting is unknown"),
            "engine current_setting literal refusal diagnostic mismatch");
  }
}

void RequireEngineTextFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), TextFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR text function envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept text function scalar projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route text function projection");
  Require(result.api_result.ok, "engine text function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine text function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 3, "engine text function scalar projection column count mismatch");
  Require(row.fields[0].first == "lowered" &&
              row.fields[0].second.descriptor.canonical_type_name == "text" &&
              row.fields[0].second.encoded_value == "alpha",
          "engine lower function scalar projection mismatch");
  Require(row.fields[1].first == "uppered" &&
              row.fields[1].second.descriptor.canonical_type_name == "text" &&
              row.fields[1].second.encoded_value == "BETA",
          "engine upper function scalar projection mismatch");
  Require(row.fields[2].first == "len_value" &&
              row.fields[2].second.descriptor.canonical_type_name == "int64" &&
              row.fields[2].second.encoded_value == "7",
          "engine length function scalar projection mismatch");

  const sblr::SblrDispatchRequest more_request{
      EngineContext(), MoreTextFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto more_result = sblr::DispatchSblrOperation(more_request);
  for (const auto& diagnostic : more_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : more_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(more_result.envelope_validated,
          "engine SBLR additional text function envelope did not validate");
  Require(more_result.accepted,
          "engine SBLR dispatch did not accept additional text function projection");
  Require(more_result.dispatched_to_api,
          "engine SBLR dispatch did not route additional text function projection");
  Require(more_result.api_result.ok,
          "engine additional text function scalar projection API failed");
  Require(more_result.api_result.result_shape.rows.size() == 1,
          "engine additional text function scalar projection did not produce one row");
  const auto& more_row = more_result.api_result.result_shape.rows.front();
  Require(more_row.fields.size() == 5,
          "engine additional text function scalar projection column count mismatch");
  Require(more_row.fields[0].first == "octets" &&
              more_row.fields[0].second.descriptor.canonical_type_name == "int64" &&
              more_row.fields[0].second.encoded_value == "5",
          "engine octet_length function scalar projection mismatch");
  Require(more_row.fields[1].first == "bits" &&
              more_row.fields[1].second.descriptor.canonical_type_name == "int64" &&
              more_row.fields[1].second.encoded_value == "40",
          "engine bit_length function scalar projection mismatch");
  Require(more_row.fields[2].first == "reversed" &&
              more_row.fields[2].second.descriptor.canonical_type_name == "text" &&
              more_row.fields[2].second.encoded_value == "cba",
          "engine reverse function scalar projection mismatch");
  Require(more_row.fields[3].first == "code_value" &&
              more_row.fields[3].second.descriptor.canonical_type_name == "int64" &&
              more_row.fields[3].second.encoded_value == "90",
          "engine ascii function scalar projection mismatch");
  Require(more_row.fields[4].first == "char_value" &&
              more_row.fields[4].second.descriptor.canonical_type_name == "character" &&
              more_row.fields[4].second.encoded_value == "Z",
          "engine chr function scalar projection mismatch");

  const sblr::SblrDispatchRequest trim_request{
      EngineContext(), TrimEncodingFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto trim_result = sblr::DispatchSblrOperation(trim_request);
  for (const auto& diagnostic : trim_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : trim_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(trim_result.envelope_validated,
          "engine SBLR trim/encoding function envelope did not validate");
  Require(trim_result.accepted,
          "engine SBLR dispatch did not accept trim/encoding function projection");
  Require(trim_result.dispatched_to_api,
          "engine SBLR dispatch did not route trim/encoding function projection");
  Require(trim_result.api_result.ok,
          "engine trim/encoding function scalar projection API failed");
  Require(trim_result.api_result.result_shape.rows.size() == 1,
          "engine trim/encoding function scalar projection did not produce one row");
  const auto& trim_row = trim_result.api_result.result_shape.rows.front();
  Require(trim_row.fields.size() == 8,
          "engine trim/encoding function scalar projection column count mismatch");
  const auto expect_text = [&](std::size_t index,
                               std::string_view name,
                               std::string_view descriptor,
                               std::string_view expected) {
    Require(trim_row.fields[index].first == name &&
                trim_row.fields[index].second.descriptor.canonical_type_name == descriptor &&
                trim_row.fields[index].second.encoded_value == expected,
            std::string("engine trim/encoding scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text(0, "trimmed", "text", "hello");
  expect_text(1, "trimmed_chars", "text", "hello");
  expect_text(2, "btrimmed", "text", "foo");
  expect_text(3, "ltrimmed", "text", "helloxyz");
  expect_text(4, "rtrimmed", "text", "xyzhello");
  expect_text(5, "left_space_trimmed", "text", "hello");
  expect_text(6, "right_space_trimmed", "text", "hello");
  expect_text(7, "hex_value", "character", "ff");

  const sblr::SblrDispatchRequest text_conditional_request{
      EngineContext(), TextConditionalFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto text_conditional_result = sblr::DispatchSblrOperation(text_conditional_request);
  for (const auto& diagnostic : text_conditional_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : text_conditional_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(text_conditional_result.envelope_validated,
          "engine SBLR text/conditional function envelope did not validate");
  Require(text_conditional_result.accepted,
          "engine SBLR dispatch did not accept text/conditional function projection");
  Require(text_conditional_result.dispatched_to_api,
          "engine SBLR dispatch did not route text/conditional function projection");
  Require(text_conditional_result.api_result.ok,
          "engine text/conditional function scalar projection API failed");
  Require(text_conditional_result.api_result.result_shape.rows.size() == 1,
          "engine text/conditional function scalar projection did not produce one row");
  const auto& text_conditional_row =
      text_conditional_result.api_result.result_shape.rows.front();
  Require(text_conditional_row.fields.size() == 22,
          "engine text/conditional function scalar projection column count mismatch");
  const auto expect_text_conditional = [&](std::size_t index,
                                           std::string_view name,
                                           std::string_view descriptor,
                                           std::string_view expected) {
    Require(text_conditional_row.fields[index].first == name &&
                text_conditional_row.fields[index].second.descriptor.canonical_type_name ==
                    descriptor &&
                text_conditional_row.fields[index].second.encoded_value == expected,
            std::string("engine text/conditional scalar projection mismatch: ") +
                std::string(name));
  };
  expect_text_conditional(0, "sub_part", "text", "world");
  expect_text_conditional(1, "sub_tail", "text", "world");
  expect_text_conditional(2, "substr_part", "text", "world");
  expect_text_conditional(3, "pos_value", "int64", "7");
  expect_text_conditional(4, "left_padded", "character", "***bird");
  expect_text_conditional(5, "right_padded", "character", "bird***");
  expect_text_conditional(6, "repeated", "character", "hahaha");
  expect_text_conditional(7, "overlay_short", "character", "hello SBSQL");
  expect_text_conditional(8, "overlay_for", "character", "hello new");
  expect_text_conditional(9, "instr_pos", "int64", "5");
  expect_text_conditional(10, "strpos_pos", "int64", "7");
  expect_text_conditional(11, "ifnull_value", "text", "fallback");
  expect_text_conditional(12, "coalesce_value", "text", "first");
  expect_text_conditional(13, "coalesce_strict_value", "text", "strict");
  Require(text_conditional_row.fields[14].first == "nullif_value" &&
              text_conditional_row.fields[14].second.descriptor.canonical_type_name == "text" &&
              text_conditional_row.fields[14].second.is_null,
          "engine nullif function scalar projection mismatch");
  expect_text_conditional(15, "nvl2_value", "text", "yes");
  expect_text_conditional(16, "greatest_value", "bigint", "9");
  expect_text_conditional(17, "least_value", "bigint", "3");
  expect_text_conditional(18, "iif_value", "text", "yes");
  expect_text_conditional(19, "initcap_value", "text", "Hello World-From Sb");
  expect_text_conditional(20, "translate_value", "character", "boxoxo");
  expect_text_conditional(21, "unicode_value", "int64", "65");

  const sblr::SblrDispatchRequest coalesce_strict_invalid_request{
      EngineContext(), CoalesceStrictInvalidProjectionEngineEnvelope(),
      api::EngineApiRequest{}};
  const auto coalesce_strict_invalid =
      sblr::DispatchSblrOperation(coalesce_strict_invalid_request);
  Require(coalesce_strict_invalid.envelope_validated,
          "engine coalesce_strict invalid-input envelope did not validate");
  Require(coalesce_strict_invalid.accepted,
          "engine SBLR dispatch did not accept coalesce_strict invalid-input route");
  Require(coalesce_strict_invalid.dispatched_to_api,
          "engine SBLR dispatch did not route coalesce_strict invalid-input projection");
  Require(!coalesce_strict_invalid.api_result.ok,
          "engine coalesce_strict empty-argument projection unexpectedly succeeded");
  Require(ApiHasDiagnostic(coalesce_strict_invalid.api_result,
                           "SB_DIAG_FUNCTION_INVALID_INPUT"),
          "engine coalesce_strict empty-argument diagnostic mismatch");

  const sblr::SblrDispatchRequest keyword_text_request{
      EngineContext(), SqlKeywordTextFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto keyword_text_result = sblr::DispatchSblrOperation(keyword_text_request);
  for (const auto& diagnostic : keyword_text_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : keyword_text_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(keyword_text_result.envelope_validated,
          "engine SBLR SQL keyword text function envelope did not validate");
  Require(keyword_text_result.accepted,
          "engine SBLR dispatch did not accept SQL keyword text function projection");
  Require(keyword_text_result.dispatched_to_api,
          "engine SBLR dispatch did not route SQL keyword text function projection");
  Require(keyword_text_result.api_result.ok,
          "engine SQL keyword text function scalar projection API failed");
  Require(keyword_text_result.api_result.result_shape.rows.size() == 1,
          "engine SQL keyword text function scalar projection did not produce one row");
  const auto& keyword_text_row =
      keyword_text_result.api_result.result_shape.rows.front();
  Require(keyword_text_row.fields.size() == 9,
          "engine SQL keyword text function scalar projection column count mismatch");
  const auto expect_keyword_text = [&](std::size_t index,
                                       std::string_view name,
                                       std::string_view descriptor,
                                       std::string_view expected) {
    Require(keyword_text_row.fields[index].first == name &&
                keyword_text_row.fields[index].second.descriptor.canonical_type_name ==
                    descriptor &&
                keyword_text_row.fields[index].second.encoded_value == expected,
            std::string("engine SQL keyword text scalar projection mismatch: ") +
                std::string(name));
  };
  expect_keyword_text(0, "position_keyword", "int64", "4");
  expect_keyword_text(1, "substring_keyword", "text", "world");
  expect_keyword_text(2, "substring_tail_keyword", "text", "world");
  expect_keyword_text(3, "trim_both_keyword", "text", "hello");
  expect_keyword_text(4, "trim_spaces_keyword", "text", "hello");
  expect_keyword_text(5, "trim_leading_keyword", "text", "hello");
  expect_keyword_text(6, "trim_trailing_keyword", "text", "hello");
  expect_keyword_text(7, "overlay_for_keyword", "character", "hello SBSQL");
  expect_keyword_text(8, "overlay_keyword", "character", "hello SBSQL");
}

void RequireEngineMultiArgumentFunctionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), MultiArgumentFunctionProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR multi-argument function envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept multi-argument function projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route multi-argument function projection");
  Require(result.api_result.ok,
          "engine multi-argument function scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine multi-argument function scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 25,
          "engine multi-argument function scalar projection column count mismatch");
  Require(row.fields[0].first == "joined" &&
              row.fields[0].second.descriptor.canonical_type_name == "character" &&
              row.fields[0].second.encoded_value == "alpha-beta",
          "engine concat function scalar projection mismatch");
  Require(row.fields[1].first == "replaced" &&
              row.fields[1].second.descriptor.canonical_type_name == "character" &&
              row.fields[1].second.encoded_value == "alpha B B",
          "engine replace function scalar projection mismatch");
  Require(row.fields[2].first == "regex_ok" &&
              row.fields[2].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[2].second.encoded_value == "1",
          "engine regexp_like function scalar projection mismatch");
  Require(row.fields[3].first == "year_part" &&
              row.fields[3].second.descriptor.canonical_type_name == "int64" &&
              row.fields[3].second.encoded_value == "2026",
          "engine date_part function scalar projection mismatch");
  Require(row.fields[4].first == "day_trunc" &&
              row.fields[4].second.descriptor.canonical_type_name == "timestamp" &&
              row.fields[4].second.encoded_value == "2026-05-11T00:00:00",
          "engine date_trunc function scalar projection mismatch");
  Require(row.fields[5].first == "json_len" &&
              row.fields[5].second.descriptor.canonical_type_name == "uint64" &&
              row.fields[5].second.encoded_value == "3",
          "engine json_array_length function scalar projection mismatch");
  Require(row.fields[6].first == "sound_code" &&
              row.fields[6].second.descriptor.canonical_type_name == "character" &&
              row.fields[6].second.encoded_value == "R163",
          "engine soundex function scalar projection mismatch");
  Require(row.fields[7].first == "edit_distance" &&
              row.fields[7].second.descriptor.canonical_type_name == "int64" &&
              row.fields[7].second.encoded_value == "3",
          "engine levenshtein function scalar projection mismatch");
  Require(row.fields[8].first == "cardinality_value" &&
              row.fields[8].second.descriptor.canonical_type_name == "int64" &&
              row.fields[8].second.encoded_value == "3",
          "engine cardinality function scalar projection mismatch");
  Require(row.fields[9].first == "stuff_value" &&
              row.fields[9].second.descriptor.canonical_type_name == "character" &&
              row.fields[9].second.encoded_value == "aXYef",
          "engine stuff function scalar projection mismatch");
  Require(row.fields[10].first == "regex_count" &&
              row.fields[10].second.descriptor.canonical_type_name == "int64" &&
              row.fields[10].second.encoded_value == "2",
          "engine regexp_count function scalar projection mismatch");
  Require(row.fields[11].first == "regex_capture" &&
              row.fields[11].second.descriptor.canonical_type_name == "array" &&
              row.fields[11].second.encoded_value == "[\"abc\",\"123\"]",
          "engine regexp_match function scalar projection mismatch");
  Require(row.fields[12].first == "regex_replaced" &&
              row.fields[12].second.descriptor.canonical_type_name == "character" &&
              row.fields[12].second.encoded_value == "X123X",
          "engine regexp_replace function scalar projection mismatch");
  Require(row.fields[13].first == "regex_parts" &&
              row.fields[13].second.descriptor.canonical_type_name == "array" &&
              row.fields[13].second.encoded_value == "[\"a\",\"b\",\"c\"]",
          "engine regexp_split_to_array function scalar projection mismatch");
  Require(row.fields[14].first == "regex_substr" &&
              row.fields[14].second.descriptor.canonical_type_name == "character" &&
              row.fields[14].second.encoded_value == "123",
          "engine regexp_substr function scalar projection mismatch");
  Require(row.fields[15].first == "occurrences_regex_count" &&
              row.fields[15].second.descriptor.canonical_type_name == "int64" &&
              row.fields[15].second.encoded_value == "2",
          "engine occurrences_regex function scalar projection mismatch");
  Require(row.fields[16].first == "position_regex_after" &&
              row.fields[16].second.descriptor.canonical_type_name == "int64" &&
              row.fields[16].second.encoded_value == "4",
          "engine position_regex function scalar projection mismatch");
  Require(row.fields[17].first == "substring_regex_group" &&
              row.fields[17].second.descriptor.canonical_type_name == "character" &&
              row.fields[17].second.encoded_value == "22",
          "engine substring_regex function scalar projection mismatch");
  Require(row.fields[18].first == "translate_regex_all" &&
              row.fields[18].second.descriptor.canonical_type_name == "character" &&
              row.fields[18].second.encoded_value == "X123X",
          "engine translate_regex function scalar projection mismatch");
  Require(row.fields[19].first == "regex_matches" &&
              row.fields[19].second.descriptor.canonical_type_name == "array" &&
              row.fields[19].second.encoded_value == "[\"abc\",\"123\"]",
          "engine regexp_matches same array payload scalar projection mismatch");
  Require(row.fields[20].first == "regex_split_rows" &&
              row.fields[20].second.descriptor.canonical_type_name == "array" &&
              row.fields[20].second.encoded_value == "[\"a\",\"b\",\"c\"]",
          "engine regexp_split_to_table scalar array payload projection mismatch");
  Require(row.fields[21].first == "converted" &&
              row.fields[21].second.descriptor.canonical_type_name == "character" &&
              row.fields[21].second.encoded_value == "deja vu",
          "engine convert function scalar projection mismatch");
  Require(row.fields[22].first == "array_joined" &&
              row.fields[22].second.descriptor.canonical_type_name == "character" &&
              row.fields[22].second.encoded_value == "a|c",
          "engine array_to_string function scalar projection mismatch");
  Require(row.fields[23].first == "ascii_text" &&
              row.fields[23].second.descriptor.canonical_type_name == "character" &&
              row.fields[23].second.encoded_value == "deja vu",
          "engine to_ascii function scalar projection mismatch");
  Require(row.fields[24].first == "formatted" &&
              row.fields[24].second.descriptor.canonical_type_name == "character" &&
              row.fields[24].second.encoded_value == "SELECT \"col name\" = 'O''Reilly' %",
          "engine format function scalar projection mismatch");
}

void RequireEngineConcatExpressionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), ConcatExpressionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR concat_expr envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept concat_expr projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route concat_expr projection");
  Require(result.api_result.ok,
          "engine concat_expr scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine concat_expr scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 1,
          "engine concat_expr scalar projection column count mismatch");
  Require(row.fields[0].first == "joined" &&
              row.fields[0].second.descriptor.canonical_type_name == "character" &&
              row.fields[0].second.encoded_value == "alpha-beta",
          "engine concat_expr scalar projection mismatch");
}

void RequireEngineOperatorProjectionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), OperatorProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine SBLR operator projection envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept operator projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route operator projection");
  Require(result.api_result.ok,
          "engine operator scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine operator scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 7,
          "engine operator scalar projection column count mismatch");
  Require(row.fields[0].first == "like_true" &&
              row.fields[0].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[0].second.encoded_value == "1",
          "engine LIKE true projection mismatch");
  Require(row.fields[1].first == "like_false" &&
              row.fields[1].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[1].second.encoded_value == "0",
          "engine LIKE false projection mismatch");
  Require(row.fields[2].first == "like_unknown" &&
              row.fields[2].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[2].second.is_null,
          "engine LIKE UNKNOWN projection mismatch");
  Require(row.fields[3].first == "not_like_true" &&
              row.fields[3].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[3].second.encoded_value == "1",
          "engine NOT(LIKE) projection mismatch");
  Require(row.fields[4].first == "not_true" &&
              row.fields[4].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[4].second.encoded_value == "0",
          "engine NOT TRUE projection mismatch");
  Require(row.fields[5].first == "ilike_true" &&
              row.fields[5].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[5].second.encoded_value == "1",
          "engine ILIKE true projection mismatch");
  Require(row.fields[6].first == "like_escape_true" &&
              row.fields[6].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[6].second.encoded_value == "1",
          "engine LIKE escaped pattern projection mismatch");

  const sblr::SblrDispatchRequest invalid_escape{
      EngineContext(),
      InvalidLikeOperatorProjectionEngineEnvelope({"text", "abc"}, {"text", "abc\\"}),
      api::EngineApiRequest{}};
  const auto invalid_escape_result = sblr::DispatchSblrOperation(invalid_escape);
  Require(invalid_escape_result.envelope_validated,
          "engine invalid LIKE pattern envelope did not validate");
  Require(invalid_escape_result.accepted,
          "engine invalid LIKE pattern route was not accepted for engine diagnostic");
  Require(!invalid_escape_result.api_result.ok,
          "engine invalid LIKE pattern unexpectedly succeeded");
  Require(ApiHasDiagnostic(invalid_escape_result.api_result, "SBLR.INVALID_PATTERN"),
          "engine invalid LIKE pattern diagnostic mismatch");

  const sblr::SblrDispatchRequest invalid_type{
      EngineContext(),
      InvalidLikeOperatorProjectionEngineEnvelope({"bigint", "7"}, {"text", "7"}),
      api::EngineApiRequest{}};
  const auto invalid_type_result = sblr::DispatchSblrOperation(invalid_type);
  Require(invalid_type_result.envelope_validated,
          "engine invalid LIKE type envelope did not validate");
  Require(invalid_type_result.accepted,
          "engine invalid LIKE type route was not accepted for engine diagnostic");
  Require(!invalid_type_result.api_result.ok,
          "engine invalid LIKE type unexpectedly succeeded");
  Require(ApiHasDiagnostic(invalid_type_result.api_result, "SBLR.DESCRIPTOR_MISMATCH"),
          "engine invalid LIKE type diagnostic mismatch");
}

void RequireEngineExtendedOperatorProjectionDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(), ExtendedOperatorProjectionEngineEnvelope(), api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          "engine extended operator projection envelope did not validate");
  Require(result.accepted,
          "engine SBLR dispatch did not accept extended operator projection");
  Require(result.dispatched_to_api,
          "engine SBLR dispatch did not route extended operator projection");
  Require(result.api_result.ok,
          "engine extended operator scalar projection API failed");
  Require(result.api_result.result_shape.rows.size() == 1,
          "engine extended operator scalar projection did not produce one row");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 9,
          "engine extended operator scalar projection column count mismatch");
  Require(row.fields[0].first == "and_false" &&
              row.fields[0].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[0].second.encoded_value == "0",
          "engine AND projection mismatch");
  Require(row.fields[1].first == "or_true" &&
              row.fields[1].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[1].second.encoded_value == "1",
          "engine OR projection mismatch");
  Require(row.fields[2].first == "negated" &&
              row.fields[2].second.descriptor.canonical_type_name == "int64" &&
              row.fields[2].second.encoded_value == "-7",
          "engine unary minus projection mismatch");
  Require(row.fields[3].first == "distinct_null" &&
              row.fields[3].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[3].second.encoded_value == "1",
          "engine IS DISTINCT FROM projection mismatch");
  Require(row.fields[4].first == "regex_true" &&
              row.fields[4].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[4].second.encoded_value == "1",
          "engine regex operator projection mismatch");
  Require(row.fields[5].first == "json_value" &&
              row.fields[5].second.descriptor.canonical_type_name == "json_document" &&
              row.fields[5].second.encoded_value == "1",
          "engine JSON get projection mismatch");
  Require(row.fields[6].first == "json_text" &&
              row.fields[6].second.descriptor.canonical_type_name == "text" &&
              row.fields[6].second.encoded_value == "bird",
          "engine JSON get text projection mismatch");
  Require(row.fields[7].first == "array_contains" &&
              row.fields[7].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[7].second.encoded_value == "1",
          "engine array contains projection mismatch");
  Require(row.fields[8].first == "xor_true" &&
              row.fields[8].second.descriptor.canonical_type_name == "boolean" &&
              row.fields[8].second.encoded_value == "1",
          "engine XOR projection mismatch");
}

}  // namespace

int main() {
  RequireScalarLowering();
  RequireFunctionProjectionLowering();
  RequireNumericFunctionProjectionLowering();
  RequireAdditionalNumericFunctionProjectionLowering();
  RequireQualifiedCanonicalNumericFunctionProjectionLowering();
  RequireExtendedNumericFunctionProjectionLowering();
  RequireTextFunctionProjectionLowering();
  RequireTextJsonFuzzyFunctionProjectionLowering();
  RequireVectorFunctionProjectionLowering();
  RequireBinaryCryptoFunctionProjectionLowering();
  RequireTemporalSessionProviderProjectionLowering();
  RequireTemporalConstructorProjectionLowering();
  RequireTemporalFieldArithmeticProjectionLowering();
  RequireTemporalDateTimeBatchProjectionLowering();
  RequireProceduralContextProjectionLowering();
  RequireSbsfc016DonorSystemVariableProjectionLowering();
  RequireSbsfc016DonorContextProjectionLowering();
  RequireSbsfc016PolicyRefusalProjectionLowering();
  RequireSbsfc027PolicyRefusalProjectionLowering();
  RequireSbsfc028UuidCompatHelperProjectionLowering();
  RequireSbsfc031TxidSurfaceProjectionLowering();
  RequireSbsfc032ScalarUtilityConversionProjectionLowering();
  RequireSbsfc033CatalogDescriptorDiagnosticProjectionLowering();
  RequireSbsfc034TextTrigramBitStringProjectionLowering();
  RequireSbsfc035RangeScalarHelperProjectionLowering();
  RequireSbsfc036SpatialGeometryScalarHelperProjectionLowering();
  RequireSbsfc037XmlMultimodelScalarHelperProjectionLowering();
  RequireSbsfc038SpatialTailScalarHelperProjectionLowering();
  RequireSbsfc039XmlDocumentQueryScalarHelperProjectionLowering();
  RequireSbsfc028AntiWalPolicyRefusalProjectionLowering();
  RequireSbsfc016DonorAliasFunctionProjectionLowering();
  RequireSbsfc016FixedPolicyLimitProjectionLowering();
  RequireSbsfc016LanguagePolicyProjectionLowering();
  RequireSbsfc016MetadataProjectionLowering();
  RequireSbsfc016ProceduralDiagnosticProjectionLowering();
  RequireCurrentSettingLiteralRefusalLowering();
  RequireMultiArgumentFunctionProjectionLowering();
  RequireConcatExpressionProjectionLowering();
  RequireQualifiedRegexAliasProjectionLowering();
  RequireQualifiedTemporalAliasProjectionLowering();
  RequireBareTemporalDateAliasProjectionLowering();
  RequireExtractSpecialAndCanonicalJsonProjectionLowering();
  RequireQualifiedTemporalProviderAliasProjectionLowering();
  RequireSpecialCurrentTimestampKeywordProjectionLowering();
  RequireCurrentValueFormProjectionLowering();
  RequireOperatorProjectionLowering();
  RequireIlikeProjectionLowering();
  RequireExtendedOperatorProjectionLowering();
  RequireTableSelectStillUsesDml();
  RequireEngineDispatch();
  RequireEngineFunctionDispatch();
  RequireEngineNumericFunctionDispatch();
  RequireEngineAdditionalNumericFunctionDispatch();
  RequireEngineExtendedNumericFunctionDispatch();
  RequireEngineTextFunctionDispatch();
  RequireEngineTextJsonFuzzyFunctionDispatch();
  RequireEngineExtendedOperatorProjectionDispatch();
  RequireEngineSbsfc013DocumentCollectionProjectionDispatch();
  RequireEngineVectorFunctionDispatch();
  RequireEngineBinaryCryptoFunctionDispatch();
  RequireEngineTemporalSessionProviderFunctionDispatch();
  RequireEngineSpecialCurrentTimestampKeywordDispatch();
  RequireEngineCurrentValueFormDispatch();
  RequireEngineTemporalConstructorFunctionDispatch();
  RequireEngineTemporalFieldArithmeticFunctionDispatch();
  RequireEngineTemporalDateTimeBatchFunctionDispatch();
  RequireEngineProceduralContextFunctionDispatch();
  RequireEngineSbsfc016FixedPolicyLimitDispatch();
  RequireEngineSbsfc016LanguagePolicyDispatch();
  RequireEngineSbsfc016MetadataDispatch();
  RequireEngineSbsfc016ProceduralDiagnosticDispatch();
  RequireEngineCurrentSettingLiteralRefusalDispatch();
  RequireEngineMultiArgumentFunctionDispatch();
  RequireEngineConcatExpressionDispatch();
  RequireEngineOperatorProjectionDispatch();
  std::cout << "sbsql_query_scalar_projection_conformance=passed\n";
  return EXIT_SUCCESS;
}
