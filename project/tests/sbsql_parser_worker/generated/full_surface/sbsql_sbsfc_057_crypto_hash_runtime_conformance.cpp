// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_dispatch.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;
using sblr::SblrResult;
using sblr::SblrStatusCode;
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

constexpr const char* kSessionUuid = "019f5700-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f5700-0000-7000-8000-000000000003";
constexpr const char* kDeterministicBytes =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr const char* kDeterministicUuid = "019dffbb-f057-4000-8000-000000000057";
constexpr std::uint64_t kLocalTransactionId = 57057;

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.text_value = std::move(input);
  value.encoded_value = value.text_value;
  value.charset_name = "UTF-8";
  value.collation_name = "unicode_root";
  value.is_null = false;
  return value;
}

SblrValue BinaryValue(std::vector<std::uint8_t> bytes) {
  SblrValue value;
  value.descriptor_id = "binary";
  value.payload_kind = SblrValuePayloadKind::binary;
  value.binary_value = std::move(bytes);
  value.is_null = false;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.int64_value = input;
  value.has_int64_value = true;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

SblrValue Uint64Value(std::uint64_t input) {
  SblrValue value;
  value.descriptor_id = "uint64";
  value.payload_kind = SblrValuePayloadKind::unsigned_integer;
  value.uint64_value = input;
  value.has_uint64_value = true;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

functions::FunctionArgument Arg(std::string name, SblrValue value) {
  return functions::FunctionArgument{std::move(name), std::move(value)};
}

sblr::SblrExecutionContext BaseSblrContext() {
  sblr::SblrExecutionContext context;
  context.session_uuid = kSessionUuid;
  context.user_uuid = kPrincipalUuid;
  context.application_name = "sbsfc057-crypto-hash";
  context.security_context_present = true;
  context.transaction_context_present = true;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.deterministic_random_bytes_hex = kDeterministicBytes;
  context.deterministic_uuid_text = kDeterministicUuid;
  return context;
}

SblrResult Run(const functions::FunctionRegistry& registry,
               std::string function_id,
               std::vector<functions::FunctionArgument> arguments = {}) {
  functions::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context = BaseSblrContext();
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index].name.empty()) arguments[index].name = "arg" + std::to_string(index);
    request.arguments.push_back(std::move(arguments[index]));
  }
  return functions::DispatchFunctionCall(registry, std::move(request)).result;
}

std::string Hex(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

bool ExpectOkScalar(const SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1 ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected one successful non-mutating scalar result"
              << "; status=" << static_cast<int>(result.status)
              << "; scalar_count=" << result.scalar_values.size() << "\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  diagnostic=" << diagnostic.diagnostic_id << "\n";
    }
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.encoded_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBinaryHex(std::string_view case_id,
                     const SblrResult& result,
                     std::string_view expected_hex) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "binary" || Hex(value.binary_value) != expected_hex) {
    std::cerr << case_id << ": expected binary " << expected_hex
              << ", got " << value.descriptor_id << " " << Hex(value.binary_value) << "\n";
    return false;
  }
  return true;
}

bool ExpectUint64(std::string_view case_id,
                  const SblrResult& result,
                  std::uint64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "uint64" || !value.has_uint64_value ||
      value.uint64_value != expected) {
    std::cerr << case_id << ": expected uint64 " << expected
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectDependencyUnavailable(std::string_view case_id, const SblrResult& result) {
  if (result.ok() || result.status != SblrStatusCode::dependency_unavailable ||
      result.diagnostics.empty() ||
      result.diagnostics.front().diagnostic_id != "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE" ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE refusal\n";
    return false;
  }
  return true;
}

bool ExpectInvalidInput(std::string_view case_id, const SblrResult& result) {
  if (result.ok() || result.status != SblrStatusCode::execution_failed ||
      result.diagnostics.empty() ||
      result.diagnostics.front().diagnostic_id != "SB_DIAG_FUNCTION_INVALID_INPUT" ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_INVALID_INPUT refusal\n";
    return false;
  }
  return true;
}

sblr::SblrOperationEnvelope ProjectionEnvelope(
    std::string function_id,
    std::vector<api::EngineProjectionFunctionArgument> arguments) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC057-crypto-hash-projection");
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_function_id", std::move(function_id)});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", std::to_string(arguments.size())});
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto prefix = "projection_0_arg_" + std::to_string(index) + "_";
    envelope.operands.push_back({"text", prefix + "name", arguments[index].name});
    envelope.operands.push_back({"text", prefix + "type", arguments[index].type_name});
    envelope.operands.push_back({"text", prefix + "value", arguments[index].encoded_value});
    envelope.operands.push_back({"text", prefix + "is_null", arguments[index].is_null ? "true" : "false"});
  }
  return envelope;
}

api::EngineRequestContext ProjectionContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsfc057-crypto-hash-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc057-crypto-hash";
  context.security_context_present = true;
  return context;
}

bool ExpectProjectionText(std::string_view case_id,
                          const sblr::SblrDispatchResult& result,
                          std::string_view descriptor,
                          std::string_view expected) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one projected scalar field\n";
    return false;
  }
  const auto& value = result.api_result.result_shape.rows.front().fields.front().second;
  if (value.is_null || value.descriptor.canonical_type_name != descriptor ||
      value.encoded_value != expected) {
    std::cerr << case_id << ": expected projected " << descriptor << " " << expected
              << ", got " << value.descriptor.canonical_type_name << " "
              << value.encoded_value << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  const std::string armor_abc =
      "-----BEGIN SCRATCHBIRD PGCRYPTO ARMOR-----\n"
      "Version: ScratchBird-SBSFC-057\n"
      "Encoding: base64\n"
      "\n"
      "YWJj\n"
      "-----END SCRATCHBIRD PGCRYPTO ARMOR-----";
  const std::string armor_binary =
      "-----BEGIN SCRATCHBIRD PGCRYPTO ARMOR-----\n"
      "Version: ScratchBird-SBSFC-057\n"
      "Encoding: base64\n"
      "\n"
      "AP8Q\n"
      "-----END SCRATCHBIRD PGCRYPTO ARMOR-----";

  ok = ExpectDependencyUnavailable("SBSQL-697D0080DA8E SBSFC057-argon2-provider-refuses",
                                   Run(registry, "sb.crypto.argon2",
                                       {Arg("password", TextValue("character", "password"))})) && ok;
  ok = ExpectText("SBSQL-F2657259D869 SBSFC057-armor-text",
                  Run(registry, "sb.crypto.armor",
                      {Arg("value", TextValue("character", "abc"))}),
                  "character", armor_abc) && ok;
  ok = ExpectText("SBSQL-A475C1402E1D SBSFC057-armor-binary",
                  Run(registry, "sb.crypto.armor_binary",
                      {Arg("value", BinaryValue({0x00, 0xff, 0x10}))}),
                  "character", armor_binary) && ok;
  ok = ExpectDependencyUnavailable("SBSQL-DEAE160F496C SBSFC057-bcrypt-provider-refuses",
                                   Run(registry, "sb.crypto.bcrypt",
                                       {Arg("password", TextValue("character", "password"))})) && ok;
  ok = ExpectText("SBSQL-333906CB50B9 SBSFC057-blake2b",
                  Run(registry, "sb.crypto.blake2b",
                      {Arg("value", TextValue("character", "abc"))}),
                  "character",
                  "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17"
                  "d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923") && ok;
  ok = ExpectDependencyUnavailable("SBSQL-EDA08840598E SBSFC057-blake3-provider-refuses",
                                   Run(registry, "sb.crypto.blake3",
                                       {Arg("value", TextValue("character", "abc"))})) && ok;
  ok = ExpectDependencyUnavailable("SBSQL-38360AA175AA SBSFC057-crypt-provider-refuses",
                                   Run(registry, "sb.crypto.crypt")) && ok;
  ok = ExpectDependencyUnavailable("SBSQL-46DCE02DBE41 SBSFC057-crypt-password-salt-provider-refuses",
                                   Run(registry, "sb.crypto.crypt_password_salt",
                                       {Arg("password", TextValue("character", "password")),
                                        Arg("salt", TextValue("character", "salt"))})) && ok;
  ok = ExpectBinaryHex("SBSQL-0253C933634F SBSFC057-dearmor",
                       Run(registry, "sb.crypto.dearmor",
                           {Arg("armor", TextValue("character", armor_abc))}),
                       "616263") && ok;
  ok = ExpectBinaryHex("SBSQL-DFF3ADE173F5 SBSFC057-dearmor-text",
                       Run(registry, "sb.crypto.dearmor_text",
                           {Arg("armor", TextValue("character", "YWJj"))}),
                       "616263") && ok;
  ok = ExpectBinaryHex("SBSQL-98AC558CD662 SBSFC057-gen-random-bytes",
                       Run(registry, "sb.crypto.gen_random_bytes"),
                       "000102030405060708090a0b0c0d0e0f") && ok;
  ok = ExpectBinaryHex("SBSQL-7612CF167F37 SBSFC057-gen-random-bytes-n",
                       Run(registry, "sb.crypto.gen_random_bytes_n",
                           {Arg("n", Int64Value(4))}),
                       "00010203") && ok;
  ok = ExpectText("SBSQL-F609742097AE SBSFC057-gen-random-uuid",
                  Run(registry, "sb.crypto.gen_random_uuid"),
                  "uuid", kDeterministicUuid) && ok;
  ok = ExpectText("SBSQL-5F23337B660A SBSFC057-gen-salt",
                  Run(registry, "sb.crypto.gen_salt"),
                  "character", "$2b$06$./0123456789ABCD./0123") && ok;
  ok = ExpectText("SBSQL-058471F02F03 SBSFC057-gen-salt-md5",
                  Run(registry, "sb.crypto.gen_salt_algo",
                      {Arg("algorithm", TextValue("character", "md5"))}),
                  "character", "$1$./012345") && ok;
  ok = ExpectText("SBSQL-6144531FD80E SBSFC057-hmac-marker",
                  Run(registry, "sb.crypto.hmac"),
                  "character", "crypto.hmac") && ok;
  ok = ExpectText("SBSQL-D0FF02C4CDDE SBSFC057-hmac-sha256",
                  Run(registry, "sb.crypto.hmac_value_key_algo",
                      {Arg("value", TextValue("character", "abc")),
                       Arg("key", TextValue("character", "key")),
                       Arg("algorithm", TextValue("character", "sha256"))}),
                  "character", "9c196e32dc0175f86f4b1cb89289d6619de6bee699e4c378e68309ed97a1a6ab") && ok;
  ok = ExpectText("SBSQL-AF4E7BFEFDD1 SBSFC057-pgcrypto-marker",
                  Run(registry, "sb.crypto.pgcrypto"),
                  "character", "pgcrypto.sbsfc057.compatibility_envelope") && ok;
  ok = ExpectText("SBSQL-2854D8B0790B SBSFC057-pgp-pub-encrypt",
                  Run(registry, "sb.crypto.pgp_pub_encrypt",
                      {Arg("value", TextValue("character", "plain")),
                       Arg("key", TextValue("character", "pubkey"))}),
                  "character", "SBPGP1:pub:cHVia2V5:cGxhaW4=") && ok;
  ok = ExpectText("SBSQL-6358314B6883 SBSFC057-pgp-pub-decrypt",
                  Run(registry, "sb.crypto.pgp_pub_decrypt",
                      {Arg("value", TextValue("character", "SBPGP1:pub:cHVia2V5:cGxhaW4=")),
                       Arg("key", TextValue("character", "pubkey"))}),
                  "character", "plain") && ok;
  ok = ExpectText("SBSQL-C98EC981ACD7 SBSFC057-pgp-sym-encrypt",
                  Run(registry, "sb.crypto.pgp_sym_encrypt",
                      {Arg("value", TextValue("character", "plain")),
                       Arg("key", TextValue("character", "secret"))}),
                  "character", "SBPGP1:sym:c2VjcmV0:cGxhaW4=") && ok;
  ok = ExpectText("SBSQL-6DBE85C4B814 SBSFC057-pgp-sym-decrypt",
                  Run(registry, "sb.crypto.pgp_sym_decrypt",
                      {Arg("value", TextValue("character", "SBPGP1:sym:c2VjcmV0:cGxhaW4=")),
                       Arg("key", TextValue("character", "secret"))}),
                  "character", "plain") && ok;
  ok = ExpectText("SBSQL-C8996122850A SBSFC057-scrypt",
                  Run(registry, "sb.crypto.scrypt",
                      {Arg("password", TextValue("character", "password")),
                       Arg("salt", TextValue("character", "NaCl"))}),
                  "character", "27b418c674c769d12501fbb1f53bac32df6514c0f28d043872b148b348961a79") && ok;
  ok = ExpectText("SBSQL-BD3080D87EA5 SBSFC057-sha3-256",
                  Run(registry, "sb.crypto.sha3_256",
                      {Arg("value", TextValue("character", "abc"))}),
                  "character", "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532") && ok;
  ok = ExpectText("SBSQL-51BB6328126C SBSFC057-sha3-512",
                  Run(registry, "sb.crypto.sha3_512",
                      {Arg("value", TextValue("character", "abc"))}),
                  "character",
                  "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
                  "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0") && ok;
  ok = ExpectUint64("SBSQL-4AD05EF7474D SBSFC057-xxhash64",
                    Run(registry, "sb.crypto.xxhash64",
                        {Arg("value", TextValue("character", "abc"))}),
                    4952883123889572249ULL) && ok;
  ok = ExpectUint64("SBSQL-B75400EDF4FB SBSFC057-xxhash64-seed",
                    Run(registry, "sb.crypto.xxhash64_value_seed",
                        {Arg("value", TextValue("character", "abc")),
                         Arg("seed", Uint64Value(42))}),
                    1423657621850124518ULL) && ok;

  ok = ExpectInvalidInput("SBSFC057-dearmor-invalid",
                          Run(registry, "sb.crypto.dearmor",
                              {Arg("armor", TextValue("character", "not valid armor"))})) && ok;
  ok = ExpectInvalidInput("SBSFC057-hmac-bad-algo",
                          Run(registry, "sb.crypto.hmac_value_key_algo",
                              {Arg("value", TextValue("character", "abc")),
                               Arg("key", TextValue("character", "key")),
                               Arg("algorithm", TextValue("character", "unknown"))})) && ok;
  ok = ExpectInvalidInput("SBSFC057-gen-random-bytes-too-large",
                          Run(registry, "sb.crypto.gen_random_bytes_n",
                              {Arg("n", Uint64Value(2048))})) && ok;

  auto projection = ProjectionEnvelope(
      "sb.crypto.sha3_256",
      {api::EngineProjectionFunctionArgument{"value", "character", "abc", false}});
  ok = ExpectProjectionText("SBSFC057-sha3-256-projection-route",
                            sblr::DispatchSblrOperation({ProjectionContext(),
                                                         projection,
                                                         api::EngineApiRequest{}}),
                            "character",
                            "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532") && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_057_crypto_hash_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
