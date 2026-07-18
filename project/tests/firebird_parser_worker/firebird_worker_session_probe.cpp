// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_worker_session.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#ifndef _WIN32
void CloseFd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}
#endif

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool ExpectResponseToken(const std::string& data_text,
                         std::string_view expected_response_data) {
  if (expected_response_data.empty() ||
      data_text.find(expected_response_data) != std::string::npos) {
    return true;
  }
  std::cerr << "Firebird response data missing expected token: "
            << expected_response_data << "\nresponse: " << data_text << '\n';
  return false;
}

#ifndef _WIN32
void AppendXdrU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
}

void AppendXdrString(std::vector<std::uint8_t>* out, std::string_view value) {
  AppendXdrU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  while ((out->size() & 3u) != 0) out->push_back(0);
}

std::uint32_t ReadXdrU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

bool ReadBoundedXdrString(const std::vector<std::uint8_t>& bytes,
                          std::size_t* offset,
                          std::string* value) {
  if (offset == nullptr || value == nullptr || *offset + 4 > bytes.size()) {
    return false;
  }
  const std::uint32_t length = ReadXdrU32(bytes, *offset);
  *offset += 4;
  const std::size_t padded =
      static_cast<std::size_t>(length) + ((4u - (length & 3u)) & 3u);
  if (*offset + padded > bytes.size()) return false;
  value->assign(reinterpret_cast<const char*>(bytes.data() + *offset), length);
  *offset += padded;
  return true;
}

bool ExpectConversionPresentation(std::string_view source) {
  scratchbird::parser::ipc::MessageVectorSet messages;
  messages.diagnostics.push_back(scratchbird::parser::ipc::MakeDiagnostic(
      "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR",
      "prose is not a presentation contract", "engine",
      {{"detail", "must not be parsed"},
       {"conversion_input_text", std::string(source)}}));
  const auto presentation =
      scratchbird::parser::firebird::PresentFirebirdConversionInputDiagnostic(
          messages, "op_execute");
  bool ok = Expect(presentation.has_value(),
                   "structured conversion diagnostic was not presented");
  if (!presentation) return false;
  ok = Expect(presentation->conversion_input_text == source,
              "conversion presentation changed the engine input") && ok;
  ok = ExpectResponseToken(presentation->response_json, "\"ok\":false") && ok;
  ok = ExpectResponseToken(presentation->response_json,
                           "FIREBIRD.CONVERSION.INPUT_ERROR") && ok;
  ok = ExpectResponseToken(presentation->response_json,
                           "\"sqlstate\":\"22018\"") && ok;
  ok = Expect(presentation->response_json.find("must not be parsed") ==
                  std::string::npos,
              "worker parsed diagnostic detail prose") && ok;
  ok = Expect(presentation->response_json.find("\"row") ==
                  std::string::npos,
              "conversion failure presentation exposed a result row") && ok;
  if (source == "bad\"\\value") {
    ok = ExpectResponseToken(
             presentation->response_json,
             "\"conversion_input\":\"bad\\\"\\\\value\"") && ok;
  }

  const auto& status = presentation->encoded_status_vector;
  std::size_t offset = 0;
  ok = Expect(status.size() >= 20 && ReadXdrU32(status, offset) == 1,
              "conversion status missing isc_arg_gds") && ok;
  offset += 4;
  ok = Expect(offset + 4 <= status.size() &&
                  ReadXdrU32(status, offset) == 335544334u,
              "conversion status did not use isc_convert_error") && ok;
  offset += 4;
  ok = Expect(offset + 4 <= status.size() &&
                  ReadXdrU32(status, offset) == 2,
              "conversion status missing isc_arg_string") && ok;
  offset += 4;
  std::string status_source;
  ok = Expect(ReadBoundedXdrString(status, &offset, &status_source) &&
                  status_source == source,
              "conversion status source string drifted") && ok;
  ok = Expect(offset + 4 <= status.size() &&
                  ReadXdrU32(status, offset) == 19,
              "conversion status missing isc_arg_sql_state") && ok;
  offset += 4;
  std::string sqlstate;
  ok = Expect(ReadBoundedXdrString(status, &offset, &sqlstate) &&
                  sqlstate == "22018",
              "conversion status SQLSTATE drifted") && ok;
  ok = Expect(offset + 4 == status.size() &&
                  ReadXdrU32(status, offset) == 0,
              "conversion status missing terminal isc_arg_end") && ok;
  return ok;
}

bool RunConversionDiagnosticPresentationCase() {
  bool ok = true;
  for (const std::string_view source :
       {std::string_view("1"), std::string_view("29.2.2002"),
        std::string_view("9:11:60"),
        std::string_view("bad\"\\value")}) {
    ok = ExpectConversionPresentation(source) && ok;
  }

  constexpr char kHex[] = "0123456789ABCDEF";
  std::string all_controls;
  std::string expected_control_escapes;
  for (unsigned value = 0; value < 0x20u; ++value) {
    all_controls.push_back(static_cast<char>(value));
    expected_control_escapes += "\\u00";
    expected_control_escapes.push_back(kHex[(value >> 4) & 0x0fu]);
    expected_control_escapes.push_back(kHex[value & 0x0fu]);
  }
  const auto escaped_controls = scratchbird::parser::firebird::
      EscapeFirebirdConversionDiagnosticJsonString(all_controls);
  ok = Expect(escaped_controls.has_value() &&
                  *escaped_controls == expected_control_escapes,
              "conversion JSON did not escape every C0 control") && ok;
  if (escaped_controls) {
    for (const unsigned char ch : *escaped_controls) {
      ok = Expect(ch >= 0x20u,
                  "conversion JSON retained a raw C0 control") && ok;
    }
  }

  const std::string non_nul_controls = all_controls.substr(1);
  ok = ExpectConversionPresentation(non_nul_controls) && ok;
  scratchbird::parser::ipc::MessageVectorSet control_messages;
  control_messages.diagnostics.push_back(
      scratchbird::parser::ipc::MakeDiagnostic(
          "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR", "ignored",
          "engine", {{"conversion_input_text", non_nul_controls}}));
  const auto control_presentation = scratchbird::parser::firebird::
      PresentFirebirdConversionInputDiagnostic(control_messages, "op_execute");
  if (control_presentation) {
    for (unsigned value = 1; value < 0x20u; ++value) {
      std::string escape = "\\u00";
      escape.push_back(kHex[(value >> 4) & 0x0fu]);
      escape.push_back(kHex[value & 0x0fu]);
      ok = ExpectResponseToken(control_presentation->response_json, escape) && ok;
    }
  }

  const std::vector<std::string> invalid_utf8{
      std::string("\x80", 1),
      std::string("\xC0\xAF", 2),
      std::string("\xE0\x80\x80", 3),
      std::string("\xED\xA0\x80", 3),
      std::string("\xF4\x90\x80\x80", 4),
      std::string("\xF0\x9F", 2),
  };
  for (const auto& invalid : invalid_utf8) {
    ok = Expect(
             !scratchbird::parser::firebird::
                  EscapeFirebirdConversionDiagnosticJsonString(invalid)
                      .has_value(),
             "conversion JSON accepted malformed UTF-8") && ok;
    scratchbird::parser::ipc::MessageVectorSet invalid_messages;
    invalid_messages.diagnostics.push_back(
        scratchbird::parser::ipc::MakeDiagnostic(
            "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR", "ignored",
            "engine", {{"conversion_input_text", invalid}}));
    ok = Expect(!scratchbird::parser::firebird::
                     PresentFirebirdConversionInputDiagnostic(
                         invalid_messages, "op_execute")
                     .has_value(),
                "conversion presenter did not fail malformed UTF-8 to generic") &&
         ok;
  }

  const auto present = [](scratchbird::parser::ipc::MessageVectorSet messages) {
    return scratchbird::parser::firebird::
        PresentFirebirdConversionInputDiagnostic(messages, "op_execute");
  };
  scratchbird::parser::ipc::MessageVectorSet unrelated;
  unrelated.diagnostics.push_back(scratchbird::parser::ipc::MakeDiagnostic(
      "SB_DIAG_FUNCTION_INVALID_INPUT", "ERROR", "generic", "engine",
      {{"conversion_input_text", "29.2.2002"}}));
  ok = Expect(!present(std::move(unrelated)).has_value(),
              "generic invalid input was mapped as Firebird conversion") && ok;

  scratchbird::parser::ipc::MessageVectorSet missing;
  missing.diagnostics.push_back(scratchbird::parser::ipc::MakeDiagnostic(
      "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR", "missing", "engine"));
  ok = Expect(!present(std::move(missing)).has_value(),
              "missing conversion field did not fail closed") && ok;

  scratchbird::parser::ipc::MessageVectorSet empty;
  empty.diagnostics.push_back(scratchbird::parser::ipc::MakeDiagnostic(
      "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR", "empty", "engine",
      {{"conversion_input_text", ""}}));
  ok = Expect(!present(std::move(empty)).has_value(),
              "empty conversion field did not fail closed") && ok;

  scratchbird::parser::ipc::MessageVectorSet duplicate;
  duplicate.diagnostics.push_back(scratchbird::parser::ipc::MakeDiagnostic(
      "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR", "duplicate", "engine",
      {{"conversion_input_text", "1"}, {"conversion_input_text", "2"}}));
  ok = Expect(!present(std::move(duplicate)).has_value(),
              "duplicate conversion fields did not fail closed") && ok;

  scratchbird::parser::ipc::MessageVectorSet oversized;
  oversized.diagnostics.push_back(scratchbird::parser::ipc::MakeDiagnostic(
      "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR", "oversized", "engine",
      {{"conversion_input_text", std::string(1025, 'x')}}));
  ok = Expect(!present(std::move(oversized)).has_value(),
              "oversized conversion field did not fail closed") && ok;

  scratchbird::parser::ipc::MessageVectorSet embedded_nul;
  embedded_nul.diagnostics.push_back(scratchbird::parser::ipc::MakeDiagnostic(
      "SB_DIAG_FUNCTION_CONVERSION_INPUT", "ERROR", "nul", "engine",
      {{"conversion_input_text", std::string("a\0b", 3)}}));
  ok = Expect(!present(std::move(embedded_nul)).has_value(),
              "NUL conversion field did not fail closed") && ok;
  return ok;
}

bool WriteAll(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto rc = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool ReadExact(int fd, std::vector<std::uint8_t>* bytes, std::size_t count) {
  bytes->assign(count, 0);
  std::size_t read_total = 0;
  while (read_total < count) {
    const auto rc = ::read(fd, bytes->data() + read_total, count - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool ReadFirebirdResponse(int fd,
                          std::vector<std::uint8_t>* header,
                          std::vector<std::uint8_t>* data_and_status,
                          std::string* data_text) {
  if (!ReadExact(fd, header, 20)) return false;
  const auto data_len = ReadXdrU32(*header, 16);
  const auto padding = (4u - (data_len & 3u)) & 3u;
  if (!ReadExact(fd, data_and_status, static_cast<std::size_t>(data_len + padding))) {
    return false;
  }
  for (;;) {
    std::vector<std::uint8_t> word;
    if (!ReadExact(fd, &word, 4)) return false;
    data_and_status->insert(data_and_status->end(), word.begin(), word.end());
    const auto tag = ReadXdrU32(word, 0);
    if (tag == 0) break;
    if (tag == 1 || tag == 4 || tag == 6 || tag == 7 || tag == 9 || tag == 18) {
      if (!ReadExact(fd, &word, 4)) return false;
      data_and_status->insert(data_and_status->end(), word.begin(), word.end());
      continue;
    }
    if (tag == 2 || tag == 5 || tag == 19) {
      if (!ReadExact(fd, &word, 4)) return false;
      data_and_status->insert(data_and_status->end(), word.begin(), word.end());
      const auto string_len = ReadXdrU32(word, 0);
      const auto string_padding = (4u - (string_len & 3u)) & 3u;
      std::vector<std::uint8_t> string_payload;
      if (!ReadExact(fd, &string_payload,
                     static_cast<std::size_t>(string_len + string_padding))) {
        return false;
      }
      data_and_status->insert(data_and_status->end(),
                              string_payload.begin(), string_payload.end());
      continue;
    }
    return false;
  }
  data_text->assign(reinterpret_cast<const char*>(data_and_status->data()), data_len);
  return true;
}

std::vector<std::uint8_t> ConnectPacket(bool include_protocol12 = true) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 1);       // op_connect
  AppendXdrU32(&out, 0);       // unused operation
  AppendXdrU32(&out, 3);       // CONNECT_VERSION3
  AppendXdrU32(&out, 1);       // arch_generic
  AppendXdrString(&out, "employee");
  AppendXdrU32(&out, include_protocol12 ? 2 : 1);
  AppendXdrString(&out, "");
  AppendXdrU32(&out, 10);
  AppendXdrU32(&out, 1);
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, 5);
  AppendXdrU32(&out, 20);
  if (include_protocol12) {
    AppendXdrU32(&out, 0x800c);
    AppendXdrU32(&out, 1);
    AppendXdrU32(&out, 0);
    AppendXdrU32(&out, 5);
    AppendXdrU32(&out, 24);
  }
  return out;
}

std::vector<std::uint8_t> MalformedConnectCountPacket() {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 1);
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, 3);
  AppendXdrU32(&out, 1);
  AppendXdrString(&out, "employee");
  AppendXdrU32(&out, 65);
  return out;
}

std::vector<std::uint8_t> AttachPacket(std::uint32_t opcode) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, opcode);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, "employee");
  AppendXdrString(&out, std::string_view("\x01\x1c\x07SBPROBE", 10));
  return out;
}

std::vector<std::uint8_t> AttachPacketWithBuffer(std::uint32_t opcode,
                                                 std::string_view buffer) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, opcode);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, "employee");
  AppendXdrString(&out, buffer);
  return out;
}

std::vector<std::uint8_t> AttachPacketNamed(std::uint32_t opcode,
                                            std::string_view database_name,
                                            std::string_view buffer) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, opcode);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, database_name);
  AppendXdrString(&out, buffer);
  return out;
}

std::vector<std::uint8_t> ExecImmediatePacket(std::uint32_t transaction_id,
                                              std::string_view sql) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 64);
  AppendXdrU32(&out, transaction_id);
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, 3);
  AppendXdrString(&out, sql);
  AppendXdrString(&out, "");
  AppendXdrU32(&out, 512);
  return out;
}

std::vector<std::uint8_t> ServiceStartPacket(std::uint32_t object_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 85);  // op_service_start
  AppendXdrU32(&out, object_id);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, std::string_view("\x01", 1));
  return out;
}

std::vector<std::uint8_t> ServiceInfoPacket(std::uint32_t object_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 84);  // op_service_info
  AppendXdrU32(&out, object_id);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, std::string_view("\x35", 1));
  AppendXdrString(&out, std::string_view("\x35", 1));
  AppendXdrU32(&out, 512);
  return out;
}

std::vector<std::uint8_t> PingPacket() {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 93);
  return out;
}

std::vector<std::uint8_t> ReleasePacket(std::uint32_t opcode, std::uint32_t object_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, opcode);
  AppendXdrU32(&out, object_id);
  return out;
}

std::vector<std::uint8_t> AllocateStatementPacket(std::uint32_t database_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 62);
  AppendXdrU32(&out, database_id);
  return out;
}

std::vector<std::uint8_t> PrepareStatementPacket(std::uint32_t statement_id,
                                                 std::string_view sql) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 68);
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, statement_id);
  AppendXdrU32(&out, 3);
  AppendXdrString(&out, sql);
  AppendXdrString(&out, "");
  AppendXdrU32(&out, 512);
  return out;
}

std::vector<std::uint8_t> InfoSqlPacket(std::uint32_t statement_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 70);
  AppendXdrU32(&out, statement_id);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, std::string_view("\x15", 1));
  AppendXdrU32(&out, 512);
  return out;
}

std::vector<std::uint8_t> ExecutePacket(std::uint32_t statement_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 63);
  AppendXdrU32(&out, statement_id);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, "");
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, 0);
  return out;
}

std::vector<std::uint8_t> FetchPacket(std::uint32_t statement_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 65);
  AppendXdrU32(&out, statement_id);
  AppendXdrString(&out, "");
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, 0);
  return out;
}

std::vector<std::uint8_t> FreeStatementPacket(std::uint32_t statement_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 67);
  AppendXdrU32(&out, statement_id);
  AppendXdrU32(&out, 2);
  return out;
}

std::vector<std::uint8_t> TransactionPacket(std::uint32_t database_id,
                                            std::string_view tpb = {}) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 29);
  AppendXdrU32(&out, database_id);
  AppendXdrString(&out, tpb);
  return out;
}

std::vector<std::uint8_t> MalformedTransactionPacket(std::uint32_t database_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 29);
  AppendXdrU32(&out, database_id);
  AppendXdrU32(&out, 64u * 1024u + 1u);
  return out;
}

std::vector<std::uint8_t> BlobPacket(std::uint32_t opcode,
                                     std::uint32_t transaction_id,
                                     std::uint32_t blob_id_low = 0) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, opcode);
  AppendXdrU32(&out, transaction_id);
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, blob_id_low);
  return out;
}

std::vector<std::uint8_t> SegmentPacket(std::uint32_t opcode,
                                        std::uint32_t blob_id,
                                        std::string_view segment = {}) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, opcode);
  AppendXdrU32(&out, blob_id);
  AppendXdrU32(&out, static_cast<std::uint32_t>(segment.size()));
  AppendXdrString(&out, segment);
  return out;
}

std::vector<std::uint8_t> SegmentReadPacket(std::uint32_t blob_id,
                                            std::uint32_t requested_length) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 36);
  AppendXdrU32(&out, blob_id);
  AppendXdrU32(&out, requested_length);
  AppendXdrString(&out, "");
  return out;
}

std::vector<std::uint8_t> EventPacket(std::uint32_t database_id,
                                      std::uint32_t client_event_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 48);
  AppendXdrU32(&out, database_id);
  AppendXdrString(&out, "event_a");
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, 0);
  AppendXdrU32(&out, client_event_id);
  return out;
}

std::vector<std::uint8_t> CancelEventPacket(std::uint32_t database_id,
                                            std::uint32_t client_event_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 49);
  AppendXdrU32(&out, database_id);
  AppendXdrU32(&out, client_event_id);
  return out;
}

bool ReadAndExpectResponse(int fd,
                           std::string_view expected_response_data,
                           std::uint32_t expected_status_code,
                           bool expect_nonzero_object,
                           std::uint32_t* object_id,
                           const char* step_name,
                           std::string* response_data_text = nullptr) {
  std::vector<std::uint8_t> header;
  std::vector<std::uint8_t> tail;
  std::string data_text;
  if (!Expect(ReadFirebirdResponse(fd, &header, &tail, &data_text), step_name)) return false;
  const auto response_object_id = ReadXdrU32(header, 4);
  if (object_id != nullptr) *object_id = response_object_id;
  if (response_data_text != nullptr) *response_data_text = data_text;
  bool ok = Expect(ReadXdrU32(header, 0) == 9, "Firebird packet was not op_response") &&
            Expect(!expect_nonzero_object || response_object_id != 0,
                   "Firebird response did not allocate an object handle") &&
            ExpectResponseToken(data_text, expected_response_data);
  if (!ok) return false;
  if (expected_status_code == 0) {
    return Expect(tail.size() >= 12, "Firebird success vector was too short") &&
           Expect(ReadXdrU32(tail, tail.size() - 12) == 1,
                  "Firebird success vector missing isc_arg_gds") &&
           Expect(ReadXdrU32(tail, tail.size() - 8) == 0,
                  "Firebird success vector code was not FB_SUCCESS") &&
           Expect(ReadXdrU32(tail, tail.size() - 4) == 0,
                  "Firebird success vector missing isc_arg_end");
  }
  return Expect(tail.size() >= 12, "Firebird error vector was too short") &&
         Expect(ReadXdrU32(tail, tail.size() - 12) == 1,
                "Firebird status vector missing isc_arg_gds") &&
         Expect(ReadXdrU32(tail, tail.size() - 8) == expected_status_code,
                ("Firebird status vector code mismatch actual=" +
                 std::to_string(ReadXdrU32(tail, tail.size() - 8)) +
                 " expected=" + std::to_string(expected_status_code)).c_str()) &&
         Expect(ReadXdrU32(tail, tail.size() - 4) == 0,
                "Firebird status vector missing isc_arg_end");
}

bool ReadAndExpectResponseContainingStatus(int fd,
                                           std::string_view expected_response_data,
                                           std::uint32_t expected_status_code,
                                           const char* step_name) {
  std::vector<std::uint8_t> header;
  std::vector<std::uint8_t> tail;
  std::string data_text;
  if (!Expect(ReadFirebirdResponse(fd, &header, &tail, &data_text), step_name)) {
    return false;
  }
  bool ok = Expect(ReadXdrU32(header, 0) == 9, "Firebird packet was not op_response") &&
            ExpectResponseToken(data_text, expected_response_data) &&
            Expect(tail.size() >= 12, "Firebird status vector was too short");
  if (!ok) return false;
  bool found = false;
  for (std::size_t offset = 0; offset + 8 <= tail.size(); offset += 4) {
    if (ReadXdrU32(tail, offset) == 1 &&
        ReadXdrU32(tail, offset + 4) == expected_status_code) {
      found = true;
      break;
    }
  }
  return Expect(found,
                ("Firebird status vector did not contain expected code " +
                 std::to_string(expected_status_code)).c_str());
}

bool ReadAndExpectFetchEof(int fd) {
  std::vector<std::uint8_t> response;
  return Expect(ReadExact(fd, &response, 12), "fetch EOF response read failed") &&
         Expect(ReadXdrU32(response, 0) == 66,
                "fetch response was not op_fetch_response") &&
         Expect(ReadXdrU32(response, 4) == 100,
                "fetch response did not return EOF status") &&
         Expect(ReadXdrU32(response, 8) == 0,
                "fetch response unexpectedly carried messages");
}

bool ReadAndExpectFetchEndOfBatch(int fd) {
  std::vector<std::uint8_t> response;
  return Expect(ReadExact(fd, &response, 12), "fetch batch response read failed") &&
         Expect(ReadXdrU32(response, 0) == 66,
                "fetch batch response was not op_fetch_response") &&
         Expect(ReadXdrU32(response, 4) == 0,
                "fetch batch response did not use in-batch status") &&
         Expect(ReadXdrU32(response, 8) == 0,
                "fetch batch response unexpectedly carried messages");
}

bool ReadAndExpectFetchSingleIntegerThenEof(int fd, std::uint32_t expected_value) {
  std::vector<std::uint8_t> response;
  return Expect(ReadExact(fd, &response, 20), "fetch row response read failed") &&
         Expect(ReadXdrU32(response, 0) == 66,
                "fetch row response was not op_fetch_response") &&
         Expect(ReadXdrU32(response, 4) == 0,
                "fetch row response did not indicate row availability") &&
         Expect(ReadXdrU32(response, 8) == 1,
                "fetch row response did not carry one message") &&
         Expect(ReadXdrU32(response, 12) == expected_value,
                "fetch row integer value mismatch") &&
         Expect(ReadXdrU32(response, 16) == 0,
                "fetch row null indicator mismatch") &&
         ReadAndExpectFetchEof(fd);
}

bool ReadAndExpectFetchTwoIntegers(int fd,
                                   std::int32_t first,
                                   std::int32_t second) {
  std::vector<std::uint8_t> response;
  return Expect(ReadExact(fd, &response, 28), "fetch two-int row response read failed") &&
         Expect(ReadXdrU32(response, 0) == 66,
                "fetch two-int response was not op_fetch_response") &&
         Expect(ReadXdrU32(response, 4) == 0,
                "fetch two-int response did not indicate row availability") &&
         Expect(ReadXdrU32(response, 8) == 1,
                "fetch two-int response did not carry one message") &&
         Expect(ReadXdrU32(response, 12) == static_cast<std::uint32_t>(first),
                "fetch first integer value mismatch") &&
         Expect(ReadXdrU32(response, 16) == 0,
                "fetch first integer null indicator mismatch") &&
         Expect(ReadXdrU32(response, 20) == static_cast<std::uint32_t>(second),
                "fetch second integer value mismatch") &&
         Expect(ReadXdrU32(response, 24) == 0,
                "fetch second integer null indicator mismatch");
}

std::string HexEncodeTest(std::string_view data) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const unsigned char ch : data) {
    out.push_back(kHex[(ch >> 4) & 0x0f]);
    out.push_back(kHex[ch & 0x0f]);
  }
  return out;
}

std::string ReadFileText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool FileExists(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return in.good();
}

bool WriteScopedUserOverlay(std::string_view database_name,
                            std::string_view user_name) {
  const std::string path =
      std::string(database_name) + ".scratchbird-firebird-metadata.tsv";
  const std::string scope = "firebird:" + std::string(database_name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "scratchbird_firebird_metadata_overlay_v6\n";
  out << "security_scope\t" << HexEncodeTest(scope) << '\n';
  out << "firebird_user_v2\t" << HexEncodeTest(scope)
      << '\t' << HexEncodeTest(user_name)
      << "\t\t"
      << '\t' << HexEncodeTest("Srp")
      << '\t' << HexEncodeTest("offline_probe_user")
      << "\t1\n";
  return true;
}

std::string DpbUserNameBuffer(std::string_view user_name) {
  std::string out;
  out.push_back('\x01');
  out.push_back('\x1c');
  out.push_back(static_cast<char>(user_name.size()));
  out.append(user_name);
  return out;
}

std::string DpbCreateCharsetBuffer(std::string_view user_name,
                                   std::string_view attachment_charset,
                                   std::string_view database_default_charset) {
  std::string out = DpbUserNameBuffer(user_name);
  auto append_text = [&](unsigned char tag, std::string_view value) {
    out.push_back(static_cast<char>(tag));
    out.push_back(static_cast<char>(value.size()));
    out.append(value);
  };
  append_text(48, attachment_charset);        // isc_dpb_lc_ctype
  append_text(68, database_default_charset);  // isc_dpb_set_db_charset
  return out;
}

bool WriteScopedCreateTableGrantOverlay(std::string_view database_name,
                                        std::string_view user_name) {
  const std::string path =
      std::string(database_name) + ".scratchbird-firebird-metadata.tsv";
  const std::string scope = "firebird:" + std::string(database_name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "scratchbird_firebird_metadata_overlay_v6\n";
  out << "security_scope\t" << HexEncodeTest(scope) << '\n';
  out << "firebird_user_v2\t" << HexEncodeTest(scope)
      << '\t' << HexEncodeTest(user_name)
      << "\t\t"
      << '\t' << HexEncodeTest("Srp")
      << '\t' << HexEncodeTest("offline_probe_user")
      << "\t1\n";
  out << "grant_v2\t" << HexEncodeTest(scope)
      << '\t' << HexEncodeTest("GRANT CREATE TABLE TO " + std::string(user_name))
      << "\t\t\n";
  return true;
}

bool WriteMixedScopeCatalogOverlay(std::string_view database_name) {
  const std::string path =
      std::string(database_name) + ".scratchbird-firebird-metadata.tsv";
  const std::string scope_a = "firebird:" + std::string(database_name);
  const std::string scope_b = scope_a + ":other";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  auto scope_row = [&](std::string_view kind,
                       std::string_view name,
                       std::string_view scope) {
    out << "catalog_scope\t" << HexEncodeTest(kind)
        << '\t' << HexEncodeTest(name)
        << '\t' << HexEncodeTest(scope)
        << '\n';
  };
  auto table_row = [&](std::string_view name,
                       std::string_view scope) {
    scope_row("TABLE", name, scope);
    out << "table\t" << HexEncodeTest(name) << '\n';
    out << "column\t" << HexEncodeTest(name)
        << '\t' << HexEncodeTest("ID")
        << '\t' << HexEncodeTest(name)
        << "\t496\t4\t0\t0\t0\t\t\t\t0\t0\n";
  };
  auto sequence_row = [&](std::string_view name,
                          std::string_view scope) {
    scope_row("SEQUENCE", name, scope);
    out << "sequence\t" << HexEncodeTest(name) << '\n';
  };
  out << "scratchbird_firebird_metadata_overlay_v6\n";
  out << "security_scope\t" << HexEncodeTest(scope_a) << '\n';
  table_row("A_ONLY", scope_a);
  table_row("B_ONLY", scope_b);
  sequence_row("A_SEQ", scope_a);
  sequence_row("B_SEQ", scope_b);
  out << "role_v2\t" << HexEncodeTest(scope_a)
      << '\t' << HexEncodeTest("ROLE_A") << "\t\t\t1\n";
  out << "role_v2\t" << HexEncodeTest(scope_b)
      << '\t' << HexEncodeTest("ROLE_B") << "\t\t\t1\n";
  out << "grant_v2\t" << HexEncodeTest(scope_a)
      << '\t' << HexEncodeTest("GRANT SELECT ON A_ONLY TO PUBLIC")
      << "\t\t\n";
  out << "grant_v2\t" << HexEncodeTest(scope_b)
      << '\t' << HexEncodeTest("GRANT SELECT ON B_ONLY TO PUBLIC")
      << "\t\t\n";
  out << "firebird_user_v2\t" << HexEncodeTest(scope_a)
      << '\t' << HexEncodeTest("SBPROBE")
      << "\t\t"
      << '\t' << HexEncodeTest("Srp")
      << '\t' << HexEncodeTest("offline_probe_user")
      << "\t1\n";
  out << "firebird_user_v2\t" << HexEncodeTest(scope_b)
      << '\t' << HexEncodeTest("DB_B_USER")
      << "\t\t"
      << '\t' << HexEncodeTest("Srp")
      << '\t' << HexEncodeTest("other_scope_user")
      << "\t1\n";
  return true;
}

struct FileCleanupGuard {
  std::vector<std::string> paths;
  ~FileCleanupGuard() {
    for (const auto& path : paths) {
      std::remove(path.c_str());
    }
  }
};

bool RunScopedCreateTableGrantCase() {
  const std::string unique =
      "sb_firebird_create_table_grant_" +
      std::to_string(static_cast<long long>(::getpid()));
  const std::string overlay = unique + ".scratchbird-firebird-metadata.tsv";
  std::remove(overlay.c_str());
  FileCleanupGuard cleanup{{overlay}};
  const std::string scope = "firebird:" + unique;
  bool ok = Expect(WriteScopedCreateTableGrantOverlay(unique, "SBPROBE"),
                   "scoped create-table grant overlay setup failed");

  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "create-table grant socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "create-table grant fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  ok = ok && Expect(WriteAll(sockets[0], ConnectPacket()),
                    "create-table grant connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "create-table grant accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3,
              "create-table grant connect did not accept");
  std::uint32_t database_id = 0;
  const std::string dpb = DpbUserNameBuffer("SBPROBE");
  ok = ok && Expect(WriteAll(sockets[0],
                             AttachPacketNamed(19, unique, dpb)),
                    "create-table grant attach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true, &database_id,
                             "create-table grant attach response failed");
  ok = ok && Expect(WriteAll(
                        sockets[0],
                        ExecImmediatePacket(
                            0, "CREATE TABLE grant_created_table (id int)")),
                    "create-table grant DDL write failed") &&
       ReadAndExpectResponse(sockets[0],
                             "FIREBIRD.TRANSACTION.HANDLE_REQUIRED",
                             335544332u, false, nullptr,
                             "route-less create-table DDL did not fail closed");
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(21, database_id)),
                    "create-table grant detach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false, nullptr,
                             "create-table grant detach response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid,
              "create-table grant waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird scoped create-table grant failed") && ok;

  const std::string text = ReadFileText(overlay);
  ok = ok && Expect(text.find("security_scope\t" + HexEncodeTest(scope)) !=
                        std::string::npos,
                    "create-table grant overlay missing scope");
  ok = ok && Expect(text.find(HexEncodeTest(
                        "GRANT DELETE ON TABLE GRANT_CREATED_TABLE TO USER SBPROBE")) ==
                        std::string::npos,
                    "route-less create-table added creator DELETE privilege");
  ok = ok && Expect(text.find(HexEncodeTest(
                        "GRANT SELECT ON TABLE GRANT_CREATED_TABLE TO USER SBPROBE")) ==
                        std::string::npos,
                    "route-less create-table added creator SELECT privilege");
  return ok;
}

bool RunWorkerCase(const std::vector<std::uint8_t>& connect_packet,
                   const std::vector<std::uint8_t>& attach_packet,
                   bool expect_accept,
                   bool expect_response,
                   std::string_view expected_response_data,
                   std::uint32_t expected_status_code,
                   bool expect_nonzero_object,
                   const char* case_name) {
  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }
  bool ok = true;
  if (!Expect(WriteAll(sockets[0], connect_packet), "failed to write Firebird connect")) {
    ok = false;
  }
  std::vector<std::uint8_t> response;
  if (ok && expect_accept) {
    ok = Expect(ReadExact(sockets[0], &response, 16), "failed to read Firebird op_accept") &&
         Expect(ReadXdrU32(response, 0) == 3, "Firebird connect did not return op_accept") &&
         Expect(ReadXdrU32(response, 4) == 0x800c, "Firebird accept protocol mismatch") &&
         Expect(ReadXdrU32(response, 8) == 1, "Firebird accept architecture mismatch") &&
         Expect(ReadXdrU32(response, 12) == 5, "Firebird accept type mismatch");
    if (ok && !attach_packet.empty()) {
      ok = Expect(WriteAll(sockets[0], attach_packet), "failed to write Firebird attach");
    }
  } else if (ok) {
    ok = Expect(ReadExact(sockets[0], &response, 4), "failed to read Firebird op_reject") &&
         Expect(ReadXdrU32(response, 0) == 4, "Firebird connect did not return op_reject");
  }
  if (ok && expect_response) {
    ok = ReadAndExpectResponse(sockets[0], expected_response_data,
                               expected_status_code, expect_nonzero_object,
                               nullptr, "failed to read Firebird op_response");
  }
  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid, "waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, case_name) && ok;
  return ok;
}

bool RunPublicCreateDatabaseRefusalCase() {
  const std::string database_name =
      "sb_firebird_public_create_refusal_" +
      std::to_string(static_cast<long long>(::getpid()));
  const std::string overlay =
      database_name + ".scratchbird-firebird-metadata.tsv";
  std::remove(database_name.c_str());
  std::remove(overlay.c_str());
  FileCleanupGuard cleanup{{database_name, overlay}};
  bool ok = true;

  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "public create refusal socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc =
        scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "public create refusal fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  ok = ok && Expect(WriteAll(sockets[0], ConnectPacket()),
                    "public create refusal connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "public create refusal accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3,
              "public create refusal connect did not accept");

  const std::string create_dpb =
      DpbCreateCharsetBuffer("SBPROBE", "WIN1251", "UTF8");
  ok = ok && Expect(
                 WriteAll(sockets[0],
                          AttachPacketNamed(20, database_name, create_dpb)),
                 "public create refusal op_create write failed") &&
       ReadAndExpectResponse(
           sockets[0], "SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED",
           335544378u, false, nullptr,
           "public create refusal op_create response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid,
              "public create refusal waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird public create refusal worker failed") && ok;
  ok = ok && Expect(!FileExists(database_name),
                    "Firebird public op_create created a database file") &&
       Expect(!FileExists(overlay),
                    "Firebird public op_create created a metadata overlay");
  return ok;
}

bool RunServiceLifecycleCase() {
  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "service socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "service fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  bool ok = Expect(WriteAll(sockets[0], ConnectPacket()), "service connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "service accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3, "service connect did not accept");

  std::uint32_t service_id = 0;
  ok = ok && Expect(WriteAll(sockets[0], AttachPacket(82)),
                    "service attach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true,
                             &service_id, "service attach response failed");
  ok = ok && Expect(WriteAll(sockets[0], ServiceStartPacket(service_id)),
                    "service start write failed") &&
       ReadAndExpectResponse(sockets[0], "isc_action_svc_backup", 0, false,
                             nullptr, "service start response failed");
  ok = ok && Expect(WriteAll(sockets[0], ServiceInfoPacket(service_id)),
                    "service info write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false,
                             nullptr, "service info response failed");
  ok = ok && Expect(WriteAll(sockets[0], PingPacket()), "ping write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false, nullptr,
                             "ping response failed");
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(83, service_id)),
                    "service detach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false, nullptr,
                             "service detach response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid, "service waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird service lifecycle failed") && ok;
  return ok;
}

bool RunStatementLifecycleCase() {
  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "statement socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "statement fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  bool ok = Expect(WriteAll(sockets[0], ConnectPacket()), "statement connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "statement accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3, "statement connect did not accept");

  std::uint32_t database_id = 0;
  std::uint32_t statement_id = 0;
  ok = ok && Expect(WriteAll(sockets[0], AttachPacket(19)),
                    "statement database attach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true,
                             &database_id, "statement attach response failed");
  ok = ok && Expect(WriteAll(sockets[0], AllocateStatementPacket(database_id)),
                    "allocate statement write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true,
                             &statement_id, "allocate statement response failed");
  ok = ok && Expect(WriteAll(sockets[0], PrepareStatementPacket(
                                statement_id,
                                "select * from (select rdb$relation_id from rdb$database) "
                                "where sum(rdb$relation_id) = 0")),
                    "prepare invalid aggregate statement write failed") &&
       ReadAndExpectResponse(sockets[0], "FIREBIRD.DSQL.AGGREGATE_WHERE",
                             335544822u, false, nullptr,
                             "prepare invalid aggregate response failed");
  ok = ok && Expect(WriteAll(sockets[0], PrepareStatementPacket(
                                statement_id,
                                "select 1 from rdb$database")),
                    "prepare statement write failed") &&
       ReadAndExpectResponse(
           sockets[0], "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
           335544378u, false, nullptr,
           "route-less executable prepare did not fail closed");
  ok = ok && Expect(WriteAll(sockets[0], FreeStatementPacket(statement_id)),
                    "free statement write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false, nullptr,
                             "free statement response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid, "statement waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird statement lifecycle failed") && ok;
  return ok;
}

bool RunRouteLessSqlRefusalCase() {
  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "route-less SQL socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "route-less SQL fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  bool ok = Expect(WriteAll(sockets[0], ConnectPacket()),
                   "route-less SQL connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "route-less SQL accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3,
              "route-less SQL connect did not accept");

  std::uint32_t database_id = 0;
  ok = ok && Expect(WriteAll(sockets[0], AttachPacket(19)),
                    "route-less SQL attach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true,
                             &database_id,
                             "route-less SQL attach response failed");
  ok = ok && Expect(WriteAll(
                        sockets[0],
                        ExecImmediatePacket(0, "CREATE DOMAIN dm_test AS INTEGER")),
                    "route-less immediate SQL write failed") &&
       ReadAndExpectResponse(sockets[0], "FIREBIRD.TRANSACTION.HANDLE_REQUIRED",
                             335544332u, false, nullptr,
                             "route-less immediate SQL did not fail closed");

  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(21, database_id)),
                    "route-less SQL detach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false, nullptr,
                             "route-less SQL detach response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid,
              "route-less SQL waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird route-less SQL refusal failed") && ok;
  return ok;
}

bool RunScopedSecurityOverlayCase() {
  const std::string unique =
      "sb_firebird_security_scope_" + std::to_string(static_cast<long long>(::getpid()));
  const std::string db_a = unique + "_a";
  const std::string db_b = unique + "_b";
  const std::string overlay_a = db_a + ".scratchbird-firebird-metadata.tsv";
  const std::string overlay_b = db_b + ".scratchbird-firebird-metadata.tsv";
  std::remove(overlay_a.c_str());
  std::remove(overlay_b.c_str());
  FileCleanupGuard cleanup{{overlay_a, overlay_b}};
  bool ok = Expect(WriteScopedUserOverlay(db_a, "SBPROBE"),
                   "scoped security overlay A setup failed") &&
            Expect(WriteScopedUserOverlay(db_b, "SBPROBE"),
                   "scoped security overlay B setup failed");

  auto run_one = [&](const std::string& database_name,
                     const char* label) -> bool {
    int sockets[2] = {-1, -1};
    if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                "scoped security socketpair failed")) {
      return false;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
      CloseFd(&sockets[0]);
      const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
      CloseFd(&sockets[1]);
      _exit(rc == 0 ? 0 : 1);
    }
    CloseFd(&sockets[1]);
    if (!Expect(pid > 0, "scoped security fork failed")) {
      CloseFd(&sockets[0]);
      return false;
    }

    bool ok = Expect(WriteAll(sockets[0], ConnectPacket()),
                     "scoped security connect write failed");
    std::vector<std::uint8_t> response;
    ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                      "scoped security accept read failed") &&
         Expect(ReadXdrU32(response, 0) == 3,
                "scoped security connect did not accept");
    std::uint32_t database_id = 0;
    ok = ok && Expect(WriteAll(
                          sockets[0],
                          AttachPacketNamed(19, database_name,
                                            std::string_view("\x01\x1c\x07SBPROBE",
                                                             10))),
                      "scoped security attach write failed") &&
         ReadAndExpectResponse(sockets[0], "", 0, true,
                               &database_id, "scoped security attach response failed");
    ok = ok && Expect(WriteAll(sockets[0],
                               ExecImmediatePacket(
                                   0, "GRANT SELECT ON scoped_table TO PUBLIC")),
                      "scoped security grant write failed") &&
         ReadAndExpectResponse(sockets[0],
                               "FIREBIRD.TRANSACTION.HANDLE_REQUIRED",
                               335544332u, false, nullptr,
                               "route-less scoped grant did not fail closed");
    ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(21, database_id)),
                      "scoped security detach write failed") &&
         ReadAndExpectResponse(sockets[0], "", 0, false, nullptr,
                               "scoped security detach response failed");

    CloseFd(&sockets[0]);
    int status = 0;
    ok = Expect(::waitpid(pid, &status, 0) == pid,
                "scoped security waitpid failed") && ok;
    ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, label) && ok;
    return ok;
  };

  ok = ok && run_one(db_a, "Firebird scoped security database A failed") &&
       run_one(db_b, "Firebird scoped security database B failed");
  const std::string text_a = ReadFileText(overlay_a);
  const std::string text_b = ReadFileText(overlay_b);
  const std::string scope_a = "firebird:" + db_a;
  const std::string scope_b = "firebird:" + db_b;
  ok = ok && Expect(text_a.find("scratchbird_firebird_metadata_overlay_v6") !=
                        std::string::npos,
                    "route-less grant rewrote scoped security overlay A");
  ok = ok && Expect(text_b.find("scratchbird_firebird_metadata_overlay_v6") !=
                        std::string::npos,
                    "route-less grant rewrote scoped security overlay B");
  ok = ok && Expect(text_a.find("security_scope\t" + HexEncodeTest(scope_a)) !=
                        std::string::npos,
                    "scoped security overlay A missing scope");
  ok = ok && Expect(text_b.find("security_scope\t" + HexEncodeTest(scope_b)) !=
                        std::string::npos,
                    "scoped security overlay B missing scope");
  ok = ok && Expect(text_a.find("grant_v2\t") == std::string::npos,
                    "route-less grant mutated scoped security overlay A");
  ok = ok && Expect(text_b.find("grant_v2\t") == std::string::npos,
                    "route-less grant mutated scoped security overlay B");
  ok = ok && Expect(text_a.find(HexEncodeTest(scope_b)) == std::string::npos,
                    "scoped security overlay A leaked database B scope");
  ok = ok && Expect(text_b.find(HexEncodeTest(scope_a)) == std::string::npos,
                    "scoped security overlay B leaked database A scope");

  std::remove(overlay_a.c_str());
  std::remove(overlay_b.c_str());
  return ok;
}

bool RunCatalogCountQuery(int fd,
                          std::uint32_t database_id,
                          std::string_view sql,
                          std::uint32_t expected_value,
                          const char* label) {
  (void)expected_value;
  std::uint32_t statement_id = 0;
  bool ok = Expect(WriteAll(fd, AllocateStatementPacket(database_id)),
                   "scoped catalog allocate statement write failed") &&
            ReadAndExpectResponse(fd, "", 0, true, &statement_id,
                                  "scoped catalog allocate statement response failed");
  ok = ok && Expect(WriteAll(fd, PrepareStatementPacket(statement_id, sql)),
                    "scoped catalog prepare write failed") &&
       ReadAndExpectResponse(
           fd, "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED", 335544378u,
           false, nullptr,
           "route-less catalog prepare did not fail closed");
  ok = ok && Expect(WriteAll(fd, FreeStatementPacket(statement_id)),
                    "scoped catalog free statement write failed") &&
       ReadAndExpectResponse(fd, "", 0, false, nullptr,
                             "scoped catalog free statement response failed");
  return Expect(ok, label);
}

bool RunScopedCatalogOverlayCase() {
  const std::string unique =
      "sb_firebird_catalog_scope_" + std::to_string(static_cast<long long>(::getpid()));
  const std::string overlay = unique + ".scratchbird-firebird-metadata.tsv";
  std::remove(overlay.c_str());
  FileCleanupGuard cleanup{{overlay}};
  bool ok = Expect(WriteMixedScopeCatalogOverlay(unique),
                   "scoped catalog overlay setup failed");

  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "scoped catalog socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "scoped catalog fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  ok = ok && Expect(WriteAll(sockets[0], ConnectPacket()),
                    "scoped catalog connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "scoped catalog accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3,
              "scoped catalog connect did not accept");
  std::uint32_t database_id = 0;
  ok = ok && Expect(WriteAll(
                        sockets[0],
                        AttachPacketNamed(19, unique,
                                          std::string_view("\x01\x1c\x07SBPROBE",
                                                           10))),
                    "scoped catalog attach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true,
                             &database_id, "scoped catalog attach response failed");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$relations "
                 "where rdb$relation_name = 'A_ONLY'",
                 1, "Firebird catalog table scope allowed object failed");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$relations "
                 "where rdb$relation_name = 'B_ONLY'",
                 0, "Firebird catalog table scope leaked sibling object");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$relation_fields "
                 "where rdb$relation_name = 'A_ONLY'",
                 1, "Firebird catalog column scope allowed object failed");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$relation_fields "
                 "where rdb$relation_name = 'B_ONLY'",
                 0, "Firebird catalog column scope leaked sibling object");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$generators "
                 "where rdb$generator_name = 'A_SEQ'",
                 1, "Firebird catalog sequence scope allowed object failed");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$generators "
                 "where rdb$generator_name = 'B_SEQ'",
                 0, "Firebird catalog sequence scope leaked sibling object");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$roles "
                 "where rdb$role_name = 'ROLE_A'",
                 1, "Firebird catalog role scope allowed object failed");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$roles "
                 "where rdb$role_name = 'ROLE_B'",
                 0, "Firebird catalog role scope leaked sibling object");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from sec$users "
                 "where sec$user_name = 'SBPROBE'",
                 1, "Firebird catalog user scope allowed object failed");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from sec$users "
                 "where sec$user_name = 'DB_B_USER'",
                 0, "Firebird catalog user scope leaked sibling object");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$user_privileges "
                 "where rdb$relation_name = 'A_ONLY'",
                 1, "Firebird catalog grant scope allowed object failed");
  ok = ok && RunCatalogCountQuery(
                 sockets[0], database_id,
                 "select count(*) from rdb$user_privileges "
                 "where rdb$relation_name = 'B_ONLY'",
                 0, "Firebird catalog grant scope leaked sibling object");
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(21, database_id)),
                    "scoped catalog detach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false, nullptr,
                             "scoped catalog detach response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid,
              "scoped catalog waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird scoped catalog overlay failed") && ok;
  std::remove(overlay.c_str());
  return ok;
}

bool RunRuntimeLifecycleCase() {
  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "runtime socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "runtime fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  bool ok = Expect(WriteAll(sockets[0], ConnectPacket()), "runtime connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "runtime accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3, "runtime connect did not accept");

  std::uint32_t database_id = 0;
  std::uint32_t event_id = 0;
  ok = ok && Expect(WriteAll(sockets[0], AttachPacket(19)),
                    "runtime database attach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true,
                             &database_id, "runtime attach response failed");
  ok = ok && Expect(WriteAll(sockets[0], TransactionPacket(database_id)),
                    "transaction start write failed") &&
       ReadAndExpectResponse(
           sockets[0], "FIREBIRD.TRANSACTION.ENGINE_ROUTE_REQUIRED",
           335544378u, false, nullptr,
           "route-less transaction did not fail closed");

  std::string event_queue_text;
  ok = ok && Expect(WriteAll(sockets[0], EventPacket(database_id, 77)),
                    "event queue write failed") &&
       ReadAndExpectResponse(sockets[0], "op_que_events", 0, true,
                             &event_id, "event queue response failed",
                             &event_queue_text);
  ok = ok && Expect(event_id != 0, "event handle was not allocated");
  ok = ok && ExpectResponseToken(
                  event_queue_text,
                  "\"event_handle\":" + std::to_string(event_id));
  ok = ok && ExpectResponseToken(event_queue_text, "\"client_event_id\":77");
  ok = ok && ExpectResponseToken(event_queue_text, "\"event_items_length\":7");
  ok = ok && ExpectResponseToken(event_queue_text,
                                 "\"real_firebird_file_effects\":false");
  ok = ok && ExpectResponseToken(event_queue_text,
                                 "\"reference_engine_sql_executed\":false");
  ok = ok && ExpectResponseToken(event_queue_text,
                                 "\"parser_storage_authority\":false");
  ok = ok && ExpectResponseToken(
                  event_queue_text,
                  "\"parser_transaction_finality_authority\":false");
  ok = ok && ExpectResponseToken(
                  event_queue_text,
                  "\"runtime_policy\":\"emulated_event_registration\"");
  std::string event_cancel_text;
  ok = ok && Expect(WriteAll(sockets[0], CancelEventPacket(database_id, 77)),
                    "event cancel write failed") &&
       ReadAndExpectResponse(sockets[0], "op_cancel_events", 0, false,
                             nullptr, "event cancel response failed",
                             &event_cancel_text);
  ok = ok && ExpectResponseToken(
                  event_cancel_text,
                  "\"object_handle\":" + std::to_string(event_id));
  ok = ok && ExpectResponseToken(event_cancel_text,
                                 "\"real_firebird_file_effects\":false");
  ok = ok && ExpectResponseToken(event_cancel_text,
                                 "\"reference_engine_sql_executed\":false");
  ok = ok && ExpectResponseToken(event_cancel_text,
                                 "\"parser_storage_authority\":false");
  ok = ok && ExpectResponseToken(
                  event_cancel_text,
                  "\"parser_transaction_finality_authority\":false");
  ok = ok && ExpectResponseToken(
                  event_cancel_text,
                  "\"runtime_policy\":\"emulated_event_cancelled\"");
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(21, database_id)),
                    "detach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, false,
                             nullptr, "detach response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid, "runtime waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird runtime lifecycle failed") && ok;
  return ok;
}

bool RunRuntimeFailureCase() {
  int sockets[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
              "failure socketpair failed")) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(sockets[1]);
    CloseFd(&sockets[1]);
    _exit(rc == 0 ? 0 : 1);
  }
  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "failure fork failed")) {
    CloseFd(&sockets[0]);
    return false;
  }

  bool ok = Expect(WriteAll(sockets[0], ConnectPacket()), "failure connect write failed");
  std::vector<std::uint8_t> response;
  ok = ok && Expect(ReadExact(sockets[0], &response, 16),
                    "failure accept read failed") &&
       Expect(ReadXdrU32(response, 0) == 3, "failure connect did not accept");

  std::uint32_t database_id = 0;
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(30, 777)),
                    "invalid commit write failed") &&
       ReadAndExpectResponse(sockets[0], "isc_bad_trans_handle", 335544332u, false,
                             nullptr, "invalid commit response failed");
  ok = ok && Expect(WriteAll(sockets[0], TransactionPacket(777)),
                    "invalid transaction write failed") &&
       ReadAndExpectResponse(sockets[0], "isc_bad_db_handle", 335544324u, false,
                             nullptr, "invalid transaction response failed");
  ok = ok && Expect(WriteAll(sockets[0], SegmentPacket(37, 777, "bad")),
                    "invalid put segment write failed") &&
       ReadAndExpectResponse(sockets[0], "isc_bad_segstr_handle", 335544328u, false,
                             nullptr, "invalid put segment response failed");
  ok = ok && Expect(WriteAll(sockets[0], AttachPacket(19)),
                    "failure attach write failed") &&
       ReadAndExpectResponse(sockets[0], "", 0, true,
                             &database_id, "failure attach response failed");
  ok = ok && Expect(WriteAll(sockets[0], CancelEventPacket(database_id, 404)),
                    "invalid cancel events write failed") &&
       ReadAndExpectResponse(sockets[0], "isc_bad_events_handle", 335545021u, false,
                             nullptr, "invalid cancel events response failed");
  ok = ok && Expect(WriteAll(sockets[0], MalformedTransactionPacket(database_id)),
                    "malformed transaction write failed") &&
       ReadAndExpectResponse(sockets[0], "fail_closed_malformed_packet", 335544378u,
                             false, nullptr, "malformed transaction response failed");

  CloseFd(&sockets[0]);
  int status = 0;
  ok = Expect(::waitpid(pid, &status, 0) == pid, "failure waitpid failed") && ok;
  ok = Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird runtime failure paths failed") && ok;
  return ok;
}
#endif

} // namespace

int main() {
#ifdef _WIN32
  return EXIT_SUCCESS;
#else
  const std::string employee_overlay = "employee.scratchbird-firebird-metadata.tsv";
  std::remove(employee_overlay.c_str());
  FileCleanupGuard cleanup{{employee_overlay}};
  if (!Expect(WriteScopedUserOverlay("employee", "SBPROBE"),
              "employee scoped security overlay setup failed")) {
    return EXIT_FAILURE;
  }
  if (!RunConversionDiagnosticPresentationCase()) {
    return EXIT_FAILURE;
  }
  if (!RunWorkerCase(ConnectPacket(), AttachPacket(19), true, true,
                     "", 0, true,
                     "Firebird attach handshake failed")) {
    return EXIT_FAILURE;
  }
  if (!RunWorkerCase(ConnectPacket(), AttachPacket(82), true, true,
                     "", 0, true,
                     "Firebird service attach handshake failed")) {
    return EXIT_FAILURE;
  }
  if (!RunWorkerCase(ConnectPacket(),
                     AttachPacketWithBuffer(19, std::string_view("\x09", 1)),
                     true, true, "FIREBIRD.WIRE.VERSION_INVALID",
                     335544378u, false,
                     "Firebird malformed attach buffer failed closed")) {
    return EXIT_FAILURE;
  }
  if (!RunWorkerCase(ConnectPacket(false), {}, false, false,
                     "", 0, false,
                     "Firebird missing protocol12 reject failed")) {
    return EXIT_FAILURE;
  }
  if (!RunWorkerCase(MalformedConnectCountPacket(), {}, false, false,
                     "", 0, false,
                     "Firebird malformed connect reject failed")) {
    return EXIT_FAILURE;
  }
  if (!RunPublicCreateDatabaseRefusalCase()) {
    return EXIT_FAILURE;
  }
  if (!RunServiceLifecycleCase()) {
    return EXIT_FAILURE;
  }
  if (!RunStatementLifecycleCase()) {
    return EXIT_FAILURE;
  }
  if (!RunRouteLessSqlRefusalCase()) {
    return EXIT_FAILURE;
  }
  if (!RunScopedSecurityOverlayCase()) {
    return EXIT_FAILURE;
  }
  if (!RunScopedCreateTableGrantCase()) {
    return EXIT_FAILURE;
  }
  if (!RunScopedCatalogOverlayCase()) {
    return EXIT_FAILURE;
  }
  if (!RunRuntimeLifecycleCase()) {
    return EXIT_FAILURE;
  }
  if (!RunRuntimeFailureCase()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
#endif
}
