// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_database_record_codec.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_page.hpp"
#include "database_lifecycle.hpp"
#include "startup_state.hpp"
#include "page_header.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
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
  return s;
}
void Roundtrip(const c::CatalogDatabaseRecord& r) {
  const auto encoded=c::EncodeCatalogDatabaseRecord(r);
  Check(encoded.ok(),"valid payload encode");
  if(!encoded.ok())return;
  Check(Bytes(encoded)==Golden(r),"independent complete payload byte oracle");
  const auto decoded=c::DecodeCatalogDatabaseRecord(Bytes(encoded));
  Check(decoded.ok(),"valid payload decode");
  if(decoded.ok())Check(Golden(*decoded.record)==Golden(r),"all fields preserved");
}
void Refused(const std::string& bytes) {
  const auto d=c::DecodeCatalogDatabaseRecord(bytes);
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
    const auto created=db::CreateDatabaseFile(cfg);
    if(!created.ok())std::cerr<<created.diagnostic.diagnostic_code<<'\n';
    Setup(created.ok(),"actual database create failed");
    db::DatabaseOpenConfig open;open.path=cfg.path;open.read_only=true;open.suppress_background_agents=true;
    const auto opened=db::OpenDatabaseFile(open);
    if(!opened.ok())std::cerr<<opened.diagnostic.diagnostic_code<<'\n';
    Check(opened.ok(),"fresh database opens with binary identity payload");
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
      Check(ReadAll(cfg.path)==before,"refused read-only admission changed database");
      write(original);
      Check(db::OpenDatabaseFile(open).ok(),"valid identity payload did not recover after refusal");
    };
    {auto foreign=expected;foreign.database_uuid=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis+1).value;
      mutate(Golden(foreign));}
    {auto bytes=Golden(expected);bytes[16]^=1;mutate(bytes);}
    {auto bytes=Golden(expected);bytes[32+6]&=15;mutate(bytes);}
    mutate(std::string("database_uuid=legacy\n")+std::string(155,'x'));
    if(failures) {std::cerr<<"fixture retained at "<<root<<'\n';return 1;}
    fs::remove_all(root); // Only the unique test-owned root created above.
    std::cout<<"checks="<<checks<<" failures="<<failures<<" real_database_payload=passed\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"; fixture retained at "<<root<<'\n';return 1;}
}
