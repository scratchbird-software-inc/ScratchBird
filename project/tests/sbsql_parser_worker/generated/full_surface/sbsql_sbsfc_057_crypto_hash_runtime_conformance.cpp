#include "../../../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSFC-057 crypto/hash runtime conformance, REWRITTEN 2026-12-20 to the engine's current TYPED semantics (owner 2026-11-26, PGP 5 a: rewrite, do not loosen).
//
// OLD (to 2026-11-26): the cases passed text carriers and untyped numbers and asserted legacy shapes (character hex digests, a deterministic RNG and UUID from the
// request context, a bare base64 "armor", an argument-less hmac "marker", a 16-byte default gen_random_bytes, a $2b$06$ salt of fixed characters, a legacy
// pgcrypto "compatibility envelope" success, and the legacy SBPGP1 pgp_* envelope that carried the key and the plaintext in base64). ~20 cases failed at baseline
// because the engine moved to typed carriers and real providers while this file kept the legacy shapes; the four pgp_* cases PASSED while certifying a non-encryption.
// NEW: every case below asserts what the engine does today, with the carriers the engine and the parser's declarations require (binary in, binary out for digests and
// HMAC; uint32/uint64/uint16 carriers; RFC 9580 armor), against expected values taken from INDEPENDENT sources (the specification vectors named at each case, or a
// property the output must have), never from the engine's own output. Where the engine is wrong, that is reported as a finding, not accommodated here.
// The four pgp_* functions and pgcrypto are asserted as REFUSALS (owner 2026-11-26 PGP 2 a / PGP 3 a).
// Independent tools used for the vectors: python3 -I hashlib (sha3, blake2b, scrypt with the OpenSSL provider, hmac), base64, and a pure-python XXH64 written
// from the published algorithm; BLAKE3 uses the official BLAKE3 test vectors (input_len 0 and 1); scrypt uses RFC 7914 section 12 vectors 1 and 2.

#include "dispatch/function_dispatch.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_dispatch.hpp"
#include "canonical_projection_test_envelope.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;
using sblr::SblrResult;
using sblr::SblrStatusCode;
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

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

std::vector<std::uint8_t> Bytes(std::string_view text) { return {text.begin(), text.end()}; }

// A native unsigned carrier with NO decimal mirrors (uint16 requires none; the others accept none).
SblrValue UintValue(std::string descriptor, std::uint64_t input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::unsigned_integer;
  value.uint64_value = input;
  value.has_uint64_value = true;
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

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

functions::FunctionArgument Arg(std::string name, SblrValue value) {
  return functions::FunctionArgument{std::move(name), std::move(value)};
}

sblr::SblrExecutionContext BaseSblrContext() {
  sblr::SblrExecutionContext context;
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f5700-0000-7000-8000-000000000002");
  context.user_uuid = scratchbird::tests::FixtureUuidLiteral("019f5700-0000-7000-8000-000000000003");
  context.application_name = "sbsfc057-crypto-hash";
  context.security_context_present = true;
  context.transaction_context_present = true;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  // FINDING (2026-12-20, reported, not asserted): the engine's RNG functions ignore these two request-context overrides (they draw from the Core CSPRNG), so a
  // fixture can no longer pin their output; the cases below assert shape and distinctness instead. They are still set so any future honoring would show here.
  context.deterministic_random_bytes_hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  context.deterministic_uuid = scratchbird::tests::FixtureUuidLiteral("019dffbb-f057-4000-8000-000000000057");
  return context;
}

SblrResult Run(const functions::FunctionRegistry& registry,
               std::string function_id,
               std::vector<functions::FunctionArgument> arguments = {}) {
  functions::FunctionCallRequest request;
  // The fixture resolves its symbolic test case through the published seed
  // registry; executable dispatch receives the registry's binary identity.
  if (const auto* entry = registry.Lookup(function_id))
    request.context.function_uuid = entry->function_uuid;
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

std::string HexOf(std::string_view text) { return Hex(Bytes(text)); }

std::string Base64(std::string_view in) {
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

bool Contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

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

// OLD: ExpectText compared value.encoded_value to a hex string under descriptor "character" for the digests and HMAC. NEW: digests/HMAC/scrypt/random bytes are
// BINARY results (the parser declares binary) and armor/salts are character results whose text_value is the exact text.
bool ExpectCharacter(std::string_view case_id, const SblrResult& result, std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "character" || value.payload_kind != SblrValuePayloadKind::text ||
      value.text_value != expected || !value.binary_value.empty()) {
    std::cerr << case_id << ": expected character " << expected << ", got " << value.descriptor_id << " " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBinaryHex(std::string_view case_id, const SblrResult& result, std::string_view expected_hex) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "binary" || value.payload_kind != SblrValuePayloadKind::binary ||
      !value.text_value.empty() || !value.encoded_value.empty() || Hex(value.binary_value) != expected_hex) {
    std::cerr << case_id << ": expected binary " << expected_hex << ", got " << value.descriptor_id << " " << Hex(value.binary_value) << "\n";
    return false;
  }
  return true;
}

bool ExpectBinaryLength(std::string_view case_id, const SblrResult& result, std::size_t length) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "binary" || value.payload_kind != SblrValuePayloadKind::binary ||
      !value.text_value.empty() || !value.encoded_value.empty() || value.binary_value.size() != length) {
    std::cerr << case_id << ": expected binary of " << length << " bytes, got " << value.descriptor_id << " " << value.binary_value.size() << " bytes\n";
    return false;
  }
  return true;
}

bool ExpectUint64(std::string_view case_id, const SblrResult& result, std::uint64_t expected) {
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

// One refusal: not ok, the exact status, the exact specific diagnostic code (owner Q193: one distinct code per cause), no result value, no mutation.
bool ExpectRefusal(std::string_view case_id, const SblrResult& result, SblrStatusCode status, std::string_view code) {
  if (result.ok() || result.status != status || result.diagnostics.empty() || result.diagnostics.front().diagnostic_id != code ||
      !result.scalar_values.empty() || !result.rows.empty() || result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected status " << static_cast<int>(status) << " refusal " << code << ", got status " << static_cast<int>(result.status)
              << " code " << (result.diagnostics.empty() ? "<none>" : result.diagnostics.front().diagnostic_id) << "\n";
    return false;
  }
  return true;
}

// OLD: ExpectDependencyUnavailable/ExpectInvalidInput demanded the generic SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE / SB_DIAG_FUNCTION_INVALID_INPUT for every refusal.
// NEW: the provider-gated functions still refuse with the generic dependency code (unchanged); every input refusal carries its family's specific code.
bool ExpectDependencyUnavailable(std::string_view case_id, const SblrResult& result) {
  return ExpectRefusal(case_id, result, SblrStatusCode::dependency_unavailable, "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE");
}

std::string FieldText(const sblr::SblrRuntimeDiagnostic& diagnostic, std::string_view key) {
  for (const auto& field : diagnostic.fields)
    if (field.key == key)
      if (const auto* text = std::get_if<std::string>(&field.value)) return *text;
  return {};
}

// Everything a result could show a user: ids, keys, details, every field value, every scalar's text/binary.
std::string EverythingIn(const SblrResult& result) {
  std::string text;
  for (const auto& d : result.diagnostics) {
    text += d.diagnostic_id + "|" + d.message_key + "|" + d.detail + "|";
    for (const auto& f : d.fields) {
      text += f.key + "=";
      if (const auto* value = std::get_if<std::string>(&f.value)) text += *value;
      text += "|";
    }
  }
  for (const auto& v : result.scalar_values) text += v.text_value + v.encoded_value + std::string(v.binary_value.begin(), v.binary_value.end());
  return text;
}

// PGP refusal (owner 2026-11-26 PGP 2 a): CRYPTO.PGP.PROFILE_UNAVAILABLE, status dependency_unavailable, the closed `operation` field, and NO plaintext, key, or
// envelope material in any code, detail, field or value (raw, base64 or hex form). The refusal is the first act: arity, NULL and carrier types do not change it.
bool ExpectPgpRefusal(std::string_view case_id, const SblrResult& result, std::string_view operation, const std::vector<std::string>& secrets) {
  bool ok = ExpectRefusal(case_id, result, SblrStatusCode::dependency_unavailable, "CRYPTO.PGP.PROFILE_UNAVAILABLE");
  if (!result.diagnostics.empty()) {
    if (FieldText(result.diagnostics.front(), "operation") != operation) {
      std::cerr << case_id << ": expected the operation field `" << operation << "`, got `" << FieldText(result.diagnostics.front(), "operation") << "`\n";
      ok = false;
    }
  }
  const auto everything = EverythingIn(result);
  for (const auto& secret : secrets) {
    for (const auto& form : {secret, Base64(secret), HexOf(secret)}) {
      if (Contains(everything, form)) {
        std::cerr << case_id << ": the response contains secret material `" << form.substr(0, 12) << "...`\n";
        ok = false;
      }
    }
  }
  if (Contains(everything, "SBPGP1")) {
    std::cerr << case_id << ": the response contains the retired SBPGP1 envelope text\n";
    ok = false;
  }
  return ok;
}

// A projection envelope for one function call. The shared canonicalizer (canonical_projection_test_envelope.hpp, unchanged) encodes every argument as UTF-8 text, which a
// BINARY argument cannot be; so the result type is declared here and each binary argument's canonical typed body (the Core binary descriptor identity, the little-endian
// byte count, then the exact bytes) is written AFTER canonicalization, on the operand the helper produced.
sblr::SblrOperationEnvelope ProjectionEnvelope(
    std::string function_id,
    std::string result_type,
    std::vector<api::EngineProjectionFunctionArgument> arguments,
    const std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>>& binary_arguments = {}) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC057-crypto-hash-projection");
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "value"});
  envelope.operands.push_back({"text", "projection_0_type", std::move(result_type)});
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
  envelope = scratchbird::tests::sbsql::CanonicalizeProjectionEnvelopeForTest(std::move(envelope));
  if (!binary_arguments.empty()) {
    namespace dt = scratchbird::core::datatypes;
    const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
    if (!catalog.ok()) throw std::logic_error("Core datatype catalog unavailable");
    const auto row = dt::LookupDatatypeCatalogRow(catalog.manifest, dt::CanonicalTypeId::binary);
    if (!row.ok() || row.manifest.descriptor_rows.size() != 1) throw std::logic_error("Core BINARY descriptor unavailable");
    const auto& descriptor = row.manifest.descriptor_rows.front().descriptor_uuid.value;
    for (const auto& [index, bytes] : binary_arguments) {
      const auto name = "projection_0_arg_" + std::to_string(index) + "_value";
      for (auto& operand : envelope.operands) {
        if (operand.name != name) continue;
        operand.type = "binary";  // the engine reconciles a binary literal by the OPERAND's own type tag (sblr_projection_binary_literals.hpp)
        operand.value.clear();
        operand.value_kind = sblr::SblrValueKind::literal_typed;
        operand.value_flags = 0;
        operand.value_body.assign(descriptor.bytes.begin(), descriptor.bytes.end());
        operand.value_body.resize(24, 0);
        for (unsigned byte = 0; byte < 8; ++byte) operand.value_body[16 + byte] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(bytes.size()) >> (byte * 8)) & 0xffu);
        operand.value_body.insert(operand.value_body.end(), bytes.begin(), bytes.end());
      }
    }
  }
  return envelope;
}

api::EngineRequestContext ProjectionContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsfc057-crypto-hash-projection";
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f5700-0000-7000-8000-000000000002");
  context.principal_uuid = scratchbird::tests::FixtureUuidLiteral("019f5700-0000-7000-8000-000000000003");
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc057-crypto-hash";
  context.security_context_present = true;
  return context;
}

// OLD: the projection case passed a "character" argument and expected a character hex digest. NEW: the digest takes a binary argument and projects a binary value.
bool ExpectProjectionBinary(std::string_view case_id,
                            const sblr::SblrDispatchResult& result,
                            std::string_view expected_hex) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one projected scalar field\n";
    for (const auto& d : result.api_result.diagnostics) std::cerr << "  diagnostic=" << d.code << " " << d.detail << "\n";
    return false;
  }
  const auto& value = result.api_result.result_shape.rows.front().fields.front().second;
  if (value.is_null || value.descriptor.canonical_type_name != "binary" || !value.encoded_value.empty() || Hex(value.binary_value) != expected_hex) {
    std::cerr << case_id << ": expected projected binary " << expected_hex
              << ", got " << value.descriptor.canonical_type_name << " "
              << Hex(value.binary_value) << "\n";
    return false;
  }
  return true;
}

// A projected pgp_* call is refused by the engine with the same specific code, one public field (the closed operation token), no row.
bool ExpectProjectionPgpRefusal(std::string_view case_id, const sblr::SblrDispatchResult& result, std::string_view operation) {
  bool ok = true;
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api || result.api_result.ok ||
      !result.api_result.result_shape.rows.empty() || result.api_result.diagnostics.size() != 1 ||
      result.api_result.diagnostics.front().code != "CRYPTO.PGP.PROFILE_UNAVAILABLE") {
    std::cerr << case_id << ": expected a projected CRYPTO.PGP.PROFILE_UNAVAILABLE refusal with no row\n";
    for (const auto& d : result.api_result.diagnostics) std::cerr << "  diagnostic=" << d.code << " " << d.detail << "\n";
    return false;
  }
  const auto& diagnostic = result.api_result.diagnostics.front();
  if (diagnostic.fields.size() != 1 || diagnostic.fields.front().key != "operation" || diagnostic.fields.front().value != operation) {
    std::cerr << case_id << ": expected exactly the public field operation=" << operation << "\n";
    ok = false;
  }
  std::string everything = diagnostic.code + diagnostic.message_key + diagnostic.detail;
  for (const auto& f : diagnostic.fields) everything += f.key + f.value;
  for (const auto& secret : {std::string("plain-sentinel-57"), std::string("key-sentinel-57")})
    for (const auto& form : {secret, Base64(secret), HexOf(secret)})
      if (Contains(everything, form)) {
        std::cerr << case_id << ": the projected refusal contains secret material\n";
        ok = false;
      }
  return ok;
}

// RFC 9580 section 6 armor as the engine emits it (LF, 64 columns, no metadata, no CRC), built here from python's base64 for the bytes 0..99 and from "abc"/00ff10.
const std::string kArmorAbc = "-----BEGIN PGP MESSAGE-----\n\nYWJj\n-----END PGP MESSAGE-----\n";
const std::string kArmor00ff10 = "-----BEGIN PGP MESSAGE-----\n\nAP8Q\n-----END PGP MESSAGE-----\n";
const std::string kArmor100 =
    "-----BEGIN PGP MESSAGE-----\n\n"
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v\n"
    "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f\n"
    "YGFiYw==\n"
    "-----END PGP MESSAGE-----\n";

bool Alphabet(std::string_view text, std::string_view alphabet) {
  return std::all_of(text.begin(), text.end(), [&](char c) { return alphabet.find(c) != std::string_view::npos; });
}

}  // namespace

int main() {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  // ---- provider-gated functions: unchanged behaviour (the engine has no pinned provider) ------------------------------------------------------------------
  ok = ExpectDependencyUnavailable("SBSQL-697D0080DA8E SBSFC057-argon2-provider-refuses",
                                   Run(registry, "sb.crypto.argon2",
                                       {Arg("password", TextValue("character", "password"))})) && ok;
  ok = ExpectDependencyUnavailable("SBSQL-DEAE160F496C SBSFC057-bcrypt-provider-refuses",
                                   Run(registry, "sb.crypto.bcrypt",
                                       {Arg("password", TextValue("character", "password"))})) && ok;
  ok = ExpectDependencyUnavailable("SBSQL-38360AA175AA SBSFC057-crypt-provider-refuses",
                                   Run(registry, "sb.crypto.crypt")) && ok;
  ok = ExpectDependencyUnavailable("SBSQL-46DCE02DBE41 SBSFC057-crypt-password-salt-provider-refuses",
                                   Run(registry, "sb.crypto.crypt_password_salt",
                                       {Arg("password", TextValue("character", "password")),
                                        Arg("salt", TextValue("character", "salt"))})) && ok;

  // ---- armor / dearmor ---------------------------------------------------------------------------------------------------------------------------------
  // OLD armor-text: a TEXT "abc" expected the ScratchBird-SBSFC-057 armor with a Version/Encoding header (CRYPTO.ARMOR.INVALID_INPUT at baseline).
  // NEW: armor takes a BINARY value and returns the RFC 9580 armor as character text; a text carrier is refused with the armor code.
  ok = ExpectCharacter("SBSQL-F2657259D869 SBSFC057-armor-text",
                       Run(registry, "sb.crypto.armor", {Arg("value", BinaryValue(Bytes("abc")))}), kArmorAbc) && ok;
  ok = ExpectRefusal("SBSQL-F2657259D869 SBSFC057-armor-text-carrier-refused",
                     Run(registry, "sb.crypto.armor", {Arg("value", TextValue("character", "abc"))}),
                     SblrStatusCode::execution_failed, "CRYPTO.ARMOR.INVALID_INPUT") && ok;
  {
    std::vector<std::uint8_t> hundred(100);
    for (std::size_t i = 0; i < hundred.size(); ++i) hundred[i] = static_cast<std::uint8_t>(i);
    ok = ExpectCharacter("SBSFC057-armor-100-bytes-64-columns", Run(registry, "sb.crypto.armor", {Arg("value", BinaryValue(hundred))}), kArmor100) && ok;
  }
  // OLD armor-binary: expected a character result. NEW: armor_binary returns the same armored bytes in a BINARY carrier (the parser declares binary).
  ok = ExpectBinaryHex("SBSQL-A475C1402E1D SBSFC057-armor-binary",
                       Run(registry, "sb.crypto.armor_binary", {Arg("value", BinaryValue({0x00, 0xff, 0x10}))}), HexOf(kArmor00ff10)) && ok;
  // OLD dearmor: the legacy ScratchBird armor expected 616263. NEW: the RFC 9580 armor decodes to the bytes, as a BINARY value.
  ok = ExpectBinaryHex("SBSQL-0253C933634F SBSFC057-dearmor",
                       Run(registry, "sb.crypto.dearmor", {Arg("armor", TextValue("character", kArmorAbc))}), "616263") && ok;
  // OLD dearmor-text: the bare base64 "YWJj" expected binary 616263. NEW: bare base64 is refused (a single matching RFC envelope is required); the armored text
  // decodes to the CHARACTER text "abc" (the declared result), validated as UTF-8.
  ok = ExpectCharacter("SBSQL-DFF3ADE173F5 SBSFC057-dearmor-text",
                       Run(registry, "sb.crypto.dearmor_text", {Arg("armor", TextValue("character", kArmorAbc))}), "abc") && ok;
  ok = ExpectRefusal("SBSQL-DFF3ADE173F5 SBSFC057-dearmor-text-bare-base64-refused",
                     Run(registry, "sb.crypto.dearmor_text", {Arg("armor", TextValue("character", "YWJj"))}),
                     SblrStatusCode::execution_failed, "CRYPTO.ARMOR.INVALID_INPUT") && ok;
  ok = ExpectBinaryHex("SBSFC057-dearmor-100-bytes-64-columns",
                       Run(registry, "sb.crypto.dearmor", {Arg("armor", TextValue("character", kArmor100))}),
                       [] { std::vector<std::uint8_t> h(100); for (std::size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::uint8_t>(i); return Hex(h); }()) && ok;

  // ---- fixed digests: binary in, binary out. OLD: text carrier in, character hex out. -----------------------------------------------------------------------
  // blake2b-512("abc"), sha3-256("abc"), sha3-512("abc"): python hashlib (the FIPS 202 / RFC 7693 values).
  ok = ExpectBinaryHex("SBSQL-333906CB50B9 SBSFC057-blake2b",
                       Run(registry, "sb.crypto.blake2b", {Arg("value", BinaryValue(Bytes("abc")))}),
                       "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17"
                       "d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923") && ok;
  // OLD blake3-provider-refuses: SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE. NEW: BLAKE3 is implemented (core/hash/blake3_digest); official test vectors input_len 0 and 1.
  ok = ExpectBinaryHex("SBSQL-EDA08840598E SBSFC057-blake3-input-len-0",
                       Run(registry, "sb.crypto.blake3", {Arg("value", BinaryValue({}))}),
                       "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262") && ok;
  ok = ExpectBinaryHex("SBSQL-EDA08840598E SBSFC057-blake3-input-len-1",
                       Run(registry, "sb.crypto.blake3", {Arg("value", BinaryValue({0x00}))}),
                       "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213") && ok;
  ok = ExpectBinaryHex("SBSQL-BD3080D87EA5 SBSFC057-sha3-256",
                       Run(registry, "sb.crypto.sha3_256", {Arg("value", BinaryValue(Bytes("abc")))}),
                       "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532") && ok;
  ok = ExpectBinaryHex("SBSQL-51BB6328126C SBSFC057-sha3-512",
                       Run(registry, "sb.crypto.sha3_512", {Arg("value", BinaryValue(Bytes("abc")))}),
                       "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
                       "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0") && ok;
  // NEW: a text carrier is refused by the digest family's specific code (it used to be accepted).
  for (const char* id : {"sb.crypto.blake2b", "sb.crypto.blake3", "sb.crypto.sha3_256", "sb.crypto.sha3_512"})
    ok = ExpectRefusal(std::string("SBSFC057-digest-text-carrier-refused ") + id,
                       Run(registry, id, {Arg("value", TextValue("character", "abc"))}),
                       SblrStatusCode::execution_failed, "CRYPTO.HASH.INVALID_INPUT") && ok;

  // ---- HMAC: three typed arguments (value, key, algorithm), value and key the same family, binary result. -------------------------------------------------
  // OLD hmac-marker: sb.crypto.hmac with NO arguments returned the text "crypto.hmac". NEW: that is refused (CRYPTO.HMAC.INVALID_INPUT).
  ok = ExpectRefusal("SBSQL-6144531FD80E SBSFC057-hmac-marker-refused", Run(registry, "sb.crypto.hmac"),
                     SblrStatusCode::execution_failed, "CRYPTO.HMAC.INVALID_INPUT") && ok;
  // hmac-sha256(key="key", "abc") and the RFC 4231 test case 2 (key "Jefe"): python hmac.
  ok = ExpectBinaryHex("SBSQL-D0FF02C4CDDE SBSFC057-hmac-sha256",
                       Run(registry, "sb.crypto.hmac_value_key_algo",
                           {Arg("value", BinaryValue(Bytes("abc"))), Arg("key", BinaryValue(Bytes("key"))), Arg("algorithm", TextValue("character", "sha256"))}),
                       "9c196e32dc0175f86f4b1cb89289d6619de6bee699e4c378e68309ed97a1a6ab") && ok;
  ok = ExpectBinaryHex("SBSQL-6144531FD80E SBSFC057-hmac-three-arguments-same-engine-path",
                       Run(registry, "sb.crypto.hmac",
                           {Arg("value", BinaryValue(Bytes("what do ya want for nothing?"))), Arg("key", BinaryValue(Bytes("Jefe"))), Arg("algorithm", TextValue("character", "sha256"))}),
                       "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") && ok;
  ok = ExpectBinaryHex("SBSFC057-hmac-sha256-character-carriers",
                       Run(registry, "sb.crypto.hmac_value_key_algo",
                           {Arg("value", TextValue("character", "abc")), Arg("key", TextValue("character", "key")), Arg("algorithm", TextValue("character", "sha256"))}),
                       "9c196e32dc0175f86f4b1cb89289d6619de6bee699e4c378e68309ed97a1a6ab") && ok;
  ok = ExpectRefusal("SBSFC057-hmac-mixed-families-refused",
                     Run(registry, "sb.crypto.hmac_value_key_algo",
                         {Arg("value", BinaryValue(Bytes("abc"))), Arg("key", TextValue("character", "key")), Arg("algorithm", TextValue("character", "sha256"))}),
                     SblrStatusCode::execution_failed, "CRYPTO.HMAC.INVALID_INPUT") && ok;

  // ---- random bytes, UUID and salts: the Core CSPRNG answers, so assert TYPE, LENGTH, SHAPE and DISTINCTNESS (OLD pinned fixture bytes) -------------------------
  // OLD gen_random_bytes: no argument returned 16 deterministic bytes. NEW: the count is one required uint32 (1..1024); no argument is refused.
  ok = ExpectRefusal("SBSQL-98AC558CD662 SBSFC057-gen-random-bytes-no-count-refused", Run(registry, "sb.crypto.gen_random_bytes"),
                     SblrStatusCode::execution_failed, "CRYPTO.RNG.INVALID_LENGTH") && ok;
  {
    const auto first = Run(registry, "sb.crypto.gen_random_bytes", {Arg("n", UintValue("uint32", 16))});
    const auto second = Run(registry, "sb.crypto.gen_random_bytes", {Arg("n", UintValue("uint32", 16))});
    ok = ExpectBinaryLength("SBSQL-98AC558CD662 SBSFC057-gen-random-bytes", first, 16) && ok;
    ok = ExpectBinaryLength("SBSQL-98AC558CD662 SBSFC057-gen-random-bytes-second-call", second, 16) && ok;
    if (first.ok() && second.ok() && first.scalar_values.size() == 1 && second.scalar_values.size() == 1 &&
        first.scalar_values[0].binary_value == second.scalar_values[0].binary_value) {
      std::cerr << "SBSFC057-gen-random-bytes: two 16-byte draws were identical\n";
      ok = false;
    }
  }
  ok = ExpectBinaryLength("SBSQL-7612CF167F37 SBSFC057-gen-random-bytes-n", Run(registry, "sb.crypto.gen_random_bytes_n", {Arg("n", UintValue("uint32", 4))}), 4) && ok;
  ok = ExpectBinaryLength("SBSFC057-gen-random-bytes-limit", Run(registry, "sb.crypto.gen_random_bytes_n", {Arg("n", UintValue("uint32", 1024))}), 1024) && ok;
  // OLD: an int64 count (4) was accepted. NEW: only a uint32 carrier is (CRYPTO.RNG.INVALID_LENGTH otherwise).
  ok = ExpectRefusal("SBSFC057-gen-random-bytes-int64-carrier-refused", Run(registry, "sb.crypto.gen_random_bytes_n", {Arg("n", Int64Value(4))}),
                     SblrStatusCode::execution_failed, "CRYPTO.RNG.INVALID_LENGTH") && ok;
  ok = ExpectRefusal("SBSFC057-gen-random-bytes-zero-refused", Run(registry, "sb.crypto.gen_random_bytes_n", {Arg("n", UintValue("uint32", 0))}),
                     SblrStatusCode::execution_failed, "CRYPTO.RNG.INVALID_LENGTH") && ok;
  // OLD gen-random-uuid: the request context's deterministic uuid. NEW: a Core-random UUIDv4 (version 4, RFC variant), native binary payload, no text mirror.
  {
    const auto first = Run(registry, "sb.crypto.gen_random_uuid");
    const auto second = Run(registry, "sb.crypto.gen_random_uuid");
    bool shaped = ExpectOkScalar(first, "SBSQL-F609742097AE SBSFC057-gen-random-uuid") && ExpectOkScalar(second, "SBSFC057-gen-random-uuid-second");
    if (shaped) {
      const auto& a = first.scalar_values.front();
      const auto& b = second.scalar_values.front();
      const bool good = a.descriptor_id == "uuid" && a.payload_kind == SblrValuePayloadKind::uuid_binary && (a.uuid_value.bytes[6] >> 4) == 4 &&
                        (a.uuid_value.bytes[8] & 0xc0) == 0x80 && a.text_value.empty() && a.encoded_value.empty() && a.binary_value.empty() && !(a.uuid_value == b.uuid_value);
      if (!good) std::cerr << "SBSQL-F609742097AE SBSFC057-gen-random-uuid: expected two distinct native UUIDv4 values\n";
      shaped = good;
    }
    ok = shaped && ok;
  }
  // gen_salt: OLD no-argument "$2b$06$./0123456789ABCD./0123" and md5 "$1$./012345". NEW: the algorithm is a required character argument; the salt text is random
  // over the crypt radix-64 alphabet. bcrypt: "$2b$" + two-digit cost + "$" + 22 chars (the last has four zero pad bits: one of ". O e u"); md5: "$1$" + 8 chars + "$".
  {
    const auto bf = Run(registry, "sb.crypto.gen_salt", {Arg("algorithm", TextValue("character", "bf"))});
    ok = ExpectOkScalar(bf, "SBSQL-5F23337B660A SBSFC057-gen-salt") && ok;
    if (bf.ok() && bf.scalar_values.size() == 1) {
      const auto& text = bf.scalar_values[0].text_value;
      const bool good = bf.scalar_values[0].descriptor_id == "character" && text.size() == 29 && text.starts_with("$2b$06$") &&
                        Alphabet(text.substr(7), "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") && std::string_view(".Oeu").find(text.back()) != std::string_view::npos;
      if (!good) std::cerr << "SBSQL-5F23337B660A SBSFC057-gen-salt: not a $2b$06$ salt of 22 radix-64 characters: " << text << "\n";
      ok = good && ok;
    }
    const auto md5 = Run(registry, "sb.crypto.gen_salt_algo", {Arg("algorithm", TextValue("character", "md5")), Arg("iter_count", UintValue("uint32", 1000))});
    ok = ExpectOkScalar(md5, "SBSQL-058471F02F03 SBSFC057-gen-salt-md5") && ok;
    if (md5.ok() && md5.scalar_values.size() == 1) {
      const auto& text = md5.scalar_values[0].text_value;
      const bool good = text.size() == 12 && text.starts_with("$1$") && text.back() == '$' && Alphabet(text.substr(3, 8), "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
      if (!good) std::cerr << "SBSQL-058471F02F03 SBSFC057-gen-salt-md5: not a $1$ salt of 8 radix-64 characters: " << text << "\n";
      ok = good && ok;
    }
    const auto bf_cost = Run(registry, "sb.crypto.gen_salt_algo", {Arg("algorithm", TextValue("character", "bf")), Arg("iter_count", UintValue("uint32", 8))});
    ok = ExpectOkScalar(bf_cost, "SBSFC057-gen-salt-bf-cost-8") && ok;
    if (bf_cost.ok() && bf_cost.scalar_values.size() == 1 && !bf_cost.scalar_values[0].text_value.starts_with("$2b$08$")) {
      std::cerr << "SBSFC057-gen-salt-bf-cost-8: expected the $2b$08$ prefix\n";
      ok = false;
    }
    const auto again = Run(registry, "sb.crypto.gen_salt", {Arg("algorithm", TextValue("character", "bf"))});
    if (bf.ok() && again.ok() && bf.scalar_values.size() == 1 && again.scalar_values.size() == 1 && bf.scalar_values[0].text_value == again.scalar_values[0].text_value) {
      std::cerr << "SBSFC057-gen-salt: two salts were identical\n";
      ok = false;
    }
  }
  ok = ExpectRefusal("SBSFC057-gen-salt-no-algorithm-refused", Run(registry, "sb.crypto.gen_salt"),
                     SblrStatusCode::execution_failed, "CRYPTO.PASSWORD.INVALID_PARAMETER") && ok;
  ok = ExpectRefusal("SBSFC057-gen-salt-unknown-algorithm-refused", Run(registry, "sb.crypto.gen_salt", {Arg("algorithm", TextValue("character", "des"))}),
                     SblrStatusCode::execution_failed, "CRYPTO.PASSWORD.UNSUPPORTED_ALGORITHM") && ok;
  ok = ExpectRefusal("SBSFC057-gen-salt-bf-cost-3-refused", Run(registry, "sb.crypto.gen_salt_algo", {Arg("algorithm", TextValue("character", "bf")), Arg("iter_count", UintValue("uint32", 3))}),
                     SblrStatusCode::execution_failed, "CRYPTO.PASSWORD.INVALID_COST") && ok;

  // ---- scrypt: (password character, salt binary, N uint64, r uint32, p uint32, key length uint16) -> binary. OLD: two text arguments and a 32-byte shim value. ---
  // RFC 7914 section 12 vectors 1 and 2 (also python hashlib.scrypt).
  ok = ExpectBinaryHex("SBSQL-C8996122850A SBSFC057-scrypt-rfc7914-vector-1",
                       Run(registry, "sb.crypto.scrypt",
                           {Arg("password", TextValue("character", "")), Arg("salt", BinaryValue({})), Arg("n", UintValue("uint64", 16)),
                            Arg("r", UintValue("uint32", 1)), Arg("p", UintValue("uint32", 1)), Arg("length", UintValue("uint16", 64))}),
                       "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906") && ok;
  ok = ExpectBinaryHex("SBSQL-C8996122850A SBSFC057-scrypt-rfc7914-vector-2",
                       Run(registry, "sb.crypto.scrypt",
                           {Arg("password", TextValue("character", "password")), Arg("salt", BinaryValue(Bytes("NaCl"))), Arg("n", UintValue("uint64", 1024)),
                            Arg("r", UintValue("uint32", 8)), Arg("p", UintValue("uint32", 16)), Arg("length", UintValue("uint16", 64))}),
                       "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640") && ok;
  ok = ExpectRefusal("SBSFC057-scrypt-two-text-arguments-refused",
                     Run(registry, "sb.crypto.scrypt", {Arg("password", TextValue("character", "password")), Arg("salt", TextValue("character", "NaCl"))}),
                     SblrStatusCode::execution_failed, "CRYPTO.PASSWORD.INVALID_PARAMETER") && ok;

  // ---- xxhash64 (unchanged and passing): independent pure-python XXH64 of "abc" with seed 0 and 42 ---------------------------------------------------------
  ok = ExpectUint64("SBSQL-4AD05EF7474D SBSFC057-xxhash64",
                    Run(registry, "sb.crypto.xxhash64", {Arg("value", TextValue("character", "abc"))}),
                    4952883123889572249ULL) && ok;
  ok = ExpectUint64("SBSQL-B75400EDF4FB SBSFC057-xxhash64-seed",
                    Run(registry, "sb.crypto.xxhash64_value_seed",
                        {Arg("value", TextValue("character", "abc")), Arg("seed", UintValue("uint64", 42))}),
                    1423657621850124518ULL) && ok;

  // ---- pgcrypto: the package capability spelling is NOT a callable value (spec: invocation always refuses). OLD: a "compatibility envelope" success marker. ----
  ok = ExpectRefusal("SBSQL-AF4E7BFEFDD1 SBSFC057-pgcrypto-refuses", Run(registry, "sb.crypto.pgcrypto"),
                     SblrStatusCode::unsupported_feature, "CRYPTO.PACKAGE.NOT_CALLABLE") && ok;
  ok = ExpectRefusal("SBSFC057-pgcrypto-with-arguments-refuses", Run(registry, "sb.crypto.pgcrypto", {Arg("value", BinaryValue(Bytes("abc")))}),
                     SblrStatusCode::unsupported_feature, "CRYPTO.PACKAGE.NOT_CALLABLE") && ok;

  // ---- pgp_*: REFUSED (owner 2026-11-26 PGP 2 a). OLD: the legacy SBPGP1 envelope "SBPGP1:<sym|pub>:" + base64(key) + ":" + base64(plaintext) was PRODUCED by encrypt and
  // OPENED by decrypt: the key and the plaintext were recoverable by base64 alone (not encryption), the result was character where the parser declares binary. NEW: each of
  // the four returns CRYPTO.PGP.PROFILE_UNAVAILABLE (status dependency_unavailable) with the closed `operation` field, no value, and no key/plaintext/envelope material anywhere.
  // The refusal is the first act, so arity, NULLs and carrier types change nothing (refuse-always while the function is not admitted; owner question recorded).
  {
    const std::string plain = "plain-sentinel-57", key = "key-sentinel-57";
    const std::vector<std::string> secrets{plain, key};
    const std::string legacy = "SBPGP1:sym:" + Base64(key) + ":" + Base64(plain);
    const struct { const char* id; const char* name; } fns[] = {{"sb.crypto.pgp_sym_encrypt", "pgp_sym_encrypt"}, {"sb.crypto.pgp_sym_decrypt", "pgp_sym_decrypt"},
                                                                  {"sb.crypto.pgp_pub_encrypt", "pgp_pub_encrypt"}, {"sb.crypto.pgp_pub_decrypt", "pgp_pub_decrypt"}};
    for (const auto& fn : fns) {
      const std::string base = std::string("SBSFC057-") + fn.name;
      ok = ExpectPgpRefusal(base + "-typed-arguments",
                            Run(registry, fn.id, {Arg("a", BinaryValue(Bytes(plain))), Arg("b", BinaryValue(Bytes(key)))}), fn.name, secrets) && ok;
      ok = ExpectPgpRefusal(base + "-text-arguments",
                            Run(registry, fn.id, {Arg("a", TextValue("character", plain)), Arg("b", TextValue("character", key))}), fn.name, secrets) && ok;
      ok = ExpectPgpRefusal(base + "-legacy-envelope-message",
                            Run(registry, fn.id, {Arg("a", BinaryValue(Bytes(legacy))), Arg("b", TextValue("character", key))}), fn.name, secrets) && ok;
      ok = ExpectPgpRefusal(base + "-legacy-envelope-text-message",
                            Run(registry, fn.id, {Arg("a", TextValue("character", legacy)), Arg("b", TextValue("character", key))}), fn.name, secrets) && ok;
      ok = ExpectPgpRefusal(base + "-no-arguments", Run(registry, fn.id), fn.name, secrets) && ok;
      ok = ExpectPgpRefusal(base + "-null-argument-is-refused-not-null",
                            Run(registry, fn.id, {Arg("a", NullValue("binary")), Arg("b", BinaryValue(Bytes(key)))}), fn.name, secrets) && ok;
      ok = ExpectPgpRefusal(base + "-options-argument",
                            Run(registry, fn.id, {Arg("a", BinaryValue(Bytes(plain))), Arg("b", BinaryValue(Bytes(key))), Arg("options", TextValue("character", "cipher-algo=aes256"))}),
                            fn.name, secrets) && ok;
    }
  }

  // ---- negative cases. OLD: each demanded the generic SB_DIAG_FUNCTION_INVALID_INPUT. NEW: the engine's specific code per cause (owner Q193). -----------------
  ok = ExpectRefusal("SBSFC057-dearmor-invalid",
                     Run(registry, "sb.crypto.dearmor", {Arg("armor", TextValue("character", "not valid armor"))}),
                     SblrStatusCode::execution_failed, "CRYPTO.ARMOR.INVALID_INPUT") && ok;
  ok = ExpectRefusal("SBSFC057-hmac-bad-algo",
                     Run(registry, "sb.crypto.hmac_value_key_algo",
                         {Arg("value", BinaryValue(Bytes("abc"))), Arg("key", BinaryValue(Bytes("key"))), Arg("algorithm", TextValue("character", "unknown"))}),
                     SblrStatusCode::execution_failed, "CRYPTO.HMAC.UNSUPPORTED_ALGORITHM") && ok;
  ok = ExpectRefusal("SBSFC057-gen-random-bytes-too-large",
                     Run(registry, "sb.crypto.gen_random_bytes_n", {Arg("n", UintValue("uint32", 2048))}),
                     SblrStatusCode::execution_failed, "CRYPTO.RNG.INVALID_LENGTH") && ok;

  // ---- the projection route (query.evaluate_projection) -----------------------------------------------------------------------------------------------------
  // OLD: a character argument expected a character hex digest. NEW: a binary argument projects a binary value (native bytes, no text mirror).
  {
    auto projection = ProjectionEnvelope("sb.crypto.sha3_256", "binary", {api::EngineProjectionFunctionArgument{"value", "binary", "", false}}, {{0, Bytes("abc")}});
    ok = ExpectProjectionBinary("SBSFC057-sha3-256-projection-route",
                                sblr::DispatchSblrOperation({ProjectionContext(), projection, api::EngineApiRequest{}}),
                                "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532") && ok;
  }
  {
    const std::string plain = "plain-sentinel-57", key = "key-sentinel-57";
    const struct { const char* id; const char* name; const char* result; std::vector<api::EngineProjectionFunctionArgument> args;
                   std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> binary; } shapes[] = {
        {"sb.crypto.pgp_sym_encrypt", "pgp_sym_encrypt", "binary", {{"message", "text", plain, false}, {"password", "text", key, false}}, {}},
        {"sb.crypto.pgp_sym_decrypt", "pgp_sym_decrypt", "character", {{"message", "binary", "", false}, {"password", "text", key, false}}, {{0, Bytes(plain)}}},
        {"sb.crypto.pgp_pub_encrypt", "pgp_pub_encrypt", "binary", {{"message", "binary", "", false}, {"key", "binary", "", false}}, {{0, Bytes(plain)}, {1, Bytes(key)}}},
        {"sb.crypto.pgp_pub_decrypt", "pgp_pub_decrypt", "binary", {{"message", "binary", "", false}, {"key", "binary", "", false}}, {{0, Bytes(plain)}, {1, Bytes(key)}}},
    };
    for (const auto& shape : shapes) {
      auto envelope = ProjectionEnvelope(shape.id, shape.result, shape.args, shape.binary);
      ok = ExpectProjectionPgpRefusal(std::string("SBSFC057-projection-route-refuses ") + shape.name,
                                      sblr::DispatchSblrOperation({ProjectionContext(), envelope, api::EngineApiRequest{}}), shape.name) && ok;
    }
  }

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_057_crypto_hash_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
