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
//
// Q125 = 3 (owner 2026-11-14, SB_CREATE_DATABASE_NOT_ON_SESSION_ROUTE):
// CREATE DATABASE stays OFF the session / statement-receipt route (create is
// pre-security: a file write, not a connection). Checked here through the
// real route (AcquireStatementContextReceipt -> server admission ->
// DispatchStatementContextReceipt), as the ia01 package fixture drives it:
//  5. a native v2 create and a legacy v1 create, each the member of an
//     admitted opcode-stream package, are refused
//     DATABASE.CREATE_NOT_ON_SESSION_ROUTE; no file appears and the handle's
//     database is not grown.
// Was (check 2): "a native V2 create through DispatchSblrOperation succeeds",
// presented as the receipt route. That call is the in-process dispatcher
// with a copy of the receipt's context and a swapped target path; it never
// entered DispatchStatementContextReceipt. It is kept below, relabelled as
// what it is (the API-level dispatcher), because checks 3 and 4 depend on it
// to show the handle's root reaching the engine; it is not the session route.
//
// Q132 = 2 (owner 2026-11-14, SB_CREATE_DATABASE_NOT_ON_SESSION_ROUTE):
// DispatchSblrOperation itself now refuses CREATE DATABASE when the context
// carries a statement receipt (statement_receipt_uuid, which every
// receipt-derived context carries: CopyStatementContextEngineContextV1 sets
// it). So every create above that used the receipt's context (checks 2, 3, 4
// and the "as issued" note) is now refused DATABASE.CREATE_NOT_ON_SESSION_ROUTE
// with no file; each changed check is annotated where it stands. What is
// still shown: (1) the handle's root reaches every receipt context it
// issues (check 1); (2) with NO receipt, a context carrying that same root
// value creates through DispatchSblrOperation, and the client option and
// missing-root refusals hold there. Recorded, not hidden: the hop between
// (1) and (2) is this test copying the value; no production route now
// carries a handle's root to a native create (the management route,
// manager_control.cpp, fills the field from server config and does not yet
// issue native creates, Q109). Where a production native create gets its
// pack root is an owner question.
#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"

#include "engine/internal_api/lifecycle/engine_lifecycle_api.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "core/catalog/catalog_language.hpp"
#include "core/hash/hash_digest.hpp"

#include <algorithm>
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

// Q132 = 2 (owner 2026-11-14): a context with NO statement receipt, as
// native_create_lifecycle_api_test's Context() builds one (the server's
// stand-in), carrying the policy pack root the test passes in. Used to show
// the engine creates with a root a handle issued; the transfer of that value
// from the receipt context into this one is the test's, not a production
// route's (recorded in the header).
api::EngineRequestContext ReceiptFreeContext(const std::string& root) {
  api::EngineRequestContext context;
  context.request_id = "q132-receipt-free-create";
  context.security_context_present = true;
  context.statement_uuid = api::EngineUuid{Fixed(0x31)};
  context.server_policy_seed_pack_root = root;
  return context;
}

// Q132: a receipt-bearing context's create is refused by DispatchSblrOperation
// itself, before validation and before any file work.
bool RefusedNotOnSessionRoute(const sblr::SblrDispatchResult& result) {
  return !result.envelope_validated && !result.dispatched_to_api && !result.api_result.ok &&
         HasDiagnostic(result.api_result, "DATABASE.CREATE_NOT_ON_SESSION_ROUTE");
}

// Q125 (SB_CREATE_DATABASE_NOT_ON_SESSION_ROUTE): one CREATE DATABASE member
// in a canonical package (begin, member, end), submitted through the real
// session route exactly as ia01_package_cancellation_fault_test's main()
// drives a query: receipt, package reservation, server admission (the
// gateway), then DispatchStatementContextReceipt. Returns the engine status
// and the first diagnostic's symbolic code.
struct RouteOutcome {
  sb_engine_status_t status = SB_ENGINE_STATUS_OK;
  std::string code;
  bool admitted = false;
};

sblr::SblrOperationEnvelope CreateMember(sblr::SblrOperand operand, const platform::Uuid& parser,
                                         const platform::Uuid& registry) {
  const auto* entry = sblr::LookupSblrOpcode("SBLR_LIFECYCLE_CREATE_DATABASE");
  Check(entry != nullptr, "the create opcode is registered");
  auto member = sblr::MakeSblrEnvelope(entry->operation_id, "SBLR_LIFECYCLE_CREATE_DATABASE",
                                       "trace.q125.session_route_create");
  member.opcode_code = entry->code;
  member.result_shape = "lifecycle_create_database_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser;
  member.registry_snapshot_uuid = registry;
  member.requires_security_context = entry->requires_security_context;
  member.requires_transaction_context = entry->requires_transaction_context;
  member.requires_cluster_authority = entry->requires_cluster_authority;
  member.contains_sql_text = false;
  member.parser_resolved_names_to_uuids = true;
  member.operands.push_back(std::move(operand));
  return member;
}

sblr::SblrOperand V2Operand(std::string name) {
  auto envelope = NativeCreateEnvelope(std::move(name));
  return envelope.operands.front();
}

sblr::SblrOperand V1Operand() {
  sblr::SblrLifecycleCreateDatabaseDescriptorV1 descriptor;
  descriptor.operation_uuid = Fixed(0x50);
  descriptor.statement_receipt_uuid = Fixed(0x51);
  descriptor.database_uuid = Fixed(0x52);
  descriptor.database_name_uuid = Fixed(0x53);
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
  sblr::SblrOperand operand;
  operand.type = "lifecycle_create_database_descriptor.v1";
  operand.name = "database";
  operand.ordinal = 1;
  operand.value_kind = sblr::SblrValueKind::lifecycle_create_database_descriptor;
  operand.value_body = sblr::EncodeSblrLifecycleCreateDatabaseDescriptorV1(descriptor);
  Check(!operand.value_body.empty(), "the v1 descriptor encodes");
  return operand;
}

// The ia01 fixture's BuildSubmission, with the contained member supplied.
Submission BuildMemberSubmission(const Fixture& fixture, const bridge::StatementContextReceiptView& view,
                                 const platform::Uuid& parser_uuid, sblr::SblrOperationEnvelope member) {
  const auto package = RawUuid(view.bound_ast_uuid);
  sblr::SblrOpcodeStream package_stream;
  package_stream.package_descriptor_uuid = view.bound_ast_uuid;
  package_stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
  package_stream.operations = {
      Frame(true, parser_uuid, view.catalog_epoch_uuid, package),
      std::move(member),
      Frame(false, parser_uuid, view.catalog_epoch_uuid, package)};
  const auto stream = sblr::EncodeSblrOpcodeStream(package_stream);
  Check(!stream.empty(), "Q125: the create package encodes as canonical SBOS");

  const auto database = RawUuid(Identity(fixture.database_uuid));
  const auto dialect = RawUuid(Identity(NewUuid(platform::UuidKind::object, 501)));
  const auto parser = RawUuid(parser_uuid);
  const auto registry = RawUuid(view.catalog_epoch_uuid);
  const auto statement = RawUuid(view.statement_uuid);
  const auto principal = RawUuid(Identity(fixture.principal_uuid));
  wire::SblrCanonicalContainer container;
  std::copy(database.begin(), database.end(), container.canonical_anchor.begin());
  std::copy(dialect.begin(), dialect.end(), container.canonical_anchor.begin() + 16);
  std::copy(parser.begin(), parser.end(), container.canonical_anchor.begin() + 32);
  auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned n = 0; n != 4; ++n) container.canonical_anchor[offset + n] = value >> (n * 8);
  };
  auto put64 = [&](std::size_t offset, std::uint64_t value) {
    for (unsigned n = 0; n != 8; ++n) container.canonical_anchor[offset + n] = value >> (n * 8);
  };
  put32(48, 1); put64(52, 1); put64(60, 1); put64(68, 1);
  std::copy(registry.begin(), registry.end(), container.canonical_anchor.begin() + 76);
  put64(92, 1); container.canonical_anchor[100] = 1;
  std::copy(statement.begin(), statement.end(), container.canonical_anchor.begin() + 116);
  container.operation_payload = stream;
  const auto outer = wire::EncodeSblrContainer(container);

  wire::SblrExecutionEnvelopeV1 ingress;
  auto& fields = ingress.fields;
  fields[0] = Bytes(statement.begin(), statement.end()); fields[1] = V16(1);
  fields[2] = V16(0); fields[3] = V32(0x10001); fields[4] = V16(1);
  fields[5] = {1}; U64(&fields[5], stream.size());
  fields[5].insert(fields[5].end(), stream.begin(), stream.end()); fields[6] = {0};
  fields[7] = {1}; U32(&fields[7], wire::SblrCrc32c(stream.data(), stream.size()));
  fields[8] = V64(stream.size()); fields[9] = V16(1);
  fields[10] = OptionalUuid(dialect); fields[11] = OptionalUuid(principal);
  fields[12] = CanonicalStruct(0x1001, 1); fields[13] = CanonicalStruct(0x1002, 2);
  fields[14] = {0}; fields[15] = V64(1); fields[16] = V32(0); fields[17] = V32(0);
  fields[18] = V32(0); fields[19] = {0}; fields[20] = V32(0);
  fields[21] = CanonicalStruct(0x1005, 5); fields[22] = {0}; fields[23] = {0};
  fields[24] = {0}; fields[25] = V16(0); fields[26] = {0}; fields[27] = {0};
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress);
  return {{outer.begin(), outer.end()}, {execution.begin(), execution.end()}, stream};
}

// Snapshot of what is on disk under `dir`: every entry and its size.
std::vector<std::pair<std::string, std::uintmax_t>> Listing(const std::filesystem::path& dir) {
  std::vector<std::pair<std::string, std::uintmax_t>> out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    out.emplace_back(entry.path().string(), entry.is_regular_file() ? entry.file_size() : 0);
  std::sort(out.begin(), out.end());
  return out;
}

RouteOutcome SessionRouteCreate(const Fixture& fixture, sb_engine_handle_t engine, sblr::SblrOperand operand,
                                std::vector<std::pair<std::string, std::uintmax_t>>* before) {
  RouteOutcome outcome;
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
        "Q125: the handle opens a session");
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = {};
  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t acquired_result = nullptr;
  Check(bridge::AcquireStatementContextReceipt(session, &acquire, &receipt, &view, &acquired_result) ==
            SB_ENGINE_STATUS_OK,
        "Q125: a statement receipt is issued");
  if (acquired_result) (void)sb_engine_result_release(acquired_result);

  const auto parser_uuid = Identity(NewUuid(platform::UuidKind::object, 502));
  const auto submission =
      BuildMemberSubmission(fixture, view, parser_uuid, CreateMember(std::move(operand), parser_uuid,
                                                                     view.catalog_epoch_uuid));
  bridge::StatementPackageAdmissionReservationRequest reservation_request;
  reservation_request.receipt = receipt;
  reservation_request.canonical_payload_bytes = submission.stream.data();
  reservation_request.canonical_payload_size = submission.stream.size();
  reservation_request.payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle reservation_handle;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  sb_engine_result_t reservation_result = nullptr;
  Check(bridge::AcquireStatementPackageAdmissionReservation(&reservation_request, &reservation_handle,
                                                            &reservation_view, &reservation_result) ==
            SB_ENGINE_STATUS_OK,
        "Q125: the engine reserves the create package for admission");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);

  server::ServerSblrAdmissionRequest admission;
  admission.encoded_sblr_container = submission.container;
  admission.encoded_execution_envelope = submission.ingress;
  admission.admitted_parser_package_uuid = parser_uuid;
  admission.admitted_parser_package_version_major = 1;
  admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  admission.authenticated_principal_uuid = Identity(fixture.principal_uuid);
  admission.catalog_snapshot_uuid = view.statement_metadata_snapshot_uuid;
  admission.engine_mga_statement_uuid = view.statement_uuid;
  admission.engine_mga_snapshot_uuid = view.statement_snapshot_uuid;
  admission.catalog_epoch = view.catalog_generation_id;
  admission.security_epoch = view.security_epoch;
  admission.resource_epoch = view.resource_epoch;
  admission.route_snapshot_uuid = view.optimizer_route_snapshot_uuid;
  admission.route_epoch = view.optimizer_route_epoch;
  admission.route_generation = view.optimizer_route_generation;
  admission.security_snapshot_uuid = view.security_context_uuid;
  admission.security_observation_generation = view.security_epoch;
  admission.route_snapshot_engine_owned = true;
  admission.security_snapshot_engine_owned = true;
  admission.package_reservation_handle = reservation_handle.opaque_id;
  admission.reserved_payload_kind = server::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = reservation_view.payload_size;
  admission.reserved_record_count = reservation_view.record_count;
  admission.reserved_resource_policy_generation = reservation_view.resource_policy_generation;
  admission.reserved_payload_sha256 = reservation_view.payload_sha256;
  const auto admitted = server::AdmitServerSblrEnvelope(admission);
  for (const auto& diagnostic : admitted.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  outcome.admitted = admitted.admitted && admitted.admission_token;
  Check(outcome.admitted, "Q125: the server's gateway admits the create package (the route reaches the engine)");
  const auto& token = *admitted.admission_token;

  bridge::StatementContextDispatchRequest dispatch;
  dispatch.receipt = receipt; dispatch.engine_session = session;
  dispatch.canonical_container_bytes = token.canonical_container_bytes;
  dispatch.canonical_execution_envelope_bytes = token.canonical_execution_envelope_bytes;
  dispatch.canonical_operation_bytes = token.canonical_operation_bytes;
  dispatch.container_sha256 = token.container_sha256;
  dispatch.execution_envelope_sha256 = token.execution_envelope_sha256;
  dispatch.operation_sha256 = token.operation_sha256;
  dispatch.admission_binding_sha256 = token.admission_binding_sha256;
  dispatch.authenticated_principal_uuid = token.authenticated_principal_uuid;
  dispatch.catalog_snapshot_uuid = token.catalog_snapshot_uuid;
  dispatch.engine_mga_statement_uuid = token.engine_mga_statement_uuid;
  dispatch.engine_mga_snapshot_uuid = token.engine_mga_snapshot_uuid;
  dispatch.catalog_epoch = token.catalog_epoch;
  dispatch.security_epoch = token.security_epoch;
  dispatch.resource_epoch = token.resource_epoch;
  dispatch.package_admission_reservation = reservation_handle;
  dispatch.admitted_payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  dispatch.gateway_evidence.source = bridge::StatementGatewayEvidenceSource::kLocalObserved;
  dispatch.gateway_evidence.disposition = bridge::StatementGatewayDisposition::kPassThrough;
  dispatch.gateway_evidence.provider_observation_generation = token.gateway_evidence.provider_observation_generation;
  dispatch.gateway_evidence.canonical_payload_sha256 = token.gateway_evidence.canonical_payload_sha256;
  dispatch.gateway_evidence.cluster_context_active = token.gateway_evidence.cluster_context_active;
  dispatch.gateway_evidence.cluster_transaction_active = token.gateway_evidence.cluster_transaction_active;
  dispatch.gateway_evidence.route_fence_present = token.gateway_evidence.route_fence_present;
  dispatch.gateway_evidence.route_snapshot_uuid = token.gateway_evidence.route_snapshot_uuid;
  dispatch.gateway_evidence.route_epoch = token.gateway_evidence.route_epoch;
  dispatch.gateway_evidence.route_generation = token.gateway_evidence.route_generation;
  dispatch.gateway_evidence.security_snapshot_uuid = token.gateway_evidence.security_snapshot_uuid;
  dispatch.gateway_evidence.security_epoch = token.gateway_evidence.security_epoch;
  dispatch.gateway_evidence.security_observation_generation =
      token.gateway_evidence.security_observation_generation;
  dispatch.package_executor_evidence.begin_executor_id = token.package_executor_evidence.begin_executor_id;
  dispatch.package_executor_evidence.end_executor_id = token.package_executor_evidence.end_executor_id;
  dispatch.package_executor_evidence.registry_snapshot_uuid = token.package_executor_evidence.registry_snapshot_uuid;
  dispatch.package_executor_evidence.executor_evidence_generation =
      token.package_executor_evidence.executor_evidence_generation;
  dispatch.package_executor_evidence.canonical_payload_sha256 =
      token.package_executor_evidence.canonical_payload_sha256;

  *before = Listing(fixture.directory);
  // Markers for the strace check (strace_q125.sh): these two stat calls of
  // paths that never exist bracket the dispatch in the syscall trace.
  (void)std::filesystem::exists(fixture.directory / "q125-dispatch-begin");
  sb_engine_result_t result = nullptr;
  outcome.status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
  (void)std::filesystem::exists(fixture.directory / "q125-dispatch-end");
  if (result != nullptr) {
    sb_engine_diagnostic_set_view_t diagnostics{};
    if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK &&
        diagnostics.diagnostic_count != 0)
      outcome.code.assign(diagnostics.diagnostics[0].symbolic_code.data,
                          diagnostics.diagnostics[0].symbolic_code.size_bytes);
    (void)sb_engine_result_release(result);
  }
  std::cerr << "Q125 session route: status=" << sb_engine_status_name(outcome.status) << " code=" << outcome.code
            << '\n';
  (void)bridge::ReleaseStatementContextReceipt(receipt);
  sb_engine_session_end_params_v1_t end{};
  end.struct_size = sizeof(end);
  end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  end.rollback_active_transactions = 1;
  end.cancel_open_results = 1;
  Check(sb_engine_session_end(session, &end, nullptr) == SB_ENGINE_STATUS_OK, "Q125: the session ends");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  (void)api::EngineRollbackTransaction(rollback);
  return outcome;
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

    Check(!issued.statement_receipt_uuid.is_nil(),
          "Q132: the receipt's engine context carries its statement receipt (the marker the dispatcher reads)");

    // As issued: the create targets the handle's own (existing) database.
    // Q132 = 2 (owner 2026-11-14, SB_CREATE_DATABASE_NOT_ON_SESSION_ROUTE):
    // DispatchSblrOperation refuses a create whose context carries a
    // statement receipt, before any file work. Was (Q113, recorded): the
    // create reached the O_EXCL open of the handle's existing database and
    // was refused there (native_database_file_open_refused:), past the pack
    // check; the database was untouched, as it still is.
    {
      const auto before = std::filesystem::file_size(path);
      const auto as_issued = sblr::DispatchSblrOperation({issued, NativeCreateEnvelope("orders"), NativeOptions()});
      if (!RefusedNotOnSessionRoute(as_issued)) Dump(as_issued);
      Check(RefusedNotOnSessionRoute(as_issued) && std::filesystem::file_size(path) == before,
            "Q132: with the receipt context unchanged, DispatchSblrOperation refuses the create "
            "DATABASE.CREATE_NOT_ON_SESSION_ROUTE; the handle's database is untouched");
      std::printf("Q132 receipt context unchanged: refused DATABASE.CREATE_NOT_ON_SESSION_ROUTE, database untouched\n");
    }

    // Q132 = 2: the same context with a fresh target path is refused too, no
    // file. Was (Q113/Q125, relabelled "direct dispatcher, not the session
    // route"): "the handle's root drives a native CREATE DATABASE through the
    // in-process SBLR dispatcher to success", one file created.
    const auto dir = fresh("handle-root");
    {
      const auto refused = DispatchCreate(issued, (dir / "created.sdb").string());
      if (!RefusedNotOnSessionRoute(refused)) Dump(refused);
      Check(RefusedNotOnSessionRoute(refused) && std::filesystem::is_empty(dir),
            "Q132: a create with a copy of the receipt context and a swapped path is refused "
            "DATABASE.CREATE_NOT_ON_SESSION_ROUTE; no file (was: created, one file)");
      std::printf("Q132 receipt context, swapped path: refused DATABASE.CREATE_NOT_ON_SESSION_ROUTE, no file\n");
    }
    // The positive create, with no receipt: a context without a statement
    // receipt (the server's stand-in), carrying the root the handle issued.
    // This shows the engine creates with that value; it does not show a
    // production route carrying it there (none does: see the header, Q132).
    {
      const auto created = DispatchCreate(ReceiptFreeContext(issued.server_policy_seed_pack_root),
                                          (dir / "created.sdb").string());
      if (!created.api_result.ok) Dump(created);
      Check(created.envelope_validated && created.dispatched_to_api && created.api_result.ok &&
                std::filesystem::exists(dir / "created.sdb") &&
                std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 1,
            "Q132: with no receipt, the root the handle issued creates a native database through "
            "DispatchSblrOperation (the value is carried over by this test, not by a route)");
      std::printf("Q132 receipt-free context with the handle's root: created, one file\n");
    }

    auto client_option = NativeOptions();
    client_option.option_envelopes.push_back("policy_seed_pack_root:/client/chosen/pack");
    const auto client_dir = fresh("client-option");
    // Q132 = 2: with the receipt context the create is refused before the
    // option is read. Was (Q104/Q113): refused
    // DATABASE.CREATE_POLICY_PACK_CLIENT_REFUSED with the handle's root
    // present. That refusal is now shown on the receipt-free context.
    {
      const auto refused = DispatchCreate(issued, (client_dir / "created.sdb").string(), client_option);
      Check(RefusedNotOnSessionRoute(refused) && std::filesystem::is_empty(client_dir),
            "Q132: a client pack option on the receipt context: refused DATABASE.CREATE_NOT_ON_SESSION_ROUTE, no file");
      const auto receipt_free = DispatchCreate(ReceiptFreeContext(issued.server_policy_seed_pack_root),
                                               (client_dir / "created.sdb").string(), client_option);
      Check(!receipt_free.api_result.ok &&
                HasDiagnostic(receipt_free.api_result, "DATABASE.CREATE_POLICY_PACK_CLIENT_REFUSED") &&
                std::filesystem::is_empty(client_dir),
            "Q104/Q113: a client option naming the pack is still refused, with the handle's root present");
    }
    std::printf("Q113 client pack option: refused\n");

    // 5. Q125 = 3 (owner 2026-11-14, SB_CREATE_DATABASE_NOT_ON_SESSION_ROUTE):
    // through the real session route, CREATE DATABASE is refused, v2 native
    // and v1 legacy alike, before any file work: nothing appears under the
    // fixture directory and no file there changes size.
    // (Observed before Q125 with this same check: the gateway admitted the
    // package and DispatchStatementContextReceipt refused it UNSUPPORTED,
    // SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH; no create ran. The Q113
    // "refused at the O_EXCL open" note above describes the direct
    // dispatcher, not this route.)
    const auto session_route = [&](const char* label, sblr::SblrOperand operand) {
      std::vector<std::pair<std::string, std::uintmax_t>> before;
      const auto outcome = SessionRouteCreate(fixture, engine, std::move(operand), &before);
      Check(outcome.admitted && outcome.status != SB_ENGINE_STATUS_OK &&
                outcome.code == "DATABASE.CREATE_NOT_ON_SESSION_ROUTE",
            "Q125: CREATE DATABASE on the session route is refused DATABASE.CREATE_NOT_ON_SESSION_ROUTE");
      Check(Listing(fixture.directory) == before,
            "Q125: the refused create made no file and grew none (the handle's database included)");
      std::printf("Q125 session route %s: refused DATABASE.CREATE_NOT_ON_SESSION_ROUTE, no file\n", label);
    };
    session_route("v2-native", V2Operand("orders"));
    session_route("v1-legacy", V1Operand());
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
    // Q132 = 2 (owner 2026-11-14): the receipt context's create is refused
    // DATABASE.CREATE_NOT_ON_SESSION_ROUTE before the pack is looked at. Was:
    // "with no root on the handle, native create refuses
    // CREATE_POLICY_PACK_REQUIRED and makes no file". That refusal is shown
    // on a receipt-free context carrying the (empty) root the handle issued.
    const auto refused = DispatchCreate(issued, (dir / "created.sdb").string());
    Check(RefusedNotOnSessionRoute(refused) && std::filesystem::is_empty(dir),
          "Q132: a receipt context's create is refused DATABASE.CREATE_NOT_ON_SESSION_ROUTE, no file");
    const auto receipt_free = DispatchCreate(ReceiptFreeContext(issued.server_policy_seed_pack_root),
                                             (dir / "created.sdb").string());
    Check(receipt_free.dispatched_to_api && !receipt_free.api_result.ok &&
              HasDiagnostic(receipt_free.api_result, "DATABASE.CREATE_POLICY_PACK_REQUIRED") &&
              std::filesystem::is_empty(dir),
          "Q113: with no root on the handle, native create refuses CREATE_POLICY_PACK_REQUIRED and makes no file");
    std::printf("Q113 %s handle without a root: receipt context refused NOT_ON_SESSION_ROUTE; "
                "receipt-free CREATE_POLICY_PACK_REQUIRED, no file\n", label);
    Check(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK, "close");
  };
  no_root("c-abi-open", false);
  no_root("served-open-no-root", true);

  std::printf("q113_engine_handle_policy_pack_root PASS (%d checks)\n", checks);
  return 0;
}
