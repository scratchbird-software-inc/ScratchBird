// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// S2 (plan 31, owner Q92/Q93/Q95/Q97/Q100 = 1, 2026-11-13):
// SB_ENGINE_CREATE_DATABASE_NATIVE. CREATE DATABASE through the production
// engine API (EngineCreateLifecycle) in the native single-file format, at
// every native page profile (8K-128K):
//  - the call creates exactly one file (the database) and nothing beside it;
//  - the result returns the engine's receipt identities, and they are the
//    identities the file carries on page zero;
//  - the language is the catalog_language constant: a missing or unknown one
//    is refused before any file exists, as is the 4K profile (Q100) and any
//    size with no native profile;
//  - no management transaction is required (Q95); the legacy default still
//    requires one and still writes the legacy format;
//  - through SBLR dispatch, the v2 create descriptor selects the format and
//    carries the name and language; its client identity fields are ignored
//    (SB_DRIFT_CREATE_DESCRIPTOR_CLIENT_IDENTITIES); v1 is unchanged.
// The name row in the given language and the read-only open verification of
// the created file in a fresh process are checked by the native enrollment
// test's producer-api mode, which runs this binary's `create` mode as its
// creating process (native_local_service_enrollment_test.cpp).
//
// Owner rulings applied 2026-11-14 (marked in the checks):
//  - Q111 = 1 (SB_ONE_DATABASE_UUID): one database identity; the receipt no
//    longer returns a separate database_object_uuid (nine identities). The
//    cold-reopened file is checked for it in the enrollment test.
//  - Q107 (SB_STANDARD_LOCAL_CLOCK_UUIDV7): the identities are standard RFC
//    9562 UUIDv7 from the local clock: version 7, variant 10, timestamps
//    inside the create's own wall-clock bracket.
//  - Q104 = 2 (SB_SERVER_POLICY_SEED_PACK_ROOT): the policy pack root is the
//    server's (the context's server_policy_seed_pack_root); a client option
//    naming it is refused, and without the server's value create is refused.
//  - Q108 = 1 (SB_NATIVE_CREATE_EMPTY_FILE_UNLINK): a refusal after the
//    O_EXCL open (bad fingerprint, bad policy pack, too few pages) leaves no
//    file, and a retry on the same path succeeds; the removal never takes a
//    non-empty file or a path that names another file. (Q122: the non-UTF-8
//    name case moved before the open; see below.)
//  - Q122 = 1 (SB_CATALOG_IDENTIFIER_TEXT_RULE): a name the catalog's name
//    rule refuses (U+0000, ill-formed or non-canonical UTF-8) is refused
//    before the file is opened: DATABASE.CREATE_NAME_INVALID, no post-open
//    detail, no file. The q122-* directories are what the strace check
//    watches: no openat of a path in them may appear.
//  - Q128 = 1 (SB_CATALOG_IDENTIFIER_TEXT_RULE in the v2 codec): the v2
//    create descriptor codec applies the same rule at encode and decode, so
//    the gateway and SBLR dispatch refuse such a name before the engine API.
//    Engine API calls still meet the engine's own refusal (defence in depth).
//  - Q127 = 3 (SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE): the name's maximum
//    length is the server pack's identifier profile's (default pack: 1024
//    encoded octets), checked before the open; the engine's hand-written
//    1024-byte cap is removed. The pack is read before the open, so a pack
//    that does not load is now a pre-open refusal (was a Q108 late case).
//  - Q133 = 1 (SB_NATIVE_CREATE_PRINCIPAL_NAMES_UNDER_PACK_PROFILE): the two
//    principal names are checked explicitly against the pack's profile before
//    the plan: a too-long one is always DATABASE.CREATE_NAME_INVALID.
//  - Q134 = 1 (SB_NATIVE_CREATE_WRITE_CONSUMES_PLAN): the create assembles
//    once, before the O_EXCL open, and the write consumes the plan. Every
//    assembly refusal (the Q108 bad-fingerprint and too-few-pages cases
//    included) is now before the open: no file is created at all. Q108's
//    removal remains for the write's own failures; a write that fails before
//    its first byte (RLIMIT_FSIZE 0) still leaves no file.
//  - Q135 = 1: the 128-byte principal-name cap is gone on the native path;
//    the pack's profile decides (default: 512 scalars).
//  - Q139 = 1: the default pack's profile is 512 unicode scalar values (was
//    1024 encoded octets).
//
// Modes: none (every check) or
//   create <path> <page size> <language: en|fr-CA>
// which performs one native create through the API and prints the receipt
// identities as `key=<32 hex>` lines; the strace measurement and the
// enrollment test's cold reopen run it.
#include "lifecycle/engine_lifecycle_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_opcode_stream.hpp"
#include "server/sblr_local_gateway.hpp"
#include "hash_digest.hpp"
#include "native_database_create.hpp"
#include "filespace_page_zero.hpp"
#include "filespace_bootstrap.hpp"
#include "catalog_language.hpp"
#include "catalog_identifier_text_rule.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_native_catalog_index.hpp"
#include "catalog_page.hpp"
#include "native_index_btree_page.hpp"
#include "physical_mga_cow_store.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <csignal>
#include <sys/resource.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace u = scratchbird::core::uuid;

int checks = 0;
void Check(bool ok, const char* what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what, at.line());
    std::exit(1);
  }
}

// The enrollment fixture's service verifier (native_runtime_principal_credential_test.cpp),
// so the producer-api cold reopen can authenticate the L5 service it creates.
constexpr std::string_view kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:salt=0123456789abcdef0123456789abcdef:"
    "verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";

std::array<std::uint8_t, 16> Fixed(std::uint8_t n) {
  std::array<std::uint8_t, 16> id{};
  id[0] = 1; id[6] = 0x70; id[8] = 0x80; id[15] = n;
  return id;
}

std::uint64_t TotalPages(std::uint32_t page_size) { return page_size == 8192 ? 2048 : 512; }

api::EngineRequestContext Context(const std::string& path) {
  api::EngineRequestContext context;
  context.request_id = "s2-native-create";
  context.database_path = path;
  // Q95: the caller's server session authorises CREATE DATABASE.
  context.security_context_present = true;
  context.statement_uuid = api::EngineUuid{Fixed(0x31)};
  // Q104 = 2: the hosting server fills its own configured policy pack root;
  // this test stands in for that server.
  context.server_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  return context;
}

void NativeOptions(std::vector<std::string>& options, std::uint32_t page_size, std::uint64_t total_pages) {
  options.push_back("page_size:" + std::to_string(page_size));
  options.push_back("total_pages:" + std::to_string(total_pages));
  // Q104 = 2: no policy_seed_pack_root option; the server's context carries it.
  options.push_back("bootstrap_principal_name:BootstrapAdmin");
  options.push_back("bootstrap_credential_fingerprint:" + std::string(kFingerprint));
  options.push_back("internal_service_principal_name:NodeService");
  options.push_back("internal_service_credential_fingerprint:" + std::string(kFingerprint));
}

api::EngineCreateLifecycleRequest NativeRequest(const std::string& path, std::uint32_t page_size,
                                                const p::Uuid& language, std::string name = "orders") {
  api::EngineCreateLifecycleRequest request;
  request.context = Context(path);
  request.database_format = api::EngineDatabaseFormat::native;
  request.database_name = std::move(name);
  request.database_language = language;
  NativeOptions(request.option_envelopes, page_size, TotalPages(page_size));
  return request;
}

std::optional<p::Uuid> RowUuid(const api::EngineApiResult& result, std::string_view name) {
  for (const auto& row : result.result_shape.rows)
    for (const auto& field : row.fields)
      if (field.first == name) {
        if (field.second.isSqlNull() || !field.second.encoded_value.empty() ||
            field.second.binary_value.size() != 16) return std::nullopt;
        p::Uuid id;
        std::copy(field.second.binary_value.begin(), field.second.binary_value.end(), id.bytes.begin());
        return id;
      }
  return std::nullopt;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) if (diagnostic.code == code) return true;
  return false;
}

std::vector<std::string> Entries(const std::filesystem::path& dir) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) names.push_back(entry.path().filename().string());
  return names;
}

// Q111: nine identities; database_object_uuid is gone (one database UUID).
constexpr std::array<const char*, 9> kReceiptKeys{
    "database_uuid", "filespace_uuid", "construction_transaction_uuid",
    "bootstrap_principal_uuid", "service_principal_uuid", "service_credential_reference_uuid",
    "service_provider_uuid", "service_policy_uuid", "service_runtime_binding_uuid"};

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

// Q107: a standard RFC 9562 UUIDv7 (version 7, variant 10) whose 48-bit Unix
// millisecond timestamp lies inside [from, to] of the local wall clock.
bool StandardV7Within(const p::Uuid& id, std::uint64_t from, std::uint64_t to) {
  std::uint64_t ms = 0;
  for (int i = 0; i < 6; ++i) ms = (ms << 8) | id.bytes[i];
  return (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80 && ms >= from && ms <= to;
}

bool DetailHas(const api::EngineApiResult& result, std::string_view text) {
  for (const auto& diagnostic : result.diagnostics)
    if (diagnostic.detail.find(text) != std::string::npos) return true;
  return false;
}

std::string Hex(const p::Uuid& id) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const auto b : id.bytes) { out.push_back(digits[b >> 4]); out.push_back(digits[b & 15]); }
  return out;
}

// One native create through the API; the receipt on stdout. Exit 0 only when
// the API returned every receipt identity.
int CreateMode(const std::string& path, const std::string& page_size_text, const std::string& language_text) {
  const auto language = c::CatalogLanguageForTag(language_text);  // test-side choice of constant
  if (!language) return 2;
  const auto page_size = static_cast<std::uint32_t>(std::stoul(page_size_text));
  const auto created = api::EngineCreateLifecycle(NativeRequest(path, page_size, *language,
                                                                "enrolled_native_node"));
  if (!created.ok) {
    for (const auto& diagnostic : created.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    return 1;
  }
  for (const auto* key : kReceiptKeys) {
    const auto id = RowUuid(created, key);
    if (!id || !u::IsEngineIdentityUuid(*id)) return 1;
    std::cout << key << '=' << Hex(*id) << '\n';
  }
  return 0;
}

sblr::SblrOperationEnvelope CreateEnvelope(sblr::SblrOperand operand) {
  const auto* entry = sblr::LookupSblrOpcode("SBLR_LIFECYCLE_CREATE_DATABASE");
  Check(entry != nullptr, "the create opcode is registered");
  auto envelope = sblr::MakeSblrEnvelope(entry->operation_id, "SBLR_LIFECYCLE_CREATE_DATABASE",
                                         "trace.s2.native_create");
  envelope.opcode_code = entry->code;
  envelope.parser_package_uuid = api::EngineUuid{Fixed(0x41)};
  envelope.registry_snapshot_uuid = api::EngineUuid{Fixed(0x42)};
  envelope.requires_security_context = entry->requires_security_context;
  envelope.requires_transaction_context = entry->requires_transaction_context;
  envelope.requires_cluster_authority = entry->requires_cluster_authority;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

template <class Descriptor>
void FillCommon(Descriptor& descriptor) {
  descriptor.operation_uuid = Fixed(0x50);
  descriptor.statement_receipt_uuid = Fixed(0x51);
  descriptor.owner_uuid = Fixed(0x54);
  descriptor.catalog_snapshot_uuid = Fixed(0x55);
  descriptor.catalog_generation = 71;
  descriptor.security_context_uuid = Fixed(0x56);
  descriptor.policy_snapshot_uuid = Fixed(0x57);
  descriptor.policy_generation = 72;
  descriptor.transaction_uuid = Fixed(0x58);
  descriptor.transaction_generation = 73;
  descriptor.durability_profile_uuid = Fixed(0x59);
  descriptor.resource_budget_uuid = Fixed(0x5a);
  descriptor.resource_budget_generation = 74;
  descriptor.executor_availability_generation = 1;
  descriptor.descriptor_evidence_uuid = Fixed(0x5b);
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(descriptor.option_vector);
  Check(hash.ok(), "option hash");
  descriptor.option_vector_sha256 = hash.digest;
}

sblr::SblrLifecycleCreateDatabaseDescriptorV2 V2(const p::Uuid& language, std::string name,
                                                 sblr::SblrLifecycleCreateDatabaseFormat format) {
  sblr::SblrLifecycleCreateDatabaseDescriptorV2 descriptor;
  FillCommon(descriptor);
  descriptor.database_language_uuid = language.bytes;
  descriptor.database_name = std::move(name);
  descriptor.database_format = format;
  return descriptor;
}

// Q128 = 1 (owner 2026-11-14): the encoder now refuses a name the catalog's
// name rule refuses, so such bytes cannot come from the codec. They are made
// from a valid encoding of `with_placeholder` by overwriting the name byte at
// `at` with `byte` (the length and option hash are unchanged; the name is not
// covered by any hash), to test the decoder and the gateway on them.
std::vector<std::uint8_t> PatchedNameBytes(const std::string& with_placeholder, std::size_t at, std::uint8_t byte) {
  auto bytes = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(
      V2(c::kCatalogLanguage_en, with_placeholder, sblr::SblrLifecycleCreateDatabaseFormat::native));
  Check(!bytes.empty() && at < with_placeholder.size(), "Q128: the placeholder name encodes");
  bytes[352 + at] = byte;
  return bytes;
}

// Q127: recompute a copied pack's content hashes the way the loader checks
// them (as native_policy_families_test's Rehash does; the pack's files are
// LF-only, so canonicalization is the identity).
std::string Sha256Hex(const std::string& text) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const p::byte*>(text.data()), text.size());
  Check(digest.ok(), "sha256");
  return scratchbird::core::hash::HexLower(digest.digest);
}
std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
void RehashPack(const std::filesystem::path& root) {
  const auto manifest_path = root / "POLICY_PACK_MANIFEST.json";
  const auto manifest = ReadAll(manifest_path);
  const std::regex entry("\"path\": \"([^\"]+)\",\\s*\"sha256\": \"([0-9a-f]{64})\"");
  std::string rebuilt, aggregate;
  std::size_t from = 0;
  for (auto it = std::sregex_iterator(manifest.begin(), manifest.end(), entry); it != std::sregex_iterator(); ++it) {
    const auto& m = *it;
    const auto sha = Sha256Hex(ReadAll(root / m[1].str()));
    rebuilt += manifest.substr(from, m.position(2) - from) + sha;
    from = m.position(2) + 64;
    aggregate += m[1].str() + std::string(1, '\0') + sha + "\n";
  }
  rebuilt += manifest.substr(from);
  rebuilt = std::regex_replace(rebuilt, std::regex("\"content_sha256\": \"[0-9a-f]{64}\""),
                               "\"content_sha256\": \"" + Sha256Hex(aggregate) + "\"",
                               std::regex_constants::format_first_only);
  std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
  out << rebuilt;
  Check(bool(out), "Q127: the rehashed manifest is written");
}

// Q129/Q130: a rehashed copy of the default pack whose identifier profile's
// maximum_identifier_length is `maximum` (the Q127 128-octet copy is made the
// same way, inline, below).
std::filesystem::path PackWithMaximum(const std::filesystem::path& under, const std::string& label,
                                      unsigned maximum) {
  const auto pack = under / label;
  std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT, pack, std::filesystem::copy_options::recursive);
  const auto naming = pack / "policies" / "native_naming_profiles.json";
  auto text = ReadAll(naming);
  // Q139 = 1 (owner 2026-11-14): the default pack's value is 512 (unicode
  // scalar values); the copy keeps that unit. Was: "...: 1024" (octets).
  const std::string from = "\"maximum_identifier_length\": 512";
  const auto at = text.find(from);
  Check(at != std::string::npos && text.find(from, at + 1) == std::string::npos,
        "Q129/Q130/Q139: the default pack declares maximum_identifier_length 512 exactly once");
  text.replace(at, from.size(), "\"maximum_identifier_length\": " + std::to_string(maximum));
  { std::ofstream out(naming, std::ios::binary | std::ios::trunc); out << text; }
  RehashPack(pack);
  return pack;
}

void DescriptorCodec() {
  // v1 unchanged: still 320 bytes and still requires its (drift) identities.
  sblr::SblrLifecycleCreateDatabaseDescriptorV1 v1;
  v1.database_uuid = Fixed(0x52);
  v1.database_name_uuid = Fixed(0x53);
  FillCommon(v1);
  const auto v1_bytes = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV1(v1);
  Check(v1_bytes.size() == 320 && sblr::SblrLifecycleCreateDatabaseDescriptorVersion(v1_bytes.data(), v1_bytes.size()) == 1,
        "S2: v1 encodes as before (320 bytes, version 1)");
  auto no_identity = v1;
  no_identity.database_uuid = {};
  Check(sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV1(no_identity).empty(),
        "S2: v1 is unchanged: it still refuses a missing database_uuid");
  sblr::SblrLifecycleCreateDatabaseDescriptorV2 decoded_v2;
  std::string detail;
  Check(!sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(v1_bytes.data(), v1_bytes.size(), &decoded_v2, &detail),
        "S2: a v1 body is not a v2 descriptor");

  // v2: name and language as data; identities not required; round trip.
  auto v2 = V2(c::kCatalogLanguage_fr_CA, "commandes", sblr::SblrLifecycleCreateDatabaseFormat::native);
  const auto v2_bytes = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(v2);
  Check(v2_bytes.size() == 352 + 9 && sblr::SblrLifecycleCreateDatabaseDescriptorVersion(v2_bytes.data(), v2_bytes.size()) == 2,
        "S2: v2 encodes with no client identities (352 + name bytes)");
  Check(sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(v2_bytes.data(), v2_bytes.size(), &decoded_v2, &detail) &&
            decoded_v2.database_name == "commandes" &&
            decoded_v2.database_language_uuid == c::kCatalogLanguage_fr_CA.bytes &&
            decoded_v2.database_format == sblr::SblrLifecycleCreateDatabaseFormat::native,
        "S2: v2 round trips the name, language and format");
  sblr::SblrLifecycleCreateDatabaseDescriptorV1 decoded_v1;
  Check(!sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV1(v2_bytes.data(), v2_bytes.size(), &decoded_v1, &detail),
        "S2: the v1 decoder refuses a v2 body (v1 parsing unchanged)");
  const auto refuses = [&](auto edit, const char* what) {
    auto changed = V2(c::kCatalogLanguage_en, "orders", sblr::SblrLifecycleCreateDatabaseFormat::native);
    edit(changed);
    Check(sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(changed).empty(), what);
  };
  refuses([](auto& x) { x.database_name.clear(); }, "S2: v2 refuses an empty name");
  // Q129 = 3 (owner 2026-11-14, SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE):
  // the codec has no name cap of its own; the pack's identifier profile sets
  // the name's maximum and the engine applies it (the Q129 block in main).
  // The codec's one bound is the catalog text maximum, 131040 bytes.
  // Was: refuses(1025 x 'a', "S2: v2 refuses a name over 1024 bytes").
  {
    auto long_name = V2(c::kCatalogLanguage_en, std::string(1025, 'a'), sblr::SblrLifecycleCreateDatabaseFormat::native);
    const auto bytes = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(long_name);
    sblr::SblrLifecycleCreateDatabaseDescriptorV2 back;
    Check(bytes.size() == 352 + 1025 &&
              sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(bytes.data(), bytes.size(), &back, &detail) &&
              back.database_name == long_name.database_name,
          "Q129: v2 encodes and decodes a 1025-byte name (was refused: the codec's own 1024 cap is gone)");
    long_name.database_name.assign(c::kCatalogIdentifierTextMaximumBytes, 'a');
    const auto at_max = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(long_name);
    Check(at_max.size() == 352 + c::kCatalogIdentifierTextMaximumBytes &&
              sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(at_max.data(), at_max.size(), &back, &detail),
          "Q129: v2 encodes and decodes a name at the catalog text maximum (131040 bytes)");
    refuses([](auto& x) { x.database_name.assign(c::kCatalogIdentifierTextMaximumBytes + 1, 'a'); },
            "Q129: v2 still refuses a name over the catalog text maximum (131041 bytes): the codec's one name bound");
    // Decode refuses a declared length over the catalog maximum (the bytes are
    // the 131040 encoding with its name length patched to 131041 and one
    // byte appended, so the size field agrees).
    auto over = at_max;
    over.push_back('a');
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
      for (int i = 0; i < 4; ++i) over[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    std::uint32_t le_size = 0;
    for (int i = 3; i >= 0; --i) le_size = (le_size << 8) | at_max[8 + i];
    Check(le_size == at_max.size(), "Q129: the descriptor size field is little-endian at offset 8");
    put32(8, static_cast<std::uint32_t>(over.size()));
    put32(336, static_cast<std::uint32_t>(c::kCatalogIdentifierTextMaximumBytes + 1));
    Check(!sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(over.data(), over.size(), &back, &detail),
          "Q129: v2 decode refuses a name length over the catalog text maximum");
  }
  refuses([](auto& x) { x.database_name = std::string("bad\xc3", 4); }, "S2: v2 refuses a name that is not UTF-8");
  // Q128 = 1 (owner 2026-11-14, SB_CATALOG_IDENTIFIER_TEXT_RULE): the codec
  // applies the catalog's name rule. New refusals (were admitted: the codec's
  // own UTF-8 check let U+0000 through).
  refuses([](auto& x) { x.database_name = std::string("a\0b", 3); }, "Q128: v2 encode refuses a U+0000 in the name");
  refuses([](auto& x) { x.database_name = std::string("\0", 1); }, "Q128: v2 encode refuses a name that is only U+0000");
  refuses([](auto& x) { x.database_name = std::string("a\xc0\x80", 3); },
          "Q128: v2 encode refuses an overlong encoding of U+0000");
  {
    const auto nul = PatchedNameBytes("aXb", 1, 0);
    Check(!sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(nul.data(), nul.size(), &decoded_v2, &detail) &&
              detail == "SCDD v2 database name is refused by the catalog name rule",
          "Q128: v2 decode refuses a U+0000 in the name, with the catalog name rule's own detail");
    const auto truncated = PatchedNameBytes("badX", 3, 0xc3);
    Check(!sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(truncated.data(), truncated.size(), &decoded_v2, &detail) &&
              detail == "SCDD v2 database name is refused by the catalog name rule",
          "Q128: v2 decode refuses a truncated UTF-8 sequence in the name");
    const auto good = PatchedNameBytes("aXb", 1, 'Y');
    Check(sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(good.data(), good.size(), &decoded_v2, &detail) &&
              decoded_v2.database_name == "aYb",
          "Q128 control: the same patch with an admitted byte decodes (the patch method itself is sound)");
  }
  refuses([](auto& x) { x.database_language_uuid = {}; }, "S2: v2 refuses a missing language");
  refuses([](auto& x) { x.database_format = static_cast<sblr::SblrLifecycleCreateDatabaseFormat>(3); },
          "S2: v2 refuses an unknown format");
  auto tampered = v2_bytes;
  tampered[341] = 1;
  Check(!sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(tampered.data(), tampered.size(), &decoded_v2, &detail),
        "S2: v2 refuses a nonzero reserved byte");
  tampered = v2_bytes;
  tampered.pop_back();
  Check(!sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(tampered.data(), tampered.size(), &decoded_v2, &detail),
        "S2: v2 refuses truncation");
}

// S2: the server's local gateway admits the v2 create descriptor and keeps
// admitting v1 exactly as before; a malformed or mislabelled body is refused.
scratchbird::server::LocalSblrGatewayDecision Gateway(sblr::SblrOperand operand) {
  const auto frame = [](bool begin) {
    auto operation = sblr::MakeSblrEnvelope(begin ? "engine.op.package_begin" : "engine.op.package_end",
                                            begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END",
                                            begin ? "s2.package.begin" : "s2.package.end");
    operation.opcode_code = begin ? 1 : 2;
    operation.result_shape = "void";
    operation.diagnostic_shape = "diagnostic_vector";
    operation.parser_package_uuid = api::EngineUuid{Fixed(0x41)};
    operation.registry_snapshot_uuid = api::EngineUuid{Fixed(0x42)};
    operation.parser_resolved_names_to_uuids = true;
    sblr::SblrOperand header;
    header.ordinal = 1;
    header.type = begin ? "package.header" : "package.footer";
    header.name = "package_descriptor";
    header.value_kind = sblr::SblrValueKind::descriptor_ref;
    const auto id = Fixed(0x43);
    header.value_body.assign(id.begin(), id.end());
    operation.operands.push_back(std::move(header));
    return operation;
  };
  auto member = CreateEnvelope(std::move(operand));
  member.result_shape = "lifecycle_create_database_result";
  member.diagnostic_shape = "diagnostic_vector";
  sblr::SblrOpcodeStream package;
  package.package_descriptor_uuid = api::EngineUuid{Fixed(0x43)};
  package.registry_snapshot_uuid = api::EngineUuid{Fixed(0x42)};
  const auto opcode = member.opcode_code;
  package.operations = {frame(true), std::move(member), frame(false)};
  scratchbird::server::LocalSblrGatewayRequest request;
  request.canonical_sbos = sblr::EncodeSblrOpcodeStream(package);
  request.root_opcode_code = static_cast<std::uint16_t>(opcode);
  request.root_opcode = "SBLR_LIFECYCLE_CREATE_DATABASE";
  request.root_operation_id = "engine.op.lifecycle_create_database";
  request.route_snapshot_uuid = api::EngineUuid{Fixed(0x44)};
  request.route_epoch = 7;
  request.route_generation = 9;
  request.security_snapshot_uuid = api::EngineUuid{Fixed(0x45)};
  request.security_epoch = 11;
  request.security_observation_generation = 13;
  request.route_snapshot_engine_owned = true;
  request.security_snapshot_engine_owned = true;
  return scratchbird::server::AdmitLocalNoClusterSblrGateway(request);
}

sblr::SblrOperand CreateOperand(std::vector<std::uint8_t> body, std::string type) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = "database";
  operand.ordinal = 1;
  operand.value_kind = sblr::SblrValueKind::lifecycle_create_database_descriptor;
  operand.value_body = std::move(body);
  return operand;
}

void GatewayAdmission() {
  sblr::SblrLifecycleCreateDatabaseDescriptorV1 v1;
  v1.database_uuid = Fixed(0x52);
  v1.database_name_uuid = Fixed(0x53);
  FillCommon(v1);
  const auto v1_body = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV1(v1);
  const auto v2_body = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(
      V2(c::kCatalogLanguage_en, "orders", sblr::SblrLifecycleCreateDatabaseFormat::native));
  const auto old_version = Gateway(CreateOperand(v1_body, "lifecycle_create_database_descriptor.v1"));
  const auto new_version = Gateway(CreateOperand(v2_body, "lifecycle_create_database_descriptor.v2"));
  if (!old_version.ok || !new_version.ok)
    std::cerr << "gateway v1=" << old_version.diagnostic_id << " v2=" << new_version.diagnostic_id << '\n';
  Check(old_version.ok, "S2: the gateway still admits the v1 create descriptor");
  Check(new_version.ok && new_version.disposition == old_version.disposition,
        "S2: the gateway admits the v2 create descriptor exactly as it admits v1");
  auto truncated = v2_body;
  truncated.pop_back();
  const auto refused = Gateway(CreateOperand(truncated, "lifecycle_create_database_descriptor.v2"));
  Check(!refused.ok, "S2: the gateway refuses a malformed v2 descriptor");
  const auto mislabelled = Gateway(CreateOperand(v2_body, "lifecycle_create_database_descriptor.v1"));
  Check(!mislabelled.ok, "S2: the gateway refuses a v2 body labelled v1");
  // Q128 = 1 (owner 2026-11-14): the gateway decodes the v2 descriptor, so a
  // name the catalog's rule refuses is refused at the gateway, before any
  // engine call. (Was: admitted here; refused by the engine.)
  const auto nul_name = Gateway(CreateOperand(PatchedNameBytes("aXb", 1, 0), "lifecycle_create_database_descriptor.v2"));
  std::fprintf(stderr, "Q128 gateway nul: ok=%d diagnostic=%s\n", int(nul_name.ok), nul_name.diagnostic_id.c_str());
  Check(!nul_name.ok && nul_name.diagnostic_id == "SBLR.OPERAND.INVALID",
        "Q128: the gateway refuses a v2 descriptor whose name holds U+0000 (SBLR.OPERAND.INVALID)");
  const auto patched_ok = Gateway(CreateOperand(PatchedNameBytes("aXb", 1, 'Y'), "lifecycle_create_database_descriptor.v2"));
  Check(patched_ok.ok, "Q128 control: the gateway admits the same patched descriptor with an admitted byte");
}

// Q110 (owner 2026-11-14, SB_CREATE_DESCRIPTOR_NOT_AN_OPTION): the database's
// name rows, read back in process from the created file through the production
// committed-catalog reader (the names relation, role 2, whose identity is the
// universal constant). The index binding is taken from the relation's own
// root page; nothing here comes from the create's in-memory workspace.
std::vector<c::CatalogNameEntry> DatabaseNameRows(const std::string& path, const p::Uuid& database,
                                                  const p::Uuid& filespace, std::uint32_t page_size) {
  namespace pg = scratchbird::storage::page;
  namespace db = scratchbird::storage::database;
  const auto* profile = d::FindCanonicalFilespacePageProfileForSize(page_size);
  Check(profile != nullptr, "Q110: the created file's page profile");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "Q110: the created file reopens natively");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok() && zero.record->bootstrap.database_uuid == database, "Q110: page zero names the returned database");
  const auto catalog_ref = std::find_if(zero.record->roots.begin(), zero.record->roots.end(),
                                        [](const auto& r) { return r.kind == 2; });
  Check(catalog_ref != zero.record->roots.end(), "Q110: page zero references the catalog root");
  const auto catalog = pg::ReadNativeCatalogRootFromOpenDevice(device, database, *catalog_ref);
  Check(catalog.ok(), "Q110: the catalog root reads");
  const auto names_root = std::find_if(catalog.root->roots.begin(), catalog.root->roots.end(),
                                       [](const auto& r) { return r.role == 2; });
  Check(names_root != catalog.root->roots.end() && names_root->page_type == 0x200,
        "Q110: the names relation is an indexed relation on the catalog root");
  std::vector<p::byte> page(page_size);
  Check(device.ReadAt(names_root->page.page_number * page_size, page.data(), page.size()).ok(),
        "Q110: the names relation's root page reads");
  const auto tree = pg::DecodeNativeBtreePage(page);
  Check(tree.ok(), "Q110: the names relation's root page decodes");
  const db::NativeCatalogRelationBinding binding{c::kNativeNameCatalogRelationUuid, tree.page->dependencies};
  const std::vector<d::NativeFilespaceDevice> devices{{filespace, profile->uuid, &device}};
  const auto read = db::ReadNativeCommittedCatalogVersionsFromOpenDevices(
      database, devices, db::NativeCatalogSource::PageZero(filespace), 2, 2, binding, 256u * 1024u * 1024u);
  Check(read.ok(), "Q110: the names relation reads back through the production reader");
  std::vector<c::CatalogNameEntry> out;
  for (const auto& row : read.rows) {
    if (!row.name_payload) continue;
    if (const auto* entry = std::get_if<c::CatalogNameEntry>(&*row.name_payload);
        entry && entry->object_uuid.value == database)
      out.push_back(*entry);
  }
  Check(device.Close().ok(), "close");
  return out;
}

// Q110: CREATE DATABASE through SBLR dispatch with the v2 descriptor, a name
// that carries option-shaped text, and the stated options (16K, 512 pages).
// The name is data: the create behaves as for a plain name, page zero has
// the stated page size and the file the stated pages, and the database's name
// row carries the name byte for byte.
sblr::SblrDispatchResult DispatchNamedCreate(const std::string& path, const std::string& name) {
  sblr::SblrOperand operand;
  operand.type = "lifecycle_create_database_descriptor.v2";
  operand.name = "database";
  operand.ordinal = 1;
  operand.value_kind = sblr::SblrValueKind::lifecycle_create_database_descriptor;
  operand.value_body = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(
      V2(c::kCatalogLanguage_en, name, sblr::SblrLifecycleCreateDatabaseFormat::native));
  Check(!operand.value_body.empty(), "Q110: the v2 descriptor encodes the option-shaped name as data");
  const auto decoded = sblr::DecodeSblrEnvelope(sblr::EncodeSblrEnvelope(CreateEnvelope(operand)));
  Check(decoded.ok, "Q110: the v2 create envelope round trips");
  api::EngineApiRequest options;
  NativeOptions(options.option_envelopes, 16384, TotalPages(16384));
  const sblr::SblrDispatchRequest dispatch{Context(path), decoded.envelope, options};
  return sblr::DispatchSblrOperation(dispatch);
}

void DispatchCreateWithName(const std::filesystem::path& root, const std::string& label, const std::string& name) {
  const auto dir = root / ("q110-" + label);
  std::filesystem::create_directory(dir);
  const auto path = (dir / "named.sdb").string();
  const auto result = DispatchNamedCreate(path, name);
  if (!result.api_result.ok) {
    for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& diagnostic : result.api_result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Check(result.envelope_validated && result.dispatched_to_api && result.api_result.ok,
        "Q110: a name carrying option-shaped text creates exactly as a plain name does");
  const auto database = RowUuid(result.api_result, "database_uuid");
  const auto filespace = RowUuid(result.api_result, "filespace_uuid");
  Check(database && filespace, "Q110: the receipt identities are returned");
  {
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
          "Q110: reopen");
    const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
    const auto size = device.Size();
    Check(zero.ok() && zero.record->bootstrap.page_size_bytes == 16384 && size.ok() &&
              size.size_bytes == TotalPages(16384) * 16384u,
          "Q110: the stated options decide the page size and pages; the name's text overrides nothing");
    Check(device.Close().ok(), "close");
  }
  const auto rows = DatabaseNameRows(path, *database, *filespace, 16384);
  Check(rows.size() == 1, "Q110: the database has one name row");
  Check(rows.front().raw_name_text == name && rows.front().raw_name_text.size() == name.size() &&
            rows.front().language_uuid.value == c::kCatalogLanguage_en,
        "Q110: the name row carries CREATE's name byte for byte, in the given language");
  std::printf("Q110 dispatch name %s: created as data, name row exact (%zu bytes)\n", label.c_str(), name.size());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string_view(argv[1]) == "create") return CreateMode(argv[2], argv[3], argv[4]);
  if (argc != 1) { std::cerr << "usage: (none) | create <path> <page size> <en|fr-CA>\n"; return 2; }

  DescriptorCodec();
  GatewayAdmission();
  scratchbird::tests::OwnedTempDirectory artifacts;

  // Refusals before any file exists.
  {
    const auto dir = artifacts.path() / "refusals";
    std::filesystem::create_directory(dir);
    const auto path = (dir / "refused").string();
    const auto refused = [&](api::EngineCreateLifecycleRequest request, const char* code, const char* what) {
      const auto result = api::EngineCreateLifecycle(request);
      if (result.ok || !HasDiagnostic(result, code))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, code), what);
      Check(Entries(dir).empty(), "S2: a refused native create leaves no file at all");
    };
    refused(NativeRequest(path, 16384, p::Uuid{}), "DATABASE.CREATE_LANGUAGE_UNKNOWN",
            "S2: a missing language is refused");
    refused(NativeRequest(path, 16384, api::EngineUuid{Fixed(0x77)}), "DATABASE.CREATE_LANGUAGE_UNKNOWN",
            "S2: a UUID that is no catalog_language constant is refused");
    refused(NativeRequest(path, 4096, c::kCatalogLanguage_en), "DATABASE.CREATE_PAGE_SIZE_UNSUPPORTED",
            "S2 (Q100): the 4K profile is refused on the native path");
    refused(NativeRequest(path, 12288, c::kCatalogLanguage_en), "DATABASE.CREATE_PAGE_SIZE_UNSUPPORTED",
            "S2: a page size with no native profile is refused");
    refused(NativeRequest(path, 16384, c::kCatalogLanguage_en, ""), "DATABASE.CREATE_NAME_INVALID",
            "S2: the name is required as data");
    auto no_pages = NativeRequest(path, 16384, c::kCatalogLanguage_en);
    std::erase_if(no_pages.option_envelopes, [](const auto& o) { return o.starts_with("total_pages:"); });
    refused(no_pages, "DATABASE.CREATE_TOTAL_PAGES_REQUIRED", "S2: total pages are stated, never defaulted");
    auto no_service = NativeRequest(path, 16384, c::kCatalogLanguage_en);
    std::erase_if(no_service.option_envelopes, [](const auto& o) { return o.starts_with("internal_service_principal_name:"); });
    refused(no_service, "DATABASE.CREATE_PRINCIPAL_REQUIRED", "S2: the L5 service principal is required");
    auto no_session = NativeRequest(path, 16384, c::kCatalogLanguage_en);
    no_session.context.security_context_present = false;
    const auto unauthorised = api::EngineCreateLifecycle(no_session);
    Check(!unauthorised.ok && Entries(dir).empty(), "S2 (Q95): without the caller's session, native create is refused");
    // Q104 = 2: the policy pack root is the server's configuration only.
    auto client_pack = NativeRequest(path, 16384, c::kCatalogLanguage_en);
    client_pack.option_envelopes.push_back(std::string("policy_seed_pack_root:") + SB_DEFAULT_POLICY_PACK_ROOT);
    refused(client_pack, "DATABASE.CREATE_POLICY_PACK_CLIENT_REFUSED",
            "Q104: a client option naming the policy pack is refused, even with the server's own value");
    auto client_other_pack = NativeRequest(path, 16384, c::kCatalogLanguage_en);
    client_other_pack.option_envelopes.push_back("policy_seed_pack_root:/client/chosen/pack");
    refused(client_other_pack, "DATABASE.CREATE_POLICY_PACK_CLIENT_REFUSED",
            "Q104: a client cannot substitute its own policy pack");
    auto no_server_pack = NativeRequest(path, 16384, c::kCatalogLanguage_en);
    no_server_pack.context.server_policy_seed_pack_root.clear();
    refused(no_server_pack, "DATABASE.CREATE_POLICY_PACK_REQUIRED",
            "Q104: without the server's configured policy pack, native create is refused");
  }

  // Q108 = 1: refusals AFTER our O_EXCL open leave no file, and a retry on
  // the same path succeeds.
  // Q134 = 1 (owner 2026-11-14, SB_NATIVE_CREATE_WRITE_CONSUMES_PLAN): both
  // cases below are assembly refusals, and the create now assembles once,
  // BEFORE the open: they are refused with no file ever created (no open of
  // their q122-q134-* directory; the strace check holds them to that), the
  // detail names the stage, and a retry on the same path succeeds.
  // (Was: late(...) here: DATABASE.CREATE_FAILED native_create_refused:<stage>
  // after the open, ;empty_file_removed, directories late-fingerprint and
  // late-pages.)
  {
    const auto before_open = [&](const char* label, auto edit, const char* stage, const char* what) {
      const auto dir = artifacts.path() / (std::string("q122-q134-") + label);
      std::filesystem::create_directory(dir);
      const auto path = (dir / "orders.sdb").string();
      auto request = NativeRequest(path, 16384, c::kCatalogLanguage_en);
      edit(request);
      const auto result = api::EngineCreateLifecycle(request);
      if (result.ok || !DetailHas(result, stage))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, "DATABASE.CREATE_FAILED") &&
                DetailHas(result, std::string("native_create_refused_before_open:") + stage) &&
                !DetailHas(result, "native_create_refused:") && !DetailHas(result, ";empty_file_removed") &&
                !DetailHas(result, ";empty_file_not_removed") && Entries(dir).empty(),
            what);
      std::printf("Q134 %s: refused before the open (%s), no file\n", label, stage);
      // The retry is a separate directory so the refusal's stays open-free.
      const auto retry_dir = artifacts.path() / (std::string("q134-retry-") + label);
      std::filesystem::create_directory(retry_dir);
      const auto retried = api::EngineCreateLifecycle(
          NativeRequest((retry_dir / "orders.sdb").string(), 16384, c::kCatalogLanguage_en));
      if (!retried.ok) for (const auto& diagnostic : retried.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(retried.ok && Entries(retry_dir).size() == 1, "Q134: the same request, corrected, creates");
    };
    before_open("fingerprint", [](auto& r) {
      std::erase_if(r.option_envelopes, [](const auto& o) { return o.starts_with("bootstrap_credential_fingerprint:"); });
      r.option_envelopes.push_back("bootstrap_credential_fingerprint:not-a-fingerprint");
    }, "invalid_principal",
       "Q134: a bad credential fingerprint is refused before the open (was: Q108, after the open, file removed)");
    // Q127 = 3 (owner 2026-11-14): the server's pack is now read before the
    // open (for its identifier profile), so a pack that does not load is a
    // pre-open refusal; see the Q127 block below. Was: late("policy-pack",
    // root = <artifacts>/no-such-policy-pack, "Q108: a bad (server) policy
    // pack is refused after the open").
    before_open("pages", [](auto& r) {
      std::erase_if(r.option_envelopes, [](const auto& o) { return o.starts_with("total_pages:"); });
      r.option_envelopes.push_back("total_pages:2");
    }, "capacity_exceeded",
       "Q134: too few pages is refused before the open (was: Q108, after the open, file removed)");
    // Q135 = 1: the principal-name character rule stays (first byte an ASCII
    // letter or '_'); a name that breaks it is an assembly refusal, so it too
    // is refused before the open.
    before_open("principal-characters", [](auto& r) {
      std::erase_if(r.option_envelopes, [](const auto& o) { return o.starts_with("bootstrap_principal_name:"); });
      r.option_envelopes.push_back("bootstrap_principal_name:9Admin");
    }, "invalid_principal",
       "Q135: a principal name that breaks the character rule is refused before the open");
    // Q122 = 1 (owner 2026-11-14): the non-UTF-8 name case is no longer a
    // late refusal; it is refused before the open (the Q122 block below).
    // Was: late("utf8", name "bad\xc3", "...is refused after the open").
  }

  // Q134 = 1: what Q108 still covers. The only refusals left after the open
  // are the write's own: a device check or an I/O failure. A write that fails
  // before its first byte leaves our own empty file, which Q108 removes; a
  // retry succeeds. The failure is real, not simulated: the process file-size
  // limit (RLIMIT_FSIZE) is set to 0 for the one call, so the first pwrite
  // fails (EFBIG, SIGXFSZ ignored). A write that fails part-way leaves a
  // partial file, which Q108 never removes (it is not empty): recorded here
  // as the behaviour today (owner question in the Q134 report).
  {
    const auto with_file_limit = [&](rlim_t limit, auto call) {
      rlimit saved{};
      Check(getrlimit(RLIMIT_FSIZE, &saved) == 0, "Q134: the file-size limit is readable");
      const auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
      rlimit lowered = saved;
      lowered.rlim_cur = limit;
      Check(setrlimit(RLIMIT_FSIZE, &lowered) == 0, "Q134: the file-size limit is lowered for one call");
      auto result = call();
      Check(setrlimit(RLIMIT_FSIZE, &saved) == 0, "Q134: the file-size limit is restored");
      std::signal(SIGXFSZ, old_handler);
      return result;
    };
    {
      const auto dir = artifacts.path() / "q134-write-fails-first-byte";
      std::filesystem::create_directory(dir);
      const auto path = (dir / "orders.sdb").string();
      const auto result = with_file_limit(0, [&] {
        return api::EngineCreateLifecycle(NativeRequest(path, 16384, c::kCatalogLanguage_en));
      });
      if (result.ok || !DetailHas(result, ";empty_file_removed"))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, "DATABASE.CREATE_FAILED") &&
                DetailHas(result, "native_create_refused:workspace_failure:io_failure") &&
                DetailHas(result, ";empty_file_removed") && Entries(dir).empty(),
            "Q108/Q134: a write that fails before its first byte (after the open) removes our own empty file");
      const auto retried = api::EngineCreateLifecycle(NativeRequest(path, 16384, c::kCatalogLanguage_en));
      Check(retried.ok && Entries(dir).size() == 1, "Q108/Q134: a retry on the same path succeeds");
      std::printf("Q134 write fails at its first byte: empty file removed, retry ok\n");
    }
    {
      const auto dir = artifacts.path() / "q134-write-fails-part-way";
      std::filesystem::create_directory(dir);
      const auto path = (dir / "orders.sdb").string();
      const auto result = with_file_limit(3 * 16384, [&] {
        return api::EngineCreateLifecycle(NativeRequest(path, 16384, c::kCatalogLanguage_en));
      });
      if (result.ok || !DetailHas(result, ";empty_file_not_removed"))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, "DATABASE.CREATE_FAILED") &&
                DetailHas(result, "native_create_refused:workspace_failure:io_failure") &&
                DetailHas(result, ";empty_file_not_removed") && Entries(dir).size() == 1 &&
                std::filesystem::file_size(dir / "orders.sdb") > 0,
            "Q108/Q134: a write that fails part-way leaves its partial file (not empty, never removed): today's behaviour");
      std::printf("Q134 write fails part-way: partial file of %llu bytes left (owner question)\n",
                  static_cast<unsigned long long>(std::filesystem::file_size(dir / "orders.sdb")));
    }
  }

  // Q122 = 1 (owner 2026-11-14, SB_CATALOG_IDENTIFIER_TEXT_RULE): every name
  // the catalog's name rule refuses is refused BEFORE the file is opened, by
  // the same rule function the name entry builder uses at assembly
  // (AdmitCatalogIdentifierText). Proof that no open happened: the refusal is
  // the pre-open code with none of the post-open details
  // (native_create_refused:, ;empty_file_removed / ;empty_file_not_removed),
  // and nothing exists in the directory; the strace run (strace_q122.sh)
  // shows no openat of any path in a q122-* directory.
  {
    const auto pre_open = [&](const char* label, std::string name, const char* what) {
      const auto dir = artifacts.path() / (std::string("q122-") + label);
      std::filesystem::create_directory(dir);
      const auto path = (dir / "named.sdb").string();
      const auto result = api::EngineCreateLifecycle(NativeRequest(path, 16384, c::kCatalogLanguage_en, std::move(name)));
      if (result.ok || !HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID"))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result, "database_name_refused_by_catalog_name_rule"), what);
      Check(!DetailHas(result, "native_create_refused:") && !DetailHas(result, ";empty_file_removed") &&
                !DetailHas(result, ";empty_file_not_removed") && !HasDiagnostic(result, "DATABASE.CREATE_FAILED"),
            "Q122: the refusal is the pre-open one; no post-open refusal or empty-file removal happened");
      Check(Entries(dir).empty() && !std::filesystem::exists(path), "Q122: the path never existed");
      std::printf("Q122 %s: refused before the open, no file\n", label);
    };
    pre_open("nul", std::string("a\0b", 3), "Q122: a U+0000 in the name is refused before the open");
    pre_open("nul-only", std::string("\0", 1), "Q122: a name that is only U+0000 is refused before the open");
    pre_open("utf8-truncated", std::string("bad\xc3", 4),
             "Q122: a truncated UTF-8 sequence is refused before the open (was a Q108 late refusal)");
    pre_open("utf8-lone-continuation", std::string("a\x80", 2),
             "Q122: a lone continuation byte is refused before the open");
    pre_open("utf8-overlong", std::string("a\xc0\x80", 3),
             "Q122: an overlong (non-canonical) encoding of U+0000 is refused before the open");
    pre_open("utf8-surrogate", std::string("a\xed\xa0\x80", 4),
             "Q122: an encoded UTF-16 surrogate is refused before the open");
    pre_open("utf8-above-max", std::string("a\xf4\x90\x80\x80", 5),
             "Q122: a scalar above U+10FFFF is refused before the open");
    // A name the rule admits is unaffected: multi-byte UTF-8 creates.
    {
      const auto dir = artifacts.path() / "q122-valid-multibyte";
      std::filesystem::create_directory(dir);
      const auto path = (dir / "named.sdb").string();
      const auto made = api::EngineCreateLifecycle(
          NativeRequest(path, 16384, c::kCatalogLanguage_en, "commandes_\xc3\xa9t\xc3\xa9_\xe6\x97\xa5"));
      if (!made.ok) for (const auto& diagnostic : made.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(made.ok && Entries(dir).size() == 1, "Q122: a valid multi-byte UTF-8 name still creates");
      std::printf("Q122 valid multi-byte name: created\n");
    }
  }

  // Q127 = 3 (owner 2026-11-14, SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE):
  // the database name's maximum length is the server pack's identifier
  // profile's (maximum_identifier_length, in its length_unit), checked before
  // the open; the engine's hand-written 1024-byte cap is gone. The default
  // pack declares 1024 encoded octets. Refusal directories are named q122-*
  // so the strace check holds them to "no open at all".
  {
    const auto attempt = [&](const std::string& label, std::string name, const std::string& root) {
      const auto dir = artifacts.path() / label;
      std::filesystem::create_directory(dir);
      auto request = NativeRequest((dir / "named.sdb").string(), 16384, c::kCatalogLanguage_en, std::move(name));
      if (!root.empty()) request.context.server_policy_seed_pack_root = root;
      auto result = api::EngineCreateLifecycle(request);
      return std::make_pair(std::move(result), dir);
    };
    const auto created = [&](const std::string& label, std::string name, const std::string& root, const char* what) {
      const auto [result, dir] = attempt(label, std::move(name), root);
      if (!result.ok) for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(result.ok && Entries(dir).size() == 1, what);
      std::printf("Q127 %s: created\n", label.c_str());
    };
    const auto too_long = [&](const std::string& label, std::string name, const std::string& root, const char* what) {
      const auto [result, dir] = attempt(label, std::move(name), root);
      if (result.ok || !DetailHas(result, "database_name_longer_than_policy_identifier_profile_maximum"))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result, "database_name_longer_than_policy_identifier_profile_maximum") &&
                !DetailHas(result, "native_create_refused:") && !HasDiagnostic(result, "DATABASE.CREATE_FAILED") &&
                Entries(dir).empty(), what);
      std::printf("Q127 %s: refused before the open, no file\n", label.c_str());
    };
    // Q139 = 1 (owner 2026-11-14): the default pack is 512 unicode scalar
    // values, counted on the unqualified name (the name row's text). Was:
    // 1024 encoded octets (Q127/Q131), under which the first case below
    // created and the next was the first refused.
    too_long("q122-q139-1024-ascii", std::string(1024, 'a'), {},
             "Q139: a 1024-scalar ASCII name is refused before the open (was: created, Q127, 1024 octets)");
    too_long("q122-q127-1025-octets", std::string(1025, 'a'), {},
             "Q127: a 1025-octet name is refused before the open by the pack's profile maximum "
             "(was: the engine's hand-written size() > 1024 cap, detail database_name_required_as_data)");
    created("q127-ok-q139-512-ascii", std::string(512, 'a'), {},
            "Q139: a 512-scalar ASCII name creates under the default pack (512 unicode scalar values)");
    too_long("q122-q139-513-ascii", std::string(513, 'a'), {},
             "Q139: a 513-scalar ASCII name is refused before the open");
    std::string e_acute_512, e_acute_513;
    for (int n = 0; n < 512; ++n) e_acute_512 += "\xc3\xa9";
    e_acute_513 = e_acute_512 + "\xc3\xa9";
    created("q127-ok-512-scalars-1024-octets", e_acute_512, {},
            "Q139: 512 two-octet scalars (1024 octets) create as 512 scalars (Q127: created as 1024 octets)");
    too_long("q122-q127-513-scalars-1026-octets", e_acute_513, {},
             "Q139: 513 two-octet scalars are refused as 513 scalars (Q127: refused as 1026 octets)");
    // Four-octet scalars: 512 of them are 2048 octets and create; the
    // profile counts scalars, not octets.
    std::string four_512, four_513;
    for (int n = 0; n < 512; ++n) four_512 += "\xf0\x9f\x90\xa6";
    four_513 = four_512 + "\xf0\x9f\x90\xa6";
    created("q127-ok-q139-512-four-octet-scalars", four_512, {},
            "Q139: 512 four-octet scalars (2048 octets) create: scalars are counted, not octets");
    too_long("q122-q139-513-four-octet-scalars", four_513, {},
             "Q139: 513 four-octet scalars (2052 octets) are refused before the open");

    // The value comes from the pack: a rehashed copy whose profile says 128.
    // (Not smaller: the maximum governs every name the create assembles under
    // that profile, and a pack at 8 was observed to refuse at policy
    // assembly, after the open, because bootstrap names are longer; the
    // longest name text in the default pack is 80 octets.)
    // Q130 = 2 (owner 2026-11-14): a pack at 8 is now refused BEFORE the
    // open, naming the class of name (the Q130 block below).
    const auto pack = artifacts.path() / "q127-pack-max-128";
    std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT, pack, std::filesystem::copy_options::recursive);
    {
      const auto naming = pack / "policies" / "native_naming_profiles.json";
      std::string text;
      { std::ifstream in(naming, std::ios::binary); text.assign(std::istreambuf_iterator<char>(in), {}); }
      // Q139 = 1: the default pack's value is 512 (was "...: 1024").
      const std::string from = "\"maximum_identifier_length\": 512", to = "\"maximum_identifier_length\": 128";
      const auto at = text.find(from);
      Check(at != std::string::npos && text.find(from, at + 1) == std::string::npos,
            "Q127/Q139: the default pack declares maximum_identifier_length 512 exactly once");
      text.replace(at, from.size(), to);
      { std::ofstream out(naming, std::ios::binary | std::ios::trunc); out << text; }
      RehashPack(pack);
    }
    created("q127-ok-pack-128-octets", std::string(128, 'p'), pack.string(),
            "Q127: with the pack's maximum at 128, a 128-octet name creates");
    too_long("q122-q127-pack-129-octets", std::string(129, 'p'), pack.string(),
             "Q127: with the pack's maximum at 128, a 129-octet name is refused before the open: the limit is the "
             "pack's");

    // A pack that does not load is refused before the open (was: after the
    // open, the Q108 late "policy-pack" case).
    {
      const auto [result, dir] = attempt("q122-q127-no-such-pack", "orders",
                                         (artifacts.path() / "no-such-policy-pack").string());
      if (result.ok || !HasDiagnostic(result, "DATABASE.CREATE_POLICY_PACK_INVALID"))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, "DATABASE.CREATE_POLICY_PACK_INVALID") &&
                !DetailHas(result, "native_create_refused:") && Entries(dir).empty(),
            "Q127: a server pack that does not load is refused before the open (was: Q108, after the open), no file");
      std::printf("Q127 unloadable pack: refused before the open, no file\n");
    }
  }

  // Q129 = 3 (owner 2026-11-14, SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE):
  // the v2 codec has no name cap of its own. A name over the default pack's
  // 1024 passes the codec and the server's gateway and is refused by the
  // engine, from the pack, before the open; a pack with a larger maximum
  // creates a longer name through the same codec, gateway and dispatch path.
  // (Was: the codec refused any name over 1024 bytes, kSblrCreateDatabaseName-
  // MaximumBytes, so neither case could reach the engine.)
  {
    const auto root = artifacts.path() / "q129";
    std::filesystem::create_directory(root);
    const auto through_codec = [&](const std::string& label, const std::string& name, const std::string& pack,
                                   std::filesystem::path* dir_out) {
      const auto body = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(
          V2(c::kCatalogLanguage_en, name, sblr::SblrLifecycleCreateDatabaseFormat::native));
      Check(body.size() == 352 + name.size(), "Q129: the v2 codec encodes the name (no codec cap)");
      const auto admitted = Gateway(CreateOperand(body, "lifecycle_create_database_descriptor.v2"));
      if (!admitted.ok) std::cerr << "Q129 gateway: " << admitted.diagnostic_id << '\n';
      Check(admitted.ok, "Q129: the server's gateway admits the descriptor (it decodes it with the same codec)");
      const auto decoded = sblr::DecodeSblrEnvelope(sblr::EncodeSblrEnvelope(
          CreateEnvelope(CreateOperand(body, "lifecycle_create_database_descriptor.v2"))));
      Check(decoded.ok, "Q129: the create envelope round trips");
      const auto dir = root / label;
      std::filesystem::create_directory(dir);
      auto context = Context((dir / "named.sdb").string());
      if (!pack.empty()) context.server_policy_seed_pack_root = pack;
      api::EngineApiRequest options;
      NativeOptions(options.option_envelopes, 16384, TotalPages(16384));
      *dir_out = dir;
      return sblr::DispatchSblrOperation({context, decoded.envelope, options});
    };
    {
      std::filesystem::path dir;
      // Q139 = 1: the default pack's maximum is 512 scalars, so 513 is the
      // first name over it. (Was: 1025 bytes against the pack's 1024 octets;
      // directory q122-q129-1025-default-pack.)
      const auto result = through_codec("q122-q129-513-default-pack", std::string(513, 'a'), {}, &dir);
      if (result.api_result.ok || !DetailHas(result.api_result, "database_name_longer_than_policy_identifier_profile_maximum"))
        for (const auto& diagnostic : result.api_result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(result.envelope_validated && result.dispatched_to_api && !result.api_result.ok &&
                HasDiagnostic(result.api_result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result.api_result, "database_name_longer_than_policy_identifier_profile_maximum") &&
                !DetailHas(result.api_result, "native_create_refused:") && Entries(dir).empty(),
            "Q129/Q139: a 513-scalar name passes the codec, the gateway and dispatch, and the engine refuses it from "
            "the default pack's profile (512 scalars) before the open, no file");
      std::printf("Q129 513 scalars, default pack: codec and gateway admit, engine refuses before the open, no file\n");
    }
    const auto pack_2048 = PackWithMaximum(artifacts.path(), "q129-pack-max-2048", 2048);
    {
      std::filesystem::path dir;
      const std::string name(2048, 'n');
      const auto result = through_codec("q129-ok-2048-pack-2048", name, pack_2048.string(), &dir);
      if (!result.api_result.ok) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& diagnostic : result.api_result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      }
      Check(result.envelope_validated && result.dispatched_to_api && result.api_result.ok && Entries(dir).size() == 1,
            "Q129: with the pack's maximum at 2048, a 2048-byte name creates through codec, gateway and dispatch");
      const auto database = RowUuid(result.api_result, "database_uuid");
      const auto filespace = RowUuid(result.api_result, "filespace_uuid");
      Check(database && filespace, "Q129: the receipt identities are returned");
      const auto rows = DatabaseNameRows((dir / "named.sdb").string(), *database, *filespace, 16384);
      Check(rows.size() == 1 && rows.front().raw_name_text == name,
            "Q129: the database's name row carries the 2048-byte name byte for byte");
      std::printf("Q129 2048 bytes, pack at 2048: created, name row exact\n");
    }
    {
      std::filesystem::path dir;
      const auto result = through_codec("q122-q129-2049-pack-2048", std::string(2049, 'n'), pack_2048.string(), &dir);
      Check(!result.api_result.ok && HasDiagnostic(result.api_result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result.api_result, "database_name_longer_than_policy_identifier_profile_maximum") &&
                Entries(dir).empty(),
            "Q129: with the pack's maximum at 2048, a 2049-byte name is refused before the open, no file");
      std::printf("Q129 2049 bytes, pack at 2048: refused before the open, no file\n");
    }
  }

  // Q130 = 2 (owner 2026-11-14, SB_NATIVE_CREATE_NAMES_UNDER_PACK_PROFILE):
  // every name native create assembles is held to the pack's identifier
  // profile before the open (the create's own assembly, run without a device,
  // PlanNativeDatabaseCreate). Refusal directories are q122-* so the strace
  // check holds them to no open at all.
  {
    const auto attempt = [&](const std::string& label, const std::filesystem::path& pack, auto edit) {
      const auto dir = artifacts.path() / label;
      std::filesystem::create_directory(dir);
      auto request = NativeRequest((dir / "named.sdb").string(), 16384, c::kCatalogLanguage_en, "orders");
      request.context.server_policy_seed_pack_root = pack.string();
      edit(request);
      auto result = api::EngineCreateLifecycle(request);
      if (result.ok || !result.diagnostics.empty())
        for (const auto& diagnostic : result.diagnostics) std::cerr << label << ' ' << diagnostic.code << ':' << diagnostic.detail << '\n';
      return std::make_pair(std::move(result), dir);
    };
    const auto replace = [](std::vector<std::string>& options, std::string_view key, std::string value) {
      std::erase_if(options, [&](const auto& o) { return o.starts_with(key); });
      options.push_back(std::string(key) + value);
    };
    const auto pre_open = [&](const api::EngineApiResult& result, const std::filesystem::path& dir) {
      return !result.ok && !DetailHas(result, "native_create_refused:") && !DetailHas(result, ";empty_file_removed") &&
             !DetailHas(result, ";empty_file_not_removed") && !HasDiagnostic(result, "DATABASE.CREATE_FAILED") &&
             Entries(dir).empty();
    };
    // A pack at 8: the database name "orders" (6) and short principal names
    // fit, but the pack's own names (UUID text, 36 octets; role and group
    // names) do not. Was (observed in the Q127 work, recorded in the Q127
    // block above): refused after the open at policy assembly,
    // DATABASE.CREATE_FAILED native_create_refused:policy_assembly, and Q108
    // removed the still-empty file.
    const auto pack_8 = PackWithMaximum(artifacts.path(), "q130-pack-max-8", 8);
    {
      const auto [result, dir] = attempt("q122-q130-pack-8", pack_8, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", "Boot");
        replace(r.option_envelopes, "internal_service_principal_name:", "Svc");
      });
      Check(HasDiagnostic(result, "DATABASE.CREATE_POLICY_PACK_INVALID") &&
                DetailHas(result, "native_create_name_longer_than_policy_identifier_profile_maximum:policy_cohort_name") &&
                pre_open(result, dir),
            "Q130: a pack at 8 is refused before the open (DATABASE.CREATE_POLICY_PACK_INVALID, the pack's own "
            "policy-cohort names), no file (was: refused after the open at policy_assembly, file removed)");
      Check(!DetailHas(result, "Boot") && !DetailHas(result, "orders"), "Q130: the refusal names a class, never name text");
      std::printf("Q130 pack at 8: refused before the open (policy_cohort_name), no file\n");
    }
    // A principal name longer than the profile allows, with a pack whose
    // maximum fits every pack and engine name (96 > the pack's longest, 80):
    // the caller's text, so a name refusal naming which principal.
    const auto pack_96 = PackWithMaximum(artifacts.path(), "q130-pack-max-96", 96);
    {
      const auto [result, dir] = attempt("q122-q130-bootstrap-principal-97", pack_96, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", std::string(97, 'b'));
      });
      Check(HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result, "bootstrap_principal_name_longer_than_policy_identifier_profile_maximum") &&
                pre_open(result, dir),
            "Q130: a 97-octet bootstrap principal name under a pack at 96 is refused before the open, no file");
      std::printf("Q130 bootstrap principal 97, pack at 96: refused before the open, no file\n");
    }
    {
      const auto [result, dir] = attempt("q122-q130-service-principal-97", pack_96, [&](auto& r) {
        replace(r.option_envelopes, "internal_service_principal_name:", std::string(97, 's'));
      });
      Check(HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result, "internal_service_principal_name_longer_than_policy_identifier_profile_maximum") &&
                pre_open(result, dir),
            "Q130: a 97-octet service principal name under a pack at 96 is refused before the open, no file");
      std::printf("Q130 service principal 97, pack at 96: refused before the open, no file\n");
    }
    {
      const auto [result, dir] = attempt("q130-ok-pack-96", pack_96, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", std::string(96, 'b'));
        replace(r.option_envelopes, "internal_service_principal_name:", std::string(96, 's'));
      });
      Check(result.ok && Entries(dir).size() == 1,
            "Q130 control: under a pack at 96, 96-octet principal names and every assembled name create");
      std::printf("Q130 pack at 96, 96-octet principals: created\n");
    }
    // Q133 = 1 (owner 2026-11-14): the principal names are checked
    // explicitly against the pack's profile BEFORE the plan, so a too-long
    // principal name is DATABASE.CREATE_NAME_INVALID even under a pack whose
    // own names would also fail (the pack at 8): the outcome does not depend
    // on which name the plan's assembly meets first.
    {
      const auto [result, dir] = attempt("q122-q133-pack-8-bootstrap-10", pack_8, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", "BootAdmin9");
        replace(r.option_envelopes, "internal_service_principal_name:", "Svc");
      });
      Check(HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result, "bootstrap_principal_name_longer_than_policy_identifier_profile_maximum") &&
                !HasDiagnostic(result, "DATABASE.CREATE_POLICY_PACK_INVALID") && pre_open(result, dir),
            "Q133: a 10-octet bootstrap name under a pack at 8 is a name refusal before the plan, no file");
      std::printf("Q133 bootstrap 10, pack at 8: name refusal before the plan, no file\n");
    }
    {
      const auto [result, dir] = attempt("q122-q133-pack-8-service-9", pack_8, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", "Boot");
        replace(r.option_envelopes, "internal_service_principal_name:", "NodeSvc99");
      });
      Check(HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result, "internal_service_principal_name_longer_than_policy_identifier_profile_maximum") &&
                !HasDiagnostic(result, "DATABASE.CREATE_POLICY_PACK_INVALID") && pre_open(result, dir),
            "Q133: a 9-octet service name under a pack at 8 is a name refusal before the plan, no file");
      std::printf("Q133 service 9, pack at 8: name refusal before the plan, no file\n");
    }
    // Q135 = 1 (owner 2026-11-14): no hand-written 128-byte principal cap on
    // the native path; the default pack's profile (512 scalars) decides.
    // (Was: IsSafeBootstrapPrincipalName refused any principal name over 128
    // bytes, invalid_principal, after the open, and Q108 removed the file.)
    const std::filesystem::path default_pack = SB_DEFAULT_POLICY_PACK_ROOT;
    {
      const auto [result, dir] = attempt("q135-ok-principals-129", default_pack, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", std::string(129, 'b'));
        replace(r.option_envelopes, "internal_service_principal_name:", std::string(129, 's'));
      });
      Check(result.ok && Entries(dir).size() == 1,
            "Q135: 129-octet principal names create under the default pack (was: refused by the 128-byte cap)");
      std::printf("Q135 129-octet principals, default pack: created\n");
    }
    {
      const auto [result, dir] = attempt("q135-ok-principals-512", default_pack, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", std::string(512, 'b'));
        replace(r.option_envelopes, "internal_service_principal_name:", std::string(512, 's'));
      });
      Check(result.ok && Entries(dir).size() == 1,
            "Q135/Q139: 512-scalar principal names create under the default pack (its maximum)");
      std::printf("Q135 512-octet principals, default pack: created\n");
    }
    {
      const auto [result, dir] = attempt("q122-q135-bootstrap-513", default_pack, [&](auto& r) {
        replace(r.option_envelopes, "bootstrap_principal_name:", std::string(513, 'b'));
      });
      Check(HasDiagnostic(result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result, "bootstrap_principal_name_longer_than_policy_identifier_profile_maximum") &&
                pre_open(result, dir),
            "Q133/Q135: a 513-scalar bootstrap name under the default pack is refused before the open, no file");
      std::printf("Q135 bootstrap 513, default pack: refused before the open, no file\n");
    }
  }

  // Q108: the removal itself, guard by guard, on FileDevice.
  {
    using R = d::FileDevice::EmptyNativeCreateRemoval;
    const auto dir = artifacts.path() / "removal";
    std::filesystem::create_directory(dir);
    const auto open_new = [&](d::FileDevice& device, const std::filesystem::path& path) {
      Check(device.Open(path.string(), d::FileOpenMode::create_new, d::DatabaseFileOwnership::native_data_file).ok(),
            "Q108: a new native file opens (O_EXCL + flock)");
    };
    {
      d::FileDevice device;
      open_new(device, dir / "empty");
      Check(device.RemoveAndCloseIfStillEmptyNativeCreate() == R::removed && !device.is_open() &&
                !std::filesystem::exists(dir / "empty"),
            "Q108: our own still-empty file is removed, then the device is closed");
    }
    {
      d::FileDevice device;
      open_new(device, dir / "written");
      const char byte = 'x';
      Check(device.WriteAt(0, &byte, 1).ok(), "Q108: one byte written");
      Check(device.RemoveAndCloseIfStillEmptyNativeCreate() == R::not_empty && !device.is_open() &&
                std::filesystem::file_size(dir / "written") == 1,
            "Q108: a non-empty file is never removed");
    }
    {
      d::FileDevice device;
      open_new(device, dir / "replaced");
      { std::ofstream other(dir / "other"); }
      std::filesystem::rename(dir / "other", dir / "replaced");
      Check(device.RemoveAndCloseIfStillEmptyNativeCreate() == R::path_replaced && !device.is_open() &&
                std::filesystem::exists(dir / "replaced"),
            "Q108: a path that now names another file is never removed");
    }
    {
      d::FileDevice device;
      open_new(device, dir / "gone");
      std::filesystem::remove(dir / "gone");
      { std::ofstream later(dir / "gone"); }
      Check(device.RemoveAndCloseIfStillEmptyNativeCreate() == R::path_replaced && !device.is_open() &&
                std::filesystem::exists(dir / "gone"),
            "Q108: an empty file someone else made at the path later is never removed");
    }
    {
      { std::ofstream existing(dir / "read-only"); }
      d::FileDevice device;
      Check(device.Open((dir / "read-only").string(), d::FileOpenMode::open_existing_read_only,
                        d::DatabaseFileOwnership::native_data_file).ok(), "Q108: an existing empty file opens read-only");
      Check(device.RemoveAndCloseIfStillEmptyNativeCreate() == R::not_owner && !device.is_open() &&
                std::filesystem::exists(dir / "read-only"),
            "Q108: a device that did not create the file (read-only) removes nothing");
    }
  }

  // Native create at every native profile: one file, receipt identities on page zero.
  unsigned n = 0;
  for (const auto& profile : d::kCanonicalFilespacePageProfiles) {
    const auto dir = artifacts.path() / ("native-" + std::to_string(n));
    std::filesystem::create_directory(dir);
    const auto path = (dir / "orders.sdb").string();
    const auto& language = (n % 2) ? c::kCatalogLanguage_fr_CA : c::kCatalogLanguage_en;
    auto request = NativeRequest(path, profile.page_size_bytes, language);
    // Q95: no management transaction exists before the database; none is required.
    Check(request.context.transaction_uuid.is_nil() && request.context.local_transaction_id == 0,
          "S2: the native request carries no transaction");
    const auto before = NowMillis();
    const auto created = api::EngineCreateLifecycle(request);
    const auto after = NowMillis();
    if (!created.ok)
      for (const auto& diagnostic : created.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Check(created.ok, "S2: native CREATE DATABASE through the engine API succeeds");
    const auto names = Entries(dir);
    if (names.size() != 1) for (const auto& name : names) std::cerr << "entry " << name << '\n';
    Check(names.size() == 1 && names.front() == "orders.sdb",
          "S2: exactly one file: the database (no .sb.*, no lock file, no side file)");
    std::array<p::Uuid, kReceiptKeys.size()> receipt{};
    for (std::size_t k = 0; k < kReceiptKeys.size(); ++k) {
      const auto id = RowUuid(created, kReceiptKeys[k]);
      Check(id && u::IsEngineIdentityUuid(*id), "S2: every receipt identity is returned as an engine UUIDv7 (binary16)");
      // The pack's provider and policy profile are fixed identities that the
      // policy pack carries, not minted by this create.
      const std::string_view key = kReceiptKeys[k];
      const bool minted = key != "service_provider_uuid" && key != "service_policy_uuid";
      if (minted && !StandardV7Within(*id, before, after))
        std::fprintf(stderr, "Q107 outside the create bracket: %s=%s\n", kReceiptKeys[k], Hex(*id).c_str());
      if (minted)
        Check(StandardV7Within(*id, before, after),
              "Q107: every minted identity is a standard RFC 9562 UUIDv7 timestamped by the local clock during the create");
      receipt[k] = *id;
    }
    Check(RowUuid(created, "database_language_uuid") == language && RowUuid(created, "page_size_profile_uuid") == profile.uuid,
          "S2: the result reports the language constant and page profile it used");
    Check(created.primary_object.uuid == receipt[0], "S2: the primary object is the engine's database identity");
    Check(!RowUuid(created, "database_object_uuid"), "Q111: no second database identity is returned");
    // The file carries the returned identities. Reopen with native ownership (S1).
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
          "S2: the created file reopens with native data-file ownership");
    const d::FilespaceBootstrapBinding expected{receipt[0], receipt[1], profile.uuid};
    const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device, &expected);
    Check(zero.ok() && zero.record->bootstrap.database_uuid == receipt[0] &&
              zero.record->bootstrap.filespace_uuid == receipt[1] &&
              zero.record->bootstrap.page_size_bytes == profile.page_size_bytes,
          "S2: page zero carries the returned database and filespace identities and the page size");
    const auto size = device.Size();
    Check(size.ok() && size.size_bytes == TotalPages(profile.page_size_bytes) * profile.page_size_bytes,
          "S2: the file has the requested total pages");
    Check(device.Close().ok(), "close");
    // A second create on the same path is refused (O_EXCL) and changes nothing.
    const auto again = api::EngineCreateLifecycle(request);
    Check(!again.ok && Entries(dir).size() == 1, "S2: a second native create on an existing path is refused");
    std::printf("profile %u: native create via API ok, language %s\n", profile.page_size_bytes, (n % 2) ? "fr-CA" : "en");
    ++n;
  }

  // The legacy default is unchanged: still requires a management transaction
  // and still writes the legacy (SBDBV001) format when given one.
  {
    const auto dir = artifacts.path() / "legacy";
    std::filesystem::create_directory(dir);
    api::EngineCreateLifecycleRequest legacy;
    legacy.context = Context((dir / "legacy.sdb").string());
    legacy.database_name = "ignored_by_legacy";
    legacy.database_language = c::kCatalogLanguage_en;
    legacy.option_envelopes.push_back("allow_minimal_resource_bootstrap:true");
    legacy.option_envelopes.push_back("page_size:16384");
    Check(legacy.database_format == api::EngineDatabaseFormat::legacy, "S2: legacy is the default format (Q93)");
    const auto refused = api::EngineCreateLifecycle(legacy);
    Check(!refused.ok && HasDiagnostic(refused, "MGA.TRANSACTION.INVALID") && Entries(dir).empty(),
          "S2: the legacy default still requires the management transaction (unchanged until C)");
    legacy.context.transaction_uuid = api::EngineUuid{Fixed(0x60)};
    legacy.context.local_transaction_id = 1;
    const auto made = api::EngineCreateLifecycle(legacy);
    if (!made.ok) for (const auto& diagnostic : made.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Check(made.ok, "S2: the legacy default still creates");
    std::ifstream in(dir / "legacy.sdb", std::ios::binary);
    std::array<char, 8> magic{};
    in.read(magic.data(), magic.size());
    Check(std::string_view(magic.data(), 8) == "SBDBV001", "S2: the legacy default still writes the legacy format");
  }

  // Through SBLR dispatch: the v2 descriptor selects native and carries the
  // name and language; its client identity fields are ignored.
  {
    const auto dir = artifacts.path() / "dispatch";
    std::filesystem::create_directory(dir);
    const auto path = (dir / "dispatched.sdb").string();
    auto v2 = V2(c::kCatalogLanguage_fr_CA, "commandes", sblr::SblrLifecycleCreateDatabaseFormat::native);
    v2.database_uuid = Fixed(0x52);
    v2.database_name_uuid = Fixed(0x53);
    v2.filespace_uuid = Fixed(0x5c);
    sblr::SblrOperand operand;
    operand.type = "lifecycle_create_database_descriptor.v2";
    operand.name = "database";
    operand.ordinal = 1;
    operand.value_kind = sblr::SblrValueKind::lifecycle_create_database_descriptor;
    operand.value_body = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(v2);
    Check(!operand.value_body.empty(), "v2 operand encodes");
    auto mislabelled = operand;
    mislabelled.type = "lifecycle_create_database_descriptor.v1";
    const auto wrong = sblr::DecodeSblrEnvelope(sblr::EncodeSblrEnvelope(CreateEnvelope(mislabelled)));
    const sblr::SblrDispatchRequest wrong_request{Context(path), wrong.envelope, api::EngineApiRequest{}};
    const auto wrong_result = sblr::DispatchSblrOperation(wrong_request);
    Check((!wrong.ok || !wrong_result.envelope_validated) && !wrong_result.dispatched_to_api &&
              !wrong_result.api_result.ok && !std::filesystem::exists(path),
          "S2: a v2 body labelled v1 is refused at envelope validation (the type names the version)");
    const auto bytes = sblr::EncodeSblrEnvelope(CreateEnvelope(operand));
    const auto decoded = sblr::DecodeSblrEnvelope(bytes);
    Check(decoded.ok, "the v2 create envelope round trips");
    api::EngineApiRequest options;
    NativeOptions(options.option_envelopes, 16384, TotalPages(16384));
    const sblr::SblrDispatchRequest dispatch{Context(path), decoded.envelope, options};
    const auto result = sblr::DispatchSblrOperation(dispatch);
    for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& diagnostic : result.api_result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Check(result.envelope_validated && result.dispatched_to_api && result.api_result.ok,
          "S2: the v2 descriptor dispatches to the native create");
    const auto database = RowUuid(result.api_result, "database_uuid");
    const auto filespace = RowUuid(result.api_result, "filespace_uuid");
    Check(database && filespace && database->bytes != v2.database_uuid && filespace->bytes != v2.filespace_uuid &&
              database->bytes != v2.database_name_uuid,
          "S2: the descriptor's client identities are ignored; the engine minted its own");
    Check(RowUuid(result.api_result, "database_language_uuid") == c::kCatalogLanguage_fr_CA,
          "S2: the descriptor's language reached the engine");
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
          "the dispatched file reopens natively");
    const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok() && zero.record->bootstrap.database_uuid == *database, "S2: the dispatched file carries the returned identity");
    Check(device.Close().ok(), "close");
    // Reported, not asserted: what dispatch itself leaves beside the database
    // (executor availability and the like are S3's).
    for (const auto& name : Entries(dir)) std::printf("dispatch dir entry: %s\n", name.c_str());
  }

  // Q110 = 1 (owner 2026-11-14, SB_CREATE_DESCRIPTOR_NOT_AN_OPTION): the
  // create descriptor (user data: the database name) never joins the
  // request's option list, and lifecycle option checks match whole option
  // keys, never a substring anywhere in an option.
  {
    const auto root = artifacts.path() / "q110";
    std::filesystem::create_directory(root);
    // Names carrying each checked token, through SBLR dispatch (the v2
    // descriptor), plus a plain control.
    DispatchCreateWithName(root, "plain", "orders");
    DispatchCreateWithName(root, "unsafe-profile-combo", "unsafe_profile_combo");
    DispatchCreateWithName(root, "x-unsafe-profile-combo", "x_unsafe_profile_combo");
    DispatchCreateWithName(root, "policy-seed-pack-root", "policy_seed_pack_root:/client/chosen/pack");
    DispatchCreateWithName(root, "page-size", "page_size:4096");
    DispatchCreateWithName(root, "total-pages", "total_pages:1");
    DispatchCreateWithName(root, "mode-read-only", "mode:read_only;allow_writes:true");
    DispatchCreateWithName(root, "cluster", "cluster:true");
    // A NUL byte: the v2 codec admits U+0000 (well-formed UTF-8) and the
    // envelope carries it; the catalog's name rule refuses NUL in any name
    // (AdmitCatalogIdentifierText, catalog_identifier_lookup_key.cpp). The
    // name is never an option: the refusal is the name's own, not an option
    // check.
    // Q122 = 1 (owner 2026-11-14): native create now applies that rule BEFORE
    // the open: DATABASE.CREATE_NAME_INVALID, no file ever created. Was
    // (Q110, recorded as observed): refused at database assembly after the
    // open (DATABASE.CREATE_FAILED, native_create_refused:database_assembly),
    // with Q108 removing the still-empty file. The codec is unchanged and
    // still admits the name.
    {
      // Q128 = 1 (owner 2026-11-14, SB_CATALOG_IDENTIFIER_TEXT_RULE): the v2
      // codec now applies the catalog's name rule, so it neither encodes nor
      // decodes this name, and SBLR dispatch refuses it at envelope
      // validation, before the engine API: no file. Was (Q110/Q122): the
      // codec admitted and round-tripped the NUL name, dispatch reached the
      // engine, and native create refused DATABASE.CREATE_NAME_INVALID before
      // the open. That engine refusal is kept (defence in depth) and is
      // checked on the engine API directly in the Q122 block.
      const std::string nul_name("a\0unsafe_profile_combo", 22);
      Check(sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(
                V2(c::kCatalogLanguage_en, nul_name, sblr::SblrLifecycleCreateDatabaseFormat::native)).empty(),
            "Q128: the v2 codec refuses to encode a name containing NUL (was: admitted and round-tripped)");
      const auto dir = root / "q122-q110-nul";
      std::filesystem::create_directory(dir);
      const auto path = (dir / "named.sdb").string();
      sblr::SblrOperand operand;
      operand.type = "lifecycle_create_database_descriptor.v2";
      operand.name = "database";
      operand.ordinal = 1;
      operand.value_kind = sblr::SblrValueKind::lifecycle_create_database_descriptor;
      // The same name, made by patching the placeholder byte to NUL.
      operand.value_body = PatchedNameBytes("aXunsafe_profile_combo", 1, 0);
      api::EngineApiRequest options;
      NativeOptions(options.option_envelopes, 16384, TotalPages(16384));
      const auto envelope = CreateEnvelope(operand);
      const auto result = sblr::DispatchSblrOperation({Context(path), envelope, options});
      std::string first;
      if (!result.diagnostics.empty()) first = result.diagnostics.front().code + ":" + result.diagnostics.front().message;
      std::fprintf(stderr, "Q128 dispatch nul: envelope_validated=%d dispatched=%d first=%s\n",
                   int(result.envelope_validated), int(result.dispatched_to_api), first.c_str());
      Check(!result.envelope_validated && !result.dispatched_to_api && !result.api_result.ok && Entries(dir).empty(),
            "Q128: SBLR dispatch refuses a v2 descriptor whose name holds NUL at envelope validation; no file");
      // Control: the same patch with an admitted byte passes validation and
      // creates, so the refusal above is the name rule's.
      {
        const auto control_dir = root / "q128-dispatch-control";
        std::filesystem::create_directory(control_dir);
        auto control = operand;
        control.value_body = PatchedNameBytes("aXunsafe_profile_combo", 1, 'Y');
        const auto made = sblr::DispatchSblrOperation(
            {Context((control_dir / "named.sdb").string()), CreateEnvelope(control), options});
        Check(made.envelope_validated && made.dispatched_to_api && made.api_result.ok && Entries(control_dir).size() == 1,
              "Q128 control: the same patched descriptor with an admitted byte validates, dispatches and creates");
      }
      std::printf("Q110/Q122/Q128 dispatch name nul: codec refuses, dispatch refuses before the engine, no file\n");
    }

    // Option values that are user text (principal names) carrying a checked
    // token, through the API directly: whole-key matching does not see them.
    const auto direct = [&](const char* label, auto edit, bool expect_ok, const char* what) {
      const auto dir = root / (std::string("direct-") + label);
      std::filesystem::create_directory(dir);
      auto request = NativeRequest((dir / "orders.sdb").string(), 16384, c::kCatalogLanguage_en);
      edit(request);
      const auto result = api::EngineCreateLifecycle(request);
      if (result.ok != expect_ok)
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      if (expect_ok) {
        Check(result.ok && Entries(dir).size() == 1, what);
      } else {
        Check(!result.ok && HasDiagnostic(result, "SB_ENGINE_API_LIFECYCLE_UNSAFE_PROFILE") && Entries(dir).empty(), what);
      }
    };
    const auto replace = [](std::vector<std::string>& options, std::string_view key, std::string value) {
      std::erase_if(options, [&](const auto& o) { return o.starts_with(key); });
      options.push_back(std::string(key) + value);
    };
    direct("principal-unsafe", [&](auto& r) {
      replace(r.option_envelopes, "bootstrap_principal_name:", "unsafe_profile_combo_admin");
    }, true, "Q110: a principal name containing unsafe_profile_combo is a value, not the unsafe-profile key");
    // Principal names refuse ':' (invalid_principal), so the free-text
    // carrier here is the audit reason the management route sends
    // (manager_control.cpp: "audit_reason:" + the administrator's text).
    direct("audit-reason-mode", [&](auto& r) {
      r.option_envelopes.push_back("audit_reason:mode:read_only");
      r.option_envelopes.push_back("audit_reason_detail:allow_writes:true");
    }, true, "Q110: mode:read_only and allow_writes:true inside other options' values are not those options");
    // Positive controls: the whole-key options themselves are still refused.
    direct("unsafe-key", [](auto& r) { r.option_envelopes.push_back("unsafe_profile_combo"); }, false,
           "Q110 control: the unsafe_profile_combo option itself is still refused");
    direct("unsafe-key-value", [](auto& r) { r.option_envelopes.push_back("unsafe_profile_combo:true"); }, false,
           "Q110 control: the unsafe_profile_combo key with a value is still refused");
    direct("read-only-writes", [](auto& r) {
      r.option_envelopes.push_back("mode:read_only");
      r.option_envelopes.push_back("allow_writes:true");
    }, false, "Q110 control: mode:read_only with allow_writes:true is still refused");
  }

  std::printf("PASS native create lifecycle api checks=%d\n", checks);
  return 0;
}
