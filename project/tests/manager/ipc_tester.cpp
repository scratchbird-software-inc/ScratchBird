// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_IPC_TESTER

#include "manager_protocol.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace proto = scratchbird::manager::protocol;

namespace {

int Usage() {
  std::cerr << "usage: sbmn_manager_ipc_tester --control PATH --command mcp.hello|mcp.db_list|manager.shutdown|manager.validate_config|manager.reload_config|listener.status|listener.start|listener.stop|listener.restart|listener.drain|listener.undrain|listener.reload|support.bundle_generate|thirdparty.status_export\n";
  return 2;
}

void PutU16(proto::Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(proto::Bytes* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

void PutLpstr(proto::Bytes* out, const std::string& value) {
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

proto::Bytes ManagerCommandPayload(const std::string& operation, const std::string& idempotency_key) {
  proto::Bytes out = {'M', 'C', 'P', '1'};
  PutLpstr(&out, operation);
  PutLpstr(&out, idempotency_key);
  PutU32(&out, 0);
  return out;
}

std::uint16_t ReadU16(const proto::Bytes& data, std::size_t off) {
  return static_cast<std::uint16_t>(data[off]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8u);
}

std::uint32_t ReadU32(const proto::Bytes& data, std::size_t off) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[off + i]) << (8 * i);
  return v;
}

std::string DefaultToken() {
  if (const char* token = std::getenv("SCRATCHBIRD_MCP_AUTH_SECRET")) return token;
  return "test-secret";
}

proto::Bytes BuildPayload(const std::string& command, const std::string& token, std::uint8_t* type_out) {
  proto::Bytes payload;
  if (command == "mcp.hello" || command == "manager.status" || command == "status") {
    *type_out = 0x65;
    PutU16(&payload, 0x0100);
    PutU16(&payload, 0);
  } else if (command == "mcp.auth_start") {
    *type_out = 0x66;
    PutLpstr(&payload, "tester");
    payload.push_back(4);
    const auto auth_token = token.empty() ? DefaultToken() : token;
    PutU32(&payload, static_cast<std::uint32_t>(auth_token.size()));
    payload.insert(payload.end(), auth_token.begin(), auth_token.end());
  } else if (command == "mcp.auth_continue") {
    *type_out = 0x67;
    const auto auth_token = token.empty() ? DefaultToken() : token;
    PutU32(&payload, static_cast<std::uint32_t>(auth_token.size()));
    payload.insert(payload.end(), auth_token.begin(), auth_token.end());
  } else if (command == "mcp.db_list") {
    *type_out = 0x68;
  } else if (command == "mcp.db_info") {
    *type_out = 0x6a;
    PutLpstr(&payload, "");
  } else if (command == "mcp.db_connect") {
    *type_out = 0x69;
    PutLpstr(&payload, "");
  } else if (command == "manager.shutdown") {
    *type_out = 0x60;
  } else if (command.rfind("listener.", 0) == 0 || command == "support.bundle_generate" ||
             command == "thirdparty.status_export" || command == "manager.validate_config" ||
             command == "manager.reload_config") {
    *type_out = 0x6b;
    const std::string key = (command == "listener.status" || command == "listener.list" || command == "manager.validate_config") ? "" : "tester-" + command;
    payload = ManagerCommandPayload(command, key);
  } else {
    *type_out = 0x65;
    PutU16(&payload, 0x0100);
    PutU16(&payload, 0);
  }
  return payload;
}

#ifndef _WIN32
bool RecvExact(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t got = 0;
  while (got < size) {
    const auto rc = ::recv(fd, data + got, size - got, 0);
    if (rc <= 0) return false;
    got += static_cast<std::size_t>(rc);
  }
  return true;
}
#endif

std::string ReadLpstr(const proto::Bytes& data, std::size_t* off) {
  if (*off + 4 > data.size()) return {};
  const auto len = ReadU32(data, *off);
  *off += 4;
  if (*off + len > data.size()) return {};
  std::string out(data.begin() + static_cast<std::ptrdiff_t>(*off),
                  data.begin() + static_cast<std::ptrdiff_t>(*off + len));
  *off += len;
  return out;
}

void PrintResponse(const proto::SbdbFrame& frame) {
  if (frame.type == 0x64 && frame.payload.size() >= 5) {
    std::size_t off = 0;
    const auto request_type = frame.payload[off++];
    const auto count = ReadU32(frame.payload, off);
    off += 4;
    std::cout << "type=STATUS_RESPONSE request_type=" << static_cast<unsigned>(request_type) << "\n";
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto key = ReadLpstr(frame.payload, &off);
      const auto value = ReadLpstr(frame.payload, &off);
      std::cout << key << '=' << value << "\n";
    }
    if (off + 4 <= frame.payload.size()) {
      const auto mvs_len = ReadU32(frame.payload, off);
      std::cout << "message_vector_set_len=" << mvs_len << "\n";
    }
    return;
  }
  if (frame.type == 0xff) {
    std::size_t off = 0;
    std::cout << "type=PROTOCOL_ERROR error=" << ReadLpstr(frame.payload, &off);
    if (off + 4 <= frame.payload.size()) {
      std::cout << " message_vector_set_len=" << ReadU32(frame.payload, off);
    }
    std::cout << "\n";
    return;
  }
  if (frame.type == 0x11 && frame.payload.size() >= 261) {
    std::string error(frame.payload.begin() + 5, frame.payload.begin() + 261);
    error.erase(error.find_last_not_of('\0') + 1);
    std::cout << "type=AUTH_RESPONSE status=" << static_cast<unsigned>(frame.payload[0])
              << " error=" << error;
    if (frame.payload.size() >= 265) {
      std::cout << " message_vector_set_len=" << ReadU32(frame.payload, 261);
    }
    std::cout << "\n";
    return;
  }
  if (frame.type == 0x12 && frame.payload.size() >= 88) {
    std::cout << "type=AUTH_CHALLENGE session_id="
              << proto::Hex(frame.payload.data(), 16) << "\n";
    return;
  }
  if (frame.type == 0x02 && frame.payload.size() >= 117) {
    std::string error;
    if (frame.payload[0] != 0 && frame.payload.size() > 117) {
      std::size_t off = 117;
      error = ReadLpstr(frame.payload, &off);
      std::cout << "type=CONNECT_RESPONSE failure=" << static_cast<unsigned>(frame.payload[0])
                << " error=" << error;
      if (off + 4 <= frame.payload.size()) {
        std::cout << " message_vector_set_len=" << ReadU32(frame.payload, off);
      }
      std::cout << "\n";
      return;
    }
    std::cout << "type=CONNECT_RESPONSE failure=" << static_cast<unsigned>(frame.payload[0])
              << " error=" << error << "\n";
    return;
  }
  std::cout << "type=0x" << std::hex << static_cast<unsigned>(frame.type)
            << " payload_hex=" << scratchbird::manager::protocol::Hex(frame.payload) << "\n";
}

#ifndef _WIN32
bool SendAndReceiveFrame(int fd, const proto::SbdbFrame& frame, proto::SbdbFrame* response_out) {
  const auto request = proto::EncodeSbdbFrame(frame);
  if (::send(fd, request.data(), request.size(), 0) < 0) return false;
  proto::Bytes header(12);
  if (!RecvExact(fd, header.data(), header.size())) return false;
  const auto payload_len = ReadU32(header, 8);
  proto::Bytes encoded = header;
  encoded.resize(12 + payload_len);
  if (payload_len != 0 && !RecvExact(fd, encoded.data() + 12, payload_len)) return false;
  std::vector<proto::Diagnostic> diagnostics;
  const auto response = proto::DecodeSbdbFrame(encoded, &diagnostics);
  if (!response) {
    for (const auto& diagnostic : diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.safe_message << "\n";
    return false;
  }
  *response_out = *response;
  return true;
}

bool RequiresAuth(const std::string& command) {
  return command == "manager.shutdown" ||
         command == "mcp.db_info" ||
         command == "mcp.db_connect" ||
         command == "manager.validate_config" ||
         command == "manager.reload_config" ||
         command.rfind("listener.", 0) == 0 ||
         command == "support.bundle_generate" ||
         command == "thirdparty.status_export";
}

int RunCommand(const std::filesystem::path& control, const std::string& command, const std::string& token) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 2;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto path = control.string();
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return 2;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 2;
  }
  if (RequiresAuth(command)) {
    std::uint8_t auth_type = 0;
    const auto auth_payload = BuildPayload("mcp.auth_start", token, &auth_type);
    proto::SbdbFrame auth_response;
    if (!SendAndReceiveFrame(fd, proto::SbdbFrame{auth_type, 0, auth_payload}, &auth_response)) {
      ::close(fd);
      return 1;
    }
    if (auth_response.type != 0x11 || auth_response.payload.empty() || auth_response.payload[0] != 0) {
      PrintResponse(auth_response);
      ::close(fd);
      return 1;
    }
  }
  std::uint8_t type = 0;
  const auto payload = BuildPayload(command, token, &type);
  proto::SbdbFrame response;
  const bool received = SendAndReceiveFrame(fd, proto::SbdbFrame{type, 0, payload}, &response);
  ::close(fd);
  if (!received) {
    return 1;
  }
  PrintResponse(response);
  return response.type == 0xff ? 1 : 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path control;
  std::string command = "manager.status";
  std::string token;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--control" && i + 1 < argc) control = argv[++i];
    else if (arg == "--command" && i + 1 < argc) command = argv[++i];
    else if (arg == "--token" && i + 1 < argc) token = argv[++i];
    else return Usage();
  }
  if (control.empty()) return Usage();
#ifdef _WIN32
  std::cerr << "sbmn_manager_ipc_tester currently requires POSIX AF_UNIX.\n";
  return 2;
#else
  return RunCommand(control, command, token);
#endif
}
