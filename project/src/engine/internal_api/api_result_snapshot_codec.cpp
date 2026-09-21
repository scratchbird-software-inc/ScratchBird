// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "api_result_snapshot_codec.hpp"
#include "query/result_metadata.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <type_traits>

namespace scratchbird::engine::internal_api {
namespace {
constexpr std::size_t limit=64u*1024u*1024u, count_limit=1048576;
constexpr std::size_t shared_overhead=4*sizeof(void*);
template<class> inline constexpr bool vector_type=false;
template<class> inline constexpr bool optional_type=false;
template<class> inline constexpr bool array_type=false;
template<class T,class A> inline constexpr bool vector_type<std::vector<T,A>> = true;
template<class T> inline constexpr bool optional_type<std::optional<T>> = true;
template<class T,std::size_t N> inline constexpr bool array_type<std::array<T,N>> = true;
template<class A,class T> void Record(A&,T&);
template<class T> bool ValidEnum(T v){
  const auto n=static_cast<std::underlying_type_t<T>>(v);
  if constexpr(std::is_same_v<T,EngineValueState>)return n<=7;
  else if constexpr(std::is_same_v<T,wire::TypedResultNullability>)return n<=2;
  else if constexpr(std::is_same_v<T,wire::TypedResultValueState>)return n<=1;
  else if constexpr(std::is_same_v<T,core::diagnostics::CanonicalSeverity>)return n>=1&&n<=13;
  else if constexpr(std::is_same_v<T,core::platform::Severity>)return n>=1&&n<=5;
  else return true; // Native status/subsystem and datatype code are source facts.
}
struct Writer {
  static constexpr bool reading=false;
  bool ok=true;std::vector<std::uint8_t> bytes;std::size_t owned=sizeof(EngineApiResult);
  bool Charge(std::size_t n){if(!ok||n>limit-owned){ok=false;return false;}owned+=n;return true;}
  void Raw(const std::uint8_t* p,std::size_t n){
    if(!ok)return;
    if(n>limit-bytes.size()){ok=false;return;}
    if(n)bytes.insert(bytes.end(),p,p+n);
  }
  template<class...T> void operator()(const T&...v){(One(v),...);}
  template<class T> void One(const T& v){
    if(!ok)return;
    if constexpr(std::is_same_v<T,bool>){One(static_cast<std::uint8_t>(v));}
    else if constexpr(std::is_unsigned_v<T>){
      std::array<std::uint8_t,sizeof(T)> b{};
      for(std::size_t i=0;i<sizeof(T);++i)b[i]=static_cast<std::uint8_t>(v>>(8*i));Raw(b.data(),b.size());
    }else if constexpr(std::is_enum_v<T>){if(!ValidEnum(v)){ok=false;return;}One(static_cast<std::underlying_type_t<T>>(v));}
    else if constexpr(std::is_same_v<T,std::string>){
      if(v.size()>limit||!Charge(v.size()+1)){ok=false;return;}One(static_cast<std::uint32_t>(v.size()));Raw(reinterpret_cast<const std::uint8_t*>(v.data()),v.size());
    }else if constexpr(std::is_same_v<T,EngineUuid>){
      if(!v.is_nil()&&!core::uuid::IsEngineIdentityUuid(v)){ok=false;return;}One(v.bytes);
    }else if constexpr(array_type<T>){
      static_assert(std::is_same_v<typename T::value_type,std::uint8_t>);
      if constexpr(std::tuple_size_v<T> == 16){const EngineUuid id{v};if(!id.is_nil()&&!core::uuid::IsEngineIdentityUuid(id)){ok=false;return;}}
      Raw(v.data(),v.size());
    }
    else if constexpr(vector_type<T>){
      constexpr bool blob=std::is_same_v<typename T::value_type,std::uint8_t>;
      if(v.size()>(blob?limit:count_limit)||v.size()>limit/sizeof(typename T::value_type)||!Charge(v.size()*sizeof(typename T::value_type))){ok=false;return;}
      One(static_cast<std::uint32_t>(v.size()));
      if constexpr(blob)Raw(v.data(),v.size());else for(const auto& x:v)One(x);
    }else if constexpr(optional_type<T>){One(v.has_value());if(v)One(*v);}
    else if constexpr(std::is_same_v<T,EngineEvidenceValue>){
      if(v.valueless_by_exception()){ok=false;return;}
      One(static_cast<std::uint8_t>(v.index()));std::visit([&](const auto& x){One(x);},v);
    }else Record(*this,v);
  }
};
struct Reader {
  static constexpr bool reading=true;
  bool ok=true;std::span<const std::uint8_t> bytes;std::size_t offset=0,owned=sizeof(EngineApiResult);
  bool Charge(std::size_t n){if(!ok||n>limit-owned){ok=false;return false;}owned+=n;return true;}
  bool Raw(std::uint8_t* p,std::size_t n){
    if(!ok||n>bytes.size()-offset){ok=false;return false;}
    if(n)std::copy_n(bytes.data()+offset,n,p);offset+=n;return true;
  }
  template<class...T>void operator()(T&...v){(One(v),...);}
  template<class T>void One(T& v){
    if(!ok)return;
    if constexpr(std::is_same_v<T,bool>){std::uint8_t b=0;One(b);if(b>1){ok=false;return;}v=b!=0;}
    else if constexpr(std::is_unsigned_v<T>){
      std::array<std::uint8_t,sizeof(T)> b{};if(!Raw(b.data(),b.size()))return;
      v=0;for(std::size_t i=0;i<sizeof(T);++i)v|=static_cast<T>(b[i])<<(8*i);
    }else if constexpr(std::is_enum_v<T>){std::underlying_type_t<T> n{};One(n);v=static_cast<T>(n);ok=ok&&ValidEnum(v);}
    else if constexpr(std::is_same_v<T,std::string>){
      std::uint32_t n=0;One(n);if(!ok||n>bytes.size()-offset||!Charge(n+1)){ok=false;return;}
      v.assign(reinterpret_cast<const char*>(bytes.data()+offset),n);offset+=n;
    }else if constexpr(std::is_same_v<T,EngineUuid>){One(v.bytes);ok=ok&&(v.is_nil()||core::uuid::IsEngineIdentityUuid(v));}
    else if constexpr(array_type<T>){
      static_assert(std::is_same_v<typename T::value_type,std::uint8_t>);Raw(v.data(),v.size());
      if constexpr(std::tuple_size_v<T> == 16){const EngineUuid id{v};ok=ok&&(id.is_nil()||core::uuid::IsEngineIdentityUuid(id));}
    }
    else if constexpr(vector_type<T>){
      using E=typename T::value_type;constexpr bool blob=std::is_same_v<E,std::uint8_t>;
      std::uint32_t n=0;One(n);
      if(!ok||n>(blob?limit:count_limit)||n>bytes.size()-offset||n>limit/sizeof(E)||!Charge(n*sizeof(E))){ok=false;return;}
      v.resize(n);if constexpr(blob)Raw(v.data(),n);else for(auto& x:v)One(x);
    }else if constexpr(optional_type<T>){bool present=false;One(present);if(!ok)return;if(present){v.emplace();One(*v);}else v.reset();}
    else if constexpr(std::is_same_v<T,EngineEvidenceValue>){
      std::uint8_t tag=0;One(tag);if(!ok)return;
      if(tag==0){std::string x;One(x);v=std::move(x);}else if(tag==1){EngineUuid x;One(x);v=x;}else ok=false;
    }else Record(*this,v);
  }
};
template<class A,class T>void Metadata(A& a,T& p){
  bool present=bool(p);a(present);if(!a.ok)return;
  if(present&&!a.Charge(sizeof(EngineQueryResultMetadataV1)+shared_overhead))return;
  if constexpr(A::reading){
    if(!present){p.reset();return;}
    auto v=std::make_shared<EngineQueryResultMetadataV1>();a(*v);p=std::move(v);
  }else if(present)a(*p);
}
template<class A,class T>void Shape(A& a,T& v){
  a(v.result_kind,v.columns,v.rows);Metadata(a,v.query_metadata);
  bool present=bool(v.query_values);a(present);if(!a.ok)return;
  if(present&&!a.Charge(sizeof(EngineQueryResultValuesV1)+shared_overhead))return;
  if constexpr(A::reading){
    if(!present){v.query_values.reset();return;}
    auto values=std::make_shared<EngineQueryResultValuesV1>();std::uint8_t association=0;a(association);
    if(association==1){if(!v.query_metadata){a.ok=false;return;}values->metadata=v.query_metadata;}
    else if(association==2){
      if(!a.Charge(sizeof(EngineQueryResultMetadataV1)+shared_overhead))return;
      auto metadata=std::make_shared<EngineQueryResultMetadataV1>();a(*metadata);values->metadata=std::move(metadata);
    }else if(association!=0){a.ok=false;return;}
    a(values->rows);v.query_values=std::move(values);
  }else if(present){
    const auto& m=v.query_values->metadata;
    const std::uint8_t association=!m?0:m==v.query_metadata?1:2;a(association);
    if(association==2){if(!a.Charge(sizeof(EngineQueryResultMetadataV1)+shared_overhead))return;a(*m);}a(v.query_values->rows);
  }
}
template<class A,class T>void Record(A& a,T& v){
  using U=std::remove_cv_t<T>;
#define REC(type,...) if constexpr(std::is_same_v<U,type>){a(__VA_ARGS__);}
  REC(EngineDescriptor,v.descriptor_uuid,v.descriptor_kind,v.canonical_type_name,v.encoded_descriptor,v.type_uuid,v.collation_uuid,v.datatype_descriptor_uuid,v.datatype_descriptor_generation,v.charset_uuid)
  else REC(EngineTypedValue,v.descriptor,v.encoded_value,v.binary_value,v.is_null,v.state)
  else REC(EngineRowValue,v.requested_row_uuid,v.fields)
  else REC(EngineEvidenceReference,v.evidence_kind,v.evidence_id)
  else REC(EngineUnsupportedFeature,v.feature,v.reason)
  else REC(EngineObjectReference,v.uuid,v.object_kind)
  else REC(EngineApiDiagnosticField,v.key,v.value)
  else REC(core::platform::DiagnosticArgument,v.key,v.value)
  else REC(core::platform::Status,v.code,v.severity,v.subsystem)
  else REC(core::platform::DiagnosticRecord,v.status,v.diagnostic_code,v.message_key,v.arguments,v.trace_id,v.source_component,v.remediation_hint)
  else REC(core::diagnostics::CanonicalDiagnosticMetadata,v.code,v.severity,v.is_failure,v.sqlstate,v.numeric_binding,v.retry_class,v.required_outcome,v.diagnostic_class)
  else REC(core::diagnostics::NativeDiagnosticSource,v.record,v.canonical_metadata)
  else if constexpr(std::is_same_v<U,EngineApiDiagnostic>){
    a(v.code,v.message_key,v.detail,v.error,v.fields,v.occurrence_uuid,v.canonical_metadata,v.native_source);
    a.ok=a.ok&&core::uuid::IsEngineIdentityUuid(EngineUuid{v.occurrence_uuid});
  }
  else REC(wire::TypedResultColumnDescriptor,v.ordinal,v.name_occurrence,v.name,v.nullability,v.descriptor_uuid,v.descriptor_generation,v.type_uuid,v.type_generation,v.canonical_type_id,v.codec_id,v.codec_version,v.codec_generation,v.canonical_value_bytes)
  else REC(EngineQueryResultColumnV1,v.transport,v.bound_descriptor_uuid,v.collation_uuid,v.timezone_profile_id,v.width,v.precision,v.scale)
  else REC(EngineQueryResultMetadataV1,v.statement_receipt_uuid,v.statement_snapshot_uuid,v.datatype_catalog_snapshot_uuid,v.datatype_catalog_generation,v.datatype_registry_generation,v.columns)
  else REC(wire::TypedResultCell,v.column_ordinal,v.name_occurrence,v.state,v.canonical_payload)
  else REC(wire::TypedResultRow,v.row_ordinal,v.cells)
  else if constexpr(std::is_same_v<U,EngineResultShape>)Shape(a,v);
  else REC(EngineDmlSummaryCounters,v.rows_changed,v.visible_rows_scanned,v.index_probes,v.append_calls,v.file_opens,v.flushes,v.page_reservations,v.row_extent_reservations,v.version_extent_reservations,v.page_extent_reservations,v.index_extent_reservations,v.preallocation_requests,v.preallocation_granted_pages,v.preallocation_capped,v.preallocation_refused,v.fallback_reasons,v.benchmark_clean)
  else REC(EngineApiResult,v.ok,v.operation_id,v.diagnostics,v.unsupported_features,v.evidence,v.result_shape,v.primary_object,v.catalog_row_uuid,v.transaction_uuid,v.local_transaction_id,v.dml_summary,v.embedded_trust_mode_observed,v.cluster_authority_required)
  else if constexpr(requires{v.first;v.second;})a(v.first,v.second);
  else static_assert(sizeof(U)==0,"unhandled snapshot record");
#undef REC
}
}
bool EncodeEngineApiResultSnapshot(const EngineApiResult& result,std::vector<std::uint8_t>* output){
  if(!output)return false;
  Writer writer;const std::array<std::uint8_t,8> header{'S','A','P','I',2,0,0,0};writer(header,result);
  if(!writer.ok)return false;output->swap(writer.bytes);return true;
}
bool DecodeEngineApiResultSnapshot(std::span<const std::uint8_t> bytes,EngineApiResult* output){
  constexpr std::array<std::uint8_t,8> header{'S','A','P','I',2,0,0,0};
  if(!output||bytes.size()<header.size()||bytes.size()>limit||!std::equal(header.begin(),header.end(),bytes.begin()))return false;
  Reader reader{true,bytes,header.size()};EngineApiResult result;reader(result);
  if(!reader.ok||reader.offset!=bytes.size())return false;
  static_assert(std::is_nothrow_move_assignable_v<EngineApiResult>);
  *output=std::move(result);return true;
}
} // namespace scratchbird::engine::internal_api
