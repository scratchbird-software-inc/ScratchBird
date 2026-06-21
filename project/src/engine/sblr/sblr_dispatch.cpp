// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"

#include "cluster_provider/cluster_provider.hpp"
#include "sblr_context_variables.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_operator_runtime.hpp"

#include "agents/agent_action_hooks_api.hpp"
#include "agents/agent_management_api.hpp"
#include "api_diagnostics.hpp"
#include "artifacts/artifact_api.hpp"
#include "catalog/descriptor_api.hpp"
#include "catalog/descriptor_mutation_api.hpp"
#include "catalog/catalog_lookup_api.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/schema_tree_api.hpp"
#include "cluster/cluster_control_api.hpp"
#include "cluster/cluster_inspect_api.hpp"
#include "cluster/cluster_insert_route_api.hpp"
#include "cluster/placement_api.hpp"
#include "cluster/profile_operation_api.hpp"
#include "cluster/remote_participant_insert_api.hpp"
#include "cluster/replication_api.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/comment_api.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/import_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/import_reject_model.hpp"
#include "dml/import_resume_checkpoint.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "dispatch/function_dispatch.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "extensibility/gpu_api.hpp"
#include "extensibility/llvm_api.hpp"
#include "extensibility/parser_package_api.hpp"
#include "extensibility/udr_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "management/config_api.hpp"
#include "management/index_management_api.hpp"
#include "management/management_api.hpp"
#include "management/memory_management_api.hpp"
#include "management/support_bundle_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/nosql_backpressure_debt_api.hpp"
#include "nosql/nosql_family_maintenance_api.hpp"
#include "nosql/nosql_statistics_api.hpp"
#include "nosql/search_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"
#include "notification/notification_api.hpp"
#include "observability/explain_api.hpp"
#include "observability/metrics_api.hpp"
#include "observability/show_api.hpp"
#include "procedural/procedural_api.hpp"
#include "query/expression_api.hpp"
#include "query/plan_api.hpp"
#include "query/predicate_api.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "security/audit_api.hpp"
#include "security/auth_challenge_api.hpp"
#include "security/auth_credential_api.hpp"
#include "security/auth_provider_observability_api.hpp"
#include "security/auth_provider_plugin_api.hpp"
#include "security/auth_provider_policy_api.hpp"
#include "security/auth_token_api.hpp"
#include "security/authentication_api.hpp"
#include "security/authorization_api.hpp"
#include "security/authority_api.hpp"
#include "security/deep_enforcement_api.hpp"
#include "security/external_group_api.hpp"
#include "security/grant_api.hpp"
#include "security/identity_api.hpp"
#include "security/policy_api.hpp"
#include "security/plugin_trust_api.hpp"
#include "security/protected_material_api.hpp"
#include "security/standard_bundle_api.hpp"
#include "security/visibility_api.hpp"
#include "security/security_model.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "storage/storage_management_api.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace functions = scratchbird::engine::functions;
namespace {

using SblrSteadyClock = std::chrono::steady_clock;

std::uint64_t SblrElapsedMicros(SblrSteadyClock::time_point start,
                                SblrSteadyClock::time_point finish) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void WriteBaseApiPhaseTrace(const SblrOperationEnvelope& envelope,
                            std::uint64_t operand_loop_us,
                            std::uint64_t compact_materialize_us,
                            std::uint64_t total_us,
                            std::size_t row_count,
                            std::size_t operand_count) {
  const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_BASE_API_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "operation=" << envelope.operation_id
      << "\toperands=" << operand_count
      << "\trows=" << row_count
      << "\toperand_loop_us=" << operand_loop_us
      << "\tcompact_materialize_us=" << compact_materialize_us
      << "\ttotal_us=" << total_us
      << '\n';
}

void WriteSblrDispatchPhaseTrace(
    std::string_view layer,
    std::string_view operation_id,
    std::size_t encoded_size,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "layer=" << layer
      << "\toperation=" << operation_id
      << "\tencoded_bytes=" << encoded_size;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << '\n';
}

std::string LowerAscii(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool HexDecodeBytes(std::string_view text, std::vector<std::uint8_t>* out) {
  if (out == nullptr || (text.size() % 2) != 0) return false;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const int high = HexValue(text[index]);
    const int low = HexValue(text[index + 1]);
    if (high < 0 || low < 0) return false;
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  *out = std::move(bytes);
  return true;
}

std::string FormatReal64(double value) {
  std::ostringstream encoded;
  encoded << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return encoded.str();
}

std::string HexEncodeBytes(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

SblrEnvelopeDiagnostic DispatchDiagnostic(std::string code, std::string message) {
  return SblrEnvelopeDiagnostic{std::move(code), std::move(message), true};
}

bool HasDispatchDiagnosticCode(const SblrDispatchResult& result,
                               std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

void PropagateClusterApiDiagnostics(SblrDispatchResult* result) {
  if (result == nullptr || result->api_result.ok) {
    return;
  }
  for (const auto& diagnostic : result->api_result.diagnostics) {
    if (std::string_view(diagnostic.code).rfind("SBLR.CLUSTER.", 0) != 0 ||
        HasDispatchDiagnosticCode(*result, diagnostic.code)) {
      continue;
    }
    result->diagnostics.push_back(DispatchDiagnostic(
        diagnostic.code,
        diagnostic.detail.empty() ? diagnostic.message_key : diagnostic.detail));
  }
}

std::string UnquoteSbsqlLiteral(std::string value) {
  if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') {
    return value;
  }
  std::string out;
  out.reserve(value.size() - 2);
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    if (value[index] == '\'' && index + 1 < value.size() - 1 && value[index + 1] == '\'') {
      out.push_back('\'');
      ++index;
    } else {
      out.push_back(value[index]);
    }
  }
  return out;
}

std::vector<std::string> SplitCommaSeparatedLiterals(std::string_view value) {
  std::vector<std::string> parts;
  std::string current;
  bool in_string = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (ch == '\'') {
      current.push_back(ch);
      if (in_string && index + 1 < value.size() && value[index + 1] == '\'') {
        current.push_back(value[index + 1]);
        ++index;
        continue;
      }
      in_string = !in_string;
      continue;
    }
    if (ch == ',' && !in_string) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  parts.push_back(current);
  return parts;
}

api::EngineTypedValue TypedValueFromLoweredLiteral(std::string value,
                                                   std::string type_name) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  if (type_name.empty()) type_name = "text";
  if (type_name == "integer") type_name = "bigint";
  typed.descriptor.canonical_type_name = type_name;
  typed.descriptor.encoded_descriptor = "type=" + type_name;
  typed.is_null = type_name == "null";
  typed.encoded_value = typed.is_null ? std::string{} : UnquoteSbsqlLiteral(std::move(value));
  return typed;
}

std::optional<std::string_view> TextOperandValue(
    const SblrOperationEnvelope& envelope,
    std::string_view name) {
  for (const auto& operand : envelope.operands) {
    if (operand.type == "text" && operand.name == name) {
      return std::string_view(operand.value);
    }
  }
  return std::nullopt;
}

std::uint64_t ParseCompactU64(std::string_view value) {
  std::uint64_t out = 0;
  if (value.empty()) return 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return 0;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return 0;
    }
    out = out * 10u + digit;
  }
  return out;
}

bool CompactBool(std::string_view value) {
  const std::string lower = LowerAscii(std::string(value));
  return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

bool LooksLikeOrdinalInsertFieldName(std::string_view name) {
  if (name.size() < 2 || name.front() != 'c') return false;
  for (std::size_t index = 1; index < name.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(name[index]))) return false;
  }
  return true;
}

bool HexDecodeString(std::string_view text, std::string* out) {
  if (out == nullptr) return false;
  std::vector<std::uint8_t> bytes;
  if (!HexDecodeBytes(text, &bytes)) return false;
  out->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

std::vector<std::string_view> SplitCompactInsertCell(std::string_view cell) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= cell.size()) {
    const std::size_t end = cell.find('|', start);
    parts.push_back(cell.substr(
        start,
        end == std::string_view::npos ? cell.size() - start : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  return parts;
}

struct CompactInsertValueCell {
  std::string name;
  std::string type;
  std::string value;
  bool is_null = false;
};

std::vector<CompactInsertValueCell> DecodeCompactInsertCells(
    std::string_view payload,
    std::uint64_t row_count,
    std::uint64_t column_count) {
  const std::uint64_t cell_count = row_count * column_count;
  std::vector<CompactInsertValueCell> cells;
  if (payload.empty() || row_count == 0 || column_count == 0 ||
      cell_count > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max())) {
    return cells;
  }
  cells.resize(static_cast<std::size_t>(cell_count));
  std::uint64_t ordinal = 0;
  std::size_t start = 0;
  while (start <= payload.size()) {
    const std::size_t end = payload.find(';', start);
    const std::string_view cell =
        payload.substr(start,
                       end == std::string_view::npos ? payload.size() - start
                                                     : end - start);
    if (ordinal >= cell_count) return {};
    const auto parts = SplitCompactInsertCell(cell);
    if (parts.size() != 4) return {};
    auto& target = cells[static_cast<std::size_t>(ordinal)];
    if (!HexDecodeString(parts[0], &target.name) ||
        !HexDecodeString(parts[1], &target.type) ||
        !HexDecodeString(parts[2], &target.value)) {
      return {};
    }
    target.is_null = CompactBool(parts[3]);
    ++ordinal;
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  if (ordinal != cell_count) return {};
  return cells;
}

std::vector<std::string> CompactDescriptorColumnNames(
    const SblrOperationEnvelope& envelope,
    std::uint64_t column_count) {
  std::vector<std::string> columns;
  columns.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(column_count, 1024)));
  for (std::uint64_t index = 0; index < column_count; ++index) {
    const auto value = TextOperandValue(
        envelope,
        "insert_values_descriptor_column_" + std::to_string(index));
    columns.push_back(value ? std::string(*value) : std::string{});
  }
  return columns;
}

std::vector<std::string> LoadDescriptorColumnNamesForCompactInsert(
    const api::EngineApiRequest& request,
    std::uint64_t column_count) {
  std::vector<std::string> columns;
  if (column_count == 0 || request.target_object.uuid.canonical.empty()) {
    return columns;
  }
  auto loaded = api::LoadMgaRelationStoreStateForInsertTarget(
      request.context,
      request.target_object.uuid.canonical);
  if (!loaded.ok) return columns;
  api::CrudState state = api::BuildCrudCompatibilityStateFromMga(
      std::move(loaded.state));
  const auto table = api::FindVisibleCrudTable(
      state,
      request.target_object.uuid.canonical,
      request.context.local_transaction_id);
  if (!table) return columns;
  columns.reserve(table->columns.size());
  for (const auto& [name, descriptor] : table->columns) {
    (void)descriptor;
    columns.push_back(name);
  }
  return columns;
}

void MaterializeCompactInsertRows(const SblrOperationEnvelope& envelope,
                                  api::EngineApiRequest* request) {
  const bool supported_operation =
      envelope.operation_id == "dml.insert_rows" ||
      envelope.operation_id == "dml.execute_native_bulk_ingest" ||
      envelope.operation_id == "dml.execute_import_rows";
  if (request == nullptr || !supported_operation || !request->rows.empty()) {
    return;
  }
  const auto compact_format = TextOperandValue(
      envelope,
      "insert_values_compact_format");
  const auto compact_payload = TextOperandValue(
      envelope,
      "insert_values_compact_payload");
  if (!compact_format || *compact_format != "sbsql.insert_values.cells.v1" ||
      !compact_payload || compact_payload->empty()) {
    return;
  }
  const std::uint64_t row_count =
      ParseCompactU64(TextOperandValue(envelope, "insert_values_row_count")
                          .value_or(std::string_view{}));
  const std::uint64_t column_count =
      ParseCompactU64(TextOperandValue(envelope, "insert_values_column_count")
                          .value_or(std::string_view{}));
  if (row_count == 0 || column_count == 0) return;
  auto cells =
      DecodeCompactInsertCells(*compact_payload, row_count, column_count);
  if (cells.empty()) return;
  const bool explicit_column_list = CompactBool(
      TextOperandValue(envelope, "insert_values_column_list_present")
          .value_or(std::string_view{}));
  std::vector<std::string> descriptor_columns;
  if (!explicit_column_list) {
    descriptor_columns = CompactDescriptorColumnNames(envelope, column_count);
    bool missing_descriptor_column = descriptor_columns.size() < column_count;
    for (const auto& column : descriptor_columns) {
      if (column.empty()) {
        missing_descriptor_column = true;
        break;
      }
    }
    if (missing_descriptor_column) {
      descriptor_columns =
          LoadDescriptorColumnNamesForCompactInsert(*request, column_count);
    }
  }
  request->rows.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
      row_count,
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))));
  for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
    api::EngineRowValue row;
    row.fields.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(column_count, 1024)));
    for (std::uint64_t column_index = 0; column_index < column_count;
         ++column_index) {
      const auto& cell =
          cells[static_cast<std::size_t>(row_index * column_count +
                                         column_index)];
      std::string name = cell.name;
      if (!explicit_column_list &&
          (name.empty() || LooksLikeOrdinalInsertFieldName(name)) &&
          column_index < descriptor_columns.size() &&
          !descriptor_columns[static_cast<std::size_t>(column_index)].empty()) {
        name = descriptor_columns[static_cast<std::size_t>(column_index)];
      }
      if (name.empty()) name = "c" + std::to_string(column_index);
      std::string type = cell.type.empty() ? "text" : cell.type;
      api::EngineTypedValue value;
      value.descriptor.descriptor_kind = "scalar";
      value.descriptor.canonical_type_name = type;
      value.descriptor.encoded_descriptor = "type=" + type;
      value.encoded_value = cell.value;
      value.is_null = cell.is_null || type == "null";
      if (value.is_null) {
        value.encoded_value.clear();
        value.setState(api::EngineValueState::sql_null);
      }
      row.fields.push_back({std::move(name), std::move(value)});
    }
    request->rows.push_back(std::move(row));
  }
  request->option_envelopes.push_back(
      "sblr.compact_insert_rowset_materialized=true");
  if (envelope.operation_id != "dml.insert_rows") {
    request->option_envelopes.push_back(
        "sblr.compact_native_rowset_materialized=true");
  }
  request->option_envelopes.push_back(
      "sblr.compact_insert_row_count:" + std::to_string(row_count));
}

api::EngineApiResult FailureResult(const api::EngineRequestContext& context,
                                   const std::string& operation_id,
                                   std::string code,
                                   std::string message_key,
                                   std::string detail) {
  api::EngineApiResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.embedded_trust_mode_observed = context.trust_mode == api::EngineTrustMode::embedded_in_process;
  result.cluster_authority_required = false;
  result.diagnostics.push_back(api::MakeEngineApiDiagnostic(std::move(code), std::move(message_key), std::move(detail), true));
  return result;
}

api::EngineApiRequest BaseApiRequest(const SblrDispatchRequest& request) {
  const auto phase_start = SblrSteadyClock::now();
  api::EngineApiRequest api_request = request.api_request;
  api_request.context = request.context;
  api_request.operation_id = request.envelope.operation_id;
  api_request.option_envelopes.reserve(api_request.option_envelopes.size() +
                                       request.envelope.operands.size());

  std::unordered_map<std::string, std::size_t> row_index_by_uuid;
  row_index_by_uuid.reserve(api_request.rows.size() +
                            request.envelope.operands.size() / 4);
  api_request.rows.reserve(api_request.rows.size() +
                           request.envelope.operands.size() / 4);
  for (std::size_t index = 0; index < api_request.rows.size(); ++index) {
    row_index_by_uuid.emplace(api_request.rows[index].requested_row_uuid.canonical,
                              index);
  }

  for (const auto& operand : request.envelope.operands) {
    const bool row_field = operand.type == "row_field" ||
                           operand.type.starts_with("row_field:");
    const bool row_null_field = operand.type == "row_null_field" ||
                                operand.type.starts_with("row_null_field:");
    if (operand.type == "text" && operand.name == "authorization_tag" &&
        !operand.value.empty() &&
        std::find(api_request.context.trace_tags.begin(),
                  api_request.context.trace_tags.end(),
                  operand.value) == api_request.context.trace_tags.end()) {
      api_request.context.trace_tags.push_back(operand.value);
      if (operand.value.rfind("right:", 0) == 0 ||
          operand.value.rfind("deny:", 0) == 0) {
        auto& authorization = api_request.context.authorization_context;
        authorization.present = true;
        if (authorization.principal_uuid.canonical.empty()) {
          authorization.principal_uuid = api_request.context.principal_uuid;
        }
        if (authorization.authority_uuid.canonical.empty()) {
          authorization.authority_uuid = api_request.context.database_uuid;
        }
        if (authorization.security_epoch == 0) {
          authorization.security_epoch = api_request.context.security_epoch;
        }
        if (authorization.policy_epoch == 0) {
          authorization.policy_epoch = 1;
        }
        if (authorization.catalog_generation_id == 0) {
          authorization.catalog_generation_id =
              api_request.context.catalog_generation_id == 0
                  ? 1
                  : api_request.context.catalog_generation_id;
        }
        if (authorization.effective_subjects.empty()) {
          authorization.effective_subjects.push_back(
              {api_request.context.principal_uuid, "principal"});
        }
        const bool deny = operand.value.rfind("deny:", 0) == 0;
        const std::string right = operand.value.substr(deny ? 5 : 6);
        const auto duplicate = std::find_if(
            authorization.grants.begin(),
            authorization.grants.end(),
            [&](const auto& grant) {
              return grant.right == right && grant.deny == deny &&
                     grant.target_uuid.canonical.empty();
            });
        if (!right.empty() && duplicate == authorization.grants.end()) {
          scratchbird::engine::internal_api::EngineMaterializedAuthorizationGrant grant;
          grant.grant_uuid.canonical =
              "public-abi-admitted-auth-tag:" + std::string(deny ? "deny:" : "right:") + right;
          grant.subject_uuid = api_request.context.principal_uuid;
          grant.subject_kind = "principal";
          grant.right = right;
          grant.deny = deny;
          grant.security_epoch = authorization.security_epoch;
          authorization.grants.push_back(std::move(grant));
        }
      }
    }
    if (!operand.name.empty() && !row_field && !row_null_field) {
      api_request.option_envelopes.push_back(operand.name + ":" + operand.value);
    }
    if (row_field || row_null_field) {
      const auto separator = operand.name.find('|');
      if (separator == std::string::npos || separator + 1 >= operand.name.size()) {
        continue;
      }
      const std::string row_uuid = operand.name.substr(0, separator);
      const std::string field_name = operand.name.substr(separator + 1);
      auto row_index = row_index_by_uuid.find(row_uuid);
      if (row_index == row_index_by_uuid.end()) {
        const std::size_t appended_index = api_request.rows.size();
        api::EngineRowValue appended;
        appended.requested_row_uuid.canonical = row_uuid;
        api_request.rows.push_back(std::move(appended));
        row_index =
            row_index_by_uuid.emplace(row_uuid, appended_index).first;
      }
      api::EngineRowValue& row = api_request.rows[row_index->second];
      api::EngineTypedValue value;
      value.descriptor.descriptor_kind = "scalar";
      const auto type_separator = operand.type.find(':');
      value.descriptor.canonical_type_name =
          type_separator == std::string::npos ? "text" : operand.type.substr(type_separator + 1);
      value.descriptor.encoded_descriptor = "type=text";
      if (value.descriptor.canonical_type_name != "text") {
        value.descriptor.encoded_descriptor = "type=" + value.descriptor.canonical_type_name;
      }
      value.encoded_value = operand.value;
      value.is_null = row_null_field || value.descriptor.canonical_type_name == "null";
      if (value.is_null) {
        value.encoded_value.clear();
        value.setState(api::EngineValueState::sql_null);
      }
      row.fields.push_back({field_name, std::move(value)});
    } else if (operand.type == "assignment" && !operand.name.empty()) {
      api::EngineTypedValue value;
      value.descriptor.descriptor_kind = "scalar";
      value.descriptor.canonical_type_name = "text";
      value.descriptor.encoded_descriptor = "type=text";
      value.encoded_value = operand.value;
      api_request.assignments.push_back({operand.name, std::move(value)});
    } else if (operand.type == "predicate" && !operand.name.empty()) {
      api_request.predicate.predicate_kind = operand.name;
      api_request.predicate.canonical_predicate_envelope = operand.value;
    }
  }
  const auto operand_loop_finish = SblrSteadyClock::now();
  const std::string current_role_uuid =
      api::SecurityOptionValue(api_request, "current_role_uuid:");
  if (!current_role_uuid.empty()) {
    api_request.context.current_role_uuid.canonical = current_role_uuid;
  }
  if (api_request.target_object.uuid.canonical.empty()) {
    api_request.target_object.uuid.canonical =
        api::SecurityOptionValue(api_request, "target_object_uuid:");
  }
  if (api_request.target_object.object_kind.empty()) {
    api_request.target_object.object_kind =
        api::SecurityOptionValue(api_request, "target_object_kind:");
  }
  if (api_request.operation_id.rfind("lifecycle.", 0) == 0) {
    const std::string lifecycle_database_path =
        api::SecurityOptionValue(api_request, "database_path:");
    if (!lifecycle_database_path.empty()) {
      api_request.context.database_path = lifecycle_database_path;
    }
  }
  if (api_request.predicate.predicate_kind.empty()) {
    const std::string predicate_kind =
        api::SecurityOptionValue(api_request, "predicate_kind:");
    const std::string predicate_column =
        api::SecurityOptionValue(api_request, "predicate_column:");
    const std::string predicate_value =
        api::SecurityOptionValue(api_request, "predicate_value:");
    if (!predicate_kind.empty() && !predicate_column.empty()) {
      api_request.predicate.predicate_kind = predicate_kind;
      api_request.predicate.canonical_predicate_envelope = predicate_column;
      if (!predicate_value.empty()) {
        const auto values = SplitCommaSeparatedLiterals(predicate_value);
        const auto types = SplitCommaSeparatedLiterals(
            api::SecurityOptionValue(api_request, "predicate_value_type:"));
        for (std::size_t index = 0; index < values.size(); ++index) {
          api_request.predicate.bound_values.push_back(TypedValueFromLoweredLiteral(
              values[index],
              index < types.size() ? types[index] : std::string{}));
        }
      }
    }
  }
  if (api_request.assignments.empty()) {
    const std::string assignment_column =
        api::SecurityOptionValue(api_request, "assignment_column:");
    const std::string assignment_value =
        api::SecurityOptionValue(api_request, "assignment_value:");
    if (!assignment_column.empty()) {
      api_request.assignments.push_back({assignment_column,
                                         TypedValueFromLoweredLiteral(
                                             assignment_value,
                                             api::SecurityOptionValue(
                                             api_request,
                                                 "assignment_value_type:"))});
    }
  }
  const auto compact_start = SblrSteadyClock::now();
  MaterializeCompactInsertRows(request.envelope, &api_request);
  const auto compact_finish = SblrSteadyClock::now();
  WriteBaseApiPhaseTrace(
      request.envelope,
      SblrElapsedMicros(phase_start, operand_loop_finish),
      SblrElapsedMicros(compact_start, compact_finish),
      SblrElapsedMicros(phase_start, compact_finish),
      api_request.rows.size(),
      request.envelope.operands.size());
  return api_request;
}

const char* ExpectedOpcodeForOperation(std::string_view operation_id) {
  if (operation_id.rfind("op.", 0) == 0) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("index.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("bridge.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("memory.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("storage_tier.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("routine.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("filespace.discovery.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("filespace.package.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("shard_placement.")) {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id.starts_with("security.encryption_key.") ||
      operation_id.starts_with("security.protected_material") ||
      operation_id == "security.encrypted_filespace.open" ||
      operation_id == "security.request_protected_material") {
    if (const auto* entry = LookupSblrOperation(operation_id)) {
      return entry->opcode.c_str();
    }
  }
  if (operation_id == "ddl.create_database") return "SBLR_DDL_CREATE_DATABASE";
  if (operation_id == "ddl.create_schema") return "SBLR_DDL_CREATE_SCHEMA";
  if (operation_id == "ddl.create_table") return "SBLR_DDL_CREATE_TABLE";
  if (operation_id == "ddl.create_index") return "SBLR_DDL_CREATE_INDEX";
  if (operation_id == "ddl.create_index_template") return "SBLR_DDL_CREATE_INDEX_TEMPLATE";
  if (operation_id == "ddl.create_statistics") return "SBLR_DDL_CREATE_STATISTICS";
  if (operation_id == "ddl.create_domain") return "SBLR_DDL_CREATE_DOMAIN";
  if (operation_id == "ddl.create_sequence") return "SBLR_DDL_CREATE_SEQUENCE";
  if (operation_id == "ddl.create_view") return "SBLR_DDL_CREATE_VIEW";
  if (operation_id == "ddl.create_synonym") return "SBLR_DDL_CREATE_SYNONYM";
  if (operation_id == "ddl.constraint.create") return "SBLR_DDL_CONSTRAINT_CREATE";
  if (operation_id == "ddl.constraint.alter") return "SBLR_DDL_CONSTRAINT_ALTER";
  if (operation_id == "ddl.constraint.drop") return "SBLR_DDL_CONSTRAINT_DROP";
  if (operation_id == "ddl.create_function") return "SBLR_DDL_CREATE_FUNCTION";
  if (operation_id == "ddl.create_procedure") return "SBLR_DDL_CREATE_PROCEDURE";
  if (operation_id == "ddl.create_trigger") return "SBLR_DDL_CREATE_TRIGGER";
  if (operation_id == "ddl.alter_object") return "SBLR_DDL_ALTER_OBJECT";
  if (operation_id == "ddl.drop_object") return "SBLR_DDL_DROP_OBJECT";
  if (operation_id == "ddl.comment_on_object") return "SBLR_DDL_COMMENT_ON_OBJECT";
  if (operation_id == "dml.insert_rows") return "SBLR_DML_INSERT_ROWS";
  if (operation_id == "dml.select_rows") return "SBLR_DML_SELECT_ROWS";
  if (operation_id == "dml.update_rows") return "SBLR_DML_UPDATE_ROWS";
  if (operation_id == "dml.delete_rows") return "SBLR_DML_DELETE_ROWS";
  if (operation_id == "dml.merge_rows") return "SBLR_DML_MERGE_ROWS";
  if (operation_id == "dml.execute_import_rows") return "SBLR_DML_EXECUTE_IMPORT_ROWS";
  if (operation_id == "dml.execute_native_bulk_ingest") return "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST";
  if (operation_id == "dml.normalize_import_checkpoint_model") return "SBLR_DML_IMPORT_CHECKPOINT_MODEL";
  if (operation_id == "dml.normalize_import_reject_model") return "SBLR_DML_IMPORT_REJECT_MODEL";
  if (operation_id == "dml.plan_import_rows") return "SBLR_DML_PLAN_IMPORT_ROWS";
  if (operation_id == "query.bind_expression") return "SBLR_QUERY_BIND_EXPRESSION";
  if (operation_id == "query.bind_predicate") return "SBLR_QUERY_BIND_PREDICATE";
  if (operation_id == "query.bind_projection") return "SBLR_QUERY_BIND_PROJECTION";
  if (operation_id == "query.evaluate_projection") return "SBLR_QUERY_EVALUATE_PROJECTION";
  if (operation_id == "expression.system_variable_read") return "SBLR_SYSTEM_VARIABLE_READ";
  if (operation_id == "query.plan_operation") return "SBLR_QUERY_PLAN_OPERATION";
  if (operation_id == "query.cast_value") return "SBLR_QUERY_CAST_VALUE";
  if (operation_id == "query.extract_value") return "SBLR_QUERY_EXTRACT_VALUE";
  if (operation_id == "query.set_operation") return "SBLR_QUERY_SET_OPERATION";
  if (operation_id == "query.apply_numeric_operation") return "SBLR_QUERY_APPLY_NUMERIC_OPERATION";
  if (operation_id == "query.canonicalize_document_value") return "SBLR_QUERY_CANONICALIZE_DOCUMENT_VALUE";
  if (operation_id == "query.evaluate_advanced_datatype_family") return "SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY";
  if (operation_id == "query.validate_domain_value") return "SBLR_QUERY_VALIDATE_DOMAIN_VALUE";
  if (operation_id == "query.invoke_domain_method") return "SBLR_QUERY_INVOKE_DOMAIN_METHOD";
  if (operation_id == "transaction.begin") return "SBLR_TRANSACTION_BEGIN";
  if (operation_id == "transaction.set_characteristics") return "SBLR_TRANSACTION_SET_CHARACTERISTICS";
  if (operation_id == "transaction.commit") return "SBLR_TRANSACTION_COMMIT";
  if (operation_id == "transaction.rollback") return "SBLR_TRANSACTION_ROLLBACK";
  if (operation_id == "transaction.prepare") return "SBLR_TRANSACTION_PREPARE";
  if (operation_id == "transaction.create_savepoint") return "SBLR_TRANSACTION_CREATE_SAVEPOINT";
  if (operation_id == "transaction.release_savepoint") return "SBLR_TRANSACTION_RELEASE_SAVEPOINT";
  if (operation_id == "transaction.rollback_to_savepoint") return "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT";
  if (operation_id == "transaction.execute_block") return "SBLR_TRANSACTION_EXECUTE_BLOCK";
  if (operation_id == "transaction.lock_table") return "SBLR_TXN_LOCK_TABLE";
  if (operation_id == "transaction.unlock_table") return "SBLR_TXN_UNLOCK_TABLE";
  if (operation_id == "transaction.lock_named") return "SBLR_TXN_LOCK_NAMED";
  if (operation_id == "transaction.unlock_named") return "SBLR_TXN_UNLOCK_NAMED";
  if (operation_id == "catalog.resolve_name") return "SBLR_CATALOG_RESOLVE_NAME";
  if (operation_id == "catalog.map_uuid_to_name") return "SBLR_CATALOG_MAP_UUID_TO_NAME";
  if (operation_id == "catalog.lookup_object") return "SBLR_CATALOG_LOOKUP_OBJECT";
  if (operation_id == "catalog.list_children") return "SBLR_CATALOG_LIST_CHILDREN";
  if (operation_id == "catalog.get_descriptor") return "SBLR_CATALOG_GET_DESCRIPTOR";
  if (operation_id == "catalog.get_dependencies") return "SBLR_CATALOG_GET_DEPENDENCIES";
  if (operation_id.starts_with("catalog.mutation.")) return nullptr;
  if (operation_id == "artifact.export_catalog") return "SBLR_ARTIFACT_EXPORT_CATALOG";
  if (operation_id == "artifact.import_catalog") return "SBLR_ARTIFACT_IMPORT_CATALOG";
  if (operation_id == "artifact.external_git.export_snapshot") return "SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT";
  if (operation_id == "artifact.external_git.diff_snapshot") return "SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT";
  if (operation_id == "artifact.external_git.rollback_plan") return "SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN";
  if (operation_id == "security.create_identity") return "SBLR_SECURITY_CREATE_IDENTITY";
  if (operation_id == "security.alter_identity") return "SBLR_SECURITY_ALTER_IDENTITY";
  if (operation_id == "security.grant_right") return "SBLR_SECURITY_GRANT_RIGHT";
  if (operation_id == "security.revoke_right") return "SBLR_SECURITY_REVOKE_RIGHT";
  if (operation_id == "security.role.create") return "SBLR_SEC_CREATE_ROLE";
  if (operation_id == "security.group.create") return "SBLR_SEC_CREATE_GROUP";
  if (operation_id == "security.principal.create") return "SBLR_SECURITY_PRINCIPAL_CREATE";
  if (operation_id == "security.principal.alter") return "SBLR_SECURITY_PRINCIPAL_ALTER";
  if (operation_id == "security.membership.grant") return "SBLR_SECURITY_MEMBERSHIP_GRANT";
  if (operation_id == "security.membership.revoke") return "SBLR_SECURITY_MEMBERSHIP_REVOKE";
  if (operation_id == "security.privilege.grant") return "SBLR_SECURITY_PRIVILEGE_GRANT";
  if (operation_id == "security.privilege.revoke") return "SBLR_SECURITY_PRIVILEGE_REVOKE";
  if (operation_id == "security.session.set_role") return "SBLR_SECURITY_SESSION_SET_ROLE";
  if (operation_id == "security.policy.create") return "SBLR_SECURITY_POLICY_CREATE";
  if (operation_id == "security.policy.alter") return "SBLR_SECURITY_POLICY_ALTER";
  if (operation_id == "security.policy.attach") return "SBLR_SECURITY_POLICY_ATTACH";
  if (operation_id == "security.policy.activate") return "SBLR_SECURITY_POLICY_ACTIVATE";
  if (operation_id == "security.policy.deactivate") return "SBLR_SECURITY_POLICY_DEACTIVATE";
  if (operation_id == "security.policy.validate") return "SBLR_SECURITY_POLICY_VALIDATE";
  if (operation_id == "security.policy.show") return "SBLR_SECURITY_POLICY_SHOW";
  if (operation_id == "security.evaluate_visibility") return "SBLR_SECURITY_EVALUATE_VISIBILITY";
  if (operation_id == "security.evaluate_policy") return "SBLR_SECURITY_EVALUATE_POLICY";
  if (operation_id == "security.evaluate_deep_enforcement") return "SBLR_SECURITY_EVALUATE_DEEP_ENFORCEMENT";
  if (operation_id == "observability.show_version") return "SBLR_OBSERVABILITY_SHOW_VERSION";
  if (operation_id == "observability.show_database") return "SBLR_OBSERVABILITY_SHOW_DATABASE";
  if (operation_id == "observability.show_system") return "SBLR_OBSERVABILITY_SHOW_SYSTEM";
  if (operation_id == "observability.show_catalog") return "SBLR_OBSERVABILITY_SHOW_CATALOG";
  if (operation_id == "observability.show_sessions") return "SBLR_OBSERVABILITY_SHOW_SESSIONS";
  if (operation_id == "observability.show_transactions") return "SBLR_OBSERVABILITY_SHOW_TRANSACTIONS";
  if (operation_id == "observability.show_locks") return "SBLR_OBSERVABILITY_SHOW_LOCKS";
  if (operation_id == "observability.show_statements") return "SBLR_OBSERVABILITY_SHOW_STATEMENTS";
  if (operation_id == "observability.show_jobs") return "SBLR_OBSERVABILITY_SHOW_JOBS";
  if (operation_id == "observability.show_management") return "SBLR_OBSERVABILITY_SHOW_MANAGEMENT";
  if (operation_id == "observability.show_diagnostics") return "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS";
  if (operation_id == "observability.show_diagnostics_extended") return "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED";
  if (operation_id == "observability.show_archive_replication") return "SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION";
  if (operation_id == "observability.show_agents_extended") return "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED";
  if (operation_id == "observability.show_filespace_extended") return "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED";
  if (operation_id == "observability.show_decision_service") return "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE";
  if (operation_id == "observability.show_acceleration") return "SBLR_OBSERVABILITY_SHOW_ACCELERATION";
  if (operation_id == "observability.show_acceleration_extended") return "SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED";
  if (operation_id == "observability.show_metrics") return "SBLR_OBSERVABILITY_SHOW_METRICS";
  if (operation_id == "observability.explain_operation") return "SBLR_OBSERVABILITY_EXPLAIN_OPERATION";
  if (operation_id == "general.signal_diagnostic") return "SBLR_GENERAL_SIGNAL_DIAGNOSTIC";
  if (operation_id == "general.raise_diagnostic") return "SBLR_GENERAL_RAISE_DIAGNOSTIC";
  if (operation_id == "general.resignal_diagnostic") return "SBLR_GENERAL_RESIGNAL_DIAGNOSTIC";
  if (operation_id == "general.procedural_operation") return "SBLR_GENERAL_PROCEDURAL_OPERATION";
  if (operation_id == "management.inspect_config") return "SBLR_MANAGEMENT_INSPECT_CONFIG";
  if (operation_id == "management.set_config") return "SBLR_MANAGEMENT_SET_CONFIG";
  if (operation_id == "management.reset_config") return "SBLR_MANAGEMENT_RESET_CONFIG";
  if (operation_id == "management.inspect_runtime") return "SBLR_MANAGEMENT_INSPECT_RUNTIME";
  if (operation_id == "management.control_runtime") return "SBLR_MANAGEMENT_CONTROL_RUNTIME";
  if (operation_id == "management.prepare_support_bundle") return "SBLR_MANAGEMENT_PREPARE_SUPPORT_BUNDLE";
  if (operation_id == "lifecycle.create_database") return "SBLR_LIFECYCLE_CREATE_DATABASE";
  if (operation_id == "lifecycle.open_database") return "SBLR_LIFECYCLE_OPEN_DATABASE";
  if (operation_id == "lifecycle.attach_database") return "SBLR_LIFECYCLE_ATTACH_DATABASE";
  if (operation_id == "lifecycle.detach_database") return "SBLR_LIFECYCLE_DETACH_DATABASE";
  if (operation_id == "lifecycle.enter_maintenance") return "SBLR_LIFECYCLE_ENTER_MAINTENANCE";
  if (operation_id == "lifecycle.exit_maintenance") return "SBLR_LIFECYCLE_EXIT_MAINTENANCE";
  if (operation_id == "lifecycle.enter_restricted_open") return "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN";
  if (operation_id == "lifecycle.exit_restricted_open") return "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN";
  if (operation_id == "lifecycle.inspect_database") return "SBLR_LIFECYCLE_INSPECT_DATABASE";
  if (operation_id == "lifecycle.verify_database") return "SBLR_LIFECYCLE_VERIFY_DATABASE";
  if (operation_id == "lifecycle.repair_database") return "SBLR_LIFECYCLE_REPAIR_DATABASE";
  if (operation_id == "lifecycle.shutdown_database") return "SBLR_LIFECYCLE_SHUTDOWN_DATABASE";
  if (operation_id == "lifecycle.shutdown_force") return "SBLR_LIFECYCLE_SHUTDOWN_FORCE";
  if (operation_id == "lifecycle.shutdown_acknowledge") return "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE";
  if (operation_id == "lifecycle.drop_database") return "SBLR_LIFECYCLE_DROP_DATABASE";
  if (operation_id == "agents.list") return "SBLR_AGENTS_LIST";
  if (operation_id == "agents.show") return "SBLR_AGENTS_SHOW";
  if (operation_id == "agents.start") return "SBLR_AGENTS_START";
  if (operation_id == "agents.stop") return "SBLR_AGENTS_STOP";
  if (operation_id == "agents.pause") return "SBLR_AGENTS_PAUSE";
  if (operation_id == "agents.resume") return "SBLR_AGENTS_RESUME";
  if (operation_id == "agents.configure") return "SBLR_AGENTS_CONFIGURE";
  if (operation_id == "agents.run") return "SBLR_AGENTS_RUN";
  if (operation_id == "agents.dry_run") return "SBLR_AGENTS_DRY_RUN";
  if (operation_id == "agents.override") return "SBLR_AGENTS_OVERRIDE";
  if (operation_id == "sys.agents") return "SBLR_SYS_AGENTS";
  if (operation_id == "cluster.sys.agents") return "SBLR_CLUSTER_SYS_AGENTS";
  if (operation_id == "agents.request_page_preallocation") return "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION";
  if (operation_id == "agents.request_page_relocation") return "SBLR_AGENT_REQUEST_PAGE_RELOCATION";
  if (operation_id == "agents.request_filespace_growth") return "SBLR_AGENT_REQUEST_FILESPACE_GROWTH";
  if (operation_id == "agents.notify_filespace_shrink_readiness") return "SBLR_AGENT_NOTIFY_FILESPACE_SHRINK_READINESS";
  if (operation_id == "event.channel.create") return "SBLR_EVENT_CHANNEL_CREATE";
  if (operation_id == "event.channel.alter") return "SBLR_EVENT_CHANNEL_ALTER";
  if (operation_id == "event.channel.drop") return "SBLR_EVENT_CHANNEL_DROP";
  if (operation_id == "event.channel.listen" || operation_id == "notification.channel.listen") return "SBLR_EVENT_CHANNEL_LISTEN";
  if (operation_id == "event.channel.unlisten" || operation_id == "notification.channel.unlisten") return "SBLR_EVENT_CHANNEL_UNLISTEN";
  if (operation_id == "event.channel.notify" || operation_id == "notification.channel.notify") return "SBLR_EVENT_CHANNEL_NOTIFY";
  if (operation_id == "event.subscription.list") return "SBLR_EVENT_SUBSCRIPTION_LIST";
  if (operation_id == "event.delivery.poll") return "SBLR_EVENT_DELIVERY_POLL";
  if (operation_id == "event.delivery.ack") return "SBLR_EVENT_DELIVERY_ACK";
  if (operation_id == "session.notification.unlisten") return "SBLR_EVENT_CHANNEL_UNLISTEN";
  if (operation_id == "session.notification.unlisten_all") return "SBLR_EVENT_CHANNEL_UNLISTEN_ALL";
  if (operation_id == "agents.request_index_delta_merge") return "SBLR_AGENT_REQUEST_INDEX_DELTA_MERGE";
  if (operation_id == "agents.request_index_rebuild_or_shadow_build") return "SBLR_AGENT_REQUEST_INDEX_REBUILD_OR_SHADOW_BUILD";
  if (operation_id == "agents.metrics.get") return "SBLR_AGENT_METRICS_GET";
  if (operation_id == "agents.policy.get") return "SBLR_AGENT_POLICY_GET";
  if (operation_id == "agents.evidence.list") return "SBLR_AGENT_EVIDENCE_LIST";
  if (operation_id == "agents.audit.list") return "SBLR_AGENT_AUDIT_LIST";
  if (operation_id == "agents.actions.list") return "SBLR_AGENT_ACTION_LIST";
  if (operation_id == "agents.overrides.list") return "SBLR_AGENT_OVERRIDE_LIST";
  if (operation_id == "agents.drain") return "SBLR_AGENT_LIFECYCLE_DRAIN";
  if (operation_id == "agents.restart") return "SBLR_AGENT_LIFECYCLE_RESTART";
  if (operation_id == "agents.enable") return "SBLR_AGENT_LIFECYCLE_ENABLE";
  if (operation_id == "agents.disable") return "SBLR_AGENT_LIFECYCLE_DISABLE";
  if (operation_id == "agents.quarantine") return "SBLR_AGENT_QUARANTINE";
  if (operation_id == "agents.unquarantine") return "SBLR_AGENT_UNQUARANTINE";
  if (operation_id == "agents.policy.attach") return "SBLR_AGENT_POLICY_ATTACH";
  if (operation_id == "agents.policy.detach") return "SBLR_AGENT_POLICY_DETACH";
  if (operation_id == "agents.policy.validate") return "SBLR_AGENT_POLICY_VALIDATE";
  if (operation_id == "agents.policy.simulate") return "SBLR_AGENT_POLICY_SIMULATE";
  if (operation_id == "agents.policy.apply") return "SBLR_AGENT_POLICY_APPLY";
  if (operation_id == "agents.policy.rollback") return "SBLR_AGENT_POLICY_ROLLBACK";
  if (operation_id == "agents.action.approve") return "SBLR_AGENT_ACTION_APPROVE";
  if (operation_id == "agents.action.cancel") return "SBLR_AGENT_ACTION_CANCEL";
  if (operation_id == "agents.action.retry") return "SBLR_AGENT_ACTION_RETRY";
  if (operation_id == "agents.action.suppress") return "SBLR_AGENT_ACTION_SUPPRESS";
  if (operation_id == "agents.override.create") return "SBLR_AGENT_OVERRIDE_CREATE";
  if (operation_id == "agents.override.update") return "SBLR_AGENT_OVERRIDE_UPDATE";
  if (operation_id == "agents.override.drop") return "SBLR_AGENT_OVERRIDE_DROP";
  if (operation_id == "agents.set_mode") return "SBLR_AGENT_SET_MODE";
  if (operation_id == "filespaces.show") return "SBLR_SHOW_FILESPACES";
  if (operation_id == "filespaces.health.show") return "SBLR_SHOW_FILESPACE_HEALTH";
  if (operation_id == "filespaces.capacity.show") return "SBLR_SHOW_FILESPACE_CAPACITY";
  if (operation_id == "pages.allocation.show") return "SBLR_SHOW_PAGE_ALLOCATION";
  if (operation_id == "pages.allocation.family.show") return "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY";
  if (operation_id == "pages.relocation_backlog.show") return "SBLR_SHOW_PAGE_RELOCATION_BACKLOG";
  if (operation_id == "filespaces.shrink_readiness.show") return "SBLR_SHOW_FILESPACE_SHRINK_READINESS";
  if (operation_id == "cluster.agent.list") return "SBLR_CLUSTER_AGENT_LIST";
  if (operation_id == "cluster.agent.get") return "SBLR_CLUSTER_AGENT_GET";
  if (operation_id == "cluster.agent.control") return "SBLR_CLUSTER_AGENT_CONTROL";
  if (operation_id == "cluster.inspect_state") return "SBLR_CLUSTER_INSPECT_STATE";
  if (operation_id == "cluster.inspect_routing_plan") return "SBLR_CLUSTER_INSPECT_ROUTING_PLAN";
  if (operation_id == "cluster.control_cluster") return "SBLR_CLUSTER_CONTROL_CLUSTER";
  if (operation_id == "cluster.inspect_provider") return "SBLR_CLUSTER_INSPECT_PROVIDER";
  if (operation_id == "cluster.place_object") return "SBLR_CLUSTER_PLACE_OBJECT";
  if (operation_id == "cluster.inspect_replication") return "SBLR_CLUSTER_INSPECT_REPLICATION";
  if (operation_id == "cluster.prepare_remote_participant_insert") return "SBLR_CLUSTER_PREPARE_REMOTE_PARTICIPANT_INSERT";
  if (operation_id == "cluster.validate_insert_route_fence") return "SBLR_CLUSTER_VALIDATE_INSERT_ROUTE_FENCE";
  if (operation_id == "cluster.profile_operation") return "SBLR_CLUSTER_PROFILE_OPERATION";
  if (operation_id == "extensibility.register_udr_package") return "SBLR_EXTENSIBILITY_REGISTER_UDR_PACKAGE";
  if (operation_id == "extensibility.load_udr_package") return "SBLR_EXTENSIBILITY_LOAD_UDR_PACKAGE";
  if (operation_id == "extensibility.unload_udr_package") return "SBLR_EXTENSIBILITY_UNLOAD_UDR_PACKAGE";
  if (operation_id == "extensibility.inspect_udr_packages") return "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES";
  if (operation_id == "extensibility.invoke_udr_package") return "SBLR_UDR_INVOKE";
  if (operation_id == "extensibility.register_parser_package") return "SBLR_EXTENSIBILITY_REGISTER_PARSER_PACKAGE";
  if (operation_id == "extensibility.compile_llvm_module") return "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE";
  if (operation_id == "extensibility.inspect_gpu_capability") return "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY";
  if (operation_id == "nosql.document_insert") return "SBLR_NOSQL_DOCUMENT_INSERT";
  if (operation_id == "nosql.document_find") return "SBLR_NOSQL_DOCUMENT_FIND";
  if (operation_id == "nosql.document_update") return "SBLR_NOSQL_DOCUMENT_UPDATE";
  if (operation_id == "nosql.document_delete") return "SBLR_NOSQL_DOCUMENT_DELETE";
  if (operation_id == "nosql.graph_query") return "SBLR_NOSQL_GRAPH_QUERY";
  if (operation_id == "nosql.key_value_get") return "SBLR_NOSQL_KEY_VALUE_GET";
  if (operation_id == "nosql.key_value_put") return "SBLR_NOSQL_KEY_VALUE_PUT";
  if (operation_id == "nosql.key_value_multiget") return "SBLR_NOSQL_KEY_VALUE_MULTIGET";
  if (operation_id == "nosql.key_value_pipeline") return "SBLR_NOSQL_KEY_VALUE_PIPELINE";
  if (operation_id == "nosql.key_value_atomic_program") return "SBLR_NOSQL_KEY_VALUE_ATOMIC_PROGRAM";
  if (operation_id == "nosql.backpressure_debt_plan") return "SBLR_NOSQL_BACKPRESSURE_DEBT_PLAN";
  if (operation_id == "nosql.family_maintenance_plan") return "SBLR_NOSQL_FAMILY_MAINTENANCE_PLAN";
  if (operation_id == "nosql.statistics_advisor_plan") return "SBLR_NOSQL_STATISTICS_ADVISOR_PLAN";
  if (operation_id == "nosql.time_series_append") return "SBLR_NOSQL_TIME_SERIES_APPEND";
  if (operation_id == "nosql.vector_search") return "SBLR_NOSQL_VECTOR_SEARCH";
  if (operation_id == "nosql.vector_collection_op") return "SBLR_NOSQL_VECTOR_COLLECTION_OP";
  if (operation_id == "nosql.search_query") return "SBLR_NOSQL_SEARCH_QUERY";
  if (operation_id == "filespace.create") return "SBLR_FILESPACE_CREATE";
  if (operation_id == "filespace.preallocate") return "SBLR_FILESPACE_PREALLOCATE";
  if (operation_id == "filespace.attach") return "SBLR_FILESPACE_ATTACH";
  if (operation_id == "filespace.detach") return "SBLR_FILESPACE_DETACH";
  if (operation_id == "filespace.disconnect") return "SBLR_FILESPACE_DISCONNECT";
  if (operation_id == "filespace.move") return "SBLR_FILESPACE_MOVE";
  if (operation_id == "filespace.merge") return "SBLR_FILESPACE_MERGE";
  if (operation_id == "filespace.promote") return "SBLR_FILESPACE_PROMOTE";
  if (operation_id == "filespace.verify") return "SBLR_FILESPACE_VERIFY";
  if (operation_id == "filespace.compact") return "SBLR_FILESPACE_COMPACT";
  if (operation_id == "filespace.fence") return "SBLR_FILESPACE_FENCE";
  if (operation_id == "filespace.release") return "SBLR_FILESPACE_RELEASE";
  if (operation_id == "filespace.archive") return "SBLR_FILESPACE_ARCHIVE";
  if (operation_id == "filespace.quarantine") return "SBLR_FILESPACE_QUARANTINE";
  if (operation_id == "filespace.snapshot.create") return "SBLR_FILESPACE_SNAPSHOT_CREATE";
  if (operation_id == "filespace.snapshot.refresh") return "SBLR_FILESPACE_SNAPSHOT_REFRESH";
  if (operation_id == "filespace.snapshot.validate") return "SBLR_FILESPACE_SNAPSHOT_VALIDATE";
  if (operation_id == "filespace.snapshot.retire") return "SBLR_FILESPACE_SNAPSHOT_RETIRE";
  if (operation_id == "filespace.shadow.create") return "SBLR_FILESPACE_SHADOW_CREATE";
  if (operation_id == "filespace.shadow.refresh") return "SBLR_FILESPACE_SHADOW_REFRESH";
  if (operation_id == "filespace.shadow.validate") return "SBLR_FILESPACE_SHADOW_VALIDATE";
  if (operation_id == "filespace.shadow.promote") return "SBLR_FILESPACE_SHADOW_PROMOTE";
  if (operation_id == "filespace.truncate") return "SBLR_FILESPACE_TRUNCATE";
  if (operation_id == "filespace.drop") return "SBLR_FILESPACE_DROP";
  if (operation_id == "filespace.delete_physical") return "SBLR_FILESPACE_DELETE_PHYSICAL";
  if (operation_id == "filespace.repair") return "SBLR_FILESPACE_REPAIR";
  if (operation_id == "filespace.rebuild") return "SBLR_FILESPACE_REBUILD";
  if (operation_id == "filespace.salvage") return "SBLR_FILESPACE_SALVAGE";
  if (operation_id == "storage.manage_operation") return "SBLR_STORAGE_MANAGEMENT_OPERATION";
  return nullptr;
}

bool IsGpuAccelerationControlOperation(std::string_view operation_id) {
  return operation_id == "op.gpu.artifact_quarantine" ||
         operation_id == "op.gpu.cache_clear" ||
         operation_id == "op.gpu.device_quarantine" ||
         operation_id == "op.gpu.kernel_quarantine" ||
         operation_id == "op.gpu.profile_disable" ||
         operation_id == "op.gpu.profile_enable";
}

bool IsGpuAccelerationInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.gpu" ||
         operation_id == "op.show.gpu_artifacts" ||
         operation_id == "op.show.gpu_capability" ||
         operation_id == "op.show.gpu_devices" ||
         operation_id == "op.show.gpu_kernels" ||
         operation_id == "op.show.gpu_memory";
}

bool IsNativeCompileControlOperation(std::string_view operation_id) {
  return operation_id == "op.native_compile.aot_rebuild" ||
         operation_id == "op.native_compile.artifact_quarantine" ||
         operation_id == "op.native_compile.cache_invalidate" ||
         operation_id == "op.native_compile.profile_disable" ||
         operation_id == "op.native_compile.profile_enable";
}

bool IsNativeCompileInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.aot_artifacts" ||
         operation_id == "op.show.llvm" ||
         operation_id == "op.show.llvm_provenance" ||
         operation_id == "op.show.llvm_targets" ||
         operation_id == "op.show.native_compile" ||
         operation_id == "op.show.native_compile_cache";
}

bool IsManagementRuntimeControlOperation(std::string_view operation_id) {
  return operation_id == "op.management.listener.drain" ||
         operation_id == "op.management.listener.undrain" ||
         operation_id == "op.management.manager.restart" ||
         operation_id == "op.management.manager.start" ||
         operation_id == "op.management.manager.stop" ||
         operation_id == "op.management.parser_pool.resize" ||
         operation_id == "op.management.config.reload" ||
         operation_id == "op.management.instruction.ack" ||
         operation_id == "op.management.instruction.apply" ||
         operation_id == "op.management.instruction.cancel" ||
         operation_id == "op.management.instruction.quarantine" ||
         operation_id == "op.management.support_bundle.create";
}

bool IsManagementRuntimeInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.management.config" ||
         operation_id == "op.show.management.drift" ||
         operation_id == "op.show.management.instructions" ||
         operation_id == "op.show.management.listeners" ||
         operation_id == "op.show.management.manager" ||
         operation_id == "op.show.management.parser_pool" ||
         operation_id == "op.show.management.readiness" ||
         operation_id == "op.show.management.servers" ||
         operation_id == "op.show.management.support_bundle_safety" ||
         operation_id == "op.show.management.support_bundles";
}

bool IsMemoryManagementOperation(std::string_view operation_id) {
  return operation_id.starts_with("memory.");
}

bool IsMemoryManagementControlOperation(std::string_view operation_id) {
  return operation_id == "memory.profile.set" ||
         operation_id == "memory.cache.flush" ||
         operation_id == "memory.cache.invalidate" ||
         operation_id == "memory.scavenge" ||
         operation_id == "memory.grant_feedback.reset" ||
         operation_id == "memory.stream_policy.set" ||
         operation_id == "memory.udr_limit.set" ||
         operation_id == "memory.dump_policy.set" ||
         operation_id == "memory.optimizer.set" ||
         operation_id == "memory.optimizer.run" ||
         operation_id == "memory.object_residency.set" ||
         operation_id == "memory.rate_limit.set" ||
         operation_id == "memory.policy_migration.plan";
}

bool IsStorageTierMigrationOperation(std::string_view operation_id) {
  return operation_id.starts_with("storage_tier.");
}

bool IsFilespaceDiscoveryOperation(std::string_view operation_id) {
  return operation_id.starts_with("filespace.discovery.");
}

bool IsFilespacePackageOperation(std::string_view operation_id) {
  return operation_id.starts_with("filespace.package.");
}

bool IsShardPlacementDescriptorOperation(std::string_view operation_id) {
  return operation_id.starts_with("shard_placement.");
}

bool IsStorageTierMigrationControlOperation(std::string_view operation_id) {
  return operation_id == "storage_tier.stage_migration" ||
         operation_id == "storage_tier.commit_migration" ||
         operation_id == "storage_tier.rollback_migration";
}

bool IsMigrationControlOperation(std::string_view operation_id) {
  return operation_id == "op.migration.begin_from_reference" ||
         operation_id == "op.migration.alter";
}

bool IsMigrationInspectOperation(std::string_view operation_id) {
  return operation_id == "op.show.migration" ||
         operation_id == "op.show.migrations";
}

bool IsSecurityInspectionOperation(std::string_view operation_id) {
  return operation_id == "op.show.audit" ||
         operation_id == "op.show.discovery_rights" ||
         operation_id == "op.show.grants" ||
         operation_id == "op.show.groups" ||
         operation_id == "op.show.identity_providers" ||
         operation_id == "op.show.masks" ||
         operation_id == "op.show.object_visibility" ||
         operation_id == "op.show.policies" ||
         operation_id == "op.show.rls" ||
         operation_id == "op.show.roles" ||
         operation_id == "op.show.security_events" ||
         operation_id == "op.show.security_profiles" ||
         operation_id == "op.show.users";
}

bool IsObservabilityExactShowOperation(std::string_view operation_id) {
  return operation_id == "op.show.buffer_pool" ||
         operation_id == "op.show.cache" ||
         operation_id == "op.show.capabilities" ||
         operation_id == "op.show.context" ||
         operation_id == "op.show.dialect" ||
         operation_id == "op.show.index_health" ||
         operation_id == "op.show.io" ||
         operation_id == "op.show.job" ||
         operation_id == "op.show.job_dependencies" ||
         operation_id == "op.show.job_runs" ||
         operation_id == "op.show.jobs" ||
         operation_id == "op.show.locks" ||
         operation_id == "op.show.metrics" ||
         operation_id == "op.show.metrics_family" ||
         operation_id == "op.show.performance" ||
         operation_id == "op.show.query_store" ||
         operation_id == "op.show.schema_path" ||
         operation_id == "op.show.search_path" ||
         operation_id == "op.show.sessions" ||
         operation_id == "op.show.statement_cache" ||
         operation_id == "op.show.statements" ||
         operation_id == "op.show.system" ||
         operation_id == "op.show.transaction" ||
         operation_id == "op.show.transaction_isolation" ||
         operation_id == "op.show.transactions" ||
         operation_id == "op.sbsql.surface_replay" ||
         operation_id == "op.show.version" ||
         operation_id == "op.show.wait_events";
}

template <typename TRequest>
TRequest TypedRequest(const SblrDispatchRequest& request) {
  TRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  return typed;
}

api::EngineBeginTransactionRequest TypedBeginTransactionRequest(
    const SblrDispatchRequest& request) {
  api::EngineBeginTransactionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.isolation_level = api::SecurityOptionValue(base, "transaction_isolation_level:");
  if (typed.isolation_level.empty()) {
    typed.isolation_level = request.context.transaction_isolation_level;
  }
  typed.transaction_policy_profile = base.policy_profile;
  const auto read_only = api::SecurityOptionValue(base, "transaction_read_only:");
  if (!read_only.empty()) {
    typed.transaction_policy_profile.encoded_profiles.push_back(
        std::string("read_only:") + LowerAscii(read_only));
  }
  return typed;
}

std::uint64_t DispatchOptionU64(const api::EngineApiRequest& request, const std::string& prefix) {
  const auto value = api::SecurityOptionValue(request, prefix);
  if (value.empty()) { return 0; }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

double DispatchOptionDouble(const api::EngineApiRequest& request, const std::string& prefix) {
  const auto value = api::SecurityOptionValue(request, prefix);
  if (value.empty()) { return 0.0; }
  try {
    return std::stod(value);
  } catch (...) {
    return 0.0;
  }
}

api::EngineMemoryManagementOperation MemoryOperationForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "memory.policy.validate") {
    return api::EngineMemoryManagementOperation::validate_governance;
  }
  if (operation_id == "memory.cache.flush" ||
      operation_id == "memory.cache.invalidate" ||
      operation_id == "memory.scavenge" ||
      operation_id == "memory.grant_feedback.reset") {
    return api::EngineMemoryManagementOperation::plan_cache_control;
  }
  if (operation_id == "memory.pressure.show") {
    return api::EngineMemoryManagementOperation::plan_pressure_response;
  }
  if (operation_id == "memory.report.create" ||
      operation_id == "memory.incident.bundle") {
    return api::EngineMemoryManagementOperation::create_report;
  }
  if (operation_id == "memory.optimizer.show") {
    return api::EngineMemoryManagementOperation::review_recommendation;
  }
  if (operation_id == "memory.optimizer.set" ||
      operation_id == "memory.optimizer.run") {
    return api::EngineMemoryManagementOperation::apply_safe_recommendation;
  }
  if (operation_id == "memory.object_residency.show") {
    return api::EngineMemoryManagementOperation::inspect_object_residency;
  }
  if (operation_id == "memory.object_residency.set") {
    return api::EngineMemoryManagementOperation::set_object_residency;
  }
  if (operation_id == "memory.rate_limit.show") {
    return api::EngineMemoryManagementOperation::inspect_rate_limit;
  }
  if (operation_id == "memory.rate_limit.set") {
    return api::EngineMemoryManagementOperation::set_rate_limit;
  }
  if (operation_id == "memory.policy_upgrade.plan") {
    return api::EngineMemoryManagementOperation::plan_policy_upgrade;
  }
  if (operation_id == "memory.policy_migration.plan" ||
      operation_id == "memory.profile.set" ||
      operation_id == "memory.stream_policy.set" ||
      operation_id == "memory.udr_limit.set" ||
      operation_id == "memory.dump_policy.set") {
    return api::EngineMemoryManagementOperation::plan_policy_migration;
  }
  return api::EngineMemoryManagementOperation::inspect_governance;
}

scratchbird::core::memory::MemoryPolicyConfig DefaultMemoryPolicyConfig() {
  scratchbird::core::memory::MemoryPolicyConfig config;
  config.policy_name = "sblr_public_memory_descriptor_policy";
  config.hard_limit_bytes = 256ull * 1024ull * 1024ull;
  config.soft_limit_bytes = 192ull * 1024ull * 1024ull;
  config.per_context_limit_bytes = 64ull * 1024ull * 1024ull;
  config.page_buffer_pool_limit_bytes = 64ull * 1024ull * 1024ull;
  config.enable_platform_memory_probe = false;
  config.policy_generation = 7;
  return config;
}

void FillMemoryGovernanceDescriptor(api::EngineMemoryManagementRequest* request) {
  request->governance.profile_uuid.canonical = "019f1000-0000-7000-8000-000000000010";
  request->governance.policy_config = DefaultMemoryPolicyConfig();
  request->governance.expected_policy_generation = 7;
  request->governance.observed_policy_generation = 7;
  request->governance.profile_resolved = true;
  request->governance.memory_tree_snapshot_present = true;
  request->governance.cache_governor_registered = true;
  request->governance.cache_flush_or_invalidation_requested = true;
  request->governance.pressure_observation_present = true;
  request->governance.grant_feedback_surface_present = true;
  request->governance.parser_front_door_limit_surface_present = true;
  request->governance.udr_limit_surface_present = true;
  request->governance.streaming_window_surface_present = true;
  request->governance.maintenance_budget_surface_present = true;
  request->governance.dump_swap_policy_present = true;
  request->governance.allocator_scavenging_surface_present = true;
  request->governance.platform_capability_matrix_present = true;
  request->governance.protected_material_redaction_validated = true;
  request->governance.activation_timing_declared = true;
  request->governance.current_snapshot.current_bytes = 128ull * 1024ull * 1024ull;
  request->governance.pressure_observation.route_label = "sblr.public.memory.management";
  request->governance.pressure_observation.operation_id = request->operation_id;
  request->governance.pressure_observation.current_bytes = 900;
  request->governance.pressure_observation.soft_limit_bytes = 700;
  request->governance.pressure_observation.hard_limit_bytes = 1000;
  request->governance.pressure_observation.unified_budget_bytes = 900;
  request->governance.pressure_observation.unified_budget_limit_bytes = 1000;
  request->governance.pressure_observation.spill_supported = true;
  request->governance.pressure_observation.page_cache_shrink_supported = true;
  request->governance.pressure_observation.background_cleanup_supported = true;
  request->governance.pressure_observation.cancellation_supported = true;
  request->governance.pressure_observation.engine_mga_authoritative = true;
}

void FillMemoryAutomationDescriptor(api::EngineMemoryManagementRequest* request) {
  request->automation.recommendation_uuid.canonical =
      "019f1000-0000-7000-8000-000000000020";
  request->automation.report_generation = 3;
  request->automation.recommendation_generation = 4;
  request->automation.report_bounded = true;
  request->automation.report_redaction_validated = true;
  request->automation.metrics_contract_present = true;
  request->automation.recommendation_explainable = true;
  request->automation.recommend_only_default = true;
  request->automation.safe_apply_requested = true;
  request->automation.maintenance_window_bound = true;
  request->automation.audit_enabled = true;
  request->automation.guardrail_policy_resolved = true;
}

void FillMemoryObjectResidencyDescriptor(api::EngineMemoryManagementRequest* request) {
  request->object_residency.object_uuid.canonical =
      request->target_object.uuid.canonical.empty()
          ? "019f1000-0000-7000-8000-000000000030"
          : request->target_object.uuid.canonical;
  request->object_residency.filespace_uuid.canonical =
      "019f1000-0000-7000-8000-000000000031";
  request->object_residency.object_kind =
      request->target_object.object_kind.empty() ? "table" : request->target_object.object_kind;
  request->object_residency.residency_class =
      api::EngineMemoryObjectResidencyClass::warm_on_open;
  request->object_residency.page_types = {
      scratchbird::storage::disk::PageType::row_data,
      scratchbird::storage::disk::PageType::index_btree_leaf};
  request->object_residency.expected_policy_generation = 7;
  request->object_residency.observed_policy_generation = 7;
  request->object_residency.warmup_budget_bytes = 16ull * 1024ull * 1024ull;
  request->object_residency.profile_resolved = true;
  request->object_residency.object_resolved = true;
  request->object_residency.filespace_placement_validated = true;
  request->object_residency.security_scope_validated = true;
  request->object_residency.cluster_placement_validated = true;
  request->object_residency.heat_history_derivative_only = true;
}

void FillMemoryRateLimitDescriptor(api::EngineMemoryManagementRequest* request) {
  request->rate_limit.limit_class = api::EngineMemoryRateLimitClass::cache_flush_abuse;
  request->rate_limit.action = api::EngineMemoryRateLimitAction::throttle;
  request->rate_limit.limit_per_window = 4;
  request->rate_limit.window_seconds = 60;
  request->rate_limit.policy_generation = 7;
  request->rate_limit.policy_resolved = true;
  request->rate_limit.audit_enabled = true;
}

void FillMemoryPolicyMigrationDescriptor(api::EngineMemoryManagementRequest* request) {
  request->migration.profile_uuid.canonical =
      "019f1000-0000-7000-8000-000000000040";
  request->migration.policy_uuid.canonical =
      "019f1000-0000-7000-8000-000000000041";
  request->migration.source_policy_version = 2;
  request->migration.target_policy_version = 3;
  request->migration.source_schema_version = 2;
  request->migration.target_schema_version = 3;
  request->migration.policy_schema_validated = true;
  request->migration.grant_feedback_migration_declared = true;
  request->migration.heat_history_migration_declared = true;
  request->migration.derivative_state_audit_enabled = true;
  request->migration.discard_incompatible_derivative_state_allowed = true;
}

api::EngineMemoryManagementRequest TypedMemoryManagementRequest(
    const SblrDispatchRequest& request) {
  api::EngineMemoryManagementRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.memory_operation = MemoryOperationForSblrOperation(base.operation_id);
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical =
        "019f1000-0000-7000-8000-0000000000ff";
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "memory_policy";
  }
  FillMemoryGovernanceDescriptor(&typed);
  FillMemoryAutomationDescriptor(&typed);
  FillMemoryObjectResidencyDescriptor(&typed);
  FillMemoryRateLimitDescriptor(&typed);
  FillMemoryPolicyMigrationDescriptor(&typed);
  typed.cluster_scoped =
      api::SecurityOptionBool(base, "cluster_scoped:", false);
  typed.parser_memory_authority = false;
  typed.transaction_finality_authority = false;
  typed.visibility_authority = false;
  typed.recovery_authority = false;
  typed.reference_or_wal_recovery_authority = false;
  typed.private_provider_dispatch_requested = false;
  return typed;
}

api::EngineStorageTierMigrationOperation StorageTierOperationForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "storage_tier.validate") {
    return api::EngineStorageTierMigrationOperation::validate;
  }
  if (operation_id == "storage_tier.plan_migration") {
    return api::EngineStorageTierMigrationOperation::plan_migration;
  }
  if (operation_id == "storage_tier.stage_migration") {
    return api::EngineStorageTierMigrationOperation::stage_migration;
  }
  if (operation_id == "storage_tier.commit_migration") {
    return api::EngineStorageTierMigrationOperation::commit_migration;
  }
  if (operation_id == "storage_tier.rollback_migration") {
    return api::EngineStorageTierMigrationOperation::rollback_migration;
  }
  return api::EngineStorageTierMigrationOperation::inspect;
}

api::EngineStorageTierMigrationRequest TypedStorageTierMigrationRequest(
    const SblrDispatchRequest& request) {
  api::EngineStorageTierMigrationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.tier_operation = StorageTierOperationForSblrOperation(base.operation_id);
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical =
        "019f2000-0000-7000-8000-000000000020";
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "filespace";
  }
  typed.descriptor.storage_tier_policy_uuid.canonical =
      "019f2000-0000-7000-8000-000000000010";
  typed.descriptor.source_tier_uuid.canonical =
      "019f2000-0000-7000-8000-000000000011";
  typed.descriptor.target_tier_uuid.canonical =
      "019f2000-0000-7000-8000-000000000012";
  typed.descriptor.source_tier_class = api::EngineStorageTierClass::hot;
  typed.descriptor.target_tier_class = api::EngineStorageTierClass::cold;
  typed.descriptor.target_filespace_role =
      scratchbird::storage::filespace::FilespaceRole::secondary_data;
  typed.descriptor.page_types = {
      scratchbird::storage::disk::PageType::row_data,
      scratchbird::storage::disk::PageType::blob};
  const auto catalog_generation =
      typed.context.catalog_generation_id == 0 ? 12 : typed.context.catalog_generation_id;
  const auto policy_generation =
      typed.context.resource_epoch == 0 ? 34 : typed.context.resource_epoch;
  typed.descriptor.expected_catalog_generation = catalog_generation;
  typed.descriptor.observed_catalog_generation = catalog_generation;
  typed.descriptor.expected_policy_generation = policy_generation;
  typed.descriptor.observed_policy_generation = policy_generation;
  typed.descriptor.storage_tier_policy_resolved = true;
  typed.descriptor.filespace_role_known = true;
  typed.descriptor.page_family_eligibility_validated = true;
  typed.descriptor.typed_dependency_manifest_validated = false;
  typed.descriptor.cluster_scoped =
      api::SecurityOptionBool(base, "cluster_scoped:", false);
  typed.descriptor.physical_data_movement_requested = false;
  return typed;
}

api::EngineFilespaceDiscoveryScope FilespaceDiscoveryScopeForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "filespace.discovery.orphan_scan") {
    return api::EngineFilespaceDiscoveryScope::orphan_only;
  }
  if (operation_id == "filespace.discovery.stale_scan") {
    return api::EngineFilespaceDiscoveryScope::stale_only;
  }
  return api::EngineFilespaceDiscoveryScope::all;
}

api::EngineFilespaceDiscoveryRequest TypedFilespaceDiscoveryRequest(
    const SblrDispatchRequest& request) {
  api::EngineFilespaceDiscoveryRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.discovery_scope = FilespaceDiscoveryScopeForSblrOperation(base.operation_id);
  typed.runtime_filesystem_scan_requested = false;
  typed.parser_filesystem_authority = false;
  typed.parser_storage_authority = false;
  typed.transaction_finality_authority = false;
  typed.recovery_authority = false;
  typed.reference_or_wal_recovery_authority = false;
  typed.mutation_requested = false;
  return typed;
}

api::EngineFilespacePackageAction FilespacePackageActionForSblrOperation(
    std::string_view operation_id) {
  if (operation_id == "filespace.package.export_manifest") {
    return api::EngineFilespacePackageAction::export_manifest;
  }
  if (operation_id == "filespace.package.import_to_quarantine") {
    return api::EngineFilespacePackageAction::import_to_quarantine;
  }
  if (operation_id == "filespace.package.admit") {
    return api::EngineFilespacePackageAction::admit;
  }
  if (operation_id == "filespace.package.reject") {
    return api::EngineFilespacePackageAction::reject;
  }
  return api::EngineFilespacePackageAction::inspect_manifest;
}

api::EngineFilespacePackageRequest TypedFilespacePackageRequest(
    const SblrDispatchRequest& request) {
  api::EngineFilespacePackageRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.package_operation = FilespacePackageActionForSblrOperation(base.operation_id);
  typed.runtime_package_file_io_requested = false;
  typed.parser_file_io_authority = false;
  typed.parser_storage_authority = false;
  typed.transaction_finality_authority = false;
  typed.recovery_authority = false;
  typed.reference_or_wal_recovery_authority = false;
  typed.private_provider_dispatch_requested = false;
  return typed;
}

std::string ShardPlacementActionForSblrOperation(std::string_view operation_id) {
  constexpr std::string_view kPrefix = "shard_placement.";
  if (operation_id.starts_with(kPrefix)) {
    return std::string(operation_id.substr(kPrefix.size()));
  }
  return {};
}

api::EngineShardPlacementDescriptor DefaultShardPlacementDescriptor(
    std::string shard_suffix,
    std::uint64_t generation) {
  api::EngineShardPlacementDescriptor descriptor;
  descriptor.shard_uuid =
      "019f4000-0000-7000-8000-000000000" + std::move(shard_suffix);
  descriptor.source_filespace_uuid = "019f4000-0000-7000-8000-000000000101";
  descriptor.target_filespace_uuid = "019f4000-0000-7000-8000-000000000102";
  descriptor.range_begin = "0000000000000000";
  descriptor.range_end = "ffffffffffffffff";
  descriptor.placement_epoch = 41;
  descriptor.placement_generation = generation;
  descriptor.state = "planned";
  return descriptor;
}

api::EngineShardPlacementOperationRequest TypedShardPlacementDescriptorRequest(
    const SblrDispatchRequest& request) {
  api::EngineShardPlacementOperationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.placement_operation = ShardPlacementActionForSblrOperation(base.operation_id);
  typed.descriptor = DefaultShardPlacementDescriptor("201", 7);
  typed.merge_inputs = {
      DefaultShardPlacementDescriptor("211", 5),
      DefaultShardPlacementDescriptor("212", 5),
  };
  typed.operator_authorized = true;
  typed.physical_data_movement_requested = false;
  return typed;
}

constexpr std::string_view kEncryptionRouteDatabaseUuid =
    "019f5000-0000-7000-8000-000000000001";
constexpr std::string_view kEncryptionRouteFilespaceUuid =
    "019f5000-0000-7000-8000-000000000002";
constexpr std::string_view kEncryptionRouteKeyUuid =
    "019f5000-0000-7000-8000-000000000003";
constexpr std::string_view kEncryptionRouteReplacementKeyUuid =
    "019f5000-0000-7000-8000-000000000004";
constexpr std::string_view kEncryptionRouteProtectedMaterialUuid =
    "019f5000-0000-7000-8000-000000000005";
constexpr std::string_view kEncryptionRouteProtectedMaterialVersionUuid =
    "019f5000-0000-7000-8000-000000000006";
constexpr std::string_view kEncryptionRouteProtectedMaterialNextVersionUuid =
    "019f5000-0000-7000-8000-000000000007";

void FillProtectedMaterialTargetDatabase(api::EngineApiRequest* request) {
  if (request == nullptr) return;
  if (request->target_database.uuid.canonical.empty()) {
    request->target_database.uuid.canonical =
        request->context.database_uuid.canonical.empty()
            ? std::string(kEncryptionRouteDatabaseUuid)
            : request->context.database_uuid.canonical;
  }
  if (request->target_database.object_kind.empty()) {
    request->target_database.object_kind = "database";
  }
}

api::EngineProtectedMaterialPolicySet ProtectedMaterialRoutePolicy() {
  api::EngineProtectedMaterialPolicySet policy;
  policy.retention_policy_uuid = "019f5000-0000-7000-8000-000000000101";
  policy.access_policy_uuid = "019f5000-0000-7000-8000-000000000102";
  policy.release_policy_uuid = "019f5000-0000-7000-8000-000000000103";
  policy.purge_policy_uuid = "019f5000-0000-7000-8000-000000000104";
  policy.audit_policy_uuid = "019f5000-0000-7000-8000-000000000105";
  policy.release_purposes = {"filespace.open"};
  return policy;
}

api::EngineAdmitEncryptionKeyRequest TypedAdmitEncryptionKeyRequest(
    const SblrDispatchRequest& request) {
  api::EngineAdmitEncryptionKeyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.key_uuid = std::string(kEncryptionRouteKeyUuid);
  typed.key_label = "filespace-key-redacted";
  typed.filespace_uuid = std::string(kEncryptionRouteFilespaceUuid);
  typed.secret_evidence = "wrapped-reference:v1:route";
  typed.cache_ttl_millis = 300000;
  return typed;
}

api::EngineRotateEncryptionKeyRequest TypedRotateEncryptionKeyRequest(
    const SblrDispatchRequest& request) {
  api::EngineRotateEncryptionKeyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.key_uuid = std::string(kEncryptionRouteKeyUuid);
  typed.replacement_key_uuid = std::string(kEncryptionRouteReplacementKeyUuid);
  typed.replacement_secret_evidence = "wrapped-reference:v1:route-replacement";
  typed.rotation_reason = "public-route-rekey";
  typed.cache_ttl_millis = 300000;
  return typed;
}

api::EngineInspectProtectedMaterialCacheRequest TypedInspectProtectedMaterialCacheRequest(
    const SblrDispatchRequest& request) {
  api::EngineInspectProtectedMaterialCacheRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  return typed;
}

api::EnginePurgeProtectedMaterialRequest TypedPurgeProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EnginePurgeProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.purge_reason = "public-route-cache-purge";
  return typed;
}

api::EngineShutdownProtectedMaterialRequest TypedShutdownProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineShutdownProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.shutdown_reason = "public-route-shutdown-purge";
  return typed;
}

api::EngineOpenEncryptedFilespaceRequest TypedOpenEncryptedFilespaceRequest(
    const SblrDispatchRequest& request) {
  api::EngineOpenEncryptedFilespaceRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.database_uuid = typed.target_database.uuid.canonical;
  typed.filespace_uuid = std::string(kEncryptionRouteFilespaceUuid);
  typed.key_uuid = std::string(kEncryptionRouteKeyUuid);
  typed.encrypted_filespace = true;
  typed.decryption_required = true;
  return typed;
}

api::EngineRequestProtectedMaterialRequest TypedRequestProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineRequestProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.purpose = "filespace.open";
  return typed;
}

api::EnginePurgeProtectedMaterialVersionRequest TypedPurgeProtectedMaterialVersionRequest(
    const SblrDispatchRequest& request) {
  api::EnginePurgeProtectedMaterialVersionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.protected_material_version_uuid =
      std::string(kEncryptionRouteProtectedMaterialVersionUuid);
  typed.purge_reason = "public-route-cryptographic-erase";
  return typed;
}

api::EngineCreateProtectedMaterialRequest TypedCreateProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineCreateProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.object_class = "filespace_encryption_key";
  typed.owner_scope_uuid = std::string(kEncryptionRouteFilespaceUuid);
  typed.purpose_class = "encryption_use";
  typed.storage_class = "wrapped";
  typed.policy = ProtectedMaterialRoutePolicy();
  typed.initial_version_uuid = std::string(kEncryptionRouteProtectedMaterialVersionUuid);
  typed.protected_reference = "kms-ref:v1:protected-material-route";
  typed.envelope_reference = "kms-envelope:v1:protected-material-route";
  typed.payload_hash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return typed;
}

api::EngineAddProtectedMaterialVersionRequest TypedAddProtectedMaterialVersionRequest(
    const SblrDispatchRequest& request) {
  api::EngineAddProtectedMaterialVersionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.protected_material_version_uuid =
      std::string(kEncryptionRouteProtectedMaterialNextVersionUuid);
  typed.protected_reference = "kms-ref:v1:protected-material-route-rotation";
  typed.envelope_reference = "kms-envelope:v1:protected-material-route-rotation";
  typed.payload_hash =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  typed.storage_class = "wrapped";
  typed.rotation_reason = "public-route-protected-material-version";
  typed.policy_override = ProtectedMaterialRoutePolicy();
  return typed;
}

api::EngineResolveProtectedMaterialRequest TypedResolveProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineResolveProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.purpose = "filespace.open";
  return typed;
}

api::EngineReleaseProtectedMaterialRequest TypedReleaseProtectedMaterialRequest(
    const SblrDispatchRequest& request) {
  api::EngineReleaseProtectedMaterialRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.protected_material_version_uuid =
      std::string(kEncryptionRouteProtectedMaterialVersionUuid);
  typed.purpose = "filespace.open";
  return typed;
}

api::EngineInspectProtectedMaterialCatalogRequest TypedInspectProtectedMaterialCatalogRequest(
    const SblrDispatchRequest& request) {
  api::EngineInspectProtectedMaterialCatalogRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.include_versions = true;
  typed.include_audit = true;
  return typed;
}

api::EngineExportProtectedMaterialPackageRequest TypedExportProtectedMaterialPackageRequest(
    const SblrDispatchRequest& request) {
  api::EngineExportProtectedMaterialPackageRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.protected_material_uuid = std::string(kEncryptionRouteProtectedMaterialUuid);
  typed.include_versions = true;
  typed.include_audit = true;
  typed.export_reason = "public-route-protected-material-package-export";
  return typed;
}

api::EngineImportProtectedMaterialPackageRequest TypedImportProtectedMaterialPackageRequest(
    const SblrDispatchRequest& request) {
  api::EngineImportProtectedMaterialPackageRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  FillProtectedMaterialTargetDatabase(&typed);
  typed.encoded_package = api::SecurityOptionValue(base, "encoded_package:");
  typed.expected_package_digest =
      api::SecurityOptionValue(base, "expected_package_digest:");
  typed.import_authorized =
      api::SecurityOptionBool(base,
                              "protected_material_package_import_authorized:",
                              false);
  typed.import_reason = "public-route-protected-material-package-import";
  return typed;
}

api::EngineTypedValue DispatchTypedValueOption(const api::EngineApiRequest& request,
                                               const std::string& prefix,
                                               const std::string& fallback_type) {
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = api::SecurityOptionValue(request, prefix + "type:");
  if (value.descriptor.canonical_type_name.empty()) {
    value.descriptor.canonical_type_name = fallback_type;
  }
  value.descriptor.encoded_descriptor = api::SecurityOptionValue(request, prefix + "descriptor:");
  if (value.descriptor.encoded_descriptor.empty()) {
    value.descriptor.encoded_descriptor = "type=" + value.descriptor.canonical_type_name;
  }
  value.encoded_value = api::SecurityOptionValue(request, prefix + "value:");
  value.is_null = api::SecurityOptionBool(request, prefix + "null:", false);
  return value;
}

api::EngineObjectReference DispatchTargetOfKind(const api::EngineApiRequest& request,
                                                const std::string& kind) {
  if (request.target_object.object_kind == kind) { return request.target_object; }
  for (const auto& object : request.related_objects) {
    if (object.object_kind == kind) { return object; }
  }
  return {};
}

api::EngineObjectReference TargetObjectForDml(const api::EngineApiRequest& request,
                                              const std::string& default_kind) {
  if (!request.target_object.uuid.canonical.empty()) { return request.target_object; }
  api::EngineObjectReference target = DispatchTargetOfKind(request, default_kind);
  if (!target.uuid.canonical.empty()) { return target; }
  target.uuid.canonical = api::SecurityOptionValue(request, "target_object_uuid:");
  target.object_kind = api::SecurityOptionValue(request, "target_object_kind:");
  if (target.object_kind.empty()) target.object_kind = default_kind;
  return target;
}

bool IsCreateSchemaRuntimeOption(std::string_view option) {
  return option.starts_with("comment:") ||
         option.starts_with("localized_comment:") ||
         option.starts_with("unresolved_schema_parent_path:");
}

std::vector<std::string> SplitDottedIdentifierPath(std::string_view path) {
  std::vector<std::string> parts;
  std::string current;
  bool quoted = false;
  for (std::size_t index = 0; index < path.size(); ++index) {
    const char ch = path[index];
    if (quoted) {
      if (ch == '"') {
        if (index + 1 < path.size() && path[index + 1] == '"') {
          current.push_back('"');
          ++index;
        } else {
          quoted = false;
        }
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '"') {
      quoted = true;
      continue;
    }
    if (ch == '.') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(ch)) || !current.empty()) {
      current.push_back(ch);
    }
  }
  if (!current.empty()) { parts.push_back(current); }
  return parts;
}

std::string JoinDottedIdentifierPath(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) {
    if (part.empty()) { continue; }
    if (!out.empty()) { out.push_back('.'); }
    out += part;
  }
  return out;
}

std::optional<std::string> ResolveSchemaParentPathToUuid(const api::EngineApiRequest& request,
                                                         const std::string& parent_path,
                                                         std::string* normalized_path) {
  if (parent_path.empty()) { return std::nullopt; }
  const auto parts = SplitDottedIdentifierPath(parent_path);
  const std::string normalized = JoinDottedIdentifierPath(parts);
  if (normalized.empty()) { return std::nullopt; }
  if (normalized_path != nullptr) { *normalized_path = normalized; }

  const std::string profile = request.context.identifier_profile_uuid.empty()
                                  ? "sbsql_v3"
                                  : request.context.identifier_profile_uuid;
  std::vector<std::string> lookup_keys;
  auto add_key = [&](const std::string& value) {
    if (value.empty()) { return; }
    const std::string folded = api::NameRegistryLookupKey(value, profile, false);
    if (std::find(lookup_keys.begin(), lookup_keys.end(), folded) == lookup_keys.end()) {
      lookup_keys.push_back(folded);
    }
  };
  add_key(parent_path);
  add_key(normalized);

  auto context = request.context;
  const std::uint64_t observer_tx =
      context.snapshot_visible_through_local_transaction_id != 0
          ? context.snapshot_visible_through_local_transaction_id
          : context.local_transaction_id;
  auto loaded = api::LoadNameRegistryState(context, observer_tx);
  if (!loaded.ok && context.local_transaction_id != 0) {
    context.local_transaction_id = 0;
    loaded = api::LoadNameRegistryState(context, observer_tx);
  }
  if (!loaded.ok) { return std::nullopt; }

  std::optional<std::string> match;
  for (const auto& entry : loaded.state.entries) {
    if (entry.deleted || entry.lifecycle_state != "active" || entry.object_class != "schema") {
      continue;
    }
    const std::string entry_full_key = entry.full_path_lookup_key.empty()
                                           ? entry.normalized_lookup_key
                                           : entry.full_path_lookup_key;
    const bool key_matches =
        std::find(lookup_keys.begin(), lookup_keys.end(), entry_full_key) != lookup_keys.end() ||
        (parts.size() == 1 &&
         std::find(lookup_keys.begin(), lookup_keys.end(), entry.normalized_lookup_key) != lookup_keys.end());
    if (!key_matches) { continue; }
    if (match && *match != entry.object_uuid) { return std::nullopt; }
    match = entry.object_uuid;
  }
  return match;
}

api::EngineCreateSchemaRequest TypedCreateSchemaRequest(const SblrDispatchRequest& request) {
  api::EngineCreateSchemaRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "schema_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "schema";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    std::string schema_name = api::SecurityOptionValue(base, "schema_name:");
    if (schema_name.empty()) schema_name = api::SecurityOptionValue(base, "name:");
    if (!schema_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        const auto parts = SplitDottedIdentifierPath(schema_parent_path);
        normalized_parent_path = JoinDottedIdentifierPath(parts);
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + schema_name;
      typed.localized_names.push_back({"en", "primary", full_path, schema_name, true});
    }
  }

  typed.option_envelopes.clear();
  for (const auto& option : base.option_envelopes) {
    if (IsCreateSchemaRuntimeOption(option)) {
      typed.option_envelopes.push_back(option);
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  return typed;
}

api::EngineCreateTableRequest TypedCreateTableRequest(const SblrDispatchRequest& request) {
  api::EngineCreateTableRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_database = base.target_database;
  typed.target_schema = base.target_schema;
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  const std::string explicit_target_schema_uuid =
      api::SecurityOptionValue(base, "target_schema_uuid:");
  const std::string explicit_schema_uuid = api::SecurityOptionValue(base, "schema_uuid:");
  const std::string explicit_parent_schema_uuid =
      api::SecurityOptionValue(base, "schema_parent_uuid:");
  const bool has_explicit_schema_uuid = !explicit_target_schema_uuid.empty() ||
                                        !explicit_schema_uuid.empty() ||
                                        !explicit_parent_schema_uuid.empty();
  bool unresolved_parent_path = false;
  if (!schema_parent_path.empty() && !has_explicit_schema_uuid) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    } else {
      typed.target_schema.uuid.canonical.clear();
      unresolved_parent_path = true;
    }
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = explicit_target_schema_uuid;
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = explicit_schema_uuid;
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = explicit_parent_schema_uuid;
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  typed.option_envelopes.clear();
  typed.option_envelopes = base.option_envelopes;
  if (unresolved_parent_path) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  typed.requested_table_uuid = base.target_object.uuid;
  if (typed.requested_table_uuid.canonical.empty()) {
    typed.requested_table_uuid.canonical = api::SecurityOptionValue(base, "table_object_uuid:");
  }
  typed.table_names = base.localized_names;
  if (typed.table_names.empty()) {
    std::string table_name = api::SecurityOptionValue(base, "table_name:");
    if (table_name.empty()) { table_name = api::SecurityOptionValue(base, "name:"); }
    if (!table_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + table_name;
      typed.table_names.push_back({"en", "primary", full_path, table_name, true});
    }
  }
  typed.table_columns = base.columns;
  if (typed.table_columns.empty()) {
    std::uint64_t column_count = DispatchOptionU64(base, "column_count:");
    if (column_count == 0) {
      column_count = DispatchOptionU64(base, "column_definition_count:");
    }
    if (column_count == 0 &&
        !api::SecurityOptionValue(base, "column_0_name:").empty()) {
      column_count = 1;
    }
    for (std::uint64_t ordinal = 0; ordinal < column_count; ++ordinal) {
      const std::string prefix = "column_" + std::to_string(ordinal) + "_";
      std::string column_name = api::SecurityOptionValue(base, prefix + "name:");
      std::string column_type = api::SecurityOptionValue(base, prefix + "type:");
      std::string column_descriptor = api::SecurityOptionValue(base, prefix + "descriptor:");
      std::string column_default = api::SecurityOptionValue(base, prefix + "default:");
      if (ordinal == 0 && column_type.empty()) {
        column_type = api::SecurityOptionValue(base, "canonical_type_name:");
      }
      if (column_name.empty() || column_type.empty()) continue;
      if (column_descriptor.empty()) {
        column_descriptor = "type=" + column_type;
      } else if (column_descriptor.find("type=") == std::string::npos &&
                 column_descriptor.find("canonical=") == std::string::npos) {
        column_descriptor = "type=" + column_type + ";" + column_descriptor;
      }
      api::EngineColumnDefinition column;
      column.ordinal = static_cast<std::uint32_t>(ordinal);
      column.names.push_back({"en", "primary", "", column_name, true});
      column.descriptor.descriptor_kind = "scalar";
      column.descriptor.canonical_type_name = column_type;
      const std::string nullable = LowerAscii(api::SecurityOptionValue(base, prefix + "nullable:"));
      column.nullable = nullable.empty() || nullable == "true" || nullable == "1";
      if (column_descriptor.find("nullable=") == std::string::npos) {
        column_descriptor += ";nullable=";
        column_descriptor += column.nullable ? "true" : "false";
      }
      if (!column_default.empty()) {
        column.default_expression_envelope = column_default;
        if (column_descriptor.find("default=") == std::string::npos &&
            column_descriptor.find("default_expression=") == std::string::npos) {
          column_descriptor += ";default=" + column_default;
        }
      }
      column.descriptor.encoded_descriptor = column_descriptor;
      typed.table_columns.push_back(std::move(column));
    }
  }
  typed.table_constraints = base.constraints;
  typed.table_indexes = base.indexes;
  typed.table_physical_profile = base.physical_profile;
  const std::string physical_profile = [&]() {
    std::string value = api::SecurityOptionValue(base, "physical_profile:");
    if (value.empty()) value = api::SecurityOptionValue(base, "table_physical_profile:");
    return value;
  }();
  if (!physical_profile.empty()) {
    bool present = false;
    for (const auto& existing : typed.table_physical_profile.encoded_profiles) {
      if (existing == physical_profile) {
        present = true;
        break;
      }
    }
    if (!present) {
      typed.table_physical_profile.encoded_profiles.push_back(physical_profile);
    }
  }
  typed.table_policy_profile = base.policy_profile;
  typed.table_compatibility_profile = base.compatibility_profile;
  return typed;
}

api::EngineCreateStatisticsRequest TypedCreateStatisticsRequest(const SblrDispatchRequest& request) {
  api::EngineCreateStatisticsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = DispatchTargetOfKind(base, "table");
  if (typed.target_table.uuid.canonical.empty()) {
    typed.target_table.uuid.canonical = api::SecurityOptionValue(base, "statistics_target_uuid:");
    typed.target_table.object_kind = api::SecurityOptionValue(base, "statistics_target_kind:");
  }
  if (typed.target_table.uuid.canonical.empty()) {
    typed.target_table.uuid.canonical = api::SecurityOptionValue(base, "target_table_uuid:");
    typed.target_table.object_kind = "table";
  }
  if (typed.target_table.object_kind.empty()) { typed.target_table.object_kind = "table"; }
  if (base.target_object.object_kind == "statistics") {
    typed.requested_statistics_uuid = base.target_object.uuid;
  }
  typed.statistics_names = base.localized_names;
  for (const auto& option : base.option_envelopes) {
    if (option.rfind("statistics_kind:", 0) == 0) {
      typed.statistics_kinds.push_back(option.substr(16));
    } else if (option.rfind("statistics_expression:", 0) == 0) {
      typed.expression_envelopes.push_back(option.substr(22));
    }
  }
  if (typed.statistics_kinds.empty()) {
    const std::string kind = api::SecurityOptionValue(base, "statistics_kind:");
    if (!kind.empty()) { typed.statistics_kinds.push_back(kind); }
  }
  return typed;
}

api::EngineCreateIndexRequest TypedCreateIndexRequest(const SblrDispatchRequest& request) {
  api::EngineCreateIndexRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  api::EngineObjectReference target_table = DispatchTargetOfKind(base, "table");
  if (target_table.uuid.canonical.empty()) {
    target_table.uuid.canonical = api::SecurityOptionValue(base, "index_target_uuid:");
    target_table.object_kind = api::SecurityOptionValue(base, "index_target_kind:");
  }
  if (target_table.uuid.canonical.empty()) {
    target_table.uuid.canonical = api::SecurityOptionValue(base, "target_table_uuid:");
    target_table.object_kind = "table";
  }
  if (target_table.object_kind.empty()) target_table.object_kind = "table";
  if (!target_table.uuid.canonical.empty()) typed.target_object = target_table;
  if (typed.indexes.empty()) {
    api::EngineIndexDefinition index;
    index.requested_index_uuid.canonical = api::SecurityOptionValue(base, "index_object_uuid:");
    if (index.requested_index_uuid.canonical.empty() &&
        base.target_object.object_kind == "index") {
      index.requested_index_uuid = base.target_object.uuid;
    }
    const std::string name = [&]() {
      const std::string index_name = api::SecurityOptionValue(base, "index_name:");
      if (!index_name.empty()) return index_name;
      return api::SecurityOptionValue(base, "name:");
    }();
    if (!name.empty()) index.names.push_back({"en", "primary", "", name, true});
    index.index_kind = api::SecurityOptionValue(base, "index_profile:");
    if (index.index_kind.empty()) index.index_kind = "btree";
    const std::string key_envelope = api::SecurityOptionValue(base, "index_key_envelope:");
    if (!key_envelope.empty()) {
      std::string current;
      std::istringstream key_envelopes{key_envelope};
      while (std::getline(key_envelopes, current, ',')) {
        if (!current.empty()) {
          index.key_envelopes.push_back(current);
        }
      }
    } else {
      const std::string key_column = api::SecurityOptionValue(base, "index_key_column:");
      if (!key_column.empty()) index.key_envelopes.push_back(key_column);
    }
    if (api::SecurityOptionBool(base, "index_unique:", false) &&
        index.index_kind == "btree") {
      index.index_kind = "btree_unique";
    }
    if (!index.key_envelopes.empty()) typed.indexes.push_back(std::move(index));
  }
  return typed;
}

api::EngineCreateIndexTemplateRequest TypedCreateIndexTemplateRequest(const SblrDispatchRequest& request) {
  api::EngineCreateIndexTemplateRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "index_template_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    const std::string template_kind = api::SecurityOptionValue(base, "index_template_kind:");
    typed.target_object.object_kind = template_kind.empty() ? "index_template" : template_kind;
  }
  if (typed.localized_names.empty()) {
    const std::string name = [&]() {
      const std::string template_name = api::SecurityOptionValue(base, "index_template_name:");
      if (!template_name.empty()) return template_name;
      return api::SecurityOptionValue(base, "name:");
    }();
    if (!name.empty()) typed.localized_names.push_back({"en", "primary", "", name, true});
  }
  return typed;
}

api::EngineCreateSequenceRequest TypedCreateSequenceRequest(const SblrDispatchRequest& request) {
  api::EngineCreateSequenceRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "sequence_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "sequence";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const auto sequence_name = api::SecurityOptionValue(base, "name:");
    if (!sequence_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + sequence_name;
      typed.localized_names.push_back({"en", "primary", full_path, sequence_name, true});
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  return typed;
}

bool IsDomainRuntimeOption(std::string_view option) {
  return option.starts_with("default_expression:") ||
         option.starts_with("check_constraint:") ||
         option.starts_with("check_constraint_append:") ||
         option.starts_with("nullable:") ||
         option.starts_with("collation:") ||
         option.starts_with("charset:") ||
         option.starts_with("cast_policy:") ||
         option.starts_with("mutation_policy:") ||
         option.starts_with("masking_policy:") ||
         option.starts_with("visibility_policy:") ||
         option.starts_with("encryption_policy:") ||
         option.starts_with("driver_metadata:") ||
         option.starts_with("wire_metadata:") ||
         option.starts_with("element_path:") ||
         option.starts_with("method_binding:") ||
         option.starts_with("comment:") ||
         option.starts_with("localized_comment:") ||
         option.starts_with("reference_alias:");
}

api::EngineCreateDomainRequest TypedCreateDomainRequest(const SblrDispatchRequest& request) {
  api::EngineCreateDomainRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.option_envelopes.clear();
  for (const auto& option : base.option_envelopes) {
    if (IsDomainRuntimeOption(option)) { typed.option_envelopes.push_back(option); }
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "domain_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "domain";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const auto domain_name = api::SecurityOptionValue(base, "name:");
    if (!domain_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + domain_name;
      typed.localized_names.push_back({"en", "primary", full_path, domain_name, true});
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  if (typed.descriptors.empty()) {
    api::EngineDescriptor descriptor;
    descriptor.descriptor_uuid.canonical = api::SecurityOptionValue(base, "base_descriptor_uuid:");
    descriptor.descriptor_kind = api::SecurityOptionValue(base, "base_descriptor_kind:");
    descriptor.canonical_type_name = api::SecurityOptionValue(base, "base_canonical_type_name:");
    descriptor.encoded_descriptor = api::SecurityOptionValue(base, "base_encoded_descriptor:");
    if (!descriptor.canonical_type_name.empty()) {
      if (descriptor.descriptor_kind.empty()) descriptor.descriptor_kind = "scalar";
      if (descriptor.encoded_descriptor.empty()) {
        descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
      }
      typed.descriptors.push_back(std::move(descriptor));
    }
  }
  return typed;
}

api::EngineCreateViewRequest TypedCreateViewRequest(const SblrDispatchRequest& request) {
  api::EngineCreateViewRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "view_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = "view";
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const auto view_name = api::SecurityOptionValue(base, "name:");
    if (!view_name.empty()) {
      typed.localized_names.push_back({"en", "primary", "", view_name, true});
    }
  }
  return typed;
}

template <typename TRequest>
TRequest TypedCreateExecutableObjectRequest(const SblrDispatchRequest& request,
                                            std::string_view object_kind,
                                            std::string_view object_uuid_prefix) {
  TRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical =
        api::SecurityOptionValue(base, std::string(object_uuid_prefix) + "_object_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = std::string(object_kind);
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    std::string object_name =
        api::SecurityOptionValue(base, std::string(object_uuid_prefix) + "_name:");
    if (object_name.empty()) object_name = api::SecurityOptionValue(base, "name:");
    if (!object_name.empty()) {
      typed.localized_names.push_back({"en", "primary", "", object_name, true});
    }
  }
  for (std::size_t related_index = 0; related_index < 64; ++related_index) {
    const std::string prefix = "related_object_" + std::to_string(related_index);
    const std::string related_uuid =
        api::SecurityOptionValue(base, prefix + "_uuid:");
    if (related_uuid.empty() &&
        api::SecurityOptionValue(base, prefix + "_kind:").empty()) {
      break;
    }
    if (related_uuid.empty()) continue;
    api::EngineObjectReference related;
    related.uuid.canonical = related_uuid;
    related.object_kind = api::SecurityOptionValue(base, prefix + "_kind:");
    if (related.object_kind.empty()) related.object_kind = "table";
    if (related.object_kind != "executable_object" &&
        related.object_kind != "procedure" &&
        related.object_kind != "function" &&
        related.object_kind != "trigger") {
      continue;
    }
    typed.related_objects.push_back(std::move(related));
  }
  return typed;
}

api::EngineCatalogDescriptorMutationRequest TypedCatalogDescriptorMutationRequest(
    const SblrDispatchRequest& request) {
  api::EngineCatalogDescriptorMutationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_database = base.target_database;
  typed.target_schema = base.target_schema;
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    const std::string object_name = api::SecurityOptionValue(base, "name:");
    if (!object_name.empty()) {
      if (normalized_parent_path.empty() && !schema_parent_path.empty()) {
        normalized_parent_path = JoinDottedIdentifierPath(SplitDottedIdentifierPath(schema_parent_path));
      }
      const std::string full_path =
          normalized_parent_path.empty() ? std::string{} : normalized_parent_path + "." + object_name;
      typed.localized_names.push_back({"en", "primary", full_path, object_name, true});
    }
  }
  return typed;
}

api::EngineAlterObjectRequest TypedAlterObjectRequest(const SblrDispatchRequest& request) {
  api::EngineAlterObjectRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "target_object_uuid:");
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "domain_target_uuid:");
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "rename_target_uuid:");
  }
  if (typed.target_object.uuid.canonical.empty()) {
    typed.target_object.uuid.canonical = api::SecurityOptionValue(base, "sequence_target_uuid:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  }
  if (typed.target_object.object_kind.empty()) {
    typed.target_object.object_kind = api::SecurityOptionValue(base, "rename_target_kind:");
  }
  if (typed.target_object.object_kind.empty()) typed.target_object.object_kind = "object";
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "target_schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_uuid:");
  }
  if (typed.target_schema.uuid.canonical.empty()) {
    typed.target_schema.uuid.canonical = api::SecurityOptionValue(base, "schema_parent_uuid:");
  }
  std::string normalized_parent_path;
  const std::string schema_parent_path = api::SecurityOptionValue(base, "schema_parent_path:");
  if (typed.target_schema.uuid.canonical.empty() && !schema_parent_path.empty()) {
    const auto resolved_parent =
        ResolveSchemaParentPathToUuid(base, schema_parent_path, &normalized_parent_path);
    if (resolved_parent) {
      typed.target_schema.uuid.canonical = *resolved_parent;
    }
  }
  if (!typed.target_schema.uuid.canonical.empty() && typed.target_schema.object_kind.empty()) {
    typed.target_schema.object_kind = "schema";
  }
  if (typed.localized_names.empty()) {
    std::string new_name = api::SecurityOptionValue(base, "rename_new_name:");
    if (new_name.empty()) new_name = api::SecurityOptionValue(base, "new_name:");
    if (new_name.empty()) new_name = api::SecurityOptionValue(base, "name:");
    if (!new_name.empty()) {
      typed.localized_names.push_back({"en", "primary", "", new_name, true});
    }
  }
  if (typed.target_object.object_kind == "domain") {
    typed.option_envelopes.clear();
    for (const auto& option : base.option_envelopes) {
      if (IsDomainRuntimeOption(option)) {
        typed.option_envelopes.push_back(option);
      }
    }
  } else if (typed.target_object.object_kind == "schema") {
    typed.option_envelopes.clear();
    for (const auto& option : base.option_envelopes) {
      if (IsCreateSchemaRuntimeOption(option)) {
        typed.option_envelopes.push_back(option);
      }
    }
  }
  if (!schema_parent_path.empty() && typed.target_schema.uuid.canonical.empty()) {
    typed.option_envelopes.push_back("unresolved_schema_parent_path:" + schema_parent_path);
  }
  return typed;
}

api::EngineInsertRowsRequest TypedInsertRowsRequest(const SblrDispatchRequest& request) {
  api::EngineInsertRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.input_rows = base.rows;
  typed.estimated_row_count = DispatchOptionU64(base, "estimated_row_count:");
  typed.insert_mode = api::SecurityOptionValue(base, "insert_mode:");
  const std::string duplicate_mode = api::SecurityOptionValue(base, "duplicate_mode:");
  if (!duplicate_mode.empty()) { typed.duplicate_mode = duplicate_mode; }
  typed.on_conflict_action = api::SecurityOptionValue(base, "on_conflict_action:");
  typed.conflict_target_column = api::SecurityOptionValue(base, "conflict_target_column:");
  const auto append_conflict_update_columns = [&typed](std::string_view encoded) {
    std::string current;
    std::istringstream columns{std::string(encoded)};
    while (std::getline(columns, current, ',')) {
      if (!current.empty()) {
        typed.conflict_update_columns.push_back(current);
      }
    }
  };
  for (const auto& option : base.option_envelopes) {
    constexpr std::string_view prefix = "conflict_update_column:";
    if (option.rfind(prefix, 0) == 0) {
      append_conflict_update_columns(std::string_view(option).substr(prefix.size()));
    }
    constexpr std::string_view lowered_prefix = "on_conflict_update_column:";
    if (option.rfind(lowered_prefix, 0) == 0) {
      append_conflict_update_columns(std::string_view(option).substr(lowered_prefix.size()));
    }
  }
  typed.strict_bulk_load_requested = api::SecurityOptionBool(base, "strict_bulk_load_requested:", false);
  typed.reference_unique_checks_relaxed = api::SecurityOptionBool(base, "reference_unique_checks_relaxed:", false);
  typed.reference_foreign_key_checks_relaxed = api::SecurityOptionBool(base, "reference_foreign_key_checks_relaxed:", false);
  return typed;
}

api::EngineSelectRowsRequest TypedSelectRowsRequest(const SblrDispatchRequest& request) {
  api::EngineSelectRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.source_object = TargetObjectForDml(base, "table");
  typed.select_projection = base.projection;
  typed.select_predicate = base.predicate;
  typed.select_ordering = base.ordering;
  const std::string order_by = api::SecurityOptionValue(base, "order_by:");
  if (!order_by.empty() && typed.select_ordering.canonical_ordering_envelopes.empty()) {
    std::string direction = api::SecurityOptionValue(base, "order_direction:");
    if (direction.empty()) direction = "asc";
    typed.select_ordering.canonical_ordering_envelopes.push_back(order_by + ":" + direction);
  }
  typed.limit = DispatchOptionU64(base, "limit:");
  typed.offset = DispatchOptionU64(base, "offset:");
  return typed;
}

bool ParseRelationRowUuid(const std::string& row_uuid,
                          std::size_t* relation_index,
                          std::string* relation_row_uuid) {
  constexpr std::string_view prefix = "relation-";
  constexpr std::string_view marker = "-row-";
  if (!row_uuid.starts_with(prefix)) return false;
  const auto marker_pos = row_uuid.find(marker, prefix.size());
  if (marker_pos == std::string::npos || marker_pos == prefix.size()) return false;
  std::size_t parsed = 0;
  for (std::size_t i = prefix.size(); i < marker_pos; ++i) {
    const unsigned char ch = static_cast<unsigned char>(row_uuid[i]);
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10u + static_cast<std::size_t>(ch - '0');
  }
  if (relation_index != nullptr) *relation_index = parsed;
  if (relation_row_uuid != nullptr) {
    *relation_row_uuid = row_uuid.substr(marker_pos + marker.size());
  }
  return true;
}

api::EnginePlanOperationRequest TypedPlanOperationRequest(const SblrDispatchRequest& request) {
  api::EnginePlanOperationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_object = TargetObjectForDml(base, "table");
  for (std::size_t index = 0; index < 16; ++index) {
    const std::string prefix = "related_object_" + std::to_string(index) + "_";
    api::EngineObjectReference related;
    related.uuid.canonical = api::SecurityOptionValue(base, prefix + "uuid:");
    related.object_kind = api::SecurityOptionValue(base, prefix + "kind:");
    if (!related.uuid.canonical.empty()) {
      if (related.object_kind.empty()) related.object_kind = "table";
      typed.related_objects.push_back(std::move(related));
    }
  }
  typed.execute = api::SecurityOptionBool(base, "execute:", false);
  typed.query_operation = api::SecurityOptionValue(base, "query_operation:");
  typed.set_operation = api::SecurityOptionValue(base, "set_operation:");
  typed.set_by_name = api::SecurityOptionBool(base, "set_by_name:", false);
  typed.join_algorithm = api::SecurityOptionValue(base, "join_algorithm:");
  if (typed.join_algorithm.empty()) typed.join_algorithm = "hash";
  typed.left_key_column = DispatchOptionU64(base, "left_key_column:");
  typed.right_key_column = DispatchOptionU64(base, "right_key_column:");
  typed.left_key_field = api::SecurityOptionValue(base, "left_key_field:");
  typed.right_key_field = api::SecurityOptionValue(base, "right_key_field:");
  typed.group_key_column = DispatchOptionU64(base, "group_key_column:");
  typed.aggregate_value_column = DispatchOptionU64(base, "aggregate_value_column:");
  typed.aggregate_pair_value_column = DispatchOptionU64(base, "aggregate_pair_value_column:");
  typed.group_key_field = api::SecurityOptionValue(base, "group_key_field:");
  typed.aggregate_value_field = api::SecurityOptionValue(base, "aggregate_value_field:");
  typed.aggregate_pair_value_field = api::SecurityOptionValue(base, "aggregate_pair_value_field:");
  typed.aggregate_function = api::SecurityOptionValue(base, "aggregate_function:");
  if (typed.aggregate_function.empty()) typed.aggregate_function = "sum";
  typed.order_column = DispatchOptionU64(base, "order_column:");
  typed.order_field = api::SecurityOptionValue(base, "order_by:");
  typed.window_function = api::SecurityOptionValue(base, "window_function:");
  if (typed.window_function.empty()) typed.window_function = "row_number";
  typed.window_value_column = DispatchOptionU64(base, "window_value_column:");
  typed.window_value_field = api::SecurityOptionValue(base, "window_value_field:");
  typed.partition_key_column = DispatchOptionU64(base, "partition_column:");
  typed.partition_key_field = api::SecurityOptionValue(base, "partition_by:");
  typed.window_n = DispatchOptionU64(base, "window_n:");
  if (typed.window_n == 0) typed.window_n = DispatchOptionU64(base, "window_bucket_count:");
  if (typed.window_n == 0) typed.window_n = 1;
  typed.limit = DispatchOptionU64(base, "limit:");
  typed.offset = DispatchOptionU64(base, "offset:");

  for (const auto& row : base.rows) {
    std::size_t relation_index = 0;
    std::string relation_row_uuid;
    if (!ParseRelationRowUuid(row.requested_row_uuid.canonical,
                              &relation_index,
                              &relation_row_uuid)) {
      continue;
    }
    if (typed.relations.size() <= relation_index) {
      typed.relations.resize(relation_index + 1);
    }
    auto& relation = typed.relations[relation_index];
    relation.relation_name = "relation-" + std::to_string(relation_index);
    relation.descriptor_digest = relation.relation_name;
    api::EngineRowValue relation_row = row;
    relation_row.requested_row_uuid.canonical = relation_row_uuid;
    relation.rows.push_back(std::move(relation_row));
  }
  return typed;
}

SblrValue SblrValueFromProjectionArgument(
    const api::EngineProjectionFunctionArgument& argument) {
  SblrValue value;
  value.descriptor_id = argument.type_name;
  value.encoded_value = argument.encoded_value;
  value.text_value = argument.encoded_value;
  value.is_null = argument.is_null;
  if (argument.is_null) {
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (argument.type_name == "binary") {
    std::vector<std::uint8_t> bytes;
    if (HexDecodeBytes(argument.encoded_value, &bytes)) {
      value.binary_value = std::move(bytes);
      value.payload_kind = SblrValuePayloadKind::binary;
      return value;
    }
  }
  if (argument.type_name == "bigint" || argument.type_name == "int64" ||
      argument.type_name == "integer") {
    try {
      value.int64_value = std::stoll(argument.encoded_value);
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::signed_integer;
      return value;
    } catch (...) {
      value.payload_kind = SblrValuePayloadKind::text;
      return value;
    }
  }
  if (argument.type_name == "boolean" || argument.type_name == "bool") {
    const std::string lowered = LowerAscii(argument.encoded_value);
    if (lowered == "true" || lowered == "1") {
      value.int64_value = 1;
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::boolean;
      value.descriptor_id = "boolean";
      return value;
    }
    if (lowered == "false" || lowered == "0") {
      value.int64_value = 0;
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::boolean;
      value.descriptor_id = "boolean";
      return value;
    }
  }
  if (argument.type_name == "real64" || argument.type_name == "double" ||
      argument.type_name == "numeric" || argument.type_name == "decimal") {
    try {
      value.real64_value = std::stod(argument.encoded_value);
      value.has_real64_value = true;
      value.payload_kind = SblrValuePayloadKind::real64;
      return value;
    } catch (...) {
      value.payload_kind = SblrValuePayloadKind::text;
      return value;
    }
  }
  value.payload_kind = SblrValuePayloadKind::text;
  return value;
}

api::EngineTypedValue EngineTypedValueFromSblrValue(const SblrValue& value) {
  api::EngineTypedValue out;
  out.descriptor.descriptor_kind = "scalar";
  out.descriptor.canonical_type_name = value.descriptor_id.empty() ? "text" : value.descriptor_id;
  out.descriptor.encoded_descriptor = "type=" + out.descriptor.canonical_type_name;
  out.is_null = value.is_null;
  if (value.payload_kind == SblrValuePayloadKind::binary) {
    out.encoded_value = HexEncodeBytes(value.binary_value);
    return out;
  }
  out.encoded_value = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (value.has_int64_value) out.encoded_value = std::to_string(value.int64_value);
  if (value.has_uint64_value) out.encoded_value = std::to_string(value.uint64_value);
  if (value.has_real64_value) out.encoded_value = FormatReal64(value.real64_value);
  return out;
}

std::vector<std::string> ActiveSavepointNamesForContext(
    const api::EngineRequestContext& context);

SblrExecutionContext SblrExecutionContextFromEngineContext(
    const api::EngineRequestContext& context) {
  SblrExecutionContext out;
  out.database_path = context.database_path;
  out.database_uuid = context.database_uuid.canonical;
  out.cluster_uuid = context.cluster_uuid.canonical;
  out.node_uuid = context.node_uuid.canonical;
  out.transaction_uuid = context.transaction_uuid.canonical;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.statement_uuid = context.statement_uuid.canonical;
  out.session_uuid = context.session_uuid.canonical;
  out.user_uuid = context.principal_uuid.canonical;
  out.current_schema_uuid = context.current_schema_uuid.canonical;
  out.current_role_uuid = context.current_role_uuid.canonical;
  out.statement_timestamp = context.statement_timestamp;
  out.transaction_timestamp = context.transaction_timestamp;
  out.current_timestamp = context.current_timestamp;
  out.current_monotonic_ns = context.current_monotonic_ns;
  out.deterministic_random_u64 = context.deterministic_random_u64;
  out.deterministic_random_u64_present =
      context.deterministic_random_u64_present;
  out.deterministic_random_bytes_hex = context.deterministic_random_bytes_hex;
  out.deterministic_uuid_text = context.deterministic_uuid_text;
  out.security_context_present = context.security_context_present;
  out.current_sqlstate = context.current_sqlstate;
  out.current_diagnostic_uuid = context.current_diagnostic_uuid.canonical;
  out.current_diagnostic_id = context.current_diagnostic_uuid.canonical;
  out.client_protocol_uuid = context.client_protocol_uuid;
  out.application_name = context.application_name;
  out.read_only_mode = context.read_only_mode;
  out.last_row_count = context.last_row_count;
  out.last_row_count_present = context.last_row_count_present;
  out.transaction_context_present =
      context.local_transaction_id != 0 ||
      !context.transaction_uuid.canonical.empty();
  out.cluster_authority_available = context.cluster_authority_available;
  out.active_savepoint_names = ActiveSavepointNamesForContext(context);
  return out;
}

api::EngineApiDiagnostic FunctionDiagnosticToApi(const SblrRuntimeDiagnostic& diagnostic) {
  api::EngineApiDiagnostic out;
  out.code = diagnostic.diagnostic_id.empty() ? "SB_DIAG_FUNCTION_EXECUTION_FAILED"
                                             : diagnostic.diagnostic_id;
  out.message_key = diagnostic.message_key.empty() ? "engine.function.execution_failed"
                                                   : diagnostic.message_key;
  out.detail = diagnostic.detail;
  out.error = diagnostic.severity != SblrDiagnosticSeverity::info;
  return out;
}

std::vector<std::string> ActiveSavepointNamesForContext(const api::EngineRequestContext& context) {
  if (context.local_transaction_id == 0 || context.database_path.empty()) {
    return {};
  }
  return api::ActiveMgaSavepointNames(context);
}

api::EngineApiResult EngineReadSystemVariable(
    const SblrDispatchRequest& request) {
  const api::EngineApiRequest base = BaseApiRequest(request);
  const std::string exact_refusal =
      LowerAscii(api::SecurityOptionValue(base, "exact_refusal:"));
  if (exact_refusal == "true" || exact_refusal == "1" ||
      exact_refusal == "yes") {
    std::string diagnostic_id =
        api::SecurityOptionValue(base, "refusal_diagnostic_id:");
    if (diagnostic_id.empty()) {
      diagnostic_id = api::SecurityOptionValue(base, "diagnostic_id:");
    }
    if (diagnostic_id.empty()) {
      diagnostic_id = "SB_DIAG_FUNCTION_RUNTIME_REFUSAL";
    }

    api::EngineApiResult result;
    result.ok = false;
    result.operation_id = "expression.system_variable_read";
    result.embedded_trust_mode_observed =
        request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
    result.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        diagnostic_id,
        "engine.system_variable.exact_refusal",
        "reference variable compatibility surface is refused by fixed policy "
        "before engine side effects",
        true));
    result.evidence.push_back({"sblr_operation",
                               "expression.system_variable_read"});
    result.evidence.push_back({"sblr_opcode", "SBLR_SYSTEM_VARIABLE_READ"});
    const std::string variable_id =
        api::SecurityOptionValue(base, "variable_id:");
    if (!variable_id.empty()) {
      result.evidence.push_back({"canonical_variable_id", variable_id});
    }
    const std::string reference_source =
        api::SecurityOptionValue(base, "reference_source_spelling:");
    if (!reference_source.empty()) {
      result.evidence.push_back({"reference_source_spelling", reference_source});
    }
    const std::string refusal_function =
        api::SecurityOptionValue(base, "refusal_function_id:");
    if (!refusal_function.empty()) {
      result.evidence.push_back({"refusal_function_id", refusal_function});
    }
    result.evidence.push_back({"exact_refusal", "true"});
    result.evidence.push_back({"mga_visibility_authority",
                               "unchanged_context_read_no_lock_no_snapshot_mutation"});
    result.evidence.push_back({"transaction_effect", "read"});
    return result;
  }

  std::string variable_id = api::SecurityOptionValue(base, "variable_id:");
  if (variable_id.empty()) {
    variable_id = api::SecurityOptionValue(base, "canonical_variable_id:");
  }
  if (variable_id.empty()) {
    return FailureResult(request.context,
                         "expression.system_variable_read",
                         "SB_DIAG_SYSTEM_VARIABLE_ID_REQUIRED",
                         "engine.system_variable.id_required",
                         "system variable read requires canonical variable_id");
  }

  const auto sblr_context =
      SblrExecutionContextFromEngineContext(request.context);
  const auto variable_result =
      ResolveSblrContextVariable(variable_id, sblr_context);
  if (!variable_result.ok() || variable_result.scalar_values.size() != 1) {
    api::EngineApiResult result;
    result.ok = false;
    result.operation_id = "expression.system_variable_read";
    result.embedded_trust_mode_observed =
        request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
    if (variable_result.diagnostics.empty()) {
      result.diagnostics.push_back(api::MakeEngineApiDiagnostic(
          "SB_DIAG_SYSTEM_VARIABLE_RESULT_SHAPE_INVALID",
          "engine.system_variable.result_shape_invalid",
          "system variable read expected exactly one scalar value",
          true));
    } else {
      for (const auto& diagnostic : variable_result.diagnostics) {
        result.diagnostics.push_back(FunctionDiagnosticToApi(diagnostic));
      }
    }
    result.evidence.push_back({"sblr_operation",
                               "expression.system_variable_read"});
    result.evidence.push_back({"sblr_opcode", "SBLR_SYSTEM_VARIABLE_READ"});
    result.evidence.push_back({"canonical_variable_id", variable_id});
    result.evidence.push_back({"mga_visibility_authority",
                               "unchanged_context_read_no_lock_no_snapshot_mutation"});
    return result;
  }

  api::EngineTypedValue value =
      EngineTypedValueFromSblrValue(variable_result.scalar_values.front());
  api::EngineApiResult result;
  result.ok = true;
  result.operation_id = "expression.system_variable_read";
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  result.result_shape.result_kind = "rs.sbsql.scalar_value.v1";
  result.result_shape.columns.push_back(value.descriptor);

  api::EngineRowValue row;
  row.requested_row_uuid.canonical = "system-variable-read-row-0";
  row.fields.push_back({"value", std::move(value)});
  result.result_shape.rows.push_back(std::move(row));
  result.evidence.push_back({"sblr_operation",
                             "expression.system_variable_read"});
  result.evidence.push_back({"sblr_opcode", "SBLR_SYSTEM_VARIABLE_READ"});
  result.evidence.push_back({"canonical_variable_id", variable_id});
  const std::string reference_source =
      api::SecurityOptionValue(base, "reference_source_spelling:");
  if (!reference_source.empty()) {
    result.evidence.push_back({"reference_source_spelling", reference_source});
  }
  result.evidence.push_back({"mga_visibility_authority",
                             "unchanged_context_read_no_lock_no_snapshot_mutation"});
  result.evidence.push_back({"transaction_effect", "read"});
  return result;
}

SblrExecutionContext ProjectionOperatorContext(const api::EngineRequestContext& context) {
  return SblrExecutionContextFromEngineContext(context);
}

SblrValue ProjectionLiteralToSblrValue(const api::EngineProjectionExpression& expression) {
  api::EngineProjectionFunctionArgument argument;
  argument.type_name = expression.type_name;
  argument.encoded_value = expression.encoded_value;
  argument.is_null = expression.is_null;
  return SblrValueFromProjectionArgument(argument);
}

SblrValue ProjectionTypedValueToSblrValue(const api::EngineTypedValue& value) {
  api::EngineProjectionFunctionArgument argument;
  argument.type_name = value.descriptor.canonical_type_name;
  argument.encoded_value = value.encoded_value;
  argument.is_null = value.is_null;
  return SblrValueFromProjectionArgument(argument);
}

bool TruthFromProjectionValue(const api::EngineTypedValue& value, SblrTruthValue* truth) {
  if (truth == nullptr || value.descriptor.canonical_type_name != "boolean") return false;
  if (value.is_null) {
    *truth = SblrTruthValue::unknown;
    return true;
  }
  const std::string lowered = LowerAscii(value.encoded_value);
  *truth = (lowered == "true" || lowered == "1") ? SblrTruthValue::true_value
                                                  : SblrTruthValue::false_value;
  return true;
}

api::EngineProjectionFunctionResult ProjectionOperatorFailure(const api::EngineProjectionOperatorRequest& request,
                                                             std::string code,
                                                             std::string detail) {
  api::EngineProjectionFunctionResult out;
  out.ok = false;
  out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
      std::move(code), "engine.operator.projection_failed", std::move(detail), true));
  out.evidence.push_back({"operator_runtime", request.expression.operator_id});
  return out;
}

api::EngineProjectionFunctionResult EvaluateProjectionOperatorExpression(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression);

std::string EncodeConstructedProjectionValue(
    const std::vector<api::EngineTypedValue>& arguments,
    std::string_view opening,
    std::string_view closing) {
  std::ostringstream encoded;
  encoded << opening;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) encoded << ',';
    const auto& argument = arguments[index];
    encoded << argument.descriptor.canonical_type_name << ':';
    encoded << (argument.is_null ? "<NULL>" : argument.encoded_value);
  }
  encoded << closing;
  return encoded.str();
}

api::EngineProjectionFunctionResult ConstructSpecialProjectionValue(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression,
    std::string_view canonical_type_name,
    std::string_view encoded_opening,
    std::string_view encoded_closing,
    std::string_view evidence_id) {
  std::vector<api::EngineTypedValue> argument_values;
  argument_values.reserve(expression.arguments.size());
  for (const auto& argument : expression.arguments) {
    auto argument_result = EvaluateProjectionOperatorExpression(context, argument);
    if (!argument_result.ok) return argument_result;
    argument_values.push_back(std::move(argument_result.value));
  }

  api::EngineProjectionFunctionResult out;
  out.ok = true;
  SblrValue value;
  value.descriptor_id = std::string(canonical_type_name);
  value.payload_kind = SblrValuePayloadKind::descriptor_payload;
  value.is_null = false;
  value.encoded_value = EncodeConstructedProjectionValue(
      argument_values, encoded_opening, encoded_closing);
  out.value = EngineTypedValueFromSblrValue(value);
  out.evidence.push_back({"special_form_runtime", std::string(evidence_id)});
  return out;
}

api::EngineProjectionFunctionResult EvaluateProjectionOperatorExpression(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression);

api::EngineProjectionFunctionResult EvaluateProjectionFunction(
    const api::EngineProjectionFunctionRequest& request);

api::EngineProjectionFunctionResult OperatorResultToProjectionResult(
    const std::string& operator_id,
    const SblrResult& result) {
  api::EngineProjectionFunctionResult out;
  out.ok = result.ok() && result.scalar_values.size() == 1;
  out.evidence.push_back({"operator_runtime", operator_id});
  if (out.ok) {
    out.value = EngineTypedValueFromSblrValue(result.scalar_values.front());
    return out;
  }
  if (result.diagnostics.empty()) {
    out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        "SB_DIAG_OPERATOR_RESULT_SHAPE_INVALID",
        "engine.operator.result_shape_invalid",
        "operator projection expected exactly one scalar value",
        true));
  } else {
    for (const auto& diagnostic : result.diagnostics) {
      out.diagnostics.push_back(FunctionDiagnosticToApi(diagnostic));
    }
  }
  return out;
}

api::EngineProjectionFunctionResult EvaluateProjectionOperatorExpression(
    const api::EngineRequestContext& context,
    const api::EngineProjectionExpression& expression) {
  if (expression.expression_kind == "literal") {
    api::EngineProjectionFunctionResult out;
    out.ok = true;
    out.value = EngineTypedValueFromSblrValue(ProjectionLiteralToSblrValue(expression));
    return out;
  }
  if (expression.expression_kind == "function") {
    api::EngineProjectionOperatorRequest failure_request;
    failure_request.context = context;
    failure_request.expression = expression;
    if (expression.function_id.empty()) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "nested function projection requires a function id");
    }
    api::EngineProjectionFunctionRequest function_request;
    function_request.context = context;
    function_request.function_id = expression.function_id;
    std::vector<api::EngineEvidenceReference> argument_evidence;
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
      auto argument_result = EvaluateProjectionOperatorExpression(context, expression.arguments[index]);
      if (!argument_result.ok) return argument_result;
      api::EngineProjectionFunctionArgument argument;
      argument.name = expression.arguments[index].name.empty()
                          ? "arg" + std::to_string(index)
                          : expression.arguments[index].name;
      argument.type_name = argument_result.value.descriptor.canonical_type_name;
      argument.encoded_value = argument_result.value.encoded_value;
      argument.is_null = argument_result.value.is_null;
      function_request.arguments.push_back(std::move(argument));
      argument_evidence.insert(argument_evidence.end(),
                               argument_result.evidence.begin(),
                               argument_result.evidence.end());
    }
    auto out = EvaluateProjectionFunction(function_request);
    if (out.ok) {
      out.evidence.insert(out.evidence.end(), argument_evidence.begin(), argument_evidence.end());
    }
    return out;
  }
  if (expression.expression_kind == "special_form") {
    api::EngineProjectionOperatorRequest failure_request;
    failure_request.context = context;
    failure_request.expression = expression;
    if (expression.special_form_id == "sb.special.array_constructor") {
      if (expression.arguments.empty()) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "ARRAY constructor projection requires at least one operand");
      }
      return ConstructSpecialProjectionValue(context,
                                             expression,
                                             "array",
                                             "array[",
                                             "]",
                                             "special_array_constructor");
    }
    if (expression.special_form_id == "sb.special.row_constructor") {
      if (expression.arguments.empty()) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "ROW constructor projection requires at least one operand");
      }
      return ConstructSpecialProjectionValue(context,
                                             expression,
                                             "row",
                                             "row(",
                                             ")",
                                             "special_row_constructor");
    }
    if (expression.special_form_id == "sb.special.between") {
      if (expression.arguments.size() != 3) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "BETWEEN projection requires value, lower, and upper operands");
      }

      const auto value = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
      if (!value.ok) return value;
      const auto lower = EvaluateProjectionOperatorExpression(context, expression.arguments[1]);
      if (!lower.ok) return lower;
      const auto upper = EvaluateProjectionOperatorExpression(context, expression.arguments[2]);
      if (!upper.ok) return upper;

      const auto sblr_context = ProjectionOperatorContext(context);
      const auto ge_result = EvaluateSblrComparison("op_ge",
                                                    ProjectionTypedValueToSblrValue(value.value),
                                                    ProjectionTypedValueToSblrValue(lower.value),
                                                    sblr_context);
      const auto ge_projection = OperatorResultToProjectionResult("op_ge", ge_result);
      if (!ge_projection.ok) return ge_projection;
      const auto le_result = EvaluateSblrComparison("op_le",
                                                    ProjectionTypedValueToSblrValue(value.value),
                                                    ProjectionTypedValueToSblrValue(upper.value),
                                                    sblr_context);
      const auto le_projection = OperatorResultToProjectionResult("op_le", le_result);
      if (!le_projection.ok) return le_projection;

      SblrTruthValue ge_truth = SblrTruthValue::unknown;
      SblrTruthValue le_truth = SblrTruthValue::unknown;
      if (!TruthFromProjectionValue(ge_projection.value, &ge_truth) ||
          !TruthFromProjectionValue(le_projection.value, &le_truth)) {
        return ProjectionOperatorFailure(failure_request,
                                         "SBLR.DESCRIPTOR_MISMATCH",
                                         "BETWEEN comparison produced a non-boolean intermediate");
      }

      api::EngineProjectionFunctionResult out;
      out.ok = true;
      out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(SblrAnd(ge_truth, le_truth)));
      out.evidence.push_back({"special_form_runtime", "special_between"});
      out.evidence.push_back({"operator_runtime", "op_ge"});
      out.evidence.push_back({"operator_runtime", "op_le"});
      return out;
    }
    if (expression.special_form_id == "sb.special.in") {
      if (expression.arguments.size() < 2) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "IN projection requires value and at least one list operand");
      }
      const auto value = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
      if (!value.ok) return value;
      const auto sblr_context = ProjectionOperatorContext(context);
      bool saw_unknown = false;
      for (std::size_t index = 1; index < expression.arguments.size(); ++index) {
        const auto candidate = EvaluateProjectionOperatorExpression(context, expression.arguments[index]);
        if (!candidate.ok) return candidate;
        const auto eq_result = EvaluateSblrComparison("op_eq",
                                                      ProjectionTypedValueToSblrValue(value.value),
                                                      ProjectionTypedValueToSblrValue(candidate.value),
                                                      sblr_context);
        const auto eq_projection = OperatorResultToProjectionResult("op_eq", eq_result);
        if (!eq_projection.ok) return eq_projection;
        SblrTruthValue eq_truth = SblrTruthValue::unknown;
        if (!TruthFromProjectionValue(eq_projection.value, &eq_truth)) {
          return ProjectionOperatorFailure(failure_request,
                                           "SBLR.DESCRIPTOR_MISMATCH",
                                           "IN comparison produced a non-boolean intermediate");
        }
        if (eq_truth == SblrTruthValue::true_value) {
          api::EngineProjectionFunctionResult out;
          out.ok = true;
          out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(SblrTruthValue::true_value));
          out.evidence.push_back({"special_form_runtime", "special_in"});
          out.evidence.push_back({"operator_runtime", "op_eq"});
          return out;
        }
        if (eq_truth == SblrTruthValue::unknown) saw_unknown = true;
      }
      api::EngineProjectionFunctionResult out;
      out.ok = true;
      out.value = EngineTypedValueFromSblrValue(
          MakeSblrTruthValue(saw_unknown ? SblrTruthValue::unknown
                                         : SblrTruthValue::false_value));
      out.evidence.push_back({"special_form_runtime", "special_in"});
      out.evidence.push_back({"operator_runtime", "op_eq"});
      return out;
    }
    if (expression.special_form_id == "sb.special.case") {
      if (expression.arguments.size() < 3 || expression.arguments.size() % 2 != 1) {
        return ProjectionOperatorFailure(failure_request,
                                         "SB_DIAG_OPERATOR_INVALID_INPUT",
                                         "CASE projection requires condition/result pairs plus ELSE operand");
      }
      for (std::size_t index = 0; index + 1 < expression.arguments.size(); index += 2) {
        const auto condition =
            EvaluateProjectionOperatorExpression(context, expression.arguments[index]);
        if (!condition.ok) return condition;
        SblrTruthValue truth = SblrTruthValue::unknown;
        if (!TruthFromProjectionValue(condition.value, &truth)) {
          return ProjectionOperatorFailure(failure_request,
                                           "SBLR.DESCRIPTOR_MISMATCH",
                                           "CASE WHEN condition produced a non-boolean intermediate");
        }
        if (truth == SblrTruthValue::true_value) {
          auto out =
              EvaluateProjectionOperatorExpression(context, expression.arguments[index + 1]);
          if (out.ok) {
            out.evidence.insert(out.evidence.end(),
                                condition.evidence.begin(),
                                condition.evidence.end());
            out.evidence.push_back({"special_form_runtime", "special_case"});
          }
          return out;
        }
      }
      auto out = EvaluateProjectionOperatorExpression(context, expression.arguments.back());
      if (out.ok) out.evidence.push_back({"special_form_runtime", "special_case"});
      return out;
    }
    return ProjectionOperatorFailure(failure_request,
                                     "SB_DIAG_OPERATOR_INVALID_INPUT",
                                     "special form projection is not registered for this bounded route");
  }
  if (expression.expression_kind != "operator") {
    api::EngineProjectionFunctionResult out;
    out.ok = false;
    out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        "SB_DIAG_OPERATOR_INVALID_INPUT",
        "engine.operator.invalid_projection_expression",
        "bounded operator projection accepts only literal and operator operands",
        true));
    return out;
  }

  api::EngineProjectionOperatorRequest failure_request;
  failure_request.context = context;
  failure_request.expression = expression;

  if (expression.operator_id == "op_not") {
    if (expression.arguments.size() != 1) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "NOT projection requires one operand");
    }
    const auto operand = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!operand.ok) return operand;
    SblrTruthValue truth = SblrTruthValue::unknown;
    if (!TruthFromProjectionValue(operand.value, &truth)) {
      return ProjectionOperatorFailure(failure_request,
                                       "SBLR.DESCRIPTOR_MISMATCH",
                                       "NOT projection requires a boolean operand");
    }
    api::EngineProjectionFunctionResult out;
    out.ok = true;
    out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(SblrNot(truth)));
    out.evidence.push_back({"operator_runtime", "op_not"});
    return out;
  }

  if (expression.operator_id == "op_is_null") {
    if (expression.arguments.size() != 1) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "IS NULL projection requires one operand");
    }
    const auto operand = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!operand.ok) return operand;
    const auto result = EvaluateSblrComparison(
        "op_is_null",
        ProjectionTypedValueToSblrValue(operand.value),
        SblrValue{},
        ProjectionOperatorContext(context));
    return OperatorResultToProjectionResult("op_is_null", result);
  }

  if (expression.operator_id == "op_and" || expression.operator_id == "op_or" ||
      expression.operator_id == "op_xor") {
    if (expression.arguments.size() != 2) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "logical projection requires two operands");
    }
    const auto left = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!left.ok) return left;
    const auto right = EvaluateProjectionOperatorExpression(context, expression.arguments[1]);
    if (!right.ok) return right;
    SblrTruthValue left_truth = SblrTruthValue::unknown;
    SblrTruthValue right_truth = SblrTruthValue::unknown;
    if (!TruthFromProjectionValue(left.value, &left_truth) ||
        !TruthFromProjectionValue(right.value, &right_truth)) {
      return ProjectionOperatorFailure(failure_request,
                                       "SBLR.DESCRIPTOR_MISMATCH",
                                       "logical projection requires boolean operands");
    }
    SblrTruthValue truth = SblrTruthValue::unknown;
    if (expression.operator_id == "op_and") {
      truth = SblrAnd(left_truth, right_truth);
    } else if (expression.operator_id == "op_or") {
      truth = SblrOr(left_truth, right_truth);
    } else {
      truth = SblrXor(left_truth, right_truth);
    }
    api::EngineProjectionFunctionResult out;
    out.ok = true;
    out.value = EngineTypedValueFromSblrValue(MakeSblrTruthValue(truth));
    out.evidence.push_back({"operator_runtime", expression.operator_id});
    return out;
  }

  if (expression.operator_id == "op_unary_minus") {
    if (expression.arguments.size() != 1) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "unary minus projection requires one operand");
    }
    const auto operand = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!operand.ok) return operand;
    const auto result = EvaluateSblrUnaryArithmetic(
        "op_unary_minus", ProjectionTypedValueToSblrValue(operand.value), ProjectionOperatorContext(context));
    return OperatorResultToProjectionResult("op_unary_minus", result);
  }

  if (expression.operator_id == "op_eq" ||
      expression.operator_id == "op_ne" ||
      expression.operator_id == "op_lt" ||
      expression.operator_id == "op_le" ||
      expression.operator_id == "op_ge" ||
      expression.operator_id == "op_add" ||
      expression.operator_id == "op_sub" ||
      expression.operator_id == "op_mul" ||
      expression.operator_id == "op_div" ||
      expression.operator_id == "op_gt" ||
      expression.operator_id == "op_like" ||
      expression.operator_id == "op_ilike" ||
      expression.operator_id == "op_regex_match" ||
      expression.operator_id == "op_is_distinct" ||
      expression.operator_id == "op_json_get" ||
      expression.operator_id == "op_json_get_text" ||
      expression.operator_id == "op_array_contains") {
    if (expression.arguments.size() != 2) {
      return ProjectionOperatorFailure(failure_request,
                                       "SB_DIAG_OPERATOR_INVALID_INPUT",
                                       "binary operator projection requires two operands");
    }
    const auto left = EvaluateProjectionOperatorExpression(context, expression.arguments[0]);
    if (!left.ok) return left;
    const auto right = EvaluateProjectionOperatorExpression(context, expression.arguments[1]);
    if (!right.ok) return right;
    const SblrValue left_value = ProjectionTypedValueToSblrValue(left.value);
    const SblrValue right_value = ProjectionTypedValueToSblrValue(right.value);
    const auto sblr_context = ProjectionOperatorContext(context);
    if (expression.operator_id == "op_like" ||
        expression.operator_id == "op_ilike" ||
        expression.operator_id == "op_regex_match") {
      const auto result = EvaluateSblrStringOperator(expression.operator_id,
                                                    left_value,
                                                    right_value,
                                                    sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    if (expression.operator_id == "op_eq" ||
        expression.operator_id == "op_ne" ||
        expression.operator_id == "op_lt" ||
        expression.operator_id == "op_le" ||
        expression.operator_id == "op_gt" ||
        expression.operator_id == "op_ge" ||
        expression.operator_id == "op_is_distinct") {
      const auto result = EvaluateSblrComparison(expression.operator_id,
                                                 left_value,
                                                 right_value,
                                                 sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    if (expression.operator_id == "op_mul" || expression.operator_id == "op_div" ||
        expression.operator_id == "op_sub" || expression.operator_id == "op_add") {
      const auto result = EvaluateSblrArithmetic(expression.operator_id,
                                                 left_value,
                                                 right_value,
                                                 sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    if (expression.operator_id == "op_json_get" || expression.operator_id == "op_json_get_text") {
      const auto result = EvaluateSblrDocumentOperator(expression.operator_id,
                                                      left_value,
                                                      right_value,
                                                      sblr_context);
      return OperatorResultToProjectionResult(expression.operator_id, result);
    }
    const auto result = EvaluateSblrCollectionOperator("op_array_contains",
                                                       left_value,
                                                       right_value,
                                                       sblr_context);
    return OperatorResultToProjectionResult("op_array_contains", result);
  }

  return ProjectionOperatorFailure(failure_request,
                                   "SB_DIAG_OPERATOR_INVALID_INPUT",
                                   "operator projection is not registered for this bounded route");
}

api::EngineProjectionFunctionResult EvaluateProjectionOperator(
    const api::EngineProjectionOperatorRequest& request) {
  return EvaluateProjectionOperatorExpression(request.context, request.expression);
}

api::EngineProjectionFunctionResult EvaluateProjectionFunction(
    const api::EngineProjectionFunctionRequest& request) {
  static const auto package = functions::BuildStandardFunctionSeedPackage();
  functions::FunctionCallRequest function_request;
  function_request.context.function_id = request.function_id;
  function_request.context.security_allowed = request.context.security_context_present;
  function_request.context.policy_allowed = request.context.security_context_present;
  function_request.context.dependency_available = true;
  function_request.context.sblr_context.database_path = request.context.database_path;
  function_request.context.sblr_context.database_uuid = request.context.database_uuid.canonical;
  function_request.context.sblr_context.cluster_uuid = request.context.cluster_uuid.canonical;
  function_request.context.sblr_context.node_uuid = request.context.node_uuid.canonical;
  function_request.context.sblr_context.transaction_uuid = request.context.transaction_uuid.canonical;
  function_request.context.sblr_context.local_transaction_id = request.context.local_transaction_id;
  function_request.context.sblr_context.snapshot_visible_through_local_transaction_id =
      request.context.snapshot_visible_through_local_transaction_id;
  function_request.context.sblr_context.transaction_isolation_level =
      request.context.transaction_isolation_level;
  function_request.context.sblr_context.statement_uuid = request.context.statement_uuid.canonical;
  function_request.context.sblr_context.session_uuid = request.context.session_uuid.canonical;
  function_request.context.sblr_context.user_uuid = request.context.principal_uuid.canonical;
  function_request.context.sblr_context.current_schema_uuid =
      request.context.current_schema_uuid.canonical;
  function_request.context.sblr_context.current_role_uuid =
      request.context.current_role_uuid.canonical;
  function_request.context.sblr_context.statement_timestamp = request.context.statement_timestamp;
  function_request.context.sblr_context.transaction_timestamp =
      request.context.transaction_timestamp;
  function_request.context.sblr_context.current_timestamp = request.context.current_timestamp;
  function_request.context.sblr_context.current_monotonic_ns = request.context.current_monotonic_ns;
  function_request.context.sblr_context.deterministic_random_u64 =
      request.context.deterministic_random_u64;
  function_request.context.sblr_context.deterministic_random_u64_present =
      request.context.deterministic_random_u64_present;
  function_request.context.sblr_context.deterministic_random_bytes_hex =
      request.context.deterministic_random_bytes_hex;
  function_request.context.sblr_context.deterministic_uuid_text =
      request.context.deterministic_uuid_text;
  function_request.context.sblr_context.security_context_present =
      request.context.security_context_present;
  function_request.context.sblr_context.current_sqlstate = request.context.current_sqlstate;
  function_request.context.sblr_context.current_diagnostic_uuid =
      request.context.current_diagnostic_uuid.canonical;
  function_request.context.sblr_context.client_protocol_uuid = request.context.client_protocol_uuid;
  function_request.context.sblr_context.application_name = request.context.application_name;
  function_request.context.sblr_context.read_only_mode = request.context.read_only_mode;
  function_request.context.sblr_context.last_row_count = request.context.last_row_count;
  function_request.context.sblr_context.last_row_count_present =
      request.context.last_row_count_present;
  function_request.context.sblr_context.transaction_context_present =
      request.context.local_transaction_id != 0 ||
      !request.context.transaction_uuid.canonical.empty();
  function_request.context.sblr_context.cluster_authority_available =
      request.context.cluster_authority_available;
  function_request.context.sblr_context.active_savepoint_names =
      ActiveSavepointNamesForContext(request.context);
  for (std::size_t index = 0; index < request.arguments.size(); ++index) {
    function_request.arguments.push_back(functions::FunctionArgument{
        request.arguments[index].name.empty() ? "arg" + std::to_string(index)
                                              : request.arguments[index].name,
        SblrValueFromProjectionArgument(request.arguments[index])});
  }

  api::EngineProjectionFunctionResult out;
  const auto function_result =
      functions::DispatchFunctionCall(package.registry, std::move(function_request)).result;
  out.ok = function_result.ok() && function_result.scalar_values.size() == 1;
  if (out.ok) {
    out.value = EngineTypedValueFromSblrValue(function_result.scalar_values.front());
    out.evidence.push_back({"function_runtime", request.function_id});
    return out;
  }
  if (function_result.diagnostics.empty()) {
    out.diagnostics.push_back(api::MakeEngineApiDiagnostic(
        "SB_DIAG_FUNCTION_RESULT_SHAPE_INVALID",
        "engine.function.result_shape_invalid",
        "function projection expected exactly one scalar value",
        true));
  } else {
    for (const auto& diagnostic : function_result.diagnostics) {
      out.diagnostics.push_back(FunctionDiagnosticToApi(diagnostic));
    }
  }
  return out;
}

api::EngineEvaluateProjectionRequest TypedEvaluateProjectionRequest(
    const SblrDispatchRequest& request) {
  api::EngineEvaluateProjectionRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.function_evaluator = EvaluateProjectionFunction;
  typed.operator_evaluator = EvaluateProjectionOperator;
  return typed;
}

api::EngineUpdateRowsRequest TypedUpdateRowsRequest(const SblrDispatchRequest& request) {
  api::EngineUpdateRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.update_predicate = base.predicate;
  typed.assignments = base.assignments;
  if (typed.assignments.empty()) {
    const std::string assignment_plan = api::SecurityOptionValue(base, "assignment_plan:");
    std::string item;
    std::istringstream items(assignment_plan);
    while (std::getline(items, item, ';')) {
      const auto separator = item.find('|');
      if (separator == std::string::npos || separator == 0) { continue; }
      api::EngineTypedValue placeholder;
      placeholder.descriptor.descriptor_kind = "scalar";
      placeholder.descriptor.canonical_type_name = "text";
      placeholder.descriptor.encoded_descriptor = "type=text";
      typed.assignments.push_back({item.substr(0, separator), std::move(placeholder)});
    }
  }
  return typed;
}

api::EngineDeleteRowsRequest TypedDeleteRowsRequest(const SblrDispatchRequest& request) {
  api::EngineDeleteRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.delete_predicate = base.predicate;
  return typed;
}

api::EngineMergeRowsRequest TypedMergeRowsRequest(const SblrDispatchRequest& request) {
  api::EngineMergeRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.match_predicate = base.predicate;
  typed.input_rows = base.rows;
  typed.update_assignments = base.assignments;
  typed.update_when_matched = api::SecurityOptionBool(base, "update_when_matched:", true);
  typed.insert_when_not_matched = api::SecurityOptionBool(base, "insert_when_not_matched:", true);
  return typed;
}

api::EngineApplyNumericOperationRequest TypedApplyNumericOperationRequest(
    const SblrDispatchRequest& request) {
  api::EngineApplyNumericOperationRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.numeric_operation = api::SecurityOptionValue(base, "numeric_operation:");
  if (typed.numeric_operation.empty()) {
    typed.numeric_operation = api::SecurityOptionValue(base, "operation_kind:");
  }
  typed.rounding_mode = api::SecurityOptionValue(base, "rounding_mode:");
  const auto precision = DispatchOptionU64(base, "precision:");
  if (precision != 0) {
    typed.precision = static_cast<std::uint32_t>(precision);
  }
  typed.scale = static_cast<std::uint32_t>(DispatchOptionU64(base, "scale:"));
  typed.allow_special_values = api::SecurityOptionBool(base, "allow_special_values:", false);
  typed.left_value = DispatchTypedValueOption(base, "left_", "decimal");
  typed.right_value = DispatchTypedValueOption(base, "right_", "decimal");
  return typed;
}

api::EngineEvaluateAdvancedDatatypeFamilyRequest TypedEvaluateAdvancedDatatypeFamilyRequest(
    const SblrDispatchRequest& request) {
  api::EngineEvaluateAdvancedDatatypeFamilyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  if (!base.descriptors.empty()) {
    typed.descriptor = base.descriptors.front();
  } else {
    typed.descriptor.descriptor_kind = "scalar";
    typed.descriptor.canonical_type_name = api::SecurityOptionValue(base, "descriptor_type:");
    typed.descriptor.encoded_descriptor = api::SecurityOptionValue(base, "descriptor:");
    if (typed.descriptor.encoded_descriptor.empty() &&
        !typed.descriptor.canonical_type_name.empty()) {
      typed.descriptor.encoded_descriptor = "type=" + typed.descriptor.canonical_type_name;
    }
  }
  typed.operation_kind = api::SecurityOptionValue(base, "operation_kind:");
  if (typed.operation_kind.empty()) {
    typed.operation_kind = api::SecurityOptionValue(base, "advanced_operation:");
  }
  typed.index_kind = api::SecurityOptionValue(base, "index_kind:");
  if (typed.index_kind.empty()) {
    typed.index_kind = api::SecurityOptionValue(base, "advanced_index:");
  }
  typed.descriptor_profile = api::SecurityOptionValue(base, "descriptor_profile:");
  typed.vector_dimension = static_cast<std::uint32_t>(DispatchOptionU64(base, "vector_dimension:"));
  return typed;
}

void ApplyImportRejectPolicyOptions(const api::EngineApiRequest& base,
                                    api::EngineImportRejectPolicyEnvelope* policy) {
  const std::string reject_mode = api::SecurityOptionValue(base, "reject_mode:");
  if (!reject_mode.empty()) policy->reject_mode = reject_mode;
  policy->reject_limit_rows = DispatchOptionU64(base, "reject_limit_rows:");
  policy->reject_limit_percent = DispatchOptionDouble(base, "reject_limit_percent:");
  const std::string reject_payload_policy =
      api::SecurityOptionValue(base, "reject_payload_policy:");
  if (!reject_payload_policy.empty()) policy->reject_payload_policy = reject_payload_policy;
  const std::string resume_policy = api::SecurityOptionValue(base, "resume_policy:");
  if (!resume_policy.empty()) policy->resume_policy = resume_policy;
  policy->reject_target.uuid.canonical = api::SecurityOptionValue(base, "reject_target_uuid:");
  policy->reject_target.object_kind = api::SecurityOptionValue(base, "reject_target_kind:");
  if (!policy->reject_target.uuid.canonical.empty() && policy->reject_target.object_kind.empty()) {
    policy->reject_target.object_kind = "table";
  }
}

void ApplyImportCheckpointPolicyOptions(const api::EngineApiRequest& base,
                                        api::EngineImportCheckpointPolicyEnvelope* policy) {
  const std::string checkpoint_mode = api::SecurityOptionValue(base, "checkpoint_mode:");
  if (!checkpoint_mode.empty()) policy->checkpoint_mode = checkpoint_mode;
  policy->checkpoint_interval_rows = DispatchOptionU64(base, "checkpoint_interval_rows:");
  policy->checkpoint_interval_bytes = DispatchOptionU64(base, "checkpoint_interval_bytes:");
  policy->checkpoint_interval_millis = DispatchOptionU64(base, "checkpoint_interval_millis:");
  const std::string checkpoint_resume_policy =
      api::SecurityOptionValue(base, "checkpoint_resume_policy:");
  if (!checkpoint_resume_policy.empty()) policy->resume_policy = checkpoint_resume_policy;
  const std::string replay_policy = api::SecurityOptionValue(base, "replay_policy:");
  if (!replay_policy.empty()) policy->replay_policy = replay_policy;
  const std::string failure_action = api::SecurityOptionValue(base, "failure_action:");
  if (!failure_action.empty()) policy->failure_action = failure_action;
  policy->checkpoint_target.uuid.canonical = api::SecurityOptionValue(base, "checkpoint_target_uuid:");
  policy->checkpoint_target.object_kind = api::SecurityOptionValue(base, "checkpoint_target_kind:");
  if (!policy->checkpoint_target.uuid.canonical.empty() &&
      policy->checkpoint_target.object_kind.empty()) {
    policy->checkpoint_target.object_kind = "table";
  }
  policy->require_source_fingerprint =
      api::SecurityOptionBool(base, "require_source_fingerprint:", policy->require_source_fingerprint);
  policy->require_source_position =
      api::SecurityOptionBool(base, "require_source_position:", policy->require_source_position);
}

api::EnginePlanImportRowsRequest TypedPlanImportRowsRequest(const SblrDispatchRequest& request) {
  api::EnginePlanImportRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.source.source_kind = api::SecurityOptionValue(base, "source_kind:");
  if (typed.source.source_kind.empty()) typed.source.source_kind = "native_sbsql_import";
  typed.format.format_family = api::SecurityOptionValue(base, "format_family:");
  if (typed.format.format_family.empty()) typed.format.format_family = "csv";
  typed.import_policy.strict_bulk_load_requested =
      api::SecurityOptionBool(base, "strict_bulk_load_requested:", false);
  typed.import_policy.reference_relaxed_semantics_requested =
      api::SecurityOptionBool(base, "reference_relaxed_semantics_requested:", false);
  ApplyImportRejectPolicyOptions(base, &typed.import_policy);
  return typed;
}

api::EngineNormalizeImportRejectModelRequest TypedNormalizeImportRejectModelRequest(
    const SblrDispatchRequest& request) {
  api::EngineNormalizeImportRejectModelRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  ApplyImportRejectPolicyOptions(base, &typed.reject_policy);
  typed.include_payload_reference_columns =
      api::SecurityOptionBool(base, "include_payload_reference_columns:", false);
  return typed;
}

api::EngineNormalizeImportCheckpointRequest TypedNormalizeImportCheckpointRequest(
    const SblrDispatchRequest& request) {
  api::EngineNormalizeImportCheckpointRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  ApplyImportCheckpointPolicyOptions(base, &typed.checkpoint_policy);
  typed.source_fingerprint = api::SecurityOptionValue(base, "source_fingerprint:");
  typed.source_position = api::SecurityOptionValue(base, "source_position:");
  return typed;
}

api::EngineExecuteImportRowsRequest TypedExecuteImportRowsRequest(
    const SblrDispatchRequest& request) {
  api::EngineExecuteImportRowsRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.source.source_kind = api::SecurityOptionValue(base, "source_kind:");
  if (typed.source.source_kind.empty()) typed.source.source_kind = "native_sbsql_import";
  typed.source.source_uuid.canonical = api::SecurityOptionValue(base, "source_uuid:");
  typed.source.source_fingerprint = api::SecurityOptionValue(base, "source_fingerprint:");
  typed.source.source_position = api::SecurityOptionValue(base, "source_position:");
  typed.source.redacted_source_handle = api::SecurityOptionValue(base, "redacted_source_handle:");
  typed.source.source_handle_sensitive =
      api::SecurityOptionBool(base, "source_handle_sensitive:", true);
  typed.format.format_family = api::SecurityOptionValue(base, "format_family:");
  if (typed.format.format_family.empty()) typed.format.format_family = "csv";
  typed.format.encoding = api::SecurityOptionValue(base, "encoding:");
  typed.format.line_ending = api::SecurityOptionValue(base, "line_ending:");
  typed.format.delimiter = api::SecurityOptionValue(base, "delimiter:");
  typed.format.quote = api::SecurityOptionValue(base, "quote:");
  typed.format.escape = api::SecurityOptionValue(base, "escape:");
  typed.format.header_policy = api::SecurityOptionValue(base, "header_policy:");
  typed.import_policy.strict_bulk_load_requested =
      api::SecurityOptionBool(base, "strict_bulk_load_requested:", false);
  typed.import_policy.reference_relaxed_semantics_requested =
      api::SecurityOptionBool(base, "reference_relaxed_semantics_requested:", false);
  ApplyImportRejectPolicyOptions(base, &typed.import_policy);
  ApplyImportCheckpointPolicyOptions(base, &typed.checkpoint_policy);
  typed.canonical_rows = base.rows;
  typed.estimated_row_count = DispatchOptionU64(base, "estimated_row_count:");
  const std::string duplicate_mode = api::SecurityOptionValue(base, "duplicate_mode:");
  if (!duplicate_mode.empty()) { typed.duplicate_mode = duplicate_mode; }
  typed.require_generated_row_uuid =
      api::SecurityOptionBool(base, "require_generated_row_uuid:", true);
  return typed;
}

api::EngineExecuteNativeBulkIngestRequest TypedExecuteNativeBulkIngestRequest(
    const SblrDispatchRequest& request) {
  api::EngineExecuteNativeBulkIngestRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.target_table = TargetObjectForDml(base, "table");
  typed.canonical_rows = base.rows;
  typed.estimated_row_count = DispatchOptionU64(base, "estimated_row_count:");
  const std::string duplicate_mode = api::SecurityOptionValue(base, "duplicate_mode:");
  if (!duplicate_mode.empty()) { typed.duplicate_mode = duplicate_mode; }
  typed.require_generated_row_uuid =
      api::SecurityOptionBool(base, "require_generated_row_uuid:", true);
  typed.native_bulk_ingest_enabled =
      api::SecurityOptionBool(base, "native_bulk_ingest_enabled:", true);
  typed.import_policy.reject_mode = api::SecurityOptionValue(base, "reject_mode:");
  if (typed.import_policy.reject_mode.empty()) {
    typed.import_policy.reject_mode = "fail_fast";
  }
  typed.import_policy.reject_payload_policy =
      api::SecurityOptionValue(base, "reject_payload_policy:");
  if (typed.import_policy.reject_payload_policy.empty()) {
    typed.import_policy.reject_payload_policy = "diagnostic_only";
  }
  typed.import_policy.resume_policy = api::SecurityOptionValue(base, "resume_policy:");
  if (typed.import_policy.resume_policy.empty()) {
    typed.import_policy.resume_policy = "fail_closed";
  }
  ApplyImportRejectPolicyOptions(base, &typed.import_policy);
  ApplyImportCheckpointPolicyOptions(base, &typed.checkpoint_policy);
  return typed;
}

api::EngineSecurityGrantPrivilegeRequest TypedSecurityGrantPrivilegeRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityGrantPrivilegeRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.grant_uuid = api::SecurityOptionValue(base, "grant_uuid:");
  typed.grantee_uuid = api::SecurityOptionValue(base, "grantee_uuid:");
  typed.grantee_kind = api::SecurityOptionValue(base, "grantee_kind:");
  if (typed.grantee_kind.empty()) typed.grantee_kind = "principal";
  typed.target_object_uuid = !base.target_object.uuid.canonical.empty()
                                 ? base.target_object.uuid.canonical
                                 : api::SecurityOptionValue(base, "target_object_uuid:");
  typed.target_object_kind = !base.target_object.object_kind.empty()
                                 ? base.target_object.object_kind
                                 : api::SecurityOptionValue(base, "target_object_kind:");
  typed.privilege = api::SecurityOptionValue(base, "privilege:");
  typed.grant_effect = api::SecurityOptionValue(base, "grant_effect:");
  if (typed.grant_effect.empty()) typed.grant_effect = "allow";
  return typed;
}

api::EngineSecurityGrantMembershipRequest TypedSecurityGrantMembershipRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityGrantMembershipRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.membership_uuid = api::SecurityOptionValue(base, "membership_uuid:");
  typed.member_principal_uuid = api::SecurityOptionValue(base, "member_principal_uuid:");
  typed.container_uuid = api::SecurityOptionValue(base, "container_uuid:");
  typed.container_kind = api::SecurityOptionValue(base, "container_kind:");
  return typed;
}

api::EngineSecurityRevokeMembershipRequest TypedSecurityRevokeMembershipRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityRevokeMembershipRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.member_principal_uuid = api::SecurityOptionValue(base, "member_principal_uuid:");
  typed.container_uuid = api::SecurityOptionValue(base, "container_uuid:");
  typed.container_kind = api::SecurityOptionValue(base, "container_kind:");
  return typed;
}

api::EngineSecurityRevokePrivilegeRequest TypedSecurityRevokePrivilegeRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityRevokePrivilegeRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.grantee_uuid = api::SecurityOptionValue(base, "grantee_uuid:");
  typed.target_object_uuid = !base.target_object.uuid.canonical.empty()
                                 ? base.target_object.uuid.canonical
                                 : api::SecurityOptionValue(base, "target_object_uuid:");
  typed.privilege = api::SecurityOptionValue(base, "privilege:");
  return typed;
}

api::EngineSecurityCreateRoleRequest TypedSecurityCreateRoleRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreateRoleRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.role_uuid = api::SecurityOptionValue(base, "role_uuid:");
  if (typed.role_uuid.empty()) { typed.role_uuid = api::SecurityOptionValue(base, "principal_uuid:"); }
  if (typed.role_uuid.empty()) { typed.role_uuid = base.target_object.uuid.canonical; }
  typed.role_name = api::SecurityOptionValue(base, "role_name:");
  if (typed.role_name.empty()) { typed.role_name = api::SecurityOptionValue(base, "principal_name:"); }
  return typed;
}

api::EngineSecurityCreateGroupRequest TypedSecurityCreateGroupRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreateGroupRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.group_uuid = api::SecurityOptionValue(base, "group_uuid:");
  if (typed.group_uuid.empty()) { typed.group_uuid = api::SecurityOptionValue(base, "principal_uuid:"); }
  if (typed.group_uuid.empty()) { typed.group_uuid = base.target_object.uuid.canonical; }
  typed.group_name = api::SecurityOptionValue(base, "group_name:");
  if (typed.group_name.empty()) { typed.group_name = api::SecurityOptionValue(base, "principal_name:"); }
  typed.external_authority_ref = api::SecurityOptionValue(base, "external_authority_ref:");
  if (typed.external_authority_ref.empty()) {
    typed.external_authority_ref =
        api::SecurityOptionValue(base, "credential_protected_material_ref:");
  }
  return typed;
}

api::EngineSecurityCreatePrincipalRequest TypedSecurityCreatePrincipalRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreatePrincipalRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.principal_uuid = !base.target_object.uuid.canonical.empty()
                             ? base.target_object.uuid.canonical
                             : api::SecurityOptionValue(base, "principal_uuid:");
  typed.principal_name = api::SecurityOptionValue(base, "principal_name:");
  typed.principal_kind = api::SecurityOptionValue(base, "principal_kind:");
  if (typed.principal_kind.empty()) typed.principal_kind = "user";
  typed.credential_protected_material_ref =
      api::SecurityOptionValue(base, "credential_protected_material_ref:");
  typed.credential_fingerprint = api::SecurityOptionValue(base, "credential_fingerprint:");
  return typed;
}

api::EngineSecurityAlterPrincipalRequest TypedSecurityAlterPrincipalRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityAlterPrincipalRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.principal_uuid = !base.target_object.uuid.canonical.empty()
                             ? base.target_object.uuid.canonical
                             : api::SecurityOptionValue(base, "principal_uuid:");
  typed.principal_name = api::SecurityOptionValue(base, "principal_name:");
  typed.principal_kind = api::SecurityOptionValue(base, "principal_kind:");
  typed.lifecycle_state = api::SecurityOptionValue(base, "lifecycle_state:");
  typed.credential_protected_material_ref =
      api::SecurityOptionValue(base, "credential_protected_material_ref:");
  typed.credential_fingerprint = api::SecurityOptionValue(base, "credential_fingerprint:");
  return typed;
}

api::EngineSecuritySetRoleRequest TypedSecuritySetRoleRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecuritySetRoleRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.role_uuid = api::SecurityOptionValue(base, "role_uuid:");
  typed.role_mode = api::SecurityOptionValue(base, "role_mode:");
  if (typed.role_mode.empty()) typed.role_mode = "explicit";
  return typed;
}

api::EngineSecurityCreatePolicyRequest TypedSecurityCreatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityCreatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = api::SecurityOptionValue(base, "policy_uuid:");
  if (typed.policy_uuid.empty()) {
    typed.policy_uuid = base.target_object.uuid.canonical;
  }
  typed.target_object_uuid = api::SecurityOptionValue(base, "target_object_uuid:");
  if (typed.target_object_uuid.empty()) { typed.target_object_uuid = base.target_schema.uuid.canonical; }
  typed.target_object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  if (typed.target_object_kind.empty()) { typed.target_object_kind = "object"; }
  typed.policy_effect = api::SecurityOptionValue(base, "policy_effect:");
  if (typed.policy_effect.empty()) { typed.policy_effect = "row_filter"; }
  typed.predicate_envelope = api::SecurityOptionValue(base, "predicate_envelope:");
  typed.definer_principal_uuid = api::SecurityOptionValue(base, "definer_principal_uuid:");
  return typed;
}

api::EngineSecurityAlterPolicyRequest TypedSecurityAlterPolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityAlterPolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  typed.target_object_uuid = api::SecurityOptionValue(base, "target_object_uuid:");
  typed.target_object_kind = api::SecurityOptionValue(base, "target_object_kind:");
  typed.policy_effect = api::SecurityOptionValue(base, "policy_effect:");
  typed.predicate_envelope = api::SecurityOptionValue(base, "predicate_envelope:");
  typed.definer_principal_uuid = api::SecurityOptionValue(base, "definer_principal_uuid:");
  typed.lifecycle_state = api::SecurityOptionValue(base, "lifecycle_state:");
  return typed;
}

api::EngineSecurityAttachPolicyRequest TypedSecurityAttachPolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityAttachPolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = api::SecurityOptionValue(base, "policy_uuid:");
  typed.target_object_uuid = !base.target_object.uuid.canonical.empty()
                                 ? base.target_object.uuid.canonical
                                 : api::SecurityOptionValue(base, "target_object_uuid:");
  typed.target_object_kind = !base.target_object.object_kind.empty()
                                 ? base.target_object.object_kind
                                 : api::SecurityOptionValue(base, "target_object_kind:");
  if (typed.target_object_kind.empty()) typed.target_object_kind = "object";
  typed.policy_scope = api::SecurityOptionValue(base, "policy_scope:");
  if (typed.policy_scope.empty()) typed.policy_scope = typed.target_object_kind;
  typed.policy_effect = api::SecurityOptionValue(base, "policy_effect:");
  if (typed.policy_effect.empty()) typed.policy_effect = "attach";
  typed.predicate_envelope = api::SecurityOptionValue(base, "predicate_envelope:");
  typed.definer_principal_uuid = api::SecurityOptionValue(base, "definer_principal_uuid:");
  return typed;
}

api::EngineSecurityActivatePolicyRequest TypedSecurityActivatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityActivatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  return typed;
}

api::EngineSecurityDeactivatePolicyRequest TypedSecurityDeactivatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityDeactivatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  return typed;
}

api::EngineSecurityValidatePolicyRequest TypedSecurityValidatePolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityValidatePolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  typed.observed_policy_generation =
      DispatchOptionU64(base, "observed_policy_generation:");
  typed.observed_cache_invalidation_epoch =
      DispatchOptionU64(base, "observed_cache_invalidation_epoch:");
  return typed;
}

api::EngineSecurityShowPolicyRequest TypedSecurityShowPolicyRequest(
    const SblrDispatchRequest& request) {
  api::EngineSecurityShowPolicyRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.policy_uuid = !base.target_object.uuid.canonical.empty()
                          ? base.target_object.uuid.canonical
                          : api::SecurityOptionValue(base, "policy_uuid:");
  typed.include_rows = api::SecurityOptionBool(base, "include_rows:", true);
  return typed;
}

template <typename TRequest>
TRequest TypedAgentHookRequest(const SblrDispatchRequest& request,
                               const std::string& default_agent_type,
                               const std::string& default_action) {
  TRequest typed;
  const api::EngineApiRequest base = BaseApiRequest(request);
  static_cast<api::EngineApiRequest&>(typed) = base;
  typed.agent_type = api::SecurityOptionValue(base, "agent_type:");
  if (typed.agent_type.empty()) { typed.agent_type = default_agent_type; }
  typed.action_class = api::SecurityOptionValue(base, "action_class:");
  if (typed.action_class.empty()) { typed.action_class = default_action; }
  typed.agent_uuid.canonical = api::SecurityOptionValue(base, "agent_uuid:");
  if (typed.agent_uuid.canonical.empty()) { typed.agent_uuid.canonical = "agent:local:" + typed.agent_type; }
  typed.policy_snapshot_uuid.canonical = api::SecurityOptionValue(base, "policy_snapshot_uuid:");
  if (typed.policy_snapshot_uuid.canonical.empty()) {
    typed.policy_snapshot_uuid.canonical = "policy:" + typed.agent_type + ":baseline";
  }
  typed.target_filespace = DispatchTargetOfKind(base, "filespace");
  typed.target_index = DispatchTargetOfKind(base, "index");
  typed.page_family = api::SecurityOptionValue(base, "page_family:");
  typed.page_type = api::SecurityOptionValue(base, "page_type:");
  typed.safety_fence_result = api::SecurityOptionValue(base, "safety_fence_result:");
  typed.cooldown_key = api::SecurityOptionValue(base, "cooldown_key:");
  typed.requested_pages = DispatchOptionU64(base, "requested_pages:");
  typed.requested_bytes = DispatchOptionU64(base, "requested_bytes:");
  typed.policy_authorized = api::SecurityOptionBool(base, "policy_authorized:", false);
  typed.evidence_sink_available = api::SecurityOptionBool(base, "evidence_sink_available:", false);
  typed.metrics_fresh = api::SecurityOptionBool(base, "metrics_fresh:", false);
  typed.cooldown_active = api::SecurityOptionBool(base, "cooldown_active:", false);
  typed.manual_override_active = api::SecurityOptionBool(base, "manual_override_active:", false);
  typed.lifecycle_fence_active = api::SecurityOptionBool(base, "lifecycle_fence_active:", false);
  typed.dry_run = api::SecurityOptionBool(base, "dry_run:", false);
  typed.shadow_build = api::SecurityOptionBool(base, "shadow_build:", false);
  return typed;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

}  // namespace

bool IsClusterOperationId(std::string_view operation_id) {
  return operation_id.starts_with("cluster.") || operation_id.starts_with("replication.") ||
         operation_id.starts_with("op.cluster.") ||
         operation_id.starts_with("op.show.cluster.") ||
         operation_id == "op.show.cluster_gpu_placement" ||
         operation_id.starts_with("placement.cluster.");
}

// SEARCH_KEY: IsAgentCommandSurfaceOperationId
bool IsAgentCommandSurfaceOperationId(std::string_view operation_id) {
  return operation_id == "agents.metrics.get" ||
         operation_id == "agents.policy.get" ||
         operation_id == "agents.evidence.list" ||
         operation_id == "agents.audit.list" ||
         operation_id == "agents.actions.list" ||
         operation_id == "agents.overrides.list" ||
         operation_id == "agents.drain" ||
         operation_id == "agents.restart" ||
         operation_id == "agents.enable" ||
         operation_id == "agents.disable" ||
         operation_id == "agents.quarantine" ||
         operation_id == "agents.unquarantine" ||
         operation_id == "agents.policy.attach" ||
         operation_id == "agents.policy.detach" ||
         operation_id == "agents.policy.validate" ||
         operation_id == "agents.policy.simulate" ||
         operation_id == "agents.policy.apply" ||
         operation_id == "agents.policy.rollback" ||
         operation_id == "agents.action.approve" ||
         operation_id == "agents.action.cancel" ||
         operation_id == "agents.action.retry" ||
         operation_id == "agents.action.suppress" ||
         operation_id == "agents.override.create" ||
         operation_id == "agents.override.update" ||
         operation_id == "agents.override.drop" ||
         operation_id == "agents.set_mode" ||
         operation_id == "filespaces.show" ||
         operation_id == "filespaces.health.show" ||
         operation_id == "filespaces.capacity.show" ||
         operation_id == "pages.allocation.show" ||
         operation_id == "pages.allocation.family.show" ||
         operation_id == "pages.relocation_backlog.show" ||
         operation_id == "filespaces.shrink_readiness.show" ||
         operation_id == "cluster.agent.list" ||
         operation_id == "cluster.agent.get" ||
         operation_id == "cluster.agent.control";
}

bool IsAgentClusterManagementOperationId(std::string_view operation_id) {
  return operation_id == "cluster.sys.agents" ||
         operation_id == "cluster.agent.list" ||
         operation_id == "cluster.agent.get" ||
         operation_id == "cluster.agent.control";
}

SblrDispatchResult DispatchSblrOperation(const SblrDispatchRequest& request) {
  SblrDispatchResult result;
  result.api_result.operation_id = request.envelope.operation_id;

  const auto validation = ValidateSblrEnvelope(request.envelope);
  result.envelope_validated = validation.ok;
  if (!validation.ok) {
    result.diagnostics = validation.diagnostics;
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_ENVELOPE_REJECTED",
                                      "engine.sblr.dispatch.envelope_rejected",
                                      "SBLR envelope failed engine validation");
    return result;
  }

  if (const char* expected_opcode = ExpectedOpcodeForOperation(request.envelope.operation_id);
      expected_opcode != nullptr && request.envelope.opcode != expected_opcode) {
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_OPCODE_MISMATCH",
                                                   "SBLR opcode does not match operation_id"));
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_OPCODE_MISMATCH",
                                      "engine.sblr.dispatch.opcode_mismatch",
                                      std::string("expected=") + expected_opcode + "; actual=" + request.envelope.opcode);
    return result;
  }

  if (request.envelope.requires_security_context && !request.context.security_context_present) {
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED",
                                                   "SBLR operation requires engine security context"));
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED",
                                      "engine.sblr.dispatch.security_context_required",
                                      "security_context_present=false");
    return result;
  }

  if (request.envelope.requires_transaction_context &&
      request.context.local_transaction_id == 0 &&
      request.context.transaction_uuid.canonical.empty()) {
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED",
                                                   "SBLR operation requires engine transaction context"));
    result.api_result = FailureResult(request.context,
                                      request.envelope.operation_id,
                                      "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED",
                                      "engine.sblr.dispatch.transaction_context_required",
                                      "transaction_uuid and local_transaction_id are both absent");
    return result;
  }

  if ((request.envelope.requires_cluster_authority ||
       (IsClusterOperationId(request.envelope.operation_id) &&
        request.envelope.operation_id != "cluster.profile_operation")) &&
      !IsAgentClusterManagementOperationId(request.envelope.operation_id)) {
    result.accepted = true;
    result.dispatched_to_api = true;
    cluster_provider::ClusterProviderRequest cluster_request;
    cluster_request.context = request.context;
    cluster_request.envelope = request.envelope;
    cluster_request.api_request = BaseApiRequest(request);
    if (request.envelope.operation_id == cluster_provider::kClusterProviderInfoOperationId) {
      result.api_result = cluster_provider::InspectClusterProvider(cluster_request);
    } else {
      result.api_result = cluster_provider::ExecuteClusterOperation(cluster_request);
    }
    PropagateClusterApiDiagnostics(&result);
    return result;
  }

  result.accepted = true;
  result.dispatched_to_api = true;
  const std::string& op = request.envelope.operation_id;

  if (IsGpuAccelerationControlOperation(op)) result.api_result = api::EngineControlGpuAcceleration(TypedRequest<api::EngineControlGpuAccelerationRequest>(request));
  else if (IsGpuAccelerationInspectOperation(op)) result.api_result = api::EngineInspectGpuAcceleration(TypedRequest<api::EngineInspectGpuAccelerationRequest>(request));
  else if (IsNativeCompileControlOperation(op)) result.api_result = api::EngineControlNativeCompile(TypedRequest<api::EngineControlNativeCompileRequest>(request));
  else if (IsNativeCompileInspectOperation(op)) result.api_result = api::EngineInspectNativeCompile(TypedRequest<api::EngineInspectNativeCompileRequest>(request));
  else if (IsManagementRuntimeControlOperation(op)) result.api_result = api::EngineControlManagementRuntime(TypedRequest<api::EngineControlManagementRuntimeRequest>(request));
  else if (IsManagementRuntimeInspectOperation(op)) result.api_result = api::EngineInspectManagementRuntime(TypedRequest<api::EngineInspectManagementRuntimeRequest>(request));
  else if (IsMemoryManagementOperation(op)) result.api_result = api::EnginePlanMemoryManagementOperation(TypedMemoryManagementRequest(request));
  else if (IsStorageTierMigrationOperation(op)) result.api_result = api::EnginePlanStorageTierMigrationOperation(TypedStorageTierMigrationRequest(request));
  else if (IsFilespaceDiscoveryOperation(op)) result.api_result = api::EngineDiscoverFilespaceAnomalies(TypedFilespaceDiscoveryRequest(request));
  else if (IsFilespacePackageOperation(op)) result.api_result = api::EngineFilespacePackageOperation(TypedFilespacePackageRequest(request));
  else if (IsShardPlacementDescriptorOperation(op)) result.api_result = api::EnginePlanShardPlacementOperation(TypedShardPlacementDescriptorRequest(request));
  else if (IsMigrationControlOperation(op)) {
    if (op == "op.migration.begin_from_reference") {
      result.api_result = api::EngineBeginMigration(TypedRequest<api::EngineBeginMigrationRequest>(request));
    } else {
      result.api_result = api::EngineAlterMigration(TypedRequest<api::EngineAlterMigrationRequest>(request));
    }
  }
  else if (IsMigrationInspectOperation(op)) {
    if (op == "op.show.migration") {
      result.api_result = api::EngineShowMigration(TypedRequest<api::EngineShowMigrationRequest>(request));
    } else {
      result.api_result = api::EngineShowMigrations(TypedRequest<api::EngineShowMigrationsRequest>(request));
    }
  }
  else if (IsSecurityInspectionOperation(op)) result.api_result = api::EngineSecurityInspectOperation(TypedRequest<api::EngineSecurityInspectOperationRequest>(request));
  else if (IsObservabilityExactShowOperation(op)) result.api_result = api::EngineInspectShowOperation(TypedRequest<api::EngineInspectShowOperationRequest>(request));
  else if (op == "observability.show_version") result.api_result = api::EngineShowVersion(TypedRequest<api::EngineShowVersionRequest>(request));
  else if (op == "observability.show_database") result.api_result = api::EngineShowDatabase(TypedRequest<api::EngineShowDatabaseRequest>(request));
  else if (op == "observability.show_system") result.api_result = api::EngineShowSystem(TypedRequest<api::EngineShowSystemRequest>(request));
  else if (op == "observability.show_catalog") result.api_result = api::EngineShowCatalog(TypedRequest<api::EngineShowCatalogRequest>(request));
  else if (op == "observability.show_sessions") result.api_result = api::EngineShowSessions(TypedRequest<api::EngineShowSessionsRequest>(request));
  else if (op == "observability.show_transactions") result.api_result = api::EngineShowTransactions(TypedRequest<api::EngineShowTransactionsRequest>(request));
  else if (op == "observability.show_locks") result.api_result = api::EngineShowLocks(TypedRequest<api::EngineShowLocksRequest>(request));
  else if (op == "observability.show_statements") result.api_result = api::EngineShowStatements(TypedRequest<api::EngineShowStatementsRequest>(request));
  else if (op == "observability.show_jobs") result.api_result = api::EngineShowJobs(TypedRequest<api::EngineShowJobsRequest>(request));
  else if (op == "observability.show_management") result.api_result = api::EngineShowManagement(TypedRequest<api::EngineShowManagementRequest>(request));
  else if (op == "observability.show_diagnostics") result.api_result = api::EngineShowDiagnostics(TypedRequest<api::EngineShowDiagnosticsRequest>(request));
  else if (op == "observability.show_diagnostics_extended") result.api_result = api::EngineShowDiagnosticsExtended(TypedRequest<api::EngineShowDiagnosticsExtendedRequest>(request));
  else if (op == "observability.show_archive_replication") result.api_result = api::EngineShowArchiveReplication(TypedRequest<api::EngineShowArchiveReplicationRequest>(request));
  else if (op == "observability.show_agents_extended") result.api_result = api::EngineShowAgentsExtended(TypedRequest<api::EngineShowAgentsExtendedRequest>(request));
  else if (op == "observability.show_filespace_extended") result.api_result = api::EngineShowFilespaceExtended(TypedRequest<api::EngineShowFilespaceExtendedRequest>(request));
  else if (op == "observability.show_decision_service") result.api_result = api::EngineShowDecisionService(TypedRequest<api::EngineShowDecisionServiceRequest>(request));
  else if (op == "observability.show_acceleration") result.api_result = api::EngineShowAcceleration(TypedRequest<api::EngineShowAccelerationRequest>(request));
  else if (op == "observability.show_acceleration_extended") result.api_result = api::EngineShowAccelerationExtended(TypedRequest<api::EngineShowAccelerationExtendedRequest>(request));
  else if (op == "observability.show_metrics") result.api_result = api::EngineShowMetrics(TypedRequest<api::EngineShowMetricsRequest>(request));
  else if (op == "observability.explain_operation") result.api_result = api::EngineExplainOperation(TypedRequest<api::EngineExplainOperationRequest>(request));
  else if (op == "general.signal_diagnostic") result.api_result = api::EngineSignalDiagnostic(TypedRequest<api::EngineSignalDiagnosticRequest>(request));
  else if (op == "general.raise_diagnostic") result.api_result = api::EngineRaiseDiagnostic(TypedRequest<api::EngineRaiseDiagnosticRequest>(request));
  else if (op == "general.resignal_diagnostic") result.api_result = api::EngineResignalDiagnostic(TypedRequest<api::EngineResignalDiagnosticRequest>(request));
  else if (op == "general.procedural_operation") result.api_result = api::EngineGeneralProceduralOperation(TypedRequest<api::EngineGeneralProceduralOperationRequest>(request));
  else if (op == "catalog.lookup_object") result.api_result = api::EngineLookupObject(TypedRequest<api::EngineLookupObjectRequest>(request));
  else if (op == "catalog.resolve_name") result.api_result = api::EngineResolveName(TypedRequest<api::EngineResolveNameRequest>(request));
  else if (op == "catalog.map_uuid_to_name") result.api_result = api::EngineMapUuidToName(TypedRequest<api::EngineMapUuidToNameRequest>(request));
  else if (op == "catalog.list_children") result.api_result = api::EngineListCatalogChildren(TypedRequest<api::EngineListCatalogChildrenRequest>(request));
  else if (op == "catalog.get_descriptor") result.api_result = api::EngineGetDescriptor(TypedRequest<api::EngineGetDescriptorRequest>(request));
  else if (op == "catalog.get_dependencies") result.api_result = api::EngineGetDependencies(TypedRequest<api::EngineGetDependenciesRequest>(request));
  else if (op.starts_with("catalog.mutation.")) result.api_result = api::EngineCatalogDescriptorMutation(TypedCatalogDescriptorMutationRequest(request));
  else if (op == "artifact.export_catalog") result.api_result = api::EngineExportCatalogArtifacts(TypedRequest<api::EngineExportCatalogArtifactsRequest>(request));
  else if (op == "artifact.import_catalog") result.api_result = api::EngineImportCatalogArtifacts(TypedRequest<api::EngineImportCatalogArtifactsRequest>(request));
  else if (op == "artifact.external_git.export_snapshot") result.api_result = api::EngineExportExternalGitSnapshot(TypedRequest<api::EngineExportExternalGitSnapshotRequest>(request));
  else if (op == "artifact.external_git.diff_snapshot") result.api_result = api::EngineDiffExternalGitSnapshot(TypedRequest<api::EngineDiffExternalGitSnapshotRequest>(request));
  else if (op == "artifact.external_git.rollback_plan") result.api_result = api::EnginePlanExternalGitRollback(TypedRequest<api::EnginePlanExternalGitRollbackRequest>(request));
  else if (op == "ddl.create_database") result.api_result = api::EngineCreateDatabase(TypedRequest<api::EngineCreateDatabaseRequest>(request));
  else if (op == "ddl.create_schema") result.api_result = api::EngineCreateSchema(TypedCreateSchemaRequest(request));
  else if (op == "ddl.create_table") result.api_result = api::EngineCreateTable(TypedCreateTableRequest(request));
  else if (op == "ddl.create_index") result.api_result = api::EngineCreateIndex(TypedCreateIndexRequest(request));
  else if (op == "ddl.create_index_template") result.api_result = api::EngineCreateIndexTemplate(TypedCreateIndexTemplateRequest(request));
  else if (op == "ddl.create_statistics") result.api_result = api::EngineCreateStatistics(TypedCreateStatisticsRequest(request));
  else if (op == "ddl.create_domain") result.api_result = api::EngineCreateDomain(TypedCreateDomainRequest(request));
  else if (op == "ddl.create_sequence") result.api_result = api::EngineCreateSequence(TypedCreateSequenceRequest(request));
  else if (op == "ddl.create_view") result.api_result = api::EngineCreateView(TypedCreateViewRequest(request));
  else if (op == "ddl.create_synonym") result.api_result = api::EngineCreateSynonym(TypedRequest<api::EngineCreateSynonymRequest>(request));
  else if (op == "ddl.constraint.create") result.api_result = api::EngineCreateConstraint(TypedRequest<api::EngineCreateConstraintRequest>(request));
  else if (op == "ddl.constraint.alter") result.api_result = api::EngineAlterConstraint(TypedRequest<api::EngineAlterConstraintRequest>(request));
  else if (op == "ddl.constraint.drop") result.api_result = api::EngineDropConstraint(TypedRequest<api::EngineDropConstraintRequest>(request));
  else if (op == "ddl.create_function") result.api_result = api::EngineCreateFunction(TypedCreateExecutableObjectRequest<api::EngineCreateFunctionRequest>(request, "function", "function"));
  else if (op == "ddl.create_procedure") result.api_result = api::EngineCreateProcedure(TypedCreateExecutableObjectRequest<api::EngineCreateProcedureRequest>(request, "procedure", "procedure"));
  else if (op == "ddl.create_trigger") result.api_result = api::EngineCreateTrigger(TypedCreateExecutableObjectRequest<api::EngineCreateTriggerRequest>(request, "trigger", "trigger"));
  else if (op == "routine.procedure_invoke" || op == "routine.function_invoke") result.api_result = api::EngineInvokeExecutableObject(TypedRequest<api::EngineInvokeExecutableObjectRequest>(request));
  else if (op == "ddl.alter_object") result.api_result = api::EngineAlterObject(TypedAlterObjectRequest(request));
  else if (op == "ddl.drop_object") result.api_result = api::EngineDropObject(TypedRequest<api::EngineDropObjectRequest>(request));
  else if (op == "ddl.comment_on_object") result.api_result = api::EngineCommentOnObject(TypedRequest<api::EngineCommentOnObjectRequest>(request));
  else if (op == "dml.insert_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(2);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedInsertRowsRequest(request);
    mark_phase("typed_insert_request");
    result.api_result = api::EngineInsertRows(typed);
    mark_phase("engine_insert_rows");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.select_rows") result.api_result = api::EngineSelectRows(TypedSelectRowsRequest(request));
  else if (op == "dml.update_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(2);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedUpdateRowsRequest(request);
    mark_phase("typed_update_request");
    result.api_result = api::EngineUpdateRows(typed);
    mark_phase("engine_update_rows");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.delete_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(2);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedDeleteRowsRequest(request);
    mark_phase("typed_delete_request");
    result.api_result = api::EngineDeleteRows(typed);
    mark_phase("engine_delete_rows");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.merge_rows") {
    auto phase_last = SblrSteadyClock::now();
    std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
    phase_micros.reserve(2);
    const auto mark_phase = [&](std::string phase) {
      const auto now = SblrSteadyClock::now();
      phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
      phase_last = now;
    };
    auto typed = TypedMergeRowsRequest(request);
    mark_phase("typed_merge_request");
    result.api_result = api::EngineMergeRows(typed);
    mark_phase("engine_merge_rows");
    WriteSblrDispatchPhaseTrace("dispatch_operation_dml",
                                op,
                                request.envelope.operands.size(),
                                phase_micros);
  }
  else if (op == "dml.plan_import_rows") result.api_result = api::EnginePlanImportRows(TypedPlanImportRowsRequest(request));
  else if (op == "dml.normalize_import_reject_model") result.api_result = api::EngineNormalizeImportRejectModel(TypedNormalizeImportRejectModelRequest(request));
  else if (op == "dml.normalize_import_checkpoint_model") result.api_result = api::EngineNormalizeImportCheckpointModel(TypedNormalizeImportCheckpointRequest(request));
  else if (op == "dml.execute_import_rows") result.api_result = api::EngineExecuteImportRows(TypedExecuteImportRowsRequest(request));
  else if (op == "dml.execute_native_bulk_ingest") result.api_result = api::EngineExecuteNativeBulkIngest(TypedExecuteNativeBulkIngestRequest(request));
  else if (op == "transaction.begin") result.api_result = api::EngineBeginTransaction(TypedBeginTransactionRequest(request));
  else if (op == "transaction.set_characteristics") result.api_result = api::EngineSetTransactionCharacteristics(TypedRequest<api::EngineSetTransactionCharacteristicsRequest>(request));
  else if (op == "transaction.commit") result.api_result = api::EngineCommitTransaction(TypedRequest<api::EngineCommitTransactionRequest>(request));
  else if (op == "transaction.rollback") result.api_result = api::EngineRollbackTransaction(TypedRequest<api::EngineRollbackTransactionRequest>(request));
  else if (op == "transaction.prepare") result.api_result = api::EnginePrepareTransaction(TypedRequest<api::EnginePrepareTransactionRequest>(request));
  else if (op == "transaction.create_savepoint") result.api_result = api::EngineCreateSavepoint(TypedRequest<api::EngineCreateSavepointRequest>(request));
  else if (op == "transaction.release_savepoint") result.api_result = api::EngineReleaseSavepoint(TypedRequest<api::EngineReleaseSavepointRequest>(request));
  else if (op == "transaction.rollback_to_savepoint") result.api_result = api::EngineRollbackToSavepoint(TypedRequest<api::EngineRollbackToSavepointRequest>(request));
  else if (op == "transaction.execute_block") result.api_result = api::EngineExecuteTransactionBlock(TypedRequest<api::EngineExecuteTransactionBlockRequest>(request));
  else if (op == "transaction.lock_table") result.api_result = api::EngineLockTable(TypedRequest<api::EngineLockTableRequest>(request));
  else if (op == "transaction.unlock_table") result.api_result = api::EngineUnlockTable(TypedRequest<api::EngineUnlockTableRequest>(request));
  else if (op == "transaction.lock_named") result.api_result = api::EngineLockNamed(TypedRequest<api::EngineLockNamedRequest>(request));
  else if (op == "transaction.unlock_named") result.api_result = api::EngineUnlockNamed(TypedRequest<api::EngineUnlockNamedRequest>(request));
  else if (op == "query.bind_expression") result.api_result = api::EngineBindExpression(TypedRequest<api::EngineBindExpressionRequest>(request));
  else if (op == "query.cast_value") result.api_result = api::EngineCastValue(TypedRequest<api::EngineCastValueRequest>(request));
  else if (op == "query.extract_value") result.api_result = api::EngineExtractValue(TypedRequest<api::EngineExtractValueRequest>(request));
  else if (op == "query.set_operation") result.api_result = api::EngineSetOperation(TypedRequest<api::EngineSetOperationRequest>(request));
  else if (op == "query.apply_numeric_operation") result.api_result = api::EngineApplyNumericOperation(TypedApplyNumericOperationRequest(request));
  else if (op == "query.canonicalize_document_value") result.api_result = api::EngineCanonicalizeDocumentValue(TypedRequest<api::EngineCanonicalizeDocumentValueRequest>(request));
  else if (op == "query.evaluate_advanced_datatype_family") result.api_result = api::EngineEvaluateAdvancedDatatypeFamily(TypedEvaluateAdvancedDatatypeFamilyRequest(request));
  else if (op == "query.validate_domain_value") result.api_result = api::EngineValidateDomainValue(TypedRequest<api::EngineValidateDomainValueRequest>(request));
  else if (op == "query.invoke_domain_method") result.api_result = api::EngineInvokeDomainMethod(TypedRequest<api::EngineInvokeDomainMethodRequest>(request));
  else if (op == "query.bind_predicate") result.api_result = api::EngineBindPredicate(TypedRequest<api::EngineBindPredicateRequest>(request));
  else if (op == "query.bind_projection") result.api_result = api::EngineBindProjection(TypedRequest<api::EngineBindProjectionRequest>(request));
  else if (op == "query.evaluate_projection") result.api_result = api::EngineEvaluateProjection(TypedEvaluateProjectionRequest(request));
  else if (op == "expression.system_variable_read") result.api_result = EngineReadSystemVariable(request);
  else if (op == "query.plan_operation") result.api_result = api::EnginePlanOperation(TypedPlanOperationRequest(request));
  else if (op == "security.resolve_authority") result.api_result = api::EngineResolveSecurityAuthority(TypedRequest<api::EngineResolveSecurityAuthorityRequest>(request));
  else if (op == "security.authenticate") result.api_result = api::EngineAuthenticate(TypedRequest<api::EngineAuthenticateRequest>(request));
  else if (op == "security.refresh_context") result.api_result = api::EngineRefreshSecurityContext(TypedRequest<api::EngineRefreshSecurityContextRequest>(request));
  else if (op == "security.authorize") result.api_result = api::EngineAuthorize(TypedRequest<api::EngineAuthorizeRequest>(request));
  else if (op == "security.sync_external_groups") result.api_result = api::EngineSyncExternalGroups(TypedRequest<api::EngineSyncExternalGroupsRequest>(request));
  else if (op == "security.explain_membership") result.api_result = api::EngineExplainMembership(TypedRequest<api::EngineExplainMembershipRequest>(request));
  else if (op == "security.emit_audit_event") result.api_result = api::EngineEmitAuditEvent(TypedRequest<api::EngineEmitAuditEventRequest>(request));
  else if (op == "security.encryption_key.admit") result.api_result = api::EngineAdmitEncryptionKey(TypedAdmitEncryptionKeyRequest(request));
  else if (op == "security.encryption_key.rotate") result.api_result = api::EngineRotateEncryptionKey(TypedRotateEncryptionKeyRequest(request));
  else if (op == "security.protected_material_cache.inspect") result.api_result = api::EngineInspectProtectedMaterialCache(TypedInspectProtectedMaterialCacheRequest(request));
  else if (op == "security.protected_material_cache.purge") result.api_result = api::EnginePurgeProtectedMaterial(TypedPurgeProtectedMaterialRequest(request));
  else if (op == "security.protected_material_cache.shutdown") result.api_result = api::EngineShutdownProtectedMaterial(TypedShutdownProtectedMaterialRequest(request));
  else if (op == "security.encrypted_filespace.open") result.api_result = api::EngineOpenEncryptedFilespace(TypedOpenEncryptedFilespaceRequest(request));
  else if (op == "security.request_protected_material") result.api_result = api::EngineRequestProtectedMaterial(TypedRequestProtectedMaterialRequest(request));
  else if (op == "security.protected_material.create") result.api_result = api::EngineCreateProtectedMaterial(TypedCreateProtectedMaterialRequest(request));
  else if (op == "security.protected_material.version.add") result.api_result = api::EngineAddProtectedMaterialVersion(TypedAddProtectedMaterialVersionRequest(request));
  else if (op == "security.protected_material.resolve") result.api_result = api::EngineResolveProtectedMaterial(TypedResolveProtectedMaterialRequest(request));
  else if (op == "security.protected_material.release") result.api_result = api::EngineReleaseProtectedMaterial(TypedReleaseProtectedMaterialRequest(request));
  else if (op == "security.protected_material.version.purge") result.api_result = api::EnginePurgeProtectedMaterialVersion(TypedPurgeProtectedMaterialVersionRequest(request));
  else if (op == "security.protected_material.catalog.inspect") result.api_result = api::EngineInspectProtectedMaterialCatalog(TypedInspectProtectedMaterialCatalogRequest(request));
  else if (op == "security.protected_material.package.export") result.api_result = api::EngineExportProtectedMaterialPackage(TypedExportProtectedMaterialPackageRequest(request));
  else if (op == "security.protected_material.package.import") result.api_result = api::EngineImportProtectedMaterialPackage(TypedImportProtectedMaterialPackageRequest(request));
  else if (op == "security.evaluate_udr_trust") result.api_result = api::EngineEvaluateUdrTrust(TypedRequest<api::EngineEvaluateUdrTrustRequest>(request));
  else if (op == "security.evaluate_manager_admission") result.api_result = api::EngineEvaluateManagerAdmission(TypedRequest<api::EngineEvaluateManagerAdmissionRequest>(request));
  else if (op == "security.seed_standard_bundles") result.api_result = api::EngineSeedStandardSecurityBundles(TypedRequest<api::EngineSeedStandardSecurityBundlesRequest>(request));
  else if (op == "security.evaluate_visibility") result.api_result = api::EngineEvaluateVisibility(TypedRequest<api::EngineEvaluateVisibilityRequest>(request));
  else if (op == "security.evaluate_policy") result.api_result = api::EngineEvaluatePolicy(TypedRequest<api::EngineEvaluatePolicyRequest>(request));
  else if (op == "security.evaluate_deep_enforcement") result.api_result = api::EngineEvaluateDeepSecurity(TypedRequest<api::EngineEvaluateDeepSecurityRequest>(request));
  else if (op == "security.grant_right") result.api_result = api::EngineGrantRight(TypedRequest<api::EngineGrantRightRequest>(request));
  else if (op == "security.revoke_right") result.api_result = api::EngineRevokeRight(TypedRequest<api::EngineRevokeRightRequest>(request));
  else if (op == "security.role.create") result.api_result = api::EngineSecurityCreateRole(TypedSecurityCreateRoleRequest(request));
  else if (op == "security.group.create") result.api_result = api::EngineSecurityCreateGroup(TypedSecurityCreateGroupRequest(request));
  else if (op == "security.principal.create") result.api_result = api::EngineSecurityCreatePrincipal(TypedSecurityCreatePrincipalRequest(request));
  else if (op == "security.principal.alter") result.api_result = api::EngineSecurityAlterPrincipal(TypedSecurityAlterPrincipalRequest(request));
  else if (op == "security.membership.grant") result.api_result = api::EngineSecurityGrantMembership(TypedSecurityGrantMembershipRequest(request));
  else if (op == "security.membership.revoke") result.api_result = api::EngineSecurityRevokeMembership(TypedSecurityRevokeMembershipRequest(request));
  else if (op == "security.privilege.grant") result.api_result = api::EngineSecurityGrantPrivilege(TypedSecurityGrantPrivilegeRequest(request));
  else if (op == "security.privilege.revoke") result.api_result = api::EngineSecurityRevokePrivilege(TypedSecurityRevokePrivilegeRequest(request));
  else if (op == "security.session.set_role") result.api_result = api::EngineSecuritySetRole(TypedSecuritySetRoleRequest(request));
  else if (op == "security.policy.create") result.api_result = api::EngineSecurityCreatePolicy(TypedSecurityCreatePolicyRequest(request));
  else if (op == "security.policy.alter") result.api_result = api::EngineSecurityAlterPolicy(TypedSecurityAlterPolicyRequest(request));
  else if (op == "security.policy.attach") result.api_result = api::EngineSecurityAttachPolicy(TypedSecurityAttachPolicyRequest(request));
  else if (op == "security.policy.activate") result.api_result = api::EngineSecurityActivatePolicy(TypedSecurityActivatePolicyRequest(request));
  else if (op == "security.policy.deactivate") result.api_result = api::EngineSecurityDeactivatePolicy(TypedSecurityDeactivatePolicyRequest(request));
  else if (op == "security.policy.validate") result.api_result = api::EngineSecurityValidatePolicy(TypedSecurityValidatePolicyRequest(request));
  else if (op == "security.policy.show") result.api_result = api::EngineSecurityShowPolicy(TypedSecurityShowPolicyRequest(request));
  else if (op == "security.create_identity") result.api_result = api::EngineCreateIdentity(TypedRequest<api::EngineCreateIdentityRequest>(request));
  else if (op == "security.alter_identity") result.api_result = api::EngineAlterIdentity(TypedRequest<api::EngineAlterIdentityRequest>(request));
  else if (op == "security.register_auth_provider") result.api_result = api::EngineRegisterAuthProvider(TypedRequest<api::EngineRegisterAuthProviderRequest>(request));
  else if (op == "security.inspect_auth_provider") result.api_result = api::EngineInspectAuthProvider(TypedRequest<api::EngineInspectAuthProviderRequest>(request));
  else if (op == "security.disable_auth_provider") result.api_result = api::EngineDisableAuthProvider(TypedRequest<api::EngineDisableAuthProviderRequest>(request));
  else if (op == "security.reload_auth_provider_policy") result.api_result = api::EngineReloadAuthProviderPolicy(TypedRequest<api::EngineReloadAuthProviderPolicyRequest>(request));
  else if (op == "security.authenticate_provider") result.api_result = api::EngineAuthenticateProvider(TypedRequest<api::EngineAuthenticateProviderRequest>(request));
  else if (op == "security.continue_auth_challenge") result.api_result = api::EngineContinueAuthChallenge(TypedRequest<api::EngineContinueAuthChallengeRequest>(request));
  else if (op == "security.rotate_credential") result.api_result = api::EngineRotateCredential(TypedRequest<api::EngineRotateCredentialRequest>(request));
  else if (op == "security.revoke_token") result.api_result = api::EngineRevokeToken(TypedRequest<api::EngineRevokeTokenRequest>(request));
  else if (op == "security.sync_provider_groups") result.api_result = api::EngineSyncExternalGroups(TypedRequest<api::EngineSyncExternalGroupsRequest>(request));
  else if (op == "security.explain_provider_membership") result.api_result = api::EngineExplainMembership(TypedRequest<api::EngineExplainMembershipRequest>(request));
  else if (op == "security.inspect_auth_provider_metrics") result.api_result = api::EngineInspectAuthProviderMetrics(TypedRequest<api::EngineInspectAuthProviderMetricsRequest>(request));
  else if (op == "management.inspect_config") result.api_result = api::EngineInspectConfig(TypedRequest<api::EngineInspectConfigRequest>(request));
  else if (op == "management.set_config") result.api_result = api::EngineSetConfig(TypedRequest<api::EngineSetConfigRequest>(request));
  else if (op == "management.reset_config") result.api_result = api::EngineResetConfig(TypedRequest<api::EngineResetConfigRequest>(request));
  else if (op == "management.inspect_runtime") result.api_result = api::EngineInspectManagementRuntime(TypedRequest<api::EngineInspectManagementRuntimeRequest>(request));
  else if (op == "management.control_runtime") result.api_result = api::EngineControlManagementRuntime(TypedRequest<api::EngineControlManagementRuntimeRequest>(request));
  else if (op == "management.prepare_support_bundle") result.api_result = api::EnginePrepareSupportBundle(TypedRequest<api::EnginePrepareSupportBundleRequest>(request));
  else if (IsMemoryManagementOperation(op)) result.api_result = api::EnginePlanMemoryManagementOperation(TypedMemoryManagementRequest(request));
  else if (IsStorageTierMigrationOperation(op)) result.api_result = api::EnginePlanStorageTierMigrationOperation(TypedStorageTierMigrationRequest(request));
  else if (IsFilespaceDiscoveryOperation(op)) result.api_result = api::EngineDiscoverFilespaceAnomalies(TypedFilespaceDiscoveryRequest(request));
  else if (IsFilespacePackageOperation(op)) result.api_result = api::EngineFilespacePackageOperation(TypedFilespacePackageRequest(request));
  else if (IsShardPlacementDescriptorOperation(op)) result.api_result = api::EnginePlanShardPlacementOperation(TypedShardPlacementDescriptorRequest(request));
  else if (op.starts_with("index.")) result.api_result = api::EngineIndexManagementOperation(TypedRequest<api::EngineIndexManagementRequest>(request));
  else if (op == "lifecycle.create_database") result.api_result = api::EngineCreateLifecycle(TypedRequest<api::EngineCreateLifecycleRequest>(request));
  else if (op == "lifecycle.open_database") result.api_result = api::EngineOpenLifecycle(TypedRequest<api::EngineOpenLifecycleRequest>(request));
  else if (op == "lifecycle.attach_database") result.api_result = api::EngineAttachLifecycle(TypedRequest<api::EngineAttachLifecycleRequest>(request));
  else if (op == "lifecycle.detach_database") result.api_result = api::EngineDetachLifecycle(TypedRequest<api::EngineDetachLifecycleRequest>(request));
  else if (op == "lifecycle.enter_maintenance") result.api_result = api::EngineEnterMaintenanceLifecycle(TypedRequest<api::EngineEnterMaintenanceLifecycleRequest>(request));
  else if (op == "lifecycle.exit_maintenance") result.api_result = api::EngineExitMaintenanceLifecycle(TypedRequest<api::EngineExitMaintenanceLifecycleRequest>(request));
  else if (op == "lifecycle.enter_restricted_open") result.api_result = api::EngineEnterRestrictedOpenLifecycle(TypedRequest<api::EngineEnterRestrictedOpenLifecycleRequest>(request));
  else if (op == "lifecycle.exit_restricted_open") result.api_result = api::EngineExitRestrictedOpenLifecycle(TypedRequest<api::EngineExitRestrictedOpenLifecycleRequest>(request));
  else if (op == "lifecycle.inspect_database") result.api_result = api::EngineInspectLifecycle(TypedRequest<api::EngineInspectLifecycleRequest>(request));
  else if (op == "lifecycle.verify_database") result.api_result = api::EngineVerifyLifecycle(TypedRequest<api::EngineVerifyLifecycleRequest>(request));
  else if (op == "lifecycle.repair_database") result.api_result = api::EngineRepairLifecycle(TypedRequest<api::EngineRepairLifecycleRequest>(request));
  else if (op == "lifecycle.shutdown_database") result.api_result = api::EngineShutdownLifecycle(TypedRequest<api::EngineShutdownLifecycleRequest>(request));
  else if (op == "lifecycle.shutdown_force") result.api_result = api::EngineForceShutdownLifecycle(TypedRequest<api::EngineForceShutdownLifecycleRequest>(request));
  else if (op == "lifecycle.shutdown_acknowledge") result.api_result = api::EngineAcknowledgeShutdownLifecycle(TypedRequest<api::EngineAcknowledgeShutdownLifecycleRequest>(request));
  else if (op == "lifecycle.drop_database") result.api_result = api::EngineDropLifecycle(TypedRequest<api::EngineDropLifecycleRequest>(request));
  else if (op == "agents.list") result.api_result = api::EngineListAgents(TypedRequest<api::EngineListAgentsRequest>(request));
  else if (op == "agents.show") result.api_result = api::EngineShowAgent(TypedRequest<api::EngineShowAgentRequest>(request));
  else if (op == "agents.start") result.api_result = api::EngineStartAgent(TypedRequest<api::EngineStartAgentRequest>(request));
  else if (op == "agents.stop") result.api_result = api::EngineStopAgent(TypedRequest<api::EngineStopAgentRequest>(request));
  else if (op == "agents.pause") result.api_result = api::EnginePauseAgent(TypedRequest<api::EnginePauseAgentRequest>(request));
  else if (op == "agents.resume") result.api_result = api::EngineResumeAgent(TypedRequest<api::EngineResumeAgentRequest>(request));
  else if (op == "agents.configure") result.api_result = api::EngineConfigureAgent(TypedRequest<api::EngineConfigureAgentRequest>(request));
  else if (op == "agents.run") result.api_result = api::EngineRunAgent(TypedRequest<api::EngineRunAgentRequest>(request));
  else if (op == "agents.dry_run") result.api_result = api::EngineDryRunAgent(TypedRequest<api::EngineDryRunAgentRequest>(request));
  else if (op == "agents.override") result.api_result = api::EngineOverrideAgent(TypedRequest<api::EngineOverrideAgentRequest>(request));
  else if (op == "sys.agents") result.api_result = api::EngineSysAgents(TypedRequest<api::EngineSysAgentsRequest>(request));
  else if (op == "cluster.sys.agents") result.api_result = api::EngineClusterSysAgents(TypedRequest<api::EngineClusterSysAgentsRequest>(request));
  else if (op == "agents.request_page_preallocation") result.api_result = api::EngineRequestPagePreallocation(TypedAgentHookRequest<api::EngineRequestPagePreallocationRequest>(request, "page_allocation_manager", "page_preallocation_request"));
  else if (op == "agents.request_page_relocation") result.api_result = api::EngineRequestPageRelocation(TypedAgentHookRequest<api::EngineRequestPageRelocationRequest>(request, "page_allocation_manager", "page_relocation_request"));
  else if (op == "agents.request_filespace_growth") result.api_result = api::EngineRequestFilespaceGrowth(TypedAgentHookRequest<api::EngineRequestFilespaceGrowthRequest>(request, "filespace_capacity_manager", "filespace_growth_request"));
  else if (op == "agents.notify_filespace_shrink_readiness") result.api_result = api::EngineNotifyFilespaceShrinkReadiness(TypedAgentHookRequest<api::EngineNotifyFilespaceShrinkReadinessRequest>(request, "page_allocation_manager", "filespace_shrink_readiness_notification"));
  else if (IsAgentCommandSurfaceOperationId(op)) result.api_result = api::EngineAgentCommandSurfaceOperation(TypedRequest<api::EngineAgentCommandSurfaceRequest>(request));
  else if (op == "event.channel.create") result.api_result = api::EngineCreateEventChannel(TypedRequest<api::EngineCreateEventChannelRequest>(request));
  else if (op == "event.channel.alter") result.api_result = api::EngineAlterEventChannel(TypedRequest<api::EngineAlterEventChannelRequest>(request));
  else if (op == "event.channel.drop") result.api_result = api::EngineDropEventChannel(TypedRequest<api::EngineDropEventChannelRequest>(request));
  else if (op == "event.channel.listen" || op == "notification.channel.listen") result.api_result = api::EngineListenNotification(TypedRequest<api::EngineListenNotificationRequest>(request));
  else if (op == "event.channel.unlisten" || op == "notification.channel.unlisten") result.api_result = api::EngineUnlistenNotification(TypedRequest<api::EngineUnlistenNotificationRequest>(request));
  else if (op == "event.channel.notify" || op == "notification.channel.notify") result.api_result = api::EngineNotifyEventChannel(TypedRequest<api::EngineNotifyEventChannelRequest>(request));
  else if (op == "event.subscription.list") result.api_result = api::EngineListEventSubscriptions(TypedRequest<api::EngineListEventSubscriptionsRequest>(request));
  else if (op == "event.delivery.poll") result.api_result = api::EnginePollEventDelivery(TypedRequest<api::EnginePollEventDeliveryRequest>(request));
  else if (op == "event.delivery.ack") result.api_result = api::EngineAcknowledgeEventDelivery(TypedRequest<api::EngineAcknowledgeEventDeliveryRequest>(request));
  else if (op == "session.notification.unlisten" || op == "session.notification.unlisten_all") result.api_result = api::EngineUnlistenSessionNotifications(TypedRequest<api::EngineUnlistenSessionNotificationsRequest>(request));
  else if (op == "agents.request_index_delta_merge") result.api_result = api::EngineRequestIndexDeltaMerge(TypedAgentHookRequest<api::EngineRequestIndexDeltaMergeRequest>(request, "index_health_manager", "index_delta_merge_request"));
  else if (op == "agents.request_index_rebuild_or_shadow_build") result.api_result = api::EngineRequestIndexRebuildOrShadowBuild(TypedAgentHookRequest<api::EngineRequestIndexRebuildOrShadowBuildRequest>(request, "index_health_manager", "index_rebuild_request"));
  else if (op == "cluster.inspect_state") result.api_result = api::EngineInspectClusterState(TypedRequest<api::EngineInspectClusterStateRequest>(request));
  else if (op == "cluster.inspect_routing_plan") result.api_result = api::EngineInspectClusterRoutingPlan(TypedRequest<api::EngineInspectClusterRoutingPlanRequest>(request));
  else if (op == "cluster.control_cluster") result.api_result = api::EngineControlCluster(TypedRequest<api::EngineControlClusterRequest>(request));
  else if (op == "cluster.place_object") result.api_result = api::EnginePlaceClusterObject(TypedRequest<api::EnginePlaceClusterObjectRequest>(request));
  else if (op == "cluster.inspect_replication") result.api_result = api::EngineInspectReplication(TypedRequest<api::EngineInspectReplicationRequest>(request));
  else if (op == "cluster.prepare_remote_participant_insert") result.api_result = api::EnginePrepareRemoteParticipantInsert(TypedRequest<api::EngineRemoteParticipantInsertRequest>(request));
  else if (op == "cluster.validate_insert_route_fence") result.api_result = api::EngineValidateClusterInsertRouteFence(TypedRequest<api::EngineClusterInsertRouteFenceRequest>(request));
  else if (op == "cluster.profile_operation") result.api_result = api::EngineClusterProfileOperation(TypedRequest<api::EngineClusterProfileOperationRequest>(request));
  else if (op == "nosql.document_insert") result.api_result = api::EngineDocumentInsert(TypedRequest<api::EngineDocumentInsertRequest>(request));
  else if (op == "nosql.document_find") result.api_result = api::EngineDocumentFind(TypedRequest<api::EngineDocumentFindRequest>(request));
  else if (op == "nosql.document_update") result.api_result = api::EngineDocumentUpdate(TypedRequest<api::EngineDocumentUpdateRequest>(request));
  else if (op == "nosql.document_delete") result.api_result = api::EngineDocumentDelete(TypedRequest<api::EngineDocumentDeleteRequest>(request));
  else if (op == "nosql.key_value_get") result.api_result = api::EngineKeyValueGet(TypedRequest<api::EngineKeyValueGetRequest>(request));
  else if (op == "nosql.key_value_put") result.api_result = api::EngineKeyValuePut(TypedRequest<api::EngineKeyValuePutRequest>(request));
  else if (op == "nosql.key_value_multiget") result.api_result = api::EngineKeyValueMultiGet(TypedRequest<api::EngineKeyValueMultiGetRequest>(request));
  else if (op == "nosql.key_value_pipeline") result.api_result = api::EngineKeyValuePipeline(TypedRequest<api::EngineKeyValuePipelineRequest>(request));
  else if (op == "nosql.key_value_atomic_program") result.api_result = api::EngineKeyValueAtomicProgram(TypedRequest<api::EngineKeyValueAtomicProgramRequest>(request));
  else if (op == "nosql.backpressure_debt_plan") result.api_result = api::EnginePlanNoSqlBackpressureDebt(TypedRequest<api::EnginePlanNoSqlBackpressureDebtRequest>(request));
  else if (op == "nosql.family_maintenance_plan") result.api_result = api::EnginePlanNoSqlFamilyMaintenance(TypedRequest<api::EnginePlanNoSqlFamilyMaintenanceRequest>(request));
  else if (op == "nosql.statistics_advisor_plan") result.api_result = api::EnginePlanNoSqlStatisticsAdvisor(TypedRequest<api::EnginePlanNoSqlStatisticsAdvisorRequest>(request));
  else if (op == "nosql.graph_query") result.api_result = api::EngineGraphQuery(TypedRequest<api::EngineGraphQueryRequest>(request));
  else if (op == "nosql.vector_search") result.api_result = api::EngineVectorSearch(TypedRequest<api::EngineVectorSearchRequest>(request));
  else if (op == "nosql.vector_collection_op") result.api_result = api::EngineVectorCollectionOperation(TypedRequest<api::EngineVectorCollectionOperationRequest>(request));
  else if (op == "nosql.search_query") result.api_result = api::EngineSearchQuery(TypedRequest<api::EngineSearchQueryRequest>(request));
  else if (op == "nosql.time_series_append") result.api_result = api::EngineTimeSeriesAppend(TypedRequest<api::EngineTimeSeriesAppendRequest>(request));
  else if (op == "extensibility.inspect_gpu_capability") result.api_result = api::EngineInspectGpuCapability(TypedRequest<api::EngineInspectGpuCapabilityRequest>(request));
  else if (op == "extensibility.compile_llvm_module") result.api_result = api::EngineCompileLlvmModule(TypedRequest<api::EngineCompileLlvmModuleRequest>(request));
  else if (op == "filespace.preallocate") result.api_result = api::EngineFilespacePreallocate(TypedRequest<api::EngineFilespacePreallocateRequest>(request));
  else if (op == "filespace.create" ||
           op == "filespace.attach" ||
           op == "filespace.detach" ||
           op == "filespace.disconnect" ||
           op == "filespace.move" ||
           op == "filespace.merge" ||
           op == "filespace.promote" ||
           op == "filespace.verify" ||
           op == "filespace.compact" ||
           op == "filespace.fence" ||
           op == "filespace.release" ||
           op == "filespace.archive" ||
           op == "filespace.quarantine" ||
           op == "filespace.snapshot.create" ||
           op == "filespace.snapshot.refresh" ||
           op == "filespace.snapshot.validate" ||
           op == "filespace.snapshot.retire" ||
           op == "filespace.shadow.create" ||
           op == "filespace.shadow.refresh" ||
           op == "filespace.shadow.validate" ||
           op == "filespace.shadow.promote" ||
           op == "filespace.truncate" ||
           op == "filespace.drop" ||
           op == "filespace.delete_physical" ||
           op == "filespace.repair" ||
           op == "filespace.rebuild" ||
           op == "filespace.salvage") result.api_result = api::EngineFilespaceLifecycleOperation(TypedRequest<api::EngineFilespaceLifecycleRequest>(request));
  else if (op == "storage.manage_operation") result.api_result = api::EngineStorageManagementOperation(TypedRequest<api::EngineStorageManagementRequest>(request));
  else if (op == "extensibility.register_udr_package") result.api_result = api::EngineRegisterUdrPackage(TypedRequest<api::EngineRegisterUdrPackageRequest>(request));
  else if (op == "extensibility.load_udr_package") result.api_result = api::EngineLoadUdrPackage(TypedRequest<api::EngineLoadUdrPackageRequest>(request));
  else if (op == "extensibility.unload_udr_package") result.api_result = api::EngineUnloadUdrPackage(TypedRequest<api::EngineUnloadUdrPackageRequest>(request));
  else if (op == "extensibility.inspect_udr_packages") result.api_result = api::EngineInspectUdrPackages(TypedRequest<api::EngineInspectUdrPackageRequest>(request));
  else if (op == "extensibility.invoke_udr_package") result.api_result = api::EngineInvokeUdrPackage(TypedRequest<api::EngineInvokeUdrPackageRequest>(request));
  else if (op == "extensibility.register_parser_package") result.api_result = api::EngineRegisterParserPackage(TypedRequest<api::EngineRegisterParserPackageRequest>(request));
  else {
    result.accepted = false;
    result.dispatched_to_api = false;
    result.diagnostics.push_back(DispatchDiagnostic("SB_SBLR_DISPATCH_UNKNOWN_OPERATION",
                                                   "SBLR operation is not mapped to an engine API function"));
    result.api_result = FailureResult(request.context,
                                      op,
                                      "SB_SBLR_DISPATCH_UNKNOWN_OPERATION",
                                      "engine.sblr.dispatch.unknown_operation",
                                      op);
  }

  PropagateClusterApiDiagnostics(&result);

  return result;
}

SblrDispatchResult DecodeAndDispatchSblrOperation(std::string_view encoded_envelope,
                                                  api::EngineRequestContext context,
                                                  api::EngineApiRequest api_request) {
  auto phase_last = SblrSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(4);
  const auto mark_phase = [&](std::string phase) {
    const auto now = SblrSteadyClock::now();
    phase_micros.push_back({std::move(phase), SblrElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const auto decoded = DecodeSblrEnvelope(encoded_envelope);
  mark_phase("decode_text_envelope");
  if (!decoded.ok) {
    SblrDispatchResult result;
    result.envelope_validated = false;
    result.diagnostics = decoded.diagnostics;
    result.api_result = FailureResult(context,
                                      decoded.envelope.operation_id,
                                      "SB_SBLR_DECODE_REJECTED",
                                      "engine.sblr.decode.rejected",
                                      "encoded envelope failed validation");
    WriteSblrDispatchPhaseTrace("decode_and_dispatch",
                                decoded.envelope.operation_id,
                                encoded_envelope.size(),
                                phase_micros);
    return result;
  }
  SblrDispatchRequest request;
  request.context = std::move(context);
  request.envelope = decoded.envelope;
  request.api_request = std::move(api_request);
  auto result = DispatchSblrOperation(request);
  mark_phase("dispatch_operation");
  WriteSblrDispatchPhaseTrace("decode_and_dispatch",
                              request.envelope.operation_id,
                              encoded_envelope.size(),
                              phase_micros);
  return result;
}

std::string SerializeSblrDispatchResultToJson(const SblrDispatchResult& result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"accepted\": " << (result.accepted ? "true" : "false") << ",\n";
  out << "  \"envelope_validated\": " << (result.envelope_validated ? "true" : "false") << ",\n";
  out << "  \"dispatched_to_api\": " << (result.dispatched_to_api ? "true" : "false") << ",\n";
  out << "  \"api_ok\": " << (result.api_result.ok ? "true" : "false") << ",\n";
  out << "  \"operation_id\": \"" << JsonEscape(result.api_result.operation_id) << "\",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    out << "    {\"code\": \"" << JsonEscape(result.diagnostics[i].code) << "\", \"message\": \""
        << JsonEscape(result.diagnostics[i].message) << "\"}";
    if (i + 1 != result.diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"api_diagnostics\": [\n";
  for (std::size_t i = 0; i < result.api_result.diagnostics.size(); ++i) {
    const auto& diagnostic = result.api_result.diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"message_key\": \"" << JsonEscape(diagnostic.message_key)
        << "\", \"detail\": \"" << JsonEscape(diagnostic.detail) << "\"}";
    if (i + 1 != result.api_result.diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"api_diagnostic_count\": " << result.api_result.diagnostics.size() << "\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::engine::sblr
