// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "wire/sbsql_test_wire.hpp"
#include "../support/client_public_result_display.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  namespace parser = scratchbird::parser::sbsql;
  parser::ParserConfig config;
  config.server_endpoint = argv[1]; config.database_token = argv[2];
  parser::SbsqlTestWireSession session(config, nullptr, nullptr);
  parser::AuthCredentialEnvelope credentials;
  credentials.provider_family = "local_password";
  credentials.principal = "alice";
  credentials.requested_database = argv[2];
  credentials.application_name = "builtin-function-binary-process-test";
  credentials.credential_evidence = argv[3]; credentials.credential_evidence_present = true;
  parser::MessageVectorSet messages;
  if (!session.AuthenticateCredentials(credentials, &messages)) {
    std::cerr << parser::MessageVectorToJson(messages) << '\n'; return 3;
  }
  const auto run = [&](std::string_view sql, std::string_view expected,
                       std::size_t expected_uuid_atoms = 0) {
    auto result = session.RunPipeline(sql, true);
    if (!result.accepted) {
      std::cerr << sql << '\n' << parser::MessageVectorToJson(result.messages) << '\n'; return false;
    }
    if (expected.empty()) return true;
    std::string packet = result.server_result_payload;
    if (!result.server_cursor_uuid.is_nil()) {
      const auto fetched = session.FetchCursorOnRoute(result.server_cursor_uuid, 2);
      if (!fetched.accepted || fetched.row_count != 1 || !fetched.end_of_cursor) return false;
      packet = fetched.row_packet;
      if (!session.CloseCursorOnRoute(result.server_cursor_uuid).accepted) return false;
    } else if (result.server_row_count != 1) return false;
    namespace result_packet = scratchbird::wire::public_result;
    const auto row = result_packet::Find(packet, "row[0]");
    std::vector<result_packet::Field> values;
    if (!row || row->kind != result_packet::Kind::row ||
        !result_packet::Decode(row->value, &values)) return false;
    std::size_t uuid_atoms = 0;
    for (const auto& value : values) {
      if (value.kind == result_packet::Kind::uuid) {
        if (value.value.size() != 16) return false;
        ++uuid_atoms;
      }
    }
    if (uuid_atoms != expected_uuid_atoms) return false;
    const auto display = "row[0]=" + scratchbird::tests::DisplayPublicResultPacket(row->value, 1);
    if (display != expected) {
      std::cerr << "unexpected scalar result: " << display << '\n'; return false;
    }
    return true;
  };
  if (!run("BEGIN TRANSACTION", {}) ||
      !run("SELECT ABS(-7) AS a, ABS(ABS(-3)) AS b", "row[0]=a=7;b=3") ||
      !run("SELECT SQRT(81) AS root, LOWER('MiXeD') AS lowered", "row[0]=root=9;lowered=mixed") ||
      !run("SELECT UUID '00000000-0000-0000-0000-000000000000' AS nil_value, "
           "UUID 'ffffffff-ffff-ffff-ffff-ffffffffffff' AS max_value, "
           "UUID '550e8400-e29b-41d4-a716-446655440000' AS v4_value, ABS(-5) AS a",
           "row[0]=nil_value=00000000-0000-0000-0000-000000000000;"
           "max_value=ffffffff-ffff-ffff-ffff-ffffffffffff;"
           "v4_value=550e8400-e29b-41d4-a716-446655440000;a=5", 3) ||
      !run("SELECT UUID_FROM_STRING('550E8400-E29B-41D4-A716-446655440000') AS id, "
           "UUID_TO_STRING(UUID 'ffffffff-ffff-ffff-ffff-ffffffffffff') AS spelling",
           "row[0]=id=550e8400-e29b-41d4-a716-446655440000;"
           "spelling=ffffffff-ffff-ffff-ffff-ffffffffffff", 1) ||
      !run("SELECT ENCODE(X'00ff10', 'hex') AS encoded, DECODE('00ff10', 'hex') AS decoded, "
           "ENCODE(X'', 'hex') AS empty", "row[0]=encoded=00ff10;decoded=hex:00ff10;empty=") ||
      !run("SELECT UUID '550e8400-e29b-41d4-a716-446655440000' AS id, X'00ff10' AS bytes_value",
           "row[0]=id=550e8400-e29b-41d4-a716-446655440000;bytes_value=hex:00ff10", 1) ||
      !run("ROLLBACK TRANSACTION", {})) return 4;
  std::cout << "builtin_function_binary_process=passed phase=" << argv[4] << '\n';
  return 0;
}
