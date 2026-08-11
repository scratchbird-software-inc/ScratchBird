// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "wire/sbsql_test_wire.hpp"

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "embedded/embedded_engine_client.hpp"
#include "ipc/sbps_client.hpp"
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"

#include "scratchbird/engine/sblr/lowering.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openssl/evp.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scratchbird::parser::sbsql {
namespace {

namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using ipc::ParserCanonicalSblrSubmission;
using ipc::ParserStatementContext;
using ipc::ParserTransactionSelector;

constexpr std::size_t kMaxNameResolutionCacheEntries = 4096;
constexpr std::size_t kMaxSharedNameResolutionCacheEntries = 16384;
constexpr std::size_t kMaxStableRelationNameResolutionCacheEntries = 4096;

std::optional<std::array<std::uint8_t, 16>> CanonicalUuidBytes(
    std::string_view text);

bool IsCanonicalStatementTimestamp(std::string_view value) {
  if (value.size() != 20 &&
      (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigitIndexes[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigitIndexes) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

using ParserPipelineClock = std::chrono::steady_clock;

std::uint64_t ParserPipelineElapsedMicros(ParserPipelineClock::time_point start,
                                          ParserPipelineClock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

void WriteParserPipelinePhaseTrace(
    std::string_view sql,
    const PipelineResult& result,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* path = std::getenv("SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE");
  if (path == nullptr || *path == '\0') return;
  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << "layer=sbsql_pipeline"
      << "\toperation=" << result.operation_family
      << "\tfamily=" << result.statement_family
      << "\taccepted=" << (result.accepted ? "true" : "false")
      << "\tsql_bytes=" << sql.size()
      << "\tstatement_hash=" << result.statement_hash;
  for (const auto& [phase, micros] : phase_micros) {
    out << '\t' << phase << "_us=" << micros;
  }
  out << '\n';
}

std::string NewRowUuid() {
  static std::uint64_t sequence = 0;
  const auto generated =
      uuid::GenerateEngineIdentityV7(UuidKind::row, CurrentUnixMillis() + (++sequence));
  return generated.ok() ? uuid::UuidToString(generated.value.value) : std::string{};
}

UuidKind UuidKindForCreatedObjectClass(std::string_view object_class) {
  std::string normalized;
  normalized.reserve(object_class.size());
  for (const char ch : object_class) {
    normalized.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(ch))));
  }
  if (normalized == "DATABASE") return UuidKind::database;
  if (normalized == "CLUSTER") return UuidKind::cluster;
  if (normalized == "FILESPACE") return UuidKind::filespace;
  if (normalized == "SCHEMA") return UuidKind::schema;
  if (normalized == "USER" || normalized == "PRINCIPAL" ||
      normalized == "ROLE" || normalized == "GROUP") {
    return UuidKind::principal;
  }
  return UuidKind::object;
}

std::string NewCreatedObjectUuid(std::string_view object_class) {
  static std::uint64_t sequence = 1000000;
  const auto generated =
      uuid::GenerateEngineIdentityV7(UuidKindForCreatedObjectClass(object_class),
                                     CurrentUnixMillis() + (++sequence));
  return generated.ok() ? uuid::UuidToString(generated.value.value) : std::string{};
}

std::string AfterCommand(std::string_view line, std::string_view command) {
  auto trimmed = TrimAscii(line);
  if (trimmed.size() <= command.size()) return {};
  return TrimAscii(std::string_view(trimmed).substr(command.size()));
}

bool WriteAll(std::intptr_t fd, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
#ifdef _WIN32
    const int want = static_cast<int>(std::min<std::size_t>(
        text.size() - written, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int rc = ::send(static_cast<SOCKET>(fd), text.data() + written, want, 0);
#else
    const auto rc = ::write(static_cast<int>(fd), text.data() + written, text.size() - written);
#endif
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
#ifndef _WIN32
    if (rc < 0 && errno == EINTR) continue;
#endif
    return false;
  }
  return true;
}

bool ReadLine(std::intptr_t fd, std::string* line) {
  line->clear();
  char ch = 0;
  for (;;) {
#ifdef _WIN32
    const int rc = ::recv(static_cast<SOCKET>(fd), &ch, 1, 0);
#else
    const auto rc = ::read(static_cast<int>(fd), &ch, 1);
#endif
    if (rc == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') line->push_back(ch);
      continue;
    }
#ifndef _WIN32
    if (rc < 0 && errno == EINTR) continue;
#endif
    return !line->empty();
  }
}

class ScopedParserState {
 public:
  ScopedParserState(ParserMetrics* metrics,
                    bool enabled,
                    ParserState active,
                    ParserState fallback)
      : metrics_(metrics), enabled_(enabled), fallback_(fallback) {
    if (metrics_ != nullptr && enabled_) metrics_->SetState(active);
  }
  ~ScopedParserState() {
    if (metrics_ != nullptr && enabled_) metrics_->SetState(fallback_);
  }

 private:
  ParserMetrics* metrics_;
  bool enabled_;
  ParserState fallback_;
};

bool ApplyExecutedTransactionState(const ServerExecutionResult& executed,
                                   SessionContext* session) {
  if (session == nullptr || !executed.transaction_state_present) return false;
  const bool changed =
      session->local_transaction_id != executed.local_transaction_id ||
      session->transaction_uuid != executed.transaction_uuid;
  session->local_transaction_id = executed.local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      executed.snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = executed.transaction_uuid;
  session->transaction_timestamp = executed.transaction_timestamp;
  session->transaction_context = "always_active";
  return changed;
}

bool ExecutionInvalidatesNameResolution(std::string_view operation_id) {
  return operation_id.rfind("ddl.", 0) == 0 ||
         operation_id.rfind("catalog.", 0) == 0 ||
         operation_id.rfind("security.", 0) == 0 ||
         operation_id.rfind("language.", 0) == 0 ||
         operation_id.rfind("policy.", 0) == 0 ||
         operation_id.rfind("auth.", 0) == 0;
}

bool ExecutionPreservesReferencedRelationNames(std::string_view operation_id) {
  return operation_id == "ddl.create_index" ||
         operation_id == "ddl.create_index_template" ||
         operation_id == "ddl.comment_on_object" ||
         operation_id == "catalog.mutation.refresh_materialized_view" ||
         operation_id.rfind("catalog.mutation.create_", 0) == 0;
}

bool IsReferencedRelationNameClass(std::string_view object_class) {
  return object_class == "relation" ||
         object_class == "table" ||
         object_class == "view" ||
         object_class == "materialized_view" ||
         object_class == "filespace" ||
         object_class == "filespace_agent";
}

struct ObjectReference {
  std::string presented_name;
  std::string object_class{"relation"};
  bool quoted{false};
  bool create_reservation{false};
};

struct ResolvedObjectReferenceSeed {
  ObjectReference ref;
  PublicNameResolutionResult resolved;
};

std::optional<std::string> EncodeQualifiedPresentedName(
    const std::vector<NativeIdentifierAstNode>& qualified_name) {
  if (qualified_name.empty()) return std::nullopt;
  std::string presented_name;
  for (const auto& component : qualified_name) {
    if (component.spelling.empty()) return std::nullopt;
    if (!presented_name.empty()) presented_name.push_back('.');
    if (!component.quoted) {
      presented_name.append(component.spelling);
      continue;
    }
    presented_name.push_back('"');
    for (const char ch : component.spelling) {
      if (ch == '"') presented_name.push_back('"');
      presented_name.push_back(ch);
    }
    presented_name.push_back('"');
  }
  return presented_name;
}

std::string CanonicalUnquotedIdentifier(std::string value) {
  for (auto& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool SameIdentifierComponent(const NativeIdentifierAstNode& left,
                             const NativeIdentifierAstNode& right) {
  if (left.quoted != right.quoted) return false;
  return left.quoted
             ? left.spelling == right.spelling
             : CanonicalUnquotedIdentifier(left.spelling) ==
                   CanonicalUnquotedIdentifier(right.spelling);
}

std::string CanonicalColumnLookupKey(
    const NativeIdentifierAstNode& component) {
  return component.quoted ? component.spelling
                          : CanonicalUnquotedIdentifier(component.spelling);
}

std::string_view NativeAggregateSemantic(
    NativeAggregateGroupingForm grouping,
    NativeAggregateProjectionForm projection) {
  if (grouping == NativeAggregateGroupingForm::kSimple) {
    if (projection == NativeAggregateProjectionForm::kKeyCountSum) {
      return "aggregate.grouped-int64-key-count-sum.v1";
    }
    if (projection == NativeAggregateProjectionForm::kKeysCountSum) {
      return "aggregate.grouped-int64-keys-count-sum.v1";
    }
    return {};
  }
  const bool metadata =
      projection == NativeAggregateProjectionForm::kKeysCountSumGrouping;
  if (projection != NativeAggregateProjectionForm::kKeysCountSum &&
      !metadata) {
    return {};
  }
  switch (grouping) {
    case NativeAggregateGroupingForm::kGroupingSets:
      return metadata
                 ? "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1"
                 : "aggregate.grouping-sets-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kRollup:
      return metadata
                 ? "aggregate.rollup-int64-keys-count-sum-grouping.v1"
                 : "aggregate.rollup-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kCube:
      return metadata
                 ? "aggregate.cube-int64-keys-count-sum-grouping.v1"
                 : "aggregate.cube-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kNone:
    case NativeAggregateGroupingForm::kSimple:
      return {};
  }
  return {};
}

std::string NativeFilterSemantic(
    const NativeRelationalAstDocument& ast,
    const NativeRelationAstNode& relation) {
  if (relation.predicate_expression_ids.size() != 1) return {};
  std::unordered_map<std::uint32_t, const NativeExpressionAstNode*> expressions;
  for (const auto& expression : ast.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  const auto find = [&](std::uint32_t id) -> const NativeExpressionAstNode* {
    const auto iterator = expressions.find(id);
    return iterator == expressions.end() ? nullptr : iterator->second;
  };
  const NativeExpressionAstNode* root =
      find(relation.predicate_expression_ids.front());
  if (root == nullptr) return {};
  unsigned not_count = 0;
  while (root->expression_kind == NativeExpressionAstKind::kUnary &&
         root->operator_name == "NOT" &&
         root->child_expression_ids.size() == 1 && not_count < 2) {
    root = find(root->child_expression_ids.front());
    if (root == nullptr) return {};
    ++not_count;
  }

  const auto comparison_function = [&](const NativeExpressionAstNode* value) {
    if (value == nullptr ||
        value->expression_kind != NativeExpressionAstKind::kBinary ||
        value->operator_name != ">" ||
        value->child_expression_ids.size() != 2) {
      return std::string{};
    }
    const auto* function = find(value->child_expression_ids.front());
    return function != nullptr &&
                   function->expression_kind ==
                       NativeExpressionAstKind::kFunctionCall
               ? ToUpperAscii(function->operator_name)
               : std::string{};
  };

  std::string core;
  if (root->expression_kind == NativeExpressionAstKind::kBinary &&
      (root->operator_name == "AND" || root->operator_name == "OR") &&
      root->child_expression_ids.size() == 2) {
    const auto left = comparison_function(find(root->child_expression_ids[0]));
    const auto right = comparison_function(find(root->child_expression_ids[1]));
    if (left == "COUNT" && right == "SUM") {
      core = "count-sum-" +
             std::string(root->operator_name == "OR" ? "or" : "and") +
             "-gt-int64-literals";
    } else if (left == "SUM" && right == "COUNT" &&
               root->operator_name == "OR" && not_count == 2) {
      core = "sum-count-or-gt-int64-literals";
    } else {
      return {};
    }
  } else {
    const auto function = comparison_function(root);
    if (function == "COUNT") {
      core = "count-gt-int64-literal";
    } else if (function == "SUM") {
      core = "sum-gt-int64-literal";
    } else {
      return {};
    }
  }
  return "filter.having-" +
         std::string(not_count == 2 ? "not-not-" :
                     not_count == 1 ? "not-" : "") +
         core + ".v1";
}

std::optional<std::string_view> EngineIssuedAggregateFunctionUuid(
    const ParserStatementContext& statement_context,
    std::string_view function_name) {
  std::string builtin_id = "sb.aggregate.";
  builtin_id.reserve(builtin_id.size() + function_name.size());
  for (const char ch : function_name) {
    builtin_id.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }
  const auto profile = std::ranges::find_if(
      statement_context.aggregate_function_profiles,
      [&](const auto& candidate) {
        return candidate.abi_version == 1 && candidate.executable &&
               candidate.builtin_id == builtin_id;
      });
  if (profile == statement_context.aggregate_function_profiles.end() ||
      !CanonicalUuidBytes(profile->function_uuid).has_value()) {
    return std::nullopt;
  }
  return profile->function_uuid;
}

struct ExactProjectedDescriptorFields {
  std::string type_uuid;
  std::optional<std::string> collation_uuid;
  std::optional<std::string> timezone_profile_id;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> precision;
  std::optional<std::uint32_t> scale;
  bool nullable{false};
};

std::optional<ExactProjectedDescriptorFields> ParseExactProjectedDescriptor(
    std::string_view encoded,
    std::string_view projected_collation_uuid,
    bool projected_nullable) {
  ExactProjectedDescriptorFields fields;
  std::optional<bool> canonical_nullable;
  std::optional<bool> storage_nullable;
  bool type_seen = false;
  bool collation_seen = false;
  bool timezone_seen = false;
  bool width_seen = false;
  bool precision_seen = false;
  bool scale_seen = false;
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto delimiter = encoded.find(';', offset);
    const auto end = delimiter == std::string_view::npos ? encoded.size()
                                                         : delimiter;
    const auto field = encoded.substr(offset, end - offset);
    const auto assign_text = [&](std::string_view prefix, bool* seen,
                                 std::string* value) {
      if (!field.starts_with(prefix)) return true;
      if (*seen || field.size() == prefix.size()) return false;
      *seen = true;
      value->assign(field.substr(prefix.size()));
      return true;
    };
    if (!assign_text("type_uuid=", &type_seen, &fields.type_uuid)) {
      return std::nullopt;
    }
    std::string collation_uuid;
    if (field.starts_with("collation_uuid=")) {
      if (!assign_text("collation_uuid=", &collation_seen,
                       &collation_uuid)) {
        return std::nullopt;
      }
      fields.collation_uuid = std::move(collation_uuid);
    }
    std::string timezone_profile_id;
    if (field.starts_with("timezone_profile_id=")) {
      if (!assign_text("timezone_profile_id=", &timezone_seen,
                       &timezone_profile_id)) {
        return std::nullopt;
      }
      fields.timezone_profile_id = std::move(timezone_profile_id);
    }
    const auto assign_u32 = [&](const std::string_view prefix, bool* seen,
                                std::optional<std::uint32_t>* value) {
      if (!field.starts_with(prefix)) return true;
      if (*seen || field.size() == prefix.size()) return false;
      std::uint32_t parsed = 0;
      const auto text = field.substr(prefix.size());
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), parsed);
      if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
      }
      *seen = true;
      *value = parsed;
      return true;
    };
    if (!assign_u32("width=", &width_seen, &fields.width) ||
        !assign_u32("precision=", &precision_seen, &fields.precision) ||
        !assign_u32("scale=", &scale_seen, &fields.scale)) {
      return std::nullopt;
    }
    if (field.starts_with("nullability=")) {
      if (canonical_nullable.has_value()) return std::nullopt;
      const auto value = field.substr(std::string_view("nullability=").size());
      if (value == "nullable") {
        canonical_nullable = true;
      } else if (value == "non_null") {
        canonical_nullable = false;
      } else {
        return std::nullopt;
      }
    } else if (field.starts_with("nullable=")) {
      if (storage_nullable.has_value()) return std::nullopt;
      const auto value = field.substr(std::string_view("nullable=").size());
      if (value == "true") {
        storage_nullable = true;
      } else if (value == "false") {
        storage_nullable = false;
      } else {
        return std::nullopt;
      }
    }
    if (delimiter == std::string_view::npos) break;
    offset = delimiter + 1;
  }
  if (!type_seen || !CanonicalUuidBytes(fields.type_uuid).has_value() ||
      (!canonical_nullable.has_value() && !storage_nullable.has_value()) ||
      (canonical_nullable.has_value() && storage_nullable.has_value() &&
       *canonical_nullable != *storage_nullable) ||
      (fields.scale.has_value() &&
       (!fields.precision.has_value() ||
        *fields.scale > *fields.precision))) {
    return std::nullopt;
  }
  fields.nullable = canonical_nullable.has_value() ? *canonical_nullable
                                                    : *storage_nullable;
  if (fields.nullable != projected_nullable ||
      fields.collation_uuid.has_value() !=
          !projected_collation_uuid.empty() ||
      (fields.collation_uuid.has_value() &&
       (*fields.collation_uuid != projected_collation_uuid ||
        !CanonicalUuidBytes(*fields.collation_uuid).has_value()))) {
    return std::nullopt;
  }
  return fields;
}

bool ExactGraphProjectedDescriptorCohort(
    const ipc::PublicRelationDescriptor& projection) {
  static constexpr std::array<std::string_view, 9> kNames{
      "vertex_uuid",       "edge_uuid",       "path_uuid",
      "vertex_labels",     "vertex_properties", "edge_properties",
      "direction",         "depth",           "cycle_policy"};
  static constexpr std::array<std::string_view, 9> kTypes{
      "uuid", "uuid", "uuid", "text", "text", "text", "text",
      "uint64", "text"};
  static constexpr std::array<bool, 9> kNullable{
      false, true, false, false, false, false, false, false, false};
  if (projection.columns.size() != kNames.size()) return false;
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return false;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> type_descriptor_uuids;
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto& column = projection.columns[ordinal];
    const auto fields = ParseExactProjectedDescriptor(
        column.encoded_type_descriptor, column.collation_uuid,
        column.nullable);
    const auto type_row =
        scratchbird::core::datatypes::LookupDatatypeCatalogRow(
            manifest.manifest,
            scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
                std::string(kTypes[ordinal])));
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return false;
    }
    const auto expected_type_uuid = scratchbird::core::uuid::UuidToString(
        type_row.manifest.descriptor_rows.front().descriptor_uuid.value);
    std::unordered_set<std::string> encoded_keys;
    std::size_t offset = 0;
    while (offset <= column.encoded_type_descriptor.size()) {
      const auto delimiter =
          column.encoded_type_descriptor.find(';', offset);
      const auto end = delimiter == std::string::npos
                           ? column.encoded_type_descriptor.size()
                           : delimiter;
      const auto field = std::string_view(column.encoded_type_descriptor)
                             .substr(offset, end - offset);
      const auto equal = field.find('=');
      if (field.empty() || equal == std::string_view::npos || equal == 0 ||
          equal + 1 == field.size() ||
          !encoded_keys.insert(std::string(field.substr(0, equal))).second ||
          (field.substr(0, equal) != "canonical" &&
           field.substr(0, equal) != "type_uuid" &&
           field.substr(0, equal) != "nullability" &&
           field.substr(0, equal) != "nullable")) {
        return false;
      }
      if (field.substr(0, equal) == "canonical" &&
          field.substr(equal + 1) != kTypes[ordinal]) {
        return false;
      }
      if (delimiter == std::string::npos) break;
      offset = delimiter + 1;
    }
    if (!encoded_keys.contains("type_uuid") ||
        (encoded_keys.contains("nullability") ==
         encoded_keys.contains("nullable")) ||
        !CanonicalUuidBytes(column.column_uuid).has_value() ||
        !column_uuids.insert(column.column_uuid).second ||
        !CanonicalUuidBytes(column.type_descriptor_uuid).has_value() ||
        !type_descriptor_uuids.insert(column.type_descriptor_uuid).second ||
        column.ordinal != ordinal ||
        column.canonical_name_key != kNames[ordinal] ||
        column.type_descriptor_kind != "canonical_type_descriptor" ||
        column.canonical_type_name != kTypes[ordinal] ||
        column.nullable != kNullable[ordinal] || column.generated ||
        column.identity_column || !column.charset_uuid.empty() ||
        !column.charset_canonical_name.empty() ||
        !column.collation_uuid.empty() ||
        !column.collation_canonical_name.empty() ||
        column.character_length != 0 || column.charset_min_bytes != 0 ||
        column.charset_max_bytes != 0 || column.charset_variable_width ||
        !fields.has_value() || fields->nullable != kNullable[ordinal] ||
        fields->type_uuid != expected_type_uuid ||
        fields->collation_uuid.has_value() ||
        fields->timezone_profile_id.has_value() || fields->width.has_value() ||
        fields->precision.has_value() || fields->scale.has_value()) {
      return false;
    }
  }
  return true;
}

bool ExactKeyValueStorageDescriptorCohort(
    const ipc::PublicRelationDescriptor& projection) {
  // QOW-SOURCE-RCP-075-KEY-VALUE-STORAGE-DESCRIPTOR-V1
  static constexpr std::array<std::string_view, 3> kNames{
      "key", "value", "expires_at"};
  static constexpr std::array<std::string_view, 3> kTypes{
      "text", "text", "timestamp_tz"};
  static constexpr std::array<bool, 3> kNullable{false, false, true};
  if (projection.columns.size() != kNames.size()) return false;
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return false;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> descriptor_uuids;
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto& column = projection.columns[ordinal];
    const auto fields = ParseExactProjectedDescriptor(
        column.encoded_type_descriptor, column.collation_uuid,
        column.nullable);
    const auto type_row = scratchbird::core::datatypes::LookupDatatypeCatalogRow(
        manifest.manifest,
        scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
            std::string(kTypes[ordinal])));
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return false;
    }
    const auto expected_type_uuid = scratchbird::core::uuid::UuidToString(
        type_row.manifest.descriptor_rows.front().descriptor_uuid.value);
    if (column.ordinal != ordinal ||
        column.canonical_name_key != kNames[ordinal] ||
        column.canonical_type_name != kTypes[ordinal] ||
        column.type_descriptor_kind != "canonical_type_descriptor" ||
        column.nullable != kNullable[ordinal] || column.generated ||
        column.identity_column ||
        !CanonicalUuidBytes(column.column_uuid).has_value() ||
        !column_uuids.insert(column.column_uuid).second ||
        !CanonicalUuidBytes(column.type_descriptor_uuid).has_value() ||
        !descriptor_uuids.insert(column.type_descriptor_uuid).second ||
        !fields.has_value() || fields->type_uuid != expected_type_uuid ||
        fields->nullable != kNullable[ordinal] ||
        fields->collation_uuid.has_value() || fields->width.has_value() ||
        fields->precision.has_value() || fields->scale.has_value() ||
        !column.charset_uuid.empty() ||
        !column.charset_canonical_name.empty() ||
        !column.collation_uuid.empty() ||
        !column.collation_canonical_name.empty() ||
        column.character_length != 0 || column.charset_min_bytes != 0 ||
        column.charset_max_bytes != 0 || column.charset_variable_width) {
      return false;
    }
    if (ordinal != 2 && fields->timezone_profile_id.has_value()) return false;
  }
  return true;
}

bool ExactTimeSeriesStorageDescriptorCohort(
    const ipc::PublicRelationDescriptor& projection) {
  // QOW-SOURCE-RCP-076-TIME-SERIES-STORAGE-DESCRIPTOR-V1
  static constexpr std::array<std::string_view, 4> kNames{
      "metric_uuid", "point_timestamp", "tags", "value"};
  static constexpr std::array<std::string_view, 4> kTypes{
      "uuid", "timestamp_tz", "text", "real64"};
  if (projection.columns.size() != kNames.size()) return false;
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return false;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> descriptor_uuids;
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto& column = projection.columns[ordinal];
    const auto fields = ParseExactProjectedDescriptor(
        column.encoded_type_descriptor, column.collation_uuid,
        column.nullable);
    const auto type_row = scratchbird::core::datatypes::LookupDatatypeCatalogRow(
        manifest.manifest,
        scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
            std::string(kTypes[ordinal])));
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return false;
    }
    const auto expected_type_uuid = scratchbird::core::uuid::UuidToString(
        type_row.manifest.descriptor_rows.front().descriptor_uuid.value);
    if (column.ordinal != ordinal ||
        column.canonical_name_key != kNames[ordinal] ||
        column.canonical_type_name != kTypes[ordinal] ||
        column.type_descriptor_kind != "canonical_type_descriptor" ||
        column.nullable || column.generated || column.identity_column ||
        !CanonicalUuidBytes(column.column_uuid).has_value() ||
        !column_uuids.insert(column.column_uuid).second ||
        !CanonicalUuidBytes(column.type_descriptor_uuid).has_value() ||
        !descriptor_uuids.insert(column.type_descriptor_uuid).second ||
        !fields.has_value() || fields->type_uuid != expected_type_uuid ||
        fields->nullable || fields->collation_uuid.has_value() ||
        fields->width.has_value() || fields->precision.has_value() ||
        fields->scale.has_value() || !column.charset_uuid.empty() ||
        !column.charset_canonical_name.empty() ||
        !column.collation_uuid.empty() ||
        !column.collation_canonical_name.empty() ||
        column.character_length != 0 || column.charset_min_bytes != 0 ||
        column.charset_max_bytes != 0 || column.charset_variable_width ||
        ((ordinal == 1) != fields->timezone_profile_id.has_value())) {
      return false;
    }
  }
  return true;
}

bool ExactTimeSeriesPreResolutionAst(
    const NativeRelationalAstDocument& ast) {
  const auto source = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& candidate) {
        return candidate.source_kind ==
               NativeRelationSourceAstKind::kTimeSeries;
      });
  if (source == ast.catalog_relation_sources.end()) return true;
  const bool bucket = source->model_bucket_expression_id.has_value();
  if (bucket != source->model_bucket_interval_expression_id.has_value() ||
      bucket != source->model_bucket_time_input_expression_id.has_value()) {
    return false;
  }
  if (!bucket) return true;
  const auto expression_for = [&](const std::uint32_t id) {
    return std::ranges::find_if(ast.expressions, [&](const auto& expression) {
      return expression.expression_id == id;
    });
  };
  const auto operation = expression_for(*source->model_bucket_expression_id);
  const auto interval =
      expression_for(*source->model_bucket_interval_expression_id);
  const auto input =
      expression_for(*source->model_bucket_time_input_expression_id);
  return operation != ast.expressions.end() &&
         interval != ast.expressions.end() && input != ast.expressions.end() &&
         operation->expression_kind ==
             NativeExpressionAstKind::kFunctionCall &&
         operation->operator_name == "TIME_BUCKET" &&
         operation->child_expression_ids ==
             std::vector<std::uint32_t>{
                 *source->model_bucket_interval_expression_id,
                 *source->model_bucket_time_input_expression_id} &&
         interval->expression_kind == NativeExpressionAstKind::kLiteral &&
         interval->literal_kind == NativeLiteralAstKind::kTemporal &&
         input->expression_kind == NativeExpressionAstKind::kIdentifier;
}

std::optional<NativeRelationalBindingContext>
BuildEngineProjectedNativeBindingContext(
    const NativeRelationalAstDocument& ast,
    const ParserStatementContext& statement_context,
    const std::vector<ResolvedObjectReferenceSeed>& resolved_object_reference_seeds,
    MessageVectorSet* messages) {
  const auto fail = [&](std::string detail)
      -> std::optional<NativeRelationalBindingContext> {
    messages->diagnostics.push_back(MakeDiagnostic(
        "SBSQL.NATIVE_BINDING.CONTEXT_INVALID", "ERROR",
        "The engine-issued native binding cohort cannot cover this typed AST.",
        "sbp_sbsql.wire", {{"detail", std::move(detail)}}));
    return std::nullopt;
  };
  if (!ast.accepted() || !statement_context.complete() ||
      statement_context.bound_ast_uuid.empty() ||
      statement_context.count_function_uuid.empty() ||
      statement_context.sum_function_uuid.empty() ||
      statement_context.avg_function_uuid.empty() ||
      statement_context.min_function_uuid.empty() ||
      statement_context.max_function_uuid.empty() ||
      statement_context.aggregate_function_profiles.size() != 43 ||
      statement_context.descriptor_profiles.empty()) {
    return fail("incomplete_statement_context");
  }
  NativeRelationalBindingContext context;
  context.bound_ast_uuid = statement_context.bound_ast_uuid;
  context.catalog_epoch_uuid = statement_context.catalog_epoch_uuid;
  context.security_context_uuid = statement_context.security_context_uuid;
  context.statement_uuid = statement_context.statement_uuid;
  context.statement_timestamp = statement_context.statement_timestamp;
  context.owning_transaction_uuid =
      statement_context.transaction.transaction_uuid;
  context.statement_snapshot_uuid =
      statement_context.statement_snapshot_uuid;
  context.statement_metadata_snapshot_uuid =
      statement_context.statement_metadata_snapshot_uuid;
  context.local_transaction_id =
      statement_context.transaction.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      statement_context.snapshot_visible_through_local_transaction_id;
  context.engine_statement_authority = {
      context.statement_uuid,
      context.statement_timestamp,
      context.owning_transaction_uuid,
      context.statement_snapshot_uuid,
      context.statement_metadata_snapshot_uuid,
      context.catalog_epoch_uuid,
      context.local_transaction_id,
      context.snapshot_visible_through_local_transaction_id};

  const auto time_series_source = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kTimeSeries;
      });
  if (time_series_source != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-076-ENGINE-TIME-SERIES-BINDING-COHORT-V1
    const auto refuse = [&](const char* diagnostic, std::string detail)
        -> std::optional<NativeRelationalBindingContext> {
      messages->diagnostics.push_back(MakeDiagnostic(
          diagnostic, "ERROR", std::move(detail), "sbp_sbsql.wire"));
      return std::nullopt;
    };
    const bool range_read =
        time_series_source->model_operation_id == "TIME_SERIES_RANGE_READ";
    const bool downsample =
        time_series_source->model_operation_id == "TIME_SERIES_DOWNSAMPLE";
    const bool bucket_projection =
        time_series_source->model_bucket_expression_id.has_value();
    if (!IsCanonicalStatementTimestamp(context.statement_timestamp)) {
      return refuse("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                    "time-series source requires the canonical engine timestamp");
    }
    if (!statement_context.native_v7_complete()) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "time-series source requires the complete V7 statement cohort");
    }
    if ((!range_read && !downsample) ||
        ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{time_series_source->source_id} ||
        ast.relations.front().predicate_expression_ids !=
            std::vector<std::uint32_t>{
                time_series_source->model_range_expression_id.value_or(0)} ||
        ast.relations.front().output_expression_ids.empty() ||
        time_series_source->source_id == 0 ||
        time_series_source->model_family_id != "time_series" ||
        time_series_source->qualified_name.empty() ||
        !time_series_source->alias.has_value() ||
        !time_series_source->model_time_series_alias_expression_id.has_value() ||
        !time_series_source->model_range_expression_id.has_value() ||
        !time_series_source->model_range_start_expression_id.has_value() ||
        !time_series_source->model_range_end_expression_id.has_value() ||
        bucket_projection !=
            time_series_source->model_bucket_interval_expression_id.has_value() ||
        bucket_projection !=
            time_series_source->model_bucket_time_input_expression_id.has_value() ||
        (downsample &&
         (!time_series_source->model_downsample_expression_id.has_value() ||
          !time_series_source->model_interval_expression_id.has_value() ||
          !time_series_source->model_time_input_expression_id.has_value())) ||
        ast.model_object_resolution_requests.size() != 1 ||
        resolved_object_reference_seeds.size() != 1) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "time-series AST or binding cohort shape is invalid");
    }
    const auto encoded_source_name =
        EncodeQualifiedPresentedName(time_series_source->qualified_name);
    const auto& object_request = ast.model_object_resolution_requests.front();
    const auto& seed = resolved_object_reference_seeds.front();
    const auto same_qualified_name = [](const auto& left, const auto& right) {
      return left.size() == right.size() &&
             std::ranges::equal(left, right, [](const auto& a, const auto& b) {
               return a.spelling == b.spelling && a.quoted == b.quoted;
             });
    };
    if (!encoded_source_name.has_value() ||
        object_request.source_id != time_series_source->source_id ||
        object_request.model_family_id != "time_series" ||
        object_request.object_class != "time_series" ||
        !same_qualified_name(object_request.qualified_name,
                             time_series_source->qualified_name) ||
        seed.ref.object_class != "time_series" || seed.ref.quoted ||
        seed.ref.create_reservation ||
        seed.ref.presented_name != *encoded_source_name) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "time-series object-resolution request was substituted");
    }
    const auto& resolved = seed.resolved;
    const auto& storage_projection = resolved.relation_descriptor;
    const bool relation_transport_class =
        resolved.object_class == "relation" || resolved.object_class == "table";
    if (!resolved.resolved || !relation_transport_class ||
        !CanonicalUuidBytes(resolved.object_uuid).has_value() ||
        resolved.catalog_epoch == 0 || resolved.security_epoch == 0 ||
        !storage_projection.present ||
        !CanonicalUuidBytes(storage_projection.descriptor_uuid).has_value() ||
        !CanonicalUuidBytes(storage_projection.relation_uuid).has_value() ||
        storage_projection.relation_uuid != resolved.object_uuid ||
        !CanonicalUuidBytes(storage_projection.schema_uuid).has_value() ||
        storage_projection.descriptor_generation == 0 ||
        storage_projection.validated_resource_epoch == 0 ||
        !ExactTimeSeriesStorageDescriptorCohort(storage_projection)) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "time-series persistent storage descriptor is incomplete");
    }
    const auto find_expression = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    const auto* range =
        find_expression(*time_series_source->model_range_expression_id);
    const auto* alias = find_expression(
        *time_series_source->model_time_series_alias_expression_id);
    const auto* bucket =
        bucket_projection
            ? find_expression(*time_series_source->model_bucket_expression_id)
            : nullptr;
    if (range == nullptr || alias == nullptr ||
        range->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        range->operator_name != "TIME_RANGE" ||
        range->child_expression_ids !=
            std::vector<std::uint32_t>{
                *time_series_source->model_time_series_alias_expression_id,
                *time_series_source->model_range_start_expression_id,
                *time_series_source->model_range_end_expression_id} ||
        alias->expression_kind != NativeExpressionAstKind::kIdentifier ||
        alias->qualified_identifier.size() != 1 ||
        !SameIdentifierComponent(alias->qualified_identifier.front(),
                                 *time_series_source->alias)) {
      return refuse("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                    "TIME_RANGE alias or child order is invalid");
    }
    if (bucket_projection &&
        (bucket == nullptr ||
         bucket->expression_kind != NativeExpressionAstKind::kFunctionCall ||
         bucket->operator_name != "TIME_BUCKET" ||
         bucket->child_expression_ids !=
             std::vector<std::uint32_t>{
                 *time_series_source->model_bucket_interval_expression_id,
                 *time_series_source->model_bucket_time_input_expression_id})) {
      return refuse("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                    "TIME_BUCKET child identities were substituted");
    }

    const auto datatype_manifest =
        scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
    if (!datatype_manifest.ok()) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "canonical datatype catalog is unavailable");
    }
    std::unordered_set<std::string> descriptor_uuids;
    const auto canonical_type_uuid = [&](const std::string& type)
        -> std::optional<std::string> {
      const auto row = scratchbird::core::datatypes::LookupDatatypeCatalogRow(
          datatype_manifest.manifest,
          scratchbird::core::datatypes::CanonicalTypeIdFromStableName(type));
      if (!row.ok() || row.manifest.descriptor_rows.size() != 1 ||
          !row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
        return std::nullopt;
      }
      return scratchbird::core::uuid::UuidToString(
          row.manifest.descriptor_rows.front().descriptor_uuid.value);
    };
    const auto uuid_type_uuid = canonical_type_uuid("uuid");
    if (!uuid_type_uuid.has_value() ||
        !descriptor_uuids.insert(storage_projection.descriptor_uuid).second) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "time-series row identity descriptor is unavailable");
    }
    context.descriptors.push_back(
        {1, storage_projection.descriptor_uuid, *uuid_type_uuid,
         BoundNullability::kNonNull, std::nullopt, std::nullopt, {}});
    if (!descriptor_uuids.insert(storage_projection.schema_uuid).second) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "time-series series descriptor identity is duplicated");
    }
    context.descriptors.push_back(
        {2, storage_projection.schema_uuid, *uuid_type_uuid,
         BoundNullability::kNonNull, std::nullopt, std::nullopt, {}});

    NativeCatalogRelationBindingInput catalog_relation;
    catalog_relation.source_id = time_series_source->source_id;
    catalog_relation.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    catalog_relation.object_uuid = resolved.object_uuid;
    catalog_relation.resolved_object_type = "time_series";
    catalog_relation.resolved_schema_uuid = storage_projection.schema_uuid;
    catalog_relation.catalog_generation_id = resolved.catalog_epoch;
    catalog_relation.security_epoch = resolved.security_epoch;
    catalog_relation.resource_epoch =
        storage_projection.validated_resource_epoch;
    std::unordered_map<std::string, std::uint32_t> descriptor_by_name;
    std::unordered_map<std::string, std::string> column_uuid_by_name;
    descriptor_by_name.emplace("row_uuid", 1);
    column_uuid_by_name.emplace("row_uuid", storage_projection.descriptor_uuid);
    descriptor_by_name.emplace("series_uuid", 2);
    column_uuid_by_name.emplace("series_uuid", storage_projection.schema_uuid);
    catalog_relation.columns.push_back(
        {0, storage_projection.descriptor_uuid, 1, "row_uuid"});
    catalog_relation.columns.push_back(
        {1, storage_projection.schema_uuid, 2, "series_uuid"});
    for (std::size_t storage_ordinal = 0;
         storage_ordinal < storage_projection.columns.size();
         ++storage_ordinal) {
      const auto& column = storage_projection.columns[storage_ordinal];
      const auto fields = ParseExactProjectedDescriptor(
          column.encoded_type_descriptor, column.collation_uuid,
          column.nullable);
      if (!fields.has_value() ||
          !descriptor_uuids.insert(column.type_descriptor_uuid).second) {
        return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "time-series public descriptor is incomplete");
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = column.type_descriptor_uuid;
      descriptor.type_uuid = fields->type_uuid;
      descriptor.nullability = BoundNullability::kNonNull;
      descriptor.timezone_profile_id = fields->timezone_profile_id;
      context.descriptors.push_back(std::move(descriptor));
      const auto descriptor_id = context.descriptors.back().descriptor_id;
      descriptor_by_name.emplace(column.canonical_name_key, descriptor_id);
      column_uuid_by_name.emplace(column.canonical_name_key, column.column_uuid);
      catalog_relation.columns.push_back(
          {static_cast<std::uint32_t>(storage_ordinal + 2), column.column_uuid,
           descriptor_id, column.canonical_name_key});
    }
    context.catalog_relations.push_back(std::move(catalog_relation));
    const auto add_canonical_descriptor = [&](const std::string& type,
                                              const bool timezone)
        -> std::optional<std::uint32_t> {
      const auto type_uuid = canonical_type_uuid(type);
      if (!type_uuid.has_value() ||
          !descriptor_uuids.insert(*type_uuid).second) {
        return std::nullopt;
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = *type_uuid;
      descriptor.type_uuid = *type_uuid;
      descriptor.nullability = BoundNullability::kNonNull;
      if (timezone) descriptor.timezone_profile_id = "UTC";
      context.descriptors.push_back(std::move(descriptor));
      return context.descriptors.back().descriptor_id;
    };
    const auto boolean_descriptor_id =
        add_canonical_descriptor("boolean", false);
    const auto interval_descriptor_id =
        time_series_source->model_interval_expression_id.has_value()
            ? add_canonical_descriptor("interval", false)
            : std::optional<std::uint32_t>{};
    const auto count_descriptor_id =
        downsample ? add_canonical_descriptor("int64", false)
                   : std::optional<std::uint32_t>{};
    const auto add_derived_descriptor = [&](const std::string& descriptor_uuid,
                                            const std::uint32_t type_source_id,
                                            const bool timezone)
        -> std::optional<std::uint32_t> {
      const auto source = std::ranges::find_if(
          context.descriptors, [&](const auto& descriptor) {
            return descriptor.descriptor_id == type_source_id;
          });
      if (source == context.descriptors.end() ||
          !descriptor_uuids.insert(descriptor_uuid).second) {
        return std::nullopt;
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = descriptor_uuid;
      descriptor.type_uuid = source->type_uuid;
      descriptor.nullability = BoundNullability::kNonNull;
      if (timezone) descriptor.timezone_profile_id = "UTC";
      context.descriptors.push_back(std::move(descriptor));
      return context.descriptors.back().descriptor_id;
    };
    const auto bucket_end_descriptor_id =
        downsample
            ? add_derived_descriptor(storage_projection.columns[1].column_uuid,
                                     descriptor_by_name.at("point_timestamp"),
                                     true)
            : std::optional<std::uint32_t>{};
    const auto count_aggregate_descriptor_id =
        downsample &&
                time_series_source->model_time_series_aggregate_id == "COUNT" &&
                count_descriptor_id.has_value()
            ? add_derived_descriptor(storage_projection.columns[3].column_uuid,
                                     *count_descriptor_id, false)
            : std::optional<std::uint32_t>{};
    if (!boolean_descriptor_id.has_value() ||
        (time_series_source->model_interval_expression_id.has_value() &&
         !interval_descriptor_id.has_value()) ||
        (downsample &&
         (!count_descriptor_id.has_value() ||
          !bucket_end_descriptor_id.has_value() ||
          (time_series_source->model_time_series_aggregate_id == "COUNT" &&
           !count_aggregate_descriptor_id.has_value())))) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "time-series operation descriptors are unavailable");
    }
    const auto point_descriptor_id = descriptor_by_name.at("point_timestamp");
    const auto value_descriptor_id = descriptor_by_name.at("value");
    const auto tags_descriptor_id = descriptor_by_name.at("tags");
    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto* expression = find_expression(expression_id);
          return expression != nullptr &&
                 expression->expression_kind == NativeExpressionAstKind::kWildcard;
        });
    if (wildcard_projection &&
        (downsample || ast.relations.front().output_expression_ids.size() != 1)) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "time-series wildcard projection is invalid");
    }
    std::unordered_set<std::uint32_t> expression_ids;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_id == 0 ||
          !expression_ids.insert(expression.expression_id).second) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series expression identity is duplicated");
      }
    }
    std::uint32_t next_expression_id = expression_ids.empty()
                                           ? 1
                                           : *std::ranges::max_element(
                                                 expression_ids) +
                                                 1;
    if (wildcard_projection) {
      for (const auto& column : context.catalog_relations.front().columns) {
        context.expressions.push_back(
            {next_expression_id++, column.descriptor_id, std::nullopt,
             column.column_uuid});
        context.outputs.push_back(
            {static_cast<std::uint32_t>(context.outputs.size() + 1),
             context.expressions.back().expression_id,
             column.canonical_name_key, column.descriptor_id, true,
             static_cast<std::uint32_t>(context.outputs.size()),
             ast.relations.front().relation_id});
      }
    }
    std::unordered_map<std::uint32_t, NativeExpressionBindingInput>
        binding_by_ast;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      std::optional<std::uint32_t> descriptor_id;
      std::optional<std::string> bound_name_uuid;
      if (expression.expression_id ==
          *time_series_source->model_time_series_alias_expression_id) {
        descriptor_id = descriptor_by_name.at("row_uuid");
        bound_name_uuid = resolved.object_uuid;
      } else if (expression.expression_id ==
                     *time_series_source->model_range_start_expression_id ||
                 expression.expression_id ==
                     *time_series_source->model_range_end_expression_id) {
        descriptor_id = point_descriptor_id;
      } else if (time_series_source->model_interval_expression_id ==
                     expression.expression_id ||
                 (expression.expression_kind ==
                      NativeExpressionAstKind::kLiteral &&
                  expression.literal_kind == NativeLiteralAstKind::kTemporal)) {
        descriptor_id = interval_descriptor_id;
      } else if (expression.expression_kind == NativeExpressionAstKind::kIdentifier &&
                 !expression.qualified_identifier.empty() &&
                 expression.qualified_identifier.size() <= 2) {
        if (expression.qualified_identifier.size() == 2 &&
            !SameIdentifierComponent(expression.qualified_identifier.front(),
                                     *time_series_source->alias)) {
          return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "time-series projection qualifier is invalid");
        }
        const auto name =
            CanonicalColumnLookupKey(expression.qualified_identifier.back());
        const auto descriptor = descriptor_by_name.find(name);
        if (descriptor != descriptor_by_name.end()) {
          descriptor_id = descriptor->second;
          bound_name_uuid = column_uuid_by_name.at(name);
        }
      } else if (expression.expression_kind == NativeExpressionAstKind::kLiteral &&
                 expression.literal_kind == NativeLiteralAstKind::kString &&
                 (expression.spelling == "COUNT" || expression.spelling == "SUM" ||
                  expression.spelling == "MIN" || expression.spelling == "MAX" ||
                  expression.spelling == "AVG")) {
        descriptor_id = tags_descriptor_id;
      } else if (expression.expression_kind ==
                 NativeExpressionAstKind::kFunctionCall) {
        if (expression.operator_name == "TIME_RANGE") {
          descriptor_id = boolean_descriptor_id;
        } else if (expression.operator_name == "TIME_BUCKET") {
          descriptor_id = point_descriptor_id;
        } else if (expression.operator_name == "TIME_DOWNSAMPLE") {
          descriptor_id =
              time_series_source->model_time_series_aggregate_id == "COUNT"
                  ? count_aggregate_descriptor_id
                  : std::optional<std::uint32_t>{value_descriptor_id};
        }
      }
      if (!descriptor_id.has_value()) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "time-series expression profile is unsupported");
      }
      NativeExpressionBindingInput binding{
          expression.expression_id, *descriptor_id, std::nullopt,
          std::move(bound_name_uuid)};
      context.expressions.push_back(binding);
      binding_by_ast.emplace(expression.expression_id, std::move(binding));
    }
    if (!downsample &&
        time_series_source->model_bucket_expression_id.has_value()) {
      static constexpr std::array<std::string_view, 4> kNames{
          "series_uuid", "metric_uuid", "tags", "value"};
      for (const auto name : kNames) {
        context.expressions.push_back(
            {next_expression_id++, descriptor_by_name.at(std::string(name)),
             std::nullopt, column_uuid_by_name.at(std::string(name))});
      }
    }
    if (downsample) {
      // The scalar TIME_DOWNSAMPLE expression selects the provider operation;
      // it is not itself the public row shape.  Publish the immutable
      // seven-field section-7 descriptor using only engine-projected
      // identities already present in the persistent relation cohort.
      static constexpr std::array<std::string_view, 7> kNames{
          "series_uuid", "metric_uuid", "bucket_start", "bucket_end",
          "tags", "sample_count", "aggregate_value"};
      const std::array<std::uint32_t, 7> descriptor_ids{
          descriptor_by_name.at("series_uuid"),
          descriptor_by_name.at("metric_uuid"), point_descriptor_id,
          *bucket_end_descriptor_id, tags_descriptor_id, *count_descriptor_id,
          time_series_source->model_time_series_aggregate_id == "COUNT"
              ? *count_aggregate_descriptor_id
              : value_descriptor_id};
      const std::array<std::string, 7> bound_names{
          column_uuid_by_name.at("series_uuid"),
          column_uuid_by_name.at("metric_uuid"),
          column_uuid_by_name.at("point_timestamp"),
          column_uuid_by_name.at("point_timestamp"),
          column_uuid_by_name.at("tags"), resolved.object_uuid,
          time_series_source->model_time_series_aggregate_id == "COUNT"
              ? resolved.object_uuid
              : column_uuid_by_name.at("value")};
      for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
        context.expressions.push_back(
            {next_expression_id++, descriptor_ids[ordinal], std::nullopt,
             bound_names[ordinal]});
        context.outputs.push_back(
            {static_cast<std::uint32_t>(ordinal + 1),
             context.expressions.back().expression_id,
             std::string(kNames[ordinal]), descriptor_ids[ordinal], true,
             static_cast<std::uint32_t>(ordinal),
             ast.relations.front().relation_id});
      }
    } else if (!wildcard_projection) {
      for (std::size_t ordinal = 0;
           ordinal < ast.relations.front().output_expression_ids.size();
           ++ordinal) {
        const auto expression_id =
            ast.relations.front().output_expression_ids[ordinal];
        const auto binding = binding_by_ast.find(expression_id);
        const auto* expression = find_expression(expression_id);
        if (binding == binding_by_ast.end() || expression == nullptr) {
          return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "time-series projection binding is missing");
        }
        std::string output_name =
            "time_series_value_" + std::to_string(ordinal + 1);
        if (expression->expression_kind == NativeExpressionAstKind::kIdentifier &&
            !expression->qualified_identifier.empty()) {
          output_name = CanonicalColumnLookupKey(
              expression->qualified_identifier.back());
        } else if (expression->operator_name == "TIME_BUCKET") {
          output_name = "bucket_start";
        } else if (expression->operator_name == "TIME_DOWNSAMPLE") {
          output_name = "aggregate_value";
        }
        context.outputs.push_back(
            {static_cast<std::uint32_t>(ordinal + 1),
             binding->second.expression_id, output_name,
             binding->second.descriptor_id, true,
             static_cast<std::uint32_t>(ordinal),
             ast.relations.front().relation_id});
      }
    }
    if (context.outputs.empty()) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "time-series public projection is empty");
    }
    context.relations.push_back(
        {ast.relations.front().relation_id,
         downsample ? "sblr.model-aggregate.time-series-downsample.v1"
                    : "sblr.model-source.time-series-range-read.v1"});
    return context;
  }

  const auto key_value_source = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kKeyValue;
      });
  if (key_value_source != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-075-ENGINE-KEY-VALUE-BINDING-COHORT-V1
    const auto refuse = [&](const char* diagnostic, std::string detail)
        -> std::optional<NativeRelationalBindingContext> {
      messages->diagnostics.push_back(MakeDiagnostic(
          diagnostic, "ERROR", std::move(detail), "sbp_sbsql.wire"));
      return std::nullopt;
    };
    if (!IsCanonicalStatementTimestamp(context.statement_timestamp)) {
      return refuse(
          "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1",
          "key/value source requires the exact canonical engine-issued "
          "statement timestamp");
    }
    if (!statement_context.native_v7_complete()) {
      return refuse(
          "SB_MODEL_BINDING_INCOMPLETE_V1",
          "key/value source requires the complete V7 native statement "
          "context cohort");
    }
    const bool exact_get =
        key_value_source->model_operation_id == "KEY_VALUE_GET";
    const bool multi_get =
        key_value_source->model_operation_id == "KEY_VALUE_MULTI_GET";
    const bool prefix =
        key_value_source->model_operation_id == "KEY_VALUE_PREFIX_RANGE";
    if ((!exact_get && !multi_get && !prefix) ||
        ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{key_value_source->source_id} ||
        ast.relations.front().predicate_expression_ids.size() != 1 ||
        ast.relations.front().output_expression_ids.empty() ||
        key_value_source->source_id == 0 ||
        key_value_source->model_family_id != "key_value" ||
        key_value_source->qualified_name.empty() ||
        !key_value_source->alias.has_value() ||
        key_value_source->model_key_expression_ids.empty() ||
        (!multi_get && key_value_source->model_key_expression_ids.size() != 1) ||
        ((exact_get && key_value_source->model_comparison_operator != "=") ||
         (!exact_get && !key_value_source->model_comparison_operator.empty())) ||
        ast.model_object_resolution_requests.size() != 1 ||
        resolved_object_reference_seeds.size() != 1) {
      return refuse(exact_get ? "SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1"
                              : "SB_MODEL_BINDING_INCOMPLETE_V1",
                    "key/value AST or binding cohort shape is invalid");
    }
    const auto encoded_source_name =
        EncodeQualifiedPresentedName(key_value_source->qualified_name);
    const auto& object_request = ast.model_object_resolution_requests.front();
    const auto& seed = resolved_object_reference_seeds.front();
    const auto same_qualified_name = [](const auto& left, const auto& right) {
      return left.size() == right.size() &&
             std::ranges::equal(left, right, [](const auto& a, const auto& b) {
               return a.spelling == b.spelling && a.quoted == b.quoted;
             });
    };
    if (!encoded_source_name.has_value() ||
        object_request.source_id != key_value_source->source_id ||
        object_request.model_family_id != "key_value" ||
        object_request.object_class != "key_value" ||
        !same_qualified_name(object_request.qualified_name,
                             key_value_source->qualified_name) ||
        seed.ref.object_class != "key_value" || seed.ref.quoted ||
        seed.ref.create_reservation ||
        seed.ref.presented_name != *encoded_source_name) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "key/value object-resolution request correspondence is invalid");
    }

    const auto find_expression = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    std::unordered_set<std::uint32_t> expression_ids;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_id == 0 ||
          !expression_ids.insert(expression.expression_id).second ||
          ((expression.expression_kind == NativeExpressionAstKind::kIdentifier) !=
           !expression.qualified_identifier.empty())) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "key/value expression identity is invalid");
      }
      for (const auto child : expression.child_expression_ids) {
        if (!expression_ids.contains(child)) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "key/value expression dependency is unreachable");
        }
      }
    }
    const auto* root =
        find_expression(ast.relations.front().predicate_expression_ids.front());
    const NativeExpressionAstNode* operation = root;
    if (exact_get) {
      if (root == nullptr ||
          root->expression_kind != NativeExpressionAstKind::kBinary ||
          root->operator_name != "=" || root->child_expression_ids.size() != 2 ||
          root->child_expression_ids[1] !=
              key_value_source->model_key_expression_ids.front()) {
        return refuse("SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1",
                      "KV_KEY admits exact equality only");
      }
      operation = find_expression(root->child_expression_ids.front());
    }
    if (operation == nullptr ||
        operation->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        operation->operator_name !=
            (exact_get ? "KV_KEY" : (multi_get ? "KV_MULTI_GET" : "KV_PREFIX")) ||
        operation->child_expression_ids.empty()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "key/value functionless operation is incomplete");
    }
    const auto* alias = find_expression(operation->child_expression_ids.front());
    if (alias == nullptr ||
        alias->expression_kind != NativeExpressionAstKind::kIdentifier ||
        alias->qualified_identifier.size() != 1 ||
        !SameIdentifierComponent(alias->qualified_identifier.front(),
                                 *key_value_source->alias)) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "key/value source alias correspondence is invalid");
    }
    std::vector<std::uint32_t> expected_children{alias->expression_id};
    if (!exact_get) {
      expected_children.insert(expected_children.end(),
                               key_value_source->model_key_expression_ids.begin(),
                               key_value_source->model_key_expression_ids.end());
    }
    std::unordered_set<std::uint32_t> unique_children(
        operation->child_expression_ids.begin(),
        operation->child_expression_ids.end());
    std::unordered_set<std::uint32_t> unique_key_nodes(
        key_value_source->model_key_expression_ids.begin(),
        key_value_source->model_key_expression_ids.end());
    if (operation->child_expression_ids != expected_children ||
        unique_children.size() != operation->child_expression_ids.size() ||
        unique_key_nodes.size() !=
            key_value_source->model_key_expression_ids.size()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "key/value typed-DAG child identity is duplicated or reordered");
    }
    for (const auto expression_id : key_value_source->model_key_expression_ids) {
      const auto* expression = find_expression(expression_id);
      if (expression == nullptr ||
          (expression->expression_kind == NativeExpressionAstKind::kLiteral &&
           expression->literal_kind != NativeLiteralAstKind::kString)) {
        return refuse("SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1",
                      "key/value input is not non-null TEXT");
      }
    }

    const auto& resolved = seed.resolved;
    const auto& storage_projection = resolved.relation_descriptor;
    const bool relation_transport_class =
        resolved.object_class == "relation" || resolved.object_class == "table";
    if (!resolved.resolved || !relation_transport_class ||
        !CanonicalUuidBytes(resolved.object_uuid).has_value() ||
        resolved.catalog_epoch == 0 || resolved.security_epoch == 0 ||
        !storage_projection.present ||
        !CanonicalUuidBytes(storage_projection.descriptor_uuid).has_value() ||
        !CanonicalUuidBytes(storage_projection.relation_uuid).has_value() ||
        storage_projection.relation_uuid != resolved.object_uuid ||
        !CanonicalUuidBytes(storage_projection.schema_uuid).has_value() ||
        storage_projection.descriptor_generation == 0 ||
        storage_projection.validated_resource_epoch == 0 ||
        !ExactKeyValueStorageDescriptorCohort(storage_projection)) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "key/value persistent storage descriptor is incomplete");
    }

    NativeCatalogRelationBindingInput catalog_relation;
    catalog_relation.source_id = key_value_source->source_id;
    catalog_relation.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    catalog_relation.object_uuid = resolved.object_uuid;
    catalog_relation.resolved_object_type = "key_value";
    catalog_relation.resolved_schema_uuid = storage_projection.schema_uuid;
    catalog_relation.catalog_generation_id = resolved.catalog_epoch;
    catalog_relation.security_epoch = resolved.security_epoch;
    catalog_relation.resource_epoch =
        storage_projection.validated_resource_epoch;
    std::unordered_map<std::string, std::uint32_t> descriptor_by_name;
    std::unordered_map<std::string, std::string> column_uuid_by_name;
    std::unordered_set<std::string> descriptor_uuids;
    const auto datatype_manifest =
        scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
    if (!datatype_manifest.ok()) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "canonical datatype catalog is unavailable");
    }
    const auto uuid_type =
        scratchbird::core::datatypes::LookupDatatypeCatalogRow(
            datatype_manifest.manifest,
            scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
                "uuid"));
    if (!uuid_type.ok() ||
        uuid_type.manifest.descriptor_rows.size() != 1 ||
        !uuid_type.manifest.descriptor_rows.front().descriptor_uuid.valid() ||
        !descriptor_uuids.insert(storage_projection.descriptor_uuid).second) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "engine row-identity descriptor is unavailable");
    }
    NativeDescriptorBindingInput row_identity_descriptor;
    row_identity_descriptor.descriptor_id = 1;
    row_identity_descriptor.descriptor_uuid =
        storage_projection.descriptor_uuid;
    row_identity_descriptor.type_uuid = scratchbird::core::uuid::UuidToString(
        uuid_type.manifest.descriptor_rows.front().descriptor_uuid.value);
    row_identity_descriptor.nullability = BoundNullability::kNonNull;
    context.descriptors.push_back(std::move(row_identity_descriptor));
    descriptor_by_name.emplace("row_uuid", 1);
    // The relation UUID is the engine-issued bound-name identity for the
    // system row-identity projection. It is not a user storage column UUID.
    column_uuid_by_name.emplace("row_uuid", resolved.object_uuid);
    catalog_relation.columns.push_back(
        {0, resolved.object_uuid, 1, "row_uuid"});

    for (std::size_t storage_ordinal = 0; storage_ordinal < 2;
         ++storage_ordinal) {
      const auto& column = storage_projection.columns[storage_ordinal];
      const auto fields = ParseExactProjectedDescriptor(
          column.encoded_type_descriptor, column.collation_uuid,
          column.nullable);
      if (!fields.has_value() ||
          !descriptor_uuids.insert(column.type_descriptor_uuid).second) {
        return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "key/value public descriptor is incomplete");
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = column.type_descriptor_uuid;
      descriptor.type_uuid = fields->type_uuid;
      descriptor.nullability = fields->nullable
                                   ? BoundNullability::kNullable
                                   : BoundNullability::kNonNull;
      descriptor.collation_uuid = fields->collation_uuid;
      descriptor.timezone_profile_id = fields->timezone_profile_id;
      context.descriptors.push_back(std::move(descriptor));
      descriptor_by_name.emplace(column.canonical_name_key,
                                 context.descriptors.back().descriptor_id);
      column_uuid_by_name.emplace(column.canonical_name_key,
                                  column.column_uuid);
      catalog_relation.columns.push_back(
          {static_cast<std::uint32_t>(storage_ordinal + 1), column.column_uuid,
           context.descriptors.back().descriptor_id,
           column.canonical_name_key});
    }
    context.catalog_relations.push_back(std::move(catalog_relation));

    const auto boolean_profile = std::ranges::find_if(
        statement_context.descriptor_profiles, [](const auto& candidate) {
          return candidate.profile_kind == 6 && candidate.slot == 0;
        });
    if (boolean_profile == statement_context.descriptor_profiles.end() ||
        !boolean_profile->nullable ||
        !CanonicalUuidBytes(boolean_profile->descriptor_uuid).has_value() ||
        !CanonicalUuidBytes(boolean_profile->type_uuid).has_value() ||
        !descriptor_uuids.insert(boolean_profile->descriptor_uuid).second) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "key/value boolean operation descriptor is unavailable");
    }
    NativeDescriptorBindingInput boolean_descriptor;
    boolean_descriptor.descriptor_id =
        static_cast<std::uint32_t>(context.descriptors.size() + 1);
    boolean_descriptor.descriptor_uuid = boolean_profile->descriptor_uuid;
    boolean_descriptor.type_uuid = boolean_profile->type_uuid;
    boolean_descriptor.nullability = BoundNullability::kNullable;
    context.descriptors.push_back(std::move(boolean_descriptor));
    const auto boolean_descriptor_id = context.descriptors.back().descriptor_id;
    const auto key_descriptor_id = descriptor_by_name.at("key");

    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto* expression = find_expression(expression_id);
          return expression != nullptr &&
                 expression->expression_kind == NativeExpressionAstKind::kWildcard;
        });
    if (wildcard_projection &&
        ast.relations.front().output_expression_ids.size() != 1) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "key/value wildcard projection is not exact");
    }
    std::uint32_t next_expression_id = 1;
    if (!expression_ids.empty()) {
      next_expression_id = *std::ranges::max_element(expression_ids) + 1;
    }
    if (wildcard_projection) {
      for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
        const auto& column = context.catalog_relations.front().columns[ordinal];
        context.expressions.push_back(
            {next_expression_id++, column.descriptor_id, std::nullopt,
             column.column_uuid});
        context.outputs.push_back(
            {static_cast<std::uint32_t>(ordinal + 1),
             context.expressions.back().expression_id,
             column.canonical_name_key, column.descriptor_id, true,
             static_cast<std::uint32_t>(ordinal),
             ast.relations.front().relation_id});
      }
    }

    std::unordered_map<std::uint32_t, NativeExpressionBindingInput>
        binding_by_ast;
    const std::unordered_set<std::uint32_t> key_expression_ids(
        key_value_source->model_key_expression_ids.begin(),
        key_value_source->model_key_expression_ids.end());
    for (const auto& expression : ast.expressions) {
      if (expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      std::optional<std::uint32_t> descriptor_id;
      std::optional<std::string> bound_name_uuid;
      if (key_expression_ids.contains(expression.expression_id)) {
        descriptor_id = key_descriptor_id;
      } else if (expression.expression_kind ==
                     NativeExpressionAstKind::kIdentifier) {
        if (expression.expression_id == alias->expression_id) {
          descriptor_id = key_descriptor_id;
          bound_name_uuid = resolved.object_uuid;
        } else if (!expression.qualified_identifier.empty() &&
                   expression.qualified_identifier.size() <= 2) {
          if (expression.qualified_identifier.size() == 2 &&
              !SameIdentifierComponent(expression.qualified_identifier.front(),
                                       *key_value_source->alias)) {
            return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                          "key/value projection qualifier is invalid");
          }
          const auto column_name =
              CanonicalColumnLookupKey(expression.qualified_identifier.back());
          const auto descriptor = descriptor_by_name.find(column_name);
          if (descriptor != descriptor_by_name.end()) {
            descriptor_id = descriptor->second;
            bound_name_uuid = column_uuid_by_name.at(column_name);
          }
        }
      } else if (expression.expression_kind ==
                     NativeExpressionAstKind::kFunctionCall &&
                 (expression.operator_name == "KV_KEY" ||
                  expression.operator_name == "KV_MULTI_GET" ||
                  expression.operator_name == "KV_PREFIX")) {
        descriptor_id = expression.operator_name == "KV_KEY"
                            ? key_descriptor_id
                            : boolean_descriptor_id;
      } else if (expression.expression_kind == NativeExpressionAstKind::kBinary &&
                 expression.operator_name == "=") {
        descriptor_id = boolean_descriptor_id;
      } else if (expression.expression_kind ==
                     NativeExpressionAstKind::kParenthesized &&
                 expression.child_expression_ids.size() == 1) {
        const auto child =
            binding_by_ast.find(expression.child_expression_ids.front());
        if (child != binding_by_ast.end()) {
          descriptor_id = child->second.descriptor_id;
        }
      }
      if (!descriptor_id.has_value()) {
        return refuse(key_expression_ids.contains(expression.expression_id)
                          ? "SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1"
                          : "SB_MODEL_BINDING_INCOMPLETE_V1",
                      "key/value expression profile is unsupported");
      }
      NativeExpressionBindingInput binding{
          expression.expression_id, *descriptor_id, std::nullopt,
          std::move(bound_name_uuid)};
      context.expressions.push_back(binding);
      binding_by_ast.emplace(expression.expression_id, std::move(binding));
    }
    if (!wildcard_projection) {
      for (std::size_t ordinal = 0;
           ordinal < ast.relations.front().output_expression_ids.size();
           ++ordinal) {
        const auto expression_id =
            ast.relations.front().output_expression_ids[ordinal];
        const auto binding = binding_by_ast.find(expression_id);
        const auto* expression = find_expression(expression_id);
        if (binding == binding_by_ast.end() || expression == nullptr ||
            expression->expression_kind != NativeExpressionAstKind::kIdentifier) {
          return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "key/value projection expression is invalid");
        }
        const auto column_name =
            CanonicalColumnLookupKey(expression->qualified_identifier.back());
        if (!descriptor_by_name.contains(column_name)) {
          return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "key/value projection exposes a hidden storage field");
        }
        context.outputs.push_back(
            {static_cast<std::uint32_t>(ordinal + 1),
             binding->second.expression_id, column_name,
             binding->second.descriptor_id, true,
             static_cast<std::uint32_t>(ordinal),
             ast.relations.front().relation_id});
      }
    }
    if (context.outputs.empty()) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "key/value public projection is empty");
    }
    context.relations.push_back(
        {ast.relations.front().relation_id,
         exact_get
             ? "sblr.model-source.key-value-get.v1"
             : (multi_get
                    ? "sblr.model-source.key-value-multi-get.v1"
                    : "sblr.model-source.key-value-prefix-range.v1")});
    return context;
  }

  const auto graph_source = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kGraph;
      });
  if (graph_source != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-074-ENGINE-GRAPH-BINDING-COHORT-V1
    const bool graph_match = graph_source->model_operation_id == "GRAPH_MATCH";
    const bool graph_expand =
        graph_source->model_operation_id == "GRAPH_EXPAND";
    if ((!graph_match && !graph_expand) ||
        ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{graph_source->source_id} ||
        ast.relations.front().predicate_expression_ids.size() != 1 ||
        ast.relations.front().output_expression_ids.empty() ||
        graph_source->source_id == 0 || graph_source->model_family_id != "graph" ||
        graph_source->qualified_name.empty() || !graph_source->alias.has_value() ||
        graph_source->model_graph_cycle_policy != "visited_set" ||
        ast.model_object_resolution_requests.size() != 1 ||
        resolved_object_reference_seeds.size() != 1) {
      return fail("graph_ast_shape_invalid");
    }
    const auto encoded_source_name =
        EncodeQualifiedPresentedName(graph_source->qualified_name);
    const auto& request = ast.model_object_resolution_requests.front();
    const auto& seed = resolved_object_reference_seeds.front();
    const auto same_qualified_name = [](const auto& left, const auto& right) {
      return left.size() == right.size() &&
             std::ranges::equal(left, right, [](const auto& a, const auto& b) {
               return a.spelling == b.spelling && a.quoted == b.quoted;
             });
    };
    if (!encoded_source_name.has_value() ||
        request.source_id != graph_source->source_id ||
        request.model_family_id != "graph" || request.object_class != "graph" ||
        !same_qualified_name(request.qualified_name,
                             graph_source->qualified_name) ||
        seed.ref.object_class != "graph" || seed.ref.quoted ||
        seed.ref.create_reservation ||
        seed.ref.presented_name != *encoded_source_name) {
      return fail("graph_resolution_request_correspondence_invalid");
    }

    const auto find_expression = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    std::unordered_set<std::uint32_t> ast_expression_ids;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_id == 0 ||
          !ast_expression_ids.insert(expression.expression_id).second ||
          ((expression.expression_kind == NativeExpressionAstKind::kIdentifier) !=
           !expression.qualified_identifier.empty())) {
        return fail("graph_expression_identity_invalid");
      }
      for (const auto child : expression.child_expression_ids) {
        if (!ast_expression_ids.contains(child)) {
          return fail("graph_expression_dependency_invalid");
        }
      }
    }
    const auto* root = find_expression(
        ast.relations.front().predicate_expression_ids.front());
    const auto* alias = graph_source->model_graph_alias_expression_id.has_value()
                            ? find_expression(
                                  *graph_source->model_graph_alias_expression_id)
                            : nullptr;
    std::unordered_set<std::uint32_t> graph_child_expression_ids;
    if (root != nullptr) {
      graph_child_expression_ids.insert(root->child_expression_ids.begin(),
                                        root->child_expression_ids.end());
    }
    if (root == nullptr || alias == nullptr ||
        root->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        root->operator_name != graph_source->model_operation_id ||
        alias->expression_kind != NativeExpressionAstKind::kIdentifier ||
        alias->qualified_identifier.size() != 1 ||
        graph_child_expression_ids.size() !=
            root->child_expression_ids.size() ||
        root->child_expression_ids.empty() ||
        root->child_expression_ids.front() != alias->expression_id) {
      return fail("graph_operation_expression_correspondence_invalid");
    }
    if (graph_match) {
      const auto* pattern =
          graph_source->model_pattern_expression_id.has_value()
              ? find_expression(*graph_source->model_pattern_expression_id)
              : nullptr;
      if (pattern == nullptr ||
          pattern->expression_kind != NativeExpressionAstKind::kLiteral ||
          pattern->literal_kind != NativeLiteralAstKind::kString ||
          !ExactBoundedGraphPatternV1(pattern->spelling) ||
          root->child_expression_ids !=
              std::vector<std::uint32_t>{alias->expression_id,
                                         pattern->expression_id} ||
          !graph_source->model_graph_direction.empty() ||
          graph_source->model_graph_minimum_depth.has_value() ||
          graph_source->model_graph_maximum_depth.has_value()) {
        return fail("graph_match_operand_shape_invalid");
      }
    } else {
      if (root->child_expression_ids.size() != 5 ||
          graph_source->model_pattern_expression_id.has_value() ||
          (graph_source->model_graph_direction != "outgoing" &&
           graph_source->model_graph_direction != "incoming" &&
           graph_source->model_graph_direction != "both") ||
          !graph_source->model_graph_minimum_depth.has_value() ||
          !graph_source->model_graph_maximum_depth.has_value() ||
          *graph_source->model_graph_minimum_depth >
              *graph_source->model_graph_maximum_depth) {
        return fail("SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1");
      }
      const auto* direction = find_expression(root->child_expression_ids[1]);
      const auto* minimum = find_expression(root->child_expression_ids[2]);
      const auto* maximum = find_expression(root->child_expression_ids[3]);
      const auto* cycle = find_expression(root->child_expression_ids[4]);
      if (direction == nullptr || minimum == nullptr || maximum == nullptr ||
          cycle == nullptr ||
          direction->literal_kind != NativeLiteralAstKind::kString ||
          ToUpperAscii(direction->spelling) !=
              ToUpperAscii(graph_source->model_graph_direction) ||
          minimum->literal_kind != NativeLiteralAstKind::kNumeric ||
          minimum->spelling != std::to_string(
                                   *graph_source->model_graph_minimum_depth) ||
          maximum->literal_kind != NativeLiteralAstKind::kNumeric ||
          maximum->spelling != std::to_string(
                                   *graph_source->model_graph_maximum_depth) ||
          cycle->literal_kind != NativeLiteralAstKind::kString ||
          cycle->spelling != "visited_set") {
        return fail("graph_expand_operand_correspondence_invalid");
      }
    }

    const auto& resolved = seed.resolved;
    const auto& projection = resolved.relation_descriptor;
    const bool relation_transport_class =
        resolved.object_class == "relation" || resolved.object_class == "table";
    if (!resolved.resolved || !relation_transport_class ||
        !CanonicalUuidBytes(resolved.object_uuid).has_value() ||
        resolved.catalog_epoch == 0 || resolved.security_epoch == 0 ||
        !projection.present ||
        !CanonicalUuidBytes(projection.descriptor_uuid).has_value() ||
        !CanonicalUuidBytes(projection.relation_uuid).has_value() ||
        projection.relation_uuid != resolved.object_uuid ||
        !CanonicalUuidBytes(projection.schema_uuid).has_value() ||
        projection.descriptor_generation == 0 ||
        projection.validated_resource_epoch == 0 ||
        !ExactGraphProjectedDescriptorCohort(projection)) {
      return fail("graph_projection_authority_incomplete");
    }

    NativeCatalogRelationBindingInput catalog_relation;
    catalog_relation.source_id = graph_source->source_id;
    catalog_relation.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    catalog_relation.object_uuid = resolved.object_uuid;
    catalog_relation.resolved_object_type = "graph";
    catalog_relation.resolved_schema_uuid = projection.schema_uuid;
    catalog_relation.catalog_generation_id = resolved.catalog_epoch;
    catalog_relation.security_epoch = resolved.security_epoch;
    catalog_relation.resource_epoch = projection.validated_resource_epoch;
    std::unordered_map<std::string, std::uint32_t> column_descriptor_by_name;
    std::unordered_map<std::string, std::string> column_uuid_by_name;
    std::unordered_set<std::string> descriptor_uuids;
    std::unordered_set<std::string> column_uuids;
    for (std::size_t ordinal = 0; ordinal < projection.columns.size(); ++ordinal) {
      const auto& column = projection.columns[ordinal];
      const auto descriptor_fields = ParseExactProjectedDescriptor(
          column.encoded_type_descriptor, column.collation_uuid,
          column.nullable);
      if (column.ordinal != ordinal || column.canonical_name_key.empty() ||
          !CanonicalUuidBytes(column.column_uuid).has_value() ||
          !column_uuids.insert(column.column_uuid).second ||
          !CanonicalUuidBytes(column.type_descriptor_uuid).has_value() ||
          !descriptor_uuids.insert(column.type_descriptor_uuid).second ||
          !descriptor_fields.has_value() ||
          column_descriptor_by_name.contains(column.canonical_name_key)) {
        return fail("graph_projection_column_descriptor_invalid");
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = column.type_descriptor_uuid;
      descriptor.type_uuid = descriptor_fields->type_uuid;
      descriptor.nullability = descriptor_fields->nullable
                                   ? BoundNullability::kNullable
                                   : BoundNullability::kNonNull;
      descriptor.collation_uuid = descriptor_fields->collation_uuid;
      descriptor.timezone_profile_id = descriptor_fields->timezone_profile_id;
      descriptor.width_precision_scale.width = descriptor_fields->width;
      descriptor.width_precision_scale.precision = descriptor_fields->precision;
      descriptor.width_precision_scale.scale = descriptor_fields->scale;
      context.descriptors.push_back(std::move(descriptor));
      const auto descriptor_id = context.descriptors.back().descriptor_id;
      column_descriptor_by_name.emplace(column.canonical_name_key,
                                        descriptor_id);
      column_uuid_by_name.emplace(column.canonical_name_key,
                                  column.column_uuid);
      catalog_relation.columns.push_back(
          {static_cast<std::uint32_t>(ordinal), column.column_uuid,
           descriptor_id, column.canonical_name_key});
    }
    context.catalog_relations.push_back(std::move(catalog_relation));

    std::array<std::uint16_t, 11> next_profile_slot{};
    const auto allocate_profile_descriptor =
        [&](const std::uint8_t kind) -> std::optional<std::uint32_t> {
      if (kind == 0 || kind >= next_profile_slot.size()) return std::nullopt;
      const auto slot = next_profile_slot[kind]++;
      const auto profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == kind && candidate.slot == slot;
          });
      const bool expected_nullable = (kind % 2) == 0;
      if (profile == statement_context.descriptor_profiles.end() ||
          profile->nullable != expected_nullable ||
          !CanonicalUuidBytes(profile->descriptor_uuid).has_value() ||
          !descriptor_uuids.insert(profile->descriptor_uuid).second ||
          !CanonicalUuidBytes(profile->type_uuid).has_value() ||
          (!profile->collation_uuid.empty() &&
           !CanonicalUuidBytes(profile->collation_uuid).has_value()) ||
          profile->scale > profile->precision) {
        return std::nullopt;
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = profile->descriptor_uuid;
      descriptor.type_uuid = profile->type_uuid;
      descriptor.nullability = profile->nullable
                                   ? BoundNullability::kNullable
                                   : BoundNullability::kNonNull;
      if (!profile->collation_uuid.empty()) {
        descriptor.collation_uuid = profile->collation_uuid;
      }
      if (profile->width != 0) {
        descriptor.width_precision_scale.width = profile->width;
      }
      if (profile->precision != 0) {
        descriptor.width_precision_scale.precision = profile->precision;
        descriptor.width_precision_scale.scale = profile->scale;
      }
      context.descriptors.push_back(std::move(descriptor));
      return context.descriptors.back().descriptor_id;
    };

    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto* expression = find_expression(expression_id);
          return expression != nullptr &&
                 expression->expression_kind == NativeExpressionAstKind::kWildcard;
        });
    if (wildcard_projection &&
        ast.relations.front().output_expression_ids.size() != 1) {
      return fail("graph_wildcard_projection_shape_invalid");
    }
    std::uint32_t next_synthetic_expression_id = 1;
    if (!ast_expression_ids.empty()) {
      next_synthetic_expression_id =
          *std::ranges::max_element(ast_expression_ids) + 1;
    }
    if (wildcard_projection) {
      for (const auto& column : context.catalog_relations.front().columns) {
        context.expressions.push_back(
            {next_synthetic_expression_id++, column.descriptor_id,
             std::nullopt, column.column_uuid});
        context.outputs.push_back(
            {static_cast<std::uint32_t>(context.outputs.size() + 1),
             context.expressions.back().expression_id,
             column.canonical_name_key, column.descriptor_id, true,
             static_cast<std::uint32_t>(context.outputs.size()),
             ast.relations.front().relation_id});
      }
    }

    std::unordered_map<std::uint32_t, NativeExpressionBindingInput>
        expression_binding_by_ast;
    const auto& operation_alias =
        graph_expand && graph_source->model_source_alias.has_value()
            ? *graph_source->model_source_alias
            : *graph_source->alias;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      std::optional<std::uint32_t> descriptor_id;
      std::optional<std::string> bound_name_uuid;
      switch (expression.expression_kind) {
        case NativeExpressionAstKind::kIdentifier: {
          if (expression.qualified_identifier.empty() ||
              expression.qualified_identifier.size() > 2) {
            break;
          }
          const auto column_name =
              CanonicalColumnLookupKey(expression.qualified_identifier.back());
          const auto column = column_descriptor_by_name.find(column_name);
          if (column != column_descriptor_by_name.end()) {
            descriptor_id = column->second;
            bound_name_uuid = column_uuid_by_name.at(column_name);
          } else if (expression.qualified_identifier.size() == 1 &&
                     SameIdentifierComponent(
                         expression.qualified_identifier.front(),
                         operation_alias)) {
            descriptor_id = allocate_profile_descriptor(8);
            bound_name_uuid = context.catalog_relations.front().object_uuid;
          }
          break;
        }
        case NativeExpressionAstKind::kLiteral:
          if (expression.literal_kind == NativeLiteralAstKind::kNumeric) {
            descriptor_id = allocate_profile_descriptor(1);
          } else if (expression.literal_kind == NativeLiteralAstKind::kString) {
            descriptor_id = allocate_profile_descriptor(3);
          }
          break;
        case NativeExpressionAstKind::kFunctionCall:
          if (expression.operator_name == graph_source->model_operation_id) {
            descriptor_id = allocate_profile_descriptor(8);
          }
          break;
        case NativeExpressionAstKind::kParameter:
        case NativeExpressionAstKind::kUnary:
        case NativeExpressionAstKind::kBinary:
        case NativeExpressionAstKind::kParenthesized:
        case NativeExpressionAstKind::kWildcard:
          break;
      }
      if (!descriptor_id.has_value()) {
        return fail("graph_expression_profile_unsupported");
      }
      NativeExpressionBindingInput expression_binding{
          expression.expression_id, *descriptor_id, std::nullopt,
          std::move(bound_name_uuid)};
      context.expressions.push_back(expression_binding);
      expression_binding_by_ast.emplace(expression.expression_id,
                                        std::move(expression_binding));
    }
    if (!wildcard_projection) {
      for (std::size_t ordinal = 0;
           ordinal < ast.relations.front().output_expression_ids.size();
           ++ordinal) {
        const auto expression_id =
            ast.relations.front().output_expression_ids[ordinal];
        const auto binding = expression_binding_by_ast.find(expression_id);
        const auto* expression = find_expression(expression_id);
        if (binding == expression_binding_by_ast.end() || expression == nullptr) {
          return fail("graph_projection_binding_missing");
        }
        std::string output_name =
            "graph_value_" + std::to_string(ordinal + 1);
        if (expression->expression_kind == NativeExpressionAstKind::kIdentifier) {
          output_name = expression->spelling;
        }
        context.outputs.push_back(
            {static_cast<std::uint32_t>(ordinal + 1),
             binding->second.expression_id, output_name,
             binding->second.descriptor_id, true,
             static_cast<std::uint32_t>(ordinal),
             ast.relations.front().relation_id});
      }
    }
    if (context.outputs.empty()) return fail("graph_projection_empty");
    context.relations.push_back(
        {ast.relations.front().relation_id,
         graph_expand ? "sblr.model-expand.graph-expand.v1"
                      : "sblr.model-source.graph-match.v1"});
    return context;
  }

  const auto document_source = std::ranges::find_if(
      ast.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kDocument;
      });
  if (document_source != ast.catalog_relation_sources.end()) {
    // QOW-SOURCE-RCP-073-ENGINE-DOCUMENT-BINDING-COHORT-V1
    const bool expression_backed_unnest =
        document_source->model_operation_id == "DOCUMENT_UNNEST";
    const bool collection_backed =
        document_source->model_operation_id == "DOCUMENT_FIND" ||
        document_source->model_operation_id == "DOCUMENT_PATH";
    if ((!expression_backed_unnest && !collection_backed) ||
        ast.catalog_relation_sources.size() != 1 || ast.relations.size() != 1 ||
        ast.root_relation_id != ast.relations.front().relation_id ||
        ast.relations.front().relation_kind !=
            NativeRelationAstKind::kCatalogSource ||
        ast.relations.front().relation_source_ids !=
            std::vector<std::uint32_t>{document_source->source_id} ||
        document_source->source_id == 0 ||
        document_source->model_family_id != "document" ||
        ast.relations.front().output_expression_ids.empty()) {
      return fail("document_ast_shape_invalid");
    }

    const auto same_qualified_name = [](const auto& left, const auto& right) {
      return left.size() == right.size() &&
             std::ranges::equal(left, right, [](const auto& a, const auto& b) {
               return a.spelling == b.spelling && a.quoted == b.quoted;
             });
    };
    const auto encoded_source_name =
        EncodeQualifiedPresentedName(document_source->qualified_name);
    if (expression_backed_unnest) {
      if (!document_source->qualified_name.empty() ||
          !document_source->model_document_expression_id.has_value() ||
          !document_source->model_path_expression_id.has_value() ||
          document_source->model_value_expression_id.has_value() ||
          !document_source->model_comparison_operator.empty() ||
          !ast.model_object_resolution_requests.empty() ||
          !resolved_object_reference_seeds.empty()) {
        return fail("document_unnest_resolution_shape_invalid");
      }
    } else {
      if (!encoded_source_name.has_value() ||
          document_source->model_document_expression_id.has_value() ||
          ast.model_object_resolution_requests.size() != 1 ||
          resolved_object_reference_seeds.size() != 1) {
        return fail("document_collection_resolution_shape_invalid");
      }
      const auto& request = ast.model_object_resolution_requests.front();
      const auto& seed = resolved_object_reference_seeds.front();
      if (request.source_id != document_source->source_id ||
          request.model_family_id != "document" ||
          request.object_class != "document_collection" ||
          !same_qualified_name(request.qualified_name,
                               document_source->qualified_name) ||
          seed.ref.object_class != "document_collection" || seed.ref.quoted ||
          seed.ref.create_reservation ||
          seed.ref.presented_name != *encoded_source_name) {
        return fail("document_collection_request_correspondence_invalid");
      }
    }

    const auto find_expression = [&](const std::uint32_t expression_id)
        -> const NativeExpressionAstNode* {
      const auto found = std::ranges::find_if(
          ast.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
      return found == ast.expressions.end() ? nullptr : &*found;
    };
    std::unordered_set<std::uint32_t> ast_expression_ids;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_id == 0 ||
          !ast_expression_ids.insert(expression.expression_id).second ||
          ((expression.expression_kind ==
                NativeExpressionAstKind::kIdentifier) !=
           !expression.qualified_identifier.empty())) {
        return fail("document_expression_identity_invalid");
      }
      for (const auto child : expression.child_expression_ids) {
        if (!ast_expression_ids.contains(child)) {
          return fail("document_expression_dependency_invalid");
        }
      }
    }

    std::optional<std::uint32_t> document_path_source_expression_id;
    if (document_source->model_operation_id == "DOCUMENT_FIND") {
      if (document_source->model_path_expression_id.has_value() ||
          document_source->model_value_expression_id.has_value() ||
          !document_source->model_comparison_operator.empty() ||
          !ast.relations.front().predicate_expression_ids.empty()) {
        return fail("document_find_operand_shape_invalid");
      }
    } else if (document_source->model_operation_id == "DOCUMENT_PATH") {
      if (!document_source->model_path_expression_id.has_value() ||
          !document_source->model_value_expression_id.has_value() ||
          document_source->model_comparison_operator.empty() ||
          ast.relations.front().predicate_expression_ids.size() != 1) {
        return fail("document_path_operand_shape_invalid");
      }
      const auto* path_literal =
          find_expression(*document_source->model_path_expression_id);
      const auto* value_expression =
          find_expression(*document_source->model_value_expression_id);
      const auto* predicate =
          find_expression(ast.relations.front().predicate_expression_ids.front());
      if (path_literal == nullptr || value_expression == nullptr ||
          predicate == nullptr ||
          path_literal->expression_kind != NativeExpressionAstKind::kLiteral ||
          path_literal->literal_kind != NativeLiteralAstKind::kString ||
          predicate->expression_kind != NativeExpressionAstKind::kBinary ||
          predicate->operator_name !=
              document_source->model_comparison_operator ||
          predicate->child_expression_ids.size() != 2 ||
          predicate->child_expression_ids.back() !=
              value_expression->expression_id) {
        return fail("document_path_expression_correspondence_invalid");
      }
      const auto* path_call = find_expression(predicate->child_expression_ids[0]);
      if (path_call == nullptr ||
          path_call->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          path_call->operator_name != "DOCUMENT_PATH" ||
          path_call->child_expression_ids.size() != 2 ||
          path_call->child_expression_ids.back() != path_literal->expression_id) {
        return fail("document_path_call_correspondence_invalid");
      }
      const auto* path_source = find_expression(path_call->child_expression_ids[0]);
      if (path_source == nullptr ||
          path_source->expression_kind != NativeExpressionAstKind::kIdentifier ||
          path_source->qualified_identifier.size() != 1 ||
          !document_source->alias.has_value() ||
          !SameIdentifierComponent(path_source->qualified_identifier.front(),
                                   *document_source->alias)) {
        return fail("document_path_source_correspondence_invalid");
      }
      document_path_source_expression_id = path_source->expression_id;
    } else {
      const auto* document_expression =
          find_expression(*document_source->model_document_expression_id);
      const auto* path_literal =
          find_expression(*document_source->model_path_expression_id);
      if (document_expression == nullptr || path_literal == nullptr ||
          path_literal->expression_kind != NativeExpressionAstKind::kLiteral ||
          path_literal->literal_kind != NativeLiteralAstKind::kString ||
          !ast.relations.front().predicate_expression_ids.empty()) {
        return fail("document_unnest_operand_correspondence_invalid");
      }
    }

    std::unordered_map<std::string, std::uint32_t> column_descriptor_by_name;
    std::unordered_map<std::string, std::string> column_uuid_by_name;
    std::unordered_set<std::string> descriptor_uuids;
    NativeCatalogRelationBindingInput catalog_relation;
    if (!expression_backed_unnest) {
      const auto& resolved = resolved_object_reference_seeds.front().resolved;
      const auto& projection = resolved.relation_descriptor;
      const bool relation_transport_class =
          resolved.object_class == "relation" || resolved.object_class == "table";
      if (!resolved.resolved || !relation_transport_class ||
          !CanonicalUuidBytes(resolved.object_uuid).has_value() ||
          resolved.catalog_epoch == 0 || resolved.security_epoch == 0 ||
          !projection.present ||
          !CanonicalUuidBytes(projection.descriptor_uuid).has_value() ||
          !CanonicalUuidBytes(projection.relation_uuid).has_value() ||
          projection.relation_uuid != resolved.object_uuid ||
          !CanonicalUuidBytes(projection.schema_uuid).has_value() ||
          projection.descriptor_generation == 0 ||
          projection.validated_resource_epoch == 0 || projection.columns.empty()) {
        // In particular, V1 name resolution and a generic result without the
        // V3 persisted projection can never substitute for document binding.
        return fail("document_collection_projection_authority_incomplete");
      }
      catalog_relation.source_id = document_source->source_id;
      catalog_relation.resolution_state =
          NativeCatalogRelationResolutionState::kBound;
      catalog_relation.object_uuid = resolved.object_uuid;
      catalog_relation.resolved_object_type = "document_collection";
      catalog_relation.resolved_schema_uuid = projection.schema_uuid;
      catalog_relation.catalog_generation_id = resolved.catalog_epoch;
      catalog_relation.security_epoch = resolved.security_epoch;
      catalog_relation.resource_epoch = projection.validated_resource_epoch;

      std::unordered_set<std::string> column_uuids;
      for (std::size_t ordinal = 0; ordinal < projection.columns.size();
           ++ordinal) {
        const auto& column = projection.columns[ordinal];
        const auto descriptor_fields = ParseExactProjectedDescriptor(
            column.encoded_type_descriptor, column.collation_uuid,
            column.nullable);
        if (column.ordinal != ordinal || column.canonical_name_key.empty() ||
            !CanonicalUuidBytes(column.column_uuid).has_value() ||
            !column_uuids.insert(column.column_uuid).second ||
            !CanonicalUuidBytes(column.type_descriptor_uuid).has_value() ||
            !descriptor_uuids.insert(column.type_descriptor_uuid).second ||
            !descriptor_fields.has_value() ||
            (descriptor_fields->width.has_value() &&
             column.character_length != 0 &&
             *descriptor_fields->width != column.character_length) ||
            column_descriptor_by_name.contains(column.canonical_name_key)) {
          return fail("document_collection_column_descriptor_invalid");
        }
        NativeDescriptorBindingInput descriptor;
        descriptor.descriptor_id =
            static_cast<std::uint32_t>(context.descriptors.size() + 1);
        descriptor.descriptor_uuid = column.type_descriptor_uuid;
        descriptor.type_uuid = descriptor_fields->type_uuid;
        descriptor.nullability = descriptor_fields->nullable
                                     ? BoundNullability::kNullable
                                     : BoundNullability::kNonNull;
        descriptor.collation_uuid = descriptor_fields->collation_uuid;
        descriptor.timezone_profile_id =
            descriptor_fields->timezone_profile_id;
        descriptor.width_precision_scale.width =
            descriptor_fields->width.has_value()
                ? descriptor_fields->width
                : (column.character_length == 0
                       ? std::optional<std::uint32_t>{}
                       : std::optional(column.character_length));
        descriptor.width_precision_scale.precision =
            descriptor_fields->precision;
        descriptor.width_precision_scale.scale = descriptor_fields->scale;
        context.descriptors.push_back(std::move(descriptor));
        const auto descriptor_id = context.descriptors.back().descriptor_id;
        column_descriptor_by_name.emplace(column.canonical_name_key,
                                          descriptor_id);
        column_uuid_by_name.emplace(column.canonical_name_key,
                                    column.column_uuid);
        catalog_relation.columns.push_back(
            {static_cast<std::uint32_t>(ordinal), column.column_uuid,
             descriptor_id, column.canonical_name_key});
      }
      context.catalog_relations.push_back(std::move(catalog_relation));
    }

    std::array<std::uint16_t, 11> next_profile_slot{};
    const auto allocate_profile_descriptor =
        [&](const std::uint8_t kind) -> std::optional<std::uint32_t> {
      if (kind == 0 || kind >= next_profile_slot.size()) return std::nullopt;
      const auto slot = next_profile_slot[kind]++;
      const auto profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == kind && candidate.slot == slot;
          });
      const bool expected_nullable = (kind % 2) == 0;
      if (profile == statement_context.descriptor_profiles.end() ||
          profile->nullable != expected_nullable ||
          !CanonicalUuidBytes(profile->descriptor_uuid).has_value() ||
          !descriptor_uuids.insert(profile->descriptor_uuid).second ||
          !CanonicalUuidBytes(profile->type_uuid).has_value() ||
          (!profile->collation_uuid.empty() &&
           !CanonicalUuidBytes(profile->collation_uuid).has_value()) ||
          profile->scale > profile->precision) {
        return std::nullopt;
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = profile->descriptor_uuid;
      descriptor.type_uuid = profile->type_uuid;
      descriptor.nullability = profile->nullable
                                   ? BoundNullability::kNullable
                                   : BoundNullability::kNonNull;
      if (!profile->collation_uuid.empty()) {
        descriptor.collation_uuid = profile->collation_uuid;
      }
      if (profile->width != 0) {
        descriptor.width_precision_scale.width = profile->width;
      }
      if (profile->precision != 0) {
        descriptor.width_precision_scale.precision = profile->precision;
        descriptor.width_precision_scale.scale = profile->scale;
      }
      context.descriptors.push_back(std::move(descriptor));
      return context.descriptors.back().descriptor_id;
    };

    const bool wildcard_projection = std::ranges::any_of(
        ast.relations.front().output_expression_ids,
        [&](const auto expression_id) {
          const auto* expression = find_expression(expression_id);
          return expression != nullptr &&
                 expression->expression_kind ==
                     NativeExpressionAstKind::kWildcard;
        });
    if (wildcard_projection &&
        ast.relations.front().output_expression_ids.size() != 1) {
      return fail("document_wildcard_projection_shape_invalid");
    }
    std::uint32_t next_synthetic_expression_id = 1;
    if (!ast_expression_ids.empty()) {
      next_synthetic_expression_id =
          *std::ranges::max_element(ast_expression_ids) + 1;
    }
    if (wildcard_projection) {
      if (expression_backed_unnest) {
        const auto descriptor_id = allocate_profile_descriptor(8);
        if (!descriptor_id.has_value()) {
          return fail("document_unnest_result_profile_unavailable");
        }
        context.expressions.push_back(
            {next_synthetic_expression_id++, *descriptor_id, std::nullopt,
             std::nullopt});
        context.outputs.push_back(
            {1, context.expressions.back().expression_id, "element",
             *descriptor_id, true, 0, ast.relations.front().relation_id});
      } else {
        const auto& resolution = context.catalog_relations.front();
        for (std::size_t ordinal = 0; ordinal < resolution.columns.size();
             ++ordinal) {
          const auto& column = resolution.columns[ordinal];
          context.expressions.push_back(
              {next_synthetic_expression_id++, column.descriptor_id,
               std::nullopt, column.column_uuid});
          context.outputs.push_back(
              {static_cast<std::uint32_t>(ordinal + 1),
               context.expressions.back().expression_id,
               column.canonical_name_key, column.descriptor_id, true,
               static_cast<std::uint32_t>(ordinal),
               ast.relations.front().relation_id});
        }
      }
    }

    std::unordered_map<std::uint32_t, NativeExpressionBindingInput>
        expression_binding_by_ast;
    for (const auto& expression : ast.expressions) {
      if (expression.expression_kind == NativeExpressionAstKind::kWildcard) {
        continue;
      }
      std::optional<std::uint32_t> descriptor_id;
      std::optional<std::string> bound_name_uuid;
      const bool exact_document_unnest_root =
          expression_backed_unnest &&
          document_source->model_document_expression_id ==
              expression.expression_id;
      if (exact_document_unnest_root) {
        // The engine statement profile, not an identifier spelling, types the
        // complete DOCUMENT_UNNEST operand as JSON. A DOCUMENT literal is
        // exactly non-null (kind 7); nullable/composed inputs use kind 8.
        const bool non_null_document_literal =
            expression.expression_kind ==
                NativeExpressionAstKind::kLiteral &&
            expression.literal_kind == NativeLiteralAstKind::kDocument;
        descriptor_id =
            allocate_profile_descriptor(non_null_document_literal ? 7 : 8);
      } else switch (expression.expression_kind) {
        case NativeExpressionAstKind::kIdentifier: {
          if (expression.qualified_identifier.empty() ||
              expression.qualified_identifier.size() > 2) {
            break;
          }
          const NativeIdentifierAstNode* column_component =
              &expression.qualified_identifier.back();
          if (!expression_backed_unnest &&
              document_path_source_expression_id == expression.expression_id) {
            descriptor_id = allocate_profile_descriptor(8);
            bound_name_uuid = context.catalog_relations.front().object_uuid;
            break;
          }
          if (expression.qualified_identifier.size() == 2 &&
              (!document_source->alias.has_value() ||
               !SameIdentifierComponent(
                   expression.qualified_identifier.front(),
                   *document_source->alias))) {
            break;
          }
          const auto column_name =
              CanonicalColumnLookupKey(*column_component);
          const auto column = column_descriptor_by_name.find(column_name);
          if (column != column_descriptor_by_name.end()) {
            descriptor_id = column->second;
            bound_name_uuid = column_uuid_by_name.at(column_name);
          } else if (!expression_backed_unnest &&
                     expression.qualified_identifier.size() == 1 &&
                     document_source->alias.has_value() &&
                     SameIdentifierComponent(
                         expression.qualified_identifier.front(),
                         *document_source->alias)) {
            descriptor_id = allocate_profile_descriptor(8);
            bound_name_uuid =
                context.catalog_relations.front().object_uuid;
          } else if (expression_backed_unnest) {
            const bool direct_document_expression =
                document_source->model_document_expression_id ==
                expression.expression_id;
            descriptor_id =
                allocate_profile_descriptor(direct_document_expression ? 8 : 2);
          }
          break;
        }
        case NativeExpressionAstKind::kParameter: {
          const bool direct_document_expression =
              document_source->model_document_expression_id ==
              expression.expression_id;
          descriptor_id =
              allocate_profile_descriptor(direct_document_expression ? 8 : 2);
          break;
        }
        case NativeExpressionAstKind::kLiteral:
          if (!expression.literal_kind.has_value()) break;
          switch (*expression.literal_kind) {
            case NativeLiteralAstKind::kNumeric:
              descriptor_id = allocate_profile_descriptor(1);
              break;
            case NativeLiteralAstKind::kString:
            case NativeLiteralAstKind::kBinary:
            case NativeLiteralAstKind::kTemporal:
            case NativeLiteralAstKind::kUuid:
            case NativeLiteralAstKind::kRegex:
            case NativeLiteralAstKind::kRange:
              descriptor_id = allocate_profile_descriptor(3);
              break;
            case NativeLiteralAstKind::kBoolean:
              descriptor_id = allocate_profile_descriptor(5);
              break;
            case NativeLiteralAstKind::kDocument:
              descriptor_id = allocate_profile_descriptor(7);
              break;
            case NativeLiteralAstKind::kNull:
              descriptor_id = allocate_profile_descriptor(8);
              break;
            case NativeLiteralAstKind::kDefault:
            case NativeLiteralAstKind::kVector:
              break;
          }
          break;
        case NativeExpressionAstKind::kFunctionCall:
          if (expression.operator_name == "DOCUMENT_PATH") {
            descriptor_id = allocate_profile_descriptor(8);
          }
          break;
        case NativeExpressionAstKind::kUnary:
          descriptor_id = expression.operator_name == "NOT"
                              ? allocate_profile_descriptor(6)
                              : allocate_profile_descriptor(2);
          break;
        case NativeExpressionAstKind::kBinary:
          if (expression.operator_name == "=" ||
              expression.operator_name == "<>" ||
              expression.operator_name == "!=" ||
              expression.operator_name == "<" ||
              expression.operator_name == "<=" ||
              expression.operator_name == ">" ||
              expression.operator_name == ">=" ||
              expression.operator_name == "AND" ||
              expression.operator_name == "OR") {
            descriptor_id = allocate_profile_descriptor(6);
          } else if (expression.operator_name == "||") {
            descriptor_id = allocate_profile_descriptor(4);
          } else if (expression.operator_name == "+" ||
                     expression.operator_name == "-" ||
                     expression.operator_name == "*" ||
                     expression.operator_name == "/" ||
                     expression.operator_name == "%") {
            descriptor_id = allocate_profile_descriptor(2);
          }
          break;
        case NativeExpressionAstKind::kParenthesized:
          if (expression.child_expression_ids.size() == 1) {
            const auto child = expression_binding_by_ast.find(
                expression.child_expression_ids.front());
            if (child != expression_binding_by_ast.end()) {
              descriptor_id = child->second.descriptor_id;
            }
          }
          break;
        case NativeExpressionAstKind::kWildcard:
          break;
      }
      if (!descriptor_id.has_value()) {
        return fail("document_expression_profile_unsupported");
      }
      NativeExpressionBindingInput expression_binding{
          expression.expression_id, *descriptor_id, std::nullopt,
          std::move(bound_name_uuid)};
      context.expressions.push_back(expression_binding);
      expression_binding_by_ast.emplace(expression.expression_id,
                                        std::move(expression_binding));
    }

    if (!wildcard_projection) {
      for (std::size_t ordinal = 0;
           ordinal < ast.relations.front().output_expression_ids.size();
           ++ordinal) {
        const auto expression_id =
            ast.relations.front().output_expression_ids[ordinal];
        const auto binding = expression_binding_by_ast.find(expression_id);
        const auto* ast_expression = find_expression(expression_id);
        if (binding == expression_binding_by_ast.end() ||
            ast_expression == nullptr) {
          return fail("document_projection_binding_missing");
        }
        std::string output_name =
            "document_value_" + std::to_string(ordinal + 1);
        if (ast_expression->expression_kind ==
                NativeExpressionAstKind::kIdentifier) {
          // The SQL spelling can be qualified (for example d.payload), but a
          // persisted document column is fetched only by its canonical MGA
          // relation-descriptor key.  Keep the SQL-facing label aligned with
          // that persisted key so no downstream consumer can reinterpret the
          // qualified presentation as a document path.
          output_name = ast_expression->spelling;
          if (!context.catalog_relations.empty() &&
              binding->second.bound_name_uuid.has_value()) {
            const auto& persisted_columns =
                context.catalog_relations.front().columns;
            const auto persisted_column = std::ranges::find_if(
                persisted_columns, [&](const auto& column) {
                  return column.column_uuid ==
                         *binding->second.bound_name_uuid;
                });
            if (persisted_column != persisted_columns.end()) {
              output_name = persisted_column->canonical_name_key;
            }
          }
        }
        context.outputs.push_back(
            {static_cast<std::uint32_t>(ordinal + 1),
             binding->second.expression_id, output_name,
             binding->second.descriptor_id, true,
             static_cast<std::uint32_t>(ordinal),
             ast.relations.front().relation_id});
      }
    }
    if (context.outputs.empty()) {
      return fail("document_projection_empty");
    }
    context.relations.push_back(
        {ast.relations.front().relation_id,
         expression_backed_unnest
             ? "sblr.model-expand.document-unnest.v1"
             : (document_source->model_operation_id == "DOCUMENT_PATH"
                    ? "sblr.model-source.document-path.v1"
                    : "sblr.model-source.document-find.v1")});
    return context;
  }

  const auto window_relation = std::ranges::find_if(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind == NativeRelationAstKind::kWindow;
      });
  if (window_relation != ast.relations.end()) {
    // QOW-SOURCE-RCP-050-ENGINE-WINDOW-BINDING-CONTEXT-V1
    const auto qualify_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind == NativeRelationAstKind::kQualify;
        });
    const bool has_qualify = qualify_relation != ast.relations.end();
    const auto source_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind ==
                 NativeRelationAstKind::kCatalogSource;
        });
    if (source_relation == ast.relations.end() ||
        ast.relations.size() != 2 + static_cast<std::size_t>(has_qualify) ||
        ast.root_relation_id !=
            (has_qualify ? qualify_relation->relation_id
                         : window_relation->relation_id) ||
        window_relation->input_relation_ids !=
            std::vector<std::uint32_t>{source_relation->relation_id} ||
        ast.catalog_relation_sources.size() != 1 ||
        ast.window_definitions.empty() ||
        ast.window_definitions.size() > 1024 ||
        ast.window_invocations.size() != 1 ||
        window_relation->window_invocation_ids !=
            std::vector<std::uint32_t>{
                ast.window_invocations.front().invocation_id} ||
        std::ranges::none_of(
            ast.window_definitions, [&](const auto& definition) {
              return definition.window_id ==
                     ast.window_invocations.front().window_definition_id;
            }) ||
        window_relation->output_expression_ids !=
            std::vector<std::uint32_t>{
                ast.window_invocations.front().function_expression_id} ||
        (has_qualify &&
         (qualify_relation->input_relation_ids !=
              std::vector<std::uint32_t>{window_relation->relation_id} ||
          qualify_relation->output_expression_ids !=
              window_relation->output_expression_ids ||
          qualify_relation->predicate_expression_ids.size() != 1)) ||
        resolved_object_reference_seeds.size() != 1 ||
        statement_context.window_function_profiles.size() != 11) {
      return fail("catalog_window_shape_invalid");
    }
    const auto& source = ast.catalog_relation_sources.front();
    const auto& resolved = resolved_object_reference_seeds.front().resolved;
    const auto& projection = resolved.relation_descriptor;
    const bool relation_object_class =
        resolved.object_class == "relation" || resolved.object_class == "table" ||
        resolved.object_class == "view" ||
        resolved.object_class == "materialized_view" ||
        resolved.object_class == "external_table" ||
        resolved.object_class == "foreign_table";
    if (source_relation->relation_source_ids !=
            std::vector<std::uint32_t>{source.source_id} ||
        source_relation->output_expression_ids.empty() || !resolved.resolved ||
        !projection.present || !relation_object_class ||
        resolved.object_uuid.empty() ||
        projection.relation_uuid != resolved.object_uuid ||
        projection.descriptor_uuid.empty() || projection.schema_uuid.empty() ||
        projection.descriptor_generation == 0 ||
        projection.validated_resource_epoch == 0 ||
        resolved.catalog_epoch == 0 || resolved.security_epoch == 0 ||
        projection.columns.empty()) {
      return fail("catalog_window_projection_authority_incomplete");
    }

    NativeCatalogRelationBindingInput catalog_relation;
    catalog_relation.source_id = source.source_id;
    catalog_relation.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    catalog_relation.object_uuid = resolved.object_uuid;
    catalog_relation.resolved_object_type = resolved.object_class;
    catalog_relation.resolved_schema_uuid = projection.schema_uuid;
    catalog_relation.catalog_generation_id = resolved.catalog_epoch;
    catalog_relation.security_epoch = resolved.security_epoch;
    catalog_relation.resource_epoch = projection.validated_resource_epoch;

    std::unordered_set<std::string> source_names;
    for (const auto ast_expression_id :
         source_relation->output_expression_ids) {
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id == ast_expression_id;
          });
      if (expression == ast.expressions.end() ||
          expression->expression_kind != NativeExpressionAstKind::kIdentifier ||
          expression->spelling.empty() ||
          !source_names.insert(expression->spelling).second) {
        return fail("catalog_window_source_expression_invalid");
      }
      const auto column = std::ranges::find_if(
          projection.columns, [&](const auto& candidate) {
            return candidate.canonical_name_key == expression->spelling;
          });
      if (column == projection.columns.end() || column->column_uuid.empty() ||
          !CanonicalUuidBytes(column->type_descriptor_uuid).has_value()) {
        return fail("catalog_window_source_column_unresolved");
      }
      const auto descriptor_fields = ParseExactProjectedDescriptor(
          column->encoded_type_descriptor, column->collation_uuid,
          column->nullable);
      if (!descriptor_fields.has_value()) {
        return fail("catalog_window_source_descriptor_invalid");
      }
      const auto binding_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id = binding_id;
      descriptor.descriptor_uuid = column->type_descriptor_uuid;
      descriptor.type_uuid = descriptor_fields->type_uuid;
      descriptor.nullability = descriptor_fields->nullable
                                   ? BoundNullability::kNullable
                                   : BoundNullability::kNonNull;
      descriptor.collation_uuid = descriptor_fields->collation_uuid;
      descriptor.timezone_profile_id = descriptor_fields->timezone_profile_id;
      if (column->character_length != 0) {
        descriptor.width_precision_scale.width = column->character_length;
      }
      context.descriptors.push_back(std::move(descriptor));
      context.expressions.push_back(
          {binding_id, binding_id, std::nullopt, column->column_uuid});
      context.outputs.push_back(
          {static_cast<std::uint32_t>(context.outputs.size() + 1), binding_id,
           column->canonical_name_key, binding_id, false,
           static_cast<std::uint32_t>(catalog_relation.columns.size()),
           source_relation->relation_id});
      catalog_relation.columns.push_back(
          {static_cast<std::uint32_t>(catalog_relation.columns.size()),
           column->column_uuid, binding_id, column->canonical_name_key});
    }

    std::vector<std::uint32_t> offset_expression_ids;
    const auto collect_offset = [&](const auto& bound) {
      if (bound.has_value() && bound->offset_expression_id.has_value() &&
          std::ranges::find(offset_expression_ids,
                            *bound->offset_expression_id) ==
              offset_expression_ids.end()) {
        offset_expression_ids.push_back(*bound->offset_expression_id);
      }
    };
    for (const auto& definition : ast.window_definitions) {
      collect_offset(definition.frame_start);
      collect_offset(definition.frame_end);
    }
    for (std::size_t ordinal = 0; ordinal < offset_expression_ids.size();
         ++ordinal) {
      const auto ast_expression_id = offset_expression_ids[ordinal];
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id == ast_expression_id;
          });
      const auto profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == 1 &&
                   candidate.slot == ordinal + 1;
          });
      if (expression == ast.expressions.end() ||
          expression->expression_kind != NativeExpressionAstKind::kLiteral ||
          expression->literal_kind != NativeLiteralAstKind::kNumeric ||
          expression->spelling.empty() ||
          profile == statement_context.descriptor_profiles.end() ||
          profile->nullable ||
          !CanonicalUuidBytes(profile->descriptor_uuid).has_value() ||
          !CanonicalUuidBytes(profile->type_uuid).has_value()) {
        return fail("catalog_window_frame_offset_profile_unavailable");
      }
      const auto binding_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      context.descriptors.push_back(
          {binding_id, profile->descriptor_uuid, profile->type_uuid,
           BoundNullability::kNonNull, std::nullopt, std::nullopt, {}});
      context.expressions.push_back(
          {binding_id, binding_id, std::nullopt, std::nullopt});
    }

    const auto& invocation = ast.window_invocations.front();
    const auto function_expression = std::ranges::find_if(
        ast.expressions, [&](const auto& candidate) {
          return candidate.expression_id == invocation.function_expression_id;
        });
    const auto function_profile = std::ranges::find_if(
        statement_context.window_function_profiles,
        [](const auto& candidate) {
          return candidate.builtin_id == "sb.window.row_number";
        });
    const auto result_profile = std::ranges::find_if(
        statement_context.descriptor_profiles, [](const auto& candidate) {
          return candidate.profile_kind == 1 && candidate.slot == 0;
        });
    if (function_expression == ast.expressions.end() ||
        function_expression->expression_kind !=
            NativeExpressionAstKind::kFunctionCall ||
        function_expression->operator_name != "ROW_NUMBER" ||
        !function_expression->child_expression_ids.empty() ||
        function_profile == statement_context.window_function_profiles.end() ||
        function_profile->abi_version != 1 || !function_profile->executable ||
        !CanonicalUuidBytes(function_profile->function_uuid).has_value() ||
        result_profile == statement_context.descriptor_profiles.end() ||
        result_profile->nullable ||
        !CanonicalUuidBytes(result_profile->descriptor_uuid).has_value() ||
        !CanonicalUuidBytes(result_profile->type_uuid).has_value()) {
      return fail("catalog_window_row_number_profile_unavailable");
    }
    const auto function_binding_id =
        static_cast<std::uint32_t>(context.descriptors.size() + 1);
    context.descriptors.push_back(
        {function_binding_id, result_profile->descriptor_uuid,
         result_profile->type_uuid, BoundNullability::kNonNull, std::nullopt,
         std::nullopt, {}});
    context.expressions.push_back(
        {function_binding_id, function_binding_id,
         function_profile->function_uuid, std::nullopt});
    context.window_functions.push_back(
        {invocation.invocation_id, function_binding_id,
         function_profile->abi_version, function_profile->builtin_id,
         function_profile->function_uuid, function_profile->executable,
         function_binding_id});
    const auto window_output_name =
        invocation.output_alias.has_value()
            ? invocation.output_alias->spelling
            : std::string("row_number");
    context.outputs.push_back(
        {static_cast<std::uint32_t>(context.outputs.size() + 1),
         function_binding_id,
         window_output_name, function_binding_id, !has_qualify, 0,
         window_relation->relation_id});
    if (has_qualify) {
      const auto predicate = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   qualify_relation->predicate_expression_ids.front();
          });
      const NativeExpressionAstNode* literal = nullptr;
      if (predicate != ast.expressions.end() &&
          predicate->expression_kind == NativeExpressionAstKind::kBinary &&
          predicate->child_expression_ids.size() == 2 &&
          predicate->child_expression_ids.front() ==
              invocation.function_expression_id &&
          (predicate->operator_name == "=" || predicate->operator_name == "<>" ||
           predicate->operator_name == "!=" || predicate->operator_name == "<" ||
           predicate->operator_name == "<=" || predicate->operator_name == ">" ||
           predicate->operator_name == ">=")) {
        const auto found_literal = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     predicate->child_expression_ids.back();
            });
        if (found_literal != ast.expressions.end()) literal = &*found_literal;
      }
      const auto numeric_profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == 1 &&
                   candidate.slot == offset_expression_ids.size() + 1;
          });
      const auto boolean_profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [](const auto& candidate) {
            return candidate.profile_kind == 6 && candidate.slot == 0;
          });
      if (literal == nullptr ||
          literal->expression_kind != NativeExpressionAstKind::kLiteral ||
          literal->literal_kind != NativeLiteralAstKind::kNumeric ||
          literal->spelling.empty() ||
          numeric_profile == statement_context.descriptor_profiles.end() ||
          numeric_profile->nullable ||
          !CanonicalUuidBytes(numeric_profile->descriptor_uuid).has_value() ||
          !CanonicalUuidBytes(numeric_profile->type_uuid).has_value() ||
          numeric_profile->type_uuid != result_profile->type_uuid ||
          boolean_profile == statement_context.descriptor_profiles.end() ||
          !boolean_profile->nullable ||
          !CanonicalUuidBytes(boolean_profile->descriptor_uuid).has_value() ||
          !CanonicalUuidBytes(boolean_profile->type_uuid).has_value()) {
        return fail("catalog_window_qualify_profile_unavailable");
      }
      const auto literal_binding_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      context.descriptors.push_back(
          {literal_binding_id, numeric_profile->descriptor_uuid,
           numeric_profile->type_uuid, BoundNullability::kNonNull,
           std::nullopt, std::nullopt, {}});
      context.expressions.push_back(
          {literal_binding_id, literal_binding_id, std::nullopt,
           std::nullopt});
      const auto predicate_binding_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      context.descriptors.push_back(
          {predicate_binding_id, boolean_profile->descriptor_uuid,
           boolean_profile->type_uuid, BoundNullability::kNullable,
           std::nullopt, std::nullopt, {}});
      context.expressions.push_back(
          {predicate_binding_id, predicate_binding_id, std::nullopt,
           std::nullopt});
      context.outputs.push_back(
          {static_cast<std::uint32_t>(context.outputs.size() + 1),
           function_binding_id, window_output_name, function_binding_id, true,
           0, qualify_relation->relation_id});
    }
    context.catalog_relations.push_back(std::move(catalog_relation));
    context.relations.push_back(
        {window_relation->relation_id, "window.row-number.v1"});
    if (has_qualify) {
      context.relations.push_back(
          {qualify_relation->relation_id,
           "qualify.window-result-numeric-comparison.v1"});
    }
    return context;
  }

  const auto catalog_join = std::ranges::find_if(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind == NativeRelationAstKind::kJoin;
      });
  if (catalog_join != ast.relations.end()) {
    std::vector<const NativeRelationAstNode*> source_relations;
    for (const auto& relation : ast.relations) {
      if (relation.relation_kind == NativeRelationAstKind::kCatalogSource) {
        source_relations.push_back(&relation);
      }
    }
    if (ast.catalog_relation_sources.size() != 2 ||
        source_relations.size() != 2 || ast.relations.size() != 3 ||
        ast.root_relation_id != catalog_join->relation_id ||
        catalog_join->input_relation_ids !=
            std::vector<std::uint32_t>{source_relations[0]->relation_id,
                                       source_relations[1]->relation_id} ||
        catalog_join->predicate_expression_ids.size() > 1 ||
        resolved_object_reference_seeds.size() != 2) {
      return fail("catalog_join_shape_invalid");
    }

    std::string join_semantic;
    switch (catalog_join->join_kind) {
      case NativeJoinAstKind::kCross:
        join_semantic = "join.cross.v1";
        break;
      case NativeJoinAstKind::kInner:
        join_semantic = "join.inner.v1";
        break;
      case NativeJoinAstKind::kLeftOuter:
        join_semantic = "join.left-outer.v1";
        break;
      case NativeJoinAstKind::kRightOuter:
        join_semantic = "join.right-outer.v1";
        break;
      case NativeJoinAstKind::kFullOuter:
        join_semantic = "join.full-outer.v1";
        break;
      case NativeJoinAstKind::kLeftSemi:
        join_semantic = "join.left-semi.v1";
        break;
      case NativeJoinAstKind::kLeftAnti:
        join_semantic = "join.left-anti.v1";
        break;
      case NativeJoinAstKind::kNone:
        return fail("catalog_join_kind_invalid");
    }
    const bool predicate_join =
        catalog_join->join_kind != NativeJoinAstKind::kCross;
    if (catalog_join->predicate_expression_ids.size() !=
        static_cast<std::size_t>(predicate_join)) {
      return fail("catalog_join_predicate_cardinality_invalid");
    }
    std::vector<const NativeExpressionAstNode*> predicate_comparisons;
    std::vector<const NativeExpressionAstNode*> predicate_nodes;
    if (predicate_join) {
      const auto find_expression = [&](const std::uint32_t expression_id) {
        const auto found = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == expression_id;
            });
        return found == ast.expressions.end() ? nullptr : &*found;
      };
      const auto* predicate = find_expression(
          catalog_join->predicate_expression_ids.front());
      const auto is_comparison = [](const NativeExpressionAstNode* candidate) {
        return candidate != nullptr &&
               candidate->expression_kind ==
                   NativeExpressionAstKind::kBinary &&
               candidate->child_expression_ids.size() == 2 &&
               (candidate->operator_name == "=" ||
                candidate->operator_name == "<>" ||
                candidate->operator_name == "!=" ||
                candidate->operator_name == "<" ||
                candidate->operator_name == "<=" ||
                candidate->operator_name == ">" ||
                candidate->operator_name == ">=" ||
                candidate->operator_name == "IS DISTINCT FROM" ||
                candidate->operator_name == "IS NOT DISTINCT FROM");
      };
      std::unordered_set<std::uint32_t> visited_predicate_ids;
      const auto collect_predicate = [&](auto&& self,
                                         const NativeExpressionAstNode* node,
                                         const std::size_t depth) -> bool {
        if (node == nullptr || depth > 32 ||
            !visited_predicate_ids.insert(node->expression_id).second) {
          return false;
        }
        if (is_comparison(node)) {
          predicate_comparisons.push_back(node);
          predicate_nodes.push_back(node);
          return true;
        }
        if (node->expression_kind != NativeExpressionAstKind::kBinary ||
            node->child_expression_ids.size() != 2 ||
            (node->operator_name != "AND" && node->operator_name != "OR")) {
          return false;
        }
        for (const auto child_id : node->child_expression_ids) {
          if (!self(self, find_expression(child_id), depth + 1)) return false;
        }
        predicate_nodes.push_back(node);
        return predicate_nodes.size() <= 32;
      };
      if (!collect_predicate(collect_predicate, predicate, 1) ||
          predicate_nodes.empty() || predicate_nodes.size() > 32) {
        return fail("catalog_inner_join_predicate_invalid");
      }
      for (const auto* comparison : predicate_comparisons) {
        const auto* left_key =
            find_expression(comparison->child_expression_ids[0]);
        const auto* right_key =
            find_expression(comparison->child_expression_ids[1]);
        if (left_key == nullptr || right_key == nullptr ||
            left_key->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            right_key->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            left_key->spelling.empty() || right_key->spelling.empty() ||
            !left_key->child_expression_ids.empty() ||
            !right_key->child_expression_ids.empty()) {
          return fail("catalog_inner_join_key_invalid");
        }
      }
    }

    std::uint32_t binding_id = 1;
    for (std::size_t source_ordinal = 0; source_ordinal < 2;
         ++source_ordinal) {
      const auto& source = ast.catalog_relation_sources[source_ordinal];
      const auto& relation = *source_relations[source_ordinal];
      const auto& resolved =
          resolved_object_reference_seeds[source_ordinal].resolved;
      const auto& projection = resolved.relation_descriptor;
      const bool relation_object_class =
          resolved.object_class == "relation" ||
          resolved.object_class == "table" ||
          resolved.object_class == "view" ||
          resolved.object_class == "materialized_view" ||
          resolved.object_class == "external_table" ||
          resolved.object_class == "foreign_table";
      if (relation.relation_source_ids !=
              std::vector<std::uint32_t>{source.source_id} ||
          relation.output_expression_ids.size() != 1 ||
          !resolved.resolved || !projection.present ||
          !relation_object_class || resolved.object_uuid.empty() ||
          projection.relation_uuid != resolved.object_uuid ||
          projection.descriptor_uuid.empty() || projection.schema_uuid.empty() ||
          projection.descriptor_generation == 0 ||
          projection.validated_resource_epoch == 0 ||
          resolved.catalog_epoch == 0 || resolved.security_epoch == 0 ||
          projection.columns.empty()) {
        return fail("catalog_cross_join_projection_authority_incomplete");
      }

      NativeCatalogRelationBindingInput catalog_relation;
      catalog_relation.source_id = source.source_id;
      catalog_relation.resolution_state =
          NativeCatalogRelationResolutionState::kBound;
      catalog_relation.object_uuid = resolved.object_uuid;
      catalog_relation.resolved_object_type = resolved.object_class;
      catalog_relation.resolved_schema_uuid = projection.schema_uuid;
      catalog_relation.catalog_generation_id = resolved.catalog_epoch;
      catalog_relation.security_epoch = resolved.security_epoch;
      catalog_relation.resource_epoch = projection.validated_resource_epoch;
      for (std::size_t ordinal = 0; ordinal < projection.columns.size();
           ++ordinal, ++binding_id) {
        const auto& column = projection.columns[ordinal];
        const auto descriptor_fields = ParseExactProjectedDescriptor(
            column.encoded_type_descriptor, column.collation_uuid,
            column.nullable);
        if (column.ordinal != ordinal || column.column_uuid.empty() ||
            column.canonical_name_key.empty() ||
            !CanonicalUuidBytes(column.type_descriptor_uuid).has_value() ||
            !descriptor_fields.has_value()) {
          return fail("catalog_cross_join_column_projection_incomplete");
        }
        NativeDescriptorBindingInput descriptor;
        descriptor.descriptor_id = binding_id;
        descriptor.descriptor_uuid = column.type_descriptor_uuid;
        descriptor.type_uuid = descriptor_fields->type_uuid;
        descriptor.nullability = descriptor_fields->nullable
                                     ? BoundNullability::kNullable
                                     : BoundNullability::kNonNull;
        descriptor.collation_uuid = descriptor_fields->collation_uuid;
        descriptor.timezone_profile_id =
            descriptor_fields->timezone_profile_id;
        if (column.character_length != 0) {
          descriptor.width_precision_scale.width = column.character_length;
        }
        context.descriptors.push_back(std::move(descriptor));
        context.expressions.push_back(
            {binding_id, binding_id, std::nullopt, column.column_uuid});
        context.outputs.push_back(
            {static_cast<std::uint32_t>(context.outputs.size() + 1),
             binding_id, column.canonical_name_key, binding_id, true,
             static_cast<std::uint32_t>(ordinal), relation.relation_id});
        catalog_relation.columns.push_back(
            {static_cast<std::uint32_t>(ordinal), column.column_uuid,
             binding_id, column.canonical_name_key});
      }
      context.catalog_relations.push_back(std::move(catalog_relation));
    }
    const auto source_descriptor_count = context.descriptors.size();
    if (predicate_join) {
      const auto find_expression = [&](const std::uint32_t expression_id) {
        const auto found = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == expression_id;
            });
        return found == ast.expressions.end() ? nullptr : &*found;
      };
      for (const auto* comparison : predicate_comparisons) {
        const auto* left_key =
            find_expression(comparison->child_expression_ids[0]);
        const auto* right_key =
            find_expression(comparison->child_expression_ids[1]);
        const auto left_column = std::ranges::find_if(
            context.catalog_relations[0].columns, [&](const auto& column) {
              return column.canonical_name_key == left_key->spelling;
            });
        const auto right_column = std::ranges::find_if(
            context.catalog_relations[1].columns, [&](const auto& column) {
              return column.canonical_name_key == right_key->spelling;
            });
        if (left_column == context.catalog_relations[0].columns.end() ||
            right_column == context.catalog_relations[1].columns.end() ||
            context.descriptors[left_column->descriptor_id - 1].type_uuid !=
                context.descriptors[right_column->descriptor_id - 1]
                    .type_uuid) {
          return fail("catalog_inner_join_key_binding_unavailable");
        }
      }
      const auto predicate_descriptor_count = predicate_nodes.size();
      for (std::size_t slot = 0; slot < predicate_descriptor_count; ++slot) {
        const auto boolean_profile = std::ranges::find_if(
            statement_context.descriptor_profiles, [&](const auto& candidate) {
              return candidate.profile_kind == 6 && candidate.slot == slot;
            });
        if (boolean_profile == statement_context.descriptor_profiles.end() ||
            !boolean_profile->nullable ||
            !CanonicalUuidBytes(boolean_profile->descriptor_uuid).has_value() ||
            !CanonicalUuidBytes(boolean_profile->type_uuid).has_value()) {
          return fail("catalog_join_boolean_descriptor_profile_unavailable");
        }
        NativeDescriptorBindingInput descriptor;
        descriptor.descriptor_id =
            static_cast<std::uint32_t>(context.descriptors.size() + 1);
        descriptor.descriptor_uuid = boolean_profile->descriptor_uuid;
        descriptor.type_uuid = boolean_profile->type_uuid;
        descriptor.nullability = BoundNullability::kNullable;
        context.descriptors.push_back(std::move(descriptor));
      }
    }
    const auto source_output_count = context.outputs.size();
    const bool left_only_join =
        catalog_join->join_kind == NativeJoinAstKind::kLeftSemi ||
        catalog_join->join_kind == NativeJoinAstKind::kLeftAnti;
    const auto join_output_count =
        left_only_join ? context.catalog_relations[0].columns.size()
                       : source_descriptor_count;
    for (std::size_t ordinal = 0; ordinal < join_output_count;
         ++ordinal) {
      const auto binding = static_cast<std::uint32_t>(ordinal + 1);
      const auto source_output = context.outputs[ordinal];
      context.outputs.push_back(
          {static_cast<std::uint32_t>(source_output_count + ordinal + 1),
           binding, source_output.output_name_utf8, binding, true,
           static_cast<std::uint32_t>(ordinal), catalog_join->relation_id});
    }
    context.relations.push_back(
        {catalog_join->relation_id, std::move(join_semantic)});
    return context;
  }

  if (!ast.catalog_relation_sources.empty()) {
    const auto source_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind ==
                 NativeRelationAstKind::kCatalogSource;
        });
    const auto limit_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind == NativeRelationAstKind::kLimit;
        });
    const auto filter_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind == NativeRelationAstKind::kFilter;
        });
    const auto aggregate_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind == NativeRelationAstKind::kAggregate;
        });
    const auto project_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind == NativeRelationAstKind::kProject;
        });
    const auto sort_relation = std::ranges::find_if(
        ast.relations, [](const auto& relation) {
          return relation.relation_kind == NativeRelationAstKind::kSort;
        });
    if (source_relation == ast.relations.end()) {
      return fail("catalog_source_projection_cardinality_invalid");
    }
    const bool filter_composition =
        filter_relation != ast.relations.end() &&
        filter_relation->input_relation_ids ==
            std::vector<std::uint32_t>{source_relation->relation_id};
    const bool sort_composition =
        sort_relation != ast.relations.end() &&
        sort_relation->input_relation_ids ==
            std::vector<std::uint32_t>{
                filter_composition ? filter_relation->relation_id
                                   : source_relation->relation_id};
    const auto expected_project_predecessor =
        sort_composition
            ? sort_relation->relation_id
            : (filter_composition ? filter_relation->relation_id
                                  : source_relation->relation_id);
    const bool project_composition =
        project_relation != ast.relations.end() &&
        project_relation->input_relation_ids ==
            std::vector<std::uint32_t>{expected_project_predecessor} &&
        !project_relation->output_expression_ids.empty();
    const auto expected_aggregate_predecessor =
        filter_composition ? filter_relation->relation_id
                           : source_relation->relation_id;
    const bool aggregate_composition =
        aggregate_relation != ast.relations.end() &&
        aggregate_relation->input_relation_ids ==
            std::vector<std::uint32_t>{expected_aggregate_predecessor} &&
        aggregate_relation->aggregate_grouping_form ==
            NativeAggregateGroupingForm::kNone &&
        aggregate_relation->aggregate_projection_form ==
            NativeAggregateProjectionForm::kGlobalUnary &&
        aggregate_relation->aggregate_expression_ids.size() == 1 &&
        aggregate_relation->output_expression_ids ==
            aggregate_relation->aggregate_expression_ids;
    const bool limit_composition = limit_relation != ast.relations.end();
    const auto expected_root =
        limit_composition ? limit_relation->relation_id
                          : (aggregate_composition
                                 ? aggregate_relation->relation_id
                                 : (project_composition
                                 ? project_relation->relation_id
                                 : (sort_composition
                                        ? sort_relation->relation_id
                                        : (filter_composition
                                               ? filter_relation->relation_id
                                               : source_relation->relation_id))));
    const auto expected_limit_input =
        aggregate_composition
            ? aggregate_relation->relation_id
            : (project_composition
            ? project_relation->relation_id
            : (sort_composition
                   ? sort_relation->relation_id
                   : (filter_composition ? filter_relation->relation_id
                                         : source_relation->relation_id)));
    const bool catalog_chain =
        ast.relations.size() ==
            1 + static_cast<std::size_t>(filter_composition) +
                static_cast<std::size_t>(sort_composition) +
                static_cast<std::size_t>(project_composition) +
                static_cast<std::size_t>(aggregate_composition) +
                static_cast<std::size_t>(limit_composition) &&
        ast.root_relation_id == expected_root &&
        (!limit_composition ||
         limit_relation->input_relation_ids ==
             std::vector<std::uint32_t>{expected_limit_input});
    if (ast.catalog_relation_sources.size() != 1 ||
        (aggregate_composition &&
         (sort_composition || project_composition)) ||
        !catalog_chain || ast.root_relation_id == 0 ||
        resolved_object_reference_seeds.size() != 1) {
      return fail("catalog_source_projection_cardinality_invalid");
    }
    const auto& source = ast.catalog_relation_sources.front();
    const auto& relation = *source_relation;
    const auto& resolved = resolved_object_reference_seeds.front().resolved;
    const auto& projection = resolved.relation_descriptor;
    const bool relation_object_class =
        resolved.object_class == "relation" || resolved.object_class == "table" ||
        resolved.object_class == "view" ||
        resolved.object_class == "materialized_view" ||
        resolved.object_class == "external_table" ||
        resolved.object_class == "foreign_table";
    if ((!filter_composition && !sort_composition && !project_composition &&
         !aggregate_composition &&
         !limit_composition &&
         relation.relation_id != ast.root_relation_id) ||
        relation.relation_kind != NativeRelationAstKind::kCatalogSource ||
        relation.relation_source_ids !=
            std::vector<std::uint32_t>{source.source_id} ||
        !resolved.resolved || !projection.present || !relation_object_class ||
        resolved.object_uuid.empty() ||
        projection.relation_uuid != resolved.object_uuid ||
        projection.descriptor_uuid.empty() || projection.schema_uuid.empty() ||
        projection.descriptor_generation == 0 ||
        projection.validated_resource_epoch == 0 ||
        resolved.catalog_epoch == 0 || resolved.security_epoch == 0 ||
        projection.columns.empty()) {
      return fail("catalog_source_projection_authority_incomplete");
    }

    std::vector<std::size_t> source_column_indexes;
    const bool wildcard_projection =
        relation.output_expression_ids.size() == 1 &&
        ast.expressions.front().expression_id ==
            relation.output_expression_ids.front() &&
        ast.expressions.front().expression_kind ==
            NativeExpressionAstKind::kWildcard;
    if (wildcard_projection) {
      source_column_indexes.reserve(projection.columns.size());
      for (std::size_t index = 0; index < projection.columns.size(); ++index) {
        source_column_indexes.push_back(index);
      }
    } else {
      source_column_indexes.reserve(relation.output_expression_ids.size());
      for (const auto expression_id : relation.output_expression_ids) {
        const auto expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id == expression_id;
            });
        if (expression == ast.expressions.end() ||
            expression->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            expression->spelling.empty() ||
            !expression->child_expression_ids.empty() ||
            !expression->operator_name.empty()) {
          return fail("catalog_projection_expression_invalid");
        }
        const auto column = std::ranges::find_if(
            projection.columns, [&](const auto& candidate) {
              return candidate.canonical_name_key == expression->spelling;
            });
        if (column == projection.columns.end()) {
          return fail("catalog_projection_column_unresolved");
        }
        const auto index = static_cast<std::size_t>(
            std::distance(projection.columns.begin(), column));
        if (std::ranges::find(source_column_indexes, index) !=
            source_column_indexes.end()) {
          return fail("catalog_projection_column_duplicate");
        }
        source_column_indexes.push_back(index);
      }
    }
    if (source_column_indexes.empty()) {
      return fail("catalog_projection_column_empty");
    }

    NativeCatalogRelationBindingInput catalog_relation;
    catalog_relation.source_id = source.source_id;
    catalog_relation.resolution_state =
        NativeCatalogRelationResolutionState::kBound;
    catalog_relation.object_uuid = resolved.object_uuid;
    catalog_relation.resolved_object_type = resolved.object_class;
    catalog_relation.resolved_schema_uuid = projection.schema_uuid;
    catalog_relation.catalog_generation_id = resolved.catalog_epoch;
    catalog_relation.security_epoch = resolved.security_epoch;
    catalog_relation.resource_epoch = projection.validated_resource_epoch;
    catalog_relation.columns.reserve(source_column_indexes.size());
    context.descriptors.reserve(
        source_column_indexes.size() +
        (aggregate_composition ? 1 : 0) +
        (limit_composition ? 1 : 0) +
        (filter_composition ? 1 : 0));
    context.expressions.reserve(source_column_indexes.size() +
                                (aggregate_composition ? 1 : 0));
    context.outputs.reserve(source_column_indexes.size() *
                            ast.relations.size());
    // QOW-SOURCE-PACKET7-PERSISTED-TYPE-BINDING-V1: catalog descriptor and
    // type identities are distinct engine-owned values. Decode only the exact
    // persisted descriptor fields transported by the selected-transaction
    // relation projection; never map canonical type names or use descriptor
    // UUIDs as type fallbacks.
    struct ExactProjectedDescriptorFields {
      std::string type_uuid;
      std::optional<std::string> collation_uuid;
      std::optional<std::string> timezone_profile_id;
      bool nullable{false};
    };
    const auto parse_exact_descriptor_fields =
        [](std::string_view encoded,
           std::string_view projected_collation_uuid,
           bool projected_nullable)
        -> std::optional<ExactProjectedDescriptorFields> {
      ExactProjectedDescriptorFields fields;
      std::optional<bool> canonical_nullable;
      std::optional<bool> storage_nullable;
      bool type_seen = false;
      bool collation_seen = false;
      bool timezone_seen = false;
      std::size_t offset = 0;
      while (offset <= encoded.size()) {
        const auto delimiter = encoded.find(';', offset);
        const auto end = delimiter == std::string_view::npos
                             ? encoded.size()
                             : delimiter;
        const auto field = encoded.substr(offset, end - offset);
        const auto assign_text = [&](std::string_view prefix,
                                     bool* seen,
                                     std::string* value) {
          if (!field.starts_with(prefix)) return true;
          if (*seen || field.size() == prefix.size()) return false;
          *seen = true;
          value->assign(field.substr(prefix.size()));
          return true;
        };
        if (!assign_text("type_uuid=", &type_seen, &fields.type_uuid)) {
          return std::nullopt;
        }
        std::string collation_uuid;
        if (field.starts_with("collation_uuid=")) {
          if (!assign_text("collation_uuid=", &collation_seen,
                           &collation_uuid)) {
            return std::nullopt;
          }
          fields.collation_uuid = std::move(collation_uuid);
        }
        std::string timezone_profile_id;
        if (field.starts_with("timezone_profile_id=")) {
          if (!assign_text("timezone_profile_id=", &timezone_seen,
                           &timezone_profile_id)) {
            return std::nullopt;
          }
          fields.timezone_profile_id = std::move(timezone_profile_id);
        }
        if (field.starts_with("nullability=")) {
          if (canonical_nullable.has_value()) return std::nullopt;
          const auto value = field.substr(std::string_view("nullability=").size());
          if (value == "nullable") {
            canonical_nullable = true;
          } else if (value == "non_null") {
            canonical_nullable = false;
          } else {
            return std::nullopt;
          }
        } else if (field.starts_with("nullable=")) {
          if (storage_nullable.has_value()) return std::nullopt;
          const auto value = field.substr(std::string_view("nullable=").size());
          if (value == "true") {
            storage_nullable = true;
          } else if (value == "false") {
            storage_nullable = false;
          } else {
            return std::nullopt;
          }
        }
        if (delimiter == std::string_view::npos) break;
        offset = delimiter + 1;
      }
      if (!type_seen || !CanonicalUuidBytes(fields.type_uuid).has_value() ||
          (!canonical_nullable.has_value() && !storage_nullable.has_value()) ||
          (canonical_nullable.has_value() && storage_nullable.has_value() &&
           *canonical_nullable != *storage_nullable)) {
        return std::nullopt;
      }
      fields.nullable = canonical_nullable.has_value()
                            ? *canonical_nullable
                            : *storage_nullable;
      if (fields.nullable != projected_nullable ||
          fields.collation_uuid.has_value() !=
              !projected_collation_uuid.empty() ||
          (fields.collation_uuid.has_value() &&
           (*fields.collation_uuid != projected_collation_uuid ||
            !CanonicalUuidBytes(*fields.collation_uuid).has_value()))) {
        return std::nullopt;
      }
      return fields;
    };
    for (std::size_t ordinal = 0; ordinal < source_column_indexes.size();
         ++ordinal) {
      const auto selected_index = source_column_indexes[ordinal];
      const auto& column = projection.columns[selected_index];
      const auto expected_ordinal = static_cast<std::uint32_t>(ordinal);
      const auto binding_id = static_cast<std::uint32_t>(ordinal + 1);
      if (column.ordinal != selected_index || column.column_uuid.empty() ||
          column.canonical_name_key.empty() ||
          !CanonicalUuidBytes(column.type_descriptor_uuid).has_value()) {
        return fail("catalog_source_column_projection_incomplete");
      }
      const auto descriptor_fields = parse_exact_descriptor_fields(
          column.encoded_type_descriptor, column.collation_uuid,
          column.nullable);
      if (!descriptor_fields.has_value()) {
        return fail("catalog_source_column_descriptor_carrier_invalid");
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id = binding_id;
      descriptor.descriptor_uuid = column.type_descriptor_uuid;
      descriptor.type_uuid = descriptor_fields->type_uuid;
      descriptor.nullability = descriptor_fields->nullable
                                   ? BoundNullability::kNullable
                                   : BoundNullability::kNonNull;
      descriptor.collation_uuid = descriptor_fields->collation_uuid;
      descriptor.timezone_profile_id =
          descriptor_fields->timezone_profile_id;
      if (column.character_length != 0) {
        descriptor.width_precision_scale.width = column.character_length;
      }
      context.descriptors.push_back(std::move(descriptor));
      context.expressions.push_back(
          {binding_id, binding_id, std::nullopt, column.column_uuid});
      context.outputs.push_back(
          {binding_id, binding_id, column.canonical_name_key, binding_id, true,
           expected_ordinal, relation.relation_id});
      catalog_relation.columns.push_back(
          {expected_ordinal, column.column_uuid, binding_id,
           column.canonical_name_key});
    }
    std::optional<std::uint32_t> aggregate_binding_id;
    std::string aggregate_output_name;
    std::string aggregate_semantic;
    if (aggregate_composition) {
      const auto aggregate_expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   aggregate_relation->aggregate_expression_ids.front();
          });
      const auto function =
          aggregate_expression == ast.expressions.end()
              ? std::string{}
              : ToUpperAscii(aggregate_expression->operator_name);
      const bool count_function = function == "COUNT";
      const bool sum_function = function == "SUM";
      const bool avg_function = function == "AVG";
      const bool min_function = function == "MIN";
      const bool max_function = function == "MAX";
      const bool bool_and_function = function == "BOOL_AND";
      const bool bool_or_function = function == "BOOL_OR";
      const bool every_function = function == "EVERY";
      const bool boolean_function =
          bool_and_function || bool_or_function || every_function;
      const bool stddev_pop_function = function == "STDDEV_POP";
      const bool variance_pop_function = function == "VARIANCE_POP";
      const bool stddev_function = function == "STDDEV";
      const bool variance_function = function == "VARIANCE";
      const bool stddev_samp_function = function == "STDDEV_SAMP";
      const bool variance_samp_function = function == "VARIANCE_SAMP";
      const bool regr_count_function = function == "REGR_COUNT";
      const bool approx_count_distinct_function =
          function == "APPROX_COUNT_DISTINCT";
      const bool approx_median_function = function == "APPROX_MEDIAN";
      const bool string_agg_function = function == "STRING_AGG";
      const bool listagg_function = function == "LISTAGG";
      const bool array_agg_function = function == "ARRAY_AGG";
      const bool json_agg_function = function == "JSON_AGG";
      const bool json_object_agg_function = function == "JSON_OBJECT_AGG";
      const bool ordered_single_collection_function =
          array_agg_function || json_agg_function;
      const bool ordered_collection_function =
          ordered_single_collection_function || json_object_agg_function;
      const bool approx_top_k_function = function == "APPROX_TOP_K";
      const bool mode_function = function == "MODE";
      const bool percentile_cont_function = function == "PERCENTILE_CONT";
      const bool percentile_disc_function = function == "PERCENTILE_DISC";
      const bool approx_percentile_cont_function =
          function == "APPROX_PERCENTILE_CONT";
      const bool approx_percentile_disc_function =
          function == "APPROX_PERCENTILE_DISC";
      const bool percentile_function =
          percentile_cont_function || percentile_disc_function ||
          approx_percentile_cont_function || approx_percentile_disc_function;
      const bool rank_function = function == "RANK";
      const bool dense_rank_function = function == "DENSE_RANK";
      const bool percent_rank_function = function == "PERCENT_RANK";
      const bool cume_dist_function = function == "CUME_DIST";
      const bool hypothetical_function =
          rank_function || dense_rank_function || percent_rank_function ||
          cume_dist_function;
      const bool direct_numeric_ordered_function =
          percentile_function || hypothetical_function;
      const bool pair_function =
          function == "CORR" || function == "COVAR_POP" ||
          function == "COVAR_SAMP" || regr_count_function ||
          function == "REGR_AVGX" || function == "REGR_AVGY" ||
          function == "REGR_INTERCEPT" || function == "REGR_R2" ||
          function == "REGR_SLOPE" || function == "REGR_SXX" ||
          function == "REGR_SXY" || function == "REGR_SYY";
      const bool expression_function =
          sum_function || avg_function || min_function || max_function ||
          boolean_function ||
          stddev_pop_function || variance_pop_function || stddev_function ||
          variance_function || stddev_samp_function || variance_samp_function ||
          approx_count_distinct_function || approx_median_function ||
          string_agg_function || listagg_function || mode_function ||
          direct_numeric_ordered_function || pair_function ||
          ordered_collection_function || approx_top_k_function;
      const auto aggregate_function_uuid =
          EngineIssuedAggregateFunctionUuid(statement_context, function);
      const bool count_star = count_function &&
                              aggregate_expression != ast.expressions.end() &&
                              aggregate_expression->child_expression_ids.empty();
      const std::uint8_t result_profile_kind =
          array_agg_function
              ? 10
              : ((json_agg_function || json_object_agg_function ||
                  approx_top_k_function)
                     ? 8
                     : ((string_agg_function || listagg_function)
                            ? 4
                            : ((count_function || regr_count_function ||
                                approx_count_distinct_function ||
                                hypothetical_function)
                                   ? 1
                                   : (boolean_function ? 6 : 2))));
      const std::size_t expected_argument_count =
          (listagg_function || json_object_agg_function)
              ? 3
              : ((pair_function || string_agg_function ||
                  ordered_single_collection_function ||
                  approx_top_k_function ||
                  direct_numeric_ordered_function)
                     ? 2
                     : 1);
      const auto result_profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == result_profile_kind &&
                   candidate.slot == 0;
          });
      const auto direct_text_profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == 3 && candidate.slot == 0;
          });
      const auto direct_numeric_profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == 1 &&
                   candidate.slot == (hypothetical_function ? 1 : 0);
          });
      bool argument_profile_exact = count_star;
      if (!count_star && aggregate_expression != ast.expressions.end() &&
          aggregate_expression->child_expression_ids.size() ==
              expected_argument_count &&
          result_profile != statement_context.descriptor_profiles.end()) {
        argument_profile_exact = true;
        for (std::size_t ordinal = 0;
             ordinal < aggregate_expression->child_expression_ids.size();
             ++ordinal) {
          const auto expression_id =
              aggregate_expression->child_expression_ids[ordinal];
          const auto argument = std::ranges::find_if(
              ast.expressions, [&](const auto& candidate) {
                return candidate.expression_id == expression_id;
              });
          if (argument == ast.expressions.end()) {
            argument_profile_exact = false;
            break;
          }
          const bool direct_text =
              (string_agg_function || listagg_function) && ordinal == 1;
          const bool direct_numeric =
              (direct_numeric_ordered_function || approx_top_k_function) &&
              ordinal == 0;
          if (direct_text || direct_numeric) {
            argument_profile_exact =
                argument->expression_kind ==
                    NativeExpressionAstKind::kLiteral &&
                argument->literal_kind ==
                    (direct_text ? NativeLiteralAstKind::kString
                                 : NativeLiteralAstKind::kNumeric) &&
                argument->child_expression_ids.empty() &&
                argument->operator_name.empty();
          } else {
            const auto source_expression = std::ranges::find(
                relation.output_expression_ids, argument->expression_id);
            argument_profile_exact =
                source_expression != relation.output_expression_ids.end() &&
                static_cast<std::size_t>(std::distance(
                    relation.output_expression_ids.begin(),
                    source_expression)) < context.descriptors.size();
          }
          if (!argument_profile_exact) break;
        }
      }
      if (aggregate_expression == ast.expressions.end() ||
          aggregate_expression->expression_kind !=
              NativeExpressionAstKind::kFunctionCall ||
          (!count_function && !expression_function) ||
          (aggregate_expression->child_expression_ids.size() !=
           (count_star
                ? 0
                : expected_argument_count)) ||
          !argument_profile_exact ||
          result_profile == statement_context.descriptor_profiles.end() ||
          ((count_function || regr_count_function ||
            approx_count_distinct_function || hypothetical_function) &&
           result_profile->nullable) ||
          (expression_function && !regr_count_function &&
           !approx_count_distinct_function &&
           !hypothetical_function &&
           !result_profile->nullable) ||
          !CanonicalUuidBytes(result_profile->descriptor_uuid).has_value() ||
          !CanonicalUuidBytes(result_profile->type_uuid).has_value() ||
          ((string_agg_function || listagg_function) &&
           (direct_text_profile ==
                statement_context.descriptor_profiles.end() ||
            direct_text_profile->nullable ||
            !CanonicalUuidBytes(direct_text_profile->descriptor_uuid)
                 .has_value() ||
            !CanonicalUuidBytes(direct_text_profile->type_uuid).has_value())) ||
          ((direct_numeric_ordered_function || approx_top_k_function) &&
           (direct_numeric_profile ==
                statement_context.descriptor_profiles.end() ||
            direct_numeric_profile->nullable ||
            !CanonicalUuidBytes(direct_numeric_profile->descriptor_uuid)
                 .has_value() ||
            !CanonicalUuidBytes(direct_numeric_profile->type_uuid)
                 .has_value())) ||
          !aggregate_function_uuid.has_value()) {
        return fail("catalog_global_aggregate_profile_unavailable");
      }
      aggregate_binding_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id = *aggregate_binding_id;
      descriptor.descriptor_uuid = result_profile->descriptor_uuid;
      descriptor.type_uuid = result_profile->type_uuid;
      descriptor.nullability = result_profile->nullable
                                   ? BoundNullability::kNullable
                                   : BoundNullability::kNonNull;
      context.descriptors.push_back(std::move(descriptor));
      context.expressions.push_back(
          {*aggregate_binding_id, *aggregate_binding_id,
           std::string(*aggregate_function_uuid),
           std::nullopt});
      if (string_agg_function || listagg_function) {
        const auto separator_expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     aggregate_expression->child_expression_ids[1];
            });
        NativeDescriptorBindingInput separator_descriptor;
        separator_descriptor.descriptor_id =
            static_cast<std::uint32_t>(context.descriptors.size() + 1);
        separator_descriptor.descriptor_uuid =
            direct_text_profile->descriptor_uuid;
        separator_descriptor.type_uuid = direct_text_profile->type_uuid;
        separator_descriptor.nullability = BoundNullability::kNonNull;
        context.descriptors.push_back(std::move(separator_descriptor));
        context.expressions.push_back(
            {separator_expression->expression_id,
             context.descriptors.back().descriptor_id, std::nullopt,
             std::nullopt});
      } else if (direct_numeric_ordered_function || approx_top_k_function) {
        const auto direct_expression = std::ranges::find_if(
            ast.expressions, [&](const auto& candidate) {
              return candidate.expression_id ==
                     aggregate_expression->child_expression_ids.front();
            });
        NativeDescriptorBindingInput direct_descriptor;
        direct_descriptor.descriptor_id =
            static_cast<std::uint32_t>(context.descriptors.size() + 1);
        direct_descriptor.descriptor_uuid =
            direct_numeric_profile->descriptor_uuid;
        direct_descriptor.type_uuid = direct_numeric_profile->type_uuid;
        direct_descriptor.nullability = BoundNullability::kNonNull;
        context.descriptors.push_back(std::move(direct_descriptor));
        context.expressions.push_back(
            {direct_expression->expression_id,
             context.descriptors.back().descriptor_id, std::nullopt,
             std::nullopt});
      }
      if (count_function) {
        aggregate_output_name = "row_count";
        aggregate_semantic = count_star
                                 ? "aggregate.global-count-star.v1"
                                 : "aggregate.global-count-expression.v1";
      } else if (sum_function) {
        aggregate_output_name = "total_amount";
        aggregate_semantic = "aggregate.global-sum-expression.v1";
      } else if (avg_function) {
        aggregate_output_name = "average_value";
        aggregate_semantic = "aggregate.global-avg-expression.v1";
      } else if (min_function) {
        aggregate_output_name = "minimum_value";
        aggregate_semantic = "aggregate.global-min-expression.v1";
      } else if (max_function) {
        aggregate_output_name = "maximum_value";
        aggregate_semantic = "aggregate.global-max-expression.v1";
      } else if (bool_and_function) {
        aggregate_output_name = "bool_and_value";
        aggregate_semantic = "aggregate.global-bool-and-expression.v1";
      } else if (bool_or_function) {
        aggregate_output_name = "bool_or_value";
        aggregate_semantic = "aggregate.global-bool-or-expression.v1";
      } else if (every_function) {
        aggregate_output_name = "every_value";
        aggregate_semantic = "aggregate.global-every-expression.v1";
      } else if (function == "CORR") {
        aggregate_output_name = "corr_value";
        aggregate_semantic = "aggregate.global-corr-expression.v1";
      } else if (function == "COVAR_POP") {
        aggregate_output_name = "covar_pop_value";
        aggregate_semantic = "aggregate.global-covar-pop-expression.v1";
      } else if (function == "COVAR_SAMP") {
        aggregate_output_name = "covar_samp_value";
        aggregate_semantic = "aggregate.global-covar-samp-expression.v1";
      } else if (regr_count_function) {
        aggregate_output_name = "regr_count_value";
        aggregate_semantic = "aggregate.global-regr-count-expression.v1";
      } else if (function == "REGR_AVGX") {
        aggregate_output_name = "regr_avgx_value";
        aggregate_semantic = "aggregate.global-regr-avgx-expression.v1";
      } else if (function == "REGR_AVGY") {
        aggregate_output_name = "regr_avgy_value";
        aggregate_semantic = "aggregate.global-regr-avgy-expression.v1";
      } else if (function == "REGR_INTERCEPT") {
        aggregate_output_name = "regr_intercept_value";
        aggregate_semantic = "aggregate.global-regr-intercept-expression.v1";
      } else if (function == "REGR_R2") {
        aggregate_output_name = "regr_r2_value";
        aggregate_semantic = "aggregate.global-regr-r2-expression.v1";
      } else if (function == "REGR_SLOPE") {
        aggregate_output_name = "regr_slope_value";
        aggregate_semantic = "aggregate.global-regr-slope-expression.v1";
      } else if (function == "REGR_SXX") {
        aggregate_output_name = "regr_sxx_value";
        aggregate_semantic = "aggregate.global-regr-sxx-expression.v1";
      } else if (function == "REGR_SXY") {
        aggregate_output_name = "regr_sxy_value";
        aggregate_semantic = "aggregate.global-regr-sxy-expression.v1";
      } else if (function == "REGR_SYY") {
        aggregate_output_name = "regr_syy_value";
        aggregate_semantic = "aggregate.global-regr-syy-expression.v1";
      } else if (stddev_pop_function) {
        aggregate_output_name = "stddev_pop_value";
        aggregate_semantic = "aggregate.global-stddev-pop-expression.v1";
      } else if (variance_pop_function) {
        aggregate_output_name = "variance_pop_value";
        aggregate_semantic = "aggregate.global-variance-pop-expression.v1";
      } else if (stddev_function) {
        aggregate_output_name = "stddev_value";
        aggregate_semantic = "aggregate.global-stddev-expression.v1";
      } else if (variance_function) {
        aggregate_output_name = "variance_value";
        aggregate_semantic = "aggregate.global-variance-expression.v1";
      } else if (stddev_samp_function) {
        aggregate_output_name = "stddev_samp_value";
        aggregate_semantic = "aggregate.global-stddev-samp-expression.v1";
      } else if (variance_samp_function) {
        aggregate_output_name = "variance_samp_value";
        aggregate_semantic = "aggregate.global-variance-samp-expression.v1";
      } else if (approx_count_distinct_function) {
        aggregate_output_name = "approx_count_distinct_value";
        aggregate_semantic =
            "aggregate.global-approx-count-distinct-expression.v1";
      } else if (approx_median_function) {
        aggregate_output_name = "approx_median_value";
        aggregate_semantic = "aggregate.global-approx-median-expression.v1";
      } else if (string_agg_function) {
        aggregate_output_name = "string_agg_value";
        aggregate_semantic = "aggregate.global-string-agg-expression.v1";
      } else if (listagg_function) {
        aggregate_output_name = "listagg_value";
        aggregate_semantic = "aggregate.global-listagg-ordered-expression.v1";
      } else if (array_agg_function) {
        aggregate_output_name = "array_agg_value";
        aggregate_semantic =
            "aggregate.global-array-agg-ordered-expression.v1";
      } else if (json_agg_function) {
        aggregate_output_name = "json_agg_value";
        aggregate_semantic =
            "aggregate.global-json-agg-ordered-expression.v1";
      } else if (json_object_agg_function) {
        aggregate_output_name = "json_object_agg_value";
        aggregate_semantic =
            "aggregate.global-json-object-agg-ordered-expression.v1";
      } else if (approx_top_k_function) {
        aggregate_output_name = "approx_top_k_value";
        aggregate_semantic = "aggregate.global-approx-top-k-expression.v1";
      } else if (mode_function) {
        aggregate_output_name = "mode_value";
        aggregate_semantic = "aggregate.global-mode-ordered-expression.v1";
      } else if (percentile_cont_function) {
        aggregate_output_name = "percentile_cont_value";
        aggregate_semantic =
            "aggregate.global-percentile-cont-ordered-expression.v1";
      } else if (percentile_disc_function) {
        aggregate_output_name = "percentile_disc_value";
        aggregate_semantic =
            "aggregate.global-percentile-disc-ordered-expression.v1";
      } else if (approx_percentile_cont_function) {
        aggregate_output_name = "approx_percentile_cont_value";
        aggregate_semantic =
            "aggregate.global-approx-percentile-cont-ordered-expression.v1";
      } else if (approx_percentile_disc_function) {
        aggregate_output_name = "approx_percentile_disc_value";
        aggregate_semantic =
            "aggregate.global-approx-percentile-disc-ordered-expression.v1";
      } else if (rank_function) {
        aggregate_output_name = "rank_value";
        aggregate_semantic =
            "aggregate.global-rank-hypothetical-expression.v1";
      } else if (dense_rank_function) {
        aggregate_output_name = "dense_rank_value";
        aggregate_semantic =
            "aggregate.global-dense-rank-hypothetical-expression.v1";
      } else if (percent_rank_function) {
        aggregate_output_name = "percent_rank_value";
        aggregate_semantic =
            "aggregate.global-percent-rank-hypothetical-expression.v1";
      } else {
        aggregate_output_name = "cume_dist_value";
        aggregate_semantic =
            "aggregate.global-cume-dist-hypothetical-expression.v1";
      }
    }
    if (limit_composition) {
      const auto numeric_profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == 1 &&
                   candidate.slot == (aggregate_composition ? 1 : 0);
          });
      if (numeric_profile == statement_context.descriptor_profiles.end() ||
          numeric_profile->nullable ||
          !CanonicalUuidBytes(numeric_profile->descriptor_uuid).has_value() ||
          !CanonicalUuidBytes(numeric_profile->type_uuid).has_value()) {
        return fail("catalog_numeric_descriptor_profile_unavailable");
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = numeric_profile->descriptor_uuid;
      descriptor.type_uuid = numeric_profile->type_uuid;
      descriptor.nullability = BoundNullability::kNonNull;
      context.descriptors.push_back(std::move(descriptor));

    }
    if (filter_composition) {
      const auto boolean_profile = std::ranges::find_if(
          statement_context.descriptor_profiles, [](const auto& candidate) {
            return candidate.profile_kind == 6 && candidate.slot == 0;
          });
      if (boolean_profile == statement_context.descriptor_profiles.end() ||
          !boolean_profile->nullable ||
          !CanonicalUuidBytes(boolean_profile->descriptor_uuid).has_value() ||
          !CanonicalUuidBytes(boolean_profile->type_uuid).has_value()) {
        return fail("catalog_filter_boolean_descriptor_profile_unavailable");
      }
      NativeDescriptorBindingInput descriptor;
      descriptor.descriptor_id =
          static_cast<std::uint32_t>(context.descriptors.size() + 1);
      descriptor.descriptor_uuid = boolean_profile->descriptor_uuid;
      descriptor.type_uuid = boolean_profile->type_uuid;
      descriptor.nullability = BoundNullability::kNullable;
      context.descriptors.push_back(std::move(descriptor));
    }
    for (const auto& downstream : ast.relations) {
      if (downstream.relation_id == source_relation->relation_id) continue;
      if (aggregate_composition &&
          (downstream.relation_id == aggregate_relation->relation_id ||
           (limit_composition &&
            downstream.relation_id == limit_relation->relation_id))) {
        const auto output_id =
            static_cast<std::uint32_t>(context.outputs.size() + 1);
        context.outputs.push_back(
            {output_id, *aggregate_binding_id,
             aggregate_output_name,
             *aggregate_binding_id, true, 0, downstream.relation_id});
        continue;
      }
      std::vector<std::size_t> downstream_source_ordinals;
      if (wildcard_projection) {
        downstream_source_ordinals.reserve(source_column_indexes.size());
        for (std::size_t ordinal = 0; ordinal < source_column_indexes.size();
             ++ordinal) {
          downstream_source_ordinals.push_back(ordinal);
        }
      } else {
        downstream_source_ordinals.reserve(
            downstream.output_expression_ids.size());
        for (const auto expression_id : downstream.output_expression_ids) {
          const auto source_expression = std::ranges::find(
              relation.output_expression_ids, expression_id);
          if (source_expression == relation.output_expression_ids.end()) {
            return fail("catalog_operator_projection_source_unresolved");
          }
          downstream_source_ordinals.push_back(
              static_cast<std::size_t>(std::distance(
                  relation.output_expression_ids.begin(), source_expression)));
        }
      }
      if (downstream_source_ordinals.empty()) {
        return fail("catalog_operator_projection_empty");
      }
      const auto first_output_id =
          static_cast<std::uint32_t>(context.outputs.size() + 1);
      for (std::size_t ordinal = 0;
           ordinal < downstream_source_ordinals.size();
           ++ordinal) {
        const auto source_ordinal = downstream_source_ordinals[ordinal];
        const auto& column =
            projection.columns[source_column_indexes[source_ordinal]];
        const auto binding_id =
            static_cast<std::uint32_t>(source_ordinal + 1);
        context.outputs.push_back(
            {first_output_id + static_cast<std::uint32_t>(ordinal), binding_id,
             column.canonical_name_key, binding_id, true,
             static_cast<std::uint32_t>(ordinal), downstream.relation_id});
      }
    }
    if (aggregate_composition) {
      context.relations.push_back(
          {aggregate_relation->relation_id, aggregate_semantic});
    }
    context.catalog_relations.push_back(std::move(catalog_relation));
    return context;
  }

  std::array<std::uint16_t, 7> next_profile_slot{};
  const auto allocate_descriptor =
      [&](std::uint8_t kind) -> std::optional<std::uint32_t> {
    const auto slot = next_profile_slot[kind]++;
    const auto profile = std::ranges::find_if(
        statement_context.descriptor_profiles, [&](const auto& candidate) {
          return candidate.profile_kind == kind && candidate.slot == slot;
        });
    if (profile == statement_context.descriptor_profiles.end()) {
      return std::nullopt;
    }
    NativeDescriptorBindingInput descriptor;
    descriptor.descriptor_id =
        static_cast<std::uint32_t>(context.descriptors.size() + 1);
    descriptor.descriptor_uuid = profile->descriptor_uuid;
    descriptor.type_uuid = profile->type_uuid;
    descriptor.nullability = profile->nullable
                                 ? BoundNullability::kNullable
                                 : BoundNullability::kNonNull;
    if (!profile->collation_uuid.empty()) {
      descriptor.collation_uuid = profile->collation_uuid;
    }
    if (profile->width != 0) descriptor.width_precision_scale.width = profile->width;
    if (profile->precision != 0) {
      descriptor.width_precision_scale.precision = profile->precision;
      descriptor.width_precision_scale.scale = profile->scale;
    }
    context.descriptors.push_back(std::move(descriptor));
    return context.descriptors.back().descriptor_id;
  };

  const auto aggregate = std::ranges::find_if(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind == NativeRelationAstKind::kAggregate;
      });
  const bool aggregate_profile = aggregate != ast.relations.end();
  std::unordered_map<std::uint32_t, std::uint32_t> descriptor_by_expression;
  std::vector<std::uint32_t> values_descriptor_by_ordinal;
  if (ast.values_rows.empty()) return fail("values_rows_missing");
  const auto values_arity = ast.values_rows.front().expression_ids.size();
  values_descriptor_by_ordinal.reserve(values_arity);
  for (std::size_t ordinal = 0; ordinal < values_arity; ++ordinal) {
    std::uint8_t kind = 2;  // numeric nullable for aggregate keys/arguments
    if (!aggregate_profile) {
      const auto expression_id = ast.values_rows.front().expression_ids[ordinal];
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      if (expression != ast.expressions.end() &&
          expression->literal_kind == NativeLiteralAstKind::kString) {
        kind = 4;
      } else if (expression != ast.expressions.end() &&
                 expression->literal_kind == NativeLiteralAstKind::kBoolean) {
        kind = 6;
      } else {
        kind = 2;
      }
    }
    const auto descriptor = allocate_descriptor(kind);
    if (!descriptor.has_value()) return fail("values_descriptor_profile_exhausted");
    values_descriptor_by_ordinal.push_back(*descriptor);
  }
  for (const auto& row : ast.values_rows) {
    if (row.expression_ids.size() != values_descriptor_by_ordinal.size()) {
      return fail("values_arity_mismatch");
    }
    for (std::size_t ordinal = 0; ordinal < row.expression_ids.size(); ++ordinal) {
      descriptor_by_expression[row.expression_ids[ordinal]] =
          values_descriptor_by_ordinal[ordinal];
    }
  }

  std::optional<std::uint32_t> count_descriptor;
  std::optional<std::uint32_t> sum_descriptor;
  std::optional<std::uint32_t> threshold_descriptor;
  std::optional<std::uint32_t> boolean_descriptor;
  std::vector<std::uint32_t> grouping_metadata_descriptors;
  const auto ensure = [&](std::optional<std::uint32_t>* value,
                          std::uint8_t kind) -> std::optional<std::uint32_t> {
    if (!value->has_value()) *value = allocate_descriptor(kind);
    return *value;
  };
  const auto values_column_for_name = [&](std::string_view name) -> std::size_t {
    if (name == "key_a") return 0;
    if (name == "key_b" && values_arity > 1) return 1;
    if (name == "amount") return values_arity - 1;
    return 0;
  };

  for (const auto& expression : ast.expressions) {
    auto descriptor = descriptor_by_expression.find(expression.expression_id);
    std::optional<std::string> function_uuid;
    std::optional<std::string> bound_name_uuid;
    if (descriptor == descriptor_by_expression.end()) {
      std::optional<std::uint32_t> selected;
      if (expression.expression_kind == NativeExpressionAstKind::kIdentifier) {
        const auto ordinal = values_column_for_name(expression.spelling);
        if (ordinal >= values_descriptor_by_ordinal.size()) {
          return fail("identifier_column_out_of_range");
        }
        selected = values_descriptor_by_ordinal[ordinal];
        bound_name_uuid =
            context.descriptors[*selected - 1].descriptor_uuid;
      } else if (expression.expression_kind ==
                 NativeExpressionAstKind::kFunctionCall) {
        const auto function = ToUpperAscii(expression.operator_name);
        if (function == "COUNT") {
          selected = ensure(&count_descriptor, 1);
          function_uuid = statement_context.count_function_uuid;
        } else if (function == "SUM") {
          selected = ensure(&sum_descriptor, 2);
          function_uuid = statement_context.sum_function_uuid;
        }
      } else if ((expression.expression_kind == NativeExpressionAstKind::kUnary &&
                  expression.operator_name == "grouping") ||
                 (expression.expression_kind == NativeExpressionAstKind::kBinary &&
                  expression.operator_name == "grouping_id")) {
        const auto metadata = allocate_descriptor(1);
        if (metadata.has_value()) grouping_metadata_descriptors.push_back(*metadata);
        selected = metadata;
      } else if (expression.expression_kind == NativeExpressionAstKind::kLiteral) {
        selected = ensure(&threshold_descriptor, 1);
      } else if (expression.expression_kind == NativeExpressionAstKind::kUnary ||
                 expression.expression_kind == NativeExpressionAstKind::kBinary) {
        selected = ensure(&boolean_descriptor, 6);
      } else if (expression.expression_kind ==
                     NativeExpressionAstKind::kParenthesized &&
                 expression.child_expression_ids.size() == 1) {
        const auto child = descriptor_by_expression.find(
            expression.child_expression_ids.front());
        if (child != descriptor_by_expression.end()) selected = child->second;
      }
      if (!selected.has_value()) return fail("expression_profile_unavailable");
      descriptor_by_expression[expression.expression_id] = *selected;
      descriptor = descriptor_by_expression.find(expression.expression_id);
    } else if (expression.expression_kind == NativeExpressionAstKind::kFunctionCall) {
      const auto function = ToUpperAscii(expression.operator_name);
      if (function == "COUNT") function_uuid = statement_context.count_function_uuid;
      if (function == "SUM") function_uuid = statement_context.sum_function_uuid;
    }
    context.expressions.push_back(
        {expression.expression_id, descriptor->second,
         std::move(function_uuid), std::move(bound_name_uuid)});
  }

  std::uint32_t output_id = 1;
  for (const auto& relation : ast.relations) {
    static constexpr std::array<std::string_view, 3> kValuesNames{
        "key_a", "key_b", "amount"};
    static constexpr std::array<std::string_view, 7> kAggregateNames{
        "key_a", "key_b", "row_count", "total_amount",
        "grouping_a", "grouping_b", "grouping_id"};
    const bool one_key =
        relation.aggregate_projection_form ==
        NativeAggregateProjectionForm::kKeyCountSum;
    for (std::size_t ordinal = 0;
         ordinal < relation.output_expression_ids.size(); ++ordinal) {
      const auto expression_id = relation.output_expression_ids[ordinal];
      const auto descriptor = descriptor_by_expression.find(expression_id);
      if (descriptor == descriptor_by_expression.end()) {
        return fail("output_expression_descriptor_missing");
      }
      std::string name;
      if (relation.relation_kind == NativeRelationAstKind::kValues) {
        name = one_key || values_arity == 2
                   ? (ordinal == 0 ? "key_a" : "amount")
                   : (ordinal < kValuesNames.size()
                          ? std::string(kValuesNames[ordinal])
                          : "column_" + std::to_string(ordinal + 1));
      } else if (one_key) {
        static constexpr std::array<std::string_view, 3> kNames{
            "key_a", "row_count", "total_amount"};
        name = std::string(kNames[ordinal]);
      } else {
        name = ordinal < kAggregateNames.size()
                   ? std::string(kAggregateNames[ordinal])
                   : "column_" + std::to_string(ordinal + 1);
      }
      context.outputs.push_back(
          {output_id++, expression_id, std::move(name), descriptor->second,
           true, static_cast<std::uint32_t>(ordinal), relation.relation_id});
    }
    if (relation.relation_kind == NativeRelationAstKind::kAggregate) {
      const auto semantic = NativeAggregateSemantic(
          relation.aggregate_grouping_form,
          relation.aggregate_projection_form);
      if (semantic.empty()) return fail("aggregate_semantic_unavailable");
      context.relations.push_back(
          {relation.relation_id, std::string(semantic)});
    } else if (relation.relation_kind == NativeRelationAstKind::kFilter) {
      const auto semantic = NativeFilterSemantic(ast, relation);
      if (semantic.empty()) return fail("filter_semantic_unavailable");
      context.relations.push_back({relation.relation_id, semantic});
    }
  }
  return context;
}

using CanonicalBytes = std::vector<std::uint8_t>;

void CanonicalAppendU16(CanonicalBytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void CanonicalAppendU32(CanonicalBytes* out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void CanonicalAppendU64(CanonicalBytes* out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void CanonicalStoreU16(CanonicalBytes* out,
                       std::size_t offset,
                       std::uint16_t value) {
  (*out)[offset] = static_cast<std::uint8_t>(value);
  (*out)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void CanonicalStoreU32(CanonicalBytes* out,
                       std::size_t offset,
                       std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    (*out)[offset + shift / 8] =
        static_cast<std::uint8_t>(value >> shift);
  }
}

void CanonicalStoreU64(CanonicalBytes* out,
                       std::size_t offset,
                       std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    (*out)[offset + shift / 8] =
        static_cast<std::uint8_t>(value >> shift);
  }
}

void CanonicalAppendText(CanonicalBytes* out, std::string_view value) {
  CanonicalAppendU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::optional<std::array<std::uint8_t, 16>> CanonicalUuidBytes(
    std::string_view text) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return std::nullopt;
  }
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
  };
  std::array<std::uint8_t, 16> bytes{};
  std::size_t output = 0;
  for (std::size_t input = 0; input < text.size();) {
    if (text[input] == '-') {
      ++input;
      continue;
    }
    if (input + 1 >= text.size() || output >= bytes.size()) {
      return std::nullopt;
    }
    const auto high = nibble(text[input]);
    const auto low = nibble(text[input + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    bytes[output++] = static_cast<std::uint8_t>((high << 4) | low);
    input += 2;
  }
  if (output != bytes.size() ||
      std::ranges::all_of(bytes, [](std::uint8_t byte) { return byte == 0; })) {
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::array<std::uint8_t, 32>> CanonicalSha256(
    const CanonicalBytes& bytes) {
  std::array<std::uint8_t, 32> digest{};
  unsigned digest_size = 0;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr ||
      EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1 ||
      digest_size != digest.size()) {
    if (context != nullptr) EVP_MD_CTX_free(context);
    return std::nullopt;
  }
  EVP_MD_CTX_free(context);
  return digest;
}

std::optional<CanonicalBytes> EncodeNativeQueryOperationBinary(
    const SblrEnvelope& lowered,
    const ParserStatementContext& statement_context,
    const SessionContext& session) {
  constexpr std::uint16_t kSectionCount = 9;
  constexpr std::uint16_t kHeaderSize = 64;
  constexpr std::uint32_t kSectionTableSize = 216;
  constexpr std::uint32_t kPayloadOffset = 280;
  constexpr std::array<std::uint16_t, kSectionCount> kTags{
      1, 2, 3, 4, 5, 6, 7, 8, 9};
  const auto parser_uuid =
      CanonicalUuidBytes(session.admitted_parser_package_uuid);
  const auto registry_uuid =
      CanonicalUuidBytes(statement_context.catalog_epoch_uuid);
  if (lowered.operation_id != "query.execute" ||
      lowered.sblr_opcode != "SBLR_QUERY_EXECUTE" ||
      lowered.result_shape_key != "query_execute_result" ||
      !parser_uuid.has_value() || !registry_uuid.has_value() ||
      statement_context.descriptor_profiles.empty()) {
    return std::nullopt;
  }
  const auto value_type_uuid = CanonicalUuidBytes(
      statement_context.descriptor_profiles.front().type_uuid);
  if (!value_type_uuid.has_value()) return std::nullopt;

  std::array<CanonicalBytes, kSectionCount> sections;
  CanonicalAppendText(&sections[0], lowered.operation_id);
  CanonicalAppendText(&sections[1], lowered.sblr_opcode);
  sections[2].insert(sections[2].end(), parser_uuid->begin(),
                     parser_uuid->end());
  CanonicalAppendU32(&sections[2],
                     session.admitted_parser_package_version_major);
  CanonicalAppendU32(&sections[2],
                     session.admitted_parser_package_version_minor);
  CanonicalAppendU32(&sections[2],
                     session.admitted_parser_package_version_patch);
  sections[3].insert(sections[3].end(), registry_uuid->begin(),
                     registry_uuid->end());
  CanonicalAppendU32(&sections[4],
                     static_cast<std::uint32_t>(lowered.operands.size()));
  std::uint32_t ordinal = 1;
  for (const auto& operand : lowered.operands) {
    CanonicalAppendU32(&sections[4], ordinal++);
    CanonicalAppendText(&sections[4], operand.type);
    const bool numeric_name = !operand.name.empty() &&
        std::ranges::all_of(operand.name, [](unsigned char ch) {
          return ch >= '0' && ch <= '9';
        });
    const auto property_uuid =
        operand.type == "relational_property_v1"
            ? CanonicalUuidBytes(operand.name)
            : std::optional<std::array<std::uint8_t, 16>>{};
    std::string encoded_name;
    if (numeric_name) {
      encoded_name = "slot_" + operand.name;
    } else if (property_uuid.has_value()) {
      static constexpr char kHex[] = "0123456789abcdef";
      encoded_name = "property_";
      encoded_name.reserve(41);
      for (const auto byte : *property_uuid) {
        encoded_name.push_back(kHex[byte >> 4]);
        encoded_name.push_back(kHex[byte & 0x0f]);
      }
    } else {
      encoded_name = operand.name;
    }
    CanonicalAppendText(&sections[4], encoded_name);
    CanonicalAppendU16(&sections[4], 5);  // literal_typed
    CanonicalAppendU16(&sections[4], 0);
    CanonicalAppendU64(&sections[4], 24 + operand.value.size());
    sections[4].insert(sections[4].end(), value_type_uuid->begin(),
                       value_type_uuid->end());
    CanonicalAppendU64(&sections[4], operand.value.size());
    sections[4].insert(sections[4].end(), operand.value.begin(),
                       operand.value.end());
  }
  CanonicalAppendText(&sections[5], lowered.result_shape_key);
  CanonicalAppendText(&sections[6], lowered.diagnostic_shape_key);
  CanonicalAppendText(&sections[7],
                      lowered.trace_key.empty() ? "query.execute.native"
                                                : lowered.trace_key);

  CanonicalBytes provenance;
  constexpr char kDomain[] =
      "ScratchBird.SBOP.ProducerProvenance.V1\0";
  provenance.insert(provenance.end(), std::begin(kDomain),
                    std::end(kDomain) - 1);
  provenance.insert(provenance.end(), sections[2].begin(), sections[2].end());
  provenance.insert(provenance.end(), sections[3].begin(), sections[3].end());
  CanonicalAppendU16(&provenance, 0x1207);
  CanonicalAppendU16(&provenance, 1);
  CanonicalAppendU16(&provenance, 0);
  provenance.insert(provenance.end(), sections[0].begin(), sections[0].end());
  provenance.insert(provenance.end(), sections[1].begin(), sections[1].end());
  const auto digest = CanonicalSha256(provenance);
  if (!digest.has_value()) return std::nullopt;
  sections[8].assign(digest->begin(), digest->end());

  std::uint64_t payload_size = 0;
  for (const auto& section : sections) payload_size += section.size();
  const std::uint64_t total_size = kPayloadOffset + payload_size + 16;
  if (total_size > scratchbird::engine::kSblrMaxPayloadBytes) {
    return std::nullopt;
  }
  CanonicalBytes encoded(static_cast<std::size_t>(total_size), 0);
  CanonicalStoreU32(&encoded, 0, 0x504f4253u);
  CanonicalStoreU16(&encoded, 4, 1);
  CanonicalStoreU16(&encoded, 6, 0);
  CanonicalStoreU16(&encoded, 8, kHeaderSize);
  CanonicalStoreU16(&encoded, 10, kSectionCount);
  CanonicalStoreU16(&encoded, 16, 0x1207);
  CanonicalStoreU16(&encoded, 18, 1);
  CanonicalStoreU16(&encoded, 20, 0);
  CanonicalStoreU32(&encoded, 24, kHeaderSize);
  CanonicalStoreU32(&encoded, 28, kSectionTableSize);
  CanonicalStoreU32(&encoded, 32, kPayloadOffset);
  CanonicalStoreU64(&encoded, 40, payload_size);
  CanonicalStoreU64(&encoded, 48, total_size);
  std::uint64_t section_offset = kPayloadOffset;
  for (std::size_t index = 0; index < sections.size(); ++index) {
    const auto table = kHeaderSize + index * 24;
    CanonicalStoreU16(&encoded, table, kTags[index]);
    CanonicalStoreU16(&encoded, table + 2, 1);
    CanonicalStoreU32(&encoded, table + 4, 1);
    CanonicalStoreU64(&encoded, table + 8, section_offset);
    CanonicalStoreU64(&encoded, table + 16, sections[index].size());
    std::copy(sections[index].begin(), sections[index].end(),
              encoded.begin() + static_cast<std::ptrdiff_t>(section_offset));
    section_offset += sections[index].size();
  }
  const auto trailer = encoded.size() - 16;
  CanonicalStoreU32(&encoded, trailer, 0x544f4253u);
  CanonicalStoreU32(
      &encoded, trailer + 4,
      scratchbird::engine::SblrCrc32c(encoded.data(), trailer));
  CanonicalStoreU64(&encoded, trailer + 8, total_size);
  return encoded;
}

CanonicalBytes CanonicalU16(std::uint16_t value) {
  CanonicalBytes out;
  CanonicalAppendU16(&out, value);
  return out;
}

CanonicalBytes CanonicalU32(std::uint32_t value) {
  CanonicalBytes out;
  CanonicalAppendU32(&out, value);
  return out;
}

CanonicalBytes CanonicalU64(std::uint64_t value) {
  CanonicalBytes out;
  CanonicalAppendU64(&out, value);
  return out;
}

CanonicalBytes CanonicalOptionalUuid(
    const std::array<std::uint8_t, 16>& uuid) {
  CanonicalBytes out{1};
  out.insert(out.end(), uuid.begin(), uuid.end());
  return out;
}

CanonicalBytes CanonicalStruct(std::uint32_t format,
                               std::uint8_t payload) {
  CanonicalBytes out;
  CanonicalAppendU32(&out, format);
  CanonicalAppendU16(&out, 1);
  CanonicalAppendU16(&out, 0);
  CanonicalAppendU64(&out, 1);
  out.push_back(payload);
  return out;
}

std::optional<ParserCanonicalSblrSubmission> BuildCanonicalNativeSubmission(
    const SblrEnvelope& lowered,
    const ParserStatementContext& statement_context,
    const SessionContext& session) {
  const auto operation = EncodeNativeQueryOperationBinary(
      lowered, statement_context, session);
  const auto database_uuid = CanonicalUuidBytes(session.database_uuid);
  const auto dialect_uuid =
      CanonicalUuidBytes(session.admitted_dialect_profile_uuid);
  const auto parser_uuid =
      CanonicalUuidBytes(session.admitted_parser_package_uuid);
  const auto registry_uuid =
      CanonicalUuidBytes(statement_context.catalog_epoch_uuid);
  const auto statement_uuid =
      CanonicalUuidBytes(statement_context.statement_uuid);
  const auto user_uuid = CanonicalUuidBytes(session.authenticated_user_uuid);
  if (!operation.has_value() || !database_uuid.has_value() ||
      !dialect_uuid.has_value() || !parser_uuid.has_value() ||
      !registry_uuid.has_value() || !statement_uuid.has_value() ||
      !user_uuid.has_value()) {
    return std::nullopt;
  }

  scratchbird::engine::SblrCanonicalContainer container;
  std::copy(database_uuid->begin(), database_uuid->end(),
            container.canonical_anchor.begin());
  std::copy(dialect_uuid->begin(), dialect_uuid->end(),
            container.canonical_anchor.begin() + 16);
  std::copy(parser_uuid->begin(), parser_uuid->end(),
            container.canonical_anchor.begin() + 32);
  const auto anchor_u16 = [&](std::size_t offset, std::uint16_t value) {
    container.canonical_anchor[offset] = static_cast<std::uint8_t>(value);
    container.canonical_anchor[offset + 1] =
        static_cast<std::uint8_t>(value >> 8);
  };
  const auto anchor_u32 = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) {
      container.canonical_anchor[offset + shift / 8] =
          static_cast<std::uint8_t>(value >> shift);
    }
  };
  const auto anchor_u64 = [&](std::size_t offset, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8) {
      container.canonical_anchor[offset + shift / 8] =
          static_cast<std::uint8_t>(value >> shift);
    }
  };
  anchor_u32(48, 1);
  anchor_u64(52, 1);
  anchor_u64(60, 1);
  anchor_u64(68, 1);
  std::copy(registry_uuid->begin(), registry_uuid->end(),
            container.canonical_anchor.begin() + 76);
  anchor_u64(92, 1);
  anchor_u16(100, 2);
  std::copy(statement_uuid->begin(), statement_uuid->end(),
            container.canonical_anchor.begin() + 116);
  container.operation_payload = *operation;
  auto container_bytes = scratchbird::engine::EncodeSblrContainer(container);

  scratchbird::engine::SblrExecutionEnvelopeV1 ingress;
  auto& fields = ingress.fields;
  fields[0].assign(statement_uuid->begin(), statement_uuid->end());
  fields[1] = CanonicalU16(1);
  fields[2] = CanonicalU16(0);
  fields[3] = CanonicalU32(0x00010001);
  fields[4] = CanonicalU16(2);
  fields[5] = {0};
  fields[6] = {1};
  CanonicalAppendU64(&fields[6], operation->size());
  fields[6].insert(fields[6].end(), operation->begin(), operation->end());
  fields[7] = {1};
  CanonicalAppendU32(
      &fields[7],
      scratchbird::engine::SblrCrc32c(operation->data(), operation->size()));
  fields[8] = CanonicalU64(operation->size());
  fields[9] = CanonicalU16(1);
  fields[10] = CanonicalOptionalUuid(*dialect_uuid);
  fields[11] = CanonicalOptionalUuid(*user_uuid);
  fields[12] = CanonicalStruct(0x1001, 1);
  fields[13] = CanonicalStruct(0x1002, 2);
  fields[14] = {0};
  fields[15] = CanonicalU64(1);
  fields[16] = CanonicalU32(0);
  fields[17] = CanonicalU32(0);
  fields[18] = CanonicalU32(0);
  fields[19] = {0};
  fields[20] = CanonicalU32(0);
  fields[21] = CanonicalStruct(0x1005, 5);
  fields[22] = {0};
  fields[23] = {0};
  fields[24] = {0};
  fields[25] = CanonicalU16(0);
  fields[26] = {0};
  fields[27] = {0};
  auto ingress_bytes =
      scratchbird::engine::EncodeSblrExecutionEnvelopeV1(ingress);
  if (container_bytes.empty() || ingress_bytes.empty()) return std::nullopt;

  ParserCanonicalSblrSubmission submission;
  submission.statement_uuid = statement_context.statement_uuid;
  submission.canonical_container_bytes = std::move(container_bytes);
  submission.canonical_execution_envelope_bytes = std::move(ingress_bytes);
  return submission;
}

bool IsWord(const Token& token, std::string_view word) {
  return !token.quoted && ToUpperAscii(token.text) == ToUpperAscii(word);
}

bool IsLiteralKind(TokenKind kind) {
  return kind == TokenKind::kNumericLiteral ||
         kind == TokenKind::kStringLiteral ||
         kind == TokenKind::kBinaryLiteral ||
         kind == TokenKind::kTemporalLiteral ||
         kind == TokenKind::kUuidLiteral ||
         kind == TokenKind::kBooleanLiteral ||
         kind == TokenKind::kNullLiteral ||
         kind == TokenKind::kDefaultLiteral ||
         kind == TokenKind::kDocumentLiteral ||
         kind == TokenKind::kVectorLiteral ||
         kind == TokenKind::kRegexLiteral ||
         kind == TokenKind::kRangeLiteral;
}

std::string JoinStable(const std::vector<std::string>& values) {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) out.push_back(';');
    out += value;
  }
  return out;
}

std::string NormalizeFrontdoorSql(std::string_view sql) {
  std::string out;
  bool in_space = false;
  bool in_string_literal = false;
  bool in_quoted_identifier = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char ch = sql[i];
    const auto uch = static_cast<unsigned char>(ch);
    if (in_quoted_identifier) {
      out.push_back(ch);
      if (ch == '"') {
        if (i + 1 < sql.size() && sql[i + 1] == '"') {
          out.push_back(sql[i + 1]);
          ++i;
        } else {
          in_quoted_identifier = false;
        }
      }
      continue;
    }
    if (in_string_literal) {
      out.push_back(ch);
      if (ch == '\'') {
        if (i + 1 < sql.size() && sql[i + 1] == '\'') {
          out.push_back(sql[i + 1]);
          ++i;
        } else {
          in_string_literal = false;
        }
      }
      continue;
    }
    if (std::isspace(uch)) {
      in_space = true;
      continue;
    }
    if (in_space && !out.empty()) out.push_back(' ');
    in_space = false;
    if (ch == '\'') {
      in_string_literal = true;
      out.push_back(ch);
      continue;
    }
    if (ch == '"') {
      in_quoted_identifier = true;
      out.push_back(ch);
      continue;
    }
    out.push_back(static_cast<char>(std::toupper(uch)));
  }
  return out;
}

std::string ParameterTypeShape(std::string_view sql) {
  std::string shape;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(sql[i]);
    if (sql[i] == '?') {
      if (!shape.empty()) shape.push_back(';');
      shape += "param";
      continue;
    }
    if (std::isdigit(ch)) {
      if (!shape.empty()) shape.push_back(';');
      shape += "numeric";
      while (i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) ++i;
      continue;
    }
    if (sql[i] == '\'') {
      if (!shape.empty()) shape.push_back(';');
      shape += "text";
      while (i + 1 < sql.size()) {
        ++i;
        if (sql[i] == '\'') break;
      }
    }
  }
  return shape.empty() ? "no_parameters" : shape;
}

// SEARCH_KEY: SBSQL_FRONTDOOR_LOWERING_CACHE_ODFR_011
// Parser-owned front-door lowering cache key. It reuses lowering artifacts only;
// execution, authorization, storage access, and transaction finality remain with
// the existing engine route and MGA/security authority.
CacheKey BuildFrontdoorLoweringCacheKey(const ParserConfig& config,
                                        const SessionContext& session,
                                        std::string_view sql) {
  const std::string normalized = NormalizeFrontdoorSql(sql);
  CacheKey key;
  key.shape_hash = Fnv1a64(normalized);
  key.normalized_statement_hash = Fnv1a64(normalized);
  key.registry_version = config.registry_version;
  key.catalog_epoch = session.catalog_epoch;
  key.security_policy_epoch = session.security_policy_epoch;
  key.grant_epoch = session.grant_epoch;
  key.descriptor_epoch = session.descriptor_epoch;
  key.udr_epoch = session.udr_epoch;
  key.name_resolution_epoch = session.localized_name_epoch != 0
                                  ? session.localized_name_epoch
                                  : session.catalog_epoch;
  key.resource_epoch = session.language_resource_epoch != 0
                           ? session.language_resource_epoch
                           : (config.resource_budget.max_statement_bytes ^
                              config.resource_budget.max_sblr_envelope_bytes);
  key.localized_name_epoch = key.name_resolution_epoch;
  key.language_resource_epoch = key.resource_epoch;
  key.parser_package_generation = Fnv1a64(config.bundle_contract_id);
  key.protocol_version = config.protocol_version;
  key.parser_package_version_hash = Fnv1a64(config.build_id);
  key.disclosure_policy_generation = Fnv1a64(session.result_rendering_policy);
  key.redaction_policy_generation = Fnv1a64(session.metric_redaction_policy);
  key.security_authority_epoch = session.security_policy_epoch ^ session.grant_epoch;
  key.cluster_policy_generation = 0;
  key.ttl_generation = 0;
  key.memory_pressure_generation = 0;
  key.parameter_type_shape_hash = Fnv1a64(ParameterTypeShape(sql));
  key.connection_uuid = session.connection_uuid;
  key.transaction_context_hash = std::to_string(Fnv1a64(session.transaction_context));
  key.dialect = config.dialect;
  key.role_set_hash = std::to_string(Fnv1a64(JoinStable(session.effective_role_uuids)));
  key.group_set_hash = std::to_string(Fnv1a64(JoinStable(session.effective_group_uuids)));
  key.search_path_hash = std::to_string(Fnv1a64(JoinStable(session.search_path)));
  key.language_profile = session.language_profile.empty()
                             ? session.default_language
                             : session.language_profile;
  key.language_tag = session.language_tag.empty()
                         ? session.default_language
                         : session.language_tag;
  key.input_syntax_profile = session.input_syntax_profile;
  key.input_language_fallback_tag = session.input_language_fallback_tag;
  key.common_resource_hash = session.common_resource_hash;
  key.policy_profile = session.policy_profile_uuid;
  key.parser_profile = config.profile_id;
  key.message_resource_epoch = session.message_resource_epoch;
  key.resource_compatibility_identity = session.resource_compatibility_identity;
  key.resource_version_identity = session.resource_version_identity;
  key.result_contract_hash =
      std::to_string(Fnv1a64(session.result_rendering_policy + "|" +
                             config.dialect + "|" + session.common_resource_hash +
                             "|" + session.resource_version_identity));
  return key;
}

std::string BuildNameResolutionCacheKey(const SessionContext& session,
                                        std::string_view presented_name,
                                        bool quoted,
                                        std::string_view object_class) {
  std::ostringstream key;
  key << "db=" << session.database_uuid
      << "|user=" << session.authenticated_user_uuid
      << "|session=" << session.session_uuid
      << "|connection=" << session.connection_uuid
      << "|presented=" << presented_name
      << "|quoted=" << (quoted ? "1" : "0")
      << "|class=" << object_class
      << "|catalog=" << session.catalog_epoch
      << "|security=" << session.security_policy_epoch
      << "|grant=" << session.grant_epoch
      << "|descriptor=" << session.descriptor_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|language_resource=" << session.language_resource_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|roles=" << JoinStable(session.effective_role_uuids)
      << "|groups=" << JoinStable(session.effective_group_uuids)
      << "|search_path=" << JoinStable(session.search_path)
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|policy_profile=" << session.policy_profile_uuid
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

std::string BuildStableRelationNameResolutionCacheKey(
    const SessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  const bool qualified = presented_name.find('.') != std::string_view::npos;
  std::ostringstream key;
  key << "db=" << session.database_uuid
      << "|user=" << session.authenticated_user_uuid
      << "|session=" << session.session_uuid
      << "|connection=" << session.connection_uuid
      << "|presented=" << presented_name
      << "|quoted=" << (quoted ? "1" : "0")
      << "|class=" << object_class
      << "|security=" << session.security_policy_epoch
      << "|grant=" << session.grant_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|language_resource=" << session.language_resource_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|roles=" << JoinStable(session.effective_role_uuids)
      << "|groups=" << JoinStable(session.effective_group_uuids)
      << "|search_path=" << (qualified ? std::string("<qualified>")
                                       : JoinStable(session.search_path))
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|policy_profile=" << session.policy_profile_uuid
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

std::mutex& SharedNameResolutionCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, CachedPublicNameResolution>& SharedNameResolutionCache() {
  static std::map<std::string, CachedPublicNameResolution> cache;
  return cache;
}

std::deque<std::string>& SharedNameResolutionLru() {
  static std::deque<std::string> lru;
  return lru;
}

std::optional<CachedPublicNameResolution> LookupSharedNameResolutionCache(
    const std::string& cache_key) {
  std::lock_guard<std::mutex> guard(SharedNameResolutionCacheMutex());
  const auto found = SharedNameResolutionCache().find(cache_key);
  if (found == SharedNameResolutionCache().end()) return std::nullopt;
  return found->second;
}

void StoreSharedNameResolutionCacheEntry(
    const std::string& cache_key,
    const CachedPublicNameResolution& cached) {
  if (cache_key.empty() || cached.object_uuid.empty()) return;
  std::lock_guard<std::mutex> guard(SharedNameResolutionCacheMutex());
  auto& cache = SharedNameResolutionCache();
  auto& lru = SharedNameResolutionLru();
  cache[cache_key] = cached;
  lru.erase(std::remove(lru.begin(), lru.end(), cache_key), lru.end());
  lru.push_back(cache_key);
  while (cache.size() > kMaxSharedNameResolutionCacheEntries && !lru.empty()) {
    cache.erase(lru.front());
    lru.pop_front();
  }
}

void ClearSharedNameResolutionCache() {
  std::lock_guard<std::mutex> guard(SharedNameResolutionCacheMutex());
  SharedNameResolutionCache().clear();
  SharedNameResolutionLru().clear();
}

std::optional<std::string> DdlResultRowField(std::string_view payload,
                                             std::string_view field_name) {
  std::istringstream in{std::string(payload)};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("row[")) continue;
    const auto eq = line.find("]=");
    if (eq == std::string::npos) continue;
    std::string_view body(line);
    body.remove_prefix(eq + 2);
    std::size_t start = 0;
    while (start <= body.size()) {
      const std::size_t end = body.find(';', start);
      const std::string_view item =
          body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);
      const std::size_t item_eq = item.find('=');
      if (item_eq != std::string_view::npos && item.substr(0, item_eq) == field_name) {
        return std::string(item.substr(item_eq + 1));
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  }
  return std::nullopt;
}

bool IsNameNotFoundDiagnostic(const MessageVectorSet& messages) {
  if (messages.diagnostics.empty()) return false;
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE") {
      return true;
    }
  }
  return false;
}

PipelineResult PipelineResultFromCacheEntry(const CacheEntry& entry) {
  PipelineResult result;
  result.accepted = !entry.sblr_payload.empty();
  result.frontdoor_cache_hit = true;
  result.parser_executes_sql = entry.parser_executes_sql;
  result.cached_storage_authority = entry.storage_authority_cached;
  result.cached_authorization_authority = entry.authorization_authority_cached;
  result.cached_finality_authority = entry.finality_authority_cached;
  result.statement_family = entry.statement_family;
  result.operation_family = entry.operation_family;
  result.statement_hash = entry.statement_hash;
  result.sblr_payload = entry.sblr_payload;
  return result;
}

bool CanReuseFrontdoorCacheForSubmit(const PipelineResult& result) {
  return result.operation_family == "sblr.dml.operation.v3" ||
         result.operation_family == "sblr.transaction.control.v3";
}

void AddResourceDiagnostic(MessageVectorSet* messages,
                           std::string code,
                           std::string message,
                           std::vector<Field> fields) {
  messages->diagnostics.push_back(MakeDiagnostic(
      std::move(code), "ERROR", std::move(message), "sbp_sbsql.wire",
      std::move(fields)));
}

std::optional<ObjectReference> ExtractFirstObjectReference(const CstDocument& cst) {
  std::size_t marker = cst.tokens.size();
  for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (IsWord(token, "COPY")) {
      marker = i + 1;
    }
    break;
  }
  if (marker == cst.tokens.size()) {
    for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
      const auto& token = cst.tokens[i];
      if (IsTriviaToken(token)) continue;
      if (token.kind != TokenKind::kKeyword && token.kind != TokenKind::kIdentifier) continue;
      if (IsWord(token, "FROM") || IsWord(token, "INTO") || IsWord(token, "UPDATE") ||
          IsWord(token, "TABLE") || IsWord(token, "CALL")) {
        marker = i + 1;
        break;
      }
    }
  }
  if (marker >= cst.tokens.size()) return std::nullopt;
  ObjectReference ref;
  for (std::size_t i = marker; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (token.kind == TokenKind::kSymbol && token.text == ".") {
      if (!ref.presented_name.empty() && ref.presented_name.back() != '.') ref.presented_name.push_back('.');
      continue;
    }
    if (token.kind != TokenKind::kIdentifier && token.kind != TokenKind::kKeyword) break;
    if (!ref.presented_name.empty() && ref.presented_name.back() != '.') break;
    ref.presented_name += token.text;
    ref.quoted = ref.quoted || token.quoted;
  }
  if (ref.presented_name.empty()) return std::nullopt;
  return ref;
}

std::optional<ObjectReference> ExtractObjectReferenceAt(const CstDocument& cst,
                                                        std::size_t marker) {
  if (marker >= cst.tokens.size()) return std::nullopt;
  ObjectReference ref;
  for (std::size_t i = marker; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (token.kind == TokenKind::kSymbol && token.text == ".") {
      if (!ref.presented_name.empty() && ref.presented_name.back() != '.') ref.presented_name.push_back('.');
      continue;
    }
    if (token.kind != TokenKind::kIdentifier && token.kind != TokenKind::kKeyword) break;
    if (!ref.presented_name.empty() && ref.presented_name.back() != '.') break;
    ref.presented_name += token.text;
    ref.quoted = ref.quoted || token.quoted;
  }
  if (ref.presented_name.empty()) return std::nullopt;
  return ref;
}

std::size_t IndexAfterObjectReferenceAt(const CstDocument& cst, std::size_t marker) {
  std::size_t index = marker;
  bool consumed_any = false;
  bool expect_name_part = true;
  while (index < cst.tokens.size()) {
    const auto& token = cst.tokens[index];
    if (IsTriviaToken(token)) {
      ++index;
      continue;
    }
    if (token.kind == TokenKind::kSymbol && token.text == ".") {
      if (!consumed_any || expect_name_part) return marker;
      ++index;
      expect_name_part = true;
      continue;
    }
    if (token.kind != TokenKind::kIdentifier && token.kind != TokenKind::kKeyword) break;
    if (!expect_name_part) break;
    consumed_any = true;
    expect_name_part = false;
    ++index;
  }
  while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
  return consumed_any && !expect_name_part ? index : marker;
}

bool ObjectReferenceHasSchemaQualifier(std::string_view presented_name) {
  return presented_name.find('.') != std::string_view::npos;
}

void DropObjectReferenceLeaf(ObjectReference* ref) {
  if (ref == nullptr) return;
  const auto dot = ref->presented_name.rfind('.');
  if (dot == std::string::npos) return;
  ref->presented_name.erase(dot);
}

std::string LowerObjectReferenceName(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char ch : text) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

bool IsEngineOwnedProjectionReference(const ObjectReference& ref) {
  if (ref.object_class != "relation" &&
      ref.object_class != "table" &&
      ref.object_class != "view") {
    return false;
  }
  const std::string lowered = LowerObjectReferenceName(ref.presented_name);
  return lowered.rfind("sys.", 0) == 0 ||
         lowered.find(".sys.") != std::string::npos ||
         lowered.rfind("information.", 0) == 0 ||
         lowered.find(".information.") != std::string::npos ||
         lowered.rfind("emulated.", 0) == 0 ||
         lowered.find(".emulated.") != std::string::npos;
}

bool IsCanonicalBuiltInFunctionReference(std::string_view presented_name) {
  const std::string lowered = LowerObjectReferenceName(presented_name);
  static constexpr std::string_view kPrefixes[] = {
      "sb.crypto.",
      "sb.cursor.",
      "sb.handle.",
      "sb.json.",
      "sb.lob.",
      "sb.locator.",
      "sb.multiset.",
      "sb.operator.",
      "sb.rowset.",
      "sb.scalar.",
      "sb.session.",
      "sb.setof.",
      "sb.stream.",
      "sb.table_value.",
      "sb.temporal.",
      "sb.type.",
      "sb.uuid.",
      "sb.vector.",
      "sb.xml.",
  };
  for (const auto prefix : kPrefixes) {
    if (lowered.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

std::vector<std::string> ExtractLeadingCteNames(const CstDocument& cst) {
  std::vector<std::string> names;
  std::size_t index = cst.tokens.size();
  for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
    if (!IsTriviaToken(cst.tokens[i])) {
      index = i;
      break;
    }
  }
  if (index < cst.tokens.size() && IsWord(cst.tokens[index], "EXPLAIN")) {
    ++index;
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
  }
  if (index >= cst.tokens.size() || !IsWord(cst.tokens[index], "WITH")) return names;
  ++index;
  while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
  if (index < cst.tokens.size() && IsWord(cst.tokens[index], "RECURSIVE")) ++index;

  while (index < cst.tokens.size()) {
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() ||
        (cst.tokens[index].kind != TokenKind::kIdentifier &&
         cst.tokens[index].kind != TokenKind::kKeyword)) {
      break;
    }
    names.push_back(LowerObjectReferenceName(cst.tokens[index].text));
    ++index;
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index < cst.tokens.size() && cst.tokens[index].kind == TokenKind::kSymbol &&
        cst.tokens[index].text == "(") {
      std::size_t depth = 1;
      ++index;
      while (index < cst.tokens.size() && depth != 0) {
        if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == "(") {
          ++depth;
        } else if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == ")") {
          --depth;
        }
        ++index;
      }
    }
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() || !IsWord(cst.tokens[index], "AS")) break;
    ++index;
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() || cst.tokens[index].kind != TokenKind::kSymbol ||
        cst.tokens[index].text != "(") {
      break;
    }
    std::size_t depth = 1;
    ++index;
    while (index < cst.tokens.size() && depth != 0) {
      if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == "(") {
        ++depth;
      } else if (cst.tokens[index].kind == TokenKind::kSymbol && cst.tokens[index].text == ")") {
        --depth;
      }
      ++index;
    }
    while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
    if (index >= cst.tokens.size() || cst.tokens[index].kind != TokenKind::kSymbol ||
        cst.tokens[index].text != ",") {
      break;
    }
    ++index;
  }
  return names;
}

std::vector<std::string> ExtractDerivedCteNames(const CstDocument& cst) {
  std::vector<std::string> names;
  for (std::size_t index = 0; index < cst.tokens.size(); ++index) {
    if (!IsWord(cst.tokens[index], "FROM")) continue;
    std::size_t cursor = index + 1;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor >= cst.tokens.size() ||
        cst.tokens[cursor].kind != TokenKind::kSymbol ||
        cst.tokens[cursor].text != "(") {
      continue;
    }
    ++cursor;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor >= cst.tokens.size() || !IsWord(cst.tokens[cursor], "WITH")) continue;
    ++cursor;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor < cst.tokens.size() && IsWord(cst.tokens[cursor], "RECURSIVE")) ++cursor;
    while (cursor < cst.tokens.size() && IsTriviaToken(cst.tokens[cursor])) ++cursor;
    if (cursor >= cst.tokens.size() ||
        (cst.tokens[cursor].kind != TokenKind::kIdentifier &&
         cst.tokens[cursor].kind != TokenKind::kKeyword)) {
      continue;
    }
    const std::string name = LowerObjectReferenceName(cst.tokens[cursor].text);
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(name);
    }
  }
  return names;
}

bool IsLocalCteReference(const ObjectReference& ref,
                         const std::vector<std::string>& local_cte_names) {
  if (ref.presented_name.empty() ||
      ref.presented_name.find('.') != std::string::npos) {
    return false;
  }
  const std::string lowered = LowerObjectReferenceName(ref.presented_name);
  return std::find(local_cte_names.begin(), local_cte_names.end(), lowered) !=
         local_cte_names.end();
}

std::vector<ObjectReference> ExtractMergeObjectReferences(const CstDocument& cst,
                                                          std::size_t first_token) {
  std::vector<ObjectReference> refs;
  bool target_seen = false;
  bool source_seen = false;
  for (std::size_t i = first_token + 1; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (!target_seen && IsWord(token, "INTO")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        target_seen = true;
      }
      continue;
    }
    if (!source_seen && IsWord(token, "USING")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        source_seen = true;
      }
      continue;
    }
    if (target_seen && source_seen) break;
  }
  return refs;
}

std::vector<ObjectReference> ExtractInsertObjectReferences(const CstDocument& cst,
                                                           std::size_t first_token) {
  std::vector<ObjectReference> refs;
  bool target_seen = false;
  std::size_t after_target = first_token + 1;
  for (std::size_t i = first_token + 1; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (IsWord(token, "INTO")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        target_seen = true;
      }
      after_target = i + 1;
      break;
    }
    if (token.kind != TokenKind::kKeyword && token.kind != TokenKind::kIdentifier) {
      break;
    }
  }
  if (!target_seen) return refs;

  bool saw_row_number = false;
  bool left_source_seen = false;
  for (std::size_t i = after_target; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (!saw_row_number) {
      if (IsWord(token, "ROW_NUMBER")) saw_row_number = true;
      continue;
    }
    if (!left_source_seen && IsWord(token, "FROM")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
        left_source_seen = true;
      }
      continue;
    }
    if (left_source_seen && IsWord(token, "JOIN")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
      }
      break;
    }
  }
  return refs;
}

std::vector<ObjectReference> ExtractCreateIndexObjectReferences(const CstDocument& cst,
                                                                std::size_t first_token) {
  std::vector<ObjectReference> refs;
  bool saw_index = false;
  for (std::size_t i = first_token + 1; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (!saw_index && IsWord(token, "INDEX")) {
      saw_index = true;
      continue;
    }
    if (saw_index && IsWord(token, "ON")) {
      if (auto ref = ExtractObjectReferenceAt(cst, i + 1)) {
        refs.push_back(*ref);
      }
      return refs;
    }
  }
  return refs;
}

std::size_t NextNonTriviaIndex(const CstDocument& cst, std::size_t index) {
  while (index < cst.tokens.size() && IsTriviaToken(cst.tokens[index])) ++index;
  return index;
}

std::vector<ObjectReference> ExtractMultimodelObjectReferences(const CstDocument& cst,
                                                               std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  const auto push_ref_at = [&](std::size_t marker) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      refs.push_back(*ref);
    }
  };

  if (IsWord(cst.tokens[first_token], "DOCUMENT") ||
      IsWord(cst.tokens[first_token], "FULLTEXT") ||
      IsWord(cst.tokens[first_token], "OPENSEARCH") ||
      IsWord(cst.tokens[first_token], "TIMESERIES") ||
      IsWord(cst.tokens[first_token], "GRAPH") ||
      IsWord(cst.tokens[first_token], "SEARCH")) {
    push_ref_at(first_token + 1);
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "TIME")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    if (second < cst.tokens.size() && IsWord(cst.tokens[second], "SERIES")) {
      push_ref_at(second + 1);
    }
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "CHANGE")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    if (second < cst.tokens.size() && IsWord(cst.tokens[second], "STREAM")) {
      push_ref_at(second + 1);
    }
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "REINDEX")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    const std::size_t third = second < cst.tokens.size() ? NextNonTriviaIndex(cst, second + 1)
                                                        : cst.tokens.size();
    if (second < cst.tokens.size() && third < cst.tokens.size() &&
        IsWord(cst.tokens[second], "VECTOR") &&
        IsWord(cst.tokens[third], "COLLECTION")) {
      push_ref_at(third + 1);
    }
    return refs;
  }

  return refs;
}

std::vector<ObjectReference> ExtractFilespaceObjectReferences(const CstDocument& cst,
                                                              std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  const auto push_ref_at = [&](std::size_t marker) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      ref->object_class = "filespace";
      refs.push_back(*ref);
    }
  };
  const auto second = NextNonTriviaIndex(cst, first_token + 1);
  if (second >= cst.tokens.size()) return refs;

  if (IsWord(cst.tokens[first_token], "ALTER") &&
      IsWord(cst.tokens[second], "FILESPACE")) {
    push_ref_at(second + 1);
    return refs;
  }

  if ((IsWord(cst.tokens[first_token], "ATTACH") ||
       IsWord(cst.tokens[first_token], "DETACH") ||
       IsWord(cst.tokens[first_token], "DISCONNECT") ||
       IsWord(cst.tokens[first_token], "MOVE") ||
       IsWord(cst.tokens[first_token], "MERGE") ||
       IsWord(cst.tokens[first_token], "PROMOTE") ||
       IsWord(cst.tokens[first_token], "GROW") ||
       IsWord(cst.tokens[first_token], "RESIZE") ||
       IsWord(cst.tokens[first_token], "SHRINK") ||
       IsWord(cst.tokens[first_token], "VERIFY") ||
       IsWord(cst.tokens[first_token], "COMPACT") ||
       IsWord(cst.tokens[first_token], "FENCE") ||
       IsWord(cst.tokens[first_token], "RELEASE") ||
       IsWord(cst.tokens[first_token], "ARCHIVE") ||
       IsWord(cst.tokens[first_token], "QUARANTINE") ||
       IsWord(cst.tokens[first_token], "REPAIR") ||
       IsWord(cst.tokens[first_token], "REBUILD") ||
       IsWord(cst.tokens[first_token], "SALVAGE")) &&
      IsWord(cst.tokens[second], "FILESPACE")) {
    push_ref_at(second + 1);
    return refs;
  }

  if ((IsWord(cst.tokens[first_token], "DROP") ||
       IsWord(cst.tokens[first_token], "DELETE")) &&
      IsWord(cst.tokens[second], "STORAGE")) {
    const auto third = NextNonTriviaIndex(cst, second + 1);
    if (third < cst.tokens.size() && IsWord(cst.tokens[third], "FILESPACE")) {
      push_ref_at(third + 1);
    }
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "STORAGE") &&
      IsWord(cst.tokens[second], "FILESPACE")) {
    push_ref_at(second + 1);
    return refs;
  }

  return refs;
}

std::vector<ObjectReference> ExtractDomainDdlObjectReferences(const CstDocument& cst,
                                                              std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  const auto push_domain_ref_at = [&](std::size_t marker) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      ref->object_class = "domain";
      refs.push_back(*ref);
    }
  };
  const auto second = NextNonTriviaIndex(cst, first_token + 1);
  if (second >= cst.tokens.size()) return refs;

  if ((IsWord(cst.tokens[first_token], "ALTER") ||
       IsWord(cst.tokens[first_token], "DROP")) &&
      IsWord(cst.tokens[second], "DOMAIN")) {
    push_domain_ref_at(second + 1);
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "COMMENT") &&
      IsWord(cst.tokens[second], "ON")) {
    const auto third = NextNonTriviaIndex(cst, second + 1);
    if (third < cst.tokens.size() && IsWord(cst.tokens[third], "DOMAIN")) {
      push_domain_ref_at(third + 1);
    }
    return refs;
  }

  return refs;
}

std::string ExtractDdlObjectClassAt(const CstDocument& cst, std::size_t* marker) {
  if (marker == nullptr) return {};
  *marker = NextNonTriviaIndex(cst, *marker);
  if (*marker >= cst.tokens.size()) return {};
  if (IsWord(cst.tokens[*marker], "MATERIALIZED")) {
    const std::size_t view_token = NextNonTriviaIndex(cst, *marker + 1);
    if (view_token < cst.tokens.size() && IsWord(cst.tokens[view_token], "VIEW")) {
      *marker = NextNonTriviaIndex(cst, view_token + 1);
      return "materialized_view";
    }
    return {};
  }
  static constexpr std::string_view kObjectClasses[] = {
      "SCHEMA", "TABLE", "INDEX", "VIEW", "SEQUENCE", "DOMAIN", "FUNCTION",
      "PROCEDURE", "TRIGGER", "PACKAGE", "UDR", "ROLE", "USER", "GROUP",
      "POLICY", "OPERATOR", "AGGREGATE", "CAST", "ROUTINE"};
  for (const auto object_class : kObjectClasses) {
    if (IsWord(cst.tokens[*marker], object_class)) {
      *marker = NextNonTriviaIndex(cst, *marker + 1);
      return LowerObjectReferenceName(object_class);
    }
  }
  return {};
}

std::vector<ObjectReference> ExtractCatalogDdlObjectReferences(const CstDocument& cst,
                                                               std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  auto push_ref = [&](std::size_t marker, std::string object_class) {
    if (auto ref = ExtractObjectReferenceAt(cst, marker)) {
      ref->object_class = object_class == "routine" ? "procedure" : std::move(object_class);
      refs.push_back(*ref);
    }
  };

  if (IsWord(cst.tokens[first_token], "COMMENT")) {
    const std::size_t on_token = NextNonTriviaIndex(cst, first_token + 1);
    if (on_token >= cst.tokens.size() || !IsWord(cst.tokens[on_token], "ON")) return refs;
    std::size_t marker = on_token + 1;
    marker = NextNonTriviaIndex(cst, marker);
    if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "COLUMN")) {
      marker = NextNonTriviaIndex(cst, marker + 1);
      if (auto ref = ExtractObjectReferenceAt(cst, marker)) {
        DropObjectReferenceLeaf(&*ref);
        ref->object_class = "relation";
        refs.push_back(*ref);
      }
      return refs;
    }
    std::string object_class = ExtractDdlObjectClassAt(cst, &marker);
    if (!object_class.empty()) push_ref(marker, std::move(object_class));
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "ALTER")) {
    std::size_t marker = first_token + 1;
    std::string object_class = ExtractDdlObjectClassAt(cst, &marker);
    if (!object_class.empty()) push_ref(marker, std::move(object_class));
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "REFRESH")) {
    std::size_t marker = first_token + 1;
    std::string object_class = ExtractDdlObjectClassAt(cst, &marker);
    if (!object_class.empty()) push_ref(marker, std::move(object_class));
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "DROP")) {
    std::size_t marker = first_token + 1;
    std::string object_class = ExtractDdlObjectClassAt(cst, &marker);
    if (object_class.empty()) return refs;
    if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "IF")) {
      const std::size_t exists_token = NextNonTriviaIndex(cst, marker + 1);
      if (exists_token < cst.tokens.size() && IsWord(cst.tokens[exists_token], "EXISTS")) {
        marker = NextNonTriviaIndex(cst, exists_token + 1);
      }
    }
    push_ref(marker, std::move(object_class));
    return refs;
  }

  if (IsWord(cst.tokens[first_token], "CREATE")) {
    std::size_t marker = first_token + 1;
    const std::string object_class = ExtractDdlObjectClassAt(cst, &marker);
    if (!object_class.empty()) {
      if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "IF")) {
        const std::size_t not_token = NextNonTriviaIndex(cst, marker + 1);
        const std::size_t exists_token = NextNonTriviaIndex(cst, not_token + 1);
        if (not_token < cst.tokens.size() && exists_token < cst.tokens.size() &&
            IsWord(cst.tokens[not_token], "NOT") &&
            IsWord(cst.tokens[exists_token], "EXISTS")) {
          marker = NextNonTriviaIndex(cst, exists_token + 1);
        }
      }
      if (auto ref = ExtractObjectReferenceAt(cst, marker)) {
        ref->object_class = object_class == "routine" ? "procedure" : object_class;
        ref->create_reservation = true;
        refs.push_back(*ref);
      }
    }
    return refs;
  }

  return refs;
}

std::vector<ObjectReference> ExtractCreateExecutableObjectReferences(
    const CstDocument& cst,
    std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size() || !IsWord(cst.tokens[first_token], "CREATE")) {
    return refs;
  }
  std::size_t marker = NextNonTriviaIndex(cst, first_token + 1);
  if (marker >= cst.tokens.size()) return refs;
  std::string object_class;
  if (IsWord(cst.tokens[marker], "FUNCTION")) {
    object_class = "function";
  } else if (IsWord(cst.tokens[marker], "PROCEDURE")) {
    object_class = "procedure";
  } else if (IsWord(cst.tokens[marker], "TRIGGER")) {
    object_class = "trigger";
  } else {
    return refs;
  }

  auto push_unique = [&](ObjectReference ref) {
    if (ref.presented_name.empty()) return;
    const auto duplicate =
        std::find_if(refs.begin(), refs.end(), [&](const ObjectReference& existing) {
          return existing.presented_name == ref.presented_name &&
                 existing.object_class == ref.object_class &&
                 existing.create_reservation == ref.create_reservation;
        });
    if (duplicate == refs.end()) refs.push_back(std::move(ref));
  };

  marker = NextNonTriviaIndex(cst, marker + 1);
  if (auto created = ExtractObjectReferenceAt(cst, marker)) {
    created->object_class = object_class;
    created->create_reservation = true;
    push_unique(*created);
  }

  for (std::size_t index = marker; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;

    if (IsWord(token, "ON")) {
      std::size_t target = NextNonTriviaIndex(cst, index + 1);
      if (target < cst.tokens.size() && IsWord(cst.tokens[target], "TABLE")) {
        target = NextNonTriviaIndex(cst, target + 1);
      }
      if (auto ref = ExtractObjectReferenceAt(cst, target);
          ref && ObjectReferenceHasSchemaQualifier(ref->presented_name)) {
        ref->object_class = "table";
        push_unique(*ref);
      }
      continue;
    }

    if (IsWord(token, "INTO") || IsWord(token, "FROM")) {
      if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, index + 1));
          ref && ObjectReferenceHasSchemaQualifier(ref->presented_name)) {
        ref->object_class = "table";
        push_unique(*ref);
      }
      continue;
    }

    if (IsWord(token, "FOR")) {
      std::size_t previous = index;
      while (previous > 0) {
        --previous;
        if (!IsTriviaToken(cst.tokens[previous])) break;
      }
      if (previous < cst.tokens.size() && IsWord(cst.tokens[previous], "VALUE")) {
        std::size_t before_value = previous;
        while (before_value > 0) {
          --before_value;
          if (!IsTriviaToken(cst.tokens[before_value])) break;
        }
        if (before_value < cst.tokens.size() &&
            IsWord(cst.tokens[before_value], "NEXT")) {
          if (auto ref = ExtractObjectReferenceAt(
                  cst, NextNonTriviaIndex(cst, index + 1));
              ref && ObjectReferenceHasSchemaQualifier(ref->presented_name)) {
            ref->object_class = "sequence";
            push_unique(*ref);
          }
        }
      }
    }
  }

  return refs;
}

std::vector<ObjectReference> ExtractRoutineCallObjectReferences(
    const CstDocument& cst,
    std::size_t first_token) {
  std::vector<ObjectReference> refs;
  for (std::size_t index = first_token; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd ||
        token.kind == TokenKind::kStatementTerminator) {
      break;
    }
    if (token.kind != TokenKind::kIdentifier && token.kind != TokenKind::kKeyword) {
      continue;
    }
    auto ref = ExtractObjectReferenceAt(cst, index);
    if (!ref || !ObjectReferenceHasSchemaQualifier(ref->presented_name)) continue;
    const std::size_t after_ref = IndexAfterObjectReferenceAt(cst, index);
    if (after_ref >= cst.tokens.size() || cst.tokens[after_ref].text != "(") continue;
    if (IsCanonicalBuiltInFunctionReference(ref->presented_name)) {
      index = after_ref;
      continue;
    }

    std::size_t previous = index;
    while (previous > 0) {
      --previous;
      if (!IsTriviaToken(cst.tokens[previous])) break;
    }
    ref->object_class = (previous < index && IsWord(cst.tokens[previous], "FROM"))
                            ? "procedure"
                            : "function";
    refs.push_back(*ref);
    if (refs.size() >= 16) break;
    index = after_ref;
  }
  return refs;
}

std::vector<ObjectReference> ExtractSecurityDclObjectReferences(const CstDocument& cst,
                                                                std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size() ||
      (!IsWord(cst.tokens[first_token], "GRANT") && !IsWord(cst.tokens[first_token], "REVOKE"))) {
    return refs;
  }
  const bool grant = IsWord(cst.tokens[first_token], "GRANT");
  bool has_on = false;
  std::size_t on_index = cst.tokens.size();
  std::size_t to_from_index = cst.tokens.size();
  for (std::size_t index = first_token + 1; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kStatementTerminator || token.kind == TokenKind::kEnd) break;
    if (IsWord(token, "ON")) {
      has_on = true;
      on_index = index;
      continue;
    }
    if (IsWord(token, grant ? "TO" : "FROM")) {
      to_from_index = index;
      break;
    }
  }

  if (!has_on) {
    if (auto member = ExtractObjectReferenceAt(cst, first_token + 1)) {
      member->object_class = "role";
      refs.push_back(*member);
    }
    if (to_from_index < cst.tokens.size()) {
      std::size_t marker = NextNonTriviaIndex(cst, to_from_index + 1);
      std::string container_class = "group";
      if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "ROLE")) {
        container_class = "role";
        marker = NextNonTriviaIndex(cst, marker + 1);
      } else if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "GROUP")) {
        container_class = "group";
        marker = NextNonTriviaIndex(cst, marker + 1);
      }
      if (auto container = ExtractObjectReferenceAt(cst, marker)) {
        container->object_class = container_class;
        refs.push_back(*container);
      }
    }
    return refs;
  }

  if (on_index < cst.tokens.size()) {
    std::size_t marker = NextNonTriviaIndex(cst, on_index + 1);
    std::string target_class = "object";
    if (marker < cst.tokens.size()) {
      const std::string word = ToUpperAscii(cst.tokens[marker].text);
      if (word == "TABLE" || word == "VIEW" || word == "DOMAIN" ||
          word == "SEQUENCE" || word == "PROCEDURE" || word == "FUNCTION" ||
          word == "PACKAGE" || word == "INDEX") {
        target_class = LowerObjectReferenceName(cst.tokens[marker].text);
        marker = NextNonTriviaIndex(cst, marker + 1);
      }
    }
    if (auto target = ExtractObjectReferenceAt(cst, marker)) {
      target->object_class = target_class == "object" ? "relation" : target_class;
      refs.push_back(*target);
    }
  }
  if (to_from_index < cst.tokens.size()) {
    std::size_t marker = NextNonTriviaIndex(cst, to_from_index + 1);
    std::string grantee_class = "role";
    if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "ROLE")) {
      grantee_class = "role";
      marker = NextNonTriviaIndex(cst, marker + 1);
    } else if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "GROUP")) {
      grantee_class = "group";
      marker = NextNonTriviaIndex(cst, marker + 1);
    } else if (marker < cst.tokens.size() &&
               (IsWord(cst.tokens[marker], "USER") || IsWord(cst.tokens[marker], "PRINCIPAL"))) {
      grantee_class = "principal";
      marker = NextNonTriviaIndex(cst, marker + 1);
    }
    if (auto grantee = ExtractObjectReferenceAt(cst, marker)) {
      grantee->object_class = grantee_class;
      refs.push_back(*grantee);
    }
  }
  return refs;
}

std::vector<ObjectReference> ExtractSecurityPolicyObjectReferences(const CstDocument& cst,
                                                                   std::size_t first_token) {
  std::vector<ObjectReference> refs;
  if (first_token >= cst.tokens.size()) return refs;
  std::optional<ObjectReference> create_policy_schema_ref;
  const auto push_ref_at = [&](std::size_t marker,
                               std::string object_class,
                               bool create_reservation = false) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      ref->object_class = std::move(object_class);
      ref->create_reservation = create_reservation;
      refs.push_back(*ref);
    }
  };
  const auto push_create_policy_ref_at = [&](std::size_t marker,
                                             std::string object_class) {
    if (auto ref = ExtractObjectReferenceAt(cst, NextNonTriviaIndex(cst, marker))) {
      ObjectReference schema_ref = *ref;
      DropObjectReferenceLeaf(&schema_ref);
      if (!schema_ref.presented_name.empty() &&
          schema_ref.presented_name != ref->presented_name) {
        schema_ref.object_class = "schema";
        schema_ref.create_reservation = false;
        create_policy_schema_ref = schema_ref;
      }
      ref->object_class = std::move(object_class);
      ref->create_reservation = true;
      refs.push_back(*ref);
    }
  };

  if (IsWord(cst.tokens[first_token], "DROP")) {
    const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
    if (second >= cst.tokens.size()) return refs;
    if (IsWord(cst.tokens[second], "ROLE")) {
      push_ref_at(second + 1, "role");
    } else if (IsWord(cst.tokens[second], "GROUP")) {
      push_ref_at(second + 1, "group");
    } else if (IsWord(cst.tokens[second], "POLICY")) {
      push_ref_at(second + 1, "policy");
    } else if (IsWord(cst.tokens[second], "MASK")) {
      push_ref_at(second + 1, "mask");
    } else if (IsWord(cst.tokens[second], "RLS")) {
      push_ref_at(second + 1, "rls");
    }
    return refs;
  }

  if (!IsWord(cst.tokens[first_token], "CREATE")) return refs;
  const std::size_t second = NextNonTriviaIndex(cst, first_token + 1);
  if (second >= cst.tokens.size()) return refs;
  if (IsWord(cst.tokens[second], "ROLE")) {
    push_ref_at(second + 1, "role", true);
    return refs;
  }
  if (IsWord(cst.tokens[second], "GROUP")) {
    push_ref_at(second + 1, "group", true);
    return refs;
  }
  if (IsWord(cst.tokens[second], "PRINCIPAL") || IsWord(cst.tokens[second], "USER")) {
    push_ref_at(second + 1, "principal", true);
    return refs;
  }
  if (!IsWord(cst.tokens[second], "POLICY") &&
      !IsWord(cst.tokens[second], "MASK") &&
      !IsWord(cst.tokens[second], "RLS")) {
    return refs;
  }
  if (IsWord(cst.tokens[second], "POLICY")) {
    push_create_policy_ref_at(second + 1, "policy");
  } else if (IsWord(cst.tokens[second], "MASK")) {
    push_create_policy_ref_at(second + 1, "mask");
  } else {
    push_create_policy_ref_at(second + 1, "rls");
  }
  for (std::size_t index = second + 1; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kStatementTerminator || token.kind == TokenKind::kEnd) break;
    if (IsWord(token, "ON")) {
      std::size_t marker = NextNonTriviaIndex(cst, index + 1);
      std::string target_class = "relation";
      bool drop_leaf = false;
      if (marker < cst.tokens.size()) {
        if (IsWord(cst.tokens[marker], "COLUMN")) {
          target_class = "relation";
          drop_leaf = true;
          marker = NextNonTriviaIndex(cst, marker + 1);
        } else if (IsWord(cst.tokens[marker], "TABLE")) {
          target_class = "table";
          marker = NextNonTriviaIndex(cst, marker + 1);
        } else if (IsWord(cst.tokens[marker], "VIEW")) {
          target_class = "view";
          marker = NextNonTriviaIndex(cst, marker + 1);
        }
      }
      if (auto ref = ExtractObjectReferenceAt(cst, marker)) {
        if (drop_leaf) DropObjectReferenceLeaf(&*ref);
        ref->object_class = std::move(target_class);
        refs.push_back(*ref);
      }
      continue;
    }
    if (IsWord(token, "TO")) {
      std::size_t marker = NextNonTriviaIndex(cst, index + 1);
      std::string subject_class = "role";
      if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "ROLE")) {
        subject_class = "role";
        marker = NextNonTriviaIndex(cst, marker + 1);
      } else if (marker < cst.tokens.size() && IsWord(cst.tokens[marker], "GROUP")) {
        subject_class = "group";
        marker = NextNonTriviaIndex(cst, marker + 1);
      } else if (marker < cst.tokens.size() &&
                 (IsWord(cst.tokens[marker], "USER") || IsWord(cst.tokens[marker], "PRINCIPAL"))) {
        subject_class = "principal";
        marker = NextNonTriviaIndex(cst, marker + 1);
      }
      push_ref_at(marker, subject_class);
    }
  }
  if (create_policy_schema_ref) refs.push_back(*create_policy_schema_ref);
  return refs;
}

std::vector<ObjectReference> ExtractObjectReferences(const CstDocument& cst,
                                                     const AstDocument& ast) {
  std::vector<ObjectReference> refs;
  const bool time_series_model_ast = std::ranges::any_of(
      ast.native_relational.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kTimeSeries;
      });
  if (time_series_model_ast) {
    // QOW-SOURCE-RCP-076-TIME-SERIES-OBJECT-REFERENCE-V1
    if (!ast.native_relational.accepted() ||
        ast.native_relational.model_object_resolution_requests.size() != 1) {
      return refs;
    }
    const auto& request =
        ast.native_relational.model_object_resolution_requests.front();
    if (request.source_id == 0 || request.model_family_id != "time_series" ||
        request.object_class != "time_series" ||
        request.qualified_name.empty()) {
      return refs;
    }
    const auto presented_name =
        EncodeQualifiedPresentedName(request.qualified_name);
    if (!presented_name.has_value()) return refs;
    ObjectReference ref;
    ref.object_class = "time_series";
    ref.presented_name = *presented_name;
    ref.quoted = false;
    refs.push_back(std::move(ref));
    return refs;
  }
  const bool key_value_model_ast = std::ranges::any_of(
      ast.native_relational.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kKeyValue;
      });
  if (key_value_model_ast) {
    // QOW-SOURCE-RCP-075-KEY-VALUE-OBJECT-REFERENCE-V1
    if (!ast.native_relational.accepted() ||
        ast.native_relational.model_object_resolution_requests.size() != 1) {
      return refs;
    }
    const auto& request =
        ast.native_relational.model_object_resolution_requests.front();
    if (request.source_id == 0 || request.model_family_id != "key_value" ||
        request.object_class != "key_value" || request.qualified_name.empty()) {
      return refs;
    }
    const auto presented_name =
        EncodeQualifiedPresentedName(request.qualified_name);
    if (!presented_name.has_value()) return refs;
    ObjectReference ref;
    ref.object_class = "key_value";
    ref.presented_name = *presented_name;
    ref.quoted = false;
    refs.push_back(std::move(ref));
    return refs;
  }
  const bool graph_model_ast = std::ranges::any_of(
      ast.native_relational.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kGraph;
      });
  if (graph_model_ast) {
    // QOW-SOURCE-RCP-074-GRAPH-OBJECT-REFERENCE-V1
    if (!ast.native_relational.accepted() ||
        ast.native_relational.model_object_resolution_requests.size() != 1) {
      return refs;
    }
    const auto& request =
        ast.native_relational.model_object_resolution_requests.front();
    if (request.source_id == 0 || request.model_family_id != "graph" ||
        request.object_class != "graph" || request.qualified_name.empty()) {
      return refs;
    }
    const auto presented_name =
        EncodeQualifiedPresentedName(request.qualified_name);
    if (!presented_name.has_value()) return refs;
    ObjectReference ref;
    ref.object_class = "graph";
    ref.presented_name = *presented_name;
    ref.quoted = false;
    refs.push_back(std::move(ref));
    return refs;
  }
  const bool document_model_ast = std::ranges::any_of(
      ast.native_relational.catalog_relation_sources, [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kDocument;
      });
  if (document_model_ast) {
    if (!ast.native_relational.accepted() ||
        ast.native_relational.model_object_resolution_requests.size() > 1) {
      return refs;
    }
    if (ast.native_relational.model_object_resolution_requests.empty()) {
      // Expression-backed DOCUMENT_UNNEST has no catalog object.  Returning
      // here also prevents the generic FROM-function scanner from treating
      // the wrapper as a procedure.
      return refs;
    }
    const auto& request =
        ast.native_relational.model_object_resolution_requests.front();
    if (request.source_id == 0 || request.model_family_id != "document" ||
        request.object_class != "document_collection" ||
        request.qualified_name.empty()) {
      return refs;
    }
    const auto presented_name =
        EncodeQualifiedPresentedName(request.qualified_name);
    if (!presented_name.has_value()) return refs;
    ObjectReference ref;
    ref.object_class = request.object_class;
    ref.presented_name = *presented_name;
    // Component quotes are retained in presented_name for the server-side
    // qualified-name splitter.  A request-level quote bit would incorrectly
    // quote every component of a mixed qualified name.
    ref.quoted = false;
    refs.push_back(std::move(ref));
    return refs;
  }
  auto local_cte_names = ExtractLeadingCteNames(cst);
  for (const auto& name : ExtractDerivedCteNames(cst)) {
    if (std::find(local_cte_names.begin(), local_cte_names.end(), name) ==
        local_cte_names.end()) {
      local_cte_names.push_back(name);
    }
  }
  std::size_t first_token = cst.tokens.size();
  for (std::size_t i = 0; i < cst.tokens.size(); ++i) {
    if (!IsTriviaToken(cst.tokens[i])) {
      first_token = i;
      break;
    }
  }
  if (first_token == cst.tokens.size()) return refs;

  if (IsWord(cst.tokens[first_token], "EXECUTE")) {
    std::size_t second = first_token + 1;
    while (second < cst.tokens.size() && IsTriviaToken(cst.tokens[second])) ++second;
    if (second < cst.tokens.size() && IsWord(cst.tokens[second], "PROCEDURE")) {
      if (auto ref = ExtractObjectReferenceAt(cst, second + 1)) {
        ref->object_class = "procedure";
        refs.push_back(*ref);
      }
      return refs;
    }
  }

  if (IsWord(cst.tokens[first_token], "COPY")) {
    if (auto ref = ExtractObjectReferenceAt(cst, first_token + 1)) refs.push_back(*ref);
    return refs;
  }
  if (IsWord(cst.tokens[first_token], "MERGE")) {
    return ExtractMergeObjectReferences(cst, first_token);
  }
  if (IsWord(cst.tokens[first_token], "INSERT") ||
      IsWord(cst.tokens[first_token], "UPSERT")) {
    return ExtractInsertObjectReferences(cst, first_token);
  }
  if (IsWord(cst.tokens[first_token], "CREATE")) {
    auto create_index_refs = ExtractCreateIndexObjectReferences(cst, first_token);
    if (!create_index_refs.empty()) {
      return create_index_refs;
    }
  }

  auto multimodel_refs = ExtractMultimodelObjectReferences(cst, first_token);
  if (!multimodel_refs.empty()) {
    return multimodel_refs;
  }

  auto filespace_refs = ExtractFilespaceObjectReferences(cst, first_token);
  if (!filespace_refs.empty()) {
    return filespace_refs;
  }

  auto domain_ddl_refs = ExtractDomainDdlObjectReferences(cst, first_token);
  if (!domain_ddl_refs.empty()) {
    return domain_ddl_refs;
  }

  auto create_executable_refs = ExtractCreateExecutableObjectReferences(cst, first_token);
  if (!create_executable_refs.empty()) {
    return create_executable_refs;
  }

  auto security_dcl_refs = ExtractSecurityDclObjectReferences(cst, first_token);
  if (!security_dcl_refs.empty()) {
    return security_dcl_refs;
  }

  auto security_policy_refs = ExtractSecurityPolicyObjectReferences(cst, first_token);
  if (!security_policy_refs.empty()) {
    return security_policy_refs;
  }

  auto catalog_ddl_refs = ExtractCatalogDdlObjectReferences(cst, first_token);
  if (!catalog_ddl_refs.empty()) {
    return catalog_ddl_refs;
  }

  auto routine_call_refs = ExtractRoutineCallObjectReferences(cst, first_token);
  if (!routine_call_refs.empty()) {
    refs.insert(refs.end(), routine_call_refs.begin(), routine_call_refs.end());
  }

  for (std::size_t i = first_token; i < cst.tokens.size(); ++i) {
    const auto& token = cst.tokens[i];
    if (IsTriviaToken(token)) continue;
    if (token.kind == TokenKind::kEnd) break;
    if (IsLiteralKind(token.kind)) continue;
    if (IsWord(token, "FROM")) {
      const auto previous_non_trivia = [&](std::size_t cursor)
          -> std::optional<std::size_t> {
        while (cursor > first_token) {
          --cursor;
          if (!IsTriviaToken(cst.tokens[cursor])) return cursor;
        }
        return std::nullopt;
      };
      const auto distinct = previous_non_trivia(i);
      const auto maybe_not =
          distinct.has_value() && IsWord(cst.tokens[*distinct], "DISTINCT")
              ? previous_non_trivia(*distinct)
              : std::nullopt;
      const auto maybe_is =
          maybe_not.has_value() && IsWord(cst.tokens[*maybe_not], "NOT")
              ? previous_non_trivia(*maybe_not)
              : maybe_not;
      if (distinct.has_value() && maybe_is.has_value() &&
          IsWord(cst.tokens[*distinct], "DISTINCT") &&
          IsWord(cst.tokens[*maybe_is], "IS")) {
        continue;
      }
    }
    if (!IsWord(token, "FROM") && !IsWord(token, "INTO") &&
        !IsWord(token, "UPDATE") && !IsWord(token, "TABLE") &&
        !IsWord(token, "CALL") && !IsWord(token, "JOIN")) {
      continue;
    }
    auto ref = ExtractObjectReferenceAt(cst, i + 1);
    if (!ref) continue;
    if (IsLocalCteReference(*ref, local_cte_names)) continue;
    const std::size_t after_ref = IndexAfterObjectReferenceAt(cst, i + 1);
    if ((IsWord(token, "FROM") || IsWord(token, "CALL")) &&
        after_ref < cst.tokens.size() && cst.tokens[after_ref].text == "(") {
      ref->object_class = "procedure";
    }
    refs.push_back(*ref);
    if (refs.size() >= 8) break;
  }

  if (refs.empty()) {
    if (auto ref = ExtractFirstObjectReference(cst)) {
      if (!IsLocalCteReference(*ref, local_cte_names)) refs.push_back(*ref);
    }
  }
  return refs;
}

bool IsIdentifierLikeForRouteExecution(const Token& token) {
  return token.kind == TokenKind::kIdentifier || token.kind == TokenKind::kKeyword;
}

void SkipTriviaTokens(const CstDocument& cst, std::size_t* index) {
  if (index == nullptr) return;
  while (*index < cst.tokens.size() && IsTriviaToken(cst.tokens[*index])) ++(*index);
}

bool ConsumeRouteKeyword(const CstDocument& cst, std::size_t* index, std::string_view keyword) {
  if (index == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || !IsWord(cst.tokens[*index], keyword)) return false;
  ++(*index);
  return true;
}

bool ConsumeRouteSymbol(const CstDocument& cst, std::size_t* index, std::string_view symbol) {
  if (index == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || cst.tokens[*index].text != symbol) return false;
  ++(*index);
  return true;
}

bool ConsumeRouteIdentifier(const CstDocument& cst, std::size_t* index, std::string* text) {
  if (index == nullptr || text == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || !IsIdentifierLikeForRouteExecution(cst.tokens[*index])) {
    return false;
  }
  *text = cst.tokens[*index].text;
  ++(*index);
  return true;
}

bool ConsumeRouteQualifiedNameLeaf(const CstDocument& cst, std::size_t* index, std::string* leaf) {
  if (index == nullptr || leaf == nullptr) return false;
  bool consumed = false;
  bool expect_part = true;
  std::string last;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (expect_part) {
      if (!IsIdentifierLikeForRouteExecution(token)) break;
      last = token.text;
      consumed = true;
      expect_part = false;
      ++(*index);
      continue;
    }
    if (token.text != ".") break;
    expect_part = true;
    ++(*index);
  }
  if (!consumed || expect_part) return false;
  *leaf = std::move(last);
  return true;
}

bool ConsumeRouteQualifiedNameParts(const CstDocument& cst,
                                    std::size_t* index,
                                    std::vector<std::string>* parts,
                                    bool* quoted = nullptr) {
  if (index == nullptr || parts == nullptr) return false;
  std::vector<std::string> parsed;
  bool saw_quoted = false;
  bool expect_part = true;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (expect_part) {
      if (!IsIdentifierLikeForRouteExecution(token)) break;
      parsed.push_back(token.text);
      saw_quoted = saw_quoted || token.quoted;
      expect_part = false;
      ++(*index);
      continue;
    }
    if (token.text != ".") break;
    expect_part = true;
    ++(*index);
  }
  if (parsed.empty() || expect_part) return false;
  *parts = std::move(parsed);
  if (quoted != nullptr) *quoted = saw_quoted;
  return true;
}

std::string JoinRouteNameParts(const std::vector<std::string>& parts,
                               std::size_t begin,
                               std::size_t end) {
  std::string out;
  for (std::size_t index = begin; index < end && index < parts.size(); ++index) {
    if (!out.empty()) out.push_back('.');
    out += parts[index];
  }
  return out;
}

bool ConsumeOptionalIfNotExists(const CstDocument& cst, std::size_t* index) {
  if (index == nullptr) return false;
  const std::size_t saved = *index;
  if (!ConsumeRouteKeyword(cst, index, "IF")) {
    *index = saved;
    return true;
  }
  if (!ConsumeRouteKeyword(cst, index, "NOT") ||
      !ConsumeRouteKeyword(cst, index, "EXISTS")) {
    *index = saved;
  }
  return true;
}

std::string RouteCanonicalTypeName(std::string_view type_text) {
  const std::string upper = ToUpperAscii(type_text);
  if (upper == "INT" || upper == "INTEGER") return "int";
  if (upper == "SMALLINT") return "smallint";
  if (upper == "BIGINT") return "bigint";
  if (upper == "DOUBLE PRECISION") return "double";
  if (upper == "FLOAT") return "float";
  if (upper == "DOUBLE") return "double";
  if (upper == "TEXT" || upper == "VARCHAR" || upper == "CHAR") return "text";
  if (upper == "CHARACTER") return "text";
  if (upper == "CHARACTER VARYING") return "text";
  if (upper == "BIT VARYING") return "bit_varying";
  if (upper == "BOOL") return "boolean";
  if (upper == "DEC" || upper == "NUMERIC") return "decimal";
  std::string lowered(type_text);
  for (auto& ch : lowered) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ch = '_';
    } else {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
  }
  return lowered;
}

std::string RouteTokenText(const Token& token) {
  return token.raw_text.empty() ? token.text : token.raw_text;
}

void AppendRouteTokenText(std::string* out, const Token& token) {
  if (out == nullptr) return;
  const std::string text = RouteTokenText(token);
  if (text.empty()) return;
  const bool punctuation = text == "(" || text == ")" || text == "," ||
                           text == "<" || text == ">" || text == "." ||
                           text == "[" || text == "]";
  const bool previous_punctuation =
      !out->empty() && (out->back() == '(' || out->back() == '<' ||
                        out->back() == '.' || out->back() == '[');
  if (!out->empty() && !punctuation && !previous_punctuation) {
    out->push_back(' ');
  }
  if ((text == ")" || text == ">" || text == "]" || text == ",") &&
      !out->empty() && out->back() == ' ') {
    out->pop_back();
  }
  *out += text;
  if (text == ",") out->push_back(' ');
}

std::string TrimRouteText(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.erase(text.begin());
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  return text;
}

bool IsColumnConstraintStarter(const Token& token) {
  return IsWord(token, "PRIMARY") || IsWord(token, "NOT") ||
         IsWord(token, "NULL") || IsWord(token, "DEFAULT") ||
         IsWord(token, "CHECK") || IsWord(token, "UNIQUE") ||
         IsWord(token, "REFERENCES") || IsWord(token, "COLLATE") ||
         IsWord(token, "GENERATED") || IsWord(token, "CONSTRAINT");
}

bool IsTableConstraintStarter(const Token& token) {
  return IsWord(token, "PRIMARY") || IsWord(token, "FOREIGN") ||
         IsWord(token, "UNIQUE") || IsWord(token, "CHECK") ||
         IsWord(token, "CONSTRAINT");
}

bool ConsumeBalancedRouteClause(const CstDocument& cst,
                                std::size_t* index,
                                std::string* text) {
  if (index == nullptr) return false;
  int paren_depth = 0;
  int angle_depth = 0;
  bool consumed = false;
  if (text != nullptr) text->clear();
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (paren_depth == 0 && angle_depth == 0 && (token.text == "," || token.text == ")")) break;
    if (token.text == "(") {
      ++paren_depth;
    } else if (token.text == ")" && paren_depth > 0) {
      --paren_depth;
    } else if (token.text == "<") {
      ++angle_depth;
    } else if (token.text == ">" && angle_depth > 0) {
      --angle_depth;
    }
    if (text != nullptr) AppendRouteTokenText(text, token);
    consumed = true;
    ++(*index);
  }
  if (text != nullptr) *text = TrimRouteText(*text);
  return consumed;
}

std::string ConsumeRouteTypeText(const CstDocument& cst,
                                 std::size_t* index,
                                 std::string* raw_type_text) {
  if (raw_type_text != nullptr) raw_type_text->clear();
  SkipTriviaTokens(cst, index);
  if (index == nullptr || *index >= cst.tokens.size() ||
      !IsIdentifierLikeForRouteExecution(cst.tokens[*index])) {
    return {};
  }

  std::vector<std::string> type_words;
  std::string rendered;
  int paren_depth = 0;
  int angle_depth = 0;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) break;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator) break;
    if (paren_depth == 0 && angle_depth == 0) {
      if (token.text == "," || token.text == ")" || IsColumnConstraintStarter(token)) break;
    }
    if (token.text == "(") {
      ++paren_depth;
    } else if (token.text == ")" && paren_depth > 0) {
      --paren_depth;
    } else if (token.text == "<") {
      ++angle_depth;
    } else if (token.text == ">" && angle_depth > 0) {
      --angle_depth;
    } else if (paren_depth == 0 && angle_depth == 0 &&
               IsIdentifierLikeForRouteExecution(token)) {
      type_words.push_back(token.text);
    }
    AppendRouteTokenText(&rendered, token);
    ++(*index);
  }
  rendered = TrimRouteText(rendered);
  if (raw_type_text != nullptr) *raw_type_text = rendered;
  if (type_words.empty()) return {};
  std::string canonical_input = type_words.front();
  if (type_words.size() > 1) {
    const std::string first_two = ToUpperAscii(type_words[0] + " " + type_words[1]);
    if (first_two == "DOUBLE PRECISION" ||
        first_two == "BIT VARYING" ||
        first_two == "CHARACTER VARYING") {
      canonical_input = type_words[0] + " " + type_words[1];
    }
  }
  return RouteCanonicalTypeName(canonical_input);
}

struct RouteColumnDefinition {
  std::string name;
  std::string canonical_type;
  std::string raw_type;
  bool nullable = true;
  bool primary_key = false;
  bool unique = false;
  std::string default_expression;
};

void AppendDescriptorFlag(std::string* descriptor,
                          std::string_view name,
                          std::string_view value) {
  if (descriptor == nullptr || name.empty()) return;
  if (!descriptor->empty() && descriptor->back() != ';') descriptor->push_back(';');
  descriptor->append(name);
  descriptor->push_back('=');
  descriptor->append(value);
}

bool ConsumeColumnConstraints(const CstDocument& cst,
                              std::size_t* index,
                              RouteColumnDefinition* column) {
  if (index == nullptr || column == nullptr) return false;
  for (;;) {
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size()) return false;
    const auto& token = cst.tokens[*index];
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kStatementTerminator ||
        token.text == "," || token.text == ")") {
      return true;
    }
    if (ConsumeRouteKeyword(cst, index, "CONSTRAINT")) {
      std::string ignored_name;
      if (!ConsumeRouteIdentifier(cst, index, &ignored_name)) return false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "PRIMARY")) {
      if (!ConsumeRouteKeyword(cst, index, "KEY")) return false;
      column->primary_key = true;
      column->unique = true;
      column->nullable = false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "UNIQUE")) {
      column->unique = true;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "NOT")) {
      if (!ConsumeRouteKeyword(cst, index, "NULL")) return false;
      column->nullable = false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "NULL")) {
      column->nullable = true;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "DEFAULT")) {
      std::string expression;
      if (!ConsumeBalancedRouteClause(cst, index, &expression)) return false;
      column->default_expression = std::move(expression);
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "CHECK")) {
      std::string ignored;
      if (!ConsumeBalancedRouteClause(cst, index, &ignored)) return false;
      continue;
    }
    if (ConsumeRouteKeyword(cst, index, "REFERENCES") ||
        ConsumeRouteKeyword(cst, index, "COLLATE") ||
        ConsumeRouteKeyword(cst, index, "GENERATED")) {
      std::string ignored;
      ConsumeBalancedRouteClause(cst, index, &ignored);
      continue;
    }
    return false;
  }
}

bool ConsumeTableConstraint(const CstDocument& cst,
                            std::size_t* index,
                            std::vector<RouteColumnDefinition>* columns) {
  if (index == nullptr || columns == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size()) return false;
  if (ConsumeRouteKeyword(cst, index, "CONSTRAINT")) {
    std::string ignored_name;
    if (!ConsumeRouteIdentifier(cst, index, &ignored_name)) return false;
  }
  if (ConsumeRouteKeyword(cst, index, "PRIMARY")) {
    if (!ConsumeRouteKeyword(cst, index, "KEY")) return false;
    if (!ConsumeRouteSymbol(cst, index, "(")) return false;
    for (;;) {
      std::string column_name;
      if (!ConsumeRouteIdentifier(cst, index, &column_name)) return false;
      for (auto& column : *columns) {
        if (ToUpperAscii(column.name) == ToUpperAscii(column_name)) {
          column.primary_key = true;
          column.unique = true;
          column.nullable = false;
        }
      }
      if (ConsumeRouteSymbol(cst, index, ")")) break;
      if (!ConsumeRouteSymbol(cst, index, ",")) return false;
    }
    return true;
  }
  if (ConsumeRouteKeyword(cst, index, "UNIQUE")) {
    if (ConsumeRouteSymbol(cst, index, "(")) {
      for (;;) {
        std::string column_name;
        if (!ConsumeRouteIdentifier(cst, index, &column_name)) return false;
        for (auto& column : *columns) {
          if (ToUpperAscii(column.name) == ToUpperAscii(column_name)) {
            column.unique = true;
          }
        }
        if (ConsumeRouteSymbol(cst, index, ")")) break;
        if (!ConsumeRouteSymbol(cst, index, ",")) return false;
      }
      return true;
    }
  }
  std::string ignored;
  return ConsumeBalancedRouteClause(cst, index, &ignored);
}

bool ConsumeOptionalTemporaryTablePrefix(const CstDocument& cst,
                                         std::size_t* index,
                                         bool* temporary,
                                         std::string* temporary_scope) {
  if (index == nullptr || temporary == nullptr || temporary_scope == nullptr) {
    return false;
  }
  SkipTriviaTokens(cst, index);
  *temporary = false;
  *temporary_scope = "private";
  if (*index < cst.tokens.size() && IsWord(cst.tokens[*index], "GLOBAL")) {
    ++(*index);
    if (!ConsumeRouteKeyword(cst, index, "TEMPORARY")) return false;
    *temporary = true;
    *temporary_scope = "global";
    return true;
  }
  if (*index < cst.tokens.size() && IsWord(cst.tokens[*index], "LOCAL")) {
    ++(*index);
    SkipTriviaTokens(cst, index);
    if (*index >= cst.tokens.size() ||
        (!IsWord(cst.tokens[*index], "TEMPORARY") &&
         !IsWord(cst.tokens[*index], "TEMP"))) {
      return false;
    }
    ++(*index);
    *temporary = true;
    *temporary_scope = "private";
    return true;
  }
  if (*index < cst.tokens.size() &&
      (IsWord(cst.tokens[*index], "TEMPORARY") ||
       IsWord(cst.tokens[*index], "TEMP"))) {
    ++(*index);
    *temporary = true;
    *temporary_scope = "private";
  }
  return true;
}

bool ConsumeOptionalOnCommitAction(const CstDocument& cst,
                                   std::size_t* index,
                                   std::string* on_commit_action) {
  if (index == nullptr || on_commit_action == nullptr) return false;
  *on_commit_action = "delete_rows";
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size() || cst.tokens[*index].kind == TokenKind::kEnd ||
      cst.tokens[*index].kind == TokenKind::kStatementTerminator) {
    return true;
  }
  if (!ConsumeRouteKeyword(cst, index, "ON") ||
      !ConsumeRouteKeyword(cst, index, "COMMIT")) {
    return false;
  }
  if (ConsumeRouteKeyword(cst, index, "DELETE")) {
    if (!ConsumeRouteKeyword(cst, index, "ROWS")) return false;
    *on_commit_action = "delete_rows";
    return true;
  }
  if (ConsumeRouteKeyword(cst, index, "PRESERVE")) {
    if (!ConsumeRouteKeyword(cst, index, "ROWS")) return false;
    *on_commit_action = "preserve_rows";
    return true;
  }
  return false;
}

std::string EscapeRouteOperandField(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || ch == '\t' || ch == '\n' || ch == '\r') out.push_back('\\');
    if (ch == '\n') {
      out.push_back('n');
    } else if (ch == '\r') {
      out.push_back('r');
    } else if (ch == '\t') {
      out.push_back('t');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

void AppendRouteTextOperand(std::string* out, std::string_view name, std::string_view value) {
  if (out == nullptr) return;
  *out += "operand=text\t";
  *out += name;
  *out += "\t";
  *out += EscapeRouteOperandField(value);
  *out += "\n";
}

std::string HexEncodeRouteText(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2u);
  for (const unsigned char ch : value) {
    out.push_back(kHex[(ch >> 4u) & 0x0fu]);
    out.push_back(kHex[ch & 0x0fu]);
  }
  return out;
}

struct FastInsertValueField {
  std::string name;
  std::string type_name;
  std::string value;
  bool is_null{false};
};

struct FastInsertValuesRoutePlan {
  ObjectReference target;
  std::vector<std::vector<FastInsertValueField>> rows;
  std::size_t column_count{0};
};

struct FastCopyFromStdinRoutePlan {
  ObjectReference target;
  std::string format_family{"csv"};
  bool copy_options_present{false};
  bool copy_header_option{false};
};

std::string FastInsertTypedLiteralType(std::string upper);

class FastCopyFromStdinScanner {
public:
  explicit FastCopyFromStdinScanner(std::string_view sql) : sql_(sql) {}

  std::optional<FastCopyFromStdinRoutePlan> Parse() {
    if (!ConsumeKeyword("COPY")) return std::nullopt;

    std::vector<std::string> target_parts;
    bool target_quoted = false;
    if (!ConsumeQualifiedNameParts(&target_parts, &target_quoted)) return std::nullopt;

    if (!ConsumeKeyword("FROM")) return std::nullopt;
    if (!ConsumeKeyword("STDIN")) return std::nullopt;

    FastCopyFromStdinRoutePlan plan;
    plan.target.presented_name = JoinRouteNameParts(target_parts, 0, target_parts.size());
    plan.target.quoted = target_quoted;
    plan.target.object_class = "relation";

    if (ConsumeKeyword("WITH")) {
      plan.copy_options_present = true;
      if (!ConsumeCopyOptions(&plan)) return std::nullopt;
    }

    SkipTrivia();
    while (pos_ < sql_.size() && sql_[pos_] == ';') {
      ++pos_;
      SkipTrivia();
    }
    if (pos_ != sql_.size()) return std::nullopt;
    return plan;
  }

private:
  void SkipTrivia() {
    for (;;) {
      while (pos_ < sql_.size() &&
             std::isspace(static_cast<unsigned char>(sql_[pos_]))) {
        ++pos_;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '-' && sql_[pos_ + 1] == '-') {
        pos_ += 2;
        while (pos_ < sql_.size() && sql_[pos_] != '\n') ++pos_;
        continue;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '/' && sql_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < sql_.size() &&
               !(sql_[pos_] == '*' && sql_[pos_ + 1] == '/')) {
          ++pos_;
        }
        if (pos_ + 1 >= sql_.size()) {
          pos_ = sql_.size();
          return;
        }
        pos_ += 2;
        continue;
      }
      break;
    }
  }

  bool IsIdentifierStart(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalpha(uch) || ch == '_';
  }

  bool IsIdentifierBody(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '$';
  }

  bool KeywordBoundary(std::size_t end) const {
    return end >= sql_.size() || !IsIdentifierBody(sql_[end]);
  }

  bool ConsumeKeyword(std::string_view keyword) {
    SkipTrivia();
    if (pos_ + keyword.size() > sql_.size()) return false;
    for (std::size_t offset = 0; offset < keyword.size(); ++offset) {
      const auto ch = static_cast<unsigned char>(sql_[pos_ + offset]);
      if (std::toupper(ch) != keyword[offset]) return false;
    }
    const std::size_t end = pos_ + keyword.size();
    if (!KeywordBoundary(end)) return false;
    pos_ = end;
    return true;
  }

  bool ConsumeChar(char expected) {
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ConsumeIdentifier(std::string* out, bool* quoted) {
    if (out == nullptr || quoted == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size()) return false;
    out->clear();
    *quoted = false;
    if (sql_[pos_] == '"') {
      *quoted = true;
      ++pos_;
      while (pos_ < sql_.size()) {
        const char ch = sql_[pos_++];
        if (ch == '"') {
          if (pos_ < sql_.size() && sql_[pos_] == '"') {
            out->push_back('"');
            ++pos_;
            continue;
          }
          return true;
        }
        out->push_back(ch);
      }
      return false;
    }
    if (!IsIdentifierStart(sql_[pos_])) return false;
    const std::size_t begin = pos_++;
    while (pos_ < sql_.size() && IsIdentifierBody(sql_[pos_])) ++pos_;
    out->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeQualifiedNameParts(std::vector<std::string>* parts, bool* quoted) {
    if (parts == nullptr || quoted == nullptr) return false;
    parts->clear();
    *quoted = false;
    for (;;) {
      std::string part;
      bool part_quoted = false;
      if (!ConsumeIdentifier(&part, &part_quoted)) return false;
      *quoted = *quoted || part_quoted;
      parts->push_back(std::move(part));
      SkipTrivia();
      if (pos_ >= sql_.size() || sql_[pos_] != '.') break;
      ++pos_;
    }
    return !parts->empty();
  }

  bool ConsumeSingleQuoted(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != '\'') return false;
    out->clear();
    ++pos_;
    while (pos_ < sql_.size()) {
      const char ch = sql_[pos_++];
      if (ch == '\'') {
        if (pos_ < sql_.size() && sql_[pos_] == '\'') {
          out->push_back('\'');
          ++pos_;
          continue;
        }
        return true;
      }
      out->push_back(ch);
    }
    return false;
  }

  bool ConsumeOptionValue(std::string* value) {
    if (value == nullptr) return false;
    SkipTrivia();
    if (ConsumeSingleQuoted(value)) return true;
    bool quoted = false;
    if (ConsumeIdentifier(value, &quoted)) {
      (void)quoted;
      return true;
    }
    if (pos_ >= sql_.size()) return false;
    const std::size_t begin = pos_;
    if (sql_[pos_] == '+' || sql_[pos_] == '-') ++pos_;
    bool saw_digit = false;
    while (pos_ < sql_.size() &&
           std::isdigit(static_cast<unsigned char>(sql_[pos_]))) {
      saw_digit = true;
      ++pos_;
    }
    if (!saw_digit) {
      pos_ = begin;
      return false;
    }
    value->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeCopyOptions(FastCopyFromStdinRoutePlan* plan) {
    if (plan == nullptr) return false;
    if (!ConsumeChar('(')) {
      if (ConsumeKeyword("HEADER")) {
        plan->copy_header_option = true;
        return true;
      }
      return false;
    }
    for (;;) {
      std::string option;
      bool quoted = false;
      if (!ConsumeIdentifier(&option, &quoted) || quoted) return false;
      const std::string upper_option = ToUpperAscii(option);
      std::string value;
      if (ConsumeChar('=')) {
        if (!ConsumeOptionValue(&value)) return false;
      } else {
        const std::size_t saved = pos_;
        if (!ConsumeOptionValue(&value)) {
          pos_ = saved;
        }
      }
      if (upper_option == "HEADER") {
        plan->copy_header_option = true;
      } else if (upper_option == "FORMAT" && !value.empty()) {
        const std::string upper_value = ToUpperAscii(value);
        if (upper_value == "JSONL" || upper_value == "JSON") {
          plan->format_family = "jsonl";
        } else if (upper_value == "CSV") {
          plan->format_family = "csv";
        } else {
          return false;
        }
      } else if (upper_option != "NATIVE_BULK_INGEST" &&
                 upper_option != "NATIVE_BULK_INGEST_ENABLED") {
        return false;
      }
      if (ConsumeChar(',')) continue;
      if (!ConsumeChar(')')) return false;
      break;
    }
    return true;
  }

  std::string_view sql_;
  std::size_t pos_{0};
};

class FastInsertValuesScanner {
public:
  explicit FastInsertValuesScanner(std::string_view sql) : sql_(sql) {}

  std::optional<FastInsertValuesRoutePlan> Parse() {
    if (!ConsumeKeyword("INSERT")) return std::nullopt;
    if (!ConsumeKeyword("INTO")) return std::nullopt;

    std::vector<std::string> target_parts;
    bool target_quoted = false;
    if (!ConsumeQualifiedNameParts(&target_parts, &target_quoted)) return std::nullopt;

    std::vector<std::string> column_names;
    if (!ConsumeChar('(')) return std::nullopt;
    for (;;) {
      std::string column_name;
      bool column_quoted = false;
      if (!ConsumeIdentifier(&column_name, &column_quoted)) return std::nullopt;
      (void)column_quoted;
      column_names.push_back(std::move(column_name));
      if (ConsumeChar(',')) continue;
      if (!ConsumeChar(')')) return std::nullopt;
      break;
    }
    if (column_names.empty()) return std::nullopt;
    if (!ConsumeKeyword("VALUES")) return std::nullopt;

    FastInsertValuesRoutePlan plan;
    plan.target.presented_name =
        JoinRouteNameParts(target_parts, 0, target_parts.size());
    plan.target.quoted = target_quoted;
    plan.target.object_class = "relation";
    plan.column_count = column_names.size();

    for (;;) {
      if (!ConsumeChar('(')) return std::nullopt;
      std::vector<FastInsertValueField> row;
      row.reserve(column_names.size());
      for (std::size_t column_index = 0; column_index < column_names.size(); ++column_index) {
        FastInsertValueField field;
        field.name = column_names[column_index];
        if (!ConsumeLiteralValue(&field)) return std::nullopt;
        row.push_back(std::move(field));
        if (column_index + 1 < column_names.size() && !ConsumeChar(',')) {
          return std::nullopt;
        }
      }
      if (!ConsumeChar(')')) return std::nullopt;
      plan.rows.push_back(std::move(row));
      if (ConsumeChar(',')) continue;
      break;
    }

    if (plan.rows.empty()) return std::nullopt;
    SkipTrivia();
    while (pos_ < sql_.size() && sql_[pos_] == ';') {
      ++pos_;
      SkipTrivia();
    }
    if (pos_ != sql_.size()) return std::nullopt;
    return plan;
  }

private:
  void SkipTrivia() {
    for (;;) {
      while (pos_ < sql_.size() &&
             std::isspace(static_cast<unsigned char>(sql_[pos_]))) {
        ++pos_;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '-' && sql_[pos_ + 1] == '-') {
        pos_ += 2;
        while (pos_ < sql_.size() && sql_[pos_] != '\n') ++pos_;
        continue;
      }
      if (pos_ + 1 < sql_.size() && sql_[pos_] == '/' && sql_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < sql_.size() &&
               !(sql_[pos_] == '*' && sql_[pos_ + 1] == '/')) {
          ++pos_;
        }
        if (pos_ + 1 >= sql_.size()) {
          pos_ = sql_.size();
          return;
        }
        pos_ += 2;
        continue;
      }
      break;
    }
  }

  bool IsIdentifierStart(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalpha(uch) || ch == '_';
  }

  bool IsIdentifierBody(char ch) const {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '$';
  }

  bool KeywordBoundary(std::size_t end) const {
    return end >= sql_.size() || !IsIdentifierBody(sql_[end]);
  }

  bool ConsumeKeyword(std::string_view keyword) {
    SkipTrivia();
    if (pos_ + keyword.size() > sql_.size()) return false;
    for (std::size_t offset = 0; offset < keyword.size(); ++offset) {
      const auto ch = static_cast<unsigned char>(sql_[pos_ + offset]);
      if (std::toupper(ch) != keyword[offset]) return false;
    }
    const std::size_t end = pos_ + keyword.size();
    if (!KeywordBoundary(end)) return false;
    pos_ = end;
    return true;
  }

  bool ConsumeChar(char expected) {
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ConsumeIdentifier(std::string* out, bool* quoted) {
    if (out == nullptr || quoted == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size()) return false;
    out->clear();
    *quoted = false;
    if (sql_[pos_] == '"') {
      *quoted = true;
      ++pos_;
      while (pos_ < sql_.size()) {
        const char ch = sql_[pos_++];
        if (ch == '"') {
          if (pos_ < sql_.size() && sql_[pos_] == '"') {
            out->push_back('"');
            ++pos_;
            continue;
          }
          return true;
        }
        out->push_back(ch);
      }
      return false;
    }
    if (!IsIdentifierStart(sql_[pos_])) return false;
    const std::size_t begin = pos_++;
    while (pos_ < sql_.size() && IsIdentifierBody(sql_[pos_])) ++pos_;
    out->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeQualifiedNameParts(std::vector<std::string>* parts, bool* quoted) {
    if (parts == nullptr || quoted == nullptr) return false;
    parts->clear();
    *quoted = false;
    for (;;) {
      std::string part;
      bool part_quoted = false;
      if (!ConsumeIdentifier(&part, &part_quoted)) return false;
      *quoted = *quoted || part_quoted;
      parts->push_back(std::move(part));
      SkipTrivia();
      if (pos_ >= sql_.size() || sql_[pos_] != '.') break;
      ++pos_;
    }
    return !parts->empty();
  }

  bool ConsumeSingleQuoted(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size() || sql_[pos_] != '\'') return false;
    out->clear();
    ++pos_;
    while (pos_ < sql_.size()) {
      const char ch = sql_[pos_++];
      if (ch == '\'') {
        if (pos_ < sql_.size() && sql_[pos_] == '\'') {
          out->push_back('\'');
          ++pos_;
          continue;
        }
        return true;
      }
      out->push_back(ch);
    }
    return false;
  }

  bool ConsumeUuidLiteral(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    constexpr std::size_t kUuidTextSize = 36;
    if (pos_ + kUuidTextSize > sql_.size()) return false;
    const auto is_hex = [](char ch) {
      const auto uch = static_cast<unsigned char>(ch);
      return std::isxdigit(uch) != 0;
    };
    for (std::size_t offset = 0; offset < kUuidTextSize; ++offset) {
      const char ch = sql_[pos_ + offset];
      if (offset == 8 || offset == 13 || offset == 18 || offset == 23) {
        if (ch != '-') return false;
      } else if (!is_hex(ch)) {
        return false;
      }
    }
    const std::size_t end = pos_ + kUuidTextSize;
    if (end < sql_.size() && IsIdentifierBody(sql_[end])) return false;
    out->assign(sql_.substr(pos_, kUuidTextSize));
    pos_ = end;
    return true;
  }

  std::string NumericTypeForLiteral(std::string_view text) const {
    const std::string upper = ToUpperAscii(text);
    const auto has_suffix = [&](std::string_view suffix) {
      return upper.size() > suffix.size() &&
             upper.compare(upper.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (has_suffix("UINT128") || has_suffix("U128")) return "uint128";
    if (has_suffix("INT128") || has_suffix("I128")) return "int128";
    if (has_suffix("REAL128") || has_suffix("R128")) return "real128";
    if (has_suffix("UINT") || has_suffix("U")) return "uint64";
    if (has_suffix("DECIMAL") || has_suffix("DEC") || has_suffix("D") ||
        has_suffix("DOUBLE") || has_suffix("FLOAT") || has_suffix("F")) {
      return "numeric";
    }
    return upper.find('.') != std::string::npos ||
                   upper.find('E') != std::string::npos
               ? "numeric"
               : "bigint";
  }

  bool ConsumeNumericLiteral(std::string* value, std::string* type_name) {
    if (value == nullptr || type_name == nullptr) return false;
    SkipTrivia();
    std::size_t cursor = pos_;
    if (cursor >= sql_.size()) return false;
    if (sql_[cursor] == '+' || sql_[cursor] == '-') ++cursor;
    const std::size_t digits_begin = cursor;
    while (cursor < sql_.size() &&
           std::isdigit(static_cast<unsigned char>(sql_[cursor]))) {
      ++cursor;
    }
    bool saw_digit = cursor > digits_begin;
    if (cursor < sql_.size() && sql_[cursor] == '.') {
      ++cursor;
      const std::size_t fraction_begin = cursor;
      while (cursor < sql_.size() &&
             std::isdigit(static_cast<unsigned char>(sql_[cursor]))) {
        ++cursor;
      }
      saw_digit = saw_digit || cursor > fraction_begin;
    }
    if (!saw_digit) return false;
    if (cursor < sql_.size() && (sql_[cursor] == 'e' || sql_[cursor] == 'E')) {
      std::size_t exponent = cursor + 1;
      if (exponent < sql_.size() && (sql_[exponent] == '+' || sql_[exponent] == '-')) {
        ++exponent;
      }
      const std::size_t exponent_digits = exponent;
      while (exponent < sql_.size() &&
             std::isdigit(static_cast<unsigned char>(sql_[exponent]))) {
        ++exponent;
      }
      if (exponent == exponent_digits) return false;
      cursor = exponent;
    }
    const std::size_t suffix_begin = cursor;
    while (cursor < sql_.size() &&
           std::isalpha(static_cast<unsigned char>(sql_[cursor]))) {
      ++cursor;
    }
    if (cursor < sql_.size() && IsIdentifierBody(sql_[cursor])) return false;
    if (suffix_begin != cursor) {
      const std::string suffix =
          ToUpperAscii(sql_.substr(suffix_begin, cursor - suffix_begin));
      if (suffix != "UINT" && suffix != "U" && suffix != "INT128" &&
          suffix != "I128" && suffix != "UINT128" && suffix != "U128" &&
          suffix != "REAL128" && suffix != "R128" && suffix != "DECIMAL" &&
          suffix != "DEC" && suffix != "D" && suffix != "DOUBLE" &&
          suffix != "FLOAT" && suffix != "F") {
        return false;
      }
    }
    value->assign(sql_.substr(pos_, cursor - pos_));
    *type_name = NumericTypeForLiteral(*value);
    pos_ = cursor;
    return true;
  }

  bool ConsumeWord(std::string* out) {
    if (out == nullptr) return false;
    SkipTrivia();
    if (pos_ >= sql_.size() || !IsIdentifierStart(sql_[pos_])) return false;
    const std::size_t begin = pos_++;
    while (pos_ < sql_.size() && IsIdentifierBody(sql_[pos_])) ++pos_;
    out->assign(sql_.substr(begin, pos_ - begin));
    return true;
  }

  bool ConsumeLiteralValue(FastInsertValueField* field) {
    if (field == nullptr) return false;
    SkipTrivia();

    std::string uuid_text;
    if (ConsumeUuidLiteral(&uuid_text)) {
      field->type_name = "uuid";
      field->value = std::move(uuid_text);
      field->is_null = false;
      return true;
    }

    std::string string_value;
    if (ConsumeSingleQuoted(&string_value)) {
      field->type_name = "text";
      field->value = std::move(string_value);
      field->is_null = false;
      return true;
    }

    std::string number_value;
    std::string number_type;
    if (ConsumeNumericLiteral(&number_value, &number_type)) {
      field->type_name = std::move(number_type);
      field->value = std::move(number_value);
      field->is_null = false;
      return true;
    }

    std::size_t word_position = pos_;
    std::string word;
    if (!ConsumeWord(&word)) return false;
    const std::string upper = ToUpperAscii(word);
    if (upper == "TRUE" || upper == "FALSE") {
      field->type_name = "boolean";
      field->value = upper == "TRUE" ? "true" : "false";
      field->is_null = false;
      return true;
    }
    if (upper == "NULL") {
      field->type_name = "null";
      field->value.clear();
      field->is_null = true;
      return true;
    }
    const std::string typed_literal = FastInsertTypedLiteralType(upper);
    if (!typed_literal.empty()) {
      if (!ConsumeSingleQuoted(&string_value)) {
        pos_ = word_position;
        return false;
      }
      field->type_name = typed_literal;
      field->value = std::move(string_value);
      field->is_null = false;
      return true;
    }
    if ((upper == "X" || upper == "B") && ConsumeSingleQuoted(&string_value)) {
      field->type_name = upper == "B" ? "bit_string" : "binary";
      field->value = std::move(string_value);
      field->is_null = false;
      return true;
    }
    pos_ = word_position;
    return false;
  }

  std::string_view sql_;
  std::size_t pos_{0};
};

bool LooksLikeFastInsertValuesCandidate(std::string_view sql) {
  std::size_t index = 0;
  for (;;) {
    while (index < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[index]))) {
      ++index;
    }
    if (index + 1 < sql.size() && sql[index] == '-' && sql[index + 1] == '-') {
      index += 2;
      while (index < sql.size() && sql[index] != '\n') ++index;
      continue;
    }
    if (index + 1 < sql.size() && sql[index] == '/' && sql[index + 1] == '*') {
      index += 2;
      while (index + 1 < sql.size() && !(sql[index] == '*' && sql[index + 1] == '/')) {
        ++index;
      }
      if (index + 1 < sql.size()) index += 2;
      continue;
    }
    break;
  }
  constexpr std::string_view kInsert = "INSERT";
  if (index + kInsert.size() > sql.size()) return false;
  for (std::size_t offset = 0; offset < kInsert.size(); ++offset) {
    const auto ch = static_cast<unsigned char>(sql[index + offset]);
    if (std::toupper(ch) != kInsert[offset]) return false;
  }
  const std::size_t next = index + kInsert.size();
  return next >= sql.size() ||
         !std::isalnum(static_cast<unsigned char>(sql[next]));
}

bool LooksLikeFastCopyFromStdinCandidate(std::string_view sql) {
  std::size_t index = 0;
  for (;;) {
    while (index < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[index]))) {
      ++index;
    }
    if (index + 1 < sql.size() && sql[index] == '-' && sql[index + 1] == '-') {
      index += 2;
      while (index < sql.size() && sql[index] != '\n') ++index;
      continue;
    }
    if (index + 1 < sql.size() && sql[index] == '/' && sql[index + 1] == '*') {
      index += 2;
      while (index + 1 < sql.size() && !(sql[index] == '*' && sql[index + 1] == '/')) {
        ++index;
      }
      if (index + 1 < sql.size()) index += 2;
      continue;
    }
    break;
  }
  constexpr std::string_view kCopy = "COPY";
  if (index + kCopy.size() > sql.size()) return false;
  for (std::size_t offset = 0; offset < kCopy.size(); ++offset) {
    const auto ch = static_cast<unsigned char>(sql[index + offset]);
    if (std::toupper(ch) != kCopy[offset]) return false;
  }
  const std::size_t next = index + kCopy.size();
  return next >= sql.size() ||
         !std::isalnum(static_cast<unsigned char>(sql[next]));
}

std::optional<FastCopyFromStdinRoutePlan> TryParseFastCopyFromStdinRoutePlan(
    std::string_view sql) {
  return FastCopyFromStdinScanner(sql).Parse();
}

std::optional<FastInsertValuesRoutePlan> TryParseFastInsertValuesRoutePlan(
    std::string_view sql) {
  return FastInsertValuesScanner(sql).Parse();
}

bool ConsumeFastInsertKeyword(const CstDocument& cst,
                              std::size_t* index,
                              std::string_view keyword) {
  return ConsumeRouteKeyword(cst, index, keyword);
}

bool ConsumeFastInsertSymbol(const CstDocument& cst,
                             std::size_t* index,
                             std::string_view symbol) {
  return ConsumeRouteSymbol(cst, index, symbol);
}

std::string FastInsertLiteralPayload(const Token& token) {
  if (token.kind == TokenKind::kBooleanLiteral) {
    return ToUpperAscii(token.text) == "TRUE" ? "true" : "false";
  }
  return token.text;
}

std::string FastInsertScalarTypeForToken(const Token& token) {
  if (token.kind == TokenKind::kNumericLiteral) {
    if (token.literal_family == "uint") return "uint64";
    if (token.literal_family == "int128") return "int128";
    if (token.literal_family == "uint128") return "uint128";
    if (token.literal_family == "real128") return "real128";
    return token.literal_family == "decimal" || token.literal_family == "float" ? "numeric"
                                                                                 : "bigint";
  }
  if (token.kind == TokenKind::kBooleanLiteral) return "boolean";
  if (token.kind == TokenKind::kBinaryLiteral) {
    return token.literal_family == "bit_binary" ? "bit_string" : "binary";
  }
  if (token.kind == TokenKind::kUuidLiteral) return "uuid";
  if (token.kind == TokenKind::kTemporalLiteral) {
    const std::string family = ToUpperAscii(token.literal_family);
    if (family == "DATE") return "date";
    if (family == "TIME") return "time";
    if (family == "TIMESTAMP") return "timestamp";
    if (family == "INTERVAL") return "interval";
  }
  if (token.kind == TokenKind::kDocumentLiteral) {
    return ToUpperAscii(token.literal_family) == "JSON" ? "json_document" : "document";
  }
  if (token.kind == TokenKind::kVectorLiteral) return "dense_vector";
  if (token.kind == TokenKind::kNullLiteral) return "null";
  return "text";
}

bool IsFastInsertScalarLiteral(const Token& token) {
  return token.kind == TokenKind::kNumericLiteral ||
         token.kind == TokenKind::kStringLiteral ||
         token.kind == TokenKind::kBinaryLiteral ||
         token.kind == TokenKind::kBooleanLiteral ||
         token.kind == TokenKind::kNullLiteral ||
         token.kind == TokenKind::kUuidLiteral ||
         (token.kind == TokenKind::kDocumentLiteral &&
          (ToUpperAscii(token.literal_family) == "DOCUMENT" ||
           ToUpperAscii(token.literal_family) == "JSON")) ||
         (token.kind == TokenKind::kVectorLiteral &&
          ToUpperAscii(token.literal_family) == "VECTOR") ||
         (token.kind == TokenKind::kTemporalLiteral &&
          (ToUpperAscii(token.literal_family) == "DATE" ||
           ToUpperAscii(token.literal_family) == "TIME" ||
           ToUpperAscii(token.literal_family) == "TIMESTAMP" ||
           ToUpperAscii(token.literal_family) == "INTERVAL"));
}

std::string FastInsertTypedLiteralType(std::string upper) {
  if (upper == "DATE") return "date";
  if (upper == "TIME") return "time";
  if (upper == "TIMESTAMP") return "timestamp";
  if (upper == "TIMESTAMPTZ") return "timestamptz";
  if (upper == "INTERVAL") return "interval";
  if (upper == "UUID") return "uuid";
  if (upper == "JSON") return "json_document";
  if (upper == "XML") return "xml_document";
  if (upper == "VECTOR") return "dense_vector";
  return {};
}

bool ConsumeFastInsertLiteralValue(const CstDocument& cst,
                                   std::size_t* index,
                                   FastInsertValueField* field) {
  if (index == nullptr || field == nullptr) return false;
  SkipTriviaTokens(cst, index);
  if (*index >= cst.tokens.size()) return false;

  bool negative = false;
  if (cst.tokens[*index].text == "-" &&
      *index + 1 < cst.tokens.size() &&
      cst.tokens[*index + 1].kind == TokenKind::kNumericLiteral) {
    negative = true;
    ++(*index);
    SkipTriviaTokens(cst, index);
  }
  if (*index >= cst.tokens.size()) return false;
  const Token& token = cst.tokens[*index];
  if (negative && token.kind != TokenKind::kNumericLiteral) return false;

  if (!negative && IsIdentifierLikeForRouteExecution(token)) {
    const std::string typed_literal = FastInsertTypedLiteralType(ToUpperAscii(token.text));
    std::size_t literal_index = *index + 1;
    SkipTriviaTokens(cst, &literal_index);
    if (!typed_literal.empty() && literal_index < cst.tokens.size() &&
        cst.tokens[literal_index].kind == TokenKind::kStringLiteral) {
      field->type_name = typed_literal;
      field->value = cst.tokens[literal_index].text;
      field->is_null = false;
      *index = literal_index + 1;
      return true;
    }
  }

  if (!IsFastInsertScalarLiteral(token)) return false;
  field->type_name = FastInsertScalarTypeForToken(token);
  field->value = negative ? "-" + FastInsertLiteralPayload(token)
                          : FastInsertLiteralPayload(token);
  field->is_null = token.kind == TokenKind::kNullLiteral;
  ++(*index);
  return true;
}

std::string FastInsertCompactPayload(const FastInsertValuesRoutePlan& plan) {
  std::string payload;
  bool first = true;
  for (const auto& row : plan.rows) {
    for (const auto& field : row) {
      if (!first) payload.push_back(';');
      first = false;
      payload += HexEncodeRouteText(field.name);
      payload.push_back('|');
      payload += HexEncodeRouteText(field.type_name);
      payload.push_back('|');
      payload += HexEncodeRouteText(field.value);
      payload.push_back('|');
      payload.push_back(field.is_null ? '1' : '0');
    }
  }
  return payload;
}

std::optional<FastInsertValuesRoutePlan> TryParseFastInsertValuesRoutePlan(
    const CstDocument& cst) {
  std::size_t index = 0;
  if (!ConsumeFastInsertKeyword(cst, &index, "INSERT")) return std::nullopt;
  if (!ConsumeFastInsertKeyword(cst, &index, "INTO")) return std::nullopt;

  std::vector<std::string> target_parts;
  bool target_quoted = false;
  if (!ConsumeRouteQualifiedNameParts(cst, &index, &target_parts, &target_quoted)) {
    return std::nullopt;
  }

  std::vector<std::string> column_names;
  if (!ConsumeFastInsertSymbol(cst, &index, "(")) return std::nullopt;
  for (;;) {
    std::string column_name;
    if (!ConsumeRouteIdentifier(cst, &index, &column_name)) return std::nullopt;
    column_names.push_back(std::move(column_name));
    if (ConsumeFastInsertSymbol(cst, &index, ",")) continue;
    if (!ConsumeFastInsertSymbol(cst, &index, ")")) return std::nullopt;
    break;
  }
  if (column_names.empty()) return std::nullopt;
  if (!ConsumeFastInsertKeyword(cst, &index, "VALUES")) return std::nullopt;

  FastInsertValuesRoutePlan plan;
  plan.target.presented_name = JoinRouteNameParts(target_parts, 0, target_parts.size());
  plan.target.quoted = target_quoted;
  plan.target.object_class = "relation";
  plan.column_count = column_names.size();

  for (;;) {
    if (!ConsumeFastInsertSymbol(cst, &index, "(")) return std::nullopt;
    std::vector<FastInsertValueField> row;
    row.reserve(column_names.size());
    for (std::size_t column_index = 0; column_index < column_names.size(); ++column_index) {
      FastInsertValueField field;
      field.name = column_names[column_index];
      if (!ConsumeFastInsertLiteralValue(cst, &index, &field)) return std::nullopt;
      row.push_back(std::move(field));
      if (column_index + 1 < column_names.size()) {
        if (!ConsumeFastInsertSymbol(cst, &index, ",")) return std::nullopt;
      }
    }
    if (!ConsumeFastInsertSymbol(cst, &index, ")")) return std::nullopt;
    plan.rows.push_back(std::move(row));
    if (ConsumeFastInsertSymbol(cst, &index, ",")) continue;
    break;
  }

  if (plan.rows.empty()) return std::nullopt;
  SkipTriviaTokens(cst, &index);
  while (index < cst.tokens.size() &&
         cst.tokens[index].kind == TokenKind::kStatementTerminator) {
    ++index;
    SkipTriviaTokens(cst, &index);
  }
  if (index < cst.tokens.size() && cst.tokens[index].kind != TokenKind::kEnd) {
    return std::nullopt;
  }
  return plan;
}

std::string BuildFastInsertNativeBulkEnvelope(
    const FastInsertValuesRoutePlan& plan,
    std::string_view target_object_uuid) {
  std::string out;
  out += "operation_id=dml.execute_native_bulk_ingest\n";
  out += "opcode=SBLR_DML_EXECUTE_NATIVE_BULK_INGEST\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=sbsql.parser.fast_insert_values.native_bulk\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "target_object_uuid=";
  out += target_object_uuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "dml_surface_variant=sbsql_insert_values_fast_native_bulk\n";
  out += "source_kind=sbsql_insert_values_compact_rowset\n";
  out += "format_family=sbsql.insert_values.cells.v1\n";
  out += "source_fingerprint=sbsql-fast-insert-values-explicit-columns\n";
  out += "source_position=row:0\n";
  out += "estimated_row_count=" + std::to_string(plan.rows.size()) + "\n";
  out += "native_bulk_ingest=true\n";
  out += "native_bulk_ingest_enabled=true\n";
  out += "reject_mode=fail_fast\n";
  out += "reject_limit_rows=0\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "result_payload_policy=summary_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  AppendRouteTextOperand(&out, "physical_mga_cow", "false");
  AppendRouteTextOperand(&out, "insert_trace.rows", "false");
  AppendRouteTextOperand(&out, "sblr.rowset_default_markers_absent", "true");
  AppendRouteTextOperand(&out, "insert_values_row_count", std::to_string(plan.rows.size()));
  AppendRouteTextOperand(&out, "insert_values_column_count", std::to_string(plan.column_count));
  AppendRouteTextOperand(&out, "insert_values_column_list_present", "true");
  AppendRouteTextOperand(&out, "insert_values_compact_format", "sbsql.insert_values.cells.v1");
  AppendRouteTextOperand(&out, "insert_values_compact_payload", FastInsertCompactPayload(plan));
  AppendRouteTextOperand(&out, "insert_values_parser_executes_sql", "false");
  AppendRouteTextOperand(&out, "sblr.canonical_rowset_shared_shape", "true");
  AppendRouteTextOperand(&out, "sblr.fast_insert_values_lowering", "true");
  return out;
}

std::string BuildFastCopyPlanExecutionEnvelope(
    const FastCopyFromStdinRoutePlan& plan,
    std::string_view target_object_uuid) {
  std::string out;
  out += "operation_id=dml.plan_import_rows\n";
  out += "opcode=SBLR_DML_PLAN_IMPORT_ROWS\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=sbsql.parser.fast_copy.plan_import\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "target_object_uuid=";
  out += target_object_uuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "dml_surface_variant=copy_import_export\n";
  out += "source_kind=native_sbsql_import\n";
  out += "format_family=";
  out += plan.format_family;
  out += "\n";
  AppendRouteTextOperand(&out, "target_object_uuid", target_object_uuid);
  AppendRouteTextOperand(&out, "target_object_kind", "table");
  AppendRouteTextOperand(&out, "dml_surface_variant", "copy_import_export");
  AppendRouteTextOperand(&out, "source_kind", "native_sbsql_import");
  AppendRouteTextOperand(&out, "format_family", plan.format_family);
  AppendRouteTextOperand(&out, "copy_options_present",
                         plan.copy_options_present ? "true" : "false");
  AppendRouteTextOperand(&out, "copy_header_option",
                         plan.copy_header_option ? "true" : "false");
  AppendRouteTextOperand(&out, "source_handle_included", "false");
  AppendRouteTextOperand(&out, "parser_decodes_bytes", "false");
  AppendRouteTextOperand(&out, "row_persistence_claimed", "false");
  AppendRouteTextOperand(&out, "import_execution_deferred", "true");
  AppendRouteTextOperand(&out, "sblr.fast_copy_plan_lowering", "true");
  (void)plan.target;
  return out;
}

std::string BuildFastCopyPlanJsonPayload(
    const FastCopyFromStdinRoutePlan& plan,
    std::string_view target_object_uuid,
    std::uint64_t statement_hash) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.dml.operation.v3\",";
  out += "\"surface_key\":\"copy_import_export\",";
  out += "\"command_family\":\"dml\",";
  out += "\"operation_id\":\"dml.plan_import_rows\",";
  out += "\"engine_api_operation_id\":\"dml.plan_import_rows\",";
  out += "\"sblr_operation\":\"SBLR_DML_PLAN_IMPORT_ROWS\",";
  out += "\"statement_surface_name\":\"copy_import_export\",";
  out += "\"sblr_operation_key\":\"sblr.dml.operation.v3\",";
  out += "\"result_shape\":\"engine.api.result.v1\",";
  out += "\"diagnostic_shape\":\"engine.diagnostic.v1\",";
  out += "\"resource_contract\":\"resource.contract.dml.import.v1\",";
  out += "\"trace_key\":\"sbsql.parser.fast_copy.plan_import\",";
  out += "\"statement_hash\":";
  out += std::to_string(statement_hash);
  out += ",\"catalog_epoch\":0,\"security_policy_epoch\":0,\"descriptor_epoch\":0,";
  out += "\"source_artifact_policy\":\"span_metadata_only\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"real_file_effects\":false,";
  out += "\"parser_executes_sql\":false,";
  out += "\"import_execution_deferred\":true,";
  out += "\"target_object_kind\":\"table\",";
  out += "\"target_object_uuid\":\"";
  out += EscapeJson(target_object_uuid);
  out += "\",\"source_kind\":\"native_sbsql_import\",";
  out += "\"format_family\":\"";
  out += EscapeJson(plan.format_family);
  out += "\",\"copy_options_present\":";
  out += plan.copy_options_present ? "true" : "false";
  out += ",\"copy_header_option\":";
  out += plan.copy_header_option ? "true" : "false";
  out += ",\"source_handle_included\":false,";
  out += "\"parser_decodes_bytes\":false,";
  out += "\"row_persistence_claimed\":false,";
  out += "\"parser_authorizes\":false,";
  out += "\"name_text_included\":false,";
  out += "\"sql_text_included\":false,";
  out += "\"resolved_object_uuids\":[\"";
  out += EscapeJson(target_object_uuid);
  out += "\"],";
  out += "\"descriptor_refs\":[\"sys.storage.row_descriptor\",\"sys.import.plan_descriptor\"],";
  out += "\"policy_refs\":[\"import_planning_authorization_policy\"],";
  out += "\"required_rights\":[\"right.write\"],";
  out += "\"required_authority_steps\":[";
  out += "\"authority.parser.syntax_evidence_only\",";
  out += "\"authority.server.resolve_name_registry_public\",";
  out += "\"authority.server.security_policy_context_required\",";
  out += "\"authority.server.transaction_context_required\",";
  out += "\"authority.engine.import_planning_api_required\",";
  out += "\"authority.parser.no_storage_or_finality\",";
  out += "\"authority.parser.no_sql_text_execution\"]}";
  return out;
}

std::optional<std::string> CreateTableRouteExecutionEnvelope(
    const CstDocument& cst,
    std::string_view operation_family) {
  std::size_t index = 0;
  if (!ConsumeRouteKeyword(cst, &index, "CREATE")) return std::nullopt;
  bool temporary = false;
  std::string temporary_scope = "private";
  if (!ConsumeOptionalTemporaryTablePrefix(
          cst, &index, &temporary, &temporary_scope)) {
    return std::nullopt;
  }
  if (!ConsumeRouteKeyword(cst, &index, "TABLE")) return std::nullopt;
  std::vector<std::string> table_name_parts;
  if (!ConsumeRouteQualifiedNameParts(cst, &index, &table_name_parts)) return std::nullopt;
  const std::string table_name = table_name_parts.back();
  const std::string schema_parent_path =
      table_name_parts.size() > 1
          ? JoinRouteNameParts(table_name_parts, 0, table_name_parts.size() - 1)
          : std::string{};
  if (!ConsumeRouteSymbol(cst, &index, "(")) return std::nullopt;
  std::vector<RouteColumnDefinition> columns;
  for (;;) {
    SkipTriviaTokens(cst, &index);
    if (index >= cst.tokens.size()) return std::nullopt;
    if (ConsumeRouteSymbol(cst, &index, ")")) break;
    if (IsTableConstraintStarter(cst.tokens[index])) {
      if (!ConsumeTableConstraint(cst, &index, &columns)) return std::nullopt;
    } else {
      RouteColumnDefinition column;
      if (!ConsumeRouteIdentifier(cst, &index, &column.name)) return std::nullopt;
      column.canonical_type = ConsumeRouteTypeText(cst, &index, &column.raw_type);
      if (column.canonical_type.empty()) return std::nullopt;
      if (!ConsumeColumnConstraints(cst, &index, &column)) return std::nullopt;
      columns.push_back(std::move(column));
    }
    SkipTriviaTokens(cst, &index);
    if (ConsumeRouteSymbol(cst, &index, ",")) continue;
    if (ConsumeRouteSymbol(cst, &index, ")")) break;
    return std::nullopt;
  }
  if (columns.empty()) return std::nullopt;
  std::string on_commit_action = "delete_rows";
  if (temporary &&
      !ConsumeOptionalOnCommitAction(cst, &index, &on_commit_action)) {
    return std::nullopt;
  }
  SkipTriviaTokens(cst, &index);
  while (index < cst.tokens.size() &&
         cst.tokens[index].kind == TokenKind::kStatementTerminator) {
    ++index;
    SkipTriviaTokens(cst, &index);
  }
  if (index < cst.tokens.size() && cst.tokens[index].kind != TokenKind::kEnd) {
    return std::nullopt;
  }

  std::string out;
  out += "operation_id=ddl.create_table\n";
  out += "opcode=SBLR_DDL_CREATE_TABLE\n";
  out += "sblr_operation_family=";
  out += operation_family.empty() ? "sblr.query.multimodel_or_ddl.v3" : operation_family;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=sbsql.parser.live_route.ddl.create_table\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  AppendRouteTextOperand(&out, "target_object_kind", "table");
  AppendRouteTextOperand(&out, "table_name", table_name);
  if (!schema_parent_path.empty()) {
    AppendRouteTextOperand(&out, "schema_parent_path", schema_parent_path);
  }
  AppendRouteTextOperand(&out, "column_count", std::to_string(columns.size()));
  for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
    const auto& column = columns[column_index];
    const std::string prefix = "column_" + std::to_string(column_index) + "_";
    std::string descriptor = "type=" + column.canonical_type;
    if (!column.raw_type.empty() && column.raw_type != column.canonical_type) {
      AppendDescriptorFlag(&descriptor, "source_type", column.raw_type);
    }
    AppendDescriptorFlag(&descriptor, "nullable", column.nullable ? "true" : "false");
    if (column.primary_key) {
      AppendDescriptorFlag(&descriptor, "primary_key", "true");
      AppendDescriptorFlag(&descriptor, "unique", "true");
    } else if (column.unique) {
      AppendDescriptorFlag(&descriptor, "unique", "true");
    }
    if (!column.default_expression.empty()) {
      AppendDescriptorFlag(&descriptor, "default", column.default_expression);
    }
    AppendRouteTextOperand(&out, prefix + "name", column.name);
    AppendRouteTextOperand(&out, prefix + "type", column.canonical_type);
    AppendRouteTextOperand(&out, prefix + "descriptor", descriptor);
    AppendRouteTextOperand(&out, prefix + "nullable", column.nullable ? "true" : "false");
    if (!column.default_expression.empty()) {
      AppendRouteTextOperand(&out, prefix + "default", column.default_expression);
    }
  }
  if (temporary) {
    AppendRouteTextOperand(&out, "temporary", "true");
    AppendRouteTextOperand(&out, "temporary_scope", temporary_scope);
    AppendRouteTextOperand(&out, "on_commit", on_commit_action);
  }
  return out;
}

struct CreatedDdlName {
  std::string presented_name;
  std::vector<std::string> object_classes;
  bool quoted{false};
};

void PushCreatedDdlClassAliases(std::string_view object_kind,
                                std::vector<std::string>* classes) {
  if (classes == nullptr) return;
  const auto kind = ToUpperAscii(object_kind);
  if (kind == "TABLE") {
    classes->push_back("table");
    classes->push_back("relation");
  } else if (kind == "VIEW") {
    classes->push_back("view");
    classes->push_back("relation");
  } else if (kind == "SCHEMA") {
    classes->push_back("schema");
  } else if (kind == "DOMAIN") {
    classes->push_back("domain");
  } else if (kind == "INDEX") {
    classes->push_back("index");
  } else if (kind == "ROLE") {
    classes->push_back("role");
    classes->push_back("security_role");
    classes->push_back("principal");
  } else if (kind == "GROUP") {
    classes->push_back("group");
    classes->push_back("security_group");
    classes->push_back("principal");
  } else if (kind == "USER" || kind == "PRINCIPAL") {
    classes->push_back("principal");
    classes->push_back("user");
  } else if (kind == "POLICY") {
    classes->push_back("policy");
    classes->push_back("security_policy");
  } else if (kind == "MASK") {
    classes->push_back("mask");
    classes->push_back("security_policy");
  } else if (kind == "RLS") {
    classes->push_back("rls");
    classes->push_back("security_policy");
  } else if (kind == "PROCEDURE") {
    classes->push_back("procedure");
    classes->push_back("routine");
  } else if (kind == "FUNCTION") {
    classes->push_back("function");
    classes->push_back("routine");
  } else if (kind == "TRIGGER") {
    classes->push_back("trigger");
  } else if (kind == "FILESPACE") {
    classes->push_back("filespace");
  } else if (!object_kind.empty()) {
    classes->push_back(std::string(object_kind));
  }
  std::sort(classes->begin(), classes->end());
  classes->erase(std::unique(classes->begin(), classes->end()), classes->end());
}

std::optional<CreatedDdlName> ExtractCreatedDdlNameFromCst(
    const CstDocument& cst,
    std::string_view object_kind) {
  std::size_t index = 0;
  if (!ConsumeRouteKeyword(cst, &index, "CREATE")) return std::nullopt;
  bool temporary = false;
  std::string temporary_scope;
  (void)ConsumeOptionalTemporaryTablePrefix(cst, &index, &temporary, &temporary_scope);
  (void)temporary;
  (void)temporary_scope;
  const auto kind = ToUpperAscii(object_kind);
  if (kind == "TABLE" || kind == "RELATION") {
    if (!ConsumeRouteKeyword(cst, &index, "TABLE")) return std::nullopt;
  } else if (kind == "SCHEMA") {
    if (!ConsumeRouteKeyword(cst, &index, "SCHEMA")) return std::nullopt;
  } else if (kind == "VIEW") {
    if (!ConsumeRouteKeyword(cst, &index, "VIEW")) return std::nullopt;
  } else if (kind == "DOMAIN") {
    if (!ConsumeRouteKeyword(cst, &index, "DOMAIN")) return std::nullopt;
  } else if (kind == "INDEX") {
    if (!ConsumeRouteKeyword(cst, &index, "INDEX")) return std::nullopt;
  } else if (kind == "ROLE") {
    if (!ConsumeRouteKeyword(cst, &index, "ROLE")) return std::nullopt;
  } else if (kind == "GROUP") {
    if (!ConsumeRouteKeyword(cst, &index, "GROUP")) return std::nullopt;
  } else if (kind == "USER" || kind == "PRINCIPAL") {
    if (ConsumeRouteKeyword(cst, &index, "USER")) {
    } else if (!ConsumeRouteKeyword(cst, &index, "PRINCIPAL")) {
      return std::nullopt;
    }
  } else if (kind == "POLICY") {
    if (!ConsumeRouteKeyword(cst, &index, "POLICY")) return std::nullopt;
  } else if (kind == "MASK") {
    if (!ConsumeRouteKeyword(cst, &index, "MASK")) return std::nullopt;
  } else if (kind == "RLS") {
    if (!ConsumeRouteKeyword(cst, &index, "RLS")) return std::nullopt;
  } else if (kind == "PROCEDURE") {
    if (!ConsumeRouteKeyword(cst, &index, "PROCEDURE")) return std::nullopt;
  } else if (kind == "FUNCTION") {
    if (!ConsumeRouteKeyword(cst, &index, "FUNCTION")) return std::nullopt;
  } else if (kind == "TRIGGER") {
    if (!ConsumeRouteKeyword(cst, &index, "TRIGGER")) return std::nullopt;
  } else if (kind == "FILESPACE") {
    if (!ConsumeRouteKeyword(cst, &index, "FILESPACE")) return std::nullopt;
  } else {
    return std::nullopt;
  }
  (void)ConsumeOptionalIfNotExists(cst, &index);
  std::vector<std::string> name_parts;
  CreatedDdlName created;
  if (!ConsumeRouteQualifiedNameParts(cst, &index, &name_parts, &created.quoted)) {
    return std::nullopt;
  }
  created.presented_name = JoinRouteNameParts(name_parts, 0, name_parts.size());
  PushCreatedDdlClassAliases(object_kind, &created.object_classes);
  if (created.presented_name.empty() || created.object_classes.empty()) return std::nullopt;
  return created;
}

bool EnforceCstResourceBudget(const CstDocument& cst,
                              const ParserResourceBudget& budget,
                              ParserMetrics* metrics,
                              MessageVectorSet* messages) {
  const auto before = messages->diagnostics.size();
  std::uint64_t token_count = 0;
  std::uint64_t parameter_count = 0;
  std::uint64_t current_depth = 0;
  std::uint64_t max_depth = 0;
  bool emitted_token_count = false;
  bool emitted_identifier = false;
  bool emitted_literal = false;

  for (const auto& token : cst.tokens) {
    if (token.kind == TokenKind::kEnd) continue;
    ++token_count;
    if (!emitted_token_count && token_count > budget.max_token_count) {
      AddResourceDiagnostic(
          messages,
          "SBSQL.RESOURCE.TOKEN_COUNT_EXCEEDED",
          "statement token count exceeds parser resource budget",
          {{"token_count", std::to_string(token_count)},
           {"max_token_count", std::to_string(budget.max_token_count)}});
      emitted_token_count = true;
    }
    if (!emitted_identifier && token.kind == TokenKind::kIdentifier &&
        token.raw_text.size() > budget.max_identifier_bytes) {
      AddResourceDiagnostic(
          messages,
          "SBSQL.RESOURCE.IDENTIFIER_TOO_LARGE",
          "identifier exceeds parser resource budget",
          {{"identifier_bytes", std::to_string(token.raw_text.size())},
           {"max_identifier_bytes", std::to_string(budget.max_identifier_bytes)},
           {"line", std::to_string(token.line)},
           {"column", std::to_string(token.column)}});
      emitted_identifier = true;
    }
    if (!emitted_literal && IsLiteralKind(token.kind) &&
        token.raw_text.size() > budget.max_literal_bytes) {
      AddResourceDiagnostic(
          messages,
          "SBSQL.RESOURCE.LITERAL_TOO_LARGE",
          "literal exceeds parser resource budget",
          {{"literal_bytes", std::to_string(token.raw_text.size())},
           {"max_literal_bytes", std::to_string(budget.max_literal_bytes)},
           {"line", std::to_string(token.line)},
           {"column", std::to_string(token.column)}});
      emitted_literal = true;
    }
    if (token.kind == TokenKind::kParameter) ++parameter_count;
    if (token.kind == TokenKind::kSymbol && token.text == "(") {
      ++current_depth;
      max_depth = std::max(max_depth, current_depth);
    } else if (token.kind == TokenKind::kSymbol && token.text == ")" &&
               current_depth > 0) {
      --current_depth;
    }
  }

  if (parameter_count > budget.max_parameter_count) {
    AddResourceDiagnostic(
        messages,
        "SBSQL.RESOURCE.PARAMETER_COUNT_EXCEEDED",
        "statement parameter count exceeds parser resource budget",
        {{"parameter_count", std::to_string(parameter_count)},
         {"max_parameter_count", std::to_string(budget.max_parameter_count)}});
  }
  if (max_depth > budget.max_ast_depth) {
    AddResourceDiagnostic(
        messages,
        "SBSQL.RESOURCE.AST_DEPTH_EXCEEDED",
        "expression or statement nesting exceeds parser resource budget",
        {{"ast_depth", std::to_string(max_depth)},
         {"max_ast_depth", std::to_string(budget.max_ast_depth)}});
  }

  if (messages->diagnostics.size() != before && metrics != nullptr) {
    metrics->Increment("sys.metrics.parsers.resource.limit_exceeded_total",
                       messages->diagnostics.size() - before);
    metrics->SetGauge("sys.metrics.parsers.resource.last_token_count",
                      static_cast<double>(token_count));
  }
  return messages->diagnostics.size() == before;
}

void InjectStreamRowCount(std::string* payload, std::uint64_t stream_row_count) {
  if (payload == nullptr || stream_row_count == 0) return;
  const auto close = payload->rfind('}');
  if (close == std::string::npos) return;
  payload->insert(close, ",\"stream_row_count\":" + std::to_string(stream_row_count));
}

void InjectCursorFetchWindow(std::string* payload,
                             std::uint64_t max_chunk_rows,
                             std::uint64_t max_chunk_bytes) {
  if (payload == nullptr || max_chunk_rows == 0) return;
  const std::string json_fields =
      ",\"cursor_max_chunk_rows\":" + std::to_string(max_chunk_rows) +
      ",\"cursor_max_chunk_bytes\":" + std::to_string(max_chunk_bytes);
  const auto close = payload->rfind('}');
  if (close != std::string::npos) {
    payload->insert(close, json_fields);
    return;
  }
  if (!payload->empty() && payload->back() != '\n') payload->push_back('\n');
  *payload += "cursor_max_chunk_rows=" + std::to_string(max_chunk_rows) + "\n";
  *payload += "cursor_max_chunk_bytes=" + std::to_string(max_chunk_bytes) + "\n";
}

std::string StripStatementTerminator(std::string sql) {
  sql = TrimAscii(sql);
  while (!sql.empty() && sql.back() == ';') {
    sql.pop_back();
    sql = TrimAscii(sql);
  }
  return sql;
}

std::optional<ServerManagementCommand> ParseServerManagementCommand(std::string_view sql) {
  const auto normalized = ToUpperAscii(StripStatementTerminator(std::string(sql)));
  ServerManagementCommand command;
  command.audit_reason = "sbsql_sbwp_tls_database_lifecycle_route";
  if (normalized == "VERIFY DATABASE") {
    command.operation_key = "verify_database";
    command.operation_id = "lifecycle.verify_database";
  } else if (normalized == "INSPECT DATABASE" || normalized == "DIAGNOSE DATABASE") {
    command.operation_key = normalized.starts_with("INSPECT") ? "inspect_database" : "diagnose_database";
    command.operation_id = "lifecycle.inspect_database";
  } else if (normalized == "SHOW SERVER LIFECYCLE") {
    command.operation_key = "show_server_lifecycle";
    command.operation_id = "lifecycle.show_server_lifecycle";
  } else if (normalized == "SHOW DATABASE SHUTDOWN STATE") {
    command.operation_key = "show_database_shutdown_state";
    command.operation_id = "lifecycle.show_database_shutdown_state";
  } else if (normalized == "SHUTDOWN DATABASE") {
    command.operation_key = "shutdown_database";
    command.operation_id = "lifecycle.shutdown_database";
    command.mode =
        "acknowledgements_satisfied:true;"
        "drain_complete:true";
  } else if (normalized == "SHUTDOWN DATABASE FORCE" || normalized == "FORCE SHUTDOWN DATABASE") {
    command.operation_key = "shutdown_database_force";
    command.operation_id = "lifecycle.shutdown_force";
    command.mode =
        "shutdown_mode:force;"
        "acknowledgements_satisfied:true;"
        "force_termination_policy_uuid:019e0ec6-d13c-7000-8000-000000000013;"
        "recovery_evidence_preserved:true";
  } else if (normalized == "DROP DATABASE" || normalized == "DROP DATABASE LOGICAL" ||
             normalized == "DROP DATABASE LOGICAL PRESERVE") {
    command.operation_key = "drop_database";
    command.operation_id = "lifecycle.drop_database";
    command.mode = "drop_mode:logical";
  } else {
    return std::nullopt;
  }
  return command;
}

std::string ChunkedParserJsonEnvelope(std::size_t parameter_bytes,
                                      std::uint64_t result_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b4.chunked_payload\",";
  out += "\"sblr_operation_key\":\"op.fspe010b4.chunked_payload\",";
  out += "\"result_shape\":\"rs.fspe010b4.large_result.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b4.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b4.v1\",";
  out += "\"trace_key\":\"FSPE-010B4\",";
  out += "\"stream_row_count\":";
  out += std::to_string(result_rows);
  out += ",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[],\"descriptor_refs\":[],\"policy_refs\":[],";
  out += "\"parameter_packet\":\"";
  out.append(parameter_bytes, 'x');
  out += "\"}";
  return out;
}

std::string CopyStreamParserJsonEnvelope(std::string_view kind,
                                         std::uint64_t total_rows,
                                         std::uint64_t reject_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.dml.operation.v3\",";
  out += "\"surface_key\":\"fspe010b5.copy_streaming\",";
  out += "\"sblr_operation_key\":\"op.fspe010b5.copy_streaming\",";
  out += "\"result_shape\":\"rs.fspe010b5.copy_stream.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b5.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b5.v1\",";
  out += "\"trace_key\":\"FSPE-010B5\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000055\"],";
  out += "\"descriptor_refs\":[\"descriptor.copy.target.uuid\"],";
  out += "\"policy_refs\":[\"policy.copy.reject_row\"],";
  out += "\"copy_stream_kind\":\"";
  out += EscapeJson(kind);
  out += "\",\"copy_total_rows\":";
  out += std::to_string(total_rows);
  out += ",\"copy_reject_rows\":";
  out += std::to_string(reject_rows);
  out += "}";
  return out;
}

std::string MultiResultParserJsonEnvelope(std::uint64_t result_sets) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b6.multi_result\",";
  out += "\"sblr_operation_key\":\"op.fspe010b6.multi_result\",";
  out += "\"result_shape\":\"rs.fspe010b6.multi_result.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b6.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b6.v1\",";
  out += "\"trace_key\":\"FSPE-010B6\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000066\"],";
  out += "\"descriptor_refs\":[\"descriptor.multi_result.sequence\"],";
  out += "\"policy_refs\":[\"policy.multi_result.forward_only\"],";
  out += "\"multi_result_count\":";
  out += std::to_string(result_sets);
  out += "}";
  return out;
}

std::string WarningStreamParserJsonEnvelope(std::uint64_t partial_rows,
                                            std::uint64_t warnings) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b7.warning_partial\",";
  out += "\"sblr_operation_key\":\"op.fspe010b7.warning_partial\",";
  out += "\"result_shape\":\"rs.fspe010b7.warning_partial.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b7.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b7.v1\",";
  out += "\"trace_key\":\"FSPE-010B7\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000077\"],";
  out += "\"descriptor_refs\":[\"descriptor.partial_result.warning_chain\"],";
  out += "\"policy_refs\":[\"policy.partial_result.forward_only\"],";
  out += "\"partial_result_rows\":";
  out += std::to_string(partial_rows);
  out += ",\"warning_chain_count\":";
  out += std::to_string(warnings);
  out += "}";
  return out;
}

std::string FinalityStreamParserJsonEnvelope(std::string_view mode,
                                             std::uint64_t rows,
                                             std::uint64_t after_fetches) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b8.stream_finality\",";
  out += "\"sblr_operation_key\":\"op.fspe010b8.stream_finality\",";
  out += "\"result_shape\":\"rs.fspe010b8.stream_finality.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b8.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b8.v1\",";
  out += "\"trace_key\":\"FSPE-010B8\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000088\"],";
  out += "\"descriptor_refs\":[\"descriptor.stream.finality\"],";
  out += "\"policy_refs\":[\"policy.stream.finality.forward_only\"],";
  out += "\"stream_row_count\":";
  out += std::to_string(rows);
  out += ",\"stream_finality_mode\":\"";
  out += EscapeJson(mode);
  out += "\",\"stream_finality_after_fetches\":";
  out += std::to_string(after_fetches);
  out += "}";
  return out;
}

std::string RoutineCursorArgumentJsonEnvelope(std::string_view cursor_uuid) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.routine.execute.v3\",";
  out += "\"operation_id\":\"routine.execute_cursor_argument\",";
  out += "\"surface_key\":\"routine_cursor_argument.live_route\",";
  out += "\"sblr_operation_key\":\"routine_cursor_argument.live_route\",";
  out += "\"result_shape\":\"routine_cursor_argument.rows.v1\",";
  out += "\"diagnostic_shape\":\"routine_cursor_argument.diag.v1\",";
  out += "\"resource_contract\":\"routine_cursor_argument.resource.v1\",";
  out += "\"trace_key\":\"ROUTINE-CURSOR-FULL-ROUTE\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"routine_cursor_uuid\":\"";
  out += EscapeJson(cursor_uuid);
  out += "\",\"routine_context_kind\":\"procedure\",";
  out += "\"routine_cursor_action\":\"fetch\",";
  out += "\"routine_cursor_borrow_policy\":\"borrowed_read\",";
  out += "\"routine_cursor_argument_binding\":\"descriptor.cursor_handle.session_registry\",";
  out += "\"routine_cursor_descriptor\":\"rowshape:int64:value\",";
  out += "\"routine_expected_cursor_descriptor\":\"rowshape:int64:value\",";
  out += "\"routine_security_recheck\":\"passed\",";
  out += "\"routine_protected_material_policy\":\"rechecked\",";
  out += "\"routine_deterministic_context\":false,";
  out += "\"routine_cursor_fetch_max_rows\":1,";
  out += "\"resolved_object_uuids\":[],";
  out += "\"descriptor_refs\":[\"sys.server.cursor_descriptor\",";
  out += "\"sys.routine.cursor_parameter_descriptor\"],";
  out += "\"policy_refs\":[\"routine_cursor_session_registry_policy\",";
  out += "\"routine_cursor_security_recheck_policy\"]}";
  return out;
}

std::string EngineShowVersionOperationEnvelope() {
  std::string out;
  out += "operation_id=observability.show_version\n";
  out += "opcode=SBLR_OBSERVABILITY_SHOW_VERSION\n";
  out += "sblr_operation_family=sblr.observability.inspect.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=FSPE-010B3\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  const auto binary = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                          .append_bytes(reinterpret_cast<const std::uint8_t*>(out.data()), out.size())
                          .encode();
  return std::string(reinterpret_cast<const char*>(binary.data()), binary.size());
}

std::string ExactOperationEnvelope(std::string_view operation_id,
                                   std::string_view opcode,
                                   std::string_view family,
                                   bool requires_transaction_context,
                                   std::string_view trace_key) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "opcode=";
  out += opcode;
  out += "\n";
  out += "sblr_operation_family=";
  out += family;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=";
  out += trace_key;
  out += "\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=";
  out += requires_transaction_context ? "true\n" : "false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

void InjectAutocommitEmulation(std::string* payload) {
  if (payload == nullptr || payload->empty() ||
      payload->find("autocommit_emulation=") != std::string::npos ||
      payload->find("\"autocommit_emulation\"") != std::string::npos) {
    return;
  }
  if (!payload->empty() && payload->back() != '\n') payload->push_back('\n');
  payload->append("autocommit_emulation=true\n");
}

std::string TransactionBeginOperationEnvelope() {
  return ExactOperationEnvelope("transaction.begin",
                                "SBLR_TRANSACTION_BEGIN",
                                "sblr.transaction.control.v3",
                                false,
                                "SBSFC-021-copy-stream-full-route-begin");
}

std::string TransactionCommitOperationEnvelope() {
  return ExactOperationEnvelope("transaction.commit",
                                "SBLR_TRANSACTION_COMMIT",
                                "sblr.transaction.control.v3",
                                true,
                                "SBSFC-021-copy-stream-full-route-commit");
}

std::string TransactionRollbackOperationEnvelope() {
  return ExactOperationEnvelope("transaction.rollback",
                                "SBLR_TRANSACTION_ROLLBACK",
                                "sblr.transaction.control.v3",
                                true,
                                "SBSFC-021-copy-stream-full-route-rollback");
}

std::string EngineBackedCopyStreamImportEnvelope(std::string_view target_object_uuid) {
  const auto good_row_uuid = NewRowUuid();
  const auto reject_row_uuid = NewRowUuid();
  std::string out = ExactOperationEnvelope("dml.execute_import_rows",
                                           "SBLR_DML_EXECUTE_IMPORT_ROWS",
                                           "sblr.dml.operation.v3",
                                           true,
                                           "SBSFC-021-copy-stream-full-route-import");
  out += "copy_stream_kind=copy_import\n";
  out += "target_object_uuid=";
  out += target_object_uuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "source_kind=csv_stream\n";
  out += "source_fingerprint=sbsfc021-copy-stream-full-route\n";
  out += "source_position=row:0\n";
  out += "format_family=csv\n";
  out += "encoding=utf8\n";
  out += "line_ending=lf\n";
  out += "delimiter=,\n";
  out += "quote=\"\n";
  out += "escape=\"\n";
  out += "header_policy=absent\n";
  out += "estimated_row_count=2\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  out += "reject_mode=reject_row\n";
  out += "reject_limit_rows=10\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "operand=row_field\t";
  out += good_row_uuid;
  out += "|id\t8\n";
  out += "operand=row_field\t";
  out += good_row_uuid;
  out += "|payload\tstream-valid\n";
  out += "operand=row_field\t";
  out += reject_row_uuid;
  out += "|id\t6\n";
  out += "operand=row_field\t";
  out += reject_row_uuid;
  out += "|payload\tstream-duplicate\n";
  return out;
}

std::string Rcp073ProofUuid(const std::uint32_t value) {
  std::string uuid_text = "00000000-0000-7000-8000-000000000000";
  const auto suffix = std::to_string(value);
  uuid_text.replace(uuid_text.size() - suffix.size(), suffix.size(), suffix);
  return uuid_text;
}

ParserStatementContext Rcp073ProofStatementContext() {
  ParserStatementContext statement;
  statement.acquired = true;
  statement.statement_uuid = Rcp073ProofUuid(1);
  statement.transaction = {17, Rcp073ProofUuid(2)};
  statement.statement_snapshot_uuid = Rcp073ProofUuid(3);
  statement.statement_metadata_snapshot_uuid = Rcp073ProofUuid(4);
  statement.catalog_epoch_uuid = Rcp073ProofUuid(5);
  statement.security_context_uuid = Rcp073ProofUuid(6);
  statement.snapshot_visible_through_local_transaction_id = 16;
  statement.bound_ast_uuid = Rcp073ProofUuid(7);
  statement.count_function_uuid = Rcp073ProofUuid(8);
  statement.sum_function_uuid = Rcp073ProofUuid(9);
  statement.avg_function_uuid = Rcp073ProofUuid(10);
  statement.min_function_uuid = Rcp073ProofUuid(11);
  statement.max_function_uuid = Rcp073ProofUuid(12);
  for (std::uint32_t ordinal = 0; ordinal < 43; ++ordinal) {
    statement.aggregate_function_profiles.push_back(
        {1, "sb.aggregate.rcp073." + std::to_string(ordinal),
         Rcp073ProofUuid(100 + ordinal), true});
  }
  for (std::uint8_t kind = 1; kind <= 10; ++kind) {
    for (std::uint16_t slot = 0; slot < 32; ++slot) {
      ParserStatementContext::DescriptorProfile profile;
      profile.profile_kind = kind;
      profile.slot = slot;
      profile.descriptor_uuid =
          Rcp073ProofUuid(1000 + static_cast<std::uint32_t>(kind) * 100 + slot);
      // Public statement profiles pair non-null/nullable kinds over one
      // canonical type UUID: 1/2 numeric, 3/4 text, 5/6 boolean, 7/8 JSON,
      // and 9/10 text-list.
      profile.type_uuid = Rcp073ProofUuid(3000 + (kind + 1) / 2);
      profile.nullable = (kind % 2) == 0;
      if (kind == 3 || kind == 4 || kind == 9 || kind == 10) {
        profile.collation_uuid = Rcp073ProofUuid(3200 + kind);
        profile.width = 512;
      }
      statement.descriptor_profiles.push_back(std::move(profile));
    }
  }
  return statement;
}

ResolvedObjectReferenceSeed Rcp073ProofSeed(const ObjectReference& ref) {
  ResolvedObjectReferenceSeed seed;
  seed.ref = ref;
  seed.resolved.resolved = true;
  seed.resolved.object_uuid = Rcp073ProofUuid(4001);
  seed.resolved.canonical_name = "app.Collection";
  seed.resolved.object_class = "relation";
  seed.resolved.catalog_epoch = 41;
  seed.resolved.security_epoch = 42;
  auto& projection = seed.resolved.relation_descriptor;
  projection.present = true;
  projection.descriptor_uuid = Rcp073ProofUuid(4002);
  projection.relation_uuid = seed.resolved.object_uuid;
  projection.schema_uuid = Rcp073ProofUuid(4003);
  projection.descriptor_generation = 43;
  projection.validated_resource_epoch = 44;
  const auto add_column = [&](const std::uint32_t ordinal,
                              const std::string& name,
                              const bool nullable,
                              const std::uint32_t character_length) {
    ipc::PublicRelationColumnDescriptor column;
    column.column_uuid = Rcp073ProofUuid(4100 + ordinal);
    column.ordinal = ordinal;
    column.canonical_name_key = name;
    column.type_descriptor_uuid = Rcp073ProofUuid(4200 + ordinal);
    const auto type_uuid = Rcp073ProofUuid(4300 + ordinal);
    column.nullable = nullable;
    column.character_length = character_length;
    column.encoded_type_descriptor =
        "type_uuid=" + type_uuid + ";nullability=" +
        (nullable ? "nullable" : "non_null");
    if (character_length != 0) {
      column.collation_uuid = Rcp073ProofUuid(4400 + ordinal);
      column.encoded_type_descriptor +=
          ";collation_uuid=" + column.collation_uuid +
          ";width=" + std::to_string(character_length);
    }
    projection.columns.push_back(std::move(column));
  };
  add_column(0, "row_uuid", false, 0);
  add_column(1, "join_key", true, 0);
  add_column(2, "payload", true, 512);
  return seed;
}

ResolvedObjectReferenceSeed Rcp074GraphProofSeed(const ObjectReference& ref) {
  auto seed = Rcp073ProofSeed(ref);
  auto& projection = seed.resolved.relation_descriptor;
  projection.columns.clear();
  static constexpr std::array<std::string_view, 9> kNames{
      "vertex_uuid",       "edge_uuid",       "path_uuid",
      "vertex_labels",     "vertex_properties", "edge_properties",
      "direction",         "depth",           "cycle_policy"};
  static constexpr std::array<std::string_view, 9> kTypes{
      "uuid", "uuid", "uuid", "text", "text", "text", "text",
      "uint64", "text"};
  static constexpr std::array<bool, 9> kNullable{
      false, true, false, false, false, false, false, false, false};
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    ipc::PublicRelationColumnDescriptor column;
    column.column_uuid = Rcp073ProofUuid(4500 + ordinal);
    column.ordinal = ordinal;
    column.canonical_name_key = std::string(kNames[ordinal]);
    column.type_descriptor_uuid = Rcp073ProofUuid(4600 + ordinal);
    column.type_descriptor_kind = "canonical_type_descriptor";
    column.canonical_type_name = std::string(kTypes[ordinal]);
    column.nullable = kNullable[ordinal];
    std::string type_uuid;
    if (manifest.ok()) {
      const auto type_row =
          scratchbird::core::datatypes::LookupDatatypeCatalogRow(
              manifest.manifest,
              scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
                  std::string(kTypes[ordinal])));
      if (type_row.ok() && type_row.manifest.descriptor_rows.size() == 1 &&
          type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
        type_uuid = scratchbird::core::uuid::UuidToString(
            type_row.manifest.descriptor_rows.front().descriptor_uuid.value);
      }
    }
    column.encoded_type_descriptor =
        "type_uuid=" + type_uuid +
        ";nullability=" +
        (kNullable[ordinal] ? "nullable" : "non_null");
    projection.columns.push_back(std::move(column));
  }
  return seed;
}

bool Rcp073BuildAndBind(const AstDocument& ast,
                       const std::vector<ResolvedObjectReferenceSeed>& seeds) {
  MessageVectorSet messages;
  const auto context = BuildEngineProjectedNativeBindingContext(
      ast.native_relational, Rcp073ProofStatementContext(), seeds, &messages);
  if (!context.has_value() || messages.has_errors()) return false;
  const auto bound = BindNativeRelationalAst(ast.native_relational, *context);
  return bound.bound && !bound.messages.has_errors();
}

std::uint64_t Rcp073DocumentFrontdoorProofMaskImpl() {
  std::uint64_t mask = 0;
  const auto mixed_ast = BuildAst(BuildCst(
      "SELECT * FROM DOCUMENT_SOURCE(app.\"Collection\") AS d;"));
  const auto mixed_refs = ExtractObjectReferences(
      BuildCst("SELECT * FROM DOCUMENT_SOURCE(app.\"Collection\") AS d;"),
      mixed_ast);
  const auto escaped_cst = BuildCst(
      "SELECT * FROM DOCUMENT_SOURCE(app.\"Col\"\"lection\") AS d;");
  const auto escaped_ast = BuildAst(escaped_cst);
  const auto escaped_refs = ExtractObjectReferences(escaped_cst, escaped_ast);
  if (mixed_ast.requires_name_resolution && !mixed_ast.produces_sblr &&
      mixed_refs.size() == 1 &&
      mixed_refs.front().presented_name == "app.\"Collection\"" &&
      mixed_refs.front().object_class == "document_collection" &&
      !mixed_refs.front().quoted && !mixed_refs.front().create_reservation &&
      escaped_refs.size() == 1 &&
      escaped_refs.front().presented_name == "app.\"Col\"\"lection\"" &&
      !escaped_refs.front().quoted) {
    mask |= 1ull << 0;
  }
  if (mixed_refs.size() == 1 &&
      Rcp073BuildAndBind(mixed_ast, {Rcp073ProofSeed(mixed_refs.front())})) {
    mask |= 1ull << 1;
  }

  const auto ordinary_cst = BuildCst(
      "SELECT d.payload, d.join_key + 1 "
      "FROM DOCUMENT_SOURCE(app.document_fixture) AS d "
      "WHERE DOCUMENT_PATH(d, '$.join_key') >= 1 + 0;");
  const auto ordinary_ast = BuildAst(ordinary_cst);
  const auto ordinary_refs = ExtractObjectReferences(ordinary_cst, ordinary_ast);
  if (ordinary_refs.size() == 1 &&
      Rcp073BuildAndBind(ordinary_ast,
                        {Rcp073ProofSeed(ordinary_refs.front())})) {
    mask |= 1ull << 2;
  }

  const auto unnest_cst = BuildCst(
      "SELECT * FROM DOCUMENT_UNNEST(DOCUMENT '{\"items\":[3,1,2]}', '$.items[*]') "
      "AS item;");
  const auto unnest_ast = BuildAst(unnest_cst);
  const auto unnest_refs = ExtractObjectReferences(unnest_cst, unnest_ast);
  if (!unnest_ast.requires_name_resolution && unnest_ast.produces_sblr &&
      unnest_refs.empty() && Rcp073BuildAndBind(unnest_ast, {})) {
    mask |= 1ull << 3;
  }

  if (ordinary_refs.size() != 1) return mask;
  const auto good_seed = Rcp073ProofSeed(ordinary_refs.front());
  MessageVectorSet messages;
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(), {},
           &messages)
           .has_value()) {
    mask |= 1ull << 4;
  }
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {good_seed, good_seed}, &messages)
           .has_value()) {
    mask |= 1ull << 5;
  }
  auto mutation = good_seed;
  mutation.resolved.resolved = false;
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 6;
  }
  mutation = good_seed;
  mutation.resolved.catalog_epoch = 0;
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 7;
  }
  mutation = good_seed;
  mutation.resolved.relation_descriptor.present = false;
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 8;
  }
  mutation = good_seed;
  mutation.resolved.relation_descriptor.relation_uuid = Rcp073ProofUuid(4999);
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 9;
  }
  mutation = good_seed;
  mutation.resolved.relation_descriptor.columns.front().type_descriptor_uuid =
      "not-a-uuid";
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 10;
  }
  mutation = good_seed;
  mutation.resolved.relation_descriptor.columns.front().encoded_type_descriptor =
      "type_uuid=not-a-uuid;nullability=non_null";
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 11;
  }
  mutation = good_seed;
  mutation.resolved.object_class = "document_collection";
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 12;
  }
  auto mismatched_ast = ordinary_ast;
  mismatched_ast.native_relational.model_object_resolution_requests.front()
      .object_class = "relation";
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           mismatched_ast.native_relational, Rcp073ProofStatementContext(),
           {good_seed}, &messages)
           .has_value()) {
    mask |= 1ull << 13;
  }
  const auto wrong_alias_cst = BuildCst(
      "SELECT other.payload FROM DOCUMENT_SOURCE(app.document_fixture) AS d;");
  const auto wrong_alias_ast = BuildAst(wrong_alias_cst);
  const auto wrong_alias_refs =
      ExtractObjectReferences(wrong_alias_cst, wrong_alias_ast);
  if (wrong_alias_refs.size() == 1) {
    messages = {};
    if (!BuildEngineProjectedNativeBindingContext(
             wrong_alias_ast.native_relational, Rcp073ProofStatementContext(),
             {Rcp073ProofSeed(wrong_alias_refs.front())}, &messages)
             .has_value()) {
      mask |= 1ull << 14;
    }
  }
  auto incomplete_statement = Rcp073ProofStatementContext();
  incomplete_statement.statement_metadata_snapshot_uuid.clear();
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           ordinary_ast.native_relational, incomplete_statement, {good_seed},
           &messages)
           .has_value()) {
    mask |= 1ull << 15;
  }
  const auto uppercase_cst = BuildCst(
      "SELECT D . PAYLOAD, D . JOIN_KEY + 1 "
      "FROM DOCUMENT_SOURCE(app.document_fixture) AS d "
      "WHERE DOCUMENT_PATH(D, '$.join_key') >= 1;");
  const auto uppercase_ast = BuildAst(uppercase_cst);
  const auto uppercase_refs = ExtractObjectReferences(uppercase_cst,
                                                       uppercase_ast);
  if (uppercase_refs.size() == 1 &&
      Rcp073BuildAndBind(uppercase_ast,
                        {Rcp073ProofSeed(uppercase_refs.front())})) {
    mask |= 1ull << 16;
  }
  const auto quoted_cst = BuildCst(
      "SELECT \"D\".\"payload\" "
      "FROM DOCUMENT_SOURCE(app.document_fixture) AS \"D\" "
      "WHERE DOCUMENT_PATH(\"D\", '$.join_key') >= 1;");
  const auto quoted_ast = BuildAst(quoted_cst);
  const auto quoted_refs = ExtractObjectReferences(quoted_cst, quoted_ast);
  if (quoted_refs.size() == 1 &&
      Rcp073BuildAndBind(quoted_ast,
                        {Rcp073ProofSeed(quoted_refs.front())})) {
    mask |= 1ull << 17;
  }
  const auto wrong_quoted_alias_cst = BuildCst(
      "SELECT \"d\".\"payload\" "
      "FROM DOCUMENT_SOURCE(app.document_fixture) AS \"D\" "
      "WHERE DOCUMENT_PATH(\"D\", '$.join_key') >= 1;");
  const auto wrong_quoted_alias_ast = BuildAst(wrong_quoted_alias_cst);
  const auto wrong_quoted_alias_refs = ExtractObjectReferences(
      wrong_quoted_alias_cst, wrong_quoted_alias_ast);
  messages = {};
  if (wrong_quoted_alias_refs.size() == 1 &&
      !BuildEngineProjectedNativeBindingContext(
           wrong_quoted_alias_ast.native_relational,
           Rcp073ProofStatementContext(),
           {Rcp073ProofSeed(wrong_quoted_alias_refs.front())}, &messages)
           .has_value()) {
    mask |= 1ull << 18;
  }
  const auto wrong_quoted_column_cst = BuildCst(
      "SELECT \"D\".\"Payload\" "
      "FROM DOCUMENT_SOURCE(app.document_fixture) AS \"D\" "
      "WHERE DOCUMENT_PATH(\"D\", '$.join_key') >= 1;");
  const auto wrong_quoted_column_ast = BuildAst(wrong_quoted_column_cst);
  const auto wrong_quoted_column_refs = ExtractObjectReferences(
      wrong_quoted_column_cst, wrong_quoted_column_ast);
  messages = {};
  if (wrong_quoted_column_refs.size() == 1 &&
      !BuildEngineProjectedNativeBindingContext(
           wrong_quoted_column_ast.native_relational,
           Rcp073ProofStatementContext(),
           {Rcp073ProofSeed(wrong_quoted_column_refs.front())}, &messages)
           .has_value()) {
    mask |= 1ull << 19;
  }
  const auto missing_alias_ast = BuildAst(BuildCst(
      "SELECT payload FROM DOCUMENT_SOURCE(app.document_fixture) "
      "WHERE DOCUMENT_PATH(d, '$.join_key') >= 1;"));
  if (missing_alias_ast.native_relational.status ==
      NativeRelationalParseStatus::kRefused) {
    mask |= 1ull << 20;
  }
  const auto quoted_trigger_ast = BuildAst(BuildCst(
      "SELECT \"DOCUMENT_PATH\", \"MONGO_PIPELINE\" FROM app.fixture;"));
  const bool model_diagnostic = std::ranges::any_of(
      quoted_trigger_ast.native_relational.messages.diagnostics,
      [](const auto& diagnostic) {
        return diagnostic.code.starts_with("SB_MODEL_");
      });
  const bool document_source_present = std::ranges::any_of(
      quoted_trigger_ast.native_relational.catalog_relation_sources,
      [](const auto& source) {
        return source.source_kind == NativeRelationSourceAstKind::kDocument;
      });
  if (!document_source_present && !model_diagnostic) {
    mask |= 1ull << 21;
  }
  return mask;
}

std::uint64_t Rcp074GraphFrontdoorProofMaskImpl() {
  // QOW-SOURCE-RCP-074-GRAPH-FRONTDOOR-PROOF-V1
  std::uint64_t mask = 0;
  const auto match_cst = BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g "
      "WHERE GRAPH_MATCH(g, 'vertex(label=Person)');");
  const auto match_ast = BuildAst(match_cst);
  const auto match_refs = ExtractObjectReferences(match_cst, match_ast);
  if (match_ast.requires_name_resolution && !match_ast.produces_sblr &&
      match_refs.size() == 1 && match_refs.front().object_class == "graph" &&
      match_refs.front().presented_name == "app.graph_fixture" &&
      !match_refs.front().quoted && !match_refs.front().create_reservation) {
    mask |= 1ull << 0;
  }
  if (match_refs.size() == 1) {
    auto seed = Rcp074GraphProofSeed(match_refs.front());
    seed.resolved.canonical_name = "app.graph_fixture";
    if (Rcp073BuildAndBind(match_ast, {seed})) mask |= 1ull << 1;
  }

  const auto expand_cst = BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g, "
      "GRAPH_EXPAND(g, OUTGOING, 1, 3) AS p;");
  const auto expand_ast = BuildAst(expand_cst);
  const auto expand_refs = ExtractObjectReferences(expand_cst, expand_ast);
  auto graph_seed = expand_refs.size() == 1
                        ? Rcp074GraphProofSeed(expand_refs.front())
                        : ResolvedObjectReferenceSeed{};
  graph_seed.resolved.canonical_name = "app.graph_fixture";
  if (expand_refs.size() == 1 &&
      Rcp073BuildAndBind(expand_ast, {graph_seed})) {
    mask |= 1ull << 2;
  }

  MessageVectorSet messages;
  if (!BuildEngineProjectedNativeBindingContext(
           match_ast.native_relational, Rcp073ProofStatementContext(), {},
           &messages)
           .has_value()) {
    mask |= 1ull << 3;
  }
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           match_ast.native_relational, Rcp073ProofStatementContext(),
           {graph_seed, graph_seed}, &messages)
           .has_value()) {
    mask |= 1ull << 4;
  }
  auto mutation = graph_seed;
  mutation.ref.object_class = "relation";
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           expand_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 5;
  }
  mutation = graph_seed;
  mutation.resolved.relation_descriptor.present = false;
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           expand_ast.native_relational, Rcp073ProofStatementContext(),
           {mutation}, &messages)
           .has_value()) {
    mask |= 1ull << 6;
  }
  auto semantic_substitution = expand_ast;
  semantic_substitution.native_relational.catalog_relation_sources.front()
      .model_operation_id = "DOCUMENT_UNNEST";
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           semantic_substitution.native_relational,
           Rcp073ProofStatementContext(), {graph_seed}, &messages)
           .has_value()) {
    mask |= 1ull << 7;
  }
  const auto donor = BuildAst(
      BuildCst("SELECT * FROM CYPHER_TEXT('MATCH (n) RETURN n');"));
  if (std::ranges::any_of(
          donor.native_relational.messages.diagnostics,
          [](const auto& diagnostic) {
            return diagnostic.code ==
                   "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1";
          })) {
    mask |= 1ull << 8;
  }
  const auto write = BuildAst(BuildCst(
      "UPDATE GRAPH_SOURCE(app.graph_fixture) SET payload = 'x';"));
  if (std::ranges::any_of(
          write.native_relational.messages.diagnostics,
          [](const auto& diagnostic) {
            return diagnostic.code == "SB_MODEL_QUERY_WRITE_REFUSED_V1";
          })) {
    mask |= 1ull << 9;
  }
  const auto unbounded = BuildAst(BuildCst(
      "SELECT * FROM GRAPH_SOURCE(app.graph_fixture) AS g, "
      "GRAPH_EXPAND(g, OUTGOING, 1, UNBOUNDED) AS p;"));
  if (std::ranges::any_of(
          unbounded.native_relational.messages.diagnostics,
          [](const auto& diagnostic) {
            return diagnostic.code ==
                   "SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1";
          })) {
    mask |= 1ull << 10;
  }
  auto allow_cycles = expand_ast;
  allow_cycles.native_relational.catalog_relation_sources.front()
      .model_graph_cycle_policy = "allow_cycles";
  messages = {};
  if (!BuildEngineProjectedNativeBindingContext(
           allow_cycles.native_relational, Rcp073ProofStatementContext(),
           {graph_seed}, &messages)
           .has_value()) {
    mask |= 1ull << 11;
  }
  const auto projection_refused = [&](ResolvedObjectReferenceSeed seed) {
    MessageVectorSet mutation_messages;
    return !BuildEngineProjectedNativeBindingContext(
                expand_ast.native_relational, Rcp073ProofStatementContext(),
                {std::move(seed)}, &mutation_messages)
                .has_value();
  };
  auto descriptor_mutation = Rcp073ProofSeed(expand_refs.front());
  descriptor_mutation.resolved.canonical_name = "app.graph_fixture";
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 12;
  descriptor_mutation = graph_seed;
  std::swap(descriptor_mutation.resolved.relation_descriptor.columns[0],
            descriptor_mutation.resolved.relation_descriptor.columns[1]);
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 13;
  descriptor_mutation = graph_seed;
  descriptor_mutation.resolved.relation_descriptor.columns[0]
      .canonical_type_name = "text";
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 14;
  descriptor_mutation = graph_seed;
  descriptor_mutation.resolved.relation_descriptor.columns[0].nullable = true;
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 15;
  descriptor_mutation = graph_seed;
  descriptor_mutation.resolved.relation_descriptor.columns[1]
      .type_descriptor_uuid =
      descriptor_mutation.resolved.relation_descriptor.columns[0]
          .type_descriptor_uuid;
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 16;
  descriptor_mutation = graph_seed;
  descriptor_mutation.resolved.relation_descriptor.columns[0]
      .encoded_type_descriptor =
      descriptor_mutation.resolved.relation_descriptor.columns[3]
          .encoded_type_descriptor;
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 17;
  descriptor_mutation = graph_seed;
  descriptor_mutation.resolved.relation_descriptor.columns[3].collation_uuid =
      Rcp073ProofUuid(4990);
  descriptor_mutation.resolved.relation_descriptor.columns[3]
      .encoded_type_descriptor += ";collation_uuid=" + Rcp073ProofUuid(4990);
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 18;
  descriptor_mutation = graph_seed;
  descriptor_mutation.resolved.relation_descriptor.columns[0]
      .identity_column = true;
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 19;
  descriptor_mutation = graph_seed;
  descriptor_mutation.resolved.relation_descriptor.columns[0]
      .encoded_type_descriptor += ";unexpected=field";
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 20;
  descriptor_mutation = graph_seed;
  auto& duplicate_encoded = descriptor_mutation.resolved.relation_descriptor
                                .columns[0]
                                .encoded_type_descriptor;
  duplicate_encoded += ";" +
                       duplicate_encoded.substr(
                           0, duplicate_encoded.find(';'));
  if (projection_refused(std::move(descriptor_mutation))) mask |= 1ull << 21;
  return mask;
}

std::uint64_t Rcp076TimeSeriesFrontdoorProofMaskImpl() {
  // QOW-SOURCE-RCP-076-TIME-SERIES-PRE-RESOLUTION-PROOF-V1
  std::uint64_t mask = 0;
  const auto parsed = BuildAst(BuildCst(
      "SELECT TIME_BUCKET(INTERVAL 'PT1M', ts.point_timestamp), "
      "TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  if (!parsed.native_relational.accepted() ||
      parsed.native_relational.catalog_relation_sources.size() != 1) {
    return mask;
  }
  std::size_t resolver_invocations = 0;
  const auto admitted_to_resolver = [&](const auto& ast) {
    if (!ExactTimeSeriesPreResolutionAst(ast)) return false;
    ++resolver_invocations;
    return true;
  };
  if (admitted_to_resolver(parsed.native_relational) &&
      resolver_invocations == 1) {
    mask |= 1ull << 0;
  }
  const auto mutation_refused_without_resolution = [&](auto mutation) {
    auto invalid = parsed.native_relational;
    mutation(&invalid.catalog_relation_sources.front());
    resolver_invocations = 0;
    return !admitted_to_resolver(invalid) && resolver_invocations == 0;
  };
  if (mutation_refused_without_resolution([](auto* source) {
        source->model_bucket_interval_expression_id.reset();
      })) {
    mask |= 1ull << 1;
  }
  if (mutation_refused_without_resolution([](auto* source) {
        source->model_bucket_time_input_expression_id.reset();
      })) {
    mask |= 1ull << 2;
  }
  if (mutation_refused_without_resolution([](auto* source) {
        std::swap(source->model_bucket_interval_expression_id,
                  source->model_bucket_time_input_expression_id);
      })) {
    mask |= 1ull << 3;
  }
  return mask;
}

} // namespace

std::uint64_t Rcp073DocumentFrontdoorProofMaskForTest() {
  return Rcp073DocumentFrontdoorProofMaskImpl();
}

std::uint64_t Rcp074GraphFrontdoorProofMaskForTest() {
  return Rcp074GraphFrontdoorProofMaskImpl();
}

std::uint64_t Rcp076TimeSeriesFrontdoorProofMaskForTest() {
  return Rcp076TimeSeriesFrontdoorProofMaskImpl();
}

SbsqlTestWireSession::SbsqlTestWireSession(ParserConfig config, ParserMetrics* metrics, SblrTemplateCache* cache)
    : config_(std::move(config)), metrics_(metrics), cache_(cache) {
  if (config_.embedded_engine_direct) {
    embedded_client_ = std::make_unique<EmbeddedEngineClient>(config_);
  } else if (!config_.server_endpoint.empty()) {
    config_.require_transaction_routing_v2 = true;
    config_.require_relation_descriptor_projection_v3 = true;
    server_client_ = std::make_unique<SbpsClient>(config_.server_endpoint);
  }
}

SbsqlTestWireSession::~SbsqlTestWireSession() = default;

bool SbsqlTestWireSession::HasExecutionRoute() const {
  return config_.embedded_engine_direct || !config_.server_endpoint.empty();
}

ServerExecutionResult SbsqlTestWireSession::ExecuteSblrOnRoute(
    std::string_view encoded_sblr_envelope,
    bool cursor_requested) {
  return ExecuteSblrOnRouteWithDataPacket(encoded_sblr_envelope, {}, cursor_requested);
}

ServerExecutionResult SbsqlTestWireSession::ExecuteSblrOnRouteWithDataPacket(
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->ExecuteSblrWithDataPacket(
        session_, encoded_sblr_envelope, data_packet, cursor_requested);
  }
  return server_client_->ExecuteSblrWithDataPacket(
      session_, encoded_sblr_envelope, data_packet, cursor_requested);
}

ServerFetchResult SbsqlTestWireSession::FetchCursorOnRoute(std::string_view cursor_uuid,
                                                           std::uint64_t max_rows,
                                                           std::uint64_t max_bytes,
                                                           std::uint32_t fetch_flags) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->FetchCursor(session_, cursor_uuid, max_rows, max_bytes, fetch_flags);
  }
  return server_client_->FetchCursor(
      session_, cursor_uuid, max_rows, max_bytes, fetch_flags);
}

ServerCloseCursorResult SbsqlTestWireSession::CloseCursorOnRoute(std::string_view cursor_uuid) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->CloseCursor(session_, cursor_uuid);
  }
  return server_client_->CloseCursor(session_, cursor_uuid);
}

ServerCloseCursorResult SbsqlTestWireSession::CancelCursorOnRoute(std::string_view cursor_uuid) {
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->CancelCursor(session_, cursor_uuid);
  }
  return server_client_->CancelCursor(session_, cursor_uuid);
}

void SbsqlTestWireSession::ClearNameResolutionCache(
    bool preserve_stable_relation_names) {
  const bool local_had_entries =
      !name_resolution_cache_.empty() || !name_resolution_lru_.empty();
  name_resolution_cache_.clear();
  name_resolution_lru_.clear();
  ClearSharedNameResolutionCache();
  if (!preserve_stable_relation_names) {
    stable_relation_name_resolution_cache_.clear();
    stable_relation_name_resolution_lru_.clear();
  }
  if (metrics_ && local_had_entries) {
    metrics_->Increment("sys.metrics.parsers.name_resolution_cache.clears_total");
  }
}

void SbsqlTestWireSession::RehydrateStableRelationNameResolutionCache() {
  std::vector<StableCachedPublicNameResolution> stable_entries;
  stable_entries.reserve(stable_relation_name_resolution_cache_.size());
  for (const auto& [_, stable] : stable_relation_name_resolution_cache_) {
    if (stable.presented_name.empty() || stable.lookup_object_class.empty() ||
        stable.resolved.object_uuid.empty()) {
      continue;
    }
    stable_entries.push_back(stable);
  }
  for (const auto& stable : stable_entries) {
    StoreNameResolutionCacheEntry(stable.presented_name,
                                  stable.quoted,
                                  stable.lookup_object_class,
                                  stable.resolved.object_uuid,
                                  stable.resolved.canonical_name,
                                  session_.catalog_epoch,
                                  session_.security_policy_epoch,
                                  stable.resolved.object_class);
  }
  if (metrics_ && !stable_entries.empty()) {
    metrics_->Increment("sys.metrics.parsers.name_resolution_cache.stable_rehydrates_total");
  }
}

void SbsqlTestWireSession::StoreNameResolutionCacheEntry(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    std::string_view object_uuid,
    std::string_view canonical_name,
    std::uint64_t catalog_epoch,
    std::uint64_t security_epoch,
    std::string_view resolved_object_class) {
  if (presented_name.empty() || object_class.empty() || object_uuid.empty()) return;
  const std::string cache_key =
      BuildNameResolutionCacheKey(session_, presented_name, quoted, object_class);
  CachedPublicNameResolution cached;
  cached.object_uuid = std::string(object_uuid);
  cached.canonical_name = canonical_name.empty() ? std::string(presented_name)
                                                 : std::string(canonical_name);
  cached.object_class = resolved_object_class.empty()
                            ? std::string(object_class)
                            : std::string(resolved_object_class);
  cached.catalog_epoch = catalog_epoch;
  cached.security_epoch = security_epoch;
  name_resolution_cache_[cache_key] = std::move(cached);
  StoreSharedNameResolutionCacheEntry(cache_key, name_resolution_cache_[cache_key]);
  name_resolution_lru_.erase(std::remove(name_resolution_lru_.begin(),
                                         name_resolution_lru_.end(),
                                         cache_key),
                             name_resolution_lru_.end());
  name_resolution_lru_.push_back(cache_key);
  while (name_resolution_cache_.size() > kMaxNameResolutionCacheEntries &&
         !name_resolution_lru_.empty()) {
    name_resolution_cache_.erase(name_resolution_lru_.front());
    name_resolution_lru_.pop_front();
  }
  if (metrics_) metrics_->Increment("sys.metrics.parsers.name_resolution_cache.stores_total");

  if (IsReferencedRelationNameClass(object_class) &&
      IsReferencedRelationNameClass(name_resolution_cache_[cache_key].object_class)) {
    const std::string stable_key =
        BuildStableRelationNameResolutionCacheKey(session_, presented_name, quoted, object_class);
    StableCachedPublicNameResolution stable;
    stable.presented_name = std::string(presented_name);
    stable.quoted = quoted;
    stable.lookup_object_class = std::string(object_class);
    stable.resolved = name_resolution_cache_[cache_key];
    stable_relation_name_resolution_cache_[stable_key] = std::move(stable);
    stable_relation_name_resolution_lru_.erase(
        std::remove(stable_relation_name_resolution_lru_.begin(),
                    stable_relation_name_resolution_lru_.end(),
                    stable_key),
        stable_relation_name_resolution_lru_.end());
    stable_relation_name_resolution_lru_.push_back(stable_key);
    while (stable_relation_name_resolution_cache_.size() >
               kMaxStableRelationNameResolutionCacheEntries &&
           !stable_relation_name_resolution_lru_.empty()) {
      stable_relation_name_resolution_cache_.erase(
          stable_relation_name_resolution_lru_.front());
      stable_relation_name_resolution_lru_.pop_front();
    }
  }
}

void SbsqlTestWireSession::SeedCreatedDdlNameResolutionCache(
    const CstDocument& cst,
    const PipelineResult& result) {
  if (!result.accepted || result.server_result_payload.empty()) return;
  auto object_uuid = DdlResultRowField(result.server_result_payload, "object_uuid");
  auto object_kind = DdlResultRowField(result.server_result_payload, "object_kind");
  std::string route_create_kind;
  std::string result_name_field{"name"};
  if (!object_uuid || object_uuid->empty()) {
    if (result.server_operation_id == "security.role.create") {
      object_uuid = DdlResultRowField(result.server_result_payload, "role_uuid");
      route_create_kind = "ROLE";
      result_name_field = "role_name";
    } else if (result.server_operation_id == "security.group.create") {
      object_uuid = DdlResultRowField(result.server_result_payload, "group_uuid");
      route_create_kind = "GROUP";
      result_name_field = "group_name";
    } else if (result.server_operation_id == "security.principal.create") {
      object_uuid = DdlResultRowField(result.server_result_payload, "principal_uuid");
      route_create_kind = "PRINCIPAL";
      result_name_field = "principal_name";
    } else if (result.server_operation_id == "security.policy.create") {
      object_uuid = DdlResultRowField(result.server_result_payload, "policy_uuid");
      std::size_t index = 0;
      if (ConsumeRouteKeyword(cst, &index, "CREATE")) {
        if (ConsumeRouteKeyword(cst, &index, "POLICY")) {
          route_create_kind = "POLICY";
        } else if (ConsumeRouteKeyword(cst, &index, "MASK")) {
          route_create_kind = "MASK";
        } else if (ConsumeRouteKeyword(cst, &index, "RLS")) {
          route_create_kind = "RLS";
        }
      }
      result_name_field = "policy_name";
    }
  }
  if ((!object_kind || object_kind->empty()) && !route_create_kind.empty()) {
    object_kind = route_create_kind;
  }
  if (!object_uuid || object_uuid->empty() || !object_kind || object_kind->empty()) return;
  const auto created = ExtractCreatedDdlNameFromCst(cst, *object_kind);
  if (!created) return;
  auto payload_name = DdlResultRowField(result.server_result_payload, result_name_field);
  if (!payload_name || payload_name->empty()) {
    payload_name = DdlResultRowField(result.server_result_payload, "name");
  }
  const std::string canonical_name =
      payload_name && !payload_name->empty() ? *payload_name : created->presented_name;
  for (const auto& object_class : created->object_classes) {
    StoreNameResolutionCacheEntry(created->presented_name,
                                  created->quoted,
                                  object_class,
                                  *object_uuid,
                                  canonical_name,
                                  session_.catalog_epoch,
                                  session_.security_policy_epoch);
  }
}

PublicNameResolutionResult SbsqlTestWireSession::ResolveNameOnRouteUncached(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  PublicNameResolutionResult resolved;
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    resolved =
        embedded_client_->ResolveNamePublic(session_, presented_name, quoted, object_class, config_);
  } else {
    ParserTransactionSelector transaction;
    transaction.local_transaction_id = session_.local_transaction_id;
    transaction.transaction_uuid = session_.transaction_uuid;
    if ((object_class == "relation" || object_class == "table") &&
        transaction.present()) {
      resolved = server_client_->ResolveRelationDescriptorPublicOnTransaction(
          session_, presented_name, quoted, object_class, config_, transaction);
    } else {
      resolved = server_client_->ResolveNamePublicUncached(
          session_, presented_name, quoted, object_class, config_);
    }
  }
  if (resolved.resolved) {
    session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
    session_.security_policy_epoch =
        std::max(session_.security_policy_epoch, resolved.security_epoch);
  }
  return resolved;
}

PublicNameResolutionResult SbsqlTestWireSession::ResolveNameOnRoute(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  const std::string cache_key =
      BuildNameResolutionCacheKey(session_, presented_name, quoted, object_class);
  if (const auto found = name_resolution_cache_.find(cache_key);
      found != name_resolution_cache_.end()) {
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.hits_total");
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.route_skips_total");
    }
    PublicNameResolutionResult result;
    result.resolved = true;
    result.object_uuid = found->second.object_uuid;
    result.canonical_name = found->second.canonical_name;
    result.object_class = found->second.object_class;
    result.catalog_epoch = found->second.catalog_epoch;
    result.security_epoch = found->second.security_epoch;
    return result;
  }
  if (auto shared = LookupSharedNameResolutionCache(cache_key)) {
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.shared_hits_total");
      metrics_->Increment("sys.metrics.parsers.name_resolution_cache.route_skips_total");
    }
    name_resolution_cache_[cache_key] = *shared;
    name_resolution_lru_.erase(std::remove(name_resolution_lru_.begin(),
                                           name_resolution_lru_.end(),
                                           cache_key),
                               name_resolution_lru_.end());
    name_resolution_lru_.push_back(cache_key);
    PublicNameResolutionResult result;
    result.resolved = true;
    result.object_uuid = shared->object_uuid;
    result.canonical_name = shared->canonical_name;
    result.object_class = shared->object_class;
    result.catalog_epoch = shared->catalog_epoch;
    result.security_epoch = shared->security_epoch;
    return result;
  }
  if (metrics_) metrics_->Increment("sys.metrics.parsers.name_resolution_cache.misses_total");
  PublicNameResolutionResult resolved =
      ResolveNameOnRouteUncached(presented_name, quoted, object_class);
  if (resolved.resolved) {
    const std::string resolved_class =
        resolved.object_class.empty() ? std::string(object_class)
                                      : resolved.object_class;
    session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
    session_.security_policy_epoch =
        std::max(session_.security_policy_epoch, resolved.security_epoch);
    StoreNameResolutionCacheEntry(presented_name,
                                  quoted,
                                  object_class,
                                  resolved.object_uuid,
                                  resolved.canonical_name,
                                  resolved.catalog_epoch,
                                  resolved.security_epoch,
                                  resolved_class);
    if (resolved_class != std::string(object_class)) {
      StoreNameResolutionCacheEntry(presented_name,
                                    quoted,
                                    resolved_class,
                                    resolved.object_uuid,
                                    resolved.canonical_name,
                                    resolved.catalog_epoch,
                                    resolved.security_epoch,
                                    resolved_class);
    }
  }
  return resolved;
}

PublicNameResolutionResult SbsqlTestWireSession::ResolvePublicNameForWire(
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class) {
  return ResolveNameOnRoute(presented_name, quoted, object_class);
}

bool SbsqlTestWireSession::DisconnectExecutionRoute(MessageVectorSet* messages) {
  if (!session_.authenticated) return true;
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return embedded_client_->DisconnectSession(session_, messages);
  }
  if (!config_.server_endpoint.empty()) {
    return server_client_->DisconnectSession(session_, messages);
  }
  return true;
}

PipelineResult SbsqlTestWireSession::RunServerManagementCommand(
    const ServerManagementCommand& command) {
  ScopedParserState active(metrics_,
                           session_.authenticated && HasExecutionRoute(),
                           ParserState::kActive,
                           ParserState::kAuthenticated);
  PipelineResult result;
  result.statement_family = "runtime_management";
  result.operation_family = "sblr.management.runtime_operation.v3";
  result.server_operation_id = command.operation_id;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE",
        "ERROR",
        "server lifecycle management requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED",
        "ERROR",
        "server lifecycle management requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  ServerManagementResult managed;
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    managed = embedded_client_->Manage(session_,
                                       command.operation_key,
                                       "",
                                       command.mode,
                                       command.audit_reason,
                                       30000,
                                       false);
  } else {
    managed = server_client_->Manage(session_,
                            command.operation_key,
                            "",
                            command.mode,
                            command.audit_reason,
                            30000,
                            false);
  }
  if (!managed.accepted) {
    result.messages = managed.messages;
    return result;
  }
  result.accepted = true;
  result.server_row_count = 1;
  result.server_result_payload =
      "row[0]=operation_key=" + command.operation_key +
      ";operation_id=" + command.operation_id +
      ";route=sbwp_tls_listener_parser_sbps_server_engine" +
      ";accepted=true;payload_bytes=" + std::to_string(managed.payload.size()) + "\n";
  return result;
}

PipelineResult SbsqlTestWireSession::RunPipeline(std::string_view sql,
                                                 bool submit,
                                                 bool cursor_requested,
                                                 std::uint64_t stream_row_count,
                                                 bool autocommit_emulation) {
  const bool phase_trace =
      std::getenv("SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE") != nullptr;
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  auto phase_start = ParserPipelineClock::now();
  auto mark_phase = [&](std::string phase) {
    if (!phase_trace) return;
    const auto now = ParserPipelineClock::now();
    phase_micros.push_back(
        {std::move(phase), ParserPipelineElapsedMicros(phase_start, now)});
    phase_start = now;
  };

  if (metrics_) metrics_->Increment("sys.metrics.parsers.parse_pipeline.attempts_total");
  ScopedParserState active(metrics_,
                           submit && session_.authenticated && HasExecutionRoute(),
                           ParserState::kActive,
                           ParserState::kAuthenticated);
  if (auto management = ParseServerManagementCommand(sql)) {
    auto result = RunServerManagementCommand(*management);
    mark_phase("server_management");
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  if (sql.size() > config_.resource_budget.max_statement_bytes) {
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.resource.limit_exceeded_total");
      metrics_->SetGauge("sys.metrics.parsers.resource.last_statement_bytes",
                         static_cast<double>(sql.size()));
    }
    PipelineResult result;
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.RESOURCE.STATEMENT_TOO_LARGE",
        "ERROR",
        "statement exceeds parser resource budget",
        "sbp_sbsql.wire"));
    mark_phase("resource_budget");
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  if (submit && !cursor_requested && session_.authenticated && HasExecutionRoute() &&
      LooksLikeFastCopyFromStdinCandidate(sql)) {
    const auto fast_copy = TryParseFastCopyFromStdinRoutePlan(sql);
    const std::uint64_t fast_statement_hash = Fnv1a64(sql);
    mark_phase(fast_copy ? "fast_copy_parse_raw"
                         : "fast_copy_parse_raw_fallback");
    if (fast_copy) {
      if (metrics_) {
        metrics_->Increment("sys.metrics.parsers.fast_copy_from_stdin.attempts_total");
      }
      PipelineResult result;
      result.statement_family = "dml.import";
      result.operation_family = "sblr.dml.operation.v3";
      result.statement_hash = fast_statement_hash;
      result.parser_executes_sql = false;
      result.cached_storage_authority = false;
      result.cached_authorization_authority = false;
      result.cached_finality_authority = false;

      auto resolved = ResolveNameOnRoute(fast_copy->target.presented_name,
                                         fast_copy->target.quoted,
                                         fast_copy->target.object_class);
      mark_phase("fast_copy_resolve_target");
      if (!resolved.resolved) {
        result.messages = std::move(resolved.messages);
        result.accepted = false;
        WriteParserPipelinePhaseTrace(sql, result, phase_micros);
        return result;
      }
      session_.catalog_epoch =
          std::max(session_.catalog_epoch, resolved.catalog_epoch);
      session_.security_policy_epoch =
          std::max(session_.security_policy_epoch, resolved.security_epoch);

      const std::string execution_payload =
          BuildFastCopyPlanExecutionEnvelope(*fast_copy, resolved.object_uuid);
      result.sblr_payload = BuildFastCopyPlanJsonPayload(*fast_copy,
                                                         resolved.object_uuid,
                                                         fast_statement_hash);
      mark_phase("fast_copy_build_sblr");
      const auto executed = ExecuteSblrOnRoute(execution_payload, false);
      mark_phase("fast_copy_execute_sblr_route");
      if (!executed.accepted) {
        result.accepted = false;
        result.messages = executed.messages;
      } else {
        result.accepted = true;
        result.server_operation_id = executed.operation_id;
        result.server_cursor_uuid = executed.cursor_uuid;
        result.server_row_count = executed.row_count;
        result.server_affected_rows = executed.affected_rows;
        result.server_affected_rows_present = executed.affected_rows_present;
        result.server_result_payload = executed.row_packet;
        ApplyExecutedTransactionState(executed, &session_);
        if (metrics_) {
          metrics_->Increment("sys.metrics.parsers.fast_copy_from_stdin.accepted_total");
        }
      }
      WriteParserPipelinePhaseTrace(sql, result, phase_micros);
      return result;
    }
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.fast_copy_from_stdin.fallbacks_total");
    }
  }
  if (submit && !cursor_requested && session_.authenticated && HasExecutionRoute() &&
      LooksLikeFastInsertValuesCandidate(sql)) {
    auto fast_insert = TryParseFastInsertValuesRoutePlan(sql);
    std::uint64_t fast_statement_hash = Fnv1a64(sql);
    mark_phase(fast_insert ? "fast_insert_parse_raw"
                           : "fast_insert_parse_raw_fallback");
    if (!fast_insert) {
      auto fast_cst = BuildCst(sql);
      mark_phase("fast_insert_build_cst");
      if (!fast_cst.messages.has_errors()) {
        fast_insert = TryParseFastInsertValuesRoutePlan(fast_cst);
        fast_statement_hash = Fnv1a64(fast_cst.source);
      }
    }
    if (fast_insert) {
        if (metrics_) {
          metrics_->Increment("sys.metrics.parsers.fast_insert_values.attempts_total");
        }
        PipelineResult result;
        result.statement_family = "dml.insert";
        result.operation_family = "sblr.dml.operation.v3";
        result.statement_hash = fast_statement_hash;
        result.parser_executes_sql = false;
        result.cached_storage_authority = false;
        result.cached_authorization_authority = false;
        result.cached_finality_authority = false;

        auto resolved = ResolveNameOnRoute(fast_insert->target.presented_name,
                                           fast_insert->target.quoted,
                                           fast_insert->target.object_class);
        mark_phase("fast_insert_resolve_target");
        if (!resolved.resolved) {
          result.messages = std::move(resolved.messages);
          result.accepted = false;
          WriteParserPipelinePhaseTrace(sql, result, phase_micros);
          return result;
        }
        session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
        session_.security_policy_epoch =
            std::max(session_.security_policy_epoch, resolved.security_epoch);
        result.sblr_payload =
            BuildFastInsertNativeBulkEnvelope(*fast_insert, resolved.object_uuid);
        if (autocommit_emulation) {
          InjectAutocommitEmulation(&result.sblr_payload);
        }
        mark_phase("fast_insert_build_sblr");
        const auto executed = ExecuteSblrOnRoute(result.sblr_payload, false);
        mark_phase("fast_insert_execute_sblr_route");
        if (!executed.accepted) {
          result.accepted = false;
          result.messages = executed.messages;
        } else {
          result.accepted = true;
          result.server_operation_id = executed.operation_id;
          result.server_cursor_uuid = executed.cursor_uuid;
          result.server_row_count = executed.row_count;
          result.server_affected_rows = executed.affected_rows;
          result.server_affected_rows_present = executed.affected_rows_present;
          result.server_result_payload = executed.row_packet;
          ApplyExecutedTransactionState(executed, &session_);
          if (metrics_) {
            metrics_->Increment("sys.metrics.parsers.fast_insert_values.accepted_total");
          }
        }
        WriteParserPipelinePhaseTrace(sql, result, phase_micros);
        return result;
    }
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.fast_insert_values.fallbacks_total");
    }
  }
  const auto frontdoor_cache_key = BuildFrontdoorLoweringCacheKey(config_, session_, sql);
  mark_phase("frontdoor_cache_key");
  if (cache_ != nullptr) {
    if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.attempts_total");
    if (auto cached = cache_->LookupEntry(frontdoor_cache_key)) {
      auto result = PipelineResultFromCacheEntry(*cached);
      if (!submit || CanReuseFrontdoorCacheForSubmit(result)) {
        if (metrics_) {
          metrics_->Increment("sys.metrics.parsers.frontdoor_cache.hits_total");
          metrics_->Increment("sys.metrics.parsers.frontdoor_cache.parse_lower_skips_total");
        }
        mark_phase("frontdoor_cache_hit");
        if (!submit) {
          WriteParserPipelinePhaseTrace(sql, result, phase_micros);
          return result;
        }
        if (!HasExecutionRoute()) {
          result.accepted = false;
          result.messages.diagnostics.push_back(MakeDiagnostic(
              "SBSQL.SERVER.UNAVAILABLE", "ERROR",
              "SBLR submission requires an execution route",
              "sbp_sbsql.wire"));
        } else if (!session_.authenticated) {
          result.accepted = false;
          result.messages.diagnostics.push_back(MakeDiagnostic(
              "SBSQL.AUTH.REQUIRED", "ERROR",
              "SBLR submission requires an authenticated server session",
              "sbp_sbsql.wire"));
        } else {
          std::string execution_payload = result.sblr_payload;
          if (autocommit_emulation && !cursor_requested) {
            InjectAutocommitEmulation(&execution_payload);
          }
          mark_phase("prepare_execution_payload");
          const auto executed = ExecuteSblrOnRoute(execution_payload, cursor_requested);
          mark_phase("execute_sblr_route");
          if (!executed.accepted) {
            result.accepted = false;
            result.messages = executed.messages;
          } else {
            result.server_operation_id = executed.operation_id;
            result.server_cursor_uuid = executed.cursor_uuid;
            result.server_row_count = executed.row_count;
            result.server_affected_rows = executed.affected_rows;
            result.server_affected_rows_present = executed.affected_rows_present;
            result.server_result_payload = executed.row_packet;
            ApplyExecutedTransactionState(executed, &session_);
            if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
              const bool preserve_stable_relations =
                  ExecutionPreservesReferencedRelationNames(executed.operation_id);
              ClearNameResolutionCache(preserve_stable_relations);
              if (preserve_stable_relations) {
                RehydrateStableRelationNameResolutionCache();
              }
            }
          }
        }
        WriteParserPipelinePhaseTrace(sql, result, phase_micros);
        return result;
      }
      if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.misses_total");
    } else if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.frontdoor_cache.misses_total");
    }
  }
  mark_phase("frontdoor_cache_lookup");
  auto cst = BuildCst(sql);
  mark_phase("build_cst");
  MessageVectorSet resource_messages = cst.messages;
  if (!EnforceCstResourceBudget(cst, config_.resource_budget, metrics_,
                                &resource_messages)) {
    PipelineResult result;
    result.accepted = false;
    result.statement_hash = Fnv1a64(cst.source);
    result.messages = std::move(resource_messages);
    mark_phase("cst_resource_budget");
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  auto ast = BuildAst(cst);
  mark_phase("build_ast");
  std::vector<std::string> resolved_object_uuids;
  std::vector<ResolvedObjectReferenceSeed> resolved_object_reference_seeds;
  PipelineResult result;
  result.statement_family = StatementFamilyName(ast.family);
  result.operation_family = ast.operation_family;
  result.statement_hash = Fnv1a64(cst.source);
  result.messages = ast.messages;
  if (!result.messages.has_errors() &&
      !ExactTimeSeriesPreResolutionAst(ast.native_relational)) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1", "ERROR",
        "TIME_BUCKET child identities are incomplete or substituted before object resolution",
        "sbp_sbsql.wire"));
  }
  if (!result.messages.has_errors() && ast.requires_name_resolution &&
      HasExecutionRoute() && session_.authenticated) {
    const auto refs = ExtractObjectReferences(cst, ast);
    const bool native_catalog_projection_required =
        !ast.native_relational.catalog_relation_sources.empty();
    mark_phase("extract_object_references");
    for (const auto& ref : refs) {
      if (IsEngineOwnedProjectionReference(ref)) {
        continue;
      }
      PublicNameResolutionResult resolved;
      if (ref.create_reservation) {
        auto existing =
            ResolveNameOnRouteUncached(ref.presented_name, ref.quoted, ref.object_class);
        if (existing.resolved) {
          result.messages.diagnostics.push_back(MakeDiagnostic(
              "SBSQL.NAME_RESOLUTION.CREATE_NAME_ALREADY_EXISTS",
              "ERROR",
              "create object name already resolves in the authenticated session: " +
                  ref.object_class + " " + ref.presented_name,
              "sbp_sbsql.wire",
              {{"object_class", ref.object_class},
               {"presented_name", ref.presented_name}}));
          break;
        }
        if (!IsNameNotFoundDiagnostic(existing.messages)) {
          result.messages = std::move(existing.messages);
          break;
        }
        resolved.resolved = true;
        resolved.object_uuid = NewCreatedObjectUuid(ref.object_class);
        resolved.canonical_name = ref.presented_name;
        resolved.object_class = ref.object_class;
        resolved.catalog_epoch = session_.catalog_epoch;
        resolved.security_epoch = session_.security_policy_epoch;
        if (resolved.object_uuid.empty()) {
          result.messages.diagnostics.push_back(MakeDiagnostic(
              "SBSQL.NAME_RESOLUTION.CREATE_UUID_RESERVATION_FAILED",
              "ERROR",
              "create object UUID reservation failed before SBLR lowering",
              "sbp_sbsql.wire",
              {{"object_class", ref.object_class},
               {"presented_name", ref.presented_name}}));
          break;
        }
      } else {
        // DOCUMENT_SOURCE/PATH is semantically a document_collection in the
        // typed AST, but persisted projection transport is intentionally the
        // engine's relation V3 route.  The document binding branch below
        // reclassifies only after the returned relation UUID and descriptor
        // projection agree exactly.
        const auto transport_object_class =
            ref.object_class == "document_collection" ||
                    ref.object_class == "graph"
                ? std::string_view("relation")
                : std::string_view(ref.object_class);
        resolved = native_catalog_projection_required
                       ? ResolveNameOnRouteUncached(
                             ref.presented_name, ref.quoted,
                             transport_object_class)
                       : ResolveNameOnRoute(
                             ref.presented_name, ref.quoted,
                             transport_object_class);
      }
      if (!resolved.resolved) {
        if (ref.object_class == "procedure") {
          auto relation_probe = ResolveNameOnRoute(ref.presented_name, ref.quoted, "relation");
          if (relation_probe.resolved) {
            if (std::find(resolved_object_uuids.begin(),
                          resolved_object_uuids.end(),
                          relation_probe.object_uuid) == resolved_object_uuids.end()) {
              resolved_object_uuids.push_back(relation_probe.object_uuid);
              ObjectReference relation_ref = ref;
              relation_ref.object_class = "relation";
              resolved_object_reference_seeds.push_back({relation_ref, relation_probe});
            }
            continue;
          }
        }
        result.messages = std::move(resolved.messages);
        result.messages.diagnostics.push_back(MakeDiagnostic(
            "SBSQL.NAME_RESOLUTION.REFERENCE_FAILED",
            "ERROR",
            "public name resolution failed for a referenced object before SBLR lowering",
            "sbp_sbsql.wire",
            {{"presented_name", ref.presented_name},
             {"object_class", ref.object_class},
             {"quoted", ref.quoted ? "true" : "false"}}));
        break;
      }
      resolved_object_uuids.push_back(resolved.object_uuid);
      resolved_object_reference_seeds.push_back({ref, resolved});
      session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
      session_.security_policy_epoch =
          std::max(session_.security_policy_epoch, resolved.security_epoch);
    }
    mark_phase("resolve_object_references");
  }
  if (result.messages.has_errors()) {
    result.accepted = false;
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  std::optional<ParserStatementContext> native_statement_context;
  std::optional<NativeRelationalBindingContext> native_binding_context;
  if (submit && ast.native_relational.recognized()) {
    if (config_.embedded_engine_direct || server_client_ == nullptr) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.NATIVE_BINDING.SERVER_ROUTE_REQUIRED", "ERROR",
          "Native relational execution requires the private server statement-context route.",
          "sbp_sbsql.wire"));
    } else {
      ParserTransactionSelector selector;
      selector.local_transaction_id = session_.local_transaction_id;
      selector.transaction_uuid = session_.transaction_uuid;
      auto acquired =
          server_client_->AcquireNativeStatementContext(session_, selector);
      mark_phase("acquire_native_statement_context");
      if (!acquired.accepted) {
        result.messages = std::move(acquired.messages);
      } else {
        native_statement_context = std::move(acquired.context);
        native_binding_context = BuildEngineProjectedNativeBindingContext(
            ast.native_relational, *native_statement_context,
            resolved_object_reference_seeds,
            &result.messages);
        mark_phase("build_native_binding_context");
      }
    }
  }
  if (result.messages.has_errors()) {
    result.accepted = false;
    WriteParserPipelinePhaseTrace(sql, result, phase_micros);
    return result;
  }
  auto bound = BindAst(
      ast, cst, config_, session_, resolved_object_uuids,
      native_binding_context.has_value() ? &*native_binding_context : nullptr);
  if (native_statement_context.has_value()) {
    bound.command_registry_snapshot_uuid =
        native_statement_context->catalog_epoch_uuid;
  }
  mark_phase("bind_ast");
  auto lowered = LowerToSblr(bound, cst, session_);
  mark_phase("lower_to_sblr");
  if (!lowered.payload.empty() &&
      lowered.payload.size() > config_.resource_budget.max_sblr_envelope_bytes) {
    lowered.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.RESOURCE.SBLR_ENVELOPE_TOO_LARGE",
        "ERROR",
        "lowered SBLR envelope exceeds parser resource budget",
        "sbp_sbsql.wire",
        {{"sblr_envelope_bytes", std::to_string(lowered.payload.size())},
         {"max_sblr_envelope_bytes",
          std::to_string(config_.resource_budget.max_sblr_envelope_bytes)}}));
  }
  result.accepted = !lowered.messages.has_errors() && !lowered.payload.empty();
  result.parser_executes_sql = false;
  result.cached_storage_authority = false;
  result.cached_authorization_authority = false;
  result.cached_finality_authority = false;
  result.statement_family = StatementFamilyName(ast.family);
  result.operation_family = lowered.operation_family;
  result.statement_hash = lowered.statement_hash;
  result.sblr_payload = lowered.payload;
  result.messages = std::move(lowered.messages);
  std::optional<ParserCanonicalSblrSubmission> native_submission;
  if (submit && result.accepted && native_statement_context.has_value()) {
    native_submission = BuildCanonicalNativeSubmission(
        lowered, *native_statement_context, session_);
    if (!native_submission.has_value()) {
      result.accepted = false;
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.NATIVE_SBLR.CANONICAL_ENCODING_FAILED", "ERROR",
          "The native relational operation could not be encoded as canonical SBLR/SBEE/SBOP.",
          "sbp_sbsql.wire"));
    }
    mark_phase("encode_native_canonical_submission");
  }
  if (cursor_requested) {
    if (stream_row_count != 0) {
      InjectStreamRowCount(&result.sblr_payload, stream_row_count);
    } else {
      InjectCursorFetchWindow(&result.sblr_payload, 1024, 4u * 1024u * 1024u);
    }
  }
  mark_phase("shape_pipeline_result");
  if (result.accepted && cache_ != nullptr &&
      (!submit || CanReuseFrontdoorCacheForSubmit(result))) {
    CacheEntry entry;
    entry.key = BuildFrontdoorLoweringCacheKey(config_, session_, sql);
    entry.sblr_payload = result.sblr_payload;
    entry.statement_family = result.statement_family;
    entry.operation_family = result.operation_family;
    entry.statement_hash = result.statement_hash;
    entry.parser_executes_sql = false;
    entry.storage_authority_cached = false;
    entry.authorization_authority_cached = false;
    entry.finality_authority_cached = false;
    cache_->StoreEntry(std::move(entry));
    if (metrics_) metrics_->Increment("sys.metrics.parsers.frontdoor_cache.stores_total");
  }
  mark_phase("frontdoor_cache_store");
  auto reseed_preserved_reference_names = [&](std::string_view operation_id) {
    if (!ExecutionPreservesReferencedRelationNames(operation_id)) return;
    for (const auto& seed : resolved_object_reference_seeds) {
      if (!seed.resolved.resolved || seed.resolved.object_uuid.empty()) continue;
      const std::string lookup_class =
          seed.ref.object_class.empty() ? std::string("relation") : seed.ref.object_class;
      const std::string resolved_class =
          seed.resolved.object_class.empty() ? lookup_class : seed.resolved.object_class;
      if (!IsReferencedRelationNameClass(lookup_class) &&
          !IsReferencedRelationNameClass(resolved_class)) {
        continue;
      }
      StoreNameResolutionCacheEntry(seed.ref.presented_name,
                                    seed.ref.quoted,
                                    lookup_class,
                                    seed.resolved.object_uuid,
                                    seed.resolved.canonical_name,
                                    session_.catalog_epoch,
                                    session_.security_policy_epoch,
                                    resolved_class);
      if (resolved_class != lookup_class && IsReferencedRelationNameClass(resolved_class)) {
        StoreNameResolutionCacheEntry(seed.ref.presented_name,
                                      seed.ref.quoted,
                                      resolved_class,
                                      seed.resolved.object_uuid,
                                      seed.resolved.canonical_name,
                                      session_.catalog_epoch,
                                      session_.security_policy_epoch,
                                      resolved_class);
      }
    }
  };
  if (submit && result.accepted) {
    if (!HasExecutionRoute()) {
      result.accepted = false;
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE", "ERROR", "SBLR submission requires an execution route",
          "sbp_sbsql.wire"));
    } else if (!session_.authenticated) {
      result.accepted = false;
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED", "ERROR", "SBLR submission requires an authenticated server session",
          "sbp_sbsql.wire"));
    } else {
      std::string execution_payload = result.sblr_payload;
      if (auto create_table_execution =
              CreateTableRouteExecutionEnvelope(cst, result.operation_family)) {
        execution_payload = std::move(*create_table_execution);
      }
      if (autocommit_emulation && !cursor_requested) {
        InjectAutocommitEmulation(&execution_payload);
      }
      mark_phase("prepare_execution_payload");
      const auto executed = native_submission.has_value()
          ? server_client_->ExecuteCanonicalSblrWithDataPacket(
                session_, *native_statement_context, *native_submission, {},
                cursor_requested)
          : ExecuteSblrOnRoute(execution_payload, cursor_requested);
      mark_phase("execute_sblr_route");
      if (!executed.accepted) {
        result.accepted = false;
        result.messages = executed.messages;
      } else {
        result.server_operation_id = executed.operation_id;
        result.server_cursor_uuid = executed.cursor_uuid;
        result.server_row_count = executed.row_count;
        result.server_affected_rows = executed.affected_rows;
        result.server_affected_rows_present = executed.affected_rows_present;
        result.server_result_payload = executed.row_packet;
        ApplyExecutedTransactionState(executed, &session_);
        if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
          const bool preserve_stable_relations =
              ExecutionPreservesReferencedRelationNames(executed.operation_id);
          ClearNameResolutionCache(preserve_stable_relations);
          if (preserve_stable_relations) {
            RehydrateStableRelationNameResolutionCache();
          }
        }
        reseed_preserved_reference_names(executed.operation_id);
        SeedCreatedDdlNameResolutionCache(cst, result);
      }
    }
  }
  WriteParserPipelinePhaseTrace(sql, result, phase_micros);
  return result;
}

PipelineResult SbsqlTestWireSession::RunSblrEnvelope(std::string_view encoded_sblr_envelope,
                                                     bool cursor_requested) {
  return RunSblrEnvelopeWithDataPacket(encoded_sblr_envelope, {}, cursor_requested);
}

PipelineResult SbsqlTestWireSession::RunSblrEnvelopeWithDataPacket(
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  PipelineResult result;
  result.accepted = false;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE", "ERROR", "SBLR submission requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "SBLR submission requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  const auto executed =
      ExecuteSblrOnRouteWithDataPacket(encoded_sblr_envelope, data_packet, cursor_requested);
  if (!executed.accepted) {
    result.messages = executed.messages;
    return result;
  }
  result.accepted = true;
  result.server_operation_id = executed.operation_id;
  result.server_cursor_uuid = executed.cursor_uuid;
  result.server_row_count = executed.row_count;
  result.server_affected_rows = executed.affected_rows;
  result.server_affected_rows_present = executed.affected_rows_present;
  result.server_result_payload = executed.row_packet;
  ApplyExecutedTransactionState(executed, &session_);
  if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
    const bool preserve_stable_relations =
        ExecutionPreservesReferencedRelationNames(executed.operation_id);
    ClearNameResolutionCache(preserve_stable_relations);
    if (preserve_stable_relations) {
      RehydrateStableRelationNameResolutionCache();
    }
  }
  return result;
}

ServerPrepareSblrResult SbsqlTestWireSession::PrepareSblrForWire(
    std::string_view encoded_sblr_envelope) {
  ServerPrepareSblrResult result;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE", "ERROR", "SBLR prepare requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "SBLR prepare requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.PREPARE.UNAVAILABLE", "ERROR",
        "server prepared SBLR handles require the SBPS server route",
        "sbp_sbsql.wire"));
    return result;
  }
  return server_client_->PrepareSblr(session_, encoded_sblr_envelope);
}

PipelineResult SbsqlTestWireSession::RunPreparedSblrEnvelopeForWire(
    std::string_view prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  PipelineResult result;
  result.accepted = false;
  if (!HasExecutionRoute()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.SERVER.UNAVAILABLE", "ERROR", "prepared SBLR execution requires an execution route",
        "sbp_sbsql.wire"));
    return result;
  }
  if (!session_.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "prepared SBLR execution requires an authenticated server session",
        "sbp_sbsql.wire"));
    return result;
  }
  if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
    return RunSblrEnvelopeWithDataPacket(encoded_sblr_envelope, data_packet, cursor_requested);
  }
  const auto executed = server_client_->ExecutePreparedSblr(session_,
                                                  prepared_statement_uuid,
                                                  encoded_sblr_envelope,
                                                  data_packet,
                                                  cursor_requested);
  if (!executed.accepted) {
    result.messages = executed.messages;
    return result;
  }
  result.accepted = true;
  result.server_operation_id = executed.operation_id;
  result.server_cursor_uuid = executed.cursor_uuid;
  result.server_row_count = executed.row_count;
  result.server_affected_rows = executed.affected_rows;
  result.server_affected_rows_present = executed.affected_rows_present;
  result.server_result_payload = executed.row_packet;
  ApplyExecutedTransactionState(executed, &session_);
  if (ExecutionInvalidatesNameResolution(executed.operation_id)) {
    const bool preserve_stable_relations =
        ExecutionPreservesReferencedRelationNames(executed.operation_id);
    ClearNameResolutionCache(preserve_stable_relations);
    if (preserve_stable_relations) {
      RehydrateStableRelationNameResolutionCache();
    }
  }
  return result;
}

WireResponse SbsqlTestWireSession::HandleLine(std::string_view line) {
  const auto trimmed = TrimAscii(line);
  const auto upper = ToUpperAscii(trimmed);
  if (upper.empty()) return {false, "OK EMPTY\n"};
  if (upper == "QUIT" || upper == "EXIT") return {true, "OK BYE\n"};
  if (upper == "PING") return {false, "OK PONG\n"};
  if (upper == "HEARTBEAT") {
    return {false, "HEARTBEAT " + metrics_->HeartbeatJson(config_, session_, *cache_, "idle") + "\n"};
  }
  if (upper == "METRICS") {
    return {false, "METRICS " + metrics_->SnapshotJson(config_, session_, *cache_) + "\n"};
  }
  if (upper == "FLUSH CACHE") {
    cache_->Flush();
    return {false, "OK CACHE_FLUSHED\n"};
  }
  if (upper.starts_with("AUTH")) {
    if (metrics_) {
      metrics_->SetState(ParserState::kAuthenticating);
      metrics_->Increment("sys.metrics.parsers.auth.attempts_total");
    }
    AuthRelayRequest request;
    request.provider_id = "sbsql_test";
    request.payload = AfterCommand(trimmed, "AUTH");
    AuthRelayResult result;
    if (config_.embedded_engine_direct && embedded_client_ != nullptr) {
      AuthCredentialEnvelope credentials;
      credentials.principal = "sysarch";
      credentials.requested_database = config_.embedded_database_path.empty()
                                           ? config_.database_token
                                           : config_.embedded_database_path;
      credentials.requested_language = "en";
      credentials.application_name = "sbp_sbsql_line";
      result.accepted =
          embedded_client_->AuthenticateAndAttachSysarch(credentials, &session_, &result.messages);
    } else if (!config_.server_endpoint.empty()) {
      result.accepted = server_client_->AuthenticateAndAttach(
          request.payload, config_, &session_, &result.messages);
    } else {
      result = config_.allow_probe_auth || config_.probe_mode ? ProbeAuthRelay(request, config_) : FailClosedAuthRelay(request, config_);
      if (result.accepted) session_ = std::move(result.session);
    }
    if (result.accepted || session_.authenticated) {
      if (metrics_) metrics_->SetState(ParserState::kAuthenticated);
      return {false, RenderMessageVectorSet(result.messages) + "OK AUTHENTICATED\n"};
    }
    if (metrics_) {
      metrics_->Increment("sys.metrics.parsers.auth.failures_total");
      metrics_->SetState(ParserState::kIdlePreAuth);
    }
    return {false, RenderMessageVectorSet(result.messages)};
  }
  if (upper.starts_with("PARSE ")) {
    const auto source = trimmed.substr(6);
    if (source.size() > config_.resource_budget.max_statement_bytes) {
      if (metrics_) {
        metrics_->Increment("sys.metrics.parsers.resource.limit_exceeded_total");
        metrics_->SetGauge("sys.metrics.parsers.resource.last_statement_bytes",
                           static_cast<double>(source.size()));
      }
      MessageVectorSet messages;
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.RESOURCE.STATEMENT_TOO_LARGE",
          "ERROR",
          "statement exceeds parser resource budget",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    auto cst = BuildCst(source);
    MessageVectorSet resource_messages = cst.messages;
    if (!EnforceCstResourceBudget(cst, config_.resource_budget, metrics_,
                                  &resource_messages)) {
      return {false, RenderMessageVectorSet(resource_messages)};
    }
    auto ast = BuildAst(cst);
    if (ast.messages.has_errors()) return {false, RenderMessageVectorSet(ast.messages)};
    return {false, "OK PARSED " + StatementFamilyName(ast.family) + "\n"};
  }
  if (upper.starts_with("EXECUTE ")) {
    auto result = RunPipeline(trimmed.substr(8), true);
    return {false, RenderPipelineResult(result)};
  }
  if (upper.starts_with("STREAM ")) {
    std::string stream_body = AfterCommand(trimmed, "STREAM");
    std::uint64_t stream_rows = 5;
    char* end = nullptr;
    const auto parsed = std::strtoull(stream_body.c_str(), &end, 10);
    if (end != nullptr && *end == ' ' && parsed > 0) {
      stream_rows = static_cast<std::uint64_t>(parsed);
      stream_body = TrimAscii(end);
    }
    auto result = RunPipeline(stream_body, true, true, stream_rows);
    if (result.accepted && !result.server_cursor_uuid.empty()) {
      last_cursor_uuid_ = result.server_cursor_uuid;
    }
    return {false, RenderPipelineResult(result)};
  }
  if (upper == "ENGINE STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "engine-backed streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "engine-backed streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto executed = ExecuteSblrOnRoute(EngineShowVersionOperationEnvelope(), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "CURSOR " << executed.cursor_uuid << ' ' << executed.row_count << " source=engine\n";
    return {false, out.str()};
  }
  if (upper == "SBPS CHUNKED EXECUTE") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "chunked SBPS execution requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "chunked SBPS execution requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    constexpr std::size_t kParameterBytes = 1100 * 1024;
    constexpr std::uint64_t kResultRows = 24000;
    const auto envelope = ChunkedParserJsonEnvelope(kParameterBytes, kResultRows);
    const auto executed = ExecuteSblrOnRoute(envelope, false);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    const bool saw_last_row =
        executed.row_packet.find("\"row_index\":23999") != std::string::npos;
    std::ostringstream out;
    out << "CHUNKED_EXECUTE accepted request_bytes=" << envelope.size()
        << " result_bytes=" << executed.row_packet.size()
        << " row_count=" << executed.row_count
        << " last_row=" << (saw_last_row ? "true" : "false") << '\n';
    return {false, out.str()};
  }
  if (upper == "COPY STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "COPY streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "COPY streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    auto resolved = ResolveNameOnRoute("users.public.sbsfc021_stream_table", false, "relation");
    if (!resolved.resolved) {
      return {false, RenderMessageVectorSet(resolved.messages)};
    }
    session_.catalog_epoch = std::max(session_.catalog_epoch, resolved.catalog_epoch);
    session_.security_policy_epoch = std::max(session_.security_policy_epoch, resolved.security_epoch);

    const auto begun = ExecuteSblrOnRoute(TransactionBeginOperationEnvelope(), false);
    if (!begun.accepted) return {false, RenderMessageVectorSet(begun.messages)};
    const auto executed =
        ExecuteSblrOnRoute(EngineBackedCopyStreamImportEnvelope(resolved.object_uuid), true);
    if (!executed.accepted) {
      const auto rolled_back =
          ExecuteSblrOnRoute(TransactionRollbackOperationEnvelope(), false);
      (void)rolled_back;
      return {false, RenderMessageVectorSet(executed.messages)};
    }
    const auto committed = ExecuteSblrOnRoute(TransactionCommitOperationEnvelope(), false);
    if (!committed.accepted) {
      const auto rolled_back =
          ExecuteSblrOnRoute(TransactionRollbackOperationEnvelope(), false);
      (void)rolled_back;
      return {false, RenderMessageVectorSet(committed.messages)};
    }
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "COPY_CURSOR " << executed.cursor_uuid << " events=" << executed.row_count
        << " source=engine operation_id=" << executed.operation_id
        << " committed=true commit_operation_id=" << committed.operation_id << '\n';
    return {false, out.str()};
  }
  if (upper == "MULTI RESULT") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "multi-result streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "multi-result streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto executed = ExecuteSblrOnRoute(MultiResultParserJsonEnvelope(3), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "MULTI_CURSOR " << executed.cursor_uuid << " events=" << executed.row_count << '\n';
    return {false, out.str()};
  }
  if (upper == "WARNING STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "warning/partial-result streaming requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "warning/partial-result streaming requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto executed = ExecuteSblrOnRoute(WarningStreamParserJsonEnvelope(3, 2), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << "WARNING_CURSOR " << executed.cursor_uuid << " events=" << executed.row_count << '\n';
    return {false, out.str()};
  }
  if (upper == "TIMEOUT STREAM" || upper == "DRAIN STREAM" || upper == "CANCEL STREAM") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "stream finality testing requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "stream finality testing requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const std::string mode = upper == "TIMEOUT STREAM" ? "timeout" :
                             (upper == "DRAIN STREAM" ? "drain" : "cancel");
    const std::uint64_t after_fetches = upper == "TIMEOUT STREAM" ? 1 : 0;
    const auto executed = ExecuteSblrOnRoute(FinalityStreamParserJsonEnvelope(mode, 2, after_fetches), true);
    if (!executed.accepted) return {false, RenderMessageVectorSet(executed.messages)};
    last_cursor_uuid_ = executed.cursor_uuid;
    std::ostringstream out;
    out << ToUpperAscii(mode) << "_CURSOR " << executed.cursor_uuid
        << " events=" << executed.row_count << '\n';
    return {false, out.str()};
  }
  if (upper == "ROUTINE CURSOR") {
    MessageVectorSet messages;
    if (!HasExecutionRoute()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.SERVER.UNAVAILABLE",
          "ERROR",
          "routine cursor testing requires an execution route",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    if (!session_.authenticated) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.AUTH.REQUIRED",
          "ERROR",
          "routine cursor testing requires an authenticated server session",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto opened = ExecuteSblrOnRoute(MultiResultParserJsonEnvelope(2), true);
    if (!opened.accepted) return {false, RenderMessageVectorSet(opened.messages)};
    const auto routine =
        ExecuteSblrOnRoute(RoutineCursorArgumentJsonEnvelope(opened.cursor_uuid), false);
    if (!routine.accepted) {
      (void)CloseCursorOnRoute(opened.cursor_uuid);
      return {false, RenderMessageVectorSet(routine.messages)};
    }
    last_cursor_uuid_ = opened.cursor_uuid;
    const bool same_cursor = routine.cursor_uuid == opened.cursor_uuid;
    const bool routine_row_zero =
        routine.row_packet.find("\"row_index\":0") != std::string::npos;
    const bool routine_operation =
        routine.operation_id == "routine.execute_cursor_argument" &&
        routine.row_packet.find("\"operation_id\":\"routine.execute_cursor_argument\"") !=
            std::string::npos;
    std::ostringstream out;
    out << "ROUTINE_CURSOR " << opened.cursor_uuid
        << " operation_id=" << routine.operation_id
        << " routine_rows=" << routine.row_count
        << " same_cursor=" << (same_cursor ? "true" : "false")
        << " routine_row_zero=" << (routine_row_zero ? "true" : "false")
        << " routine_operation=" << (routine_operation ? "true" : "false") << '\n';
    return {false, out.str()};
  }
  if (upper.starts_with("FETCH")) {
    MessageVectorSet messages;
    if (last_cursor_uuid_.empty()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.CURSOR_REQUIRED",
          "ERROR",
          "FETCH requires a live server cursor.",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    std::uint64_t max_rows = 1;
    const auto argument = AfterCommand(trimmed, "FETCH");
    if (!argument.empty()) {
      char* end = nullptr;
      const auto parsed = std::strtoull(argument.c_str(), &end, 10);
      if (end != nullptr && *end == '\0' && parsed > 0) {
        max_rows = static_cast<std::uint64_t>(parsed);
      }
    }
    const auto fetched = FetchCursorOnRoute(last_cursor_uuid_, max_rows);
    if (!fetched.accepted) return {false, RenderMessageVectorSet(fetched.messages)};
    std::ostringstream out;
    out << "FETCH " << fetched.cursor_uuid << ' ' << fetched.row_count
        << " end=" << (fetched.end_of_cursor ? "true" : "false") << ' ';
    if (!fetched.detail.empty()) {
      out << "detail=" << fetched.detail << ' ';
    }
    out
        << fetched.row_packet;
    if (fetched.row_packet.empty() || fetched.row_packet.back() != '\n') out << '\n';
    return {false, out.str()};
  }
  if (upper == "CLOSE CURSOR") {
    MessageVectorSet messages;
    if (last_cursor_uuid_.empty()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.CURSOR_REQUIRED",
          "ERROR",
          "CLOSE CURSOR requires a live server cursor.",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto closed = CloseCursorOnRoute(last_cursor_uuid_);
    if (!closed.accepted) return {false, RenderMessageVectorSet(closed.messages)};
    last_cursor_uuid_.clear();
    return {false, "OK CURSOR_CLOSED\n"};
  }
  if (upper == "CANCEL CURSOR") {
    MessageVectorSet messages;
    if (last_cursor_uuid_.empty()) {
      messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.CURSOR_REQUIRED",
          "ERROR",
          "CANCEL CURSOR requires a live server cursor.",
          "sbp_sbsql.wire"));
      return {false, RenderMessageVectorSet(messages)};
    }
    const auto closed = CancelCursorOnRoute(last_cursor_uuid_);
    if (!closed.accepted) return {false, RenderMessageVectorSet(closed.messages)};
    last_cursor_uuid_.clear();
    return {false, "OK CURSOR_CANCELLED detail=" + closed.detail + "\n"};
  }
  MessageVectorSet messages;
  messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.TEST_WIRE.COMMAND_UNKNOWN", "ERROR", "test wire command is not recognized",
      "sbp_sbsql.wire", {{"command", trimmed}}));
  return {false, RenderMessageVectorSet(messages)};
}

int SbsqlTestWireSession::ServeFd(std::intptr_t fd) {
  if (metrics_) metrics_->SetState(ParserState::kIdlePreAuth);
  if (config_.tls_required) {
    const int rc = ServeSbwp(fd);
    if (metrics_) metrics_->SetState(rc == 0 ? ParserState::kDisconnected : ParserState::kFailed);
    return rc;
  }

  fd_set read_fds;
  FD_ZERO(&read_fds);
#ifdef _WIN32
  FD_SET(static_cast<SOCKET>(fd), &read_fds);
#else
  const int posix_fd = static_cast<int>(fd);
  FD_SET(posix_fd, &read_fds);
#endif
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 100000;
#ifdef _WIN32
  const int ready = ::select(0, &read_fds, nullptr, nullptr, &timeout);
#else
  const int ready = ::select(posix_fd + 1, &read_fds, nullptr, nullptr, &timeout);
#endif
#ifdef _WIN32
  if (ready > 0 && FD_ISSET(static_cast<SOCKET>(fd), &read_fds)) {
#else
  if (ready > 0 && FD_ISSET(posix_fd, &read_fds)) {
#endif
    char magic[4]{};
    int rc = 0;
#ifdef _WIN32
    rc = ::recv(static_cast<SOCKET>(fd), magic, sizeof(magic), MSG_PEEK);
#else
    do {
      rc = static_cast<int>(::recv(posix_fd, magic, sizeof(magic), MSG_PEEK));
    } while (rc < 0 && errno == EINTR);
#endif
    if (rc == static_cast<int>(sizeof(magic)) &&
        std::memcmp(magic, "SBWP", sizeof(magic)) == 0) {
      const int sbwp_rc = ServeSbwp(fd);
      if (metrics_) metrics_->SetState(sbwp_rc == 0 ? ParserState::kDisconnected : ParserState::kFailed);
      return sbwp_rc;
    }
  }

  if (!WriteAll(fd, "ScratchBird SBSQL parser ready\n")) return 1;
  std::string line;
  int rc = 0;
  while (ReadLine(fd, &line)) {
    const auto response = HandleLine(line);
    if (!WriteAll(fd, response.text)) {
      rc = 1;
      break;
    }
    if (response.close) break;
  }
  if (session_.authenticated && HasExecutionRoute()) {
    MessageVectorSet disconnect_messages;
    (void)DisconnectExecutionRoute(&disconnect_messages);
  }
  if (metrics_) metrics_->SetState(rc == 0 ? ParserState::kDisconnected : ParserState::kFailed);
  return rc;
}

} // namespace scratchbird::parser::sbsql
