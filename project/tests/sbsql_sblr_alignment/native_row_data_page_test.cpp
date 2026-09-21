// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define METRIC_SAMPLE_CODEC_MAIN MetricSamplePageBaseMain
#include "metric_sample_codec_test.cpp"
#undef METRIC_SAMPLE_CODEC_MAIN
#include "native_row_data_page.hpp"
#include <openssl/sha.h>
namespace page=scratchbird::storage::page;
namespace disk=scratchbird::storage::disk;
namespace platform=scratchbird::core::platform;
namespace {
using byte=platform::byte;
using u64=platform::u64;
void Number(Bytes& b,std::size_t at,unsigned width,u64 n){Put(b,at,n,width);}
void PutUuid(Bytes& b,std::size_t at,const m::MetricUuid& u){std::copy(u.bytes.begin(),u.bytes.end(),b.begin()+at);}
u64 BodyFnv(const byte* b,std::size_t n){u64 h=1469598103934665603ull;for(std::size_t i=0;i<n;++i){h^=b[i];h*=1099511628211ull;}return h;}
void RowSeal(Bytes& b){std::fill(b.end()-32,b.end(),0);std::array<byte,32> h{};SHA256(b.data(),b.size(),h.data());std::copy(h.begin(),h.end(),b.end()-32);}
Bytes RowOracle(const page::NativeRowDataPage& leaf) {
  const auto& h=leaf.header; const auto& body=leaf.body; Bytes b(h.page_size_bytes,0);
  const std::string_view cm="SBPGV002",rm="SBROW004",vm="SBDVAL01";
  std::copy(cm.begin(),cm.end(),b.begin()); Number(b,8,4,128); Number(b,12,4,h.page_size_bytes);
  Number(b,16,4,h.page_type); Number(b,20,2,1); Number(b,22,2,1);
  PutUuid(b,24,h.database_uuid); PutUuid(b,40,h.filespace_uuid); PutUuid(b,56,h.page_uuid);
  Number(b,72,8,h.page_number); Number(b,80,8,h.page_generation); Number(b,88,8,h.flags);
  PutUuid(b,104,h.page_size_profile_uuid); Number(b,120,2,1);
  u64 common=14695981039346656037ull; for(unsigned i=0;i<128;++i) {common^=b[i];common*=1099511628211ull;} Number(b,96,8,common);
  std::copy(rm.begin(),rm.end(),b.begin()+128); Number(b,136,4,96); Number(b,140,4,body.rows.size());
  Number(b,152,8,body.next_page_number); PutUuid(b,168,body.relation_uuid.value);
  Number(b,184,8,body.page_generation); Number(b,192,8,body.segment_id);
  Number(b,200,8,body.segment_generation); Number(b,208,8,body.compaction_generation);
  struct Slot {unsigned stable,offset,size;u64 hash;bool deleted;}; std::vector<Slot> slots;
  unsigned at=224;
  for(unsigned i=0;i<body.rows.size();++i) {
    const auto& row=body.rows[i]; const auto start=at;
    PutUuid(b,at,row.row_uuid.value); PutUuid(b,at+16,row.transaction_uuid.value);
    Number(b,at+32,8,row.local_transaction_id); Number(b,at+40,8,row.row_version);
    Number(b,at+48,2,row.deleted?1:0); Number(b,at+50,2,row.cells.size()); Number(b,at+52,4,i+1);
    Number(b,at+60,4,row.stable_slot_id); PutUuid(b,at+72,row.version_uuid);
    Number(b,at+88,8,row.previous_row_version); Number(b,at+96,8,row.next_row_version);
    PutUuid(b,at+104,row.previous_version_uuid); PutUuid(b,at+120,row.next_version_uuid);
    Number(b,at+136,8,row.storage_generation); at+=144;
    for(const auto& cell:row.cells) {
      const auto& payload=cell.value.payload; const unsigned value=at+16;
      Number(b,at,2,cell.column_ordinal); Number(b,at+4,4,32+payload.size());
      std::copy(vm.begin(),vm.end(),b.begin()+value); Number(b,value+8,4,static_cast<unsigned>(cell.value.type_id));
      Number(b,value+12,2,(cell.value.is_null?1:0)|(cell.value.payload_is_toast_reference?2:0));
      Number(b,value+14,2,32); Number(b,value+16,4,payload.size());
      Number(b,value+24,8,BodyFnv(payload.data(),payload.size()));
      std::copy(payload.begin(),payload.end(),b.begin()+value+32);
      Number(b,at+8,8,BodyFnv(b.data()+value,32+payload.size())); at+=48+payload.size();
    }
    Number(b,start+56,4,at-start); const auto hash=BodyFnv(b.data()+start,at-start); Number(b,start+64,8,hash);
    slots.push_back({row.stable_slot_id,start-128,at-start,hash,row.deleted});
  }
  Number(b,216,4,at-128);
  for(const auto& slot:slots) {
    Number(b,at,4,slot.stable); Number(b,at+4,4,slot.offset); Number(b,at+8,4,slot.size);
    Number(b,at+12,4,slot.deleted?1:0); Number(b,at+16,8,slot.hash); at+=24;
  }
  Number(b,144,4,at-128); Number(b,220,4,b.size()-32-at);
  Number(b,160,8,BodyFnv(b.data()+128,b.size()-160)); RowSeal(b); return b;
}
page::NativeRowDataPage RowExample(unsigned profile,const SampleFixture& f){
  page::NativeRowDataPage p;const auto& format=disk::kCanonicalFilespacePageProfiles[profile];
  p.header.page_size_bytes=format.page_size_bytes;p.header.page_size_profile_uuid=format.uuid;
  p.header.database_uuid=f.sample.database_uuid;p.header.filespace_uuid=Id(60);p.header.page_uuid=Id(61);
  p.header.page_type=0x0100;p.header.page_number=21;p.header.page_generation=7;
  p.body.relation_uuid={platform::UuidKind::object,Id(62)};p.body.segment_id=1;p.body.segment_generation=2;
  p.body.compaction_generation=3;p.body.page_number=21;p.body.page_generation=7;
  page::RowDataRecord row;row.row_uuid={platform::UuidKind::row,f.sample.sample_uuid};
  row.version_uuid=Id(63);row.transaction_uuid={platform::UuidKind::transaction,Id(64)};
  row.local_transaction_id=1;row.row_version=f.sample.revision;row.storage_generation=1;
  row.stable_slot_id=1;row.internal_row_ordinal=1;
  page::RowDataCell cell;cell.column_ordinal=1;cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary;
  cell.value.payload=GoldenSample(f);row.cells.push_back(std::move(cell));p.body.rows.push_back(std::move(row));return p;
}
void PageRefused(const Bytes& b){auto r=page::DecodeNativeRowDataPage(b);Check(!r.ok()&&!r.page&&r.bytes.empty(),"invalid native row image exposed rows");}
void PageEncodeRefused(const page::NativeRowDataPage& p){auto r=page::EncodeNativeRowDataPage(p);Check(!r.ok()&&!r.page&&r.bytes.empty(),"invalid row image encoded");}
// Repair every outer checksum so semantic refusals cannot pass merely because
// a damaged container was rejected before its rows were inspected.
void BodySeal(Bytes& b){Number(b,160,8,0);Number(b,160,8,BodyFnv(b.data()+128,b.size()-160));RowSeal(b);}
void SemanticImages(){
  const auto f=Sample();
  for(unsigned profile=0;profile<5;++profile){
    const auto original=RowExample(profile,f);
    const auto refuse=[&](auto change){auto p=original;change(p);PageEncodeRefused(p);PageRefused(RowOracle(p));};
    refuse([](auto& p){p.body.next_page_number=1;});
    refuse([](auto& p){++p.body.page_generation;});
    refuse([](auto& p){p.body.segment_id=0;});
    refuse([](auto& p){p.body.segment_generation=0;});
    refuse([](auto& p){p.body.compaction_generation=0;});
    refuse([](auto& p){p.body.relation_uuid.value={};});
    refuse([](auto& p){p.body.rows[0].row_uuid.value.bytes[6]=0x40;});
    refuse([](auto& p){p.body.rows[0].transaction_uuid.value={};});
    refuse([](auto& p){p.body.rows[0].version_uuid=p.body.rows[0].row_uuid.value;});
    refuse([](auto& p){p.body.rows[0].local_transaction_id=0;});
    refuse([](auto& p){p.body.rows[0].row_version=0;});
    refuse([](auto& p){p.body.rows[0].storage_generation=p.body.page_generation+1;});
    refuse([](auto& p){p.body.rows[0].stable_slot_id=0;});
    refuse([](auto& p){p.body.rows[0].previous_row_version=1;});
    refuse([](auto& p){p.body.rows[0].previous_version_uuid=Id(65);});
    refuse([](auto& p){p.body.rows[0].next_row_version=p.body.rows[0].row_version; p.body.rows[0].next_version_uuid=Id(65);});
    refuse([](auto& p){p.body.rows[0].deleted=true;p.body.rows[0].cells[0].value.type_id=scratchbird::core::datatypes::CanonicalTypeId::boolean;});
    auto p=original;auto second=p.body.rows[0];second.row_uuid.value=Id(66);second.version_uuid=Id(67);
    second.stable_slot_id=19;second.internal_row_ordinal=2;p.body.rows.push_back(second);
    const auto valid=RowOracle(p);auto encoded=page::EncodeNativeRowDataPage(p);
    Check(encoded.ok()&&encoded.bytes==valid&&page::DecodeNativeRowDataPage(valid).ok(),"multiple native rows rejected");
    p.body.rows[1].version_uuid=p.body.rows[0].version_uuid;PageEncodeRefused(p);PageRefused(RowOracle(p));
    p.body.rows[1]=second;p.body.rows[1].row_uuid=p.body.rows[0].row_uuid;PageEncodeRefused(p);PageRefused(RowOracle(p));
    p.body.rows[1]=second;p.body.rows[1].row_version=2;p.body.rows[1].previous_row_version=1;
    p.body.rows[1].previous_version_uuid=p.body.rows[0].version_uuid;PageEncodeRefused(p);PageRefused(RowOracle(p));
    auto padding=RowOracle(original);padding[padding.size()-33]=1;BodySeal(padding);PageRefused(padding);
    auto reserved=RowOracle(original);reserved[148]=1;BodySeal(reserved);PageRefused(reserved);
    auto directory=RowOracle(original);Number(directory,216,4,96);BodySeal(directory);PageRefused(directory);
  }
}
void Images(){
  const auto f=Sample();
  for(unsigned profile=0;profile<5;++profile){
    auto p=RowExample(profile,f);const auto expected=RowOracle(p);
    auto e=page::EncodeNativeRowDataPage(p);Check(e.ok()&&e.bytes==expected,"native row image disagrees with byte oracle");
    auto d=page::DecodeNativeRowDataPage(expected);Check(d.ok()&&d.bytes==expected,"native row image oracle decode");
    if(d.ok()){
      const auto& payload=d.page->body.rows[0].cells[0].value.payload;
      auto sample=m::DecodeMetricRawSample(f.descriptor,f.series,payload);
      Check(sample.ok()&&sample.record->sample_uuid==d.page->body.rows[0].row_uuid.value&&
        sample.record->database_uuid==d.page->header.database_uuid,"nested sample lost row/node identity");
    }
    for(std::size_t n=0;n<expected.size();++n){Bytes truncated(expected.begin(),expected.begin()+n);PageRefused(truncated);}
    auto extra=expected;extra.push_back(0);PageRefused(extra);
    for(auto at:{0u,96u,128u,160u,224u,280u,300u}){auto bad=expected;bad[at]^=1;PageRefused(bad);}
    auto bad=expected;bad.back()^=1;PageRefused(bad);
    p.header.page_type=6;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.next_page_number=1;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.page_generation++;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.compaction_generation=0;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.segment_id=0;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.segment_generation=0;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.rows[0].stable_slot_id=0;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.rows[0].storage_generation=0;PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.rows[0].version_uuid={};PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.rows[0].cells[0].value.payload.resize(p.header.page_size_bytes);PageEncodeRefused(p);
    p=RowExample(profile,f);p.body.rows.clear();auto empty=page::EncodeNativeRowDataPage(p);
    Check(empty.ok()&&page::DecodeNativeRowDataPage(empty.bytes).ok(),"empty allocated row image rejected");
    p=RowExample(profile,f);p.body.rows[0].deleted=true;p.body.rows[0].cells.clear();auto deleted=page::EncodeNativeRowDataPage(p);
    Check(deleted.ok()&&page::DecodeNativeRowDataPage(deleted.bytes).ok(),"native delete marker image rejected");
  }
}
void PageFaults(){
  const auto f=Sample();const auto p=RowExample(0,f);const auto bytes=RowOracle(p);
  for(unsigned op=0;op<2;++op){unsigned faults=0;bool finished=false;
    for(long point=0;point<1000;++point){codec_fault::remaining=point;codec_fault::fired=false;
      auto r=op? page::DecodeNativeRowDataPage(bytes):page::EncodeNativeRowDataPage(p);
      const bool fired=codec_fault::fired;codec_fault::remaining=-1;
      Check(r.ok()||(!r.page&&r.bytes.empty()),"native row allocation failure exposed partial image");
      if(fired){++faults;Check(!r.ok(),"native row allocation fault reported success");}
      else {Check(r.ok(),"unfaulted row image failed");finished=true;break;}
    }
    Check(finished&&faults,"native row allocation sweep incomplete");
    std::cout<<"native row allocation operation="<<op<<" injected="<<faults<<'\n';
  }
}
}
int main(){MetricSamplePageBaseMain();auto before=checks;Images();SemanticImages();PageFaults();
  std::cout<<"native row image checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
