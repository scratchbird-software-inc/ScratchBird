// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_worker_session.hpp"
#include "parser_server_client.hpp"
#include "sbps.hpp"
#include "sblr_dispatch_server.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fb = scratchbird::parser::firebird;
namespace ipc = scratchbird::parser::ipc;
using scratchbird::server::ServerDiagnosticField;

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t* offset) {
  Require(offset != nullptr && *offset + 4 <= bytes.size(),
          "truncated Firebird packet");
  const std::uint32_t value =
      (static_cast<std::uint32_t>(bytes[*offset]) << 24u) |
      (static_cast<std::uint32_t>(bytes[*offset + 1]) << 16u) |
      (static_cast<std::uint32_t>(bytes[*offset + 2]) << 8u) |
      static_cast<std::uint32_t>(bytes[*offset + 3]);
  *offset += 4;
  return value;
}

std::string ReadString(const std::vector<std::uint8_t>& bytes,
                       std::size_t* offset) {
  const std::uint32_t length = ReadU32(bytes, offset);
  Require(*offset + length <= bytes.size(), "truncated Firebird string");
  std::string value(reinterpret_cast<const char*>(bytes.data() + *offset),
                    length);
  *offset += length;
  while ((*offset & 3u) != 0) ++*offset;
  Require(*offset <= bytes.size(), "truncated Firebird string padding");
  return value;
}

struct StatusItem {
  std::uint32_t kind = 0;
  std::uint32_t number = 0;
  std::string text;
};

std::vector<StatusItem> DecodeStatus(
    const std::vector<std::uint8_t>& packet,
    std::string* response_json) {
  std::size_t offset = 0;
  Require(ReadU32(packet, &offset) == 9, "not a Firebird op_response");
  (void)ReadU32(packet, &offset);
  (void)ReadU32(packet, &offset);
  (void)ReadU32(packet, &offset);
  const std::string response = ReadString(packet, &offset);
  if (response_json != nullptr) *response_json = response;
  std::vector<StatusItem> status;
  for (;;) {
    StatusItem item;
    item.kind = ReadU32(packet, &offset);
    if (item.kind == 0) {
      status.push_back(item);
      break;
    }
    if (item.kind == 1 || item.kind == 4) {
      item.number = ReadU32(packet, &offset);
    } else if (item.kind == 2 || item.kind == 5 || item.kind == 19) {
      item.text = ReadString(packet, &offset);
    } else {
      Require(false, "unexpected Firebird status argument kind");
    }
    status.push_back(std::move(item));
  }
  Require(offset == packet.size(), "trailing Firebird status bytes");
  return status;
}

std::vector<ServerDiagnosticField> Fields(std::string violation) {
  return {{"violation_kind", std::move(violation)},
          {"constraint_display_label", "INTEG_1"},
          {"owner_relation_display_label", "DETAIL_TABLE"},
          {"owner_schema_display_label", ""},
          {"child_column_display_label", "FKEY"},
          {"key_display_text", "(\"FKEY\" = 1)"}};
}

bool SameFields(const std::vector<ServerDiagnosticField>& left,
                const std::vector<ServerDiagnosticField>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].key != right[index].key ||
        left[index].value != right[index].value) {
      return false;
    }
  }
  return true;
}

ipc::MessageVectorSet Messages(const std::vector<ServerDiagnosticField>& fields,
                               std::string prose) {
  ipc::MessageVectorSet messages;
  std::vector<ipc::Field> public_fields;
  for (const auto& field : fields) {
    public_fields.push_back({field.key, field.value});
  }
  messages.diagnostics.push_back(ipc::MakeDiagnostic(
      "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION", "ERROR", std::move(prose),
      "engine", std::move(public_fields)));
  return messages;
}

ipc::MessageVectorSet RoundTripRegistered(
    const std::vector<ServerDiagnosticField>& candidates) {
  constexpr std::string_view kCode =
      "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION";
  auto fields =
      scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
          kCode, candidates);
  // Production SBPS public-field policy must drop this trailing private field.
  fields.push_back({"internal_path", "/tmp/secret"});
  auto diagnostic = scratchbird::server::sbps::IpcDiagnostic(
      std::string(kCode), std::string(kCode),
      "SQL/prose is not a presentation input", std::move(fields));
  scratchbird::server::sbps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(
      scratchbird::server::sbps::MessageType::kDiagnostic);
  header.flags = scratchbird::server::sbps::kFlagResponse |
                 scratchbird::server::sbps::kFlagError |
                 scratchbird::server::sbps::kFlagFinal;
  header.payload_schema_id =
      scratchbird::server::sbps::kSchemaMessageVectorSetV1;
  header.request_uuid = scratchbird::server::sbps::MakeUuidV7Bytes();
  const auto payload = scratchbird::server::sbps::EncodeMessageVectorSet(
      {diagnostic}, header.request_uuid);
  const auto frame = scratchbird::server::sbps::EncodeFrame(header, payload);
  ipc::MessageVectorSet decoded;
  Require(ipc::DecodeDiagnosticFrameForTest(frame, &decoded),
          "registered FK diagnostic did not cross production SBPS decoder");
  Require(decoded.diagnostics.size() == 1,
          "SBPS FK diagnostic cardinality changed");
  for (const auto& field : decoded.diagnostics.front().fields) {
    Require(field.name != "internal_path",
            "SBPS diagnostic leaked a private trailing field");
  }
  return decoded;
}

}  // namespace

int main() {
  constexpr std::string_view kCode =
      "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION";
  const auto missing = Fields("parent_missing");
  const auto registered =
      scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
          kCode, missing);
  Require(SameFields(registered, missing),
          "exact FK diagnostic registry rejected");

  auto malformed = missing;
  malformed.pop_back();
  Require(scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
              kCode, malformed)
              .empty(),
          "short FK diagnostic shape was admitted");
  malformed = missing;
  std::swap(malformed[0], malformed[1]);
  Require(scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
              kCode, malformed)
              .empty(),
          "reordered FK diagnostic shape was admitted");
  malformed = missing;
  malformed[5].key = "detail";
  Require(scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
              kCode, malformed)
              .empty(),
          "unknown FK diagnostic field was admitted");
  malformed = missing;
  malformed[0].value = "invented";
  Require(scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
              kCode, malformed)
              .empty(),
          "unknown FK violation kind was admitted");
  malformed = missing;
  malformed[1].value = "bad\nlabel";
  Require(scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
              kCode, malformed)
              .empty(),
          "control-bearing FK diagnostic label was admitted");

  const auto missing_packet = fb::RenderFirebirdForeignKeyDiagnosticPacket(
      RoundTripRegistered(missing),
      "op_exec_immediate");
  Require(missing_packet.has_value(),
          "worker rejected exact parent-missing diagnostic");
  std::string missing_json;
  const auto missing_status = DecodeStatus(*missing_packet, &missing_json);
  Require(missing_status.size() == 8,
          "parent-missing Firebird status argument count drifted");
  Require(missing_status[0].kind == 1 &&
              missing_status[0].number == 335544466u &&
              missing_status[1].kind == 2 &&
              missing_status[1].text == "INTEG_1" &&
              missing_status[2].kind == 2 &&
              missing_status[2].text == "DETAIL_TABLE" &&
              missing_status[3].kind == 1 &&
              missing_status[3].number == 335544838u &&
              missing_status[4].kind == 1 &&
              missing_status[4].number == 335545072u &&
              missing_status[5].kind == 2 &&
              missing_status[5].text == "(\"FKEY\" = 1)" &&
              missing_status[6].kind == 19 &&
              missing_status[6].text == "23000" &&
              missing_status[7].kind == 0,
          "parent-missing Firebird status vector is not native-exact");
  Require(missing_json.find("SQL/prose is not a presentation input") ==
                  std::string::npos &&
              missing_json.find("(\\\"FKEY\\\" = 1)") !=
                  std::string::npos,
          "worker inferred FK presentation from prose/SQL");

  const auto references = Fields("references_present");
  const auto references_packet =
      fb::RenderFirebirdForeignKeyDiagnosticPacket(
          RoundTripRegistered(references),
          "op_execute");
  Require(references_packet.has_value(),
          "worker rejected exact references-present diagnostic");
  const auto references_status = DecodeStatus(*references_packet, nullptr);
  Require(references_status.size() == 6 &&
              references_status[0].kind == 1 &&
              references_status[0].number == 335544466u &&
              references_status[1].text == "INTEG_1" &&
              references_status[2].text == "DETAIL_TABLE" &&
              references_status[3].kind == 1 &&
              references_status[3].number == 335544839u &&
              references_status[4].kind == 19 &&
              references_status[4].text == "23000" &&
              references_status[5].kind == 0,
          "references-present Firebird status vector is not native-exact");

  malformed = missing;
  std::swap(malformed[0], malformed[1]);
  Require(!fb::RenderFirebirdForeignKeyDiagnosticPacket(
               RoundTripRegistered(malformed), "op_execute")
               .has_value(),
          "SBPS/worker accepted reordered structured fields");
  Require(!fb::RenderFirebirdForeignKeyDiagnosticPacket(
               Messages(malformed,
                        "SQL/prose is not a presentation input"),
               "op_execute")
               .has_value(),
          "standalone worker accepted reordered structured fields");

  std::cout << "Firebird structured FK diagnostic boundary passed\n";
  return EXIT_SUCCESS;
}
