// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Owner Q113 = 1 (2026-11-14), SB_SERVER_POLICY_SEED_PACK_ROOT: the server
// attaches its own policy seed pack root to the ENGINE HANDLE when it opens
// it (server_engine_bridge::OpenServedEngine), and every statement context
// that handle issues (AcquireStatementContextReceipt) carries exactly that
// root. The C ABI (sb_engine_open, sb_engine_request_context_v1_t) has no
// field for it and is unchanged.
//
// Checked here, on a real database, session, MGA transaction and statement
// receipt (the ia01 production-boundary fixture, no mocks):
//  1. a handle opened with the server's root issues receipts whose engine
//     context carries that root, even when the caller's context held another
//     value (the handle is the one source);
//  2. that receipt context drives a native CREATE DATABASE through SBLR
//     dispatch (DispatchSblrOperation, the dispatcher the receipt route
//     calls) to success;
//  3. a handle opened without a root (sb_engine_open, or the server bridge's
//     four-argument open) issues receipts with no root, and the same create
//     refuses DATABASE.CREATE_POLICY_PACK_REQUIRED, leaving no file;
//  4. a client option naming the pack is still refused
//     (DATABASE.CREATE_POLICY_PACK_CLIENT_REFUSED), with the handle's root
//     present.
// Recorded, not hidden: on the receipt route a create's target path is the
// context's database_path, which the receipt pins to the handle's own (open)
// database; so a create dispatched with the receipt context unchanged is
// refused at the O_EXCL open (the file exists). Checks 2 and 3 therefore
// substitute a fresh target path into a copy of the receipt context; where a
// create's target path should come from on this route is an owner question.
#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"

#include "engine/internal_api/lifecycle/engine_lifecycle_api.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "core/catalog/catalog_language.hpp"
#include "core/hash/hash_digest.hpp"

#include <cstdio>
#include <memory>
#include <source_location>

namespace {

namespace c = scratchbird::core::catalog;

int checks = 0;
void Check(bool ok, const char* what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what, at.line());
    std::exit(1);
  }
}

constexpr std::string_view kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:salt=0123456789abcdef0123456789abcdef:"
    "verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";

std::array<std::uint8_t, 16> Fixed(std::uint8_t n) {
  std::array<std::uint8_t, 16> id{};
  id[0] = 1; id[6] = 0x70; id[8] = 0x80; id[15] = n;
  return id;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) if (diagnostic.code == code) return true;
  return false;
}
bool DetailHas(const api::EngineApiResult& result, std::string_view text) {
  for (const auto& diagnostic : result.diagnostics)
    if (diagnostic.detail.find(text) != std::string::npos) return true;
  return false;
}
void Dump(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  for (const auto& diagnostic : result.api_result.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
}

// The v2 create descriptor (native format, name and language as data) in a
// canonical SBLR envelope, as native_create_lifecycle_api_test builds it.
sblr::SblrOperationEnvelope NativeCreateEnvelope(std::string name) {
  sblr::SblrLifecycleCreateDatabaseDescriptorV2 descriptor;
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
  descriptor.database_language_uuid = c::kCatalogLanguage_en.bytes;
  descriptor.database_name = std::move(name);
  descriptor.database_format = sblr::SblrLifecycleCreateDatabaseFormat::native;

  sblr::SblrOperand operand;
  operand.type = "lifecycle_create_database_descriptor.v2";
  operand.name = "database";
  operand.ordinal = 1;
  operand.value_kind = sblr::SblrValueKind::lifecycle_create_database_descriptor;
  operand.value_body = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV2(descriptor);
  Check(!operand.value_body.empty(), "the v2 descriptor encodes");

  const auto* entry = sblr::LookupSblrOpcode("SBLR_LIFECYCLE_CREATE_DATABASE");
  Check(entry != nullptr, "the create opcode is registered");
  auto envelope = sblr::MakeSblrEnvelope(entry->operation_id, "SBLR_LIFECYCLE_CREATE_DATABASE",
                                         "trace.q113.handle_policy_pack");
  envelope.opcode_code = entry->code;
  envelope.parser_package_uuid = api::EngineUuid{Fixed(0x41)};
  envelope.registry_snapshot_uuid = api::EngineUuid{Fixed(0x42)};
  envelope.requires_security_context = entry->requires_security_context;
  envelope.requires_transaction_context = entry->requires_transaction_context;
  envelope.requires_cluster_authority = entry->requires_cluster_authority;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back(std::move(operand));
  const auto decoded = sblr::DecodeSblrEnvelope(sblr::EncodeSblrEnvelope(envelope));
  Check(decoded.ok, "the v2 create envelope round trips");
  return decoded.envelope;
}

api::EngineApiRequest NativeOptions() {
  api::EngineApiRequest options;
  options.option_envelopes = {
      "page_size:16384", "total_pages:512",
      "bootstrap_principal_name:BootstrapAdmin",
      "bootstrap_credential_fingerprint:" + std::string(kFingerprint),
      "internal_service_principal_name:NodeService",
      "internal_service_credential_fingerprint:" + std::string(kFingerprint)};
  return options;
}

// A session on `engine` and one statement receipt; returns the receipt's
// engine context as the engine issued it.
api::EngineRequestContext ReceiptContext(const Fixture& fixture, sb_engine_handle_t engine,
                                         const std::string& caller_root) {
  sb_engine_session_params_v1_t begin{};
  begin.struct_size = sizeof(begin);
  begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  begin.effective_user_uuid = PublicUuid(fixture.principal_uuid);
  begin.session_uuid = PublicUuid(fixture.session_uuid);
  begin.default_language_utf8 = "en";
  begin.default_language_size = 2;
  begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  sb_engine_session_t session = nullptr;
  Check(sb_engine_session_begin(engine, &begin, &session, nullptr) == SB_ENGINE_STATUS_OK && session != nullptr,
        "the handle opens a session");

  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = {};
  // The caller (the server) cannot choose the root: whatever it puts here is
  // replaced by the handle's.
  context.server_policy_seed_pack_root = caller_root;
  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t result = nullptr;
  const auto acquired = bridge::AcquireStatementContextReceipt(session, &acquire, &receipt, &view, &result);
  if (acquired != SB_ENGINE_STATUS_OK) std::cerr << "acquire: " << sb_engine_status_name(acquired) << '\n';
  if (result != nullptr) (void)sb_engine_result_release(result);
  Check(acquired == SB_ENGINE_STATUS_OK, "a statement receipt is issued");
  api::EngineRequestContext issued;
  Check(bridge::CopyStatementContextEngineContextV1(receipt, &issued, nullptr) == SB_ENGINE_STATUS_OK,
        "the receipt's engine context is copied");
  (void)bridge::ReleaseStatementContextReceipt(receipt);
  sb_engine_session_end_params_v1_t end{};
  end.struct_size = sizeof(end);
  end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  end.rollback_active_transactions = 1;
  end.cancel_open_results = 1;
  Check(sb_engine_session_end(session, &end, nullptr) == SB_ENGINE_STATUS_OK, "the session ends");
  return issued;
}

sb_engine_open_params_v1_t OpenParams(const std::string& path) {
  sb_engine_open_params_v1_t params{};
  params.struct_size = sizeof(params);
  params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  params.database_path_utf8 = path.data();
  params.database_path_size = path.size();
  params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  return params;
}

sblr::SblrDispatchResult DispatchCreate(api::EngineRequestContext context, const std::string& path,
                                        api::EngineApiRequest options = NativeOptions()) {
  context.database_path = path;
  return sblr::DispatchSblrOperation({std::move(context), NativeCreateEnvelope("orders"), std::move(options)});
}

}  // namespace

int main() {
  const std::string server_root = SB_DEFAULT_POLICY_PACK_ROOT;
  auto fixture = CreateFixture();
  const auto path = fixture.database_path.string();
  const auto fresh = [&](const char* label) {
    const auto dir = fixture.directory / (std::string("q113-") + label);
    std::filesystem::create_directory(dir);
    return dir;
  };

  // 1-2, 4: the server's open attaches its root.
  {
    const auto params = OpenParams(path);
    sb_engine_handle_t engine = nullptr;
    Check(bridge::OpenServedEngine(&params, {}, server_root, &engine, nullptr) == SB_ENGINE_STATUS_OK &&
              engine != nullptr,
          "Q113: the server opens the handle with its policy pack root");
    const auto issued = ReceiptContext(fixture, engine, "/caller/chosen/pack");
    Check(issued.server_policy_seed_pack_root == server_root,
          "Q113: the receipt context carries the handle's root, not the caller's");
    Check(!issued.database_path.empty() && issued.database_path == path,
          "Q113: the receipt context is pinned to the handle's own database");
    std::printf("Q113 receipt context: root from the handle (caller value replaced)\n");

    // As issued: the create targets the handle's own (existing) database.
    {
      const auto before = std::filesystem::file_size(path);
      const auto as_issued = sblr::DispatchSblrOperation({issued, NativeCreateEnvelope("orders"), NativeOptions()});
      Check(as_issued.dispatched_to_api && !as_issued.api_result.ok &&
                DetailHas(as_issued.api_result, "native_database_file_open_refused:") &&
                !HasDiagnostic(as_issued.api_result, "DATABASE.CREATE_POLICY_PACK_REQUIRED") &&
                std::filesystem::file_size(path) == before,
            "Q113 (recorded): with the receipt context unchanged, the create reaches the O_EXCL open of the "
            "handle's existing database and is refused there, past the pack check; the database is untouched");
      std::printf("Q113 receipt context unchanged: past the pack check, refused at the O_EXCL open (path exists)\n");
    }

    const auto dir = fresh("handle-root");
    const auto created = DispatchCreate(issued, (dir / "created.sdb").string());
    if (!created.api_result.ok) Dump(created);
    Check(created.envelope_validated && created.dispatched_to_api && created.api_result.ok &&
              std::filesystem::exists(dir / "created.sdb") &&
              std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 1,
          "Q113: the handle's root drives a native CREATE DATABASE through SBLR dispatch to success");
    std::printf("Q113 native create with the handle's root: created, one file\n");

    auto client_option = NativeOptions();
    client_option.option_envelopes.push_back("policy_seed_pack_root:/client/chosen/pack");
    const auto client_dir = fresh("client-option");
    const auto refused = DispatchCreate(issued, (client_dir / "created.sdb").string(), client_option);
    Check(!refused.api_result.ok &&
              HasDiagnostic(refused.api_result, "DATABASE.CREATE_POLICY_PACK_CLIENT_REFUSED") &&
              std::filesystem::is_empty(client_dir),
          "Q104/Q113: a client option naming the pack is still refused, with the handle's root present");
    std::printf("Q113 client pack option: refused\n");
    Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK, "close");
  }

  // 3: no root on the handle.
  const auto no_root = [&](const char* label, bool served) {
    const auto params = OpenParams(path);
    sb_engine_handle_t engine = nullptr;
    const auto opened = served ? bridge::OpenServedEngine(&params, {}, &engine, nullptr)
                               : sb_engine_open(&params, &engine, nullptr);
    Check(opened == SB_ENGINE_STATUS_OK && engine != nullptr, "the handle opens");
    const auto issued = ReceiptContext(fixture, engine, server_root);
    Check(issued.server_policy_seed_pack_root.empty(),
          "Q113: a handle opened without a root issues no root, whatever the caller's context held");
    const auto dir = fresh(label);
    const auto refused = DispatchCreate(issued, (dir / "created.sdb").string());
    Check(refused.dispatched_to_api && !refused.api_result.ok &&
              HasDiagnostic(refused.api_result, "DATABASE.CREATE_POLICY_PACK_REQUIRED") &&
              std::filesystem::is_empty(dir),
          "Q113: with no root on the handle, native create refuses CREATE_POLICY_PACK_REQUIRED and makes no file");
    std::printf("Q113 %s handle without a root: CREATE_POLICY_PACK_REQUIRED, no file\n", label);
    Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK, "close");
  };
  no_root("c-abi-open", false);
  no_root("served-open-no-root", true);

  std::printf("q113_engine_handle_policy_pack_root PASS (%d checks)\n", checks);
  return 0;
}
