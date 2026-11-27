// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_PGP_REFUSAL_ROUTE (2026-12-20; owner rulings 2026-11-26: PGP 2 a, PGP 3 a).
//
// The four pgp_* scalar functions are REFUSED by the engine with CRYPTO.PGP.PROFILE_UNAVAILABLE until real OpenPGP is wired; the retired legacy
// SBPGP1 envelope (which carried the key and the plaintext in base64) is removed. This test drives the REAL parser pipeline in process (SQL text ->
// CST -> AST -> bind -> SBLR lowering -> verifier -> server admission) and hands the parser's OWN lowered operands (not a hand-built operand list) to the
// engine's SBLR dispatch (query.evaluate_projection), then runs the engine diagnostic's fields through the server's registered-field filter.
//
// WHAT THIS ROUTE IS: parser pipeline + server admission + engine SBLR dispatch IN PROCESS, with no database file. It is the strongest route that reaches the
// engine FUNCTION today: on a production-created native database a scalar function call stops at the native dispatch gate (SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED,
// opcode_code 1024) before any engine function, which sbsql_sblr_alignment_native_pgp_refusal_* pins. The legacy seeded-database process route
// (sbsql_builtin_function_binary_process_e2e) is unavailable (its seeder fails at baseline: durable bootstrap principal unavailable or mismatched).
// WHAT IT CANNOT SHOW: the SBPS/SBMV wire bytes of this refusal, nor a refusal on a native database (needs native scalar function execution).
// The sentinels are distinctive plaintext/key literals: neither the raw text, its base64 nor its hex may appear in any code, detail or field.

#include "../support/binary_uuid_fixture.hpp"
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
#include "cst/cst.hpp"
#include "engine/functions/registry/function_seed_registry.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "server/sblr_dispatch_server.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace srv = scratchbird::server;

int failures = 0;
int checks = 0;
void Check(bool condition, const std::string& message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

bool Contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

std::string Base64(const std::string& in) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < in.size(); i += 3) {
    const std::uint32_t a = static_cast<unsigned char>(in[i]);
    const std::uint32_t b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
    const std::uint32_t c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
    const std::uint32_t t = (a << 16) | (b << 8) | c;
    out += kAlphabet[(t >> 18) & 63];
    out += kAlphabet[(t >> 12) & 63];
    out += i + 1 < in.size() ? kAlphabet[(t >> 6) & 63] : '=';
    out += i + 2 < in.size() ? kAlphabet[t & 63] : '=';
  }
  return out;
}

std::string Hex(const std::string& in) {
  std::string out;
  for (const unsigned char ch : in) {
    out += "0123456789abcdef"[ch >> 4];
    out += "0123456789abcdef"[ch & 15];
  }
  return out;
}

const std::string kPlain = "SENTINEL-PLAIN-5e1d";
const std::string kKey = "SENTINEL-KEY-9b27";

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057301");
  session.connection_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057302");
  session.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057303");
  session.dialect_profile_uuid = scratchbird::tests::FixtureUuid(1027, 1);
  session.catalog_epoch = 96;
  session.security_policy_epoch = 97;
  session.descriptor_epoch = 98;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_pgp_refusal_route";
  config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057304");
  config.bundle_contract_id = "sbp_sbsql@pgp-refusal-route-test";
  config.build_id = "sbsql-pgp-refusal-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  static const auto builtin_functions = [] {
    std::vector<scratchbird::wire::BuiltinFunctionIdentity> result;
    const auto package = scratchbird::engine::functions::BuildStandardFunctionSeedPackage();
    for (const auto& entry : package.registry.Entries())
      if (entry.catalog_visible) result.push_back({entry.function_id, entry.function_uuid});
    return result;
  }();
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {}, nullptr, builtin_functions);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-pgp-refusal-route";
  context.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057401");
  context.node_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057402");
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057403");
  context.principal_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057404");
  context.transaction_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057405");
  context.statement_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057406");
  context.current_schema_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057407");
  context.current_role_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000057408");
  context.local_transaction_id = 57;
  context.security_context_present = true;
  return context;
}

// The engine envelope is built from the PARSER's lowered operands (the production conversion: type, name, value), never from a hand-written operand list.
sblr::SblrOperationEnvelope EngineEnvelopeFromLowered(const SblrEnvelope& lowered) {
  auto envelope = sblr::MakeSblrEnvelope(lowered.operation_id, lowered.sblr_opcode, "trace.pgp_refusal.route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  for (const auto& operand : lowered.operands) {
    envelope.operands.push_back({operand.type, operand.name, operand.value});
    // an identity operand (the function's binary UUID) travels as its canonical kind and body, exactly as the parser lowered it
    if (operand.canonical_value_kind != 0) {
      envelope.operands.back().value_kind = static_cast<sblr::SblrValueKind>(operand.canonical_value_kind);
      envelope.operands.back().value_body = operand.canonical_value_body;
    }
  }
  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(std::move(envelope));
}

struct Shape {
  std::string name;          // the SQL spelling and the engine operation token
  std::string sql;
  std::string declared;      // the parser's declared result descriptor
  bool envelope_message;     // the message argument is a decrypt input carrying the retired envelope
};

std::vector<Shape> Shapes() {
  const std::string legacy_envelope = "SBPGP1:sym:" + Base64(kKey) + ":" + Base64(kPlain);
  return {
      {"pgp_sym_encrypt", "SELECT pgp_sym_encrypt('" + kPlain + "','" + kKey + "') AS out_value;", "binary", false},
      {"pgp_sym_decrypt", "SELECT pgp_sym_decrypt(X'" + Hex(kPlain) + "','" + kKey + "') AS out_value;", "character", false},
      {"pgp_pub_encrypt", "SELECT pgp_pub_encrypt(X'" + Hex(kPlain) + "',X'" + Hex(kKey) + "') AS out_value;", "binary", false},
      {"pgp_pub_decrypt", "SELECT pgp_pub_decrypt(X'" + Hex(kPlain) + "',X'" + Hex(kKey) + "') AS out_value;", "binary", false},
      // a message that IS the retired SBPGP1 envelope: it must not decrypt to the plaintext, it is refused like any other
      {"pgp_sym_decrypt", "SELECT pgp_sym_decrypt(X'" + Hex(legacy_envelope) + "','" + kKey + "') AS out_value;", "character", true},
  };
}

std::string AllEngineText(const sblr::SblrDispatchResult& result) {
  std::string text;
  for (const auto& d : result.diagnostics) text += d.code + "|" + d.message + "|";
  for (const auto& d : result.api_result.diagnostics) {
    text += d.code + "|" + d.message_key + "|" + d.detail + "|";
    for (const auto& f : d.fields) text += f.key + "=" + f.value + "|";
  }
  return text;
}

void CheckNoMaterial(const std::string& label, const std::string& text) {
  for (const auto& needle : {kPlain, kKey, Base64(kPlain), Base64(kKey), Hex(kPlain), Hex(kKey)})
    Check(!Contains(text, needle), label + ": no plaintext/key material (raw, base64 or hex form `" + needle.substr(0, 12) + "...`) in any code, detail, field or message");
  Check(!Contains(text, "SBPGP1"), label + ": the retired SBPGP1 envelope text is nowhere in the response");
}

void CheckOne(const Shape& shape) {
  const std::string label = "'" + shape.name + (shape.envelope_message ? " (legacy envelope message)" : "") + "'";
  const auto artifacts = RunPipeline(shape.sql);
  Check(!artifacts.cst.messages.has_errors() && !artifacts.ast.messages.has_errors(), label + ": parses");
  Check(artifacts.bound.bound, label + ": the parser binds the registered function");
  Check(artifacts.verifier.admitted, label + ": the SBLR verifier admits the lowered statement");
  Check(artifacts.envelope.operation_id == "query.evaluate_projection" && artifacts.envelope.sblr_opcode == "SBLR_QUERY_EVALUATE_PROJECTION",
        label + ": lowers to query.evaluate_projection: " + artifacts.envelope.operation_id);
  Check(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.crypto." + shape.name + "\""),
        label + ": the lowered function id is sb.crypto." + shape.name);
  Check(Contains(artifacts.envelope.payload, "\"projection_0_type\":\"" + shape.declared + "\""),
        label + ": the parser's declared result type is " + shape.declared);
  Check(!Contains(artifacts.envelope.payload, "SELECT pgp_"), label + ": the lowered payload carries no SQL text");
  Check(!artifacts.envelope.parser_executes_sql, label + ": the parser executes nothing");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(artifacts.envelope));
  Check(admission.admitted && admission.requires_public_abi_dispatch, label + ": server admission admits it and requires public ABI dispatch (the engine is asked)");

  const auto dispatched = sblr::DispatchSblrOperation({EngineContext(), EngineEnvelopeFromLowered(artifacts.envelope), api::EngineApiRequest{}});
  Check(dispatched.envelope_validated && dispatched.accepted && dispatched.dispatched_to_api, label + ": the engine dispatch accepts the envelope and routes to the projection API");
  Check(!dispatched.api_result.ok, label + ": the engine API result is a refusal, not a success");
  Check(dispatched.api_result.result_shape.rows.empty(), label + ": the engine returns NO result row");
  const std::string all = AllEngineText(dispatched);
  Check(dispatched.api_result.diagnostics.size() == 1 && dispatched.api_result.diagnostics.front().code == "CRYPTO.PGP.PROFILE_UNAVAILABLE",
        label + ": the one engine diagnostic is CRYPTO.PGP.PROFILE_UNAVAILABLE: " + all);
  if (!dispatched.api_result.diagnostics.empty()) {
    const auto& diagnostic = dispatched.api_result.diagnostics.front();
    Check(diagnostic.fields.size() == 1 && diagnostic.fields.front().key == "operation" && diagnostic.fields.front().value == shape.name,
          label + ": the engine API diagnostic carries exactly one public field, operation=" + shape.name + " (function_id, function_uuid and package_name stay private): " + all);

    // The server's registered-field filter: ONLY `operation` (one of the four exact tokens) is forwarded to the parser.
    std::vector<srv::ServerDiagnosticField> candidates;
    for (const auto& f : diagnostic.fields) candidates.push_back({f.key, f.value});
    const auto forwarded = srv::RegisteredEngineDiagnosticFieldsForTest(diagnostic.code, candidates);
    Check(forwarded.size() == 1 && forwarded[0].key == "operation" && forwarded[0].value == shape.name,
          label + ": the server forwards exactly one field, operation=" + shape.name + " (only the engine's own public field exists): " + std::to_string(forwarded.size()));
    std::string forwarded_text;
    for (const auto& f : forwarded) forwarded_text += f.key + "=" + f.value + "|";
    CheckNoMaterial(label + " forwarded fields", forwarded_text);
  }
  CheckNoMaterial(label + " engine response", all);
}

void CheckRegistry() {
  // The filter is closed: a candidate `operation` that is not one of the four exact tokens forwards nothing (no argument-derived text can ride this code),
  // another code forwards none of these fields, and a missing key forwards nothing.
  const auto fields = [](std::vector<srv::ServerDiagnosticField> f) { return srv::RegisteredEngineDiagnosticFieldsForTest("CRYPTO.PGP.PROFILE_UNAVAILABLE", f); };
  Check(fields({{"operation", "pgp_sym_encrypt"}}).size() == 1, "registry: a valid operation token is forwarded");
  Check(fields({{"operation", kPlain}}).empty(), "registry: an operation carrying plaintext is dropped");
  Check(fields({{"operation", "PGP_SYM_ENCRYPT"}}).empty(), "registry: an operation token in another case is dropped (exact tokens only)");
  Check(fields({{"operation", "pgp_sym_encrypt "}}).empty(), "registry: an operation token with trailing text is dropped");
  Check(fields({{"function_id", "sb.crypto.pgp_sym_encrypt"}}).empty() && fields({}).empty(), "registry: no `operation` candidate forwards nothing");
  Check(fields({{"function_id", "x"}, {"operation", "pgp_pub_decrypt"}, {"package_name", "y"}}).size() == 1, "registry: only operation is picked out of the engine's other fields");
  Check(srv::RegisteredEngineDiagnosticFieldsForTest("CRYPTO.HASH.INVALID_INPUT", {{"operation", "pgp_sym_encrypt"}}).empty(),
        "registry: the operation field is registered for CRYPTO.PGP.PROFILE_UNAVAILABLE only");
  Check(srv::RegisteredEngineDiagnosticFieldsForTest("CRYPTO.PGP.INVALID_KEY", {{"operation", "pgp_sym_encrypt"}}).empty(),
        "registry: a sibling CRYPTO.PGP.* code does not register the field");
}

void CheckPgcrypto() {
  // pgcrypto is the package capability spelling with NO scalar result (spec: invocation always refuses). Through the parser it has no scalar route.
  const auto artifacts = RunPipeline("SELECT pgcrypto() AS out_value;");
  std::string codes;
  for (const auto& d : artifacts.envelope.messages.diagnostics) codes += d.code + " ";
  for (const auto& d : artifacts.bound.messages.diagnostics) codes += d.code + " ";
  Check(!artifacts.envelope.messages.diagnostics.empty() || !artifacts.bound.messages.diagnostics.empty() || !artifacts.verifier.admitted,
        "pgcrypto(): the parser pipeline refuses it before any engine call");
  Check(!Contains(artifacts.envelope.payload, "sb.crypto.pgcrypto\""), "pgcrypto(): no lowered function call to sb.crypto.pgcrypto reaches an engine envelope");
  Check(!HasScalarProjectionDemand(BuildCst("SELECT pgcrypto();")), "pgcrypto(): no scalar projection route");
  std::cout << "pgcrypto() parser diagnostics: " << codes << " verifier_admitted=" << artifacts.verifier.admitted << '\n';
}

}  // namespace

int main() {
  for (const auto& shape : Shapes()) CheckOne(shape);
  CheckRegistry();
  CheckPgcrypto();
  if (failures != 0) {
    std::cerr << "sbsql_pgp_refusal_route_conformance=failed failures=" << failures << " checks=" << checks << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "sbsql_pgp_refusal_route_conformance=passed checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
