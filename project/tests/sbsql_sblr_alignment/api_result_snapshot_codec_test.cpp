// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "api_result_snapshot_codec.hpp"
#include "query/result_metadata.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
namespace { long fail_after=-1;unsigned checks=0,failures=0,faults=0; }
void* operator new(std::size_t n){if(fail_after==0)throw std::bad_alloc();if(fail_after>0)--fail_after;if(auto*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete(void*p,std::size_t)noexcept{std::free(p);}
void operator delete[](void*p)noexcept{std::free(p);}void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
namespace api=scratchbird::engine::internal_api;
namespace wire=scratchbird::wire;
namespace cd=scratchbird::core::diagnostics;
namespace cp=scratchbird::core::platform;
namespace {
void Check(bool ok,const char* why){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
void CheckCanonical(const cd::CanonicalDiagnosticMetadata& a,
                    const cd::CanonicalDiagnosticMetadata& b) {
  Check(a.code == b.code && a.severity == b.severity &&
        a.is_failure == b.is_failure && a.sqlstate == b.sqlstate &&
        a.numeric_binding == b.numeric_binding && a.retry_class == b.retry_class &&
        a.required_outcome == b.required_outcome &&
        a.diagnostic_class == b.diagnostic_class, "canonical source metadata lost");
}
void CheckMetadata(const api::EngineQueryResultMetadataV1& a,
                   const api::EngineQueryResultMetadataV1& b) {
  Check(a.statement_receipt_uuid == b.statement_receipt_uuid &&
        a.statement_snapshot_uuid == b.statement_snapshot_uuid &&
        a.datatype_catalog_snapshot_uuid == b.datatype_catalog_snapshot_uuid &&
        a.datatype_catalog_generation == b.datatype_catalog_generation &&
        a.datatype_registry_generation == b.datatype_registry_generation,
        "query schema owner or generation lost");
  Check(a.columns.size() == b.columns.size(), "query schema column count lost");
  for (std::size_t n = 0; n < std::min(a.columns.size(), b.columns.size()); ++n) {
    const auto& x = a.columns[n];
    const auto& y = b.columns[n];
    const auto& p = x.transport;
    const auto& q = y.transport;
    Check(p.ordinal == q.ordinal && p.name_occurrence == q.name_occurrence &&
          p.name == q.name && p.nullability == q.nullability &&
          p.descriptor_uuid == q.descriptor_uuid &&
          p.descriptor_generation == q.descriptor_generation &&
          p.type_uuid == q.type_uuid && p.type_generation == q.type_generation &&
          p.canonical_type_id == q.canonical_type_id && p.codec_id == q.codec_id &&
          p.codec_version == q.codec_version && p.codec_generation == q.codec_generation &&
          p.canonical_value_bytes == q.canonical_value_bytes,
          "query transport descriptor field lost");
    Check(x.bound_descriptor_uuid == y.bound_descriptor_uuid &&
          x.collation_uuid == y.collation_uuid &&
          x.timezone_profile_id == y.timezone_profile_id && x.width == y.width &&
          x.precision == y.precision && x.scale == y.scale,
          "query expression descriptor field lost");
  }
}
api::EngineUuid Id(unsigned n){api::EngineUuid v;v.bytes[0]=1;v.bytes[6]=0x70;v.bytes[8]=0x80;v.bytes[15]=n;return v;}
std::vector<std::uint8_t> Encode(const api::EngineApiResult& value){std::vector<std::uint8_t> out;if(!api::EncodeEngineApiResultSnapshot(value,&out))throw std::runtime_error("fixture encode refused");return out;}
api::EngineApiResult Fixture(){
  api::EngineApiResult r;r.ok=true;r.operation_id="fixture.source.operation";r.primary_object={Id(1),"table"};r.catalog_row_uuid=Id(2);r.transaction_uuid=Id(3);r.local_transaction_id=0xfedcba9876543210ull;
  r.embedded_trust_mode_observed=true;r.cluster_authority_required=true;
  api::EngineDescriptor d;d.descriptor_uuid=Id(4);d.type_uuid=Id(5);d.collation_uuid=Id(6);d.datatype_descriptor_uuid=Id(7);d.charset_uuid=Id(8);d.datatype_descriptor_generation=999;
  d.descriptor_kind="scalar";d.canonical_type_name="fixture.value";d.encoded_descriptor=std::string("metadata\0bytes",14);
  r.result_shape.result_kind="fixture.rows";r.result_shape.columns.push_back(d);
  api::EngineTypedValue v;v.descriptor=d;v.binary_value={0,1,2,3,255};v.encoded_value="source text";
  for(unsigned n=0;n<8;++n){v.state=static_cast<api::EngineValueState>(n);v.is_null=n==1;r.result_shape.rows.push_back({Id(20+n),{{"same_name",v},{"same_name",v}}});}
  r.evidence={{"binary",Id(9)},{"absent",api::EngineUuid{}},{"text","019d0000-0000-7000-8000-000000000009"}};
  r.unsupported_features.push_back({"historical_source_fact","not an implementation deferral"});
  api::EngineApiDiagnostic diagnostic;diagnostic.code="CATALOG.INVALID_INPUT";diagnostic.message_key="source.key";diagnostic.detail="private detail";diagnostic.fields={{"field","original"}};diagnostic.occurrence_uuid=Id(10).bytes;
  diagnostic.canonical_metadata=cd::CaptureCanonicalDiagnosticMetadata(diagnostic.code);
  cp::DiagnosticRecord native;native.status={cp::StatusCode::memory_limit_exceeded,cp::Severity::warning,cp::Subsystem::memory};native.diagnostic_code="native.source";native.message_key="native.key";native.arguments={{"argument","source value"}};native.trace_id="trace";native.source_component="source";native.remediation_hint="owner hint";
  diagnostic.native_source=cd::NativeDiagnosticSource{native,diagnostic.canonical_metadata};r.diagnostics.push_back(diagnostic);
  auto metadata=std::make_shared<api::EngineQueryResultMetadataV1>();metadata->statement_receipt_uuid=Id(11).bytes;metadata->statement_snapshot_uuid=Id(12).bytes;metadata->datatype_catalog_snapshot_uuid=Id(13).bytes;metadata->datatype_catalog_generation=123;metadata->datatype_registry_generation=456;
  api::EngineQueryResultColumnV1 c;c.transport.name="column";c.transport.nullability=wire::TypedResultNullability::nullable;c.transport.descriptor_uuid=Id(14).bytes;c.transport.descriptor_generation=2;c.transport.type_uuid=Id(15).bytes;c.transport.type_generation=3;c.transport.canonical_type_id=scratchbird::core::datatypes::CanonicalTypeId::int64;c.transport.codec_id="fixture.codec";c.transport.codec_version=1;c.transport.codec_generation=4;c.transport.canonical_value_bytes=8;c.bound_descriptor_uuid=Id(16).bytes;c.collation_uuid=Id(17).bytes;c.timezone_profile_id="source.timezone";c.width=8;c.precision=19;c.scale=0;metadata->columns.push_back(c);
  r.result_shape.query_metadata=metadata;auto values=std::make_shared<api::EngineQueryResultValuesV1>();values->metadata=metadata;
  wire::TypedResultCell cell;cell.canonical_payload={1,0,0,0,0,0,0,0};values->rows.push_back({123,{cell}});r.result_shape.query_values=values;
  auto& s=r.dml_summary;s.rows_changed=1;s.visible_rows_scanned=2;s.index_probes=3;s.append_calls=4;s.file_opens=5;s.flushes=6;s.page_reservations=7;s.row_extent_reservations=8;s.version_extent_reservations=9;s.page_extent_reservations=10;s.index_extent_reservations=11;s.preallocation_requests=12;s.preallocation_granted_pages=13;s.preallocation_capped=14;s.preallocation_refused=15;s.fallback_reasons={"original reason"};s.benchmark_clean=false;
  return r;
}
}
int main(){
  api::EngineApiResult empty;auto minimal=Encode(empty);
  std::vector<std::uint8_t> oracle(226,0);oracle[0]='S';oracle[1]='A';oracle[2]='P';oracle[3]='I';oracle[4]=2;oracle[223]=1;
  Check(minimal==oracle,"independent empty snapshot layout differs");
  auto source=Fixture();const auto bytes=Encode(source);api::EngineApiResult decoded;
  Check(api::DecodeEngineApiResultSnapshot(bytes,&decoded),"complete source result refused");
  Check(Encode(decoded)==bytes,"source snapshot not byte-exact after replay");
  Check(decoded.result_shape.columns==source.result_shape.columns,"bound descriptor fields lost");
  Check(decoded.primary_object.uuid==source.primary_object.uuid && decoded.primary_object.object_kind=="table" && decoded.catalog_row_uuid==Id(2) && decoded.transaction_uuid==Id(3) && decoded.local_transaction_id==source.local_transaction_id && decoded.embedded_trust_mode_observed && decoded.cluster_authority_required,"result identities or outcome flags lost");
  for(std::size_t n=0;n<source.result_shape.rows.size();++n){
    const auto& expected=source.result_shape.rows[n];const auto& actual=decoded.result_shape.rows[n];
    Check(actual.requested_row_uuid==expected.requested_row_uuid && actual.fields.size()==2,"row identity or duplicate fields lost");
    for(std::size_t f=0;f<2;++f)Check(actual.fields[f].first==expected.fields[f].first && actual.fields[f].second.descriptor==expected.fields[f].second.descriptor && actual.fields[f].second.binary_value==expected.fields[f].second.binary_value && actual.fields[f].second.encoded_value==expected.fields[f].second.encoded_value && actual.fields[f].second.is_null==expected.fields[f].second.is_null,"row descriptor/value/NULL flag lost");
  }
  const auto& counters=decoded.dml_summary;
  const std::array<std::uint64_t,15> counts{counters.rows_changed,counters.visible_rows_scanned,counters.index_probes,counters.append_calls,counters.file_opens,counters.flushes,counters.page_reservations,counters.row_extent_reservations,counters.version_extent_reservations,counters.page_extent_reservations,counters.index_extent_reservations,counters.preallocation_requests,counters.preallocation_granted_pages,counters.preallocation_capped,counters.preallocation_refused};
  for(std::size_t n=0;n<counts.size();++n)Check(counts[n]==n+1,"DML counter lost");
  Check(!counters.benchmark_clean && counters.fallback_reasons==std::vector<std::string>({"original reason"}),"counter metadata lost");
  Check(std::get<api::EngineUuid>(decoded.evidence[0].evidence_id)==Id(9) && std::get<api::EngineUuid>(decoded.evidence[1].evidence_id).is_nil() && std::get<std::string>(decoded.evidence[2].evidence_id)==std::get<std::string>(source.evidence[2].evidence_id),"evidence tag or value changed");
  Check(decoded.diagnostics.at(0).occurrence_uuid==source.diagnostics.at(0).occurrence_uuid,"diagnostic occurrence replaced");
  Check(decoded.diagnostics.at(0).canonical_metadata->retry_class==source.diagnostics.at(0).canonical_metadata->retry_class && decoded.diagnostics.at(0).native_source->record.arguments.at(0).value=="source value","source diagnostic metadata lost");
  Check(decoded.result_shape.query_values->metadata==decoded.result_shape.query_metadata,"shared schema identity lost");
  CheckMetadata(*decoded.result_shape.query_metadata, *source.result_shape.query_metadata);
  const auto& original_diagnostic = source.diagnostics[0];
  const auto& replayed_diagnostic = decoded.diagnostics[0];
  Check(replayed_diagnostic.code == original_diagnostic.code &&
        replayed_diagnostic.message_key == original_diagnostic.message_key &&
        replayed_diagnostic.detail == original_diagnostic.detail &&
        replayed_diagnostic.error == original_diagnostic.error &&
        replayed_diagnostic.fields.size() == 1 &&
        replayed_diagnostic.fields[0].key == "field" &&
        replayed_diagnostic.fields[0].value == "original", "diagnostic facts lost");
  CheckCanonical(*replayed_diagnostic.canonical_metadata, *original_diagnostic.canonical_metadata);
  CheckCanonical(*replayed_diagnostic.native_source->canonical_metadata,
                 *original_diagnostic.native_source->canonical_metadata);
  const auto& native = replayed_diagnostic.native_source->record;
  Check(native.status.code == cp::StatusCode::memory_limit_exceeded &&
        native.status.severity == cp::Severity::warning &&
        native.status.subsystem == cp::Subsystem::memory &&
        native.diagnostic_code == "native.source" && native.message_key == "native.key" &&
        native.arguments.size() == 1 && native.arguments[0].key == "argument" &&
        native.arguments[0].value == "source value" && native.trace_id == "trace" &&
        native.source_component == "source" && native.remediation_hint == "owner hint",
        "native diagnostic facts lost");
  Check(decoded.ok == source.ok && decoded.operation_id == source.operation_id &&
        decoded.unsupported_features.size() == 1 &&
        decoded.unsupported_features[0].feature == source.unsupported_features[0].feature &&
        decoded.unsupported_features[0].reason == source.unsupported_features[0].reason &&
        decoded.result_shape.result_kind == source.result_shape.result_kind,
        "source result classification lost");
  Check(decoded.result_shape.query_values->rows.at(0).row_ordinal==123 && decoded.result_shape.query_values->rows.at(0).cells.at(0).canonical_payload==std::vector<std::uint8_t>({1,0,0,0,0,0,0,0}),"canonical query values lost");
  for(unsigned n=0;n<8;++n)Check(decoded.result_shape.rows[n].fields[0].second.state==static_cast<api::EngineValueState>(n),"value state lost");
  for(unsigned association=0;association<3;++association){
    auto changed=source;auto values=std::make_shared<api::EngineQueryResultValuesV1>(*source.result_shape.query_values);
    if(association==0)values->metadata.reset();if(association==2)values->metadata=std::make_shared<api::EngineQueryResultMetadataV1>(*source.result_shape.query_metadata);changed.result_shape.query_values=values;
    auto encoded=Encode(changed);Check(api::DecodeEngineApiResultSnapshot(encoded,&decoded)&&Encode(decoded)==encoded,"query schema association changed");
    Check(association==0 ? !decoded.result_shape.query_values->metadata : (decoded.result_shape.query_values->metadata==decoded.result_shape.query_metadata)==(association==1),"metadata association semantics lost");
  }
  for(unsigned position=0;position<16;++position){auto changed=source;changed.primary_object.uuid.bytes[position]^=1;auto encoded=Encode(changed);Check(encoded!=bytes&&api::DecodeEngineApiResultSnapshot(encoded,&decoded)&&decoded.primary_object.uuid==changed.primary_object.uuid,"identity byte lost");}
  const auto refuse=[&](std::span<const std::uint8_t> malformed){auto prior=source;Check(!api::DecodeEngineApiResultSnapshot(malformed,&prior)&&Encode(prior)==bytes,"malformed decode accepted or changed output");};
  for(std::size_t n=0;n<bytes.size();++n)refuse(std::span(bytes).first(n));
  for(auto offset:{4u,6u,8u}){auto bad=minimal;bad[offset]=255;refuse(bad);}
  auto bad=minimal;bad.push_back(0);refuse(bad);bad=minimal;bad[13]=bad[14]=bad[15]=bad[16]=255;refuse(bad);
  auto evidence=empty;evidence.evidence.push_back({"id",Id(1)});bad=Encode(evidence);bad[31]=2;refuse(bad);
  for (unsigned tag = 1; tag < 256; ++tag) {
    if (tag == 2) continue; // Independent metadata requires a following record.
    bad = minimal;
    bad[38] = 1; // Query values are present; query_metadata remains absent.
    bad.insert(bad.begin() + 39, {static_cast<std::uint8_t>(tag), 0, 0, 0, 0});
    refuse(bad); // Shared-but-absent and every unknown metadata association.
  }
  for (unsigned severity = 1; severity <= 13; ++severity) {
    auto value = source;
    value.diagnostics[0].canonical_metadata->severity = static_cast<cd::CanonicalSeverity>(severity);
    Check(api::DecodeEngineApiResultSnapshot(Encode(value), &decoded) &&
          decoded.diagnostics[0].canonical_metadata->severity ==
              value.diagnostics[0].canonical_metadata->severity,
          "canonical severity flattened during replay");
  }
  // IDs are located by their independent fixture bytes, not codec internals.
  for (unsigned identity = 1; identity <= 27; ++identity) {
    if (identity == 18 || identity == 19) continue;
    const auto id = Id(identity);
    const auto pos = std::search(bytes.begin(), bytes.end(), id.bytes.begin(), id.bytes.end());
    Check(pos != bytes.end(), "fixture identity absent from binary snapshot");
    if (pos == bytes.end()) continue;
    const auto offset = static_cast<std::size_t>(pos - bytes.begin());
    for (unsigned version = 0; version < 16; ++version) {
      if (version == 7) continue;
      bad = bytes; bad[offset + 6] = static_cast<std::uint8_t>(version << 4); refuse(bad);
    }
    bad = bytes; bad[offset + 8] = 0; refuse(bad);
  }
  // These bytes are user data, not identity authority: retain all UUID versions.
  for (unsigned version = 0; version < 16; ++version) {
    auto user_uuid = Id(90); user_uuid.bytes[6] = static_cast<std::uint8_t>(version << 4);
    auto value = empty;
    api::EngineTypedValue cell;
    cell.binary_value.assign(user_uuid.bytes.begin(), user_uuid.bytes.end());
    value.result_shape.rows.push_back({Id(91), {{"user_uuid", cell}}});
    Check(api::DecodeEngineApiResultSnapshot(Encode(value), &decoded) &&
          decoded.result_shape.rows[0].fields[0].second.binary_value == cell.binary_value,
          "user UUID version reinterpreted as system identity");
  }
  // Tiny valid wire records can expand to large in-memory descriptor objects. Refuse
  // the decoded-storage bound before allocating the declared collection.
  auto expanded = minimal;
  constexpr std::uint32_t many_descriptors =
      64u*1024u*1024u / sizeof(api::EngineDescriptor) + 1;
  for (unsigned byte = 0; byte < 4; ++byte)
    expanded[29 + byte] = static_cast<std::uint8_t>(many_descriptors >> (8 * byte));
  // The normative empty descriptor has five raw16 UUIDs, three u32 string
  // lengths and one u64 generation: exactly 100 bytes, without host padding.
  expanded.insert(expanded.begin() + 33, std::size_t(many_descriptors) * 100, 0);
  Check(expanded.size() < 64u*1024u*1024u && many_descriptors <= 1048576,
        "decoded bound fixture exceeds independent wire/count bounds");
  refuse(expanded);
  for(unsigned version=0;version<16;++version)if(version!=7){auto invalid=source;invalid.primary_object.uuid.bytes[6]=version<<4;std::vector<std::uint8_t> output{42};Check(!api::EncodeEngineApiResultSnapshot(invalid,&output)&&output==std::vector<std::uint8_t>({42}),"invalid system UUID encoded or changed output");}
  auto nil_occurrence=source;nil_occurrence.diagnostics[0].occurrence_uuid={};std::vector<std::uint8_t> output{42};Check(!api::EncodeEngineApiResultSnapshot(nil_occurrence,&output)&&output[0]==42,"nil diagnostic occurrence encoded");
  Check(!api::EncodeEngineApiResultSnapshot(source,nullptr)&&!api::DecodeEngineApiResultSnapshot(bytes,nullptr),"null output accepted");
  auto excessive=empty;excessive.operation_id.assign(64u*1024u*1024u,'x');output={42};Check(!api::EncodeEngineApiResultSnapshot(excessive,&output)&&output==std::vector<std::uint8_t>({42}),"snapshot bound ignored or output changed");
  auto invalid_state=source;invalid_state.result_shape.rows[0].fields[0].second.state=static_cast<api::EngineValueState>(255);Check(!api::EncodeEngineApiResultSnapshot(invalid_state,&output),"unknown value state admitted");
  for(bool decode:{false,true}){bool completed=false;unsigned mode_faults=0;
    for(long point=0;point<512;++point){auto prior=source;std::vector<std::uint8_t> out{42};
      try{fail_after=point;const bool ok=decode?api::DecodeEngineApiResultSnapshot(bytes,&prior):api::EncodeEngineApiResultSnapshot(source,&out);fail_after=-1;completed=true;Check(ok&&(decode?Encode(prior)==bytes:out==bytes),"allocation retry lost snapshot");break;}
      catch(const std::bad_alloc&){fail_after=-1;++faults;++mode_faults;Check(Encode(prior)==bytes&&out==std::vector<std::uint8_t>({42}),"allocation failure changed output");}
    }Check(completed&&mode_faults>0,"allocation fault sweep incomplete");
  }
  std::cout<<"api_result_snapshot checks="<<checks<<" faults="<<faults<<" failures="<<failures<<'\n';return failures?1:0;
}
