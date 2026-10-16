// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define METRIC_SAMPLE_CODEC_MAIN MetricSamplePageBaseMain
#include "metric_sample_codec_test.cpp"
#undef METRIC_SAMPLE_CODEC_MAIN
#include "native_row_data_page.hpp"
#include "datatype_date.hpp"
#include "datatype_timestamp.hpp"
#include "datatype_interval.hpp"
#include "datatype_bit_string.hpp"
#include "datatype_blob.hpp"
#include "filespace_page_zero.hpp"
#include <filesystem>
#include <unistd.h>
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
Bytes RowOracle(const page::NativeRowDataPage& leaf, u64 relation_generation=0,
    std::span<const page::RowDataProfileColumnBinding> columns={}) {
  const auto& h=leaf.header; const auto& body=leaf.body; Bytes b(h.page_size_bytes,0);
  const bool external=std::any_of(body.rows.begin(),body.rows.end(),[](const auto& row){
    return std::any_of(row.cells.begin(),row.cells.end(),[](const auto& cell){return cell.external_value.has_value();});});
  const bool blob=!columns.empty();
  const unsigned bindings=std::count_if(columns.begin(),columns.end(),[](const auto& c){return bool(c.blob);});
  const unsigned body_header=blob?112+32*bindings:96;
  const std::string_view cm="SBPGV002",rm=blob?"SBROW006":external?"SBROW005":"SBROW004",vm="SBDVAL01";
  std::copy(cm.begin(),cm.end(),b.begin()); Number(b,8,4,128); Number(b,12,4,h.page_size_bytes);
  Number(b,16,4,h.page_type); Number(b,20,2,1); Number(b,22,2,1);
  PutUuid(b,24,h.database_uuid); PutUuid(b,40,h.filespace_uuid); PutUuid(b,56,h.page_uuid);
  Number(b,72,8,h.page_number); Number(b,80,8,h.page_generation); Number(b,88,8,h.flags);
  PutUuid(b,104,h.page_size_profile_uuid); Number(b,120,2,1);
  u64 common=14695981039346656037ull; for(unsigned i=0;i<128;++i) {common^=b[i];common*=1099511628211ull;} Number(b,96,8,common);
  std::copy(rm.begin(),rm.end(),b.begin()+128); Number(b,136,4,body_header); Number(b,140,4,body.rows.size());
  if(blob){Number(b,224,8,relation_generation);Number(b,232,4,bindings);unsigned entry=240;
    for(const auto& c:columns)if(c.blob){Number(b,entry,2,c.column_ordinal);PutUuid(b,entry+8,c.column_uuid);
      Number(b,entry+24,8,c.column_generation);entry+=32;}}
  Number(b,152,8,body.next_page_number); PutUuid(b,168,body.relation_uuid.value);
  Number(b,184,8,body.page_generation); Number(b,192,8,body.segment_id);
  Number(b,200,8,body.segment_generation); Number(b,208,8,body.compaction_generation);
  struct Slot {unsigned stable,offset,size;u64 hash;bool deleted;}; std::vector<Slot> slots;
  unsigned at=128+body_header;
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
      if(cell.external_value){
        const auto& locator=*cell.external_value;const unsigned value=at+16;
        Number(b,at,2,cell.column_ordinal);Number(b,at+2,2,1);Number(b,at+4,4,40);
        const std::string_view magic="SBMGLV02";
        std::copy(magic.begin(),magic.end(),b.begin()+value);PutUuid(b,value+8,locator.object_uuid);
        Number(b,value+24,8,locator.content_checksum);Number(b,value+32,8,locator.logical_bytes);
        Number(b,at+8,8,BodyFnv(b.data()+value,40));at+=56;continue;
      }
      const auto& payload=cell.value.payload; const unsigned value=at+16;
      if(blob&&cell.value.type_id==scratchbird::core::datatypes::CanonicalTypeId::blob){
        Number(b,at,2,cell.column_ordinal);Number(b,at+2,2,cell.value.is_null?3:2);
        Number(b,at+4,4,payload.size());std::copy(payload.begin(),payload.end(),b.begin()+value);
        Number(b,at+8,8,BodyFnv(payload.data(),payload.size()));at+=16+payload.size();continue;
      }
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
  const auto f=Sample();
  for(unsigned variant=0;variant<2;++variant){
  auto p=RowExample(0,f);
  if(variant){page::RowDataCell cell;cell.column_ordinal=2;
    cell.external_value=page::RowExternalValueLocator{Id(80),123,12000};p.body.rows[0].cells.push_back(cell);}
  const auto bytes=RowOracle(p);
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
void ExternalImages(){
  const auto f=Sample();
  for(unsigned profile=0;profile<5;++profile){
    for(const u64 length:{u64{0},u64{12000},std::numeric_limits<u64>::max()}){
      auto p=RowExample(profile,f);
      page::RowDataCell external;external.column_ordinal=2;
      external.external_value=page::RowExternalValueLocator{Id(80),0xff00112233445566ull,length};
      p.body.rows[0].cells.push_back(external);
      const auto expected=RowOracle(p);
      const auto encoded=page::EncodeNativeRowDataPage(p);
      const auto decoded=page::DecodeNativeRowDataPage(expected);
      Check(encoded.ok()&&encoded.bytes==expected&&decoded.ok(),"external native image disagrees with independent byte oracle");
      if(decoded.ok())Check(decoded.page->body.rows[0].cells[1].external_value==external.external_value&&
          decoded.page->body.rows[0].cells[0].value.payload==p.body.rows[0].cells[0].value.payload,
          "external native image lost locator or adjacent logical value");
      for(unsigned version=0;version<16;++version)if(version!=7){
        auto invalid=p;invalid.body.rows[0].cells[1].external_value->object_uuid.bytes[6]=version<<4;
        PageEncodeRefused(invalid);PageRefused(RowOracle(invalid));
      }
    }
  }
}
}
void TemporalReceivers() {
  namespace dt = scratchbird::core::datatypes;
  for (unsigned generation : {10u, 11u}) for (const auto type : {dt::CanonicalTypeId::time, dt::CanonicalTypeId::timestamp, dt::CanonicalTypeId::interval}) {
    const bool timestamp = type == dt::CanonicalTypeId::timestamp;
    const bool interval = type == dt::CanonicalTypeId::interval;
    const auto snapshot = generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11;
    page::RowDataTemporalColumnBinding date_column, time_column;
    date_column.column_ordinal = 1; date_column.null_allowed = true;
    time_column.column_ordinal = 2; time_column.null_allowed = true;
    for (const auto& identity : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
      if (identity.legacy_fields.catalog_snapshot_uuid != snapshot) continue;
      if (identity.legacy_fields.canonical_binary_type_code == 400) {
        auto profile = dt::BuildDateValidatedProfileHandleV3({snapshot, snapshot, generation, generation}, identity);
        Check(profile.ok(), "DATE receiver fixture profile");
        date_column.date = std::make_shared<const dt::DateValidatedProfileHandleV3>(std::move(profile.profile));
      }
      if (!timestamp && !interval && identity.legacy_fields.canonical_binary_type_code == 401) {
        auto profile = dt::BuildTimeValidatedProfileHandleV3({snapshot, snapshot, generation, generation}, identity);
        Check(profile.ok(), "TIME receiver fixture profile");
        time_column.time = std::make_shared<const dt::TimeValidatedProfileHandleV3>(std::move(profile.profile));
      }
      if (timestamp && identity.legacy_fields.canonical_binary_type_code == static_cast<unsigned>(dt::CanonicalTypeId::timestamp)) {
        auto profile = dt::BuildTimestampValidatedProfileHandleV3({snapshot, snapshot, generation, generation}, identity);
        Check(profile.ok(), "TIMESTAMP receiver fixture profile");
        time_column.timestamp = std::make_shared<const dt::TimestampValidatedProfileHandleV3>(std::move(profile.profile));
      }
      if (interval && identity.legacy_fields.canonical_binary_type_code == static_cast<unsigned>(type)) {
        auto profile = dt::BuildIntervalValidatedProfileHandleV3({snapshot, snapshot, generation, generation}, identity);
        Check(profile.ok(), "INTERVAL receiver fixture profile");
        time_column.interval = std::make_shared<const dt::IntervalValidatedProfileHandleV3>(std::move(profile.profile));
      }
    }
    Check(date_column.date && (time_column.time || time_column.timestamp || time_column.interval), "explicit temporal receiver fixture missing");
    const std::array<page::RowDataTemporalColumnBinding, 2> columns{date_column, time_column};
    for (unsigned page_profile = 0; page_profile < 5; ++page_profile) {
      for (unsigned sample = 0; sample < 5; ++sample) {
        auto p = RowExample(page_profile, Sample());
        p.body.rows.front().cells.front().column_ordinal = 0;
        const page::RowDataTemporalReceiver receiver{p.body.relation_uuid, columns};
        const std::array<u64, 5> days{0x80000000u, 0xffffffffu, 0, 1, 0x7fffffffu};
        const std::array<u64, 5> times{0, 1, 999'999'999, 1'000'000'000, 86'399'999'999'999ull};
        const std::array<std::int64_t, 5> seconds{dt::kTimestampMinimumCivilSecondV3, -1, 0, 1, dt::kTimestampMaximumCivilSecondV3};
        for (bool null : {false, true}) {
          p.body.rows.front().cells.resize(1);
          for (unsigned i = 0; i < 2; ++i) {
            page::RowDataCell cell;
            cell.column_ordinal = i + 1;
            cell.value.type_id = i ? type : dt::CanonicalTypeId::date;
            cell.value.is_null = null;
            if (!null) for (unsigned j = 0; j < (i ? 8u : 4u); ++j)
              cell.value.payload.push_back(static_cast<byte>((i ? (timestamp ? static_cast<u64>(seconds[sample]) : times[sample]) : days[sample]) >> (8 * j)));
            if (!null && i && timestamp) {
              const unsigned fraction = sample % 2 ? 999999999 : 0;
              for (unsigned j = 0; j < 4; ++j) cell.value.payload.push_back(static_cast<byte>(fraction >> (8*j)));
              cell.value.payload.resize(16, 0);
            }
            if (!null && i && interval) {
              cell.value.payload.clear();
              const std::array<u64,5> nanos{0x8000000000000000ull,0xffffffffffffffffull,0,1,0x7fffffffffffffffull};
              // Independent LE32 months, LE32 civil days, LE64 fixed nanos;
              // opposing signs and extrema must not normalize/carry to a day.
              for(unsigned field=0;field<3;++field) {
                const u64 value=field==0?days[sample]:field==1?days[4-sample]:nanos[sample];
                for(unsigned j=0;j<(field==2?8u:4u);++j)
                  cell.value.payload.push_back(static_cast<byte>(value>>(8*j)));
              }
            }
            p.body.rows.front().cells.push_back(std::move(cell));
          }
          const auto oracle = RowOracle(p);
          const Bytes expected(oracle.begin()+128, oracle.end()-32);
          const auto built = page::BuildRowDataPageBody(p.body, p.header.page_size_bytes-32, receiver);
          Check(built.ok() && built.serialized == expected, "bound temporal body differs from independent binary oracle");
          const auto absent = page::BuildRowDataPageBody(p.body, p.header.page_size_bytes-32);
          Check(!absent.ok() && absent.body.rows.empty() && absent.serialized.empty(), "profileless temporal writer admitted");
          const auto unbound = page::ParseRowDataPageBody(expected, p.body.page_number);
          Check(!unbound.ok() && unbound.body.rows.empty() && unbound.serialized.empty(), "profileless temporal reader admitted");
          if (interval) {
            // Do not let the preceding DATE refusal mask an enum-only INTERVAL
            // fallback. Exercise a page containing only this component.
            auto isolated=p;
            isolated.body.rows.front().cells={p.body.rows.front().cells.back()};
            const auto isolated_image=RowOracle(isolated);
            const Bytes isolated_body(isolated_image.begin()+128,isolated_image.end()-32);
            Check(page::BuildRowDataPageBody(isolated.body,p.header.page_size_bytes-32,receiver).ok()&&
                  page::ParseRowDataPageBody(isolated_body,p.body.page_number,receiver).ok(),
                  "isolated bound INTERVAL positive failed");
            const auto missing_read=page::ParseRowDataPageBody(isolated_body,p.body.page_number);
            const auto missing_write=page::BuildRowDataPageBody(isolated.body,p.header.page_size_bytes-32);
            Check(!missing_read.ok()&&missing_read.body.rows.empty()&&missing_read.serialized.empty()&&
                  !missing_write.ok()&&missing_write.body.rows.empty()&&missing_write.serialized.empty(),
                  "isolated INTERVAL escaped receiving authority");
            for(const auto* refused:{&missing_read,&missing_write})
              Check(refused->diagnostic.diagnostic_code=="CTI.INTERVAL.DESCRIPTOR_INVALID"&&
                    refused->diagnostic.message_key=="storage.row_data_page.interval_profile_missing",
                    "isolated INTERVAL refused for the wrong reason");
          }
          for (bool retain : {false, true}) {
            const auto decoded = retain ? page::ParseRowDataPageBody(expected, p.body.page_number, receiver) :
                page::ParseRowDataPageRows(expected, p.body.page_number, receiver);
            Check(decoded.ok() && decoded.body.rows.size() == 1 && decoded.body.rows[0].cells.size() == 3,
                  "bound temporal row decode lost cells");
            if (!decoded.ok() || decoded.body.rows.size() != 1 || decoded.body.rows[0].cells.size() != 3) continue;
            Check(retain ? decoded.serialized == expected : decoded.serialized.empty(), "row reader image ownership drift");
            for (unsigned i = 1; i < 3; ++i) {
              const auto& actual = decoded.body.rows[0].cells[i].value;
              const auto& source = p.body.rows[0].cells[i].value;
              Check(actual.type_id == source.type_id && actual.is_null == null && actual.payload == source.payload,
                    "temporal row decode altered native component");
            }
            const auto rebuilt = page::BuildRowDataPageBodyOwned(decoded.body, p.header.page_size_bytes-32, receiver);
            Check(rebuilt.ok() && rebuilt.serialized == expected, "temporal read/rebuild changed canonical image");
          }
          for (unsigned fault = 0; fault < 7; ++fault) {
            auto bad_columns = columns;
            auto bad_receiver = receiver;
            bad_receiver.columns = bad_columns;
            if (fault == 0) bad_receiver.relation_uuid.value.bytes[0] ^= 1;
            if (fault == 1) bad_columns[1].column_ordinal = 1;
            if (fault == 2) bad_columns[0].date.reset();
            if (fault == 3) {
              if (timestamp) bad_columns[0].timestamp = time_column.timestamp;
              else if (interval) bad_columns[0].interval = time_column.interval;
              else bad_columns[0].time = time_column.time;
            }
            if (fault == 4) bad_columns[1].column_ordinal = 3;
            if (fault == 5) {
              auto bad = std::make_shared<dt::DateValidatedProfileHandleV3>(*date_column.date);
              bad->profile_fingerprint[0] ^= 1; bad_columns[0].date = std::move(bad);
            }
            if (fault == 6 && !timestamp && !interval) {
              auto bad = std::make_shared<dt::TimeValidatedProfileHandleV3>(*time_column.time);
              ++bad->receipt.registry_generation; bad_columns[1].time = std::move(bad);
            }
            if (fault == 6 && timestamp) {
              auto bad = std::make_shared<dt::TimestampValidatedProfileHandleV3>(*time_column.timestamp);
              ++bad->receipt.registry_generation; bad_columns[1].timestamp = std::move(bad);
            }
            if (fault == 6 && interval) {
              auto bad = std::make_shared<dt::IntervalValidatedProfileHandleV3>(*time_column.interval);
              ++bad->receipt.registry_generation; bad_columns[1].interval = std::move(bad);
            }
            const auto rejected = page::ParseRowDataPageBody(expected, p.body.page_number, bad_receiver);
            const auto rejected_write = page::BuildRowDataPageBody(p.body, p.header.page_size_bytes-32, bad_receiver);
            Check(!rejected.ok() && rejected.body.rows.empty() && rejected.serialized.empty() &&
                      !rejected_write.ok() && rejected_write.body.rows.empty() && rejected_write.serialized.empty(),
                  "invalid temporal receiver published rows or image");
          }
          if (null) {
            auto strict = columns; strict[0].null_allowed = strict[1].null_allowed = false;
            const page::RowDataTemporalReceiver nonnull{p.body.relation_uuid, strict};
            Check(!page::BuildRowDataPageBody(p.body, p.header.page_size_bytes-32, nonnull).ok() &&
                      !page::ParseRowDataPageBody(expected, p.body.page_number, nonnull).ok(), "NULL crossed nonnull receiver");
          }
          for (unsigned fault = 0; fault < 4; ++fault) {
            auto malformed = p;
            auto& cell = malformed.body.rows.front().cells.back();
            if (fault == 0) cell.value.payload.push_back(0);
            if (fault == 1) cell.value.type_id = dt::CanonicalTypeId::int64;
            if (fault == 2) cell.value.payload_is_toast_reference = true;
            if (fault == 3) { cell.value.is_null = false; cell.value.payload.assign(interval ? 15 : timestamp ? 16 : 8, 255); }
            const auto image = RowOracle(malformed);
            const Bytes bytes(image.begin()+128, image.end()-32);
            const auto failed_read = page::ParseRowDataPageBody(bytes, p.body.page_number, receiver);
            const auto failed_write = page::BuildRowDataPageBody(malformed.body, p.header.page_size_bytes-32, receiver);
            Check(!failed_read.ok() && failed_read.body.rows.empty() && failed_read.serialized.empty() &&
                      !failed_write.ok() && failed_write.body.rows.empty() && failed_write.serialized.empty(),
                  "malformed temporal component crossed page boundary");
            if (fault == 3) {
              const auto check_fact = [&](const auto& fact, auto get_fact) {
              for (const auto* failed : {&failed_read, &failed_write}) {
                Check(get_fact(*failed).has_value(), "temporal typed diagnostic was discarded");
                if (!get_fact(*failed)) continue;
                const auto& actual = *get_fact(*failed);
                Check(actual.status.code == fact.status.code && actual.status.severity == fact.status.severity &&
                    actual.status.subsystem == fact.status.subsystem && actual.diagnostic_code == fact.diagnostic_code &&
                    actual.detail == fact.detail && actual.parameter_count == fact.parameter_count,
                    "temporal diagnostic fact changed at row boundary");
                for (unsigned i = 0; i < fact.parameters.size(); ++i) {
                  const auto& a = actual.parameters[i]; const auto& e = fact.parameters[i];
                  Check(a.kind == e.kind && a.name == e.name && a.unsigned_value == e.unsigned_value &&
                      a.signed_value == e.signed_value && a.uuid_value == e.uuid_value && a.token_value == e.token_value,
                      "temporal diagnostic parameter changed at row boundary");
                }
              }
              };
              if (timestamp)
                check_fact(dt::DecodeCanonicalTimestampComponentNoAllocV3(*time_column.timestamp,
                    dt::TimestampValueStateV3::value, true, cell.value.payload).diagnostic,
                    [](const auto& result) -> const auto& { return result.timestamp_diagnostic; });
              else if (interval)
                check_fact(dt::DecodeCanonicalIntervalComponentNoAllocV3(*time_column.interval,
                    dt::IntervalValueStateV3::value, true, cell.value.payload).diagnostic,
                    [](const auto& result) -> const auto& { return result.interval_diagnostic; });
              else
                check_fact(dt::DecodeCanonicalTimeComponentNoAllocV3(*time_column.time,
                    dt::TimeValueStateV3::value, true, cell.value.payload).diagnostic,
                    [](const auto& result) -> const auto& { return result.time_diagnostic; });
            }
          }
          auto external = p;
          external.body.rows.front().cells.back().value = {};
          external.body.rows.front().cells.back().external_value = page::RowExternalValueLocator{Id(80), 123, 12000};
          const auto external_image = RowOracle(external);
          const Bytes external_body(external_image.begin()+128, external_image.end()-32);
          Check(!page::BuildRowDataPageBody(external.body, p.header.page_size_bytes-32, receiver).ok() &&
                    !page::ParseRowDataPageBody(external_body, p.body.page_number, receiver).ok(),
                "temporal ordinal accepted external locator substitution");
          if (sample == 0 && page_profile == 0 && !null) {
            // Supplied-but-unused profiles are not an unchecked side channel,
            // even on a valid empty page.
            auto empty = p; empty.body.rows.clear();
            const auto empty_image = RowOracle(empty);
            const Bytes empty_body(empty_image.begin()+128, empty_image.end()-32);
            auto invalid_columns = columns;
            invalid_columns.back().time.reset();
            invalid_columns.back().timestamp.reset();
            invalid_columns.back().interval.reset();
            const page::RowDataTemporalReceiver invalid{p.body.relation_uuid, invalid_columns};
            Check(!page::BuildRowDataPageBody(empty.body, p.header.page_size_bytes-32, invalid).ok() &&
                      !page::ParseRowDataPageBody(empty_body, p.body.page_number, invalid).ok(),
                  "empty page bypassed unused receiving-profile validation");
            for (unsigned operation = 0; operation < 2; ++operation) {
              unsigned faults = 0; bool finished = false;
              for (long point = 0; point < 1000; ++point) {
                std::optional<page::RowDataPageResult> result;
                bool threw = false;
                codec_fault::remaining = point; codec_fault::fired = false;
                try {
                  result = operation ? page::ParseRowDataPageBody(expected, p.body.page_number, receiver) :
                      page::BuildRowDataPageBody(p.body, p.header.page_size_bytes-32, receiver);
                } catch (const std::bad_alloc&) { threw = true; }
                const bool fired = codec_fault::fired; codec_fault::remaining = -1;
                Check(!result || result->ok() || (result->body.rows.empty() && result->serialized.empty()),
                      "temporal allocation failure exposed partial output");
                if (fired) { ++faults; Check(threw || (result && !result->ok()), "temporal allocation failure reported success"); }
                else { Check(result && result->ok() && !threw, "unfaulted temporal row codec failed"); finished = true; break; }
              }
              Check(finished && faults, "temporal row allocation sweep incomplete");
            }
          }
        }
      }
    }
  }
}

void BitReceivers() {
  namespace dt = scratchbird::core::datatypes;
  const platform::Uuid descriptor{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x29}};
  for (unsigned generation : {10u, 11u}) {
    const auto snapshot = generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11;
    const auto identity = dt::LookupDatatypeTypeCodecIdentityV3(snapshot, generation, generation, descriptor, 1);
    Check(identity.ok, "BIT row identity unavailable");
    for (auto kind : {dt::BitStringSurfaceProfileKindV3::unqualified,
                      dt::BitStringSurfaceProfileKindV3::fixed,
                      dt::BitStringSurfaceProfileKindV3::varying}) {
      const auto profile = dt::BuildBitStringDescriptorProfileV3(
          {{Id(90), snapshot, generation, generation}, identity.row, kind, 9});
      Check(profile.ok(), "BIT row profile unavailable");
      std::array<page::RowDataProfileColumnBinding, 1> columns;
      columns[0].column_ordinal = 1;
      columns[0].null_allowed = true;
      columns[0].bit_string = std::make_shared<const dt::BitStringDescriptorProfileV3>(profile.profile);
      for (unsigned page_profile = 0; page_profile < 5; ++page_profile) {
        for (unsigned sample = 0; sample <= 10; ++sample) {
          auto p = RowExample(page_profile, Sample());
          auto& cell = p.body.rows[0].cells[0];
          cell.value.type_id = dt::CanonicalTypeId::bit_string;
          cell.value.is_null = sample == 10;
          cell.value.payload.clear();
          if (!cell.value.is_null) {
            const unsigned count = kind == dt::BitStringSurfaceProfileKindV3::fixed ? 9 : sample;
            cell.value.payload = {static_cast<byte>(count), 0, 0, 0};
            for (unsigned i = 0; i < (count + 7) / 8; ++i) cell.value.payload.push_back(0xaa);
            if (count % 8) cell.value.payload.back() &= static_cast<byte>(0xffu << (8 - count % 8));
          }
          const page::RowDataProfileReceiver receiver{p.body.relation_uuid, columns};
          const auto oracle = RowOracle(p);
          const Bytes body(oracle.begin()+128, oracle.end()-32);
          const auto encoded = page::BuildRowDataPageBody(p.body, p.header.page_size_bytes-32, receiver);
          Check(encoded.ok() && encoded.serialized == body, "BIT row differs from independent exact page oracle");
          const auto absent = page::BuildRowDataPageBody(p.body, p.header.page_size_bytes-32);
          Check(!absent.ok() && absent.body.rows.empty() && absent.serialized.empty() &&
                    absent.diagnostic.diagnostic_code == "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
                "profileless BIT writer admitted");
          const auto unbound = page::ParseRowDataPageBody(body, p.body.page_number);
          Check(!unbound.ok() && unbound.body.rows.empty() && unbound.serialized.empty(), "profileless BIT reader admitted");
          for (bool retained : {false, true}) {
            const auto decoded = retained ? page::ParseRowDataPageBody(body, p.body.page_number, receiver) :
                page::ParseRowDataPageRows(body, p.body.page_number, receiver);
            Check(decoded.ok() && decoded.body.rows.size() == 1 && decoded.body.rows[0].cells.size() == 1,
                  "BIT bound row lost cells");
            if (!decoded.ok() || decoded.body.rows.size() != 1 || decoded.body.rows[0].cells.size() != 1) continue;
            const auto& actual = decoded.body.rows[0].cells[0].value;
            Check(actual.type_id == cell.value.type_id && actual.is_null == cell.value.is_null &&
                      actual.payload == cell.value.payload, "BIT row lost state or exact native bits");
            const auto rebuilt = page::BuildRowDataPageBodyOwned(decoded.body, p.header.page_size_bytes-32, receiver);
            Check(rebuilt.ok() && rebuilt.serialized == body, "BIT row re-encode changed bytes");
          }
          const auto refuse = [&](auto change_page, auto change_receiver) {
            auto bad = p;
            auto bindings = columns;
            auto receiving = receiver;
            receiving.columns = bindings;
            change_page(bad); change_receiver(receiving, bindings);
            const auto bytes = RowOracle(bad);
            const Bytes damaged(bytes.begin()+128, bytes.end()-32);
            const auto write = page::BuildRowDataPageBody(bad.body, bad.header.page_size_bytes-32, receiving);
            const auto read = page::ParseRowDataPageBody(damaged, bad.body.page_number, receiving);
            Check(!write.ok() && write.body.rows.empty() && write.serialized.empty() &&
                      !read.ok() && read.body.rows.empty() && read.serialized.empty(),
                  "invalid BIT receiver or payload published partial rows");
          };
          const auto no_page_change = [](auto&){};
          const auto no_receiver_change = [](auto&, auto&){};
          refuse(no_page_change, [](auto& r, auto&){ r.relation_uuid.value.bytes[0] ^= 1; });
          refuse(no_page_change, [](auto&, auto& b){ b[0].column_ordinal = 2; });
          refuse(no_page_change, [](auto&, auto& b){ b[0].bit_string.reset(); });
          refuse(no_page_change, [](auto&, auto& b){
            auto changed = std::make_shared<dt::BitStringDescriptorProfileV3>(*b[0].bit_string);
            changed->profile_fingerprint[0] ^= 1; b[0].bit_string = std::move(changed);
          });
          refuse([](auto& q){ q.body.rows[0].cells[0].value.payload.push_back(1); }, no_receiver_change);
          auto external = p;
          external.body.rows[0].cells[0].value = {};
          external.body.rows[0].cells[0].external_value = page::RowExternalValueLocator{Id(80),123,12000};
          const auto external_image = RowOracle(external);
          const Bytes external_body(external_image.begin()+128,external_image.end()-32);
          const auto external_write = page::BuildRowDataPageBody(external.body, p.header.page_size_bytes-32, receiver);
          const auto external_read = page::ParseRowDataPageBody(external_body, p.body.page_number, receiver);
          for (const auto* rejected : {&external_write, &external_read})
            Check(!rejected->ok() && rejected->diagnostic.diagnostic_code == "CTB.BIT.DESCRIPTOR_INVALID" &&
                      rejected->body.rows.empty() && rejected->serialized.empty(), "BIT locator substitution lost exact refusal");
          if (cell.value.is_null)
            refuse(no_page_change, [](auto&, auto& b){ b[0].null_allowed = false; });
          else if (cell.value.payload[0] % 8)
            refuse([](auto& q){ q.body.rows[0].cells[0].value.payload.back() |= 1; }, no_receiver_change);
        }
      }
    }
  }
}

namespace {
void BlobReceivers(){
  namespace dt=scratchbird::core::datatypes;
  const auto profile=dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid);
  Check(profile.ok(),"native BLOB exact profile");if(!profile.ok())return;
  const auto f=Sample();
  std::string pattern=(std::filesystem::temp_directory_path()/"sb_blob_rows.XXXXXX").string();
  std::vector<char> root(pattern.begin(),pattern.end());root.push_back(0);
  const auto* created=::mkdtemp(root.data());Check(created,"BLOB row private fixture");if(!created)return;
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{created};
  for(unsigned size=0;size<5;++size){
    auto p=RowExample(size,f);
    std::vector<page::RowDataProfileColumnBinding> columns;
    for(unsigned ordinal=2;ordinal<=5;++ordinal){
      page::RowDataProfileColumnBinding column;column.column_ordinal=ordinal;column.null_allowed=true;
      column.column_uuid=Id(90+ordinal);column.column_generation=17+ordinal;
      column.blob=std::make_shared<const dt::BlobValidatedProfileHandleV3>(profile.profile);columns.push_back(column);
      page::RowDataCell cell;cell.column_ordinal=ordinal;
      if(ordinal==5)cell.external_value=page::RowExternalValueLocator{Id(80),123,12000};
      else{cell.value.type_id=dt::CanonicalTypeId::blob;cell.value.is_null=ordinal==3;
        if(ordinal==4){for(unsigned i=0;i<1024;++i)cell.value.payload.push_back(static_cast<byte>(i));
          const std::string_view magic="SBBLOB03SBMGLV02SBDVAL01";cell.value.payload.insert(cell.value.payload.end(),magic.begin(),magic.end());}}
      p.body.rows[0].cells.push_back(std::move(cell));
    }
    const page::RowDataProfileReceiver receiver{p.body.relation_uuid,columns,13};
    const auto expected=RowOracle(p,13,columns);
    auto encoded=page::EncodeNativeRowDataPage(p,receiver);
    auto decoded=page::DecodeNativeRowDataPage(expected,receiver);
    Check(encoded.ok()&&encoded.bytes==expected&&decoded.ok(),"BLOB native page disagrees with independent oracle");
    {auto body=p.body;
      const auto* original_payload=body.rows[0].cells[3].value.payload.data();
      auto owned=page::BuildRowDataPageBodyOwned(std::move(body),p.header.page_size_bytes-32,receiver);
      Check(owned.ok()&&owned.body.rows[0].cells[3].value.payload.data()==original_payload&&
          std::equal(owned.serialized.begin(),owned.serialized.end(),expected.begin()+128,expected.end()-32),
          "owned BLOB row builder cloned payload ownership or changed canonical image");}
    if(decoded.ok()){
      const auto& cells=decoded.page->body.rows[0].cells;
      Check(cells.size()==5&&!cells[1].value.is_null&&cells[1].value.payload.empty()&&
          cells[2].value.is_null&&cells[2].value.payload.empty()&&
          cells[3].value.payload==p.body.rows[0].cells[3].value.payload&&
          cells[4].external_value==p.body.rows[0].cells[4].external_value,
          "native BLOB changed octets,empty,NULL or external locator");
    }
    const auto refused=[&](const auto& result){Check(!result.ok()&&!result.page&&result.bytes.empty(),"BLOB refused image leaked rows");};
    refused(page::EncodeNativeRowDataPage(p));refused(page::DecodeNativeRowDataPage(expected));
    for(unsigned fault=0;fault<16;++fault){
      auto bad=p;auto bindings=columns;auto receiving=receiver;receiving.columns=bindings;
      if(fault==0)receiving.relation_generation=0;
      if(fault==1)bindings[0].column_generation=0;
      if(fault==2)receiving.relation_uuid.value=Id(90);
      if(fault==3)bindings[0].blob.reset();
      if(fault==4){auto stale=profile.profile;++stale.profile_generation;bindings[0].blob=std::make_shared<const dt::BlobValidatedProfileHandleV3>(stale);}
      if(fault==5)bad.body.rows[0].cells[2].value.payload={0};
      if(fault==6)bindings[1].null_allowed=false;
      if(fault==7)bad.body.rows[0].cells[4].external_value->logical_bytes=dt::kBlobMaximumLogicalBytesV3+1;
      if(fault==8)bad.body.rows[0].cells[3].value.type_id=dt::CanonicalTypeId::binary;
      if(fault==9)bindings[1].column_ordinal=2;
      if(fault==10)bindings[0].column_ordinal=0;
      if(fault==11)bad.body.rows[0].cells[1].column_ordinal=0;
      if(fault==12)bad.body.rows[0].cells[2].column_ordinal=2;
      if(fault==13)bindings[0].column_uuid={};
      if(fault==14)bindings[0].column_uuid=p.body.relation_uuid.value;
      if(fault==15)bindings[1].column_uuid=bindings[0].column_uuid;
      refused(page::EncodeNativeRowDataPage(bad,receiving));
      refused(page::DecodeNativeRowDataPage(RowOracle(bad,13,columns),receiving));
    }
    for(unsigned fault=0;fault<2;++fault){auto stale=receiver;auto bindings=columns;stale.columns=bindings;
      if(fault)++bindings[0].column_generation;else ++stale.relation_generation;
      refused(page::DecodeNativeRowDataPage(expected,stale));
    }
    // The binding directory itself is covered by the body/native seals.
    // Reseal mutations to exercise exact structural/schema admission.
    for(unsigned fault=0;fault<9;++fault){auto bad=expected;
      if(fault==0)Number(bad,232,4,0);
      if(fault==1)Number(bad,232,4,65536);
      if(fault==2)Number(bad,232,4,columns.size()+1);
      if(fault==3)bad[236]=1;
      if(fault==4)bad[242]=1;
      if(fault==5)bad[248]^=1;
      if(fault==6)Number(bad,264,8,0);
      if(fault==7)Number(bad,240,2,0);
      if(fault==8)Number(bad,136,4,96);
      BodySeal(bad);refused(page::DecodeNativeRowDataPage(bad,receiver));
    }
    // Recompute every container checksum after changing the kind. A failure
    // must come from representation admission, not an earlier damaged seal.
    const auto read_number=[](const Bytes& b,std::size_t at,unsigned width){
      u64 result=0;for(unsigned i=0;i<width;++i)result|=u64(b[at+i])<<(8*i);return result;};
    const unsigned row_start=128+112+32*columns.size();
    unsigned first_blob=row_start+144+16+32+p.body.rows[0].cells[0].value.payload.size();
    for(unsigned kind:{0u,4u,65535u}){
      auto malformed=expected;Number(malformed,first_blob+2,2,kind);
      const auto row_bytes=read_number(malformed,row_start+56,4);
      Number(malformed,row_start+64,8,0);const auto hash=BodyFnv(malformed.data()+row_start,row_bytes);
      Number(malformed,row_start+64,8,hash);
      const auto directory=read_number(malformed,216,4)+128;Number(malformed,directory+16,8,hash);
      BodySeal(malformed);refused(page::DecodeNativeRowDataPage(malformed,receiver));
    }
    // A generic V1/V2 route remains refused even after a native row positive.
    dt::DatatypeBinaryValue legacy;legacy.type_id=dt::CanonicalTypeId::blob;legacy.payload={0,255};
    const auto generic=dt::EncodeDatatypeBinaryValue(legacy);
    Check(!generic.ok()&&generic.diagnostic.diagnostic_code=="BLOB.V1_V2_REFUSED", "native row enabled generic BLOB");
    auto oversized=p;oversized.body.rows[0].cells[3].value.payload.resize(p.header.page_size_bytes);
    refused(page::EncodeNativeRowDataPage(oversized,receiver));
    auto empty=p;empty.body.rows.clear();auto empty_bytes=page::EncodeNativeRowDataPage(empty,receiver);
    Check(empty_bytes.ok()&&empty_bytes.bytes==RowOracle(empty,13,columns)&&
        page::DecodeNativeRowDataPage(empty_bytes.bytes,receiver).ok(),"empty BLOB-profile page lost binding");
    for(unsigned operation=0;operation<2;++operation){bool finished=false;unsigned faults=0;
      for(long point=0;point<200;++point){codec_fault::remaining=point;codec_fault::fired=false;
        auto result=operation?page::DecodeNativeRowDataPage(expected,receiver):page::EncodeNativeRowDataPage(p,receiver);
        const bool fired=codec_fault::fired;codec_fault::remaining=-1;
        if(fired){++faults;refused(result);}else{Check(result.ok(),"unfaulted native BLOB image failed");finished=true;break;}}
      Check(finished&&faults,"native BLOB allocation sweep incomplete");
    }
    disk::FilespacePageZero zero;zero.bootstrap.database_uuid=p.header.database_uuid;
    zero.bootstrap.filespace_uuid=p.header.filespace_uuid;zero.bootstrap.page_size_profile_uuid=p.header.page_size_profile_uuid;
    zero.bootstrap.page_size_bytes=p.header.page_size_bytes;zero.bootstrap.filespace_role=5;zero.bootstrap.lifecycle_state=1;
    zero.bootstrap.checksum_profile_uuid=disk::kNativeBootstrapIntegrityProfile;
    zero.page_uuid=Id(81);zero.creation_operation_uuid=Id(82);zero.writer_identity_uuid=Id(83);
    zero.page_generation=1;zero.root_set_generation=1;zero.total_pages=32;zero.free_pages=9;zero.creation_utc_millis=1790000000000ull;
    zero.roots.push_back({3,3,p.header.filespace_uuid,13,1,p.header.page_size_profile_uuid,Id(84)});
    const auto bootstrap=disk::EncodeFilespacePageZero(zero);Check(bootstrap.ok(),"BLOB fixture native pagezero");if(!bootstrap.ok())continue;
    const auto path=(cleanup.path/std::to_string(size)).string();disk::FileDevice file;
    Check(file.Open(path,disk::FileOpenMode::create_new).ok(),"BLOB fixture own device");
    const byte padding=0;
    Check(file.WriteAt(0,bootstrap.bytes->data(),bootstrap.bytes->size()).ok()&&
        file.WriteAt(zero.total_pages*p.header.page_size_bytes-1,&padding,1).ok()&&
        file.WriteAt(p.header.page_number*p.header.page_size_bytes,expected.data(),expected.size()).ok()&&file.Sync().ok(),
        "BLOB exact native image physical write/sync");
    Check(file.Close().ok()&&file.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"BLOB independent read-only reopen");
    const page::NativeRowDataReference reference{p.body.relation_uuid.value,
        {p.header.filespace_uuid,p.header.page_number,p.header.page_generation,p.header.page_size_profile_uuid},p.header.page_uuid};
    auto reopened=page::ReadNativeRowDataPageFromOpenDevice(file,p.header.database_uuid,reference,receiver);
    Check(reopened.ok()&&reopened.bytes==expected,"BLOB actual owned-device reopen lost native image");
    refused(page::ReadNativeRowDataPageFromOpenDevice(file,p.header.database_uuid,reference));
    auto stale=receiver;auto bindings=columns;++bindings[0].column_generation;stale.columns=bindings;
    refused(page::ReadNativeRowDataPageFromOpenDevice(file,p.header.database_uuid,reference,stale));
    Check(file.Close().ok(),"BLOB fixture release device");
  }
}
}
int main(){MetricSamplePageBaseMain();auto before=checks;Images();SemanticImages();PageFaults();ExternalImages();TemporalReceivers();BitReceivers();BlobReceivers();
  std::cout<<"native row image checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
