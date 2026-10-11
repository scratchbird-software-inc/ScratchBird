// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/timestamp_successor_fixture.hpp"
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
p::Uuid D710(){return p::Uuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}};}
std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> Profile(){auto r=dt::BuildCurrentTimestampValidatedProfileHandleV3(D710());Check(r.ok(),"profile");return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(std::move(r.profile));}
const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId type, bool successor=false){for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(row.legacy_fields.catalog_snapshot_uuid==(successor?dt::kDatatypeCohortV11:D710())&&row.legacy_fields.catalog_generation==(successor?11:10)&&row.legacy_fields.registry_generation==(successor?11:10)&&row.legacy_fields.canonical_binary_type_code==static_cast<p::u32>(type))return &row;return nullptr;}
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
 {INT32_MIN,0,"79549c04c82189982af22598926d5990ae9ae5cc3d88a619fc3bcd69412fa5d1"},
 {INT32_MIN,86'399'999'999'999ull,"8958dbe3655bada6a86696ba2c9c2fcc229004bc5ba0fde5cb26d83b1e04769c"},
 {-719528,0,"6cb636ce5d78fa7c325f3f8396c53ceaf86729df823ed08a025e59a9c5687a73"},
 {-719469,43'200'000'000'000ull,"bba23ad63920ee88f4823153ebe7192a735dbf3aa0633d400c948398426b15d4"},
 {-1,86'399'999'999'999ull,"1a0e9cefc3c9489e3103c9fc7dad8cb29cbb302f49d945b3ca2253e4faed6680"},
 {0,0,"42f156cd01adbabf7a295c4510cc12369186e36b2c645e7b1fb8879bebe363fe"},
 {0,1,"512a389710a0b2642d922d2e4221a4d67fe949cccd142fbe94c9caeb4d0b26cd"},
 {0,123'400'000,"4b0f8155a78fb33e5684489fb5129effad6a87a3bb064f0e65fcda82ddee2a4d"},
 {789,0,"ea46a6dc11bc45884d991b7e455077e00f9962d3a636aaeda55fbac4aeb6e17a"},
 {-25509,86'399'000'000'000ull,"647186e95340cad3fe84d3c279a8f5054a8541fda2ec0f671dca42895278bff0"},
 {11016,45'296'123'400'000ull,"35ba1a8a1b402572d1b013969a36af3e0ef06b39f6fa23d1ae96cb2b3f975121"},
 {11322,86'399'999'999'999ull,"72a2a5d0c7f66ad30faaf0bfa171ba770ed99c8ac09c177eb7881fde98e7287e"},
 {11323,0,"5f5ce7144df76d034562215a9ea919a38d144c21d0c1224b1ecd52e2aaec0d0a"},
 {19904,43'200'000'000'000ull,"a643ffcedfedfbe8556d01662929ce4b26781079ab2ab5ac31f48efa6d9781b0"},
 {19782,22'028'900'000'000ull,"5e8c7cf4fa3f583c2344b114cc16c99836201ca93150090aa57090d81ca1100a"},
 {20090,11'045'100'000'000ull,"d37a48c2a878999dbe3087bea61e5a4f2afd3f06608f24a310866fa38d258476"},
 {20090,11'045'123'456'780ull,"da63e6b3ac33b1f031e9ca854d7be361ed2f7294adf209b7a311f886689228f2"},
 {2932896,86'399'999'999'999ull,"33afbf1406ff55d8bce7f60c771339be2fea490deae78c3f607e9ca96263b02f"},
 {2932897,0,"fe8b3f42380516d47ba841f45aa1c1849d5270dafce87d3c6146d2cbdd8c1739"},
 {INT32_MAX,86'399'999'999'999ull,"a59e5f30764509f291dbaea607cb1dd25c1abf48a0285e11c4d1b97ecb044692"},
}};
constexpr std::string_view kNullHash="e21379194989dd7aa6ae43ca7601c171a6deb8094d2bd409e41d56d3ca25047a";
std::array<p::byte,32> Hex32(std::string_view text){std::array<p::byte,32> out{};auto n=[](char c){return static_cast<unsigned>(c<='9'?c-'0':(c|32)-'a'+10);};for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<p::byte>((n(text[2*i])<<4)|n(text[2*i+1]));return out;}

// Independent SHA-256 over the specified D711 header and LE16 fixtures.
constexpr std::array<std::string_view, 20> kD711ValueHashes{{
  "c73b238ca6f93da5c401d0f951115d6e8a4959f4a734b74319eafe20c3dedc3b",
  "eca92968f5101786bfb7811e287b79d98d131d88ef48ee52503978aefb6209cb",
  "d4ebfe5e00c023d65f0be961f2287a928731ab0d223e56f72f1ee6e44bf3feb4",
  "f58decc29efc6863f05612bfecd7cda6c5a07a9c5cfcedbe0dc06dc654d1acf4",
  "d8554a80955deab44924922ee611b525bc8f93e74de4a5151331a89ae695054a",
  "833264d581e4c1e83d23bfccb31a6fa7bbbf5aabf359fcfe0fbacdb5b0b83262",
  "805ae5c7ef7a894f8cfa433d999d1556b7e6176b865145463dd983455d724402",
  "a14b6499c419ac0748a405047acf97a8d9ee710751b9ede3a5157757a972ee84",
  "4c794b70d7a69d1084f36011ee1ddc9605ccd4393fab5dbbce07e9ea7206e32e",
  "ca5a3f0384326043ad445ee49153faaef840c9a15dddd7d48fa159d2e272e596",
  "80245fde1e2570ccf3958b5e97c8468d475917ee99972ba1c4f8fcb6c5c51eab",
  "cee1dd92e84dd3524fdce538ab6416381513dbd8af2c51a894fc522c72b3f930",
  "d83ba0de87cde7f87b5c82cc9f1b9a9cf559a3db15f82a79c9dbbad171dc4807",
  "ca69b94c88ab8f087e90e39f10092a87aabbae5e9d6e3f14cac71c9677fd106c",
  "968fff3649c5eb2f07f46abfe3ef81b3ccd800655cea6f658c423f690d2428b1",
  "849931e46b4ba381956be6d4b66b542f61227619e5db966d5b6f1a6350fe8753",
  "b76e205bef96d03b903a5dbf92630a645326d0879830cdfdb834291c4fc51be0",
  "07c7ae45df602394069a1abd431dfbe1b7078207ff63f6a530c996fa5334b146",
  "ed589acd479f3b1856fe2cce54c4f836d9626dc098fcfd8bdf5044d85a0e0e15",
  "a35e0607da9fefdc864efc1f7bbf8cc5bd0fae52057feee3a08f52a490e3892c",
}};

void HashAndCompare(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& p0){
  std::array<dt::TimestampOwnedValueV3,21> values{};
  for(std::size_t i=0;i<kValueHashes.size();++i)values[i]={p0,dt::TimestampValueStateV3::value,kValueHashes[i].day,kValueHashes[i].nanos};
  values.back()={p0,dt::TimestampValueStateV3::sql_null,0,0};
  for(std::size_t i=0;i<kValueHashes.size();++i){auto hash=dt::HashTimestampValueV3(values[i]);Check(hash.ok()&&hash.bytes.size()==32&&std::equal(hash.bytes.begin(),hash.bytes.end(),Hex32(p0->receipt.catalog_generation==11?kD711ValueHashes[i]:kValueHashes[i].hash).begin()),"exact SBTSPH01 PRESENT hash");}
  auto null_hash=dt::HashTimestampValueV3(values.back());Check(null_hash.ok()&&std::equal(null_hash.bytes.begin(),null_hash.bytes.end(),Hex32(p0->receipt.catalog_generation==11?"9c216850057be2015961474a90f3045fe868d726322f9c9f1d88890fd97382cb":kNullHash).begin()),"exact SBTSPH01 NULL hash");
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

void Casts(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& p0, bool successor_peer=false){
  auto* character_identity=Identity(dt::CanonicalTypeId::character,successor_peer);Check(character_identity!=nullptr,"character identity");auto character_descriptor=Descriptor(dt::CanonicalTypeId::character);auto timestamp_descriptor=Descriptor(dt::CanonicalTypeId::timestamp);
  dt::DatatypeOperationValue text{dt::CanonicalTypeId::character,"2024-02-29T06:07:08.9",false,character_descriptor},null_value{dt::CanonicalTypeId::null_type,"",true,{}};dt::TimestampOwnedValueV3 value{p0,dt::TimestampValueStateV3::value,19782,22'028'900'000'000ull};
  unsigned decisions=0,admitted=0;
  dt::DatatypeOperationValue scalar;
  for(unsigned row=1;row<=221;++row){const auto shape=Shape(row);for(auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,dt::DatatypeCastContext::explicit_cast}){auto want=dt::TimestampCastPolicyDispositionV3::forbidden;if(row==1)want=dt::TimestampCastPolicyDispositionV3::contextual_null;else if(row==24&&context==dt::DatatypeCastContext::explicit_cast)want=dt::TimestampCastPolicyDispositionV3::explicit_character_to_timestamp;else if(row==50&&context==dt::DatatypeCastContext::explicit_cast)want=dt::TimestampCastPolicyDispositionV3::explicit_timestamp_to_character;else if(row==53)want=dt::TimestampCastPolicyDispositionV3::identity;Check(dt::ClassifyTimestampCastPolicyRowV3(row,context)==want,"221x3 cast classification");dt::TimestampCastRequestV3 q;q.one_based_policy_row=row;q.context=context;
    if(row==53){q.timestamp_source=&value;q.timestamp_target=&p0;}
    else if(shape.incoming){scalar={};scalar.type_id=shape.peer;scalar.is_null=shape.contextual;scalar.encoded_value=shape.contextual?"":(row==24?"2024-02-29T06:07:08.9":"poison-present-payload-must-not-be-read");if(shape.exact){q.scalar_source_identity=Identity(shape.peer,successor_peer);Check(q.scalar_source_identity!=nullptr,"exact incoming identity");scalar.descriptor=Descriptor(shape.peer);}q.scalar_source=&scalar;q.timestamp_target=&p0;q.timestamp_target_descriptor=&timestamp_descriptor;}
    else {q.timestamp_source=&value;q.scalar_target=shape.peer;if(shape.exact){q.scalar_target_identity=Identity(shape.peer,successor_peer);Check(q.scalar_target_identity!=nullptr,"exact outgoing identity");q.scalar_target_descriptor=Descriptor(shape.peer);}}
    auto result=dt::CastTimestampValueV3(q);if(want==dt::TimestampCastPolicyDispositionV3::forbidden)Check(!result.ok()&&result.diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN","221x3 endpoint-bound runtime forbidden cast");else{Check(result.ok(),"221x3 endpoint-bound runtime admitted cast");++admitted;}++decisions;}}
  Check(decisions==663&&admitted==8,"closed cast matrix");

  // Exact UUID/generation/profile/descriptor authority is resolved before
  // state and before the closed-policy refusal.
  auto* int32_identity=Identity(dt::CanonicalTypeId::int32,successor_peer);Check(int32_identity!=nullptr,"int32 identity");auto int32_descriptor=Descriptor(dt::CanonicalTypeId::int32);
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

  // timestamp(p) has no admitted d710 profile: a precision modifier cannot be
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
  for (bool null : {false, true}) {
    text.is_null = null;
    text.encoded_value = null ? "" : "2024-02-29T06:07:08.9";
    dt::TimestampOwnedValueV3 outgoing_source{p0,
        null ? dt::TimestampValueStateV3::sql_null : dt::TimestampValueStateV3::value, 0, 0};
    out.timestamp_source = &outgoing_source;
    for (unsigned field = 0; field < 8; ++field) {
      auto altered = *character_identity;
      auto& native = altered.native_fields;
      switch (field) {
        case 0: native.present = !native.present; break;
        case 1: ++native.canonical_value_minimum_bytes; break;
        case 2: ++native.canonical_value_maximum_bytes; break;
        case 3: ++native.canonical_value_transport_width; break;
        case 4: native.canonical_value_variable_width = !native.canonical_value_variable_width; break;
        case 5: native.policy_profile_uuid.bytes[0] ^= 1; break;
        case 6: ++native.policy_profile_generation; break;
        case 7: native.profile_fingerprint_sha256[0] ^= 1; break;
      }
      in.scalar_source_identity = &altered;
      out.scalar_target_identity = &altered;
      Check(dt::CastTimestampValueV3(in).diagnostic.diagnostic_code == "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
            dt::CastTimestampValueV3(out).diagnostic.diagnostic_code == "CTI.TEMPORAL.DESCRIPTOR_INVALID",
            "mutated peer native fields refuse before state/resource/output handling");
    }
    auto other = p0->receipt.catalog_generation == 10 ?
        scratchbird::tests::D711TimestampProfile() : Profile();
    dt::TimestampOwnedValueV3 source{p0, null ? dt::TimestampValueStateV3::sql_null : dt::TimestampValueStateV3::value, 0, 0};
    dt::TimestampCastRequestV3 identity_cast;
    identity_cast.one_based_policy_row = 53;
    identity_cast.timestamp_source = &source;
    identity_cast.timestamp_target = &other;
    Check(!dt::CastTimestampValueV3(identity_cast).ok(), "cross-cohort identity cast refuses for equal components and NULL");
  }
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

void PropertyAndMutationFuzz(
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  p::u64 state = 0xd709'01a1'04ec'8e3cULL;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  dt::TimestampOwnedValueV3 previous{
      profile, dt::TimestampValueStateV3::value, 0, 0};
  for (unsigned iteration = 0; iteration != 2048; ++iteration) {
    const p::u32 day_bits = static_cast<p::u32>(next());
    std::int32_t day = 0;
    std::memcpy(&day, &day_bits, sizeof(day));
    const p::u64 nanoseconds = next() % 86'400'000'000'000ULL;
    dt::TimestampOwnedValueV3 value{
        profile, dt::TimestampValueStateV3::value, day, nanoseconds};

    const auto component = dt::EncodeCanonicalTimestampComponentV3(value);
    const auto decoded = dt::DecodeCanonicalTimestampComponentNoAllocV3(
        *profile, dt::TimestampValueStateV3::value, true, component.bytes);
    Check(component.ok() && component.bytes.size() == 16 && decoded.ok() &&
              decoded.value.civil_day == day &&
              decoded.value.nanoseconds_since_midnight == nanoseconds,
          "property fuzz canonical component roundtrip");
    const auto reencoded = dt::EncodeCanonicalTimestampComponentV3(
        {profile, decoded.value.state, decoded.value.civil_day,
         decoded.value.nanoseconds_since_midnight});
    Check(reencoded.ok() && reencoded.bytes == component.bytes,
          "property fuzz canonical component byte stability");

    const auto ascending = dt::MakeTimestampSortKeyV3(
        value, dt::TimestampSortDirectionV3::ascending,
        dt::TimestampNullModeV3::nulls_last);
    const auto descending = dt::MakeTimestampSortKeyV3(
        value, dt::TimestampSortDirectionV3::descending,
        dt::TimestampNullModeV3::nulls_last);
    const auto ascending_decoded = dt::DecodeTimestampSortKeyNoAllocV3(
        *profile, ascending.bytes);
    const auto descending_decoded = dt::DecodeTimestampSortKeyNoAllocV3(
        *profile, descending.bytes);
    Check(ascending.ok() && descending.ok() && ascending_decoded.ok() &&
              descending_decoded.ok() &&
              ascending_decoded.value.civil_day == day &&
              ascending_decoded.value.nanoseconds_since_midnight == nanoseconds &&
              descending_decoded.value.civil_day == day &&
              descending_decoded.value.nanoseconds_since_midnight == nanoseconds,
          "property fuzz ordered-key bidirectional roundtrip");

    const auto forward = dt::CompareTimestampValuesV3(previous.view(), value.view());
    const auto reverse = dt::CompareTimestampValuesV3(value.view(), previous.view());
    const auto expected = previous.civil_day < day ||
                                  (previous.civil_day == day &&
                                   previous.nanoseconds_since_midnight < nanoseconds)
                              ? dt::TimestampComparisonFactV3::less
                              : previous.civil_day == day &&
                                        previous.nanoseconds_since_midnight == nanoseconds
                                    ? dt::TimestampComparisonFactV3::equal
                                    : dt::TimestampComparisonFactV3::greater;
    const auto reverse_expected = expected == dt::TimestampComparisonFactV3::less
                                      ? dt::TimestampComparisonFactV3::greater
                                      : expected == dt::TimestampComparisonFactV3::greater
                                            ? dt::TimestampComparisonFactV3::less
                                            : dt::TimestampComparisonFactV3::equal;
    Check(forward.ok() && reverse.ok() && forward.fact == expected &&
              reverse.fact == reverse_expected,
          "property fuzz comparison antisymmetry");
    const auto hash_once = dt::HashTimestampValueV3(value);
    const auto hash_twice = dt::HashTimestampValueV3(value);
    Check(hash_once.ok() && hash_twice.ok() && hash_once.bytes == hash_twice.bytes,
          "property fuzz hash determinism");

    const auto envelope = dt::EncodeTimestampSbdvalComposedV3(value, true);
    Check(envelope.ok() && envelope.bytes.size() == 48,
          "mutation fuzz envelope baseline");
    auto corrupt = envelope.bytes;
    const std::size_t payload_offset = 32 + (next() % 16);
    corrupt[payload_offset] ^= static_cast<p::byte>(1u << (next() % 8));
    const auto refused = dt::DecodeTimestampSbdvalComposedNoAllocV3(
        *profile, true, corrupt);
    Check(!refused.ok() && refused.diagnostic.diagnostic_code ==
                               "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "mutation fuzz checksum-protected payload refuses");
    previous = std::move(value);
  }
}
}  // namespace
int main(){auto p0=Profile();HashAndCompare(p0);OrderedKeys(p0);Casts(p0);PropertyAndMutationFuzz(p0);auto successor=scratchbird::tests::D711TimestampProfile();HashAndCompare(successor);OrderedKeys(successor);Casts(successor);Casts(p0,true);Casts(successor,true);PropertyAndMutationFuzz(successor);std::cout<<"PASS base.timestamp V3 operations/casts checks="<<checks<<" hashes=21 keys=84 decisions=663 property_fuzz_iterations=2048\n";}
