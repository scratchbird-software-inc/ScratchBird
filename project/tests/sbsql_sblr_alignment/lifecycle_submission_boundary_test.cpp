// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, std::string_view why) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
std::string Read(const std::string& path) {
  std::ifstream input(path);
  Check(input.good(), "source guard input readable");
  return {std::istreambuf_iterator<char>(input), {}};
}
scratchbird::core::platform::Uuid Id(unsigned suffix) {
  scratchbird::core::platform::Uuid id;
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  id.bytes[15] = static_cast<std::uint8_t>(suffix);
  return id;
}
}

int main(int argc, char** argv) {
  using namespace scratchbird::parser::sbsql;
  // This guard examines the production caller, which is intentionally not
  // replaced with a mock dispatcher. The linked pipeline checks below are
  // component evidence, not proof of a live server or lifecycle effect.
  const std::string root = argc == 2 ? argv[1] : SB_LIFECYCLE_SOURCE_ROOT;
  const auto wire = Read(root + "/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp");
  const auto header = Read(root + "/src/parsers/sbsql_worker/wire/sbsql_test_wire.hpp");
  for (const auto forbidden : {"RunServerManagementCommand", "ParseServerManagementCommand",
       "ServerManagementCommand", "->Manage(", "acknowledgements_satisfied:true",
       "drain_complete:true", "recovery_evidence_preserved:true",
       "force_termination_policy_uuid:019e0ec6-d13c-7000-8000-000000000013",
       ";accepted=true;payload_bytes="}) {
    Check(wire.find(forbidden) == std::string::npos, forbidden);
    Check(header.find(forbidden) == std::string::npos, forbidden);
  }
  // Explicit expected operations/opcodes, independent of the mapping table.
  struct Case { const char* sql; const char* operation; const char* opcode; };
  constexpr std::array cases{
      Case{"VERIFY DATABASE", "lifecycle.verify_database", "SBLR_LIFECYCLE_VERIFY_DATABASE"},
      Case{"INSPECT DATABASE", "lifecycle.inspect_database", "SBLR_LIFECYCLE_INSPECT_DATABASE"},
      Case{"DIAGNOSE DATABASE", "lifecycle.inspect_database", "SBLR_LIFECYCLE_INSPECT_DATABASE"},
      Case{"SHUTDOWN DATABASE", "lifecycle.shutdown_database", "SBLR_LIFECYCLE_SHUTDOWN_DATABASE"},
      Case{"SHUTDOWN DATABASE FORCE", "lifecycle.shutdown_force", "SBLR_LIFECYCLE_SHUTDOWN_FORCE"},
      Case{"FORCE SHUTDOWN DATABASE", "lifecycle.shutdown_force", "SBLR_LIFECYCLE_SHUTDOWN_FORCE"},
      Case{"DROP DATABASE", "lifecycle.drop_database", "SBLR_LIFECYCLE_DROP_DATABASE"},
      Case{"DROP DATABASE LOGICAL", "lifecycle.drop_database", "SBLR_LIFECYCLE_DROP_DATABASE"},
      Case{"DROP DATABASE LOGICAL PRESERVE", "lifecycle.drop_database", "SBLR_LIFECYCLE_DROP_DATABASE"}};
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = Id(1); session.connection_uuid = Id(2);
  session.database_uuid = Id(3); session.dialect_profile_uuid = Id(4);
  session.catalog_epoch = 71; session.security_policy_epoch = 72; session.descriptor_epoch = 73;
  ParserConfig config;
  for (const auto& item : cases) {
    for (const auto suffix : {"", ";", " ;  "}) {
      const auto cst = BuildCst(std::string(item.sql) + suffix);
      const auto ast = BuildAst(cst);
      const auto bound = BindAst(ast, cst, config, session, {});
      const auto envelope = LowerToSblr(bound, cst, session);
      Check(envelope.lifecycle_mapping, item.sql);
      Check(envelope.operation_id == item.operation, "lifecycle operation mapping");
      Check(envelope.sblr_opcode == item.opcode, "lifecycle opcode mapping");
      Check(!envelope.parser_executes_sql && !envelope.real_file_effects,
            "lowering cannot establish execution or durable effects");
      Check(envelope.catalog_epoch == 71 && envelope.security_policy_epoch == 72 &&
            envelope.descriptor_epoch == 73, "source epochs retained");
      for (const auto forbidden : {"acknowledgements_satisfied", "drain_complete",
           "recovery_evidence_preserved", "force_termination_policy_uuid"})
        Check(envelope.payload.find(forbidden) == std::string::npos,
              "lowering invents lifecycle evidence");
    }
  }
  std::cout << "lifecycle submission boundary checks=" << checks
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
