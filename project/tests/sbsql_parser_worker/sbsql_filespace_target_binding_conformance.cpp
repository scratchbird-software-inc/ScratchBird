// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "sblr_engine_envelope.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace p = scratchbird::parser::sbsql;
using Uuid = scratchbird::core::platform::Uuid;
namespace {
void Check(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}
p::SessionContext Session() {
  p::SessionContext s;
  s.authenticated = true;
  s.session_uuid = scratchbird::tests::FixtureUuid(2077, 1);
  s.connection_uuid = scratchbird::tests::FixtureUuid(2077, 2);
  s.database_uuid = scratchbird::tests::FixtureUuid(2077, 3);
  s.dialect_profile_uuid = scratchbird::tests::FixtureUuid(1027, 1);
  s.catalog_epoch = 1; s.security_policy_epoch = 1; s.descriptor_epoch = 1;
  return s;
}
p::SblrEnvelope Lower(const p::CstDocument& cst, const p::AstDocument& ast,
                       const std::vector<Uuid>& identities) {
  p::ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "component-resolver";
  config.parser_uuid = scratchbird::tests::FixtureUuid(2077, 4);
  auto bound = p::BindAst(ast, cst, config, Session(), identities);
  if (!identities.empty()) {
    Check(bound.bound, "resolved filespace binding refused");
    Check(bound.resolved_object_uuids == identities, "binder discarded filespace identities");
  } else Check(!bound.bound, "unresolved filespace was bound without the server resolver");
  return p::LowerToSblr(bound, cst, Session());
}
void FilespaceTargets() {
  const auto target = scratchbird::tests::FixtureUuid(2077, 5);
  const auto other = scratchbird::tests::FixtureUuid(2077, 6);
  for (const auto* sql : {"DISCONNECT FILESPACE fs_runtime;",
                          "disconnect filespace fs_runtime;",
                          "DISCONNECT /* storage, not session */ FILESPACE fs_runtime;"}) {
    const auto cst = p::BuildCst(sql);
    const auto ast = p::BuildAst(cst);
    Check(!cst.messages.has_errors() && !ast.messages.has_errors(), "filespace syntax rejected");
    Check(ast.family == p::StatementFamily::kStorageManagement &&
              ast.requires_name_resolution && ast.produces_sblr &&
              ast.operation_family == "sblr.filespace.management.v3",
          "DISCONNECT FILESPACE was captured by generic session management");
    const auto envelope = Lower(cst, ast, {target});
    Check(p::VerifySblrEnvelope(envelope).admitted, "filespace component verifier refused");
    Check(envelope.operation_id == "filespace.disconnect" &&
              envelope.sblr_opcode == "SBLR_FILESPACE_DISCONNECT" &&
              !envelope.parser_executes_sql && !envelope.real_file_effects,
          "filespace route/effect contract changed");
    unsigned identities = 0;
    for (const auto& operand : envelope.operands) {
      if (operand.type != "uuid") continue;
      ++identities;
      Check(operand.value.empty() && operand.canonical_value_kind ==
                static_cast<std::uint16_t>(scratchbird::engine::sblr::SblrValueKind::uuid_ref) &&
                operand.canonical_value_body.size() == 16 &&
                std::equal(target.bytes.begin(), target.bytes.end(), operand.canonical_value_body.begin()),
            "filespace target is not exact binary16");
    }
    Check(identities == 1 && envelope.resolved_object_uuids == std::vector<Uuid>{target},
          "filespace identity multiplicity changed");
    for (const auto& invalid : std::vector<std::vector<Uuid>>{
             {}, {Uuid{}}, {target, other}, {target, target}}) {
      const auto refused = Lower(cst, ast, invalid);
      Check(!p::VerifySblrEnvelope(refused).admitted,
            "missing/nil/ambiguous filespace target was admitted");
    }
  }
  // Session disconnect remains a different command: never resolve it as a
  // storage object just because it shares its first keyword.
  const auto session = p::BuildAst(p::BuildCst("DISCONNECT;"));
  Check(session.family != p::StatementFamily::kStorageManagement &&
            !session.requires_name_resolution, "session disconnect became a filespace operation");
}
}
int main() {
  try {
    FilespaceTargets();
    std::cout << "filespace target binding component=passed; physical execution not asserted\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
