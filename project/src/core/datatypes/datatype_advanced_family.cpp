// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_advanced_family.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

struct DescriptorProfileEvaluation {
  bool ok = false;
  std::string canonical_profile;
  std::vector<std::string> required_fields;
  std::string failure_detail;
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

std::string LowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::string TrimAscii(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

bool IsIdentifierText(const std::string& value) {
  if (value.empty()) { return false; }
  for (char c : value) {
    const auto ch = static_cast<unsigned char>(c);
    if (std::isalnum(ch) == 0 && c != '_' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

bool IsUuidTextLocal(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') { return false; }
    } else if (std::isxdigit(static_cast<unsigned char>(value[i])) == 0) {
      return false;
    }
  }
  return true;
}

bool ParsePositiveU32(const std::string& value, u32* out) {
  if (value.empty()) { return false; }
  u32 parsed = 0;
  const char* begin = value.data();
  const char* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed == 0) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ValueIn(const std::string& value, std::initializer_list<std::string_view> allowed) {
  for (std::string_view item : allowed) {
    if (value == item) { return true; }
  }
  return false;
}

std::string JoinCanonical(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::ostringstream out;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) { out << ';'; }
    out << fields[i].first << '=' << fields[i].second;
  }
  return out.str();
}

std::map<std::string, std::string> ParseProfileFields(const std::string& profile,
                                                      std::string* failure_detail) {
  std::map<std::string, std::string> fields;
  if (profile.empty()) {
    *failure_detail = "descriptor_profile_required";
    return fields;
  }

  std::size_t begin = 0;
  while (begin <= profile.size()) {
    const std::size_t end = profile.find(';', begin);
    const std::string_view part = end == std::string::npos
                                      ? std::string_view(profile).substr(begin)
                                      : std::string_view(profile).substr(begin, end - begin);
    const std::size_t equals = part.find('=');
    if (equals == std::string_view::npos) {
      *failure_detail = "descriptor_profile_bad_field";
      return {};
    }
    std::string key = LowerAscii(TrimAscii(part.substr(0, equals)));
    std::string value = LowerAscii(TrimAscii(part.substr(equals + 1)));
    if (key.empty() || value.empty()) {
      *failure_detail = "descriptor_profile_empty_field";
      return {};
    }
    if (!fields.emplace(std::move(key), std::move(value)).second) {
      *failure_detail = "descriptor_profile_duplicate_field";
      return {};
    }
    if (end == std::string::npos) { break; }
    begin = end + 1;
    if (begin == profile.size()) {
      *failure_detail = "descriptor_profile_trailing_separator";
      return {};
    }
  }
  return fields;
}

bool HasRequiredFields(const std::map<std::string, std::string>& fields,
                       const std::vector<std::string>& required,
                       std::string* failure_detail) {
  for (const auto& field : required) {
    if (fields.find(field) == fields.end()) {
      *failure_detail = "descriptor_profile_missing_field:" + field;
      return false;
    }
  }
  return true;
}

bool RejectUnknownFields(const std::map<std::string, std::string>& fields,
                         const std::vector<std::string>& required,
                         std::string* failure_detail) {
  for (const auto& [field, _] : fields) {
    if (field == "family" || field == "type") { continue; }
    if (std::find(required.begin(), required.end(), field) == required.end()) {
      *failure_detail = "descriptor_profile_unknown_field:" + field;
      return false;
    }
  }
  return true;
}

bool ValidateOptionalIdentifiers(const std::map<std::string, std::string>& fields,
                                 AdvancedDatatypeFamily family,
                                 CanonicalTypeId type_id,
                                 std::string* failure_detail) {
  const auto family_it = fields.find("family");
  if (family_it != fields.end() && family_it->second != AdvancedDatatypeFamilyName(family)) {
    *failure_detail = "descriptor_profile_family_mismatch";
    return false;
  }
  const auto type_it = fields.find("type");
  if (type_it != fields.end() && type_it->second != CanonicalTypeName(type_id)) {
    *failure_detail = "descriptor_profile_type_mismatch";
    return false;
  }
  return true;
}

bool CommonProfileValidation(const std::map<std::string, std::string>& fields,
                             const std::vector<std::string>& required,
                             AdvancedDatatypeFamily family,
                             CanonicalTypeId type_id,
                             std::string* failure_detail) {
  return HasRequiredFields(fields, required, failure_detail) &&
         RejectUnknownFields(fields, required, failure_detail) &&
         ValidateOptionalIdentifiers(fields, family, type_id, failure_detail);
}

std::string SketchAlgorithmFor(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::hll_sketch: return "hll";
    case CanonicalTypeId::bloom_filter: return "bloom";
    case CanonicalTypeId::quantile_sketch: return "quantile";
    case CanonicalTypeId::histogram_sketch: return "histogram";
    case CanonicalTypeId::ranking_summary: return "ranking";
    case CanonicalTypeId::vector_summary: return "vector_summary";
    default: return {};
  }
}

std::string LocatorScopeFor(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::lob_locator: return "local";
    case CanonicalTypeId::external_file_locator: return "external";
    case CanonicalTypeId::remote_object_locator: return "remote";
    case CanonicalTypeId::bridge_handle: return "bridge";
    case CanonicalTypeId::cursor_handle: return "cursor";
    case CanonicalTypeId::system_reference: return "system";
    default: return {};
  }
}

DescriptorProfileEvaluation EvaluateDescriptorProfile(const AdvancedDatatypeFamilyRequest& request,
                                                      AdvancedDatatypeFamily family) {
  DescriptorProfileEvaluation result;
  std::string parse_failure;
  const auto fields = ParseProfileFields(request.descriptor_profile, &parse_failure);
  if (!parse_failure.empty()) {
    result.failure_detail = parse_failure + ":" + AdvancedDatatypeFamilyName(family);
    return result;
  }

  result.required_fields = [&]() -> std::vector<std::string> {
    switch (family) {
      case AdvancedDatatypeFamily::spatial: return {"format", "srid"};
      case AdvancedDatatypeFamily::vector: return {"dimension", "element_type"};
      case AdvancedDatatypeFamily::search: return {"language", "tokenizer"};
      case AdvancedDatatypeFamily::graph: return {"direction", "schema_uuid"};
      case AdvancedDatatypeFamily::time_series: return {"timestamp_type", "value_type"};
      case AdvancedDatatypeFamily::sketch: return {"algorithm", "precision"};
      case AdvancedDatatypeFamily::aggregate_state:
        return {"aggregate_function_uuid", "state_version"};
      case AdvancedDatatypeFamily::locator: return {"scope", "target_kind"};
      case AdvancedDatatypeFamily::unsupported: return {};
    }
    return {};
  }();

  if (!CommonProfileValidation(fields,
                               result.required_fields,
                               family,
                               request.type_id,
                               &result.failure_detail)) {
    return result;
  }

  std::vector<std::pair<std::string, std::string>> canonical = {
      {"family", AdvancedDatatypeFamilyName(family)},
      {"type", CanonicalTypeName(request.type_id)},
  };

  switch (family) {
    case AdvancedDatatypeFamily::spatial: {
      const std::string& format = fields.at("format");
      if (!ValueIn(format, {"wkb", "wkt", "native"})) {
        result.failure_detail = "spatial_format_unsupported:" + format;
        return result;
      }
      u32 srid = 0;
      if (!ParsePositiveU32(fields.at("srid"), &srid)) {
        result.failure_detail = "spatial_srid_invalid";
        return result;
      }
      canonical.push_back({"format", format});
      canonical.push_back({"srid", std::to_string(srid)});
      break;
    }
    case AdvancedDatatypeFamily::vector: {
      u32 dimension = 0;
      if (!ParsePositiveU32(fields.at("dimension"), &dimension)) {
        result.failure_detail = "vector_dimension_invalid";
        return result;
      }
      if (request.vector_dimension != 0 && request.vector_dimension != dimension) {
        result.failure_detail = "vector_dimension_mismatch";
        return result;
      }
      const std::string& element_type = fields.at("element_type");
      if (!ValueIn(element_type,
                   {"real16", "bfloat16", "real32", "real64", "int8", "uint8",
                    "binary", "bit", "quantized8"})) {
        result.failure_detail = "vector_element_type_unsupported:" + element_type;
        return result;
      }
      canonical.push_back({"dimension", std::to_string(dimension)});
      canonical.push_back({"element_type", element_type});
      break;
    }
    case AdvancedDatatypeFamily::search: {
      const std::string& language = fields.at("language");
      const std::string& tokenizer = fields.at("tokenizer");
      if (!IsIdentifierText(language)) {
        result.failure_detail = "search_language_invalid";
        return result;
      }
      if (!IsIdentifierText(tokenizer)) {
        result.failure_detail = "search_tokenizer_invalid";
        return result;
      }
      canonical.push_back({"language", language});
      canonical.push_back({"tokenizer", tokenizer});
      break;
    }
    case AdvancedDatatypeFamily::graph: {
      const std::string& direction = fields.at("direction");
      if (!ValueIn(direction, {"directed", "undirected"})) {
        result.failure_detail = "graph_direction_invalid:" + direction;
        return result;
      }
      const std::string& schema_uuid = fields.at("schema_uuid");
      if (!IsUuidTextLocal(schema_uuid)) {
        result.failure_detail = "graph_schema_uuid_invalid";
        return result;
      }
      canonical.push_back({"direction", direction});
      canonical.push_back({"schema_uuid", schema_uuid});
      break;
    }
    case AdvancedDatatypeFamily::time_series: {
      const std::string& timestamp_type = fields.at("timestamp_type");
      const std::string& value_type = fields.at("value_type");
      if (!ValueIn(timestamp_type, {"timestamp", "date", "time"})) {
        result.failure_detail = "time_series_timestamp_type_invalid:" + timestamp_type;
        return result;
      }
      if (!IsIdentifierText(value_type)) {
        result.failure_detail = "time_series_value_type_invalid";
        return result;
      }
      canonical.push_back({"timestamp_type", timestamp_type});
      canonical.push_back({"value_type", value_type});
      break;
    }
    case AdvancedDatatypeFamily::sketch: {
      const std::string expected = SketchAlgorithmFor(request.type_id);
      const std::string& algorithm = fields.at("algorithm");
      if (expected.empty() || algorithm != expected) {
        result.failure_detail = "sketch_algorithm_mismatch:" + algorithm;
        return result;
      }
      u32 precision = 0;
      if (!ParsePositiveU32(fields.at("precision"), &precision)) {
        result.failure_detail = "sketch_precision_invalid";
        return result;
      }
      canonical.push_back({"algorithm", algorithm});
      canonical.push_back({"precision", std::to_string(precision)});
      break;
    }
    case AdvancedDatatypeFamily::aggregate_state: {
      const std::string& aggregate_uuid = fields.at("aggregate_function_uuid");
      if (!IsUuidTextLocal(aggregate_uuid)) {
        result.failure_detail = "aggregate_function_uuid_invalid";
        return result;
      }
      u32 state_version = 0;
      if (!ParsePositiveU32(fields.at("state_version"), &state_version)) {
        result.failure_detail = "aggregate_state_version_invalid";
        return result;
      }
      canonical.push_back({"aggregate_function_uuid", aggregate_uuid});
      canonical.push_back({"state_version", std::to_string(state_version)});
      break;
    }
    case AdvancedDatatypeFamily::locator: {
      const std::string expected = LocatorScopeFor(request.type_id);
      const std::string& scope = fields.at("scope");
      if (expected.empty() || scope != expected) {
        result.failure_detail = "locator_scope_mismatch:" + scope;
        return result;
      }
      const std::string& target_kind = fields.at("target_kind");
      if (!IsIdentifierText(target_kind)) {
        result.failure_detail = "locator_target_kind_invalid";
        return result;
      }
      canonical.push_back({"scope", scope});
      canonical.push_back({"target_kind", target_kind});
      break;
    }
    case AdvancedDatatypeFamily::unsupported:
      result.failure_detail = "advanced_family_unsupported";
      return result;
  }

  result.canonical_profile = JoinCanonical(canonical);
  result.ok = true;
  return result;
}

bool IsSpatialOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::contains ||
         operation == AdvancedDatatypeOperationKind::intersects ||
         operation == AdvancedDatatypeOperationKind::distance;
}

bool IsVectorOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::distance ||
         operation == AdvancedDatatypeOperationKind::nearest_neighbor;
}

bool IsSearchOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::tokenize ||
         operation == AdvancedDatatypeOperationKind::search_match ||
         operation == AdvancedDatatypeOperationKind::rank;
}

bool IsGraphOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::graph_traverse ||
         operation == AdvancedDatatypeOperationKind::path_match;
}

bool IsTimeSeriesOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::append_point ||
         operation == AdvancedDatatypeOperationKind::aggregate_window;
}

bool IsSketchOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::estimate ||
         operation == AdvancedDatatypeOperationKind::merge;
}

bool IsAggregateStateOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::merge;
}

bool IsLocatorOperation(AdvancedDatatypeOperationKind operation) {
  return operation == AdvancedDatatypeOperationKind::validate ||
         operation == AdvancedDatatypeOperationKind::resolve_locator;
}

bool IsSpatialIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::rtree ||
         index_kind == AdvancedDatatypeIndexKind::geohash;
}

bool IsVectorIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::hnsw ||
         index_kind == AdvancedDatatypeIndexKind::ivfflat;
}

bool IsSearchIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::inverted;
}

bool IsGraphIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::adjacency;
}

bool IsTimeSeriesIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::time_partition;
}

bool IsSketchIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::sketch_summary;
}

bool IsAggregateStateIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::aggregate_state;
}

bool IsLocatorIndex(AdvancedDatatypeIndexKind index_kind) {
  return index_kind == AdvancedDatatypeIndexKind::none ||
         index_kind == AdvancedDatatypeIndexKind::locator_exact;
}

AdvancedDatatypeFamilyResult Failure(AdvancedDatatypeFamily family, std::string detail) {
  AdvancedDatatypeFamilyResult result;
  result.status = ErrorStatus();
  result.family = family;
  result.diagnostic_detail = detail;
  if (detail.find("compare") != std::string::npos || detail.find("hash") != std::string::npos) {
    result.compare_hash_refusal_detail = detail;
  }
  result.diagnostic = MakeAdvancedDatatypeFamilyDiagnostic(result.status,
                                                          "SB_DATATYPE_ADVANCED_FAMILY_REJECTED",
                                                          "datatype.advanced_family.rejected",
                                                          std::move(detail));
  return result;
}

AdvancedDatatypeFamilyResult Success(const AdvancedDatatypeFamilyRequest& request,
                                     AdvancedDatatypeFamily family,
                                     const DescriptorProfileEvaluation& descriptor,
                                     bool operation_supported,
                                     bool index_supported) {
  AdvancedDatatypeFamilyResult result;
  result.status = OkStatus();
  result.family = family;
  result.descriptor_supported = true;
  result.operation_supported = operation_supported;
  result.index_supported = index_supported;
  result.optimizer_admitted = operation_supported && index_supported &&
                              request.operation != AdvancedDatatypeOperationKind::validate &&
                              request.index_kind != AdvancedDatatypeIndexKind::none;
  result.canonical_descriptor_profile = descriptor.canonical_profile;
  result.required_descriptor_fields = descriptor.required_fields;
  result.compare_supported = false;
  result.hash_supported = false;
  result.compare_hash_refusal_detail =
      "advanced_family_compare_hash_requires_family_specific_operator";
  result.optimizer_support_path = result.optimizer_admitted
                                      ? std::string("descriptor_schema_canonicalized:") +
                                            AdvancedDatatypeIndexKindName(request.index_kind)
                                      : "not_admitted";
  result.diagnostic = MakeAdvancedDatatypeFamilyDiagnostic(result.status,
                                                          "SB_DATATYPE_OK",
                                                          "datatype.ok");
  return result;
}

}  // namespace

const char* AdvancedDatatypeFamilyName(AdvancedDatatypeFamily family) {
  switch (family) {
    case AdvancedDatatypeFamily::spatial: return "spatial";
    case AdvancedDatatypeFamily::vector: return "vector";
    case AdvancedDatatypeFamily::search: return "search";
    case AdvancedDatatypeFamily::graph: return "graph";
    case AdvancedDatatypeFamily::time_series: return "time_series";
    case AdvancedDatatypeFamily::sketch: return "sketch";
    case AdvancedDatatypeFamily::aggregate_state: return "aggregate_state";
    case AdvancedDatatypeFamily::locator: return "locator";
    case AdvancedDatatypeFamily::unsupported: return "unsupported";
  }
  return "unsupported";
}

const char* AdvancedDatatypeOperationKindName(AdvancedDatatypeOperationKind operation) {
  switch (operation) {
    case AdvancedDatatypeOperationKind::validate: return "validate";
    case AdvancedDatatypeOperationKind::compare: return "compare";
    case AdvancedDatatypeOperationKind::hash: return "hash";
    case AdvancedDatatypeOperationKind::contains: return "contains";
    case AdvancedDatatypeOperationKind::intersects: return "intersects";
    case AdvancedDatatypeOperationKind::distance: return "distance";
    case AdvancedDatatypeOperationKind::nearest_neighbor: return "nearest_neighbor";
    case AdvancedDatatypeOperationKind::tokenize: return "tokenize";
    case AdvancedDatatypeOperationKind::search_match: return "search_match";
    case AdvancedDatatypeOperationKind::rank: return "rank";
    case AdvancedDatatypeOperationKind::graph_traverse: return "graph_traverse";
    case AdvancedDatatypeOperationKind::path_match: return "path_match";
    case AdvancedDatatypeOperationKind::append_point: return "append_point";
    case AdvancedDatatypeOperationKind::aggregate_window: return "aggregate_window";
    case AdvancedDatatypeOperationKind::estimate: return "estimate";
    case AdvancedDatatypeOperationKind::merge: return "merge";
    case AdvancedDatatypeOperationKind::resolve_locator: return "resolve_locator";
    case AdvancedDatatypeOperationKind::unsupported: return "unsupported";
  }
  return "unsupported";
}

const char* AdvancedDatatypeIndexKindName(AdvancedDatatypeIndexKind index_kind) {
  switch (index_kind) {
    case AdvancedDatatypeIndexKind::none: return "none";
    case AdvancedDatatypeIndexKind::btree: return "btree";
    case AdvancedDatatypeIndexKind::rtree: return "rtree";
    case AdvancedDatatypeIndexKind::geohash: return "geohash";
    case AdvancedDatatypeIndexKind::inverted: return "inverted";
    case AdvancedDatatypeIndexKind::hnsw: return "hnsw";
    case AdvancedDatatypeIndexKind::ivfflat: return "ivfflat";
    case AdvancedDatatypeIndexKind::adjacency: return "adjacency";
    case AdvancedDatatypeIndexKind::time_partition: return "time_partition";
    case AdvancedDatatypeIndexKind::sketch_summary: return "sketch_summary";
    case AdvancedDatatypeIndexKind::aggregate_state: return "aggregate_state";
    case AdvancedDatatypeIndexKind::locator_exact: return "locator_exact";
    case AdvancedDatatypeIndexKind::unsupported: return "unsupported";
  }
  return "unsupported";
}

AdvancedDatatypeFamily AdvancedDatatypeFamilyFor(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::geometry:
    case CanonicalTypeId::geography:
    case CanonicalTypeId::point:
    case CanonicalTypeId::shape:
    case CanonicalTypeId::raster:
      return AdvancedDatatypeFamily::spatial;
    case CanonicalTypeId::vector:
    case CanonicalTypeId::dense_vector:
    case CanonicalTypeId::sparse_vector:
    case CanonicalTypeId::binary_vector:
    case CanonicalTypeId::quantized_vector:
      return AdvancedDatatypeFamily::vector;
    case CanonicalTypeId::token_stream:
    case CanonicalTypeId::search_query:
    case CanonicalTypeId::search_rank_feature:
    case CanonicalTypeId::search_completion:
    case CanonicalTypeId::search_percolator:
      return AdvancedDatatypeFamily::search;
    case CanonicalTypeId::graph_node:
    case CanonicalTypeId::graph_edge:
    case CanonicalTypeId::graph_path:
      return AdvancedDatatypeFamily::graph;
    case CanonicalTypeId::time_series_value:
      return AdvancedDatatypeFamily::time_series;
    case CanonicalTypeId::hll_sketch:
    case CanonicalTypeId::bloom_filter:
    case CanonicalTypeId::quantile_sketch:
    case CanonicalTypeId::histogram_sketch:
    case CanonicalTypeId::ranking_summary:
    case CanonicalTypeId::vector_summary:
      return AdvancedDatatypeFamily::sketch;
    case CanonicalTypeId::aggregate_state:
      return AdvancedDatatypeFamily::aggregate_state;
    case CanonicalTypeId::lob_locator:
    case CanonicalTypeId::external_file_locator:
    case CanonicalTypeId::remote_object_locator:
    case CanonicalTypeId::bridge_handle:
    case CanonicalTypeId::cursor_handle:
    case CanonicalTypeId::system_reference:
      return AdvancedDatatypeFamily::locator;
    default:
      return AdvancedDatatypeFamily::unsupported;
  }
}

AdvancedDatatypeFamilyResult EvaluateAdvancedDatatypeFamily(const AdvancedDatatypeFamilyRequest& request) {
  const AdvancedDatatypeFamily family = AdvancedDatatypeFamilyFor(request.type_id);
  if (family == AdvancedDatatypeFamily::unsupported) {
    return Failure(family, "advanced_family_unsupported_type:" + std::string(CanonicalTypeName(request.type_id)));
  }
  if (request.operation == AdvancedDatatypeOperationKind::unsupported ||
      request.index_kind == AdvancedDatatypeIndexKind::unsupported) {
    return Failure(family, "advanced_family_unsupported_operation_or_index");
  }

  DescriptorProfileEvaluation descriptor = EvaluateDescriptorProfile(request, family);
  if (!descriptor.ok) {
    return Failure(family, descriptor.failure_detail);
  }

  if (request.operation == AdvancedDatatypeOperationKind::compare) {
    auto result = Failure(family, "advanced_compare_refused:family_specific_operator_required");
    result.canonical_descriptor_profile = descriptor.canonical_profile;
    result.required_descriptor_fields = descriptor.required_fields;
    return result;
  }
  if (request.operation == AdvancedDatatypeOperationKind::hash) {
    auto result = Failure(family, "advanced_hash_refused:family_specific_operator_required");
    result.canonical_descriptor_profile = descriptor.canonical_profile;
    result.required_descriptor_fields = descriptor.required_fields;
    return result;
  }

  bool operation_supported = false;
  bool index_supported = false;
  switch (family) {
    case AdvancedDatatypeFamily::spatial:
      operation_supported = IsSpatialOperation(request.operation);
      index_supported = IsSpatialIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::vector:
      operation_supported = IsVectorOperation(request.operation);
      index_supported = IsVectorIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::search:
      operation_supported = IsSearchOperation(request.operation);
      index_supported = IsSearchIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::graph:
      operation_supported = IsGraphOperation(request.operation);
      index_supported = IsGraphIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::time_series:
      operation_supported = IsTimeSeriesOperation(request.operation);
      index_supported = IsTimeSeriesIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::sketch:
      operation_supported = IsSketchOperation(request.operation);
      index_supported = IsSketchIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::aggregate_state:
      operation_supported = IsAggregateStateOperation(request.operation);
      index_supported = IsAggregateStateIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::locator:
      operation_supported = IsLocatorOperation(request.operation);
      index_supported = IsLocatorIndex(request.index_kind);
      break;
    case AdvancedDatatypeFamily::unsupported:
      break;
  }
  if (!operation_supported) {
    return Failure(family, std::string("operation_not_supported_for_family:") +
                               AdvancedDatatypeOperationKindName(request.operation));
  }
  if (!index_supported) {
    return Failure(family, std::string("index_not_supported_for_family:") +
                               AdvancedDatatypeIndexKindName(request.index_kind));
  }
  return Success(request, family, descriptor, operation_supported, index_supported);
}

DiagnosticRecord MakeAdvancedDatatypeFamilyDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) { arguments.push_back({"detail", std::move(detail)}); }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.advanced_family");
}

}  // namespace scratchbird::core::datatypes
