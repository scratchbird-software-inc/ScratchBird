// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_exchange.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace scratchbird::engine::executor {
namespace {

bool CanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool CanonicalStatementTimestamp(const std::string_view value) {
  if (value.size() != 20 && (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigits[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigits) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](const std::size_t begin,
                           const std::size_t count) {
    unsigned out = 0;
    for (std::size_t index = 0; index < count; ++index) {
      out = out * 10 + static_cast<unsigned>(value[begin + index] - '0');
    }
    return out;
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
  constexpr unsigned kDays[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDays[month];
  if (month == 2 &&
      ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    ++maximum_day;
  }
  return day != 0 && day <= maximum_day;
}

constexpr std::int64_t DaysFromCivil(const int year,
                                     const unsigned month,
                                     const unsigned day) {
  const int adjusted_year = year - (month <= 2 ? 1 : 0);
  const int era = (adjusted_year >= 0 ? adjusted_year
                                      : adjusted_year - 399) /
                  400;
  const unsigned year_of_era =
      static_cast<unsigned>(adjusted_year - era * 400);
  const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
  const unsigned day_of_year =
      (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
      day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

bool CanonicalTimestampNs(const std::string_view value, std::int64_t* out) {
  if (out == nullptr || value.size() != 30 || value[19] != '.' ||
      value.back() != 'Z' || !CanonicalStatementTimestamp(value)) {
    return false;
  }
  const auto decimal = [&](const std::size_t begin,
                           const std::size_t count) {
    std::uint64_t parsed = 0;
    for (std::size_t index = 0; index < count; ++index) {
      parsed = parsed * 10 +
               static_cast<std::uint64_t>(value[begin + index] - '0');
    }
    return parsed;
  };
  constexpr __int128 kNsPerSecond = 1'000'000'000;
  const auto seconds =
      static_cast<__int128>(DaysFromCivil(static_cast<int>(decimal(0, 4)),
                                         static_cast<unsigned>(decimal(5, 2)),
                                         static_cast<unsigned>(decimal(8, 2)))) *
          86'400 +
      static_cast<__int128>(decimal(11, 2)) * 3600 +
      static_cast<__int128>(decimal(14, 2)) * 60 + decimal(17, 2);
  const auto encoded = seconds * kNsPerSecond + decimal(20, 9);
  if (encoded < std::numeric_limits<std::int64_t>::min() ||
      encoded > std::numeric_limits<std::int64_t>::max()) {
    return false;
  }
  *out = static_cast<std::int64_t>(encoded);
  return true;
}

bool WellFormedUtf8(const std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t code_point = 0;
    std::size_t continuation_count = 0;
    if (first <= 0x7f) {
      code_point = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1f;
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0f;
      continuation_count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07;
      continuation_count = 3;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1) return false;
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto next = static_cast<unsigned char>(value[offset + index]);
      if ((next & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool UnsignedUtf8Less(const std::string_view left,
                      const std::string_view right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](const char l, const char r) {
        return static_cast<unsigned char>(l) <
               static_cast<unsigned char>(r);
      });
}

bool ExchangeCancellationRequested(
    const std::function<bool()>& cancellation_requested) {
  try {
    return cancellation_requested && cancellation_requested();
  } catch (...) {
    return true;
  }
}

bool ParseCanonicalTagJsonString(
    const std::string_view input,
    std::size_t* offset,
    const std::function<bool()>& cancellation_requested,
    bool* cancellation_observed,
    std::string* decoded) {
  if (offset == nullptr || cancellation_observed == nullptr ||
      decoded == nullptr || *offset >= input.size() ||
      input[*offset] != '"') {
    return false;
  }
  ++*offset;
  decoded->clear();
  while (*offset < input.size()) {
    if (ExchangeCancellationRequested(cancellation_requested)) {
      *cancellation_observed = true;
      return false;
    }
    const auto byte = static_cast<unsigned char>(input[(*offset)++]);
    if (byte == '"') return WellFormedUtf8(*decoded);
    if (byte < 0x20) return false;
    if (byte != '\\') {
      decoded->push_back(static_cast<char>(byte));
      continue;
    }
    if (*offset >= input.size()) return false;
    const char escaped = input[(*offset)++];
    switch (escaped) {
      case '"': decoded->push_back('"'); break;
      case '\\': decoded->push_back('\\'); break;
      case 'b': decoded->push_back('\b'); break;
      case 'f': decoded->push_back('\f'); break;
      case 'n': decoded->push_back('\n'); break;
      case 'r': decoded->push_back('\r'); break;
      case 't': decoded->push_back('\t'); break;
      case 'u': {
        if (*offset + 4 > input.size() || input[*offset] != '0' ||
            input[*offset + 1] != '0') {
          return false;
        }
        const auto hex = [](const char value) -> int {
          if (value >= '0' && value <= '9') return value - '0';
          if (value >= 'a' && value <= 'f') return value - 'a' + 10;
          return -1;
        };
        const int high = hex(input[*offset + 2]);
        const int low = hex(input[*offset + 3]);
        if (high < 0 || low < 0) return false;
        const auto control = static_cast<unsigned char>((high << 4) | low);
        *offset += 4;
        if (control >= 0x20 || control == '\b' || control == '\f' ||
            control == '\n' || control == '\r' || control == '\t') {
          return false;
        }
        decoded->push_back(static_cast<char>(control));
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

bool CanonicalTimeSeriesTags(
    const std::string_view input,
    const std::function<bool()>& cancellation_requested,
    bool* cancellation_observed) {
  if (cancellation_observed == nullptr || input.empty() ||
      input.front() != '{') {
    return false;
  }
  *cancellation_observed = false;
  std::size_t offset = 1;
  std::string previous_key;
  bool first = true;
  if (offset < input.size() && input[offset] == '}') {
    return ++offset == input.size();
  }
  while (offset < input.size()) {
    std::string key;
    std::string value;
    if (!ParseCanonicalTagJsonString(input, &offset,
                                     cancellation_requested,
                                     cancellation_observed, &key) ||
        *cancellation_observed || key.empty() || offset >= input.size() ||
        input[offset++] != ':' ||
        !ParseCanonicalTagJsonString(input, &offset,
                                     cancellation_requested,
                                     cancellation_observed, &value) ||
        *cancellation_observed ||
        (!first && !UnsignedUtf8Less(previous_key, key))) {
      return false;
    }
    previous_key = std::move(key);
    first = false;
    if (offset >= input.size()) return false;
    if (input[offset] == '}') {
      ++offset;
      break;
    }
    if (input[offset++] != ',') return false;
  }
  return offset == input.size();
}

bool CanonicalFiniteReal64(const std::string_view encoded) {
  if (encoded.empty()) return false;
  double value = 0.0;
  const auto parsed = std::from_chars(encoded.data(),
                                      encoded.data() + encoded.size(), value,
                                      std::chars_format::general);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != encoded.data() + encoded.size() || !std::isfinite(value)) {
    return false;
  }
  if (value == 0.0) value = 0.0;
  char canonical[128]{};
  const auto rendered = std::to_chars(
      std::begin(canonical), std::end(canonical), value,
      std::chars_format::general, std::numeric_limits<double>::max_digits10);
  return rendered.ec == std::errc{} &&
         encoded == std::string_view(canonical,
                                     static_cast<std::size_t>(
                                         rendered.ptr - canonical));
}

bool CanonicalInt64(const std::string_view encoded,
                    const bool require_positive = false) {
  if (encoded.empty()) return false;
  std::int64_t value = 0;
  const auto parsed = std::from_chars(encoded.data(),
                                      encoded.data() + encoded.size(), value);
  return parsed.ec == std::errc{} &&
         parsed.ptr == encoded.data() + encoded.size() &&
         (!require_positive || value > 0) &&
         encoded == std::to_string(value);
}

ModelExchangeResultV1 Refuse(const char* diagnostic, std::string detail) {
  ModelExchangeResultV1 result;
  result.diagnostic_id = diagnostic;
  result.detail = std::move(detail);
  return result;
}

}  // namespace

bool ValidateCanonicalTimeSeriesTagsV1(
    const std::string_view tags,
    const std::function<bool()>& cancellation_requested,
    bool* cancellation_observed) {
  bool local_cancellation = false;
  auto* observed = cancellation_observed == nullptr
                       ? &local_cancellation
                       : cancellation_observed;
  return CanonicalTimeSeriesTags(tags, cancellation_requested, observed);
}

bool ParseCanonicalTimeSeriesTimestampNsV1(
    const std::string_view timestamp,
    std::int64_t* timestamp_ns) {
  return CanonicalTimestampNs(timestamp, timestamp_ns);
}

ModelInputValidationResultV1 ValidateModelFamilySourceInputV1(
    const ModelSourceInputDescriptorV1& input) {
  ModelInputValidationResultV1 result;
  const bool document_family = input.family_id == "document";
  const bool graph_family = input.family_id == "graph";
  const bool key_value_family = input.family_id == "key_value";
  const bool time_series_family = input.family_id == "time_series";
  const bool vector_family = input.family_id == "vector";
  const bool valid_operation =
      (document_family &&
       (input.operation_id == "DOCUMENT_FIND" ||
        input.operation_id == "DOCUMENT_PATH" ||
        input.operation_id == "DOCUMENT_UNNEST")) ||
      (graph_family &&
       (input.operation_id == "GRAPH_MATCH" ||
        input.operation_id == "GRAPH_EXPAND")) ||
      (key_value_family &&
       (input.operation_id == "KEY_VALUE_GET" ||
        input.operation_id == "KEY_VALUE_MULTI_GET" ||
        input.operation_id == "KEY_VALUE_PREFIX_RANGE")) ||
      (time_series_family &&
       (input.operation_id == "TIME_SERIES_RANGE_READ" ||
        input.operation_id == "TIME_SERIES_BUCKET" ||
        input.operation_id == "TIME_SERIES_DOWNSAMPLE")) ||
      (vector_family &&
       (input.operation_id == "VECTOR_EXACT_SEARCH" ||
        input.operation_id == "VECTOR_ANN_SEARCH" ||
        input.operation_id == "VECTOR_FILTERED_SEARCH"));
  const bool timestamp_family =
      key_value_family || time_series_family || vector_family;
  if (timestamp_family !=
          !input.mga_statement_context.statement_timestamp.empty() ||
      (timestamp_family &&
       !CanonicalStatementTimestamp(
           input.mga_statement_context.statement_timestamp))) {
    result.diagnostic_id =
        time_series_family
            ? "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1"
            : (vector_family
                   ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                   : "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");
    result.detail = "timestamp-carrying typed input timestamp is invalid";
    return result;
  }
  std::unordered_set<std::uint32_t> output_descriptor_ids;
  const bool exact_output_descriptor_ids = std::ranges::all_of(
      input.output_descriptor_ids, [&](const std::uint32_t descriptor_id) {
        return descriptor_id != 0 &&
               output_descriptor_ids.insert(descriptor_id).second;
      });
  if (input.abi_version != 1 ||
      input.input_descriptor_id != "SB_MODEL_SOURCE_INPUT_DESCRIPTOR_V1" ||
      (!document_family && !graph_family && !key_value_family &&
       !time_series_family && !vector_family) ||
      !valid_operation ||
      (input.operation_id == "DOCUMENT_UNNEST" && !input.object_uuid.empty()) ||
      (input.operation_id != "DOCUMENT_UNNEST" &&
       !CanonicalUuid(input.object_uuid)) ||
      input.physical_node_id == 0 || input.causal_counter_id == 0 ||
      input.provider_generation == 0 || input.catalog_generation == 0 ||
      input.descriptor_generation == 0 || input.security_generation == 0 ||
      input.policy_generation == 0 || input.resource_generation == 0 ||
      input.output_descriptor_ids.empty() || input.maximum_rows == 0 ||
      !exact_output_descriptor_ids ||
      (vector_family && input.output_descriptor_ids.size() != 3) ||
      input.maximum_cells == 0 || input.maximum_memory_bytes == 0 ||
      !CanonicalUuid(input.selected_alternative_uuid) ||
      !CanonicalUuid(input.capability_uuid) ||
      !CanonicalUuid(input.provider_uuid) ||
      !CanonicalUuid(input.result_handle_uuid) ||
      !CanonicalUuid(input.catalog_epoch_uuid) ||
      !CanonicalUuid(input.security_context_uuid) ||
      !CanonicalUuid(input.policy_snapshot_uuid) ||
      !CanonicalUuid(input.resource_contract_uuid) ||
      !PhysicalMgaStatementContextValid(input.mga_statement_context) ||
      input.parser_execution_authority_claimed ||
      input.transaction_finality_authority_claimed) {
    result.diagnostic_id = kModelTypedExchangeInvalid;
    result.detail = "model source input descriptor is incomplete";
    return result;
  }
  result.accepted = true;
  return result;
}

ModelExchangeResultV1 PublishModelFamilyExchangeV1(
    const ModelSourceInputDescriptorV1& input,
    const ModelProviderBatchV1& provider_batch,
    const std::function<bool()>& cancellation_requested) {
  // QOW-SOURCE-CES05-MODEL-TYPED-EXCHANGE-V1
  const bool graph_family = input.family_id == "graph";
  const bool key_value_family = input.family_id == "key_value";
  const bool time_series_family = input.family_id == "time_series";
  const bool vector_family = input.family_id == "vector";
  const auto input_validation = ValidateModelFamilySourceInputV1(input);
  if (!input_validation.accepted) {
    return Refuse(input_validation.diagnostic_id.c_str(),
                  input_validation.detail);
  }
  if (ExchangeCancellationRequested(cancellation_requested)) {
    return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "model-family exchange was cancelled before validation");
  }
  if (provider_batch.abi_version != 1) {
    return Refuse(kModelTypedExchangeInvalid,
                  "model provider batch version is unsupported");
  }
  if (provider_batch.provider_uuid != input.provider_uuid ||
      provider_batch.provider_generation != input.provider_generation ||
      ((key_value_family || time_series_family || vector_family) &&
       (provider_batch.selected_alternative_uuid !=
            input.selected_alternative_uuid ||
        provider_batch.capability_uuid != input.capability_uuid ||
        provider_batch.exact_fallback_selected !=
            input.exact_fallback_selected)) ||
      provider_batch.result_handle_uuid != input.result_handle_uuid ||
      provider_batch.causal_counter_id != input.causal_counter_id ||
      !PhysicalMgaStatementContextEqual(provider_batch.mga_statement_context,
                                        input.mga_statement_context) ||
      !CanonicalUuid(provider_batch.security_receipt_uuid) ||
      provider_batch.provider_visibility_authority_claimed ||
      provider_batch.provider_finality_authority_claimed) {
    return Refuse(kModelMgaContextMismatch,
                  "provider exchange identity or MGA context was substituted");
  }
  if (provider_batch.output_descriptor_ids != input.output_descriptor_ids) {
    return Refuse(kModelTypedExchangeInvalid,
                  "provider output descriptors differ from the bound input");
  }
  if (graph_family || key_value_family || time_series_family || vector_family) {
    std::size_t preflight_cell_count = 0;
    // The provider batch is caller/provider-owned. The grant covers every
    // allocation copied into the engine-owned output descriptor at the same
    // time as the normalized batch.
    std::uint64_t preflight_memory_bytes =
        sizeof(ModelSourceOutputDescriptorV1);
    const auto preflight_account = [&](const std::uint64_t bytes) {
      if (bytes > std::numeric_limits<std::uint64_t>::max() -
                      preflight_memory_bytes) {
        return false;
      }
      preflight_memory_bytes += bytes;
      return preflight_memory_bytes <= input.maximum_memory_bytes;
    };
    const auto preflight_string = [&](const std::string_view value) {
      return preflight_account(value.size());
    };
    if (provider_batch.batch.rows.size() > input.maximum_rows) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph exchange row preflight exceeded its resource contract");
    }
    if (provider_batch.ordered_row_identities.size() !=
        provider_batch.batch.rows.size()) {
      return Refuse(kModelTypedExchangeInvalid,
                    "graph ordered row identity cardinality is incomplete");
    }
    if (!preflight_string("SB_MODEL_SOURCE_OUTPUT_DESCRIPTOR_V1") ||
        !preflight_string("SB_MODEL_PROPERTY_DESCRIPTOR_V1") ||
        !preflight_string(input.input_descriptor_id) ||
        !preflight_string(input.family_id) ||
        !preflight_string(input.operation_id) ||
        !preflight_string(input.object_uuid) ||
        !preflight_string(input.selected_alternative_uuid) ||
        !preflight_string(input.capability_uuid) ||
        !preflight_string(input.provider_uuid) ||
        !preflight_string(input.result_handle_uuid) ||
        !preflight_account(input.output_descriptor_ids.size() *
                           sizeof(std::uint32_t)) ||
        !preflight_string(provider_batch.properties.property_descriptor_id) ||
        !preflight_string(provider_batch.properties.property_uuid) ||
        !preflight_string(provider_batch.properties.ordering_id) ||
        !preflight_string(provider_batch.properties.partitioning_id) ||
        !preflight_string(provider_batch.properties.uniqueness_id) ||
        !preflight_string(input.mga_statement_context.statement_uuid) ||
        !preflight_string(input.mga_statement_context.statement_timestamp) ||
        !preflight_string(input.mga_statement_context.owning_transaction_uuid) ||
        !preflight_string(input.mga_statement_context.statement_snapshot_uuid) ||
        !preflight_string(
            input.mga_statement_context.statement_metadata_snapshot_uuid) ||
        !preflight_string(input.mga_statement_context.snapshot_kind) ||
        !preflight_account(
            input.mga_statement_context.active_excluded_local_transaction_ids
                .size() * sizeof(std::uint64_t)) ||
        !preflight_account(
            input.mga_statement_context.in_doubt_excluded_local_transaction_ids
                .size() * sizeof(std::uint64_t)) ||
        !preflight_string(provider_batch.security_receipt_uuid)) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph exchange output metadata exceeded its resource contract");
    }
    for (std::size_t identity_ordinal = 0;
         identity_ordinal < provider_batch.ordered_row_identities.size();
         ++identity_ordinal) {
      const auto& identity =
          provider_batch.ordered_row_identities[identity_ordinal];
      if (ExchangeCancellationRequested(cancellation_requested)) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "model-family exchange identity preflight was cancelled");
      }
      if (!preflight_account(sizeof(ModelProviderRowIdentityV1)) ||
          !preflight_string(identity.document_uuid) ||
          !preflight_string(identity.row_uuid) ||
          !preflight_string(identity.key) ||
          !preflight_string(identity.vertex_uuid) ||
          !preflight_string(identity.edge_uuid) ||
          !preflight_string(identity.path_uuid) ||
          !preflight_string(identity.series_uuid) ||
          !preflight_string(identity.metric_uuid) ||
          !preflight_string(identity.tags) ||
          !preflight_string(identity.time_series_payload_kind) ||
          !preflight_string(identity.time_series_raw_value) ||
          !preflight_string(identity.time_series_sample_count) ||
          !preflight_string(identity.time_series_aggregate_value) ||
          !preflight_string(identity.vector_distance) ||
          !preflight_string(identity.vector_score)) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph exchange identity preflight exceeded its resource contract");
      }
    }
    for (const auto& column : provider_batch.batch.columns) {
      if (ExchangeCancellationRequested(cancellation_requested)) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "model-family exchange descriptor preflight was cancelled");
      }
      if (!preflight_account(sizeof(ExecutorColumnDescriptor)) ||
          !preflight_account(column.stable_name.size()) ||
          !preflight_account(
              column.descriptor.descriptor_uuid.canonical.size()) ||
          !preflight_account(column.descriptor.descriptor_kind.size()) ||
          !preflight_account(column.descriptor.canonical_type_name.size()) ||
          !preflight_account(column.descriptor.encoded_descriptor.size())) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph exchange descriptor preflight exceeded its resource contract");
      }
    }
    for (const auto& row : provider_batch.batch.rows) {
      if (ExchangeCancellationRequested(cancellation_requested)) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "model-family exchange row preflight was cancelled");
      }
      if (preflight_cell_count > input.maximum_cells ||
          row.values.size() > input.maximum_cells - preflight_cell_count ||
          !preflight_account(sizeof(DescriptorTuple))) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph exchange row preflight exceeded its resource contract");
      }
      preflight_cell_count += row.values.size();
      for (const auto& value : row.values) {
        if (!preflight_account(sizeof(internal_api::EngineTypedValue)) ||
            !preflight_account(
                value.descriptor.descriptor_uuid.canonical.size()) ||
            !preflight_account(value.descriptor.descriptor_kind.size()) ||
            !preflight_account(value.descriptor.canonical_type_name.size()) ||
            !preflight_account(value.descriptor.encoded_descriptor.size()) ||
            !preflight_account(value.encoded_value.size()) ||
            !preflight_account(value.binary_value.size())) {
          return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "graph exchange value preflight exceeded its resource contract");
        }
      }
    }
    if (preflight_cell_count > input.maximum_cells) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph exchange cell preflight exceeded its resource contract");
    }
  }
  if (provider_batch.ordered_row_identities.size() !=
      provider_batch.batch.rows.size()) {
    return Refuse(kModelTypedExchangeInvalid,
                  "model-family ordered row identity cardinality is incomplete");
  }
  std::uint64_t time_series_uniqueness_peak = 0;
  if (graph_family || time_series_family || vector_family) {
    std::uint64_t uniqueness_peak =
        3 * sizeof(std::unordered_set<std::string>);
    for (std::size_t identity_ordinal = 0;
         identity_ordinal < provider_batch.ordered_row_identities.size();
         ++identity_ordinal) {
      const auto& identity =
          provider_batch.ordered_row_identities[identity_ordinal];
      if (ExchangeCancellationRequested(cancellation_requested)) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "model-family uniqueness preflight was cancelled");
      }
      constexpr std::uint64_t kSetNodeOverhead =
          sizeof(std::string) + 4 * sizeof(void*) + 64;
      const auto dynamic = static_cast<std::uint64_t>(
          identity.row_uuid.size() + identity.vector_distance.size() +
          identity.vector_score.size() +
          (graph_family ? identity.path_uuid.size() : 0));
      const std::uint64_t node_count =
          graph_family
              ? 2
              : input.operation_id == "TIME_SERIES_DOWNSAMPLE" ? 0 : 1;
      if (node_count * kSetNodeOverhead >
              std::numeric_limits<std::uint64_t>::max() - uniqueness_peak ||
          dynamic > std::numeric_limits<std::uint64_t>::max() -
                        uniqueness_peak - node_count * kSetNodeOverhead) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph uniqueness preflight overflowed");
      }
      uniqueness_peak += node_count * kSetNodeOverhead + dynamic;
    }
    if (uniqueness_peak > input.maximum_memory_bytes) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph uniqueness validation exceeded its resource contract");
    }
    if (time_series_family) {
      time_series_uniqueness_peak = uniqueness_peak;
    }
  }
  if (time_series_family) {
    std::uint64_t maximum_tag_temporary = 0;
    for (const auto& identity : provider_batch.ordered_row_identities) {
      const std::uint64_t fixed = 3 * sizeof(std::string);
      const std::uint64_t tag_size =
          static_cast<std::uint64_t>(identity.tags.size());
      if (tag_size >
          (std::numeric_limits<std::uint64_t>::max() - fixed) / 3 - 1) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series tag exchange validation preflight overflowed");
      }
      maximum_tag_temporary = std::max<std::uint64_t>(
          maximum_tag_temporary,
          3 * (tag_size + 1) + fixed);
    }
    if (maximum_tag_temporary >
            std::numeric_limits<std::uint64_t>::max() -
                time_series_uniqueness_peak ||
        time_series_uniqueness_peak + maximum_tag_temporary >
            input.maximum_memory_bytes) {
      return Refuse(
          "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
          "time-series tag exchange validation exceeded its resource contract");
    }
  }
  {
    // These temporary uniqueness sets are destroyed before the normalized
    // output batch is copied, keeping their accounted peak disjoint.
    std::unordered_set<std::string> document_uuids;
    std::unordered_set<std::string> row_uuids;
    std::unordered_set<std::string> path_uuids;
    for (std::size_t identity_ordinal = 0;
         identity_ordinal < provider_batch.ordered_row_identities.size();
         ++identity_ordinal) {
      const auto& identity =
          provider_batch.ordered_row_identities[identity_ordinal];
      if (ExchangeCancellationRequested(cancellation_requested)) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "model-family row identity validation was cancelled");
      }
      bool tag_cancellation_observed = false;
      const bool canonical_time_series_tags =
          !time_series_family ||
          CanonicalTimeSeriesTags(identity.tags, cancellation_requested,
                                  &tag_cancellation_observed);
      if (tag_cancellation_observed) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "time-series tag exchange validation was cancelled");
      }
      const bool empty_time_series_payload =
          identity.time_series_payload_kind.empty() &&
          identity.time_series_raw_value.empty() &&
          identity.time_series_sample_count.empty() &&
          identity.time_series_aggregate_value.empty();
      const bool empty_vector_payload =
          identity.vector_distance.empty() && identity.vector_score.empty();
      const bool document_identity =
          !graph_family && !key_value_family && !time_series_family &&
          !vector_family &&
          CanonicalUuid(identity.document_uuid) &&
          CanonicalUuid(identity.row_uuid) &&
          document_uuids.insert(identity.document_uuid).second &&
          row_uuids.insert(identity.row_uuid).second &&
          identity.key.empty() &&
          identity.vertex_uuid.empty() && identity.edge_uuid.empty() &&
          identity.path_uuid.empty() && identity.graph_depth == 0 &&
          identity.series_uuid.empty() && identity.metric_uuid.empty() &&
          identity.tags.empty() && identity.point_timestamp_ns == 0 &&
          identity.bucket_start_ns == 0 && empty_time_series_payload &&
          empty_vector_payload;
      const bool graph_edge_identity =
          (input.operation_id == "GRAPH_MATCH" && identity.graph_depth == 0 &&
           identity.edge_uuid.empty()) ||
          (input.operation_id == "GRAPH_EXPAND" &&
           ((identity.graph_depth == 0 && identity.edge_uuid.empty()) ||
            (identity.graph_depth > 0 && CanonicalUuid(identity.edge_uuid))));
      const bool graph_identity =
          graph_family && identity.document_uuid.empty() &&
          CanonicalUuid(identity.row_uuid) &&
          CanonicalUuid(identity.vertex_uuid) && graph_edge_identity &&
          CanonicalUuid(identity.path_uuid) &&
          identity.key.empty() &&
          identity.series_uuid.empty() && identity.metric_uuid.empty() &&
          identity.tags.empty() && identity.point_timestamp_ns == 0 &&
          identity.bucket_start_ns == 0 && empty_time_series_payload &&
          empty_vector_payload &&
          row_uuids.insert(identity.row_uuid).second &&
          path_uuids.insert(identity.path_uuid).second;
      const bool key_value_identity =
          key_value_family && identity.document_uuid.empty() &&
          CanonicalUuid(identity.row_uuid) && WellFormedUtf8(identity.key) &&
          identity.vertex_uuid.empty() && identity.edge_uuid.empty() &&
          identity.path_uuid.empty() && identity.graph_depth == 0 &&
          identity.series_uuid.empty() && identity.metric_uuid.empty() &&
          identity.tags.empty() && identity.point_timestamp_ns == 0 &&
          identity.bucket_start_ns == 0 && empty_time_series_payload &&
          empty_vector_payload &&
          row_uuids.insert(identity.row_uuid).second &&
          document_uuids.insert(identity.key).second;
      const bool time_series_raw =
          time_series_family &&
          input.operation_id == "TIME_SERIES_RANGE_READ" &&
          identity.document_uuid.empty() && CanonicalUuid(identity.row_uuid) &&
          identity.vertex_uuid.empty() && identity.edge_uuid.empty() &&
          identity.path_uuid.empty() && identity.graph_depth == 0 &&
          identity.key.empty() && identity.series_uuid == input.object_uuid &&
          CanonicalUuid(identity.metric_uuid) && canonical_time_series_tags &&
          identity.bucket_start_ns == 0 &&
          identity.time_series_payload_kind == "raw.real64.v1" &&
          !identity.time_series_raw_value.empty() &&
          identity.time_series_sample_count.empty() &&
          identity.time_series_aggregate_value.empty() &&
          empty_vector_payload &&
          row_uuids.insert(identity.row_uuid).second;
      const bool time_series_bucket =
          time_series_family &&
          input.operation_id == "TIME_SERIES_BUCKET" &&
          identity.document_uuid.empty() && CanonicalUuid(identity.row_uuid) &&
          identity.vertex_uuid.empty() && identity.edge_uuid.empty() &&
          identity.path_uuid.empty() && identity.graph_depth == 0 &&
          identity.key.empty() && identity.series_uuid == input.object_uuid &&
          CanonicalUuid(identity.metric_uuid) && canonical_time_series_tags &&
          empty_time_series_payload && empty_vector_payload &&
          row_uuids.insert(identity.row_uuid).second;
      const bool time_series_downsample =
          time_series_family &&
          input.operation_id == "TIME_SERIES_DOWNSAMPLE" &&
          identity.document_uuid.empty() && identity.row_uuid.empty() &&
          identity.vertex_uuid.empty() && identity.edge_uuid.empty() &&
          identity.path_uuid.empty() && identity.graph_depth == 0 &&
          identity.key.empty() && identity.series_uuid == input.object_uuid &&
          CanonicalUuid(identity.metric_uuid) && canonical_time_series_tags &&
          identity.point_timestamp_ns == 0 &&
          identity.time_series_raw_value.empty() &&
          !identity.time_series_payload_kind.empty() &&
          !identity.time_series_sample_count.empty() &&
          !identity.time_series_aggregate_value.empty() &&
          empty_vector_payload;
      const bool vector_identity =
          vector_family && identity.document_uuid.empty() &&
          CanonicalUuid(identity.row_uuid) && identity.vertex_uuid.empty() &&
          identity.edge_uuid.empty() && identity.path_uuid.empty() &&
          identity.graph_depth == 0 && identity.key.empty() &&
          identity.series_uuid.empty() && identity.metric_uuid.empty() &&
          identity.tags.empty() && identity.point_timestamp_ns == 0 &&
          identity.bucket_start_ns == 0 && empty_time_series_payload &&
          CanonicalFiniteReal64(identity.vector_distance) &&
          CanonicalFiniteReal64(identity.vector_score) &&
          row_uuids.insert(identity.row_uuid).second;
      if (!document_identity && !graph_identity && !key_value_identity &&
          !time_series_raw && !time_series_bucket &&
          !time_series_downsample && !vector_identity) {
        return Refuse(
            kModelTypedExchangeInvalid,
            "model-family row uniqueness or ordering identity is invalid");
      }
      if (time_series_family && identity_ordinal != 0) {
        const auto& previous =
            provider_batch.ordered_row_identities[identity_ordinal - 1];
        const auto text_less = [](const std::string& left,
                                  const std::string& right) {
          return UnsignedUtf8Less(left, right);
        };
        const auto raw_less = [&](const auto& left, const auto& right) {
          if (left.series_uuid != right.series_uuid)
            return text_less(left.series_uuid, right.series_uuid);
          if (left.metric_uuid != right.metric_uuid)
            return text_less(left.metric_uuid, right.metric_uuid);
          if (left.point_timestamp_ns != right.point_timestamp_ns)
            return left.point_timestamp_ns < right.point_timestamp_ns;
          if (left.tags != right.tags) return text_less(left.tags, right.tags);
          return text_less(left.row_uuid, right.row_uuid);
        };
        const auto downsample_less = [&](const auto& left,
                                         const auto& right) {
          if (left.series_uuid != right.series_uuid)
            return text_less(left.series_uuid, right.series_uuid);
          if (left.metric_uuid != right.metric_uuid)
            return text_less(left.metric_uuid, right.metric_uuid);
          if (left.tags != right.tags) return text_less(left.tags, right.tags);
          return left.bucket_start_ns < right.bucket_start_ns;
        };
        const bool exact_order =
            input.operation_id == "TIME_SERIES_DOWNSAMPLE"
                ? downsample_less(previous, identity)
                : raw_less(previous, identity);
        if (!exact_order) {
          return Refuse(
              kModelTypedExchangeInvalid,
              "time-series ordered identities do not satisfy the exact receipt");
        }
      }
      if (vector_family && identity_ordinal != 0) {
        const auto& previous =
            provider_batch.ordered_row_identities[identity_ordinal - 1];
        double previous_distance = 0.0;
        double current_distance = 0.0;
        const auto previous_parse = std::from_chars(
            previous.vector_distance.data(),
            previous.vector_distance.data() + previous.vector_distance.size(),
            previous_distance, std::chars_format::general);
        const auto current_parse = std::from_chars(
            identity.vector_distance.data(),
            identity.vector_distance.data() + identity.vector_distance.size(),
            current_distance, std::chars_format::general);
        if (previous_parse.ec != std::errc{} || current_parse.ec != std::errc{} ||
            current_distance < previous_distance ||
            (current_distance == previous_distance &&
             !UnsignedUtf8Less(previous.row_uuid, identity.row_uuid))) {
          return Refuse(
              kModelTypedExchangeInvalid,
              "vector ordered identities do not satisfy distance/row UUID order");
        }
      }
    }
  }
  if (provider_batch.properties.abi_version != 1 ||
      provider_batch.properties.property_descriptor_id !=
          "SB_MODEL_PROPERTY_DESCRIPTOR_V1" ||
      !CanonicalUuid(provider_batch.properties.property_uuid) ||
      !provider_batch.properties.exact ||
      provider_batch.properties.partitioning_id !=
          "single_local_partition" ||
      provider_batch.properties.uniqueness_id !=
          (graph_family ? "path_uuid"
                        : key_value_family
                              ? "key"
                              : time_series_family
                                    ? (input.operation_id !=
                                               "TIME_SERIES_DOWNSAMPLE"
                                           ? "row_uuid"
                                           : "series_metric_tags_bucket_v1")
                                    : vector_family ? "row_uuid"
                                    : "document_uuid") ||
      provider_batch.properties.ordering_id !=
          (vector_family
               ? "vector_distance_row_uuid_ascending_v1"
               : input.operation_id == "TIME_SERIES_RANGE_READ" ||
                   input.operation_id == "TIME_SERIES_BUCKET"
               ? "series_metric_timestamp_tags_row_ascending_v1"
               : input.operation_id == "TIME_SERIES_DOWNSAMPLE"
                     ? "series_metric_tags_bucket_start_ascending_v1"
                     : input.operation_id == "KEY_VALUE_GET"
               ? "key_value_unordered_v1"
               : input.operation_id == "KEY_VALUE_MULTI_GET"
                     ? "first_distinct_request_order_v1"
                     : input.operation_id == "KEY_VALUE_PREFIX_RANGE"
                           ? "key_utf8_byte_ascending_v1"
                           : "fixture_order") ||
      !provider_batch.residual_recheck_complete ||
      !provider_batch.base_row_mga_recheck_complete ||
      !provider_batch.security_recheck_complete ||
      provider_batch.properties.residual_recheck_complete !=
          provider_batch.residual_recheck_complete ||
      provider_batch.properties.base_row_mga_recheck_complete !=
          provider_batch.base_row_mga_recheck_complete ||
      provider_batch.properties.security_recheck_complete !=
          provider_batch.security_recheck_complete) {
    return Refuse(kModelTypedExchangeInvalid,
                  "model-family exactness or recheck receipt is incomplete");
  }

  if (graph_family) {
    const auto validation = ValidateCanonicalDescriptorBatch(
        provider_batch.batch, input.output_descriptor_ids);
    if (!validation.ok) {
      return Refuse(kModelTypedExchangeInvalid, validation.detail);
    }
    const auto column_ordinal = [&](const std::string_view name)
        -> std::optional<std::size_t> {
      const auto column = std::ranges::find_if(
          provider_batch.batch.columns,
          [&](const auto& candidate) { return candidate.stable_name == name; });
      return column == provider_batch.batch.columns.end()
                 ? std::nullopt
                 : std::optional<std::size_t>(std::distance(
                       provider_batch.batch.columns.begin(), column));
    };
    const auto vertex_ordinal = column_ordinal("vertex_uuid");
    const auto row_ordinal_column = column_ordinal("row_uuid");
    const auto edge_ordinal = column_ordinal("edge_uuid");
    const auto path_ordinal = column_ordinal("path_uuid");
    const auto labels_ordinal = column_ordinal("vertex_labels");
    const auto vertex_properties_ordinal = column_ordinal("vertex_properties");
    const auto edge_properties_ordinal = column_ordinal("edge_properties");
    const auto direction_ordinal = column_ordinal("direction");
    const auto depth_ordinal = column_ordinal("depth");
    const auto cycle_ordinal = column_ordinal("cycle_policy");
    const auto exact_known_column = [&](const std::optional<std::size_t> ordinal,
                                        const std::string_view type,
                                        const bool nullable) {
      if (!ordinal.has_value()) return true;
      const auto& column = provider_batch.batch.columns[*ordinal];
      return column.descriptor.canonical_type_name == type &&
             column.nullable == nullable;
    };
    if (!exact_known_column(row_ordinal_column, "uuid", false) ||
        !exact_known_column(vertex_ordinal, "uuid", false) ||
        !exact_known_column(edge_ordinal, "uuid", true) ||
        !exact_known_column(path_ordinal, "uuid", false) ||
        !exact_known_column(labels_ordinal, "text", false) ||
        !exact_known_column(vertex_properties_ordinal, "text", false) ||
        !exact_known_column(edge_properties_ordinal, "text", false) ||
        !exact_known_column(direction_ordinal, "text", false) ||
        !exact_known_column(depth_ordinal, "uint64", false) ||
        !exact_known_column(cycle_ordinal, "text", false)) {
      return Refuse(kModelTypedExchangeInvalid,
                    "graph known-column descriptor contract drifted");
    }
    for (std::size_t row_ordinal = 0;
         row_ordinal < provider_batch.batch.rows.size(); ++row_ordinal) {
      const auto& row = provider_batch.batch.rows[row_ordinal];
      const auto& identity =
          provider_batch.ordered_row_identities[row_ordinal];
      if (std::ranges::any_of(row.values, [](const auto& value) {
            return value.state == internal_api::EngineValueState::missing;
          })) {
        return Refuse(kModelTypedExchangeInvalid,
                      "graph provider returned an unbound missing value");
      }
      const auto exact_uuid_value = [&](const std::optional<std::size_t> ordinal,
                                        const std::string& expected) {
        if (!ordinal.has_value()) return true;
        const auto& column = provider_batch.batch.columns[*ordinal];
        const auto& value = row.values[*ordinal];
        return column.descriptor.canonical_type_name == "uuid" &&
               value.state == internal_api::EngineValueState::value &&
               CanonicalUuid(value.encoded_value) &&
               value.encoded_value == expected && value.binary_value.empty();
      };
      if (!exact_uuid_value(row_ordinal_column, identity.row_uuid) ||
          !exact_uuid_value(vertex_ordinal, identity.vertex_uuid) ||
          !exact_uuid_value(path_ordinal, identity.path_uuid)) {
        return Refuse(kModelTypedExchangeInvalid,
                      "graph row UUID value differs from ordered identity");
      }
      if (edge_ordinal.has_value()) {
        const auto& column = provider_batch.batch.columns[*edge_ordinal];
        const auto& value = row.values[*edge_ordinal];
        const bool exact_edge =
            column.descriptor.canonical_type_name == "uuid" &&
            ((identity.graph_depth == 0 && column.nullable &&
              value.state == internal_api::EngineValueState::sql_null &&
              value.encoded_value.empty() && value.binary_value.empty() &&
              identity.edge_uuid.empty()) ||
             (identity.graph_depth > 0 &&
              value.state == internal_api::EngineValueState::value &&
              CanonicalUuid(value.encoded_value) &&
              value.encoded_value == identity.edge_uuid &&
              value.binary_value.empty()));
        if (!exact_edge) {
          return Refuse(kModelTypedExchangeInvalid,
                        "graph edge value differs from ordered identity");
        }
      }
      if (depth_ordinal.has_value()) {
        const auto& column = provider_batch.batch.columns[*depth_ordinal];
        const auto& value = row.values[*depth_ordinal];
        std::uint64_t parsed = 0;
        const auto converted = std::from_chars(
            value.encoded_value.data(),
            value.encoded_value.data() + value.encoded_value.size(), parsed);
        if (column.descriptor.canonical_type_name != "uint64" ||
            value.state != internal_api::EngineValueState::value ||
            !value.binary_value.empty() ||
            value.encoded_value.empty() || converted.ec != std::errc{} ||
            converted.ptr !=
                value.encoded_value.data() + value.encoded_value.size() ||
            parsed != identity.graph_depth) {
          return Refuse(kModelTypedExchangeInvalid,
                        "graph depth value differs from ordered identity");
        }
      }
      const auto exact_nonnull_text = [&](
          const std::optional<std::size_t> ordinal) {
        return !ordinal.has_value() ||
               (row.values[*ordinal].state ==
                    internal_api::EngineValueState::value &&
                row.values[*ordinal].binary_value.empty());
      };
      if (!exact_nonnull_text(labels_ordinal) ||
          !exact_nonnull_text(vertex_properties_ordinal) ||
          !exact_nonnull_text(edge_properties_ordinal) ||
          !exact_nonnull_text(direction_ordinal) ||
          !exact_nonnull_text(cycle_ordinal) ||
          (direction_ordinal.has_value() &&
           row.values[*direction_ordinal].encoded_value != "outgoing" &&
           row.values[*direction_ordinal].encoded_value != "incoming" &&
           row.values[*direction_ordinal].encoded_value != "both") ||
          (cycle_ordinal.has_value() &&
           row.values[*cycle_ordinal].encoded_value != "visited_set")) {
        return Refuse(kModelTypedExchangeInvalid,
                      "graph typed result value/state contract drifted");
      }
    }
  }

  if (key_value_family) {
    const auto validation = ValidateCanonicalDescriptorBatch(
        provider_batch.batch, input.output_descriptor_ids);
    if (!validation.ok || provider_batch.batch.columns.size() != 3) {
      return Refuse(kModelTypedExchangeInvalid,
                    validation.ok
                        ? "key/value public descriptor width is not three"
                        : validation.detail);
    }
    static constexpr std::string_view kNames[] = {
        "row_uuid", "key", "value"};
    static constexpr std::string_view kTypes[] = {"uuid", "text", "text"};
    for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
      const auto& column = provider_batch.batch.columns[ordinal];
      if (column.stable_name != kNames[ordinal] || column.nullable ||
          column.descriptor.canonical_type_name != kTypes[ordinal]) {
        return Refuse(kModelTypedExchangeInvalid,
                      "key/value public descriptor contract drifted");
      }
    }
    for (std::size_t ordinal = 0;
         ordinal < provider_batch.batch.rows.size(); ++ordinal) {
      if (ExchangeCancellationRequested(cancellation_requested)) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "time-series exchange row validation was cancelled");
      }
      const auto& row = provider_batch.batch.rows[ordinal];
      const auto& identity = provider_batch.ordered_row_identities[ordinal];
      if (row.values.size() != 3 ||
          row.values[0].state != internal_api::EngineValueState::value ||
          row.values[0].is_null || !row.values[0].binary_value.empty() ||
          row.values[0].encoded_value != identity.row_uuid ||
          row.values[1].state != internal_api::EngineValueState::value ||
          row.values[1].is_null || !row.values[1].binary_value.empty() ||
          row.values[1].encoded_value != identity.key ||
          !WellFormedUtf8(row.values[1].encoded_value) ||
          row.values[2].state != internal_api::EngineValueState::value ||
          row.values[2].is_null || !row.values[2].binary_value.empty() ||
          !WellFormedUtf8(row.values[2].encoded_value)) {
        return Refuse(kModelTypedExchangeInvalid,
                      "key/value row values differ from ordered identity");
      }
    }
  }

  if (vector_family) {
    // QOW-SOURCE-RCP-077-VECTOR-TYPED-EXCHANGE-V1
    const auto validation = ValidateCanonicalDescriptorBatch(
        provider_batch.batch, input.output_descriptor_ids);
    if (!validation.ok || provider_batch.batch.columns.size() != 3) {
      return Refuse(kModelTypedExchangeInvalid,
                    validation.ok
                        ? "vector public descriptor width is not three"
                        : validation.detail);
    }
    static constexpr std::array<std::string_view, 3> kNames{
        "row_uuid", "distance", "score"};
    static constexpr std::array<std::string_view, 3> kTypes{
        "uuid", "real64", "real64"};
    for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
      const auto& column = provider_batch.batch.columns[ordinal];
      if (column.stable_name != kNames[ordinal] || column.nullable ||
          column.descriptor.canonical_type_name != kTypes[ordinal] ||
          column.descriptor.descriptor_uuid.canonical.empty()) {
        return Refuse(kModelTypedExchangeInvalid,
                      "vector public descriptor contract drifted");
      }
    }
    if (provider_batch.batch.columns[0].descriptor.descriptor_uuid.canonical ==
            provider_batch.batch.columns[1].descriptor.descriptor_uuid.canonical ||
        provider_batch.batch.columns[0].descriptor.descriptor_uuid.canonical ==
            provider_batch.batch.columns[2].descriptor.descriptor_uuid.canonical ||
        provider_batch.batch.columns[1].descriptor.descriptor_uuid.canonical ==
            provider_batch.batch.columns[2].descriptor.descriptor_uuid.canonical) {
      return Refuse(kModelTypedExchangeInvalid,
                    "vector public descriptor identities are duplicated");
    }
    for (std::size_t ordinal = 0;
         ordinal < provider_batch.batch.rows.size(); ++ordinal) {
      if (ExchangeCancellationRequested(cancellation_requested)) {
        return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "vector exchange row validation was cancelled");
      }
      const auto& row = provider_batch.batch.rows[ordinal];
      const auto& identity = provider_batch.ordered_row_identities[ordinal];
      if (row.values.size() != 3 ||
          std::ranges::any_of(row.values, [](const auto& value) {
            return value.state != internal_api::EngineValueState::value ||
                   value.is_null || !value.binary_value.empty();
          }) ||
          row.values[0].encoded_value != identity.row_uuid ||
          !CanonicalUuid(row.values[0].encoded_value) ||
          row.values[1].encoded_value != identity.vector_distance ||
          !CanonicalFiniteReal64(row.values[1].encoded_value) ||
          row.values[2].encoded_value != identity.vector_score ||
          !CanonicalFiniteReal64(row.values[2].encoded_value)) {
        return Refuse(kModelTypedExchangeInvalid,
                      "vector row differs from its ordered exact identity");
      }
    }
  }

  if (time_series_family) {
    const auto validation = ValidateCanonicalDescriptorBatch(
        provider_batch.batch, input.output_descriptor_ids);
    const bool raw = input.operation_id == "TIME_SERIES_RANGE_READ";
    const bool bucket = input.operation_id == "TIME_SERIES_BUCKET";
    const std::size_t expected_width = raw ? 6 : (bucket ? 1 : 7);
    if (!validation.ok || provider_batch.batch.columns.size() != expected_width) {
      return Refuse(kModelTypedExchangeInvalid,
                    validation.ok
                        ? "time-series public descriptor width is invalid"
                        : validation.detail);
    }
    static constexpr std::string_view kRawNames[] = {
        "row_uuid", "series_uuid", "metric_uuid", "point_timestamp",
        "tags", "value"};
    static constexpr std::string_view kRawTypes[] = {
        "uuid", "uuid", "uuid", "timestamp_tz", "text", "real64"};
    static constexpr std::string_view kDownsampleNames[] = {
        "series_uuid", "metric_uuid", "bucket_start", "bucket_end", "tags",
        "sample_count", "aggregate_value"};
    static constexpr std::string_view kDownsampleTypes[] = {
        "uuid", "uuid", "timestamp_tz", "timestamp_tz", "text", "int64",
        "real64"};
    for (std::size_t ordinal = 0; ordinal < expected_width; ++ordinal) {
      const auto& column = provider_batch.batch.columns[ordinal];
      const auto expected_name = raw ? kRawNames[ordinal]
                                     : bucket
                                           ? std::string_view("bucket_start")
                                           : kDownsampleNames[ordinal];
      const auto expected_type = raw ? kRawTypes[ordinal]
                                     : bucket
                                           ? std::string_view("timestamp_tz")
                                           : kDownsampleTypes[ordinal];
      const bool aggregate_count_type =
          !raw && !bucket && ordinal == 6 &&
          column.descriptor.canonical_type_name == "int64";
      if (column.stable_name != expected_name || column.nullable ||
          (column.descriptor.canonical_type_name != expected_type &&
           !aggregate_count_type)) {
        return Refuse(kModelTypedExchangeInvalid,
                      "time-series public descriptor contract drifted");
      }
    }
    for (std::size_t ordinal = 0;
         ordinal < provider_batch.batch.rows.size(); ++ordinal) {
      const auto& row = provider_batch.batch.rows[ordinal];
      const auto& identity = provider_batch.ordered_row_identities[ordinal];
      if (row.values.size() != expected_width ||
          std::ranges::any_of(row.values, [](const auto& value) {
            return value.state != internal_api::EngineValueState::value ||
                   value.is_null || !value.binary_value.empty();
          })) {
        return Refuse(kModelTypedExchangeInvalid,
                      "time-series returned a null, missing, or binary cell");
      }
      const auto exact_utf8 = [&](const std::size_t cell,
                                  const std::string& expected) {
        return row.values[cell].encoded_value == expected &&
               WellFormedUtf8(row.values[cell].encoded_value);
      };
      if (raw) {
        std::int64_t timestamp_ns = 0;
        if (row.values[0].encoded_value != identity.row_uuid ||
            row.values[1].encoded_value != identity.series_uuid ||
            row.values[2].encoded_value != identity.metric_uuid ||
            !CanonicalTimestampNs(row.values[3].encoded_value, &timestamp_ns) ||
            timestamp_ns != identity.point_timestamp_ns ||
            !exact_utf8(4, identity.tags) ||
            !CanonicalFiniteReal64(row.values[5].encoded_value) ||
            identity.time_series_payload_kind != "raw.real64.v1" ||
            row.values[5].encoded_value !=
                identity.time_series_raw_value ||
            !identity.time_series_sample_count.empty() ||
            !identity.time_series_aggregate_value.empty()) {
          return Refuse(kModelTypedExchangeInvalid,
                        "time-series raw row differs from ordered identity");
        }
      } else if (bucket) {
        std::int64_t bucket_start_ns = 0;
        if (!CanonicalTimestampNs(row.values[0].encoded_value,
                                  &bucket_start_ns) ||
            bucket_start_ns != identity.bucket_start_ns) {
          return Refuse(
              kModelTypedExchangeInvalid,
              "time-series scalar bucket differs from ordered raw identity");
        }
      } else {
        std::int64_t bucket_start_ns = 0;
        std::int64_t bucket_end_ns = 0;
        std::int64_t sample_count = 0;
        const auto converted = std::from_chars(
            row.values[5].encoded_value.data(),
            row.values[5].encoded_value.data() +
                row.values[5].encoded_value.size(),
            sample_count);
        const bool count_payload =
            provider_batch.batch.columns[6].descriptor.canonical_type_name ==
            "int64";
        const bool exact_payload_kind =
            count_payload
                ? identity.time_series_payload_kind ==
                      "downsample.count.int64.v1"
                : identity.time_series_payload_kind ==
                          "downsample.sum.real64.v1" ||
                      identity.time_series_payload_kind ==
                          "downsample.min.real64.v1" ||
                      identity.time_series_payload_kind ==
                          "downsample.max.real64.v1" ||
                      identity.time_series_payload_kind ==
                          "downsample.avg.real64.v1";
        if (row.values[0].encoded_value != identity.series_uuid ||
            row.values[1].encoded_value != identity.metric_uuid ||
            !CanonicalTimestampNs(row.values[2].encoded_value,
                                  &bucket_start_ns) ||
            bucket_start_ns != identity.bucket_start_ns ||
            !CanonicalTimestampNs(row.values[3].encoded_value,
                                  &bucket_end_ns) ||
            bucket_end_ns <= bucket_start_ns || !exact_utf8(4, identity.tags) ||
            row.values[5].encoded_value.empty() ||
            converted.ec != std::errc{} ||
            converted.ptr != row.values[5].encoded_value.data() +
                                 row.values[5].encoded_value.size() ||
            sample_count <= 0 ||
            !CanonicalInt64(row.values[5].encoded_value, true) ||
            !identity.time_series_raw_value.empty() ||
            row.values[5].encoded_value !=
                identity.time_series_sample_count ||
            row.values[6].encoded_value !=
                identity.time_series_aggregate_value ||
            !exact_payload_kind ||
            (count_payload
                 ? !CanonicalInt64(row.values[6].encoded_value)
                 : !CanonicalFiniteReal64(row.values[6].encoded_value))) {
          return Refuse(kModelTypedExchangeInvalid,
                        "time-series downsample row differs from ordered identity");
        }
      }
    }
  }

  std::size_t cell_count = 0;
  std::uint64_t memory_bytes = sizeof(DescriptorBatch);
  const auto account_bytes = [&](const std::uint64_t bytes) {
    if (std::numeric_limits<std::uint64_t>::max() - memory_bytes < bytes) {
      return false;
    }
    memory_bytes += bytes;
    return true;
  };
  for (const auto& identity : provider_batch.ordered_row_identities) {
    if (ExchangeCancellationRequested(cancellation_requested)) {
      return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "model-family exchange output accounting was cancelled");
    }
    if (!account_bytes(sizeof(ModelProviderRowIdentityV1)) ||
        !account_bytes(identity.document_uuid.size()) ||
        !account_bytes(identity.row_uuid.size()) ||
        !account_bytes(identity.key.size()) ||
        !account_bytes(identity.vertex_uuid.size()) ||
        !account_bytes(identity.edge_uuid.size()) ||
        !account_bytes(identity.path_uuid.size()) ||
        !account_bytes(identity.series_uuid.size()) ||
        !account_bytes(identity.metric_uuid.size()) ||
        !account_bytes(identity.tags.size()) ||
        !account_bytes(identity.time_series_payload_kind.size()) ||
        !account_bytes(identity.time_series_raw_value.size()) ||
        !account_bytes(identity.time_series_sample_count.size()) ||
        !account_bytes(identity.time_series_aggregate_value.size()) ||
        !account_bytes(identity.vector_distance.size()) ||
        !account_bytes(identity.vector_score.size())) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange identity memory counter overflowed");
    }
  }
  for (const auto& column : provider_batch.batch.columns) {
    if (ExchangeCancellationRequested(cancellation_requested)) {
      return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "model-family descriptor output accounting was cancelled");
    }
    if (!account_bytes(sizeof(ExecutorColumnDescriptor)) ||
        !account_bytes(column.stable_name.size()) ||
        !account_bytes(column.descriptor.descriptor_uuid.canonical.size()) ||
        !account_bytes(column.descriptor.descriptor_kind.size()) ||
        !account_bytes(column.descriptor.canonical_type_name.size()) ||
        !account_bytes(column.descriptor.encoded_descriptor.size())) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange descriptor memory counter overflowed");
    }
  }
  for (const auto& row : provider_batch.batch.rows) {
    if (ExchangeCancellationRequested(cancellation_requested)) {
      return Refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "model-family row output accounting was cancelled");
    }
    if (!account_bytes(sizeof(DescriptorTuple))) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange row memory counter overflowed");
    }
    if (std::numeric_limits<std::size_t>::max() - cell_count <
        row.values.size()) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange cell counter overflowed");
    }
    cell_count += row.values.size();
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      const auto& value = row.values[column];
      if (!graph_family && !key_value_family && !time_series_family &&
          !vector_family &&
          value.state ==
              scratchbird::engine::internal_api::EngineValueState::missing) {
        if (column >= provider_batch.batch.columns.size() ||
            !provider_batch.batch.columns[column].nullable) {
          return Refuse(kModelDocumentMissingBindingRefused,
                        "missing document path has no nullable bound output descriptor");
        }
      } else if ((graph_family || key_value_family || time_series_family ||
                  vector_family) &&
                 value.state ==
                     scratchbird::engine::internal_api::EngineValueState::missing) {
        return Refuse(kModelTypedExchangeInvalid,
                      "model-family provider returned an unbound missing value");
      }
      if (!account_bytes(sizeof(internal_api::EngineTypedValue)) ||
          !account_bytes(value.descriptor.descriptor_uuid.canonical.size()) ||
          !account_bytes(value.descriptor.descriptor_kind.size()) ||
          !account_bytes(value.descriptor.canonical_type_name.size()) ||
          !account_bytes(value.descriptor.encoded_descriptor.size()) ||
          !account_bytes(value.encoded_value.size()) ||
          !account_bytes(value.binary_value.size())) {
        return Refuse(kModelTypedExchangeInvalid,
                      "document exchange memory counter overflowed");
      }
    }
  }
  if (provider_batch.batch.rows.size() > input.maximum_rows ||
      cell_count > input.maximum_cells ||
      memory_bytes > input.maximum_memory_bytes) {
    return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "document exchange exceeded its bound resource contract");
  }
  DescriptorBatch normalized;
  try {
    normalized = provider_batch.batch;
  } catch (const std::bad_alloc&) {
    return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "model-family exchange batch allocation was refused");
  }
  if (!graph_family && !key_value_family && !time_series_family &&
      !vector_family) {
    for (auto& row : normalized.rows) {
      for (auto& value : row.values) {
        if (value.state ==
            scratchbird::engine::internal_api::EngineValueState::missing) {
          value.encoded_value.clear();
          value.binary_value.clear();
          value.setState(
              scratchbird::engine::internal_api::EngineValueState::sql_null);
        }
      }
    }
  }
  const auto validation = ValidateCanonicalDescriptorBatch(
      normalized, input.output_descriptor_ids);
  if (!validation.ok) {
    return Refuse(kModelTypedExchangeInvalid, validation.detail);
  }

  ModelExchangeResultV1 result;
  result.accepted = true;
  result.root_publishable = true;
  result.output.family_id = input.family_id;
  result.output.operation_id = input.operation_id;
  result.output.object_uuid = input.object_uuid;
  result.output.physical_node_id = input.physical_node_id;
  result.output.selected_alternative_uuid = input.selected_alternative_uuid;
  result.output.capability_uuid = input.capability_uuid;
  result.output.provider_uuid = input.provider_uuid;
  result.output.provider_generation = input.provider_generation;
  result.output.result_handle_uuid = input.result_handle_uuid;
  result.output.causal_counter_id = input.causal_counter_id;
  result.output.output_descriptor_ids = input.output_descriptor_ids;
  result.output.ordered_row_identities =
      provider_batch.ordered_row_identities;
  result.output.batch = std::move(normalized);
  result.output.properties = provider_batch.properties;
  result.output.mga_statement_context = input.mga_statement_context;
  result.output.security_receipt_uuid = provider_batch.security_receipt_uuid;
  result.output.exact_exchange_validated = true;
  result.output.exact_fallback_selected = input.exact_fallback_selected;
  return result;
}

}  // namespace scratchbird::engine::executor
