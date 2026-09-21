// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual companion storage test. This does not qualify row/MGA/SQL execution.
#include "mga_relation_store/mga_bulk_import_publication.hpp"
#include "core/hash/hash_digest.hpp"
#include "core/uuid/uuid.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <span>
#include <cstdlib>
#include <new>
#include <thread>
#include <atomic>
using namespace scratchbird::engine::internal_api;
using Bytes=std::vector<std::uint8_t>;
using Sha=MgaBulkImportSha256V1;
using Pub=MgaBulkImportPublicationRecordV1;
using Event=MgaBulkImportImportedRowEventV1;
using Life=MgaBulkImportPublicationLifecycleV1;
static unsigned checks=0;
static long fail_allocation=-1;
static bool allocation_injected=false;
static std::atomic<unsigned long> allocations{0};
void* operator new(std::size_t n) {
  ++allocations;
  if(fail_allocation==0) {fail_allocation=-1;allocation_injected=true;throw std::bad_alloc();}
  if(fail_allocation>0)--fail_allocation;
  if(void* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
static void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
static EngineUuid Id(unsigned n) {
  EngineUuid u{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,0}};
  u.bytes[14]=n>>8;u.bytes[15]=n;return u;
}
static Sha Hash(std::string_view domain,std::span<const std::uint8_t> data) {
  Bytes b(domain.begin(),domain.end());b.insert(b.end(),data.begin(),data.end());
  auto r=scratchbird::core::hash::ComputeSha256Digest(b);
  Check(r.ok(),"reference hash");return r.digest;
}
static Sha Filled(unsigned n){Sha h;h.fill(n);return h;}
static void Num(Bytes& b,std::size_t at,std::uint64_t n,unsigned width=8) {
  for(unsigned i=0;i<width;++i)b.at(at+i)=n>>(8*i);
}
static std::uint64_t Num(const Bytes& b,std::size_t at,unsigned width=8) {
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(b.at(at+i))<<(i*8);return n;
}
template<class A> static void Put(Bytes& b,std::size_t at,const A& a) {
  Check(at+a.size()<=b.size(),"reference bounds");std::copy(a.begin(),a.end(),b.begin()+at);
}
static Bytes Read(const std::string& path) {
  std::ifstream f(path,std::ios::binary);
  Check(bool(f),"fixture read");return Bytes(std::istreambuf_iterator<char>(f),{});
}
static void Write(const std::string& path,const Bytes& b) {
  std::ofstream f(path,std::ios::binary|std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()),b.size());f.close();Check(bool(f),"fixture write");
}
// Independent fixed-offset Core oracle; no production companion codec calls.
static Bytes RowWire(const Event& r) {
  Bytes b(388,0);Num(b,0,1,2);
  Put(b,4,r.durable_publication_uuid.bytes);Num(b,20,r.durable_publication_generation);
  Put(b,28,r.recovery_idempotency_key);Put(b,60,r.mutation_uuid.bytes);Put(b,76,r.bulk_batch_uuid.bytes);
  Put(b,92,r.owning_transaction_uuid.bytes);Num(b,108,r.owning_local_transaction_id);
  Put(b,116,r.statement_uuid.bytes);Num(b,132,r.savepoint_ordinal);
  Put(b,140,r.target_relation_uuid.bytes);Num(b,156,r.target_relation_generation);Num(b,164,r.import_ordinal);
  Put(b,172,r.row_uuid.bytes);Put(b,188,r.row_version_uuid.bytes);Put(b,204,r.row_image_uuid.bytes);
  Num(b,220,r.row_image_metadata_generation);Put(b,228,r.row_image_domain_hash);Put(b,260,r.row_image_value_hash);
  Put(b,292,r.column_descriptor_set_sha256);Put(b,324,r.canonical_typed_field_vector_sha256);
  Put(b,356,Hash("ScratchBird.BulkImportStreamImportedRowEvent.V1",{b.data(),356}));return b;
}
static Bytes PubWire(const Pub& r) {
  Bytes b(612,0);Num(b,0,1,2);Put(b,4,r.durable_publication_uuid.bytes);Num(b,20,r.durable_publication_generation);
  Put(b,28,r.recovery_idempotency_key);Put(b,60,r.stream_uuid.bytes);Num(b,76,r.stream_generation);
  Put(b,84,r.descriptor_evidence);Put(b,116,r.target_relation_uuid.bytes);Num(b,132,r.target_relation_generation);
  Put(b,140,r.owning_transaction_uuid.bytes);Num(b,156,r.owning_local_transaction_id);
  Put(b,164,r.authenticated_receipt_uuid.bytes);Put(b,180,r.statement_uuid.bytes);Num(b,196,r.savepoint_ordinal);
  Put(b,204,r.mutation_uuid.bytes);Put(b,220,r.bulk_batch_uuid.bytes);Put(b,236,r.content_sha256);
  Num(b,268,r.total_stream_bytes);Num(b,276,r.chunk_count);Num(b,284,r.input_row_count);
  Num(b,292,r.affected_rows);Num(b,300,r.rejected_rows);Num(b,308,r.imported_row_postcondition_count);
  Put(b,316,r.imported_row_postcondition_sha256);Put(b,348,r.normalized_statement_effect_sha256);
  Put(b,380,r.column_descriptor_set_sha256);Put(b,412,r.import_policy_bundle_sha256);
  Put(b,444,r.default_descriptor_set_sha256);Put(b,476,r.constraint_set_sha256);
  Put(b,508,r.trigger_set_sha256);Put(b,540,r.index_set_sha256);Num(b,572,r.executor_availability_generation);
  Put(b,580,Hash("ScratchBird.BulkImportStreamMgaPublicationRecord.V1",{b.data(),580}));return b;
}
struct Fixture {
  std::filesystem::path dir;EngineRequestContext c;Pub p;std::vector<Event> rows;
  std::string pubpath,rowpath;
  Fixture() {
    const auto uuid=scratchbird::core::uuid::IssueRuntimeIdentityV7();Check(uuid.has_value(),"fixture id");
    dir=std::filesystem::temp_directory_path()/("sb-bulk-companion-"+scratchbird::core::uuid::UuidToString(*uuid));
    Check(std::filesystem::create_directory(dir),"exclusive fixture");
    c.database_path=(dir/"node").string();Write(c.database_path,{0});
    c.database_uuid=Id(1);c.principal_uuid=Id(2);c.session_uuid=Id(3);
    c.statement_receipt_uuid=Id(4);c.transaction_uuid=Id(5);c.statement_uuid=Id(6);
    c.local_transaction_id=7;c.security_context_present=true;
    pubpath=c.database_path+".sb.mga_bulk_import_publication.v1";
    rowpath=c.database_path+".sb.mga_bulk_import_imported_rows.v1";
    p.durable_publication_uuid=Id(10);p.durable_publication_generation=1;p.recovery_idempotency_key=Filled(11);
    p.stream_uuid=Id(12);p.stream_generation=13;p.descriptor_evidence=Filled(14);
    p.target_relation_uuid=Id(15);p.target_relation_generation=16;p.owning_transaction_uuid=c.transaction_uuid;
    p.owning_local_transaction_id=c.local_transaction_id;p.authenticated_receipt_uuid=c.statement_receipt_uuid;
    p.statement_uuid=c.statement_uuid;p.savepoint_ordinal=17;p.mutation_uuid=Id(18);p.bulk_batch_uuid=Id(19);
    p.content_sha256=Filled(20);p.total_stream_bytes=21;p.chunk_count=22;
    p.input_row_count=p.affected_rows=p.imported_row_postcondition_count=2;
    p.normalized_statement_effect_sha256=Filled(23);p.column_descriptor_set_sha256=Filled(24);
    p.import_policy_bundle_sha256=Filled(25);p.default_descriptor_set_sha256=Filled(26);
    p.constraint_set_sha256=Filled(27);p.trigger_set_sha256=Filled(28);p.index_set_sha256=Filled(29);
    p.executor_availability_generation=30;
    for(unsigned i=0;i<2;++i) {
      Event r;r.durable_publication_uuid=p.durable_publication_uuid;r.durable_publication_generation=1;
      r.recovery_idempotency_key=p.recovery_idempotency_key;r.mutation_uuid=p.mutation_uuid;r.bulk_batch_uuid=p.bulk_batch_uuid;
      r.owning_transaction_uuid=c.transaction_uuid;r.owning_local_transaction_id=c.local_transaction_id;
      r.statement_uuid=c.statement_uuid;r.savepoint_ordinal=p.savepoint_ordinal;r.target_relation_uuid=p.target_relation_uuid;
      r.target_relation_generation=p.target_relation_generation;r.import_ordinal=i+1;
      r.row_uuid=Id(100+i*3);r.row_version_uuid=Id(101+i*3);r.row_image_uuid=Id(102+i*3);
      r.row_image_metadata_generation=31;r.row_image_domain_hash=r.column_descriptor_set_sha256=p.column_descriptor_set_sha256;
      r.row_image_value_hash=r.canonical_typed_field_vector_sha256=Filled(32+i);
      auto wire=RowWire(r);std::copy_n(wire.begin()+356,32,r.event_evidence_sha256.begin());rows.push_back(r);
    }
    Bytes material(24+rows.size()*40);Put(material,0,p.durable_publication_uuid.bytes);Num(material,16,rows.size());
    for(unsigned i=0;i<rows.size();++i){Num(material,24+i*40,rows[i].import_ordinal);Put(material,32+i*40,rows[i].event_evidence_sha256);}
    p.imported_row_postcondition_sha256=Hash("ScratchBird.BulkImportStreamImportedRows.V1",material);
    auto wire=PubWire(p);std::copy_n(wire.begin()+580,32,p.record_evidence_sha256.begin());
  }
  ~Fixture(){std::error_code ec;std::filesystem::remove_all(dir,ec);}
  void Prepare(){auto r=PrepareMgaBulkImportPublicationV1(c,p);Check(r.ok&&r.found&&r.record==p,"prepare actual store");}
  void Store(){auto r=StoreMgaBulkImportImportedRowEventsV1(c,rows);Check(r.ok&&r.events==rows,"store actual cohort");}
  void Publish(){auto r=PublishMgaBulkImportPublicationV1(c,p);Check(r.ok&&r.found&&r.record.lifecycle==Life::published_uncommitted,"publish actual store");}
};
// Verify frame chain and independent body evidence, not just success flags.
static void Verify(Fixture& f,const Bytes& b,bool rows,unsigned frames) {
  Check(b.size()>=32,"journal minimum");Check(std::equal(b.begin(),b.begin()+8,rows?"BIRJ2\0\0\0":"BIPJ2\0\0\0"),"journal magic");
  Check(Num(b,8,2)==2&&Num(b,10,2)==32&&Num(b,12,4)==(rows?2:1),"journal header");
  Check(std::equal(f.c.database_uuid.bytes.begin(),f.c.database_uuid.bytes.end(),b.begin()+16),"binary node");
  Sha prev{};std::size_t at=32;
  Bytes expected=rows?RowWire(f.rows[0]):PubWire(f.p);
  if(rows){auto second=RowWire(f.rows[1]);expected.insert(expected.end(),second.begin(),second.end());}
  for(unsigned i=0;i<frames;++i) {
    Check(at+208+expected.size()+64<=b.size(),"frame bound");
    Check(std::equal(b.begin()+at,b.begin()+at+8,"BICF2\0\0\0"),"frame magic");
    const auto total=Num(b,at+8);Check(total==208+expected.size()+64&&Num(b,at+16)==i+1,"frame extent sequence");
    Check(Num(b,at+128)==(rows?2:1)&&Num(b,at+136)==expected.size(),"whole cohort extent");
    Check(std::equal(prev.begin(),prev.end(),b.begin()+at+144),"frame chain");
    const auto header=Hash("ScratchBird.MgaBulkImportCompanionHeader.V2",{b.data()+at,176});
    Check(std::equal(header.begin(),header.end(),b.begin()+at+176),"header checksum");
    Check(std::equal(expected.begin(),expected.end(),b.begin()+at+208),"independent canonical payload");
    const auto foot=at+208+expected.size();
    Check(std::equal(b.begin()+foot,b.begin()+foot+8,"BICC2\0\0\0"),"commit footer");
    Check(Num(b,foot+8)==total&&Num(b,foot+16)==i+1&&Num(b,foot+24)==0,"footer binding");
    prev=Hash("ScratchBird.MgaBulkImportCompanionFrame.V2",{b.data()+at,static_cast<std::size_t>(total-32)});
    Check(std::equal(prev.begin(),prev.end(),b.begin()+foot+32),"commit hash");at+=total;
  }
  Check(at==b.size(),"no unexplained trailing bytes");
}
static void Basic() {
  Fixture f;
  Check(RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key).ok,"initial absent");
  Check(!PublishMgaBulkImportPublicationV1(f.c,f.p).ok,"no prepare no publish");
  Check(!StoreMgaBulkImportImportedRowEventsV1(f.c,f.rows).ok,"no publication no cohort");
  f.Prepare();const auto prepared=Read(f.pubpath);Verify(f,prepared,false,1);
  auto replay=PrepareMgaBulkImportPublicationV1(f.c,f.p);Check(replay.ok&&replay.replayed&&Read(f.pubpath)==prepared,"prepare replay");
  Check(!PublishMgaBulkImportPublicationV1(f.c,f.p).ok&&Read(f.pubpath)==prepared,"no rows no publication");
  auto partial=f.rows;partial.pop_back();Check(!StoreMgaBulkImportImportedRowEventsV1(f.c,partial).ok,"partial cohort refusal");
  auto dup=f.rows;dup[1].row_uuid=dup[0].row_uuid;Check(!StoreMgaBulkImportImportedRowEventsV1(f.c,dup).ok,"duplicate row refusal");
  f.Store();const auto rows=Read(f.rowpath);Verify(f,rows,true,1);
  auto again=StoreMgaBulkImportImportedRowEventsV1(f.c,f.rows);Check(again.ok&&again.replayed&&Read(f.rowpath)==rows,"cohort replay");
  f.Publish();const auto published=Read(f.pubpath);Verify(f,published,false,2);
  replay=PublishMgaBulkImportPublicationV1(f.c,f.p);Check(replay.ok&&replay.replayed&&Read(f.pubpath)==published,"publish replay");
  Check(!AbortMgaBulkImportPublicationV1(f.c,f.p).ok&&Read(f.pubpath)==published,"published cannot abort");
  auto loaded=RecoverMgaBulkImportImportedRowEventsV1(f.c,f.p.recovery_idempotency_key);
  Check(loaded.ok&&loaded.events==f.rows,"whole cohort recovery");
  Write(f.rowpath,{});Check(!PublishMgaBulkImportPublicationV1(f.c,f.p).ok,"replay rechecks row journal");
}
static void Owners() {
  Fixture f;f.Prepare();f.Store();const auto pub=Read(f.pubpath),rows=Read(f.rowpath);
  for(unsigned role=0;role<8;++role) {
    auto c=f.c;
    if(role==0)c.database_uuid=Id(401);if(role==1)c.principal_uuid=Id(402);
    if(role==2)c.session_uuid=Id(403);if(role==3)c.statement_receipt_uuid=Id(404);
    if(role==4)c.transaction_uuid=Id(405);if(role==5)c.statement_uuid=Id(406);
    if(role==6)++c.local_transaction_id;if(role==7)c.security_context_present=false;
    Check(!RecoverMgaBulkImportPublicationV1(c,f.p.recovery_idempotency_key).ok,"publication owner refusal");
    Check(!RecoverMgaBulkImportImportedRowEventsV1(c,f.p.recovery_idempotency_key).ok,"cohort owner refusal");
    Check(!PublishMgaBulkImportPublicationV1(c,f.p).ok,"publish owner refusal");
    Check(Read(f.pubpath)==pub&&Read(f.rowpath)==rows,"owner refusal preserves bytes");
  }
  Check(AbortMgaBulkImportPublicationV1(f.c,f.p).ok,"abort prepared");
  auto aborted=Read(f.pubpath);auto r=AbortMgaBulkImportPublicationV1(f.c,f.p);
  Check(r.ok&&r.replayed&&Read(f.pubpath)==aborted,"abort replay");
  Check(!PublishMgaBulkImportPublicationV1(f.c,f.p).ok,"aborted cannot publish");
}
static void Corruption() {
  Fixture f;f.Prepare();const auto good=Read(f.pubpath);
  for(std::size_t i=0;i<good.size();++i) {
    auto bad=good;bad[i]^=0x40;Write(f.pubpath,bad);
    auto r=RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key);
    Check(!r.ok&&!r.found&&Read(f.pubpath)==bad,"corrupt byte refusal without erasure");
  }
  for(const Bytes legacy:{Bytes{},Bytes{'a','|','b','\n'},Bytes(32,0)}) {
    Write(f.pubpath,legacy);Check(!RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key).ok,"legacy/empty refusal");
    Check(Read(f.pubpath)==legacy,"legacy unchanged");
  }
  Write(f.pubpath,good);f.Store();f.Publish();const auto full=Read(f.pubpath);
  const auto tail=good.size();
  for(std::size_t bytes=1;bytes<full.size()-tail;++bytes) {
    Bytes torn(full.begin(),full.begin()+tail+bytes);Write(f.pubpath,torn);
    Check(!PublishMgaBulkImportPublicationV1(f.c,f.p).ok&&Read(f.pubpath)==torn,"ordinary access refuses incomplete frame");
    auto wrong=f.c;wrong.transaction_uuid=Id(600);
    Check(!RecoverMgaBulkImportPublicationV1(wrong,f.p.recovery_idempotency_key).ok&&Read(f.pubpath)==torn,"wrong owner never repairs");
    auto r=RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key);
    if(bytes<208)Check(!r.ok&&Read(f.pubpath)==torn,"unproven header not erased");
    else Check(r.ok&&r.found&&r.record==f.p&&Read(f.pubpath)==good,"authorized prefix recovery");
  }
}

#if defined(SB_BULK_COMPANION_FAULTS)
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
enum Fault {none,write_fail,sync_fail,rename_fail,dir_sync_fail,crash_before_rename,crash_after_rename,crash_before_footer,crash_after_footer};
static Fault fault=none;
static int fault_after=0;
static bool fault_hit=false;
static bool Hit(Fault kind) {
  if(fault!=kind)return false;
  if(fault_after--!=0)return false;
  fault=none;fault_hit=true;return true;
}
static bool Companion(int fd) {
  char link[64],path[4096];std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
  const auto n=::readlink(link,path,sizeof(path)-1);if(n<0)return false;path[n]=0;
  return std::strstr(path,".sb.mga_bulk_import_")&&!std::strstr(path,".sb.owner.lock");
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" void RealRename(const std::filesystem::path&,const std::filesystem::path&,std::error_code&) noexcept
  asm("__real__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code");
extern "C" void WrapRename(const std::filesystem::path&,const std::filesystem::path&,std::error_code&) noexcept
  asm("__wrap__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code");
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t off) {
  const bool target=Companion(fd),footer=target&&n==64&&std::memcmp(p,"BICC2\0\0\0",8)==0;
  if(target&&Hit(write_fail)){errno=EIO;return -1;}
  if(footer&&Hit(crash_before_footer))::_exit(86);
  const auto r=__real_pwrite(fd,p,n,off);
  if(footer&&r==static_cast<ssize_t>(n)&&Hit(crash_after_footer))::_exit(86);
  return r;
}
extern "C" int __wrap_fsync(int fd) {
  struct stat st{};const bool dir=::fstat(fd,&st)==0&&S_ISDIR(st.st_mode);
  if((dir&&Hit(dir_sync_fail))||(!dir&&Companion(fd)&&Hit(sync_fail))){errno=EIO;return -1;}
  return __real_fsync(fd);
}
extern "C" void WrapRename(const std::filesystem::path& from,const std::filesystem::path& to,std::error_code& ec) noexcept {
  const bool target=std::strstr(from.c_str(),".sb.mga_bulk_import_")&&std::strstr(from.c_str(),".tmp.");
  if(target&&Hit(crash_before_rename))::_exit(86);
  if(target&&Hit(rename_fail)){ec=std::make_error_code(std::errc::io_error);return;}
  RealRename(from,to,ec);
  if(target&&!ec&&Hit(crash_after_rename))::_exit(86);
}
static void Wait(pid_t pid,int code) {
  int status=0;Check(pid>0&&::waitpid(pid,&status,0)==pid,"wait subprocess");
  Check(WIFEXITED(status)&&WEXITSTATUS(status)==code,"subprocess exit");
}
static void Fresh(const char* executable,Fixture& f,int expected) {
  const auto text=std::to_string(expected);const auto pid=::fork();
  if(pid==0){::execl(executable,executable,"recover",f.c.database_path.c_str(),text.c_str(),nullptr);::_exit(90);}
  Wait(pid,0);
}
static void Faults(const char* executable) {
  for(unsigned op=0;op<4;++op)for(auto kind:{write_fail,sync_fail,rename_fail,dir_sync_fail}) {
    bool exhausted=false;
    for(int ordinal=0;ordinal<25;++ordinal) {
      Fixture f;if(op)f.Prepare();if(op>=2)f.Store();
      fault=kind;fault_after=ordinal;fault_hit=false;
      bool ok=false;
      if(op==0)ok=PrepareMgaBulkImportPublicationV1(f.c,f.p).ok;
      if(op==1)ok=StoreMgaBulkImportImportedRowEventsV1(f.c,f.rows).ok;
      if(op==2)ok=PublishMgaBulkImportPublicationV1(f.c,f.p).ok;
      if(op==3)ok=AbortMgaBulkImportPublicationV1(f.c,f.p).ok;
      fault=none;
      if(fault_hit)Check(!ok,"actual I/O failure cannot succeed");
      else Check(ok,"fault sweep terminal success");
      // Recover visible complete evidence or a validated append tear. A refused
      // writer is never treated as a rollback merely because it returned error.
      const auto p=RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key);
      Check(p.ok,"fault recovery publication");
      const auto r=RecoverMgaBulkImportImportedRowEventsV1(f.c,f.p.recovery_idempotency_key);
      Check(r.ok,"fault recovery rows");
      if(p.found&&p.record.lifecycle==Life::published_uncommitted)Check(r.events==f.rows,"published requires exact full cohort");
      if(ok&&op==0)Check(p.found&&p.record==f.p,"successful prepare durable");
      if(ok&&op==1)Check(r.events==f.rows,"successful store durable");
      if(ok&&op==2)Check(p.found&&p.record.lifecycle==Life::published_uncommitted,"successful publish durable");
      if(ok&&op==3)Check(p.found&&p.record.lifecycle==Life::aborted,"successful abort durable");
      if(!fault_hit){exhausted=true;break;}
    }
    Check(exhausted,"all actual I/O failure points exhausted");
  }
  for(auto kind:{crash_before_rename,crash_after_rename}) {
    Fixture f;auto pid=::fork();
    if(pid==0){fault=kind;fault_after=0;(void)PrepareMgaBulkImportPublicationV1(f.c,f.p);::_exit(91);}
    Wait(pid,86);Fresh(executable,f,kind==crash_before_rename?0:1);
  }
  for(auto kind:{crash_before_footer,crash_after_footer}) {
    Fixture f;f.Prepare();f.Store();auto pid=::fork();
    if(pid==0){fault=kind;fault_after=0;(void)PublishMgaBulkImportPublicationV1(f.c,f.p);::_exit(91);}
    Wait(pid,86);Fresh(executable,f,kind==crash_before_footer?1:2);
  }
}
#endif

static void Reseal(Bytes& b,std::size_t at) {
  const auto payload=Num(b,at+136),footer=at+208+payload;
  Put(b,at+176,Hash("ScratchBird.MgaBulkImportCompanionHeader.V2",{b.data()+at,176}));
  Put(b,footer+32,Hash("ScratchBird.MgaBulkImportCompanionFrame.V2",{b.data()+at,static_cast<std::size_t>(208+payload+32)}));
}
static void ResealedAndConcurrent() {
  Fixture f;f.Prepare();f.Store();const auto prepared=Read(f.pubpath),rows=Read(f.rowpath);
  for(unsigned variant=0;variant<12;++variant) {
    auto bad=prepared;
    if(variant==0)Num(bad,32+16,2);
    if(variant==1)bad[32+144]=1;
    if(variant==2)bad[32+24]=2;
    if(variant==3)bad[32+25]=3;
    if(variant==4)bad[32+26]=1;
    if(variant==5)Put(bad,32+32,Id(600).bytes);
    if(variant==6)bad[32+48+6]=0x40;
    if(variant==7)Put(bad,32+80,Id(601).bytes);
    if(variant==8)bad[32+208+4+6]=0x40;
    if(variant==9)Num(bad,32+208+300,1); // Rejected rows forbidden in this exact profile.
    if(variant==10)Num(bad,32+208+284,3); // Inconsistent immutable row counts.
    if(variant==11)bad[32+208+2]=1; // Unknown canonical flags.
    if(variant>=8)Put(bad,32+208+580,Hash("ScratchBird.BulkImportStreamMgaPublicationRecord.V1",{bad.data()+32+208,580}));
    Reseal(bad,32);Write(f.pubpath,bad);
    Check(!RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key).ok&&Read(f.pubpath)==bad,"resealed malformed authority refused");
  }
  Write(f.pubpath,prepared);
  for(unsigned variant=0;variant<5;++variant) {
    auto bad=rows;const auto body=32+208+388;
    if(variant==0)Num(bad,body+164,1);
    if(variant==1)Put(bad,body+172,f.rows[0].row_uuid.bytes);
    if(variant==2)Put(bad,body+116,Id(603).bytes);
    if(variant==3)Num(bad,body+156,88);
    if(variant==4)Put(bad,body+204,f.rows[0].row_version_uuid.bytes);
    Put(bad,body+356,Hash("ScratchBird.BulkImportStreamImportedRowEvent.V1",{bad.data()+body,356}));
    Reseal(bad,32);Write(f.rowpath,bad);
    Check(!RecoverMgaBulkImportImportedRowEventsV1(f.c,f.p.recovery_idempotency_key).ok&&Read(f.rowpath)==bad,"resealed cohort mismatch refused");
  }
  Write(f.rowpath,rows);f.Publish();const auto published=Read(f.pubpath);
  // A physically short footer with contradictory visible bytes is corruption.
  for(unsigned index=0;index<63;++index) {
    Bytes bad(published.begin(),published.end()-64+index+1);bad.back()^=0x20;Write(f.pubpath,bad);
    Check(!RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key).ok&&Read(f.pubpath)==bad,"bad partial footer not erased");
  }
  // Existing-key recovery cannot erase a partial body belonging to other authority.
  Bytes bad(published.begin(),published.begin()+prepared.size()+208+190);
  bad[prepared.size()+208+180]^=1;Write(f.pubpath,bad);
  Check(!RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key).ok&&Read(f.pubpath)==bad,"bad partial immutable body not erased");
  Write(f.pubpath,prepared);
  auto other=f.c;other.principal_uuid=Id(800);auto other_pub=f.p;other_pub.recovery_idempotency_key=Filled(81);
  const auto second=PrepareMgaBulkImportPublicationV1(other,other_pub);Check(second.ok,"other owner journal cohort");
  Check(RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key).ok,"selected owner in multi-owner journal");
  Check(!RecoverMgaBulkImportPublicationV1(f.c,other_pub.recovery_idempotency_key).ok,"multi-owner selection isolation");
  Check(RecoverMgaBulkImportPublicationV1(other,other_pub.recovery_idempotency_key).ok,"second owner can recover");
  Fixture concurrent;
  std::array<MgaBulkImportPublicationResultV1,8> replies;
  std::vector<std::thread> threads;
  for(unsigned i=0;i<replies.size();++i)threads.emplace_back([&,i]{replies[i]=PrepareMgaBulkImportPublicationV1(concurrent.c,concurrent.p);});
  for(auto& thread:threads)thread.join();
  unsigned first=0;for(const auto& r:replies){Check(r.ok&&r.record==concurrent.p,"concurrent exact prepare");first+=!r.replayed;}
  Check(first==1,"one concurrent publication");Verify(concurrent,Read(concurrent.pubpath),false,1);
}

static void Allocations() {
  unsigned injections=0,accepted_faults=0;
  for(unsigned op=0;op<6;++op) {
    Fixture f;if(op)f.Prepare();if(op>=2)f.Store();
    const auto before_pub=op?Read(f.pubpath):Bytes{};
    const auto before_rows=op>=2?Read(f.rowpath):Bytes{};
    bool exhausted=false;
    for(long ordinal=0;ordinal<16000;++ordinal) {
      if(op)Write(f.pubpath,before_pub);else std::filesystem::remove(f.pubpath);
      if(op>=2)Write(f.rowpath,before_rows);else std::filesystem::remove(f.rowpath);
      allocation_injected=false;fail_allocation=ordinal;
      const auto allocation_start=allocations.load();
      bool ok=false;MgaBulkImportPublicationResultV1 p;MgaBulkImportImportedRowEventResultV1 r;
      try {
        if(op==0)p=PrepareMgaBulkImportPublicationV1(f.c,f.p);
        if(op==1)r=StoreMgaBulkImportImportedRowEventsV1(f.c,f.rows);
        if(op==2)p=PublishMgaBulkImportPublicationV1(f.c,f.p);
        if(op==3)p=AbortMgaBulkImportPublicationV1(f.c,f.p);
        if(op==4)p=RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key);
        if(op==5)r=RecoverMgaBulkImportImportedRowEventsV1(f.c,f.p.recovery_idempotency_key);
      } catch(...) {fail_allocation=-1;throw;}
      fail_allocation=-1;ok=(op==1||op==5)?r.ok:p.ok;
      const auto pub=RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key);
      const auto rows=RecoverMgaBulkImportImportedRowEventsV1(f.c,f.p.recovery_idempotency_key);
      Check(pub.ok&&rows.ok,"allocation leaves recoverable exact evidence");
      if(ok) {
        if(op==1||op==5)Check(r.events==f.rows&&rows.events==f.rows,"allocation success exact cohort");
        else Check(p.found&&pub.found&&p.record==pub.record,"allocation success exact publication");
        if(op==0||op==4)Check(p.record==f.p,"allocation success immutable authority");
        if(op==2)Check(p.record.lifecycle==Life::published_uncommitted,"allocation publish effect");
        if(op==3)Check(p.record.lifecycle==Life::aborted,"allocation abort effect");
        if(allocation_injected)++accepted_faults;
      } else {
        if(op==1||op==5)Check(r.events.empty()&&r.diagnostic.error,"allocation error no row evidence");
        else Check(!p.found&&p.diagnostic.error,"allocation error no publication evidence");
      }
      if(!allocation_injected){Check(ok,"allocation sweep terminal success");exhausted=true;
        std::cout<<"allocation_op="<<op<<" exhausted_at="<<ordinal<<" observed="<<(allocations-allocation_start)<<std::endl;
        break;}
      ++injections;
    }
    Check(exhausted,"all allocation points exhausted");
  }
  std::cout<<"allocation_injections="<<injections<<" exact_effect_optional_faults="<<accepted_faults<<"\n";
}

int main(int argc,char** argv) {
  try {
    if(argc==4&&std::string_view(argv[1])=="recover") {
      Fixture f;f.c.database_path=argv[2];const int expected=std::atoi(argv[3]);
      const auto p=RecoverMgaBulkImportPublicationV1(f.c,f.p.recovery_idempotency_key);
      Check(p.ok&&p.found==(expected!=0),"fresh process presence");
      if(expected)Check(static_cast<int>(p.record.lifecycle)==expected,"fresh process exact lifecycle");
      return 0;
    }
    Basic();Owners();Corruption();ResealedAndConcurrent();Allocations();
#if defined(SB_BULK_COMPANION_FAULTS)
    Faults(argv[0]);
#endif
    std::cout<<"PASS bulk companion checks="<<checks<<"\n";return 0;
  }
  catch(const std::exception& e){std::cerr<<"FAIL check "<<checks<<": "<<e.what()<<"\n";return 1;}
}
