// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "executable_object_lifecycle.hpp"
#include "catalog/catalog_object_lifecycle_codec.hpp"
#include "catalog/binary_view_options.hpp"
namespace scratchbird::engine::internal_api {
namespace executable_record_codec {
template<class T> struct Traits;
template<> struct Traits<EngineExecutableObjectRecord> {
  static constexpr const char* kind="EngineExecutableObjectRecord";
  template<class T> static auto Fields(T& r) { return std::tie(r.creator_tx,r.object_uuid,r.catalog_row_uuid,r.object_kind,r.schema_uuid,r.owner_principal_uuid,r.package_uuid,r.lifecycle_state,r.executable_generation,r.metadata_epoch,r.executor_kind,r.stored_sblr_hash,r.stored_sblr_provenance,r.internal_procedure_id,r.side_effect_class,r.event_trigger_event,r.payload,r.invalidated,r.invalidated_generation,r.invalidation_reason_uuid,r.deleted); }
  static const EngineUuid& Identity(const EngineExecutableObjectRecord& r) { return r.object_uuid; }
};
template<> struct Traits<EngineExecutableDependencyRecord> {
  static constexpr const char* kind="EngineExecutableDependencyRecord";
  template<class T> static auto Fields(T& r) { return std::tie(r.creator_tx,r.source_uuid,r.source_kind,r.dependency_uuid,r.dependency_kind,r.dependency_generation,r.metadata_epoch,r.deleted); }
  static const EngineUuid& Identity(const EngineExecutableDependencyRecord& r) { return r.source_uuid; }
};
template<> struct Traits<EngineExecutableInvocationRecord> {
  static constexpr const char* kind="EngineExecutableInvocationRecord";
  template<class T> static auto Fields(T& r) { return std::tie(r.creator_tx,r.invocation_lease_uuid,r.object_uuid,r.executable_generation,r.lifecycle_state,r.metadata_epoch); }
  static const EngineUuid& Identity(const EngineExecutableInvocationRecord& r) { return r.invocation_lease_uuid; }
};
template<> struct Traits<EngineExecutableInvalidationRecord> {
  static constexpr const char* kind="EngineExecutableInvalidationRecord";
  template<class T> static auto Fields(T& r) { return std::tie(r.creator_tx,r.object_uuid,r.reason_uuid,r.dependency_generation,r.metadata_epoch); }
  static const EngineUuid& Identity(const EngineExecutableInvalidationRecord& r) { return r.object_uuid; }
};
template<> struct Traits<EngineExecutableCacheRecord> {
  static constexpr const char* kind="EngineExecutableCacheRecord";
  template<class T> static auto Fields(T& r) { return std::tie(r.creator_tx,r.object_uuid,r.operation_id,r.metadata_epoch,r.security_epoch,r.resource_epoch); }
  static const EngineUuid& Identity(const EngineExecutableCacheRecord& r) { return r.object_uuid; }
};
template<> struct Traits<EngineExecutableTriggerFireRecord> {
  static constexpr const char* kind="EngineExecutableTriggerFireRecord";
  template<class T> static auto Fields(T& r) { return std::tie(r.creator_tx,r.object_uuid,r.event_name,r.command_tag,r.metadata_epoch); }
  static const EngineUuid& Identity(const EngineExecutableTriggerFireRecord& r) { return r.object_uuid; }
};
}
using ExecutableLifecycleRecord=std::variant<EngineExecutableObjectRecord,EngineExecutableDependencyRecord,EngineExecutableInvocationRecord,EngineExecutableInvalidationRecord,EngineExecutableCacheRecord,EngineExecutableTriggerFireRecord>;
template<class Record> bool ExecutableRecordAdmitted(const Record& record) {
  if constexpr(std::is_same_v<Record,EngineExecutableObjectRecord>) {
    std::vector<std::string> options;
    return !record.catalog_row_uuid.is_nil()&&DecodeBinaryViewOptions(record.payload,&options);
  } else if constexpr(std::is_same_v<Record,EngineExecutableDependencyRecord>) {
    return !record.dependency_uuid.is_nil();
  } else if constexpr(std::is_same_v<Record,EngineExecutableInvocationRecord>) {
    return !record.object_uuid.is_nil();
  }
  return true;
}
template<class Record> bool EncodeExecutableLifecycleRecord(const Record& record,std::string* output) {
  using Traits=executable_record_codec::Traits<Record>;
  if(!ExecutableRecordAdmitted(record))return false;
  ApiBehaviorRecord frame;
  frame.creator_tx=record.creator_tx;frame.object_uuid=Traits::Identity(record);
  frame.operation_id="executable.lifecycle.record";frame.object_kind=Traits::kind;
  frame.state="record";frame.payload="SBEXE002";
  if(!std::apply([&](const auto&... value){return(catalog_record_codec::Put(frame.payload,value)&&...);},Traits::Fields(record)))return false;
  return EncodeApiBehaviorRecord(frame,output);
}
template<class Record> bool DecodeExecutableLifecycleBody(const ApiBehaviorRecord& frame,Record* output) {
  using Traits=executable_record_codec::Traits<Record>;
  if(!output||frame.object_kind!=Traits::kind||frame.operation_id!="executable.lifecycle.record"||
      frame.state!="record"||frame.deleted||!frame.default_name.empty()||
      !frame.target_database_uuid.is_nil()||!frame.target_schema_uuid.is_nil()||!frame.target_object_uuid.is_nil()||
      !frame.payload.starts_with("SBEXE002"))return false;
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(frame.payload.data()),frame.payload.size());
  std::size_t cursor=8;Record staged;
  if(!std::apply([&](auto&... value){return(catalog_record_codec::Get(bytes,cursor,value)&&...);},Traits::Fields(staged))||
      cursor!=bytes.size()||!ExecutableRecordAdmitted(staged)||staged.creator_tx!=frame.creator_tx||Traits::Identity(staged)!=frame.object_uuid)return false;
  *output=std::move(staged);return true;
}
inline bool DecodeExecutableLifecycleFrame(const ApiBehaviorRecord& frame,ExecutableLifecycleRecord* output) {
  if(!output)return false;
  if(frame.object_kind==executable_record_codec::Traits<EngineExecutableObjectRecord>::kind){
    EngineExecutableObjectRecord record;if(!DecodeExecutableLifecycleBody(frame,&record))return false;
    *output=std::move(record);return true;
  }
  if(frame.object_kind==executable_record_codec::Traits<EngineExecutableDependencyRecord>::kind){
    EngineExecutableDependencyRecord record;if(!DecodeExecutableLifecycleBody(frame,&record))return false;
    *output=std::move(record);return true;
  }
  if(frame.object_kind==executable_record_codec::Traits<EngineExecutableInvocationRecord>::kind){
    EngineExecutableInvocationRecord record;if(!DecodeExecutableLifecycleBody(frame,&record))return false;
    *output=std::move(record);return true;
  }
  if(frame.object_kind==executable_record_codec::Traits<EngineExecutableInvalidationRecord>::kind){
    EngineExecutableInvalidationRecord record;if(!DecodeExecutableLifecycleBody(frame,&record))return false;
    *output=std::move(record);return true;
  }
  if(frame.object_kind==executable_record_codec::Traits<EngineExecutableCacheRecord>::kind){
    EngineExecutableCacheRecord record;if(!DecodeExecutableLifecycleBody(frame,&record))return false;
    *output=std::move(record);return true;
  }
  if(frame.object_kind==executable_record_codec::Traits<EngineExecutableTriggerFireRecord>::kind){
    EngineExecutableTriggerFireRecord record;if(!DecodeExecutableLifecycleBody(frame,&record))return false;
    *output=std::move(record);return true;
  }
  return false;
}
} // namespace scratchbird::engine::internal_api
