// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_wire_descriptor.hpp"

#include "firebird_dialect.hpp"

#include <sstream>
#include <unordered_map>

namespace scratchbird::parser::firebird {
namespace {

struct TagInfo {
  std::string_view name;
  std::string_view policy;
};

struct BlrSlotDescriptor {
  std::uint16_t slot{0};
  std::uint8_t opcode{0};
  std::string name;
  std::string role;
  std::uint16_t length{0};
  std::uint16_t charset{0};
  std::int8_t scale{0};
};

constexpr std::uint8_t kDpbVersion1 = 1;
constexpr std::uint8_t kDpbVersion2 = 2;
constexpr std::uint8_t kTpbVersion1 = 1;
constexpr std::uint8_t kTpbVersion3 = 3;
constexpr std::uint8_t kBpbVersion1 = 1;
constexpr std::uint8_t kSpbVersion1 = 1;
constexpr std::uint8_t kSpbVersion2 = 2;
constexpr std::uint8_t kSpbVersion3 = 3;
constexpr std::uint8_t kBlrVersion4 = 4;
constexpr std::uint8_t kBlrVersion5 = 5;
constexpr std::uint8_t kBlrEoc = 76;
constexpr std::uint8_t kBlrEnd = 255;
constexpr std::uint16_t kMaxDescriptorSlots = 256;

std::string EscapeJsonLocal(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
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

std::uint16_t ReadLeU16(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
  return static_cast<std::uint16_t>(buffer[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(buffer[offset + 1]) << 8);
}

std::uint32_t ReadLeU32(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
  return static_cast<std::uint32_t>(buffer[offset]) |
         (static_cast<std::uint32_t>(buffer[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(buffer[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
}

const std::unordered_map<std::uint8_t, TagInfo>& DpbTags() {
  static const std::unordered_map<std::uint8_t, TagInfo> tags = {
      {28, {"isc_dpb_user_name", "attach_identity"}},
      {29, {"isc_dpb_password", "secret_redacted"}},
      {48, {"isc_dpb_lc_ctype", "charset_descriptor"}},
      {57, {"isc_dpb_connect_timeout", "attach_policy"}},
      {60, {"isc_dpb_sql_role_name", "role_descriptor"}},
      {63, {"isc_dpb_sql_dialect", "dialect_profile"}},
      {72, {"isc_dpb_no_db_triggers", "attach_policy"}},
      {77, {"isc_dpb_utf8_filename", "database_name_descriptor"}},
      {79, {"isc_dpb_auth_block", "auth_descriptor"}},
      {85, {"isc_dpb_auth_plugin_list", "auth_descriptor"}},
      {86, {"isc_dpb_auth_plugin_name", "auth_descriptor"}},
      {87, {"isc_dpb_config", "configuration_descriptor"}},
      {91, {"isc_dpb_session_time_zone", "session_descriptor"}},
      {93, {"isc_dpb_set_bind", "datatype_binding_policy"}},
      {94, {"isc_dpb_decfloat_round", "decfloat_descriptor"}},
      {95, {"isc_dpb_decfloat_traps", "decfloat_descriptor"}},
      {97, {"isc_dpb_upgrade_db", "upgrade_policy"}},
      {100, {"isc_dpb_parallel_workers", "service_policy"}},
  };
  return tags;
}

const std::unordered_map<std::uint8_t, TagInfo>& BlrOpcodes() {
  static const std::unordered_map<std::uint8_t, TagInfo> tags = {
      {1, {"blr_assignment", "statement_opcode"}},
      {2, {"blr_begin", "statement_block"}},
      {4, {"blr_message", "message_descriptor"}},
      {7, {"blr_short", "datatype_descriptor"}},
      {8, {"blr_long", "datatype_descriptor"}},
      {9, {"blr_quad", "datatype_descriptor"}},
      {10, {"blr_float", "datatype_descriptor"}},
      {11, {"blr_d_float", "datatype_descriptor"}},
      {12, {"blr_sql_date", "datatype_descriptor"}},
      {13, {"blr_sql_time", "datatype_descriptor"}},
      {14, {"blr_text", "datatype_descriptor"}},
      {15, {"blr_text2", "datatype_descriptor"}},
      {16, {"blr_int64", "datatype_descriptor"}},
      {17, {"blr_blob2", "datatype_descriptor"}},
      {23, {"blr_bool", "datatype_descriptor"}},
      {24, {"blr_dec64", "datatype_descriptor"}},
      {25, {"blr_dec128", "datatype_descriptor"}},
      {26, {"blr_int128", "datatype_descriptor"}},
      {27, {"blr_double", "datatype_descriptor"}},
      {28, {"blr_sql_time_tz", "datatype_descriptor"}},
      {29, {"blr_timestamp_tz", "datatype_descriptor"}},
      {30, {"blr_ex_time_tz", "datatype_descriptor"}},
      {31, {"blr_ex_timestamp_tz", "datatype_descriptor"}},
      {35, {"blr_timestamp", "datatype_descriptor"}},
      {37, {"blr_varying", "datatype_descriptor"}},
      {38, {"blr_varying2", "datatype_descriptor"}},
      {40, {"blr_cstring", "datatype_descriptor"}},
      {41, {"blr_cstring2", "datatype_descriptor"}},
      {45, {"blr_blob_id", "datatype_descriptor"}},
      {76, {"blr_eoc", "terminator"}},
      {255, {"blr_end", "terminator"}},
  };
  return tags;
}

std::string BlrOpcodeName(std::uint8_t opcode) {
  const auto found = BlrOpcodes().find(opcode);
  if (found != BlrOpcodes().end()) return std::string(found->second.name);
  return "blr_opcode_" + std::to_string(static_cast<unsigned>(opcode));
}

std::string SqlTypeName(std::uint16_t sql_type) {
  const auto base_type = static_cast<std::uint16_t>(sql_type & ~1u);
  switch (base_type) {
    case 448: return "SQL_VARYING";
    case 452: return "SQL_TEXT";
    case 480: return "SQL_DOUBLE";
    case 482: return "SQL_FLOAT";
    case 496: return "SQL_LONG";
    case 500: return "SQL_SHORT";
    case 510: return "SQL_TIMESTAMP";
    case 520: return "SQL_BLOB";
    case 530: return "SQL_D_FLOAT";
    case 540: return "SQL_ARRAY";
    case 550: return "SQL_QUAD";
    case 560: return "SQL_TYPE_TIME";
    case 570: return "SQL_TYPE_DATE";
    case 580: return "SQL_INT64";
    case 32748: return "SQL_TIMESTAMP_TZ_EX";
    case 32750: return "SQL_TIME_TZ_EX";
    case 32752: return "SQL_INT128";
    case 32754: return "SQL_TIMESTAMP_TZ";
    case 32756: return "SQL_TIME_TZ";
    case 32760: return "SQL_DEC16";
    case 32762: return "SQL_DEC34";
    case 32764: return "SQL_BOOLEAN";
    case 32766: return "SQL_NULL";
    default: return "SQL_TYPE_" + std::to_string(base_type);
  }
}

const std::unordered_map<std::uint8_t, TagInfo>& TpbTags() {
  static const std::unordered_map<std::uint8_t, TagInfo> tags = {
      {1, {"isc_tpb_consistency", "transaction_isolation"}},
      {2, {"isc_tpb_concurrency", "transaction_isolation"}},
      {3, {"isc_tpb_shared", "lock_policy"}},
      {4, {"isc_tpb_protected", "lock_policy"}},
      {5, {"isc_tpb_exclusive", "lock_policy"}},
      {6, {"isc_tpb_wait", "wait_policy"}},
      {7, {"isc_tpb_nowait", "wait_policy"}},
      {8, {"isc_tpb_read", "access_mode"}},
      {9, {"isc_tpb_write", "access_mode"}},
      {10, {"isc_tpb_lock_read", "relation_lock_descriptor"}},
      {11, {"isc_tpb_lock_write", "relation_lock_descriptor"}},
      {14, {"isc_tpb_ignore_limbo", "limbo_policy"}},
      {15, {"isc_tpb_read_committed", "transaction_isolation"}},
      {16, {"isc_tpb_autocommit", "transaction_lifecycle"}},
      {17, {"isc_tpb_rec_version", "read_committed_policy"}},
      {18, {"isc_tpb_no_rec_version", "read_committed_policy"}},
      {20, {"isc_tpb_no_auto_undo", "transaction_lifecycle"}},
      {21, {"isc_tpb_lock_timeout", "wait_policy"}},
      {22, {"isc_tpb_read_consistency", "transaction_isolation"}},
      {23, {"isc_tpb_at_snapshot_number", "snapshot_descriptor"}},
      {24, {"isc_tpb_auto_release_temp_blobid", "blob_lifecycle"}},
  };
  return tags;
}

const std::unordered_map<std::uint8_t, TagInfo>& BpbTags() {
  static const std::unordered_map<std::uint8_t, TagInfo> tags = {
      {1, {"isc_bpb_source_type", "blob_descriptor"}},
      {2, {"isc_bpb_target_type", "blob_descriptor"}},
      {3, {"isc_bpb_type", "blob_stream_policy"}},
      {4, {"isc_bpb_source_interp", "charset_descriptor"}},
      {5, {"isc_bpb_target_interp", "charset_descriptor"}},
      {6, {"isc_bpb_filter_parameter", "filter_descriptor"}},
      {7, {"isc_bpb_storage", "storage_policy"}},
  };
  return tags;
}

const std::unordered_map<std::uint8_t, TagInfo>& SpbClumpletTags() {
  static const std::unordered_map<std::uint8_t, TagInfo> tags = {
      {28, {"isc_spb_user_name", "service_identity"}},
      {29, {"isc_spb_password", "secret_redacted"}},
      {60, {"isc_spb_sql_role_name", "role_descriptor"}},
      {105, {"isc_spb_command_line", "service_command_descriptor"}},
      {106, {"isc_spb_dbname", "database_name_descriptor"}},
      {107, {"isc_spb_verbose", "service_output_policy"}},
      {108, {"isc_spb_options", "service_options"}},
      {114, {"isc_spb_verbint", "service_output_policy"}},
      {115, {"isc_spb_auth_block", "auth_descriptor"}},
      {116, {"isc_spb_auth_plugin_name", "auth_descriptor"}},
      {117, {"isc_spb_auth_plugin_list", "auth_descriptor"}},
      {118, {"isc_spb_utf8_filename", "database_name_descriptor"}},
      {123, {"isc_spb_config", "configuration_descriptor"}},
      {124, {"isc_spb_expected_db", "security_database_descriptor"}},
  };
  return tags;
}

const std::unordered_map<std::uint8_t, TagInfo>& ServiceActions() {
  static const std::unordered_map<std::uint8_t, TagInfo> tags = {
      {1, {"isc_action_svc_backup", "emulated_backup_restore"}},
      {2, {"isc_action_svc_restore", "emulated_backup_restore"}},
      {3, {"isc_action_svc_repair", "emulated_validation_repair"}},
      {4, {"isc_action_svc_add_user", "scratchbird_security_projection"}},
      {5, {"isc_action_svc_delete_user", "scratchbird_security_projection"}},
      {6, {"isc_action_svc_modify_user", "scratchbird_security_projection"}},
      {7, {"isc_action_svc_display_user", "scratchbird_security_projection"}},
      {8, {"isc_action_svc_properties", "emulated_database_properties"}},
      {11, {"isc_action_svc_db_stats", "emulated_statistics_report"}},
      {12, {"isc_action_svc_get_fb_log", "emulated_log_report"}},
      {20, {"isc_action_svc_nbak", "emulated_incremental_backup"}},
      {21, {"isc_action_svc_nrest", "emulated_incremental_restore"}},
      {22, {"isc_action_svc_trace_start", "emulated_trace_service"}},
      {23, {"isc_action_svc_trace_stop", "emulated_trace_service"}},
      {24, {"isc_action_svc_trace_suspend", "emulated_trace_service"}},
      {25, {"isc_action_svc_trace_resume", "emulated_trace_service"}},
      {26, {"isc_action_svc_trace_list", "emulated_trace_service"}},
      {27, {"isc_action_svc_set_mapping", "scratchbird_security_projection"}},
      {28, {"isc_action_svc_drop_mapping", "scratchbird_security_projection"}},
      {29, {"isc_action_svc_display_user_adm", "scratchbird_security_projection"}},
      {30, {"isc_action_svc_validate", "emulated_validation_repair"}},
      {31, {"isc_action_svc_nfix", "emulated_incremental_backup"}},
  };
  return tags;
}

ParameterBufferDecodeResult Fail(std::string_view kind,
                                 std::string diagnostic_code,
                                 std::string diagnostic_message) {
  ParameterBufferDecodeResult result;
  result.kind = std::string(kind);
  result.diagnostic_code = std::move(diagnostic_code);
  result.diagnostic_message = std::move(diagnostic_message);
  result.runtime_policy = "fail_closed";
  result.json = "{\"ok\":false,\"kind\":\"" + EscapeJsonLocal(result.kind) +
                "\",\"diagnostic_code\":\"" +
                EscapeJsonLocal(result.diagnostic_code) +
                "\",\"diagnostic_message\":\"" +
                EscapeJsonLocal(result.diagnostic_message) +
                "\",\"runtime_policy\":\"fail_closed\"}";
  return result;
}

std::string ItemsToJson(const ParameterBufferDecodeResult& result) {
  std::ostringstream out;
  out << "{\"ok\":" << (result.ok ? "true" : "false")
      << ",\"kind\":\"" << EscapeJsonLocal(result.kind)
      << "\",\"version\":" << static_cast<unsigned>(result.version)
      << ",\"runtime_policy\":\"" << EscapeJsonLocal(result.runtime_policy)
      << "\",\"items\":[";
  for (std::size_t i = 0; i < result.items.size(); ++i) {
    if (i != 0) out << ',';
    const auto& item = result.items[i];
    out << "{\"tag\":" << static_cast<unsigned>(item.tag)
        << ",\"name\":\"" << EscapeJsonLocal(item.name)
        << "\",\"payload_length\":" << item.payload.size()
        << ",\"policy\":\"" << EscapeJsonLocal(item.policy) << "\"}";
  }
  out << "]";
  if (!result.service_action.empty()) {
    out << ",\"service_action\":\"" << EscapeJsonLocal(result.service_action) << "\"";
  }
  out << "}";
  return out.str();
}

ParameterBufferDecodeResult DecodeClumpletBuffer(
    std::string_view kind,
    const std::vector<std::uint8_t>& buffer,
    const std::unordered_map<std::uint8_t, TagInfo>& tags) {
  if (buffer.empty()) return Fail(kind, "FIREBIRD.WIRE.BUFFER_EMPTY", "Parameter buffer is empty.");
  ParameterBufferDecodeResult result;
  result.ok = true;
  result.kind = std::string(kind);
  result.version = buffer[0];
  result.runtime_policy = "descriptor_only_no_engine_authority";
  std::size_t offset = 1;
  while (offset < buffer.size()) {
    const auto tag = buffer[offset++];
    if (offset >= buffer.size()) {
      return Fail(kind, "FIREBIRD.WIRE.CLUMPLET_LENGTH_MISSING",
                  "Parameter buffer clumplet tag is missing its length byte.");
    }
    const auto length = buffer[offset++];
    if (offset + length > buffer.size()) {
      return Fail(kind, "FIREBIRD.WIRE.CLUMPLET_LENGTH_INVALID",
                  "Parameter buffer clumplet length exceeds remaining bytes.");
    }
    const auto found = tags.find(tag);
    if (found == tags.end()) {
      return Fail(kind, "FIREBIRD.WIRE.UNKNOWN_TAG",
                  "Parameter buffer contains an unassigned tag.");
    }
    result.items.push_back({tag, std::string(found->second.name),
                            {buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                             buffer.begin() + static_cast<std::ptrdiff_t>(offset + length)},
                            std::string(found->second.policy)});
    offset += length;
  }
  result.json = ItemsToJson(result);
  return result;
}

bool EnsureAvailable(std::size_t offset,
                     std::size_t needed,
                     std::size_t size,
                     std::string* diagnostic) {
  if (offset + needed <= size) return true;
  *diagnostic = "FIREBIRD.WIRE.BLR_TRUNCATED";
  return false;
}

bool ReadBlrSlot(const std::vector<std::uint8_t>& buffer,
                 std::size_t* offset,
                 std::uint16_t slot,
                 BlrSlotDescriptor* descriptor,
                 std::string* diagnostic) {
  if (!EnsureAvailable(*offset, 1, buffer.size(), diagnostic)) return false;
  descriptor->slot = slot;
  descriptor->opcode = buffer[(*offset)++];
  descriptor->name = BlrOpcodeName(descriptor->opcode);
  descriptor->role = (slot & 1u) == 0 ? "value" : "null_indicator";
  switch (descriptor->opcode) {
    case 7:   // blr_short
    case 8:   // blr_long
    case 9:   // blr_quad
    case 16:  // blr_int64
    case 26:  // blr_int128
      if (!EnsureAvailable(*offset, 1, buffer.size(), diagnostic)) return false;
      descriptor->scale = static_cast<std::int8_t>(buffer[(*offset)++]);
      return true;
    case 14:  // blr_text
    case 37:  // blr_varying
    case 40:  // blr_cstring
      if (!EnsureAvailable(*offset, 2, buffer.size(), diagnostic)) return false;
      descriptor->length = ReadLeU16(buffer, *offset);
      *offset += 2;
      return true;
    case 15:  // blr_text2
    case 38:  // blr_varying2
    case 41:  // blr_cstring2
      if (!EnsureAvailable(*offset, 4, buffer.size(), diagnostic)) return false;
      descriptor->charset = ReadLeU16(buffer, *offset);
      descriptor->length = ReadLeU16(buffer, *offset + 2);
      *offset += 4;
      return true;
    case 17:  // blr_blob2
      if (!EnsureAvailable(*offset, 4, buffer.size(), diagnostic)) return false;
      descriptor->length = ReadLeU16(buffer, *offset);
      descriptor->charset = ReadLeU16(buffer, *offset + 2);
      *offset += 4;
      return true;
    case 10:  // blr_float
    case 11:  // blr_d_float
    case 12:  // blr_sql_date
    case 13:  // blr_sql_time
    case 23:  // blr_bool
    case 24:  // blr_dec64
    case 25:  // blr_dec128
    case 27:  // blr_double
    case 28:  // blr_sql_time_tz
    case 29:  // blr_timestamp_tz
    case 30:  // blr_ex_time_tz
    case 31:  // blr_ex_timestamp_tz
    case 35:  // blr_timestamp
    case 45:  // blr_blob_id
      return true;
    default:
      *diagnostic = "FIREBIRD.WIRE.BLR_OPCODE_UNKNOWN";
      return false;
  }
}

ParameterBufferDecodeResult DecodeBlrBuffer(std::string_view kind,
                                            const std::vector<std::uint8_t>& buffer,
                                            bool message_only) {
  if (buffer.empty() || (buffer[0] != kBlrVersion4 && buffer[0] != kBlrVersion5)) {
    return Fail(kind, "FIREBIRD.WIRE.BLR_VERSION_INVALID", "BLR version is invalid.");
  }
  if (buffer.size() < 3) {
    return Fail(kind, "FIREBIRD.WIRE.BLR_TRUNCATED", "BLR buffer is truncated.");
  }
  if (buffer.back() != kBlrEoc) {
    return Fail(kind, "FIREBIRD.WIRE.BLR_EOC_MISSING", "BLR buffer is missing blr_eoc.");
  }

  ParameterBufferDecodeResult result;
  result.ok = true;
  result.kind = message_only ? "MESSAGE_BLR" : "BLR";
  result.version = buffer[0];
  result.runtime_policy = "descriptor_only_no_engine_authority";
  std::vector<BlrSlotDescriptor> slots;
  bool saw_message = false;
  std::uint8_t message_number = 0;
  std::uint16_t message_slot_count = 0;

  std::size_t offset = 1;
  while (offset < buffer.size()) {
    const auto opcode = buffer[offset++];
    result.items.push_back({opcode, BlrOpcodeName(opcode), {}, "blr_opcode_descriptor"});
    if (opcode == kBlrEoc) break;
    if (opcode == kBlrEnd) continue;
    if (opcode == 4) {  // blr_message
      if (!EnsureAvailable(offset, 3, buffer.size(), &result.diagnostic_code)) {
        return Fail(kind, result.diagnostic_code, "BLR message descriptor is truncated.");
      }
      saw_message = true;
      message_number = buffer[offset++];
      message_slot_count = ReadLeU16(buffer, offset);
      offset += 2;
      if (message_slot_count > kMaxDescriptorSlots) {
        return Fail(kind, "FIREBIRD.WIRE.BLR_MESSAGE_COUNT_INVALID",
                    "BLR message slot count exceeds the descriptor budget.");
      }
      for (std::uint16_t slot = 0; slot < message_slot_count; ++slot) {
        BlrSlotDescriptor descriptor;
        if (!ReadBlrSlot(buffer, &offset, slot, &descriptor, &result.diagnostic_code)) {
          return Fail(kind, result.diagnostic_code, "BLR message slot descriptor is invalid.");
        }
        slots.push_back(std::move(descriptor));
      }
      continue;
    }
    if (message_only && opcode != 2 && opcode != kBlrEnd && opcode != kBlrEoc) {
      return Fail(kind, "FIREBIRD.WIRE.BLR_MESSAGE_EXPECTED",
                  "Message BLR descriptor must contain a blr_message block.");
    }
  }
  if (message_only && !saw_message) {
    return Fail(kind, "FIREBIRD.WIRE.BLR_MESSAGE_EXPECTED",
                "Message BLR descriptor must contain a blr_message block.");
  }

  std::ostringstream out;
  out << "{\"ok\":true,\"kind\":\"" << EscapeJsonLocal(result.kind)
      << "\",\"version\":" << static_cast<unsigned>(result.version)
      << ",\"runtime_policy\":\"" << EscapeJsonLocal(result.runtime_policy)
      << "\",\"opcode_count\":" << result.items.size()
      << ",\"saw_message\":" << (saw_message ? "true" : "false");
  if (saw_message) {
    out << ",\"message_number\":" << static_cast<unsigned>(message_number)
        << ",\"message_slot_count\":" << message_slot_count
        << ",\"slots\":[";
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (i != 0) out << ',';
      out << "{\"slot\":" << slots[i].slot
          << ",\"opcode\":" << static_cast<unsigned>(slots[i].opcode)
          << ",\"name\":\"" << EscapeJsonLocal(slots[i].name)
          << "\",\"role\":\"" << EscapeJsonLocal(slots[i].role)
          << "\",\"length\":" << slots[i].length
          << ",\"charset\":" << slots[i].charset
          << ",\"scale\":" << static_cast<int>(slots[i].scale)
          << "}";
    }
    out << "]";
  }
  out << ",\"items\":[";
  for (std::size_t i = 0; i < result.items.size(); ++i) {
    if (i != 0) out << ',';
    out << "{\"tag\":" << static_cast<unsigned>(result.items[i].tag)
        << ",\"name\":\"" << EscapeJsonLocal(result.items[i].name)
        << "\",\"policy\":\"" << EscapeJsonLocal(result.items[i].policy) << "\"}";
  }
  out << "]}";
  result.json = out.str();
  return result;
}

ParameterBufferDecodeResult DecodeSqldaBuffer(std::string_view kind,
                                              const std::vector<std::uint8_t>& buffer) {
  constexpr std::size_t kCompactHeaderSize = 18;
  constexpr std::size_t kCompactVarSize = 8;
  if (buffer.size() < kCompactHeaderSize) {
    return Fail(kind, "FIREBIRD.WIRE.SQLDA_TRUNCATED", "SQLDA descriptor is truncated.");
  }
  const auto version = ReadLeU16(buffer, 0);
  if (version != 1 && version != 2) {
    return Fail(kind, "FIREBIRD.WIRE.SQLDA_VERSION_INVALID", "SQLDA version is invalid.");
  }
  const auto sqldabc = ReadLeU32(buffer, 10);
  const auto sqln = ReadLeU16(buffer, 14);
  const auto sqld = ReadLeU16(buffer, 16);
  if (sqln > kMaxDescriptorSlots || sqld > kMaxDescriptorSlots || sqld > sqln) {
    return Fail(kind, "FIREBIRD.WIRE.SQLDA_COUNT_INVALID",
                "SQLDA variable counts exceed the descriptor budget.");
  }
  if (buffer.size() < kCompactHeaderSize + (static_cast<std::size_t>(sqld) * kCompactVarSize)) {
    return Fail(kind, "FIREBIRD.WIRE.SQLDA_TRUNCATED",
                "SQLDA XSQLVAR descriptors are truncated.");
  }

  ParameterBufferDecodeResult result;
  result.ok = true;
  result.kind = "SQLDA";
  result.version = static_cast<std::uint8_t>(version);
  result.runtime_policy = "descriptor_only_no_engine_authority";

  std::ostringstream out;
  out << "{\"ok\":true,\"kind\":\"SQLDA\","
      << "\"version\":" << version << ','
      << "\"sqldaid\":\"" << EscapeJsonLocal(
             std::string_view(reinterpret_cast<const char*>(buffer.data() + 2), 8))
      << "\",\"sqldabc\":" << sqldabc
      << ",\"sqln\":" << sqln
      << ",\"sqld\":" << sqld
      << ",\"runtime_policy\":\"" << EscapeJsonLocal(result.runtime_policy)
      << "\",\"vars\":[";
  std::size_t offset = kCompactHeaderSize;
  for (std::uint16_t i = 0; i < sqld; ++i) {
    const auto sqltype = ReadLeU16(buffer, offset);
    const auto sqlscale = static_cast<std::int16_t>(ReadLeU16(buffer, offset + 2));
    const auto sqlsubtype = static_cast<std::int16_t>(ReadLeU16(buffer, offset + 4));
    const auto sqllen = ReadLeU16(buffer, offset + 6);
    if (i != 0) out << ',';
    out << "{\"index\":" << i
        << ",\"sqltype\":" << sqltype
        << ",\"base_type\":\"" << SqlTypeName(sqltype)
        << "\",\"nullable\":" << ((sqltype & 1u) != 0 ? "true" : "false")
        << ",\"sqlscale\":" << sqlscale
        << ",\"sqlsubtype\":" << sqlsubtype
        << ",\"sqllen\":" << sqllen
        << "}";
    result.items.push_back({static_cast<std::uint8_t>(i & 0xffu), SqlTypeName(sqltype), {},
                            (sqltype & 1u) != 0 ? "nullable_xsqlvar" : "required_xsqlvar"});
    offset += kCompactVarSize;
  }
  out << "]}";
  result.json = out.str();
  return result;
}

} // namespace

const std::vector<WireApiSurface>& WireApiSurfaces() {
  static const std::vector<WireApiSurface> surfaces = {
      {"fb_wire_attach_auth", "attach_authentication", "sbl_firebird_wire",
       "Firebird status vector for attach auth policy version and database-name diagnostics",
       "firebird_bridge_service_surface_conformance", "bridge_or_authority_diagnostic"},
      {"fb_wire_statement", "statement_prepare_describe_execute_fetch_close_cancel",
       "sbl_firebird_wire",
       "Firebird status vector plus SQLDA/result metadata diagnostics",
       "firebird_bridge_service_surface_conformance", "bridge_to_sblr_or_diagnostic"},
      {"fb_wire_sqlda_message", "sqlda_and_message_metadata", "sbl_firebird_wire",
       "SQLDA descriptor and message BLR diagnostics",
       "firebird_blr_parameter_buffer_conformance", "bridge_to_descriptor_or_diagnostic"},
      {"fb_wire_parameter_buffers", "dpb_tpb_spb_bpb", "sbl_firebird_wire",
       "Parameter-buffer malformed tag length and policy diagnostics",
       "firebird_blr_parameter_buffer_conformance", "bridge_to_policy_or_diagnostic"},
      {"fb_wire_blob_array", "blob_segment_array_slice", "sbl_firebird_wire",
       "Blob and array descriptor slice diagnostics",
       "firebird_bridge_service_surface_conformance", "bridge_to_sblr_or_diagnostic"},
      {"fb_wire_events", "event_api", "sbl_firebird_wire",
       "Event registration cancel and delivery diagnostics",
       "firebird_bridge_service_surface_conformance", "bridge_to_scratchbird_events"},
      {"fb_wire_services", "services_api", "sbl_firebird_wire",
       "Service attach action info and response diagnostics",
       "firebird_bridge_service_surface_conformance", "emulated_service_or_diagnostic"},
      {"fb_wire_backup_restore", "backup_restore_api", "sbl_firebird_wire",
       "Backup restore service report and authority diagnostics",
       "firebird_service_tool_regression_gate", "emulated_service_or_diagnostic"},
      {"fb_wire_validation_stats", "validation_statistics_api", "sbl_firebird_wire",
       "Validation statistics report and no-file authority diagnostics",
       "firebird_service_tool_regression_gate", "emulated_service_or_diagnostic"},
      {"fb_wire_security", "user_role_security_service_api", "sbl_firebird_wire",
       "ScratchBird security projection rendered as Firebird service diagnostics",
       "firebird_bridge_service_surface_conformance", "bridge_to_scratchbird_security"},
      {"fb_wire_trace_profiler", "trace_profiler_api", "sbl_firebird_wire",
       "Trace session report and policy diagnostics",
       "firebird_bridge_service_surface_conformance", "emulated_service_or_diagnostic"},
      {"fb_wire_replication_migration", "replication_migration_feed_api",
       "sbl_firebird_wire",
       "Replication and migration feed report or policy diagnostics",
       "firebird_wire_api_scope_gate", "scratchbird_service_report_or_diagnostic"},
      {"fb_wire_proxy_topology", "proxy_migration_topology_api", "sbl_firebird_wire",
       "Proxy topology report or policy diagnostics",
       "firebird_wire_api_scope_gate", "scratchbird_service_report_or_diagnostic"},
  };
  return surfaces;
}

const std::vector<ParameterBufferSurface>& ParameterBufferSurfaces() {
  static const std::vector<ParameterBufferSurface> surfaces = {
      {"fb_blr_core", "blr", "BLR", "sbl_firebird_wire",
       "BLR opcode malformed length and admitted-subset diagnostics",
       "firebird_blr_parameter_buffer_conformance"},
      {"fb_message_blr", "message_blr", "Message BLR", "sbl_firebird_wire",
       "Message BLR descriptor diagnostics", "firebird_blr_parameter_buffer_conformance"},
      {"fb_sqlda", "sqlda", "SQLDA", "sbl_firebird_wire",
       "SQLDA version XSQLVAR descriptor and bind diagnostics",
       "firebird_blr_parameter_buffer_conformance"},
      {"fb_dpb", "parameter_buffer", "DPB", "sbl_firebird_wire",
       "DPB clumplet tag length auth and attach diagnostics",
       "firebird_blr_parameter_buffer_conformance"},
      {"fb_tpb", "parameter_buffer", "TPB", "sbl_firebird_wire",
       "TPB isolation lock timeout and conflict diagnostics",
       "firebird_blr_parameter_buffer_conformance"},
      {"fb_spb", "parameter_buffer", "SPB", "sbl_firebird_wire",
       "SPB service action option and response diagnostics",
       "firebird_blr_parameter_buffer_conformance"},
      {"fb_bpb", "parameter_buffer", "BPB", "sbl_firebird_wire",
       "BPB blob subtype charset and filter diagnostics",
       "firebird_blr_parameter_buffer_conformance"},
  };
  return surfaces;
}

ParameterBufferDecodeResult DecodeFirebirdParameterBuffer(
    std::string_view kind,
    const std::vector<std::uint8_t>& buffer) {
  const auto upper_kind = ToUpperAscii(kind);
  if (upper_kind == "BLR") {
    return DecodeBlrBuffer("BLR", buffer, false);
  }
  if (upper_kind == "MESSAGE_BLR" || upper_kind == "MESSAGE BLR") {
    return DecodeBlrBuffer("MESSAGE_BLR", buffer, true);
  }
  if (upper_kind == "SQLDA") {
    return DecodeSqldaBuffer("SQLDA", buffer);
  }
  if (upper_kind == "DPB") {
    if (buffer.empty() || (buffer[0] != kDpbVersion1 && buffer[0] != kDpbVersion2)) {
      return Fail(kind, "FIREBIRD.WIRE.VERSION_INVALID", "DPB version is invalid.");
    }
    return DecodeClumpletBuffer("DPB", buffer, DpbTags());
  }
  if (upper_kind == "BPB") {
    if (buffer.empty() || buffer[0] != kBpbVersion1) {
      return Fail(kind, "FIREBIRD.WIRE.VERSION_INVALID", "BPB version is invalid.");
    }
    return DecodeClumpletBuffer("BPB", buffer, BpbTags());
  }
  if (upper_kind == "TPB") {
    if (buffer.empty() || (buffer[0] != kTpbVersion1 && buffer[0] != kTpbVersion3)) {
      return Fail(kind, "FIREBIRD.WIRE.VERSION_INVALID", "TPB version is invalid.");
    }
    ParameterBufferDecodeResult result;
    result.ok = true;
    result.kind = "TPB";
    result.version = buffer[0];
    result.runtime_policy = "descriptor_only_no_engine_authority";
    for (std::size_t offset = 1; offset < buffer.size(); ++offset) {
      const auto tag = buffer[offset];
      const auto found = TpbTags().find(tag);
      if (found == TpbTags().end()) {
        return Fail(kind, "FIREBIRD.WIRE.UNKNOWN_TAG", "TPB contains an unassigned tag.");
      }
      result.items.push_back({tag, std::string(found->second.name), {},
                              std::string(found->second.policy)});
    }
    result.json = ItemsToJson(result);
    return result;
  }
  if (upper_kind == "SPB") {
    if (buffer.empty() ||
        (buffer[0] != kSpbVersion1 && buffer[0] != kSpbVersion2 &&
         buffer[0] != kSpbVersion3)) {
      return Fail(kind, "FIREBIRD.WIRE.VERSION_INVALID", "SPB version is invalid.");
    }
    ParameterBufferDecodeResult result;
    result.ok = true;
    result.kind = "SPB";
    result.version = buffer[0];
    result.runtime_policy = "emulated_service_or_authority_diagnostic";
    std::size_t offset = 1;
    while (offset < buffer.size()) {
      const auto tag = buffer[offset++];
      const auto clumplet = SpbClumpletTags().find(tag);
      if (clumplet != SpbClumpletTags().end() && offset < buffer.size()) {
        const auto length = buffer[offset++];
        if (offset + length > buffer.size()) {
          return Fail(kind, "FIREBIRD.WIRE.CLUMPLET_LENGTH_INVALID",
                      "SPB clumplet length exceeds remaining bytes.");
        }
        result.items.push_back({tag, std::string(clumplet->second.name),
                                {buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                                 buffer.begin() + static_cast<std::ptrdiff_t>(offset + length)},
                                std::string(clumplet->second.policy)});
        offset += length;
        continue;
      }
      if (const auto action = ServiceActions().find(tag); action != ServiceActions().end()) {
        result.items.push_back({tag, std::string(action->second.name), {},
                                std::string(action->second.policy)});
        if (result.service_action.empty()) result.service_action = std::string(action->second.name);
        continue;
      }
      return Fail(kind, "FIREBIRD.WIRE.UNKNOWN_TAG", "SPB contains an unassigned tag.");
    }
    result.json = ItemsToJson(result);
    return result;
  }
  return Fail(kind, "FIREBIRD.WIRE.BUFFER_KIND_INVALID",
              "Parameter buffer kind must be DPB TPB SPB or BPB.");
}

std::string FirebirdWireApiScopeJson() {
  return "{\"dialect\":\"firebird\","
         "\"wire_api_surface_count\":" + std::to_string(WireApiSurfaces().size()) + ","
         "\"parameter_buffer_surface_count\":" +
         std::to_string(ParameterBufferSurfaces().size()) + ","
         "\"owner\":\"sbl_firebird_wire\","
         "\"standalone_dialect_package\":true,"
         "\"cross_dialect_dependencies\":false,"
         "\"dependency_isolation\":\"firebird_wire_only\"}";
}

} // namespace scratchbird::parser::firebird
