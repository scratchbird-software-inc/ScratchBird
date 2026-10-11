// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual lowering/submission codecs; fixture identities grant no live authority.
#include "wire/sbsql_test_wire.hpp"
#include "lowering/lowering.hpp"
#include "cst/cst.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace scratchbird::parser::sbsql {
std::optional<ipc::ParserCanonicalSblrSubmission>
BuildCanonicalScalarProjectionSubmissionForTest(const SblrEnvelope&,
    const ipc::ParserStatementContext&, const SessionContext&);
}
namespace p = scratchbird::parser::sbsql;
namespace ipc = scratchbird::parser::ipc;
namespace sb = scratchbird::engine::sblr;
using Uuid = scratchbird::core::platform::Uuid;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* detail) {
  ++checks;
  if (!ok) throw std::runtime_error(detail);
}
constexpr Uuid Id(unsigned n) {
  Uuid id{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,0}};
  id.bytes[15] = static_cast<std::uint8_t>(n);
  return id;
}
}
int main() try {
  p::ParserConfig config;
  config.parser_uuid = Id(1);
  config.bundle_contract_id = "sbp_sbsql@native-scalar-submission";
  config.build_id = "native-scalar-submission";
  p::SessionContext session;
  session.authenticated = true;
  session.session_uuid = Id(2); session.connection_uuid = Id(3);
  session.database_uuid = Id(4); session.authenticated_user_uuid = Id(5);
  session.dialect_profile_uuid = Id(6); session.admitted_dialect_profile_uuid = Id(6);
  session.admitted_parser_package_uuid = config.parser_uuid;
  session.catalog_epoch = 1; session.security_policy_epoch = 1; session.descriptor_epoch = 1;
  ipc::ParserStatementContext context;
  context.acquired = true;
  context.statement_uuid = Id(7); context.bound_ast_uuid = Id(8);
  context.catalog_epoch_uuid = Id(9); context.security_context_uuid = Id(10);
  context.transaction = {73, Id(11)};
  context.statement_snapshot_uuid = Id(12); context.statement_metadata_snapshot_uuid = Id(13);
  context.preliminary_statement_catalog_generation = 1;
  context.preliminary_security_epoch = 1; context.preliminary_resource_epoch = 1;
  ipc::ParserStatementContext::DescriptorProfile text;
  text.profile_kind = 3; text.slot = 0; text.type_uuid = Id(14);
  text.descriptor_uuid = Id(15); text.nullable = false;
  context.descriptor_profiles.push_back(text);
  const auto cst = p::BuildCst("SELECT 1e2 AS amount;");
  auto bound = p::BindAst(p::BuildAst(cst), cst, config, session, {});
  const auto lowered = p::LowerToSblr(bound, cst, session);
  Check(!lowered.messages.has_errors() && lowered.operation_id == "query.evaluate_projection",
        "actual scalar producer failed");
  const auto literal = std::ranges::find_if(lowered.operands, [](const auto& o) {
    return o.name == "projection_0_value";
  });
  Check(literal != lowered.operands.end() && literal->type == "real64" &&
      literal->value.empty() && literal->canonical_value_body.size() == 32 &&
      literal->canonical_value_kind == static_cast<unsigned>(sb::SblrValueKind::literal_typed),
      "lowering did not emit native REAL64");
  // Independent IEEE754 little-endian representation of 100, not a roundtrip
  // through the production numeric encoder.
  const std::array<std::uint8_t,8> hundred{0,0,0,0,0,0,0x59,0x40};
  Check(std::equal(hundred.begin(), hundred.end(), literal->canonical_value_body.begin() + 24),
        "REAL64 lowering bytes changed");
  const auto submit = [&](const p::SblrEnvelope& e) {
    return p::BuildCanonicalScalarProjectionSubmissionForTest(e, context, session);
  };
  const auto verify = [&](const p::SblrEnvelope& e) {
    const auto submission = submit(e);
    Check(submission && submission->complete(), "native scalar submission refused");
    const auto& bytes = submission->canonical_operation_bytes;
    const auto decoded = sb::DecodeSblrOpcodeStream(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    Check(decoded.ok && decoded.stream.operations.size() == 3 &&
        decoded.stream.operations[1].opcode_code == 0x040e,
        "native scalar package/opcode changed");
    for (const auto& expected : e.operands) {
      if (expected.canonical_value_body.empty()) continue;
      const auto& operands = decoded.stream.operations[1].operands;
      const auto actual = std::ranges::find(operands, expected.name, &sb::SblrOperand::name);
      Check(actual != operands.end() && actual->type == expected.type && actual->value.empty() &&
          actual->value_body == expected.canonical_value_body &&
          static_cast<unsigned>(actual->value_kind) == expected.canonical_value_kind,
          "native carrier was changed by submission");
    }
  };
  verify(lowered);
  const auto position = static_cast<std::size_t>(literal - lowered.operands.begin());
  // Exercise nested occurrence paths produced by the actual binder/lowerer.
  // This is compiled builtin metadata, not a live execution authorization.
  const std::vector<scratchbird::wire::BuiltinFunctionIdentity> functions{{
      "sb.scalar.abs", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x76,0x71,
                            0x9f,0x1b,0x53,0x50,0xd1,0x34,0xab,0x0f}}}};
  const auto nested_cst = p::BuildCst("SELECT ABS(ABS(1e2)) AS amount;");
  const auto nested_bound = p::BindAst(p::BuildAst(nested_cst), nested_cst,
      config, session, {}, nullptr, functions);
  const auto nested = p::LowerToSblr(nested_bound, nested_cst, session);
  Check(!nested.messages.has_errors(), "nested actual producer failed");
  const auto nested_literal = std::ranges::find(nested.operands,
      "projection_0_arg_0_arg_0_value", &p::SblrOperand::name);
  Check(nested_literal != nested.operands.end() &&
      nested_literal->type == "real64" &&
      nested_literal->canonical_value_body == literal->canonical_value_body,
      "nested REAL64 occurrence changed");
  verify(nested);
  // Existing UUID and BINARY carriers remain byte-preserving, including UUID
  // data that is not a system identity and empty non-NULL binary values.
  for (const auto& payload : std::vector<std::vector<std::uint8_t>>{
      std::vector<std::uint8_t>(16, 0),
      {1,2,3,4,5,6,0x40,8,0x80,10,11,12,13,14,15,16},
      {}, {0,0xff,0,0x80,0x41}}) {
    auto e = lowered;
    auto& value = e.operands[position];
    value.type = payload.size() == 16 ? "uuid" : "binary";
    value.canonical_value_body.resize(24);
    std::fill(value.canonical_value_body.begin() + 16,
              value.canonical_value_body.end(), 0);
    value.canonical_value_body[16] = static_cast<std::uint8_t>(payload.size());
    value.canonical_value_body.insert(value.canonical_value_body.end(),
                                      payload.begin(), payload.end());
    verify(e);
    if (value.type == "uuid") {
      value.canonical_value_body.pop_back();
      value.canonical_value_body[16] = 15;
      Check(!submit(e), "short UUID data carrier forwarded");
    }
  }
  for (const auto bits : {0ull, 0x8000000000000000ull, 1ull,
                         0x3ff0000000000000ull, 0x7fefffffffffffffull}) {
    auto e = lowered;
    for (unsigned i = 0; i < 8; ++i)
      e.operands[position].canonical_value_body[24 + i] = static_cast<std::uint8_t>(bits >> (8*i));
    verify(e); // Framing never rounds, normalizes signed zero, or flushes subnormals.
  }
  for (unsigned mutation = 0; mutation != 12; ++mutation) {
    auto e = lowered;
    auto& value = e.operands[position];
    switch (mutation) {
      case 0: value.value = "100"; break;
      case 1: value.type = "text"; break;
      case 2: value.name = "projection_0_function_uuid"; break;
      case 3: value.name = "projection_00_value"; break;
      case 4: value.name = "projection_0_arg__value"; break;
      case 5: value.canonical_value_kind = static_cast<unsigned>(sb::SblrValueKind::uuid_ref); break;
      case 6: value.canonical_value_body[16] = 7; break;
      case 7: value.canonical_value_body.push_back(0); break;
      case 8: value.canonical_value_body.pop_back(); value.canonical_value_body[16] = 7; break;
      case 9: std::fill_n(value.canonical_value_body.begin(), 16, 0); break;
      case 10: value.canonical_value_body[6] = 0x40; break;
      case 11: value.canonical_value_body[8] = 0; break;
    }
    Check(!submit(e), "malformed native scalar was forwarded");
    verify(lowered);
  }
  for (std::size_t size = 0; size < 32; ++size) {
    auto e = lowered;
    e.operands[position].canonical_value_body.resize(size);
    Check(!submit(e), "truncated REAL64 forwarded");
  }
  std::cout << "native scalar submission checks=" << checks << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
