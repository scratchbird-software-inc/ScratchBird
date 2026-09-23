// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/extensibility/executable_object_lifecycle.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()) {
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n) {
  a::EngineUuid id;id.bytes={1,144,10,9,0,124,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;
}
template<class T> static void RoundTrip(const T& record) {
  std::string bytes;Check(a::EncodeExecutableLifecycleRecord(record,&bytes));
  a::ApiBehaviorRecord frame;
  Check(a::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},&frame));
  a::ExecutableLifecycleRecord event;
  Check(a::DecodeExecutableLifecycleFrame(frame,&event)&&std::holds_alternative<T>(event));
  std::string reencoded;Check(a::EncodeExecutableLifecycleRecord(std::get<T>(event),&reencoded)&&reencoded==bytes);
  frame.object_uuid=Id(99);Check(!a::DecodeExecutableLifecycleFrame(frame,&event));
  for(std::size_t n=0;n<bytes.size();++n)
    Check(!a::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(bytes.data()),n},&frame));
  bytes.back()^=1;Check(!a::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},&frame));
}
int main() {
  a::EngineApiRequest request;request.context.local_transaction_id=7;
  request.option_envelopes={a::BinaryViewUuidOption("procedure_body_sblr_uuid:",Id(4)),
      std::string("compiled_body_descriptor:")+std::string("a;|\0b",5)};
  const auto payload=a::PayloadFromRequest(request);
  Check(!payload.empty());
  Check(a::BinaryViewUuid(a::PayloadFieldValue(payload,"procedure_body_sblr_uuid:"))==Id(4));
  Check(a::PayloadFieldValue(payload,"compiled_body_descriptor:")==std::string("a;|\0b",5));
  request.option_envelopes={"procedure_body_sblr_uuid:01900a09-007c-7000-8000-000000000004"};
  Check(a::PayloadFromRequest(request).empty());
  a::EngineExecutableObjectRecord object;
  object.creator_tx=7;object.object_uuid=Id(1);object.catalog_row_uuid=Id(2);object.schema_uuid=Id(3);
  object.object_kind="procedure";object.payload=payload;object.stored_sblr_hash=std::string("sha256:x\0y",10);
  RoundTrip(object);
  object.catalog_row_uuid={};std::string bytes;Check(!a::EncodeExecutableLifecycleRecord(object,&bytes));
  a::EngineExecutableDependencyRecord dependency;
  dependency.creator_tx=7;dependency.source_uuid=Id(1);dependency.dependency_uuid=Id(3);RoundTrip(dependency);
  a::EngineExecutableInvocationRecord invocation;
  invocation.creator_tx=7;invocation.invocation_lease_uuid=Id(5);invocation.object_uuid=Id(1);RoundTrip(invocation);
  a::EngineExecutableInvalidationRecord invalidation;
  invalidation.creator_tx=7;invalidation.object_uuid=Id(1);invalidation.reason_uuid=Id(6);RoundTrip(invalidation);
  a::EngineExecutableCacheRecord cache;cache.creator_tx=7;cache.object_uuid=Id(1);cache.operation_id="alter";RoundTrip(cache);
  a::EngineExecutableTriggerFireRecord fire;fire.creator_tx=7;fire.object_uuid=Id(1);fire.event_name="ddl_start";RoundTrip(fire);
  bytes=std::string(a::kRoutineDeleteColumnRangeCountDescriptorV1)+"|";
  Check(a::catalog_record_codec::Put(bytes,Id(7)));Check(a::catalog_record_codec::Put(bytes,Id(8)));
  for(std::uint32_t slot:{0u,1u,2u,2u})Check(a::catalog_record_codec::Put(bytes,slot));
  a::RoutineDeleteColumnRangeCountDescriptor routine;
  Check(!a::ParseRoutineDeleteColumnRangeCountDescriptor(bytes,&routine).error);
  Check(routine.table_uuid==Id(7)&&routine.column_uuid==Id(8));
  Check(a::ParseRoutineDeleteColumnRangeCountDescriptor(bytes+"x",&routine).error);
}
