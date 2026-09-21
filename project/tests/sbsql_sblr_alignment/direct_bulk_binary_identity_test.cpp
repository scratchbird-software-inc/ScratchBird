// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual bulk allocation and shared CRUD/cryptographic source tests. This does
// not qualify the still-required database/cluster clock policy or physical rows.
#include "dml/direct_bulk_uuid_authority.hpp"
#include "crud_support/crud_store.hpp"
#include "core/time/time.hpp"
#include "core/uuid/uuid.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <thread>
namespace api=scratchbird::engine::internal_api;
namespace bulk=api::dml::detail;
namespace t=scratchbird::core::time;
namespace p=scratchbird::core::platform;
static unsigned checks=0;
static std::atomic<unsigned> entropy_calls{0},clock_calls{0};
static bool real_sources=false;
static int entropy_failure=-1,clock_failure=-1;
static bool repeat_entropy=false;
static long allocation=-1;
static bool allocation_hit=false;
void* operator new(std::size_t n) {
  if(allocation==0){allocation=-1;allocation_hit=true;throw std::bad_alloc();}
  if(allocation>0)--allocation;
  if(void* ptr=std::malloc(n?n:1))return ptr;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
static void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static api::EngineUuid Id(unsigned n) {
  api::EngineUuid u{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,0}};
  u.bytes[14]=n>>8;u.bytes[15]=n;return u;
}
static api::EngineUuid Generated(unsigned n) {
  api::EngineUuid id;id.bytes.fill(0x5c);
  for(unsigned i=0;i<6;++i)id.bytes[i]=static_cast<std::uint64_t>(12345)>>(40-i*8);
  id.bytes[6]=0x7c;id.bytes[8]=0x9c;
  for(unsigned i=0;i<4;++i)id.bytes[15-i]=n>>(i*8);
  return id;
}
static void Reset(){entropy_calls=clock_calls=0;entropy_failure=clock_failure=-1;repeat_entropy=false;}
static bulk::DirectBulkUuidBatch Sentinel() {
  bulk::DirectBulkUuidBatch b;b.row_uuids={Id(600)};b.version_uuids={Id(601)};b.row_image_uuids={Id(602)};
  b.generated_row_uuids=73;b.batch_evidence_id="unchanged";return b;
}
static bool Unchanged(const bulk::DirectBulkUuidBatch& b) {
  return b.row_uuids==std::vector{Id(600)}&&b.version_uuids==std::vector{Id(601)}&&
      b.row_image_uuids==std::vector{Id(602)}&&b.generated_row_uuids==73&&b.batch_evidence_id=="unchanged";
}
static void Refuse(const api::dml::DirectPhysicalBulkAppendRequest& request,std::size_t count,const char* code) {
  auto output=Sentinel();bool refused=false;
  try{output=bulk::BuildDirectBulkUuidBatch(request,count);}
  catch(const api::CrudIdentityIssuanceError& e){refused=true;Check(e.diagnostic().diagnostic_code==code,"exact native cause");}
  Check(refused&&Unchanged(output),"refusal published partial cohort");
}
static void Cohorts() {
  static_assert(sizeof(api::EngineUuid)==16);
  static_assert(std::is_same_v<decltype(bulk::DirectBulkUuidBatch{}.row_uuids)::value_type,api::EngineUuid>);
  for(bool images:{false,true})for(unsigned n:{0u,1u,2u,19u,512u}) {
    Reset();std::vector<api::EngineRowValue> input(n);unsigned supplied=0,callbacks=0;
    for(unsigned i=0;i<n;++i)if(i%3==0){input[i].requested_row_uuid=Id(i+1);++supplied;}
    api::dml::DirectPhysicalBulkAppendRequest request;request.borrowed_input_rows=input;request.context.request_id="binary";
    if(images)request.before_row_publication=[&](auto,auto,auto){++callbacks;return api::EngineApiDiagnostic{};};
    const auto output=bulk::BuildDirectBulkUuidBatch(request,n);
    Check(output.row_uuids.size()==n&&output.version_uuids.size()==n&&output.row_image_uuids.size()==(images?n:0),"exact cohort sizes");
    Check(output.caller_row_uuids==supplied&&output.generated_row_uuids==n-supplied,"actual row origin counters");
    const auto generated=n*(images?3:2)-supplied;
    Check(entropy_calls==generated&&clock_calls==generated&&output.reservoir_sync_generated_uuids==generated,"actual source calls");
    Check(!output.reservoir_async_refill_requested&&!output.reservoir_served_uuids&&!callbacks,"no detached refill or callback publication");
    std::set<api::EngineUuid> all;unsigned ordinal=0;
    for(unsigned i=0;i<n;++i) {
      Check(output.row_uuids[i]==(i%3==0?input[i].requested_row_uuid:Generated(++ordinal)),"row exact raw bytes");
      Check(output.version_uuids[i]==Generated(++ordinal),"version exact raw bytes");
      if(images)Check(output.row_image_uuids[i]==Generated(++ordinal),"image exact raw bytes");
      for(const auto& id:{output.row_uuids[i],output.version_uuids[i]})Check(all.insert(id).second,"binary collision");
      if(images)Check(all.insert(output.row_image_uuids[i]).second,"image collision");
    }
  }
}
static void Supplied() {
  api::dml::DirectPhysicalBulkAppendRequest request;std::vector<api::EngineRowValue> input(1);
  request.borrowed_input_rows=input;
  for(unsigned byte=0;byte<16;++byte)for(unsigned value=0;value<256;++value) {
    Reset();input[0].requested_row_uuid=Id(99);input[0].requested_row_uuid.bytes[byte]=value;
    const bool valid=(input[0].requested_row_uuid.bytes[6]>>4)==7&&(input[0].requested_row_uuid.bytes[8]&0xc0)==0x80;
    if(!valid){Refuse(request,1,"UUID.ENGINE_IDENTITY_NOT_V7");Check(!entropy_calls&&!clock_calls,"invalid input issued identity");}
    else {const auto b=bulk::BuildDirectBulkUuidBatch(request,1);Check(b.row_uuids[0]==input[0].requested_row_uuid,"supplied identity changed");}
  }
  input.resize(2);request.borrowed_input_rows=input;input[0].requested_row_uuid=input[1].requested_row_uuid=Id(31);
  Reset();Refuse(request,2,"BULK.IMPORT.RECOVERY_CONFLICT");Check(!entropy_calls,"duplicate input issued identity");
  // Fresh first-row issuance collides with a later supplied row. Reservation
  // must cover the whole input before issuing any generated identities.
  input[0].requested_row_uuid={};input[1].requested_row_uuid=Generated(1);
  Reset();Refuse(request,2,"BULK.IMPORT.RECOVERY_CONFLICT");Check(entropy_calls==1,"future supplied collision not immediate");
  request.borrowed_input_rows={};Reset();repeat_entropy=true;
  Refuse(request,1,"BULK.IMPORT.RECOVERY_CONFLICT");Check(entropy_calls==2,"source collision not refused");
}
static void Faults() {
  api::dml::DirectPhysicalBulkAppendRequest request;request.before_row_publication=[](auto,auto,auto){return api::EngineApiDiagnostic{};};
  for(int position=1;position<=9;++position) {
    Reset();entropy_failure=position;Refuse(request,3,"TIME.UUID_RANDOMNESS_UNAVAILABLE");
    Check(entropy_calls==unsigned(position),"entropy failure boundary");
    Reset();clock_failure=position;Refuse(request,3,"TIME.SOURCE_FAILED");
    Check(clock_calls==unsigned(position)&&entropy_calls==unsigned(position-1),"clock failure boundary");
  }
  Reset();bool extent=false;auto output=Sentinel();
  try{output=bulk::BuildDirectBulkUuidBatch(request,std::numeric_limits<std::size_t>::max());}
  catch(const std::length_error&){extent=true;}
  Check(extent&&Unchanged(output)&&!entropy_calls&&!clock_calls,"extent overflow before issuance");
  unsigned faults=0;bool exhausted=false;
  for(long i=0;i<500;++i) {
    Reset();output=Sentinel();allocation_hit=false;allocation=i;bool ok=false;
    try{output=bulk::BuildDirectBulkUuidBatch(request,3);ok=true;}
    catch(const std::bad_alloc&){}
    allocation=-1;
    if(ok) {
      Check(output.row_uuids.size()==3&&output.row_image_uuids.size()==3&&output.version_uuids.size()==3,"allocation success incomplete");
      Check(output.row_image_uuids.back()==Generated(9),"allocation success missing final identity");
    } else Check(Unchanged(output),"allocation partial cohort escaped");
    if(!allocation_hit){Check(ok,"allocation terminal failed");exhausted=true;break;}
    ++faults;
  }
  Check(exhausted&&faults>0,"allocation sweep incomplete");
  std::cout<<"allocation_faults="<<faults<<"\n";
}
static void RealSources() {
  Reset();real_sources=true;
  std::array<bulk::DirectBulkUuidBatch,8> batches;
  std::array<std::exception_ptr,8> errors;
  std::vector<std::thread> workers;
  for(unsigned i=0;i<batches.size();++i)workers.emplace_back([&,i] {
    try {
      api::dml::DirectPhysicalBulkAppendRequest request;
      request.before_row_publication=[](auto,auto,auto){return api::EngineApiDiagnostic{};};
      batches[i]=bulk::BuildDirectBulkUuidBatch(request,64);
    } catch(...) {errors[i]=std::current_exception();}
  });
  for(auto& worker:workers)worker.join();real_sources=false;
  std::set<api::EngineUuid> ids;
  for(unsigned i=0;i<batches.size();++i) {
    Check(!errors[i],"real source worker failed");
    const auto& b=batches[i];
    Check(b.row_uuids.size()==64&&b.version_uuids.size()==64&&b.row_image_uuids.size()==64,"real source batch incomplete");
    for(const auto* values:{&b.row_uuids,&b.version_uuids,&b.row_image_uuids})
      for(const auto& id:*values)Check(scratchbird::core::uuid::IsEngineIdentityUuid(id)&&ids.insert(id).second,"real binary UUID admission/collision");
  }
  Check(entropy_calls==1536&&clock_calls==1536,"real source call count");
}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" t::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" int __wrap_RAND_bytes(unsigned char* b,int n) {
  ++entropy_calls;
  if(real_sources)return __real_RAND_bytes(b,n);
  if(entropy_failure==int(entropy_calls)){if(n>0)std::memset(b,0xff,n/2);return 0;}
  std::memset(b,0x5c,n);const unsigned value=repeat_entropy?1:entropy_calls.load();
  for(unsigned i=0;i<4&&i<unsigned(n);++i)b[n-1-i]=value>>(8*i);return 1;
}
extern "C" t::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv() {
  ++clock_calls;
  if(real_sources)return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
  t::ClockSnapshotResult r;r.value.wall_clock={12,345000000};r.value.monotonic.ticks=clock_calls;
  if(clock_failure==int(clock_calls)) {
    r.status={p::StatusCode::time_source_unavailable,p::Severity::error,p::Subsystem::time};
    r.diagnostic=p::MakeDiagnostic(r.status.code,r.status.severity,r.status.subsystem,"TIME.SOURCE_FAILED","fixture.source_failed",{});
  }
  return r;
}
int main() {
  try{Cohorts();Supplied();Faults();RealSources();std::cout<<"PASS direct bulk binary checks="<<checks<<"\n";return 0;}
  catch(const std::exception& e){allocation=-1;std::cerr<<"FAIL check "<<checks<<": "<<e.what()<<"\n";return 1;}
}
