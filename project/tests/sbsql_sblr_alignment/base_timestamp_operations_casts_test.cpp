// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_timestamp.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;

namespace {
unsigned checks = 0;
void Check(bool value, std::string_view message) {
  ++checks;
  if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); }
}
p::Uuid D709(){return p::Uuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,9}};}
std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> Profile(){auto r=dt::BuildCurrentTimestampValidatedProfileHandleV3(D709());Check(r.ok(),"profile");return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(std::move(r.profile));}
const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId type){for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(row.legacy_fields.catalog_snapshot_uuid==D709()&&row.legacy_fields.catalog_generation==9&&row.legacy_fields.registry_generation==9&&row.legacy_fields.canonical_binary_type_code==static_cast<p::u32>(type))return &row;return nullptr;}
scratchbird::engine::ExecutionTypeDescriptor Descriptor(dt::CanonicalTypeId type){auto manifest=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(manifest.ok(),"catalog");auto row=dt::LookupDatatypeCatalogRow(manifest.manifest,type);Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"descriptor row");dt::CatalogExecutionTypeMetadata metadata;metadata.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;metadata.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;auto result=dt::LookupExecutionTypeDescriptorFromCatalog(type,metadata);Check(result.ok(),"execution descriptor");return result.descriptor;}

struct CastShape { bool incoming=false, contextual=false, exact=false; dt::CanonicalTypeId peer=dt::CanonicalTypeId::unknown; };
CastShape Shape(unsigned row){
  if(row==1)return {true,true,false,dt::CanonicalTypeId::null_type};
  static constexpr std::array<dt::CanonicalTypeId,29> first{{
    dt::CanonicalTypeId::boolean,dt::CanonicalTypeId::int8,dt::CanonicalTypeId::int16,dt::CanonicalTypeId::int32,dt::CanonicalTypeId::int64,dt::CanonicalTypeId::int128,
    dt::CanonicalTypeId::uint8,dt::CanonicalTypeId::uint16,dt::CanonicalTypeId::uint32,dt::CanonicalTypeId::uint64,dt::CanonicalTypeId::uint128,
    dt::CanonicalTypeId::bfloat16,dt::CanonicalTypeId::real16,dt::CanonicalTypeId::real32,dt::CanonicalTypeId::real64,dt::CanonicalTypeId::real128,
    dt::CanonicalTypeId::decimal,dt::CanonicalTypeId::decimal_float,dt::CanonicalTypeId::uuid,dt::CanonicalTypeId::ip_address,
    dt::CanonicalTypeId::network_prefix,dt::CanonicalTypeId::mac_address,dt::CanonicalTypeId::character,dt::CanonicalTypeId::binary,
    dt::CanonicalTypeId::bit_string,dt::CanonicalTypeId::timestamp,dt::CanonicalTypeId::date,dt::CanonicalTypeId::time,dt::CanonicalTypeId::interval}};
  static constexpr std::array<dt::CanonicalTypeId,56> rest{{
    dt::CanonicalTypeId::blob,dt::CanonicalTypeId::document,dt::CanonicalTypeId::json_document,dt::CanonicalTypeId::binary_json_document,
    dt::CanonicalTypeId::bson_document,dt::CanonicalTypeId::xml_document,dt::CanonicalTypeId::hstore_document,dt::CanonicalTypeId::object_document,
    dt::CanonicalTypeId::flattened_object_document,dt::CanonicalTypeId::enum_value,dt::CanonicalTypeId::set_value,dt::CanonicalTypeId::array,
    dt::CanonicalTypeId::list,dt::CanonicalTypeId::map,dt::CanonicalTypeId::row,dt::CanonicalTypeId::composite,dt::CanonicalTypeId::variant,
    dt::CanonicalTypeId::range,dt::CanonicalTypeId::multirange,dt::CanonicalTypeId::token_stream,dt::CanonicalTypeId::search_query,
    dt::CanonicalTypeId::search_rank_feature,dt::CanonicalTypeId::search_completion,dt::CanonicalTypeId::search_percolator,
    dt::CanonicalTypeId::geometry,dt::CanonicalTypeId::geography,dt::CanonicalTypeId::point,dt::CanonicalTypeId::shape,dt::CanonicalTypeId::raster,
    dt::CanonicalTypeId::vector,dt::CanonicalTypeId::dense_vector,dt::CanonicalTypeId::sparse_vector,dt::CanonicalTypeId::binary_vector,
    dt::CanonicalTypeId::quantized_vector,dt::CanonicalTypeId::graph_node,dt::CanonicalTypeId::graph_edge,dt::CanonicalTypeId::graph_path,
    dt::CanonicalTypeId::time_series_value,dt::CanonicalTypeId::columnar_segment,dt::CanonicalTypeId::aggregate_state,dt::CanonicalTypeId::hll_sketch,
    dt::CanonicalTypeId::bloom_filter,dt::CanonicalTypeId::quantile_sketch,dt::CanonicalTypeId::histogram_sketch,
    dt::CanonicalTypeId::ranking_summary,dt::CanonicalTypeId::vector_summary,dt::CanonicalTypeId::lob_locator,
    dt::CanonicalTypeId::external_file_locator,dt::CanonicalTypeId::remote_object_locator,dt::CanonicalTypeId::bridge_handle,
    dt::CanonicalTypeId::cursor_handle,dt::CanonicalTypeId::system_reference,dt::CanonicalTypeId::opaque_extension,
    dt::CanonicalTypeId::cursor,dt::CanonicalTypeId::result_set,dt::CanonicalTypeId::table_value}};
  bool incoming=row>=138;unsigned out=incoming?row-84:row;dt::CanonicalTypeId peer=dt::CanonicalTypeId::unknown;bool exact=false;
  if(out>=28&&out<=56){peer=first[out-28];exact=true;}
  else if(out>=57&&out<=112){peer=rest[out-57];exact=out==59||out==66||out==69||out==81;}
  else if(row>=2&&row<=26){incoming=true;peer=first[row-2];exact=true;}
  else if(out==27)peer=dt::CanonicalTypeId::null_type;
  return {incoming,false,exact,peer};
}

struct ValueHash { std::int32_t day; p::u64 nanos; std::string_view hash; };
constexpr std::array<ValueHash,20> kValueHashes{{
 {INT32_MIN,0,"2642abccdb75bbb49970a764d7b9fb4f94e7d55e555468771c5e4c0d1b300e99"},
 {INT32_MIN,86'399'999'999'999ull,"65d25c1e618c6197e387c5687dc26fcaf97d3fc93ee612d0a9387ea88f92b151"},
 {-719528,0,"60aee601628db8274bdd9fe7fcdc2cc367d6c7e619f49be4606607338d99cd6c"},
 {-719469,43'200'000'000'000ull,"dfc9df1b41eb5bf12b8ab89637e5c8d49c87b82a3de332b0c0d833224f316759"},
 {-1,86'399'999'999'999ull,"9ef7c39140dde00e549f6666ad3fb029451857feacbe659ef577fd21764422ed"},
 {0,0,"70ac0050666cfaee4f60e6b1ac9bf8f350d53fc756acd2cb01c1b849cc884a5b"},
 {0,1,"9921d9c0b74d9819272b3cd8063fccd9f3ef8e96e83ff90eb698cd28eb901270"},
 {0,123'400'000,"83cd90fb0df755166c631b8f0937e70f0690b4b6649c1ff929ee42a45a732690"},
 {789,0,"6680e5c4cd1b59fdf8fd12f5f2c04f8f6b65d63cfc25c4ad469fcf6334dd282d"},
 {-25509,86'399'000'000'000ull,"adb85bc4bc91b1dbcfe1026308af7ceab36472e134c175c28a830fd04358d91f"},
 {11016,45'296'123'400'000ull,"cb6f3ef04e0715e4cd6f38dea058e2b37795f4f2134820d041708666cbebf829"},
 {11322,86'399'999'999'999ull,"412589b3ee68eadd0fd6cd7b690be54cd54c96c1f3efa9c4099fc1288d2b68f2"},
 {11323,0,"dca56e0f14ae123c8e57b2fffa29be6b3f84df0fa186fbc6f6cd4972cab4f9e5"},
 {19904,43'200'000'000'000ull,"5f802fc9465c3b82e3613e9be16467384e3c70ef3eb60123d6e32011bdc8f634"},
 {19782,22'028'900'000'000ull,"ef03d0ee5e2e30dab11821743d328345669765e3b1ca3b7370ee4b3a0d709150"},
 {20090,11'045'100'000'000ull,"7259626574c552373b332309bdfe9b3e75a611a59c217271a4f0c5cac39148f3"},
 {20090,11'045'123'456'780ull,"9888e88883d7661fd336af2a5ed0b0268349d28cc5184adf30bdbf9a60adcf76"},
 {2932896,86'399'999'999'999ull,"003339967cac7bd80ba918bdab0ac206b0424fde60d91bdd6899a4d5b59aa117"},
 {2932897,0,"5fc51d5802674f72234d8575206c16e25f4b1f652e3afc65cb5711bfbb4ae8ab"},
 {INT32_MAX,86'399'999'999'999ull,"218a0446f90e1045b53f2f1d51f64329eb95820ca03e74c9a00db8490c18c4ef"},
}};
constexpr std::string_view kNullHash="363cc0395d66be1eb59cfdbc266030d1fda560b7b3556d80635e0cf4cc0691bc";
std::array<p::byte,32> Hex32(std::string_view text){std::array<p::byte,32> out{};auto n=[](char c){return static_cast<unsigned>(c<='9'?c-'0':(c|32)-'a'+10);};for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<p::byte>((n(text[2*i])<<4)|n(text[2*i+1]));return out;}

void HashAndCompare(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& p0){
  std::array<dt::TimestampOwnedValueV3,21> values{};
  for(std::size_t i=0;i<kValueHashes.size();++i)values[i]={p0,dt::TimestampValueStateV3::value,kValueHashes[i].day,kValueHashes[i].nanos};
  values.back()={p0,dt::TimestampValueStateV3::sql_null,0,0};
  for(std::size_t i=0;i<kValueHashes.size();++i){auto hash=dt::HashTimestampValueV3(values[i]);Check(hash.ok()&&hash.bytes.size()==32&&std::equal(hash.bytes.begin(),hash.bytes.end(),Hex32(kValueHashes[i].hash).begin()),"exact SBTSPH01 PRESENT hash");}
  auto null_hash=dt::HashTimestampValueV3(values.back());Check(null_hash.ok()&&std::equal(null_hash.bytes.begin(),null_hash.bytes.end(),Hex32(kNullHash).begin()),"exact SBTSPH01 NULL hash");
  for(std::size_t i=0;i<values.size();++i)for(std::size_t j=0;j<values.size();++j){auto relation=dt::CompareTimestampValuesV3(values[i].view(),values[j].view());Check(relation.ok(),"comparison admitted");if(i==20||j==20)Check(relation.fact==dt::TimestampComparisonFactV3::unordered_null,"NULL unordered");else{auto expected=kValueHashes[i].day<kValueHashes[j].day?dt::TimestampComparisonFactV3::less:kValueHashes[i].day>kValueHashes[j].day?dt::TimestampComparisonFactV3::greater:kValueHashes[i].nanos<kValueHashes[j].nanos?dt::TimestampComparisonFactV3::less:kValueHashes[i].nanos>kValueHashes[j].nanos?dt::TimestampComparisonFactV3::greater:dt::TimestampComparisonFactV3::equal;Check(relation.fact==expected,"signed-day then unsigned-nanosecond comparison");}}
  auto bad=*p0;bad.comparison_fingerprint[0]^=1;Check(!dt::CompareTimestampValuesWithValidatedCohortForConformanceV3(values[0].view(),{&bad,dt::TimestampValueStateV3::value,0,0},bad.comparison_fingerprint).ok(),"comparison cohort mismatch");
  dt::TimestampValueViewV3 poisoned_present{p0.get(),dt::TimestampValueStateV3::value,0,~p::u64{0}};auto null_before_present=dt::CompareTimestampValuesV3(values.back().view(),poisoned_present);Check(null_before_present.ok()&&null_before_present.fact==dt::TimestampComparisonFactV3::unordered_null,"comparison clean NULL propagates without other PRESENT payload access");

  auto bad_profile=*p0;bad_profile.profile_fingerprint[0]^=1;
  dt::TimestampValueViewV3 dirty_left{p0.get(),dt::TimestampValueStateV3::sql_null,1,0};
  dt::TimestampValueViewV3 bad_profile_right{&bad_profile,dt::TimestampValueStateV3::value,0,0};
  auto precedence=dt::CompareTimestampValuesV3(dirty_left,bad_profile_right);
  Check(!precedence.ok()&&precedence.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "comparison validates every profile before any value state");
  auto difference=dt::DifferenceTimestampV3(dirty_left,bad_profile_right);
  Check(!difference.ok()&&difference.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "difference validates every profile before any value state");

  dt::TimestampOwnedValueV3 overlap_value{p0,dt::TimestampValueStateV3::value,0,0};
  const auto overlap_day=overlap_value.civil_day;
  const auto overlap_nanos=overlap_value.nanoseconds_since_midnight;
  auto* overlap_output=reinterpret_cast<p::byte*>(&overlap_value);
  dt::TimestampExecutionControlV3 no_budget;no_budget.maximum_allocation_bytes=0;
  auto overlap=dt::HashTimestampValueIntoNoAllocV3(
      overlap_value,overlap_output,0,no_budget);
  Check(!overlap.ok()&&overlap.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&
            overlap_value.civil_day==overlap_day&&
            overlap_value.nanoseconds_since_midnight==overlap_nanos,
        "hash overlap precedes capacity and resource and publishes nothing");
  overlap=dt::MakeTimestampSortKeyIntoNoAllocV3(
      overlap_value,dt::TimestampSortDirectionV3::ascending,
      dt::TimestampNullModeV3::nulls_last,overlap_output,0,no_budget);
  Check(!overlap.ok()&&overlap.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&
            overlap_value.civil_day==overlap_day&&
            overlap_value.nanoseconds_since_midnight==overlap_nanos,
        "sort-key overlap precedes capacity and resource and publishes nothing");
}

void OrderedKeys(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& p0){
  std::array<dt::TimestampOwnedValueV3,21> values{};for(std::size_t i=0;i<20;++i)values[i]={p0,dt::TimestampValueStateV3::value,kValueHashes[i].day,kValueHashes[i].nanos};values[20]={p0,dt::TimestampValueStateV3::sql_null,0,0};
  unsigned rows=0;
  for(auto direction:{dt::TimestampSortDirectionV3::ascending,dt::TimestampSortDirectionV3::descending})for(auto null_mode:{dt::TimestampNullModeV3::nulls_first,dt::TimestampNullModeV3::nulls_last})for(const auto& value:values){auto key=dt::MakeTimestampSortKeyV3(value,direction,null_mode);Check(key.ok()&&key.bytes.size()==(value.state==dt::TimestampValueStateV3::sql_null?100:112),"ordered key extent");Check(std::memcmp(key.bytes.data(),"SBTSPK01",8)==0,"ordered key magic");auto decoded=dt::DecodeTimestampSortKeyNoAllocV3(*p0,key.bytes);Check(decoded.ok()&&decoded.value.state==value.state&&decoded.value.civil_day==value.civil_day&&decoded.value.nanoseconds_since_midnight==value.nanoseconds_since_midnight,"ordered key decode/reencode");++rows;}
  Check(rows==84,"exact 84 ordered key vectors");
  auto before=dt::MakeTimestampSortKeyV3({p0,dt::TimestampValueStateV3::value,-1,86'399'999'999'999ull},dt::TimestampSortDirectionV3::ascending,dt::TimestampNullModeV3::nulls_first);
  auto epoch=dt::MakeTimestampSortKeyV3({p0,dt::TimestampValueStateV3::value,0,0},dt::TimestampSortDirectionV3::ascending,dt::TimestampNullModeV3::nulls_first);
  Check(before.bytes<epoch.bytes,"durable suffix preserves pre-epoch local-civil order");
  auto bad_mode=epoch.bytes;bad_mode[96]=2;
  auto decoded_bad_mode=dt::DecodeTimestampSortKeyNoAllocV3(*p0,bad_mode);
  Check(!decoded_bad_mode.ok()&&decoded_bad_mode.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "malformed persisted sort direction is canonical corruption");
}

void Casts(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& p0){
  auto* character_identity=Identity(dt::CanonicalTypeId::character);Check(character_identity!=nullptr,"character identity");auto character_descriptor=Descriptor(dt::CanonicalTypeId::character);auto timestamp_descriptor=Descriptor(dt::CanonicalTypeId::timestamp);
  dt::DatatypeOperationValue text{dt::CanonicalTypeId::character,"2024-02-29T06:07:08.9",false,character_descriptor},null_value{dt::CanonicalTypeId::null_type,"",true,{}};dt::TimestampOwnedValueV3 value{p0,dt::TimestampValueStateV3::value,19782,22'028'900'000'000ull};
  unsigned decisions=0,admitted=0;
  dt::DatatypeOperationValue scalar;
  for(unsigned row=1;row<=221;++row){const auto shape=Shape(row);for(auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,dt::DatatypeCastContext::explicit_cast}){auto want=dt::TimestampCastPolicyDispositionV3::forbidden;if(row==1)want=dt::TimestampCastPolicyDispositionV3::contextual_null;else if(row==24&&context==dt::DatatypeCastContext::explicit_cast)want=dt::TimestampCastPolicyDispositionV3::explicit_character_to_timestamp;else if(row==50&&context==dt::DatatypeCastContext::explicit_cast)want=dt::TimestampCastPolicyDispositionV3::explicit_timestamp_to_character;else if(row==53)want=dt::TimestampCastPolicyDispositionV3::identity;Check(dt::ClassifyTimestampCastPolicyRowV3(row,context)==want,"221x3 cast classification");dt::TimestampCastRequestV3 q;q.one_based_policy_row=row;q.context=context;
    if(row==53){q.timestamp_source=&value;q.timestamp_target=&p0;}
    else if(shape.incoming){scalar={};scalar.type_id=shape.peer;scalar.is_null=shape.contextual;scalar.encoded_value=shape.contextual?"":(row==24?"2024-02-29T06:07:08.9":"poison-present-payload-must-not-be-read");if(shape.exact){q.scalar_source_identity=Identity(shape.peer);Check(q.scalar_source_identity!=nullptr,"exact incoming identity");scalar.descriptor=Descriptor(shape.peer);}q.scalar_source=&scalar;q.timestamp_target=&p0;q.timestamp_target_descriptor=&timestamp_descriptor;}
    else {q.timestamp_source=&value;q.scalar_target=shape.peer;if(shape.exact){q.scalar_target_identity=Identity(shape.peer);Check(q.scalar_target_identity!=nullptr,"exact outgoing identity");q.scalar_target_descriptor=Descriptor(shape.peer);}}
    auto result=dt::CastTimestampValueV3(q);if(want==dt::TimestampCastPolicyDispositionV3::forbidden)Check(!result.ok()&&result.diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","221x3 endpoint-bound runtime forbidden cast");else{Check(result.ok(),"221x3 endpoint-bound runtime admitted cast");++admitted;}++decisions;}}
  Check(decisions==663&&admitted==8,"closed cast matrix");

  // Exact UUID/generation/profile/descriptor authority is resolved before
  // state and before the closed-policy refusal.
  auto* int32_identity=Identity(dt::CanonicalTypeId::int32);Check(int32_identity!=nullptr,"int32 identity");auto int32_descriptor=Descriptor(dt::CanonicalTypeId::int32);
  dt::TimestampCastRequestV3 exact_out;exact_out.one_based_policy_row=31;exact_out.timestamp_source=&value;exact_out.scalar_target=dt::CanonicalTypeId::int32;exact_out.scalar_target_identity=int32_identity;exact_out.scalar_target_descriptor=int32_descriptor;
  Check(dt::CastTimestampValueV3(exact_out).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","exact outbound endpoint reaches policy");
  auto changed_identity=*int32_identity;changed_identity.legacy_fields.type_uuid.bytes[0]^=1;exact_out.scalar_target_identity=&changed_identity;Check(dt::CastTimestampValueV3(exact_out).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","peer UUID mutation precedes policy");exact_out.scalar_target_identity=int32_identity;exact_out.scalar_target_descriptor.descriptor_uuid.bytes[0]^=1;Check(dt::CastTimestampValueV3(exact_out).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","peer descriptor mutation precedes policy");
  exact_out.scalar_target_descriptor=int32_descriptor;changed_identity=*int32_identity;changed_identity.legacy_fields.codec_generation++;exact_out.scalar_target_identity=&changed_identity;Check(dt::CastTimestampValueV3(exact_out).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","peer generation mutation precedes policy");exact_out.scalar_target_identity=int32_identity;
  dt::DatatypeOperationValue int32_value{dt::CanonicalTypeId::int32,"poison-present-payload-must-not-be-read",false,int32_descriptor};dt::TimestampCastRequestV3 exact_in;exact_in.one_based_policy_row=5;exact_in.scalar_source=&int32_value;exact_in.scalar_source_identity=int32_identity;exact_in.timestamp_target=&p0;exact_in.timestamp_target_descriptor=&timestamp_descriptor;Check(dt::CastTimestampValueV3(exact_in).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","forbidden incoming PRESENT payload is not decoded");
  int32_value.is_null=true;int32_value.encoded_value="dirty";Check(dt::CastTimestampValueV3(exact_in).diagnostic.diagnostic_code=="DATATYPE.NULL_STATE.INVALID","forbidden incoming dirty NULL precedes policy");int32_value.encoded_value.clear();exact_in.target_null_allowed=false;Check(dt::CastTimestampValueV3(exact_in).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED","forbidden incoming nullability precedes policy");

  // Unresolved Core rows bind only the row's sealed type code (or unknown for
  // spec-only rows); fabricated identity or descriptor authority is refused.
  dt::TimestampCastRequestV3 unresolved;unresolved.one_based_policy_row=57;unresolved.timestamp_source=&value;unresolved.scalar_target=dt::CanonicalTypeId::blob;Check(dt::CastTimestampValueV3(unresolved).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","unresolved base peer fails closed at policy");unresolved.scalar_target_identity=int32_identity;Check(dt::CastTimestampValueV3(unresolved).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","unresolved peer identity claim refused");unresolved.scalar_target_identity=nullptr;unresolved.scalar_target_descriptor=int32_descriptor;Check(dt::CastTimestampValueV3(unresolved).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","unresolved peer descriptor claim refused");
  dt::TimestampCastRequestV3 spec_only;spec_only.one_based_policy_row=113;spec_only.timestamp_source=&value;Check(dt::CastTimestampValueV3(spec_only).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","spec-only unknown peer fails closed");spec_only.scalar_target=dt::CanonicalTypeId::blob;Check(dt::CastTimestampValueV3(spec_only).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","spec-only row rejects invented type binding");

  // Dynamic timestamp sources prove host carriers before policy lookup.
  dt::TimestampOperandV3 dynamic{p0,dt::TimestampValueStateV3::value,dt::TimestampDayCarrierKindV3::signed_i32,19782,dt::TimestampUnsignedCarrierKindV3::unsigned_u64,22'028'900'000'000ull};exact_out.scalar_target_descriptor=int32_descriptor;exact_out.dynamic_timestamp_source=&dynamic;exact_out.timestamp_source=nullptr;Check(dt::CastTimestampValueV3(exact_out).diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","dynamic exact endpoint reaches policy");dynamic.day_carrier=dt::TimestampDayCarrierKindV3::wrong_host_type;Check(dt::CastTimestampValueV3(exact_out).diagnostic.diagnostic_code=="SBLR.OPERAND_INVALID","dynamic carrier refusal precedes policy");dynamic.day_carrier=dt::TimestampDayCarrierKindV3::signed_i32;

  // timestamp(p) has no admitted d709 profile: a precision modifier cannot be
  // rebound to base.timestamp and fails at the target descriptor gate.
  auto timestamp_p=timestamp_descriptor;timestamp_p.precision=3;timestamp_p.modifier_flags=scratchbird::engine::ExecutionTypeModifierFlagBit(scratchbird::engine::ExecutionTypeModifierFlag::precision);dt::TimestampCastRequestV3 parameterized;parameterized.one_based_policy_row=1;parameterized.scalar_source=&null_value;parameterized.timestamp_target=&p0;parameterized.timestamp_target_descriptor=&timestamp_p;Check(dt::CastTimestampValueV3(parameterized).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","timestamp(p) explicitly unsupported and fails closed");
  auto invalid_profile_mutable=std::make_shared<dt::TimestampValidatedProfileHandleV3>(*p0);invalid_profile_mutable->receipt.catalog_generation++;std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> invalid_profile=invalid_profile_mutable;parameterized.timestamp_target=&invalid_profile;parameterized.timestamp_target_descriptor=&timestamp_descriptor;Check(dt::CastTimestampValueV3(parameterized).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","timestamp target receipt mutation precedes NULL state and policy");
  dt::TimestampCastRequestV3 malformed_shape;malformed_shape.one_based_policy_row=24;malformed_shape.context=dt::DatatypeCastContext::explicit_cast;malformed_shape.scalar_source=&text;malformed_shape.scalar_source_identity=character_identity;malformed_shape.timestamp_target=&p0;malformed_shape.timestamp_target_descriptor=&timestamp_descriptor;malformed_shape.timestamp_source=&value;Check(dt::CastTimestampValueV3(malformed_shape).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","incoming row rejects contradictory timestamp source");

  dt::TimestampCastRequestV3 out;out.one_based_policy_row=50;out.context=dt::DatatypeCastContext::explicit_cast;out.timestamp_source=&value;out.scalar_target=dt::CanonicalTypeId::character;out.scalar_target_identity=character_identity;out.scalar_target_descriptor=character_descriptor;auto rendered=dt::CastTimestampValueV3(out);Check(rendered.ok()&&rendered.scalar_value.encoded_value=="2024-02-29T06:07:08.9","timestamp to character exact text");
  std::array<char,33> cast_output{};out.use_character_output_buffer=true;out.character_output=cast_output.data();out.character_output_capacity=20;out.control.maximum_allocation_bytes=0;auto capacity_first=dt::CastTimestampValueV3(out);Check(!capacity_first.ok()&&capacity_first.diagnostic.diagnostic_code=="CTB.TEXT.LENGTH_EXCEEDED","cast caller capacity precedes resource budget");out.character_output=reinterpret_cast<char*>(&out);out.character_output_capacity=33;auto overlap_first=dt::CastTimestampValueV3(out);Check(!overlap_first.ok()&&overlap_first.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","cast output overlap precedes resource budget");out.character_output=cast_output.data();auto resource=dt::CastTimestampValueV3(out);Check(!resource.ok()&&resource.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED","cast resource refusal follows capacity and overlap");out.control.maximum_allocation_bytes=33;auto bounded=dt::CastTimestampValueV3(out);Check(bounded.ok()&&bounded.bytes_written==21&&std::string_view(cast_output.data(),bounded.bytes_written)=="2024-02-29T06:07:08.9","cast exact caller buffer publication");
  out.character_output=reinterpret_cast<char*>(&out);out.character_output_capacity=1;
  auto combined_overlap=dt::CastTimestampValueV3(out);
  Check(!combined_overlap.ok()&&combined_overlap.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "cast overlap precedes caller capacity");
  text.encoded_value="2024-02-29T06:07:08.9Z";dt::TimestampCastRequestV3 in;in.one_based_policy_row=24;in.context=dt::DatatypeCastContext::explicit_cast;in.scalar_source=&text;in.scalar_source_identity=character_identity;in.timestamp_target=&p0;in.timestamp_target_descriptor=&timestamp_descriptor;Check(dt::CastTimestampValueV3(in).diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","zone suffix refused without timezone access");
  Check(dt::ParseCanonicalTimestampV3(p0,"1970-00-01X12:34:59").diagnostic.diagnostic_code==
            "CTI.TEMPORAL.ZERO_DATE_REFUSED",
        "date zero diagnostic precedes separator fault");
  text.encoded_value="2024-02-29T06:07:08.9";
  text.descriptor=character_descriptor;
  text.descriptor.length=1;
  text.descriptor.modifier_flags=scratchbird::engine::ExecutionTypeModifierFlagBit(
      scratchbird::engine::ExecutionTypeModifierFlag::length);
  Check(dt::CastTimestampValueV3(in).diagnostic.diagnostic_code==
            "CTB.TEXT.LENGTH_EXCEEDED",
        "character to timestamp enforces declared source length after parse");
  text.descriptor=character_descriptor;
  in.control.maximum_allocation_bytes=15;
  Check(dt::CastTimestampValueV3(in).diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED",
        "character to timestamp enforces PRESENT component resource grant");
  dt::TimestampOperandV3 bad_operand;
  bad_operand.day_carrier=dt::TimestampDayCarrierKindV3::wrong_host_type;
  bad_operand.time_carrier=dt::TimestampUnsignedCarrierKindV3::wrong_host_type;
  bad_operand.state=static_cast<dt::TimestampValueStateV3>(255);
  Check(dt::AdmitTimestampOperandV3(bad_operand).diagnostic.diagnostic_code==
            "SBLR.OPERAND_INVALID",
        "operand carrier refusal precedes profile and state");
  dt::TimestampTextOperandV3 bad_text;
  bad_text.carrier=dt::TimestampTextCarrierKindV3::wrong_host_type;
  Check(dt::ParseCanonicalTimestampOperandV3({},bad_text).diagnostic.diagnostic_code==
            "SBLR.OPERAND_INVALID",
        "text carrier refusal precedes profile and descriptor");
  dt::TimestampOwnedValueV3 bad_timestamp;
  bad_timestamp.state=static_cast<dt::TimestampValueStateV3>(255);
  dt::TimestampNullableI64FactV3 bad_delta;
  bad_delta.carrier=dt::TimestampI64CarrierKindV3::wrong_host_type;
  Check(dt::AddTimestampNanosecondsV3(bad_timestamp,bad_delta).diagnostic.diagnostic_code==
            "SBLR.OPERAND_INVALID"&&
            dt::SubtractTimestampNanosecondsV3(bad_timestamp,bad_delta).diagnostic.diagnostic_code==
            "SBLR.OPERAND_INVALID",
        "delta carrier refusal precedes timestamp profile and state");
}
}  // namespace
int main(){auto p0=Profile();HashAndCompare(p0);OrderedKeys(p0);Casts(p0);std::cout<<"PASS base.timestamp V3 operations/casts checks="<<checks<<" hashes=21 keys=84 decisions=663\n";}
