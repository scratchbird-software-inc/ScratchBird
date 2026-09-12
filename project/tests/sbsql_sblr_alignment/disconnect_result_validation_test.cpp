// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "wire/parser_server_ipc/disconnect_result_validation.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
namespace ipc = scratchbird::parser::ipc;
using Bytes = std::vector<std::uint8_t>;
using D = ipc::DisconnectResultDisposition;
unsigned checks = 0;
void Require(bool value, const char* label) {
  ++checks;
  if (!value) { std::cerr << label << '\n'; std::exit(EXIT_FAILURE); }
}
// Independent legacy framing fixture. Not canonical1074 or engine evidence.
void Text(Bytes& bytes, std::string_view value) {
  bytes.push_back(value.size() & 255);
  bytes.push_back(value.size() >> 8);
  bytes.insert(bytes.end(), value.begin(), value.end());
}
const ipc::DisconnectIdentity connection{
    1, 0xa0, 0, 0, 0, 0, 0x70, 1, 0x80, 0, 0, 0, 0, 0, 0, 1};
const ipc::DisconnectIdentity session{
    1, 0xa0, 0, 0, 0, 0, 0x70, 1, 0x80, 0, 0, 0, 0, 0, 0, 2};
Bytes Payload(std::string_view outcome, std::string_view detail = "cleanup evidence") {
  Bytes bytes;
  Text(bytes, outcome);
  bytes.insert(bytes.end(), session.begin(), session.end());
  Text(bytes, detail);
  return bytes;
}
D Decode(std::span<const std::uint8_t> payload,
         std::uint16_t type = 74, std::uint32_t schema = 3005,
         std::uint32_t flags = 5,
         ipc::DisconnectIdentity response_connection = connection,
         ipc::DisconnectIdentity response_session = session,
         ipc::DisconnectIdentity expected_connection = connection,
         ipc::DisconnectIdentity expected_session = session) {
  return ipc::ValidatePrivateDisconnectResult(
      type, schema, flags, response_connection, response_session,
      expected_connection, expected_session, payload);
}
}

int main() {
  const auto valid = Payload("detached");
  Require(Decode(valid) == D::detached, "exact detached result");
  Require(Decode(Payload("detached", "")) == D::detached, "empty non-authoritative detail");
  Require(Decode(Payload("recovery_quarantined")) == D::recovery_quarantined,
          "quarantine must not be successful cleanup");
  Require(Decode(Payload("session_not_found")) == D::session_not_found,
          "missing session must not be successful cleanup");
  Require(Decode(Payload("binding_mismatch")) == D::binding_mismatch,
          "binding refusal must not be successful cleanup");
  for (const auto outcome : {"", "accepted", "DETACHED", "detached ", "unknown"}) {
    Require(Decode(Payload(outcome)) == D::invalid, "unknown outcome");
  }
  for (std::size_t n = 0; n < valid.size(); ++n) {
    Require(Decode(std::span(valid).first(n)) == D::invalid, "truncated reply");
  }
  auto bytes = valid; bytes.push_back(0);
  Require(Decode(bytes) == D::invalid, "trailing reply byte");
  for (const auto offset : {std::size_t(0), std::size_t(26)}) {
    bytes = valid; bytes[offset] = 255; bytes[offset + 1] = 255;
    Require(Decode(bytes) == D::invalid, "oversized string length");
  }
  bytes = valid; bytes[2] = 0;
  Require(Decode(bytes) == D::invalid, "NUL outcome");
  bytes = valid; bytes.back() = 0;
  Require(Decode(bytes) == D::invalid, "NUL detail");
  for (unsigned i = 0; i != 16; ++i) {
    bytes = valid; bytes[10 + i] ^= 1;
    Require(Decode(bytes) == D::invalid, "payload identity substitution");
    auto wrong_connection = connection; wrong_connection[i] ^= 1;
    auto wrong_session = session; wrong_session[i] ^= 1;
    Require(Decode(valid, 74, 3005, 5, wrong_connection) == D::invalid,
            "header connection substitution");
    Require(Decode(valid, 74, 3005, 5, connection, wrong_session) == D::invalid,
            "header session substitution");
  }
  for (unsigned type = 0; type != 256; ++type) {
    Require((Decode(valid, type) == D::detached) == (type == 74), "exact message type");
  }
  for (const auto schema : {0u, 1074u, 2001u, 3004u, 3006u, 0xffffffffu}) {
    Require(Decode(valid, 74, schema) == D::invalid, "exact private schema");
  }
  for (unsigned flags = 0; flags != 256; ++flags) {
    Require((Decode(valid, 74, 3005, flags) == D::detached) == (flags == 5),
            "exact terminal response flags");
  }
  Require(Decode(valid, 74, 3005, 5, {}, session, {}, session) == D::invalid,
          "zero connection authority");
  Require(Decode(valid, 74, 3005, 5, connection, {}, connection, {}) == D::invalid,
          "zero session authority");
  std::cout << "disconnect_result_validation checks=" << checks
            << " PASS canonical1074=false engine_execution=false\n";
}
