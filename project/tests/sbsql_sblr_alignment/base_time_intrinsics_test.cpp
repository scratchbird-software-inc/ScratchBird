// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_time.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace dt=scratchbird::core::datatypes;namespace p=scratchbird::core::platform;
namespace{
void Check(bool v,std::string_view m){if(!v){std::cerr<<"FAIL "<<m<<'\n';std::exit(1);}}
p::Uuid D709(){return p::Uuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,9}};}
std::shared_ptr<const dt::TimeValidatedProfileHandleV3> Profile(){auto b=dt::BuildCurrentTimeValidatedProfileHandleV3(D709());Check(b.ok(),"profile");return std::make_shared<const dt::TimeValidatedProfileHandleV3>(b.profile);}
const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId type){for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(row.legacy_fields.catalog_snapshot_uuid==D709()&&row.legacy_fields.catalog_generation==9&&row.legacy_fields.registry_generation==9&&row.legacy_fields.canonical_binary_type_code==static_cast<p::u32>(type))return &row;return nullptr;}
scratchbird::engine::ExecutionTypeDescriptor Descriptor(dt::CanonicalTypeId type){auto manifest=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(manifest.ok(),"catalog");auto row=dt::LookupDatatypeCatalogRow(manifest.manifest,type);Check(row.ok()&&row.manifest.descriptor_rows.size()==1,"descriptor row");dt::CatalogExecutionTypeMetadata metadata;metadata.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;metadata.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;auto result=dt::LookupExecutionTypeDescriptorFromCatalog(type,metadata);Check(result.ok(),"execution descriptor");return result.descriptor;}
bool Stop(void*)noexcept{return true;}
struct Invalid{std::string_view text,code;};
struct ClassificationFixture{std::string_view case_id;dt::TimeIntrinsicOperationV3 operation;dt::TimeIntrinsicDispositionV3 disposition;};
constexpr std::array<ClassificationFixture,21> classifications{{
 {"validate_canonicalize.classification",dt::TimeIntrinsicOperationV3::validate_canonicalize,dt::TimeIntrinsicDispositionV3::admitted},
 {"civil_construct.classification",dt::TimeIntrinsicOperationV3::civil_construct,dt::TimeIntrinsicDispositionV3::admitted},
 {"decompose_civil.classification",dt::TimeIntrinsicOperationV3::decompose_civil,dt::TimeIntrinsicDispositionV3::admitted},
 {"parse_canonical.classification",dt::TimeIntrinsicOperationV3::parse_canonical,dt::TimeIntrinsicDispositionV3::admitted},
 {"render_canonical.classification",dt::TimeIntrinsicOperationV3::render_canonical,dt::TimeIntrinsicDispositionV3::admitted},
 {"add_nanoseconds.classification",dt::TimeIntrinsicOperationV3::add_nanoseconds,dt::TimeIntrinsicDispositionV3::admitted},
 {"subtract_nanoseconds.classification",dt::TimeIntrinsicOperationV3::subtract_nanoseconds,dt::TimeIntrinsicDispositionV3::admitted},
 {"successor.classification",dt::TimeIntrinsicOperationV3::successor,dt::TimeIntrinsicDispositionV3::admitted},
 {"predecessor.classification",dt::TimeIntrinsicOperationV3::predecessor,dt::TimeIntrinsicDispositionV3::admitted},
 {"difference_nanoseconds.classification",dt::TimeIntrinsicOperationV3::difference_nanoseconds,dt::TimeIntrinsicDispositionV3::admitted},
 {"extract_hour.classification",dt::TimeIntrinsicOperationV3::extract_hour,dt::TimeIntrinsicDispositionV3::admitted},
 {"extract_minute.classification",dt::TimeIntrinsicOperationV3::extract_minute,dt::TimeIntrinsicDispositionV3::admitted},
 {"extract_second.classification",dt::TimeIntrinsicOperationV3::extract_second,dt::TimeIntrinsicDispositionV3::admitted},
 {"extract_nanosecond.classification",dt::TimeIntrinsicOperationV3::extract_nanosecond,dt::TimeIntrinsicDispositionV3::admitted},
 {"nanosecond_of_day.classification",dt::TimeIntrinsicOperationV3::nanosecond_of_day,dt::TimeIntrinsicDispositionV3::admitted},
 {"truncate_nanosecond.classification",dt::TimeIntrinsicOperationV3::truncate_nanosecond,dt::TimeIntrinsicDispositionV3::admitted},
 {"round_nanosecond.classification",dt::TimeIntrinsicOperationV3::round_nanosecond,dt::TimeIntrinsicDispositionV3::admitted},
 {"larger_truncate_round_or_bucket.classification",dt::TimeIntrinsicOperationV3::larger_truncate_round_or_bucket,dt::TimeIntrinsicDispositionV3::registered_refused},
 {"duration_interval_or_modulo_day.classification",dt::TimeIntrinsicOperationV3::duration_interval_or_modulo_day,dt::TimeIntrinsicDispositionV3::registered_refused},
 {"date_timestamp_or_timezone_cross_temporal.classification",dt::TimeIntrinsicOperationV3::date_timestamp_or_timezone_cross_temporal,dt::TimeIntrinsicDispositionV3::registered_refused},
 {"aggregate_min_max_count_dispatch.classification",dt::TimeIntrinsicOperationV3::aggregate_min_max_count_dispatch,dt::TimeIntrinsicDispositionV3::receiving_owner}
}};
constexpr std::array<Invalid,25> invalid{{
 {"","CTI.TEMPORAL.INVALID_LITERAL"},{" 00:00:00","CTI.TEMPORAL.INVALID_LITERAL"},
 {"00:00:00 ","CTI.TEMPORAL.INVALID_LITERAL"},{"1:02:03","CTI.TEMPORAL.INVALID_LITERAL"},
 {"01:2:03","CTI.TEMPORAL.INVALID_LITERAL"},{"01:02:3","CTI.TEMPORAL.INVALID_LITERAL"},
 {"01-02-03","CTI.TEMPORAL.INVALID_LITERAL"},{"00:00:00,1","CTI.TEMPORAL.INVALID_LITERAL"},
 {"00:00:00.","CTI.TEMPORAL.INVALID_LITERAL"},{"12:34:56.100","CTI.TEMPORAL.INVALID_LITERAL"},
 {"12:34:56.000","CTI.TEMPORAL.INVALID_LITERAL"},{"24:00:00","CTI.TEMPORAL.INVALID_LITERAL"},
 {"99:00:00","CTI.TEMPORAL.INVALID_LITERAL"},{"00:60:00","CTI.TEMPORAL.INVALID_LITERAL"},
 {"23:59:60","CTI.TEMPORAL.LEAP_SECOND_REFUSED"},{"23:59:61","CTI.TEMPORAL.INVALID_LITERAL"},
 {"12:34:56.1234567891","CTI.TEMPORAL.PRECISION_LOSS"},{"00:00:00Z","CTI.TEMPORAL.INVALID_LITERAL"},
 {"00:00:00+00:00","CTI.TEMPORAL.INVALID_LITERAL"},{"1970-01-01 00:00:00","CTI.TEMPORAL.INVALID_LITERAL"},
 {"12:34:60.1234567890","CTI.TEMPORAL.LEAP_SECOND_REFUSED"},{"24:34:60.1234567890","CTI.TEMPORAL.INVALID_LITERAL"},
 {"12:60:60.1234567890","CTI.TEMPORAL.INVALID_LITERAL"},{"12:34:60.","CTI.TEMPORAL.LEAP_SECOND_REFUSED"},
 {"12:34:60.X","CTI.TEMPORAL.LEAP_SECOND_REFUSED"}
}};
std::string Hex(std::string_view h){std::string out;for(std::size_t i=0;i<h.size();i+=2){auto n=[](char c){return c<='9'?c-'0':c-'a'+10;};out.push_back(static_cast<char>((n(h[i])<<4)|n(h[i+1])));}return out;}
bool Unpublished(const dt::TimeValueResultV3&r){return !r.value.profile&&r.value.nanoseconds_since_midnight==0;}
bool Unpublished(const dt::TimeCivilResultV3&r){return !r.is_null&&r.civil.hour==0&&r.civil.minute==0&&r.civil.second==0&&r.civil.nanosecond==0;}
bool Unpublished(const dt::TimeTextResultV3&r){return r.text.empty()&&!r.containing_null;}
bool Unpublished(const dt::TimeScalarResultV3&r){return !r.is_null&&r.signed_value==0&&r.unsigned_value==0;}
bool Unpublished(const dt::TimeAggregateHandoffResultV3&r){return !r.operand.profile&&r.operand.nanoseconds_since_midnight==0;}
}

int main(){
 auto profile=Profile();unsigned admitted=0,refused=0,owner=0,executed_vectors=0;
 auto Seal=[&](std::string_view case_id,bool accepted){Check(accepted,case_id);++executed_vectors;};
 Check(dt::ClassifyTimeIntrinsicOperationV3(static_cast<dt::TimeIntrinsicOperationV3>(99))==dt::TimeIntrinsicDispositionV3::unknown,"unknown operation");

 auto x=dt::ConstructTimeFromCivilV3(profile,12,34,56,100'000'000);Check(x.ok()&&x.value.nanoseconds_since_midnight==45'296'100'000'000ull,"construct");
 auto civil=dt::DecomposeTimeCivilV3(x.value.view());Check(civil.ok()&&civil.civil.hour==12&&civil.civil.minute==34&&civil.civil.second==56&&civil.civil.nanosecond==100'000'000,"decompose");
 Check(dt::ConstructTimeFromCivilV3(profile,23,59,59,999'999'999).value.nanoseconds_since_midnight==dt::kTimeMaximumNanosecondsV3,"construct max");
 Check(dt::ConstructTimeFromCivilV3(profile,24,59,60,1'000'000'000).diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","hour precedence");
 Check(dt::ConstructTimeFromCivilV3(profile,23,60,60,1'000'000'000).diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","minute precedence");
 Check(dt::ConstructTimeFromCivilV3(profile,23,59,60,1'000'000'000).diagnostic.diagnostic_code=="CTI.TEMPORAL.LEAP_SECOND_REFUSED","leap precedence");
 Check(dt::ConstructTimeFromCivilV3(profile,23,59,59,1'000'000'000).diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","nanosecond invalid");

 auto rendered=dt::RenderCanonicalTimeV3(x.value);Check(rendered.ok()&&rendered.text=="12:34:56.1","minimal render");
 auto literal=dt::RenderCanonicalTimeV3(x.value,true);Check(literal.ok()&&literal.text=="TIME '12:34:56.1'","export literal");
 Check(dt::ParseCanonicalTimeV3(profile,rendered.text).value.nanoseconds_since_midnight==x.value.nanoseconds_since_midnight,"parse roundtrip");
 auto* character_identity=Identity(dt::CanonicalTypeId::character);Check(character_identity!=nullptr,"character identity");
 auto character_descriptor=Descriptor(dt::CanonicalTypeId::character);
 dt::TimeTextOperandV3 text_operand{character_identity,&character_descriptor,dt::TimeTextCarrierKindV3::utf8_bytes,dt::TimeValueStateV3::sql_null,{},0};
 Check(dt::ParseCanonicalTimeOperandV3(profile,text_operand).ok(),"authorized clean text NULL");
 auto wrong_character=*character_identity;wrong_character.legacy_fields.type_uuid.bytes[0]^=1;text_operand.identity=&wrong_character;
 Check(dt::ParseCanonicalTimeOperandV3(profile,text_operand).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","text authority precedes clean NULL");
 text_operand.bytes="poison";text_operand.extent=6;Check(dt::ParseCanonicalTimeOperandV3(profile,text_operand).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","text authority precedes dirty NULL and poison");
 text_operand.identity=character_identity;auto wrong_descriptor=character_descriptor;wrong_descriptor.descriptor_uuid.bytes[0]^=1;text_operand.descriptor=&wrong_descriptor;
 Check(dt::ParseCanonicalTimeOperandV3(profile,text_operand).diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID","text descriptor precedes dirty NULL");
 unsigned invalid_executed=0;for(const auto& v:invalid){auto r=dt::ParseCanonicalTimeV3(profile,v.text);Check(!r.ok()&&r.diagnostic.diagnostic_code==v.code,v.text);++invalid_executed;}
 for(auto hex:{"d9a0d9a03ad9a0d9a03ad9a0d9a0","e2889230303a30303a3030","30303a30303a303000","30303a30303a30300a","30303a30303a30300d0a","30303a30303a3030ff","30303a30303a3030c0af","efbc90efbc903aefbc90efbc903aefbc90efbc90","30303a30303a093030"}){auto bytes=Hex(hex);auto r=dt::ParseCanonicalTimeV3(profile,std::string_view(bytes.data(),bytes.size()));Check(!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL","invalid bytes");++invalid_executed;}

 dt::TimeOwnedValueV3 zero{profile,dt::TimeValueStateV3::value,0},maximum{profile,dt::TimeValueStateV3::value,dt::kTimeMaximumNanosecondsV3},middle{profile,dt::TimeValueStateV3::value,43'200'000'000'000ull},nullv{profile,dt::TimeValueStateV3::sql_null,0};
 for(const auto& fixture:classifications){
  const auto disposition=dt::ClassifyTimeIntrinsicOperationV3(fixture.operation);bool exact=false;
  switch(fixture.operation){
   case dt::TimeIntrinsicOperationV3::validate_canonicalize:{dt::TimeOwnedValueV3 input{profile,dt::TimeValueStateV3::value,dt::kTimeMaximumNanosecondsV3+1};const auto r=dt::ValidateCanonicalTimeV3(input);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&!r.value.profile&&r.value.nanoseconds_since_midnight==0;break;}
   case dt::TimeIntrinsicOperationV3::civil_construct:{const auto r=dt::ConstructTimeFromCivilV3(profile,24,0,0,0);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&!r.value.profile&&r.value.nanoseconds_since_midnight==0;break;}
   case dt::TimeIntrinsicOperationV3::decompose_civil:{const auto r=dt::DecomposeTimeCivilV3({profile.get(),dt::TimeValueStateV3::value,dt::kTimeMaximumNanosecondsV3+1});exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&!r.is_null&&r.civil.hour==0&&r.civil.minute==0&&r.civil.second==0&&r.civil.nanosecond==0;break;}
   case dt::TimeIntrinsicOperationV3::parse_canonical:{const dt::TimeTextOperandV3 input{character_identity,&character_descriptor,dt::TimeTextCarrierKindV3::utf8_bytes,dt::TimeValueStateV3::value,"x",1};const auto r=dt::ParseCanonicalTimeOperandV3(profile,input);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&!r.value.profile&&r.value.nanoseconds_since_midnight==0;break;}
   case dt::TimeIntrinsicOperationV3::render_canonical:{dt::TimeOwnedValueV3 input{profile,dt::TimeValueStateV3::value,dt::kTimeMaximumNanosecondsV3+1};const auto r=dt::RenderCanonicalTimeV3(input);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"&&r.text.empty();break;}
   case dt::TimeIntrinsicOperationV3::add_nanoseconds:{const auto r=dt::AddTimeNanosecondsV3(maximum,1);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED"&&!r.value.profile;break;}
   case dt::TimeIntrinsicOperationV3::subtract_nanoseconds:{const auto r=dt::SubtractTimeNanosecondsV3(zero,1);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED"&&!r.value.profile;break;}
   case dt::TimeIntrinsicOperationV3::successor:{const auto r=dt::TimeSuccessorV3(maximum);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED"&&!r.value.profile;break;}
   case dt::TimeIntrinsicOperationV3::predecessor:{const auto r=dt::TimePredecessorV3(zero);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED"&&!r.value.profile;break;}
   case dt::TimeIntrinsicOperationV3::difference_nanoseconds:{const auto r=dt::DifferenceTimeNanosecondsV3(maximum.view(),zero.view());exact=r.ok()&&!r.is_null&&r.signed_value==86'399'999'999'999ll;break;}
   case dt::TimeIntrinsicOperationV3::extract_hour:{const auto r=dt::ExtractTimeHourV3(x.value.view());exact=r.ok()&&!r.is_null&&r.unsigned_value==12;break;}
   case dt::TimeIntrinsicOperationV3::extract_minute:{const auto r=dt::ExtractTimeMinuteV3(x.value.view());exact=r.ok()&&!r.is_null&&r.unsigned_value==34;break;}
   case dt::TimeIntrinsicOperationV3::extract_second:{const auto r=dt::ExtractTimeSecondV3(x.value.view());exact=r.ok()&&!r.is_null&&r.unsigned_value==56;break;}
   case dt::TimeIntrinsicOperationV3::extract_nanosecond:{const auto r=dt::ExtractTimeNanosecondV3(x.value.view());exact=r.ok()&&!r.is_null&&r.unsigned_value==100'000'000;break;}
   case dt::TimeIntrinsicOperationV3::nanosecond_of_day:{const auto r=dt::TimeNanosecondOfDayV3(x.value.view());exact=r.ok()&&!r.is_null&&r.unsigned_value==45'296'100'000'000ull;break;}
   case dt::TimeIntrinsicOperationV3::truncate_nanosecond:{const auto r=dt::TruncateTimeNanosecondV3(x.value);exact=r.ok()&&r.value.nanoseconds_since_midnight==x.value.nanoseconds_since_midnight;break;}
   case dt::TimeIntrinsicOperationV3::round_nanosecond:{const auto r=dt::RoundTimeNanosecondV3(x.value);exact=r.ok()&&r.value.nanoseconds_since_midnight==x.value.nanoseconds_since_midnight;break;}
   case dt::TimeIntrinsicOperationV3::larger_truncate_round_or_bucket:
   case dt::TimeIntrinsicOperationV3::duration_interval_or_modulo_day:
   case dt::TimeIntrinsicOperationV3::date_timestamp_or_timezone_cross_temporal:{const auto r=dt::RefuseTimeIntrinsicOperationV3(middle.view(),fixture.operation);exact=!r.ok()&&r.diagnostic.diagnostic_code=="CTI.INTERVAL.CALENDAR_OPERATION_REFUSED"&&!r.value.profile;break;}
   case dt::TimeIntrinsicOperationV3::aggregate_min_max_count_dispatch:{const auto r=dt::ValidateTimeAggregateHandoffV3(middle);exact=r.ok()&&r.operand.nanoseconds_since_midnight==middle.nanoseconds_since_midnight;break;}
  }
  Seal(fixture.case_id,disposition==fixture.disposition&&exact);admitted+=disposition==dt::TimeIntrinsicDispositionV3::admitted;refused+=disposition==dt::TimeIntrinsicDispositionV3::registered_refused;owner+=disposition==dt::TimeIntrinsicDispositionV3::receiving_owner;
 }
 Check(admitted==17&&refused==3&&owner==1,"21 named operation classification executions");
 Check(dt::TimeSuccessorV3(zero).value.nanoseconds_since_midnight==1,"successor");
 Check(!dt::TimeSuccessorV3(maximum).ok(),"successor max refuses");
 Check(!dt::TimePredecessorV3(zero).ok(),"predecessor zero refuses");
 Check(dt::TimePredecessorV3(maximum).value.nanoseconds_since_midnight==dt::kTimeMaximumNanosecondsV3-1,"predecessor");
 Check(dt::AddTimeNanosecondsV3(middle,1).value.nanoseconds_since_midnight==43'200'000'000'001ull,"add");
 Check(dt::SubtractTimeNanosecondsV3(middle,1).value.nanoseconds_since_midnight==43'199'999'999'999ull,"subtract");
 Check(!dt::AddTimeNanosecondsV3(zero,std::numeric_limits<std::int64_t>::min()).ok(),"add INT64_MIN");
 Check(!dt::SubtractTimeNanosecondsV3(zero,std::numeric_limits<std::int64_t>::min()).ok(),"subtract INT64_MIN");
 Check(dt::DifferenceTimeNanosecondsV3(maximum.view(),zero.view()).signed_value==static_cast<std::int64_t>(dt::kTimeMaximumNanosecondsV3),"difference positive");
 Check(dt::DifferenceTimeNanosecondsV3(zero.view(),maximum.view()).signed_value==-static_cast<std::int64_t>(dt::kTimeMaximumNanosecondsV3),"difference negative");
 Check(dt::ExtractTimeHourV3(x.value.view()).unsigned_value==12&&dt::ExtractTimeMinuteV3(x.value.view()).unsigned_value==34&&dt::ExtractTimeSecondV3(x.value.view()).unsigned_value==56&&dt::ExtractTimeNanosecondV3(x.value.view()).unsigned_value==100'000'000,"extract components");
 Check(dt::TimeNanosecondOfDayV3(x.value.view()).unsigned_value==x.value.nanoseconds_since_midnight,"nanosecond of day");
 Check(dt::TruncateTimeNanosecondV3(x.value).value.nanoseconds_since_midnight==x.value.nanoseconds_since_midnight&&dt::RoundTimeNanosecondV3(x.value).value.nanoseconds_since_midnight==x.value.nanoseconds_since_midnight,"identity precision");

 dt::TimeNullableUnsignedFactV3 clean_null{dt::TimeUnsignedCarrierKindV3::unsigned_u64,dt::TimeValueStateV3::sql_null,0},poison{dt::TimeUnsignedCarrierKindV3::unsigned_u64,dt::TimeValueStateV3::value,std::numeric_limits<p::u64>::max()};
 Check(dt::ConstructTimeFromCivilV3(profile,clean_null,poison,poison,poison).value.state==dt::TimeValueStateV3::sql_null,"civil NULL propagation");
 auto dirty=clean_null;dirty.value=1;Check(!dt::ConstructTimeFromCivilV3(profile,dirty,poison,poison,poison).ok(),"civil dirty NULL");
 auto malformed_unsigned=clean_null;malformed_unsigned.state=dt::TimeValueStateV3::value;malformed_unsigned.carrier=dt::TimeUnsignedCarrierKindV3::wrong_host_type;
 Check(dt::ConstructTimeFromCivilV3(profile,malformed_unsigned,poison,poison,poison).diagnostic.diagnostic_code=="SBLR.OPERAND_INVALID","unsigned carrier rejected");
 auto malformed_delta=dt::TimeNullableI64FactV3{dt::TimeI64CarrierKindV3::wrong_host_type,dt::TimeValueStateV3::value,1};
 Check(dt::AddTimeNanosecondsV3(x.value,malformed_delta).diagnostic.diagnostic_code=="SBLR.OPERAND_INVALID","signed add carrier rejected");
 Check(dt::SubtractTimeNanosecondsV3(x.value,malformed_delta).diagnostic.diagnostic_code=="SBLR.OPERAND_INVALID","signed subtract carrier rejected");
 text_operand={character_identity,&character_descriptor,dt::TimeTextCarrierKindV3::wrong_host_type,dt::TimeValueStateV3::value,"00:00:00",8};
 Check(dt::ParseCanonicalTimeOperandV3(profile,text_operand).diagnostic.diagnostic_code=="SBLR.OPERAND_INVALID","text carrier rejected");
 dt::TimeOwnedValueV3 out_of_range{profile,dt::TimeValueStateV3::value,dt::kTimeMaximumNanosecondsV3+1};
 Check(dt::ValidateCanonicalTimeV3(out_of_range).diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","validate classification range behavior");
 auto wrong_mutable=std::make_shared<dt::TimeValidatedProfileHandleV3>(*profile);wrong_mutable->identity.operation_policy.uuid.bytes[0]^=1;std::shared_ptr<const dt::TimeValidatedProfileHandleV3> wrong=wrong_mutable;
 dt::TimeOwnedValueV3 wrong_owned{wrong,dt::TimeValueStateV3::value,0},dirty_owned{profile,dt::TimeValueStateV3::sql_null,1};
 unsigned registry_mutations=0;auto Expect=[&](std::string_view case_id,const auto& result,std::string_view code){Seal(case_id,!result.ok()&&result.diagnostic.diagnostic_code==code&&Unpublished(result));++registry_mutations;};
 Expect("validate_canonicalize.wrong_policy",dt::ValidateCanonicalTimeV3(wrong_owned),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("validate_canonicalize.dirty_null",dt::ValidateCanonicalTimeV3(dirty_owned),"DATATYPE.NULL_STATE.INVALID");
 Expect("civil_construct.wrong_policy",dt::ConstructTimeFromCivilV3(wrong,0,0,0,0),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("civil_construct.dirty_null",dt::ConstructTimeFromCivilV3(profile,dirty,poison,poison,poison),"DATATYPE.NULL_STATE.INVALID");
 Expect("decompose_civil.wrong_policy",dt::DecomposeTimeCivilV3(wrong_owned.view()),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("decompose_civil.dirty_null",dt::DecomposeTimeCivilV3(dirty_owned.view()),"DATATYPE.NULL_STATE.INVALID");
 text_operand={character_identity,&character_descriptor,dt::TimeTextCarrierKindV3::utf8_bytes,dt::TimeValueStateV3::value,"00:00:00",8};Expect("parse_canonical.wrong_policy",dt::ParseCanonicalTimeOperandV3(wrong,text_operand),"CTI.TEMPORAL.DESCRIPTOR_INVALID");text_operand.state=dt::TimeValueStateV3::sql_null;text_operand.bytes="x";text_operand.extent=1;Expect("parse_canonical.dirty_null",dt::ParseCanonicalTimeOperandV3(profile,text_operand),"DATATYPE.NULL_STATE.INVALID");
 Expect("render_canonical.wrong_policy",dt::RenderCanonicalTimeV3(wrong_owned),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("render_canonical.dirty_null",dt::RenderCanonicalTimeV3(dirty_owned),"DATATYPE.NULL_STATE.INVALID");
 Expect("add_nanoseconds.wrong_policy",dt::AddTimeNanosecondsV3(wrong_owned,1),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("add_nanoseconds.dirty_null",dt::AddTimeNanosecondsV3(dirty_owned,1),"DATATYPE.NULL_STATE.INVALID");
 Expect("subtract_nanoseconds.wrong_policy",dt::SubtractTimeNanosecondsV3(wrong_owned,1),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("subtract_nanoseconds.dirty_null",dt::SubtractTimeNanosecondsV3(dirty_owned,1),"DATATYPE.NULL_STATE.INVALID");
 Expect("successor.wrong_policy",dt::TimeSuccessorV3(wrong_owned),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("successor.dirty_null",dt::TimeSuccessorV3(dirty_owned),"DATATYPE.NULL_STATE.INVALID");
 Expect("predecessor.wrong_policy",dt::TimePredecessorV3(wrong_owned),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("predecessor.dirty_null",dt::TimePredecessorV3(dirty_owned),"DATATYPE.NULL_STATE.INVALID");
 Expect("difference_nanoseconds.wrong_policy",dt::DifferenceTimeNanosecondsV3(wrong_owned.view(),zero.view()),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("difference_nanoseconds.dirty_null",dt::DifferenceTimeNanosecondsV3(dirty_owned.view(),zero.view()),"DATATYPE.NULL_STATE.INVALID");
 Expect("extract_hour.wrong_policy",dt::ExtractTimeHourV3(wrong_owned.view()),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("extract_hour.dirty_null",dt::ExtractTimeHourV3(dirty_owned.view()),"DATATYPE.NULL_STATE.INVALID");
 Expect("extract_minute.wrong_policy",dt::ExtractTimeMinuteV3(wrong_owned.view()),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("extract_minute.dirty_null",dt::ExtractTimeMinuteV3(dirty_owned.view()),"DATATYPE.NULL_STATE.INVALID");
 Expect("extract_second.wrong_policy",dt::ExtractTimeSecondV3(wrong_owned.view()),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("extract_second.dirty_null",dt::ExtractTimeSecondV3(dirty_owned.view()),"DATATYPE.NULL_STATE.INVALID");
 Expect("extract_nanosecond.wrong_policy",dt::ExtractTimeNanosecondV3(wrong_owned.view()),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("extract_nanosecond.dirty_null",dt::ExtractTimeNanosecondV3(dirty_owned.view()),"DATATYPE.NULL_STATE.INVALID");
 Expect("nanosecond_of_day.wrong_policy",dt::TimeNanosecondOfDayV3(wrong_owned.view()),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("nanosecond_of_day.dirty_null",dt::TimeNanosecondOfDayV3(dirty_owned.view()),"DATATYPE.NULL_STATE.INVALID");
 Expect("truncate_nanosecond.wrong_policy",dt::TruncateTimeNanosecondV3(wrong_owned),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("truncate_nanosecond.dirty_null",dt::TruncateTimeNanosecondV3(dirty_owned),"DATATYPE.NULL_STATE.INVALID");
 Expect("round_nanosecond.wrong_policy",dt::RoundTimeNanosecondV3(wrong_owned),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect("round_nanosecond.dirty_null",dt::RoundTimeNanosecondV3(dirty_owned),"DATATYPE.NULL_STATE.INVALID");
 Check(registry_mutations==34,"all 17 admitted wrong-policy and dirty-NULL rows");
 Check(dt::AddTimeNanosecondsV3(nullv,1).value.state==dt::TimeValueStateV3::sql_null,"arithmetic NULL");
 Check(dt::DifferenceTimeNanosecondsV3(nullv.view(),maximum.view()).is_null,"difference NULL");
 Check(dt::DecomposeTimeCivilV3(nullv.view()).is_null,"decompose NULL");
 Check(dt::RenderCanonicalTimeV3(nullv).containing_null,"render NULL");
 Check(dt::ValidateTimeAggregateHandoffV3(middle).ok(),"aggregate PRESENT handoff");
 Check(dt::ValidateTimeAggregateHandoffV3(nullv).ok(),"aggregate clean NULL handoff");
 Expect("aggregate_min_max_count_dispatch.dirty_null",dt::ValidateTimeAggregateHandoffV3(dirty_owned),"DATATYPE.NULL_STATE.INVALID");
 Expect("aggregate_min_max_count_dispatch.wrong_policy",dt::ValidateTimeAggregateHandoffV3(wrong_owned),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
 Check(!dt::ValidateTimeAggregateHandoffV3(middle,true,{~p::u64{0},Stop,nullptr}).ok(),"aggregate cancellation");
 constexpr std::array<std::pair<dt::TimeIntrinsicOperationV3,std::string_view>,3> refused_cases{{
   {dt::TimeIntrinsicOperationV3::larger_truncate_round_or_bucket,"larger_truncate_round_or_bucket"},
   {dt::TimeIntrinsicOperationV3::duration_interval_or_modulo_day,"duration_interval_or_modulo_day"},
   {dt::TimeIntrinsicOperationV3::date_timestamp_or_timezone_cross_temporal,"date_timestamp_or_timezone_cross_temporal"}}};
 for(const auto& [op,name]:refused_cases){Check(dt::RefuseTimeIntrinsicOperationV3(nullv.view(),op).diagnostic.diagnostic_code=="CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","registered refusal remains refusal for NULL");Expect(std::string(name)+".wrong_policy",dt::RefuseTimeIntrinsicOperationV3(wrong_owned.view(),op),"CTI.TEMPORAL.DESCRIPTOR_INVALID");Expect(std::string(name)+".dirty_null",dt::RefuseTimeIntrinsicOperationV3(dirty_owned.view(),op),"DATATYPE.NULL_STATE.INVALID");}
 const dt::TimeExecutionControlV3 stop{~p::u64{0},Stop,nullptr};
 Check(!dt::ValidateCanonicalTimeV3(middle,true,stop).ok(),"validate cancellation");
 Check(!dt::ConstructTimeFromCivilV3(profile,12,34,56,0,true,stop).ok(),"construct cancellation");
 Check(!dt::ConstructTimeFromCivilV3(profile,clean_null,poison,poison,poison,true,stop).ok(),"construct NULL cancellation");
 Check(!dt::DecomposeTimeCivilV3(middle.view(),true,stop).ok(),"decompose cancellation");
 Check(!dt::ParseCanonicalTimeV3(profile,"12:34:56",true,stop).ok(),"parse cancellation");
 Check(!dt::RenderCanonicalTimeV3(middle,false,stop).ok(),"render cancellation");
 Check(!dt::AddTimeNanosecondsV3(middle,1,true,stop).ok(),"add cancellation");
 Check(!dt::AddTimeNanosecondsV3(nullv,1,true,stop).ok(),"add NULL cancellation");
 Check(!dt::SubtractTimeNanosecondsV3(middle,1,true,stop).ok(),"subtract cancellation");
 Check(!dt::SubtractTimeNanosecondsV3(nullv,1,true,stop).ok(),"subtract NULL cancellation");
 Check(!dt::TimeSuccessorV3(middle,true,stop).ok(),"successor cancellation");
 Check(!dt::TimePredecessorV3(middle,true,stop).ok(),"predecessor cancellation");
 Check(!dt::DifferenceTimeNanosecondsV3(middle.view(),zero.view(),true,stop).ok(),"difference cancellation");
 Check(!dt::ExtractTimeHourV3(middle.view(),true,stop).ok(),"hour cancellation");
 Check(!dt::ExtractTimeMinuteV3(middle.view(),true,stop).ok(),"minute cancellation");
 Check(!dt::ExtractTimeSecondV3(middle.view(),true,stop).ok(),"second cancellation");
 Check(!dt::ExtractTimeNanosecondV3(middle.view(),true,stop).ok(),"nanosecond cancellation");
 Check(!dt::TimeNanosecondOfDayV3(middle.view(),true,stop).ok(),"nanosecond-of-day cancellation");
 Check(!dt::TruncateTimeNanosecondV3(middle,true,stop).ok(),"truncate cancellation");
 Check(!dt::RoundTimeNanosecondV3(middle,true,stop).ok(),"round cancellation");
 Check(!dt::ValidateCanonicalTimeV3(nullv,true,stop).ok(),"validate clean NULL cancellation");
 Check(!dt::DecomposeTimeCivilV3(nullv.view(),true,stop).ok(),"decompose clean NULL cancellation");
 text_operand={character_identity,&character_descriptor,dt::TimeTextCarrierKindV3::utf8_bytes,dt::TimeValueStateV3::sql_null,{},0};Check(!dt::ParseCanonicalTimeOperandV3(profile,text_operand,true,stop).ok(),"parse clean NULL cancellation");
 Check(!dt::RenderCanonicalTimeV3(nullv,false,stop).ok(),"render clean NULL cancellation");
 Check(!dt::TimeSuccessorV3(nullv,true,stop).ok(),"successor clean NULL cancellation");
 Check(!dt::TimePredecessorV3(nullv,true,stop).ok(),"predecessor clean NULL cancellation");
 Check(!dt::DifferenceTimeNanosecondsV3(nullv.view(),middle.view(),true,stop).ok(),"difference clean NULL cancellation");
 Check(!dt::ExtractTimeHourV3(nullv.view(),true,stop).ok()&&!dt::ExtractTimeMinuteV3(nullv.view(),true,stop).ok()&&!dt::ExtractTimeSecondV3(nullv.view(),true,stop).ok()&&!dt::ExtractTimeNanosecondV3(nullv.view(),true,stop).ok()&&!dt::TimeNanosecondOfDayV3(nullv.view(),true,stop).ok(),"all extract clean NULL cancellation");
 Check(!dt::TruncateTimeNanosecondV3(nullv,true,stop).ok()&&!dt::RoundTimeNanosecondV3(nullv,true,stop).ok(),"identity precision clean NULL cancellation");
 Seal("construct_maximum",dt::ConstructTimeFromCivilV3(profile,23,59,59,999'999'999).value.nanoseconds_since_midnight==dt::kTimeMaximumNanosecondsV3);
 Seal("construct_hour_24",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,24,0,0,0);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&Unpublished(r);}());
 Seal("construct_second_60",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,12,34,60,0);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.LEAP_SECOND_REFUSED"&&Unpublished(r);}());
 Seal("construct_hour_precedes_leap",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,24,34,60,0);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&Unpublished(r);}());
 Seal("construct_minute_precedes_leap",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,12,60,60,0);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&Unpublished(r);}());
 Seal("construct_hour_precedes",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,24,60,60,1'000'000'000);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&Unpublished(r);}());
 Seal("construct_minute_precedes",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,23,60,60,1'000'000'000);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&Unpublished(r);}());
 Seal("construct_leap_precedes_nanosecond",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,23,59,60,1'000'000'000);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.LEAP_SECOND_REFUSED"&&Unpublished(r);}());
 Seal("construct_nanosecond_invalid",[&]{const auto r=dt::ConstructTimeFromCivilV3(profile,23,59,59,1'000'000'000);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.INVALID_LITERAL"&&Unpublished(r);}());
 Seal("add_no_wrap_over",[&]{const auto r=dt::AddTimeNanosecondsV3(maximum,1);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED"&&Unpublished(r);}());
 Seal("subtract_no_wrap_under",[&]{const auto r=dt::SubtractTimeNanosecondsV3(zero,1);return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED"&&Unpublished(r);}());
 Seal("subtract_int64_min",[&]{const auto r=dt::SubtractTimeNanosecondsV3(zero,std::numeric_limits<std::int64_t>::min());return r.diagnostic.diagnostic_code=="CTI.TEMPORAL.RANGE_EXCEEDED"&&Unpublished(r);}());
 Seal("difference_extremes",dt::DifferenceTimeNanosecondsV3(maximum.view(),zero.view()).signed_value==86'399'999'999'999ll);
 Seal("difference_reverse_extremes",dt::DifferenceTimeNanosecondsV3(zero.view(),maximum.view()).signed_value==-86'399'999'999'999ll);
 Check(executed_vectors==77,"exact Core intrinsic vector count");Check(invalid_executed==34,"exact 25 text plus 9 byte invalid vectors");
 std::cout<<"PASS base.time V3 intrinsics operations="<<(admitted+refused+owner)<<" vectors="<<executed_vectors<<" invalid="<<invalid_executed<<"\n";
}
