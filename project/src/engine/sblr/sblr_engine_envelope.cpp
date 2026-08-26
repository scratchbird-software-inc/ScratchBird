// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"
#include "sblr_ddl_drop_sequence_runtime.hpp"
#include "sblr_ddl_alter_timeseries_value_cache_runtime.hpp"
#include "sblr_ddl_drop_timeseries_value_cache_runtime.hpp"
#include "sblr_ddl_alter_publication_runtime.hpp"
#include "sblr_sec_create_role_runtime.hpp"
#include "sblr_sec_drop_role_runtime.hpp"
#include "sblr_sec_create_policy_runtime.hpp"
#include "sblr_sec_drop_policy_runtime.hpp"
#include "sblr_sec_alter_role_runtime.hpp"
#include "sblr_sec_create_group_mapping_runtime.hpp"
#include "sblr_sec_drop_group_mapping_runtime.hpp"
#include "sblr_sec_grant_runtime.hpp"
#include "sblr_sec_revoke_runtime.hpp"
#include "sblr_sec_alter_policy_runtime.hpp"
#include "sblr_sec_drop_user_runtime.hpp"
#include "sblr_sec_authenticate_runtime.hpp"
#include "sblr_sec_deauthenticate_runtime.hpp"
#include "sblr_session_role_switch_runtime.hpp"
#include "sblr_session_setting_set_runtime.hpp"
#include "sblr_session_setting_reset_runtime.hpp"
#include "sblr_session_setting_get_runtime.hpp"
#include "sblr_session_default_qualifier_runtime.hpp"
#include "sblr_session_discard_runtime.hpp"
#include "sblr_session_snapshot_handle_runtime.hpp"
#include "sblr_context_set_runtime.hpp"
#include "sblr_context_unset_runtime.hpp"
#include "sblr_context_get_runtime.hpp"
#include "sblr_stmt_prepare_runtime.hpp"
#include "sblr_ddl_create_table_as_query_runtime.hpp"
#include "sblr_literal_runtime.hpp"
#include "sblr_parameter_runtime.hpp"
#include "sblr_variable_runtime.hpp"
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
#include "sblr_transaction_rollback_runtime.hpp"
#include "sblr_savepoint_runtime.hpp"
#include "sblr_autonomous_frame_runtime.hpp"
#include "sblr_reservation_release_runtime.hpp"
#include "sblr_temporary_instance_cleanup_runtime.hpp"
#include "sblr_cursor_open_runtime.hpp"
#include "sblr_cursor_fetch_runtime.hpp"
#include "sblr_cursor_close_runtime.hpp"
#include "sblr_read_by_key_runtime.hpp"
#include "sblr_read_range_runtime.hpp"
#include "sblr_read_stream_runtime.hpp"
#include "sblr_result_set_pass_runtime.hpp"
#include "sblr_access_cursor_open_runtime.hpp"
#include "sblr_access_cursor_fetch_runtime.hpp"
#include "sblr_access_cursor_close_runtime.hpp"
#include "sblr_insert_runtime.hpp"
#include "sblr_update_runtime.hpp"
#include "sblr_delete_runtime.hpp"
#include "sblr_merge_runtime.hpp"
#include "sblr_table_truncate_runtime.hpp"
#include "sblr_table_analyze_runtime.hpp"
#include "sblr_ddl_drop_procedure_runtime.hpp"
#include "sblr_ddl_create_temporary_table_runtime.hpp"
#include "sblr_ddl_drop_temporary_table_runtime.hpp"
#include "sblr_ddl_rename_object_vector_runtime.hpp"
#include "sblr_ddl_rename_object_runtime.hpp"
#include "sblr_ddl_create_or_replace_srs_runtime.hpp"
#include "sblr_ddl_drop_srs_runtime.hpp"
#include "sblr_ddl_create_rewrite_rule_runtime.hpp"
#include "sblr_ddl_alter_rewrite_rule_runtime.hpp"
#include "sblr_ddl_drop_rewrite_rule_runtime.hpp"
#include "sblr_ddl_validate_constraint_runtime.hpp"
#include "sblr_security_create_privilege_template_runtime.hpp"
#include "sblr_sec_alter_user_runtime.hpp"
#include "sblr_security_alter_privilege_template_runtime.hpp"
#include "sblr_security_drop_privilege_template_runtime.hpp"
#include "sblr_database_create_template_clone_runtime.hpp"
#include "sblr_ddl_create_aggregate_runtime.hpp"
#include "sblr_ddl_alter_aggregate_runtime.hpp"
#include "sblr_ddl_drop_aggregate_runtime.hpp"
#include "sblr_ddl_purge_system_history_runtime.hpp"
#include "sblr_ddl_set_index_optimizer_eligibility_runtime.hpp"
#include "sblr_ddl_set_table_type_enforcement_runtime.hpp"
#include "sblr_database_serialize_logical_snapshot_runtime.hpp"
#include "sblr_database_deserialize_logical_snapshot_runtime.hpp"
#include "sblr_ddl_create_macro_runtime.hpp"
#include "sblr_ddl_drop_macro_runtime.hpp"
#include "sblr_admin_register_external_relation_resolver_runtime.hpp"
#include "sblr_admin_unregister_external_relation_resolver_runtime.hpp"
#include "sblr_ddl_create_dictionary_runtime.hpp"
#include "sblr_ddl_drop_dictionary_runtime.hpp"
#include "sblr_ddl_alter_dictionary_runtime.hpp"
#include "sblr_ddl_create_continuous_view_runtime.hpp"
#include "sblr_ddl_alter_continuous_view_runtime.hpp"
#include "sblr_ddl_drop_continuous_view_runtime.hpp"
#include "sblr_dml_async_insert_submit_runtime.hpp"
#include "sblr_dml_async_insert_status_runtime.hpp"
#include "sblr_dml_async_insert_cancel_runtime.hpp"
#include "sblr_dml_conditional_mutate_runtime.hpp"
#include "sblr_dml_counter_add_runtime.hpp"
#include "sblr_dml_timeseries_schema_write_runtime.hpp"
#include "sblr_ddl_timeseries_series_cardinality_policy_runtime.hpp"
#include "sblr_ddl_create_timeseries_value_cache_runtime.hpp"
#include "sblr_ddl_create_function_runtime.hpp"
#include "sblr_ddl_alter_function_runtime.hpp"
#include "sblr_ddl_drop_function_runtime.hpp"
#include "sblr_ddl_create_package_runtime.hpp"
#include "sblr_ddl_create_synonym_runtime.hpp"
#include "sblr_ddl_create_foreign_table_runtime.hpp"
#include "sblr_ddl_create_fdw_runtime.hpp"
#include "sblr_ddl_drop_fdw_runtime.hpp"
#include "sblr_security_create_user_runtime.hpp"
#include "sblr_ddl_drop_foreign_table_runtime.hpp"
#include "sblr_ddl_drop_synonym_runtime.hpp"
#include "sblr_ddl_drop_package_runtime.hpp"
#include "sblr_ddl_alter_package_runtime.hpp"
#include "sblr_ddl_create_sequence_runtime.hpp"
#include "sblr_ddl_alter_sequence_runtime.hpp"
#include "sblr_ddl_create_materialized_view_runtime.hpp"
#include "sblr_ddl_create_type_runtime.hpp"
#include "sblr_ddl_alter_type_runtime.hpp"
#include "sblr_ddl_drop_materialized_view_runtime.hpp"
#include "sblr_ddl_drop_type_runtime.hpp"
#include "sblr_ddl_refresh_materialized_view_runtime.hpp"
#include "sblr_bulk_import_stream_runtime.hpp"
#include "sblr_bulk_export_stream_runtime.hpp"
#include "sblr_statement_batch_runtime.hpp"
#include "sblr_atomic_cas_runtime.hpp"
#include "sblr_atomic_read_modify_write_runtime.hpp"
#include "sblr_advisory_lock_runtime.hpp"
#include "sblr_advisory_lock_release_runtime.hpp"
#include "sblr_function_call_runtime.hpp"
#include "sblr_operator_call_runtime.hpp"
#include "sblr_cast_runtime.hpp"
#include "sblr_compare_runtime.hpp"
#include "sblr_domain_operation_runtime.hpp"
#include "sblr_udr_invoke_runtime.hpp"
#include "sblr_procedure_invoke_runtime.hpp"
#include "sblr_function_invoke_runtime.hpp"
#include "sblr_aggregate_invoke_runtime.hpp"
#include "sblr_sequence_nextval_runtime.hpp"
#include "sblr_sequence_currval_runtime.hpp"
#include "sblr_sequence_setval_runtime.hpp"
#include "sblr_query_numeric_runtime.hpp"
#include "sblr_advanced_datatype_family_runtime.hpp"
#include "sblr_project_runtime.hpp"
#include "sblr_catalog_introspect_runtime.hpp"
#include "sblr_aggregate_runtime.hpp"
#include "sblr_group_runtime.hpp"
#include "sblr_sort_runtime.hpp"
#include "sblr_limit_runtime.hpp"
#include "sblr_window_runtime.hpp"
#include "sblr_return_result_set_runtime.hpp"
#include "sblr_kv_structured_read_runtime.hpp"
#include "sblr_kv_structured_mutate_runtime.hpp"
#include "sblr_kv_structured_scan_runtime.hpp"
#include "sblr_kv_structured_stream_read_runtime.hpp"
#include "sblr_kv_structured_stream_append_runtime.hpp"
#include "sblr_kv_structured_timeseries_runtime.hpp"
#include "sblr_system_config_set_runtime.hpp"
#include "sblr_ddl_create_domain_runtime.hpp"
#include "sblr_ddl_alter_domain_runtime.hpp"
#include "sblr_ddl_create_view_runtime.hpp"
#include "sblr_ddl_drop_view_runtime.hpp"
#include "sblr_ddl_create_trigger_runtime.hpp"
#include "sblr_ddl_alter_trigger_runtime.hpp"
#include "sblr_ddl_drop_trigger_runtime.hpp"
#include "sblr_ddl_create_procedure_runtime.hpp"
#include "sblr_ddl_alter_procedure_runtime.hpp"
#include "sblr_ddl_alter_view_runtime.hpp"
#include "sblr_ddl_drop_view_runtime.hpp"
#include "sblr_ddl_create_trigger_runtime.hpp"
#include "sblr_ddl_alter_view_runtime.hpp"
#include "sblr_ddl_create_schema_runtime.hpp"
#include "sblr_ddl_create_table_runtime.hpp"
#include "sblr_ddl_drop_table_runtime.hpp"
#include "sblr_ddl_create_index_runtime.hpp"
#include "sblr_ddl_drop_index_runtime.hpp"

#include "hash_digest.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kFormatMajor = 1;
constexpr std::uint16_t kFormatMinor = 0;
constexpr std::size_t kTrailerSize = 16;
constexpr std::array<std::uint16_t, kSblrOperationSectionCount> kSectionTags{
    0x0001, 0x0002, 0x0003, 0x0004, 0x0005,
    0x0006, 0x0007, 0x0008, 0x0009};
constexpr char kProvenanceDomain[] =
    "ScratchBird.SBOP.ProducerProvenance.V1\0";

SblrEnvelopeDiagnostic Diagnostic(std::string code, std::string message) {
  return SblrEnvelopeDiagnostic{std::move(code), std::move(message), true};
}

SblrDecodeResult DecodeFailure(std::string code, std::string message) {
  SblrDecodeResult result;
  result.diagnostics.push_back(Diagnostic(std::move(code), std::move(message)));
  return result;
}

void Append16(Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void Append32(Bytes* out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void Append64(Bytes* out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void Store16(Bytes* out, std::size_t offset, std::uint16_t value) {
  (*out)[offset] = static_cast<std::uint8_t>(value);
  (*out)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Store32(Bytes* out, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    (*out)[offset + (shift / 8)] = static_cast<std::uint8_t>(value >> shift);
  }
}

void Store64(Bytes* out, std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    (*out)[offset + (shift / 8)] = static_cast<std::uint8_t>(value >> shift);
  }
}

std::uint16_t Load16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t Load32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         static_cast<std::uint32_t>(data[1]) << 8 |
         static_cast<std::uint32_t>(data[2]) << 16 |
         static_cast<std::uint32_t>(data[3]) << 24;
}

std::uint64_t Load64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift != 64; shift += 8) {
    value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
  }
  return value;
}

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool Read16(std::uint16_t* value) {
    if (!Take(2, &last_)) return false;
    *value = Load16(last_);
    return true;
  }

  bool Read32(std::uint32_t* value) {
    if (!Take(4, &last_)) return false;
    *value = Load32(last_);
    return true;
  }

  bool Read64(std::uint64_t* value) {
    if (!Take(8, &last_)) return false;
    *value = Load64(last_);
    return true;
  }

  bool Take(std::size_t count, const std::uint8_t** value) {
    if (count > size_ - offset_) return false;
    *value = data_ + offset_;
    offset_ += count;
    return true;
  }

  std::size_t remaining() const { return size_ - offset_; }
  std::size_t offset() const { return offset_; }

 private:
  const std::uint8_t* data_ = nullptr;
  const std::uint8_t* last_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

bool IsValidUtf8(std::string_view value) {
  const auto* data = reinterpret_cast<const std::uint8_t*>(value.data());
  std::size_t i = 0;
  while (i < value.size()) {
    const std::uint8_t first = data[i];
    if (first <= 0x7f) {
      if (first == 0 || first < 0x20) return false;
      ++i;
      continue;
    }
    std::size_t width = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      width = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      width = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      width = 4;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (width > value.size() - i) return false;
    for (std::size_t j = 1; j < width; ++j) {
      if ((data[i + j] & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (data[i + j] & 0x3f);
    }
    if ((width == 3 && codepoint < 0x800) ||
        (width == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff) {
      return false;
    }
    i += width;
  }
  return true;
}

bool IsOperationKey(std::string_view value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  bool component_start = false;
  for (std::size_t i = 1; i < value.size(); ++i) {
    const char ch = value[i];
    if (ch == '.') {
      if (component_start || i + 1 == value.size()) return false;
      component_start = true;
      continue;
    }
    if (component_start) {
      if (ch < 'a' || ch > 'z') return false;
      component_start = false;
      continue;
    }
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
      return false;
    }
  }
  return true;
}

bool IsCanonicalOperandName(const SblrOperand& operand) {
  if (operand.value_kind != SblrValueKind::expression_node_ref &&
      operand.value_kind != SblrValueKind::parameter_node_ref &&
      operand.value_kind != SblrValueKind::variable_node_ref) {
    return IsOperationKey(operand.name, 256);
  }
  // SBLR-RELATIONAL-EXPRESSION-LITERAL-REFERENCE-V1 binds the reference to
  // the relational DAG's numeric expression identity.  Its canonical text is
  // unsigned decimal without sign, leading zero, whitespace, or overflow.
  if (operand.name.empty() || operand.name.size() > 20 ||
      operand.name.front() < '1' || operand.name.front() > '9') {
    return false;
  }
  std::uint64_t value = 0;
  for (const char ch : operand.name) {
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  return value != 0;
}

bool IsOpcodeMnemonic(std::string_view value) {
  if (value.empty() || value.size() > 256 ||
      value.front() < 'A' || value.front() > 'Z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
  });
}

void AppendText(Bytes* out, std::string_view value) {
  Append32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool DecodeText(const std::uint8_t* data,
                std::size_t size,
                std::size_t maximum,
                std::string* value) {
  if (size < 4) return false;
  const std::uint32_t count = Load32(data);
  if (count == 0 || count > maximum || size != static_cast<std::size_t>(count) + 4) {
    return false;
  }
  std::string decoded(reinterpret_cast<const char*>(data + 4), count);
  if (!IsValidUtf8(decoded)) return false;
  *value = std::move(decoded);
  return true;
}

int HexNibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

bool ParseUuid(std::string_view text, std::array<std::uint8_t, 16>* uuid) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return false;
  }
  std::size_t out = 0;
  bool nonzero = false;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '-') {
      ++i;
      continue;
    }
    if (i + 1 >= text.size()) return false;
    const int high = HexNibble(text[i]);
    const int low = HexNibble(text[i + 1]);
    if (high < 0 || low < 0 || out == uuid->size()) return false;
    (*uuid)[out] = static_cast<std::uint8_t>((high << 4) | low);
    nonzero = nonzero || (*uuid)[out] != 0;
    ++out;
    i += 2;
  }
  return out == uuid->size() && nonzero;
}

bool IsNonzeroUuidBytes(const std::uint8_t* data) {
  for (std::size_t i = 0; i < 16; ++i) {
    if (data[i] != 0) return true;
  }
  return false;
}

std::string FormatUuid(const std::uint8_t* uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(kHex[uuid[i] >> 4]);
    text.push_back(kHex[uuid[i] & 0x0f]);
  }
  return text;
}

bool ValidateValueBody(SblrValueKind kind,
                       const std::uint8_t* data,
                       std::size_t size,
                       std::uint32_t depth,
                       std::uint64_t* value_count,
                       bool* limit_exceeded);

bool ValidateNestedValue(Reader* reader,
                         std::uint32_t depth,
                         std::uint64_t* value_count,
                         bool* limit_exceeded) {
  std::uint16_t raw_kind = 0;
  std::uint16_t flags = 0;
  std::uint64_t size = 0;
  if (!reader->Read16(&raw_kind) || !reader->Read16(&flags) ||
      !reader->Read64(&size) || flags != 0 || size > reader->remaining()) {
    return false;
  }
  const std::uint8_t* body = nullptr;
  if (!reader->Take(static_cast<std::size_t>(size), &body)) return false;
  return ValidateValueBody(static_cast<SblrValueKind>(raw_kind), body,
                           static_cast<std::size_t>(size), depth,
                           value_count, limit_exceeded);
}

bool ValidateValueBody(SblrValueKind kind,
                       const std::uint8_t* data,
                       std::size_t size,
                       std::uint32_t depth,
                       std::uint64_t* value_count,
                       bool* limit_exceeded) {
  if (depth > kSblrOperationMaximumDepth ||
      ++(*value_count) > kSblrOperationMaximumValues) {
    *limit_exceeded = true;
    return false;
  }
  switch (kind) {
    case SblrValueKind::uuid_ref:
    case SblrValueKind::policy_ref:
    case SblrValueKind::principal_ref:
    case SblrValueKind::udr_ref:
      return size == 16 && IsNonzeroUuidBytes(data);
    case SblrValueKind::descriptor_ref:
      // Core descriptor carriers use fixed-size descriptor bodies; envelope-level
      // validation narrows 320/384-byte forms to their exact operations.
      return (size == 16 && data != nullptr && IsNonzeroUuidBytes(data)) ||
             size == 320 || size == 384;
    case SblrValueKind::literal_typed:
    case SblrValueKind::proof_token: {
      if (size < 24 || !IsNonzeroUuidBytes(data)) return false;
      const std::uint64_t count = Load64(data + 16);
      if (count > kSblrOperationMaximumScalarBytes) {
        *limit_exceeded = true;
        return false;
      }
      return count == size - 24;
    }
    case SblrValueKind::parameter_slot:
    case SblrValueKind::result_target:
      return size == 20 && Load32(data) != 0 && IsNonzeroUuidBytes(data + 4);
    case SblrValueKind::epoch_token:
      return size == 12 && Load16(data) != 0 && Load16(data + 2) == 0;
    case SblrValueKind::profile_ref:
      return size == 24 && IsNonzeroUuidBytes(data);
    case SblrValueKind::artifact_ref:
      if (size < 29 || !IsNonzeroUuidBytes(data)) return false;
      if (data[24] == 1) return size == 29;
      if (data[24] == 2) return size == 57;
      return false;
    case SblrValueKind::list: {
      if (size < 4) return false;
      Reader reader(data, size);
      std::uint32_t count = 0;
      if (!reader.Read32(&count)) return false;
      if (count > kSblrOperationMaximumValues - *value_count) {
        *limit_exceeded = true;
        return false;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        if (!ValidateNestedValue(&reader, depth + 1, value_count, limit_exceeded)) {
          return false;
        }
      }
      return reader.remaining() == 0;
    }
    case SblrValueKind::map: {
      if (size < 4) return false;
      Reader reader(data, size);
      std::uint32_t count = 0;
      if (!reader.Read32(&count)) return false;
      if (count > kSblrOperationMaximumValues - *value_count) {
        *limit_exceeded = true;
        return false;
      }
      std::string previous;
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key_size = 0;
        if (!reader.Read32(&key_size) || key_size == 0 || key_size > 256 ||
            key_size > reader.remaining()) {
          return false;
        }
        const std::uint8_t* key_data = nullptr;
        if (!reader.Take(key_size, &key_data)) return false;
        std::string key(reinterpret_cast<const char*>(key_data), key_size);
        if (!IsValidUtf8(key) ||
            (!previous.empty() &&
             !std::lexicographical_compare(previous.begin(), previous.end(),
                                           key.begin(), key.end(),
                                           [](unsigned char lhs, unsigned char rhs) {
                                             return lhs < rhs;
                                           }))) {
          return false;
        }
        previous = std::move(key);
        if (!ValidateNestedValue(&reader, depth + 1, value_count, limit_exceeded)) {
          return false;
        }
      }
      return reader.remaining() == 0;
    }
    case SblrValueKind::null_value:
      return size == 0;
    case SblrValueKind::expression_node_table:
      return DecodeSblrExpressionNodeTableV1(data, size).ok;
    case SblrValueKind::expression_node_ref: {
      SblrExpressionNodeReferenceV1 reference;
      return DecodeSblrExpressionNodeReferenceV1(data, size, &reference);
    }
    case SblrValueKind::parameter_node_table:
      return DecodeSblrParameterNodeTableV1(data, size).ok;
    case SblrValueKind::parameter_node_ref: {
      SblrParameterNodeReferenceV1 reference;
      return DecodeSblrParameterNodeReferenceV1(data, size, &reference);
    }
    case SblrValueKind::variable_node_table:
      return DecodeSblrVariableNodeTableV1(data, size).ok;
    case SblrValueKind::variable_node_ref: {
      SblrVariableNodeReferenceV1 reference;
      return DecodeSblrVariableNodeReferenceV1(data, size, &reference);
    }
    case SblrValueKind::transaction_begin_options: {
      SblrTransactionBeginOptionsV1 options;
      std::string detail;
      return DecodeSblrTransactionBeginOptionsV1(data, size, &options,
                                                 &detail);
    }
    case SblrValueKind::transaction_commit_options: {
      SblrTransactionCommitOptionsV1 options;
      std::string detail;
      return DecodeSblrTransactionCommitOptionsV1(data, size, &options,
                                                  &detail);
    }
    case SblrValueKind::transaction_rollback_options: { SblrTransactionRollbackOptionsV1 options; std::string detail; return DecodeSblrTransactionRollbackOptionsV1(data,size,&options,&detail); }
    case SblrValueKind::savepoint_descriptor: { SblrSavepointDescriptorV1 descriptor; std::string detail; return DecodeSblrSavepointDescriptorV1(data,size,&descriptor,&detail); }
    case SblrValueKind::savepoint_release_handle: { SblrSavepointReleaseOperandV1 operand; std::string detail; return DecodeSblrSavepointReleaseOperandV1(data,size,&operand,&detail); }
    case SblrValueKind::savepoint_rollback_handle: { SblrSavepointRollbackOperandV1 operand; std::string detail; return DecodeSblrSavepointRollbackOperandV1(data,size,&operand,&detail); }
    case SblrValueKind::psql_autonomous_frame_descriptor: { SblrAutonomousFrameDescriptorV1 descriptor; std::string detail; return DecodeSblrAutonomousFrameDescriptorV1(data,size,&descriptor,&detail,true); }
    case SblrValueKind::relation_reservation_release_descriptor: { SblrReservationReleaseDescriptorV1 descriptor; std::string detail; return DecodeSblrReservationReleaseDescriptorV1(data,size,&descriptor,&detail,true); }
    case SblrValueKind::temporary_instance_cleanup_descriptor: { SblrTemporaryInstanceCleanupDescriptorV1 descriptor; std::string detail; return DecodeSblrTemporaryInstanceCleanupDescriptorV1(data,size,&descriptor,&detail,true); }
    case SblrValueKind::cursor_open_plan_ref: { SblrCursorOpenDescriptorV1 descriptor; std::string detail; return DecodeSblrCursorOpenDescriptorV1(data,size,&descriptor,&detail,true); }
    case SblrValueKind::cursor_fetch_handle: { SblrCursorFetchOperandV1 operand; std::string detail; return DecodeSblrCursorFetchOperandV1(data,size,&operand,&detail); }
    case SblrValueKind::cursor_close_handle: { SblrCursorCloseOperandV1 operand; std::string detail; return DecodeSblrCursorCloseOperandV1(data,size,&operand,&detail); }
    case SblrValueKind::read_by_key_descriptor: { SblrReadByKeyDescriptorV1 operand; std::string detail; return DecodeSblrReadByKeyDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::read_range_descriptor: { SblrReadRangeDescriptorV1 operand; std::string detail; return DecodeSblrReadRangeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::read_stream_descriptor: { SblrReadStreamDescriptorV1 operand; std::string detail; return DecodeSblrReadStreamDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::result_set_pass_descriptor: { SblrResultSetPassDescriptorV1 operand; std::string detail; return DecodeSblrResultSetPassDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::access_cursor_open_descriptor: { SblrAccessCursorOpenDescriptorV1 operand; std::string detail; return DecodeSblrAccessCursorOpenDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::access_cursor_fetch_descriptor: { SblrAccessCursorFetchDescriptorV1 operand; std::string detail; return DecodeSblrAccessCursorFetchDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::access_cursor_close_descriptor: { SblrAccessCursorCloseDescriptorV1 operand; std::string detail; return DecodeSblrAccessCursorCloseDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::insert_descriptor: { SblrInsertDescriptorV1 operand; std::string detail; return DecodeSblrInsertDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::update_descriptor: { SblrUpdateDescriptorV1 operand; std::string detail; return DecodeSblrUpdateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::delete_descriptor: { SblrDeleteDescriptorV1 operand; std::string detail; return DecodeSblrDeleteDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::merge_descriptor: { SblrMergeDescriptorV1 operand; std::string detail; return DecodeSblrMergeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::truncate_table_descriptor: { SblrTableTruncateDescriptorV1 operand; std::string detail; return DecodeSblrTableTruncateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::analyze_table_descriptor: { SblrTableAnalyzeDescriptorV1 operand; std::string detail; return DecodeSblrTableAnalyzeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::privilege_template_descriptor: { SblrSecurityCreatePrivilegeTemplateDescriptorV1 operand; std::string detail; return DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::privilege_template_alter_descriptor: { SblrSecurityAlterPrivilegeTemplateDescriptorV1 operand; std::string detail; return DecodeSblrSecurityAlterPrivilegeTemplateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::privilege_template_drop_descriptor: { SblrSecurityDropPrivilegeTemplateDescriptorV1 operand; std::string detail; return DecodeSblrSecurityDropPrivilegeTemplateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::bulk_import_stream_descriptor: { SblrBulkImportStreamDescriptorV1 operand; std::string detail; return DecodeSblrBulkImportStreamDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::bulk_export_stream_descriptor: { SblrBulkExportStreamDescriptorV1 operand; std::string detail; return DecodeSblrBulkExportStreamDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::statement_batch_descriptor: { SblrStatementBatchDescriptorV1 operand; std::string detail; return DecodeSblrStatementBatchDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::atomic_cas_descriptor: { SblrAtomicCasDescriptorV1 operand; std::string detail; return DecodeSblrAtomicCasDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::atomic_rmw_descriptor: { SblrAtomicRmwDescriptorV1 operand; std::string detail; return DecodeSblrAtomicRmwDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::advisory_lock_descriptor: { SblrAdvisoryLockDescriptorV1 operand; std::string detail; return DecodeSblrAdvisoryLockDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::advisory_lock_release_descriptor: { SblrAdvisoryLockReleaseDescriptorV1 operand; std::string detail; return DecodeSblrAdvisoryLockReleaseDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::function_call_descriptor: { SblrFunctionCallDescriptorV1 operand; std::string detail; return DecodeSblrFunctionCallDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::operator_call_descriptor: { SblrOperatorCallDescriptorV1 operand; std::string detail; return DecodeSblrOperatorCallDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::cast_descriptor: { SblrCastDescriptorV1 operand; std::string detail; return DecodeSblrCastDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::comparison_descriptor: { SblrCompareDescriptorV1 operand; std::string detail; return DecodeSblrCompareDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::domain_operation_descriptor: { SblrDomainOperationDescriptorV1 operand; std::string detail; return DecodeSblrDomainOperationDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::registered_cpp_udr_invocation: { SblrUdrInvokeDescriptorV1 operand; std::string detail; return DecodeSblrUdrInvokeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::procedure_invoke_descriptor: { SblrProcedureInvokeDescriptorV1 operand; std::string detail; return DecodeSblrProcedureInvokeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::function_invoke_descriptor: { SblrFunctionInvokeDescriptorV1 operand; std::string detail; return DecodeSblrFunctionInvokeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::aggregate_invoke_descriptor: { SblrAggregateInvokeDescriptorV1 operand; std::string detail; return DecodeSblrAggregateInvokeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::sequence_nextval_descriptor: { SblrSequenceNextvalDescriptorV1 operand; std::string detail; return DecodeSblrSequenceNextvalDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::sequence_currval_descriptor: { SblrSequenceCurrvalDescriptorV1 operand; std::string detail; return DecodeSblrSequenceCurrvalDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::sequence_setval_descriptor: { SblrSequenceSetvalDescriptorV1 operand; std::string detail; return DecodeSblrSequenceSetvalDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::numeric_operation_descriptor: { SblrQueryNumericDescriptorV1 operand; std::string detail; return DecodeSblrQueryNumericDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::advanced_datatype_family_descriptor: { SblrAdvancedDatatypeFamilyDescriptorV1 operand; std::string detail; return DecodeSblrAdvancedDatatypeFamilyDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::projection_descriptor: { SblrProjectDescriptorV1 operand; std::string detail; return DecodeSblrProjectDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::aggregate_descriptor: { SblrAggregateDescriptorV1 operand; std::string detail; return DecodeSblrAggregateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::ddl_create_aggregate_descriptor: { SblrDdlCreateAggregateDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateAggregateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::ddl_alter_aggregate_descriptor: { SblrDdlAlterAggregateDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterAggregateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::ddl_drop_aggregate_descriptor: { SblrDdlDropAggregateDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropAggregateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::system_history_purge_descriptor: { SblrDdlPurgeSystemHistoryDescriptorV1 operand; std::string detail; return DecodeSblrDdlPurgeSystemHistoryDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::index_optimizer_eligibility_descriptor: { SblrDdlSetIndexOptimizerEligibilityDescriptorV1 operand; std::string detail; return DecodeSblrDdlSetIndexOptimizerEligibilityDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::table_type_enforcement_descriptor: { SblrDdlSetTableTypeEnforcementDescriptorV1 operand; std::string detail; return DecodeSblrDdlSetTableTypeEnforcementDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::logical_snapshot_serialization_descriptor: { SblrDatabaseSerializeLogicalSnapshotDescriptorV1 operand; std::string detail; return DecodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::logical_snapshot_deserialization_descriptor: { SblrDatabaseDeserializeLogicalSnapshotDescriptorV1 operand; std::string detail; return DecodeSblrDatabaseDeserializeLogicalSnapshotDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::macro_descriptor: { SblrDdlCreateMacroDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateMacroDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::macro_drop_descriptor: { SblrDdlDropMacroDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropMacroDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::external_relation_resolver_registration_descriptor: { SblrAdminRegisterExternalRelationResolverDescriptorV1 operand; std::string detail; return DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::external_relation_resolver_unregistration_descriptor: { SblrAdminUnregisterExternalRelationResolverDescriptorV1 operand; std::string detail; return DecodeSblrAdminUnregisterExternalRelationResolverDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::external_dictionary_descriptor: { SblrDdlCreateDictionaryDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateDictionaryDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::external_dictionary_drop_descriptor: { SblrDdlDropDictionaryDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropDictionaryDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::external_dictionary_alter_descriptor: { SblrDdlAlterDictionaryDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterDictionaryDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::continuous_view_descriptor: { SblrDdlCreateContinuousViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateContinuousViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::continuous_view_alter_descriptor: { SblrDdlAlterContinuousViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterContinuousViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::continuous_view_drop_descriptor: { SblrDdlDropContinuousViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropContinuousViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::async_insert_submission_descriptor: { SblrDmlAsyncInsertSubmitDescriptorV1 operand; std::string detail; return DecodeSblrDmlAsyncInsertSubmitDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::async_insert_status_descriptor: { SblrDmlAsyncInsertStatusDescriptorV1 operand; std::string detail; return DecodeSblrDmlAsyncInsertStatusDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::async_insert_cancel_descriptor: { SblrDmlAsyncInsertCancelDescriptorV1 operand; std::string detail; return DecodeSblrDmlAsyncInsertCancelDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::conditional_mutation_descriptor: { SblrDmlConditionalMutateDescriptorV1 operand; std::string detail; return DecodeSblrDmlConditionalMutateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::counter_delta_descriptor: { SblrDmlCounterAddDescriptorV1 operand; std::string detail; return DecodeSblrDmlCounterAddDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::timeseries_schema_write_descriptor: { SblrDmlTimeseriesSchemaWriteDescriptorV1 operand; std::string detail; return DecodeSblrDmlTimeseriesSchemaWriteDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::timeseries_series_cardinality_policy_descriptor: { SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1 operand; std::string detail; return DecodeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::timeseries_value_cache_descriptor: { SblrDdlCreateTimeseriesValueCacheDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::timeseries_value_cache_alter_descriptor: { SblrDdlAlterTimeseriesValueCacheDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterTimeseriesValueCacheDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::timeseries_value_cache_drop_descriptor: { SblrDdlDropTimeseriesValueCacheDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropTimeseriesValueCacheDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::group_descriptor: { SblrGroupDescriptorV1 operand; std::string detail; return DecodeSblrGroupDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_create_group_mapping_descriptor: { SblrSecCreateGroupMappingDescriptorV1 operand; std::string detail; return DecodeSblrSecCreateGroupMappingDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_drop_group_mapping_descriptor: { SblrSecDropGroupMappingDescriptorV1 operand; std::string detail; return DecodeSblrSecDropGroupMappingDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_grant_descriptor: { SblrSecGrantDescriptorV1 operand; std::string detail; return DecodeSblrSecGrantDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_revoke_descriptor: { SblrSecRevokeDescriptorV1 operand; std::string detail; return DecodeSblrSecRevokeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_alter_policy_descriptor: { SblrSecAlterPolicyDescriptorV1 operand; std::string detail; return DecodeSblrSecAlterPolicyDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_drop_user_descriptor: { SblrSecDropUserDescriptorV1 operand; std::string detail; return DecodeSblrSecDropUserDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_authenticate_descriptor: { SblrSecAuthenticateDescriptorV1 operand; std::string detail; return DecodeSblrSecAuthenticateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_deauthenticate_descriptor: { SblrSecDeauthenticateDescriptorV1 operand; std::string detail; return DecodeSblrSecDeauthenticateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::session_role_switch_descriptor: { SblrSessionRoleSwitchDescriptorV1 operand; std::string detail; return DecodeSblrSessionRoleSwitchDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::session_setting_set_descriptor: { SblrSessionSettingSetDescriptorV1 operand; std::string detail; return DecodeSblrSessionSettingSetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::session_setting_reset_descriptor: { SblrSessionSettingResetDescriptorV1 operand; std::string detail; return DecodeSblrSessionSettingResetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::session_setting_get_descriptor: { SblrSessionSettingGetDescriptorV1 operand; std::string detail; return DecodeSblrSessionSettingGetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::session_default_qualifier_set_descriptor: { SblrSessionDefaultQualifierSetDescriptorV1 operand; std::string detail; return DecodeSblrSessionDefaultQualifierSetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::session_discard_descriptor: { SblrSessionDiscardDescriptorV1 operand; std::string detail; return DecodeSblrSessionDiscardDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::session_snapshot_handle_descriptor: { SblrSessionSnapshotHandleDescriptorV1 operand; std::string detail; return DecodeSblrSessionSnapshotHandleDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::context_set_descriptor: { SblrContextSetDescriptorV1 operand; std::string detail; return DecodeSblrContextSetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::context_unset_descriptor: { SblrContextUnsetDescriptorV1 operand; std::string detail; return DecodeSblrContextUnsetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::context_get_descriptor: { SblrContextGetDescriptorV1 operand; std::string detail; return DecodeSblrContextGetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::stmt_prepare_descriptor: { SblrStmtPrepareDescriptorV1 operand; std::string detail; return DecodeSblrStmtPrepareDescriptorV1(data,size,&operand,&detail); }
    case SblrValueKind::sort_descriptor: { SblrSortDescriptorV1 operand; std::string detail; return DecodeSblrSortDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::limit_descriptor: { SblrLimitDescriptorV1 operand; std::string detail; return DecodeSblrLimitDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::window_descriptor: { SblrWindowDescriptorV1 operand; std::string detail; return DecodeSblrWindowDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::observability_show_version_descriptor:
      // SVDO v1 is a fixed, engine-issued identity descriptor.  The
      // canonical route carries no SQL text; bytes 0..3 are the SVDO magic,
      // bytes 4..5 are version 1, and the remaining bytes are reserved zero.
      return size == 64 && data[0] == 'S' && data[1] == 'V' &&
             data[2] == 'D' && data[3] == 'O' && data[4] == 1 &&
             data[5] == 0 && std::all_of(data + 6, data + size,
                                          [](std::uint8_t byte) {
                                            return byte == 0;
                                          });
    case SblrValueKind::catalog_introspect_descriptor: {
      SblrCatalogIntrospectDescriptorV1 operand; std::string detail;
      return DecodeSblrCatalogIntrospectDescriptorV1(data, size, &operand, &detail, true);
    }
    case SblrValueKind::result_set_return_descriptor: { SblrReturnResultSetDescriptorV1 operand; std::string detail; return DecodeSblrReturnResultSetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::kv_structured_read_descriptor: { SblrKvStructuredReadDescriptorV1 operand; std::string detail; return DecodeSblrKvStructuredReadDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::kv_structured_mutate_descriptor: { SblrKvStructuredMutateDescriptorV1 operand; std::string detail; return DecodeSblrKvStructuredMutateDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::kv_structured_scan_descriptor: { SblrKvStructuredScanDescriptorV1 operand; std::string detail; return DecodeSblrKvStructuredScanDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::kv_structured_stream_read_descriptor: { SblrKvStructuredStreamReadDescriptorV1 operand; std::string detail; return DecodeSblrKvStructuredStreamReadDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::kv_structured_stream_append_descriptor: { SblrKvStructuredStreamAppendDescriptorV1 operand; std::string detail; return DecodeSblrKvStructuredStreamAppendDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::kv_structured_timeseries_descriptor: { SblrKvStructuredTimeseriesDescriptorV1 operand; std::string detail; return DecodeSblrKvStructuredTimeseriesDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::system_config_set_descriptor: { SblrSystemConfigSetDescriptorV1 operand; std::string detail; return DecodeSblrSystemConfigSetDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_domain_descriptor: { SblrDdlCreateDomainDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateDomainDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_schema_descriptor: { SblrDdlCreateSchemaDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateSchemaDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_table_descriptor: { SblrDdlCreateTableDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateTableDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_table_descriptor: { SblrDdlDropTableDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropTableDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_table_as_query_with_data_descriptor: { SblrCreateTableAsQueryDescriptorV1 operand; std::string detail; return DecodeSblrCreateTableAsQueryDescriptorV1(data,size,&operand,&detail); }
    case SblrValueKind::create_table_as_query_with_no_data_descriptor: { SblrCreateTableAsQueryDescriptorV1 operand; std::string detail; return DecodeSblrCreateTableAsQueryDescriptorV1(data,size,&operand,&detail); }
    case SblrValueKind::create_index_descriptor: { SblrDdlCreateIndexDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateIndexDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_index_descriptor: { SblrDdlDropIndexDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropIndexDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_domain_descriptor: { SblrDdlAlterDomainDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterDomainDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_view_descriptor: { SblrDdlCreateViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_view_descriptor: { SblrDdlAlterViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_trigger_descriptor: { SblrDdlCreateTriggerDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateTriggerDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_trigger_descriptor: { SblrDdlAlterTriggerDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterTriggerDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_trigger_descriptor: { SblrDdlDropTriggerDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropTriggerDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_procedure_descriptor: { SblrDdlCreateProcedureDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateProcedureDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_procedure_descriptor: { SblrDdlAlterProcedureDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterProcedureDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_procedure_descriptor: { SblrDdlDropProcedureDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropProcedureDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_function_descriptor: { SblrDdlCreateFunctionDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateFunctionDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_function_descriptor: { SblrDdlDropFunctionDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropFunctionDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_package_descriptor: { SblrDdlCreatePackageDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreatePackageDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_synonym_descriptor: { SblrDdlCreateSynonymDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateSynonymDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_foreign_table_descriptor: { SblrDdlCreateForeignTableDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateForeignTableDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_fdw_descriptor: { SblrDdlCreateFdwDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateFdwDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_fdw_descriptor: { SblrDdlDropFdwDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropFdwDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_create_user_descriptor: { SblrSecurityCreateUserDescriptorV1 operand; std::string detail; return DecodeSblrSecurityCreateUserDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_create_role_descriptor: { SblrSecCreateRoleDescriptorV1 operand; std::string detail; return DecodeSblrSecCreateRoleDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_drop_role_descriptor: { SblrSecDropRoleDescriptorV1 operand; std::string detail; return DecodeSblrSecDropRoleDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_create_policy_descriptor: { SblrSecCreatePolicyDescriptorV1 operand; std::string detail; return DecodeSblrSecCreatePolicyDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_drop_policy_descriptor: { SblrSecDropPolicyDescriptorV1 operand; std::string detail; return DecodeSblrSecDropPolicyDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::security_alter_role_descriptor: { SblrSecAlterRoleDescriptorV1 operand; std::string detail; return DecodeSblrSecAlterRoleDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_user_descriptor: { SblrSecAlterUserDescriptorV1 operand; std::string detail; return DecodeSblrSecAlterUserDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_foreign_table_descriptor: { SblrDdlDropForeignTableDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropForeignTableDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_synonym_descriptor: { SblrDdlDropSynonymDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropSynonymDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_publication_descriptor: { SblrDdlAlterPublicationDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterPublicationDescriptorV1(data,size,&operand,&detail); }
    case SblrValueKind::drop_package_descriptor: { SblrDdlDropPackageDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropPackageDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_package_descriptor: { SblrDdlAlterPackageDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterPackageDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_sequence_descriptor: { SblrDdlCreateSequenceDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateSequenceDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_sequence_descriptor: { SblrDdlAlterSequenceDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterSequenceDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_sequence_descriptor: { SblrDdlDropSequenceDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropSequenceDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_materialized_view_descriptor: { SblrDdlCreateMaterializedViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateMaterializedViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_type_descriptor: { SblrDdlCreateTypeDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateTypeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_type_descriptor: { SblrDdlAlterTypeDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterTypeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_materialized_view_descriptor: { SblrDdlDropMaterializedViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropMaterializedViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_type_descriptor: { SblrDdlDropTypeDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropTypeDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::refresh_materialized_view_descriptor: { SblrDdlRefreshMaterializedViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlRefreshMaterializedViewDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::create_temporary_table_descriptor: { SblrDdlCreateTemporaryTableDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateTemporaryTableDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_temporary_table_descriptor: { SblrDdlDropTemporaryTableDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropTemporaryTableDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::object_rename_vector_descriptor: { SblrDdlRenameObjectVectorDescriptorV1 operand; std::string detail; return DecodeSblrDdlRenameObjectVectorDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::rename_object_descriptor: { SblrDdlRenameObjectDescriptorV1 operand; std::string detail; return DecodeSblrDdlRenameObjectDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::spatial_reference_system_descriptor: { SblrDdlCreateOrReplaceSrsDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateOrReplaceSrsDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::spatial_reference_system_drop_descriptor: { SblrDdlDropSrsDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropSrsDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::rewrite_rule_descriptor: { SblrDdlCreateRewriteRuleDescriptorV1 operand; std::string detail; return DecodeSblrDdlCreateRewriteRuleDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::rewrite_rule_alter_descriptor: { SblrDdlAlterRewriteRuleDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterRewriteRuleDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::rewrite_rule_drop_descriptor: { SblrDdlDropRewriteRuleDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropRewriteRuleDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::constraint_validation_descriptor: { SblrDdlValidateConstraintDescriptorV1 operand; std::string detail; return DecodeSblrDdlValidateConstraintDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::template_database_creation_descriptor: { SblrDatabaseCreateTemplateCloneDescriptorV1 operand; std::string detail; return DecodeSblrDatabaseCreateTemplateCloneDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::alter_function_descriptor: { SblrDdlAlterFunctionDescriptorV1 operand; std::string detail; return DecodeSblrDdlAlterFunctionDescriptorV1(data,size,&operand,&detail,true); }
    case SblrValueKind::drop_view_descriptor: { SblrDdlDropViewDescriptorV1 operand; std::string detail; return DecodeSblrDdlDropViewDescriptorV1(data,size,&operand,&detail,true); }
  }
  return false;
}

Bytes EncodeOperandVector(const std::vector<SblrOperand>& operands) {
  Bytes section;
  Append32(&section, static_cast<std::uint32_t>(operands.size()));
  for (const auto& operand : operands) {
    Append32(&section, operand.ordinal);
    AppendText(&section, operand.type);
    AppendText(&section, operand.name);
    Append16(&section, static_cast<std::uint16_t>(operand.value_kind));
    Append16(&section, operand.value_flags);
    Append64(&section, operand.value_body.size());
    section.insert(section.end(), operand.value_body.begin(), operand.value_body.end());
  }
  return section;
}

bool DecodeOperandVector(const std::uint8_t* data,
                         std::size_t size,
                         std::vector<SblrOperand>* operands,
                         bool* limit_exceeded) {
  Reader reader(data, size);
  std::uint32_t count = 0;
  if (!reader.Read32(&count)) return false;
  if (count > kSblrOperationMaximumOperands) {
    *limit_exceeded = true;
    return false;
  }
  operands->reserve(count);
  std::uint64_t value_count = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    SblrOperand operand;
    std::uint32_t type_size = 0;
    std::uint32_t slot_size = 0;
    std::uint16_t raw_kind = 0;
    std::uint64_t value_size = 0;
    if (!reader.Read32(&operand.ordinal) || operand.ordinal != index + 1 ||
        !reader.Read32(&type_size) || type_size == 0 || type_size > 256 ||
        type_size > reader.remaining()) {
      return false;
    }
    const std::uint8_t* text_data = nullptr;
    if (!reader.Take(type_size, &text_data)) return false;
    operand.type.assign(reinterpret_cast<const char*>(text_data), type_size);
    if (!IsValidUtf8(operand.type) || !IsOperationKey(operand.type, 256) ||
        !reader.Read32(&slot_size) || slot_size == 0 || slot_size > 256 ||
        slot_size > reader.remaining()) {
      return false;
    }
    if (!reader.Take(slot_size, &text_data)) return false;
    operand.name.assign(reinterpret_cast<const char*>(text_data), slot_size);
    if (!IsValidUtf8(operand.name) ||
        !reader.Read16(&raw_kind) || !reader.Read16(&operand.value_flags) ||
        !reader.Read64(&value_size) || operand.value_flags != 0 ||
        value_size > reader.remaining()) {
      return false;
    }
    operand.value_kind = static_cast<SblrValueKind>(raw_kind);
    if (!IsCanonicalOperandName(operand)) return false;
    const std::uint8_t* value_data = nullptr;
    if (!reader.Take(static_cast<std::size_t>(value_size), &value_data)) {
      return false;
    }
    const bool deferred_source_map_descriptor =
        operand.value_kind == SblrValueKind::descriptor_ref && value_size == 24 &&
        IsNonzeroUuidBytes(value_data) && Load64(value_data + 16) != 0;
    if (!deferred_source_map_descriptor &&
        !ValidateValueBody(operand.value_kind, value_data,
                           static_cast<std::size_t>(value_size), 1,
                           &value_count, limit_exceeded)) return false;
    operand.value_body.assign(value_data, value_data + value_size);
    operands->push_back(std::move(operand));
  }
  return reader.remaining() == 0;
}

Bytes ProducerIdentity(const SblrOperationEnvelope& envelope,
                       const std::array<std::uint8_t, 16>& uuid) {
  Bytes section(uuid.begin(), uuid.end());
  Append32(&section, envelope.parser_package_version_major);
  Append32(&section, envelope.parser_package_version_minor);
  Append32(&section, envelope.parser_package_version_patch);
  return section;
}

std::array<std::uint8_t, 32> ProvenanceDigest(
    const SblrOperationEnvelope& envelope,
    const Bytes& producer,
    const Bytes& registry,
    const Bytes& operation_key,
    const Bytes& mnemonic,
    bool* ok) {
  Bytes input;
  input.insert(input.end(), std::begin(kProvenanceDomain),
               std::end(kProvenanceDomain) - 1);
  input.insert(input.end(), producer.begin(), producer.end());
  input.insert(input.end(), registry.begin(), registry.end());
  Append16(&input, envelope.opcode_code);
  Append16(&input, envelope.operation_version_major);
  Append16(&input, envelope.operation_version_minor);
  input.insert(input.end(), operation_key.begin(), operation_key.end());
  input.insert(input.end(), mnemonic.begin(), mnemonic.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(input);
  *ok = digest.ok();
  return digest.digest;
}

bool HasSourceArtifactMetadata(const SblrSourceArtifactMap& map) {
  return map.policy_status != "absent" || !map.source_identity.empty() ||
         !map.source_hash.empty() || !map.symbols.empty() ||
         !map.operation_render_hints.empty() || map.contains_sql_text ||
         map.raw_sql_text_authoritative || !map.render_metadata_only;
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
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4) & 0x0f] << kHex[ch & 0x0f];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

}  // namespace

std::uint32_t SblrCrc32c(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

SblrOperationEnvelope MakeSblrEnvelope(std::string operation_id,
                                       std::string opcode,
                                       std::string trace_key) {
  SblrOperationEnvelope envelope;
  envelope.operation_id = std::move(operation_id);
  envelope.opcode = std::move(opcode);
  envelope.trace_key = std::move(trace_key);
  if (envelope.operation_id == "engine.op.ddl_alter_rewrite_rule") {
    envelope.requires_transaction_context = false;
    envelope.requires_security_context = false;
  }
  envelope.result_shape = "engine.api.result.v1";
  envelope.diagnostic_shape = "engine.diagnostic.v1";
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

SblrEnvelopeValidationResult ValidateSblrEnvelope(const SblrOperationEnvelope& envelope) {
  SblrEnvelopeValidationResult result;
  result.ok = true;
  const auto fail = [&result](std::string code, std::string message) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic(std::move(code), std::move(message)));
  };

  if (envelope.envelope_major != kEngineSblrEnvelopeMajor ||
      envelope.envelope_minor != kEngineSblrEnvelopeMinor ||
      envelope.operation_version_major != 1 || envelope.operation_version_minor != 0) {
    fail("SBLR.OPERATION.VERSION_INVALID", "SBOP v1.0 is the only admitted operation encoding");
  }
  if (!IsOperationKey(envelope.operation_id, 1024) ||
      !IsOpcodeMnemonic(envelope.opcode)) {
    fail("SBLR.OPERATION.TEXT_INVALID", "operation key or opcode mnemonic is not canonical");
  }
  if (envelope.result_shape.empty() || envelope.result_shape.size() > 1024 ||
      !IsValidUtf8(envelope.result_shape) || envelope.diagnostic_shape.empty() ||
      envelope.diagnostic_shape.size() > 1024 || !IsValidUtf8(envelope.diagnostic_shape) ||
      envelope.trace_key.empty() || envelope.trace_key.size() > 4096 ||
      !IsValidUtf8(envelope.trace_key)) {
    fail("SBLR.OPERATION.TEXT_INVALID", "result, diagnostic, or trace identity is not canonical");
  }
  std::array<std::uint8_t, 16> uuid{};
  if (!ParseUuid(envelope.parser_package_uuid, &uuid) ||
      !ParseUuid(envelope.registry_snapshot_uuid, &uuid)) {
    fail("SBLR.OPERATION.HEADER_INVALID", "producer and registry identities require nonzero canonical UUIDs");
  }
  const auto identity = ValidateSblrOpcodeIdentity(envelope.opcode_code,
                                                   envelope.operation_id,
                                                   envelope.opcode);
  if (!identity.ok) {
    fail("SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH", identity.detail);
  }
  if (envelope.contains_sql_text || HasSourceArtifactMetadata(envelope.source_artifact_map)) {
    fail("SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY",
         "SBOP cannot carry SQL text, source artifacts, or SBEE-owned authority");
  }
  if (envelope.operands.size() > kSblrOperationMaximumOperands) {
    fail("SBLR.OPERATION.LIMIT_EXCEEDED", "operand count exceeds the v1 limit");
  }
  std::uint64_t value_count = 0;
  std::size_t expression_node_table_count = 0;
  std::optional<SblrExpressionNodeTableCodecResultV1> expression_node_table;
  std::vector<SblrExpressionNodeReferenceV1> expression_node_references;
  std::size_t parameter_node_table_count = 0;
  std::optional<SblrParameterNodeTableCodecResultV1> parameter_node_table;
  std::vector<SblrParameterNodeReferenceV1> parameter_node_references;
  std::size_t variable_node_table_count = 0;
  std::optional<SblrVariableNodeTableCodecResultV1> variable_node_table;
  std::vector<SblrVariableNodeReferenceV1> variable_node_references;
  for (std::size_t i = 0; i < envelope.operands.size(); ++i) {
    const auto& operand = envelope.operands[i];
    bool limit_exceeded = false;
    const bool source_map_descriptor =
        envelope.operation_id == "engine.op.source_map" &&
        envelope.opcode == "SBLR_SOURCE_MAP" && envelope.opcode_code == 6 &&
        envelope.operands.size() == 1 && operand.ordinal == 1 &&
        operand.type == "source_map.vector" && operand.name == "source_map" &&
        operand.value_kind == SblrValueKind::descriptor_ref &&
        operand.value_body.size() == 24 &&
        IsNonzeroUuidBytes(operand.value_body.data()) &&
        Load64(operand.value_body.data() + 16) != 0;
    const bool error_vector_descriptor =
        envelope.operation_id == "engine.op.error_vector" &&
        envelope.opcode == "SBLR_ERROR_VECTOR" && envelope.opcode_code == 7 &&
        envelope.operands.size() == 1 && operand.ordinal == 1 &&
        operand.type == "diagnostic.vector" && operand.name == "diagnostics" &&
        operand.value_kind == SblrValueKind::descriptor_ref &&
        operand.value_body.size() == 24 &&
        IsNonzeroUuidBytes(operand.value_body.data()) &&
        Load64(operand.value_body.data() + 16) != 0;
    const bool alter_publication_descriptor =
        ((envelope.operation_id == "engine.op.ddl_alter_publication" &&
        envelope.opcode == "SBLR_DDL_ALTER_PUBLICATION" && envelope.opcode_code == 1583 &&
        operand.type == "alter_publication_descriptor") ||
        (envelope.operation_id == "engine.op.ddl_drop_publication" &&
        envelope.opcode == "SBLR_DDL_DROP_PUBLICATION" && envelope.opcode_code == 1584 &&
        operand.type == "drop_publication_descriptor")) &&
        envelope.operands.size() == 1 && operand.ordinal == 1 &&
        operand.name == "publication" &&
        operand.value_kind == SblrValueKind::descriptor_ref && operand.value_body.size() == 320;
    const bool create_subscription_descriptor =
        (envelope.operation_id == "engine.op.ddl_create_subscription" ||
         envelope.operation_id == "ddl.subscription.create") &&
        envelope.opcode == "SBLR_DDL_CREATE_SUBSCRIPTION" && envelope.opcode_code == 1585 &&
        envelope.operands.size() == 1 && operand.ordinal == 1 &&
        operand.type == "create_subscription_descriptor" && operand.name == "subscription" &&
        operand.value_kind == SblrValueKind::descriptor_ref && operand.value_body.size() == 384;
    const bool alter_subscription_descriptor =
        envelope.operation_id == "ddl.subscription.alter" &&
        envelope.opcode == "SBLR_DDL_ALTER_SUBSCRIPTION" && envelope.opcode_code == 1586 &&
        envelope.operands.size() == 1 && operand.ordinal == 1 &&
        operand.type == "alter_subscription_descriptor" && operand.name == "subscription" &&
        operand.value_kind == SblrValueKind::descriptor_ref && operand.value_body.size() == 384;
    const bool drop_subscription_descriptor =
        envelope.operation_id == "ddl.subscription.drop" &&
        envelope.opcode == "SBLR_DDL_DROP_SUBSCRIPTION" && envelope.opcode_code == 1587 &&
        envelope.operands.size() == 1 && operand.ordinal == 1 &&
        operand.type == "drop_subscription_descriptor" && operand.name == "subscription" &&
        operand.value_kind == SblrValueKind::descriptor_ref && operand.value_body.size() == 384;
    const bool create_operator_descriptor =
        envelope.operation_id == "ddl.operator.create" && envelope.opcode == "SBLR_DDL_CREATE_OPERATOR" && envelope.opcode_code == 1590 &&
        envelope.operands.size() == 1 && operand.ordinal == 1 && operand.type == "create_operator_descriptor" && operand.name == "operator" &&
        operand.value_kind == SblrValueKind::descriptor_ref && operand.value_body.size() == 384;
    const bool drop_operator_descriptor = envelope.operation_id=="ddl.operator.drop"&&envelope.opcode=="SBLR_DDL_DROP_OPERATOR"&&envelope.opcode_code==1591&&envelope.operands.size()==1&&operand.ordinal==1&&operand.type=="drop_operator_descriptor"&&operand.name=="operator"&&operand.value_kind==SblrValueKind::descriptor_ref&&operand.value_body.size()==384;
    const bool canonical_value_body = source_map_descriptor || error_vector_descriptor || alter_publication_descriptor || create_subscription_descriptor || alter_subscription_descriptor || drop_subscription_descriptor || create_operator_descriptor || drop_operator_descriptor ||
        ValidateValueBody(operand.value_kind, operand.value_body.data(),
                          operand.value_body.size(), 1, &value_count,
                          &limit_exceeded);
    if (operand.ordinal != i + 1 || !operand.value.empty() ||
        operand.value_flags != 0 || !IsOperationKey(operand.type, 256) ||
        !IsCanonicalOperandName(operand) ||
        !canonical_value_body) {
      fail(limit_exceeded ? "SBLR.OPERATION.LIMIT_EXCEEDED"
                          : "SBLR.OPERATION.OPERAND_INVALID",
           "operand vector is not canonical typed SBOP data");
      break;
    }
    if (operand.value_kind == SblrValueKind::descriptor_ref &&
        operand.value_body.size() == 24 && !source_map_descriptor &&
        !error_vector_descriptor) {
      fail("SBLR.OPERAND_INVALID",
           "24-byte descriptor references require an exact registered metadata opcode");
      break;
    }
    if (operand.value_kind == SblrValueKind::descriptor_ref &&
        operand.value_body.size() == 320 && !alter_publication_descriptor) {
      fail("SBLR.OPERAND_INVALID", "320-byte descriptor references require the ALTER PUBLICATION carrier");
      break;
    }
    if (operand.value_kind == SblrValueKind::descriptor_ref &&
        operand.value_body.size() == 384 && !create_subscription_descriptor) {
      if (!alter_subscription_descriptor && !drop_subscription_descriptor && !create_operator_descriptor && !drop_operator_descriptor) {
        fail("SBLR.OPERAND_INVALID", "384-byte descriptor references require a subscription carrier");
        break;
      }
    }
    if (operand.value_kind == SblrValueKind::expression_node_table) {
      ++expression_node_table_count;
      if (envelope.operation_id != "query.execute" ||
          envelope.opcode != "SBLR_QUERY_EXECUTE" ||
          operand.type != "expression.node_table.v1" ||
          operand.name != "expression_nodes" ||
          expression_node_table_count != 1) {
        fail("SBLR.OPERAND_INVALID",
             "SBXN v1 is admitted only as the unique query.execute expression_nodes carrier");
        break;
      }
      expression_node_table = DecodeSblrExpressionNodeTableV1(
          operand.value_body.data(), operand.value_body.size());
    }
    if (operand.value_kind == SblrValueKind::expression_node_ref) {
      SblrExpressionNodeReferenceV1 reference;
      if (envelope.operation_id != "query.execute" ||
          envelope.opcode != "SBLR_QUERY_EXECUTE" ||
          operand.type != "relational_expression_v1" ||
          !DecodeSblrExpressionNodeReferenceV1(
              operand.value_body.data(), operand.value_body.size(),
              &reference)) {
        fail("SBLR.OPERAND_INVALID",
             "expression_node_ref is not an exact query relational literal reference");
        break;
      }
      expression_node_references.push_back(reference);
    }
    if (operand.value_kind == SblrValueKind::parameter_node_table) {
      ++parameter_node_table_count;
      if (envelope.operation_id != "query.execute" ||
          envelope.opcode != "SBLR_QUERY_EXECUTE" ||
          operand.type != "expression.parameter_node_table.v1" ||
          operand.name != "parameter_nodes" ||
          parameter_node_table_count != 1) {
        fail("SBLR.OPERAND_INVALID",
             "SBPN v1 is admitted only as the unique query.execute parameter_nodes carrier");
        break;
      }
      parameter_node_table = DecodeSblrParameterNodeTableV1(
          operand.value_body.data(), operand.value_body.size());
    }
    if (operand.value_kind == SblrValueKind::parameter_node_ref) {
      SblrParameterNodeReferenceV1 reference;
      if (envelope.operation_id != "query.execute" ||
          envelope.opcode != "SBLR_QUERY_EXECUTE" ||
          operand.type != "relational_expression_v1" ||
          !DecodeSblrParameterNodeReferenceV1(
              operand.value_body.data(), operand.value_body.size(),
              &reference)) {
        fail("SBLR.OPERAND_INVALID",
             "parameter_node_ref is not an exact query relational parameter reference");
        break;
      }
      parameter_node_references.push_back(reference);
    }
    if (operand.value_kind == SblrValueKind::variable_node_table) {
      ++variable_node_table_count;
      if (envelope.operation_id != "query.execute" ||
          envelope.opcode != "SBLR_QUERY_EXECUTE" ||
          operand.type != "expression.variable_node_table.v1" ||
          operand.name != "variable_nodes" || variable_node_table_count != 1) {
        fail("SBLR.OPERAND_INVALID",
             "SBVN v1 is admitted only as the unique query.execute variable_nodes carrier");
        break;
      }
      variable_node_table = DecodeSblrVariableNodeTableV1(
          operand.value_body.data(), operand.value_body.size());
    }
    if (operand.value_kind == SblrValueKind::variable_node_ref) {
      SblrVariableNodeReferenceV1 reference;
      if (envelope.operation_id != "query.execute" ||
          envelope.opcode != "SBLR_QUERY_EXECUTE" ||
          operand.type != "relational_expression_v1" ||
          !DecodeSblrVariableNodeReferenceV1(
              operand.value_body.data(), operand.value_body.size(),
              &reference)) {
        fail("SBLR.OPERAND_INVALID",
             "variable_node_ref is not an exact query relational variable reference");
        break;
      }
      variable_node_references.push_back(reference);
    }
  }
  if (expression_node_table.has_value()) {
    if (!ValidateSblrLiteralReferenceBijectionV1(
            *expression_node_table, expression_node_references)) {
      fail("SBLR.OPERAND_INVALID",
           "SBXN nodes and relational literal references are not bijective");
    }
  } else if (!expression_node_references.empty()) {
    fail("SBLR.OPERAND_INVALID",
         "relational literal reference requires the exact SBXN table");
  }
  if (parameter_node_table.has_value()) {
    if (!ValidateSblrParameterReferenceBijectionV1(
            *parameter_node_table, parameter_node_references)) {
      fail("SBLR.OPERAND_INVALID",
           "SBPN nodes and relational parameter references are not bijective");
    }
  } else if (!parameter_node_references.empty()) {
    fail("SBLR.OPERAND_INVALID",
         "relational parameter reference requires the exact SBPN table");
  }
  if (variable_node_table.has_value()) {
    if (!ValidateSblrVariableReferenceBijectionV1(
            *variable_node_table, variable_node_references)) {
      fail("SBLR.OPERAND_INVALID",
           "SBVN nodes and relational variable references are not bijective");
    }
  } else if (!variable_node_references.empty()) {
    fail("SBLR.OPERAND_INVALID",
         "relational variable reference requires the exact SBVN table");
  }
  if (envelope.opcode_code == 3 || envelope.opcode == "SBLR_LITERAL" ||
      envelope.operation_id == "engine.op.literal") {
    fail("SBLR.OPERAND_INVALID",
         "SBLR_LITERAL is forbidden as a top-level operation root");
  }
  if (envelope.opcode_code == 4 || envelope.opcode == "SBLR_PARAMETER" ||
      envelope.operation_id == "engine.op.parameter") {
    fail("SBLR.OPERAND_INVALID",
         "SBLR_PARAMETER is forbidden as a top-level operation root");
  }
  if (envelope.opcode_code == 5 || envelope.opcode == "SBLR_VARIABLE" ||
      envelope.operation_id == "engine.op.variable") {
    fail("SBLR.OPERAND_INVALID",
         "SBLR_VARIABLE is forbidden as a top-level operation root");
  }
  return result;
}

std::string EncodeSblrEnvelope(const SblrOperationEnvelope& envelope) {
  if (!ValidateSblrEnvelope(envelope).ok) return {};

  std::array<std::uint8_t, 16> producer_uuid{};
  std::array<std::uint8_t, 16> registry_uuid{};
  if (!ParseUuid(envelope.parser_package_uuid, &producer_uuid) ||
      !ParseUuid(envelope.registry_snapshot_uuid, &registry_uuid)) {
    return {};
  }

  std::array<Bytes, kSblrOperationSectionCount> sections;
  AppendText(&sections[0], envelope.operation_id);
  AppendText(&sections[1], envelope.opcode);
  sections[2] = ProducerIdentity(envelope, producer_uuid);
  sections[3].assign(registry_uuid.begin(), registry_uuid.end());
  sections[4] = EncodeOperandVector(envelope.operands);
  AppendText(&sections[5], envelope.result_shape);
  AppendText(&sections[6], envelope.diagnostic_shape);
  AppendText(&sections[7], envelope.trace_key);
  bool digest_ok = false;
  const auto provenance = ProvenanceDigest(envelope, sections[2], sections[3],
                                           sections[0], sections[1], &digest_ok);
  if (!digest_ok) return {};
  sections[8].assign(provenance.begin(), provenance.end());

  std::uint64_t payload_size = 0;
  for (const auto& section : sections) {
    if (section.empty() || section.size() > kSblrOperationMaximumBytes - payload_size) {
      return {};
    }
    payload_size += section.size();
  }
  const std::uint64_t total_size = kSblrOperationSectionPayloadOffset +
                                   payload_size + kTrailerSize;
  if (total_size > kSblrOperationMaximumBytes ||
      total_size > std::numeric_limits<std::size_t>::max()) {
    return {};
  }

  Bytes encoded(static_cast<std::size_t>(total_size), 0);
  Store32(&encoded, 0, kSblrOperationMagic);
  Store16(&encoded, 4, kFormatMajor);
  Store16(&encoded, 6, kFormatMinor);
  Store16(&encoded, 8, kSblrOperationHeaderSize);
  Store16(&encoded, 10, kSblrOperationSectionCount);
  Store16(&encoded, 16, envelope.opcode_code);
  Store16(&encoded, 18, envelope.operation_version_major);
  Store16(&encoded, 20, envelope.operation_version_minor);
  Store32(&encoded, 24, kSblrOperationHeaderSize);
  Store32(&encoded, 28, kSblrOperationSectionTableSize);
  Store32(&encoded, 32, kSblrOperationSectionPayloadOffset);
  Store64(&encoded, 40, payload_size);
  Store64(&encoded, 48, total_size);

  std::uint64_t section_offset = kSblrOperationSectionPayloadOffset;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const std::size_t entry = kSblrOperationHeaderSize + i * 24;
    Store16(&encoded, entry, kSectionTags[i]);
    Store16(&encoded, entry + 2, 1);
    Store32(&encoded, entry + 4, 1);
    Store64(&encoded, entry + 8, section_offset);
    Store64(&encoded, entry + 16, sections[i].size());
    std::copy(sections[i].begin(), sections[i].end(),
              encoded.begin() + static_cast<std::ptrdiff_t>(section_offset));
    section_offset += sections[i].size();
  }
  const std::size_t trailer = encoded.size() - kTrailerSize;
  Store32(&encoded, trailer, kSblrOperationTrailerMagic);
  Store32(&encoded, trailer + 4, SblrCrc32c(encoded.data(), trailer));
  Store64(&encoded, trailer + 8, total_size);
  return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

SblrDecodeResult DecodeSblrEnvelope(std::string_view encoded) {
  if (encoded.size() > kSblrOperationMaximumBytes) {
    return DecodeFailure("SBLR.OPERATION.LIMIT_EXCEEDED", "operation exceeds the v1 byte limit");
  }
  if (encoded.size() < kSblrOperationSectionPayloadOffset + kTrailerSize) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "operation is smaller than the v1 fixed structure");
  }
  const auto* data = reinterpret_cast<const std::uint8_t*>(encoded.data());
  if (Load32(data) != kSblrOperationMagic) {
    return DecodeFailure("SBLR.OPERATION.MAGIC_INVALID", "operation magic is not literal SBOP");
  }
  if (Load16(data + 4) != kFormatMajor || Load16(data + 6) != kFormatMinor ||
      Load16(data + 18) != 1 || Load16(data + 20) != 0) {
    return DecodeFailure("SBLR.OPERATION.VERSION_INVALID", "operation version is unsupported");
  }
  const std::uint64_t payload_size = Load64(data + 40);
  const std::uint64_t declared_total = Load64(data + 48);
  if (declared_total != encoded.size() ||
      payload_size != encoded.size() - kSblrOperationSectionPayloadOffset - kTrailerSize) {
    return DecodeFailure("SBLR.OPERATION.TOTAL_SIZE_MISMATCH", "header total size does not match the byte stream");
  }
  if (Load16(data + 8) != kSblrOperationHeaderSize ||
      Load16(data + 10) != kSblrOperationSectionCount || Load32(data + 12) != 0 ||
      Load16(data + 16) == 0 || Load16(data + 22) != 0 ||
      Load32(data + 24) != kSblrOperationHeaderSize ||
      Load32(data + 28) != kSblrOperationSectionTableSize ||
      Load32(data + 32) != kSblrOperationSectionPayloadOffset ||
      Load32(data + 36) != 0 || Load64(data + 56) != 0) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "operation header contains a noncanonical field");
  }

  const std::size_t trailer = encoded.size() - kTrailerSize;
  if (Load32(data + trailer) != kSblrOperationTrailerMagic) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "operation trailer magic is invalid");
  }
  if (Load64(data + trailer + 8) != declared_total) {
    return DecodeFailure("SBLR.OPERATION.TOTAL_SIZE_MISMATCH", "trailer total size does not match the header");
  }
  if (Load32(data + trailer + 4) != SblrCrc32c(data, trailer)) {
    return DecodeFailure("SBLR.OPERATION.CRC_MISMATCH", "operation CRC-32C differs");
  }

  struct SectionView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
  };
  std::array<SectionView, kSblrOperationSectionCount> sections;
  std::uint64_t expected_offset = kSblrOperationSectionPayloadOffset;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const std::size_t entry = kSblrOperationHeaderSize + i * 24;
    const std::uint64_t offset = Load64(data + entry + 8);
    const std::uint64_t size = Load64(data + entry + 16);
    if (Load16(data + entry) != kSectionTags[i] || Load16(data + entry + 2) != 1 ||
        Load32(data + entry + 4) != 1) {
      return DecodeFailure("SBLR.OPERATION.SECTION_TABLE_INVALID", "section table identity, version, or flags differ");
    }
    if (size == 0) {
      return DecodeFailure("SBLR.OPERATION.SECTION_MISSING", "a required section is absent");
    }
    if (offset != expected_offset || size > trailer - expected_offset) {
      return DecodeFailure("SBLR.OPERATION.SECTION_OVERLAP_OR_GAP", "section offsets are not a contiguous ordinal concatenation");
    }
    sections[i] = {data + static_cast<std::size_t>(offset), static_cast<std::size_t>(size)};
    expected_offset += size;
  }
  if (expected_offset != trailer) {
    return DecodeFailure("SBLR.OPERATION.SECTION_OVERLAP_OR_GAP", "section payload has a gap, padding, or size mismatch");
  }

  SblrOperationEnvelope envelope;
  envelope.opcode_code = Load16(data + 16);
  envelope.operation_version_major = Load16(data + 18);
  envelope.operation_version_minor = Load16(data + 20);
  if (!DecodeText(sections[0].data, sections[0].size, 1024, &envelope.operation_id) ||
      !IsOperationKey(envelope.operation_id, 1024) ||
      !DecodeText(sections[1].data, sections[1].size, 256, &envelope.opcode) ||
      !IsOpcodeMnemonic(envelope.opcode) ||
      !DecodeText(sections[5].data, sections[5].size, 1024, &envelope.result_shape) ||
      !DecodeText(sections[6].data, sections[6].size, 1024, &envelope.diagnostic_shape) ||
      !DecodeText(sections[7].data, sections[7].size, 4096, &envelope.trace_key)) {
    return DecodeFailure("SBLR.OPERATION.TEXT_INVALID", "operation contains noncanonical text");
  }
  if (sections[2].size != 28 || !IsNonzeroUuidBytes(sections[2].data) ||
      sections[3].size != 16 || !IsNonzeroUuidBytes(sections[3].data)) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "producer or registry section is invalid");
  }
  envelope.parser_package_uuid = FormatUuid(sections[2].data);
  envelope.parser_package_version_major = Load32(sections[2].data + 16);
  envelope.parser_package_version_minor = Load32(sections[2].data + 20);
  envelope.parser_package_version_patch = Load32(sections[2].data + 24);
  envelope.registry_snapshot_uuid = FormatUuid(sections[3].data);
  envelope.parser_resolved_names_to_uuids = true;
  bool limit_exceeded = false;
  if (!DecodeOperandVector(sections[4].data, sections[4].size,
                           &envelope.operands, &limit_exceeded)) {
    return DecodeFailure(limit_exceeded ? "SBLR.OPERATION.LIMIT_EXCEEDED"
                                        : "SBLR.OPERATION.OPERAND_INVALID",
                         "operand vector is malformed or noncanonical");
  }
  if (sections[8].size != 32) {
    return DecodeFailure("SBLR.OPERATION.PROVENANCE_MISMATCH", "producer provenance has the wrong size");
  }
  Bytes producer(sections[2].data, sections[2].data + sections[2].size);
  Bytes registry(sections[3].data, sections[3].data + sections[3].size);
  Bytes operation_key(sections[0].data, sections[0].data + sections[0].size);
  Bytes mnemonic(sections[1].data, sections[1].data + sections[1].size);
  bool digest_ok = false;
  const auto provenance = ProvenanceDigest(envelope, producer, registry,
                                           operation_key, mnemonic, &digest_ok);
  if (!digest_ok || !std::equal(provenance.begin(), provenance.end(), sections[8].data)) {
    return DecodeFailure("SBLR.OPERATION.PROVENANCE_MISMATCH", "producer provenance SHA-256 differs");
  }
  const auto validation = ValidateSblrEnvelope(envelope);
  if (!validation.ok) {
    SblrDecodeResult result;
    result.diagnostics = validation.diagnostics;
    return result;
  }
  const std::string canonical = EncodeSblrEnvelope(envelope);
  if (canonical.size() != encoded.size() ||
      !std::equal(canonical.begin(), canonical.end(), encoded.begin())) {
    return DecodeFailure("SBLR.OPERATION.NONCANONICAL", "decoded operation does not re-encode byte-for-byte");
  }

  SblrDecodeResult result;
  result.ok = true;
  result.envelope = std::move(envelope);
  result.canonical_bytes.assign(data, data + encoded.size());
  return result;
}

std::string SerializeSblrEnvelopeToJson(const SblrOperationEnvelope& envelope) {
  std::ostringstream out;
  out << "{\n"
      << "  \"format\": \"SBOP\",\n"
      << "  \"format_version\": \"1.0\",\n"
      << "  \"opcode_code\": " << envelope.opcode_code << ",\n"
      << "  \"operation_id\": \"" << JsonEscape(envelope.operation_id) << "\",\n"
      << "  \"opcode\": \"" << JsonEscape(envelope.opcode) << "\",\n"
      << "  \"producer_uuid\": \"" << JsonEscape(envelope.parser_package_uuid) << "\",\n"
      << "  \"registry_snapshot_uuid\": \"" << JsonEscape(envelope.registry_snapshot_uuid) << "\",\n"
      << "  \"result_shape\": \"" << JsonEscape(envelope.result_shape) << "\",\n"
      << "  \"diagnostic_shape\": \"" << JsonEscape(envelope.diagnostic_shape) << "\",\n"
      << "  \"trace_key\": \"" << JsonEscape(envelope.trace_key) << "\",\n"
      << "  \"operand_count\": " << envelope.operands.size() << "\n"
      << "}\n";
  return out.str();
}

std::string SerializeSblrValidationToJson(const SblrEnvelopeValidationResult& result) {
  std::ostringstream out;
  out << "{\n  \"ok\": " << (result.ok ? "true" : "false")
      << ",\n  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const auto& diagnostic = result.diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message)
        << "\", \"error\": " << (diagnostic.error ? "true" : "false") << "}";
    if (i + 1 != result.diagnostics.size()) out << ',';
    out << '\n';
  }
  out << "  ]\n}\n";
  return out.str();
}

}  // namespace scratchbird::engine::sblr
