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
#include "catalog_name_record_codec.hpp"
#include "catalog_native_catalog_index.hpp"
#include "catalog_page.hpp"
#include "native_index_btree_page.hpp"
#include "physical_mga_cow_store.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
  refuses([](auto& x) { x.database_name = std::string(1025, 'a'); }, "S2: v2 refuses a name over 1024 bytes");
  refuses([](auto& x) { x.database_name = std::string("bad\xc3", 4); }, "S2: v2 refuses a name that is not UTF-8");
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
  {
    const auto late = [&](const char* label, auto edit, const char* what) {
      const auto dir = artifacts.path() / (std::string("late-") + label);
      std::filesystem::create_directory(dir);
      const auto path = (dir / "orders.sdb").string();
      auto request = NativeRequest(path, 16384, c::kCatalogLanguage_en);
      edit(request);
      const auto result = api::EngineCreateLifecycle(request);
      if (result.ok || !DetailHas(result, "native_create_refused:"))
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(!result.ok && HasDiagnostic(result, "DATABASE.CREATE_FAILED") && DetailHas(result, "native_create_refused:"),
            what);
      Check(DetailHas(result, ";empty_file_removed") && Entries(dir).empty(),
            "Q108: the refused create removed its own still-empty file; nothing is left");
      const auto retried = api::EngineCreateLifecycle(NativeRequest(path, 16384, c::kCatalogLanguage_en));
      if (!retried.ok) for (const auto& diagnostic : retried.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(retried.ok && Entries(dir).size() == 1, "Q108: a retry on the same path succeeds");
      std::printf("Q108 %s: refused after open, no file left, retry ok\n", label);
    };
    late("fingerprint", [](auto& r) {
      std::erase_if(r.option_envelopes, [](const auto& o) { return o.starts_with("bootstrap_credential_fingerprint:"); });
      r.option_envelopes.push_back("bootstrap_credential_fingerprint:not-a-fingerprint");
    }, "Q108: a bad credential fingerprint is refused after the open");
    late("policy-pack", [&](auto& r) {
      r.context.server_policy_seed_pack_root = (artifacts.path() / "no-such-policy-pack").string();
    }, "Q108: a bad (server) policy pack is refused after the open");
    late("pages", [](auto& r) {
      std::erase_if(r.option_envelopes, [](const auto& o) { return o.starts_with("total_pages:"); });
      r.option_envelopes.push_back("total_pages:2");
    }, "Q108: too few pages is refused after the open");
    // Q122 = 1 (owner 2026-11-14): the non-UTF-8 name case is no longer a
    // late refusal; it is refused before the open (the Q122 block below).
    // Was: late("utf8", name "bad\xc3", "...is refused after the open").
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
      const std::string nul_name("a\0unsafe_profile_combo", 22);
      sblr::SblrLifecycleCreateDatabaseDescriptorV2 decoded_v2;
      std::string detail;
      const auto bytes = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(
          V2(c::kCatalogLanguage_en, nul_name, sblr::SblrLifecycleCreateDatabaseFormat::native));
      Check(!bytes.empty() && sblr::DecodeSblrLifecycleCreateDatabaseDescriptorV2(bytes.data(), bytes.size(), &decoded_v2, &detail) &&
                decoded_v2.database_name == nul_name,
            "Q110: the v2 codec admits and round-trips a name containing NUL, byte for byte");
      const auto dir = root / "q122-q110-nul";
      std::filesystem::create_directory(dir);
      const auto result = DispatchNamedCreate((dir / "named.sdb").string(), nul_name);
      if (result.api_result.ok || !HasDiagnostic(result.api_result, "DATABASE.CREATE_NAME_INVALID"))
        for (const auto& diagnostic : result.api_result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      Check(result.envelope_validated && result.dispatched_to_api && !result.api_result.ok &&
                HasDiagnostic(result.api_result, "DATABASE.CREATE_NAME_INVALID") &&
                DetailHas(result.api_result, "database_name_refused_by_catalog_name_rule") &&
                !HasDiagnostic(result.api_result, "DATABASE.CREATE_FAILED") &&
                !DetailHas(result.api_result, "native_create_refused:") &&
                !DetailHas(result.api_result, ";empty_file_") &&
                !HasDiagnostic(result.api_result, "SB_ENGINE_API_LIFECYCLE_UNSAFE_PROFILE") && Entries(dir).empty(),
            "Q110/Q122: a NUL in the name is refused by the catalog's name rule before the file is opened, not by "
            "an option check, and no file ever exists");
      std::printf("Q110/Q122 dispatch name nul: codec admits, catalog name rule refuses before the open, no file\n");
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
