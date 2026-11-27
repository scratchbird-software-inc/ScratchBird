// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_SELECT_ONE (thin-slice S4g part 2; owner Q302 / Q310 / controller Q1-Q4, 2026-12-06).
// `SELECT 1 AS one` (a statement that REALLY lowers to query.evaluate_projection, 0x040E) runs natively, in process, on a
// production-created single-file database: SBsql text -> the real parser/lowering -> the production scalar-projection
// submission (BuildCanonicalScalarProjectionSubmission, the builder the parser wire uses) -> a native receipt (the HandleAuthHandoff
// session and its transaction) -> package reservation -> server admission -> DispatchStatementContextReceipt -> the executor ->
// a RESULT MESSAGE VECTOR. Reuses native_receipt_test.cpp's helpers (the serve process, the approved session, the receipt acquire).
#define SB_NATIVE_RECEIPT_FIXTURE_ONLY 1
#include "native_receipt_test.cpp"

#include "../support/binary_uuid_fixture.hpp"
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "lowering/lowering.hpp"
#include "server/sblr_admission.hpp"
#include "wire/native_result_vector.hpp"
#include "wire/public_result_packet.hpp"
#include "wire/sbsql_test_wire.hpp"

namespace scratchbird::parser::sbsql {
std::optional<ipc::ParserCanonicalSblrSubmission> BuildCanonicalScalarProjectionSubmissionForTest(
    const SblrEnvelope&, const ipc::ParserStatementContext&, const SessionContext&);
}

namespace {
namespace ps = scratchbird::parser::sbsql;
namespace pipc = scratchbird::parser::ipc;

std::string HexOf(const std::uint8_t* data, std::size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < size; ++i) { out.push_back(digits[data[i] >> 4]); out.push_back(digits[data[i] & 15]); }
  return out;
}

// The parser side: the REAL SBsql parser and lowering, and the production submission builder.
struct ParserSide {
  ps::ParserConfig config;
  ps::SessionContext session;
};
ParserSide MakeParserSide(const Approved& a) {
  ParserSide side;
  side.config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000003114");
  side.config.bundle_contract_id = "sbp_sbsql@native-select-one";
  side.config.build_id = "native-select-one";
  side.session.authenticated = true;
  side.session.admitted_parser_package_uuid = side.config.parser_uuid;
  side.session.session_uuid = a.session();
  side.session.connection_uuid = p::Uuid{a.connection};
  side.session.database_uuid = served.facts->database_uuid;
  side.session.authenticated_user_uuid = p::Uuid{a.record->effective_user_uuid};
  side.session.dialect_profile_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000003116");
  side.session.admitted_dialect_profile_uuid = side.session.dialect_profile_uuid;
  side.session.admitted_parser_package_version_major = 1;
  side.session.catalog_epoch = 1;
  side.session.security_policy_epoch = 1;
  side.session.descriptor_epoch = 1;
  return side;
}
ps::SblrEnvelope Lower(const ParserSide& side, const std::string& sql) {
  const auto cst = ps::BuildCst(sql);
  auto bound = ps::BindAst(ps::BuildAst(cst), cst, side.config, side.session, {});
  return ps::LowerToSblr(bound, cst, side.session);
}

// S4h part 1: the parser's own output names for a projection, from the aliases it lowered (projection_N_name), as the wire layer reads them.
std::vector<std::string> ProjectionNames(const ps::SblrEnvelope& lowered) {
  std::vector<std::string> names;
  for (std::size_t i = 0;; ++i) {
    const std::string key = "\"projection_" + std::to_string(i) + "_name\":\"";
    const auto at = lowered.payload.find(key);
    if (at == std::string::npos) break;
    const auto start = at + key.size();
    const auto end = lowered.payload.find('"', start);
    if (end == std::string::npos) break;
    names.push_back(lowered.payload.substr(start, end - start));
  }
  return names;
}
// The REAL engine vector through the SBsql parser's converter, then through the production row decoder the client path uses.
struct ParserRows {
  bool converted = false;
  std::string code, detail;
  std::vector<std::uint32_t> oids;
  std::vector<std::vector<std::optional<std::string>>> rows;
  std::vector<std::string> names;
};
ParserRows ParserBuildsRows(const ParserSide& side, const std::string& sql, const std::string& engine_payload) {
  ParserRows out;
  const auto lowered = Lower(side, sql);
  out.names = ProjectionNames(lowered);
  const auto conv = ps::ConvertNativeScalarProjectionVector(engine_payload, out.names);
  out.converted = conv.status == ps::NativeVectorConversionStatus::converted;
  out.code = conv.refusal_code;
  out.detail = conv.refusal_detail;
  if (out.converted) (void)ps::DecodePublicResultRowsForTest(conv.payload, &out.oids, &out.rows);
  return out;
}
pipc::ParserStatementContext ParserContextFrom(const Acquired& r, const Approved& a) {
  pipc::ParserStatementContext context;
  const auto& view = r.view;
  context.acquired = true;
  context.statement_uuid = view.statement_uuid;
  context.transaction = {a.approval.transaction_order, a.approval.transaction_uuid};
  context.statement_snapshot_uuid = view.statement_snapshot_uuid;
  context.statement_metadata_snapshot_uuid = view.statement_metadata_snapshot_uuid;
  context.catalog_epoch_uuid = view.catalog_epoch_uuid;
  context.security_context_uuid = view.security_context_uuid;
  context.bound_ast_uuid = view.bound_ast_uuid;
  context.preliminary_statement_catalog_generation = view.catalog_generation_id;
  context.preliminary_security_epoch = view.security_epoch;
  context.preliminary_resource_epoch = view.resource_epoch;
  for (const auto& d : view.descriptor_profiles) {
    pipc::ParserStatementContext::DescriptorProfile profile;
    profile.profile_kind = static_cast<std::uint8_t>(d.profile_kind);
    profile.slot = d.slot;
    profile.descriptor_uuid = d.descriptor_uuid;
    profile.type_uuid = d.type_uuid;
    profile.collation_uuid = d.collation_uuid;
    profile.nullable = d.nullable;
    profile.width = d.width;
    profile.precision = d.precision;
    profile.scale = d.scale;
    context.descriptor_profiles.push_back(profile);
  }
  return context;
}

struct Dispatched {
  sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
  sb_engine_result_t result = nullptr;
  std::string code;                 // the first diagnostic's symbolic code ("" when none)
  std::string reason;               // lowering/submission/admission refusal before the engine saw it
  std::vector<std::uint8_t> operation_bytes;
};
// One statement through the production chain, on a native receipt. `lowered_operation` is reported for the trace.
Dispatched RunStatement(const std::string& sql, PublicSession& ps_, Acquired& receipt, const Approved& approved, std::string* lowered_operation = nullptr) {
  Dispatched out;
  const auto side = MakeParserSide(approved);
  const auto lowered = Lower(side, sql);
  if (lowered_operation) *lowered_operation = lowered.operation_id + "/" + lowered.sblr_opcode;
  if (lowered.messages.has_errors()) { out.reason = "lowering refused"; return out; }
  const auto context = ParserContextFrom(receipt, approved);
  const auto submission = ps::BuildCanonicalScalarProjectionSubmissionForTest(lowered, context, side.session);
  if (!submission) { out.reason = "submission refused"; return out; }
  out.operation_bytes = submission->canonical_operation_bytes;
  bridge::StatementPackageAdmissionReservationRequest rr;
  rr.receipt = receipt.receipt;
  rr.canonical_payload_bytes = submission->canonical_operation_bytes.data();
  rr.canonical_payload_size = submission->canonical_operation_bytes.size();
  rr.payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle reservation;
  bridge::StatementPackageAdmissionReservationView rv;
  sb_engine_result_t rres = nullptr;
  const auto rstatus = bridge::AcquireStatementPackageAdmissionReservation(&rr, &reservation, &rv, &rres);
  if (rstatus != SB_ENGINE_STATUS_OK) { out.status = rstatus; out.code = Code(rres); out.reason = "reservation refused"; return out; }
  if (rres) (void)sb_engine_result_release(rres);
  const auto& view = receipt.view;
  srv::ServerSblrAdmissionRequest admission;
  admission.encoded_sblr_container.assign(submission->canonical_container_bytes.begin(), submission->canonical_container_bytes.end());
  admission.encoded_execution_envelope.assign(submission->canonical_execution_envelope_bytes.begin(), submission->canonical_execution_envelope_bytes.end());
  admission.admitted_parser_package_uuid = side.session.admitted_parser_package_uuid;
  admission.admitted_parser_package_version_major = 1;
  admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  admission.authenticated_principal_uuid = api::EngineUuid{approved.record->effective_user_uuid};
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
  admission.package_reservation_handle = reservation.opaque_id;
  admission.reserved_payload_kind = srv::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = rv.payload_size;
  admission.reserved_record_count = rv.record_count;
  admission.reserved_resource_policy_generation = rv.resource_policy_generation;
  admission.reserved_payload_sha256 = rv.payload_sha256;
  const auto admitted = srv::AdmitServerSblrEnvelope(admission);
  if (!admitted.admitted || !admitted.admission_token) {
    out.reason = "admission refused:";
    for (const auto& d : admitted.diagnostics) out.reason += " [" + d.code + " / " + d.safe_message + "]";
    (void)bridge::ReleaseStatementPackageAdmissionReservation(reservation, bridge::StatementPackageReservationReleaseReason::kRelease);
    return out;
  }
  const auto& token = admitted.admission_token;
  bridge::StatementContextDispatchRequest dispatch;
  dispatch.receipt = receipt.receipt;
  dispatch.engine_session = ps_.session;
  dispatch.canonical_container_bytes = token->canonical_container_bytes;
  dispatch.canonical_execution_envelope_bytes = token->canonical_execution_envelope_bytes;
  dispatch.canonical_operation_bytes = token->canonical_operation_bytes;
  dispatch.container_sha256 = token->container_sha256;
  dispatch.execution_envelope_sha256 = token->execution_envelope_sha256;
  dispatch.operation_sha256 = token->operation_sha256;
  dispatch.admission_binding_sha256 = token->admission_binding_sha256;
  dispatch.authenticated_principal_uuid = token->authenticated_principal_uuid;
  dispatch.catalog_snapshot_uuid = token->catalog_snapshot_uuid;
  dispatch.engine_mga_statement_uuid = token->engine_mga_statement_uuid;
  dispatch.engine_mga_snapshot_uuid = token->engine_mga_snapshot_uuid;
  dispatch.catalog_epoch = token->catalog_epoch;
  dispatch.security_epoch = token->security_epoch;
  dispatch.resource_epoch = token->resource_epoch;
  dispatch.package_admission_reservation = reservation;
  dispatch.admitted_payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  dispatch.gateway_evidence.source = bridge::StatementGatewayEvidenceSource::kLocalObserved;
  dispatch.gateway_evidence.disposition = bridge::StatementGatewayDisposition::kPassThrough;
  dispatch.gateway_evidence.provider_observation_generation = token->gateway_evidence.provider_observation_generation;
  dispatch.gateway_evidence.canonical_payload_sha256 = token->gateway_evidence.canonical_payload_sha256;
  dispatch.gateway_evidence.cluster_context_active = token->gateway_evidence.cluster_context_active;
  dispatch.gateway_evidence.cluster_transaction_active = token->gateway_evidence.cluster_transaction_active;
  dispatch.gateway_evidence.route_fence_present = token->gateway_evidence.route_fence_present;
  dispatch.gateway_evidence.route_snapshot_uuid = token->gateway_evidence.route_snapshot_uuid;
  dispatch.gateway_evidence.route_epoch = token->gateway_evidence.route_epoch;
  dispatch.gateway_evidence.route_generation = token->gateway_evidence.route_generation;
  dispatch.gateway_evidence.security_snapshot_uuid = token->gateway_evidence.security_snapshot_uuid;
  dispatch.gateway_evidence.security_epoch = token->gateway_evidence.security_epoch;
  dispatch.gateway_evidence.security_observation_generation = token->gateway_evidence.security_observation_generation;
  dispatch.package_executor_evidence.begin_executor_id = token->package_executor_evidence.begin_executor_id;
  dispatch.package_executor_evidence.end_executor_id = token->package_executor_evidence.end_executor_id;
  dispatch.package_executor_evidence.registry_snapshot_uuid = token->package_executor_evidence.registry_snapshot_uuid;
  dispatch.package_executor_evidence.executor_evidence_generation = token->package_executor_evidence.executor_evidence_generation;
  dispatch.package_executor_evidence.canonical_payload_sha256 = token->package_executor_evidence.canonical_payload_sha256;
  out.status = bridge::DispatchStatementContextReceipt(&dispatch, &out.result);
  if (out.result) {
    sb_engine_diagnostic_set_view_t ds{};
    (void)sb_engine_result_diagnostics(out.result, &ds);
    if (ds.diagnostic_count) out.code = std::string(View(ds.diagnostics[0].symbolic_code));
  }
  return out;
}

// ---- the vector the engine returns (S4g part 2, owner Q310) ---------------------------------------------------------------
struct Vector {
  bool ok = false;
  std::string operation_id, result_kind;
  std::uint64_t column_count = 0, row_count = 0;
  std::vector<p::Uuid> column_descriptor, column_datatype, column_datatype_descriptor;
  std::vector<std::uint64_t> column_datatype_generation;
  std::vector<std::int64_t> values;  // row-major
  std::size_t field_count = 0;
};
std::uint64_t LE64(const std::string& bytes) {
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < 8 && i < bytes.size(); ++i) out |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  return out;
}
p::Uuid UuidOf(const std::string& bytes) {
  p::Uuid id{};
  if (bytes.size() == 16) std::copy(bytes.begin(), bytes.end(), id.bytes.begin());
  return id;
}
// Decodes the SBRES002 framing (the decoder the parser already has) and checks the layout field by field. Returns ok=false on any deviation.
Vector DecodeVector(const std::string& payload) {
  namespace pr = scratchbird::wire::public_result;
  Vector out;
  std::vector<pr::Field> fields;
  if (!pr::Decode(payload, &fields)) return out;
  out.field_count = fields.size();
  const auto find = [&](const std::string& name) -> const pr::Field* {
    const pr::Field* found = nullptr;
    for (const auto& f : fields) if (f.name == name) { if (found) return nullptr; found = &f; }
    return found;
  };
  const auto* operation = find("operation_id");
  const auto* kind = find("result_kind");
  const auto* columns = find("column_count");
  const auto* rows = find("row_count");
  if (!operation || !kind || !columns || !rows || columns->kind != pr::Kind::unsigned_integer || rows->kind != pr::Kind::unsigned_integer) return out;
  out.operation_id = operation->value;
  out.result_kind = kind->value;
  out.column_count = LE64(columns->value);
  out.row_count = LE64(rows->value);
  if (out.column_count == 0 || out.column_count > 64 || out.row_count == 0 || out.row_count > 64) return out;
  // exactly: 4 header fields + 4 per column + 1 per value; nothing else (no name, no evidence, no rendered row)
  if (fields.size() != 4 + 4 * out.column_count + out.column_count * out.row_count) return out;
  for (std::uint64_t c = 0; c < out.column_count; ++c) {
    const auto n = std::to_string(c);
    const auto* d = find("column_descriptor[" + n + "]");
    const auto* t = find("column_datatype[" + n + "]");
    const auto* td = find("column_datatype_descriptor[" + n + "]");
    const auto* g = find("column_datatype_generation[" + n + "]");
    if (!d || !t || !td || !g || d->kind != pr::Kind::uuid || t->kind != pr::Kind::uuid || td->kind != pr::Kind::uuid || g->kind != pr::Kind::unsigned_integer) return out;
    out.column_descriptor.push_back(UuidOf(d->value));
    out.column_datatype.push_back(UuidOf(t->value));
    out.column_datatype_descriptor.push_back(UuidOf(td->value));
    out.column_datatype_generation.push_back(LE64(g->value));
  }
  for (std::uint64_t r = 0; r < out.row_count; ++r)
    for (std::uint64_t c = 0; c < out.column_count; ++c) {
      const auto* v = find("value[" + std::to_string(r) + "." + std::to_string(c) + "]");
      if (!v || v->kind != pr::Kind::signed_integer || v->value.size() != 8) return out;
      out.values.push_back(static_cast<std::int64_t>(LE64(v->value)));
    }
  out.ok = true;
  return out;
}

// ---- file evidence around a statement ---------------------------------------------------------------------------------------
std::map<std::string, std::string> ListingOf(const fs::path& root) {
  std::map<std::string, std::string> out;
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
    const auto rel = fs::relative(it->path(), root).string();
    out[rel] = it->is_directory(error) ? "dir" : "file:" + std::to_string(it->file_size(error));
  }
  return out;
}
struct FileEvidence {
  std::vector<p::byte> image;
  std::map<std::string, std::string> listing;
  std::uint64_t next = 0;
  FileEvidence() {
    image = Image(served.path);
    listing = ListingOf(served.directory);
    // SB_ROUTEB_COST_READALL (2026-12-22). OLD: Next came from a SECOND whole-file read (off::ReadFile(path): read + copy + decode). NEW: the same offline
    // decode (same chain readers) over pread of only the pages it needs (off::ReadInventory); the byte-identity image above is read once, with one read().
    next = off::ReadInventory(served.path, served.facts->page_size_bytes).counters.next_transaction;
  }
};
// `what`'s statement left the data file byte-identical, the directory listing unchanged (no TIP/ledger write, no side file) and Next unchanged.
// SB_ROUTEB_COST_READALL (2026-12-22). OLD: CheckUntouched built a whole second FileEvidence (a second 64 MB image at 128K pages, allocated, zero-filled and
// compared with operator==). NEW: the file is read again in 1 MiB chunks, each compared byte for byte with the same range of the earlier image (the same
// comparison, every byte, plus the size; no second whole-file allocation); the listing and Next are read exactly as before.
bool FileEqualsImage(const std::filesystem::path& path, const std::vector<p::byte>& image) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return image.empty();
  std::vector<char> chunk(1u << 20);
  std::size_t at = 0;
  while (in) {
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) break;
    if (at + got > image.size() || std::memcmp(chunk.data(), image.data() + at, got) != 0) return false;
    at += got;
  }
  return at == image.size();
}
void CheckUntouched(const FileEvidence& before, const std::string& what) {
  const auto listing = ListingOf(served.directory);
  const auto next = off::ReadInventory(served.path, served.facts->page_size_bytes).counters.next_transaction;
  Check(FileEqualsImage(served.path, before.image), what + ": the data file is byte-identical");
  Check(listing == before.listing && listing.size() == 1, what + ": the directory listing is unchanged (only the data file)");
  Check(next == before.next, what + ": the transaction counter (Next) is unchanged");
}

std::string FieldOf(const bridge::EngineDiagnosticSnapshot& snapshot, const std::string& key) {
  for (const auto& f : snapshot.fields) if (f.key == key) return f.value;
  return {};
}
struct Refusal {
  sb_engine_status_t status = SB_ENGINE_STATUS_OK;
  std::string code, opcode_code, operation, requires_token;
  bool has_table_identity = false;
  std::string keys;
};
Refusal ReadRefusal(sb_engine_status_t status, sb_engine_result_t& result) {
  Refusal out;
  out.status = status;
  if (result) {
    bridge::EngineDiagnosticSnapshot snapshot;
    if (bridge::CopyEngineDiagnosticSnapshot(result, 0, &snapshot)) {
      out.code = snapshot.code;
      out.opcode_code = FieldOf(snapshot, "opcode_code");
      out.operation = FieldOf(snapshot, "operation");
      out.requires_token = FieldOf(snapshot, "requires");
      for (const auto& f : snapshot.fields) out.keys += f.key + "(" + std::to_string(f.value.size()) + ") ";
      const auto table = FieldOf(snapshot, "availability_table_uuid");
      out.has_table_identity = table == std::string(reinterpret_cast<const char*>(api::NativeExecutorAvailabilityTableUuid().bytes.data()), 16);
    }
    (void)sb_engine_result_release(result);
    result = nullptr;
  }
  return out;
}

// One statement, prepared up to (not including) the dispatch, so a test can dispatch it later (a replay, an ended session).
struct Prepared {
  bool ok = false;
  std::string reason;
  bridge::StatementContextDispatchRequest dispatch;
  bridge::StatementPackageAdmissionReservationHandle reservation;
  std::string lowered_operation;
};
Prepared Prepare(const std::string& sql, PublicSession& ps_, Acquired& receipt, const Approved& approved) {
  Prepared out;
  const auto side = MakeParserSide(approved);
  const auto lowered = Lower(side, sql);
  out.lowered_operation = lowered.operation_id + "/" + lowered.sblr_opcode;
  if (lowered.messages.has_errors()) { out.reason = "lowering refused"; return out; }
  const auto context = ParserContextFrom(receipt, approved);
  const auto submission = ps::BuildCanonicalScalarProjectionSubmissionForTest(lowered, context, side.session);
  if (!submission) { out.reason = "submission refused"; return out; }
  bridge::StatementPackageAdmissionReservationRequest rr;
  rr.receipt = receipt.receipt;
  rr.canonical_payload_bytes = submission->canonical_operation_bytes.data();
  rr.canonical_payload_size = submission->canonical_operation_bytes.size();
  rr.payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationView rv;
  sb_engine_result_t rres = nullptr;
  const auto rstatus = bridge::AcquireStatementPackageAdmissionReservation(&rr, &out.reservation, &rv, &rres);
  if (rstatus != SB_ENGINE_STATUS_OK) { out.reason = "reservation refused: " + Code(rres); return out; }
  if (rres) (void)sb_engine_result_release(rres);
  const auto& view = receipt.view;
  srv::ServerSblrAdmissionRequest admission;
  admission.encoded_sblr_container.assign(submission->canonical_container_bytes.begin(), submission->canonical_container_bytes.end());
  admission.encoded_execution_envelope.assign(submission->canonical_execution_envelope_bytes.begin(), submission->canonical_execution_envelope_bytes.end());
  admission.admitted_parser_package_uuid = side.session.admitted_parser_package_uuid;
  admission.admitted_parser_package_version_major = 1;
  admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  admission.authenticated_principal_uuid = api::EngineUuid{approved.record->effective_user_uuid};
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
  admission.package_reservation_handle = out.reservation.opaque_id;
  admission.reserved_payload_kind = srv::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = rv.payload_size;
  admission.reserved_record_count = rv.record_count;
  admission.reserved_resource_policy_generation = rv.resource_policy_generation;
  admission.reserved_payload_sha256 = rv.payload_sha256;
  const auto admitted = srv::AdmitServerSblrEnvelope(admission);
  if (!admitted.admitted || !admitted.admission_token) {
    out.reason = "admission refused";
    for (const auto& d : admitted.diagnostics) out.reason += " [" + d.code + "]";
    (void)bridge::ReleaseStatementPackageAdmissionReservation(out.reservation, bridge::StatementPackageReservationReleaseReason::kRelease);
    return out;
  }
  const auto& token = admitted.admission_token;
  auto& dispatch = out.dispatch;
  dispatch.receipt = receipt.receipt;
  dispatch.engine_session = ps_.session;
  dispatch.canonical_container_bytes = token->canonical_container_bytes;
  dispatch.canonical_execution_envelope_bytes = token->canonical_execution_envelope_bytes;
  dispatch.canonical_operation_bytes = token->canonical_operation_bytes;
  dispatch.container_sha256 = token->container_sha256;
  dispatch.execution_envelope_sha256 = token->execution_envelope_sha256;
  dispatch.operation_sha256 = token->operation_sha256;
  dispatch.admission_binding_sha256 = token->admission_binding_sha256;
  dispatch.authenticated_principal_uuid = token->authenticated_principal_uuid;
  dispatch.catalog_snapshot_uuid = token->catalog_snapshot_uuid;
  dispatch.engine_mga_statement_uuid = token->engine_mga_statement_uuid;
  dispatch.engine_mga_snapshot_uuid = token->engine_mga_snapshot_uuid;
  dispatch.catalog_epoch = token->catalog_epoch;
  dispatch.security_epoch = token->security_epoch;
  dispatch.resource_epoch = token->resource_epoch;
  dispatch.package_admission_reservation = out.reservation;
  dispatch.admitted_payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  dispatch.gateway_evidence.source = bridge::StatementGatewayEvidenceSource::kLocalObserved;
  dispatch.gateway_evidence.disposition = bridge::StatementGatewayDisposition::kPassThrough;
  dispatch.gateway_evidence.provider_observation_generation = token->gateway_evidence.provider_observation_generation;
  dispatch.gateway_evidence.canonical_payload_sha256 = token->gateway_evidence.canonical_payload_sha256;
  dispatch.gateway_evidence.cluster_context_active = token->gateway_evidence.cluster_context_active;
  dispatch.gateway_evidence.cluster_transaction_active = token->gateway_evidence.cluster_transaction_active;
  dispatch.gateway_evidence.route_fence_present = token->gateway_evidence.route_fence_present;
  dispatch.gateway_evidence.route_snapshot_uuid = token->gateway_evidence.route_snapshot_uuid;
  dispatch.gateway_evidence.route_epoch = token->gateway_evidence.route_epoch;
  dispatch.gateway_evidence.route_generation = token->gateway_evidence.route_generation;
  dispatch.gateway_evidence.security_snapshot_uuid = token->gateway_evidence.security_snapshot_uuid;
  dispatch.gateway_evidence.security_epoch = token->gateway_evidence.security_epoch;
  dispatch.gateway_evidence.security_observation_generation = token->gateway_evidence.security_observation_generation;
  dispatch.package_executor_evidence.begin_executor_id = token->package_executor_evidence.begin_executor_id;
  dispatch.package_executor_evidence.end_executor_id = token->package_executor_evidence.end_executor_id;
  dispatch.package_executor_evidence.registry_snapshot_uuid = token->package_executor_evidence.registry_snapshot_uuid;
  dispatch.package_executor_evidence.executor_evidence_generation = token->package_executor_evidence.executor_evidence_generation;
  dispatch.package_executor_evidence.canonical_payload_sha256 = token->package_executor_evidence.canonical_payload_sha256;
  out.ok = true;
  return out;
}
struct Outcome {
  sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
  Vector vector;
  Refusal refusal;
  std::size_t payload_size = 0;
  std::string payload;
  std::uint64_t rows_produced = 0;
};
// Dispatches a prepared statement and reads either the vector or the refusal; releases the result.
Outcome Dispatch(Prepared& prepared) {
  Outcome out;
  sb_engine_result_t result = nullptr;
  out.status = bridge::DispatchStatementContextReceipt(&prepared.dispatch, &result);
  if (out.status == SB_ENGINE_STATUS_OK && result) {
    sb_engine_execution_summary_view_v1_t summary{};
    (void)sb_engine_result_summary(result, &summary);
    out.rows_produced = summary.rows_produced;
    sb_engine_string_view_t payload{};
    if (sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK) out.payload.assign(View(payload));
    out.payload_size = out.payload.size();
    out.vector = DecodeVector(out.payload);
    (void)sb_engine_result_release(result);
  } else {
    out.refusal = ReadRefusal(out.status, result);
  }
  return out;
}
Outcome Run(const std::string& sql, PublicSession& ps_, Acquired& receipt, const Approved& approved, std::string* lowered = nullptr) {
  auto prepared = Prepare(sql, ps_, receipt, approved);
  if (lowered) *lowered = prepared.lowered_operation;
  Check(prepared.ok, "'" + sql + "' lowers, packages and is admitted (" + prepared.reason + ")");
  return Dispatch(prepared);
}

// The datatype cohort's int64 identity for a receipt's context, read from the compiled registry independently of the engine's encoder.
struct CohortInt64 {
  p::Uuid type_uuid, descriptor_uuid;
  std::uint64_t generation = 0;
};
CohortInt64 CohortInt64Of(const api::EngineRequestContext& context) {
  CohortInt64 out;
  int found = 0;
  for (const auto& row3 : scratchbird::core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = row3.legacy_fields;
    if (row.codec_id == "datatype.int64.le.v1" && row.canonical_name == "bigint" && row.catalog_snapshot_uuid == context.datatype_catalog_snapshot_uuid &&
        row.catalog_generation == context.datatype_catalog_generation && row.registry_generation == context.datatype_registry_generation) {
      out = {row.type_uuid, row.descriptor_uuid, row.descriptor_generation};
      ++found;
    }
  }
  Check(found == 1, "exactly one int64 identity in the receipt's datatype cohort (" + std::to_string(found) + ")");
  return out;
}

// A hand-framed SBOS envelope (package begin, the members, package end) for the GATE's refused shapes. Only the opcode codes matter to the gate (it
// refuses before any decode, hash or reservation consume), so each member is encoded as a valid CREATE TABLE operation and its u16 opcode code (record
// byte 16) is then set to the wanted code; the checksum is not recomputed (nothing below the gate would accept it, and none is reached).
std::vector<std::uint8_t> FramedStream(const std::vector<std::uint16_t>& member_codes, const bridge::StatementContextReceiptView& view) {
  namespace sb = scratchbird::engine::sblr;
  const auto parser = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000003114");
  const auto frame = [&](bool begin) {
    auto op = sb::MakeSblrEnvelope(begin ? "engine.op.package_begin" : "engine.op.package_end", begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END", "select_one.gate");
    op.opcode_code = begin ? 1 : 2;
    op.result_shape = "void";
    op.diagnostic_shape = "diagnostic_vector";
    op.parser_package_uuid = parser;
    op.registry_snapshot_uuid = view.catalog_epoch_uuid;
    op.parser_resolved_names_to_uuids = true;
    sb::SblrOperand operand;
    operand.ordinal = 1;
    operand.type = begin ? "package.header" : "package.footer";
    operand.name = "package_descriptor";
    operand.value_kind = sb::SblrValueKind::descriptor_ref;
    operand.value_body.assign(view.bound_ast_uuid.bytes.begin(), view.bound_ast_uuid.bytes.end());
    op.operands.push_back(std::move(operand));
    return op;
  };
  sb::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = view.bound_ast_uuid;
  stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
  stream.operations.push_back(frame(true));
  const auto* entry = sb::LookupSblrOperation("engine.op.ddl_create_table");
  Check(entry != nullptr, "the registry knows engine.op.ddl_create_table");
  for (std::size_t i = 0; i < member_codes.size(); ++i) {
    auto member = sb::MakeSblrEnvelope(entry->operation_id, entry->opcode, "select_one.gate");
    member.opcode_code = entry->code;
    member.result_shape = entry->result_contract;
    member.diagnostic_shape = "diagnostic_vector";
    member.parser_package_uuid = parser;
    member.registry_snapshot_uuid = view.catalog_epoch_uuid;
    member.parser_resolved_names_to_uuids = true;
    stream.operations.push_back(std::move(member));
  }
  stream.operations.push_back(frame(false));
  auto text = sb::EncodeSblrOpcodeStream(stream);
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  // walk the records (header 64 bytes; u64 size + record) and patch the members' codes
  std::size_t offset = 64, index = 0;
  for (std::size_t record = 0; record < stream.operations.size() && offset + 8 <= bytes.size(); ++record) {
    std::uint64_t size = 0;
    for (unsigned i = 0; i < 8; ++i) size |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    if (record >= 1 && record <= member_codes.size()) {
      bytes[offset + 8 + 16] = static_cast<std::uint8_t>(member_codes[index] & 0xff);
      bytes[offset + 8 + 17] = static_cast<std::uint8_t>(member_codes[index] >> 8);
      ++index;
    }
    offset += 8 + static_cast<std::size_t>(size);
  }
  return bytes;
}

std::string RowKey(const api::SblrExecutorAvailabilityRowIdentity& row) { return row.executor_id + "#" + std::to_string(row.opcode_code); }

// RECORDING (owner Q302): the recorder GRANTS every row it is asked about while `SELECT 1 AS one` runs, and what it logs is what the allow-list holds.
void RecordState() {
  auto s1 = Approve();
  auto ps1 = OpenPublicFor(*s1);
  auto c1 = ContextFor(*s1, served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id);
  auto r1 = Acquire(*ps1, c1, s1->approval.transaction_uuid);
  Check(r1.ok(), "a native receipt: " + r1.code);
  const auto record = [&](const std::string& sql, std::vector<std::string> expected, bool expect_ok) {
    api::StartNativeExecutorRecordingForTest(true);
    auto out = Run(sql, *ps1, r1, *s1);
    const auto rows = api::StopNativeExecutorRecordingForTest();
    std::vector<std::string> keys;
    for (const auto& r : rows) keys.push_back(RowKey(r.row));
    std::printf("RECORDED %-28s status=%d lookups: ", sql.c_str(), static_cast<int>(out.status));
    for (const auto& k : keys) std::printf("%s ", k.c_str());
    std::printf("\n");
    Check(keys == expected, "'" + sql + "': the recorder logged exactly the expected rows");
    Check((out.status == SB_ENGINE_STATUS_OK) == expect_ok, "'" + sql + "': recorded run status");
    for (const auto& r : rows) {
      const bool listed = RowKey(r.row) == "engine.op.literal#3" || RowKey(r.row) == RowKey({api::kSblrNameResolveExecutorId, api::kSblrNameResolveOpcodeCode, "", "", "", 0});
      Check(r.in_allow_list == listed, "the log's allow-list flag matches the table for " + RowKey(r.row));
    }
    if (out.vector.ok) {
      Check(out.vector.operation_id == "query.evaluate_projection", "the granted run still returns the vector");
    }
  };
  // the allow-list is exactly the rows logged for the shape that runs: engine.op.literal (one lookup per literal), nothing else
  record("SELECT 1 AS one", {"engine.op.literal#3"}, true);
  record("SELECT 1 AS one, 2 AS two", {"engine.op.literal#3", "engine.op.literal#3"}, true);
  // shapes that ask for rows NOT on the list: the recorder shows which row each needs (the operator-call row for `1 + 0`)
  record("SELECT 1 + 0 AS s", {"engine.op.operator_call#1025"}, true);
  const auto& allowed = api::NativeExecutorAvailabilityAllowList();
  std::set<std::string> table;
  for (const auto& row : allowed) table.insert(RowKey(row));
  Check(table == std::set<std::string>({"engine.op.literal#3", "engine.op.name_resolve#4865"}),
        "the production allow-list is the logged literal row plus the name-resolve row of part 1, nothing else");
  Release(r1);
  ps1.reset();
  Disconnect(*s1);
}

// THE TEST (production table: no recorder is active).
void SelectOneState() {
  const auto g0 = off::ReadInventory(served.path, served.facts->page_size_bytes);
  (void)g0;
  const auto catalog_generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  // the production tables, pinned
  {
    std::set<std::string> table;
    for (const auto& row : api::NativeExecutorAvailabilityAllowList()) table.insert(RowKey(row));
    Check(table == std::set<std::string>({"engine.op.literal#3", "engine.op.name_resolve#4865"}), "the production allow-list holds exactly the literal and name-resolve rows");
    for (const std::uint16_t code : {1, 2, 0x040E}) Check(api::NativeExecutorOpcodeCodeAllowed(code), "dispatch root opcode " + std::to_string(code) + " is admitted");
    for (const std::uint16_t code : {3, 0x0101, 0x030E, 0x0601, 0x0604, 0x1207, 0x1301, 1024, 1025, 1280, 1286, 0})
      Check(!api::NativeExecutorOpcodeCodeAllowed(code), "opcode " + std::to_string(code) + " is not a dispatch root");
  }
  auto s1 = Approve();
  auto ps1 = OpenPublicFor(*s1);
  auto c1 = ContextFor(*s1, catalog_generation);
  auto r1 = Acquire(*ps1, c1, s1->approval.transaction_uuid);
  Check(r1.ok(), "a native receipt for session 1: " + r1.code);
  const auto cohort = CohortInt64Of(CopyContext(r1));
  const auto order1 = s1->approval.transaction_order;
  const auto active = [&](std::uint64_t order) { return off::ReadInventory(served.path, served.facts->page_size_bytes).words.at(order).state == pgs::TipStateCode::active; };
  Check(active(order1), "session 1's transaction is active before the statements");

  // ---- the allow-listed shape runs, repeatedly, and returns the message vector -----------------------------------------------
  std::set<std::string> descriptors_seen;
  for (int round = 0; round < 3; ++round) {
    const FileEvidence before;
    std::string lowered;
    const auto got = Run("SELECT 1 AS one", *ps1, r1, *s1, &lowered);
    CheckUntouched(before, "SELECT 1 AS one (round " + std::to_string(round) + ")");
    Check(lowered == "query.evaluate_projection/SBLR_QUERY_EVALUATE_PROJECTION", "SELECT 1 AS one lowers to the 0x040E scalar projection: " + lowered);
    Check(got.status == SB_ENGINE_STATUS_OK && got.vector.ok, "SELECT 1 AS one dispatches and returns a decodable vector (status " + std::to_string(got.status) + " " + got.refusal.code + ")");
    Check(got.rows_produced == 1 && got.vector.operation_id == "query.evaluate_projection" && got.vector.result_kind == "native_scalar_projection_vector" &&
              got.vector.column_count == 1 && got.vector.row_count == 1 && got.vector.values == std::vector<std::int64_t>{1},
          "the vector is one row, one column, the int64 value 1");
    Check(got.vector.column_datatype[0] == cohort.type_uuid && got.vector.column_datatype_descriptor[0] == cohort.descriptor_uuid &&
              got.vector.column_datatype_generation[0] == cohort.generation,
          "the datatype UUID, descriptor UUID and generation equal the datatype cohort's int64 identity");
    Check(!got.vector.column_descriptor[0].is_nil() && got.vector.column_descriptor[0] != got.vector.column_datatype[0] &&
              descriptors_seen.insert(Hex(got.vector.column_descriptor[0])).second,
          "the column descriptor identity is a fresh engine-issued uuid per result column");
    // no column name and no rendered row: the only text fields are the two machine tokens
    {
      std::vector<scratchbird::wire::public_result::Field> fields;
      Check(scratchbird::wire::public_result::Decode(got.payload, &fields), "the payload decodes with the existing SBRES002 decoder");
      int text_fields = 0;
      for (const auto& f : fields) {
        if (f.kind == scratchbird::wire::public_result::Kind::text) ++text_fields;
        Check(f.kind != scratchbird::wire::public_result::Kind::row && f.kind != scratchbird::wire::public_result::Kind::evidence,
              "no rendered row and no evidence field in the vector");
      }
      Check(text_fields == 2, "exactly two text fields (operation_id, result_kind: machine tokens); no column name, no rendered value");
      Check(got.payload.find("one") == std::string::npos, "the parser's column name does not appear in the payload");
    }
    // S4h part 1: the REAL engine vector, through the SBsql parser's converter: the parser builds the row and the name `one` from its own alias.
    {
      const auto rows = ParserBuildsRows(MakeParserSide(*s1), "SELECT 1 AS one", got.payload);
      Check(rows.converted && rows.names == std::vector<std::string>{"one"} && rows.rows.size() == 1 && rows.rows[0].size() == 1 && rows.rows[0][0] &&
                *rows.rows[0][0] == "1" && rows.oids.size() == 1,
            "the parser builds the client row (one = 1) from the real engine vector: " + rows.code + " " + rows.detail);
    }
  }
  // other values and shapes of the same row set
  {
    const FileEvidence before;
    const auto two = Run("SELECT 2 AS two", *ps1, r1, *s1);
    CheckUntouched(before, "SELECT 2 AS two");
    Check(two.status == SB_ENGINE_STATUS_OK && two.vector.ok && two.vector.values == std::vector<std::int64_t>{2},
          "SELECT 2 AS two runs: the SAME literal row, the value 2");
    const auto pair = Run("SELECT 1 AS one, 2 AS two", *ps1, r1, *s1);
    CheckUntouched(before, "SELECT 1 AS one, 2 AS two");
    Check(pair.status == SB_ENGINE_STATUS_OK && pair.vector.ok && pair.vector.column_count == 2 && pair.vector.values == std::vector<std::int64_t>({1, 2}) &&
              pair.vector.column_descriptor[0] != pair.vector.column_descriptor[1],
          "two literal columns: two descriptors, values 1 and 2");
    {
      const auto rows2 = ParserBuildsRows(MakeParserSide(*s1), "SELECT 2 AS two", two.payload);
      Check(rows2.converted && rows2.names == std::vector<std::string>{"two"} && rows2.rows.size() == 1 && rows2.rows[0][0] && *rows2.rows[0][0] == "2",
            "the parser builds (two = 2) from the real engine vector: " + rows2.code);
      const auto rows_pair = ParserBuildsRows(MakeParserSide(*s1), "SELECT 1 AS one, 2 AS two", pair.payload);
      Check(rows_pair.converted && rows_pair.names == std::vector<std::string>({"one", "two"}) && rows_pair.rows.size() == 1 && rows_pair.rows[0].size() == 2 &&
                *rows_pair.rows[0][0] == "1" && *rows_pair.rows[0][1] == "2",
            "the parser builds (one = 1, two = 2) from the real two-column engine vector: " + rows_pair.code);
      // the same engine bytes with another statement's alias: the NAME is the parser's, not the engine's
      const auto renamed = ps::ConvertNativeScalarProjectionVector(two.payload, {"renamed"});
      Check(renamed.status == ps::NativeVectorConversionStatus::converted && renamed.payload.find("renamed") != std::string::npos &&
                renamed.payload.find("two") == std::string::npos,
            "the column name is whatever the parser supplies");
    }
  }

  // ---- refused shapes: the table's own code, the opcode field, nothing written --------------------------------------------------
  {
    const FileEvidence before;
    auto op = Prepare("SELECT 1 + 0 AS s", *ps1, r1, *s1);
    Check(op.ok, "SELECT 1 + 0 AS s lowers (0x040E, an operator expression) and is admitted: " + op.reason);
    const auto got = Dispatch(op);
    CheckUntouched(before, "SELECT 1 + 0 AS s");
    // the executor reports the table's refusal through the projection failure path, with the engine's own status for a refused request (not UNSUPPORTED)
    Check(got.status != SB_ENGINE_STATUS_OK && got.refusal.code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && got.refusal.opcode_code == "1025",
          "SELECT 1 + 0 refuses SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED for the operator-call row (opcode_code 1025): status " + std::to_string(got.status) + " " + got.refusal.code + " " + got.refusal.opcode_code + " keys: " + got.refusal.keys);
    Check(!got.vector.ok, "no vector was produced");
  }
  {
    const FileEvidence before;
    const auto got = Run("SELECT 'a' AS t", *ps1, r1, *s1);
    CheckUntouched(before, "SELECT 'a' AS t");
    // the literal row is the same row for every literal: the table cannot tell 'a' from 1. The VECTOR builder refuses it (only int64 is built).
    Check(got.status == SB_ENGINE_STATUS_UNSUPPORTED && got.refusal.code == "ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT" && !got.vector.ok,
          "SELECT 'a' refuses ENGINE.NATIVE.RESULT_VECTOR_TYPE_NOT_BUILT (no text reaches a native client): " + got.refusal.code);
  }
  {
    // the bare form is the NEXT slice (route B): the parser lowers it to query.execute with a refusal of its own (no engine binding context), and the
    // gate refuses a hand-framed query.execute envelope. Nothing is written.
    // ROUTE B (2026-12-11), annotation only: the assertions below stay TRUE (no cohort: the lowering refuses; a hand-framed query.execute that is not the
    // exact literal-VALUES template is refused at the gate with opcode_code 4615). What changed is the REAL route: with the engine cohort the production
    // pipeline now produces the exact template and the gate ADMITS it (native_dispatch_is_exact_literal_values_query); native_wire_exchange.py carries the
    // flipped pins (OLD: bare `SELECT 1` = SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED; NEW: the table with the parser's default column name and the value).
    const auto side = MakeParserSide(*s1);
    const auto bare = Lower(side, "SELECT 1");
    Check(bare.operation_id == "query.execute" && bare.messages.has_errors(), "bare SELECT 1 lowers to query.execute (typed VALUES DAG), refused without an engine binding cohort: route B, not this slice");
  }
  struct Shape { const char* what; std::vector<std::uint16_t> ops; const char* opcode_code; };
  const std::vector<Shape> shapes = {
      {"a DDL (CREATE TABLE, 0x0601)", {0x0601}, "1537"},
      {"a DDL (CREATE INDEX, 1540)", {1540}, "1540"},
      {"a DML (INSERT, 0x030E)", {0x030E}, "782"},
      {"a commit (0x0101)", {0x0101}, "257"},
      {"the typed VALUES query (query.execute 0x1207)", {0x1207}, "4615"},
      {"a name resolve (0x1301)", {0x1301}, "4865"},
      {"the literal opcode as a root (3)", {3}, "3"},
      {"a projection followed by a DDL", {0x040E, 0x0601}, "1537"},
  };
  for (const auto& shape : shapes) {
    const FileEvidence before;
    bridge::StatementContextDispatchRequest dispatch;
    dispatch.receipt = r1.receipt;
    dispatch.canonical_operation_bytes = FramedStream(shape.ops, r1.view);
    Check(!dispatch.canonical_operation_bytes.empty(), std::string(shape.what) + ": the framed envelope encodes");
    sb_engine_result_t result = nullptr;
    const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
    const auto refusal = ReadRefusal(status, result);
    CheckUntouched(before, shape.what);
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && refusal.code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED" && refusal.opcode_code == shape.opcode_code,
          std::string(shape.what) + ": the gate refuses SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED with the member opcode_code: " + refusal.code + " " + refusal.opcode_code + " keys: " + refusal.keys + " status " + std::to_string(status));
  }
  // a bare non-framing request keeps the S4f refusal (no operation bytes: nothing to admit)
  {
    const FileEvidence before;
    bridge::StatementContextDispatchRequest dispatch;
    dispatch.receipt = r1.receipt;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
    const auto refusal = ReadRefusal(status, result);
    CheckUntouched(before, "an empty dispatch");
    Check(status == SB_ENGINE_STATUS_UNSUPPORTED && refusal.code == "ENGINE.NATIVE.OPERATION_NOT_BUILT" && refusal.operation == "dispatch_statement_context",
          "an empty dispatch keeps ENGINE.NATIVE.OPERATION_NOT_BUILT: " + refusal.code);
  }
  // two statements in one envelope (SELECT 1; SELECT 2): the engine refuses a package of other than 3 records
  {
    const FileEvidence before;
    bridge::StatementContextDispatchRequest dispatch;
    dispatch.receipt = r1.receipt;
    dispatch.canonical_operation_bytes = FramedStream({0x040E, 0x040E}, r1.view);
    sb_engine_result_t result = nullptr;
    const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
    const auto refusal = ReadRefusal(status, result);
    CheckUntouched(before, "two projections in one envelope");
    Check(status != SB_ENGINE_STATUS_OK, "two projections in one envelope (SELECT 1; SELECT 2) are refused past the gate: " + refusal.code);
  }

  // ---- the transaction is still active and owned; Next did not move; another receipt is acquired --------------------------------
  Check(active(order1), "session 1's transaction is STILL active after every statement (no commit from a statement)");
  {
    auto again = Acquire(*ps1, c1, s1->approval.transaction_uuid);
    Check(again.ok(), "another receipt is acquired for the same transaction afterwards: " + again.code);
    const auto next = Run("SELECT 3 AS three", *ps1, again, *s1);
    Check(next.status == SB_ENGINE_STATUS_OK && next.vector.ok && next.vector.values == std::vector<std::int64_t>{3}, "and a statement runs on it");
    Release(again);
  }

  // ---- a second session: independent -------------------------------------------------------------------------------------------
  auto s2 = Approve();
  auto ps2 = OpenPublicFor(*s2);
  auto c2 = ContextFor(*s2, catalog_generation);
  auto r2 = Acquire(*ps2, c2, s2->approval.transaction_uuid);
  Check(r2.ok(), "a native receipt for session 2: " + r2.code);
  {
    const FileEvidence before;
    const auto a = Run("SELECT 11 AS a", *ps1, r1, *s1);
    const auto b = Run("SELECT 22 AS b", *ps2, r2, *s2);
    CheckUntouched(before, "two sessions' statements");
    Check(a.vector.ok && b.vector.ok && a.vector.values == std::vector<std::int64_t>{11} && b.vector.values == std::vector<std::int64_t>{22} &&
              a.vector.column_descriptor[0] != b.vector.column_descriptor[0],
          "two sessions' statements are independent: each gets its own value and its own column descriptor");
    // session 1's receipt cannot be used by session 2's engine session
    auto cross = Prepare("SELECT 5 AS five", *ps1, r1, *s1);
    cross.dispatch.engine_session = ps2->session;
    const auto crossed = Dispatch(cross);
    std::printf("CROSS-SESSION dispatch: status=%d vector=%d code=%s\n", static_cast<int>(crossed.status), crossed.vector.ok ? 1 : 0, crossed.refusal.code.c_str());
    Check(crossed.status != SB_ENGINE_STATUS_OK && !crossed.vector.ok, "session 1's receipt cannot be dispatched through session 2's engine session: " + crossed.refusal.code);
    (void)bridge::ReleaseStatementPackageAdmissionReservation(cross.reservation, bridge::StatementPackageReservationReleaseReason::kRelease);
  }

  // ---- the receipt of an ended transaction cannot dispatch -----------------------------------------------------------------------
  {
    auto s3 = Approve();
    auto ps3 = OpenPublicFor(*s3);
    auto c3 = ContextFor(*s3, catalog_generation);
    auto r3 = Acquire(*ps3, c3, s3->approval.transaction_uuid);
    Check(r3.ok(), "a native receipt for session 3");
    auto prepared = Prepare("SELECT 7 AS seven", *ps3, r3, *s3);
    Check(prepared.ok, "a statement for session 3 is admitted");
    const auto order3 = s3->approval.transaction_order;
    ps3.reset();       // the engine session ends: it rolls its transaction back and drops the receipt
    Disconnect(*s3);   // the loop ends the session (S4e): the transaction is rolled back in the file
    const auto word = off::ReadInventory(served.path, served.facts->page_size_bytes).words.at(order3).state;
    Check(word != pgs::TipStateCode::active, "the disconnected session's transaction is no longer active in the file");
    prepared.dispatch.engine_session = nullptr;
    const FileEvidence before;
    const auto got = Dispatch(prepared);
    CheckUntouched(before, "the ended transaction's receipt");
    Check(got.status != SB_ENGINE_STATUS_OK && !got.vector.ok, "the receipt of an ended transaction cannot dispatch: " + got.refusal.code);
  }

  // ---- disconnect rolls the transaction back, as in S4e ---------------------------------------------------------------------------
  Release(r1);
  Release(r2);
  ps1.reset();
  ps2.reset();
  Disconnect(*s1);
  Disconnect(*s2);
  const auto end = off::ReadInventory(served.path, served.facts->page_size_bytes);
  Check(end.words.at(order1).state != pgs::TipStateCode::active && end.words.at(s2->approval.transaction_order).state != pgs::TipStateCode::active,
        "both transactions are ended by the disconnects");
}

// THE STRACE STATE: the statements' region holds only the engine (the test's own file reads sit in prepare and verify).
void TraceState() {
  Mark("approve");
  auto s1 = Approve();
  auto ps1 = OpenPublicFor(*s1);
  const auto catalog_generation = served.facts->receipt_source->ReadCatalogEpochs().catalog_generation_id;
  auto c1 = ContextFor(*s1, catalog_generation);
  Mark("prepare");
  const auto image_before = Image(served.path);
  const auto listing_before = ListingOf(served.directory);
  Mark("statement");
  auto r1 = Acquire(*ps1, c1, s1->approval.transaction_uuid, false);
  Check(r1.ok(), "a receipt (traced)");
  for (int i = 0; i < 2; ++i) {
    const auto got = Run("SELECT 1 AS one", *ps1, r1, *s1);
    Check(got.status == SB_ENGINE_STATUS_OK && got.vector.ok && got.vector.values == std::vector<std::int64_t>{1}, "SELECT 1 AS one (traced)");
  }
  {
    auto refused = Prepare("SELECT 1 + 0 AS s", *ps1, r1, *s1);
    const auto got = Dispatch(refused);
    Check(got.refusal.code == "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED", "the refused shape (traced)");
  }
  Release(r1);
  Mark("verify");
  Check(Image(served.path) == image_before, "the statements wrote nothing: the file's bytes are identical");
  Check(ListingOf(served.directory) == listing_before, "and no file appeared");
  Mark("disconnect");
  ps1.reset();
  Disconnect(*s1);
}

#if !defined(SB_NATIVE_SELECT_ONE_FIXTURE_ONLY)
std::optional<int> ExtraReceiptStateForTest(const std::string& state, const Offline&) {
  if (state == "record") { RecordState(); return 0; }
  if (state == "select_one") { SelectOneState(); return 0; }
  if (state == "trace") { TraceState(); return 0; }
  return std::nullopt;
}
#endif
}  // namespace

#if !defined(SB_NATIVE_SELECT_ONE_FIXTURE_ONLY)
int main(int argc, char** argv) {
  if (argc == 7 && std::string(argv[1]) == "serve") {
    served.bootstrap = FromHex(argv[5]);
    served.service = FromHex(argv[6]);
    return ServeMode(argv[2], argv[3]);
  }
  if (argc == 3 && std::string(argv[1]) == "size") {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    for (const char* state : {"record", "select_one"}) RunState(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])), state);
    artifacts.Cleanup();
    std::printf("native_select_one: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
#endif  // SB_NATIVE_SELECT_ONE_FIXTURE_ONLY
