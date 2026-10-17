// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/key_value_api.cpp"
#include "../../src/engine/internal_api/dml/text_destination_validation.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
namespace {
namespace index=scratchbird::core::index;
namespace platform=scratchbird::core::platform;
class CountingBoundedMemory final:public std::pmr::memory_resource {
 public:
  explicit CountingBoundedMemory(std::size_t limit):limit_(limit){}
  ~CountingBoundedMemory(){Check(live_bytes==0&&allocations==deallocations);}
  std::size_t live_bytes=0,peak_bytes=0,allocations=0,deallocations=0;
 private:
  struct Block {void* pointer=nullptr;std::size_t bytes=0,alignment=0;};
  std::array<Block,64> blocks_{};
  std::size_t limit_;
  void* do_allocate(std::size_t bytes,std::size_t alignment)override {
    if(bytes>limit_-live_bytes)throw std::bad_alloc();
    auto slot=std::find_if(blocks_.begin(),blocks_.end(),[](const auto& b){return !b.pointer;});Check(slot!=blocks_.end());
    auto* p=std::pmr::new_delete_resource()->allocate(bytes,alignment);*slot={p,bytes,alignment};
    live_bytes+=bytes;peak_bytes=std::max(peak_bytes,live_bytes);++allocations;return p;
  }
  void do_deallocate(void* p,std::size_t bytes,std::size_t alignment)override {
    auto slot=std::find_if(blocks_.begin(),blocks_.end(),[&](const auto& b){return b.pointer==p;});
    Check(slot!=blocks_.end()&&slot->bytes==bytes&&slot->alignment==alignment&&bytes<=live_bytes);
    *slot={};live_bytes-=bytes;++deallocations;std::pmr::new_delete_resource()->deallocate(p,bytes,alignment);
  }
  bool do_is_equal(const std::pmr::memory_resource& other)const noexcept override{return this==&other;}
};
enum class ProviderFault {none,v4,bad_variant,wrong_kind,not_exact,not_visible,not_authorized};
struct ObservedBatchInput {
  platform::TypedUuid row{platform::UuidKind::row,scratchbird::tests::FixtureUuid(1112,12)};
  std::array<std::string_view,16> calls{};
  std::size_t call_count=0;
  ProviderFault fault=ProviderFault::none;
};
index::BoundedUniqueBatchValueV1 ObservedBatchRow(void* opaque,std::string_view key){
  auto& input=*static_cast<ObservedBatchInput*>(opaque);Check(input.call_count<input.calls.size());
  input.calls[input.call_count++]=key;if(key=="missing")return {};
  index::BoundedUniqueBatchValueV1 value{true,input.row,std::string_view("v\0data",6),true,true,true};
  if(key=="first")switch(input.fault){
    case ProviderFault::none:break;
    case ProviderFault::v4:value.row_uuid.value.bytes[6]=std::uint8_t((value.row_uuid.value.bytes[6]&15)|0x40);break;
    case ProviderFault::bad_variant:value.row_uuid.value.bytes[8]=0;break;
    case ProviderFault::wrong_kind:value.row_uuid.kind=platform::UuidKind::principal;break;
    case ProviderFault::not_exact:value.exact_match=false;break;
    case ProviderFault::not_visible:value.mga_visible=false;break;
    case ProviderFault::not_authorized:value.security_authorized=false;break;
  }
  return value;
}
void UniqueBatchControls(){
  // These are supplied observed-data profile controls of the Core primitive,
  // NOT a fabricated authorization issuer or evidence of persisted KV rows.
  index::CandidateSetAuthorityContext observed;
  const std::vector<std::string> keys{"second","","second","missing","","missing","first"};
  std::size_t required_capacity=0;
  {CountingBoundedMemory memory(65536);ObservedBatchInput input;
    {const auto result=index::RunBoundedUniqueBatchPointLookupV1(keys,observed,&memory,&input,ObservedBatchRow);
      Check(result.ok()&&result.unique_key_count==4&&result.rows.size()==5&&result.misses.size()==2);
      Check(input.call_count==4&&input.calls[0]=="second"&&input.calls[1].empty()&&input.calls[2]=="missing"&&input.calls[3]=="first");
      constexpr std::array<std::size_t,5> ordinals{0,1,2,4,6};
      constexpr std::array<std::uint64_t,5> duplicates{0,0,1,1,0};
      for(std::size_t n=0;n<ordinals.size();++n){const auto& row=result.rows[n];
        Check(row.encoded_key==keys[ordinals[n]]&&row.input_ordinal==ordinals[n]&&row.duplicate_ordinal==duplicates[n]&&
            row.duplicate_key==(duplicates[n]!=0)&&row.row_uuid.kind==platform::UuidKind::row&&row.row_uuid.value==input.row.value&&
            row.payload==std::string_view("v\0data",6)&&scratchbird::core::uuid::IsEngineIdentityUuid(row.row_uuid.value));}
      Check(result.misses[0].encoded_key=="missing"&&result.misses[0].input_ordinal==3&&
          result.misses[1].encoded_key=="missing"&&result.misses[1].input_ordinal==5);
      Check(memory.live_bytes>0&&memory.allocations>0);required_capacity=memory.peak_bytes;
    }
    Check(memory.live_bytes==0&&memory.allocations==memory.deallocations);
  }
  // Fail every byte capacity below the measured real peak, not an arbitrary
  // generous allocation allowance. Each unwinding path must release every
  // actual PMR allocation; the exact measured capacity must then succeed.
  Check(required_capacity>0);
  for(std::size_t capacity=0;capacity<=required_capacity;++capacity){CountingBoundedMemory memory(capacity);ObservedBatchInput input;bool exhausted=false;
    try{const auto result=index::RunBoundedUniqueBatchPointLookupV1(keys,observed,&memory,&input,ObservedBatchRow);
      Check(result.ok()&&capacity==required_capacity&&result.rows.size()==5&&result.misses.size()==2);
    }catch(const std::bad_alloc&){exhausted=true;}
    Check(exhausted==(capacity<required_capacity)&&!memory.live_bytes&&memory.allocations==memory.deallocations);
  }
  for(auto fault:{ProviderFault::v4,ProviderFault::bad_variant,ProviderFault::wrong_kind,
      ProviderFault::not_exact,ProviderFault::not_visible,ProviderFault::not_authorized}){
    CountingBoundedMemory memory(65536);ObservedBatchInput input;input.fault=fault;
    {const auto result=index::RunBoundedUniqueBatchPointLookupV1(keys,observed,&memory,&input,ObservedBatchRow);
      Check(!result.ok()&&result.rows.empty()&&result.misses.empty()&&input.call_count==4&&
          result.diagnostic.diagnostic_code=="SB_BATCH_POINT_LOOKUP.EXACT_RECHECK_REQUIRED");}
    Check(!memory.live_bytes&&memory.allocations==memory.deallocations);
  }
  const std::array<std::pair<bool index::CandidateSetAuthorityContext::*,std::string_view>,5> required{{
      {&index::CandidateSetAuthorityContext::engine_mga_authoritative,"SB_BATCH_POINT_LOOKUP.MGA_RECHECK_REQUIRED"},
      {&index::CandidateSetAuthorityContext::row_mga_recheck_required,"SB_BATCH_POINT_LOOKUP.MGA_RECHECK_REQUIRED"},
      {&index::CandidateSetAuthorityContext::security_context_bound,"SB_BATCH_POINT_LOOKUP.SECURITY_RECHECK_REQUIRED"},
      {&index::CandidateSetAuthorityContext::row_security_recheck_required,"SB_BATCH_POINT_LOOKUP.SECURITY_RECHECK_REQUIRED"},
      {&index::CandidateSetAuthorityContext::exact_recheck_available,"SB_BATCH_POINT_LOOKUP.EXACT_RECHECK_REQUIRED"}}};
  for(const auto& [field,code]:required){CountingBoundedMemory memory(65536);ObservedBatchInput input;auto missing=observed;missing.*field=false;
    const auto result=index::RunBoundedUniqueBatchPointLookupV1(keys,missing,&memory,&input,ObservedBatchRow);
    Check(!result.ok()&&result.rows.empty()&&result.misses.empty()&&!input.call_count&&!memory.allocations&&result.diagnostic.diagnostic_code==code);
  }
  for(auto forbidden:{&index::CandidateSetAuthorityContext::parser_or_reference_finality_or_visibility_authority,
      &index::CandidateSetAuthorityContext::client_finality_or_visibility_authority,
      &index::CandidateSetAuthorityContext::provider_finality_or_visibility_authority,
      &index::CandidateSetAuthorityContext::wal_recovery_or_finality_authority}){
    CountingBoundedMemory memory(65536);ObservedBatchInput input;auto unsafe=observed;unsafe.*forbidden=true;
    const auto result=index::RunBoundedUniqueBatchPointLookupV1(keys,unsafe,&memory,&input,ObservedBatchRow);
    Check(!result.ok()&&result.rows.empty()&&result.misses.empty()&&!input.call_count&&!memory.allocations&&
        result.diagnostic.diagnostic_code=="SB_BATCH_POINT_LOOKUP.UNSAFE_AUTHORITY");
  }
  for(bool null_memory:{false,true}){CountingBoundedMemory memory(65536);ObservedBatchInput input;
    const auto result=index::RunBoundedUniqueBatchPointLookupV1(keys,observed,null_memory?nullptr:&memory,&input,null_memory?ObservedBatchRow:nullptr);
    Check(!result.ok()&&result.rows.empty()&&result.misses.empty()&&!input.call_count&&!memory.allocations&&
        result.diagnostic.diagnostic_code=="SB_BATCH_POINT_LOOKUP.PROVIDER_REQUIRED");
  }
  {CountingBoundedMemory memory(0);ObservedBatchInput input;
    const auto empty=index::RunBoundedUniqueBatchPointLookupV1({},observed,&memory,&input,ObservedBatchRow);
    Check(empty.ok()&&empty.rows.empty()&&empty.misses.empty()&&!empty.unique_key_count&&!input.call_count&&!memory.allocations);
  }
}
}
void TextDestinationControls() {
  namespace dt = scratchbird::core::datatypes;
  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV3();
  const auto row = std::find_if(rows.begin(), rows.end(), [](const auto& item) {
    return item.legacy_fields.codec_id == "datatype.text.utf8.v1";
  });
  Check(row != rows.end());
  const auto& identity = row->legacy_fields;
  a::MgaRelationColumnStorageDescriptor column;
  column.column_uuid = scratchbird::tests::FixtureUuid(1112, 80);
  column.column_generation = 7;
  column.character_length = 4;
  column.max_inline_bytes = 1; // Deliberately below every non-empty boundary value.
  auto& descriptor = column.value_descriptor;
  descriptor.canonical_type_name = "text";
  descriptor.type_uuid = identity.type_uuid;
  descriptor.datatype_descriptor_uuid = identity.descriptor_uuid;
  descriptor.datatype_descriptor_generation = identity.descriptor_generation;
  descriptor.datatype_cohort.catalog_snapshot_uuid = identity.catalog_snapshot_uuid;
  descriptor.datatype_cohort.catalog_generation = identity.catalog_generation;
  descriptor.datatype_cohort.registry_generation = identity.registry_generation;
  const auto validate = [&](std::string_view bytes) {
    return a::dml::ValidateTextDestinationValue(column, a::EngineValueState::value, bytes);
  };
  for (std::string_view valid : {"", "1234", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9",
        "\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80"})
    Check(!validate(valid));
  Check(!validate(std::string_view("a\0bc",4)));
  for (const std::string invalid : {std::string("\xff"), std::string("\xc0\x80"),
        std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xf0\x9f")}) {
    const auto diagnostic = validate(invalid);
    Check(diagnostic && diagnostic->code == "CTB.TEXT.INVALID_ENCODING");
  }
  const auto too_long = validate("12345");
  Check(too_long && too_long->code == "CTB.TEXT.LENGTH_EXCEEDED" &&
      too_long->detail == "text_destination_character_limit_exceeded" &&
      too_long->identity_fields.front().second == column.column_uuid);
  column.character_length = 0;
  Check(!validate("12345"));
  const auto registry_limit = validate(std::string(identity.canonical_value_maximum_bytes + 1,'x'));
  Check(registry_limit && registry_limit->code == "CTB.TEXT.LENGTH_EXCEEDED" &&
      registry_limit->detail == "text_destination_registry_byte_limit_exceeded");
  descriptor.datatype_descriptor_generation++;
  const auto stale = validate("x");
  Check(stale && stale->code == "DATATYPE.DESCRIPTOR.INVALID");
}
int main() {
  TextDestinationControls();
  UniqueBatchControls();
  auto id = scratchbird::tests::FixtureUuid(1112, 1);
  id.bytes[9] = 0; id.bytes[15] = 0xff;
  const auto row_uuid = scratchbird::tests::FixtureUuid(1112, 2);
  const std::string key = "018f0000-0000-7000-8000-000000000001";
  const std::string user_value = "user value";
  a::EngineKeyValueRequest request;
  request.context.database_uuid = scratchbird::tests::FixtureUuid(1112, 3);
  request.target_object.uuid = id;
  scratchbird::core::datatypes::DatatypeTypeCodecIdentityRowV1 registry;
  registry.type_uuid = scratchbird::tests::FixtureUuid(1112, 4);
  registry.descriptor_uuid = scratchbird::tests::FixtureUuid(1112, 5);
  registry.codec_uuid = scratchbird::tests::FixtureUuid(1112, 6);
  registry.descriptor_generation = 2; registry.type_generation = 3;
  registry.codec_id = "text.utf8"; registry.codec_version = 1; registry.codec_generation = 4;
  a::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1112, 7);
  descriptor.type_uuid = registry.type_uuid;
  descriptor.datatype_descriptor_uuid = registry.descriptor_uuid;
  descriptor.datatype_descriptor_generation = registry.descriptor_generation;
  descriptor.descriptor_kind = "canonical_type_descriptor"; descriptor.canonical_type_name = "text";
  a::CatalogColumnMetadata metadata;
  metadata.identities = {{"column_uuid", id}, {"type_uuid", registry.type_uuid},
      {"datatype_descriptor_uuid", registry.descriptor_uuid}, {"codec_uuid", registry.codec_uuid}};
  metadata.text = {{"canonical", "text"}, {"nullable", "false"},
      {"datatype_descriptor_generation", "2"}, {"type_generation", "3"},
      {"codec_id", registry.codec_id}, {"codec_version", "1"}, {"codec_generation", "4"},
      {"null_encoding", std::to_string(registry.null_encoding_code)}};
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor));
  auto admitted = [&] {return a::ExactKeyValueValueDescriptor(descriptor, "text", registry.type_uuid,
      &registry, id, registry.descriptor_uuid, registry.descriptor_generation, false);};
  Check(admitted());
  // Pure descriptor/result presentation controls. These supplied unit inputs
  // do NOT establish storage, catalog/security admission or persistence; the
  // separate real-file collection gate qualifies that owning production path.
  auto expiry_descriptor = descriptor;
  expiry_descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1112, 8);
  expiry_descriptor.type_uuid = scratchbird::tests::FixtureUuid(1112, 9);
  expiry_descriptor.datatype_descriptor_uuid = scratchbird::tests::FixtureUuid(1112, 10);
  expiry_descriptor.canonical_type_name = "uint64";
  auto expiry_metadata = metadata;
  expiry_metadata.identities = {{"type_uuid",expiry_descriptor.type_uuid},
      {"datatype_descriptor_uuid",expiry_descriptor.datatype_descriptor_uuid},
      {"codec_uuid",scratchbird::tests::FixtureUuid(1112, 11)}};
  expiry_metadata.text["canonical"] = "uint64";
  expiry_metadata.text["codec_id"] = "datatype.uint64.le.v1";
  Check(a::EncodeCatalogColumnMetadata(expiry_metadata,&expiry_descriptor.encoded_descriptor));
  const auto same_descriptor = [](const a::EngineDescriptor& actual,const a::EngineDescriptor& expected){
    Check(actual==expected);
  };
  for(const std::uint64_t number : {UINT64_C(0),UINT64_C(1),UINT64_C(0x80ff007654321000),UINT64_MAX}){
    const auto raw=a::KvNativeExpiry(expiry_descriptor,number);
    Check(raw.encoded_value.empty()&&raw.binary_value.size()==8);same_descriptor(raw.descriptor,expiry_descriptor);
    std::uint64_t decoded=0;for(unsigned i=0;i<8;++i){Check(raw.binary_value[i]==std::uint8_t(number>>(8*i)));
      decoded|=std::uint64_t(raw.binary_value[i])<<(8*i);}Check(decoded==number);
  }
  a::KvCollection collection;
  collection.read.descriptor.columns.resize(3);
  collection.read.descriptor.columns[0].value_descriptor=descriptor;
  collection.read.descriptor.columns[1].value_descriptor=descriptor;
  collection.read.descriptor.columns[2].value_descriptor=expiry_descriptor;
  collection.read.scanned_row_version_count=7;
  std::pmr::monotonic_buffer_resource memory;
  a::KvPlan plan(&memory);plan.identities.push_back(row_uuid);
  plan.outputs.emplace_back(&memory,0,key,user_value,UINT64_MAX,false,0);
  a::EngineApiResult result;a::PublishKvRows(request,collection,plan,result);
  Check(result.result_shape.rows.size()==1&&result.primary_object.uuid==id&&
      result.dml_summary.visible_rows_scanned==7&&result.dml_summary.index_probes==0);
  std::array<bool,5> seen{};
  for(const auto& [name,value]:result.result_shape.rows[0].fields){
    if(name=="key_uuid"||name=="row_uuid"){
      seen[name=="key_uuid"?0:1]=true;
      Check(value.encoded_value.empty()&&value.binary_value==std::vector<std::uint8_t>(row_uuid.bytes.begin(),row_uuid.bytes.end()));
    }else if(name=="key"||name=="value"){
      seen[name=="key"?2:3]=true;
      Check(value.encoded_value==(name=="key"?key:user_value)&&value.binary_value.empty());same_descriptor(value.descriptor,descriptor);
    }else if(name=="ttl_expires_after_tx"){
      seen[4]=true;
      Check(value.encoded_value.empty()&&value.binary_value==std::vector<std::uint8_t>(8,0xff));same_descriptor(value.descriptor,expiry_descriptor);
    }
  }
  Check(std::all_of(seen.begin(),seen.end(),[](bool present){return present;}));
  auto invalid=row_uuid;invalid.bytes[8]=0;plan.identities[0]=invalid;
  a::EngineApiResult refused;bool rejected=false;
  try{a::PublishKvRows(request,collection,plan,refused);}catch(const a::KvFailure& failure){
    rejected=true;Check(failure.diagnostics.size()==1&&failure.diagnostics[0].code=="DATATYPE.DESCRIPTOR.INVALID"&&
        failure.diagnostics[0].detail=="key_value_result_identity_missing");}
  Check(rejected&&refused.result_shape.rows.empty());
  descriptor.datatype_descriptor_uuid = {}; Check(!admitted());
  descriptor.datatype_descriptor_uuid = registry.descriptor_uuid;
  descriptor.datatype_descriptor_generation = 0; Check(!admitted());
  descriptor.datatype_descriptor_generation = registry.descriptor_generation;
  Check(admitted());
  descriptor.type_uuid = id; Check(!admitted()); descriptor.type_uuid = registry.type_uuid;
  metadata.identities["codec_uuid"] = id;
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor)); Check(!admitted());
  metadata.identities["codec_uuid"] = registry.codec_uuid;
  metadata.text["unrecognized"] = "true";
  Check(a::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor)); Check(!admitted());
  descriptor.encoded_descriptor = "canonical=text;type_uuid=018f0000-0000-7000-8000-000000000001;nullable=false";
  Check(!admitted());
}
