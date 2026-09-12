// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_common_page_header.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {
long allocation_budget=-1;
unsigned reads=0;
std::size_t read_bytes=0;
off_t read_offset=-1;
bool track_reads=false, fail_read=false, grow_during_read=false;
}
void* operator new(std::size_t n) {
  if(allocation_budget==0) { allocation_budget=-1; throw std::bad_alloc(); }
  if(allocation_budget>0) --allocation_budget;
  if(auto* p=std::malloc(n?n:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* b,size_t n,off_t offset) {
  if(track_reads) { ++reads; read_bytes+=n; read_offset=offset; }
  if(fail_read) { fail_read=false; errno=EIO; return -1; }
  if(grow_during_read) {
    grow_during_read=false; const auto end=::lseek(fd,0,SEEK_END); const char zero=0;
    if(end<0||::pwrite(fd,&zero,1,end)!=1||::fsync(fd)) { errno=EIO; return -1; }
  }
  return __real_pread(fd,b,n,offset);
}

namespace {
namespace disk=scratchbird::storage::disk;
using disk::Uuid; using disk::byte; using disk::u64;
using Header=disk::NativeCommonPageHeader;
using Binding=disk::NativeCommonPageHeaderBinding;
using Error=disk::NativeCommonPageHeaderError;
using Bytes=std::array<byte,128>;
unsigned checks=0, executed_tuples=0;
void Check(bool ok,std::string_view why,std::source_location at=std::source_location::current()) {
  ++checks;
  if(!ok) throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));
}
// Independent explicit Core entries, not runtime ranges or enum casts.
constexpr std::array<unsigned,97> codes{{
  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
  256,257,258,259,260,261,262,263,264,
  512,513,514,515,516,517,518,519,520,521,522,523,524,525,526,
  768,769,770,771,772,773,774,775,776,777,778,779,780,781,
  1024,1025,1026,1027,1028,1029,1030,1031,1032,
  1280,1281,1282,1283,1284,1285,1286,1287,1288,1289,1290,1291,1292,
  1293,1294,1295,1296,1297,1298,1299,1300,1301,1302,1303,1304,
  1536,1537,1538,4094,4095}};
constexpr std::array<unsigned,5> sizes{{8192,16384,32768,65536,131072}};
constexpr std::array<std::array<byte,3>,5> suffixes{{
  {{0,0x81,0x92}},{{1,0x63,0x84}},{{3,0x27,0x68}},{{6,0x55,0x36}},{{0x13,0x10,0x72}}}};
Uuid Id(byte tag) { return Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,tag}}; }
Uuid Profile(unsigned p) { return Uuid{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,suffixes[p][0],suffixes[p][1],suffixes[p][2]}}; }
Header Example(unsigned profile=0,unsigned type=256,u64 flags=0) {
  Header h; h.page_size_bytes=sizes[profile]; h.page_type=type;
  h.database_uuid=Id(1); h.filespace_uuid=Id(2); h.page_uuid=Id(3);
  h.page_number=type==1||type==2?0:7; h.page_generation=1234; h.flags=flags;
  h.page_size_profile_uuid=Profile(profile); return h;
}
Binding Bind(const Header& h) {
  return {{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid},h.page_number,h.page_generation,h.page_type,h.page_uuid};
}
void Number(Bytes& b,unsigned offset,unsigned width,u64 value) {
  for(unsigned i=0;i<width;++i) b[offset+i]=static_cast<byte>(value>>(8*i));
}
void PutId(Bytes& b,unsigned offset,const Uuid& id) {
  for(unsigned i=0;i<16;++i) b[offset+i]=id.bytes[i];
}
void Seal(Bytes& b) {
  u64 checksum=14695981039346656037ull;
  for(unsigned i=0;i<128;++i) { checksum^=(i>=96&&i<104)?0:b[i]; checksum*=1099511628211ull; }
  Number(b,96,8,checksum);
}
Bytes Oracle(const Header& h) {
  Bytes b{}; const std::string_view magic="SBPGV002";
  for(unsigned i=0;i<8;++i) b[i]=static_cast<byte>(magic[i]);
  Number(b,8,4,128); Number(b,12,4,h.page_size_bytes); Number(b,16,4,h.page_type);
  Number(b,20,2,1); Number(b,22,2,1); PutId(b,24,h.database_uuid); PutId(b,40,h.filespace_uuid);
  PutId(b,56,h.page_uuid); Number(b,72,8,h.page_number); Number(b,80,8,h.page_generation);
  Number(b,88,8,h.flags); PutId(b,104,h.page_size_profile_uuid); Number(b,120,2,1);
  Seal(b); return b;
}
void Reject(const Bytes& bytes,Error error,const Binding* binding=nullptr) {
  const auto r=disk::DecodeNativeCommonPageHeader(bytes.data(),bytes.size(),binding);
  Check(!r.header&&r.error==error,"exact refusal with no partial header");
}
void Codecs() {
  for(unsigned type=0;type<=65536;++type) {
    const bool present=std::find(codes.begin(),codes.end(),type)!=codes.end();
    Check(disk::IsRegisteredNativePageType(type)==present,"complete uint16 namespace");
  }
  Check(!disk::IsRegisteredNativePageType(std::numeric_limits<unsigned>::max()),"wide sentinel not durable");
  for(unsigned p=0;p<5;++p) for(unsigned code:codes) for(unsigned flags=0;flags<16;++flags) {
    const auto h=Example(p,code,flags); const auto bytes=Oracle(h); const auto binding=Bind(h);
    const auto encoded=disk::EncodeNativeCommonPageHeader(h);
    Check(encoded.ok()&&*encoded.bytes==bytes,"exact independent bytes for every finite header tuple");
    const auto decoded=disk::DecodeNativeCommonPageHeader(bytes.data(),bytes.size(),&binding);
    Check(decoded.ok()&&Oracle(*decoded.header)==bytes,"all fields preserved with exact binding");
    for(unsigned other=0;other<5;++other) if(other!=p) {
      auto wrong=binding; wrong.filespace.page_size_profile_uuid=Profile(other);
      Reject(bytes,Error::binding_mismatch,&wrong);
    }
    ++executed_tuples;
  }
  Check(executed_tuples==7760,"independent expected finite tuple count");
  const auto h=Example(); const auto good=Oracle(h); const auto binding=Bind(h);
  for(unsigned bit=0;bit<1024;++bit) {
    auto b=good; b[bit/8]^=static_cast<byte>(1u<<(bit%8));
    const auto r=disk::DecodeNativeCommonPageHeader(b.data(),b.size());
    Check(!r.ok()&&!r.header,"every one-bit mutation refused");
  }
  for(unsigned n=0;n<128;++n) {
    const auto r=disk::DecodeNativeCommonPageHeader(good.data(),n);
    Check(!r.header&&r.error==Error::invalid_framing,"every truncation");
  }
  Check(!disk::DecodeNativeCommonPageHeader(nullptr,128).header,"null input");
  Check(!disk::DecodeNativeCommonPageHeader(good.data(),129).header,"oversized input never read");
  for(unsigned offset:{20u,22u,120u}) { auto b=good; Number(b,offset,2,2); Seal(b); Reject(b,Error::invalid_extension); }
  for(unsigned offset=122;offset<128;++offset) { auto b=good; b[offset]=1; Seal(b); Reject(b,Error::reserved_nonzero); }
  for(unsigned bit=4;bit<64;++bit) { auto b=good; Number(b,88,8,u64{1}<<bit); Seal(b); Reject(b,Error::unknown_flags); }
  for(unsigned offset:{24u,40u,56u}) {
    for(unsigned version=0;version<16;++version) if(version!=7) {
      auto b=good; b[offset+6]=static_cast<byte>((version<<4)|1); Seal(b); Reject(b,Error::invalid_identity);
    }
    for(unsigned variant:{0u,1u,3u}) { auto b=good; b[offset+8]=static_cast<byte>(variant<<6); Seal(b); Reject(b,Error::invalid_identity); }
    auto b=good; PutId(b,offset,Uuid{}); Seal(b); Reject(b,Error::invalid_identity);
  }
  for(unsigned p=1;p<5;++p) { auto b=good; PutId(b,104,Profile(p)); Seal(b); Reject(b,Error::unsupported_profile); }
  { auto b=good; Number(b,12,4,4096); Seal(b); Reject(b,Error::unsupported_profile); }
  { auto b=good; PutId(b,104,Id(88)); Seal(b); Reject(b,Error::unsupported_profile); }
  { auto b=good; Number(b,16,4,65536); Seal(b); Reject(b,Error::unregistered_type); }
  { auto b=good; Number(b,16,4,20); Seal(b); Reject(b,Error::unregistered_type); }
  { auto b=good; Number(b,72,8,0); Seal(b); Reject(b,Error::invalid_page_number); }
  { auto b=good; Number(b,16,4,1); Seal(b); Reject(b,Error::invalid_page_number); }
  { auto b=good; Number(b,80,8,0); Seal(b); Reject(b,Error::invalid_generation); }
  for(unsigned field=0;field<6;++field) {
    auto wrong=binding;
    switch(field) {
      case 0: wrong.filespace.database_uuid=Id(99); break;
      case 1: wrong.filespace.filespace_uuid=Id(99); break;
      case 2: wrong.page_uuid=Id(99); break;
      case 3: ++wrong.page_number; break;
      case 4: ++wrong.page_generation; break;
      case 5: ++wrong.page_type; break;
    }
    Reject(good,Error::binding_mismatch,&wrong);
  }
  { auto b=binding; b.page_generation=0; Reject(good,Error::invalid_binding,&b); }
  { auto b=binding; b.page_uuid=Uuid{}; Reject(good,Error::invalid_binding,&b); }
  { auto b=binding; b.page_uuid.reset(); Check(disk::DecodeNativeCommonPageHeader(good.data(),128,&b).ok(),"PageRef without independently known page UUID"); }
  // B.1 requires expected identity checks before registry/family selection.
  { auto b=good; Number(b,16,4,20); Seal(b); auto wrong=binding; wrong.filespace.database_uuid=Id(99);
    Reject(b,Error::binding_mismatch,&wrong); }
  { auto b=good; Number(b,16,4,20); b[24+6]=0x41; Seal(b); Reject(b,Error::invalid_identity); }
  { auto b=good; Number(b,88,8,16); PutId(b,104,Id(99)); Seal(b); Reject(b,Error::unknown_flags); }
  { auto b=good; PutId(b,104,Id(99)); b[24+6]=0x41; Seal(b); Reject(b,Error::unsupported_profile); }
  { auto b=good; b[122]=1; Number(b,88,8,16); Seal(b); Reject(b,Error::reserved_nonzero); }
  { auto b=good; b[122]=1; Reject(b,Error::checksum_mismatch); }
  { auto high=h; high.page_generation=std::numeric_limits<u64>::max(); const auto b=Oracle(high);
    const auto bound=Bind(high); Check(disk::DecodeNativeCommonPageHeader(b.data(),128,&bound).ok(),"nonzero generation uses full64bits without wrap"); }
  allocation_budget=0;
  const auto encoded=disk::EncodeNativeCommonPageHeader(h);
  const auto decoded=disk::DecodeNativeCommonPageHeader(good.data(),128,&binding);
  const bool untouched=allocation_budget==0; allocation_budget=-1;
  Check(encoded.ok()&&decoded.ok()&&untouched,"valid codec has no allocations");
  // Positive controls ensure corruption tests do not merely reject everything.
  for(unsigned p=0;p<5;++p) Check(disk::EncodeNativeCommonPageHeader(Example(p)).ok(),"valid profiles still admitted");
}
bool Locked(const disk::IoResult& r) {
  return !r.ok()&&(r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-OWNER-LOCK-HELD"
      ||r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD"
      ||r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD");
}
void Exclusive(const std::string& path) {
  disk::FileDevice second; Check(Locked(second.Open(path,disk::FileOpenMode::open_existing)),"same-process second open excluded");
  const auto pid=::fork(); Check(pid>=0,"fork ownership probe");
  if(pid==0) { ::execl("/proc/self/exe","common_test","--probe",path.c_str(),nullptr); ::_exit(125); }
  int status=0; Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process excluded");
}
void Files(const std::filesystem::path& root) {
  std::array<disk::FileDevice,5> devices;
  for(unsigned p=0;p<5;++p) {
    auto& device=devices[p]; const auto path=(root/std::to_string(p)).string();
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own independent filespace fixture");
    const byte zero=0;
    Check(device.WriteAt(8ull*sizes[p]-1,&zero,1).ok(),"full physical extent");
    for(unsigned type:{1u,256u}) {
      const auto h=Example(p,type); const auto bytes=Oracle(h); const auto binding=Bind(h);
      const u64 offset=type==1?4096:7ull*sizes[p];
      Check(device.WriteAt(offset,bytes.data(),bytes.size()).ok()&&device.Sync().ok(),"persist independent oracle header");
      reads=0; read_bytes=0; read_offset=-1; track_reads=true;
      auto r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,binding); track_reads=false;
      Check(r.ok()&&Oracle(*r.header)==bytes&&reads==1&&read_bytes==128&&read_offset==static_cast<off_t>(offset),"actual addressed profile and exact header read");
      auto wrong=binding; wrong.page_generation++;
      r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,wrong);
      Check(!r.header&&r.error==Error::binding_mismatch,"actual stale generation rejected");
      fail_read=true; r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,binding);
      Check(!r.header&&r.error==Error::io_failure&&!fail_read,"actual pread EIO consumed");
      allocation_budget=0; fail_read=true;
      r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,binding);
      allocation_budget=-1;
      Check(!r.header&&r.error==Error::resource_exhausted&&!fail_read,"diagnostic allocation failure contained");
      Exclusive(path);
    }
  }
  // All five differently sized files remain owned simultaneously.
  for(unsigned p=0;p<5;++p) {
    auto& device=devices[p]; const auto path=(root/std::to_string(p)).string();
    const auto h=Example(p); const auto binding=Bind(h);
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"readonly reopen");
    Check(disk::ReadNativeCommonPageHeaderFromOpenDevice(device,binding).ok()&&device.read_only(),"readonly actual header");
    Exclusive(path); Check(device.Close().ok(),"explicit ownership release");
    auto r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,binding);
    Check(!r.header&&r.error==Error::device_not_open,"closed device rejected");
    Check(device.Open(path,disk::FileOpenMode::open_existing).ok(),"reopen for owned fault fixture");
    auto bad=binding; bad.page_number=std::numeric_limits<u64>::max(); reads=0; track_reads=true;
    r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,bad); track_reads=false;
    Check(!r.header&&r.error==Error::invalid_extent&&reads==0,"address overflow before IO");
    bad=binding; bad.page_number=8; r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,bad);
    Check(!r.header&&r.error==Error::invalid_extent,"one page beyond actual EOF");
    grow_during_read=true; r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,binding);
    Check(!r.header&&r.error==Error::invalid_extent&&!grow_during_read,"real intervening file growth rejected");
    r=disk::ReadNativeCommonPageHeaderFromOpenDevice(device,binding);
    Check(!r.header&&r.error==Error::invalid_extent,"partial trailing page not ignored");
    Exclusive(path); Check(device.Close().ok(),"release fault fixture");
  }
}
}  // namespace
int main(int argc,char** argv) {
  if(argc==3&&std::string_view(argv[1])=="--probe") { disk::FileDevice d; return Locked(d.Open(argv[2],disk::FileOpenMode::open_existing))?0:1; }
  if(argc==2&&std::string_view(argv[1])=="--registered-types") {
    for(unsigned type=0;type<65536;++type) if(disk::IsRegisteredNativePageType(type)) std::cout<<type<<'\n';
    return 0;
  }
  std::string root;
  try {
    std::cout<<"expected_header_tuples=7760;namespace_values=65537;metadata_only=true\n";
    Codecs(); char name[]="/tmp/sbnch_XXXXXX"; auto* dir=::mkdtemp(name);
    Check(dir!=nullptr,"create isolated fixture root"); root=dir;
    Files(root);
    std::error_code error; std::filesystem::remove_all(root,error);
    Check(!error&&!std::filesystem::exists(root),"verified owned fixture cleanup");
    std::cout<<"PASS checks="<<checks<<" executed_header_tuples="<<executed_tuples<<" family_admission=false\n";
    return 0;
  } catch(const std::exception& e) {
    allocation_budget=-1; std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<" retained_fixture="<<root<<'\n'; return 1;
  }
}
