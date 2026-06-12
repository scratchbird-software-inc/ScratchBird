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
#include <iostream>
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
  if (!ReadExact(fd, data_and_status, static_cast<std::size_t>(data_len + padding + 12))) {
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
  AppendXdrString(&out, std::string_view("\x01\x1c\x06SYSDBA", 9));
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

std::vector<std::uint8_t> ServiceStartPacket(std::uint32_t object_id) {
  std::vector<std::uint8_t> out;
  AppendXdrU32(&out, 85);  // op_service_start
  AppendXdrU32(&out, object_id);
  AppendXdrU32(&out, 0);
  AppendXdrString(&out, std::string_view("\x01\x01", 2));
  AppendXdrU32(&out, 256);
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
  return Expect(ReadXdrU32(header, 0) == 9, "Firebird packet was not op_response") &&
         Expect(!expect_nonzero_object || response_object_id != 0,
                "Firebird response did not allocate an object handle") &&
         ExpectResponseToken(data_text, expected_response_data) &&
         Expect(ReadXdrU32(tail, tail.size() - 12) == 1,
                "Firebird status vector missing isc_arg_gds") &&
         Expect(ReadXdrU32(tail, tail.size() - 8) == expected_status_code,
                "Firebird status vector code mismatch") &&
         Expect(ReadXdrU32(tail, tail.size() - 4) == 0,
                "Firebird status vector missing isc_arg_end");
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
       ReadAndExpectResponse(sockets[0], "op_service_attach", 0, true,
                             &service_id, "service attach response failed");
  ok = ok && Expect(WriteAll(sockets[0], ServiceStartPacket(service_id)),
                    "service start write failed") &&
       ReadAndExpectResponse(sockets[0], "isc_action_svc_backup", 0, false,
                             nullptr, "service start response failed");
  ok = ok && Expect(WriteAll(sockets[0], ServiceInfoPacket(service_id)),
                    "service info write failed") &&
       ReadAndExpectResponse(sockets[0], "emulated_service_report", 0, false,
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
       ReadAndExpectResponse(sockets[0], "op_attach", 0, true,
                             &database_id, "statement attach response failed");
  ok = ok && Expect(WriteAll(sockets[0], AllocateStatementPacket(database_id)),
                    "allocate statement write failed") &&
       ReadAndExpectResponse(sockets[0], "op_allocate_statement", 0, true,
                             &statement_id, "allocate statement response failed");
  ok = ok && Expect(WriteAll(sockets[0], PrepareStatementPacket(statement_id, "select 1")),
                    "prepare statement write failed") &&
       ReadAndExpectResponse(sockets[0], "firebird.query.select", 0, false,
                             nullptr, "prepare statement response failed");
  ok = ok && Expect(WriteAll(sockets[0], InfoSqlPacket(statement_id)),
                    "info sql write failed") &&
       ReadAndExpectResponse(sockets[0], "statement_info_descriptor", 0, false,
                             nullptr, "info sql response failed");
  ok = ok && Expect(WriteAll(sockets[0], ExecutePacket(statement_id)),
                    "execute statement write failed") &&
       ReadAndExpectResponse(sockets[0], "op_execute", 0, false,
                             nullptr, "execute response failed");
  ok = ok && Expect(WriteAll(sockets[0], FetchPacket(statement_id)),
                    "fetch write failed") &&
       ReadAndExpectFetchEof(sockets[0]);
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
  std::uint32_t transaction_id = 0;
  std::uint32_t blob_id = 0;
  std::uint32_t event_id = 0;
  ok = ok && Expect(WriteAll(sockets[0], AttachPacket(19)),
                    "runtime database attach write failed") &&
       ReadAndExpectResponse(sockets[0], "op_attach", 0, true,
                             &database_id, "runtime attach response failed");
  std::string transaction_start_text;
  ok = ok && Expect(WriteAll(sockets[0], TransactionPacket(database_id)),
                    "transaction start write failed") &&
       ReadAndExpectResponse(sockets[0], "op_transaction", 0, true,
                             &transaction_id, "transaction response failed",
                             &transaction_start_text);
  ok = ok && ExpectResponseToken(
                  transaction_start_text,
                  "\"database_handle\":" + std::to_string(database_id));
  ok = ok && ExpectResponseToken(
                  transaction_start_text,
                  "\"transaction_handle\":" + std::to_string(transaction_id));
  ok = ok && ExpectResponseToken(transaction_start_text,
                                 "\"real_firebird_file_effects\":false");
  ok = ok && ExpectResponseToken(transaction_start_text,
                                 "\"reference_engine_sql_executed\":false");
  ok = ok && ExpectResponseToken(transaction_start_text,
                                 "\"parser_storage_authority\":false");
  ok = ok && ExpectResponseToken(
                  transaction_start_text,
                  "\"parser_transaction_finality_authority\":false");
  ok = ok && ExpectResponseToken(
                  transaction_start_text,
                  "\"runtime_policy\":\"emulated_transaction_handle_started\"");
  ok = ok && Expect(WriteAll(sockets[0], BlobPacket(34, transaction_id)),
                    "create blob write failed") &&
       ReadAndExpectResponse(sockets[0], "op_create_blob", 0, true,
                             &blob_id, "create blob response failed");
  ok = ok && Expect(WriteAll(sockets[0], SegmentPacket(37, blob_id, "hello")),
                    "put segment write failed") &&
       ReadAndExpectResponse(sockets[0], "op_put_segment", 0, false,
                             nullptr, "put segment response failed");
  ok = ok && Expect(WriteAll(sockets[0], SegmentPacket(36, blob_id)),
                    "get segment write failed") &&
       ReadAndExpectResponse(sockets[0], "op_get_segment", 335544367u, false,
                             nullptr, "get segment EOF response failed");
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(39, blob_id)),
                    "close blob write failed") &&
       ReadAndExpectResponse(sockets[0], "op_close_blob", 0, false,
                             nullptr, "close blob response failed");
  std::string commit_text;
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(30, transaction_id)),
                    "commit write failed") &&
       ReadAndExpectResponse(sockets[0], "op_commit", 0, false,
                             nullptr, "commit response failed", &commit_text);
  ok = ok && ExpectResponseToken(
                  commit_text,
                  "\"object_handle\":" + std::to_string(transaction_id));
  ok = ok && ExpectResponseToken(commit_text,
                                 "\"reference_engine_sql_executed\":false");
  ok = ok && ExpectResponseToken(commit_text,
                                 "\"parser_storage_authority\":false");
  ok = ok && ExpectResponseToken(
                  commit_text,
                  "\"parser_transaction_finality_authority\":false");
  ok = ok && ExpectResponseToken(commit_text,
                                 "\"runtime_policy\":\"emulated_transaction_closed\"");

  std::string second_transaction_start_text;
  ok = ok && Expect(WriteAll(sockets[0], TransactionPacket(database_id)),
                    "second transaction start write failed") &&
       ReadAndExpectResponse(sockets[0], "op_transaction", 0, true,
                             &transaction_id, "second transaction response failed",
                             &second_transaction_start_text);
  ok = ok && ExpectResponseToken(
                  second_transaction_start_text,
                  "\"transaction_handle\":" + std::to_string(transaction_id));
  ok = ok && ExpectResponseToken(
                  second_transaction_start_text,
                  "\"parser_transaction_finality_authority\":false");
  ok = ok && Expect(WriteAll(sockets[0], BlobPacket(35, transaction_id, 7)),
                    "open blob write failed") &&
       ReadAndExpectResponse(sockets[0], "op_open_blob", 0, true,
                             &blob_id, "open blob response failed");
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(38, blob_id)),
                    "cancel blob write failed") &&
       ReadAndExpectResponse(sockets[0], "op_cancel_blob", 0, false,
                             nullptr, "cancel blob response failed");
  std::string rollback_text;
  ok = ok && Expect(WriteAll(sockets[0], ReleasePacket(31, transaction_id)),
                    "rollback write failed") &&
       ReadAndExpectResponse(sockets[0], "op_rollback", 0, false,
                             nullptr, "rollback response failed", &rollback_text);
  ok = ok && ExpectResponseToken(
                  rollback_text,
                  "\"object_handle\":" + std::to_string(transaction_id));
  ok = ok && ExpectResponseToken(rollback_text,
                                 "\"reference_engine_sql_executed\":false");
  ok = ok && ExpectResponseToken(rollback_text,
                                 "\"parser_storage_authority\":false");
  ok = ok && ExpectResponseToken(
                  rollback_text,
                  "\"parser_transaction_finality_authority\":false");
  ok = ok && ExpectResponseToken(rollback_text,
                                 "\"runtime_policy\":\"emulated_transaction_closed\"");

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
       ReadAndExpectResponse(sockets[0], "op_detach", 0, false,
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
       ReadAndExpectResponse(sockets[0], "op_attach", 0, true,
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
  if (!RunWorkerCase(ConnectPacket(), AttachPacket(19), true, true,
                     "isc_dpb_user_name", 0, true,
                     "Firebird attach handshake failed")) {
    return EXIT_FAILURE;
  }
  if (!RunWorkerCase(ConnectPacket(), AttachPacket(82), true, true,
                     "op_service_attach", 0, true,
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
  if (!RunServiceLifecycleCase()) {
    return EXIT_FAILURE;
  }
  if (!RunStatementLifecycleCase()) {
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
