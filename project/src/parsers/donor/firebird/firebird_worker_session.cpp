// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_worker_session.hpp"

#include "donor_dialect.hpp"
#include "firebird_dialect.hpp"
#include "firebird_wire_descriptor.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scratchbird::parser::firebird {
namespace {

int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

bool IsHexSeparator(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) || ch == ':' || ch == '-' || ch == '_';
}

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

bool ParseHexBytes(std::string_view hex,
                   std::vector<std::uint8_t>* out,
                   std::string* diagnostic) {
  out->clear();
  int high_nibble = -1;
  for (const char ch : hex) {
    if (IsHexSeparator(ch)) continue;
    const int digit = HexDigit(ch);
    if (digit < 0) {
      *diagnostic = "FIREBIRD.WIRE.HEX_INVALID";
      return false;
    }
    if (high_nibble < 0) {
      high_nibble = digit;
      continue;
    }
    out->push_back(static_cast<std::uint8_t>((high_nibble << 4) | digit));
    high_nibble = -1;
  }
  if (high_nibble >= 0) {
    *diagnostic = "FIREBIRD.WIRE.HEX_ODD_LENGTH";
    return false;
  }
  return true;
}

std::string AfterCommand(std::string_view line, std::string_view command) {
  const auto trimmed = TrimAscii(line);
  if (trimmed.size() <= command.size()) return {};
  return TrimAscii(std::string_view(trimmed).substr(command.size()));
}

std::string FailJson(std::string_view code) {
  return "{\"ok\":false,\"diagnostic_code\":\"" + std::string(code) +
         "\",\"runtime_policy\":\"fail_closed\"}";
}

const scratchbird::parser::donor::DialectProfile& FirebirdPolicyProfile() {
  static const scratchbird::parser::donor::DialectProfile profile{
      "firebird",
      "Firebird",
      "sbp_firebird",
      "sbu_firebird_parser_support",
      "5.0.4",
      "FIREBIRD",
      "sblr.donor.firebird.profile.v1",
      {},
      {},
      {},
      {},
      {},
      318,
      258,
      0,
      0,
      0,
      0,
      0,
      0,
      0};
  return profile;
}

std::string HandleLine(std::string_view line, bool* close) {
  const auto trimmed = TrimAscii(line);
  const auto upper = ToUpperAscii(trimmed);
  if (upper.empty()) return "OK EMPTY\n";
  if (upper == "QUIT" || upper == "EXIT") {
    *close = true;
    return "OK BYE\n";
  }
  if (upper == "PING") return "OK PONG\n";
  if (upper == "LIFECYCLE_MAPPING") {
    return "MAPPING " + FirebirdLifecycleMappingReportJson() + "\n";
  }
  if (upper == "CONNECTION_SANDBOX_REPORT") {
    return "SANDBOX " + scratchbird::parser::donor::ConnectionSandboxReportJson(
                            FirebirdPolicyProfile()) +
           "\n";
  }
  if (upper == "DIALECT_VARIANT_REPORT") {
    return "VARIANTS " + scratchbird::parser::donor::DialectVariantReportJson(
                             FirebirdPolicyProfile()) +
           "\n";
  }
  if (upper.starts_with("DECODE_PARAMETER_BUFFER ")) {
    const auto body = AfterCommand(trimmed, "DECODE_PARAMETER_BUFFER");
    const auto space = body.find(' ');
    if (space == std::string::npos) {
      return "ERROR " + FailJson("FIREBIRD.WIRE.REQUEST_INVALID") + "\n";
    }
    const auto kind = body.substr(0, space);
    const auto hex = TrimAscii(std::string_view(body).substr(space + 1));
    std::vector<std::uint8_t> buffer;
    std::string diagnostic;
    if (!ParseHexBytes(hex, &buffer, &diagnostic)) {
      return "ERROR " + FailJson(diagnostic) + "\n";
    }
    const auto decoded = DecodeFirebirdParameterBuffer(kind, buffer);
    return std::string(decoded.ok ? "PARAMETER_BUFFER " : "ERROR ") + decoded.json + "\n";
  }
  if (upper.starts_with("PARSE ")) {
    const auto result = ParseStatement(AfterCommand(trimmed, "PARSE"));
    if (!result.ok) return "ERROR " + result.message_vector_json + "\n";
    return "SBLR " + result.sblr_envelope + "\n";
  }
  return "ERROR " + MessageVectorToJson({
                       {"FIREBIRD.WORKER.COMMAND_UNKNOWN",
                        "ERROR",
                        "Firebird worker session command is not recognized.",
                        "sbp_firebird.worker",
                        {{"command", std::string(trimmed)}}},
                   }) +
         "\n";
}

#ifndef _WIN32
bool IsXdrOpcode(const std::uint8_t* bytes, std::uint32_t opcode) {
  return bytes[0] == static_cast<std::uint8_t>((opcode >> 24) & 0xffu) &&
         bytes[1] == static_cast<std::uint8_t>((opcode >> 16) & 0xffu) &&
         bytes[2] == static_cast<std::uint8_t>((opcode >> 8) & 0xffu) &&
         bytes[3] == static_cast<std::uint8_t>(opcode & 0xffu);
}

void AppendXdrU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
}

bool WriteAll(int fd, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const auto rc = ::write(fd, text.data() + written, text.size() - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool ReadBytes(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t read_total = 0;
  while (read_total < size) {
    const auto rc = ::read(fd, data + read_total, size - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool ReadXdrU32(int fd, std::uint32_t* value) {
  std::uint8_t bytes[4]{};
  if (!ReadBytes(fd, bytes, sizeof(bytes))) return false;
  *value = (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
  return true;
}

bool ReadXdrString(int fd, std::vector<std::uint8_t>* value) {
  constexpr std::uint32_t kMaxFirebirdWorkerXdrString = 64u * 1024u;
  std::uint32_t length = 0;
  if (!ReadXdrU32(fd, &length)) return false;
  if (length > kMaxFirebirdWorkerXdrString) return false;
  value->assign(length, 0);
  if (length != 0 && !ReadBytes(fd, value->data(), length)) return false;
  const auto padding = (4u - (length & 3u)) & 3u;
  std::uint8_t ignored[4]{};
  return padding == 0 || ReadBytes(fd, ignored, padding);
}

bool WriteBytes(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const auto rc = ::write(fd, data + written, size - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WritePacket(int fd, const std::vector<std::uint8_t>& packet) {
  return packet.empty() || WriteBytes(fd, packet.data(), packet.size());
}

bool ReadLine(int fd, std::string* line) {
  line->clear();
  char ch = 0;
  for (;;) {
    const auto rc = ::read(fd, &ch, 1);
    if (rc == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') line->push_back(ch);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return !line->empty();
  }
}

struct FirebirdConnectOffer {
  bool protocol12{false};
};

struct FirebirdAttachRequest {
  std::uint32_t object_id{0};
  std::vector<std::uint8_t> file_name;
  std::vector<std::uint8_t> parameter_buffer;
};

struct FirebirdInfoRequest {
  std::uint32_t object_id{0};
  std::uint32_t incarnation{0};
  std::vector<std::uint8_t> items;
  std::vector<std::uint8_t> recv_items;
  std::uint32_t buffer_length{0};
};

struct FirebirdRemoteHandle {
  std::uint32_t id{0};
  std::uint32_t parent_id{0};
  std::uint32_t client_id{0};
  std::string kind;
  std::string descriptor_json;
  std::vector<std::uint8_t> data;
};

struct FirebirdBinarySessionState {
  std::uint32_t next_handle{1};
  std::vector<FirebirdRemoteHandle> handles;
};

bool ReadFirebirdConnectBody(int fd, FirebirdConnectOffer* offer) {
  constexpr std::uint32_t kConnectVersion3 = 3;
  constexpr std::uint32_t kProtocolVersion12 = 0x800c;
  constexpr std::uint32_t kArchGeneric = 1;
  constexpr std::uint32_t kMaxProtocolOffers = 32;
  std::uint32_t value = 0;
  if (!ReadXdrU32(fd, &value)) return false; // unused operation
  if (!ReadXdrU32(fd, &value)) return false;
  if (value < kConnectVersion3) return false;
  if (!ReadXdrU32(fd, &value)) return false; // client architecture
  std::vector<std::uint8_t> counted;
  if (!ReadXdrString(fd, &counted)) return false; // database/service name
  std::uint32_t count = 0;
  if (!ReadXdrU32(fd, &count)) return false;
  if (count > kMaxProtocolOffers) return false;
  if (!ReadXdrString(fd, &counted)) return false; // user identification clumplets
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t version = 0;
    std::uint32_t architecture = 0;
    std::uint32_t min_type = 0;
    std::uint32_t max_type = 0;
    std::uint32_t weight = 0;
    if (!ReadXdrU32(fd, &version) ||
        !ReadXdrU32(fd, &architecture) ||
        !ReadXdrU32(fd, &min_type) ||
        !ReadXdrU32(fd, &max_type) ||
        !ReadXdrU32(fd, &weight)) {
      return false;
    }
    (void)min_type;
    (void)max_type;
    (void)weight;
    if (version == kProtocolVersion12 && architecture == kArchGeneric) {
      offer->protocol12 = true;
    }
  }
  return true;
}

bool ReadFirebirdAttachBody(int fd, FirebirdAttachRequest* request) {
  return ReadXdrU32(fd, &request->object_id) &&
         ReadXdrString(fd, &request->file_name) &&
         ReadXdrString(fd, &request->parameter_buffer);
}

bool ReadFirebirdReleaseBody(int fd, std::uint32_t* object_id) {
  return ReadXdrU32(fd, object_id);
}

bool ReadFirebirdInfoBody(int fd, bool service_info, FirebirdInfoRequest* request) {
  return ReadXdrU32(fd, &request->object_id) &&
         ReadXdrU32(fd, &request->incarnation) &&
         ReadXdrString(fd, &request->items) &&
         (!service_info || ReadXdrString(fd, &request->recv_items)) &&
         ReadXdrU32(fd, &request->buffer_length);
}

std::vector<std::uint8_t> FirebirdOpRejectPacket() {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 4);
  return out;
}

std::vector<std::uint8_t> FirebirdOpAcceptProtocol12Packet() {
  constexpr std::uint32_t kProtocolVersion12 = 0x800c;
  constexpr std::uint32_t kArchGeneric = 1;
  constexpr std::uint32_t kPtypeLazySend = 5;
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 3);
  AppendXdrU32(&out, kProtocolVersion12);
  AppendXdrU32(&out, kArchGeneric);
  AppendXdrU32(&out, kPtypeLazySend);
  return out;
}

void AppendXdrString(std::vector<std::uint8_t>* out, std::string_view value) {
  AppendXdrU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  while ((out->size() & 3u) != 0) out->push_back(0);
}

std::string BytesToJsonString(const std::vector<std::uint8_t>& bytes) {
  std::string text(bytes.begin(), bytes.end());
  return EscapeJsonLocal(text);
}

std::string FirebirdWireLifecycleMappingJson(std::string_view wire_operation) {
  if (wire_operation == "op_create") {
    return "\"scratchbird_lifecycle_api\":true,"
           "\"lifecycle_operation_id\":\"lifecycle.create_database\","
           "\"sblr_operation\":\"SBLR_LIFECYCLE_CREATE_DATABASE\","
           "\"engine_api_function\":\"EngineCreateLifecycle\",";
  }
  if (wire_operation == "op_attach") {
    return "\"scratchbird_lifecycle_api\":true,"
           "\"lifecycle_operation_id\":\"lifecycle.attach_database\","
           "\"sblr_operation\":\"SBLR_LIFECYCLE_ATTACH_DATABASE\","
           "\"engine_api_function\":\"EngineAttachLifecycle\",";
  }
  if (wire_operation == "op_detach") {
    return "\"scratchbird_lifecycle_api\":true,"
           "\"lifecycle_operation_id\":\"lifecycle.detach_database\","
           "\"sblr_operation\":\"SBLR_LIFECYCLE_DETACH_DATABASE\","
           "\"engine_api_function\":\"EngineDetachLifecycle\",";
  }
  if (wire_operation == "op_drop_database") {
    return "\"scratchbird_lifecycle_api\":true,"
           "\"lifecycle_operation_id\":\"lifecycle.drop_database\","
           "\"sblr_operation\":\"SBLR_LIFECYCLE_DROP_DATABASE\","
           "\"engine_api_function\":\"EngineDropLifecycle\",";
  }
  return "\"scratchbird_lifecycle_api\":false,"
         "\"lifecycle_operation_id\":\"\","
         "\"sblr_operation\":\"\","
         "\"engine_api_function\":\"\",";
}

const FirebirdRemoteHandle* FindHandle(const FirebirdBinarySessionState& state,
                                       std::uint32_t handle_id) {
  for (const auto& handle : state.handles) {
    if (handle.id == handle_id) return &handle;
  }
  return nullptr;
}

FirebirdRemoteHandle* FindMutableHandle(FirebirdBinarySessionState* state,
                                        std::uint32_t handle_id) {
  for (auto& handle : state->handles) {
    if (handle.id == handle_id) return &handle;
  }
  return nullptr;
}

bool RemoveHandle(FirebirdBinarySessionState* state, std::uint32_t handle_id) {
  for (auto it = state->handles.begin(); it != state->handles.end(); ++it) {
    if (it->id == handle_id) {
      state->handles.erase(it);
      return true;
    }
  }
  return false;
}

void RemoveHandlesByParent(FirebirdBinarySessionState* state, std::uint32_t parent_id) {
  for (auto it = state->handles.begin(); it != state->handles.end();) {
    if (it->parent_id == parent_id) {
      it = state->handles.erase(it);
      continue;
    }
    ++it;
  }
}

const FirebirdRemoteHandle* FindEventByClientId(const FirebirdBinarySessionState& state,
                                                std::uint32_t database_id,
                                                std::uint32_t client_event_id) {
  for (const auto& handle : state.handles) {
    if (handle.kind == "event" && handle.parent_id == database_id &&
        handle.client_id == client_event_id) {
      return &handle;
    }
  }
  return nullptr;
}

std::string FirebirdAttachDescriptorJson(std::uint32_t opcode,
                                         const FirebirdAttachRequest& request,
                                         std::uint32_t handle_id,
                                         const ParameterBufferDecodeResult& decoded) {
  const bool service = opcode == 82;
  const std::string wire_operation =
      service ? "op_service_attach" : (opcode == 20 ? "op_create" : "op_attach");
  return std::string("{\"ok\":true,\"wire_operation\":\"") +
         wire_operation +
         "\",\"object_handle\":" + std::to_string(handle_id) +
         ",\"client_object_id\":" + std::to_string(request.object_id) +
         ",\"database_name\":\"" + BytesToJsonString(request.file_name) +
         "\",\"parameter_buffer_kind\":\"" + (service ? "SPB" : "DPB") +
         "\",\"parameter_buffer\":" + decoded.json +
         "," + FirebirdWireLifecycleMappingJson(wire_operation) +
         "\"real_firebird_file_effects\":false,"
         "\"donor_engine_sql_executed\":false,"
         "\"runtime_policy\":\"emulated_session_handle_admitted\"}";
}

std::string FirebirdBufferFailureJson(std::uint32_t opcode,
                                      const ParameterBufferDecodeResult& decoded) {
  return std::string("{\"ok\":false,\"wire_operation\":\"") +
         (opcode == 82 ? "op_service_attach" : (opcode == 20 ? "op_create" : "op_attach")) +
         "\",\"parameter_buffer\":" + decoded.json +
         ",\"runtime_policy\":\"fail_closed_parameter_buffer\"}";
}

std::string FirebirdServiceStartJson(const FirebirdInfoRequest& request,
                                     const ParameterBufferDecodeResult& decoded) {
  return "{\"ok\":true,\"wire_operation\":\"op_service_start\","
         "\"object_handle\":" + std::to_string(request.object_id) + ","
         "\"incarnation\":" + std::to_string(request.incarnation) + ","
         "\"parameter_buffer_kind\":\"SPB\","
         "\"parameter_buffer\":" + decoded.json + ","
         "\"runtime_policy\":\"emulated_service_action_admitted\"}";
}

std::string FirebirdServiceInfoJson(const FirebirdInfoRequest& request,
                                    const FirebirdRemoteHandle& handle) {
  return "{\"ok\":true,\"wire_operation\":\"op_service_info\","
         "\"object_handle\":" + std::to_string(request.object_id) + ","
         "\"handle_kind\":\"" + EscapeJsonLocal(handle.kind) + "\","
         "\"request_items_length\":" + std::to_string(request.items.size()) + ","
         "\"recv_items_length\":" + std::to_string(request.recv_items.size()) + ","
         "\"buffer_length\":" + std::to_string(request.buffer_length) + ","
         "\"emulated_service_report\":true,"
         "\"runtime_policy\":\"emulated_service_info_descriptor\"}";
}

struct FirebirdPrepareStatementRequest {
  std::uint32_t transaction_id{0};
  std::uint32_t statement_id{0};
  std::uint32_t sql_dialect{0};
  std::vector<std::uint8_t> sql_text;
  std::vector<std::uint8_t> items;
  std::uint32_t buffer_length{0};
};

struct FirebirdSqlDataRequest {
  std::uint32_t statement_id{0};
  std::uint32_t transaction_id{0};
  std::vector<std::uint8_t> blr;
  std::uint32_t message_number{0};
  std::uint32_t message_count{0};
};

struct FirebirdFreeStatementRequest {
  std::uint32_t statement_id{0};
  std::uint32_t option{0};
};

struct FirebirdTransactionRequest {
  std::uint32_t database_id{0};
  std::vector<std::uint8_t> parameter_buffer;
};

struct FirebirdBlobRequest {
  std::uint32_t transaction_id{0};
  std::uint32_t blob_id_high{0};
  std::uint32_t blob_id_low{0};
  std::vector<std::uint8_t> parameter_buffer;
};

struct FirebirdSegmentRequest {
  std::uint32_t blob_id{0};
  std::uint32_t segment_length{0};
  std::vector<std::uint8_t> segment;
};

struct FirebirdEventRequest {
  std::uint32_t database_id{0};
  std::vector<std::uint8_t> items;
  std::uint32_t ast{0};
  std::uint32_t argument{0};
  std::uint32_t client_event_id{0};
};

bool ReadFirebirdPrepareStatementBody(int fd, FirebirdPrepareStatementRequest* request) {
  return ReadXdrU32(fd, &request->transaction_id) &&
         ReadXdrU32(fd, &request->statement_id) &&
         ReadXdrU32(fd, &request->sql_dialect) &&
         ReadXdrString(fd, &request->sql_text) &&
         ReadXdrString(fd, &request->items) &&
         ReadXdrU32(fd, &request->buffer_length);
}

bool ReadFirebirdSqlDataBody(int fd, bool with_transaction, FirebirdSqlDataRequest* request) {
  if (!ReadXdrU32(fd, &request->statement_id)) return false;
  if (with_transaction && !ReadXdrU32(fd, &request->transaction_id)) return false;
  return ReadXdrString(fd, &request->blr) &&
         ReadXdrU32(fd, &request->message_number) &&
         ReadXdrU32(fd, &request->message_count);
}

bool ReadFirebirdFreeStatementBody(int fd, FirebirdFreeStatementRequest* request) {
  return ReadXdrU32(fd, &request->statement_id) &&
         ReadXdrU32(fd, &request->option);
}

bool ReadFirebirdTransactionBody(int fd, FirebirdTransactionRequest* request) {
  return ReadXdrU32(fd, &request->database_id) &&
         ReadXdrString(fd, &request->parameter_buffer);
}

bool ReadFirebirdBlobBody(int fd, bool has_parameter_buffer, FirebirdBlobRequest* request) {
  if (has_parameter_buffer && !ReadXdrString(fd, &request->parameter_buffer)) return false;
  return ReadXdrU32(fd, &request->transaction_id) &&
         ReadXdrU32(fd, &request->blob_id_high) &&
         ReadXdrU32(fd, &request->blob_id_low);
}

bool ReadFirebirdSegmentBody(int fd, FirebirdSegmentRequest* request) {
  return ReadXdrU32(fd, &request->blob_id) &&
         ReadXdrU32(fd, &request->segment_length) &&
         ReadXdrString(fd, &request->segment);
}

bool ReadFirebirdEventBody(int fd, bool cancel_only, FirebirdEventRequest* request) {
  if (!ReadXdrU32(fd, &request->database_id)) return false;
  if (cancel_only) return ReadXdrU32(fd, &request->client_event_id);
  return ReadXdrString(fd, &request->items) &&
         ReadXdrU32(fd, &request->ast) &&
         ReadXdrU32(fd, &request->argument) &&
         ReadXdrU32(fd, &request->client_event_id);
}

std::string FirebirdStatementAllocateJson(std::uint32_t database_id,
                                          std::uint32_t statement_id) {
  return "{\"ok\":true,\"wire_operation\":\"op_allocate_statement\","
         "\"database_handle\":" + std::to_string(database_id) + ","
         "\"statement_handle\":" + std::to_string(statement_id) + ","
         "\"runtime_policy\":\"statement_handle_allocated\"}";
}

std::string FirebirdStatementPrepareJson(
    const FirebirdPrepareStatementRequest& request,
    const ParseResult& parsed) {
  return "{\"ok\":true,\"wire_operation\":\"op_prepare_statement\","
         "\"statement_handle\":" + std::to_string(request.statement_id) + ","
         "\"transaction_handle\":" + std::to_string(request.transaction_id) + ","
         "\"sql_dialect\":" + std::to_string(request.sql_dialect) + ","
         "\"items_length\":" + std::to_string(request.items.size()) + ","
         "\"buffer_length\":" + std::to_string(request.buffer_length) + ","
         "\"statement_family\":\"" + EscapeJsonLocal(parsed.statement_family) + "\","
         "\"operation_family\":\"" + EscapeJsonLocal(parsed.operation_family) + "\","
         "\"lifecycle_operation_id\":\"" + EscapeJsonLocal(parsed.lifecycle_operation_id) + "\","
         "\"sblr_operation\":\"" + EscapeJsonLocal(parsed.sblr_operation) + "\","
         "\"engine_api_function\":\"" + EscapeJsonLocal(parsed.engine_api_function) + "\","
         "\"scratchbird_lifecycle_api\":" + (parsed.scratchbird_lifecycle_api ? "true" : "false") + ","
         "\"exact_emulated_diagnostic\":" + (parsed.exact_emulated_diagnostic ? "true" : "false") + ","
         "\"real_firebird_file_effects\":false,"
         "\"donor_engine_sql_executed\":false,"
         "\"sblr\":" + parsed.sblr_envelope + ","
         "\"sql_text_included\":false,"
         "\"runtime_policy\":\"prepared_to_sblr_descriptor\"}";
}

std::string FirebirdStatementInfoJson(const FirebirdInfoRequest& request,
                                      const FirebirdRemoteHandle& handle) {
  return "{\"ok\":true,\"wire_operation\":\"op_info_sql\","
         "\"statement_handle\":" + std::to_string(request.object_id) + ","
         "\"handle_kind\":\"" + EscapeJsonLocal(handle.kind) + "\","
         "\"items_length\":" + std::to_string(request.items.size()) + ","
         "\"buffer_length\":" + std::to_string(request.buffer_length) + ","
         "\"statement_descriptor\":" + handle.descriptor_json + ","
         "\"runtime_policy\":\"statement_info_descriptor\"}";
}

std::string FirebirdStatementExecuteJson(const FirebirdSqlDataRequest& request,
                                         const FirebirdRemoteHandle& handle) {
  return "{\"ok\":true,\"wire_operation\":\"op_execute\","
         "\"statement_handle\":" + std::to_string(request.statement_id) + ","
         "\"transaction_handle\":" + std::to_string(request.transaction_id) + ","
         "\"blr_length\":" + std::to_string(request.blr.size()) + ","
         "\"message_number\":" + std::to_string(request.message_number) + ","
         "\"message_count\":" + std::to_string(request.message_count) + ","
         "\"statement_descriptor\":" + handle.descriptor_json + ","
         "\"runtime_policy\":\"execute_admitted_to_sblr_descriptor\"}";
}

std::string FirebirdTransactionStartJson(const FirebirdTransactionRequest& request,
                                         std::uint32_t transaction_id) {
  return "{\"ok\":true,\"wire_operation\":\"op_transaction\","
         "\"database_handle\":" + std::to_string(request.database_id) + ","
         "\"transaction_handle\":" + std::to_string(transaction_id) + ","
         "\"tpb_length\":" + std::to_string(request.parameter_buffer.size()) + ","
         "\"real_firebird_file_effects\":false,"
         "\"donor_engine_sql_executed\":false,"
         "\"parser_storage_authority\":false,"
         "\"parser_transaction_finality_authority\":false,"
         "\"runtime_policy\":\"emulated_transaction_handle_started\"}";
}

std::string FirebirdReleaseJson(std::string_view operation,
                                std::uint32_t object_id,
                                std::string_view policy) {
  return "{\"ok\":true,\"wire_operation\":\"" + std::string(operation) + "\","
         "\"object_handle\":" + std::to_string(object_id) + ","
         + FirebirdWireLifecycleMappingJson(operation) +
         "\"real_firebird_file_effects\":false,"
         "\"donor_engine_sql_executed\":false,"
         "\"parser_storage_authority\":false,"
         "\"parser_transaction_finality_authority\":false,"
         "\"runtime_policy\":\"" + std::string(policy) + "\"}";
}

std::string FirebirdBlobJson(std::string_view operation,
                             const FirebirdBlobRequest& request,
                             std::uint32_t blob_handle,
                             std::size_t payload_size) {
  return "{\"ok\":true,\"wire_operation\":\"" + std::string(operation) + "\","
         "\"transaction_handle\":" + std::to_string(request.transaction_id) + ","
         "\"blob_handle\":" + std::to_string(blob_handle) + ","
         "\"blob_id_high\":" + std::to_string(request.blob_id_high) + ","
         "\"blob_id_low\":" + std::to_string(request.blob_id_low) + ","
         "\"bpb_length\":" + std::to_string(request.parameter_buffer.size()) + ","
         "\"payload_size\":" + std::to_string(payload_size) + ","
         "\"real_firebird_file_effects\":false,"
         "\"runtime_policy\":\"emulated_blob_handle_admitted\"}";
}

std::string FirebirdSegmentJson(std::string_view operation,
                                const FirebirdSegmentRequest& request,
                                std::size_t payload_size,
                                std::string_view policy) {
  return "{\"ok\":true,\"wire_operation\":\"" + std::string(operation) + "\","
         "\"blob_handle\":" + std::to_string(request.blob_id) + ","
         "\"segment_length\":" + std::to_string(request.segment_length) + ","
         "\"payload_size\":" + std::to_string(payload_size) + ","
         "\"runtime_policy\":\"" + std::string(policy) + "\"}";
}

std::string FirebirdEventJson(std::string_view operation,
                              const FirebirdEventRequest& request,
                              std::uint32_t event_handle) {
  return "{\"ok\":true,\"wire_operation\":\"" + std::string(operation) + "\","
         "\"database_handle\":" + std::to_string(request.database_id) + ","
         "\"event_handle\":" + std::to_string(event_handle) + ","
         "\"client_event_id\":" + std::to_string(request.client_event_id) + ","
         "\"event_items_length\":" + std::to_string(request.items.size()) + ","
         "\"real_firebird_file_effects\":false,"
         "\"donor_engine_sql_executed\":false,"
         "\"parser_storage_authority\":false,"
         "\"parser_transaction_finality_authority\":false,"
         "\"runtime_policy\":\"emulated_event_registration\"}";
}

std::string FirebirdInvalidHandleJson(std::string_view operation,
                                      std::string_view diagnostic_code,
                                      std::string_view policy) {
  return "{\"ok\":false,\"wire_operation\":\"" + std::string(operation) + "\","
         "\"diagnostic_code\":\"" + std::string(diagnostic_code) + "\","
         "\"runtime_policy\":\"" + std::string(policy) + "\"}";
}

std::string FirebirdMalformedPacketJson(std::string_view operation) {
  return "{\"ok\":false,\"wire_operation\":\"" + std::string(operation) + "\","
         "\"diagnostic_code\":\"FIREBIRD.WIRE.REQUEST_INVALID\","
         "\"runtime_policy\":\"fail_closed_malformed_packet\"}";
}

std::vector<std::uint8_t> FirebirdResponsePacket(
    std::uint32_t object_id,
    std::string_view response_data,
    const std::vector<std::uint32_t>& status_vector) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 9);  // op_response
  AppendXdrU32(&out, object_id);
  AppendXdrU32(&out, 0);  // blob id high
  AppendXdrU32(&out, 0);  // blob id low
  AppendXdrString(&out, response_data);
  for (const auto token : status_vector) AppendXdrU32(&out, token);
  return out;
}

std::vector<std::uint8_t> FirebirdSuccessResponsePacket(
    std::uint32_t object_id,
    std::string_view response_data = {}) {
  return FirebirdResponsePacket(object_id, response_data, {1, 0, 0});
}

std::vector<std::uint8_t> FirebirdWishListResponsePacket(std::string_view response_data) {
  return FirebirdResponsePacket(0, response_data, {1, 335544378u, 0});
}

std::vector<std::uint8_t> FirebirdStatusResponsePacket(std::uint32_t status_code,
                                                       std::string_view response_data) {
  return FirebirdResponsePacket(0, response_data, {1, status_code, 0});
}

std::vector<std::uint8_t> FirebirdFetchEofPacket() {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 66);   // op_fetch_response
  AppendXdrU32(&out, 100);  // EOF/no row status
  AppendXdrU32(&out, 0);    // no messages
  return out;
}

bool PeerStartsWithFirebirdConnect(int fd) {
  std::uint8_t prefix[4]{};
  for (;;) {
    const auto rc = ::recv(fd, prefix, sizeof(prefix), MSG_PEEK);
    if (rc == static_cast<ssize_t>(sizeof(prefix))) return IsXdrOpcode(prefix, 1);
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
}

int ServeFirebirdBinaryConnect(int fd) {
  constexpr std::uint32_t kIscBadDbHandle = 335544324u;
  constexpr std::uint32_t kIscBadBlobHandle = 335544328u;
  constexpr std::uint32_t kIscBadTransHandle = 335544332u;
  constexpr std::uint32_t kIscSegstrEof = 335544367u;
  constexpr std::uint32_t kIscWishList = 335544378u;
  constexpr std::uint32_t kIscBadEventsHandle = 335545021u;
  std::uint32_t opcode = 0;
  if (!ReadXdrU32(fd, &opcode) || opcode != 1) return 1;
  FirebirdConnectOffer offer;
  if (!ReadFirebirdConnectBody(fd, &offer) || !offer.protocol12) {
    const auto reject = FirebirdOpRejectPacket();
    return WritePacket(fd, reject) ? 0 : 1;
  }
  const auto accept = FirebirdOpAcceptProtocol12Packet();
  if (!WritePacket(fd, accept)) return 1;

  FirebirdBinarySessionState state;
  while (ReadXdrU32(fd, &opcode)) {
    if (opcode == 6) return 0;  // op_disconnect
    if (opcode == 93) {
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(0))) return 1;
      continue;
    }
    if (opcode == 19 || opcode == 20 || opcode == 82) {
      FirebirdAttachRequest request;
      if (!ReadFirebirdAttachBody(fd, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      const bool service = opcode == 82;
      const auto decoded =
          DecodeFirebirdParameterBuffer(service ? "SPB" : "DPB", request.parameter_buffer);
      if (!decoded.ok) {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 FirebirdBufferFailureJson(opcode, decoded)))) {
          return 1;
        }
        continue;
      }
      const auto handle_id = state.next_handle++;
      const std::string descriptor =
          FirebirdAttachDescriptorJson(opcode, request, handle_id, decoded);
      state.handles.push_back(
          {handle_id, 0, request.object_id,
           service ? "service" : (opcode == 20 ? "database_create" : "database"),
           descriptor, {}});
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(handle_id, descriptor))) return 1;
      continue;
    }
    if (opcode == 85) {  // op_service_start
      FirebirdInfoRequest request;
      if (!ReadFirebirdInfoBody(fd, false, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      const auto* handle = FindHandle(state, request.object_id);
      if (handle == nullptr || handle->kind != "service") {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 "{\"ok\":false,\"wire_operation\":\"op_service_start\","
                                 "\"runtime_policy\":\"invalid_service_handle\"}"))) {
          return 1;
        }
        continue;
      }
      const auto decoded = DecodeFirebirdParameterBuffer("SPB", request.items);
      if (!decoded.ok) {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 FirebirdServiceStartJson(request, decoded)))) {
          return 1;
        }
        continue;
      }
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               request.object_id, FirebirdServiceStartJson(request, decoded)))) {
        return 1;
      }
      continue;
    }
    if (opcode == 84) {  // op_service_info
      FirebirdInfoRequest request;
      if (!ReadFirebirdInfoBody(fd, true, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      const auto* handle = FindHandle(state, request.object_id);
      if (handle == nullptr || handle->kind != "service") {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 "{\"ok\":false,\"wire_operation\":\"op_service_info\","
                                 "\"runtime_policy\":\"invalid_service_handle\"}"))) {
          return 1;
        }
        continue;
      }
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               request.object_id, FirebirdServiceInfoJson(request, *handle)))) {
        return 1;
      }
      continue;
    }
    if (opcode == 29) {  // op_transaction
      FirebirdTransactionRequest request;
      if (!ReadFirebirdTransactionBody(fd, &request)) {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscWishList,
                                 FirebirdMalformedPacketJson("op_transaction")))) {
          return 1;
        }
        continue;
      }
      const auto* database = FindHandle(state, request.database_id);
      if (database == nullptr ||
          (database->kind != "database" && database->kind != "database_create")) {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscBadDbHandle,
                                 FirebirdInvalidHandleJson(
                                     "op_transaction", "isc_bad_db_handle",
                                     "invalid_database_handle")))) {
          return 1;
        }
        continue;
      }
      const auto transaction_id = state.next_handle++;
      const std::string descriptor =
          FirebirdTransactionStartJson(request, transaction_id);
      state.handles.push_back(
          {transaction_id, request.database_id, 0, "transaction", descriptor, {}});
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(transaction_id, descriptor))) {
        return 1;
      }
      continue;
    }
    if (opcode == 30 || opcode == 31 || opcode == 50 || opcode == 86) {
      std::uint32_t transaction_id = 0;
      if (!ReadFirebirdReleaseBody(fd, &transaction_id)) {
        const auto op_name = opcode == 30 ? "op_commit" :
                             opcode == 31 ? "op_rollback" :
                             opcode == 50 ? "op_commit_retaining" :
                                            "op_rollback_retaining";
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscWishList,
                                 FirebirdMalformedPacketJson(op_name)))) {
          return 1;
        }
        continue;
      }
      const auto* transaction = FindHandle(state, transaction_id);
      if (transaction == nullptr || transaction->kind != "transaction") {
        const auto op_name = opcode == 30 ? "op_commit" :
                             opcode == 31 ? "op_rollback" :
                             opcode == 50 ? "op_commit_retaining" :
                                            "op_rollback_retaining";
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscBadTransHandle,
                                 FirebirdInvalidHandleJson(
                                     op_name, "isc_bad_trans_handle",
                                     "invalid_transaction_handle")))) {
          return 1;
        }
        continue;
      }
      const auto op_name = opcode == 30 ? "op_commit" :
                           opcode == 31 ? "op_rollback" :
                           opcode == 50 ? "op_commit_retaining" :
                                          "op_rollback_retaining";
      if (opcode == 30 || opcode == 31) {
        RemoveHandlesByParent(&state, transaction_id);
        (void)RemoveHandle(&state, transaction_id);
      }
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               transaction_id,
                               FirebirdReleaseJson(op_name, transaction_id,
                                                   "emulated_transaction_closed")))) {
        return 1;
      }
      continue;
    }
    if (opcode == 34 || opcode == 35 || opcode == 56 || opcode == 57) {
      FirebirdBlobRequest request;
      if (!ReadFirebirdBlobBody(fd, opcode == 56 || opcode == 57, &request)) {
        const auto op_name = opcode == 34 ? "op_create_blob" :
                             opcode == 35 ? "op_open_blob" :
                             opcode == 56 ? "op_open_blob2" :
                                            "op_create_blob2";
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscWishList,
                                 FirebirdMalformedPacketJson(op_name)))) {
          return 1;
        }
        continue;
      }
      const auto* transaction = FindHandle(state, request.transaction_id);
      if (transaction == nullptr || transaction->kind != "transaction") {
        const auto op_name = opcode == 34 ? "op_create_blob" :
                             opcode == 35 ? "op_open_blob" :
                             opcode == 56 ? "op_open_blob2" :
                                            "op_create_blob2";
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscBadTransHandle,
                                 FirebirdInvalidHandleJson(
                                     op_name, "isc_bad_trans_handle",
                                     "invalid_transaction_handle")))) {
          return 1;
        }
        continue;
      }
      const auto blob_handle = state.next_handle++;
      const auto op_name = opcode == 34 ? "op_create_blob" :
                           opcode == 35 ? "op_open_blob" :
                           opcode == 56 ? "op_open_blob2" :
                                          "op_create_blob2";
      const std::string descriptor =
          FirebirdBlobJson(op_name, request, blob_handle, 0);
      state.handles.push_back(
          {blob_handle, request.transaction_id, request.blob_id_low,
           "blob", descriptor, {}});
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(blob_handle, descriptor))) {
        return 1;
      }
      continue;
    }
    if (opcode == 37 || opcode == 36) {
      FirebirdSegmentRequest request;
      if (!ReadFirebirdSegmentBody(fd, &request)) {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscWishList,
                                 FirebirdMalformedPacketJson(
                                     opcode == 37 ? "op_put_segment" :
                                                    "op_get_segment")))) {
          return 1;
        }
        continue;
      }
      auto* blob = FindMutableHandle(&state, request.blob_id);
      if (blob == nullptr || blob->kind != "blob") {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscBadBlobHandle,
                                 FirebirdInvalidHandleJson(
                                     opcode == 37 ? "op_put_segment" :
                                                    "op_get_segment",
                                     "isc_bad_segstr_handle",
                                     "invalid_blob_handle")))) {
          return 1;
        }
        continue;
      }
      if (opcode == 37) {
        blob->data.insert(blob->data.end(), request.segment.begin(), request.segment.end());
        if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                                 request.blob_id,
                                 FirebirdSegmentJson("op_put_segment", request,
                                                     blob->data.size(),
                                                     "emulated_blob_segment_stored")))) {
          return 1;
        }
        continue;
      }
      if (!WritePacket(fd, FirebirdStatusResponsePacket(
                               kIscSegstrEof,
                               FirebirdSegmentJson("op_get_segment", request,
                                                   blob->data.size(),
                                                   "emulated_blob_segment_eof")))) {
        return 1;
      }
      continue;
    }
    if (opcode == 38 || opcode == 39) {
      std::uint32_t blob_id = 0;
      if (!ReadFirebirdReleaseBody(fd, &blob_id)) {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscWishList,
                                 FirebirdMalformedPacketJson(
                                     opcode == 38 ? "op_cancel_blob" :
                                                    "op_close_blob")))) {
          return 1;
        }
        continue;
      }
      const auto* blob = FindHandle(state, blob_id);
      if (blob == nullptr || blob->kind != "blob") {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscBadBlobHandle,
                                 FirebirdInvalidHandleJson(
                                     opcode == 38 ? "op_cancel_blob" :
                                                    "op_close_blob",
                                     "isc_bad_segstr_handle",
                                     "invalid_blob_handle")))) {
          return 1;
        }
        continue;
      }
      (void)RemoveHandle(&state, blob_id);
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               blob_id,
                               FirebirdReleaseJson(
                                   opcode == 38 ? "op_cancel_blob" : "op_close_blob",
                                   blob_id, "emulated_blob_handle_released")))) {
        return 1;
      }
      continue;
    }
    if (opcode == 48 || opcode == 49) {
      FirebirdEventRequest request;
      if (!ReadFirebirdEventBody(fd, opcode == 49, &request)) {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscWishList,
                                 FirebirdMalformedPacketJson(
                                     opcode == 48 ? "op_que_events" :
                                                    "op_cancel_events")))) {
          return 1;
        }
        continue;
      }
      const auto* database = FindHandle(state, request.database_id);
      if (database == nullptr ||
          (database->kind != "database" && database->kind != "database_create")) {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscBadDbHandle,
                                 FirebirdInvalidHandleJson(
                                     opcode == 48 ? "op_que_events" :
                                                    "op_cancel_events",
                                     "isc_bad_db_handle",
                                     "invalid_database_handle")))) {
          return 1;
        }
        continue;
      }
      if (opcode == 48) {
        const auto event_handle = state.next_handle++;
        const std::string descriptor =
            FirebirdEventJson("op_que_events", request, event_handle);
        state.handles.push_back(
            {event_handle, request.database_id, request.client_event_id,
             "event", descriptor, {}});
        if (!WritePacket(fd, FirebirdSuccessResponsePacket(event_handle, descriptor))) {
          return 1;
        }
        continue;
      }
      const auto* event = FindEventByClientId(state, request.database_id,
                                             request.client_event_id);
      if (event == nullptr) {
        if (!WritePacket(fd, FirebirdStatusResponsePacket(
                                 kIscBadEventsHandle,
                                 FirebirdInvalidHandleJson(
                                     "op_cancel_events", "isc_bad_events_handle",
                                     "invalid_event_handle")))) {
          return 1;
        }
        continue;
      }
      const auto event_handle = event->id;
      (void)RemoveHandle(&state, event_handle);
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               event_handle,
                               FirebirdReleaseJson("op_cancel_events", event_handle,
                                                   "emulated_event_cancelled")))) {
        return 1;
      }
      continue;
    }
    if (opcode == 21 || opcode == 81 || opcode == 83) {
      std::uint32_t object_id = 0;
      if (!ReadFirebirdReleaseBody(fd, &object_id)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      RemoveHandlesByParent(&state, object_id);
      (void)RemoveHandle(&state, object_id);
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               object_id,
                               FirebirdReleaseJson(opcode == 21 ? "op_detach" :
                                                   opcode == 81 ? "op_drop_database" :
                                                                  "op_service_detach",
                                                   object_id,
                                                   "emulated_attachment_released")))) {
        return 1;
      }
      continue;
    }
    if (opcode == 62) {  // op_allocate_statement
      std::uint32_t database_id = 0;
      if (!ReadFirebirdReleaseBody(fd, &database_id)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      const auto* database = FindHandle(state, database_id);
      if (database == nullptr ||
          (database->kind != "database" && database->kind != "database_create")) {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 "{\"ok\":false,\"wire_operation\":\"op_allocate_statement\","
                                 "\"runtime_policy\":\"invalid_database_handle\"}"))) {
          return 1;
        }
        continue;
      }
      const auto statement_id = state.next_handle++;
      const std::string descriptor =
          FirebirdStatementAllocateJson(database_id, statement_id);
      state.handles.push_back({statement_id, database_id, 0, "statement", descriptor, {}});
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(statement_id, descriptor))) return 1;
      continue;
    }
    if (opcode == 68) {  // op_prepare_statement
      FirebirdPrepareStatementRequest request;
      if (!ReadFirebirdPrepareStatementBody(fd, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      auto* statement = FindMutableHandle(&state, request.statement_id);
      if (statement == nullptr || statement->kind != "statement") {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 "{\"ok\":false,\"wire_operation\":\"op_prepare_statement\","
                                 "\"runtime_policy\":\"invalid_statement_handle\"}"))) {
          return 1;
        }
        continue;
      }
      const std::string sql_text(request.sql_text.begin(), request.sql_text.end());
      const auto parsed = ParseStatement(sql_text);
      if (!parsed.ok) {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(parsed.message_vector_json))) {
          return 1;
        }
        continue;
      }
      statement->descriptor_json = FirebirdStatementPrepareJson(request, parsed);
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               request.statement_id, statement->descriptor_json))) {
        return 1;
      }
      continue;
    }
    if (opcode == 70) {  // op_info_sql
      FirebirdInfoRequest request;
      if (!ReadFirebirdInfoBody(fd, false, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      const auto* statement = FindHandle(state, request.object_id);
      if (statement == nullptr || statement->kind != "statement") {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 "{\"ok\":false,\"wire_operation\":\"op_info_sql\","
                                 "\"runtime_policy\":\"invalid_statement_handle\"}"))) {
          return 1;
        }
        continue;
      }
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               request.object_id, FirebirdStatementInfoJson(request, *statement)))) {
        return 1;
      }
      continue;
    }
    if (opcode == 63) {  // op_execute
      FirebirdSqlDataRequest request;
      if (!ReadFirebirdSqlDataBody(fd, true, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      const auto* statement = FindHandle(state, request.statement_id);
      if (statement == nullptr || statement->kind != "statement") {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 "{\"ok\":false,\"wire_operation\":\"op_execute\","
                                 "\"runtime_policy\":\"invalid_statement_handle\"}"))) {
          return 1;
        }
        continue;
      }
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(
                               request.statement_id,
                               FirebirdStatementExecuteJson(request, *statement)))) {
        return 1;
      }
      continue;
    }
    if (opcode == 65) {  // op_fetch
      FirebirdSqlDataRequest request;
      if (!ReadFirebirdSqlDataBody(fd, false, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      const auto* statement = FindHandle(state, request.statement_id);
      if (statement == nullptr || statement->kind != "statement") {
        if (!WritePacket(fd, FirebirdWishListResponsePacket(
                                 "{\"ok\":false,\"wire_operation\":\"op_fetch\","
                                 "\"runtime_policy\":\"invalid_statement_handle\"}"))) {
          return 1;
        }
        continue;
      }
      if (!WritePacket(fd, FirebirdFetchEofPacket())) return 1;
      continue;
    }
    if (opcode == 67) {  // op_free_statement
      FirebirdFreeStatementRequest request;
      if (!ReadFirebirdFreeStatementBody(fd, &request)) {
        const auto reject = FirebirdOpRejectPacket();
        return WritePacket(fd, reject) ? 0 : 1;
      }
      (void)RemoveHandle(&state, request.statement_id);
      if (!WritePacket(fd, FirebirdSuccessResponsePacket(request.statement_id))) return 1;
      continue;
    }
    const auto response = FirebirdWishListResponsePacket(
        "{\"ok\":false,\"wire_operation\":\"unknown\",\"runtime_policy\":\"status_vector_diagnostic\"}");
    if (!WritePacket(fd, response)) return 1;
  }
  return 0;
}
#endif

} // namespace

int ServeFirebirdWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  if (PeerStartsWithFirebirdConnect(fd)) {
    return ServeFirebirdBinaryConnect(fd);
  }
  std::string line;
  while (ReadLine(fd, &line)) {
    bool close = false;
    const auto response = HandleLine(line, &close);
    if (!WriteAll(fd, response)) return 1;
    if (close) break;
  }
  return 0;
#endif
}

} // namespace scratchbird::parser::firebird
