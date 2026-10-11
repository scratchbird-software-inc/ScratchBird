// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_database_record_codec.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_page.hpp"
#include "database_lifecycle.hpp"
#include "startup_state.hpp"
#include "page_header.hpp"
#include "transaction_inventory_page.hpp"
#include "uuid.hpp"
#include "admitted_datatype_cohort.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <cstdlib>
#include <new>
#include <stdexcept>
namespace allocation_probe { thread_local bool forbidden=false; }
void* operator new(std::size_t bytes) {
  if(allocation_probe::forbidden)throw std::bad_alloc();
  if(void* value=std::malloc(bytes?bytes:1))return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* value) noexcept{std::free(value);}
void operator delete[](void* value) noexcept{std::free(value);}
void operator delete(void* value,std::size_t) noexcept{std::free(value);}
void operator delete[](void* value,std::size_t) noexcept{std::free(value);}
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
namespace fs = std::filesystem;
unsigned checks=0, failures=0;
void Check(bool ok,const char* message) {
  ++checks;
  if (!ok && ++failures < 20) std::cerr << "FAIL " << message << '\n';
}
void Setup(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
c::CatalogDatabaseRecordDecodeResult DecodeNoAllocation(std::string_view bytes) {
  allocation_probe::forbidden=true;
  c::CatalogDatabaseRecordDecodeResult result;
  try{result=c::DecodeCatalogDatabaseRecord(bytes);}
  catch(...){allocation_probe::forbidden=false;throw;}
  allocation_probe::forbidden=false;
  return result;
}
std::string Bytes(const c::CatalogValueEncodeResult& r) { return {r.bytes.begin(),r.bytes.end()}; }
void Put(std::string& s,std::size_t offset,p::u64 value,unsigned count) {
  for(unsigned i=0;i<count;++i) s[offset+i]=static_cast<char>((value>>(8*i))&255);
}
std::string Golden(const c::CatalogDatabaseRecord& r) {
  // Independent fixed offsets and type tags from Core, not the encoder.
  std::string s(176,'\0');s.replace(0,4,"SBCV");
  Put(s,4,1,2);Put(s,6,24,2);Put(s,8,176,4);Put(s,12,9,4);
  Put(s,16,65537,4);Put(s,20,1,2);
  Put(s,24,1,2);Put(s,26,5,1);Put(s,28,16,4);
  s.replace(32,16,reinterpret_cast<const char*>(r.database_uuid.value.bytes.data()),16);
  const std::array<p::u64,8> values{r.database_header_format_major,r.database_header_format_minor,
      r.catalog_manifest_format_version,r.page_size,r.creation_unix_epoch_millis,
      r.feature_flags,r.compatibility_flags,r.creator_transaction_number};
  for(unsigned i=0;i<8;++i) {
    auto offset=48+i*16;Put(s,offset,i+2,2);Put(s,offset+2,1,1);
    Put(s,offset+4,8,4);Put(s,offset+8,values[i],8);
  }
  if(r.datatype_cohort) {
    s.resize(232,'\0');Put(s,8,232,4);Put(s,12,12,4);Put(s,20,2,2);
    Put(s,176,10,2);Put(s,178,5,1);Put(s,180,16,4);
    s.replace(184,16,reinterpret_cast<const char*>(
        r.datatype_cohort->catalog_snapshot_uuid.value.bytes.data()),16);
    Put(s,200,11,2);Put(s,202,1,1);Put(s,204,8,4);
    Put(s,208,r.datatype_cohort->catalog_generation,8);
    Put(s,216,12,2);Put(s,218,1,1);Put(s,220,8,4);
    Put(s,224,r.datatype_cohort->registry_generation,8);
  }
  return s;
}
void Roundtrip(const c::CatalogDatabaseRecord& r) {
  const auto encoded=c::EncodeCatalogDatabaseRecord(r);
  Check(encoded.ok(),"valid payload encode");
  if(!encoded.ok())return;
  Check(Bytes(encoded)==Golden(r),"independent complete payload byte oracle");
  const auto decoded=DecodeNoAllocation(Bytes(encoded));
  Check(decoded.ok(),"valid payload decode");
  if(decoded.ok()) {
    Check(Golden(*decoded.record)==Golden(r),"all fields preserved");
    Check(decoded.record->datatype_cohort==r.datatype_cohort,
          "cohort presence and exact tuple preserved without defaulting");
  }
}
void Refused(const std::string& bytes) {
  const auto d=DecodeNoAllocation(bytes);
  Check(!d.ok()&&!d.record.has_value(),"malformed payload returned authority");
}
void Codec(const c::CatalogDatabaseRecord& base) {
  Roundtrip(base);const auto valid=Golden(base);
  for(std::size_t size=0;size<valid.size();++size)Refused(valid.substr(0,size));
  Refused(valid+"x");Refused(std::string(176,'x'));
  for(unsigned kind=0;kind<256;++kind) {
    auto r=base;r.database_uuid.kind=static_cast<p::UuidKind>(kind);
    const auto encoded=c::EncodeCatalogDatabaseRecord(r);
    if(r.database_uuid.kind==p::UuidKind::database)Roundtrip(r);
    else Check(!encoded.ok()&&encoded.bytes.empty(),"wrong identity kind encode");
  }
  for(unsigned version=0;version<16;++version)for(unsigned variant=0;variant<4;++variant) {
    if(version==7&&variant==2)continue;
    auto r=base;
    r.database_uuid.value.bytes[6]=(r.database_uuid.value.bytes[6]&15)|(version<<4);
    r.database_uuid.value.bytes[8]=(r.database_uuid.value.bytes[8]&63)|(variant<<6);
    Check(!c::EncodeCatalogDatabaseRecord(r).ok(),"bad UUID policy encoded");
    Refused(Golden(r));
  }
  {auto r=base;r.database_uuid.value={};Check(!c::EncodeCatalogDatabaseRecord(r).ok(),"nil identity encoded");Refused(Golden(r));}
  for(unsigned field=0;field<9;++field) {
    const auto offset=field==0?24:48+(field-1)*16;
    for(unsigned type=0;type<256;++type) {
      if(type==(field==0?5:1))continue;
      auto s=valid;Put(s,offset+2,type,1);Refused(s);
    }
    auto s=valid;Put(s,offset,0,2);Refused(s);
    s=valid;Put(s,offset+3,1,1);Refused(s);
    s=valid;Put(s,offset+4,0xffffffffu,4);Refused(s);
  }
  for(unsigned offset : {0,4,6,8,12,16,20,22}) {
    auto s=valid;Put(s,offset,0xff,1);Refused(s);
  }
  for(unsigned index : {0,1,2,3}) {
    auto s=valid;Put(s,56+index*16,0x100000000ull,8);Refused(s);
  }
  for(unsigned index : {0,2,3,7}) {
    auto s=valid;Put(s,56+index*16,0,8);Refused(s);
  }
  auto limits=base;limits.creation_unix_epoch_millis=~p::u64{0};
  limits.feature_flags=~p::u64{0};limits.compatibility_flags=~p::u64{0};
  limits.creator_transaction_number=~p::u64{0};Roundtrip(limits);
}
void CohortCodec(const c::CatalogDatabaseRecord& base) {
  namespace dt=scratchbird::core::datatypes;
  const std::array<p::Uuid,11> cohorts{dt::kDatatypeCohortV1,dt::kDatatypeCohortV2,
      dt::kDatatypeCohortV3,dt::kDatatypeCohortV4,dt::kDatatypeCohortV5,
      dt::kDatatypeCohortV6,dt::kDatatypeCohortV7,dt::kDatatypeCohortV8,
      dt::kDatatypeCohortV9,dt::kDatatypeCohortV10,dt::kDatatypeCohortV11};
  for(std::size_t index=0;index<cohorts.size();++index) {
    auto r=base;
    r.datatype_cohort=c::CatalogDatabaseDatatypeCohort{
        {p::UuidKind::object,cohorts[index]},index+1,index+1};
    Roundtrip(r);
    const auto valid=Golden(r);
    for(std::size_t size=0;size<valid.size();++size)Refused(valid.substr(0,size));
    Refused(valid+"x");
    for(unsigned field=0;field<12;++field) {
      const unsigned offset=field==0?24:field<9?48+(field-1)*16:
          field==9?176:200+(field-10)*16;
      for(unsigned type=0;type<256;++type) {
        if(type==((field==0||field==9)?5:1))continue;
        auto bytes=valid;Put(bytes,offset+2,type,1);Refused(bytes);
      }
      auto bytes=valid;Put(bytes,offset,0,2);Refused(bytes);
      bytes=valid;Put(bytes,offset+3,1,1);Refused(bytes);
      bytes=valid;Put(bytes,offset+4,0xffffffffu,4);Refused(bytes);
    }
    for(unsigned offset:{0,4,6,8,12,16,20,22}) {
      auto bytes=valid;Put(bytes,offset,0xff,1);Refused(bytes);
    }
    // Every other registered generation is wrong for this binary snapshot.
    // Zero, overflow, an independently changed UUID and split generations
    // cannot become an admitted cohort or a partial decoded record.
    for(p::u64 generation:{p::u64{0},p::u64{12},~p::u64{0}}) {
      auto wrong=r;wrong.datatype_cohort->catalog_generation=generation;
      Check(!c::EncodeCatalogDatabaseRecord(wrong).ok(),"invalid cohort generation encoded");
      Refused(Golden(wrong));
      wrong=r;wrong.datatype_cohort->registry_generation=generation;
      Check(!c::EncodeCatalogDatabaseRecord(wrong).ok(),"invalid registry generation encoded");
      Refused(Golden(wrong));
    }
    for(std::size_t other=0;other<cohorts.size();++other)if(other!=index) {
      auto wrong=r;wrong.datatype_cohort->catalog_snapshot_uuid.value=cohorts[other];
      Check(!c::EncodeCatalogDatabaseRecord(wrong).ok(),"mismatched cohort UUID encoded");
      Refused(Golden(wrong));
    }
    for(unsigned bit=0;bit<128;++bit) {
      auto wrong=r;wrong.datatype_cohort->catalog_snapshot_uuid.value.bytes[bit/8]^=1u<<(bit%8);
      Check(!c::EncodeCatalogDatabaseRecord(wrong).ok(),"modified cohort identity encoded");
      Refused(Golden(wrong));
    }
    for(unsigned kind=0;kind<256;++kind)if(kind!=static_cast<unsigned>(p::UuidKind::object)) {
      auto wrong=r;wrong.datatype_cohort->catalog_snapshot_uuid.kind=static_cast<p::UuidKind>(kind);
      Check(!c::EncodeCatalogDatabaseRecord(wrong).ok(),"wrong cohort identity kind encoded");
    }
    auto bytes=valid;Put(bytes,216,11,2);Refused(bytes); // duplicate generation field
    bytes=valid;Put(bytes,20,1,2);Refused(bytes); // V2 bytes cannot claim V1
  }
  auto v1=Golden(base);Put(v1,20,2,2);Refused(v1); // V1 bytes cannot claim V2
  const auto legacy=c::DecodeCatalogDatabaseRecord(Golden(base));
  Check(legacy.ok()&&!legacy.record->datatype_cohort,
        "legacy database was implicitly assigned a current cohort");
}
std::string ReadAll(const std::string& path) {
  std::ifstream in(path,std::ios::binary);Setup(in.is_open(),"read fixture");
  return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
p::u64 Fnv(const std::string& s) {
  p::u64 v=1469598103934665603ull;
  for(unsigned char b:s) {v^=b;v*=1099511628211ull;}
  return v;
}
int main() {
  fs::path root;
  try {
    const auto millis=static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto id=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis);
    const auto fsid=uuid::GenerateEngineIdentityV7(p::UuidKind::filespace,millis);
    Setup(id.ok()&&fsid.ok(),"identity generation");
    root=fs::temp_directory_path()/("scratchbird_database_payload_"+uuid::UuidToString(id.value.value));
    Setup(fs::create_directory(root),"unique fixture directory");
    db::DatabaseCreateConfig cfg;
    cfg.path=(root/"primary.sdb").string();cfg.database_uuid=id.value;cfg.filespace_uuid=fsid.value;
    cfg.page_size=16384;cfg.creation_unix_epoch_millis=millis;
    cfg.allow_minimal_resource_bootstrap=true;cfg.require_resource_seed_pack=false;
    c::CatalogDatabaseRecord expected;
    expected.database_uuid=cfg.database_uuid;
    expected.database_header_format_major=disk::kScratchBirdDatabaseFormatMajor;
    expected.database_header_format_minor=disk::kScratchBirdDatabaseFormatMinor;
    expected.catalog_manifest_format_version=1;expected.page_size=cfg.page_size;
    expected.creation_unix_epoch_millis=cfg.creation_unix_epoch_millis;
    expected.feature_flags=cfg.feature_flags;expected.compatibility_flags=cfg.compatibility_flags;
    expected.creator_transaction_number=1;
    Codec(expected);
    CohortCodec(expected);
    const auto created=db::CreateDatabaseFile(cfg);
    if(!created.ok())std::cerr<<created.diagnostic.diagnostic_code<<'\n';
    Setup(created.ok(),"actual database create failed");
    db::DatabaseOpenConfig open;open.path=cfg.path;open.read_only=true;open.suppress_background_agents=true;
    const auto opened=db::OpenDatabaseFile(open);
    if(!opened.ok())std::cerr<<opened.diagnostic.diagnostic_code<<'\n';
    Check(opened.ok(),"fresh database opens with binary identity payload");
    const auto before_identity_read=ReadAll(cfg.path);
    const auto owned=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
    if(!owned.ok())std::cerr<<owned.diagnostic.diagnostic_code<<'\n';
    Check(owned.ok()&&!owned.record->datatype_cohort,
          "owned V1 read must preserve absence, never infer D711");
    if(owned.ok())Check(Golden(*owned.record)==Golden(expected),"owned record differs from durable bytes");
    Check(ReadAll(cfg.path)==before_identity_read,"owned identity read changed database");
    const auto foreign_owner=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis+1);
    Setup(foreign_owner.ok(),"foreign identity generation");
    const auto foreign_read=db::ReadDatabaseCatalogIdentity(cfg.path,foreign_owner.value.value);
    Check(!foreign_read.ok()&&!foreign_read.record,"foreign node yielded cohort authority");
    const auto absent_owner=db::ReadDatabaseCatalogIdentity(cfg.path,{});
    Check(!absent_owner.ok()&&!absent_owner.record,"absent node identity yielded authority");
    p::u64 found_page=0;std::size_t found_offset=0;
    std::vector<p::byte> original;
    {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"open real device");
      p::u64 number=db::kCatalogPageNumber;
      for(unsigned guard=0;number!=0&&guard<1000;++guard) {
        std::vector<p::byte> body(cfg.page_size-disk::kPageHeaderSerializedBytes);
        Setup(device.ReadAt(number*cfg.page_size+disk::kPageHeaderSerializedBytes,body.data(),body.size()).ok(),"read catalog page");
        const auto parsed=page::ParseCatalogPageBody(body,number);Setup(parsed.ok(),"parse real page");
        std::size_t offset=page::kCatalogPageBodyHeaderBytes;
        for(const auto& row:parsed.body.rows) {
          if(row.kind==page::CatalogPageRowKind::typed_catalog_record) {
            const auto record=c::DecodeCatalogTypedRecord(row);Setup(record.ok(),"decode actual catalog record");
            if(record.record.header.kind==c::CatalogRecordKind::database) {
              Setup(found_page==0,"duplicate database record");
              found_page=number;found_offset=offset;original=body;
              Check(record.record.payload==Golden(expected),"actual persisted database payload differs from independent oracle");
              Check(record.record.payload.find(uuid::UuidToString(cfg.database_uuid.value))==std::string::npos,
                    "database payload still stores identity as text");
            }
          }
          offset+=20+row.payload.size();
        }
        number=parsed.body.next_page_number;
      }
    }
    Setup(found_page!=0,"actual database record missing");
    const auto write = [&](const std::vector<p::byte>& body) {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"mutation device open");
      Setup(device.WriteAt(found_page*cfg.page_size+disk::kPageHeaderSerializedBytes,body.data(),body.size()).ok(),"test page mutation");
      Setup(device.Sync().ok(),"test page sync");
    };
    const auto mutate = [&](std::string payload) {
      auto body=original;
      const auto row_size=p::LoadLittle32(body.data()+found_offset+8);
      std::string record(reinterpret_cast<const char*>(body.data()+found_offset+20),row_size);
      Setup(payload.size()==176&&record.size()==96+176,"fixed fixture size");
      record.replace(96,176,payload);
      std::copy(record.begin(),record.end(),body.begin()+found_offset+20);
      p::StoreLittle64(body.data()+found_offset+12,Fnv(record));
      p::StoreLittle64(body.data()+40,page::ComputeCatalogPageBodyChecksum(body));
      Setup(page::ParseCatalogPageBody(body,found_page).ok(),"mutated page must retain valid checksums");
      write(body);
      const auto before=ReadAll(cfg.path);
      const auto result=db::OpenDatabaseFile(open);
      Check(!result.ok(),"checksum-valid invalid database payload admitted");
      const auto invalid_identity=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
      Check(!invalid_identity.ok()&&!invalid_identity.record,
            "owned reader returned malformed or foreign database authority");
      Check(ReadAll(cfg.path)==before,"refused read-only admission changed database");
      write(original);
      Check(db::OpenDatabaseFile(open).ok(),"valid identity payload did not recover after refusal");
    };
    {auto foreign=expected;foreign.database_uuid=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis+1).value;
      mutate(Golden(foreign));}
    {auto bytes=Golden(expected);bytes[16]^=1;mutate(bytes);}
    {auto bytes=Golden(expected);bytes[32+6]&=15;mutate(bytes);}
    mutate(std::string("database_uuid=legacy\n")+std::string(155,'x'));
    // Real-file V2 source fixture. This is deliberately a physical fixture
    // construction, not a claim that rewriting bytes is a migration API.
    const auto replace_payload = [&](const std::string& payload, bool deleted=false,
                                     std::optional<std::size_t> target=std::nullopt) {
      const auto offset=target.value_or(found_offset);
      const auto old_size=p::LoadLittle32(original.data()+offset+8);
      const auto template_size=p::LoadLittle32(original.data()+found_offset+8);
      const auto old_used=p::LoadLittle32(original.data()+24);
      const auto typed=c::DecodeCatalogTypedRecordView(page::CatalogPageRowKind::typed_catalog_record,
          {reinterpret_cast<const char*>(original.data()+found_offset+20),template_size});
      Setup(typed.ok(),"original record view");
      c::CatalogTypedRecord replacement{typed.record->header,payload};
      replacement.header.deleted=deleted;
      const auto encoded=c::EncodeCatalogTypedRecord(replacement,p::LoadLittle32(original.data()+offset+4));
      Setup(encoded.ok(),"replacement record encode");
      const auto& record=encoded.row.payload;
      Setup(old_used+record.size()>=old_size&&old_used+record.size()-old_size<=original.size(),
            "V2 fixture needs bounded free catalog-page bytes");
      std::vector<p::byte> body(original.size());
      const auto start=offset+20;
      std::copy_n(original.begin(),start,body.begin());
      std::copy(record.begin(),record.end(),body.begin()+start);
      std::copy(original.begin()+start+old_size,original.begin()+old_used,
                body.begin()+start+record.size());
      p::StoreLittle32(body.data()+offset+8,record.size());
      p::StoreLittle64(body.data()+offset+12,Fnv(record));
      p::StoreLittle32(body.data()+24,old_used+record.size()-old_size);
      p::StoreLittle64(body.data()+40,page::ComputeCatalogPageBodyChecksum(body));
      Setup(page::ParseCatalogPageBody(body,found_page).ok(),"V2 fixture body checksum");
      write(body);
    };
    auto bound=expected;
    bound.datatype_cohort=c::CatalogDatabaseDatatypeCohort{
        {p::UuidKind::object,scratchbird::core::datatypes::kDatatypeCohortV11},11,11};
    replace_payload(Golden(bound));
    const auto before_v2=ReadAll(cfg.path);
    const auto durable_v2=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
    Check(durable_v2.ok()&&durable_v2.record->datatype_cohort==bound.datatype_cohort,
          "owned V2 read lost exact persisted cohort");
    Check(ReadAll(cfg.path)==before_v2,"owned V2 read changed database");
    for(unsigned variant=0;variant<3;++variant) {
      auto bad=bound;
      if(variant==0)bad.datatype_cohort->registry_generation=10;
      if(variant==1)bad.creator_transaction_number=2;
      replace_payload(Golden(bad),variant==2);
      const auto before=ReadAll(cfg.path);
      const auto refused=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
      Check(!refused.ok()&&!refused.record,"invalid owned V2 record returned authority");
      Check(ReadAll(cfg.path)==before,"invalid owned V2 refusal changed database");
    }
    write(original);

    {
      const auto parsed=page::ParseCatalogPageBody(original,found_page);
      Setup(parsed.ok(),"duplicate fixture source");
      std::size_t offset=page::kCatalogPageBodyHeaderBytes;
      std::optional<std::size_t> victim;
      for(const auto& row:parsed.body.rows) {
        if(offset!=found_offset&&row.kind==page::CatalogPageRowKind::typed_catalog_record&&
            row.payload.size()>=96+176) {victim=offset;break;}
        offset+=20+row.payload.size();
      }
      Setup(victim.has_value(),"bounded duplicate fixture row required");
      replace_payload(Golden(expected),false,victim);
      const auto before=ReadAll(cfg.path);
      const auto duplicate=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
      Check(!duplicate.ok()&&!duplicate.record,"duplicate database record returned authority");
      Check(ReadAll(cfg.path)==before,"duplicate refusal changed database");
      write(original);
    }

    // A valid body does not authorize an overflow page belonging to another
    // owner, location or page family. Every chain header must be admitted.
    disk::SerializedPageHeader overflow_original{};
    p::u64 overflow=0;
    {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing_read_only).ok(),"overflow source open");
      std::vector<p::byte> root_body(cfg.page_size-disk::kPageHeaderSerializedBytes);
      Setup(device.ReadAt(db::kCatalogPageNumber*cfg.page_size+disk::kPageHeaderSerializedBytes,
                         root_body.data(),root_body.size()).ok(),"read catalog chain root");
      const auto root_page=page::ParseCatalogPageBody(root_body,db::kCatalogPageNumber);
      Setup(root_page.ok()&&root_page.body.next_page_number!=0,"actual catalog overflow required");
      overflow=root_page.body.next_page_number;
      Setup(device.ReadAt(overflow*cfg.page_size,overflow_original.data(),overflow_original.size()).ok(),"read overflow header");
    }
    const auto write_overflow_header=[&](const disk::SerializedPageHeader& header) {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"overflow mutation open");
      Setup(device.WriteAt(overflow*cfg.page_size,header.data(),header.size()).ok(),"overflow header mutation");
      Setup(device.Sync().ok(),"overflow header sync");
    };
    const auto parsed_overflow=disk::ParsePageHeader(overflow_original);
    Setup(parsed_overflow.ok(),"actual overflow header parse");
    for(unsigned variant=0;variant<6;++variant) {
      auto changed=parsed_overflow.header;
      if(variant==0)changed.database_uuid=foreign_owner.value.value;
      if(variant==1)changed.filespace_uuid=foreign_owner.value.value;
      if(variant==2)changed.page_type=disk::PageType::row_data;
      if(variant==3)++changed.page_number;
      if(variant==4)changed.flags|=disk::PageHeaderFlag::encrypted_payload;
      if(variant==5)changed.flags|=disk::PageHeaderFlag::cluster_only;
      const auto encoded=disk::SerializePageHeader(changed);Setup(encoded.ok(),"checksum-valid foreign overflow header");
      write_overflow_header(encoded.serialized);
      const auto before=ReadAll(cfg.path);
      const auto refused=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
      Check(!refused.ok()&&!refused.record,"wrong-owner/type/location overflow admitted");
      Check(!db::OpenDatabaseFile(open).ok(),"lifecycle open bypassed overflow header admission");
      Check(ReadAll(cfg.path)==before,"overflow refusal changed database");
      write_overflow_header(overflow_original);
    }
    Check(db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value).ok(),"owned reader did not recover after restoring header");
    {
      std::vector<p::byte> inventory_original(cfg.page_size-disk::kPageHeaderSerializedBytes);
      disk::SerializedDatabaseHeader database_original{};
      {
        disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing_read_only).ok(),"inventory fixture source");
        Setup(device.ReadAt(db::kTransactionInventoryPageNumber*cfg.page_size+disk::kPageHeaderSerializedBytes,
            inventory_original.data(),inventory_original.size()).ok(),"read owning inventory");
        Setup(device.ReadAt(0,database_original.data(),database_original.size()).ok(),"read owning database header");
      }
      const auto parsed=page::ParseTransactionInventoryPageBody(inventory_original,db::kTransactionInventoryPageNumber);
      Setup(parsed.ok()&&parsed.body.next_page_number==0,"bounded actual inventory fixture");
      const auto write_inventory=[&](const std::vector<p::byte>& bytes) {
        disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"inventory fixture mutation");
        Setup(device.WriteAt(db::kTransactionInventoryPageNumber*cfg.page_size+disk::kPageHeaderSerializedBytes,
            bytes.data(),bytes.size()).ok(),"write inventory fixture");
        Setup(device.Sync().ok(),"inventory fixture sync");
      };
      for(bool missing:{false,true}) {
        auto altered=parsed.body;
        auto entry=std::find_if(altered.inventory.entries.begin(),altered.inventory.entries.end(),
            [](const auto& e){return e.identity.local_id.value==1;});
        Setup(entry!=altered.inventory.entries.end(),"owning creator inventory record");
        if(missing)altered.inventory.entries.erase(entry);
        else {
          entry->state=scratchbird::transaction::mga::TransactionState::rolled_back;
          entry->commit_sequence=0;
        }
        altered.horizons={};
        const auto encoded=page::BuildTransactionInventoryPageBody(altered,cfg.page_size);
        Setup(encoded.ok(),"structurally valid noncommitted inventory fixture");
        write_inventory(encoded.serialized);
        const auto before=ReadAll(cfg.path);
        const auto refused=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
        Check(!refused.ok()&&!refused.record,"missing/uncommitted creator returned authority");
        Check(refused.diagnostic.diagnostic_code==(missing?"SB-DB-DATATYPE-CATALOG-TX-MISSING":
              "SB-DB-DATATYPE-CATALOG-TX-NOT-COMMITTED"),"creator refusal did not reach actual commitment check");
        Check(ReadAll(cfg.path)==before,"creator refusal changed database");
        write_inventory(inventory_original);
      }
      // Place the actual creator commitment only in a second inventory page.
      // Checksums/digests remain valid, so a foreign page must be rejected for
      // ownership, not incidentally because the root already proves commitment.
      const auto original_size=fs::file_size(cfg.path);
      Setup(original_size%cfg.page_size==0,"inventory append alignment");
      const p::u64 inventory_overflow=original_size/cfg.page_size;
      auto root_inventory=parsed.body;
      root_inventory.inventory.entries.clear();
      root_inventory.horizons={};
      root_inventory.next_page_number=inventory_overflow;
      auto tail_inventory=parsed.body;
      tail_inventory.page_number=inventory_overflow;
      tail_inventory.previous_page_number=db::kTransactionInventoryPageNumber;
      tail_inventory.horizons={};
      const auto root_encoded=page::BuildTransactionInventoryPageBody(root_inventory,cfg.page_size);
      const auto tail_encoded=page::BuildTransactionInventoryPageBody(tail_inventory,cfg.page_size);
      Setup(root_encoded.ok()&&tail_encoded.ok(),"valid two-page inventory body fixture");
      disk::PageHeader inventory_header;
      {
        disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing_read_only).ok(),"inventory root header source");
        disk::SerializedPageHeader bytes{};
        Setup(device.ReadAt(db::kTransactionInventoryPageNumber*cfg.page_size,bytes.data(),bytes.size()).ok(),"inventory root header read");
        const auto header=disk::ParsePageHeader(bytes);Setup(header.ok(),"inventory root header parse");
        inventory_header=header.header;
      }
      inventory_header.page_number=inventory_overflow;
      inventory_header.page_uuid=uuid::GenerateEngineIdentityV7(p::UuidKind::object,millis+2).value.value;
      const auto write_inventory_tail=[&](const disk::PageHeader& header) {
        const auto encoded=disk::SerializePageHeader(header);Setup(encoded.ok(),"inventory tail header checksum");
        disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"inventory tail write source");
        Setup(device.WriteAt(original_size,encoded.serialized.data(),encoded.serialized.size()).ok(),"inventory tail header write");
        Setup(device.WriteAt(original_size+disk::kPageHeaderSerializedBytes,
            tail_encoded.serialized.data(),tail_encoded.serialized.size()).ok(),"inventory tail body write");
        Setup(device.Sync().ok(),"inventory tail sync");
      };
      write_inventory_tail(inventory_header);
      write_inventory(root_encoded.serialized);
      Check(db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value).ok(),
            "valid multi-page creator commitment refused");
      for(unsigned variant=0;variant<4;++variant) {
        auto changed=inventory_header;
        if(variant==0)changed.database_uuid=foreign_owner.value.value;
        if(variant==1)changed.filespace_uuid=foreign_owner.value.value;
        if(variant==2)++changed.page_number;
        if(variant==3)changed.page_size=32768;
        write_inventory_tail(changed);
        const auto before=ReadAll(cfg.path);
        const auto refused=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
        Check(!refused.ok()&&!refused.record,"foreign inventory overflow supplied creator authority");
        Check(refused.diagnostic.diagnostic_code=="SB-DB-LIFECYCLE-INVENTORY-PAGE-IDENTITY-MISMATCH",
              "inventory overflow did not reach exact owner/location/profile check");
        Check(ReadAll(cfg.path)==before,"inventory overflow refusal changed database");
      }
      write_inventory_tail(inventory_header);
      Check(db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value).ok(),
            "restored multi-page commitment refused");
      write_inventory(inventory_original);
      fs::resize_file(cfg.path,original_size); // Remove only this fixture's appended page.
      const auto parsed_header=disk::ParseDatabaseHeader(database_original);
      Setup(parsed_header.ok(),"source database header");
      for(bool cluster:{false,true}) {
        auto header=parsed_header.header;
        header.feature_flags|=cluster?disk::DatabaseFeatureFlag::cluster_structures_present:
            disk::DatabaseFeatureFlag::encrypted_database;
        const auto encoded=disk::SerializeDatabaseHeader(header);Setup(encoded.ok(),"external-authority header fixture");
        const auto write_header=[&](const auto& bytes) {
          disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"database header fixture mutation");
          Setup(device.WriteAt(0,bytes.data(),bytes.size()).ok(),"database header fixture write");
          Setup(device.Sync().ok(),"database header fixture sync");
        };
        write_header(encoded.serialized);
        const auto before=ReadAll(cfg.path);
        const auto refused=db::ReadDatabaseCatalogIdentity(cfg.path,cfg.database_uuid.value);
        Check(!refused.ok()&&!refused.record,"external authority requirement bypassed");
        Check(ReadAll(cfg.path)==before,"external authority refusal changed source");
        write_header(database_original);
      }
    }
    if(failures) {std::cerr<<"fixture retained at "<<root<<'\n';return 1;}
    fs::remove_all(root); // Only the unique test-owned root created above.
    std::cout<<"checks="<<checks<<" failures="<<failures<<" real_database_payload=passed\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"; fixture retained at "<<root<<'\n';return 1;}
}
