// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_interval.hpp"

#include "datatype_binary_view.hpp"
#include "datatype_physical_encoding.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <new>

namespace scratchbird::core::datatypes {
namespace {

using platform::LoadLittle32;
using platform::LoadLittle64;
using platform::Severity;
using platform::StatusCode;
using platform::StoreLittle16;
using platform::StoreLittle32;
using platform::StoreLittle64;
using platform::Subsystem;

constexpr platform::Uuid U(std::array<byte, 16> b) noexcept { return {b}; }
inline constexpr platform::Uuid kSnapshot = U({0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10});
inline constexpr platform::Uuid kDescriptor = U({0x93,0x01,0,0,0x69,0x6e,0x74,0x65,0xb2,0x76,0x61,0x6c,0,0,0,0});
inline constexpr platform::Uuid kType = U({0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x22});
inline constexpr platform::Uuid kCodec = U({0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x23});

constexpr DatatypePolicyIdentityV3 P(byte suffix) noexcept {
  return {U({0x01,0xa1,0x05,0x10,0x69,0x6e,0x70,0x00,0x80,0,0,0,0,0,0,suffix}),1};
}
inline constexpr auto kDescriptorPolicy=P(1), kCanonicalPolicy=P(2), kNoOrderPolicy=P(3),
    kHashPolicy=P(4), kOperationPolicy=P(5), kRenderPolicy=P(6), kCastPolicy=P(7),
    kSubtypePolicy=P(8), kPrecisionPolicy=P(9), kNormalizationPolicy=P(10),
    kTemporalBoundaryPolicy=P(11), kStorageEpochPolicy=P(12), kIndexPolicy=P(13),
    kStatisticsPolicy=P(14), kBackupPolicy=P(15), kProtectionPolicy=P(16),
    kComponentPolicy=P(17), kDiagnosticPolicy=P(18), kMetricPolicy=P(19);

inline constexpr std::array<byte,32> kProfileDigest{{
  0xdb,0x1e,0x02,0x36,0xa8,0xdd,0x8a,0x04,0x56,0x1a,0x57,0x86,0x8c,0xcd,0x1a,0xb8,
  0xba,0xd5,0x08,0xf3,0xc1,0xc7,0x51,0xaa,0x5d,0x6b,0xa8,0xb5,0xb4,0x28,0x23,0xeb}};
inline constexpr std::array<byte,32> kEqualityDigest{{
  0xc0,0x78,0x39,0x42,0xe7,0xdb,0x60,0xaa,0x79,0x34,0x6d,0xdc,0x22,0x3e,0x48,0xfa,
  0xbc,0xc1,0x97,0xb1,0x89,0x4c,0xc8,0x14,0x90,0xa4,0xf4,0x8b,0x90,0xd4,0x98,0xf6}};

Status OkStatus() noexcept { return {StatusCode::ok,Severity::info,Subsystem::datatypes}; }
Status ErrorStatus() noexcept { return {StatusCode::platform_required_feature_missing,Severity::error,Subsystem::datatypes}; }
Status ResourceStatus() noexcept { return {StatusCode::memory_allocation_failed,Severity::error,Subsystem::datatypes}; }

template<class R> R Success() noexcept { R r; r.status=OkStatus(); r.diagnostic.status=r.status; return r; }
template<class R> R Failure(std::string_view code,std::string_view detail,Status status=ErrorStatus()) noexcept {
  R r; r.status=status; r.diagnostic.status=status; r.diagnostic.diagnostic_code=code; r.diagnostic.detail=detail; return r;
}
bool Cancelled(const IntervalExecutionControlV3& c) noexcept { return c.cancelled && c.cancelled(c.cancellation_context); }
void SecureClear(void* p,std::size_t n) noexcept { auto* q=static_cast<volatile unsigned char*>(p); while(n--)*q++=0; }
class Clear final {
 public: Clear(void* p,std::size_t n,IntervalScrubClassV3 k,const IntervalExecutionControlV3* c=nullptr) noexcept:p_(p),n_(n),k_(k),c_(c){}
 ~Clear(){SecureClear(p_,n_);
 if(c_&&c_->observe_scrubbed)c_->observe_scrubbed(c_->scrub_observer_context,k_,static_cast<const byte*>(p_),n_);} private:void* p_;
 std::size_t n_;IntervalScrubClassV3 k_;
 const IntervalExecutionControlV3* c_;
};
template<class T> class VectorClear final { public: VectorClear(std::vector<T>*v,IntervalScrubClassV3 k,const IntervalExecutionControlV3&c):v_(v),k_(k),c_(c){} ~VectorClear(){if(v_&&!v_->empty()){auto n=v_->size()*sizeof(T);
 SecureClear(v_->data(),n);
 if(c_.observe_scrubbed)c_.observe_scrubbed(c_.scrub_observer_context,k_,reinterpret_cast<const byte*>(v_->data()),n);}} void Disarm(){v_=nullptr;} private:std::vector<T>*v_;IntervalScrubClassV3 k_;
 const IntervalExecutionControlV3&c_;};

bool Same(const DatatypePolicyIdentityV3&a,const DatatypePolicyIdentityV3&b) noexcept{return a.uuid==b.uuid&&a.generation==b.generation;}
bool SameLegacy(const DatatypeTypeCodecIdentityRowV1&a,const DatatypeTypeCodecIdentityRowV1&b) noexcept {
  return a.catalog_snapshot_uuid==b.catalog_snapshot_uuid&&a.catalog_generation==b.catalog_generation&&a.registry_generation==b.registry_generation&&
    a.descriptor_uuid==b.descriptor_uuid&&a.descriptor_generation==b.descriptor_generation&&a.type_uuid==b.type_uuid&&a.type_generation==b.type_generation&&
    a.codec_uuid==b.codec_uuid&&a.codec_version==b.codec_version&&a.codec_generation==b.codec_generation&&a.canonical_value_bytes==b.canonical_value_bytes&&
    a.null_supported==b.null_supported&&a.datatype_identity_code==b.datatype_identity_code&&a.null_encoding_code==b.null_encoding_code&&
    a.byte_order_code==b.byte_order_code&&a.signed_code==b.signed_code&&a.representation_code==b.representation_code&&
    a.canonical_value_minimum_bytes==b.canonical_value_minimum_bytes&&a.canonical_value_maximum_bytes==b.canonical_value_maximum_bytes&&
    a.canonical_value_exact_bytes==b.canonical_value_exact_bytes&&a.canonical_binary_type_code==b.canonical_binary_type_code&&
    a.canonical_value_variable_width==b.canonical_value_variable_width&&a.canonical_value_exact_zero_is_width_marker==b.canonical_value_exact_zero_is_width_marker&&
    a.canonical_byte_order==b.canonical_byte_order&&a.canonical_representation==b.canonical_representation&&a.canonical_charset==b.canonical_charset&&
    a.shortest_form_utf8_required==b.shortest_form_utf8_required&&a.implicit_normalization_allowed==b.implicit_normalization_allowed&&
    a.descriptor_bound_collation_required==b.descriptor_bound_collation_required&&a.empty_value_distinct_from_sql_null==b.empty_value_distinct_from_sql_null&&
    a.sql_null_requires_zero_payload==b.sql_null_requires_zero_payload&&a.variable_width_storage_without_truncation==b.variable_width_storage_without_truncation&&
    a.invalid_encoding_diagnostic_id==b.invalid_encoding_diagnostic_id&&a.numeric_context_uuid==b.numeric_context_uuid&&
    a.numeric_context_generation==b.numeric_context_generation&&a.special_value_policy_uuid==b.special_value_policy_uuid&&
    a.special_value_policy_generation==b.special_value_policy_generation&&a.comparison_policy_uuid==b.comparison_policy_uuid&&
    a.comparison_policy_generation==b.comparison_policy_generation&&a.comparison_profile==b.comparison_profile&&a.allow_special_values==b.allow_special_values;
}
bool SameIdentity(const DatatypeTypeCodecIdentityRowV3&a,const DatatypeTypeCodecIdentityRowV3&b) noexcept {
 return SameLegacy(a.legacy_fields,b.legacy_fields)&&Same(a.descriptor_policy,b.descriptor_policy)&&Same(a.canonicalization_policy,b.canonicalization_policy)&&Same(a.ordering_policy,b.ordering_policy)&&Same(a.hash_policy,b.hash_policy)&&Same(a.operation_policy,b.operation_policy);
}
const DatatypeTypeCodecIdentityRowV3* Current(CanonicalTypeId type) noexcept {
 const auto rows=CurrentDatatypeTypeCodecIdentityRowsV3();
 if(rows.size()!=259)return nullptr;
 const auto code=static_cast<u32>(type);
 const DatatypeTypeCodecIdentityRowV3* found=nullptr;
 for(const auto&r:rows)if(r.legacy_fields.catalog_snapshot_uuid==kSnapshot&&r.legacy_fields.catalog_generation==10&&r.legacy_fields.registry_generation==10&&r.legacy_fields.canonical_binary_type_code==code){if(found)return nullptr;found=&r;}
 return found;
}
bool ExactReceipt(const IntervalAuthorityReceiptV3&r) noexcept{return r.statement_receipt_uuid==kSnapshot&&r.catalog_snapshot_uuid==kSnapshot&&r.catalog_generation==10&&r.registry_generation==10;}
void PutUuid(byte*out,const platform::Uuid&u) noexcept{std::memcpy(out,u.bytes.data(),16);} void PutPolicy(byte*out,const DatatypePolicyIdentityV3&p) noexcept{PutUuid(out,p.uuid);StoreLittle64(out+16,p.generation);}

template<std::size_t N> void CommonHeader(std::array<byte,N>&m,const char magic[9],const IntervalValidatedProfileHandleV3&p) noexcept {
 const auto&r=p.identity.legacy_fields;
 std::memcpy(m.data(),magic,8);StoreLittle16(m.data()+8,1);StoreLittle16(m.data()+10,static_cast<u32>(N));StoreLittle32(m.data()+12,static_cast<u32>(N));PutUuid(m.data()+16,p.receipt.catalog_snapshot_uuid);StoreLittle64(m.data()+32,p.receipt.catalog_generation);StoreLittle64(m.data()+40,p.receipt.registry_generation);PutUuid(m.data()+48,r.descriptor_uuid);StoreLittle64(m.data()+64,r.descriptor_generation);PutUuid(m.data()+72,r.type_uuid);StoreLittle64(m.data()+88,r.type_generation);PutUuid(m.data()+96,r.codec_uuid);StoreLittle32(m.data()+112,r.codec_version);StoreLittle32(m.data()+116,0);StoreLittle64(m.data()+120,r.codec_generation);
}
std::array<byte,608> ProfileMaterial(const IntervalValidatedProfileHandleV3&p) noexcept {
 std::array<byte,608>m{};CommonHeader(m,"SBINPP01",p);
 std::array<DatatypePolicyIdentityV3,19>x{{p.identity.descriptor_policy,p.identity.canonicalization_policy,p.identity.ordering_policy,p.identity.hash_policy,p.identity.operation_policy,p.render_policy,p.cast_policy,p.subtype_policy,p.precision_policy,p.normalization_policy,p.temporal_application_boundary_policy,p.storage_epoch_policy,p.index_policy,p.statistics_policy,p.backup_transport_policy,p.protection_policy,p.component_adapter_policy,p.diagnostic_policy,p.metric_policy}};
 std::size_t o=128;
 for(auto&q:x){PutPolicy(m.data()+o,q);o+=24;}StoreLittle32(m.data()+584,1);StoreLittle32(m.data()+588,16);StoreLittle32(m.data()+592,7);StoreLittle32(m.data()+596,1);StoreLittle32(m.data()+600,221);StoreLittle32(m.data()+604,13);
 return m;
}
std::array<byte,344> EqualityMaterial(const IntervalValidatedProfileHandleV3&p) noexcept {
 std::array<byte,344>m{};CommonHeader(m,"SBINEQ01",p);
 std::array<DatatypePolicyIdentityV3,8>x{{p.identity.descriptor_policy,p.identity.canonicalization_policy,p.identity.ordering_policy,p.identity.hash_policy,p.subtype_policy,p.precision_policy,p.normalization_policy,p.storage_epoch_policy}};
 std::size_t o=128;
 for(auto&q:x){PutPolicy(m.data()+o,q);o+=24;}StoreLittle32(m.data()+320,63);StoreLittle32(m.data()+324,16);StoreLittle32(m.data()+328,1);StoreLittle32(m.data()+332,1);StoreLittle32(m.data()+336,1);StoreLittle32(m.data()+340,0);
 return m;
}

bool Digest(std::span<const byte> in,std::array<byte,32>*out,const IntervalExecutionControlV3*ctl=nullptr) noexcept {
 if(!out)return false;
 std::array<u32,8>s{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};Clear cs(s.data(),sizeof(s),IntervalScrubClassV3::sha_state,ctl);
 constexpr std::array<u32,64>k{{0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u}};
 auto ro=[](u32 v,unsigned n){return(v>>n)|(v<<(32-n));};
 auto block=[&](const byte*b){std::array<u32,64>w{};Clear cw(w.data(),sizeof(w),IntervalScrubClassV3::sha_schedule,ctl);
 for(unsigned i=0;i<16;++i)w[i]=(u32(b[4*i])<<24)|(u32(b[4*i+1])<<16)|(u32(b[4*i+2])<<8)|b[4*i+3];
 for(unsigned i=16;i<64;++i){u32 a=ro(w[i-15],7)^ro(w[i-15],18)^(w[i-15]>>3),z=ro(w[i-2],17)^ro(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+a+w[i-7]+z;}u32 a=s[0],b0=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
 for(unsigned i=0;i<64;++i){u32 t1=h+(ro(e,6)^ro(e,11)^ro(e,25))+((e&f)^((~e)&g))+k[i]+w[i],t2=(ro(a,2)^ro(a,13)^ro(a,22))+((a&b0)^(a&c)^(b0&c));h=g;g=f;f=e;e=d+t1;d=c;c=b0;b0=a;a=t1+t2;}s[0]+=a;s[1]+=b0;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;};
 std::size_t o=0;while(in.size()-o>=64){block(in.data()+o);o+=64;}std::array<byte,128>tail{};Clear ct(tail.data(),tail.size(),IntervalScrubClassV3::sha_tail,ctl);
 auto rem=in.size()-o;
 if(rem)std::memcpy(tail.data(),in.data()+o,rem);
 tail[rem]=0x80;
 auto n=rem<56?64u:128u;
 u64 bits=static_cast<u64>(in.size())*8;
 for(unsigned i=0;i<8;++i)tail[n-1-i]=static_cast<byte>(bits>>(8*i));
 block(tail.data());
 if(n==128)block(tail.data()+64);
 for(unsigned i=0;i<8;++i){(*out)[4*i]=byte(s[i]>>24);(*out)[4*i+1]=byte(s[i]>>16);(*out)[4*i+2]=byte(s[i]>>8);(*out)[4*i+3]=byte(s[i]);}
 return true;
}

bool ProfileValid(const IntervalValidatedProfileHandleV3&p,const IntervalExecutionControlV3*ctl=nullptr) noexcept {
 if(!ExactReceipt(p.receipt)||!IsExactCanonicalIntervalTypeCodecIdentityV3(p.identity)||p.identity.legacy_fields.descriptor_uuid!=kDescriptor||p.identity.legacy_fields.type_uuid!=kType||p.identity.legacy_fields.codec_uuid!=kCodec||
 !Same(p.identity.descriptor_policy,kDescriptorPolicy)||!Same(p.identity.canonicalization_policy,kCanonicalPolicy)||!Same(p.identity.ordering_policy,kNoOrderPolicy)||!Same(p.identity.hash_policy,kHashPolicy)||!Same(p.identity.operation_policy,kOperationPolicy)||
 !Same(p.render_policy,kRenderPolicy)||!Same(p.cast_policy,kCastPolicy)||!Same(p.subtype_policy,kSubtypePolicy)||!Same(p.precision_policy,kPrecisionPolicy)||!Same(p.normalization_policy,kNormalizationPolicy)||!Same(p.temporal_application_boundary_policy,kTemporalBoundaryPolicy)||!Same(p.storage_epoch_policy,kStorageEpochPolicy)||!Same(p.index_policy,kIndexPolicy)||!Same(p.statistics_policy,kStatisticsPolicy)||!Same(p.backup_transport_policy,kBackupPolicy)||!Same(p.protection_policy,kProtectionPolicy)||!Same(p.component_adapter_policy,kComponentPolicy)||!Same(p.diagnostic_policy,kDiagnosticPolicy)||!Same(p.metric_policy,kMetricPolicy))return false;
 auto pm=ProfileMaterial(p);Clear cp(pm.data(),pm.size(),IntervalScrubClassV3::profile_material,ctl);
 auto em=EqualityMaterial(p);Clear ce(em.data(),em.size(),IntervalScrubClassV3::equality_material,ctl);
 std::array<byte,32>pd{},ed{};Clear cpd(pd.data(),32,IntervalScrubClassV3::profile_digest,ctl),ced(ed.data(),32,IntervalScrubClassV3::equality_digest,ctl);
 return pm==p.profile_material&&em==p.equality_material&&Digest(pm,&pd,ctl)&&Digest(em,&ed,ctl)&&pd==p.profile_fingerprint&&ed==p.equality_fingerprint&&pd==kProfileDigest&&ed==kEqualityDigest;
}
bool ValidState(IntervalValueStateV3 s) noexcept{return s==IntervalValueStateV3::value||s==IntervalValueStateV3::sql_null;}
bool CleanNull(const IntervalValueViewV3&v) noexcept{return v.months==0&&v.civil_days==0&&v.fixed_nanoseconds==0;}
void StoreComponent(byte*b,std::int32_t m,std::int32_t d,std::int64_t n) noexcept{StoreLittle32(b,static_cast<u32>(m));StoreLittle32(b+4,static_cast<u32>(d));StoreLittle64(b+8,static_cast<u64>(n));}
bool RangesOverlap(const void*a,std::size_t an,const void*b,std::size_t bn) noexcept{if(!a||!b||!an||!bn)return false;
 auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
 return x<=y?y-x<an:x-y<bn;}
bool OutputOverlapsProfile(const void* output, std::size_t extent,
                           const IntervalValidatedProfileHandleV3& profile) noexcept {
  return RangesOverlap(output, extent, &profile, sizeof(profile)) ||
      RangesOverlap(output, extent, profile.profile_material.data(),
                    profile.profile_material.size()) ||
      RangesOverlap(output, extent, profile.equality_material.data(),
                    profile.equality_material.size()) ||
      RangesOverlap(output, extent, profile.profile_fingerprint.data(),
                    profile.profile_fingerprint.size()) ||
      RangesOverlap(output, extent, profile.equality_fingerprint.data(),
                    profile.equality_fingerprint.size());
}

struct Parsed { bool ok=false; std::string_view code,detail; std::int64_t value=0; const char* end=nullptr; };
Parsed ParseSigned(const char*b,const char*e,std::int64_t min,std::int64_t max,std::string_view field) noexcept {
 if(b==e)return{false,"CTI.TEMPORAL.INVALID_LITERAL",field,0,b};
 bool neg=*b=='-';
 if(*b=='+')return{false,"CTI.TEMPORAL.INVALID_LITERAL",field,0,b};
 if(neg&&++b==e)return{false,"CTI.TEMPORAL.INVALID_LITERAL",field,0,b};
 if(*b<'0'||*b>'9')return{false,"CTI.TEMPORAL.INVALID_LITERAL",field,0,b};
 if(*b=='0'&&(neg||(b+1<e&&b[1]>='0'&&b[1]<='9')))return{false,"CTI.TEMPORAL.INVALID_LITERAL",field,0,b};
 const char*p=b;
 std::uint64_t mag=0;
 const std::uint64_t limit=neg?static_cast<std::uint64_t>(-(min+1))+1:static_cast<std::uint64_t>(max);while(p<e&&*p>='0'&&*p<='9'){const auto digit=static_cast<std::uint64_t>(*p-'0');
 if(mag>limit/10||(mag==limit/10&&digit>limit%10))return{false,"CTI.TEMPORAL.RANGE_EXCEEDED",field,0,p};
 mag=mag*10+digit;++p;}std::int64_t value=0;
 if(neg)value=mag==std::uint64_t{1}<<63?std::numeric_limits<std::int64_t>::min():-static_cast<std::int64_t>(mag);else value=static_cast<std::int64_t>(mag);return{true,{},{},value,p};
}
template<class I> char* AppendInt(char*p,char*e,I value) noexcept{auto r=std::to_chars(p,e,value);
 return r.ec==std::errc{}?r.ptr:nullptr;}

bool EngineUuidEqual(const scratchbird::engine::Uuid&a,const platform::Uuid&b) noexcept{return std::equal(std::begin(a.bytes),std::end(a.bytes),b.bytes.begin());}
bool EngineUuidNil(const scratchbird::engine::Uuid&a) noexcept{return std::all_of(std::begin(a.bytes),std::end(a.bytes),[](byte b){return b==0;});}
bool DescriptorBinds(const scratchbird::engine::ExecutionTypeDescriptor&d,const DatatypeTypeCodecIdentityRowV3&i,CanonicalTypeId t) noexcept {
 const auto*c=Current(t);
 if(!c||!SameIdentity(i,*c)||!EngineUuidEqual(d.descriptor_uuid,i.legacy_fields.descriptor_uuid)||d.descriptor_epoch!=i.legacy_fields.descriptor_generation||d.canonical_type_id!=static_cast<u32>(t)||!d.descriptor_authoritative||!d.parser_independent||!EngineUuidNil(d.domain_uuid)||!d.domain_stack.empty()||!EngineUuidNil(d.charset_uuid)||!EngineUuidNil(d.collation_uuid)||!EngineUuidNil(d.timezone_uuid)||!EngineUuidNil(d.element_descriptor_uuid)||!EngineUuidNil(d.security_policy_uuid)||d.vector_dimensions!=0||d.container_rank!=0)return false;
 using F=scratchbird::engine::ExecutionTypeFamily;
 using W=scratchbird::engine::ExecutionTypeWidthClass;
 if(t==CanonicalTypeId::interval)return d.family==F::temporal&&d.width_class==W::fixed&&d.bit_width==128&&d.precision==0&&d.scale==0&&d.length==0&&d.modifier_flags==0;
 const u64 length_flag=scratchbird::engine::ExecutionTypeModifierFlagBit(scratchbird::engine::ExecutionTypeModifierFlag::length);
 return t==CanonicalTypeId::character&&d.family==F::character&&d.width_class==W::variable&&d.bit_width==0&&d.precision==0&&d.scale==0&&d.length<=16'777'216&&d.modifier_flags==(d.length==0?0:length_flag);
}
bool ExactPeer(const DatatypeTypeCodecIdentityRowV3*p,CanonicalTypeId t) noexcept{const auto*c=Current(t);
 return p&&c&&SameIdentity(*p,*c);}

template<class R> R FromFailure(const IntervalDiagnosticFactV3&d,Status s) noexcept{R r;
 r.status=s;
 r.diagnostic=d;
 return r;}
IntervalValueResultV3 Publish(std::shared_ptr<const IntervalValidatedProfileHandleV3>p,IntervalValueStateV3 s,std::int32_t m,std::int32_t d,std::int64_t n,const IntervalExecutionControlV3&c) noexcept {if(Cancelled(c))return Failure<IntervalValueResultV3>("PROCESS.CANCELLED","before_publication");
 auto r=Success<IntervalValueResultV3>();
 r.value={std::move(p),s,m,d,n};
 return r;}

} // namespace

IntervalValueViewV3 IntervalOwnedValueV3::view() const noexcept{return{profile.get(),state,months,civil_days,fixed_nanoseconds};}

IntervalProfileResultV3 BuildCurrentIntervalValidatedProfileHandleV3(const platform::Uuid&statement) noexcept {
 const auto*i=Current(CanonicalTypeId::interval);
 if(!i)return Failure<IntervalProfileResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","current_interval_identity_missing");
 return BuildIntervalValidatedProfileHandleV3({statement,kSnapshot,10,10},*i);
}
IntervalProfileResultV3 BuildIntervalValidatedProfileHandleV3(const IntervalAuthorityReceiptV3&r,const DatatypeTypeCodecIdentityRowV3&i) noexcept {
 if(!ExactReceipt(r)||!IsExactCanonicalIntervalTypeCodecIdentityV3(i))return Failure<IntervalProfileResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","receipt_or_identity_invalid");
 try{auto out=Success<IntervalProfileResultV3>();auto&p=out.profile;p.receipt=r;p.identity=i;p.render_policy=kRenderPolicy;p.cast_policy=kCastPolicy;p.subtype_policy=kSubtypePolicy;p.precision_policy=kPrecisionPolicy;p.normalization_policy=kNormalizationPolicy;p.temporal_application_boundary_policy=kTemporalBoundaryPolicy;p.storage_epoch_policy=kStorageEpochPolicy;p.index_policy=kIndexPolicy;p.statistics_policy=kStatisticsPolicy;p.backup_transport_policy=kBackupPolicy;p.protection_policy=kProtectionPolicy;p.component_adapter_policy=kComponentPolicy;p.diagnostic_policy=kDiagnosticPolicy;p.metric_policy=kMetricPolicy;p.profile_material=ProfileMaterial(p);p.equality_material=EqualityMaterial(p);
 if(!Digest(p.profile_material,&p.profile_fingerprint)||!Digest(p.equality_material,&p.equality_fingerprint)||!ProfileValid(p))return Failure<IntervalProfileResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","profile_material_invalid");
 return out;}
 catch(...){return Failure<IntervalProfileResultV3>("RESOURCE.BUDGET_EXCEEDED","profile_allocation",ResourceStatus());}
}
IntervalValidationResultV3 ValidateIntervalProfileHandleV3(const IntervalValidatedProfileHandleV3&p,const IntervalExecutionControlV3&c) noexcept{if(!ProfileValid(p,&c))return Failure<IntervalValidationResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","profile_invalid");
 if(Cancelled(c))return Failure<IntervalValidationResultV3>("PROCESS.CANCELLED","before_publication");
 return Success<IntervalValidationResultV3>();}

IntervalViewResultV3 ValidateIntervalValueViewV3(const IntervalValueViewV3&v,bool null_allowed) noexcept {
 if(!v.profile||!ProfileValid(*v.profile))return Failure<IntervalViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","profile_missing_or_invalid");
 if(!ValidState(v.state))return Failure<IntervalViewResultV3>("DATATYPE.NULL_STATE.INVALID","state_invalid");
 if(v.state==IntervalValueStateV3::sql_null){if(!CleanNull(v))return Failure<IntervalViewResultV3>("DATATYPE.NULL_STATE.INVALID","dirty_null");
 if(!null_allowed)return Failure<IntervalViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","slot_nonnullable");}auto r=Success<IntervalViewResultV3>();
 r.value=v;
 return r;
}
IntervalViewResultV3 AdmitIntervalOperandV3(const IntervalOperandV3&o,bool null_allowed) noexcept {
 if(!o.profile||!ProfileValid(*o.profile))return Failure<IntervalViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","operand_profile_invalid");
 if(!ValidState(o.state))return Failure<IntervalViewResultV3>("DATATYPE.NULL_STATE.INVALID","operand_state_invalid");
 if(o.state==IntervalValueStateV3::sql_null){if(o.months_carrier!=IntervalI32CarrierKindV3::signed_i32||o.civil_days_carrier!=IntervalI32CarrierKindV3::signed_i32||o.fixed_nanoseconds_carrier!=IntervalI64CarrierKindV3::signed_i64||o.months||o.civil_days||o.fixed_nanoseconds)return Failure<IntervalViewResultV3>("DATATYPE.NULL_STATE.INVALID","operand_dirty_null");
 if(!null_allowed)return Failure<IntervalViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","operand_nonnullable");
 auto r=Success<IntervalViewResultV3>();
 r.value={o.profile.get(),o.state,0,0,0};
 return r;}if(o.months_carrier==IntervalI32CarrierKindV3::below_i32||o.months_carrier==IntervalI32CarrierKindV3::above_i32)return Failure<IntervalViewResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","months");
 if(o.civil_days_carrier==IntervalI32CarrierKindV3::below_i32||o.civil_days_carrier==IntervalI32CarrierKindV3::above_i32)return Failure<IntervalViewResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_days");
 if(o.fixed_nanoseconds_carrier==IntervalI64CarrierKindV3::below_i64||o.fixed_nanoseconds_carrier==IntervalI64CarrierKindV3::above_i64)return Failure<IntervalViewResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","fixed_nanoseconds");
 if(o.months_carrier!=IntervalI32CarrierKindV3::signed_i32||o.civil_days_carrier!=IntervalI32CarrierKindV3::signed_i32||o.fixed_nanoseconds_carrier!=IntervalI64CarrierKindV3::signed_i64)return Failure<IntervalViewResultV3>("SBLR.OPERAND_INVALID","operand_carrier_invalid");
 if(o.months<std::numeric_limits<std::int32_t>::min()||o.months>std::numeric_limits<std::int32_t>::max())return Failure<IntervalViewResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","months");
 if(o.civil_days<std::numeric_limits<std::int32_t>::min()||o.civil_days>std::numeric_limits<std::int32_t>::max())return Failure<IntervalViewResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_days");
 auto r=Success<IntervalViewResultV3>();
 r.value={o.profile.get(),o.state,static_cast<std::int32_t>(o.months),static_cast<std::int32_t>(o.civil_days),o.fixed_nanoseconds};
 return r;
}

IntervalViewResultV3 DecodeCanonicalIntervalComponentNoAllocV3(const IntervalValidatedProfileHandleV3&p,IntervalValueStateV3 s,bool null_allowed,std::span<const byte>b,const IntervalExecutionControlV3&c) noexcept {
 if(!ProfileValid(p,&c))return Failure<IntervalViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","component_profile_invalid");
 if(!ValidState(s))return Failure<IntervalViewResultV3>("DATATYPE.NULL_STATE.INVALID","component_state_invalid");
 if(s==IntervalValueStateV3::sql_null){if(!b.empty())return Failure<IntervalViewResultV3>("DATATYPE.NULL_STATE.INVALID","sql_null_component_nonempty");
 if(!null_allowed)return Failure<IntervalViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","slot_nonnullable");
 if(Cancelled(c))return Failure<IntervalViewResultV3>("PROCESS.CANCELLED","before_publication");
 auto r=Success<IntervalViewResultV3>();
 r.value={&p,s,0,0,0};
 return r;}if(b.size()!=16)return Failure<IntervalViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_extent_invalid");
 const auto m=static_cast<std::int32_t>(LoadLittle32(b.data()));
 const auto d=static_cast<std::int32_t>(LoadLittle32(b.data()+4));
 const auto n=static_cast<std::int64_t>(LoadLittle64(b.data()+8));
 std::array<byte,16>x{};Clear cx(x.data(),16,IntervalScrubClassV3::component_decode_reencode,&c);StoreComponent(x.data(),m,d,n);
 if(c.force_reencode_mismatch_for_conformance)x[0]^=1;
 if(!std::equal(x.begin(),x.end(),b.begin()))return Failure<IntervalViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_reencode");
 if(Cancelled(c))return Failure<IntervalViewResultV3>("PROCESS.CANCELLED","before_publication");
 auto r=Success<IntervalViewResultV3>();
 r.value={&p,s,m,d,n};
 return r;
}
IntervalNoAllocWriteResultV3 EncodeCanonicalIntervalComponentIntoNoAllocV3(const IntervalOwnedValueV3&o,byte*out,u64 cap,const IntervalExecutionControlV3&c) noexcept {
 auto v=ValidateIntervalValueViewV3(o.view(),true);
 if(!v.ok())return FromFailure<IntervalNoAllocWriteResultV3>(v.diagnostic,v.status);
 auto r=Success<IntervalNoAllocWriteResultV3>();
 r.containing_null=o.state==IntervalValueStateV3::sql_null;
 r.bytes_required=r.containing_null?0:16;
 if(out&&(RangesOverlap(out,r.bytes_required,&o,sizeof(o))||RangesOverlap(out,r.bytes_required,&c,sizeof(c))||OutputOverlapsProfile(out,r.bytes_required,*o.profile)))return Failure<IntervalNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_output_alias");
 if(r.bytes_required>cap||r.bytes_required>c.maximum_allocation_bytes||(r.bytes_required&&!out)){auto f=Failure<IntervalNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","component_capacity",ResourceStatus());f.bytes_required=r.bytes_required;
 return f;}std::array<byte,16>x{};Clear cx(x.data(),16,IntervalScrubClassV3::component_encode_staging,&c);
 if(r.bytes_required)StoreComponent(x.data(),o.months,o.civil_days,o.fixed_nanoseconds);
 if(Cancelled(c))return Failure<IntervalNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
 if(r.bytes_required)std::memcpy(out,x.data(),16);
 r.bytes_written=r.bytes_required;
 return r;
}
IntervalBytesResultV3 EncodeCanonicalIntervalComponentV3(const IntervalOwnedValueV3&o,const IntervalExecutionControlV3&c) noexcept {auto v=ValidateIntervalValueViewV3(o.view(),true);
 if(!v.ok())return FromFailure<IntervalBytesResultV3>(v.diagnostic,v.status);
 u64 n=o.state==IntervalValueStateV3::value?16:0;
 if(n>c.maximum_allocation_bytes)return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","component_capacity",ResourceStatus());
 if(Cancelled(c))return Failure<IntervalBytesResultV3>("PROCESS.CANCELLED","before_allocation");
 auto r=Success<IntervalBytesResultV3>();
 VectorClear<byte>cl(&r.bytes,IntervalScrubClassV3::component_owned_buffer,c);
 try{r.bytes.resize(n);}
 catch(...){return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","component_allocation",ResourceStatus());}auto w=EncodeCanonicalIntervalComponentIntoNoAllocV3(o,r.bytes.data(),r.bytes.size(),c);
 if(!w.ok())return FromFailure<IntervalBytesResultV3>(w.diagnostic,w.status);
 cl.Disarm();
 return r;}

IntervalValueResultV3 ConstructIntervalV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&p,std::int64_t m,std::int64_t d,std::int64_t n,bool null_allowed,const IntervalExecutionControlV3&c) noexcept{(void)null_allowed;
 if(!p||!ProfileValid(*p,&c))return Failure<IntervalValueResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","construct_profile_invalid");
 if(m<std::numeric_limits<std::int32_t>::min()||m>std::numeric_limits<std::int32_t>::max())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","months");
 if(d<std::numeric_limits<std::int32_t>::min()||d>std::numeric_limits<std::int32_t>::max())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_days");
 return Publish(p,IntervalValueStateV3::value,static_cast<std::int32_t>(m),static_cast<std::int32_t>(d),n,c);}
IntervalValueResultV3 ConstructIntervalV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&p,const IntervalNullableI32FactV3&m,const IntervalNullableI32FactV3&d,const IntervalNullableI64FactV3&n,bool null_allowed,const IntervalExecutionControlV3&c) noexcept {
 if(!p||!ProfileValid(*p,&c))return Failure<IntervalValueResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","construct_profile_invalid");
 if(!ValidState(m.state)||!ValidState(d.state)||!ValidState(n.state))return Failure<IntervalValueResultV3>("DATATYPE.NULL_STATE.INVALID","construct_state_invalid");
 if(m.carrier==IntervalI32CarrierKindV3::below_i32||m.carrier==IntervalI32CarrierKindV3::above_i32)return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","months");
 if(d.carrier==IntervalI32CarrierKindV3::below_i32||d.carrier==IntervalI32CarrierKindV3::above_i32)return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_days");
 if(n.carrier==IntervalI64CarrierKindV3::below_i64||n.carrier==IntervalI64CarrierKindV3::above_i64)return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","fixed_nanoseconds");
 if(m.carrier!=IntervalI32CarrierKindV3::signed_i32||d.carrier!=IntervalI32CarrierKindV3::signed_i32||n.carrier!=IntervalI64CarrierKindV3::signed_i64)return Failure<IntervalValueResultV3>("SBLR.OPERAND_INVALID","construct_carrier_invalid");
 if((m.state==IntervalValueStateV3::sql_null&&m.value)||(d.state==IntervalValueStateV3::sql_null&&d.value)||(n.state==IntervalValueStateV3::sql_null&&n.value))return Failure<IntervalValueResultV3>("DATATYPE.NULL_STATE.INVALID","construct_dirty_null");
 if(m.state==IntervalValueStateV3::sql_null||d.state==IntervalValueStateV3::sql_null||n.state==IntervalValueStateV3::sql_null){if(!null_allowed)return Failure<IntervalValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","construct_nonnullable");
 return Publish(p,IntervalValueStateV3::sql_null,0,0,0,c);}
 return ConstructIntervalV3(p,m.value,d.value,n.value,null_allowed,c);
}
IntervalComponentResultV3 DecomposeIntervalV3(const IntervalValueViewV3&v,bool a,const IntervalExecutionControlV3&c) noexcept{auto q=ValidateIntervalValueViewV3(v,a);
 if(!q.ok())return FromFailure<IntervalComponentResultV3>(q.diagnostic,q.status);
 if(Cancelled(c))return Failure<IntervalComponentResultV3>("PROCESS.CANCELLED","before_publication");
 auto r=Success<IntervalComponentResultV3>();
 r.is_null=v.state==IntervalValueStateV3::sql_null;
 if(!r.is_null)r.component={v.months,v.civil_days,v.fixed_nanoseconds};
 return r;}

IntervalValueResultV3 ParseCanonicalIntervalV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&p,std::string_view t,bool null_allowed,const IntervalExecutionControlV3&c) noexcept {
 (void)null_allowed;
 if(!p||!ProfileValid(*p,&c))return Failure<IntervalValueResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","parse_profile_invalid");
 if(t.size()>63)return Failure<IntervalValueResultV3>("CTB.TEXT.LENGTH_EXCEEDED","input_extent");
 constexpr std::string_view pre="SBINTERVAL1:M=";
 if(!t.starts_with(pre))return Failure<IntervalValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","prefix");
 const char*b=t.data()+pre.size(),*e=t.data()+t.size();
 auto m=ParseSigned(b,e,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),"months_syntax");
 if(!m.ok)return Failure<IntervalValueResultV3>(m.code,m.code=="CTI.TEMPORAL.RANGE_EXCEEDED"?"months_range":"months_syntax");
 if(e-m.end<3||std::memcmp(m.end,";D=",3))return Failure<IntervalValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","separator_D");
 auto d=ParseSigned(m.end+3,e,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),"days_syntax");
 if(!d.ok)return Failure<IntervalValueResultV3>(d.code,d.code=="CTI.TEMPORAL.RANGE_EXCEEDED"?"days_range":"days_syntax");
 if(e-d.end<4||std::memcmp(d.end,";NS=",4))return Failure<IntervalValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","separator_NS");
 auto n=ParseSigned(d.end+4,e,std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max(),"nanoseconds_syntax");
 if(!n.ok)return Failure<IntervalValueResultV3>(n.code,n.code=="CTI.TEMPORAL.RANGE_EXCEEDED"?"nanoseconds_range":"nanoseconds_syntax");
 if(n.end!=e)return Failure<IntervalValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","trailing_bytes");
 return ConstructIntervalV3(p,m.value,d.value,n.value,true,c);
}
IntervalValueResultV3 ParseCanonicalIntervalOperandV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&p,const IntervalTextOperandV3&o,bool null_allowed,const IntervalExecutionControlV3&c) noexcept {
 if(!p||!ProfileValid(*p,&c))return Failure<IntervalValueResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","parse_profile_invalid");
 if(!ExactPeer(o.identity,CanonicalTypeId::character)||!o.descriptor||!DescriptorBinds(*o.descriptor,*o.identity,CanonicalTypeId::character))return Failure<IntervalValueResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","text_operand_authority_invalid");
 if(!ValidState(o.state))return Failure<IntervalValueResultV3>("DATATYPE.NULL_STATE.INVALID","text_operand_state_invalid");
 if(o.state==IntervalValueStateV3::sql_null){if(o.carrier!=IntervalTextCarrierKindV3::utf8_bytes||o.extent||!o.bytes.empty())return Failure<IntervalValueResultV3>("DATATYPE.NULL_STATE.INVALID","text_operand_dirty_null");
 if(!null_allowed||!o.descriptor->nullable_allowed)return Failure<IntervalValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","parse_nonnullable");
 return Publish(p,IntervalValueStateV3::sql_null,0,0,0,c);}if(o.carrier!=IntervalTextCarrierKindV3::utf8_bytes)return Failure<IntervalValueResultV3>("SBLR.OPERAND_INVALID","text_operand_carrier");
 if(o.extent!=o.bytes.size())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","text_operand_extent");
 if(o.descriptor->length&&o.extent>o.descriptor->length)return Failure<IntervalValueResultV3>("CTB.TEXT.LENGTH_EXCEEDED","text_operand_extent_limit");
 return ParseCanonicalIntervalV3(p,o.bytes,null_allowed,c);
}
IntervalNoAllocWriteResultV3 RenderCanonicalIntervalIntoNoAllocV3(const IntervalOwnedValueV3&o,char*out,u64 cap,const IntervalExecutionControlV3&c) noexcept {
 auto v=ValidateIntervalValueViewV3(o.view(),true);
 if(!v.ok())return FromFailure<IntervalNoAllocWriteResultV3>(v.diagnostic,v.status);
 std::array<char,64>x{};Clear cx(x.data(),x.size(),IntervalScrubClassV3::render_staging,&c);char*p=x.data(),*e=x.data()+x.size();
 if(o.state==IntervalValueStateV3::value){constexpr char a[]="SBINTERVAL1:M=";
 std::memcpy(p,a,sizeof(a)-1);p+=sizeof(a)-1;
 if(!(p=AppendInt(p,e,o.months)))return Failure<IntervalNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","render_internal");
 std::memcpy(p,";D=",3);p+=3;
 if(!(p=AppendInt(p,e,o.civil_days)))return Failure<IntervalNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","render_internal");
 std::memcpy(p,";NS=",4);p+=4;
 if(!(p=AppendInt(p,e,o.fixed_nanoseconds)))return Failure<IntervalNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","render_internal");}u64 need=static_cast<u64>(p-x.data());
 if(out&&(RangesOverlap(out,need,&o,sizeof(o))||RangesOverlap(out,need,&c,sizeof(c))||OutputOverlapsProfile(out,need,*o.profile)))return Failure<IntervalNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","render_output_alias");
 if(need>cap||(need&&!out)){auto f=Failure<IntervalNoAllocWriteResultV3>("CTB.TEXT.LENGTH_EXCEEDED","render_capacity");f.bytes_required=need;
 return f;}if(need>c.maximum_allocation_bytes){auto f=Failure<IntervalNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","render_resource",ResourceStatus());f.bytes_required=need;
 return f;}if(Cancelled(c))return Failure<IntervalNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
 if(need)std::memcpy(out,x.data(),need);
 auto r=Success<IntervalNoAllocWriteResultV3>();
 r.containing_null=o.state==IntervalValueStateV3::sql_null;
 r.bytes_required=need;
 r.bytes_written=need;
 return r;
}
IntervalTextResultV3 RenderCanonicalIntervalV3(const IntervalOwnedValueV3&o,const IntervalExecutionControlV3&c) noexcept {auto v=ValidateIntervalValueViewV3(o.view(),true);
 if(!v.ok())return FromFailure<IntervalTextResultV3>(v.diagnostic,v.status);
 std::array<char,63>x{};Clear cx(x.data(),x.size(),IntervalScrubClassV3::render_owned_buffer,&c);
 auto w=RenderCanonicalIntervalIntoNoAllocV3(o,x.data(),x.size(),c);
 if(!w.ok())return FromFailure<IntervalTextResultV3>(w.diagnostic,w.status);
 auto r=Success<IntervalTextResultV3>();
 r.containing_null=w.containing_null;
 try{r.text.assign(x.data(),x.data()+w.bytes_written);}
 catch(...){return Failure<IntervalTextResultV3>("RESOURCE.BUDGET_EXCEEDED","render_allocation",ResourceStatus());}if(Cancelled(c)){if(!r.text.empty())SecureClear(r.text.data(),r.text.size());
 return Failure<IntervalTextResultV3>("PROCESS.CANCELLED","before_publication");}
 return r;}

IntervalValueResultV3 CanonicalizeIntervalIdentityV3(const IntervalOwnedValueV3&o,bool a,const IntervalExecutionControlV3&c) noexcept{auto v=ValidateIntervalValueViewV3(o.view(),a);
 if(!v.ok())return FromFailure<IntervalValueResultV3>(v.diagnostic,v.status);
 return Publish(o.profile,o.state,o.months,o.civil_days,o.fixed_nanoseconds,c);}
IntervalValueResultV3 NegateIntervalCheckedV3(const IntervalOwnedValueV3&o,bool a,const IntervalExecutionControlV3&c) noexcept{auto v=ValidateIntervalValueViewV3(o.view(),a);
 if(!v.ok())return FromFailure<IntervalValueResultV3>(v.diagnostic,v.status);
 if(o.state==IntervalValueStateV3::sql_null)return Publish(o.profile,o.state,0,0,0,c);
 if(o.months==std::numeric_limits<std::int32_t>::min())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","months");
 if(o.civil_days==std::numeric_limits<std::int32_t>::min())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_days");
 if(o.fixed_nanoseconds==std::numeric_limits<std::int64_t>::min())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","fixed_nanoseconds");
 return Publish(o.profile,o.state,-o.months,-o.civil_days,-o.fixed_nanoseconds,c);}
template<bool Sub> IntervalValueResultV3 Binary(const IntervalOwnedValueV3&a,const IntervalOwnedValueV3&b,bool na,const IntervalExecutionControlV3&c) noexcept {auto l=ValidateIntervalValueViewV3(a.view(),na);
 if(!l.ok())return FromFailure<IntervalValueResultV3>(l.diagnostic,l.status);
 auto r=ValidateIntervalValueViewV3(b.view(),na);
 if(!r.ok())return FromFailure<IntervalValueResultV3>(r.diagnostic,r.status);
 if(a.profile->equality_fingerprint!=b.profile->equality_fingerprint)return Failure<IntervalValueResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","equality_cohort_mismatch");
 if(a.state==IntervalValueStateV3::sql_null||b.state==IntervalValueStateV3::sql_null)return Publish(a.profile,IntervalValueStateV3::sql_null,0,0,0,c);
 std::int64_t m=Sub?std::int64_t(a.months)-b.months:std::int64_t(a.months)+b.months,d=Sub?std::int64_t(a.civil_days)-b.civil_days:std::int64_t(a.civil_days)+b.civil_days;__int128 n=Sub?__int128(a.fixed_nanoseconds)-b.fixed_nanoseconds:__int128(a.fixed_nanoseconds)+b.fixed_nanoseconds;
 if(m<std::numeric_limits<std::int32_t>::min()||m>std::numeric_limits<std::int32_t>::max())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","months");
 if(d<std::numeric_limits<std::int32_t>::min()||d>std::numeric_limits<std::int32_t>::max())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_days");
 if(n<std::numeric_limits<std::int64_t>::min()||n>std::numeric_limits<std::int64_t>::max())return Failure<IntervalValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","fixed_nanoseconds");
 return Publish(a.profile,IntervalValueStateV3::value,static_cast<std::int32_t>(m),static_cast<std::int32_t>(d),static_cast<std::int64_t>(n),c);}
IntervalValueResultV3 AddIntervalCheckedV3(const IntervalOwnedValueV3&a,const IntervalOwnedValueV3&b,bool n,const IntervalExecutionControlV3&c) noexcept{return Binary<false>(a,b,n,c);}IntervalValueResultV3 SubtractIntervalCheckedV3(const IntervalOwnedValueV3&a,const IntervalOwnedValueV3&b,bool n,const IntervalExecutionControlV3&c) noexcept{return Binary<true>(a,b,n,c);}

template<IntervalScalarKindV3 K> IntervalScalarResultV3 Extract(const IntervalValueViewV3&v,bool a,const IntervalExecutionControlV3&c) noexcept{auto q=ValidateIntervalValueViewV3(v,a);
 if(!q.ok())return FromFailure<IntervalScalarResultV3>(q.diagnostic,q.status);
 if(Cancelled(c))return Failure<IntervalScalarResultV3>("PROCESS.CANCELLED","before_publication");
 auto r=Success<IntervalScalarResultV3>();
 r.kind=K;
 r.is_null=v.state==IntervalValueStateV3::sql_null;
 if(!r.is_null){if constexpr(K==IntervalScalarKindV3::months_i32)r.signed_i32_value=v.months;else if constexpr(K==IntervalScalarKindV3::civil_days_i32)r.signed_i32_value=v.civil_days;else r.signed_i64_value=v.fixed_nanoseconds;}
 return r;}
IntervalScalarResultV3 ExtractIntervalMonthsV3(const IntervalValueViewV3&v,bool a,const IntervalExecutionControlV3&c) noexcept{return Extract<IntervalScalarKindV3::months_i32>(v,a,c);}IntervalScalarResultV3 ExtractIntervalCivilDaysV3(const IntervalValueViewV3&v,bool a,const IntervalExecutionControlV3&c) noexcept{return Extract<IntervalScalarKindV3::civil_days_i32>(v,a,c);}IntervalScalarResultV3 ExtractIntervalFixedNanosecondsV3(const IntervalValueViewV3&v,bool a,const IntervalExecutionControlV3&c) noexcept{return Extract<IntervalScalarKindV3::fixed_nanoseconds_i64>(v,a,c);}

IntervalEqualityResultV3 EqualIntervalValuesV3(const IntervalValueViewV3&a,const IntervalValueViewV3&b,const IntervalExecutionControlV3&c) noexcept {auto l=ValidateIntervalValueViewV3(a,true);
 if(!l.ok())return FromFailure<IntervalEqualityResultV3>(l.diagnostic,l.status);
 auto r=ValidateIntervalValueViewV3(b,true);
 if(!r.ok())return FromFailure<IntervalEqualityResultV3>(r.diagnostic,r.status);
 if(a.profile->equality_fingerprint!=b.profile->equality_fingerprint)return Failure<IntervalEqualityResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","equality_cohort_mismatch");
 if(Cancelled(c))return Failure<IntervalEqualityResultV3>("PROCESS.CANCELLED","before_publication");
 auto x=Success<IntervalEqualityResultV3>();
 if(a.state==IntervalValueStateV3::sql_null||b.state==IntervalValueStateV3::sql_null){x.fact=IntervalEqualityFactV3::unordered_null;x.grouping_equivalent=x.null_equivalent=a.state==b.state;}else{x.fact=(a.months==b.months&&a.civil_days==b.civil_days&&a.fixed_nanoseconds==b.fixed_nanoseconds)?IntervalEqualityFactV3::equal:IntervalEqualityFactV3::not_equal;x.grouping_equivalent=x.fact==IntervalEqualityFactV3::equal;}
 return x;}
IntervalEqualityResultV3 DistinctIntervalValuesV3(const IntervalValueViewV3&a,const IntervalValueViewV3&b,const IntervalExecutionControlV3&c) noexcept{auto r=EqualIntervalValuesV3(a,b,c);
 if(!r.ok())return r;
 if(r.fact==IntervalEqualityFactV3::equal)r.fact=IntervalEqualityFactV3::not_equal;else if(r.fact==IntervalEqualityFactV3::not_equal)r.fact=IntervalEqualityFactV3::equal;
 return r;}

IntervalNoAllocWriteResultV3 HashIntervalValueIntoNoAllocV3(const IntervalOwnedValueV3&o,byte*out,u64 cap,const IntervalExecutionControlV3&c) noexcept {auto v=ValidateIntervalValueViewV3(o.view(),true);
 if(!v.ok())return FromFailure<IntervalNoAllocWriteResultV3>(v.diagnostic,v.status);
 if(out&&(RangesOverlap(out,32,&o,sizeof(o))||RangesOverlap(out,32,&c,sizeof(c))||OutputOverlapsProfile(out,32,*o.profile)))return Failure<IntervalNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","hash_output_alias");
 if(!out||cap<32||c.maximum_allocation_bytes<32){auto f=Failure<IntervalNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_capacity",ResourceStatus());f.bytes_required=32;
 return f;}std::array<byte,133>p{};Clear cp(p.data(),p.size(),IntervalScrubClassV3::hash_preimage,&c);
 std::memcpy(p.data(),"SBINTH01",8);PutUuid(p.data()+8,o.profile->receipt.catalog_snapshot_uuid);StoreLittle64(p.data()+24,o.profile->receipt.catalog_generation);StoreLittle64(p.data()+32,o.profile->receipt.registry_generation);PutUuid(p.data()+40,o.profile->identity.legacy_fields.descriptor_uuid);StoreLittle64(p.data()+56,o.profile->identity.legacy_fields.descriptor_generation);PutUuid(p.data()+64,o.profile->identity.legacy_fields.type_uuid);StoreLittle64(p.data()+80,o.profile->identity.legacy_fields.type_generation);PutPolicy(p.data()+88,o.profile->identity.hash_policy);p[112]=o.state==IntervalValueStateV3::value?1:0;StoreLittle32(p.data()+113,o.state==IntervalValueStateV3::value?16:0);
 std::size_t n=117;
 if(o.state==IntervalValueStateV3::value){StoreComponent(p.data()+117,o.months,o.civil_days,o.fixed_nanoseconds);n=133;}std::array<byte,32>d{};Clear cd(d.data(),32,IntervalScrubClassV3::hash_digest,&c);
 if(!Digest(std::span<const byte>(p.data(),n),&d,&c))return Failure<IntervalNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_provider",ResourceStatus());
 if(Cancelled(c))return Failure<IntervalNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
 std::memcpy(out,d.data(),32);
 auto r=Success<IntervalNoAllocWriteResultV3>();
 r.bytes_required=r.bytes_written=32;
 return r;}
IntervalBytesResultV3 HashIntervalValueV3(const IntervalOwnedValueV3&o,const IntervalExecutionControlV3&c) noexcept{if(c.maximum_allocation_bytes<32)return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_capacity",ResourceStatus());
 std::array<byte,32>x{};Clear cx(x.data(),32,IntervalScrubClassV3::hash_owned_buffer,&c);
 auto w=HashIntervalValueIntoNoAllocV3(o,x.data(),32,c);
 if(!w.ok())return FromFailure<IntervalBytesResultV3>(w.diagnostic,w.status);
 auto r=Success<IntervalBytesResultV3>();
 VectorClear<byte>cl(&r.bytes,IntervalScrubClassV3::hash_owned_buffer,c);
 try{r.bytes.assign(x.begin(),x.end());}
 catch(...){return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_allocation",ResourceStatus());}cl.Disarm();
 return r;}

IntervalValidationResultV3 ValidateIntervalBatchViewV3(const IntervalBatchViewV3&b) noexcept {
 if(!b.profile||!ProfileValid(*b.profile))return Failure<IntervalValidationResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","batch_profile_invalid");
 const auto n=b.months.size();
 if(b.civil_days.size()!=n||b.fixed_nanoseconds.size()!=n)return Failure<IntervalValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","batch_component_extents");
 if(b.null_bitmap_lsb0.size()!=(n+7)/8)return Failure<IntervalValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","batch_bitmap_extent");
 if(n%8&&n){byte mask=static_cast<byte>(0xffu<<(n%8));
 if((b.null_bitmap_lsb0.back()&mask)!=0)return Failure<IntervalValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","batch_bitmap_unused_bits");}
 for(std::size_t i=0;i<n;++i)if((b.null_bitmap_lsb0[i/8]&(byte(1u<<(i%8))))&&(b.months[i]||b.civil_days[i]||b.fixed_nanoseconds[i]))return Failure<IntervalValidationResultV3>("DATATYPE.NULL_STATE.INVALID","batch_dirty_null");
 return Success<IntervalValidationResultV3>();
}
IntervalBatchExtentsResultV3 ComputeIntervalBatchExtentsV3(u64 n,u64 limit) noexcept {
 auto r=Success<IntervalBatchExtentsResultV3>();
 if(n>std::numeric_limits<u64>::max()/16)return Failure<IntervalBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_extent_overflow",ResourceStatus());
 r.months_bytes=n*4;
 r.civil_days_bytes=n*4;
 r.fixed_nanoseconds_bytes=n*8;
 r.bitmap_bytes=n/8+(n%8!=0);
 if(r.months_bytes>std::numeric_limits<u64>::max()-r.civil_days_bytes||r.months_bytes+r.civil_days_bytes>std::numeric_limits<u64>::max()-r.fixed_nanoseconds_bytes||r.months_bytes+r.civil_days_bytes+r.fixed_nanoseconds_bytes>std::numeric_limits<u64>::max()-r.bitmap_bytes)return Failure<IntervalBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_extent_overflow",ResourceStatus());
 r.combined_bytes=r.months_bytes+r.civil_days_bytes+r.fixed_nanoseconds_bytes+r.bitmap_bytes;
 if(r.combined_bytes>limit){auto f=Failure<IntervalBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_size_limit",ResourceStatus());f.months_bytes=r.months_bytes;f.civil_days_bytes=r.civil_days_bytes;f.fixed_nanoseconds_bytes=r.fixed_nanoseconds_bytes;f.bitmap_bytes=r.bitmap_bytes;f.combined_bytes=r.combined_bytes;
 return f;}
 return r;
}
IntervalBatchExtentsResultV3 MaterializeIntervalBatchIntoV3(const std::shared_ptr<const IntervalValidatedProfileHandleV3>&profile,std::span<const std::int32_t>m,std::span<const std::int32_t>d,std::span<const std::int64_t>n,std::span<const byte>bitmap,std::int32_t*om,u64 omb,std::int32_t*od,u64 odb,std::int64_t*on,u64 onb,byte*ob,u64 obb,const IntervalExecutionControlV3&c) noexcept {
 auto valid=ValidateIntervalBatchViewV3({profile.get(),m,d,n,bitmap});
 if(!valid.ok())return FromFailure<IntervalBatchExtentsResultV3>(valid.diagnostic,valid.status);
 auto e=ComputeIntervalBatchExtentsV3(m.size(),c.maximum_allocation_bytes);
 if(!e.ok())return e;
 auto overlap=[&](const void*out,u64 sz){return RangesOverlap(out,sz,m.data(),m.size_bytes())||RangesOverlap(out,sz,d.data(),d.size_bytes())||RangesOverlap(out,sz,n.data(),n.size_bytes())||RangesOverlap(out,sz,bitmap.data(),bitmap.size());};
 if(overlap(om,e.months_bytes)||overlap(od,e.civil_days_bytes)||overlap(on,e.fixed_nanoseconds_bytes)||overlap(ob,e.bitmap_bytes)||RangesOverlap(om,e.months_bytes,od,e.civil_days_bytes)||RangesOverlap(om,e.months_bytes,on,e.fixed_nanoseconds_bytes)||RangesOverlap(om,e.months_bytes,ob,e.bitmap_bytes)||RangesOverlap(od,e.civil_days_bytes,on,e.fixed_nanoseconds_bytes)||RangesOverlap(od,e.civil_days_bytes,ob,e.bitmap_bytes)||RangesOverlap(on,e.fixed_nanoseconds_bytes,ob,e.bitmap_bytes))return Failure<IntervalBatchExtentsResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","batch_overlap");
 if((e.months_bytes&&(!om||omb<e.months_bytes||reinterpret_cast<std::uintptr_t>(om)%alignof(std::int32_t)))||(e.civil_days_bytes&&(!od||odb<e.civil_days_bytes||reinterpret_cast<std::uintptr_t>(od)%alignof(std::int32_t)))||(e.fixed_nanoseconds_bytes&&(!on||onb<e.fixed_nanoseconds_bytes||reinterpret_cast<std::uintptr_t>(on)%alignof(std::int64_t)))||(e.bitmap_bytes&&(!ob||obb<e.bitmap_bytes))){auto f=Failure<IntervalBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_destination",ResourceStatus());f=e;f.status=ResourceStatus();f.diagnostic.status=f.status;f.diagnostic.diagnostic_code="RESOURCE.BUDGET_EXCEEDED";f.diagnostic.detail="batch_destination";
 return f;}if(Cancelled(c))return Failure<IntervalBatchExtentsResultV3>("PROCESS.CANCELLED","batch_before_allocation");
 std::vector<std::int32_t>sm,sd;
 std::vector<std::int64_t>sn;
 std::vector<byte>sb;
 VectorClear<std::int32_t>cm(&sm,IntervalScrubClassV3::batch_month_staging,c),cd(&sd,IntervalScrubClassV3::batch_day_staging,c);
 VectorClear<std::int64_t>cn(&sn,IntervalScrubClassV3::batch_nanosecond_staging,c);
 VectorClear<byte>cb(&sb,IntervalScrubClassV3::batch_bitmap_staging,c);
 try{sm.assign(m.begin(),m.end());sd.assign(d.begin(),d.end());sn.assign(n.begin(),n.end());sb.assign(bitmap.begin(),bitmap.end());}
 catch(...){return Failure<IntervalBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_allocation",ResourceStatus());}
 for(std::size_t i=0;i<m.size();i+=4096)if(Cancelled(c))return Failure<IntervalBatchExtentsResultV3>("PROCESS.CANCELLED",i?"batch_row4096":"batch_row0");
 if(Cancelled(c))return Failure<IntervalBatchExtentsResultV3>("PROCESS.CANCELLED","batch_before_publication");
 if(e.months_bytes)std::memcpy(om,sm.data(),e.months_bytes);
 if(e.civil_days_bytes)std::memcpy(od,sd.data(),e.civil_days_bytes);
 if(e.fixed_nanoseconds_bytes)std::memcpy(on,sn.data(),e.fixed_nanoseconds_bytes);
 if(e.bitmap_bytes)std::memcpy(ob,sb.data(),e.bitmap_bytes);
 return e;
}
IntervalBatchResultV3 MaterializeIntervalBatchV3(std::shared_ptr<const IntervalValidatedProfileHandleV3>profile,std::span<const std::int32_t>m,std::span<const std::int32_t>d,std::span<const std::int64_t>n,std::span<const byte>b,const IntervalExecutionControlV3&c) noexcept {
 auto valid=ValidateIntervalBatchViewV3({profile.get(),m,d,n,b});
 if(!valid.ok())return FromFailure<IntervalBatchResultV3>(valid.diagnostic,valid.status);
 auto e=ComputeIntervalBatchExtentsV3(m.size(),c.maximum_allocation_bytes);
 if(!e.ok())return FromFailure<IntervalBatchResultV3>(e.diagnostic,e.status);
 if(Cancelled(c))return Failure<IntervalBatchResultV3>("PROCESS.CANCELLED","batch_before_allocation");
 IntervalOwnedBatchV3 s;s.profile=std::move(profile);
 VectorClear<std::int32_t>cm(&s.months,IntervalScrubClassV3::batch_month_staging,c),cd(&s.civil_days,IntervalScrubClassV3::batch_day_staging,c);
 VectorClear<std::int64_t>cn(&s.fixed_nanoseconds,IntervalScrubClassV3::batch_nanosecond_staging,c);
 VectorClear<byte>cb(&s.null_bitmap_lsb0,IntervalScrubClassV3::batch_bitmap_staging,c);
 try{s.months.assign(m.begin(),m.end());s.civil_days.assign(d.begin(),d.end());s.fixed_nanoseconds.assign(n.begin(),n.end());s.null_bitmap_lsb0.assign(b.begin(),b.end());}
 catch(...){return Failure<IntervalBatchResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_allocation",ResourceStatus());}
 for(std::size_t i=0;i<m.size();i+=4096)if(Cancelled(c))return Failure<IntervalBatchResultV3>("PROCESS.CANCELLED",i?"batch_row4096":"batch_row0");
 if(Cancelled(c))return Failure<IntervalBatchResultV3>("PROCESS.CANCELLED","batch_before_publication");
 auto r=Success<IntervalBatchResultV3>();
 r.batch=std::move(s);cm.Disarm();cd.Disarm();cn.Disarm();cb.Disarm();
 return r;
}

IntervalViewResultV3 DecodeIntervalSbdvalComposedNoAllocV3(const IntervalValidatedProfileHandleV3&p,bool na,std::span<const byte>e,const IntervalExecutionControlV3&c) noexcept {
 if(!ProfileValid(p,&c))return Failure<IntervalViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","sbdval_profile");
 auto f=DecodeDatatypeBinaryStructuralValueViewNoAlloc(e.empty()?nullptr:e.data(),e.size());
 if(!f.ok())return Failure<IntervalViewResultV3>(f.diagnostic.diagnostic_code,"sbdval_structure",f.status);
 if(f.value.type_id!=CanonicalTypeId::interval)return Failure<IntervalViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","sbdval_type");
 if(f.value.payload_is_toast_reference)return Failure<IntervalViewResultV3>("CTB.BINARY.FRAME_INVALID","sbdval_toast");
 auto dc=c;dc.force_reencode_mismatch_for_conformance=false;
 auto v=DecodeCanonicalIntervalComponentNoAllocV3(p,f.value.is_null?IntervalValueStateV3::sql_null:IntervalValueStateV3::value,na,{f.value.payload_data,f.value.payload_bytes},dc);
 if(!v.ok())return v;
 std::array<byte,48>x{};Clear cx(x.data(),x.size(),IntervalScrubClassV3::sbdval_reencode,&c);
 auto w=EncodeDatatypeBinaryStructuralValueIntoNoAlloc(f.value,x.data(),e.size());
 if(c.force_reencode_mismatch_for_conformance)x[0]^=1;
 if(!w.ok()||w.bytes_written!=e.size()||!std::equal(x.begin(),x.begin()+e.size(),e.begin()))return Failure<IntervalViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdval_reencode");
 if(Cancelled(c))return Failure<IntervalViewResultV3>("PROCESS.CANCELLED","sbdval_before_publication");
 return v;
}
IntervalBytesResultV3 EncodeIntervalSbdvalComposedV3(const IntervalOwnedValueV3&o,bool na,const IntervalExecutionControlV3&c) noexcept {
 auto v=ValidateIntervalValueViewV3(o.view(),na);
 if(!v.ok())return FromFailure<IntervalBytesResultV3>(v.diagnostic,v.status);
 u64 payload=o.state==IntervalValueStateV3::value?16:0,total=32+payload;
 if(total>c.maximum_allocation_bytes)return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdval_capacity",ResourceStatus());
 if(Cancelled(c))return Failure<IntervalBytesResultV3>("PROCESS.CANCELLED","before_allocation");
 std::array<byte,16>component{};Clear cc(component.data(),16,IntervalScrubClassV3::sbdval_component_staging,&c);
 if(payload)StoreComponent(component.data(),o.months,o.civil_days,o.fixed_nanoseconds);
 auto r=Success<IntervalBytesResultV3>();
 VectorClear<byte>cr(&r.bytes,IntervalScrubClassV3::sbdval_owned_buffer,c);
 try{r.bytes.resize(total);}
 catch(...){return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdval_allocation",ResourceStatus());}DatatypeBinaryValueView f{CanonicalTypeId::interval,o.state==IntervalValueStateV3::sql_null,false,payload?component.data():nullptr,static_cast<std::size_t>(payload)};
 auto w=EncodeDatatypeBinaryStructuralValueIntoNoAlloc(f,r.bytes.data(),r.bytes.size());
 if(!w.ok())return Failure<IntervalBytesResultV3>(w.diagnostic.diagnostic_code,"sbdval_encode",w.status);
 auto check=DecodeIntervalSbdvalComposedNoAllocV3(*o.profile,na,r.bytes,{});
 if(!check.ok())return FromFailure<IntervalBytesResultV3>(check.diagnostic,check.status);
 if(Cancelled(c))return Failure<IntervalBytesResultV3>("PROCESS.CANCELLED","before_publication");
 cr.Disarm();
 return r;
}
IntervalViewResultV3 DecodeIntervalSbdpvComposedNoAllocV3(const IntervalValidatedProfileHandleV3&p,bool na,std::span<const byte>e,const IntervalExecutionControlV3&c) noexcept {
 if(!ProfileValid(p,&c))return Failure<IntervalViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","sbdpv_profile");
 auto f=DecodeDatatypePhysicalStructuralValueViewNoAlloc(e.empty()?nullptr:e.data(),e.size());
 if(!f.ok())return Failure<IntervalViewResultV3>(f.diagnostic.diagnostic_code,"sbdpv_structure",f.status);
 if(f.value.type_id!=CanonicalTypeId::interval)return Failure<IntervalViewResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","sbdpv_type");
 if(f.value.state!=DatatypePhysicalValueState::value&&f.value.state!=DatatypePhysicalValueState::sql_null)return Failure<IntervalViewResultV3>("DATATYPE.NULL_STATE.INVALID","sbdpv_state");
 auto dc=c;dc.force_reencode_mismatch_for_conformance=false;
 auto v=DecodeCanonicalIntervalComponentNoAllocV3(p,f.value.state==DatatypePhysicalValueState::sql_null?IntervalValueStateV3::sql_null:IntervalValueStateV3::value,na,{f.value.payload_data,f.value.payload_bytes},dc);
 if(!v.ok())return v;
 std::array<byte,40>x{};Clear cx(x.data(),x.size(),IntervalScrubClassV3::sbdpv_reencode,&c);
 auto w=EncodeDatatypePhysicalStructuralValueIntoNoAlloc(f.value,x.data(),e.size());
 if(c.force_reencode_mismatch_for_conformance)x[0]^=1;
 if(!w.ok()||w.bytes_written!=e.size()||!std::equal(x.begin(),x.begin()+e.size(),e.begin()))return Failure<IntervalViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdpv_reencode");
 if(Cancelled(c))return Failure<IntervalViewResultV3>("PROCESS.CANCELLED","sbdpv_before_publication");
 return v;
}
IntervalBytesResultV3 EncodeIntervalSbdpvComposedV3(const IntervalOwnedValueV3&o,bool na,const IntervalExecutionControlV3&c) noexcept {
 auto v=ValidateIntervalValueViewV3(o.view(),na);
 if(!v.ok())return FromFailure<IntervalBytesResultV3>(v.diagnostic,v.status);
 u64 payload=o.state==IntervalValueStateV3::value?16:0,total=24+payload;
 if(total>c.maximum_allocation_bytes)return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdpv_capacity",ResourceStatus());
 if(Cancelled(c))return Failure<IntervalBytesResultV3>("PROCESS.CANCELLED","before_allocation");
 std::array<byte,16>component{};Clear cc(component.data(),16,IntervalScrubClassV3::sbdpv_component_staging,&c);
 if(payload)StoreComponent(component.data(),o.months,o.civil_days,o.fixed_nanoseconds);
 auto r=Success<IntervalBytesResultV3>();
 VectorClear<byte>cr(&r.bytes,IntervalScrubClassV3::sbdpv_owned_buffer,c);
 try{r.bytes.resize(total);}
 catch(...){return Failure<IntervalBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdpv_allocation",ResourceStatus());}DatatypePhysicalValueView f{CanonicalTypeId::interval,o.state==IntervalValueStateV3::sql_null?DatatypePhysicalValueState::sql_null:DatatypePhysicalValueState::value,payload?component.data():nullptr,static_cast<std::size_t>(payload)};
 auto w=EncodeDatatypePhysicalStructuralValueIntoNoAlloc(f,r.bytes.data(),r.bytes.size());
 if(!w.ok())return Failure<IntervalBytesResultV3>(w.diagnostic.diagnostic_code,"sbdpv_encode",w.status);
 auto check=DecodeIntervalSbdpvComposedNoAllocV3(*o.profile,na,r.bytes,{});
 if(!check.ok())return FromFailure<IntervalBytesResultV3>(check.diagnostic,check.status);
 if(Cancelled(c))return Failure<IntervalBytesResultV3>("PROCESS.CANCELLED","before_publication");
 cr.Disarm();
 return r;
}

IntervalIntrinsicDispositionV3 ClassifyIntervalIntrinsicOperationV3(IntervalIntrinsicOperationV3 o) noexcept{auto v=static_cast<unsigned>(o);
 return v<=6?IntervalIntrinsicDispositionV3::admitted:v<=12?IntervalIntrinsicDispositionV3::registered_refused:IntervalIntrinsicDispositionV3::unknown;}
IntervalValueResultV3 RefuseIntervalIntrinsicOperationV3(const IntervalValueViewV3&v,IntervalIntrinsicOperationV3 o) noexcept{if(!v.profile||!ProfileValid(*v.profile))return Failure<IntervalValueResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","intrinsic_profile");
 if(!ValidState(v.state))return Failure<IntervalValueResultV3>("DATATYPE.NULL_STATE.INVALID","intrinsic_state");
 auto d=ClassifyIntervalIntrinsicOperationV3(o);
 if(d==IntervalIntrinsicDispositionV3::admitted)return Failure<IntervalValueResultV3>("CTI.TEMPORAL.OPERATION_REFUSED","admitted_intrinsic_requires_implementation_entrypoint");
 if(d==IntervalIntrinsicDispositionV3::unknown||o==IntervalIntrinsicOperationV3::apply_to_temporal)return Failure<IntervalValueResultV3>("CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",d==IntervalIntrinsicDispositionV3::unknown?"unknown_intrinsic":"later_operator_owner");
 return Failure<IntervalValueResultV3>("CTI.TEMPORAL.ORDERING_REFUSED","no_natural_order");}

IntervalCastPolicyDispositionV3 ClassifyIntervalCastPolicyRowV3(u32 row,DatatypeCastContext c) noexcept{if(row==0||row>kIntervalClosedCastPolicyRowsV3||(c!=DatatypeCastContext::implicit&&c!=DatatypeCastContext::assignment&&c!=DatatypeCastContext::explicit_cast))return IntervalCastPolicyDispositionV3::forbidden;
 if(row==158)return IntervalCastPolicyDispositionV3::contextual_null;
 if(row==39||row==100||row==210)return IntervalCastPolicyDispositionV3::identity;
 if(row==124&&c==DatatypeCastContext::explicit_cast)return IntervalCastPolicyDispositionV3::explicit_character_to_interval;
 if(row==13&&c==DatatypeCastContext::explicit_cast)return IntervalCastPolicyDispositionV3::explicit_interval_to_character;
 return IntervalCastPolicyDispositionV3::forbidden;}
IntervalCastResultV3 CastIntervalValueV3(const IntervalCastRequestV3&r) noexcept {
 const auto disposition = ClassifyIntervalCastPolicyRowV3(
     r.one_based_policy_row, r.context);
 if (disposition == IntervalCastPolicyDispositionV3::forbidden)
   return Failure<IntervalCastResultV3>("DATATYPE.CAST_FORBIDDEN",
                                        "closed_cast_matrix");
 auto fail=[](std::string_view code,std::string_view detail) {
   return Failure<IntervalCastResultV3>(code,detail);
 };
 if(disposition==IntervalCastPolicyDispositionV3::contextual_null){
   if(r.interval_source||r.dynamic_interval_source||!r.scalar_source||
      r.scalar_source_identity||!r.interval_target||!(*r.interval_target)||
      !r.interval_target_descriptor||r.scalar_target!=CanonicalTypeId::unknown||
      r.scalar_target_identity||r.use_character_output_buffer||
      r.character_output||r.character_output_capacity)
     return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","contextual_null_shape_invalid");
   if(!ProfileValid(**r.interval_target)||
      !DescriptorBinds(*r.interval_target_descriptor,
                       (**r.interval_target).identity,CanonicalTypeId::interval))
     return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","target_authority_invalid");
   if(r.scalar_source->type_id!=CanonicalTypeId::null_type)
     return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","contextual_null_authority_invalid");
   if(!r.scalar_source->is_null||!r.scalar_source->encoded_value.empty())
     return fail("DATATYPE.NULL_STATE.INVALID","contextual_null_invalid");
   if(!r.target_null_allowed||!r.interval_target_descriptor->nullable_allowed)
     return fail("DATATYPE.NULL_NOT_ADMITTED","target_nonnullable");
   if(Cancelled(r.control))return fail("PROCESS.CANCELLED","before_cast_publication");
   auto out=Success<IntervalCastResultV3>();out.category=DatatypeCastCategory::identity;
   out.produced_interval=true;out.interval_value={*r.interval_target,
       IntervalValueStateV3::sql_null,0,0,0};
 return out;
 }
 if(disposition==IntervalCastPolicyDispositionV3::identity){
   const bool owned=r.interval_source!=nullptr,dynamic=r.dynamic_interval_source!=nullptr;
   if(owned==dynamic||!r.interval_target||!(*r.interval_target)||
      r.scalar_source||r.scalar_source_identity||r.interval_target_descriptor||
      r.scalar_target!=CanonicalTypeId::unknown||r.scalar_target_identity||
      r.use_character_output_buffer||r.character_output||r.character_output_capacity)
     return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","identity_cast_shape_invalid");
   const auto& source_profile=owned?r.interval_source->profile:r.dynamic_interval_source->profile;
   if(!source_profile||!ProfileValid(*source_profile)||!ProfileValid(**r.interval_target)||
      source_profile->equality_fingerprint!=(**r.interval_target).equality_fingerprint)
     return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","identity_cohort_mismatch");
   const auto source=owned?ValidateIntervalValueViewV3(r.interval_source->view(),true):
       AdmitIntervalOperandV3(*r.dynamic_interval_source,true);
   if(!source.ok())return FromFailure<IntervalCastResultV3>(source.diagnostic,source.status);
   if(source.value.state==IntervalValueStateV3::sql_null&&!r.target_null_allowed)
     return fail("DATATYPE.NULL_NOT_ADMITTED","identity_target_nonnullable");
   if(source.value.state==IntervalValueStateV3::value&&
      r.control.maximum_allocation_bytes<kIntervalComponentBytesV3)
     return Failure<IntervalCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                          "identity_result_grant",ResourceStatus());
   if(Cancelled(r.control))return fail("PROCESS.CANCELLED","before_cast_publication");
   auto out=Success<IntervalCastResultV3>();out.category=DatatypeCastCategory::identity;
   out.produced_interval=true;out.interval_value={*r.interval_target,source.value.state,
       source.value.months,source.value.civil_days,source.value.fixed_nanoseconds};
 return out;
 }
 if(disposition==IntervalCastPolicyDispositionV3::explicit_character_to_interval){
   if(r.interval_source||r.dynamic_interval_source||!r.scalar_source||
      !r.scalar_source_identity||!r.interval_target||!(*r.interval_target)||
      !r.interval_target_descriptor||r.scalar_target!=CanonicalTypeId::unknown||
      r.scalar_target_identity||r.use_character_output_buffer||
      r.character_output||r.character_output_capacity)
     return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","incoming_cast_shape_invalid");
   if(!ProfileValid(**r.interval_target)||
      !DescriptorBinds(*r.interval_target_descriptor,(**r.interval_target).identity,
                       CanonicalTypeId::interval)||
      r.scalar_source->type_id!=CanonicalTypeId::character||
      !ExactPeer(r.scalar_source_identity,CanonicalTypeId::character)||
      !DescriptorBinds(r.scalar_source->descriptor,*r.scalar_source_identity,
                       CanonicalTypeId::character))
     return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","character_source_authority_invalid");
   IntervalTextOperandV3 operand{r.scalar_source_identity,&r.scalar_source->descriptor,
       IntervalTextCarrierKindV3::utf8_bytes,
       r.scalar_source->is_null?IntervalValueStateV3::sql_null:IntervalValueStateV3::value,
       r.scalar_source->encoded_value,r.scalar_source->encoded_value.size()};
   auto parsed=ParseCanonicalIntervalOperandV3(*r.interval_target,operand,
                                                r.target_null_allowed,r.control);
   if(!parsed.ok())return FromFailure<IntervalCastResultV3>(parsed.diagnostic,parsed.status);
   if(parsed.value.state==IntervalValueStateV3::value&&
      r.control.maximum_allocation_bytes<kIntervalComponentBytesV3)
     return Failure<IntervalCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                          "interval_result_grant",ResourceStatus());
   if(Cancelled(r.control))return fail("PROCESS.CANCELLED","before_cast_publication");
   auto out=Success<IntervalCastResultV3>();out.category=DatatypeCastCategory::lossless_explicit;
   out.produced_interval=true;out.interval_value=std::move(parsed.value);
 return out;
 }
 const bool owned=r.interval_source!=nullptr,dynamic=r.dynamic_interval_source!=nullptr;
 if(owned==dynamic||r.interval_target||r.interval_target_descriptor||r.scalar_source||
    r.scalar_source_identity||r.scalar_target!=CanonicalTypeId::character||
    !r.scalar_target_identity||
    (!r.use_character_output_buffer&&(r.character_output||r.character_output_capacity)))
   return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","outgoing_cast_shape_invalid");
 const auto& source_profile=owned?r.interval_source->profile:r.dynamic_interval_source->profile;
 if(!source_profile||!ProfileValid(*source_profile)||
    !ExactPeer(r.scalar_target_identity,CanonicalTypeId::character)||
    !DescriptorBinds(r.scalar_target_descriptor,*r.scalar_target_identity,
                     CanonicalTypeId::character))
   return fail("CTI.INTERVAL.DESCRIPTOR_INVALID","character_target_authority_invalid");
 const auto source=owned?ValidateIntervalValueViewV3(r.interval_source->view(),true):
     AdmitIntervalOperandV3(*r.dynamic_interval_source,true);
 if(!source.ok())return FromFailure<IntervalCastResultV3>(source.diagnostic,source.status);
 if(source.value.state==IntervalValueStateV3::sql_null&&
    (!r.target_null_allowed||!r.scalar_target_descriptor.nullable_allowed))
   return fail("DATATYPE.NULL_NOT_ADMITTED","character_target_nonnullable");
 IntervalOwnedValueV3 stable{source_profile,source.value.state,source.value.months,
                             source.value.civil_days,source.value.fixed_nanoseconds};
 std::array<char,63> staged{};Clear clear(staged.data(),staged.size(),
      IntervalScrubClassV3::render_staging,&r.control);
 auto rendered=RenderCanonicalIntervalIntoNoAllocV3(stable,staged.data(),staged.size(),{});
 if(!rendered.ok())return FromFailure<IntervalCastResultV3>(rendered.diagnostic,rendered.status);
 const auto extent=rendered.bytes_written;
 if(r.scalar_target_descriptor.length&&extent>r.scalar_target_descriptor.length){
   auto out=Failure<IntervalCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED",
                                          "character_target_length");
   out.bytes_required=extent;
 return out;
 }
 if(r.use_character_output_buffer&&
    (extent>r.character_output_capacity||(extent&&!r.character_output))){
   auto out=Failure<IntervalCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED",
                                          "character_output_capacity");
   out.bytes_required=extent;
 return out;
 }
 if(extent>r.control.maximum_allocation_bytes)
   return Failure<IntervalCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                        "render_resource_grant",ResourceStatus());
 if(r.use_character_output_buffer&&r.character_output&&
    (RangesOverlap(r.character_output,extent,&r,sizeof(r))||
     (owned&&RangesOverlap(r.character_output,extent,r.interval_source,
                           sizeof(*r.interval_source)))||
     (dynamic&&RangesOverlap(r.character_output,extent,r.dynamic_interval_source,
                             sizeof(*r.dynamic_interval_source)))||
     OutputOverlapsProfile(r.character_output,extent,*source_profile)))
   return fail("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","character_output_alias");
 IntervalCastResultV3 out=Success<IntervalCastResultV3>();
 out.category=DatatypeCastCategory::lossless_explicit;out.scalar_value.type_id=CanonicalTypeId::character;
 out.scalar_value.is_null=source.value.state==IntervalValueStateV3::sql_null;
 out.scalar_value.descriptor=r.scalar_target_descriptor;out.bytes_required=extent;
 if(!r.use_character_output_buffer){
   try{out.scalar_value.encoded_value.assign(staged.data(),staged.data()+extent);}
   catch(...){return Failure<IntervalCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                                   "character_allocation",ResourceStatus());}
 }
 if(Cancelled(r.control))return fail("PROCESS.CANCELLED","before_cast_publication");
 if(r.use_character_output_buffer&&extent)std::memcpy(r.character_output,staged.data(),extent);
 out.used_character_output_buffer=r.use_character_output_buffer;
 out.bytes_written=r.use_character_output_buffer?extent:0;
 return out;
}

IntervalAliasAuditResultV3 AuditIntervalRequiredAliasResolutionV3(const IntervalAliasResolutionEvidenceV3&e,const IntervalValidatedProfileHandleV3&p) noexcept{auto fail=[&](IntervalAliasAuditDispositionV3 d){auto r=Failure<IntervalAliasAuditResultV3>("CTI.INTERVAL.DESCRIPTOR_INVALID","alias_resolution_invalid");
 r.disposition=d;
 return r;};
 if(static_cast<unsigned>(e.audit_case)>4)return fail(IntervalAliasAuditDispositionV3::unknown_audit_case);
 if(!ProfileValid(p))return fail(IntervalAliasAuditDispositionV3::profile_invalid);
 if(!e.registry_resolution_succeeded)return fail(IntervalAliasAuditDispositionV3::registry_unresolved);
 if(!ExactReceipt(e.registry_receipt)||e.registry_receipt.statement_receipt_uuid!=p.receipt.statement_receipt_uuid)return fail(IntervalAliasAuditDispositionV3::receipt_mismatch);
 if(!IsExactCanonicalIntervalTypeCodecIdentityV3(e.resolved_identity)||!SameIdentity(e.resolved_identity,p.identity))return fail(IntervalAliasAuditDispositionV3::identity_mismatch);
 auto r=Success<IntervalAliasAuditResultV3>();
 r.disposition=IntervalAliasAuditDispositionV3::same_authenticated_interval_cohort;
 r.same_interval_cohort=true;
 return r;}

}  // namespace scratchbird::core::datatypes
