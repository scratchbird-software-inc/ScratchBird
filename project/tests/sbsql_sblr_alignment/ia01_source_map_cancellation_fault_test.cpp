// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"
#include "engine/sblr/sblr_source_map_runtime.hpp"
#include "engine/sblr/sblr_source_artifact_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <cstdlib>
#include <fstream>

namespace sm=scratchbird::engine::sblr;

namespace {
class ScopedSourceMapTrace {
 public:
  explicit ScopedSourceMapTrace(const std::string& path) {
    const auto* previous = std::getenv(kName);
    had_previous_ = previous != nullptr;
    if (previous) previous_ = previous;
    Set(path.c_str());
  }
  ~ScopedSourceMapTrace() {
    if (had_previous_) Set(previous_.c_str());
    else {
#ifdef _WIN32
      (void)_putenv_s(kName, "");
#else
      (void)unsetenv(kName);
#endif
    }
  }
 private:
  static constexpr const char* kName = "SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE";
  static void Set(const char* value) {
#ifdef _WIN32
    Require(_putenv_s(kName, value) == 0, "set scoped trace path");
#else
    Require(setenv(kName, value, 1) == 0, "set scoped trace path");
#endif
  }
  bool had_previous_ = false;
  std::string previous_;
};
std::string ReadTrace(const std::string& path) {
  if (!std::filesystem::exists(path)) return {};
  std::ifstream source(path, std::ios::binary);
  Require(source.good(), "read source map trace");
  // Preflight observations are permitted before dispatch. Only executor
  // success evidence is governed by the SOURCE_MAP publication barrier.
  std::string evidence;
  std::string line;
  while (std::getline(source, line)) {
    if (line.rfind("layer=source_map_executor\t", 0) == 0) {
      evidence += line;
      evidence += '\n';
    }
  }
  return evidence;
}
unsigned authority_checks = 0;
void AcceptedSourceMap(const Fixture& fixture,
                       bridge::StatementContextReceiptHandle receipt,
                       sm::SblrSourceMapIssueRequestV1 request) {
  const auto journal=fixture.database_path.string()+".sb.sblr_source_map_registry.v1";
  const auto before=std::filesystem::exists(journal)?std::filesystem::file_size(journal):0;
  const auto bytes=sm::EncodeSblrSourceMapIssueRequestV1(&request);
  Require(!bytes.empty(),"positive source map request encoding");
  Bytes output;sb_engine_result_t result=nullptr;
  Require(bridge::IssueStatementSourceMapDescriptorV1(receipt,bytes,&output,&result)==SB_ENGINE_STATUS_OK,
          "retained/redacted source map issuance");
  if(result)(void)sb_engine_result_release(result);
  Require(std::filesystem::exists(journal)&&std::filesystem::file_size(journal)>before,
          "source map success lacked durable descriptor records");
  sm::SblrSourceMapIssueResultV1 response;std::string detail;
  Require(sm::DecodeSblrSourceMapIssueResultV1(output.data(),output.size(),&response,&detail),
          "source map issuance response decode");
  const auto decoded=sm::DecodeSblrSourceMapDescriptorVectorV1(
      response.canonical_smvd.data(),response.canonical_smvd.size());
  Require(decoded.status==sm::SblrSourceMapDecodeStatusV1::ok &&
          decoded.vector.statement_receipt_uuid==request.statement_receipt_uuid &&
          decoded.vector.entries.size()==1 &&
          decoded.vector.entries[0].source_artifact_uuid==request.entries[0].source_artifact_uuid &&
          decoded.vector.entries[0].source_artifact_generation==request.entries[0].source_artifact_generation &&
          decoded.vector.entries[0].redaction_class==request.entries[0].redaction_class,
          "engine issued artifact/receipt/redaction echo mismatch");
}
void RefusedSourceMap(const Fixture& fixture,
                      bridge::StatementContextReceiptHandle receipt,
                      sm::SblrSourceMapIssueRequestV1 request,
                      sb_engine_status_t expected, std::string_view key) {
  auto bytes = sm::EncodeSblrSourceMapIssueRequestV1(&request);
  Require(!bytes.empty(), "authority negative fixture failed canonical encoding");
  const auto journal = fixture.database_path.string()+".sb.sblr_source_map_registry.v1";
  const auto size = [&] { return std::filesystem::exists(journal)
      ? std::filesystem::file_size(journal) : std::uintmax_t(0); };
  const auto before = size();
  Bytes output{0xff}; sb_engine_result_t result = nullptr;
  const auto status = bridge::IssueStatementSourceMapDescriptorV1(
      receipt, bytes, &output, &result);
  if (status != expected) std::cerr << "source map refusal: " << key << " status=" << status << '\n';
  Require(status == expected, "source map authority refusal status");
  Require(output.empty(), "refused source map left successful output");
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(result && sb_engine_result_diagnostics(result, &diagnostics)==SB_ENGINE_STATUS_OK &&
          diagnostics.diagnostic_count==1, "source map refusal diagnostic count");
  const auto& message = diagnostics.diagnostics[0].message_key;
  Require(std::string_view(message.data,message.size_bytes)==key,
          "source map refusal diagnostic precedence");
  const auto& code=diagnostics.diagnostics[0].symbolic_code;
  const std::string_view expected_code=expected==SB_ENGINE_STATUS_SECURITY_DENIED
      ? "SECURITY.ACCESS_DENIED" : expected==SB_ENGINE_STATUS_CONFLICT
      ? "SBLR.SOURCE_MAP.STALE" : "SBLR.OPERAND_INVALID";
  Require(std::string_view(code.data,code.size_bytes)==expected_code,
          "source map authority diagnostic code");
  (void)sb_engine_result_release(result);
  Require(size()==before, "refused source map published journal records");
  ++authority_checks;
}
bridge::StatementSourceArtifactRetentionV1 RetainArtifact(
    bridge::StatementContextReceiptHandle receipt,
    const bridge::StatementContextReceiptView& view,
    const std::string& parser, const sm::SblrSourceMapEntryV1& entry,
    sm::SblrSourceArtifactRedactionClassV1 redaction) {
  sm::SblrSourceArtifactMapV1 artifact;
  artifact.artifact_uuid=entry.source_artifact_uuid;
  artifact.sblr_envelope_uuid=RawUuid(view.statement_uuid);
  artifact.dialect_family_uuid=RawUuid(parser);
  artifact.parser_package_uuid=RawUuid(parser);
  artifact.redaction_class=redaction;
  sm::SblrSourceArtifactSpanV1 span;
  span.source_span_id=1;span.node_id=entry.node_id;
  span.byte_start=entry.byte_offset;span.byte_length=entry.byte_length;
  span.line_start=span.line_end=entry.line;
  span.column_start=entry.column;span.column_end=entry.column+entry.byte_length;
  artifact.source_spans={span};
  sm::SblrSourceArtifactRetainRequestV1 retain;
  retain.authenticated_receipt_uuid=RawUuid(view.receipt_uuid);
  retain.sblr_envelope_uuid=artifact.sblr_envelope_uuid;
  retain.artifact_uuid=artifact.artifact_uuid;
  retain.canonical_artifact_bytes=sm::EncodeSblrSourceArtifactMapV1(artifact);
  retain.declared_size=retain.canonical_artifact_bytes.size();
  retain.crc32c=wire::SblrCrc32c(retain.canonical_artifact_bytes.data(),retain.declared_size);
  retain.artifact_sha256=sm::HashSblrSourceArtifactBytesV1(
      retain.canonical_artifact_bytes.data(),retain.declared_size);
  retain.redaction_class=artifact.redaction_class;
  retain.decompile_policy=artifact.decompile_policy;
  const auto bytes=sm::EncodeSblrSourceArtifactRetainRequestV1(retain);
  Require(!bytes.empty(),"source map artifact retention encoding");
  bridge::StatementSourceArtifactRetentionV1 retained;
  sb_engine_result_t result=nullptr;
  const auto status=bridge::RetainStatementSourceArtifactV1(
      receipt,bytes.data(),bytes.size(),&retained,&result);
  if(status!=SB_ENGINE_STATUS_OK)std::cerr<<retained.failure_message_key<<':'<<retained.failure_detail<<'\n';
  Require(status==SB_ENGINE_STATUS_OK,"source map artifact retention");
  if(result)(void)sb_engine_result_release(result);
  Require(retained.retention_generation!=0,"artifact retention generation");
  return retained;
}
}

int main(){auto fixture=CreateFixture();PublicSession session(fixture);std::atomic<unsigned> probes{0}; std::atomic<unsigned> cancel_at{2};
  const auto trace_path = fixture.database_path.string() + ".source_map_barrier.trace";
  std::size_t trace_bytes_before_dispatch = 0;
  bool premature_evidence = false;
  sb_engine_result_t* observed_public_slot = nullptr;
  bool premature_result = false;
  const auto parser_uuid=Text(NewUuid(platform::UuidKind::object,9402));
  auto context=BeginTransaction(fixture,&probes);context.current_package_uuid.canonical=parser_uuid;
  context.query_cancellation_requested=[&] {
    if (probes.fetch_add(1) + 1 != cancel_at.load()) return false;
    premature_evidence = ReadTrace(trace_path).size() != trace_bytes_before_dispatch;
    premature_result = observed_public_slot && *observed_public_slot != nullptr;
    return true;
  };
  bridge::StatementContextAcquireRequest acquire;acquire.engine_context=&context;acquire.exact_transaction_uuid=context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;bridge::StatementContextReceiptView view;sb_engine_result_t result=nullptr;
  Require(bridge::AcquireStatementContextReceipt(session.session,&acquire,&receipt,&view,&result)==SB_ENGINE_STATUS_OK,"002340 receipt acquire failed");if(result)(void)sb_engine_result_release(result);
  sm::SblrSourceMapBoundAstV1 ast;ast.statement_receipt_uuid=RawUuid(view.receipt_uuid);ast.nodes.push_back({7,0,1});const auto smba=sm::EncodeSblrSourceMapBoundAstV1(&ast);
  sm::SblrSourceMapEntryV1 entry;entry.node_id=7;entry.source_artifact_uuid=RawUuid(Text(NewUuid(platform::UuidKind::object,9401)));entry.source_artifact_generation=1;entry.byte_length=1;entry.line=1;entry.column=1;
  sm::SblrSourceMapIssueRequestV1 issue;issue.statement_receipt_uuid=ast.statement_receipt_uuid;issue.registry_snapshot_uuid=RawUuid(view.catalog_epoch_uuid);issue.registry_generation=view.literal_catalog_generation;issue.canonical_bound_ast=smba;issue.entries={entry};auto smrq=sm::EncodeSblrSourceMapIssueRequestV1(&issue);Bytes smrs;

  RefusedSourceMap(fixture,receipt,issue,SB_ENGINE_STATUS_SECURITY_DENIED,"sblr.source_map.hidden");
  auto masked=issue; ++masked.registry_generation;
  RefusedSourceMap(fixture,receipt,masked,SB_ENGINE_STATUS_SECURITY_DENIED,"sblr.source_map.hidden");
  auto retained=RetainArtifact(receipt,view,parser_uuid,entry,sm::SblrSourceArtifactRedactionClassV1::none);
  Require(retained.retention_generation==entry.source_artifact_generation,"retention generation drift");
  auto stale=issue;++stale.entries[0].source_artifact_generation;
  RefusedSourceMap(fixture,receipt,stale,SB_ENGINE_STATUS_CONFLICT,"sblr.source_map.artifact_stale");
  auto mixed=stale;auto mixed_ast=ast;mixed_ast.nodes.push_back({8,7,1});
  mixed.canonical_bound_ast=sm::EncodeSblrSourceMapBoundAstV1(&mixed_ast);
  auto hidden_entry=entry;hidden_entry.node_id=8;hidden_entry.parent_node_id=7;
  hidden_entry.source_artifact_uuid=RawUuid(Text(NewUuid(platform::UuidKind::object,9404)));
  mixed.entries.push_back(hidden_entry);
  RefusedSourceMap(fixture,receipt,mixed,SB_ENGINE_STATUS_SECURITY_DENIED,"sblr.source_map.hidden");
  for(unsigned field=0;field!=5;++field){
    auto changed=issue;
    if(field==0)++changed.entries[0].byte_offset;
    if(field==1)++changed.entries[0].byte_length;
    if(field==2)++changed.entries[0].line;
    if(field==3)++changed.entries[0].column;
    if(field==4)changed.entries[0].byte_offset=UINT64_MAX;
    RefusedSourceMap(fixture,receipt,changed,SB_ENGINE_STATUS_INVALID_ARGUMENT,"sblr.source_map.artifact_span_invalid");
  }
  auto unmapped=issue;auto other_ast=ast;other_ast.nodes[0].node_id=8;
  unmapped.entries[0].node_id=8;unmapped.canonical_bound_ast=sm::EncodeSblrSourceMapBoundAstV1(&other_ast);
  RefusedSourceMap(fixture,receipt,unmapped,SB_ENGINE_STATUS_INVALID_ARGUMENT,"sblr.source_map.artifact_span_invalid");
  auto protected_entry=entry;protected_entry.source_artifact_uuid=RawUuid(Text(NewUuid(platform::UuidKind::object,9403)));
  RetainArtifact(receipt,view,parser_uuid,protected_entry,sm::SblrSourceArtifactRedactionClassV1::security_redacted);
  auto downgrade=issue;downgrade.entries={protected_entry};
  RefusedSourceMap(fixture,receipt,downgrade,SB_ENGINE_STATUS_SECURITY_DENIED,"sblr.source_map.hidden");
  downgrade.entries[0].redaction_class=3;
  AcceptedSourceMap(fixture,receipt,downgrade);
  bridge::StatementContextReceiptHandle other_receipt;bridge::StatementContextReceiptView other_view;
  Require(bridge::AcquireStatementContextReceipt(session.session,&acquire,&other_receipt,&other_view,&result)==SB_ENGINE_STATUS_OK,"second artifact receipt");
  if(result)(void)sb_engine_result_release(result);
  auto other_issue=issue;other_issue.statement_receipt_uuid=RawUuid(other_view.receipt_uuid);
  auto other_bound=ast;other_bound.statement_receipt_uuid=other_issue.statement_receipt_uuid;
  other_issue.canonical_bound_ast=sm::EncodeSblrSourceMapBoundAstV1(&other_bound);
  other_issue.registry_snapshot_uuid=RawUuid(other_view.catalog_epoch_uuid);
  other_issue.registry_generation=other_view.literal_catalog_generation;
  RefusedSourceMap(fixture,other_receipt,other_issue,SB_ENGINE_STATUS_SECURITY_DENIED,"sblr.source_map.hidden");
  auto absent=other_issue;absent.entries[0].redaction_class=4;
  absent.entries[0].source_artifact_uuid={};absent.entries[0].source_artifact_generation=0;
  absent.entries[0].byte_offset=0;absent.entries[0].byte_length=0;
  absent.entries[0].line=0;absent.entries[0].column=0;
  AcceptedSourceMap(fixture,other_receipt,absent);
  Require(bridge::ReleaseStatementContextReceipt(other_receipt)==SB_ENGINE_STATUS_OK,"release other receipt");
  RefusedSourceMap(fixture,other_receipt,other_issue,SB_ENGINE_STATUS_SECURITY_DENIED,"sblr.source_map.hidden");
  // A second real public session and MGA transaction cannot borrow retention
  // from the first session, even for the same principal and artifact UUID.
  const auto other_session_uuid=NewUuid(platform::UuidKind::object,9405);
  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size=sizeof(session_params);session_params.abi_version=SB_ENGINE_ABI_VERSION_PACKED;
  session_params.effective_user_uuid=PublicUuid(fixture.principal_uuid);
  session_params.session_uuid=PublicUuid(other_session_uuid);
  session_params.default_language_utf8="en";session_params.default_language_size=2;
  session_params.trust_mode=SB_ENGINE_TRUST_SERVER_ISOLATED;
  sb_engine_session_t second_session=nullptr;
  Require(sb_engine_session_begin(session.engine,&session_params,&second_session,nullptr)==SB_ENGINE_STATUS_OK,"second public session");
  api::EngineBeginTransactionRequest second_begin;second_begin.context=context;
  second_begin.context.session_uuid.canonical=Text(other_session_uuid);
  second_begin.context.local_transaction_id=0;second_begin.context.transaction_uuid={};
  second_begin.isolation_level="read_committed";
  const auto second_begun=api::EngineBeginTransaction(second_begin);
  Require(second_begun.ok,"second session MGA begin");
  auto second_context=second_begin.context;
  second_context.local_transaction_id=second_begun.local_transaction_id;
  second_context.transaction_uuid=second_begun.transaction_uuid;
  second_context.snapshot_visible_through_local_transaction_id=second_begun.snapshot_visible_through_local_transaction_id;
  bridge::StatementContextAcquireRequest second_acquire;
  second_acquire.engine_context=&second_context;
  second_acquire.exact_transaction_uuid=second_context.transaction_uuid.canonical;
  Require(bridge::AcquireStatementContextReceipt(second_session,&second_acquire,&other_receipt,&other_view,&result)==SB_ENGINE_STATUS_OK,"cross-session receipt");
  if(result)(void)sb_engine_result_release(result);
  other_issue.statement_receipt_uuid=RawUuid(other_view.receipt_uuid);
  other_bound.statement_receipt_uuid=other_issue.statement_receipt_uuid;
  other_issue.canonical_bound_ast=sm::EncodeSblrSourceMapBoundAstV1(&other_bound);
  other_issue.registry_snapshot_uuid=RawUuid(other_view.catalog_epoch_uuid);
  other_issue.registry_generation=other_view.literal_catalog_generation;
  RefusedSourceMap(fixture,other_receipt,other_issue,SB_ENGINE_STATUS_SECURITY_DENIED,"sblr.source_map.hidden");
  Require(bridge::ReleaseStatementContextReceipt(other_receipt)==SB_ENGINE_STATUS_OK,"cross-session receipt release");
  sb_engine_session_end_params_v1_t end{};end.struct_size=sizeof(end);end.abi_version=SB_ENGINE_ABI_VERSION_PACKED;
  end.rollback_active_transactions=1;end.cancel_open_results=1;
  Require(sb_engine_session_end(second_session,&end,nullptr)==SB_ENGINE_STATUS_OK,"cross-session end");
  api::EngineRollbackTransactionRequest second_rollback;second_rollback.context=second_context;
  Require(api::EngineRollbackTransaction(second_rollback).ok,"cross-session rollback");
  std::cout<<"source_map_artifact_authority negative_cases="<<authority_checks<<" PASS\n";

  Require(bridge::IssueStatementSourceMapDescriptorV1(receipt,smrq,&smrs,&result)==SB_ENGINE_STATUS_OK,"002340 source map issue failed");if(result)(void)sb_engine_result_release(result);sm::SblrSourceMapIssueResultV1 issued;std::string detail;Require(sm::DecodeSblrSourceMapIssueResultV1(smrs.data(),smrs.size(),&issued,&detail),"002340 SMRS invalid");
  auto member=sblr::MakeSblrEnvelope("engine.op.source_map","SBLR_SOURCE_MAP","ia01.source_map.cancel");member.opcode_code=6;member.result_shape="void";member.diagnostic_shape="diagnostic_vector";member.parser_package_uuid=parser_uuid;member.registry_snapshot_uuid=view.catalog_epoch_uuid;member.parser_resolved_names_to_uuids=true;sblr::SblrOperand operand;operand.ordinal=1;operand.type="source_map.vector";operand.name="source_map";operand.value_kind=sblr::SblrValueKind::descriptor_ref;operand.value_body.assign(issued.descriptor_uuid.begin(),issued.descriptor_uuid.end());U64(&operand.value_body,issued.descriptor_generation);member.operands.push_back(std::move(operand));const auto member_validation=sblr::ValidateSblrEnvelope(member);if(!member_validation.ok)for(const auto&d:member_validation.diagnostics)std::cerr<<d.code<<':'<<d.message<<'\n';Require(member_validation.ok,"002340 source map member invalid");
  const auto package=RawUuid(view.bound_ast_uuid);sblr::SblrOpcodeStream package_stream;package_stream.package_descriptor_uuid=view.bound_ast_uuid;package_stream.registry_snapshot_uuid=view.catalog_epoch_uuid;package_stream.operations={Frame(true,parser_uuid,view.catalog_epoch_uuid,package),std::move(member),Frame(false,parser_uuid,view.catalog_epoch_uuid,package)};const auto stream=sblr::EncodeSblrOpcodeStream(package_stream);Require(!stream.empty(),"002340 SBOS invalid");auto submission=BuildSubmission(fixture,view,parser_uuid);auto dc=wire::DecodeSblrContainerBytes(reinterpret_cast<const std::uint8_t*>(submission.container.data()),submission.container.size());dc.container.operation_payload=stream;const auto outer=wire::EncodeSblrContainer(dc.container);auto de=wire::DecodeSblrExecutionEnvelopeV1Bytes(reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),submission.ingress.size());de.envelope.fields[5]={1};U64(&de.envelope.fields[5],stream.size());de.envelope.fields[5].insert(de.envelope.fields[5].end(),stream.begin(),stream.end());de.envelope.fields[7]={1};U32(&de.envelope.fields[7],wire::SblrCrc32c(stream.data(),stream.size()));de.envelope.fields[8]=V64(stream.size());const auto ingress=wire::EncodeSblrExecutionEnvelopeV1(de.envelope);submission={{outer.begin(),outer.end()},{ingress.begin(),ingress.end()},stream};
  bridge::StatementPackageAdmissionReservationRequest rr;rr.receipt=receipt;rr.canonical_payload_bytes=stream.data();rr.canonical_payload_size=stream.size();rr.payload_kind=bridge::StatementSblrPayloadKind::kOpcodeStream;bridge::StatementPackageAdmissionReservationHandle reservation;bridge::StatementPackageAdmissionReservationView rv;Require(bridge::AcquireStatementPackageAdmissionReservation(&rr,&reservation,&rv,&result)==SB_ENGINE_STATUS_OK,"002340 reservation failed");if(result)(void)sb_engine_result_release(result);
  server::ServerSblrAdmissionRequest a;a.encoded_sblr_container=submission.container;a.encoded_execution_envelope=submission.ingress;a.admitted_parser_package_uuid=parser_uuid;a.admitted_parser_package_version_major=1;a.admitted_registry_snapshot_uuid=view.catalog_epoch_uuid;a.authenticated_principal_uuid=Text(fixture.principal_uuid);a.catalog_snapshot_uuid=view.statement_metadata_snapshot_uuid;a.engine_mga_statement_uuid=view.statement_uuid;a.engine_mga_snapshot_uuid=view.statement_snapshot_uuid;a.catalog_epoch=view.catalog_generation_id;a.security_epoch=view.security_epoch;a.resource_epoch=view.resource_epoch;a.route_snapshot_uuid=view.optimizer_route_snapshot_uuid;a.route_epoch=view.optimizer_route_epoch;a.route_generation=view.optimizer_route_generation;a.security_snapshot_uuid=view.security_context_uuid;a.security_observation_generation=view.security_epoch;a.route_snapshot_engine_owned=true;a.security_snapshot_engine_owned=true;a.package_reservation_handle=reservation.opaque_id;a.reserved_payload_kind=server::ServerSblrPayloadKind::opcode_stream;a.reserved_payload_size=rv.payload_size;a.reserved_record_count=rv.record_count;a.reserved_resource_policy_generation=rv.resource_policy_generation;a.reserved_payload_sha256=rv.payload_sha256;const auto admitted=server::AdmitServerSblrEnvelope(a);Require(admitted.admitted&&admitted.admission_token,"002340 admission failed");auto dispatch=DispatchRequest(receipt,session.session,reservation,admitted.admission_token);result=nullptr;const auto status=bridge::DispatchStatementContextReceipt(&dispatch,&result);Require(status==SB_ENGINE_STATUS_TIMEOUT&&result,"002340 cancellation status drifted");sb_engine_diagnostic_set_view_t ds{};Require(sb_engine_result_diagnostics(result,&ds)==SB_ENGINE_STATUS_OK&&ds.diagnostic_count==1,"002340 diagnostic missing");Require(std::string(ds.diagnostics[0].message_key.data,ds.diagnostics[0].message_key.size_bytes)=="sblr.source_map.cancelled_before_entry","002340 cancellation boundary drifted");Require(probes.load()==2,"002340 callback sequence drifted");(void)sb_engine_result_release(result);result=nullptr;Require(bridge::DispatchStatementContextReceipt(&dispatch,&result)==SB_ENGINE_STATUS_INVALID_HANDLE,"002340 token replay admitted");if(result)(void)sb_engine_result_release(result);
  bool observed_final_cancellation = false;
  unsigned publication_cases = 0;
  ScopedSourceMapTrace trace_scope(trace_path);
  const auto evidence_digest = scratchbird::core::hash::ComputeSha256Digest(
      package_stream.operations[1].operands.front().value_body);
  Require(evidence_digest.ok(), "independent SOURCE_MAP evidence digest");
  const auto evidence = "sha256:" + scratchbird::core::hash::HexLower(evidence_digest.digest);
  constexpr std::string_view cancellation_keys[] = {
      "", "sblr.opcode_stream.cancelled_before_decode", "sblr.source_map.cancelled_before_entry",
      "sblr.source_map.cancelled_before_parent", "sblr.opcode_stream.rejected",
      "sblr.source_map.cancelled_before_success_barrier"};
  for (unsigned cancel_phase = 0; cancel_phase != 9; ++cancel_phase) {
    probes.store(0); cancel_at.store(cancel_phase);
    premature_evidence = false;
    premature_result = false;
    const auto trace_before = ReadTrace(trace_path);
    trace_bytes_before_dispatch = trace_before.size();
    bridge::StatementPackageAdmissionReservationHandle next;
    bridge::StatementPackageAdmissionReservationView next_view;
    result = nullptr;
    Require(bridge::AcquireStatementPackageAdmissionReservation(&rr, &next, &next_view, &result)
                == SB_ENGINE_STATUS_OK, "publication reservation acquisition");
    if (result) (void)sb_engine_result_release(result);
    auto next_admission = a;
    next_admission.package_reservation_handle = next.opaque_id;
    next_admission.reserved_payload_size = next_view.payload_size;
    next_admission.reserved_record_count = next_view.record_count;
    next_admission.reserved_resource_policy_generation = next_view.resource_policy_generation;
    next_admission.reserved_payload_sha256 = next_view.payload_sha256;
    const auto admitted_next = server::AdmitServerSblrEnvelope(next_admission);
    Require(admitted_next.admitted && admitted_next.admission_token, "publication server admission");
    auto next_dispatch = DispatchRequest(receipt, session.session, next, admitted_next.admission_token);
    result = nullptr;
    observed_public_slot = &result;
    const auto next_status = bridge::DispatchStatementContextReceipt(&next_dispatch, &result);
    observed_public_slot = nullptr;
    Require(result != nullptr, "publication result absent");
    std::string key;
    sb_engine_diagnostic_set_view_t diagnostics{};
    Require(sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK,
            "publication diagnostic access");
    if (diagnostics.diagnostic_count) {
      const auto& message = diagnostics.diagnostics[0].message_key;
      key.assign(message.data, message.size_bytes);
    }
    if (key == "sblr.source_map.cancelled_before_success_barrier") {
      observed_final_cancellation = true;
      Require(next_status == SB_ENGINE_STATUS_TIMEOUT, "final barrier cancellation status");
    }
    const bool cancelled = cancel_phase >= 1 && cancel_phase <= 5;
    Require(next_status == (cancelled ? SB_ENGINE_STATUS_TIMEOUT : SB_ENGINE_STATUS_OK),
            "publication cancellation phase outcome");
    Require(probes.load() == (cancelled ? cancel_phase : 5), "publication cancellation phase count");
    const auto trace_after = ReadTrace(trace_path);
    Require(!premature_evidence, "SOURCE_MAP evidence was published before final cancellation check");
    Require(!premature_result, "SOURCE_MAP result was exposed before final cancellation check");
    if (cancelled) {
      Require(key == cancellation_keys[cancel_phase], "publication exact cancellation diagnostic");
      Require(trace_after == trace_before, "cancelled SOURCE_MAP published executor evidence");
      Require(diagnostics.diagnostic_count == 1, "publication cancellation diagnostic count");
      const auto& symbolic = diagnostics.diagnostics[0].symbolic_code;
      Require(std::string_view(symbolic.data, symbolic.size_bytes) == "PROCESS.CANCELLED",
              "publication cancellation diagnostic code");
    } else {
      sb_engine_result_class_t result_class{};
      sb_engine_command_completion_view_v1_t completion{};
      sb_engine_execution_summary_view_v1_t summary{};
      Require(sb_engine_result_class(result, &result_class) == SB_ENGINE_STATUS_OK &&
                  result_class == SB_ENGINE_RESULT_COMMAND_COMPLETION,
              "SOURCE_MAP void must not publish a row batch");
      Require(sb_engine_result_completion(result, &completion) == SB_ENGINE_STATUS_OK &&
                  std::string_view(completion.operation_id.data, completion.operation_id.size_bytes)
                      == "engine.op.source_map" && completion.affected_rows == 0,
              "SOURCE_MAP typed void completion identity");
      Require(sb_engine_result_summary(result, &summary) == SB_ENGINE_STATUS_OK &&
                  summary.rows_produced == 0 && summary.diagnostics_count == 0,
              "SOURCE_MAP void must not fabricate rows or diagnostics");
      sb_engine_string_view_t payload{};
      Require(sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK &&
                  std::string_view(payload.data, payload.size_bytes).find("result_kind=void\n")
                      != std::string_view::npos &&
                  std::string_view(payload.data, payload.size_bytes).find(evidence)
                      != std::string_view::npos,
              "SOURCE_MAP public payload must carry typed void and correctly bound evidence");
      const auto suffix = trace_after.substr(trace_before.size());
      Require(suffix.find("layer=source_map_executor") != std::string::npos &&
                  suffix.find("executor_evidence_sha256=" + evidence) != std::string::npos &&
                  suffix.find("result_descriptor_id=void") != std::string::npos &&
                  suffix.find("parent_success_barrier=passed") != std::string::npos &&
                  std::count(suffix.begin(), suffix.end(), '\n') == 1,
              "SOURCE_MAP success must publish exactly one correctly bound evidence record");
    }
    std::cout << "source_map_publication phase=" << cancel_phase << " probes=" << probes.load()
              << " status=" << next_status << " key=" << key << '\n';
    (void)sb_engine_result_release(result);
    result = nullptr;
    Require(bridge::DispatchStatementContextReceipt(&next_dispatch, &result)
                == SB_ENGINE_STATUS_INVALID_HANDLE, "publication reservation replay");
    if (result) (void)sb_engine_result_release(result);
    ++publication_cases;
  }
  Require(observed_final_cancellation, "SOURCE_MAP final success barrier never observed cancellation");
  std::cout << "source_map_publication cases=" << publication_cases << " PASS\n";
  cancel_at.store(0);
  Require(session.End()==SB_ENGINE_STATUS_OK,"002340 cleanup failed");api::EngineRollbackTransactionRequest rollback;rollback.context=context;Require(api::EngineRollbackTransaction(rollback).ok,"002340 rollback failed");return EXIT_SUCCESS;}
