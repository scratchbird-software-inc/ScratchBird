#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"
#include "engine/sblr/sblr_error_vector_runtime.hpp"
#include "engine/internal_api/sblr_error_vector_descriptor_registry.hpp"
#include "engine/statement_context_receipt_retention.hpp"
#include "engine/internal_api/dml/update_resource_authority_provider.hpp"
#if defined(__linux__)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace ev=scratchbird::engine::sblr;

#if defined(SB_EV_PUBLIC_IO_FAULTS)
namespace ev_public_fault {
thread_local bool armed=false, committed=false, next_allocation=false, injected=false;
thread_local bool reject_revoke=false, revoke_rejected=false;
thread_local long retention_allocation_countdown=-1;
thread_local std::string journal, directory;
thread_local std::array<std::uint8_t,16> descriptor{};
bool Target(int fd,const std::string& expected) {
  char name[64],path[4096];const int size=std::snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);
  if(size<=0 || static_cast<std::size_t>(size)>=sizeof(name))return false;
  const auto count=::readlink(name,path,sizeof(path));
  return count>0 && std::string_view(path,count)==expected;
}
}
void* operator new(std::size_t size) {
  if(ev_public_fault::retention_allocation_countdown>=0 &&
     ev_public_fault::retention_allocation_countdown--==0) {
    ev_public_fault::injected=true;throw std::bad_alloc();
  }
  if(ev_public_fault::next_allocation) {
    ev_public_fault::next_allocation=false;ev_public_fault::injected=true;throw std::bad_alloc();
  }
  if(void* p=std::malloc(size?size:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
// Keep the throwing and nothrow allocation families paired with our test-only
// malloc/free replacement, including when the sanitizer supplies nothrow new.
void* operator new(std::size_t size,const std::nothrow_t&) noexcept {
  try {return ::operator new(size);} catch(const std::bad_alloc&) {return nullptr;}
}
void* operator new[](std::size_t size,const std::nothrow_t&) noexcept {
  try {return ::operator new[](size);} catch(const std::bad_alloc&) {return nullptr;}
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
void operator delete(void* p,const std::nothrow_t&) noexcept {std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept {std::free(p);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd,const void* bytes,size_t size,off_t at) {
  if(ev_public_fault::reject_revoke && size>=280 &&
     ev_public_fault::Target(fd,ev_public_fault::journal) &&
     std::memcmp(bytes,"EVDE",4)==0 && static_cast<const std::uint8_t*>(bytes)[24]==2) {
    ev_public_fault::reject_revoke=false;ev_public_fault::revoke_rejected=true;
    errno=EIO;return -1;
  }
  const auto result=__real_pwrite(fd,bytes,size,at);
  if(ev_public_fault::armed && result==64 && size==64 &&
     ev_public_fault::Target(fd,ev_public_fault::journal) &&
     std::memcmp(bytes,"EVDC",4)==0) {
    std::copy_n(static_cast<const std::uint8_t*>(bytes)+16,16,ev_public_fault::descriptor.begin());
    ev_public_fault::committed=true;
  }
  return result;
}
extern "C" int __wrap_fsync(int fd) {
  const auto result=__real_fsync(fd);
  if(result==0 && ev_public_fault::armed && ev_public_fault::committed &&
     ev_public_fault::Target(fd,ev_public_fault::directory)) {
    ev_public_fault::armed=false;ev_public_fault::next_allocation=true;
  }
  return result;
}
void VerifyUnpublishedIssueCleanup(sb_engine_session_t session,
                                  const api::EngineRequestContext& context,
                                  const ev::SblrErrorVectorEntryV1& entry) {
  bridge::StatementContextAcquireRequest request;request.engine_context=&context;
  request.exact_transaction_uuid=context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;bridge::StatementContextReceiptView view;
  sb_engine_result_t result=nullptr;
  Require(bridge::AcquireStatementContextReceipt(session,&request,&receipt,&view,&result)==SB_ENGINE_STATUS_OK,
          "ERROR_VECTOR fault receipt acquire failed");
  if(result)(void)sb_engine_result_release(result);
  api::EngineRequestContext owner;
  Require(bridge::CopyStatementContextEngineContextV1(receipt,&owner,&result)==SB_ENGINE_STATUS_OK,
          "ERROR_VECTOR fault owner context copy failed");
  if(result)(void)sb_engine_result_release(result);
  owner.statement_metadata_snapshot_engine_owned=true;
  ev::SblrErrorVectorIssueRequestV1 issue;
  issue.statement_receipt_uuid=RawUuid(view.receipt_uuid);
  issue.registry_snapshot_uuid=RawUuid(view.catalog_epoch_uuid);
  issue.registry_generation=view.literal_catalog_generation;
  issue.diagnostic_registry_snapshot_uuid=view.diagnostic_registry_snapshot_uuid;
  issue.diagnostic_registry_generation=view.diagnostic_registry_generation;
  issue.entries={entry};const auto evrq=ev::EncodeSblrErrorVectorIssueRequestV1(&issue);
  Bytes response{0xde,0xad};
  ev_public_fault::journal=context.database_path+".sb.sblr_error_vector_registry.v1";
  ev_public_fault::directory=std::filesystem::path(context.database_path).parent_path().string();
  ev_public_fault::armed=true;ev_public_fault::committed=false;ev_public_fault::injected=false;
  const auto status=bridge::IssueStatementErrorVectorDescriptorV1(receipt,evrq,&response,&result);
  ev_public_fault::armed=false;ev_public_fault::next_allocation=false;
  if(result)(void)sb_engine_result_release(result);
  Require(ev_public_fault::committed&&ev_public_fault::injected &&
          status==SB_ENGINE_STATUS_RESOURCE_EXHAUSTED && response.empty(),
          "post-commit allocation failure published success or partial EVRS");
  Require(api::LookupSblrErrorVectorDescriptorV1(owner,issue.statement_receipt_uuid,
          ev_public_fault::descriptor,1).ok,"post-commit fault did not leave the actual durable precondition");
  Require(bridge::ReleaseStatementContextReceipt(receipt)==SB_ENGINE_STATUS_OK,
          "unpublished descriptor receipt cleanup failed");
  const auto revoked=api::LookupSblrErrorVectorDescriptorV1(owner,issue.statement_receipt_uuid,
      ev_public_fault::descriptor,1);
  Require(!revoked.ok&&revoked.diagnostic.code=="SBLR.ERROR_VECTOR.STALE",
          "unpublished durable descriptor escaped receipt cleanup");
}
void VerifyRetainedReceiptCleanup(sb_engine_session_t session,
                                 const api::EngineRequestContext& context,
                                 const ev::SblrErrorVectorEntryV1& entry) {
  unsigned allocation_failures=0;
  for(bool retain : {false,true}) {
    bridge::StatementContextAcquireRequest request;
    request.engine_context=&context;
    request.exact_transaction_uuid=context.transaction_uuid.canonical;
    bridge::StatementContextReceiptHandle receipt;
    bridge::StatementContextReceiptView view;
    Require(bridge::AcquireStatementContextReceipt(session,&request,&receipt,&view,nullptr)==
                SB_ENGINE_STATUS_OK,"retained fault receipt acquisition failed");
    api::EngineRequestContext owner;
    Require(bridge::CopyStatementContextEngineContextV1(receipt,&owner,nullptr)==
                SB_ENGINE_STATUS_OK,"retained fault receipt context copy failed");
    owner.statement_metadata_snapshot_engine_owned=true;
    const auto resource_receipt=owner.dml_update_resource_receipt.lock();
    Require(resource_receipt && !resource_receipt->IsRevoked(),
            "retained fault real resource receipt missing");
    const auto owner_uuid=RawUuid(owner.session_uuid.canonical);
    const auto receipt_uuid=RawUuid(view.receipt_uuid);
    // Sweep every allocation in actual retention, stopping only at the first
    // uninjected success. Failed allocations must not leave an armed owner.
    bool sweep_complete=false;
    for(long at=0;at!=128;++at) {
      api::RetainedStatementResultAuthoritiesV1 attempted;
      ev_public_fault::injected=false;
      ev_public_fault::retention_allocation_countdown=at;
      const auto status=api::RetainStatementContextForResultV1(
          receipt,owner_uuid,receipt_uuid,&attempted);
      ev_public_fault::retention_allocation_countdown=-1;
      if(status==SB_ENGINE_STATUS_OK) {
        Require(!ev_public_fault::injected && attempted.statement_receipt && attempted.snapshot,
                "allocation failure returned retained ownership success");
        sweep_complete=true;break;
      }
      Require(ev_public_fault::injected && status==SB_ENGINE_STATUS_RESOURCE_EXHAUSTED &&
                  !attempted.statement_receipt && !attempted.snapshot,
              "failed retention leaked partial private ownership");
      ++allocation_failures;
    }
    Require(sweep_complete,"retention allocation sweep did not reach uninjected completion");
    ev::SblrErrorVectorIssueRequestV1 issue;
    issue.statement_receipt_uuid=receipt_uuid;
    issue.registry_snapshot_uuid=RawUuid(view.catalog_epoch_uuid);
    issue.registry_generation=view.literal_catalog_generation;
    issue.diagnostic_registry_snapshot_uuid=view.diagnostic_registry_snapshot_uuid;
    issue.diagnostic_registry_generation=view.diagnostic_registry_generation;
    issue.entries={entry};
    const auto encoded=ev::EncodeSblrErrorVectorIssueRequestV1(&issue);
    Bytes response;
    Require(bridge::IssueStatementErrorVectorDescriptorV1(receipt,encoded,&response,nullptr)==
                SB_ENGINE_STATUS_OK,"retained fault descriptor issuance failed");
    ev::SblrErrorVectorIssueResultV1 descriptor;
    std::string detail;
    Require(ev::DecodeSblrErrorVectorIssueResultV1(response.data(),response.size(),&descriptor,&detail),
            "retained fault descriptor decoding failed");
    api::RetainedStatementResultAuthoritiesV1 owners;
    if(retain) Require(api::RetainStatementContextForResultV1(receipt,owner_uuid,receipt_uuid,&owners)==
                          SB_ENGINE_STATUS_OK,"fault probe retention failed");
    ev_public_fault::journal=context.database_path+".sb.sblr_error_vector_registry.v1";
    ev_public_fault::reject_revoke=true;
    ev_public_fault::revoke_rejected=false;
    const auto retired=bridge::ReleaseStatementContextReceipt(receipt);
    if(retain) {
      Require(retired==SB_ENGINE_STATUS_OK && !ev_public_fault::revoke_rejected &&
                  !resource_receipt->IsRevoked(),
              "public retirement cleaned resources still owned by a result");
      owners.statement_receipt->Release(api::TypedResultProducerReleaseReasonV1::eos);
    } else Require(retired==SB_ENGINE_STATUS_INTERNAL_ERROR,
                   "failed ordinary durable cleanup falsely reported success");
    Require(ev_public_fault::revoke_rejected,"real retained receipt revoke fault was not reached");
    ev_public_fault::reject_revoke=false;
    Require(api::LookupSblrErrorVectorDescriptorV1(owner,receipt_uuid,
                descriptor.descriptor_uuid,descriptor.descriptor_generation).ok,
            "failed first write changed durable descriptor authority");
    Require(bridge::ReleaseStatementContextReceipt(receipt)==SB_ENGINE_STATUS_OK,
            "failed durable cleanup lost its retryable retired owner");
    Require(resource_receipt->IsRevoked(),
            "allocation/cleanup failure stranded a private result owner");
    const auto revoked=api::LookupSblrErrorVectorDescriptorV1(owner,receipt_uuid,
        descriptor.descriptor_uuid,descriptor.descriptor_generation);
    Require(!revoked.ok && revoked.diagnostic.code=="SBLR.ERROR_VECTOR.STALE",
            "successful release retry failed to revoke actual durable descriptor");
    Require(bridge::ReleaseStatementContextReceipt(receipt)==SB_ENGINE_STATUS_ALREADY_RELEASED,
            "release retry did not finish receipt ownership");
  }
  std::cout << "retained receipt allocation faults=" << allocation_failures
            << "; actual durable release faults=2; retry cleanup PASS\n";
}
#endif

int main(int argc,char** argv){
if(argc==3 && std::string_view(argv[1])=="--node-startup"){
  sb_engine_open_params_v1_t open{};open.struct_size=sizeof(open);
  open.abi_version=SB_ENGINE_ABI_VERSION_PACKED;open.database_path_utf8=argv[2];
  open.database_path_size=std::strlen(argv[2]);open.mode=SB_ENGINE_OPEN_NORMAL;
  sb_engine_handle_t engine=nullptr;
  const auto status=sb_engine_open(&open,&engine,nullptr);
  if(status!=SB_ENGINE_STATUS_OK)return 91;
  return sb_engine_close(engine,nullptr)==SB_ENGINE_STATUS_OK?0:92;
}
auto fixture=CreateFixture();PublicSession session(fixture);std::atomic<unsigned> probes{0};auto context=BeginTransaction(fixture,&probes);context.query_cancellation_requested=[&probes]{return probes.fetch_add(1)+1==2;};
bridge::StatementContextAcquireRequest acquire;acquire.engine_context=&context;acquire.exact_transaction_uuid=context.transaction_uuid.canonical;bridge::StatementContextReceiptHandle receipt;bridge::StatementContextReceiptView view;sb_engine_result_t result=nullptr;Require(bridge::AcquireStatementContextReceipt(session.session,&acquire,&receipt,&view,&result)==SB_ENGINE_STATUS_OK,"002344 receipt acquire failed");if(result)(void)sb_engine_result_release(result);Require(view.diagnostic_identity_rows.size()>1,"002344 non-prefix diagnostic cohort absent");const auto& identity=view.diagnostic_identity_rows.back();Require(identity.precedence_ordinal>1,"002344 selected row is still a prefix");
ev::SblrErrorVectorEntryV1 entry;entry.occurrence_ordinal=1;entry.diagnostic_uuid=identity.diagnostic_uuid;entry.diagnostic_generation=identity.diagnostic_generation;entry.precedence_ordinal=identity.precedence_ordinal;entry.severity_code=identity.severity_code;entry.redaction_class=identity.redaction_class;entry.safe_field_count=0;entry.safe_fields_sha256=ev::SblrErrorVectorEmptySafeFieldsHashV1();ev::SblrErrorVectorIssueRequestV1 issue;issue.statement_receipt_uuid=RawUuid(view.receipt_uuid);issue.registry_snapshot_uuid=RawUuid(view.catalog_epoch_uuid);issue.registry_generation=view.literal_catalog_generation;issue.diagnostic_registry_snapshot_uuid=view.diagnostic_registry_snapshot_uuid;issue.diagnostic_registry_generation=view.diagnostic_registry_generation;issue.entries={entry};auto evrq=ev::EncodeSblrErrorVectorIssueRequestV1(&issue);
#if defined(SB_EV_PUBLIC_IO_FAULTS)
VerifyUnpublishedIssueCleanup(session.session,context,entry);
VerifyRetainedReceiptCleanup(session.session,context,entry);
#endif
Bytes evrs;Require(bridge::IssueStatementErrorVectorDescriptorV1(receipt,evrq,&evrs,&result)==SB_ENGINE_STATUS_OK,"002344 error vector issue failed");if(result)(void)sb_engine_result_release(result);ev::SblrErrorVectorIssueResultV1 issued;std::string detail;Require(ev::DecodeSblrErrorVectorIssueResultV1(evrs.data(),evrs.size(),&issued,&detail),"002344 EVRS invalid");

auto maximum_request=issue;maximum_request.entries.assign(4096,entry);
for(std::size_t i=0;i<maximum_request.entries.size();++i)maximum_request.entries[i].occurrence_ordinal=i+1;
const auto maximum_evrq=ev::EncodeSblrErrorVectorIssueRequestV1(&maximum_request);
Bytes maximum_evrs;
Require(bridge::IssueStatementErrorVectorDescriptorV1(receipt,maximum_evrq,&maximum_evrs,&result)==SB_ENGINE_STATUS_OK,
        "full 4096-entry real receipt issue failed");
if(result)(void)sb_engine_result_release(result);
ev::SblrErrorVectorIssueResultV1 maximum_result;
Require(ev::DecodeSblrErrorVectorIssueResultV1(maximum_evrs.data(),maximum_evrs.size(),&maximum_result,&detail) &&
        maximum_result.registry_generation==issue.registry_generation &&
        maximum_result.canonical_ervd.size()==524440 && maximum_result.descriptor_uuid!=issued.descriptor_uuid,
        "full-size real receipt descriptor drifted or reused identity");

const auto parser_uuid=Text(NewUuid(platform::UuidKind::object,9442));auto member=sblr::MakeSblrEnvelope("engine.op.error_vector","SBLR_ERROR_VECTOR","ia01.error_vector.cancel");member.opcode_code=7;member.result_shape="void";member.diagnostic_shape="diagnostic_vector";member.parser_package_uuid=parser_uuid;member.registry_snapshot_uuid=view.catalog_epoch_uuid;member.parser_resolved_names_to_uuids=true;sblr::SblrOperand operand;operand.ordinal=1;operand.type="diagnostic.vector";operand.name="diagnostics";operand.value_kind=sblr::SblrValueKind::descriptor_ref;operand.value_body.assign(issued.descriptor_uuid.begin(),issued.descriptor_uuid.end());U64(&operand.value_body,issued.descriptor_generation);member.operands.push_back(std::move(operand));const auto validated=sblr::ValidateSblrEnvelope(member);if(!validated.ok)for(const auto&d:validated.diagnostics)std::cerr<<d.code<<':'<<d.message<<'\n';Require(validated.ok,"002344 member invalid");
const auto package=RawUuid(view.bound_ast_uuid);sblr::SblrOpcodeStream package_stream;package_stream.package_descriptor_uuid=view.bound_ast_uuid;package_stream.registry_snapshot_uuid=view.catalog_epoch_uuid;package_stream.operations={Frame(true,parser_uuid,view.catalog_epoch_uuid,package),std::move(member),Frame(false,parser_uuid,view.catalog_epoch_uuid,package)};const auto stream=sblr::EncodeSblrOpcodeStream(package_stream);Require(!stream.empty(),"002344 SBOS invalid");auto submission=BuildSubmission(fixture,view,parser_uuid);auto dc=wire::DecodeSblrContainerBytes(reinterpret_cast<const std::uint8_t*>(submission.container.data()),submission.container.size());dc.container.operation_payload=stream;const auto outer=wire::EncodeSblrContainer(dc.container);auto de=wire::DecodeSblrExecutionEnvelopeV1Bytes(reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),submission.ingress.size());de.envelope.fields[5]={1};U64(&de.envelope.fields[5],stream.size());de.envelope.fields[5].insert(de.envelope.fields[5].end(),stream.begin(),stream.end());de.envelope.fields[7]={1};U32(&de.envelope.fields[7],wire::SblrCrc32c(stream.data(),stream.size()));de.envelope.fields[8]=V64(stream.size());const auto ingress=wire::EncodeSblrExecutionEnvelopeV1(de.envelope);submission={{outer.begin(),outer.end()},{ingress.begin(),ingress.end()},stream};
bridge::StatementPackageAdmissionReservationRequest rr;rr.receipt=receipt;rr.canonical_payload_bytes=stream.data();rr.canonical_payload_size=stream.size();rr.payload_kind=bridge::StatementSblrPayloadKind::kOpcodeStream;bridge::StatementPackageAdmissionReservationHandle reservation;bridge::StatementPackageAdmissionReservationView rv;Require(bridge::AcquireStatementPackageAdmissionReservation(&rr,&reservation,&rv,&result)==SB_ENGINE_STATUS_OK,"002344 reservation failed");if(result)(void)sb_engine_result_release(result);server::ServerSblrAdmissionRequest a;a.encoded_sblr_container=submission.container;a.encoded_execution_envelope=submission.ingress;a.admitted_parser_package_uuid=parser_uuid;a.admitted_parser_package_version_major=1;a.admitted_registry_snapshot_uuid=view.catalog_epoch_uuid;a.authenticated_principal_uuid=Text(fixture.principal_uuid);a.catalog_snapshot_uuid=view.statement_metadata_snapshot_uuid;a.engine_mga_statement_uuid=view.statement_uuid;a.engine_mga_snapshot_uuid=view.statement_snapshot_uuid;a.catalog_epoch=view.catalog_generation_id;a.security_epoch=view.security_epoch;a.resource_epoch=view.resource_epoch;a.route_snapshot_uuid=view.optimizer_route_snapshot_uuid;a.route_epoch=view.optimizer_route_epoch;a.route_generation=view.optimizer_route_generation;a.security_snapshot_uuid=view.security_context_uuid;a.security_observation_generation=view.security_epoch;a.route_snapshot_engine_owned=true;a.security_snapshot_engine_owned=true;a.package_reservation_handle=reservation.opaque_id;a.reserved_payload_kind=server::ServerSblrPayloadKind::opcode_stream;a.reserved_payload_size=rv.payload_size;a.reserved_record_count=rv.record_count;a.reserved_resource_policy_generation=rv.resource_policy_generation;a.reserved_payload_sha256=rv.payload_sha256;const auto admitted=server::AdmitServerSblrEnvelope(a);Require(admitted.admitted&&admitted.admission_token,"002344 admission failed");auto dispatch=DispatchRequest(receipt,session.session,reservation,admitted.admission_token);result=nullptr;const auto status=bridge::DispatchStatementContextReceipt(&dispatch,&result);Require(status==SB_ENGINE_STATUS_TIMEOUT&&result,"002344 cancellation status drifted");sb_engine_diagnostic_set_view_t ds{};Require(sb_engine_result_diagnostics(result,&ds)==SB_ENGINE_STATUS_OK&&ds.diagnostic_count==1,"002344 diagnostic missing");Require(std::string(ds.diagnostics[0].message_key.data,ds.diagnostics[0].message_key.size_bytes)=="sblr.error_vector.cancelled_before_entry","002344 cancellation boundary drifted");Require(probes.load()==2,"002344 callback sequence drifted");(void)sb_engine_result_release(result);result=nullptr;Require(bridge::DispatchStatementContextReceipt(&dispatch,&result)==SB_ENGINE_STATUS_INVALID_HANDLE,"002344 token replay admitted");if(result)(void)sb_engine_result_release(result);api::EngineRequestContext owner_context;
result=nullptr;
Require(bridge::CopyStatementContextEngineContextV1(receipt,&owner_context,&result)==SB_ENGINE_STATUS_OK,"002344 owner context copy failed");
if(result)(void)sb_engine_result_release(result);
owner_context.statement_metadata_snapshot_engine_owned=true;
Require(api::LookupSblrErrorVectorDescriptorV1(owner_context,issue.statement_receipt_uuid,issued.descriptor_uuid,issued.descriptor_generation).ok,"002344 descriptor lost before receipt release");
Require(bridge::ReleaseStatementContextReceipt(receipt)==SB_ENGINE_STATUS_OK,"002344 actual receipt release failed");
const auto revoked=api::LookupSblrErrorVectorDescriptorV1(owner_context,issue.statement_receipt_uuid,issued.descriptor_uuid,issued.descriptor_generation);
Require(!revoked.ok&&revoked.diagnostic.code=="SBLR.ERROR_VECTOR.STALE","002344 released descriptor remains executable");
Require(bridge::ReleaseStatementContextReceipt(receipt)==SB_ENGINE_STATUS_ALREADY_RELEASED,"002344 receipt release replay changed");

bridge::StatementContextReceiptHandle implicit_receipt;bridge::StatementContextReceiptView implicit_view;
result=nullptr;
Require(bridge::AcquireStatementContextReceipt(session.session,&acquire,&implicit_receipt,&implicit_view,&result)==SB_ENGINE_STATUS_OK,
        "implicit session cleanup receipt acquire failed");
if(result)(void)sb_engine_result_release(result);
api::EngineRequestContext implicit_owner;
Require(bridge::CopyStatementContextEngineContextV1(implicit_receipt,&implicit_owner,&result)==SB_ENGINE_STATUS_OK,
        "implicit session cleanup context copy failed");
if(result)(void)sb_engine_result_release(result);
implicit_owner.statement_metadata_snapshot_engine_owned=true;
auto implicit_issue=issue;implicit_issue.statement_receipt_uuid=RawUuid(implicit_view.receipt_uuid);
implicit_issue.registry_snapshot_uuid=RawUuid(implicit_view.catalog_epoch_uuid);
implicit_issue.registry_generation=implicit_view.literal_catalog_generation;
implicit_issue.diagnostic_registry_snapshot_uuid=implicit_view.diagnostic_registry_snapshot_uuid;
implicit_issue.diagnostic_registry_generation=implicit_view.diagnostic_registry_generation;
const auto implicit_evrq=ev::EncodeSblrErrorVectorIssueRequestV1(&implicit_issue);
Bytes implicit_evrs;
Require(bridge::IssueStatementErrorVectorDescriptorV1(implicit_receipt,implicit_evrq,&implicit_evrs,&result)==SB_ENGINE_STATUS_OK,
        "implicit session cleanup descriptor issue failed");
if(result)(void)sb_engine_result_release(result);
ev::SblrErrorVectorIssueResultV1 implicit_descriptor;
Require(ev::DecodeSblrErrorVectorIssueResultV1(implicit_evrs.data(),implicit_evrs.size(),&implicit_descriptor,&detail),
        "implicit session cleanup descriptor decode failed");
#if defined(SB_EV_PUBLIC_IO_FAULTS)
ev_public_fault::reject_revoke=true;ev_public_fault::revoke_rejected=false;
Require(session.End()==SB_ENGINE_STATUS_INTERNAL_ERROR && ev_public_fault::revoke_rejected,
        "failed session revocation falsely reported a successful shutdown");
ev_public_fault::reject_revoke=false;
api::EngineRequestContext retained_owner;
Require(bridge::CopyStatementContextEngineContextV1(implicit_receipt,&retained_owner,&result)==SB_ENGINE_STATUS_OK,
        "failed shutdown discarded receipt ownership needed for recovery");
if(result)(void)sb_engine_result_release(result);
Require(api::LookupSblrErrorVectorDescriptorV1(implicit_owner,implicit_issue.statement_receipt_uuid,
        implicit_descriptor.descriptor_uuid,implicit_descriptor.descriptor_generation).ok,
        "failed first revoke write changed valid durable descriptor authority");
#endif
Require(session.End()==SB_ENGINE_STATUS_OK,"002344 cleanup failed");
const auto implicitly_revoked=api::LookupSblrErrorVectorDescriptorV1(implicit_owner,implicit_issue.statement_receipt_uuid,
    implicit_descriptor.descriptor_uuid,implicit_descriptor.descriptor_generation);
Require(!implicitly_revoked.ok&&implicitly_revoked.diagnostic.code=="SBLR.ERROR_VECTOR.STALE",
        "session shutdown erased receipt without durable ERROR_VECTOR revocation");
api::EngineRollbackTransactionRequest rollback;rollback.context=context;Require(api::EngineRollbackTransaction(rollback).ok,"002344 rollback failed");
#if defined(__linux__)
Require(sb_engine_close(session.engine,nullptr)==SB_ENGINE_STATUS_OK,"002344 engine close failed");
session.engine=nullptr;
// Create real durable pre-startup residue through the private issuer. This is
// a lifecycle component precondition, not an ordinary SQL/E2E assertion.
const auto orphan=api::IssueSblrErrorVectorDescriptorV1(owner_context,issue.statement_receipt_uuid,
    issue.registry_snapshot_uuid,issue.registry_generation,issue.diagnostic_registry_snapshot_uuid,
    issue.diagnostic_registry_generation,{entry});
Require(orphan.ok,"002344 pre-startup descriptor issue failed");
const auto child=::fork();Require(child>=0,"002344 startup fork failed");
if(child==0){::execl(argv[0],argv[0],"--node-startup",session.path.c_str(),nullptr);::_exit(93);}
int child_status=0;Require(::waitpid(child,&child_status,0)==child,"002344 startup wait failed");
Require(WIFEXITED(child_status)&&WEXITSTATUS(child_status)==0,"002344 real normal node startup failed");
const auto recovered=api::LookupSblrErrorVectorDescriptorV1(owner_context,issue.statement_receipt_uuid,
    orphan.snapshot.descriptor_uuid,orphan.snapshot.descriptor_generation);
Require(!recovered.ok&&recovered.diagnostic.code=="SBLR.ERROR_VECTOR.STALE",
        "002344 normal node startup did not revoke surviving descriptor");
#endif
return EXIT_SUCCESS;}
