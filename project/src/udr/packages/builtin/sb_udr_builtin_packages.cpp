// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sb_udr_builtin_packages.hpp"

#ifdef SB_UDR_BUILTIN_HAS_ENGINE_OUTBOX
#include "cloud/external_effect_outbox.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::udr::builtin_packages {
namespace {

using runtime::UdrCallInput;
using runtime::UdrCallResult;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

// SEARCH_KEY: SB_UDR_BUILTIN_PACKAGE_CATALOG
constexpr std::array<BuiltinUdrPackageSpec, 49> kBuiltinPackages{{
    {"csv_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000001", "sbup_file_csv", "connector.file_provider.csv", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"tsv_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000002", "sbup_file_tsv", "connector.file_provider.tsv", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"json_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000003", "sbup_file_json", "connector.file_provider.json", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"xml_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000004", "sbup_file_xml", "connector.file_provider.xml", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"parquet_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000005", "sbup_file_parquet", "connector.file_provider.parquet", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"arrow_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000006", "sbup_file_arrow", "connector.file_provider.arrow", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"avro_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000007", "sbup_file_avro", "connector.file_provider.avro", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"protobuf_file_provider", "file_provider", "019f7100-0000-7000-8000-000000000008", "sbup_file_protobuf", "connector.file_provider.protobuf", "policy_admitted_metadata_only", "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED", true, false, false, false},
    {"blob_filter", "cross_cutting", "019f7100-0000-7000-8000-000000000009", "sbup_blob_filter", "udr.blob_filter", "policy_admitted_metadata_only", "UDR.BLOB_FILTER.POLICY_REQUIRED", false, false, false, false},
    {"remote_engine_connector", "connector", "019f7100-0000-7000-8000-000000000010", "sbup_remote_engine", "connector.remote_engine", "policy_admitted_metadata_only", "UDR.REMOTE_ENGINE.CONNECTOR_POLICY_REQUIRED", true, false, false, false},
    {"emulated_engine_support", "compatibility", "019f7100-0000-7000-8000-000000000011", "sbup_emulated_engine", "compatibility.emulated_engine", "policy_admitted_metadata_only", "UDR.EMULATED_ENGINE.POLICY_REQUIRED", false, false, false, false},
    {"generic_remote_engine_emulation", "connector", "019f7100-0000-7000-8000-000000000012", "sbup_remote_engine_emulation", "connector.remote_engine_emulation", "policy_admitted_metadata_only", "UDR.REMOTE_ENGINE_EMULATION.POLICY_REQUIRED", true, false, false, false},
    {"cluster_fabric", "cluster", "019f7100-0000-7000-8000-000000000013", "sbup_cluster_fabric", "cluster.fabric", "cluster_provider_required", "UDR.CLUSTER.PROVIDER_REQUIRED", false, false, false, true},
    {"side_effects_outbox", "cross_cutting", "019f7100-0000-7000-8000-000000000014", "sbup_side_effects_outbox", "udr.side_effects_outbox", "mga_outbox_required", "UDR.SIDE_EFFECTS.OUTBOX_REQUIRED", false, true, false, false},
    {"security_definer_context", "security", "019f7100-0000-7000-8000-000000000015", "sbup_security_definer_context", "security.definer_context", "security_context_required", "UDR.SECURITY_DEFINER.CONTEXT_REQUIRED", false, false, true, false},
    {"financial_analytics", "vertical", "019f7100-0000-7000-8000-000000000016", "sbup_financial_analytics", "vertical.financial_analytics", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"domain_finance_science_education", "vertical", "019f7100-0000-7000-8000-000000000017", "sbup_domain_finance_science_education", "vertical.domain_finance_science_education", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"healthcare_fhir", "vertical", "019f7100-0000-7000-8000-000000000018", "sbup_healthcare_fhir", "vertical.healthcare_fhir", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"dicom_metadata", "vertical", "019f7100-0000-7000-8000-000000000019", "sbup_dicom_metadata", "vertical.dicom_metadata", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"geospatial_crs", "vertical", "019f7100-0000-7000-8000-000000000020", "sbup_geospatial_crs", "vertical.geospatial_crs", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"business_calendar", "vertical", "019f7100-0000-7000-8000-000000000021", "sbup_business_calendar", "vertical.business_calendar", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"industry_interchange", "vertical", "019f7100-0000-7000-8000-000000000022", "sbup_industry_interchange", "vertical.industry_interchange", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"bayesian_modeling", "scientific", "019f7100-0000-7000-8000-000000000023", "sbup_bayesian_modeling", "vertical.bayesian_modeling", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"autodiff", "scientific", "019f7100-0000-7000-8000-000000000024", "sbup_autodiff", "vertical.autodiff", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"probability_stochastic", "scientific", "019f7100-0000-7000-8000-000000000025", "sbup_probability_stochastic", "vertical.probability_stochastic", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"optimization_solver", "scientific", "019f7100-0000-7000-8000-000000000026", "sbup_optimization_solver", "vertical.optimization_solver", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"graph_science", "scientific", "019f7100-0000-7000-8000-000000000027", "sbup_graph_science", "vertical.graph_science", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"differential_equations", "scientific", "019f7100-0000-7000-8000-000000000028", "sbup_differential_equations", "vertical.differential_equations", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"scientific_statistics_timeseries", "scientific", "019f7100-0000-7000-8000-000000000029", "sbup_scientific_statistics_timeseries", "vertical.scientific_statistics_timeseries", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"science_verticals_education", "scientific", "019f7100-0000-7000-8000-000000000030", "sbup_science_verticals_education", "vertical.science_verticals_education", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"numeric_array_expression", "numeric", "019f7100-0000-7000-8000-000000000031", "sbup_numeric_array_expression", "vertical.numeric_array_expression", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"labeled_nd_array", "numeric", "019f7100-0000-7000-8000-000000000032", "sbup_labeled_nd_array", "vertical.labeled_nd_array", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"units_uncertainty_exact_math", "numeric", "019f7100-0000-7000-8000-000000000033", "sbup_units_uncertainty_exact_math", "vertical.units_uncertainty_exact_math", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"symbolic_formula_codegen", "numeric", "019f7100-0000-7000-8000-000000000034", "sbup_symbolic_formula_codegen", "vertical.symbolic_formula_codegen", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"machine_learning", "ml_ai", "019f7100-0000-7000-8000-000000000035", "sbup_machine_learning", "vertical.machine_learning", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"columnar_interchange_parquet", "data_format", "019f7100-0000-7000-8000-000000000036", "sbup_columnar_interchange_parquet", "vertical.columnar_interchange_parquet", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", true, false, false, false},
    {"delimited_text_file", "data_format", "019f7100-0000-7000-8000-000000000037", "sbup_delimited_text_file", "vertical.delimited_text_file", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", true, false, false, false},
    {"document_extraction", "data_processing", "019f7100-0000-7000-8000-000000000038", "sbup_document_extraction", "vertical.document_extraction", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"text_language_nlp", "data_processing", "019f7100-0000-7000-8000-000000000039", "sbup_text_language_nlp", "vertical.text_language_nlp", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"entity_matching", "data_processing", "019f7100-0000-7000-8000-000000000040", "sbup_entity_matching", "vertical.entity_matching", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"data_contract_validation", "governance", "019f7100-0000-7000-8000-000000000041", "sbup_data_contract_validation", "vertical.data_contract_validation", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"data_quality_expectations", "governance", "019f7100-0000-7000-8000-000000000042", "sbup_data_quality_expectations", "vertical.data_quality_expectations", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"rules_policy_decision", "business_logic", "019f7100-0000-7000-8000-000000000043", "sbup_rules_policy_decision", "vertical.rules_policy_decision", "policy_admitted_metadata_only", "UDR.VERTICAL.PACKAGE_POLICY_REQUIRED", false, false, false, false},
    {"python_inspired_program", "procedural_runtime", "019f7100-0000-7000-8000-000000000044", "sbup_python_inspired_program", "external_tool_boundary", "trusted_cpp_wrapper_required", "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN", false, false, false, false},
    {"managed_wasm_wasi", "procedural_runtime", "019f7100-0000-7000-8000-000000000045", "sbup_managed_wasm_wasi", "external_tool_boundary", "trusted_cpp_wrapper_required", "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN", false, false, false, false},
    {"mssql_tds_bridge", "connector", "019f7100-0000-7000-8000-000000000046", "sbup_mssql_tds_bridge", "connector.mssql_tds", "policy_admitted_metadata_only", "UDR.CONNECTOR.POLICY_REQUIRED", true, false, false, false},
    {"odbc_datasource_connector", "connector", "019f7100-0000-7000-8000-000000000047", "sbup_odbc_datasource_connector", "connector.odbc_datasource", "policy_admitted_metadata_only", "UDR.CONNECTOR.POLICY_REQUIRED", true, false, false, false},
    {"function_dispatch_catalog", "function_dispatch", "019f7100-0000-7000-8000-000000000048", "sbup_function_dispatch_catalog", "udr.function_dispatch", "canonical_function_id_required", "UDR.FUNCTION_DISPATCH.CANONICAL_ID_REQUIRED", false, false, false, false},
    {"package_deployment_manifest", "deployment", "019f7100-0000-7000-8000-000000000049", "sbup_package_deployment_manifest", "udr.package_deployment", "descriptor_manifest_required", "UDR.PACKAGE_DEPLOYMENT.MANIFEST_REQUIRED", false, false, false, false},
}};

bool HasContextToken(std::string_view packet, std::string_view token) {
  return packet.find(token) != std::string_view::npos;
}

std::string ContextValue(std::string_view packet, std::string_view key) {
  const std::string needle = std::string(key) + "=";
  const std::size_t start = packet.find(needle);
  if (start == std::string_view::npos) return {};
  const std::size_t value_start = start + needle.size();
  const std::size_t value_end = packet.find(';', value_start);
  return std::string(packet.substr(value_start,
                                   value_end == std::string_view::npos
                                       ? std::string_view::npos
                                       : value_end - value_start));
}

std::vector<std::string> Split(std::string_view text, char delimiter) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(delimiter, start);
    out.emplace_back(text.substr(start,
                                 end == std::string_view::npos
                                     ? std::string_view::npos
                                     : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return out;
}

std::vector<std::string> Lines(std::string_view text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string line(text.substr(start, end - start));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) out.push_back(std::move(line));
    start = end + 1;
  }
  return out;
}

std::vector<std::string> SchemaColumnNames(std::string_view schema) {
  std::vector<std::string> out;
  for (const auto& part : Split(schema, ',')) {
    const std::size_t type_separator = part.find(':');
    out.push_back(type_separator == std::string::npos
                      ? part
                      : part.substr(0, type_separator));
  }
  return out;
}

bool ParseUnsigned(std::string_view text, std::size_t* value) {
  if (text.empty()) return false;
  std::size_t parsed = 0;
  for (const unsigned char ch : text) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
  }
  *value = parsed;
  return true;
}

bool ParseU64(std::string_view text, std::uint64_t* value) {
  if (text.empty()) return false;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : text) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  *value = parsed;
  return true;
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= kFnvPrime;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableHash(std::string_view prefix, std::string_view material) {
  return std::string(prefix) + Hex64(Fnv1a64(material));
}

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

std::string BoolJson(bool value) {
  return value ? "true" : "false";
}

std::string SpecJson(const BuiltinUdrPackageSpec& spec) {
  std::ostringstream out;
  out << "{\"package_uuid\":\"" << EscapeJson(spec.package_uuid) << "\","
      << "\"package_name\":\"" << EscapeJson(spec.package_name) << "\","
      << "\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
      << "\"category\":\"" << EscapeJson(spec.category) << "\","
      << "\"capability_role\":\"" << EscapeJson(spec.capability_role) << "\","
      << "\"release_policy\":\"" << EscapeJson(spec.release_policy) << "\","
      << "\"diagnostic_code\":\"" << EscapeJson(spec.diagnostic_code) << "\","
      << "\"trusted_cpp\":true,"
      << "\"abi_version\":\"sb_udr_v1\","
      << "\"engine_authority\":\"sblr_uuid_mga_security\","
      << "\"parser_execution_authority\":false,"
      << "\"external_io\":" << BoolJson(spec.external_io) << ","
      << "\"side_effects\":" << BoolJson(spec.side_effects) << ","
      << "\"security_definer\":" << BoolJson(spec.security_definer) << ","
      << "\"cluster_scoped\":" << BoolJson(spec.cluster_scoped) << "}";
  return out.str();
}

UdrCallResult Refuse(const BuiltinUdrPackageSpec& spec,
                     std::string_view detail) {
  std::ostringstream message;
  message << "{\"diagnostic\":\"" << EscapeJson(spec.diagnostic_code) << "\","
          << "\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"release_policy\":\"" << EscapeJson(spec.release_policy) << "\","
          << "\"detail\":\"" << EscapeJson(detail) << "\","
          << "\"engine_mga_authority\":true,"
          << "\"engine_security_authority\":true,"
          << "\"sblr_uuid_revalidation_required\":true,"
          << "\"no_external_effects\":true,"
          << "\"no_parser_execution_authority\":true}";
  return {false, {}, message.str()};
}

UdrCallResult FileProviderBudgetRefusal(const BuiltinUdrPackageSpec& spec,
                                        std::string_view detail) {
  std::ostringstream message;
  message << "{\"diagnostic\":\"UDR.FILE_PROVIDER.RESOURCE_BUDGET_EXCEEDED\","
          << "\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"detail\":\"" << EscapeJson(detail) << "\","
          << "\"engine_mga_authority\":true,"
          << "\"engine_security_authority\":true,"
          << "\"no_external_effects\":true}";
  return {false, {}, message.str()};
}

UdrCallResult FileProviderSchemaRefusal(const BuiltinUdrPackageSpec& spec,
                                        std::string_view detail) {
  std::ostringstream message;
  message << "{\"diagnostic\":\"UDR.FILE_PROVIDER.SCHEMA_MISMATCH\","
          << "\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"detail\":\"" << EscapeJson(detail) << "\","
          << "\"engine_mga_authority\":true,"
          << "\"engine_security_authority\":true,"
          << "\"no_external_effects\":true}";
  return {false, {}, message.str()};
}

UdrCallResult SideEffectRefusal(const BuiltinUdrPackageSpec& spec,
                                std::string_view detail,
                                std::string_view diagnostic = "UDR.SIDE_EFFECTS.OUTBOX_REQUIRED") {
  std::ostringstream message;
  message << "{\"diagnostic\":\"" << EscapeJson(diagnostic) << "\","
          << "\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"detail\":\"" << EscapeJson(detail) << "\","
          << "\"external_effect_visible\":false,"
          << "\"mga_evidence_before_success\":false,"
          << "\"engine_mga_authority\":true,"
          << "\"engine_security_authority\":true,"
          << "\"no_parser_execution_authority\":true,"
          << "\"no_external_effects\":true}";
  return {false, {}, message.str()};
}

UdrCallResult SecurityDefinerRefusal(
    const BuiltinUdrPackageSpec& spec,
    std::string_view detail,
    std::string_view diagnostic = "UDR.SECURITY_DEFINER.CONTEXT_REQUIRED") {
  std::ostringstream message;
  message << "{\"diagnostic\":\"" << EscapeJson(diagnostic) << "\","
          << "\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"detail\":\"" << EscapeJson(detail) << "\","
          << "\"payload_context_ignored\":true,"
          << "\"engine_security_authority\":true,"
          << "\"engine_mga_authority\":true,"
          << "\"no_parser_execution_authority\":true,"
          << "\"no_external_effects\":true}";
  return {false, {}, message.str()};
}

UdrCallResult ExecuteFileProvider(const BuiltinUdrPackageSpec& spec,
                                  const UdrCallInput& input) {
  const std::string credential_ref = ContextValue(input.context_packet, "credential_ref");
  const std::string path_policy = ContextValue(input.context_packet, "path_policy");
  const std::string path_root_uuid = ContextValue(input.context_packet, "path_root_uuid");
  if (credential_ref.empty() || path_policy.empty() || path_root_uuid.empty()) {
      return Refuse(spec, "credential_ref_path_policy_and_path_root_uuid_required");
  }

  std::string source = input.payload;
  const std::string source_path = ContextValue(input.context_packet, "source_path");
  if (!source_path.empty()) {
    const std::string path_root = ContextValue(input.context_packet, "path_root");
    if (path_root.empty()) return Refuse(spec, "path_root_required_for_source_path");
    if (source_path.size() < path_root.size() ||
        source_path.substr(0, path_root.size()) != path_root) {
      return Refuse(spec, "source_path_outside_admitted_path_root");
    }
    std::ifstream stream(source_path);
    if (!stream.is_open()) return Refuse(spec, "source_path_not_readable");
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    source = buffer.str();
  }

  const char delimiter = spec.family_id == std::string_view("tsv_file_provider") ? '\t' : ',';
  const std::vector<std::string> lines = Lines(source);
  if (lines.empty()) return FileProviderSchemaRefusal(spec, "inline_stream_header_required");
  const std::vector<std::string> columns = Split(lines.front(), delimiter);
  if (columns.empty()) return FileProviderSchemaRefusal(spec, "inline_stream_columns_required");

  const std::string schema = ContextValue(input.context_packet, "schema");
  if (!schema.empty()) {
    const auto expected_columns = SchemaColumnNames(schema);
    if (expected_columns != columns) {
      return FileProviderSchemaRefusal(spec, "schema_columns_do_not_match_stream_header");
    }
  }

  std::vector<std::vector<std::string>> rows;
  for (std::size_t index = 1; index < lines.size(); ++index) {
    auto values = Split(lines[index], delimiter);
    if (values.size() != columns.size()) {
      return FileProviderSchemaRefusal(spec, "row_arity_does_not_match_stream_header");
    }
    rows.push_back(std::move(values));
  }

  const std::string pushdown = ContextValue(input.context_packet, "pushdown");
  std::string pushdown_column;
  std::string pushdown_value;
  if (!pushdown.empty()) {
    const std::size_t equals = pushdown.find('=');
    if (equals == std::string::npos) {
      return FileProviderSchemaRefusal(spec, "pushdown_requires_column_equals_value");
    }
    pushdown_column = pushdown.substr(0, equals);
    pushdown_value = pushdown.substr(equals + 1);
  }

  std::size_t pushdown_column_index = columns.size();
  if (!pushdown_column.empty()) {
    const auto it = std::find(columns.begin(), columns.end(), pushdown_column);
    if (it == columns.end()) {
      return FileProviderSchemaRefusal(spec, "pushdown_column_not_in_schema");
    }
    pushdown_column_index = static_cast<std::size_t>(std::distance(columns.begin(), it));
  }

  std::vector<std::vector<std::string>> projected_rows;
  for (const auto& row : rows) {
    if (pushdown_column_index != columns.size() &&
        row[pushdown_column_index] != pushdown_value) {
      continue;
    }
    projected_rows.push_back(row);
  }

  const std::string max_rows_text = ContextValue(input.context_packet, "max_output_rows");
  if (!max_rows_text.empty()) {
    std::size_t max_rows = 0;
    if (!ParseUnsigned(max_rows_text, &max_rows)) {
      return FileProviderBudgetRefusal(spec, "max_output_rows_must_be_unsigned");
    }
    if (projected_rows.size() > max_rows) {
      return FileProviderBudgetRefusal(spec, "max_output_rows_exceeded");
    }
  }

  std::ostringstream payload;
  payload << "{\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"format\":\"" << EscapeJson(spec.capability_role) << "\","
          << "\"credential_ref\":\"" << EscapeJson(credential_ref) << "\","
          << "\"path_policy\":\"" << EscapeJson(path_policy) << "\","
          << "\"path_root_uuid\":\"" << EscapeJson(path_root_uuid) << "\","
          << "\"source_path_admitted\":" << BoolJson(!source_path.empty()) << ","
          << "\"schema_enforced\":" << BoolJson(!schema.empty()) << ","
          << "\"pushdown_applied\":" << BoolJson(!pushdown.empty()) << ","
          << "\"streaming\":" << BoolJson(HasContextToken(input.context_packet, "stream_chunk_bytes=")) << ","
          << "\"resource_budget_checked\":" << BoolJson(!max_rows_text.empty()) << ","
          << "\"input_rows\":" << rows.size() << ","
          << "\"output_rows\":" << projected_rows.size() << ","
          << "\"columns\":[";
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) payload << ',';
    payload << '"' << EscapeJson(columns[index]) << '"';
  }
  payload << "],\"rows\":[";
  for (std::size_t row_index = 0; row_index < projected_rows.size(); ++row_index) {
    if (row_index != 0) payload << ',';
    payload << '{';
    for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
      if (column_index != 0) payload << ',';
      payload << '"' << EscapeJson(columns[column_index]) << "\":\""
              << EscapeJson(projected_rows[row_index][column_index]) << '"';
    }
    payload << '}';
  }
  payload << "],\"engine_mga_authority\":true,"
          << "\"engine_security_authority\":true,"
          << "\"sblr_uuid_revalidation_required\":true,"
          << "\"no_external_effects\":true}";

  return {true, payload.str(), "{\"diagnostic\":\"UDR.FILE_PROVIDER.STREAM_READ\","
                               "\"engine_mga_authority\":true,"
                               "\"engine_security_authority\":true,"
                               "\"no_external_effects\":true}"};
}

UdrCallResult ExecuteSideEffectsOutbox(const BuiltinUdrPackageSpec& spec,
                                       const UdrCallInput& input) {
  const std::string idempotency_key = ContextValue(input.context_packet, "idempotency_key");
  const std::string transaction_uuid = ContextValue(input.context_packet, "transaction_uuid");
  const std::string local_tx_text = ContextValue(input.context_packet, "local_transaction_id");
  const std::string generation_text =
      ContextValue(input.context_packet, "transaction_inventory_generation");
  const std::string commit_hash = ContextValue(input.context_packet, "commit_evidence_hash");
  const std::string finality_mode = ContextValue(input.context_packet, "finality_mode");
  const std::string provider_profile_uuid =
      ContextValue(input.context_packet, "provider_profile_uuid");
  const std::string redaction_policy_uuid =
      ContextValue(input.context_packet, "redaction_policy_uuid");
  const std::string source_object_ref =
      ContextValue(input.context_packet, "source_object_ref");
  if (!HasContextToken(input.context_packet, "mga_evidence=true") ||
      idempotency_key.empty() ||
      transaction_uuid.empty() ||
      local_tx_text.empty() ||
      generation_text.empty() ||
      commit_hash.empty() ||
      finality_mode.empty() ||
      provider_profile_uuid.empty() ||
      redaction_policy_uuid.empty() ||
      source_object_ref.empty()) {
    return SideEffectRefusal(
        spec,
        "mga_commit_evidence_idempotency_provider_redaction_and_source_required");
  }
  if (!HasContextToken(input.context_packet, "mga_commit_visible=true") ||
      !HasContextToken(input.context_packet, "durable_commit_evidence=true")) {
    return SideEffectRefusal(spec,
                             "side_effects_admitted_only_after_durable_mga_commit",
                             "UDR.SIDE_EFFECTS.PRECOMMIT_REFUSED");
  }

  std::uint64_t local_tx = 0;
  std::uint64_t generation = 0;
  if (!ParseU64(local_tx_text, &local_tx) ||
      !ParseU64(generation_text, &generation) ||
      local_tx == 0 ||
      generation == 0) {
    return SideEffectRefusal(spec, "local_transaction_and_inventory_generation_must_be_positive");
  }

#ifndef SB_UDR_BUILTIN_HAS_ENGINE_OUTBOX
  return SideEffectRefusal(spec,
                           "external_effect_outbox_api_unavailable",
                           "UDR.SIDE_EFFECTS.OUTBOX_API_UNAVAILABLE");
#else
  namespace api = scratchbird::engine::internal_api;
  api::EngineExternalEffectOutboxAdmission admission;
  admission.commit_evidence.transaction_uuid = transaction_uuid;
  admission.commit_evidence.local_transaction_id = local_tx;
  admission.commit_evidence.transaction_inventory_generation = generation;
  admission.commit_evidence.commit_evidence_hash = commit_hash;
  admission.commit_evidence.finality_mode = finality_mode;
  admission.commit_evidence.mga_commit_visible = true;
  admission.commit_evidence.durable_commit_evidence = true;
  admission.source_object_ref = source_object_ref;
  admission.effect_class = ContextValue(input.context_packet, "effect_class");
  if (admission.effect_class.empty()) {
    admission.effect_class = "udr_side_effect";
  }
  admission.provider_profile_uuid = provider_profile_uuid;
  admission.idempotency_key = idempotency_key;
  admission.redaction_policy_uuid = redaction_policy_uuid;
  admission.payload_hash = ContextValue(input.context_packet, "payload_hash");
  if (admission.payload_hash.empty()) {
    admission.payload_hash =
        StableHash("udr-payload-", input.payload + "|" + idempotency_key);
  }
  admission.payload_metadata = "udr_family=" + std::string(spec.family_id) +
                               ";redacted=true;payload_hash_only=true";
  admission.audit_event_uuid = ContextValue(input.context_packet, "audit_event_uuid");
  std::uint64_t now = 0;
  if (ParseU64(ContextValue(input.context_packet, "now_epoch_ms"), &now)) {
    admission.now_epoch_ms = now;
  }
  std::uint64_t max_pending = 0;
  if (ParseU64(ContextValue(input.context_packet, "max_pending_records"), &max_pending) &&
      max_pending > 0) {
    admission.max_pending_records = max_pending;
  }

  const auto admitted = api::AdmitExternalEffectAfterCommit(admission);
  if (!admitted.ok) {
    std::string diagnostic = "UDR.SIDE_EFFECTS.OUTBOX_ADMISSION_FAILED";
    std::string detail = "external_effect_outbox_refused";
    if (!admitted.diagnostics.empty()) {
      diagnostic = admitted.diagnostics.front().code;
      detail = admitted.diagnostics.front().detail;
    }
    return SideEffectRefusal(spec, detail, diagnostic);
  }

  std::ostringstream payload;
  payload << "{\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"outbox_event_uuid\":\""
          << EscapeJson(admitted.record.outbox_event_uuid) << "\","
          << "\"idempotency_key\":\""
          << EscapeJson(admitted.record.idempotency_key) << "\","
          << "\"source_transaction_uuid\":\""
          << EscapeJson(admitted.record.source_transaction_uuid) << "\","
          << "\"effect_class\":\"" << EscapeJson(admitted.record.effect_class) << "\","
          << "\"provider_profile_uuid\":\""
          << EscapeJson(admitted.record.provider_profile_uuid) << "\","
          << "\"redaction_policy_uuid\":\""
          << EscapeJson(admitted.record.redaction_policy_uuid) << "\","
          << "\"payload_hash\":\"" << EscapeJson(admitted.record.payload_hash) << "\","
          << "\"final_state\":\"" << EscapeJson(admitted.record.final_state) << "\","
          << "\"idempotent_replay\":" << BoolJson(admitted.deduplicated) << ","
          << "\"outbox_durable_intent\":true,"
          << "\"mga_evidence_before_success\":true,"
          << "\"external_effect_visible\":false,"
          << "\"engine_mga_authority\":true,"
          << "\"engine_security_authority\":true,"
          << "\"sblr_uuid_revalidation_required\":true,"
          << "\"no_parser_execution_authority\":true}";
  std::ostringstream message;
  message << "{\"diagnostic\":\""
          << (admitted.deduplicated ? "UDR.SIDE_EFFECTS.IDEMPOTENT_REPLAY"
                                    : "UDR.SIDE_EFFECTS.OUTBOX_ADMITTED")
          << "\","
          << "\"outbox_event_uuid\":\""
          << EscapeJson(admitted.record.outbox_event_uuid) << "\","
          << "\"idempotency_key\":\""
          << EscapeJson(admitted.record.idempotency_key) << "\","
          << "\"mga_evidence_before_success\":true,"
          << "\"external_effect_visible\":false,"
          << "\"engine_mga_authority\":true,"
          << "\"engine_security_authority\":true}";
  return {true, payload.str(), message.str()};
#endif
}

UdrCallResult ExecuteSecurityDefinerContext(const BuiltinUdrPackageSpec& spec,
                                            const UdrCallInput& input) {
  const std::string caller = ContextValue(input.context_packet, "caller_principal_uuid");
  const std::string effective = ContextValue(input.context_packet, "effective_principal_uuid");
  const std::string role_chain = ContextValue(input.context_packet, "role_chain_proof");
  const std::string group_chain = ContextValue(input.context_packet, "group_chain_proof");
  const std::string policy_epoch = ContextValue(input.context_packet, "policy_epoch");
  const std::string security_epoch = ContextValue(input.context_packet, "security_epoch");
  const std::string masking_epoch = ContextValue(input.context_packet, "masking_epoch");
  const std::string authorization = ContextValue(input.context_packet,
                                                 "security_definer_authorized");
  if (caller.empty() ||
      effective.empty() ||
      role_chain.empty() ||
      group_chain.empty() ||
      policy_epoch.empty() ||
      security_epoch.empty() ||
      masking_epoch.empty() ||
      authorization != "true") {
    return SecurityDefinerRefusal(
        spec,
        "caller_effective_role_group_policy_security_masking_and_authorization_required");
  }
  if (HasContextToken(input.payload, "effective_principal_uuid=") ||
      HasContextToken(input.payload, "caller_principal_uuid=") ||
      HasContextToken(input.payload, "security_definer_authorized=true")) {
    return SecurityDefinerRefusal(
        spec,
        "payload_principal_or_authorization_context_is_spoofable",
        "UDR.SECURITY_DEFINER.SPOOFED_PAYLOAD_CONTEXT");
  }

  std::ostringstream payload;
  payload << "{\"family_id\":\"" << EscapeJson(spec.family_id) << "\","
          << "\"caller_principal_uuid\":\"" << EscapeJson(caller) << "\","
          << "\"effective_principal_uuid\":\"" << EscapeJson(effective) << "\","
          << "\"role_chain_proof\":\"" << EscapeJson(role_chain) << "\","
          << "\"group_chain_proof\":\"" << EscapeJson(group_chain) << "\","
          << "\"policy_epoch\":\"" << EscapeJson(policy_epoch) << "\","
          << "\"security_epoch\":\"" << EscapeJson(security_epoch) << "\","
          << "\"masking_epoch\":\"" << EscapeJson(masking_epoch) << "\","
          << "\"payload_context_ignored\":true,"
          << "\"redaction_enforced\":true,"
          << "\"engine_security_authority\":true,"
          << "\"engine_mga_authority\":true,"
          << "\"sblr_uuid_revalidation_required\":true,"
          << "\"no_parser_execution_authority\":true,"
          << "\"no_external_effects\":true}";
  return {true, payload.str(), "{\"diagnostic\":\"UDR.SECURITY_DEFINER.CONTEXT_ADMITTED\","
                               "\"payload_context_ignored\":true,"
                               "\"redaction_enforced\":true,"
                               "\"engine_security_authority\":true,"
                               "\"engine_mga_authority\":true}"};
}

UdrCallResult Describe(const UdrCallInput& input) {
  const auto* spec = FindBuiltinUdrPackageSpec(input.package_uuid);
  if (spec == nullptr) {
    return {false, {}, "{\"diagnostic\":\"UDR.BUILTIN.PACKAGE_UNKNOWN\"}"};
  }
  return {true, SpecJson(*spec), "{\"diagnostic\":\"UDR.BUILTIN.DESCRIBED\"}"};
}

UdrCallResult ValidateAdmission(const UdrCallInput& input) {
  const auto* spec = FindBuiltinUdrPackageSpec(input.package_uuid);
  if (spec == nullptr) {
    return {false, {}, "{\"diagnostic\":\"UDR.BUILTIN.PACKAGE_UNKNOWN\"}"};
  }
  if (!HasContextToken(input.context_packet, "engine_context=trusted") ||
      !HasContextToken(input.context_packet, "sblr_authorized_invocation=true")) {
    return {false, {}, "{\"diagnostic\":\"UDR.BUILTIN.CONTEXT_REQUIRED\","
                       "\"required\":\"engine_context=trusted;sblr_authorized_invocation=true\"}"};
  }
  if (spec->cluster_scoped &&
      !HasContextToken(input.context_packet, "cluster_provider_authority=true")) {
    return Refuse(*spec, "cluster_provider_authority_required");
  }
  return {true, SpecJson(*spec), "{\"diagnostic\":\"UDR.BUILTIN.ADMISSION_VALIDATED\","
                                 "\"engine_mga_authority\":true,"
                                 "\"engine_security_authority\":true}"};
}

UdrCallResult Execute(const UdrCallInput& input) {
  const auto* spec = FindBuiltinUdrPackageSpec(input.package_uuid);
  if (spec == nullptr) {
    return {false, {}, "{\"diagnostic\":\"UDR.BUILTIN.PACKAGE_UNKNOWN\"}"};
  }
  if (!HasContextToken(input.context_packet, "engine_context=trusted") ||
      !HasContextToken(input.context_packet, "sblr_authorized_invocation=true")) {
    return {false, {}, "{\"diagnostic\":\"UDR.BUILTIN.CONTEXT_REQUIRED\","
                       "\"required\":\"engine_context=trusted;sblr_authorized_invocation=true\"}"};
  }
  if (spec->external_io &&
      !HasContextToken(input.context_packet, "credential_ref=")) {
    return Refuse(*spec, "credential_ref_and_path_policy_required");
  }
  if (spec->category == std::string_view("file_provider")) {
    return ExecuteFileProvider(*spec, input);
  }
  if (spec->side_effects) {
    return ExecuteSideEffectsOutbox(*spec, input);
  }
  if (spec->security_definer) {
    return ExecuteSecurityDefinerContext(*spec, input);
  }
  if (spec->cluster_scoped &&
      !HasContextToken(input.context_packet, "cluster_provider_authority=true")) {
    return Refuse(*spec, "cluster_provider_authority_required");
  }
  return Refuse(*spec, "release_policy_refusal_before_external_effects");
}

}  // namespace

std::span<const BuiltinUdrPackageSpec> BuiltinUdrPackageSpecs() {
  return kBuiltinPackages;
}

const BuiltinUdrPackageSpec* FindBuiltinUdrPackageSpec(std::string_view package_uuid) {
  for (const auto& spec : kBuiltinPackages) {
    if (spec.package_uuid == package_uuid) return &spec;
  }
  return nullptr;
}

runtime::UdrPackageDescriptor BuiltinUdrPackageDescriptor(
    const BuiltinUdrPackageSpec& spec) {
  runtime::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = spec.package_uuid;
  descriptor.package_name = spec.package_name;
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "scratchbird-builtin-udr-catalog-v1";
  descriptor.binary_hash = "sha256:scratchbird-builtin-udr-catalog-v1";
  descriptor.signature_policy = "release-key-or-local-dev-admission";
  descriptor.capability_role = spec.capability_role;
  descriptor.runtime_language = "cpp";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints = {
      {"describe_capabilities", "metadata", &Describe},
      {"validate_admission", "policy", &ValidateAdmission},
      {"execute", "operation", &Execute},
  };
  return descriptor;
}

std::vector<runtime::UdrPackageDescriptor> BuiltinUdrPackageDescriptors() {
  std::vector<runtime::UdrPackageDescriptor> descriptors;
  descriptors.reserve(kBuiltinPackages.size());
  for (const auto& spec : kBuiltinPackages) {
    descriptors.push_back(BuiltinUdrPackageDescriptor(spec));
  }
  return descriptors;
}

std::vector<BuiltinUdrPackageDeploymentManifestRow>
BuiltinUdrPackageDeploymentManifest() {
  std::vector<BuiltinUdrPackageDeploymentManifestRow> rows;
  rows.reserve(kBuiltinPackages.size());
  for (const auto& spec : kBuiltinPackages) {
    const auto descriptor = BuiltinUdrPackageDescriptor(spec);
    std::ostringstream entrypoints;
    for (std::size_t index = 0; index < descriptor.entrypoints.size(); ++index) {
      if (index != 0) entrypoints << ';';
      entrypoints << descriptor.entrypoints[index].name << ':'
                  << descriptor.entrypoints[index].role;
    }
    BuiltinUdrPackageDeploymentManifestRow row;
    row.package_uuid = descriptor.package_uuid;
    row.package_name = descriptor.package_name;
    row.family_id = spec.family_id;
    row.category = spec.category;
    row.abi_version = descriptor.abi_version;
    row.runtime_language = descriptor.runtime_language;
    row.source_revision = descriptor.source_revision;
    row.binary_hash = descriptor.binary_hash;
    row.signature_policy = descriptor.signature_policy;
    row.capability_role = descriptor.capability_role;
    row.release_policy = spec.release_policy;
    row.diagnostic_code = spec.diagnostic_code;
    row.entrypoints_csv = entrypoints.str();
    row.install_component = "lib/scratchbird/udr";
    row.public_abi_status = "frozen_builtin_udr_sb_udr_v1";
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string BuiltinUdrPackageDeploymentManifestJson() {
  const auto rows = BuiltinUdrPackageDeploymentManifest();
  std::ostringstream out;
  out << "{\"manifest_kind\":\"builtin_trusted_cpp_udr_packages\","
      << "\"public_abi_status\":\"frozen_builtin_udr_sb_udr_v1\","
      << "\"abi_version\":\"sb_udr_v1\","
      << "\"package_count\":" << rows.size() << ","
      << "\"packages\":[";
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    if (index != 0) out << ',';
    out << "{\"package_uuid\":\"" << EscapeJson(row.package_uuid) << "\","
        << "\"package_name\":\"" << EscapeJson(row.package_name) << "\","
        << "\"family_id\":\"" << EscapeJson(row.family_id) << "\","
        << "\"category\":\"" << EscapeJson(row.category) << "\","
        << "\"abi_version\":\"" << EscapeJson(row.abi_version) << "\","
        << "\"runtime_language\":\"" << EscapeJson(row.runtime_language) << "\","
        << "\"source_revision\":\"" << EscapeJson(row.source_revision) << "\","
        << "\"binary_hash\":\"" << EscapeJson(row.binary_hash) << "\","
        << "\"signature_policy\":\"" << EscapeJson(row.signature_policy) << "\","
        << "\"capability_role\":\"" << EscapeJson(row.capability_role) << "\","
        << "\"release_policy\":\"" << EscapeJson(row.release_policy) << "\","
        << "\"diagnostic_code\":\"" << EscapeJson(row.diagnostic_code) << "\","
        << "\"entrypoints_csv\":\"" << EscapeJson(row.entrypoints_csv) << "\","
        << "\"install_component\":\"" << EscapeJson(row.install_component) << "\","
        << "\"public_abi_status\":\"" << EscapeJson(row.public_abi_status) << "\"}";
  }
  out << "]}";
  return out.str();
}

}  // namespace scratchbird::udr::builtin_packages
