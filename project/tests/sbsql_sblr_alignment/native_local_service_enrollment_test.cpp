// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse real protected/provider/fault fixtures, without invoking their entry.
#define main UnusedNativeCredentialFixtureMain
#define __wrap_pread NativeEnrollmentFixturePread
#include "native_runtime_principal_credential_test.cpp"
#undef __wrap_pread
#undef main
#define Check CheckCredentialTest
#include "../support/native_local_service_creation_fixture.hpp"
#include "../../src/server/native_hosted_database_runtime.hpp"
#include "../../src/core/agents/agent_bootstrap_configuration.hpp"
#include "engine/internal_api/catalog/native_catalog_name_verification.hpp"
#include "engine/internal_api/catalog/native_catalog_namespace_verification.hpp"
#include "engine/internal_api/catalog/native_catalog_index_verification.hpp"
#include "engine/internal_api/verification/native_startup_catalog_allocation.hpp"
#include "engine/internal_api/verification/native_startup_initial_control_allocation.hpp"
#include "native_database_creation.hpp"
#include "native_catalog_index_rows.hpp"
#include "native_database_create.hpp"
#include "native_construction_audit.hpp"
#include "catalog_schema_creation.hpp"
#include "admitted_datatype_cohort.hpp"
#include <sys/file.h>
#include <sys/stat.h>
#include <charconv>
#include <fstream>
#include <chrono>
#include <climits>
#include <time.h>

namespace {
// Authorization records the native L5 projection reads from a complete
// enrollment (2026-11-03): pack security, bootstrap, L5 with its SYSARCH
// membership, and per agent type a principal, its grants and its membership.
constexpr std::size_t kEnrolledAuthorizationRecords=702;
static_assert(!std::is_default_constructible_v<runtime_security::NativeLocalServiceAuthentication>);
static_assert(!std::is_copy_constructible_v<runtime_security::NativeLocalServiceAuthentication>);
static_assert(!std::is_default_constructible_v<runtime_security::NativeLocalServiceSession>);
static_assert(!std::is_copy_constructible_v<runtime_security::NativeLocalServiceSession>);
bool corrupt_startup_system_read=false;
unsigned corrupted_startup_system_reads=0;
std::uint32_t corrupt_startup_control_type=0;
unsigned corrupted_startup_control_reads=0;
// SB_TRANSITIONAL_HOSTED_SOURCE: byte ranges of the checkpoint layer's pages
// in one database file. Any read touching them fails, to show the fixed-root
// startup chain never reads them.
std::array<std::pair<off_t,off_t>,8> denied_pages{};
std::size_t denied_page_count=0,denied_page_total=0;
unsigned denied_page_reads=0;
dev_t denied_device=0;ino_t denied_inode=0;
bool deny_heap_on_native_file=false;
bool native_clock_failure=false,native_clock_regression=false;
unsigned native_clock_advance_seconds=0;
bool observe_guarded_releases=false;
std::uint64_t guarded_release_calls=0;
dev_t native_probe_device{};
ino_t native_probe_inode{};
struct ObservedMutex {pthread_mutex_t* mutex=nullptr;unsigned depth=0;bool selected=false;};
thread_local std::array<ObservedMutex,512> observed_mutexes{};
thread_local pthread_mutex_t* last_observed_mutex=nullptr;
thread_local bool observe_mutexes=false,mutex_observation_overflow=false,observe_handoff_releases=false;
thread_local std::uint64_t guarded_handoff_releases=0;
thread_local long handoff_budget_at_unlock=1;
thread_local bool handoff_guard_unlocked=false;
void (*cleanup_hook)()=nullptr;
server::NativeHostedDatabaseRuntime::CredentialRead* cleanup_read=nullptr;
std::shared_ptr<server::NativeHostedDatabaseRuntime>* cleanup_host=nullptr;
server::NativeHostedRuntimeError callback_close=server::NativeHostedRuntimeError::none;
server::NativeHostedRuntimeError callback_retain=server::NativeHostedRuntimeError::none;
bool callback_read_ok=true;
void ProbeCleanupCallback() {
  callback_close=server::NativeHostedDatabaseRuntime::Close(*cleanup_host).error;
  callback_retain=server::NativeHostedDatabaseRuntime::RetainStartupObservation(*cleanup_read);
  callback_read_ok=cleanup_read->ok();
}
unsigned SelectedGuardDepth() {
  unsigned count=0;for(const auto& item:observed_mutexes)if(item.selected)count+=item.depth;return count;
}
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_clock_gettime(clockid_t,timespec*);
extern "C" int __wrap_clock_gettime(clockid_t clock,timespec* value) {
#if defined(CLOCK_MONOTONIC_RAW)
  constexpr auto source=CLOCK_MONOTONIC_RAW;
#else
  constexpr auto source=CLOCK_MONOTONIC;
#endif
  if(clock==source&&native_clock_failure){errno=EIO;return -1;}
  const auto result=__real_clock_gettime(clock,value);
  if(!result&&clock==source){
    if(native_clock_regression)*value={};
    else value->tv_sec+=native_clock_advance_seconds;
  }
  return result;
}
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  const auto result=__real_pthread_mutex_lock(mutex);
  if(!result&&observe_mutexes){
    last_observed_mutex=mutex;
    auto slot=std::find_if(observed_mutexes.begin(),observed_mutexes.end(),[&](const auto& item){return item.mutex==mutex;});
    if(slot==observed_mutexes.end())slot=std::find_if(observed_mutexes.begin(),observed_mutexes.end(),[](const auto& item){return !item.mutex;});
    if(slot==observed_mutexes.end())mutex_observation_overflow=true;
    else {slot->mutex=mutex;++slot->depth;
      if(observe_handoff_releases&&slot->selected){count_allocations=true;allocation_budget=0;}}
  }
  return result;
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  const auto result=__real_pthread_mutex_unlock(mutex);
  if(!result&&observe_mutexes)for(auto& item:observed_mutexes)if(item.mutex==mutex){
    if(item.depth)--item.depth;
    if(observe_handoff_releases&&item.selected&&!SelectedGuardDepth()){
      // Deferred I/O observations run AFTER the last actual device unlock.
      // Permit their allocations; retain independent evidence of every new
      // attempt and free that happened while a selected guard was held.
      if(!handoff_guard_unlocked){handoff_budget_at_unlock=allocation_budget;handoff_guard_unlocked=true;}
      count_allocations=false;allocation_budget=-1;
    }
    break;
  }
  return result;
}
extern "C" ssize_t __wrap_pread(int fd,void* bytes,size_t count,off_t offset) {
  // The protected credential descriptor is read BEFORE device guards and
  // before the selected-source allocation. Arm on this fixture's actual
  // retained native inode, never on a secret-source read or a guessed FD.
  if(deny_heap_on_native_file){
    struct stat identity{};
    if(::fstat(fd,&identity)==0&&identity.st_dev==native_probe_device&&identity.st_ino==native_probe_inode){
      deny_heap_on_native_file=false;deny_heap_on_next_read=true;observe_guarded_releases=true;
      // The actual FileDevice mutex is recursively held by both the selected
      // source and this ReadAt call. Credential/resource mutexes are distinct
      // and must remain held after the device guard retires.
      for(auto& item:observed_mutexes)if(item.mutex==last_observed_mutex&&item.depth>1)item.selected=true;
    }
  }
  if(denied_page_count){
    struct stat identity{};
    if(::fstat(fd,&identity)==0&&identity.st_dev==denied_device&&identity.st_ino==denied_inode)
      for(std::size_t i=0;i<denied_page_count;++i)
        if(offset<denied_pages[i].second&&offset+static_cast<off_t>(count)>denied_pages[i].first){
          ++denied_page_reads;errno=EIO;return -1;}
  }
  const auto result=NativeEnrollmentFixturePread(fd,bytes,count,offset);
  if(corrupt_startup_control_type&&result>=8192&&platform::LoadLittle32(static_cast<const byte*>(bytes)+16)==corrupt_startup_control_type){
    static_cast<byte*>(bytes)[result-1]^=1;++corrupted_startup_control_reads;
  }
  if(corrupt_startup_system_read&&result>=8192&&platform::LoadLittle32(static_cast<const byte*>(bytes)+16)==8){
    static_cast<byte*>(bytes)[result-1]^=1;++corrupted_startup_system_reads;
  }
  return result;
}
extern "C" void __real_free(void*);
extern "C" void __wrap_free(void* pointer) {
  if(pointer&&observe_guarded_releases)++guarded_release_calls;
  if(pointer&&observe_handoff_releases&&SelectedGuardDepth())++guarded_handoff_releases;
  if(pointer&&cleanup_hook){const auto hook=std::exchange(cleanup_hook,nullptr);hook();}
  __real_free(pointer);
}

namespace {
namespace enrollment=scratchbird::tests::native_local_service;
using ConfigurationError=server::NativeLocalServiceStartupConfigurationError;
using AgentError=server::NativeLocalServiceAgentConfigurationError;
using EnvelopeError=server::NativeDatabaseMemoryEnvelopeError;
using PolicyError=server::NativeAgentPolicyError;

// SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS: the open-time refusal each
// catalog index fault variant must produce.
runtime_security::NativeCatalogIndexVerificationError ExpectedIndexError(unsigned variant) {
  using I=runtime_security::NativeCatalogIndexVerificationError;
  switch(variant){
    case 1:return I::dependency_map_digest;
    case 2:return I::missing_dependency_map;
    case 3:return I::dependency_map_binding;
    case 4:return I::missing_key_profile;
    case 5:return I::name_relation_identity;
    case 6:return I::missing_visibility_profile;
    default:return I::none;
  }
}

PolicyError ExpectedPolicyError(unsigned variant) {
  switch(variant){
    case 1:case 2:case 18:return PolicyError::invalid_property;
    case 3:case 4:case 5:case 23:case 24:case 27:case 28:case 29:return PolicyError::schema_refused;
    case 6:case 13:case 14:case 19:case 20:case 21:return PolicyError::lifecycle_invariant;
    case 7:return PolicyError::descriptor_mismatch;
    case 8:return PolicyError::generation_mismatch;
    case 9:return PolicyError::metric_contract_mismatch;
    case 10:return PolicyError::dependency_missing;
    case 11:case 12:return PolicyError::dependency_cycle;
    case 15:return PolicyError::cluster_admission_required;
    case 16:return PolicyError::unknown_agent;
    default:return PolicyError::none;
  }
}

void ChangeMemoryEnvelopeFixture(db::NativeCreationCatalogSeed& seed,unsigned variant) {
  if(!variant)return;
  auto& rows=seed[4];
  const auto found=std::find_if(rows.begin(),rows.end(),[](const auto& row){
    return row.metadata.record.header.object_uuid.value==security_fixture::Id(12000);});
  Check(found!=rows.end(),"actual source memory body for envelope admission");
  auto decoded=catalog::DecodeCatalogMemoryConfiguration(found->metadata.record.payload);
  Check(decoded.ok(),"actual typed source memory body");
  auto& memory=*decoded.record;
  // Variant 12 needs its context limit above the base credit and below base
  // credit plus its 16 MiB loan, under the database ceiling. Measured
  // 2026-11-03 with every agent type's principal enrolled: base context credit
  // 58.5 MB, above the 48 MiB it had under a 64 MiB ceiling. So 96 MiB / 64 MiB.
  memory.hard_limit_bytes=variant==2?64*1024:variant==12?96*1024*1024:64*1024*1024;
  memory.soft_limit_bytes=memory.per_context_limit_bytes=memory.hard_limit_bytes/2;
  memory.page_buffer_pool_limit_bytes=memory.hard_limit_bytes;
  memory.openssl_budget_bytes=variant==2?16*1024:4*1024*1024;
  memory.min_startup_available_bytes=0;
  if(variant==4||variant==5) {
    memory.soft_limit_bytes=1;memory.reject_over_soft_limit=variant==4;
  }
  // The original 64 MiB fixture declared a 32 MiB page cap despite larger
  // live bootstrap storage credit. It now remains an explicit refusal case.
  if(variant==6)memory.page_buffer_pool_limit_bytes=32*1024*1024;
  if(variant==7)memory.openssl_budget_bytes=1024*1024;
  if(variant==9||variant==12)memory.openssl_budget_bytes=32*1024*1024;
  if(variant==10)memory.per_context_limit_bytes=1;
  if(variant==11||variant==12) {
    memory.per_context_limit_bytes=64*1024*1024;  // variant 12: 48 MiB until 2026-11-03
    memory.failure_mode=catalog::MemoryConfigurationFailureMode::fatal_status;
    memory.zero_memory_on_allocate=memory.zero_memory_on_release=true;
  }
  if(variant==8) {
    memory.hard_limit_bytes=2ull*1024*1024*1024;
    memory.min_startup_available_bytes=memory.hard_limit_bytes;
    // This explicit stored setting isolates the real parent process ceiling.
    memory.enable_platform_memory_probe=false;
  }
  std::array<byte,2048> bytes;
  const auto encoded=catalog::EncodeCatalogMemoryConfigurationInto(memory,bytes);
  Check(encoded.ok(),"smaller selected memory limit is a valid persisted configuration");
  found->metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
  if(variant==3){
    auto& security=seed[3];
    const auto grant=std::find_if(security.begin(),security.end(),[](const auto& row){
      return row.metadata.record.header.object_uuid.value==security_fixture::Id(15100);});
    Check(grant!=security.end(),"actual enrolled service CONNECT grant for denial fixture");
    security.erase(grant);
  }
  if(variant==13){
    unsigned changed=0;
    for(auto& row:seed[3]){
      const auto id=row.metadata.record.header.object_uuid.value;
      if(id!=security_fixture::Id(15100)&&id!=security_fixture::Id(15102))continue;
      auto grant=catalog::DecodeCatalogSecurityRecord(row.metadata.record.header.kind,row.metadata.record.payload);
      Check(grant.ok(),"actual service grant decodes before database-target denial fixture");
      grant.record->attributes["right"]="CONNECT";
      if(id==security_fixture::Id(15100)){
        grant.record->identities["target_uuid"]=Id(1);grant.record->attributes["effect"]="deny";
      }
      const auto encoded=catalog::EncodeCatalogSecurityRecord(*grant.record);
      Check(encoded.ok(),"targeted denial and separate global allow remain canonical security records");
      row.metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());++changed;
    }
    Check(changed==2,"database denial fixture retains both the target deny and a distinct global CONNECT allow");
  }
}

// Test-only byte fingerprint, with no allocation while selected guards are
// held. Exercise the actual borrowed payloads again after handoff and move.
std::uint64_t AgentPayloadFingerprint(const server::NativeLocalServiceAgentConfigurationResult& selected) {
  std::uint64_t value=14695981039346656037ULL;
  for(const auto& entry:selected.entries())
    for(const auto text:{entry.definition.agent_type,entry.definition.properties}){
      for(const unsigned char byte:text){value^=byte;value*=1099511628211ULL;}
      value^=0xff;value*=1099511628211ULL;
    }
  return value;
}

std::string CatalogVariableBytes(const catalog::CatalogMetadataVersionView& metadata,
    const std::optional<catalog::CatalogNamePayloadView>& name) {
  std::string bytes;
  const auto append=[&](std::string_view value){bytes+=std::to_string(value.size());bytes+=':';bytes+=value;};
  // Name envelopes receive actual physical version locators at construction;
  // compare their independently supplied logical fields below.
  if(!name)append(metadata.record.payload);
  append(metadata.trace_search_key);append(metadata.object_subtype);append(metadata.retention_class);
  if(name){
    if(const auto* vector=std::get_if<catalog::CatalogNameVectorView>(&*name)){
      append(vector->object_class);append(vector->default_language_tag);
    }else{
      const auto& entry=std::get<catalog::CatalogNameEntryView>(*name);
      for(const auto value:{entry.object_class,entry.language_tag,entry.donor_id,entry.raw_name_text,entry.display_name})append(value);
      for(const auto value:{entry.normalized_lookup_key,entry.exact_lookup_key,entry.full_path_lookup_key})
        append({reinterpret_cast<const char*>(value.data()),value.size()});
    }
  }
  return bytes;
}

void AddDatabaseDefinitionFixture(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype,
    const db::NativeFilespaceInitializationRequest& construction,unsigned name_variant,unsigned namespace_variant,unsigned filespace_variant) {
  auto input=prototype;
  const auto typed=[](platform::UuidKind kind,unsigned id){return platform::TypedUuid{kind,security_fixture::Id(id)};};
  auto& definition=input.definition;
  definition.record.header.kind=catalog::CatalogRecordKind::database;
  definition.record.header.object_uuid=typed(platform::UuidKind::object,90000);
  definition.record.header.row_uuid=typed(platform::UuidKind::row,90001);
  definition.record.header.parent_uuid={};definition.owning_schema_uuid={};
  definition.record.payload.clear();definition.object_subtype="database";
  definition.default_name_uuid=typed(platform::UuidKind::object,90002);
  definition.name_vector_uuid=typed(platform::UuidKind::object,90003);
  auto& vector_metadata=input.vector_metadata;vector_metadata=definition;
  vector_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
  vector_metadata.record.header.object_uuid=definition.name_vector_uuid;
  vector_metadata.record.header.row_uuid=typed(platform::UuidKind::row,90004);
  vector_metadata.record.header.parent_uuid=definition.record.header.object_uuid;
  vector_metadata.object_subtype="name_vector";
  auto& entry_metadata=input.entry_metadata;entry_metadata=definition;
  entry_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
  entry_metadata.record.header.object_uuid=definition.default_name_uuid;
  entry_metadata.record.header.row_uuid=typed(platform::UuidKind::row,90005);
  entry_metadata.record.header.parent_uuid=definition.name_vector_uuid;
  entry_metadata.object_subtype="name_entry";entry_metadata.resource_epoch=1;
  input.vector.name_vector_uuid=definition.name_vector_uuid;
  input.vector.object_uuid=definition.record.header.object_uuid;
  input.vector.object_class="database";input.vector.owning_schema_uuid.reset();
  input.vector.default_name_entry_uuid=definition.default_name_uuid;
  input.entry.name_entry_uuid=definition.default_name_uuid;input.entry.name_vector_uuid=definition.name_vector_uuid;
  input.entry.object_uuid=definition.record.header.object_uuid;input.entry.object_class="database";
  input.entry.scope_uuid=definition.record.header.object_uuid;
  input.entry.parent_schema_uuid.reset();input.entry.parent_object_uuid.reset();
  input.entry.raw_name_text=input.entry.display_name="enrolled_native_node";
  input.entry.normalized_lookup_key.assign(input.entry.raw_name_text.begin(),input.entry.raw_name_text.end());
  input.entries[0]={&input.entry_metadata,&input.entry};
  db::NativeInitialDatabaseRequest request{&construction,
      {{platform::UuidKind::object,scratchbird::core::datatypes::kDatatypeCohortV10},10,10},"en-US",
      {&definition,&vector_metadata,&input.vector,input.entries},{3,1024*1024}};
  auto file_input=input;
  auto& file=file_input.definition;
  file.record.header.kind=catalog::CatalogRecordKind::filespace;
  file.record.header.object_uuid=typed(platform::UuidKind::object,90030);
  file.record.header.row_uuid=typed(platform::UuidKind::row,90031);
  file.record.header.parent_uuid=definition.record.header.object_uuid;
  file.object_subtype="filespace";
  file.default_name_uuid=typed(platform::UuidKind::object,90032);
  file.name_vector_uuid=typed(platform::UuidKind::object,90033);
  file_input.vector_metadata=file;file_input.vector_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
  file_input.vector_metadata.record.header.object_uuid=file.name_vector_uuid;
  file_input.vector_metadata.record.header.row_uuid=typed(platform::UuidKind::row,90034);
  file_input.vector_metadata.record.header.parent_uuid=file.record.header.object_uuid;
  file_input.vector_metadata.object_subtype="name_vector";
  file_input.entry_metadata=file;file_input.entry_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
  file_input.entry_metadata.record.header.object_uuid=file.default_name_uuid;
  file_input.entry_metadata.record.header.row_uuid=typed(platform::UuidKind::row,90035);
  file_input.entry_metadata.record.header.parent_uuid=file.name_vector_uuid;
  file_input.entry_metadata.object_subtype="name_entry";file_input.entry_metadata.resource_epoch=1;
  file_input.vector.object_uuid=file.record.header.object_uuid;file_input.vector.name_vector_uuid=file.name_vector_uuid;
  file_input.vector.default_name_entry_uuid=file.default_name_uuid;file_input.vector.object_class="filespace";
  file_input.entry.object_uuid=file.record.header.object_uuid;file_input.entry.name_vector_uuid=file.name_vector_uuid;
  file_input.entry.name_entry_uuid=file.default_name_uuid;file_input.entry.object_class="filespace";
  file_input.entry.raw_name_text=file_input.entry.display_name="enrolled_primary_filespace";
  file_input.entry.normalized_lookup_key.assign(file_input.entry.raw_name_text.begin(),file_input.entry.raw_name_text.end());
  file_input.entries[0]={&file_input.entry_metadata,&file_input.entry};
  request.limits.maximum_records=6;
  auto result=db::AssembleNativeInitialDatabaseFilespaceRecords({request,
      {&file,&file_input.vector_metadata,&file_input.vector,file_input.entries}});
  Check(result.ok(),"production native assembler binds database and actual primary filespace definitions and aliases");
  for(auto& row:result.records->definitions)seed[0].push_back(std::move(row));
  for(auto& row:result.records->names){
    if(name_variant==5&&row.name)if(auto* entry=std::get_if<catalog::CatalogNameEntry>(&*row.name);entry&&entry->object_uuid.value==definition.record.header.object_uuid.value)
      entry->scope_uuid=typed(platform::UuidKind::object,91000);
    seed[1].push_back(std::move(row));
  }
  std::array<enrollment::NativeLocalServiceCreationItem,3> schema_items;
  std::array<catalog::CatalogSchemaDefinition,3> schemas;
  std::array<catalog::CatalogInitialSchemaInput,3> schema_inputs;
  for(unsigned i=0;i<schema_items.size();++i){
    auto& named=schema_items[i];named=prototype;auto& metadata=named.definition;
    metadata.record.header.kind=catalog::CatalogRecordKind::schema;
    metadata.record.header.object_uuid=typed(platform::UuidKind::object,60+i);
    metadata.record.header.row_uuid=typed(platform::UuidKind::row,90010+3*i);
    metadata.record.header.parent_uuid=i?typed(platform::UuidKind::object,60):definition.record.header.object_uuid;
    metadata.owning_schema_uuid=i?typed(platform::UuidKind::schema,60):platform::TypedUuid{};
    metadata.name_vector_uuid=typed(platform::UuidKind::object,90020+2*i);
    metadata.default_name_uuid=typed(platform::UuidKind::object,90021+2*i);
    auto& schema=schemas[i];schema.schema_object_uuid=metadata.record.header.object_uuid;
    schema.database_catalog_object_uuid=definition.record.header.object_uuid;
    if(i)schema.parent_schema_uuid=typed(platform::UuidKind::object,60);
    schema.schema_type=i==1?catalog::CatalogSchemaType::application:catalog::CatalogSchemaType::system;
    schema.permissions_policy_uuid=metadata.security_policy_uuid;
    schema.origin_transaction_uuid=security_tx;schema.origin_local_transaction_id=1;
    metadata.object_subtype=catalog::CatalogSchemaTypeName(schema.schema_type);metadata.record.payload.clear();
    named.vector_metadata=metadata;named.vector_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
    named.vector_metadata.record.header.object_uuid=metadata.name_vector_uuid;
    named.vector_metadata.record.header.row_uuid=typed(platform::UuidKind::row,90011+3*i);
    named.vector_metadata.record.header.parent_uuid=metadata.record.header.object_uuid;
    named.vector_metadata.object_subtype="name_vector";
    named.entry_metadata=metadata;named.entry_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
    named.entry_metadata.record.header.object_uuid=metadata.default_name_uuid;
    named.entry_metadata.record.header.row_uuid=typed(platform::UuidKind::row,90012+3*i);
    named.entry_metadata.record.header.parent_uuid=metadata.name_vector_uuid;
    named.entry_metadata.object_subtype="name_entry";named.entry_metadata.resource_epoch=1;
    named.vector.object_uuid=metadata.record.header.object_uuid;named.vector.name_vector_uuid=metadata.name_vector_uuid;
    named.vector.default_name_entry_uuid=metadata.default_name_uuid;named.vector.object_class="schema";
    named.vector.owning_schema_uuid=i?std::optional{metadata.owning_schema_uuid}:std::nullopt;
    named.entry.name_entry_uuid=metadata.default_name_uuid;named.entry.name_vector_uuid=metadata.name_vector_uuid;
    named.entry.object_uuid=metadata.record.header.object_uuid;named.entry.object_class="schema";
    named.entry.scope_uuid=metadata.record.header.parent_uuid;named.entry.parent_object_uuid.reset();
    named.entry.parent_schema_uuid=named.vector.owning_schema_uuid;
    named.entry.raw_name_text=named.entry.display_name=i==2?"audit":i==1?"verification_child":"verification_system";
    named.entry.normalized_lookup_key.assign(named.entry.raw_name_text.begin(),named.entry.raw_name_text.end());
    named.entries[0]={&named.entry_metadata,&named.entry};
    schema_inputs[i]={&schema,&metadata,&named.vector_metadata,&named.vector,named.entries};
  }
  auto schema_result=catalog::AssembleLocalInitialSchemaRecords(construction.bootstrap.database_uuid,
      seed[0].front().metadata,security_tx,"en-US",schema_inputs,{9,1024*1024});
  Check(schema_result.ok(),"production native schema assembler supplies the actual namespace forest");
  for(auto& row:schema_result.records->definitions)seed[0].push_back(std::move(row));
  for(auto& row:schema_result.records->names)seed[1].push_back(std::move(row));
  // Semantic faults are applied to individually valid rows only after the
  // complete production family assembly, and before native disk publication.
  const auto root_schema=typed(platform::UuidKind::object,60);
  for(auto& relation:seed)for(auto& row:relation){
    auto& m=row.metadata;
    if((namespace_variant==2||namespace_variant==6)&&m.record.header.kind==catalog::CatalogRecordKind::database){
      auto decoded=catalog::DecodeCatalogDatabaseRecord(m.record.payload);Check(decoded.ok(),"stored database payload before fault");
      if(namespace_variant==2)decoded.record->database_uuid=typed(platform::UuidKind::database,91010);
      else decoded.record->page_size=construction.bootstrap.page_size_bytes==8192?16384:8192;
      const auto encoded=catalog::EncodeCatalogDatabaseRecord(*decoded.record);Check(encoded.ok(),"valid database payload with foreign identity");
      m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    }
    if((namespace_variant==3||namespace_variant==4)&&m.record.header.object_uuid.value==root_schema.value){
      auto decoded=catalog::DecodeCatalogSchemaDefinition(m.record.payload);Check(decoded.ok(),"stored root schema payload before fault");
      if(namespace_variant==3){
        decoded.definition->parent_schema_uuid=typed(platform::UuidKind::object,61);
        m.owning_schema_uuid=typed(platform::UuidKind::schema,61);
      }else decoded.definition->database_catalog_object_uuid=typed(platform::UuidKind::object,91011);
      m.record.header.parent_uuid=namespace_variant==3?decoded.definition->parent_schema_uuid:decoded.definition->database_catalog_object_uuid;
      const auto encoded=catalog::EncodeCatalogSchemaDefinition(*decoded.definition);Check(encoded.ok(),"individually valid schema graph fault");
      m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    }
    if((namespace_variant==3||namespace_variant==4)&&row.name){
      if(auto* vector=std::get_if<catalog::CatalogNameVector>(&*row.name);vector&&vector->object_uuid.value==root_schema.value){
        if(namespace_variant==3){m.owning_schema_uuid=typed(platform::UuidKind::schema,61);vector->owning_schema_uuid=m.owning_schema_uuid;}
      }else if(auto* entry=std::get_if<catalog::CatalogNameEntry>(&*row.name);entry&&entry->object_uuid.value==root_schema.value){
        entry->scope_uuid=typed(platform::UuidKind::object,namespace_variant==3?61:91011);
        if(namespace_variant==3){m.owning_schema_uuid=typed(platform::UuidKind::schema,61);entry->parent_schema_uuid=m.owning_schema_uuid;}
      }
    }
  }
  if(filespace_variant){
    for(auto& relation:seed)for(auto& row:relation){
      auto& m=row.metadata;
      if(m.record.header.kind==catalog::CatalogRecordKind::filespace){
        auto decoded=catalog::DecodeCatalogFilespaceRecord(m.record.payload);Check(decoded.ok(),"actual filespace manifest before fault");
        auto& manifest=*decoded.record;
        if(filespace_variant==2)manifest.database_uuid=typed(platform::UuidKind::database,91012);
        if(filespace_variant==3)manifest.filespace_uuid=typed(platform::UuidKind::filespace,91013);
        if(filespace_variant==4)manifest.filespace_role=2;
        if(filespace_variant==5)manifest.state=1;
        if(filespace_variant==6)manifest.first_filespace=false;
        if(filespace_variant==7)manifest.startup_authority=false;
        if(filespace_variant==8)m.record.header.parent_uuid=typed(platform::UuidKind::object,91014);
        const auto encoded=catalog::EncodeCatalogFilespaceRecord(manifest);Check(encoded.ok(),"individually valid filespace manifest fault");
        m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
      }
      if(filespace_variant==8&&row.name)if(auto* entry=std::get_if<catalog::CatalogNameEntry>(&*row.name);
          entry&&entry->object_uuid.value==file.record.header.object_uuid.value)
        entry->scope_uuid=typed(platform::UuidKind::object,91014);
    }
    if(filespace_variant==1)for(auto& relation:seed)std::erase_if(relation,[&](const auto& row){
      if(row.metadata.record.header.object_uuid.value==file.record.header.object_uuid.value)return true;
      return row.name&&std::visit([&](const auto& name){return name.object_uuid.value==file.record.header.object_uuid.value;},*row.name);
    });
    if(filespace_variant==9){
      // A second well-named catalog object falsely claims the same physical
      // filespace. Per-row admission cannot establish the required bijection.
      for(auto& relation:seed){
        std::decay_t<decltype(relation)> duplicate;
        for(const auto& row:relation){
          const bool selected=row.metadata.record.header.object_uuid.value==file.record.header.object_uuid.value||
            (row.name&&std::visit([&](const auto& name){return name.object_uuid.value==file.record.header.object_uuid.value;},*row.name));
          if(!selected)continue;
          auto copy=row;auto& m=copy.metadata;
          const auto remap=[&](platform::TypedUuid& id){
            for(unsigned n=0;n<6;++n)if(id.value==security_fixture::Id(90030+n))id.value=security_fixture::Id(91020+n);
          };
          remap(m.record.header.object_uuid);remap(m.record.header.row_uuid);remap(m.record.header.parent_uuid);
          remap(m.default_name_uuid);remap(m.name_vector_uuid);
          if(copy.name)std::visit([&](auto& name){remap(name.object_uuid);remap(name.name_vector_uuid);},*copy.name);
          if(copy.name)if(auto* vector=std::get_if<catalog::CatalogNameVector>(&*copy.name))remap(vector->default_name_entry_uuid);
          else{auto& entry=std::get<catalog::CatalogNameEntry>(*copy.name);remap(entry.name_entry_uuid);
            entry.raw_name_text=entry.display_name="duplicate_primary_manifest";
            entry.normalized_lookup_key.assign(entry.raw_name_text.begin(),entry.raw_name_text.end());}
          duplicate.push_back(std::move(copy));
        }
        for(auto& row:duplicate)relation.push_back(std::move(row));
      }
    }
  }
  if(namespace_variant==1||namespace_variant==5){
    const auto removed=namespace_variant==1?root_schema:definition.record.header.object_uuid;
    for(auto& relation:seed)std::erase_if(relation,[&](const auto& row){
      if(row.metadata.record.header.object_uuid.value==removed.value)return true;
      return row.name&&std::visit([&](const auto& name){return name.object_uuid.value==removed.value;},*row.name);
    });
  }
}

// Actual named native catalog rows, using the same declarations as embedded
// CREATE. Identities are explicit fixture inputs, never startup authority.
AgentError AddAgentConfigurationFixture(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype,unsigned variant,unsigned policy_variant=0) {
  namespace agents=scratchbird::core::agents;
  using K=catalog::AgentBootstrapDefinitionKind;
  const auto declarations=agents::DefaultAgentBootstrapDeclarations();
  Check(declarations.size()==66,"complete default declarations: 32 agents and 34 policies");
  if(variant==1)return AgentError::absent;
  std::vector<enrollment::NativeLocalServiceCreationItem> items;
  items.reserve(declarations.size());
  AgentError expected=AgentError::none;
  const auto change=[](std::string& body,std::string_view key,std::string_view value){
    const auto prefix=std::string(key)+"=";const auto at=body.find(prefix);
    Check(at!=std::string::npos&&(at==0||body[at-1]=='\n'),"explicit agent property exists in fixture");
    body.replace(at+prefix.size(),body.find('\n',at)-at-prefix.size(),value);
  };
  for(unsigned n=0;n<declarations.size();++n){
    const auto& declaration=declarations[n];
    if(variant==2&&declaration.agent_type=="startup_agent"){
      expected=AgentError::missing_lifecycle_agent;continue;
    }
    const auto object=security_fixture::Id(400000+n);
    auto agent_type=declaration.agent_type,properties=declaration.properties;
    catalog::CatalogAgentBootstrapDefinition definition{object,Id(1),
        security_fixture::Id(400000+declaration.primary_policy_index),declaration.kind,
        agent_type,properties,security_tx,1,1};
    if(declaration.agent_type=="startup_agent"&&declaration.kind==K::registration){
      if(variant==3){definition.database_uuid=Id(249);expected=AgentError::database_mismatch;}
      if(variant==4){definition.policy_uuid=security_fixture::Id(400000);expected=AgentError::policy_mismatch;}
      if(variant==5){properties+="starts_during_create=0\n";expected=AgentError::invalid_properties;}
      if(variant==6){change(properties,"state","disabled");expected=AgentError::policy_mismatch;}
      if(variant==7){definition.origin_transaction_uuid.value=Id(248);expected=AgentError::origin_not_committed;}
      if(variant==8){definition.policy_uuid=Id(247);expected=AgentError::policy_missing;}
      if(variant==9){change(properties,"launch_trigger","server_open");expected=AgentError::invalid_lifecycle_agent;}
      if(variant==10){change(properties,"managed_by","ipc_server");expected=AgentError::invalid_properties;}
      if(variant==11){
        agent_type="shutdown_agent";change(properties,"agent_type",agent_type);
        expected=AgentError::duplicate_registration;
      }
      if(variant==12){change(properties,"policy_families","agent_startup_policy,absent_family");expected=AgentError::policy_missing;}
      if(variant==13){change(properties,"policy_families","agent_startup_policy,agent_startup_policy");expected=AgentError::policy_mismatch;}
    }
    if(variant==14&&declaration.agent_type=="startup_agent"&&declaration.kind==K::policy){
      agent_type="shutdown_agent";change(properties,"agent_type",agent_type);
      change(properties,"policy_family","agent_shutdown_policy");expected=AgentError::duplicate_policy_family;
    }
    if(declaration.kind==K::policy&&declaration.agent_type=="memory_governor"){
      if(policy_variant==1)change(properties,"run_interval_microseconds","-1");
      if(policy_variant==2)change(properties,"run_interval_microseconds","18446744073709551616");
      if(policy_variant==7)change(properties,"layer","L5");
      if(policy_variant==8)change(properties,"policy_generation","2");
      if(policy_variant==9)change(properties,"required_metric_families","");
      if(policy_variant==10)change(properties,"policy_dependencies","missing_policy");
      if(policy_variant==11)change(properties,"policy_dependencies","memory_governor_policy");
      if(policy_variant==12)change(properties,"policy_dependencies","filespace_capacity_policy");
      if(policy_variant==17)change(properties,"run_interval_microseconds","2100000");
      if(policy_variant==18)change(properties,"action_mode","accept_any");
    }
    if(policy_variant==12&&declaration.kind==K::policy&&declaration.agent_type=="filespace_capacity_manager"&&
        properties.find("policy_family=filespace_capacity_policy\n")!=std::string::npos)
      change(properties,"policy_dependencies","memory_governor_policy");
    if(declaration.kind==K::policy&&declaration.agent_type=="startup_agent"){
      if(policy_variant==3){const auto at=properties.find("config.readiness_timeout_microseconds=");
        Check(at!=std::string::npos,"required startup schema field exists");properties.erase(at,properties.find('\n',at)-at+1);}
      if(policy_variant==4)properties+="config.forged_gate=true\n";
      if(policy_variant==5)change(properties,"config.engine_verification_required","maybe");
      if(policy_variant==6)change(properties,"config.engine_verification_required","false");
      if(policy_variant==13)change(properties,"config.dependency_order","memory_governor,memory_governor");
      if(policy_variant==14)change(properties,"config.dependency_order","cluster_autoscale_manager");
      if(policy_variant==21)change(properties,"config.configured_components_only","false");
      if(policy_variant==22){change(properties,"require_manual_approval","1");change(properties,"require_dry_run_before_live","1");}
    }
    if(policy_variant==19&&declaration.kind==K::policy&&declaration.agent_type=="agent_coordinator")
      change(properties,"config.worker_launch_forbidden","false");
    if(declaration.kind==K::policy&&declaration.agent_type=="agent_coordinator"){
      if(policy_variant==23)change(properties,"config.startup_verification_class","skip");
      if(policy_variant==24){const auto at=properties.find("config.startup_verification_class=");
        Check(at!=std::string::npos,"explicit selected startup verification class exists");
        properties.erase(at,properties.find('\n',at)-at+1);}
      if(policy_variant==25)change(properties,"config.startup_verification_class","startup_light_verify");
      if(policy_variant==27)change(properties,"config.startup_uuid_same_ms_wait_timeout_ms","0");
      if(policy_variant==28){const auto at=properties.find("config.startup_uuid_same_ms_wait_timeout_ms=");
        Check(at!=std::string::npos,"persisted startup clock policy is present");properties.erase(at,properties.find('\n',at)-at+1);}
      if(policy_variant==29)change(properties,"config.startup_clock_max_backward_ns","18446744073709551616");
    }
    if(policy_variant==20&&declaration.kind==K::policy&&declaration.agent_type=="shutdown_agent")
      change(properties,"config.cooperative_cancellation_only","false");
    if(policy_variant==15&&declaration.agent_type=="cluster_autoscale_manager"){
      if(declaration.kind==K::registration)change(properties,"state","registered");
      else {change(properties,"enabled","1");change(properties,"activation","recommend_only");change(properties,"action_mode","request_action");}
    }
    if(policy_variant==16&&declaration.agent_type=="memory_governor"){
      agent_type="future_memory";change(properties,"agent_type",agent_type);
    }
    definition.agent_type=agent_type;definition.properties=properties;
    if(definition.kind==K::policy&&definition.agent_type=="agent_coordinator"&&policy_variant!=26)
      definition.startup_verification=catalog::CatalogAgentStartupVerificationSelection{
          security_fixture::Id(92000),security_fixture::Id(92001),security_fixture::Id(92002),security_fixture::Id(92003),3};
    const auto encoded=catalog::EncodeCatalogAgentBootstrapDefinition(definition);
    Check(encoded.ok(),"agent refusal fixtures remain legal typed native frames");
    auto& item=items.emplace_back(prototype);auto& m=item.definition;
    m.record.header.object_uuid={platform::UuidKind::object,object};
    m.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,500000+3*n);
    m.record.header.kind=declaration.kind==K::policy?catalog::CatalogRecordKind::policy:catalog::CatalogRecordKind::config_profile;
    m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    m.object_subtype=declaration.kind==K::policy?"agent_policy":"agent_registration";
    m.default_name_uuid=enrollment::Typed(platform::UuidKind::object,600000+2*n);
    m.name_vector_uuid=enrollment::Typed(platform::UuidKind::object,600001+2*n);
    item.vector_metadata=m;item.vector_metadata.record.payload.clear();
    item.vector_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
    item.vector_metadata.record.header.object_uuid=m.name_vector_uuid;
    item.vector_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,500001+3*n);
    item.vector_metadata.record.header.parent_uuid=m.record.header.object_uuid;
    item.vector_metadata.object_subtype="name_vector";
    item.vector.name_vector_uuid=m.name_vector_uuid;item.vector.object_uuid=m.record.header.object_uuid;
    item.vector.object_class=catalog::CatalogRecordKindName(m.record.header.kind);
    item.vector.default_name_entry_uuid=m.default_name_uuid;
    item.entry_metadata=item.vector_metadata;item.entry_metadata.record.header.object_uuid=m.default_name_uuid;
    item.entry_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,500002+3*n);
    item.entry_metadata.record.header.parent_uuid=m.name_vector_uuid;item.entry_metadata.object_subtype="name_entry";
    item.entry_metadata.resource_epoch=1;
    item.entry.name_entry_uuid=m.default_name_uuid;item.entry.name_vector_uuid=m.name_vector_uuid;
    item.entry.object_uuid=m.record.header.object_uuid;item.entry.object_class=item.vector.object_class;
    item.entry.raw_name_text=item.entry.display_name="native_agent_alias_"+std::to_string(n);
    item.entry.normalized_lookup_key.assign(item.entry.raw_name_text.begin(),item.entry.raw_name_text.end());
  }
  std::vector<catalog::CatalogNamedCreationInput> named;
  for(auto& item:items){item.entries[0]={&item.entry_metadata,&item.entry};
    named.push_back({&item.definition,&item.vector_metadata,&item.vector,item.entries});}
  auto result=catalog::AssembleLocalNamedCreationRecords(Id(1),security_tx,"en-US",named,{1000,4*1024*1024});
  Check(result.ok(),"complete named agent definitions assemble atomically before native persistence");
  for(auto& row:result.records->definitions)seed[4].push_back(std::move(row));
  for(auto& row:result.records->names)seed[1].push_back(std::move(row));
  if(!variant)Check(seed[4].size()==95&&seed[1].size()==1594,"all 66 agent definitions and 132 names join native creation");  // 1462 pack names (616 before agent principals, 2026-11-03) + 132
  Check(!variant||expected!=AgentError::none,"known agent configuration refusal fixture");
  return expected;
}

// SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS (owner decision 2026-11-04,
// option a): the production builder's key profile, visibility profile and one
// dependency map per indexed relation, and the index bindings that reference
// them. They replace constant fixture tuples (a digest filled with 1+role and
// profile identities that named no row). The names relation takes the shared
// constant identity. None of these rows is addressed by name, so each carries
// its UUID text as its reference name (owner 2026-11-04).
db::NativeCreationCatalogIndexes AddCatalogIndexRowsFixture(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype) {
  db::NativeCatalogIndexRowsRequest request{Id(1),security_tx,1,{}};
  for(unsigned i:{1u,3u,4u,5u})request.roles[i]=db::NativeCatalogIndexRoleIdentities{
      i==1?platform::Uuid{}:security_fixture::Id(8000+i),security_fixture::Id(8010+i),security_fixture::Id(8040+i)};
  const auto built=db::BuildNativeCatalogIndexRows(request);
  Check(built.ok()&&built.rows.size()==6,"production builder issues both profiles and one map per indexed relation");
  Check(built.indexes[1]&&built.indexes[1]->relation_uuid==catalog::kNativeNameCatalogRelationUuid,
      "the names relation carries the shared constant identity");
  std::vector<enrollment::NativeLocalServiceCreationItem> items;items.reserve(built.rows.size());
  for(unsigned n=0;n<built.rows.size();++n){
    const auto& row=built.rows[n];
    auto& item=items.emplace_back(prototype);auto& m=item.definition;
    m.record.header.kind=catalog::CatalogRecordKind::config_profile;
    m.record.header.object_uuid={platform::UuidKind::object,row.object_uuid};
    m.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,700000+3*n);
    m.record.payload=row.payload;m.object_subtype=std::string(row.object_subtype);
    m.default_name_uuid=enrollment::Typed(platform::UuidKind::object,800000+2*n);
    m.name_vector_uuid=enrollment::Typed(platform::UuidKind::object,800001+2*n);
    item.vector_metadata=m;item.vector_metadata.record.payload.clear();
    item.vector_metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
    item.vector_metadata.record.header.object_uuid=m.name_vector_uuid;
    item.vector_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,700001+3*n);
    item.vector_metadata.record.header.parent_uuid=m.record.header.object_uuid;
    item.vector_metadata.object_subtype="name_vector";
    item.vector.name_vector_uuid=m.name_vector_uuid;item.vector.object_uuid=m.record.header.object_uuid;
    item.vector.object_class=catalog::CatalogRecordKindName(m.record.header.kind);
    item.vector.default_name_entry_uuid=m.default_name_uuid;
    item.entry_metadata=item.vector_metadata;item.entry_metadata.record.header.object_uuid=m.default_name_uuid;
    item.entry_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,700002+3*n);
    item.entry_metadata.record.header.parent_uuid=m.name_vector_uuid;item.entry_metadata.object_subtype="name_entry";
    item.entry_metadata.resource_epoch=1;
    item.entry.name_entry_uuid=m.default_name_uuid;item.entry.name_vector_uuid=m.name_vector_uuid;
    item.entry.object_uuid=m.record.header.object_uuid;item.entry.object_class=item.vector.object_class;
    item.entry.raw_name_text=item.entry.display_name=scratchbird::core::uuid::UuidToString(row.object_uuid);
    item.entry.normalized_lookup_key.assign(item.entry.raw_name_text.begin(),item.entry.raw_name_text.end());
  }
  std::vector<catalog::CatalogNamedCreationInput> named;
  for(auto& item:items){item.entries[0]={&item.entry_metadata,&item.entry};
    named.push_back({&item.definition,&item.vector_metadata,&item.vector,item.entries});}
  auto result=catalog::AssembleLocalNamedCreationRecords(Id(1),security_tx,"en-US",named,{100,1024*1024});
  Check(result.ok(),"catalog index rows assemble as complete named definitions");
  for(auto& row:result.records->definitions)seed[4].push_back(std::move(row));
  for(auto& row:result.records->names)seed[1].push_back(std::move(row));
  return built.indexes;
}

ConfigurationError ChangeConfigurationFixture(db::NativeCreationCatalogSeed& seed,unsigned variant) {
  // Corrupt owning-catalog inputs before physical fixture construction. These
  // are deliberate independent runtime-admission probes, never a production
  // policy mutation or permission to skip complete reference admission.
  auto& rows=seed[4];
  const auto find=[&](platform::Uuid id){return std::find_if(rows.begin(),rows.end(),
      [&](const auto& row){return row.metadata.record.header.object_uuid.value==id;});};
  auto provider=find(enrollment::SourceId(0x400));auto profile=find(enrollment::SourceId(0x403));
  auto memory=find(security_fixture::Id(12000));
  Check(provider!=rows.end()&&profile!=rows.end()&&memory!=rows.end(),"actual assembled configuration objects for refusal fixture");
  if(variant==1){rows.erase(provider);return ConfigurationError::provider_selection_absent;}
  if(variant==2){rows.erase(profile);return ConfigurationError::memory_profile_absent;}
  if(variant==3){rows.erase(memory);return ConfigurationError::memory_absent;}
  if(variant==4){memory->metadata.lifecycle=catalog::CatalogObjectLifecycle::archived;
    memory->metadata.status=catalog::CatalogObjectStatus::retired;memory->metadata.retired_transaction_uuid=security_tx;
    return ConfigurationError::memory_inactive;}
  if(variant==5){profile->metadata.lifecycle=catalog::CatalogObjectLifecycle::archived;
    profile->metadata.status=catalog::CatalogObjectStatus::retired;profile->metadata.retired_transaction_uuid=security_tx;
    return ConfigurationError::memory_profile_inactive;}
  std::array<byte,2048> backing;
  if(variant>=6&&variant<=8){
    auto& selected=variant==6?provider->metadata:profile->metadata;
    auto decoded=catalog::DecodeCatalogBootstrapPolicyProfile(selected.record.payload);
    Check(decoded.ok(),"actual profile refusal fixture decode");
    if(variant==6)decoded.record->mode="external_provider_required";
    if(variant==7)decoded.record->resource_definition_generation=2;
    if(variant==8)decoded.record->policy_pack_generation=2;
    const auto encoded=catalog::EncodeCatalogBootstrapPolicyProfileInto(*decoded.record,backing);
    Check(encoded.ok(),"typed profile refusal fixture framing");selected.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    return variant==6?ConfigurationError::provider_selection_unsupported:
        variant==7?ConfigurationError::memory_binding_mismatch:ConfigurationError::pack_generation_mismatch;
  }
  if(variant==9||variant==10){
    auto decoded=catalog::DecodeCatalogMemoryConfiguration(memory->metadata.record.payload);
    Check(decoded.ok(),"actual memory body refusal fixture decode");
    if(variant==9)decoded.record->profile_uuid=Id(249);else decoded.record->policy_pack_generation=2;
    const auto encoded=catalog::EncodeCatalogMemoryConfigurationInto(*decoded.record,backing);
    Check(encoded.ok(),"typed memory body refusal fixture framing");memory->metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    return ConfigurationError::memory_binding_mismatch;
  }
  Check(!variant,"known configuration refusal variant");return ConfigurationError::none;
}

// This is the provisioner's fixture, separate from startup. It supplies an
// explicit sealed descriptor; startup cannot generate or authorize its secret.
struct EnrolledCredentialCustodian {
  mem::MemoryManager manager{CredentialCryptoOwner::Policy()};
  mem::MemoryTag tag;
  std::shared_ptr<server::StartupCredentialSource> source=std::make_shared<server::StartupCredentialSource>(Id(231),4);
  mem::ScopedProtectedBuffer derived;
  // Defaults are the enrollment fixture's identities; a database written by the
  // production producer supplies its receipt's (SB_NATIVE_DATABASE_CREATE_PRODUCER).
  explicit EnrolledCredentialCustodian(platform::Uuid database=Id(1),platform::Uuid principal=security_fixture::Id(15000),
      platform::Uuid provider=enrollment::SourceId(0x10),platform::Uuid credential_reference=security_fixture::Id(15002)) {
    const server::StartupCredentialTransportBinding binding{database,principal,
        provider,credential_reference,7,static_cast<std::uint32_t>(::geteuid())};
    tag.category=mem::MemoryCategory::core_runtime;
    tag.purpose="native service credential custody with heap-backed immutable purpose metadata";
    tag.callsite="native enrollment lifetime qualification with retained heap-backed callsite metadata";
    tag.binary_ownership[mem::MemoryBinaryScopeKind::database]=database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner]=binding.principal.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::context]=Id(222).bytes;
    mem::ProtectedMemoryRequest request;request.bytes=server::kStartupCredentialHeaderBytes+password.size();request.tag=tag;
    request.material_class="native service enrollment staging";
    request.platform_policy=mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto staging=manager.AllocateProtected(request);Check(staging.ok(),"actual protected provisioner staging");
    Check(server::EncodeStartupCredentialFrame(binding,
        {reinterpret_cast<const std::uint8_t*>(password.data()),password.size()},
        {static_cast<std::uint8_t*>(staging.buffer.data()),staging.buffer.size()})==server::StartupCredentialFrameError::none,
        "explicit enrolled principal/provider/reference frame");
    struct Descriptor {int fd;~Descriptor(){if(fd>=0)::close(fd);}} fd{
        ::memfd_create("native-enrolled-service-test",MFD_CLOEXEC|MFD_ALLOW_SEALING)};
    Check(fd.fd>=0,"actual independently provisioned service descriptor");
    Check(::write(fd.fd,staging.buffer.data(),staging.buffer.size())==static_cast<ssize_t>(staging.buffer.size())&&
        ::fchmod(fd.fd,0400)==0&&::fcntl(fd.fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)==0,
        "actual sealed owner-only service source");
    Check(source->Publish({Id(231),Id(232),0,{},{}},fd.fd,binding,manager,tag).ok(),
        "provisioner publishes exact separately bound source generation");
    staging.buffer={};request.bytes=32;request.material_class="native service PBKDF2 scratch";
    auto scratch=manager.AllocateProtected(request);Check(scratch.ok(),"actual locked no-dump derived workspace");
    derived=std::move(scratch.buffer);
  }
};

runtime_security::NativeVerificationPolicyRequest VerificationPolicyRequest() {
  return {security_fixture::Id(92000),security_fixture::Id(92001),security_fixture::Id(92002),
      security_fixture::Id(92003),runtime_security::NativeVerificationClass::startup_full,
      runtime_security::NativeVerificationAuthority::startup_recovery,3};
}
using VerificationPolicyError=runtime_security::NativeVerificationPolicyError;
VerificationPolicyError ExpectedVerificationPolicyError(unsigned variant) {
  using E=VerificationPolicyError;
  switch(variant){case 1:return E::missing_policy;case 2:case 14:return E::missing_body;
    case 3:case 4:case 13:case 15:return E::body_binding;
    case 5:return E::verification_class_refused;case 6:return E::authority_class_refused;
    case 7:return E::verification_level_refused;case 8:return E::evidence_binding;
    case 9:return E::invalid_descriptor;case 10:return E::missing_filespace;
    case 11:return E::access_policy_binding;case 12:return E::invalid_descriptor;
    case 16:case 17:case 18:return E::access_schema_binding;
    case 19:return E::invalid_descriptor;
    case 20:case 21:case 22:return E::invalid_body;
    default:return E::none;}
}
void AddVerificationPolicies(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype,unsigned variant) {
  using namespace catalog;
  std::array<enrollment::NativeLocalServiceCreationItem,10> items;
  std::array<catalog::CatalogNamedCreationInput,10> named;
  std::array<byte,kCatalogEvidencePolicyMaxBytes> backing;
  for(unsigned n=0;n<items.size();++n){
    auto& item=items[n];item=prototype;auto& m=item.definition;
    const auto id=security_fixture::Id(n<5?92000+n:92010+n-5);
    m.record.header.object_uuid={platform::UuidKind::object,id};
    m.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,94000+3*n);
    m.record.header.kind=CatalogRecordKind::policy;
    m.default_name_uuid=enrollment::Typed(platform::UuidKind::object,93000+2*n);
    m.name_vector_uuid=enrollment::Typed(platform::UuidKind::object,93001+2*n);
    CatalogValueEncodeIntoResult encoded;
    if(n<5){
      CatalogPolicyDescriptor d;d.policy_uuid=id;d.policy_version_uuid=security_fixture::Id(92020+n);
      d.database_uuid=Id(1);d.body_uuid=security_fixture::Id(92010+n);d.body_generation=d.generation=1;
      d.policy_class=n<3?CatalogPolicyClass::verification_repair:n==3?CatalogPolicyClass::evidence:CatalogPolicyClass::authorization;
      d.origin_transaction_uuid=d.creating_transaction_uuid=security_tx;
      d.origin_local_transaction_id=d.creating_transaction_number=1;
      d.scope=CatalogPolicyScope::database;d.scope_uuid=Id(1);d.authority_owner_uuid=m.owner_uuid.value;
      d.precedence_rank=1;d.enforcement_points=1;
      if(n<3)d.evidence_profile_uuid=security_fixture::Id(92003);
      if(n==0&&variant==3)d.body_generation=2;
      if(n==0&&variant==4)d.body_uuid=security_fixture::Id(92011);
      if(n==3&&variant==9)d.policy_class=CatalogPolicyClass::audit;
      if(n==4&&variant==11)d.policy_class=CatalogPolicyClass::audit;
      if(n==3&&variant==13)d.body_generation=2;
      if(n==4&&variant==15)d.body_generation=2;
      if(n==0&&variant==12)d.policy_class=CatalogPolicyClass::authorization;
      encoded=EncodeCatalogPolicyDescriptorInto(d,backing);m.object_subtype="policy_descriptor";
    }else if(n<8){
      CatalogVerificationPolicyBody b;b.body_uuid=id;b.database_uuid=Id(1);b.generation=1;
      b.role=VerificationPolicyRole(n-4);b.verification_class_mask=511;b.authority_class_mask=63;b.verification_level_mask=1023;
      // Scan pages 512 until 2026-11-03; the file is now 1024 pages (2048 for
      // the history case) because the complete catalog needs about 469.
      b.maximum_scan_pages=4096;b.maximum_work_bytes=128*1024*1024;b.maximum_findings=64;
      b.evidence_policy_uuid=security_fixture::Id(92003);
      b.origin_transaction_uuid=b.creating_transaction_uuid=security_tx;b.origin_local_transaction_id=b.creating_local_transaction_id=1;
      if(n==6)b.access_policy_uuid=security_fixture::Id(92004);
      if(n==7)b.flags|=kVerificationPolicyMarkSalvagedOutput;
      if(n==5&&variant==5)b.verification_class_mask=1;
      if(n==5&&variant==6)b.authority_class_mask=2;
      if(n==5&&variant==7)b.verification_level_mask=1;
      if(n==5&&variant==8)b.evidence_policy_uuid=security_fixture::Id(92004);
      encoded=EncodeCatalogVerificationPolicyInto(b,backing);m.object_subtype="verification_policy_body";
    }else if(n==8){
      CatalogEvidencePolicyBody b;b.body_uuid=id;b.database_uuid=Id(1);b.generation=1;
      b.required_evidence_classes.entries[0]="verification";b.required_evidence_classes.count=1;
      b.required_search_keys.entries[0]="SB-MGA-VERIFY";b.required_search_keys.count=1;
      b.storage_filespace_uuid=variant==10?security_fixture::Id(92099):Id(2);
      b.maximum_record_bytes=4096;b.maximum_batch_bytes=65536;
      b.origin_transaction_uuid=b.creating_transaction_uuid=security_tx;b.origin_local_transaction_id=b.creating_local_transaction_id=1;
      encoded=EncodeCatalogEvidencePolicyInto(b,backing);m.object_subtype="evidence_policy_body";
    }else{
      CatalogAuthorizationPolicyBody b;b.body_uuid=id;b.database_uuid=Id(1);b.generation=1;
      b.user_schema_uuid=b.role_schema_uuid=b.group_schema_uuid=b.grant_schema_uuid=
          b.row_policy_schema_uuid=b.column_policy_schema_uuid=security_fixture::Id(60);
      // Work 65536 until 2026-11-03; the complete security relation (702
      // records with every agent type's principal) measured 103107.
      b.maximum_records=1024;b.maximum_membership_depth=32;b.maximum_work=262144;
      if(variant==23){b.maximum_records=1;b.maximum_membership_depth=1;}
      if(variant==24)b.maximum_work=1;
      b.origin_transaction_uuid=b.creating_transaction_uuid=security_tx;b.origin_local_transaction_id=b.creating_local_transaction_id=1;
      if(variant==16)b.user_schema_uuid=security_fixture::Id(92000);
      if(variant==17)b.role_schema_uuid=security_fixture::Id(92099);
      if(variant==18){b.external_mapping=AuthorizationExternalMapping::internal_snapshot;b.external_mapping_schema_uuid=security_fixture::Id(92099);}
      encoded=EncodeCatalogAuthorizationPolicyInto(b,backing);m.object_subtype="authorization_policy_body";
    }
    Check(encoded.ok(),"complete typed verification/evidence/access policy fixture encodes");
    m.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size());
    item.vector_metadata=m;item.vector_metadata.record.payload.clear();
    item.vector_metadata.record.header.kind=CatalogRecordKind::localized_name;
    item.vector_metadata.record.header.object_uuid=m.name_vector_uuid;
    item.vector_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,94001+3*n);
    item.vector_metadata.record.header.parent_uuid=m.record.header.object_uuid;item.vector_metadata.object_subtype="name_vector";
    item.entry_metadata=item.vector_metadata;item.entry_metadata.record.header.object_uuid=m.default_name_uuid;
    item.entry_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,94002+3*n);
    item.entry_metadata.record.header.parent_uuid=m.name_vector_uuid;item.entry_metadata.object_subtype="name_entry";item.entry_metadata.resource_epoch=1;
    item.vector.name_vector_uuid=item.entry.name_vector_uuid=m.name_vector_uuid;
    item.vector.object_uuid=item.entry.object_uuid=m.record.header.object_uuid;
    item.vector.default_name_entry_uuid=item.entry.name_entry_uuid=m.default_name_uuid;
    item.vector.object_class=item.entry.object_class="policy";
    item.entry.raw_name_text=item.entry.display_name="verification_fixture_"+std::to_string(n);
    item.entry.normalized_lookup_key.assign(item.entry.raw_name_text.begin(),item.entry.raw_name_text.end());
    item.entries[0]={&item.entry_metadata,&item.entry};
    named[n]={&m,&item.vector_metadata,&item.vector,item.entries};
  }
  auto result=AssembleLocalNamedCreationRecords(Id(1),security_tx,"en-US",named,{30,1024*1024});
  Check(result.ok(),"actual verification policy declarations and names assemble before native creation");
  const auto removed=variant==1?security_fixture::Id(92000):variant==2?security_fixture::Id(92010):variant==14?security_fixture::Id(92014):platform::Uuid{};
  for(auto& row:result.records->definitions){
    const auto id=row.metadata.record.header.object_uuid.value;
    // Fault injection follows valid named-creation assembly. The physical
    // fixture then carries a disabled definition with its original names.
    if((variant==19&&id==security_fixture::Id(92000))||
        (variant==20&&id==security_fixture::Id(92010))||
        (variant==21&&id==security_fixture::Id(92013))||
        (variant==22&&id==security_fixture::Id(92014)))
      row.metadata.status=CatalogObjectStatus::disabled_by_policy;
    if(id!=removed)seed[4].push_back(std::move(row));
  }
  for(auto& row:result.records->names)if(!row.name||!std::visit([&](const auto& name){return name.object_uuid.value==removed;},*row.name))seed[1].push_back(std::move(row));
}

runtime_security::NativeStartupManagementDefinitionError ExpectedManagementError(unsigned variant){
  using E=runtime_security::NativeStartupManagementDefinitionError;
  switch(variant){case 1:return E::missing_descriptor;case 2:return E::missing_profile;
    case 3:case 4:return E::profile_code_mismatch;case 6:case 9:return E::invalid_profile;
    case 7:return E::invalid_descriptor;case 8:return E::duplicate_descriptor;
    default:return E::none;}
}
void AddManagementDefinitions(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype,unsigned variant){
  using namespace catalog;
  std::array<enrollment::NativeLocalServiceCreationItem,12> items;
  std::vector<CatalogNamedCreationInput> named;
  std::array<byte,kCatalogManagementDescriptorMaxBytes> backing;
  for(unsigned n=0;n<(variant==8?12u:11u);++n){
    if((variant==1&&n==10)||(variant==2&&n==0))continue;
    auto& item=items[n];item=prototype;auto& m=item.definition;
    const auto id=security_fixture::Id(n<10?95000+n:95020+n-10);
    m.record.header.object_uuid={platform::UuidKind::object,id};
    m.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,97000+3*n);
    m.record.header.kind=CatalogRecordKind::management_operation;
    m.default_name_uuid=enrollment::Typed(platform::UuidKind::object,96000+2*n);
    m.name_vector_uuid=enrollment::Typed(platform::UuidKind::object,96001+2*n);
    CatalogValueEncodeIntoResult encoded;
    if(n<10){CatalogManagementProfile p;p.profile_uuid=id;p.database_uuid=Id(1);p.generation=1;
      p.code=CatalogManagementProfileCode(variant==4&&n==0?2:n+1);
      p.origin_transaction_uuid=p.creating_transaction_uuid=security_tx;p.origin_local_transaction_id=p.creating_local_transaction_id=1;
      encoded=EncodeCatalogManagementProfileInto(p,backing);m.object_subtype="management_profile";
    }else{CatalogManagementOperationDescriptor d;d.descriptor_uuid=id;d.database_uuid=Id(1);d.generation=1;
      d.code=CatalogManagementOperationCode::verify_start;d.flags=31;
      for(unsigned i=0;i<10;++i)d.profiles[i]=security_fixture::Id(95000+i);
      if(variant==3)std::swap(d.profiles[0],d.profiles[1]);
      if(variant==9)d.profiles[0]=security_fixture::Id(92000);
      d.origin_transaction_uuid=d.creating_transaction_uuid=security_tx;d.origin_local_transaction_id=d.creating_local_transaction_id=1;
      encoded=EncodeCatalogManagementDescriptorInto(d,backing);m.object_subtype="management_descriptor";
    }
    Check(encoded.ok(),"actual registered management definitions encode before native construction");
    m.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size());
    item.vector_metadata=m;item.vector_metadata.record.payload.clear();item.vector_metadata.record.header.kind=CatalogRecordKind::localized_name;
    item.vector_metadata.record.header.object_uuid=m.name_vector_uuid;
    item.vector_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,97001+3*n);
    item.vector_metadata.record.header.parent_uuid=m.record.header.object_uuid;item.vector_metadata.object_subtype="name_vector";
    item.entry_metadata=item.vector_metadata;item.entry_metadata.record.header.object_uuid=m.default_name_uuid;
    item.entry_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,97002+3*n);
    item.entry_metadata.record.header.parent_uuid=m.name_vector_uuid;item.entry_metadata.object_subtype="name_entry";item.entry_metadata.resource_epoch=1;
    item.vector.name_vector_uuid=item.entry.name_vector_uuid=m.name_vector_uuid;
    item.vector.object_uuid=item.entry.object_uuid=m.record.header.object_uuid;
    item.vector.default_name_entry_uuid=item.entry.name_entry_uuid=m.default_name_uuid;
    item.vector.object_class=item.entry.object_class="management_operation";
    item.entry.raw_name_text=item.entry.display_name="management_fixture_"+std::to_string(n);
    item.entry.normalized_lookup_key.assign(item.entry.raw_name_text.begin(),item.entry.raw_name_text.end());
    item.entries[0]={&item.entry_metadata,&item.entry};named.push_back({&m,&item.vector_metadata,&item.vector,item.entries});
  }
  auto result=AssembleLocalNamedCreationRecords(Id(1),security_tx,"en-US",named,{36,1024*1024});
  Check(result.ok(),"complete management profiles and descriptor assemble with real named metadata");
  for(auto& row:result.records->definitions){const auto id=row.metadata.record.header.object_uuid.value;
    const bool wrong_role=(variant==6&&id==security_fixture::Id(95000))||(variant==7&&id==security_fixture::Id(95020));
    seed[wrong_role?4:5].push_back(std::move(row));}
  for(auto& row:result.records->names)seed[1].push_back(std::move(row));
}

runtime_security::NativeTransactionPolicyError ExpectedTransactionPolicyError(unsigned variant){
  using E=runtime_security::NativeTransactionPolicyError;
  switch(variant){case 1:return E::invalid_selection;case 2:case 3:case 7:return E::body_binding;
    case 4:return E::missing_isolation;case 5:case 9:return E::invalid_isolation;
    case 6:case 13:return E::invalid_descriptor;case 8:case 15:case 16:return E::evidence_binding;
    case 10:case 14:return E::missing_descriptor;case 11:return E::invalid_body;
    case 12:case 17:case 18:return E::unsupported_reference;default:return E::none;}
}
void AddTransactionPolicies(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype,unsigned variant){
  using namespace catalog;
  std::array<enrollment::NativeLocalServiceCreationItem,7> items;
  const unsigned item_count=variant>=31&&variant<=40?7:6;
  std::vector<CatalogNamedCreationInput> named;
  std::array<byte,kCatalogTransactionPolicyMaxBytes> backing;
  for(unsigned n=0;n<item_count;++n){
    auto& item=items[n];item=prototype;auto& m=item.definition;
    const auto id=security_fixture::Id(n<2?98000+n:n<4?98010+n-2:98020+n-4);
    m.record.header.object_uuid={platform::UuidKind::object,id};
    m.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,100000+3*n);
    m.record.header.kind=CatalogRecordKind::policy;
    m.default_name_uuid=enrollment::Typed(platform::UuidKind::object,99000+2*n);
    m.name_vector_uuid=enrollment::Typed(platform::UuidKind::object,99001+2*n);
    CatalogValueEncodeIntoResult encoded;
    if(n<2||n==6){CatalogPolicyDescriptor d;d.policy_uuid=id;d.policy_version_uuid=security_fixture::Id(98030+n);
      d.database_uuid=Id(1);d.body_uuid=security_fixture::Id(98010+n);d.body_generation=d.generation=1;
      d.policy_class=n==0?CatalogPolicyClass::transaction:CatalogPolicyClass::lock_admission;
      d.origin_transaction_uuid=d.creating_transaction_uuid=security_tx;d.origin_local_transaction_id=d.creating_transaction_number=1;
      d.scope=CatalogPolicyScope::database;d.scope_uuid=Id(1);d.authority_owner_uuid=m.owner_uuid.value;
      d.precedence_rank=1;d.enforcement_points=1|(1ULL<<7);d.default_decision=CatalogPolicyDefault::allow;
      d.evidence_profile_uuid=security_fixture::Id(92003);
      if((n==0&&variant==15)||(n==1&&variant==16))d.evidence_profile_uuid=security_fixture::Id(92004);
      if(n==0&&variant==17)d.disclosure_profile_uuid=security_fixture::Id(98099);
      if(n==1&&variant==18)d.retention_profile_uuid=security_fixture::Id(98099);
      if((n==0&&variant==19)||(n==1&&variant==24))d.default_decision=CatalogPolicyDefault::deny;
      if((n==0&&variant==25)||(n==1&&variant==26))d.enforcement_points=1;
      if(n==0&&variant==23)d.default_decision=CatalogPolicyDefault::require_higher_authority;
      if(n==0&&variant==27)d.effective_transaction_number=3;
      if(n==0&&variant==28)d.superseding_transaction_number=2;
      if(n==0&&variant==3)d.body_generation=2;
      if(n==1&&variant==6)d.policy_class=CatalogPolicyClass::authorization;
      if(n==1&&variant==7)d.body_generation=2;
      if(n==6){d.body_uuid=security_fixture::Id(98010);d.policy_class=CatalogPolicyClass::transaction;
        d.default_decision=CatalogPolicyDefault::deny;
        if(variant==32){d.scope=CatalogPolicyScope::user;d.scope_uuid=security_fixture::Id(15000);}
        if(variant==33){d.default_decision=CatalogPolicyDefault::allow;d.policy_class=CatalogPolicyClass::metadata;}
        if(variant==34)d.effective_transaction_number=3;
        if(variant==35)d.enforcement_points=1;
        if(variant>=36&&variant<=38){d.scope=variant==36?CatalogPolicyScope::role:
            variant==37?CatalogPolicyScope::group:CatalogPolicyScope::external_authorization;
          d.scope_uuid=security_fixture::Id(15000);}
        if(variant==39){d.scope=CatalogPolicyScope::object;d.scope_uuid=Id(1);}
        if(variant==40){d.scope=CatalogPolicyScope::filespace;d.scope_uuid=Id(2);}
        d.cache_invalidation_mask=255;
      }
      encoded=EncodeCatalogPolicyDescriptorInto(d,backing);m.object_subtype="policy_descriptor";
    }else if(n==2){CatalogTransactionPolicyBody b;b.body_uuid=id;b.database_uuid=Id(1);b.generation=1;
      b.default_isolation_profile_uuid=security_fixture::Id(variant==9?92013:98020);b.startup_isolation_profile_uuid=security_fixture::Id(98021);
      b.allowed_isolation_profiles[0]=b.default_isolation_profile_uuid;b.allowed_isolation_profiles[1]=b.startup_isolation_profile_uuid;
      // Transaction lifetime, snapshot age and active count: 0, no limit, the
      // pack default (owner 2026-11-04, SB_DRIFT_TRANSACTION_POLICY_ZERO_IS_NO_LIMIT);
      // they were 60 s, 60 s and 64. Variants 30 and 41 set finite bounds.
      b.allowed_isolation_profile_count=2;b.read_mode_mask=3;b.maximum_transaction_microseconds=0;
      b.maximum_snapshot_microseconds=0;b.maximum_active_transactions=0;b.lock_policy_uuid=security_fixture::Id(98001);
      b.evidence_policy_uuid=security_fixture::Id(variant==8?92004:92003);
      if(variant==20)b.read_mode_mask=1;
      if(variant==22)b.cluster_eligibility=TransactionClusterEligibility::require_cluster_admission;
      if(variant==29)b.autocommit_mode=TransactionAutocommitMode::statement_transaction;
      if(variant==30){b.maximum_transaction_microseconds=180000000;b.maximum_snapshot_microseconds=60000000;}
      if(variant==41)b.maximum_transaction_microseconds=b.maximum_snapshot_microseconds=60000000;
      b.origin_transaction_uuid=b.creating_transaction_uuid=security_tx;b.origin_local_transaction_id=b.creating_local_transaction_id=1;
      encoded=EncodeCatalogTransactionPolicyInto(b,backing);m.object_subtype="transaction_policy_body";
    }else if(n==3){CatalogLockPolicyBody b;b.body_uuid=id;b.database_uuid=Id(1);b.generation=1;
      // Lock count, memory, wait graph and deadlock work: 0, no limit (owner
      // 2026-11-04, SB_DRIFT_LOCK_POLICY_ZERO_IS_NO_LIMIT); they were 256,
      // 64 KiB, 128, 256 and 4096.
      b.fairness_threshold_microseconds=1000000;b.maximum_locks=0;b.maximum_lock_memory_bytes=0;
      b.maximum_graph_nodes=0;b.maximum_graph_edges=0;b.maximum_deadlock_work=0;
      // Owner 2026-11-04: Firebird's 10 s DeadlockTimeout is the default (was 100 ms).
      b.deadlock_detection_interval_microseconds=10000000;
      b.evidence_policy_uuid=security_fixture::Id(92003);
      b.origin_transaction_uuid=b.creating_transaction_uuid=security_tx;b.origin_local_transaction_id=b.creating_local_transaction_id=1;
      encoded=EncodeCatalogLockPolicyInto(b,backing);m.object_subtype="lock_policy_body";
    }else{CatalogIsolationProfile b;b.profile_uuid=id;b.database_uuid=Id(1);b.generation=1;
      b.profile_kind=n==4?IsolationProfileKind::read_committed_statement:IsolationProfileKind::maintenance_operation_isolation;
      b.snapshot_construction_point=n==4?IsolationSnapshotPoint::statement_start:IsolationSnapshotPoint::operation_start;
      b.snapshot_refresh_mask=n==4?2:1;b.write_conflict_detection_mask=3;
      b.metadata_visibility=n==4?IsolationMetadataVisibility::statement_epoch:IsolationMetadataVisibility::transaction_epoch;
      b.wait_policy_uuid=b.deadlock_policy_uuid=security_fixture::Id(98001);
      b.origin_transaction_uuid=b.creating_transaction_uuid=security_tx;b.origin_local_transaction_id=b.creating_local_transaction_id=1;
      if(n==5&&variant==12)b.donor_mapping_uuid=security_fixture::Id(98099);
      if(n==5&&variant==14)b.deadlock_policy_uuid=security_fixture::Id(98099);
      if(n==5&&variant==21){b.profile_kind=IsolationProfileKind::read_committed_statement;
        b.snapshot_construction_point=IsolationSnapshotPoint::statement_start;b.snapshot_refresh_mask=2;
        b.metadata_visibility=IsolationMetadataVisibility::statement_epoch;}
      encoded=EncodeCatalogIsolationProfileInto(b,backing);m.object_subtype="isolation_profile";
    }
    Check(encoded.ok(),"actual transaction/isolation/lock definitions encode before native construction");
    m.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size());
    item.vector_metadata=m;item.vector_metadata.record.payload.clear();item.vector_metadata.record.header.kind=CatalogRecordKind::localized_name;
    item.vector_metadata.record.header.object_uuid=m.name_vector_uuid;
    item.vector_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,100001+3*n);
    item.vector_metadata.record.header.parent_uuid=m.record.header.object_uuid;item.vector_metadata.object_subtype="name_vector";
    item.entry_metadata=item.vector_metadata;item.entry_metadata.record.header.object_uuid=m.default_name_uuid;
    item.entry_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,100002+3*n);
    item.entry_metadata.record.header.parent_uuid=m.name_vector_uuid;item.entry_metadata.object_subtype="name_entry";item.entry_metadata.resource_epoch=1;
    item.vector.name_vector_uuid=item.entry.name_vector_uuid=m.name_vector_uuid;
    item.vector.object_uuid=item.entry.object_uuid=m.record.header.object_uuid;
    item.vector.default_name_entry_uuid=item.entry.name_entry_uuid=m.default_name_uuid;
    item.vector.object_class=item.entry.object_class="policy";
    item.entry.raw_name_text=item.entry.display_name="transaction_fixture_"+std::to_string(n);
    item.entry.normalized_lookup_key.assign(item.entry.raw_name_text.begin(),item.entry.raw_name_text.end());
    item.entries[0]={&item.entry_metadata,&item.entry};named.push_back({&m,&item.vector_metadata,&item.vector,item.entries});
  }
  auto result=AssembleLocalNamedCreationRecords(Id(1),security_tx,"en-US",named,{3*item_count,1024*1024});
  Check(result.ok(),"complete transaction policy closure assembles with actual names");
  const auto removed=variant==4?security_fixture::Id(98020):variant==10?security_fixture::Id(98000):platform::Uuid{};
  for(auto& row:result.records->definitions){const auto id=row.metadata.record.header.object_uuid.value;
    if((variant==5&&id==security_fixture::Id(98021))||(variant==11&&id==security_fixture::Id(98010)))row.metadata.status=CatalogObjectStatus::disabled_by_policy;
    if(id!=removed)seed[4].push_back(std::move(row));}
  for(auto& row:result.records->names)if(!row.name||!std::visit([&](const auto& name){return name.object_uuid.value==removed;},*row.name))seed[1].push_back(std::move(row));
  bool selected=false;
  for(auto& row:seed[4])if(row.metadata.object_subtype=="bootstrap_policy_profile"){
    auto profile=DecodeCatalogBootstrapPolicyProfile(row.metadata.record.payload);Check(profile.ok(),"actual bootstrap profile decodes");
    if(profile.record->area!="transaction_mga_cleanup_archive_backup_forward")continue;
    Check(!selected,"one actual transaction selection");selected=true;
    if(variant!=1){profile.record->resource_definition_uuid=security_fixture::Id(variant==13?98001:98000);
      profile.record->resource_definition_generation=variant==2?2:1;}
    const auto encoded=EncodeCatalogBootstrapPolicyProfileInto(*profile.record,backing);Check(encoded.ok(),"actual selected descriptor and generation encode");
    row.metadata.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size());
  }
  Check(selected,"explicit persisted transaction selection exists");
}


void AddNamingProfileFixture(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype,unsigned variant,unsigned copy=0) {
  using namespace catalog;
  CatalogIdentifierResolutionProfile identifier;
  identifier.profile_uuid=security_fixture::Id(57+100*copy);identifier.database_uuid=Id(1);identifier.generation=1;
  identifier.profile_name="explicit_internal_aliases";
  identifier.escape_rule=IdentifierEscapeRule::not_applicable;
  identifier.comparison_mode=IdentifierComparisonMode::exact;
  identifier.storage_display_mode=IdentifierDisplayMode::preserve_user_text;
  identifier.reserved_word_behavior=IdentifierReservedWordMode::parser_profile_specific;
  identifier.length_unit=IdentifierLengthUnit::encoded_octets;
  identifier.origin_transaction_uuid=security_tx;identifier.origin_local_transaction_id=1;
  if(variant==6)identifier.dialect_mode_flags=1;
  if(variant==16)identifier.resources=CatalogIdentifierResourceBinding{security_fixture::Id(111),security_fixture::Id(112),1,1};
  CatalogNamingDialectProfile dialect;
  dialect.profile_uuid=security_fixture::Id(56+100*copy);dialect.database_uuid=Id(1);dialect.identifier_profile_uuid=identifier.profile_uuid;
  dialect.generation=dialect.identifier_profile_generation=1;
  dialect.origin_transaction_uuid=dialect.creating_transaction_uuid=security_tx;
  dialect.origin_local_transaction_id=dialect.creating_local_transaction_id=1;
  if(variant==3)dialect.identifier_profile_uuid=security_fixture::Id(99981);
  if(variant==4)dialect.identifier_profile_uuid=enrollment::SourceId(0x402);
  if(variant==5)dialect.identifier_profile_generation=2;
  std::array<enrollment::NativeLocalServiceCreationItem,2> items{prototype,prototype};
  std::array<CatalogNamedCreationInput,2> named;
  std::array<platform::byte,1024> backing;
  for(unsigned n=0;n<items.size();++n){
    auto& x=items[n];auto& m=x.definition;
    m.record.header.kind=CatalogRecordKind::config_profile;
    m.record.header.object_uuid={platform::UuidKind::object,n?identifier.profile_uuid:dialect.profile_uuid};
    m.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,101000+100*copy+3*n);
    m.object_subtype=n?"identifier_resolution_profile":"naming_dialect_profile";
    const auto encoded=n?EncodeCatalogIdentifierProfileInto(identifier,backing):EncodeCatalogNamingDialectProfileInto(dialect,backing);
    Check(encoded.ok(),"explicit naming profile persists supplied values, no inferred label algorithm");
    m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    m.default_name_uuid=enrollment::Typed(platform::UuidKind::object,101010+100*copy+2*n);
    m.name_vector_uuid=enrollment::Typed(platform::UuidKind::object,101011+100*copy+2*n);
    x.vector_metadata=m;x.vector_metadata.record.payload.clear();x.vector_metadata.record.header.kind=CatalogRecordKind::localized_name;
    x.vector_metadata.record.header.object_uuid=m.name_vector_uuid;
    x.vector_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,101001+100*copy+3*n);
    x.vector_metadata.record.header.parent_uuid=m.record.header.object_uuid;x.vector_metadata.object_subtype="name_vector";
    x.entry_metadata=x.vector_metadata;x.entry_metadata.record.header.object_uuid=m.default_name_uuid;
    x.entry_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,101002+100*copy+3*n);
    x.entry_metadata.record.header.parent_uuid=m.name_vector_uuid;x.entry_metadata.object_subtype="name_entry";x.entry_metadata.resource_epoch=1;
    x.vector.name_vector_uuid=m.name_vector_uuid;x.vector.object_uuid=m.record.header.object_uuid;
    x.vector.object_class=CatalogRecordKindName(m.record.header.kind);x.vector.default_name_entry_uuid=m.default_name_uuid;
    x.entry.name_entry_uuid=m.default_name_uuid;x.entry.name_vector_uuid=m.name_vector_uuid;
    x.entry.object_uuid=m.record.header.object_uuid;x.entry.object_class=x.vector.object_class;
    x.entry.raw_name_text=x.entry.display_name="native_naming_profile_"+std::to_string(n+2*copy);
    x.entry.normalized_lookup_key.assign(x.entry.raw_name_text.begin(),x.entry.raw_name_text.end());
    x.entries[0]={&x.entry_metadata,&x.entry};named[n]={&m,&x.vector_metadata,&x.vector,x.entries};
  }
  auto assembled=AssembleLocalNamedCreationRecords(Id(1),security_tx,"en-US",named,{6,1024*1024});
  Check(assembled.ok(),"actual named identifier and dialect definitions assemble before physical creation");
  for(auto& row:assembled.records->definitions){
    if((variant==9&&row.metadata.object_subtype=="identifier_resolution_profile")||
        (variant==10&&row.metadata.object_subtype=="naming_dialect_profile"))row.metadata.status=CatalogObjectStatus::disabled_by_policy;
    seed[4].push_back(std::move(row));
  }
  for(auto& row:assembled.records->names)seed[1].push_back(std::move(row));
  if(variant==1||variant==2||variant==7||variant==8||(variant>=11&&variant<=15)){
    bool changed=false;
    for(auto& row:seed[1])if(row.name)if(auto* entry=std::get_if<CatalogNameEntry>(&*row.name);
        entry&&entry->object_uuid.value==enrollment::SourceId(1)){
      if(variant==1)entry->dialect_profile_uuid.value=security_fixture::Id(99980);
      if(variant==2)entry->dialect_profile_uuid.value=enrollment::SourceId(0x402);
      if(variant==7)entry->identifier_profile_uuid.value=enrollment::SourceId(0x402);
      if(variant==8)entry->case_fold_profile_uuid=enrollment::Typed(platform::UuidKind::object,99982);
      if(variant==11)entry->normalized_lookup_key[0]^=1;
      if(variant==12)entry->normalized_lookup_key.clear();
      if(variant==13)entry->display_name="forged_display";
      if(variant==14){entry->was_quoted=entry->requires_exact_match=true;entry->quote_style=CatalogNameQuoteStyle::double_quote;}
      if(variant==15)entry->exact_lookup_key={1,2,3};
      changed=true;break;
    }
    Check(changed,"naming reference fault changes an actual submitted entry");
  }
}

runtime_security::NativeCatalogCollisionError ExpectedCollisionError(unsigned variant) {
  using E=runtime_security::NativeCatalogCollisionError;
  constexpr std::array errors{E::none,E::missing_policy,E::invalid_policy,E::invalid_policy,
      E::missing_body,E::invalid_body,E::body_generation,E::unsupported_scope,E::missing_class,
      E::invalid_policy,E::invalid_body,E::duplicate_name,E::none,E::duplicate_name,E::none,
      E::duplicate_name,E::duplicate_name,E::none,E::none,E::none,E::none,E::conflicting_policy,E::invalid_body};
  Check(variant<errors.size(),"collision fixture variant has an explicit expected outcome");
  return errors[variant];
}

void AddCollisionPolicyFixture(db::NativeCreationCatalogSeed& seed,
    const enrollment::NativeLocalServiceCreationItem& prototype,unsigned variant,unsigned copy=0) {
  using namespace catalog;
  CatalogNamingCollisionPolicyBody body;
  body.body_uuid=security_fixture::Id(102000+1000*copy);body.database_uuid=Id(1);body.generation=1;
  body.origin_transaction_uuid=body.creating_transaction_uuid=security_tx;
  body.origin_local_transaction_id=body.creating_local_transaction_id=1;
  // These are explicit test inputs. No runtime default is inferred from them.
  body.group_count=variant==17?2:1;
  body.groups[0]={security_fixture::Id(102100+1000*copy),
      variant==13?CatalogCollisionLanguageLane::all_languages:CatalogCollisionLanguageLane::exact_tag,
      variant==15?CatalogCollisionKeyLane::shared:CatalogCollisionKeyLane::separate};
  if(variant==17)body.groups[1]={security_fixture::Id(102101),CatalogCollisionLanguageLane::exact_tag,CatalogCollisionKeyLane::separate};
  for(const auto& kind:BuiltinCatalogRecordDescriptorViews()){
    if(variant==8&&kind.kind==CatalogRecordKind::config_profile)continue;
    body.classes[body.class_count++]={kind.kind,body.groups[variant==17&&kind.kind==CatalogRecordKind::config_profile?1:0].group_uuid};
  }
  std::sort(body.classes.begin(),body.classes.begin()+body.class_count,
      [](const auto& a,const auto& b){return platform::u16(a.kind)<platform::u16(b.kind);});
  CatalogPolicyDescriptor policy;
  policy.policy_uuid=security_fixture::Id(55+100*copy);policy.policy_version_uuid=security_fixture::Id(102001+1000*copy);
  policy.database_uuid=Id(1);policy.generation=policy.body_generation=1;
  policy.body_uuid=variant==4?security_fixture::Id(99971):variant==5?security_fixture::Id(57):body.body_uuid;
  if(variant==6)policy.body_generation=2;
  policy.policy_class=variant==3?CatalogPolicyClass::authorization:CatalogPolicyClass::metadata;
  policy.scope=variant==7?CatalogPolicyScope::object:CatalogPolicyScope::database;
  policy.scope_uuid=Id(1);policy.authority_owner_uuid=prototype.definition.owner_uuid.value;
  policy.precedence_rank=1;policy.enforcement_points=platform::u64{1}<<13;
  policy.origin_transaction_uuid=policy.creating_transaction_uuid=security_tx;
  policy.origin_local_transaction_id=policy.creating_transaction_number=1;
  std::array<enrollment::NativeLocalServiceCreationItem,2> items{prototype,prototype};
  std::array<CatalogNamedCreationInput,2> named;
  std::array<platform::byte,kCatalogNamingCollisionPolicyMaximumBytes> backing;
  for(unsigned n=0;n<items.size();++n){
    auto& x=items[n];auto& m=x.definition;m.record.header.kind=CatalogRecordKind::policy;
    m.record.header.object_uuid={platform::UuidKind::object,n?body.body_uuid:policy.policy_uuid};
    m.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,102200+1000*copy+3*n);
    m.object_subtype=n?"naming_collision_policy_body":"policy_descriptor";
    const auto encoded=n?EncodeCatalogNamingCollisionPolicyInto(body,backing):EncodeCatalogPolicyDescriptorInto(policy,backing);
    Check(encoded.ok(),"actual collision descriptor and body encode explicit rules");
    m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    m.default_name_uuid=enrollment::Typed(platform::UuidKind::object,102210+1000*copy+2*n);
    m.name_vector_uuid=enrollment::Typed(platform::UuidKind::object,102211+1000*copy+2*n);
    x.vector_metadata=m;x.vector_metadata.record.payload.clear();x.vector_metadata.record.header.kind=CatalogRecordKind::localized_name;
    x.vector_metadata.record.header.object_uuid=m.name_vector_uuid;
    x.vector_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,102201+1000*copy+3*n);
    x.vector_metadata.record.header.parent_uuid=m.record.header.object_uuid;x.vector_metadata.object_subtype="name_vector";
    x.entry_metadata=x.vector_metadata;x.entry_metadata.record.header.object_uuid=m.default_name_uuid;
    x.entry_metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,102202+1000*copy+3*n);
    x.entry_metadata.record.header.parent_uuid=m.name_vector_uuid;x.entry_metadata.object_subtype="name_entry";x.entry_metadata.resource_epoch=1;
    x.vector.name_vector_uuid=x.entry.name_vector_uuid=m.name_vector_uuid;
    x.vector.object_uuid=x.entry.object_uuid=m.record.header.object_uuid;
    x.vector.default_name_entry_uuid=x.entry.name_entry_uuid=m.default_name_uuid;
    x.vector.object_class=x.entry.object_class="policy";
    x.entry.raw_name_text=x.entry.display_name="native_collision_policy_"+std::to_string(n+2*copy);
    x.entry.normalized_lookup_key.assign(x.entry.raw_name_text.begin(),x.entry.raw_name_text.end());
    x.entries[0]={&x.entry_metadata,&x.entry};named[n]={&m,&x.vector_metadata,&x.vector,x.entries};
  }
  auto assembled=AssembleLocalNamedCreationRecords(Id(1),security_tx,"en-US",named,{6,1024*1024});
  Check(assembled.ok(),"actual named collision definitions assemble before physical creation");
  for(auto& row:assembled.records->definitions){
    if((variant==9&&row.metadata.object_subtype=="policy_descriptor")||
        (variant==10&&row.metadata.object_subtype=="naming_collision_policy_body"))row.metadata.status=CatalogObjectStatus::disabled_by_policy;
    seed[variant==22&&row.metadata.object_subtype=="naming_collision_policy_body"?0:4].push_back(std::move(row));
  }
  for(auto& row:assembled.records->names)seed[1].push_back(std::move(row));
  if(variant==19)AddNamingProfileFixture(seed,prototype,0,1);
  if(variant==21)AddCollisionPolicyFixture(seed,prototype,0,1);
  if(variant==14||variant==15)for(auto& row:seed[4])if(row.metadata.record.header.object_uuid.value==security_fixture::Id(57)){
    auto decoded=DecodeCatalogIdentifierProfile(row.metadata.record.payload);Check(decoded.ok(),"actual profile selected for quote-lane case");
    decoded.record->quote_styles=1;decoded.record->escape_rule=IdentifierEscapeRule::doubled_delimiter;decoded.record->quoted_preserves_case=true;
    const auto encoded=EncodeCatalogIdentifierProfileInto(*decoded.record,backing);Check(encoded.ok(),"explicit quoted profile encodes before borrowed source retires");
    row.metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
  }
  std::string duplicate;
  for(const auto& row:seed[1])if(row.name)if(const auto* entry=std::get_if<CatalogNameEntry>(&*row.name);
      entry&&entry->object_uuid.value==enrollment::SourceId(1))duplicate=entry->raw_name_text;
  // Only the duplicate-alias variants consume the pack alias. Other cases run
  // this fixture too, including the name fault that deliberately repoints the
  // pack's entry, so requiring the alias there failed a case it does not test.
  const bool uses_duplicate=(variant>=11&&variant<=17)||variant==19||variant==20;
  if(uses_duplicate)Check(!duplicate.empty(),"collision case uses the actual persisted pack alias");
  const auto target=variant==16||variant==17?security_fixture::Id(12000):enrollment::SourceId(0x10);
  for(auto& row:seed[1])if(row.name){
    if(auto* vector=std::get_if<CatalogNameVector>(&*row.name);vector&&vector->object_uuid.value==target){
      if(variant==1||variant==2||variant==21)vector->name_collision_policy_uuid.value=security_fixture::Id(variant==1?99970:variant==2?57:155);
      if(variant==12||variant==13)vector->default_language_tag="fr-FR";
      if(variant==20){vector->owning_schema_uuid=enrollment::Typed(platform::UuidKind::schema,61);
        row.metadata.owning_schema_uuid=*vector->owning_schema_uuid;}
    }
    if(auto* entry=std::get_if<CatalogNameEntry>(&*row.name);entry&&entry->object_uuid.value==target){
      if((variant>=11&&variant<=17)||variant==19||variant==20){
        entry->raw_name_text=entry->display_name=duplicate;
        entry->normalized_lookup_key.assign(duplicate.begin(),duplicate.end());
      }
      if(variant==12||variant==13)entry->language_tag="fr-FR";
      if(variant==14||variant==15){entry->was_quoted=entry->requires_exact_match=true;
        entry->quote_style=CatalogNameQuoteStyle::double_quote;entry->exact_lookup_key=entry->normalized_lookup_key;}
      if(variant==19){entry->dialect_profile_uuid.value=security_fixture::Id(156);entry->identifier_profile_uuid.value=security_fixture::Id(157);}
      if(variant==20){entry->parent_schema_uuid=enrollment::Typed(platform::UuidKind::schema,61);
        entry->scope_uuid=enrollment::Typed(platform::UuidKind::object,61);row.metadata.owning_schema_uuid=*entry->parent_schema_uuid;}
    }
  }
  if(variant==20)for(auto& row:seed[4])if(row.metadata.record.header.object_uuid.value==target){
    row.metadata.record.header.parent_uuid=enrollment::Typed(platform::UuidKind::object,61);
    row.metadata.owning_schema_uuid=enrollment::Typed(platform::UuidKind::schema,61);
  }
  if(variant==18){
    const auto at=std::find_if(seed[1].begin(),seed[1].end(),[](const auto& row){return row.name&&
        std::holds_alternative<CatalogNameEntry>(*row.name)&&std::get<CatalogNameEntry>(*row.name).object_uuid.value==enrollment::SourceId(1);});
    Check(at!=seed[1].end(),"same-object alias derives from actual selected vector");auto alias=*at;
    auto& entry=std::get<CatalogNameEntry>(*alias.name);
    entry.name_entry_uuid=enrollment::Typed(platform::UuidKind::object,104000);entry.name_class=CatalogNameClass::alias;
    entry.default_for_object=entry.default_for_language=false;
    alias.metadata.record.header.object_uuid=alias.metadata.default_name_uuid=entry.name_entry_uuid;
    alias.metadata.record.header.row_uuid=enrollment::Typed(platform::UuidKind::row,104001);
    seed[1].push_back(std::move(alias));
  }
}

// SB_NATIVE_POLICY_FAMILY_SEEDS (owner direction 2026-11-04). The pack's native
// policy families, assembled by production code with the identities the
// builders above use, equal those builders' default records byte for byte,
// placed in the same roles, and select the same transaction policy. The
// builders stay the source of the fault variants; this ties the variants'
// starting point to the pack's values.
void PackPolicyFamiliesMatchFixture() {
  namespace sf=security_fixture;
  db::NativeInitialPolicyFamilyIdentities ids;
  for(unsigned n=0;n<5;++n)ids.verification_descriptors.push_back({sf::Id(92000+n),sf::Id(92020+n)});
  ids.verification_bodies={sf::Id(92010),sf::Id(92011),sf::Id(92012)};
  ids.evidence_bodies={sf::Id(92013)};ids.authorization_bodies={sf::Id(92014)};
  ids.transaction_descriptors={{sf::Id(98000),sf::Id(98030)},{sf::Id(98001),sf::Id(98031)}};
  ids.transaction_bodies={sf::Id(98010)};ids.lock_bodies={sf::Id(98011)};
  ids.isolation_profiles={sf::Id(98020),sf::Id(98021)};
  for(unsigned n=0;n<10;++n)ids.management_profiles.push_back(sf::Id(95000+n));
  ids.management_descriptors={sf::Id(95020)};
  ids.identifier_profiles={sf::Id(57)};ids.dialect_profiles={sf::Id(56)};
  ids.collision_descriptors={{sf::Id(55),sf::Id(102001)}};ids.collision_groups={sf::Id(102100)};
  ids.collision_bodies={sf::Id(102000)};
  ids.primary_filespace_uuid=Id(2);ids.security_schema_uuid=sf::Id(60);
  enrollment::NativeLocalServiceCreationFixture reference(Id(1),security_tx,verifier);
  auto expected=db::AssembleNativeInitialPolicySecurityCatalogs(reference.ServiceRequest(SB_DEFAULT_POLICY_PACK_ROOT));
  Check(expected.ok(),"pack enrollment without native policy families assembles");
  AddVerificationPolicies(expected.plan->catalogs,reference.items[0],0);
  AddManagementDefinitions(expected.plan->catalogs,reference.items[0],0);
  AddTransactionPolicies(expected.plan->catalogs,reference.items[0],0);
  AddNamingProfileFixture(expected.plan->catalogs,reference.items[0],0);
  AddCollisionPolicyFixture(expected.plan->catalogs,reference.items[0],0);
  // Descriptors 5+2+1, bodies 3+1+1+1+1, isolation 2, management 10+1,
  // identifier and dialect profiles, the collision body.
  constexpr std::size_t kFamilyObjects=5+3+1+1+2+1+1+2+10+1+1+1+1+1;
  enrollment::NativeLocalServiceCreationFixture inputs(Id(1),security_tx,verifier);
  inputs.AddPolicyFamilies(ids);
  auto request=inputs.ServiceRequest(SB_DEFAULT_POLICY_PACK_ROOT);request.policy_families=ids;
  const auto without=db::EnumerateNativeInitialPolicySecurityDefinitions(reference.ServiceRequest(SB_DEFAULT_POLICY_PACK_ROOT));
  const auto with=db::EnumerateNativeInitialPolicySecurityDefinitions(request);
  if(!with.ok())std::cerr<<"pack policy families: enumeration error="<<int(with.error)<<'\n';
  Check(without.ok()&&with.ok()&&with.definitions.size()==without.definitions.size()+kFamilyObjects,
      "the enumeration carries exactly one definition per family object");
  bool supplied_match=with.definitions.size()==inputs.inputs.size();
  for(const auto& input:inputs.inputs){
    const auto& header=input.definition->record.header;
    const auto found=std::find_if(with.definitions.begin(),with.definitions.end(),
        [&](const auto& d){return d.object_uuid==header.object_uuid.value;});
    if(found==with.definitions.end()||found->kind!=header.kind||found->object_subtype!=input.definition->object_subtype){
      std::cerr<<"supplied definition "<<scratchbird::core::uuid::UuidToString(header.object_uuid.value)<<" "
          <<input.definition->object_subtype<<" kind="<<int(header.kind)<<" enumerated="
          <<(found==with.definitions.end()?std::string("absent"):found->object_subtype+" kind="+std::to_string(int(found->kind)))<<'\n';
      supplied_match=false;
    }
  }
  Check(supplied_match,"the fixture supplies exactly the enumerated definitions, by identity, kind and subtype");
  const auto made=db::AssembleNativeInitialPolicySecurityCatalogs(request);
  if(!made.ok())std::cerr<<"pack policy families: catalog error="<<int(made.error)<<" source="<<int(made.source_error)<<'\n';
  Check(made.ok(),"production assembler admits the pack's native policy families");
  Check(made.plan->catalogs[1].size()==expected.plan->catalogs[1].size()&&
      made.plan->catalogs[1].size()==1462+2*kFamilyObjects,"one name vector and one default name per family object");
  for(const std::size_t role:{3u,4u,5u}){
    std::map<platform::Uuid,std::string> want,have;
    for(const auto& row:expected.plan->catalogs[role])
      want.emplace(row.metadata.record.header.object_uuid.value,row.metadata.record.payload);
    for(const auto& row:made.plan->catalogs[role])
      have.emplace(row.metadata.record.header.object_uuid.value,row.metadata.record.payload);
    Check(want.size()==expected.plan->catalogs[role].size()&&have.size()==made.plan->catalogs[role].size(),
        "family comparison identities are unique per role");
    if(want!=have)for(const auto& [id,payload]:want){
      const auto found=have.find(id);
      if(found==have.end())std::cerr<<"role "<<role<<" missing "<<scratchbird::core::uuid::UuidToString(id)<<'\n';
      else if(found->second!=payload)std::cerr<<"role "<<role<<" payload differs "<<scratchbird::core::uuid::UuidToString(id)<<'\n';
    }
    Check(want==have,"pack-driven native policy family records equal the fixture's byte for byte, in the same role");
  }
  Check(made.plan->catalogs[5].size()==11&&made.plan->catalogs[0].empty()&&made.plan->catalogs[2].empty(),
      "management definitions take role6; roles1/3 untouched");
  // The production path refuses a family that cannot be placed honestly.
  auto unresolved=ids;unresolved.security_schema_uuid={};
  request.policy_families=unresolved;
  Check(db::AssembleNativeInitialPolicySecurityCatalogs(request).source_error==db::NativeInitialPolicyError::invalid_request,
      "a family without its sys.security schema identity refuses");
  auto reused=ids;reused.collision_groups[0]=sf::Id(92000);
  request.policy_families=reused;
  Check(db::AssembleNativeInitialPolicySecurityCatalogs(request).source_error==db::NativeInitialPolicyError::duplicate_identity,
      "a family identity reused within the families refuses");
  auto aliased=ids;aliased.verification_descriptors[0].version_uuid=enrollment::SourceId(1);
  request.policy_families=aliased;
  Check(db::AssembleNativeInitialPolicySecurityCatalogs(request).source_error==db::NativeInitialPolicyError::duplicate_identity,
      "a descriptor version identity aliasing a pack definition refuses");
  auto short_ids=ids;short_ids.lock_bodies.clear();
  request.policy_families=short_ids;
  Check(db::AssembleNativeInitialPolicySecurityCatalogs(request).source_error==db::NativeInitialPolicyError::invalid_request,
      "a family identity set that does not match the pack's shape refuses");
  // Issued identities have exactly the pack's shape.
  unsigned issued=0;
  const auto fresh=db::IssueNativeInitialPolicyFamilyIdentities(SB_DEFAULT_POLICY_PACK_ROOT,
      [&]()->std::optional<platform::Uuid>{return sf::Id(110000+issued++);});
  Check(fresh.ok()&&issued==2*(5+2+1)+3+1+1+1+1+2+10+1+1+1+1+1&&fresh.identities->collision_bodies.size()==1&&
      fresh.identities->primary_filespace_uuid.is_nil()&&fresh.identities->security_schema_uuid.is_nil(),
      "issued family identities follow the pack's shape and leave the references to the caller");
}

void EnrolledNativeSource(unsigned profile,bool indexed,server::ServerProcessCryptoOwner& process,
    unsigned configuration_variant=0,unsigned graph_variant=0,unsigned death=0,unsigned agent_variant=0,
    unsigned memory_variant=0,unsigned policy_variant=0,unsigned authentication_variant=0,unsigned name_variant=0,
    unsigned namespace_variant=0,unsigned filespace_variant=0,unsigned audit_variant=0,unsigned verification_policy_variant=0,
    unsigned management_variant=0,unsigned transaction_policy_variant=0,unsigned reference_variant=0,unsigned naming_profile_variant=0,
    unsigned collision_variant=0,unsigned index_variant=0) {
  scratchbird::tests::OwnedTempDirectory artifacts;
  enrollment::NativeLocalServiceCreationFixture inputs(Id(1),security_tx,verifier);
  auto catalogs=db::AssembleNativeInitialPolicySecurityCatalogs(inputs.ServiceRequest(SB_DEFAULT_POLICY_PACK_ROOT));
  Check(catalogs.ok(),"production assembler admits complete native service policy/security/name enrollment");
  // +2 definitions, +4 names: the pack's CONNECT grant to PUBLIC and
  // OBS_AGENT_CONTROL_INTERNAL for transaction_pressure (2026-10-30): 308
  // definitions, 616 names, 279 security rows. 2026-11-03
  // (SB_NATIVE_CREATE_AGENT_PRINCIPALS): + L5's SYSARCH membership (D5) and per
  // pack agent type a principal, its grants and its membership (160+102+160),
  // all security rows: 731 definitions, 1462 names, 702 security rows.
  Check(catalogs.plan->catalogs[1].size()==1462&&catalogs.plan->catalogs[3].size()==702&&
      catalogs.plan->catalogs[4].size()==29,"complete 731 definitions and 1462 names, including bootstrap, service and agents");
  if(reference_variant){
    auto& definitions=catalogs.plan->catalogs[4];
    const auto target=std::find_if(definitions.begin(),definitions.end(),[](const auto& row){
      return row.metadata.record.header.object_uuid.value==enrollment::SourceId(1);});
    Check(target!=definitions.end(),"actual pack definition for independent security-reference corruption");
    if(reference_variant<=2)target->metadata.owner_uuid.value=reference_variant==1?
        security_fixture::Id(99990):enrollment::SourceId(0x402);
    else target->metadata.security_policy_uuid.value=reference_variant==3?security_fixture::Id(99991):
        reference_variant==4?security_fixture::Id(14000):enrollment::SourceId(0x400);
  }
  const auto agent_error=AddAgentConfigurationFixture(catalogs.plan->catalogs,inputs.items[0],agent_variant,policy_variant);
  ChangeMemoryEnvelopeFixture(catalogs.plan->catalogs,memory_variant);
  const auto configuration_error=ChangeConfigurationFixture(catalogs.plan->catalogs,configuration_variant);
  if(authentication_variant==1){
    auto& rows=catalogs.plan->catalogs[4];
    const auto provider=std::find_if(rows.begin(),rows.end(),[](const auto& row){
      return row.metadata.record.header.object_uuid.value==enrollment::SourceId(0x10);});
    Check(provider!=rows.end(),"actual selected service provider for disabled-provider fixture");
    auto decoded=catalog::DecodeCatalogBootstrapSecurityProvider(provider->metadata.record.payload);
    Check(decoded.ok(),"actual native provider decodes before explicit disable");
    decoded.record->enabled_by_default=false;
    provider->metadata.record.payload=security_fixture::EncodeGolden(*decoded.record);
  }
  if(name_variant&&name_variant<=4){
    // Individually well-formed native name rows with deliberately broken
    // cross-object links. Storage must not invent semantic reference closure.
    bool changed=false;
    for(auto& row:catalogs.plan->catalogs[1])if(row.name){
      if(name_variant==2){
        if(auto* vector=std::get_if<catalog::CatalogNameVector>(&*row.name);
            vector&&vector->object_uuid.value==enrollment::SourceId(1)){
          vector->default_language_tag="de-DE";changed=true;break;
        }
      }else if(auto* entry=std::get_if<catalog::CatalogNameEntry>(&*row.name);
          entry&&entry->object_uuid.value==enrollment::SourceId(1)){
        if(name_variant==1)entry->object_uuid.value=security_fixture::Id(91000);
        if(name_variant==3)entry->object_class="schema";
        if(name_variant==4)entry->default_for_object=false;
        changed=true;break;
      }
    }
    Check(changed,"semantic name fault targets an actual submitted catalog row");
  }
  db::NativeCreationCatalogIndexes indexes;
  if(indexed)indexes=AddCatalogIndexRowsFixture(catalogs.plan->catalogs,inputs.items[0]);
  // Individually valid rows and bindings whose cross-references only the
  // open-time check can refuse: storage admits structure, not these references.
  if(index_variant){
    Check(indexed,"catalog index refusals need indexed relations");
    auto& rows=catalogs.plan->catalogs[4];auto& names=catalogs.plan->catalogs[1];
    const auto drop=[&](const platform::Uuid& id){
      const auto before=rows.size();
      std::erase_if(rows,[&](const auto& row){return row.metadata.record.header.object_uuid.value==id;});
      std::erase_if(names,[&](const auto& row){
        return row.name&&std::visit([&](const auto& name){return name.object_uuid.value==id;},*row.name);});
      Check(rows.size()+1==before,"refusal fixture drops one actual catalog index row and its names");
    };
    auto& names_relation=*indexes[1];auto& security=*indexes[3];
    if(index_variant==1)security.dependencies.dependency_map_sha256[0]^=1;
    if(index_variant==2)drop(security.dependencies.dependency_map_uuid);
    if(index_variant==3){
      // A different storage generation with a matching digest: only the
      // field comparison can refuse it.
      const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& r){
        return r.metadata.record.header.object_uuid.value==security.dependencies.dependency_map_uuid;});
      Check(row!=rows.end(),"actual security relation dependency map");
      auto map=catalog::DecodeCatalogNativeCatalogIndexDependencyMap(row->metadata.record.payload);
      Check(map.ok(),"actual dependency map decodes");map.record->storage_generation=2;
      const auto encoded=catalog::EncodeCatalogNativeCatalogIndexDependencyMap(*map.record);
      Check(encoded.ok(),"valid map with a different storage generation");
      row->metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
      const auto digest=scratchbird::core::hash::ComputeSha256Digest(encoded.bytes);
      Check(digest.ok(),"replacement map digest");security.dependencies.dependency_map_sha256=digest.digest;
    }
    if(index_variant==4)drop(catalog::kNativeCatalogIndexKeyProfileUuid);
    if(index_variant==5)names_relation.relation_uuid=security_fixture::Id(8001);
    if(index_variant==6)drop(catalog::kNativeCatalogIndexVisibilityProfileUuid);
  }
  db::NativeFilespaceInitializationRequest construction;
  construction.bootstrap={Id(1),Id(2),Profile(profile),disk::kNativeBootstrapIntegrityProfile,{},sizes[profile],1,0,1,7};
  construction.operation_uuid=Id(3);construction.writer_uuid=Id(4);construction.policy_snapshot_uuid=Id(10);
  construction.creator.transaction_uuid=security_tx;construction.creator.local_id=mga::MakeLocalTransactionId(1);
  // Ordinary fixtures: 512 pages (256 before the agent task-group roles
  // enlarged the catalog, 2026-10-29). Since every agent type's principal
  // (2026-11-03) the catalog alone is about 469 pages at 8 KiB, leaving no free
  // page to publish TX2, so 8 KiB gets 1024; at 16 KiB it is about 240 pages
  // and larger profiles keep 512 (their selections hold whole page images).
  construction.creator.scope=mga::TransactionScope::local_node;
  construction.total_pages=sizes[profile]==8192?1024:512;
  // This case retains the additional verification phase history. Provision its
  // explicit physical capacity up front. No allocator may reuse immutable
  // history or bypass a full filespace.
  if(profile==0&&indexed&&!configuration_variant&&!graph_variant&&!death&&!agent_variant&&!memory_variant&&
      !policy_variant&&!authentication_variant&&!name_variant&&!namespace_variant&&!filespace_variant&&!audit_variant&&
      !verification_policy_variant&&!management_variant&&!transaction_policy_variant&&!reference_variant&&!naming_profile_variant&&!collision_variant&&!index_variant)construction.total_pages=2048;
  construction.creation_utc_millis=1789357072000ULL;
  AddDatabaseDefinitionFixture(catalogs.plan->catalogs,inputs.items[0],construction,name_variant,namespace_variant,filespace_variant);
  AddVerificationPolicies(catalogs.plan->catalogs,inputs.items[0],verification_policy_variant);
  AddManagementDefinitions(catalogs.plan->catalogs,inputs.items[0],management_variant);
  AddTransactionPolicies(catalogs.plan->catalogs,inputs.items[0],transaction_policy_variant);
  AddNamingProfileFixture(catalogs.plan->catalogs,inputs.items[0],naming_profile_variant);
  AddCollisionPolicyFixture(catalogs.plan->catalogs,inputs.items[0],collision_variant);
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),Id(10)},{{},0,1000});
  const auto audit=db::AcquireNativeConstructionIntent(construction,Id(4),issuer);
  Check(audit.ok()&&!audit.event->actor_uuid,"actual construction owner obtains pre-policy event identity and admitted time");
  auto audit_metadata=inputs.items[0].definition;
  audit_metadata.record.header.kind=catalog::CatalogRecordKind::audit_evidence;
  audit_metadata.record.header.object_uuid={platform::UuidKind::object,audit.event->event_uuid};
  audit_metadata.record.header.row_uuid=audit.row_uuid;
  audit_metadata.record.header.parent_uuid={platform::UuidKind::object,security_fixture::Id(62)};
  audit_metadata.owning_schema_uuid={platform::UuidKind::schema,security_fixture::Id(62)};
  audit_metadata.object_subtype="node_construction_intent";
  audit_metadata.default_name_uuid={};audit_metadata.name_vector_uuid={};
  audit_metadata.audit_uuid=audit_metadata.record.header.object_uuid;
  std::array<byte,catalog::kCatalogConstructionIntentMaxBytes> audit_bytes;
  const auto encoded_audit=catalog::EncodeCatalogConstructionIntentInto(*audit.event,audit_bytes);
  Check(encoded_audit.ok(),"actual acquired construction event encodes without an invented policy decision");
  audit_metadata.record.payload.assign(reinterpret_cast<const char*>(encoded_audit.bytes.data()),encoded_audit.bytes.size());
  for(auto& relation:catalogs.plan->catalogs)for(auto& row:relation)row.metadata.audit_uuid=audit_metadata.audit_uuid;
  if(audit_variant!=4)catalogs.plan->catalogs[5].push_back({audit_metadata,{}});
  if(audit_variant&&audit_variant!=4){
    auto& reference=catalogs.plan->catalogs[0].front().metadata.audit_uuid;
    if(audit_variant==1)reference.value=security_fixture::Id(91050);
    if(audit_variant==2)reference.value=security_fixture::Id(60);
    if(audit_variant==3)reference.value=audit.row_uuid.value;
  }
  if(name_variant==6){
    auto& ordinary=catalogs.plan->catalogs[0].front().metadata;
    ordinary.default_name_uuid={};ordinary.name_vector_uuid={};
  }
  const auto path=(artifacts.path()/"enrolled-native-node").string();disk::FileDevice created;
  Check(created.Open(path,disk::FileOpenMode::create_new).ok(),"real native enrolled node construction");
  // The host's own source backing (512 pages, at least 8 MiB since 2026-11-03).
  const u64 budget=db::NativeHostedSourceBackingBytes(sizes[profile]);
  const auto construction_writes=stage_writes,construction_syncs=stage_syncs;
  auto initialized=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(created,construction,
      catalogs.plan->catalogs,indexes,budget,issuer);
  if(!indexed){
    // The complete name/configuration cohort exceeds a single native leaf.
    // Preserve its bounded refusal; production fixtures must provision indexes,
    // rather than truncate enrollment or increase the selected page profile.
    const auto size=created.Size();
    Check(initialized.error==db::NativeCreationWorkspaceError::catalog_capacity_exceeded&&!initialized.receipt&&
        size.ok()&&!size.size_bytes&&stage_writes==construction_writes&&stage_syncs==construction_syncs,
        "complete unindexed enrollment refuses before physical writes instead of truncating the cohort");
    Check(created.Close().ok(),"refused unindexed construction retains a closable empty owned file");
    return;
  }
  if(!initialized.ok())std::cerr<<"enrolled construction error="<<int(initialized.error)<<" profile="<<profile<<" indexed="<<indexed
      <<" checkpoint="<<int(initialized.checkpoint_error)<<" selection="<<int(initialized.selection_error)
      <<" catalog="<<(initialized.catalog_read_error?int(*initialized.catalog_read_error):-1)
      <<" relation="<<(initialized.catalog_relation_error?int(*initialized.catalog_relation_error):-1)<<'\n';
  Check(initialized.ok(),"actual complete native enrollment persists in selected checkpoint/inventory");
  Check(created.Close().ok(),"construction descriptor retires before host custody");
  struct stat native_identity{};
  Check(::stat(path.c_str(),&native_identity)==0,"actual native probe inode selected outside any storage guard");
  native_probe_device=native_identity.st_dev;native_probe_inode=native_identity.st_ino;
  // Deliberately discard creation receipts. Cold startup knows the node locator
  // and explicit logical binding, and must discover all physical catalog layout.
  std::array<std::size_t,6> expected_catalog_rows{};
  std::map<platform::Uuid,std::string> expected_catalog_bytes;
  std::size_t expected_vectors=0,expected_entries=0,expected_definitions=0;
  for(std::size_t i=0;i<expected_catalog_rows.size();++i)expected_catalog_rows[i]=catalogs.plan->catalogs[i].size();
  for(const auto& relation:catalogs.plan->catalogs)for(const auto& row:relation){
    std::optional<catalog::CatalogNamePayloadView> name;
    if(row.name){name=catalog::BorrowCatalogNamePayload(*row.name);
      if(std::holds_alternative<catalog::CatalogNameVector>(*row.name))++expected_vectors;else ++expected_entries;
    }else ++expected_definitions;
    expected_catalog_bytes.emplace(row.metadata.record.header.object_uuid.value,
        CatalogVariableBytes(catalog::BorrowCatalogMetadataVersion(row.metadata),name));
  }
  // Maps, checkpoint, directory, and the in-place transaction inventory: its
  // state page and its header page (SB_TRANSACTION_HEADER_PAGE).
  const auto expected_initial_control_pages=initialized.receipt->map_pages+4;
  initialized.receipt.reset();catalogs.plan.reset();
  EnrolledCredentialCustodian provisioner;
  platform::Uuid previous_engine_instance;
  for(unsigned reopen=0;reopen<(configuration_variant||graph_variant||agent_variant||policy_variant||name_variant||namespace_variant||filespace_variant||reference_variant||naming_profile_variant||collision_variant||index_variant?1u:2u);++reopen){
    // Credential-only bounds, the host's own (engine_host.cpp): room for the
    // 702-row, ~710 KiB security relation of a complete database (277 rows
    // before agent principals, 2026-11-03). Base pages are bounded in bytes so
    // small pages get enough leaves.
    const std::size_t credential_base_pages=db::NativeHostedCatalogBasePages(sizes[profile]);
    // The host's own bound (SB_NATIVE_HOSTED_CATALOG_ROW_BOUND), not a copy of it.
    constexpr auto bound=db::kNativeHostedCatalogRelationMaximumRows;
    const db::NativeCatalogRelationMemoryLimits limits{Profile(profile),1,credential_base_pages,bound,bound,
        db::NativeHostedCatalogNavigationLimits(sizes[profile])};
    // Variant 1 stays exactly one record short so the bound still refuses.
    // History: 277 records with the agent task-group roles (2026-10-29), bound
    // 276. kEnrolledAuthorizationRecords adds L5's SYSARCH membership and every
    // agent type's principal, grants and membership (2026-11-03).
    const runtime_security::NativeRuntimeAuthorizationLimits graph_limits{
        graph_variant==1?kEnrolledAuthorizationRecords-1:bound,graph_variant==1?kEnrolledAuthorizationRecords-1:128u,
        graph_variant==2?1u:1000000u};
    const auto manager_baseline=mem::DefaultMemoryManager().Snapshot();
    server::DatabaseOwnershipRequest ownership;ownership.database_path=path;ownership.storage_access=disk::RouteStorageAccess::read_write;
    using Host=server::NativeHostedDatabaseRuntime;
    std::vector<std::unique_ptr<disk::FileDevice>> secondary;
    const server::NativeHostedRuntimeLimits host_limits{{64*1024,{budget,32*1024*1024}},limits,2*1024*1024,graph_limits};
    if(profile==0&&reopen==0&&!configuration_variant&&!graph_variant&&!agent_variant) {
      auto read_only=ownership;read_only.storage_access=disk::RouteStorageAccess::read_only;
      const auto invalid=Host::Open(process,read_only,host_limits,secondary);
      Check(invalid.error==server::NativeHostedRuntimeError::invalid_request&&!invalid.runtime,
          "native writer mount never upgrades a read-only initial capability");
      auto missing_graph=host_limits;missing_graph.authorization={};
      const auto missing=Host::Open(process,ownership,missing_graph,secondary);
      Check(missing.error==server::NativeHostedRuntimeError::invalid_request&&!missing.runtime,
          "native host requires explicit graph bounds before OS or process memory custody");
      const auto before_refusal=mem::DefaultMemoryManager().Snapshot();
      auto insufficient=host_limits;insufficient.database_crypto_bytes=1;
      auto refused=Host::Open(process,ownership,insufficient,secondary);
      Check(!refused.ok()&&refused.error==server::NativeHostedRuntimeError::crypto_failure&&
          !refused.crypto.ok()&&refused.runtime,
          "actual undersized native provider backing refuses before writer adoption and retains its original owner");
      Check(refused.runtime->engine_instance_uuid().is_nil(),"failed native open exposes no live engine instance");
      Check(Host::Close(refused.runtime).ok(),"refused native admission explicitly retires its own provisional resources and OS owner");
      refused.runtime.reset();
      const auto after_refusal=mem::DefaultMemoryManager().Snapshot();
      Check(after_refusal.current_bytes==before_refusal.current_bytes&&
          after_refusal.reserved_capacity_bytes==before_refusal.reserved_capacity_bytes&&
          after_refusal.active_capacity_reservation_count==before_refusal.active_capacity_reservation_count,
          "native admission refusal restores every actual shared-manager charge before corrected retry");
    }
    auto host=Host::Open(process,ownership,host_limits,secondary);
    std::unique_ptr<mem::ReservationBackedMemoryResource> held_memory;
    if(!host.ok())std::cerr<<"native host open error="<<int(host.error)<<" memory="<<int(host.memory.error)
        <<" pool="<<int(host.crypto.error)<<" adoption="<<int(host.adoption.source.error)<<'\n';
    Check(host.ok()&&host.runtime->database_uuid()==Id(1)&&host.runtime->primary_filespace_uuid()==Id(2),
        "production native owner probes retained inode and adopts the complete cohort with actual process-funded grants");
    const auto engine_instance=host.runtime->engine_instance_uuid();
    Check(scratchbird::core::uuid::IsEngineIdentityUuid(engine_instance)&&engine_instance!=Id(1)&&
        engine_instance!=previous_engine_instance,"a successful native owner has its own instance identity and cold reopen cannot reuse it");
    previous_engine_instance=engine_instance;
    const auto concurrent=Host::Open(process,ownership,host_limits,secondary);
    Check(concurrent.error==server::NativeHostedRuntimeError::process_admission_failure&&
        concurrent.process_admission_error==server::HostedProcessAdmissionError::opening&&!concurrent.runtime,
        "actual process gate rejects a second native mount before filesystem or memory admission");
    server::NativeLocalServiceStartupCredentialsRequest request;
    request.credential.security.database_uuid=Id(1);request.credential.security.runtime_binding_uuid=security_fixture::Id(15001);
    request.credential.security.runtime_binding_generation=1;request.credential.security.maximum_retained_image_bytes=budget;
    request.credential.security.catalog_selection=runtime_security::NativeRuntimeCatalogSelection::discover_selected;
    request.source={Id(231),Id(235),7,server::StartupCredentialSource::Mutex::Clock::now()+std::chrono::seconds(10),{}};
    request.checkpoint_limits={budget,32*1024*1024};
    const auto writes=stage_writes,syncs=stage_syncs;
    if(profile==0&&reopen==0&&!configuration_variant&&!graph_variant&&!agent_variant) {
      server::NativeHostedRuntimeCloseResult foreign;
      platform::Uuid foreign_instance;
      std::thread other([&]{foreign_instance=host.runtime->engine_instance_uuid();foreign=Host::Close(host.runtime);});other.join();
      Check(foreign_instance.is_nil()&&foreign.error==server::NativeHostedRuntimeError::wrong_owner&&!foreign.original_route_released,
          "foreign native lifecycle thread refuses before withdrawing the real writer or closing provider backing");
      const auto child=::fork();Check(child>=0,"fork native runtime ownership refusal fixture");
      if(child==0)::_exit(Host::Close(host.runtime).error==server::NativeHostedRuntimeError::wrong_owner?0:1);
      int status=0;pid_t waited;do{waited=::waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
      Check(waited==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
          "inherited native runtime refuses before inherited control or provider synchronization");
      auto stale=request;++stale.source.generation;
      auto refused=Host::ReadStartupCredentials(host.runtime,stale,provisioner.source,provisioner.tag,provisioner.derived);
      Check(refused.error==server::NativeHostedRuntimeError::credential_failure&&
          refused.credentials().credential_source.error==server::StartupCredentialSource::Error::stale,
          "production native credential read preserves exact protected-source generation refusal");
      refused.Reset();
      std::stop_source stop;stop.request_stop();auto cancelled=request;cancelled.credential.stop=stop.get_token();
      auto cancellation=Host::ReadStartupCredentials(host.runtime,cancelled,provisioner.source,provisioner.tag,provisioner.derived);
      Check(cancellation.error==server::NativeHostedRuntimeError::credential_failure&&
          cancellation.credentials().error==server::NativeLocalServiceStartupCredentialsError::cancelled,
          "production native credential read preserves cancellation without publication or an invented session");
      cancellation.Reset();
      Check(stage_writes==writes&&stage_syncs==syncs,"native owner, source-generation and cancellation refusals have no physical mutation effects");
    }
    deny_heap_on_native_file=true;
    guarded_release_calls=0;
    observed_mutexes={};last_observed_mutex=nullptr;mutex_observation_overflow=false;observe_mutexes=true;
    auto measured_request=request;
    std::stop_source authentication_stop;
    std::stop_source session_stop;
    {std::stop_source temporary;measured_request.credential.stop=temporary.get_token();measured_request.source.stop=temporary.get_token();}
    measured_request.source.stop=session_stop.get_token();
    if(authentication_variant>=2)measured_request.credential.stop=authentication_stop.get_token();
    // Transfer the last stop-state owners as well as non-SSO tag labels. Their
    // cleanup must wait for the actual selected device guards to retire.
    auto read=Host::ReadStartupCredentials(host.runtime,std::move(measured_request),provisioner.source,provisioner.tag,provisioner.derived);
    observe_guarded_releases=false;
    const auto guarded_budget=allocation_budget;allocation_budget=-1;
    Check(!deny_heap_on_native_file&&!deny_heap_on_next_read&&guarded_budget==0,
        "complete native selected read, credential comparison and host-owned graph use no ordinary heap after first guarded I/O");
    Check(!guarded_release_calls,"input labels and last stop-state owners never deallocate while native selected guards remain held");
    const auto& admitted=read.credentials();
    if(configuration_variant||graph_variant||agent_variant){
      observe_mutexes=false;
      Check(admitted.ok()&&Zero(provisioner.derived)&&!read.ok(),
          "real credential comparison cannot hide a configuration or graph admission refusal");
      Check(Host::RetainStartupObservation(read)==read.error&&
          admitted.comparison->lease->has_selected_source()&&bool(admitted.operation),
          "failed configuration or graph cannot retire guards or transfer the original reservation");
      if(configuration_variant)Check(read.error==server::NativeHostedRuntimeError::configuration_failure&&
          read.configuration().error==configuration_error&&!read.configuration().configuration&&
          !read.configuration().failing_object_uuid.is_nil(),
          "actual retained policy profiles and exact memory body refuse with complete typed binary cause");
      else if(agent_variant)Check(read.error==server::NativeHostedRuntimeError::agent_configuration_failure&&
          read.agents().error==agent_error&&read.agents().entries().empty()&&!read.agents().definition_count&&
          !read.authorization().ok(),
          "invalid actual agent cohort refuses atomically before grant projection or startup handoff");
      else Check(read.error==server::NativeHostedRuntimeError::authorization_failure&&
          read.authorization().error==(graph_variant==1?runtime_security::NativeRuntimeAuthorizationError::resource_exhausted:
              runtime_security::NativeRuntimeAuthorizationError::work_limit)&&!read.authorization().projection.present(),
          "host-owned graph honors exact prepared record/work bounds without a usable prefix");
      const auto& failure=read.configuration();using F=server::NativeStartupConfigurationBindingField;
      if(configuration_variant==7)Check(failure.failing_binding==F::definition_generation&&
          failure.expected_generation==2&&failure.observed_generation==1,
          "memory body semantic generation refusal preserves expected and actual generations");
      if(configuration_variant==8||configuration_variant==10)Check(failure.failing_binding==F::pack_generation&&
          failure.expected_generation==1&&failure.observed_generation==2,
          "profile and body pack-generation refusals preserve exact independent values");
      if(configuration_variant==9)Check(failure.failing_binding==F::profile_identity&&
          failure.expected_identity==enrollment::SourceId(0x403)&&failure.observed_identity==Id(249),
          "memory body profile refusal preserves both binary identities");
      Check(stage_writes==writes&&stage_syncs==syncs&&
          Host::Close(host.runtime).custody.writer.error==db::NativeWriterError::busy,
          "refusal retains its original credential/catalog operation and no physical activation effects");
      read.Reset();
      Check(!read.configuration().ok()&&!read.authorization().ok()&&!read.agents().ok(),"reset invalidates all borrowed policy/grant access before releasing backing");
      Check(Host::Close(host.runtime).ok(),"explicit refusal retirement releases full native host custody");
      host.runtime.reset();const auto restored=mem::DefaultMemoryManager().Snapshot();
      Check(restored.current_bytes==manager_baseline.current_bytes&&restored.reserved_capacity_bytes==manager_baseline.reserved_capacity_bytes&&
          restored.active_capacity_reservation_count==manager_baseline.active_capacity_reservation_count,
          "configuration and graph refusal retire every actual host memory charge");
      continue;
    }
    if(!admitted.ok())std::cerr<<"enrolled comparison error="<<int(admitted.error)<<" profile="<<profile<<" indexed="<<indexed
        <<" security="<<(admitted.comparison?int(admitted.comparison->security_error):-1)<<" relation="
        <<(admitted.comparison?int(admitted.comparison->relation_error):-1)<<'\n';
    if(!read.ok())std::cerr<<"native startup prerequisite error="<<int(read.error)<<" agent="<<int(read.agents().error)<<" variant="<<agent_variant<<'\n';
    Check(read.ok()&&admitted.ok()&&Zero(provisioner.derived),"actual enrolled service verifies through selected-root discovery and protected source");
    Check(read.agents().ok()&&read.agents().definition_count==66&&read.agents().registration_count==32&&
        read.agents().policy_count==34,"cold startup selects the entire persisted agent configuration");
    unsigned critical=0;for(const auto& entry:read.agents().entries()){
      Check(entry.definition.database_uuid==Id(1)&&entry.definition.origin_transaction_uuid.value==security_tx.value&&
          entry.definition.origin_local_transaction_id==1&&entry.definition.generation==1&&!entry.version_uuid.is_nil(),
          "each agent definition retains actual database, creation and version provenance");
      if(entry.definition.kind!=catalog::AgentBootstrapDefinitionKind::registration)continue;
      if(entry.definition.agent_type=="agent_coordinator")critical|=1;
      if(entry.definition.agent_type=="startup_agent")critical|=2;
      if(entry.definition.agent_type=="shutdown_agent")critical|=4;
      if(entry.definition.agent_type=="memory_governor")critical|=8;
      if(entry.definition.agent_type=="filespace_capacity_manager")critical|=16;
      if(entry.definition.agent_type=="page_allocation_manager")critical|=32;
    }
    Check(critical==(policy_variant==16?55:63),"coordinator lifecycle and memory/file/page managers survive real cold native reopen");
    const auto agent_payload_fingerprint=AgentPayloadFingerprint(read.agents());
    const auto& configuration=*read.configuration().configuration;
    const auto& actual_catalog=admitted.comparison->lease->selected_catalog();
    Check(configuration.memory.configuration_uuid==security_fixture::Id(12000)&&
        configuration.memory.profile_uuid==enrollment::SourceId(0x403)&&configuration.memory.generation==1&&
        configuration.memory.hard_limit_bytes==(memory_variant==8?2147483648ULL:
            memory_variant?(memory_variant==2?64*1024:memory_variant==12?96*1024*1024:64*1024*1024):1073741824)&&
        configuration.memory.openssl_budget_bytes==(memory_variant==2?16*1024:memory_variant==7?1024*1024:
            (memory_variant==9||memory_variant==12)?32*1024*1024:4194304)&&
        configuration.provider_selection.mode=="local_password_only"&&actual_catalog.catalog_generation==1&&
        actual_catalog.security_epoch==1&&!actual_catalog.resource_epoch,
        "production host retains exact submitted memory body, provider selection and independent actual catalog epochs");
    allocation_budget=0;
    using AuthenticationError=runtime_security::NativeLocalServiceAuthenticationError;
    using SessionError=runtime_security::NativeLocalServiceSessionError;
    Check(!read.session()&&Host::AttachStartupServiceSession(read).error==SessionError::not_ready,
        "selected source and credential comparison alone cannot attach a native service session");
    Check(Host::AuthenticateStartupService(read,provisioner.derived).error==AuthenticationError::selected_guards_held&&
        !read.authentication(),"service authentication cannot invoke a provider under selected native device guards");
    auto competing=Host::ReadStartupCredentials(host.runtime,request,provisioner.source,provisioner.tag,provisioner.derived);
    const auto competing_budget=allocation_budget;allocation_budget=-1;
    Check(competing.error==server::NativeHostedRuntimeError::busy&&competing_budget==0,
        "retained selected credential refuses a second read before allocator/provider callbacks under its device guards");
    competing.Reset();
    Check(admitted.comparison->lease->principal().principal_uuid==security_fixture::Id(15000)&&
        admitted.comparison->lease->principal().principal_uuid!=security_fixture::Id(14000),"service proof retains distinct provisioned bootstrap owner");
    allocation_budget=0;const auto rights=read.authorization();
    const auto remaining=allocation_budget;allocation_budget=-1;
    // D5 (appendix-agent-security-roles, owner 2026-10-29): the database is
    // the L5 agent and a member of SYSARCH. Before 2026-11-03 native create
    // omitted that membership and this asserted one subject, no engine-owned
    // role and exactly the five direct rights.
    if(!rights.ok()||remaining!=0||rights.projection.subjects().size()!=2||rights.projection.grants().size()!=(memory_variant==3?4:5)||
        rights.projection.engine_owned_bootstrap_role_uuid()!=db::kCanonicalSysarchRoleIdentity||
        // Variant 3 removes one service grant row.
        rights.projection.source_record_count()!=kEnrolledAuthorizationRecords-(memory_variant==3?1:0)){
      platform::Uuid failing;int kind=-1;
      if(rights.failing_record<admitted.comparison->lease->security_records().size()){
        const auto& row=admitted.comparison->lease->security_records()[rights.failing_record];
        failing=row.object_uuid;kind=static_cast<int>(row.record.kind);
      }
      // Render diagnostic facts only after actual source/device guards release.
      read.Reset();
      std::cerr<<"enrolled rights error="<<int(rights.error)<<" record="<<rights.failing_record<<" record_error="
          <<int(rights.record_error)<<" kind="<<kind<<" object_tail="
          <<((unsigned(failing.bytes[12])<<24)|(unsigned(failing.bytes[13])<<16)|(unsigned(failing.bytes[14])<<8)|failing.bytes[15])
          <<" subjects="<<rights.projection.subjects().size()<<" grants="<<rights.projection.grants().size()
          <<" records="<<rights.projection.source_record_count()<<'\n';
      Check(false,"actual service has its five direct grants and its SYSARCH membership");
    }
    // As SYSARCH, L5 holds every right database-wide, a removed direct grant
    // included (memory variant 3). Only an explicit deny narrows it (variant 13).
    for(const auto& right:runtime_security::KnownSecurityRights())
      Check(rights.projection.Allows(right),"L5 as a SYSARCH member holds every known right database-wide");
    if(memory_variant==13)Check(rights.projection.Allows("CONNECT")&&!rights.projection.Allows("CONNECT",Id(1)),
        "a global CONNECT allow cannot override the persisted denial for this actual database");
    Check(stage_writes==writes&&stage_syncs==syncs,"credential/rights prerequisites publish no activation or transaction");
    allocation_budget=0;
    Check(Host::InstallStartupAllocationBehavior(read).error==EnvelopeError::selected_guards_held,
        "selected allocator behavior cannot bypass original device-guard retirement");
    const auto premature_envelope=Host::ConstrainStartupMemory(read);
    const auto premature_budget=allocation_budget;allocation_budget=-1;
    Check(premature_envelope.error==EnvelopeError::selected_guards_held&&premature_budget==0,
        "memory constraint cannot allocate or activate under retained selected device guards");
    if(reopen||death>5||memory_variant||policy_variant||authentication_variant){
      const auto operation_uuid=admitted.operation.identity();
      Check(!mutex_observation_overflow&&SelectedGuardDepth()>0&&
          admitted.comparison->lease->has_selected_source(),"probe retains actual selected native mutexes before handoff");
      guarded_handoff_releases=0;handoff_budget_at_unlock=1;handoff_guard_unlocked=false;
      observed_allocations=0;count_allocations=true;observe_handoff_releases=true;allocation_budget=0;
      cleanup_read=&read;cleanup_host=&host.runtime;cleanup_hook=ProbeCleanupCallback;
      auto retained=Host::RetainStartupObservation(read);
      const auto handoff_budget=handoff_budget_at_unlock;count_allocations=false;
      const auto guarded_allocations=observed_allocations;allocation_budget=-1;observe_handoff_releases=false;
      const auto callback_invoked=!cleanup_hook;cleanup_hook=nullptr;
      const auto guard_depth=SelectedGuardDepth();
      if(ExpectedPolicyError(policy_variant)!=PolicyError::none){
        observe_mutexes=false;
        if(retained!=server::NativeHostedRuntimeError::agent_policy_failure||read.agent_policies().error!=ExpectedPolicyError(policy_variant))
          std::cerr<<"policy variant="<<policy_variant<<" handoff="<<int(retained)<<" actual="<<int(read.agent_policies().error)
              <<" expected="<<int(ExpectedPolicyError(policy_variant))<<" field="<<read.agent_policies().field.data()
              <<" diagnostic="<<read.agent_policies().diagnostic_code.data()<<'\n';
        Check(retained==server::NativeHostedRuntimeError::agent_policy_failure&&
            read.agent_policies().error==ExpectedPolicyError(policy_variant)&&
            !read.agent_policies().failing_object_uuid.is_nil()&&!read.agent_policies().policy_count&&
            !read.agent_policies().registration_count&&read.ok()&&!guard_depth&&
            admitted.comparison->lease->has_retained_operation()&&!read.publication().ok(),
            "invalid persisted policy cohort refuses as a whole after guard retirement and before publication/resource setup");
        if(policy_variant==1){
          allocation_budget=0;const auto exhausted=Host::RetainStartupObservation(read);allocation_budget=-1;
          Check(exhausted==server::NativeHostedRuntimeError::agent_policy_failure&&
              read.agent_policies().error==PolicyError::resource_exhausted&&read.ok()&&
              admitted.comparison->lease->has_retained_operation()&&!admitted.comparison->lease->has_selected_source(),
              "allocation refusal during policy retry preserves exact source and original writer custody");
          Check(Host::RetainStartupObservation(read)==server::NativeHostedRuntimeError::agent_policy_failure&&
              read.agent_policies().error==ExpectedPolicyError(policy_variant),
              "policy retry after allocation failure revalidates the same persisted invalid input");
        }
        Check(Host::ConstrainStartupMemory(read).error==EnvelopeError::not_ready&&stage_writes==writes&&stage_syncs==syncs,
            "policy refusal cannot advance resource setup or publish activation effects");
        Check(Host::Close(host.runtime).custody.writer.error==db::NativeWriterError::busy,
            "policy refusal keeps the original credential/operation owner until explicit retirement");
        read.Reset();Check(!read.agent_policies().ok()&&Host::Close(host.runtime).ok(),"refused policy cohort retires explicitly");
        host.runtime.reset();const auto restored=mem::DefaultMemoryManager().Snapshot();
        Check(restored.current_bytes==manager_baseline.current_bytes&&restored.reserved_capacity_bytes==manager_baseline.reserved_capacity_bytes&&
            restored.active_capacity_reservation_count==manager_baseline.active_capacity_reservation_count,
            "policy refusal releases all actual host allocations and reservations");
        continue;
      }
      if(retained!=server::NativeHostedRuntimeError::none||handoff_budget||!handoff_guard_unlocked||guarded_allocations||guard_depth||
          guarded_handoff_releases||mutex_observation_overflow){
        std::cerr<<"policy error="<<int(read.agent_policies().error)<<" field="<<read.agent_policies().field.data()
            <<" diagnostic="<<read.agent_policies().diagnostic_code.data()<<'\n';
        observe_mutexes=false;read.Reset();
        std::cerr<<"handoff error="<<int(retained)<<" budget="<<handoff_budget<<" depth="<<guard_depth
            <<" guarded_allocations="<<guarded_allocations<<" guarded_frees="<<guarded_handoff_releases
            <<" overflow="<<mutex_observation_overflow<<'\n';
      }
      Check(retained==server::NativeHostedRuntimeError::none&&handoff_guard_unlocked&&handoff_budget==0&&
          !guarded_allocations&&!SelectedGuardDepth()&&!guarded_handoff_releases&&!mutex_observation_overflow,
          "exact hosted handoff releases every selected native guard before ordinary allocation or free");
      Check(callback_invoked&&callback_close==server::NativeHostedRuntimeError::busy&&
          callback_retain==server::NativeHostedRuntimeError::busy&&callback_read_ok,
          "guard retirement callback cannot reenter retirement or close the original host");
      Check(read.publication().ok()&&read.publication().snapshot->watermark.watermark==1&&
          read.publication().snapshot->selection.checkpoint_generation==1&&
          read.publication().effects.write_attempts==0&&read.publication().effects.sync_attempts==0,
          "native host observes actual publication state through original retained operation with no startup mutation");
      Check(read.agent_policies().ok()&&read.agent_policies().policy_count==34&&read.agent_policies().registration_count==32,
          "every actual persisted policy and registration passes canonical descriptor, schema and static dependency validation");
      const auto clock_entry=std::find_if(read.agents().entries().begin(),read.agents().entries().end(),[](const auto& entry){
        return entry.definition.kind==catalog::AgentBootstrapDefinitionKind::policy&&entry.definition.agent_type=="agent_coordinator";});
      Check(clock_entry!=read.agents().entries().end(),"actual retained coordinator owns startup clock configuration");
      allocation_budget=0;
      const auto clock=scratchbird::core::agents::SelectAgentStartupClockPolicy(clock_entry->definition,clock_entry->version_uuid);
      const auto clock_heap=allocation_budget;allocation_budget=-1;
      Check(clock.ok()&&clock_heap==0&&clock.database_uuid==Id(1)&&clock.policy_uuid==clock_entry->definition.definition_uuid&&
          clock.version_uuid==clock_entry->version_uuid&&clock.generation==clock_entry->definition.generation&&
          clock.policy.max_uuid_regression_ms==0&&clock.policy.same_ms_wait_timeout_ms==1000&&
          clock.policy.clock.max_wall_clock_backward_nanoseconds==0&&clock.policy.clock.max_wall_clock_forward_jump_nanoseconds==60000000000ULL&&
          clock.policy.clock.fail_closed_on_wall_clock_rollback&&clock.policy.clock.fail_closed_on_monotonic_regression&&
          !clock.policy.clock.fail_closed_on_wall_clock_forward_jump,
          "native source retains exact persisted standalone clock limits and distinct coordinator version without default substitution");
      if(profile==0&&reopen==0&&!policy_variant&&!memory_variant){
        using CE=scratchbird::core::agents::AgentStartupClockPolicyError;
        constexpr std::pair<std::string_view,std::string_view> invalid_clock[]{
          {"startup_uuid_same_ms_wait_timeout_ms","0"},{"startup_uuid_same_ms_wait_timeout_ms","18446744073710"},
          {"startup_uuid_max_regression_ms","281474976710656"},{"startup_clock_max_backward_ns","18446744073709551616"},
          {"startup_clock_max_backward_ns","-1"},{"startup_clock_max_backward_ns","+1"},
          {"startup_clock_max_backward_ns","1x"},{"startup_clock_max_backward_ns"," 1"},
          {"startup_clock_max_backward_ns",""},{"startup_clock_fail_on_rollback","1"},
          {"startup_clock_fail_on_monotonic_regression","maybe"},{"startup_clock_fail_on_forward_jump","TRUE"}};
        for(const auto& [key,value]:invalid_clock){
          std::string body(clock_entry->definition.properties);const auto name="config."+std::string(key)+"=";
          const auto at=body.find(name)+name.size();body.replace(at,body.find('\n',at)-at,value);
          auto declaration=clock_entry->definition;declaration.properties=body;
          allocation_budget=0;const auto invalid=scratchbird::core::agents::SelectAgentStartupClockPolicy(declaration,clock_entry->version_uuid);
          const auto heap=allocation_budget;allocation_budget=-1;
          Check(invalid.error==CE::invalid_field&&heap==0&&invalid.policy_uuid.is_nil(),
              "invalid explicit clock fields refuse without allocating or exposing partial source binding");
        }
        for(unsigned fault=0;fault<4;++fault){
          std::string body(clock_entry->definition.properties);
          const auto at=body.find("config.startup_uuid_same_ms_wait_timeout_ms=");
          if(fault==0)body.erase(at,body.find('\n',at)-at+1);
          if(fault==1)body+="config.startup_uuid_same_ms_wait_timeout_ms=1000\n";
          if(fault==2)body.pop_back();
          if(fault==3)body.replace(at,body.find('\n',at)-at,"config.startup_uuid_same_ms_wait_timeout_ms=2500");
          auto declaration=clock_entry->definition;declaration.properties=body;
          allocation_budget=0;const auto decoded=scratchbird::core::agents::SelectAgentStartupClockPolicy(declaration,clock_entry->version_uuid);
          const auto heap=allocation_budget;allocation_budget=-1;
          Check(heap==0&&(fault==3?decoded.ok()&&decoded.policy.same_ms_wait_timeout_ms==2500:
              decoded.error==(fault==0?CE::missing_field:CE::invalid_field)),
              "clock selection requires unique complete explicit fields and preserves configured nondefault limits");
        }
      }
      if(policy_variant==17){
        const auto configured=std::find_if(read.agents().entries().begin(),read.agents().entries().end(),[](const auto& entry){
          return entry.definition.kind==catalog::AgentBootstrapDefinitionKind::policy&&entry.definition.agent_type=="memory_governor";});
        Check(configured!=read.agents().entries().end()&&server::native_agent_configuration_detail::Property(
            configured->definition.properties,"run_interval_microseconds")=="2100000","valid configured scheduling survives without baseline substitution");
      }
      if(policy_variant==22){
        const auto configured=std::find_if(read.agents().entries().begin(),read.agents().entries().end(),[](const auto& entry){
          return entry.definition.kind==catalog::AgentBootstrapDefinitionKind::policy&&entry.definition.agent_type=="startup_agent";});
        Check(configured!=read.agents().entries().end()&&
            server::native_agent_configuration_detail::Property(configured->definition.properties,"require_manual_approval")=="1"&&
            server::native_agent_configuration_detail::Property(configured->definition.properties,"require_dry_run_before_live")=="1",
            "valid stricter approval and dry-run prerequisites are retained without satisfying their runtime gates");
      }
      Check(read.ok()&&!admitted.operation&&admitted.comparison->lease->has_retained_operation()&&
          !admitted.comparison->lease->has_selected_source()&&
          admitted.comparison->lease->writer_operation_uuid()==operation_uuid&&
          admitted.comparison->lease->credential_binding().generation==7&&
          read.configuration().ok()&&read.agents().ok()&&read.agents().entries().size()==66&&
          AgentPayloadFingerprint(read.agents())==agent_payload_fingerprint&&
          // D5: SYSARCH keeps CONNECT even without the direct grant (variant 3).
          read.authorization().projection.Allows("CONNECT"),
          "original writer reservation, protected credential generation and governed observations survive guard retirement");
      Check(Host::RetainStartupObservation(read)==server::NativeHostedRuntimeError::none&&
          !server::SelectNativeLocalServiceStartupDeclarations(*admitted.comparison->lease).ok()&&
          !server::SelectNativeLocalServiceStartupConfiguration(*admitted.comparison->lease).ok()&&
          !server::SelectNativeLocalServiceAgentConfiguration(*admitted.comparison->lease).ok(),
          "retirement is idempotent; a retired selected source cannot be navigated again");
      auto revocation_request=request.source;
      revocation_request.deadline=server::StartupCredentialSource::Mutex::Clock::now()+std::chrono::milliseconds(5);
      const auto refused_revocation=provisioner.source->Revoke(revocation_request);
      Check(!refused_revocation.ok()&&refused_revocation.error==server::StartupCredentialSource::Error::wait&&
          admitted.comparison->lease->valid()&&admitted.comparison->lease->credential_binding().generation==7,
          "guard retirement cannot unlock or revoke the separately retained protected credential generation");
      // Resource/provider preparation may now allocate and free. The mutex
      // probe names the actual retained guards, not another FileDevice/inode.
      auto allocated=std::make_unique<std::array<byte,8192>>();allocated.reset();
      Check(!SelectedGuardDepth()&&stage_writes==writes&&stage_syncs==syncs,
          "allocation after guard retirement changes no physical publication or admission state");
      // D5: L5 is a SYSARCH member, so removing its direct CONNECT grant
      // (variant 3) no longer refuses; only an explicit database deny does
      // (variant 13). Before 2026-11-03 native create omitted the membership and
      // variant 3 refused here and at authentication and memory admission.
      Check(Host::InstallStartupAllocationBehavior(read).error==(memory_variant==13?
          EnvelopeError::authorization_refused:EnvelopeError::not_ready),
          "allocator behavior requires actual CONNECT and prior reservation constraints");
      const auto before_authentication=password_calls;
      Check(Host::AuthenticateStartupService(read,provisioner.derived).error==(memory_variant==13?
          AuthenticationError::authorization_refused:AuthenticationError::resource_policy_required)&&
          password_calls==before_authentication&&!read.authentication(),
          "engine service authentication requires real CONNECT and actual installed physical policy before verifier invocation");
      std::unique_ptr<mem::ReservationBackedMemoryResource> excess_bootstrap_credit;
      if(memory_variant==9) {
        // Each category fits its configured cap, but their combined live
        // credit exceeds the 64 MiB database cap. This reaches the atomic
        // parent-budget installation after the platform/child preflight.
        auto extra=process.AcquireNativeBootstrapResource(
            {Id(1),Id(243),Id(244),Id(245),30*1024*1024,server::NativeBootstrapMemoryKind::database_crypto});
        Check(extra.ok(),"actual provisional crypto credit before selected memory installation");
        excess_bootstrap_credit=std::move(extra.admission.resource);
      }
      if(memory_variant!=13){
        allocation_budget=0;const auto exhausted=Host::ConstrainStartupMemory(read);allocation_budget=-1;
        Check(exhausted.error==EnvelopeError::allocation_failure||exhausted.error==EnvelopeError::budget_refused,
            "memory admission allocation failure leaves the old budget and retained owner available for retry");
      }
      auto envelope=Host::ConstrainStartupMemory(read);
      if(memory_variant==9) {
        Check(envelope.error==EnvelopeError::budget_refused&&
            envelope.diagnostic.diagnostic_code=="SB-MEMORY-BUDGET-LIVE-SHRINK-REFUSED"&&read.ok()&&
            Host::Close(host.runtime).custody.writer.error==db::NativeWriterError::busy,
            "combined live credit refuses the parent limit without retiring original startup custody");
        auto retained_budget=process.AcquireNativeBootstrapResource(
            {Id(1),Id(246),Id(247),Id(248),64,server::NativeBootstrapMemoryKind::storage});
        Check(retained_budget.ok(),"failed parent installation leaves the previous actual budget usable");
        retained_budget.admission.resource.reset();excess_bootstrap_credit.reset();
        envelope=Host::ConstrainStartupMemory(read);
        Check(envelope.ok(),"the same selected policy installs after excess credit retires without reopening or replacing credentials");
      }
      if(memory_variant==2||memory_variant==13||memory_variant==6||memory_variant==7||memory_variant==8){
        const auto expected=memory_variant==2?EnvelopeError::policy_refused:
            memory_variant==13?EnvelopeError::authorization_refused:
            memory_variant==6?EnvelopeError::page_budget_refused:
            memory_variant==7?EnvelopeError::crypto_budget_refused:EnvelopeError::startup_floor_unavailable;
        if(envelope.error!=expected)std::cerr<<"memory variant="<<memory_variant<<" error="<<int(envelope.error)
            <<" diagnostic="<<envelope.diagnostic.diagnostic_code<<'\n';
        Check(envelope.error==expected&&
            read.ok()&&read.publication().ok(),
            "actual invalid memory policy, child budget, startup floor or missing CONNECT refuses with startup custody retained");
        if(memory_variant==2)Check(envelope.diagnostic.diagnostic_code=="MEMORY.POLICY_HARD_LIMIT_TOO_SMALL",
            "real platform policy resolver preserves the original minimum-capacity diagnostic");
        if(memory_variant==6||memory_variant==7) {
          Check(envelope.diagnostic.diagnostic_code=="SB-MEMORY-BUDGET-LIVE-SHRINK-REFUSED",
              "child-budget refusal retains its actual live-credit cause");
          const auto used=std::find_if(envelope.diagnostic.arguments.begin(),envelope.diagnostic.arguments.end(),
              [](const auto& argument){return argument.key=="existing_bytes";});
          Check(used!=envelope.diagnostic.arguments.end()&&used->text()&&
              std::stoull(*used->text())>(memory_variant==6?configuration.memory.page_buffer_pool_limit_bytes:
                  configuration.memory.openssl_budget_bytes),"refusal reports actual existing credit above the selected child limit");
        }
        if(memory_variant==8)Check(envelope.diagnostic.diagnostic_code=="CONFIG.MEMORY_POLICY_MIN_STARTUP_UNAVAILABLE",
            "startup-floor refusal carries its original policy cause");
        const auto retry=Host::ConstrainStartupMemory(read);
        Check(retry.error==expected,"refused resource configuration cannot leave a cached successful installation");
      }else{
        if(!envelope.ok())std::cerr<<"memory variant="<<memory_variant<<" error="<<int(envelope.error)
            <<" diagnostic="<<envelope.diagnostic.diagnostic_code<<'\n';
        Check(envelope.ok()&&envelope.database==Id(1)&&envelope.configuration==configuration.memory.configuration_uuid&&
            envelope.version==configuration.memory_version_uuid&&envelope.configuration_generation==configuration.memory.generation&&
            envelope.configured_hard_limit_bytes==configuration.memory.hard_limit_bytes&&
            envelope.effective_hard_limit_bytes>0&&envelope.effective_hard_limit_bytes<=envelope.configured_hard_limit_bytes,
            "native host installs the exact observed database ceiling in the real process ledger");
        Check(envelope.configured_soft_limit_bytes==configuration.memory.soft_limit_bytes&&
            envelope.enforced_soft_limit_bytes==(configuration.memory.reject_over_soft_limit?configuration.memory.soft_limit_bytes:0)&&
            envelope.page_buffer_limit_bytes==configuration.memory.page_buffer_pool_limit_bytes&&
            envelope.crypto_limit_bytes==configuration.memory.openssl_budget_bytes,
            "actual selected soft, page and crypto settings bind the installed native budget");
        allocation_budget=0;const auto again=Host::ConstrainStartupMemory(read);
        const auto repeated_budget=allocation_budget;allocation_budget=-1;
        Check(again.ok()&&again.effective_hard_limit_bytes==envelope.effective_hard_limit_bytes&&repeated_budget==0,
            "same-source envelope admission is idempotent without replacing live credit or allocating");
        const auto too_large=[&]{return process.AcquireNativeBootstrapResource(
            {Id(1),Id(240),Id(241),Id(242),envelope.effective_hard_limit_bytes+1,server::NativeBootstrapMemoryKind::storage});};
        Check(!too_large().ok(),"actual governed allocation above the selected database ceiling refuses");
        auto small=process.AcquireNativeBootstrapResource(
            {Id(1),Id(240),Id(241),Id(242),64,server::NativeBootstrapMemoryKind::storage});
        if(memory_variant==4) {
          Check(!small.ok()&&small.admission.diagnostic.diagnostic_code=="SB-MEMORY-BUDGET-SOFT-LIMIT-RECOMMENDATION",
              "configured rejecting soft limit prevents new reservations without revoking retained startup credit");
        }else Check(small.ok(),"smaller actual database allocation honors configured advisory soft limits");
        Check(!too_large().ok(),"later bootstrap allocations cannot reset the selected limit to the larger process ceiling");
        if(memory_variant==1)held_memory=std::move(small.admission.resource);
        if(memory_variant==1||memory_variant==5) {
          auto crypto=process.AcquireNativeBootstrapResource(
              {Id(1),Id(243),Id(244),Id(245),2*1024*1024,server::NativeBootstrapMemoryKind::database_crypto});
          Check(crypto.ok(),"remaining actual selected crypto credit can be reserved");
          auto extra=process.AcquireNativeBootstrapResource(
              {Id(1),Id(246),Id(247),Id(248),1,server::NativeBootstrapMemoryKind::database_crypto});
          Check(extra.error==server::NativeBootstrapMemoryError::child_budget_refused,
              "multiple crypto grants cannot exceed the aggregate selected database crypto budget");
          crypto.admission.resource.reset();
          extra=process.AcquireNativeBootstrapResource(
              {Id(1),Id(246),Id(247),Id(248),64,server::NativeBootstrapMemoryKind::database_crypto});
          Check(extra.ok(),"retiring a real crypto grant restores its actual credit");
          auto page=process.AcquireNativeBootstrapResource(
              {Id(1),Id(243),Id(244),Id(245),envelope.page_buffer_limit_bytes,server::NativeBootstrapMemoryKind::storage});
          Check(page.error==server::NativeBootstrapMemoryError::child_budget_refused,
              "page admission includes the original live native storage backing in the selected aggregate cap");
        }
      }
      if(envelope.ok()) {
        using BehaviorStatus=mem::DatabaseAllocationBehaviorStatus;
        std::unique_ptr<mem::ReservationBackedMemoryResource> extra_context;
        if(memory_variant==12) {
          auto extra=process.AcquireNativeBootstrapResource(
              {Id(1),Id(243),Id(244),Id(245),16*1024*1024,server::NativeBootstrapMemoryKind::database_crypto});
          Check(extra.ok(),"additional real bootstrap capacity fits database and crypto budgets before context selection");
          extra_context=std::move(extra.admission.resource);
        }
        auto behavior=Host::InstallStartupAllocationBehavior(read);
        if(memory_variant==12) {
          Check(!behavior.ok()&&behavior.allocation.status==BehaviorStatus::existing_credit_exceeds_limit&&
              behavior.allocation.existing_bytes>configuration.memory.per_context_limit_bytes,
              "context policy refuses actual retained excess without replacing startup owner");
          Check(read.ok()&&read.credentials().comparison->lease->has_retained_operation(),
              "failed allocator installation preserves exact credential and writer operation");
          extra_context.reset();behavior=Host::InstallStartupAllocationBehavior(read);
        }
        // Variant 3 carries variant 1's memory body (ChangeMemoryEnvelopeFixture)
        // and reaches here since D5 (it was refused at CONNECT before).
        if(memory_variant==1||memory_variant==3||memory_variant==4||memory_variant==5||memory_variant==9||memory_variant==10) {
          Check(!behavior.ok()&&behavior.allocation.status==BehaviorStatus::existing_credit_exceeds_limit&&
              behavior.allocation.existing_bytes>configuration.memory.per_context_limit_bytes&&
              behavior.allocation.limit_bytes==configuration.memory.per_context_limit_bytes,
              "stored per-context ceiling below existing native bootstrap credit refuses explicitly");
        }else {
          Check(behavior.ok()&&behavior.allocation.binding.policy_uuid==configuration.memory.configuration_uuid.bytes&&
              behavior.allocation.binding.generation==configuration.memory.generation,
              "retained actual catalog definition installs behavior in owning physical manager");
          allocation_budget=0;const auto repeated=Host::InstallStartupAllocationBehavior(read);
          const auto repeated_budget=allocation_budget;allocation_budget=-1;
          Check(repeated.ok()&&repeated_budget==0,"same selected allocation behavior is idempotent without allocation");
          mem::MemoryTag selected_tag;
          selected_tag.binary_ownership[mem::MemoryBinaryScopeKind::database]=Id(1).bytes;
          selected_tag.binary_ownership[mem::MemoryBinaryScopeKind::owner]=Id(241).bytes;
          selected_tag.binary_ownership[mem::MemoryBinaryScopeKind::context]=Id(242).bytes;
          const auto available=mem::DefaultMemoryManager().allocator()->AvailableCapacity(selected_tag);
          Check(available.ok()&&available.available_bytes<configuration.memory.per_context_limit_bytes,
              "selected physical context headroom includes all native bootstrap credit");
          const auto over=mem::DefaultMemoryManager().allocator()->ReserveCapacity(available.available_bytes+1,selected_tag);
          Check(!over.ok(),"direct physical capacity admission cannot bypass selected native context ceiling");
          if(memory_variant==11||memory_variant==12)
            Check(over.status.severity==platform::Severity::fatal,"selected native failure severity reaches owning allocator");
          if(memory_variant==11) {
            auto loan=process.AcquireNativeBootstrapResource(
                {Id(1),Id(240),Id(241),Id(242),64,server::NativeBootstrapMemoryKind::storage});
            Check(loan.ok(),"actual governed capacity remains usable under installed context behavior");
            auto allocation=loan.admission.resource->Allocate({64,64,{}});
            Check(allocation.ok()&&allocation.policy_binding==behavior.allocation.binding,
                "post-installation native allocation binds actual selected policy definition");
            Check(loan.admission.resource->DeallocateNoAlloc(allocation.pointer,64,64).ok(),"selected allocation payload retires");
            held_memory=std::move(loan.admission.resource);
          }
          const auto calls_before=password_calls;
          if(authentication_variant==1){
            const auto disabled=Host::AuthenticateStartupService(read,provisioner.derived);
            Check(disabled.error==AuthenticationError::provider_disabled&&!read.authentication()&&
                password_calls==calls_before&&read.ok(),
                "disabled actual provider cannot become active through a prior successful comparison");
          }else if(authentication_variant>=2){
            if(authentication_variant==2)password_cancel=&authentication_stop;
            else authentication_stop.request_stop();
            const auto cancelled=Host::AuthenticateStartupService(read,provisioner.derived);
            password_cancel=nullptr;
            Check(cancelled.error==AuthenticationError::cancelled&&!read.authentication()&&Zero(provisioner.derived)&&
                password_calls==calls_before+(authentication_variant==2?1:0),
                "cancellation before or inside engine provider work issues no authenticated decision");
          }else {
            mem::ScopedProtectedBuffer absent;
            Check(Host::AuthenticateStartupService(read,absent).error==AuthenticationError::protected_workspace_required&&
                password_calls==calls_before&&!read.authentication(),
                "native authentication rejects missing protected workspace before provider use");
            password_provider_fault=true;
            const auto fault=Host::AuthenticateStartupService(read,provisioner.derived);
            Check(fault.error==AuthenticationError::provider_failure&&
                fault.verifier_error==runtime_security::LocalPasswordCredentialError::provider_failure&&
                !read.authentication()&&Zero(provisioner.derived)&&read.ok(),
                "actual engine verifier failure erases partial output and preserves original custody for retry");
            const auto authenticated=Host::AuthenticateStartupService(read,provisioner.derived);
            Check(authenticated.ok()&&read.authentication()&&
                *read.authentication()==authenticated.observation&&
                scratchbird::core::uuid::IsEngineIdentityUuid(authenticated.observation.decision_uuid)&&
                authenticated.observation.database_uuid==Id(1)&&
                authenticated.observation.principal_uuid==security_fixture::Id(15000)&&
                authenticated.observation.provider_uuid==enrollment::SourceId(0x10)&&
                authenticated.observation.provider_version_uuid==admitted.comparison->lease->provider_version_uuid()&&
                authenticated.observation.operation_uuid==operation_uuid&&
                authenticated.observation.credential_source_generation==7&&Zero(provisioner.derived)&&
                password_calls==calls_before+2,
                "engine verifier authenticates exact retained local service after actual policy admission");
            allocation_budget=0;const auto same=Host::AuthenticateStartupService(read,absent);
            const auto repeat_budget=allocation_budget;allocation_budget=-1;
            Check(same.ok()&&same.observation==authenticated.observation&&repeat_budget==0&&
                password_calls==calls_before+2,
                "live authentication is idempotent without a new verifier call, identity or heap allocation");
            const auto held_source=provisioner.source->Revoke(revocation_request);
            Check(Host::AttachStartupServiceSession(read).error==SessionError::not_ready&&!read.session(),
                "authenticated service cannot attach before actual catalog and history validation");
            Check(!held_source.ok()&&held_source.error==server::StartupCredentialSource::Error::wait,
                "authenticated decision retains its actual protected-source generation fence after plaintext erasure");
            if(!memory_variant&&!policy_variant){
              const db::NativeSelectedCheckpointMemoryLimits catalog_limits{budget,12*1024*1024};
              // Bound the page working set in bytes across all page profiles;
              // larger pages need fewer leaf and navigation slots. The row
              // index still covers the complete submitted name catalog.
              // 2 MiB page working set (was 512 KiB) for the larger catalog with the
              // agent task-group roles (2026-10-29). Since 2026-11-03 the shared
              // complete-scan tier: the name relation (1666 rows, 2.07 MiB)
              // exceeded 1024 rows and 2 MiB.
              const auto catalog_page_slots=db::NativeCatalogScanBasePages(sizes[profile]);
              const db::NativeCatalogRelationMemoryLimits catalog_scan_limits{Profile(profile),1,catalog_page_slots,
                  db::kNativeCatalogScanRelationMaximumRows,db::kNativeCatalogScanRelationMaximumRows,
                  db::NativeCatalogScanNavigationLimits(sizes[profile])};
              allocation_budget=0;const auto catalog_no_memory=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits);allocation_budget=-1;
              Check(catalog_no_memory==server::NativeHostedRuntimeError::memory_failure&&!read.catalogs().ok(),
                  "complete native catalog scan requires actual shared memory admission");
              reads=0;track_reads=true;read_fault=1;
              const auto catalog_io=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits);
              track_reads=false;read_fault=0;
              if(catalog_io!=server::NativeHostedRuntimeError::catalog_failure||!reads)std::cerr
                  <<"catalog injected I/O error="<<int(catalog_io)<<" reads="<<reads<<" profile="<<profile
                  <<" reopen="<<reopen<<" workspace="<<db::NativeCatalogCohortWorkspaceBytes(catalog_scan_limits)<<'\n';
              Check(catalog_io==server::NativeHostedRuntimeError::catalog_failure&&!read.catalogs().ok()&&reads>0,
                  "catalog source I/O failure produces no complete catalog observation");
              reads=0;track_reads=true;
              const auto catalog_read=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits);track_reads=false;
              if(catalog_read!=server::NativeHostedRuntimeError::none)std::cerr<<"startup catalogs="<<int(catalog_read)
                  <<" scan="<<int(read.catalogs().error)<<" root="<<int(read.catalogs().root_error)
                  <<" visibility="<<int(read.catalogs().visibility_error)<<" relation="<<int(read.catalogs().relation_error)
                  <<" role="<<read.catalogs().failing_role<<" profile="<<profile<<'\n';
              Check(catalog_read==server::NativeHostedRuntimeError::none&&read.catalogs().ok()&&reads>0&&
                  read.catalogs().relation_count==6&&read.catalogs().source->transitional_selection_key.checkpoint_sha256==read.publication().snapshot->selection.checkpoint_sha256,
                  "actual startup scan covers every catalog role, metadata chain and committed visibility on original custody");
              for(std::size_t i=0;i<expected_catalog_rows.size();++i){
                const auto& relation=read.catalogs().relations[i];
                Check(relation.selector==2&&relation.role==i+1&&relation.versions==expected_catalog_rows[i]&&
                    relation.visible_rows==expected_catalog_rows[i]&&relation.base_pages>0,
                    "full catalog observation matches every independently submitted row, including empty roles");
              }
              reads=0;track_reads=true;allocation_budget=0;
              const auto catalog_again=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits);
              const auto catalog_heap=allocation_budget;allocation_budget=-1;track_reads=false;
              Check(catalog_again==server::NativeHostedRuntimeError::none&&catalog_heap==0&&!reads,
                  "completed bounded catalog observation replays without I/O or allocation");
              Check(Host::ReadStartupCatalogs(read,{1,catalog_limits.maximum_backing_bytes},catalog_scan_limits)==server::NativeHostedRuntimeError::invalid_request&&
                  Host::ReadStartupCatalogs(read,{budget,1},catalog_scan_limits)==server::NativeHostedRuntimeError::invalid_request,
                  "catalog scan cache cannot bypass requested work or backing limits");
              Check(Host::ReadStartupCatalogs(read,catalog_limits,limits)==server::NativeHostedRuntimeError::invalid_request,
                  "cached complete catalog scan cannot bypass the smaller credential-only relation bounds");
              // 1225 captured rows with the agent task-group roles (2026-10-29); text
              // (0.7 MB) and physical pages (at most 238) fit the original bounds
              // {1280, 2 MiB, 512 pages}. Measured 2026-11-03 with every agent
              // type's principal, grants and membership: 2500 rows, 1.41 MB of
              // text, 469 physical pages at 8 KiB.
              const db::NativeCatalogCaptureLimits capture_limits{4096,4*1024*1024,1,4,1024,128};
              Check(!read.catalog_capture().ok()&&read.catalog_capture().rows().empty(),
                  "count-only inspection grants no retained catalog row view");
              Check(Host::VerifyStartupCatalogMetadata(read)==server::NativeHostedRuntimeError::catalog_failure&&
                  !read.catalog_metadata().ok(),"count-only catalog inspection cannot satisfy engine metadata verification");
              const auto short_capture=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,{1,1,1});
              if(short_capture!=server::NativeHostedRuntimeError::catalog_failure)std::cerr<<"short capture="<<int(short_capture)<<'\n';
              Check(short_capture==server::NativeHostedRuntimeError::catalog_failure&&!read.catalog_capture().ok(),
                  "insufficient exact row/text backing exposes no partial catalog capture");
              if(profile==0&&!configuration_variant&&!graph_variant&&!death&&!agent_variant&&!memory_variant&&!policy_variant&&
                  !authentication_variant&&!name_variant&&!namespace_variant&&!filespace_variant&&!audit_variant&&!verification_policy_variant){
                auto without_inventory=capture_limits;without_inventory.maximum_transactions=0;without_inventory.maximum_physical_pages=0;
                without_inventory.maximum_initial_control_pages=0;
                Check(Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,without_inventory)==server::NativeHostedRuntimeError::none&&
                    read.catalog_capture().ok()&&!read.catalog_capture().inventory()&&!read.catalog_capture().has_physical_pages()&&
                    !read.catalog_capture().has_initial_controls(),
                    "catalog-only capture cannot manufacture retained transaction outcomes or physical page coverage");
                allocation_budget=0;
                const auto failed_upgrade=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,capture_limits);
                allocation_budget=-1;
                Check(failed_upgrade==server::NativeHostedRuntimeError::memory_failure&&read.catalog_capture().ok()&&!read.catalog_capture().inventory(),
                    "inventory upgrade needs a fresh actual grant and preserves previous catalog-only backing on refusal");
              }
              const auto captured=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,capture_limits);
              Check(captured==server::NativeHostedRuntimeError::none&&read.catalog_capture().ok()&&
                  read.catalog_capture().rows().size()==expected_catalog_bytes.size()&&
                  read.catalog_capture().source()->transitional_selection_key.checkpoint_sha256==read.publication().snapshot->selection.checkpoint_sha256,
                  "actual source-fenced catalog rows retain independent admitted backing after reader retirement");
              const auto captured_inventory=read.catalog_capture().inventory();
              std::size_t physical_count=1; // Master/feature root is one actual shared page here.
              for(std::size_t i=0;i<read.catalogs().relation_count;++i)
                physical_count+=read.catalogs().relations[i].base_pages+read.catalogs().relations[i].navigation_pages;
              Check(read.catalog_capture().has_physical_pages()&&read.catalog_capture().physical_pages().size()==physical_count&&
                  Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,{1024,2*1024*1024,1,4,1})==server::NativeHostedRuntimeError::invalid_request,
                  "complete actual root/navigation/base identities are retained and a smaller physical budget cannot reuse them");
              Check(read.catalog_capture().has_initial_controls()&&
                  read.catalog_capture().initial_control_pages().size()==expected_initial_control_pages&&
                  !read.catalog_capture().initial_control_roots().empty()&&
                  Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,{1024,2*1024*1024,1,4,512,1})==server::NativeHostedRuntimeError::invalid_request,
                  "complete actual initial checkpoint/map/inventory/directory identities survive reader retirement and cannot bypass a tighter control-page bound");
              if(profile==0&&!configuration_variant&&!graph_variant&&!death&&!agent_variant&&!memory_variant&&!policy_variant&&
                  !authentication_variant&&!name_variant&&!namespace_variant&&!filespace_variant&&!audit_variant&&!verification_policy_variant&&
                  !management_variant&&!transaction_policy_variant){
                const auto& captured_page=read.catalog_capture().physical_pages().front();const auto& h=captured_page.header;
                std::array<scratchbird::storage::page::NativeAllocationState,1> states{scratchbird::storage::page::NativeAllocationState::allocated};
                std::array<scratchbird::storage::page::NativeAllocationRecord,1> records{};auto& record=records[0];
                record.page_number=h.page_number;record.page_uuid=h.page_uuid;record.owner_uuid=captured_page.owner_uuid;
                record.page_generation=h.page_generation;record.page_type=h.page_type;
                std::array<db::NativeReadAllocationPage,1> pages{};auto& map=pages[0].map;
                map.header=h;map.first_page=h.page_number;map.states=states;map.records=records;
                db::NativeReadAllocationChain comparison;comparison.error=scratchbird::storage::page::NativeAllocationError::none;comparison.pages=pages;
                const auto checked=runtime_security::CheckNativeStartupCatalogAllocation(read.catalog_capture(),comparison);
                Check(checked.ok()&&checked.matched_pages==1,"pure allocation comparison matches an actual captured physical identity without granting a full scan");
                record.owner_uuid=Id(223);
                Check(runtime_security::CheckNativeStartupCatalogAllocation(read.catalog_capture(),comparison).error==runtime_security::NativeStartupCatalogAllocationError::invalid_record,
                    "a matching slot and generation cannot hide the wrong physical catalog owner");
                record.owner_uuid=captured_page.owner_uuid;record.page_number=map.first_page=1000000;
                Check(runtime_security::CheckNativeStartupCatalogAllocation(read.catalog_capture(),comparison).error==runtime_security::NativeStartupCatalogAllocationError::orphan_page,
                    "an extra catalog page outside the actually visited graph cannot be classified as verified");
                record.page_type=0x100;
                Check(runtime_security::CheckNativeStartupCatalogAllocation(read.catalog_capture(),comparison).error==runtime_security::NativeStartupCatalogAllocationError::orphan_page,
                    "unvisited user content cannot become an empty first-create graph");
              }
              Check(captured_inventory&&captured_inventory->generation==1&&
                  captured_inventory->inventory.next_local_transaction_id==2&&captured_inventory->inventory.next_commit_sequence==2&&
                  captured_inventory->inventory.entries.size()==1&&
                  captured_inventory->inventory.entries[0].identity.transaction_uuid.value==security_tx.value&&
                  captured_inventory->inventory.entries[0].identity.transaction_uuid.kind==platform::UuidKind::transaction&&
                  captured_inventory->inventory.entries[0].identity.local_id.value==1&&
                  mga::HasCommittedInventoryOutcome(captured_inventory->inventory.entries[0])&&
                  captured_inventory->inventory.entries[0].commit_sequence==1,
                  "retained native inventory carries actual construction commit and counters after physical guards retire");
              const auto& inventory_entry=captured_inventory->inventory.entries[0];
              Check(!read.catalog_capture().DisjointWorkspace(std::as_bytes(std::span{&inventory_entry,1})),
                  "consumer workspace cannot overwrite retained inventory outcomes");
              for(const auto& row:read.catalog_capture().rows()){
                const auto found=expected_catalog_bytes.find(row.row.metadata.record.header.object_uuid.value);
                Check(found!=expected_catalog_bytes.end()&&found->second==CatalogVariableBytes(row.row.metadata,row.row.name_payload),
                    "every captured payload, metadata label and name byte survives relation workspace reuse");
              }
              // Sized for the default pack with agent task-group roles (2026-10-29).
              std::array<std::size_t,4096> name_workspace;
              allocation_budget=0;
              const auto names=runtime_security::VerifyNativeCatalogNameBindings(read.catalog_capture(),name_workspace);
              const auto name_heap=allocation_budget;allocation_budget=-1;
              if(!name_variant)Check(names.ok()&&name_heap==0&&names.definitions==expected_definitions&&
                  names.vectors==expected_vectors&&names.entries==expected_entries,
                  "engine name verification resolves every actual object, vector and default alias without allocation");
              else {
                using N=runtime_security::NativeCatalogNameVerificationError;
                const auto expected=name_variant==6?N::missing_vector:name_variant==1?N::missing_object:(name_variant==3||name_variant==5)?N::object_binding:N::default_binding;
                Check(names.error==expected&&name_heap==0&&!names.object_uuid.is_nil()&&
                    !names.definitions&&!names.vectors&&!names.entries,
                    "engine refuses actual persisted dangling, wrong-class, wrong-language or missing-default name binding");
              }
              Check(runtime_security::VerifyNativeCatalogNameBindings(read.catalog_capture(),{}).error==
                  runtime_security::NativeCatalogNameVerificationError::workspace_required,
                  "engine name verification refuses missing bounded index workspace");
              {
                allocation_budget=0;
                const auto index_check=runtime_security::VerifyNativeCatalogIndexBindings(read.catalog_capture());
                const auto index_heap=allocation_budget;allocation_budget=-1;
                using I=runtime_security::NativeCatalogIndexVerificationError;
                const auto expected=ExpectedIndexError(index_variant);
                Check(index_check.error==expected&&index_heap==0&&(expected==I::none?
                    index_check.indexed_relations==4&&index_check.referenced_uuid.is_nil():
                    !index_check.indexed_relations&&index_check.role==(index_variant>=4?2:4)&&!index_check.referenced_uuid.is_nil()),
                    "engine resolves every catalog index tuple against its actual profile and map rows without allocation");
              }
              const auto fixed_rows=read.catalog_capture().rows();
              auto* aliased_index=reinterpret_cast<std::size_t*>(
                  const_cast<db::NativeCapturedCatalogRow*>(fixed_rows.data()));
              Check(runtime_security::VerifyNativeCatalogNameBindings(read.catalog_capture(),
                  {aliased_index,fixed_rows.size()}).error==
                  runtime_security::NativeCatalogNameVerificationError::workspace_required,
                  "engine rejects index scratch overlapping captured rows before writing it");
              for(const auto& captured_row:fixed_rows){
                const auto& payload=captured_row.row.metadata.record.payload;
                if(!payload.empty())Check(!read.catalog_capture().DisjointWorkspace(
                    std::as_bytes(std::span{payload.data(),payload.size()})),
                    "engine scratch must not alias retained catalog payload bytes");
              }
              Check(read.catalog_capture().DisjointWorkspace(std::as_bytes(std::span{name_workspace})),
                  "independent caller scratch remains admissible after alias refusals");
              std::array<std::size_t,4096> namespace_parents;
              std::array<platform::byte,4096> namespace_colors;
              allocation_budget=0;
              const auto namespaces=runtime_security::VerifyNativeCatalogNamespaces(read.catalog_capture(),
                  {name_workspace,namespace_parents,namespace_colors});
              const auto namespace_heap=allocation_budget;allocation_budget=-1;
              using NE=runtime_security::NativeCatalogNamespaceError;
              if(!namespace_variant)Check(namespaces.ok()&&namespace_heap==0&&namespaces.schemas==3&&
                  namespaces.database_object_uuid==security_fixture::Id(90000)&&namespaces.scoped_rows>0,
                  "engine resolves the actual native database and every namespace through an acyclic schema forest");
              else{
                const std::array expected{NE::none,NE::missing_schema,NE::database_binding,NE::cyclic_parent,
                    NE::database_reference,NE::missing_database,NE::database_binding};
                Check(namespaces.error==expected[namespace_variant]&&namespace_heap==0&&
                    !namespaces.schemas&&!namespaces.scoped_rows&&namespaces.database_object_uuid.is_nil(),
                    "persisted missing namespace, database mismatch, cycle and wrong root retain no verification success");
              }
              Check(runtime_security::VerifyNativeCatalogNamespaces(read.catalog_capture(),
                  {name_workspace,name_workspace,namespace_colors}).error==NE::workspace_required,
                  "namespace index and parent storage cannot alias");
              Check(read.catalog_capture().filespaces().size()==1&&
                  read.catalog_capture().filespaces()[0].bootstrap.filespace_uuid==construction.bootstrap.filespace_uuid&&
                  read.catalog_capture().filespaces()[0].bootstrap.page_size_bytes==sizes[profile],
                  "captured actual directory retains the original filespace identity and page profile after guards retire");
              allocation_budget=0;
              const auto filespaces=runtime_security::VerifyNativeCatalogFilespaces(read.catalog_capture(),
                  security_fixture::Id(90000),namespace_colors);
              const auto filespace_heap=allocation_budget;allocation_budget=-1;
              using FE=runtime_security::NativeCatalogFilespaceError;
              const std::array expected_filespace{FE::none,FE::missing_manifest,FE::database_binding,
                  FE::unknown_filespace,FE::physical_binding,FE::physical_binding,FE::physical_binding,
                  FE::physical_binding,FE::invalid_manifest,FE::duplicate_filespace};
              Check(filespaces.error==expected_filespace[filespace_variant]&&filespace_heap==0&&
                  (filespace_variant?!filespaces.manifests:filespaces.manifests==1),
                  "engine binds every actual filespace manifest and refuses missing, foreign, duplicate or incompatible physical declarations");
              Check(runtime_security::VerifyNativeCatalogFilespaces(read.catalog_capture(),security_fixture::Id(90000),{}).error==FE::workspace_required,
                  "filespace closure requires bounded workspace before changing it");
              auto* alias=reinterpret_cast<platform::byte*>(const_cast<scratchbird::storage::page::NativeFilespaceDirectoryRecord*>(read.catalog_capture().filespaces().data()));
              Check(runtime_security::VerifyNativeCatalogFilespaces(read.catalog_capture(),security_fixture::Id(90000),
                  {alias,sizeof(scratchbird::storage::page::NativeFilespaceDirectoryRecord)}).error==FE::workspace_required,
                  "filespace verifier refuses scratch over its retained source directory");
              allocation_budget=0;
              const auto audit_check=runtime_security::VerifyNativeCatalogAuditReferences(read.catalog_capture(),name_workspace);
              const auto audit_heap=allocation_budget;allocation_budget=-1;
              using AE=runtime_security::NativeCatalogAuditError;
              const auto expected_audit=audit_variant==2?AE::invalid_event:audit_variant?AE::missing_event:AE::none;
              Check(audit_check.error==expected_audit&&audit_heap==0&&
                  (audit_variant?(!audit_check.events&&!audit_check.references):
                    (audit_check.events==1&&audit_check.references==expected_catalog_bytes.size())),
                  "native audit references resolve retained actual intent and reject dangling, wrong-class and row-identity substitutes");
              Check(runtime_security::VerifyNativeCatalogAuditReferences(read.catalog_capture(),{}).error==AE::workspace_required&&
                  runtime_security::VerifyNativeCatalogAuditReferences(read.catalog_capture(),
                    {aliased_index,fixed_rows.size()}).error==AE::workspace_required,
                  "audit verifier refuses absent or source-overlapping scratch before writing it");
              allocation_budget=0;
              const auto security_check=runtime_security::VerifyNativeCatalogSecurityReferences(read.catalog_capture(),name_workspace);
              const auto security_heap=allocation_budget;allocation_budget=-1;
              using SE=runtime_security::NativeCatalogSecurityReferenceError;
              const std::array reference_errors{SE::none,SE::missing_owner,SE::invalid_owner,SE::missing_policy,SE::invalid_policy,SE::invalid_policy};
              Check(security_check.error==reference_errors[reference_variant]&&security_heap==0&&
                  (reference_variant?(!security_check.owners&&!security_check.policies&&!security_check.pack_references):
                    (security_check.owners==expected_catalog_bytes.size()&&security_check.policies==expected_catalog_bytes.size()&&
                     security_check.pack_references==expected_catalog_bytes.size())),
                  "every actual owner and security-policy reference resolves its native family and exact policy pack; dangling or wrong-family values refuse without partial success");
              Check(runtime_security::VerifyNativeCatalogSecurityReferences(read.catalog_capture(),{}).error==SE::workspace_required&&
                  runtime_security::VerifyNativeCatalogSecurityReferences(read.catalog_capture(),
                    {aliased_index,fixed_rows.size()}).error==SE::workspace_required,
                  "security reference checks refuse absent or captured-source-overlapping workspace");
              allocation_budget=0;
              const auto naming_check=runtime_security::VerifyNativeCatalogNamingProfileReferences(read.catalog_capture(),name_workspace);
              const auto naming_heap=allocation_budget;allocation_budget=-1;
              using PE=runtime_security::NativeCatalogNamingProfileError;
              const std::array profile_errors{PE::none,PE::missing_dialect,PE::invalid_dialect,PE::missing_identifier,
                  PE::invalid_identifier,PE::identifier_generation,PE::mode_flags,PE::entry_binding,
                  PE::unsupported_supplement,PE::invalid_identifier,PE::invalid_dialect,
                  PE::lookup_key,PE::lookup_key,PE::lookup_key,PE::lookup_key,PE::lookup_key,PE::lookup_key};
              const auto actual_entry_count=std::count_if(read.catalog_capture().rows().begin(),read.catalog_capture().rows().end(),
                  [](const auto& row){return row.row.name_payload&&std::holds_alternative<catalog::CatalogNameEntryView>(*row.row.name_payload);});
              Check(naming_check.error==profile_errors[naming_profile_variant]&&naming_heap==0&&
                  (naming_profile_variant?(!naming_check.identifiers&&!naming_check.dialects&&!naming_check.entries):
                    (naming_check.identifiers==1+(collision_variant==19)&&naming_check.dialects==1+(collision_variant==19)&&naming_check.entries==actual_entry_count)),
                  "actual dialect and identifier profiles bind every name with exact generation, allowed flags and available definitions");
              if(naming_profile_variant>=11){
                using KE=catalog::CatalogIdentifierKeyError;
                const std::array key_errors{KE::key_mismatch,KE::key_mismatch,KE::display_mismatch,
                    KE::invalid_quote_mode,KE::key_mismatch,KE::resource_admission_required};
                Check(naming_check.lookup_error==key_errors[naming_profile_variant-11],
                    "actual persisted keys, quoting, display and resource requirements retain their exact semantic refusal");
              }
              Check(runtime_security::VerifyNativeCatalogNamingProfileReferences(read.catalog_capture(),{}).error==PE::workspace_required&&
                  runtime_security::VerifyNativeCatalogNamingProfileReferences(read.catalog_capture(),
                    {aliased_index,fixed_rows.size()}).error==PE::workspace_required,
                  "naming reference checks reject absent and captured-source-overlapping workspace before writes");
              using CE=runtime_security::NativeCatalogCollisionError;
              if(names.ok()&&namespaces.ok()&&filespaces.ok()&&audit_check.ok()&&security_check.ok()&&naming_check.ok()){
                allocation_budget=0;
                const auto collision=runtime_security::VerifyNativeCatalogNamingCollisions(read.catalog_capture(),
                    {name_workspace,namespace_parents,namespace_colors});
                const auto collision_heap=allocation_budget;allocation_budget=-1;
                Check(collision.error==ExpectedCollisionError(collision_variant)&&collision_heap==0&&
                    (collision.ok()?(collision.bodies==1+(collision_variant==21)&&collision.groups==1+(collision_variant==17||collision_variant==21)&&
                     collision.entries==expected_entries&&collision.policy_references==expected_entries):
                    (!collision.bodies&&!collision.groups&&!collision.entries&&!collision.policy_references)),
                    "actual persisted collision policy checks references, scope, profile, language, class and quote lanes; same-object aliases deduplicate");
                Check(runtime_security::VerifyNativeCatalogNamingCollisions(read.catalog_capture(),{}).error==CE::workspace_required&&
                    runtime_security::VerifyNativeCatalogNamingCollisions(read.catalog_capture(),{name_workspace,name_workspace,namespace_colors}).error==CE::workspace_required&&
                    runtime_security::VerifyNativeCatalogNamingCollisions(read.catalog_capture(),{{aliased_index,fixed_rows.size()},namespace_parents,namespace_colors}).error==CE::workspace_required,
                    "collision verification requires bounded independent scratch disjoint from captured source");
              }
              allocation_budget=0;
              const auto failed_metadata=Host::VerifyStartupCatalogMetadata(read);
              allocation_budget=-1;
              Check(failed_metadata==server::NativeHostedRuntimeError::memory_failure&&!read.catalog_metadata().ok(),
                  "engine metadata verification refuses missing allocator backing without losing captured source");
              reads=0;track_reads=true;
              const auto metadata=Host::VerifyStartupCatalogMetadata(read);
              track_reads=false;
              if(!name_variant&&!index_variant&&!namespace_variant&&!filespace_variant&&!audit_variant&&!reference_variant&&!naming_profile_variant&&ExpectedCollisionError(collision_variant)==CE::none){
                Check(metadata==server::NativeHostedRuntimeError::none&&read.catalog_metadata().ok()&&!reads&&
                    read.catalog_metadata().source->transitional_selection_key.checkpoint_sha256==read.catalog_capture().source()->transitional_selection_key.checkpoint_sha256,
                    "native host invokes the engine verifier under its installed memory policy on the retained exact source");
                allocation_budget=0;
                const auto cached_metadata=Host::VerifyStartupCatalogMetadata(read);
                const auto cached_heap=allocation_budget;allocation_budget=-1;
                Check(cached_metadata==server::NativeHostedRuntimeError::none&&cached_heap==0,
                    "source-bound successful metadata verification replays without allocating");
                allocation_budget=0;
                const auto management=runtime_security::ResolveNativeStartupManagementDefinition(read.catalog_capture(),name_workspace);
                const auto management_heap=allocation_budget;allocation_budget=-1;
                Check(management_heap==0&&management.error==ExpectedManagementError(management_variant),
                    "engine resolves real native management descriptor and every exact registered profile without allocation");
                if(management_variant)Check(!management.source&&management.descriptor_version_uuid.is_nil(),
                    "failed management closure exposes no partial selected descriptor");
                else Check(management.descriptor.descriptor_uuid==security_fixture::Id(95020)&&
                    management.profiles[9].code==catalog::CatalogManagementProfileCode::verify_start_conformance&&
                    !management.descriptor_version_uuid.is_nil()&&management.profile_versions[0]!=management.profiles[0].profile_uuid,
                    "management selection retains distinct actual physical versions and registered semantic profiles");
                Check(runtime_security::ResolveNativeStartupManagementDefinition(read.catalog_capture(),{}).error==
                    runtime_security::NativeStartupManagementDefinitionError::workspace_required,
                    "management resolution requires bounded source-disjoint scratch");
                allocation_budget=0;const auto management_no_memory=Host::ResolveStartupManagementDefinition(read);allocation_budget=-1;
                Check(management_no_memory==server::NativeHostedRuntimeError::memory_failure&&!read.management_definition().ok(),
                    "management resolution requires actual installed native allocator backing");
                reads=0;track_reads=true;const auto management_selected=Host::ResolveStartupManagementDefinition(read);track_reads=false;
                Check(!reads&&management_selected==(management_variant?server::NativeHostedRuntimeError::management_definition_failure:
                    server::NativeHostedRuntimeError::none)&&read.management_definition().error==management.error,
                    "native host resolves actual management source with no implicit I/O or fabricated registration");
                if(!management_variant){allocation_budget=0;const auto cached=Host::ResolveStartupManagementDefinition(read);
                  const auto heap=allocation_budget;allocation_budget=-1;Check(cached==server::NativeHostedRuntimeError::none&&heap==0,
                      "source-bound management declaration cache needs no allocation");}
                auto policy_request=VerificationPolicyRequest();
                if(policy_variant==25)policy_request.verification_class=runtime_security::NativeVerificationClass::startup_light;
                allocation_budget=0;
                const auto selected_policies=runtime_security::ResolveNativeVerificationPolicyDeclarations(
                    read.catalog_capture(),policy_request,name_workspace);
                const auto selection_heap=allocation_budget;allocation_budget=-1;
                Check(selection_heap==0&&selected_policies.error==ExpectedVerificationPolicyError(verification_policy_variant),
                    "engine resolves exact native policy bodies and all selected verification/evidence references without allocation");
                if(ExpectedVerificationPolicyError(verification_policy_variant)!=VerificationPolicyError::none)Check(!selected_policies.ok()&&!selected_policies.source&&
                    selected_policies.evidence.body_uuid.is_nil()&&selected_policies.bodies[0].body_uuid.is_nil(),
                    "failed policy reference closure exposes no partially selected body or source");
                else {
                  Check(selected_policies.ok()&&selected_policies.evidence.required_evidence_classes.entries[0]=="verification"&&
                      selected_policies.bodies[0].maximum_scan_pages==4096&&selected_policies.bodies[1].role==catalog::VerificationPolicyRole::quarantine&&
                      selected_policies.bodies[2].role==catalog::VerificationPolicyRole::salvage&&
                      selected_policies.access_descriptor.policy_class==catalog::CatalogPolicyClass::authorization&&
                      selected_policies.access_body.body_uuid==security_fixture::Id(92014)&&selected_policies.access_body.maximum_work==(verification_policy_variant==24?1u:262144u),
                      "source-bound declarations preserve exact persisted rules and borrowed evidence text after native guards retire");
                  auto invalid=policy_request;invalid.repair_policy_uuid=invalid.quarantine_policy_uuid;
                  Check(runtime_security::ResolveNativeVerificationPolicyDeclarations(read.catalog_capture(),invalid,name_workspace).error==VerificationPolicyError::invalid_request,
                      "a single policy cannot stand in for separate verification roles");
                  Check(runtime_security::ResolveNativeVerificationPolicyDeclarations(read.catalog_capture(),policy_request,{}).error==VerificationPolicyError::workspace_required,
                      "policy resolution requires bounded caller backing");
                  auto* alias=reinterpret_cast<std::size_t*>(const_cast<db::NativeCapturedCatalogRow*>(read.catalog_capture().rows().data()));
                  Check(runtime_security::ResolveNativeVerificationPolicyDeclarations(read.catalog_capture(),policy_request,
                      {alias,read.catalog_capture().rows().size()}).error==VerificationPolicyError::workspace_required,
                      "policy selection cannot overwrite captured source rows");
                }
                allocation_budget=0;
                const auto no_policy_memory=policy_variant==26?Host::ResolveStartupVerificationPolicies(read,policy_request):
                    Host::ResolveConfiguredStartupVerificationPolicies(read);
                allocation_budget=-1;
                Check(no_policy_memory==server::NativeHostedRuntimeError::memory_failure&&!read.verification_policies().ok(),
                    "native policy selection requires actual installed allocator backing");
                reads=0;track_reads=true;
                const auto policy_selected=policy_variant==26?Host::ResolveStartupVerificationPolicies(read,policy_request):
                    Host::ResolveConfiguredStartupVerificationPolicies(read);track_reads=false;
                Check(!reads&&policy_selected==(ExpectedVerificationPolicyError(verification_policy_variant)!=VerificationPolicyError::none?server::NativeHostedRuntimeError::verification_policy_failure:server::NativeHostedRuntimeError::none)&&
                    read.verification_policies().error==selected_policies.error,
                    "native host retains exact policy resolution from the same captured checkpoint");
                const auto configured=Host::ResolveConfiguredStartupVerificationPolicies(read);
                Check(configured==(policy_variant==26?server::NativeHostedRuntimeError::verification_policy_failure:policy_selected),
                    "coordinator uses persisted binary references; an earlier manual resolution cannot invent a missing configured selection");
                // SB_OPEN_VERIFICATION_READ_ONLY (owner 2026-11-05): L5's first
                // startup duty, read-only, before any startup transaction.
                if(configured==server::NativeHostedRuntimeError::none){
                  const auto open_budget=(construction.total_pages==2048?32:8*construction.total_pages/512)*512*u64{sizes[profile]};
                  const auto writes=stage_writes,syncs=stage_syncs;
                  const auto opened=Host::VerifyStartupDatabaseFile(read,{{open_budget,64*1024*1024},8*1024*1024});
                  if(!opened.ok())std::cerr<<"fixture open verification="<<int(opened.error)<<" stage="<<int(opened.stage)
                      <<" source="<<int(opened.source_error)<<" profile="<<profile<<'\n';
                  Check(opened.ok()&&read.open_verification()&&stage_writes==writes&&stage_syncs==syncs,
                      "the fixture's database verifies read-only before any startup transaction, with no write or sync");
                }
                const auto attached=Host::AttachStartupServiceSession(read);
                if(ExpectedVerificationPolicyError(verification_policy_variant)!=VerificationPolicyError::none||policy_variant==26){
                  Check(attached.error==SessionError::policy_required&&!read.session(),
                      "missing or invalid configured verification policies cannot attach a startup session");
                }else {
                  const auto session=read.session();
                  Check(attached.ok()&&session&&scratchbird::core::uuid::IsEngineIdentityUuid(session->session_uuid)&&
                      session->session_uuid!=authenticated.observation.decision_uuid&&
                      session->engine_instance_uuid==engine_instance&&
                      scratchbird::core::uuid::IsEngineIdentityUuid(session->attachment_uuid)&&
                      session->attachment_uuid!=session->session_uuid&&session->attachment_uuid!=engine_instance&&
                      session->authentication_decision_uuid==authenticated.observation.decision_uuid&&
                      session->database_uuid==Id(1)&&session->principal_uuid==authenticated.observation.principal_uuid&&
                      session->original_operation_uuid==operation_uuid&&
                      session->clock_policy_uuid==clock.policy_uuid&&session->clock_policy_version_uuid==clock.version_uuid&&
                      scratchbird::storage::database::SameReadSource(
                        session->source,*read.catalog_capture().source()),
                      "private native service session binds actual authentication and complete selected source");
                  allocation_budget=0;const auto cached=Host::AttachStartupServiceSession(read);
                  const auto session_heap=allocation_budget;allocation_budget=-1;
                  Check(cached.ok()&&cached.observation.session_uuid==session->session_uuid&&
                      cached.observation.attachment_uuid==session->attachment_uuid&&cached.observation.engine_instance_uuid==engine_instance&&session_heap==0,
                      "live native session replay retains its original identity without allocation");
                }
                if(ExpectedVerificationPolicyError(verification_policy_variant)==VerificationPolicyError::none){
                  using TPE=runtime_security::NativeTransactionPolicyError;
                  const runtime_security::NativeTransactionPolicyRequest transaction_request{
                    configuration.declarations.pack.policy_pack_uuid,configuration.declarations.pack.generation};
                  allocation_budget=0;
                  const auto transaction=runtime_security::ResolveNativeTransactionPolicyDeclarations(read.catalog_capture(),transaction_request,name_workspace);
                  const auto transaction_heap=allocation_budget;allocation_budget=-1;
                  if(transaction.error!=ExpectedTransactionPolicyError(transaction_policy_variant))
                    std::cerr<<"transaction policy variant="<<transaction_policy_variant<<" error="<<int(transaction.error)<<'\n';
                  Check(transaction_heap==0&&transaction.error==ExpectedTransactionPolicyError(transaction_policy_variant),
                      "actual selected transaction/isolation/lock/evidence closure refuses persisted invalid references without heap");
                  Check(runtime_security::ResolveNativeTransactionPolicyDeclarations(read.catalog_capture(),transaction_request,{}).error==TPE::workspace_required&&
                      runtime_security::ResolveNativeTransactionPolicyDeclarations(read.catalog_capture(),transaction_request,
                        {aliased_index,fixed_rows.size()}).error==TPE::workspace_required,
                      "transaction policy selection refuses missing or source-overlapping workspace");
                  if(ExpectedTransactionPolicyError(transaction_policy_variant)==TPE::none){
                    Check(transaction.ok()&&transaction.descriptor.policy_uuid==security_fixture::Id(98000)&&
                        transaction.body.body_uuid==security_fixture::Id(98010)&&
                        transaction.body.autocommit_mode==(transaction_policy_variant==29?catalog::TransactionAutocommitMode::statement_transaction:
                          catalog::TransactionAutocommitMode::explicit_transaction)&&
                        transaction.default_isolation.profile_kind==catalog::IsolationProfileKind::read_committed_statement&&
                        transaction.startup_isolation.profile_kind==(transaction_policy_variant==21?catalog::IsolationProfileKind::read_committed_statement:
                          catalog::IsolationProfileKind::maintenance_operation_isolation)&&
                        transaction.locks[0].body.body_uuid==security_fixture::Id(98011)&&transaction.locks[1].descriptor.policy_uuid==security_fixture::Id(98001)&&
                        transaction.evidence_descriptor.policy_uuid==security_fixture::Id(92003),
                        "real native selected descriptor resolves complete admission and isolation behavior with exact physical versions");
                    auto stale=transaction_request;++stale.policy_pack_generation;
                    Check(runtime_security::ResolveNativeTransactionPolicyDeclarations(read.catalog_capture(),stale,name_workspace).error==TPE::invalid_pack,
                        "actual pack generation cannot be guessed or substituted");
                  }else Check(!transaction.ok()&&!transaction.source,"a failed declaration closure exposes no selected receipt");
                  allocation_budget=0;const auto no_transaction_memory=Host::ResolveStartupTransactionPolicies(read);allocation_budget=-1;
                  Check(no_transaction_memory==server::NativeHostedRuntimeError::memory_failure&&!read.transaction_policies().ok(),
                      "transaction selection uses actual installed database allocator backing");
                  reads=0;track_reads=true;const auto transaction_selected=Host::ResolveStartupTransactionPolicies(read);track_reads=false;
                  Check(!reads&&transaction_selected==(ExpectedTransactionPolicyError(transaction_policy_variant)!=TPE::none?server::NativeHostedRuntimeError::transaction_policy_failure:
                        server::NativeHostedRuntimeError::none)&&read.transaction_policies().error==transaction.error,
                      "host resolves its own actual configured pack and retains exact source-backed transaction policies");
                  if(ExpectedTransactionPolicyError(transaction_policy_variant)==TPE::none){allocation_budget=0;const auto cached_transaction=Host::ResolveStartupTransactionPolicies(read);
                    const auto cached_heap=allocation_budget;allocation_budget=-1;
                    Check(cached_transaction==server::NativeHostedRuntimeError::none&&cached_heap==0,
                        "immutable transaction policy replay retains the original closure without allocation");
                  }
                  allocation_budget=0;const auto policy_cached=Host::ResolveStartupVerificationPolicies(read,policy_request);
                  const auto policy_heap=allocation_budget;allocation_budget=-1;
                  Check(policy_cached==server::NativeHostedRuntimeError::none&&policy_heap==0,
                      "cached exact policy selection borrows retained source without allocation");
                  auto different=policy_request;different.verification_levels=1;
                  Check(Host::ResolveStartupVerificationPolicies(read,different)==server::NativeHostedRuntimeError::invalid_request&&
                      read.verification_policies().request==policy_request,"cached selection cannot silently change requested verification levels");
                }
              }else Check(metadata==server::NativeHostedRuntimeError::catalog_verification_failure&&!read.catalog_metadata().ok()&&
                  !read.catalog_metadata().source&&!reads&&
                  (name_variant?read.catalog_metadata().names.error==names.error:
                    index_variant?read.catalog_metadata().indexes.error==ExpectedIndexError(index_variant):
                    namespace_variant?read.catalog_metadata().namespaces.error==namespaces.error:
                    filespace_variant?read.catalog_metadata().filespaces.error==filespaces.error:
                    audit_variant?read.catalog_metadata().audit.error==audit_check.error:
                    reference_variant?read.catalog_metadata().security.error==security_check.error:
                    naming_profile_variant?read.catalog_metadata().naming_profiles.error==naming_check.error:
                    read.catalog_metadata().collisions.error==ExpectedCollisionError(collision_variant)),
                  "native host retains the exact engine metadata or audit refusal and no successful source receipt");
              reads=0;track_reads=true;allocation_budget=0;
              const auto replayed=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,capture_limits);
              const auto capture_heap=allocation_budget;allocation_budget=-1;track_reads=false;
              Check(replayed==server::NativeHostedRuntimeError::none&&!reads&&capture_heap==0,
                  "retained catalog capture replays without native I/O or heap allocation");
              Check(Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,{1,capture_limits.maximum_text_bytes,1})==server::NativeHostedRuntimeError::invalid_request&&
                  Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,{capture_limits.maximum_rows,1,1})==server::NativeHostedRuntimeError::invalid_request,
                  "retained catalog capture cannot bypass tighter row or text bounds");
            }
          }
        }
      }
      Check(stage_writes==writes&&stage_syncs==syncs,
          "resource admission and internal service authentication publish no transaction or durable readiness");
    }
    observe_mutexes=false;
    const auto memory_version=configuration.memory_version_uuid;
    const auto memory_configuration_generation=configuration.memory.generation;
    const auto policies_validated=read.agent_policies().ok();
    const auto authenticated_before_move=read.authentication();
    const auto session_before_move=read.session();
    const auto had_catalogs=read.catalogs().ok();
    const auto had_capture=read.catalog_capture().ok();
    const auto had_metadata=read.catalog_metadata().ok();
    const auto had_inventory=read.catalog_capture().inventory().has_value();
    const auto had_verification_policies=read.verification_policies().ok();
    const auto had_management_definition=read.management_definition().ok();
    const auto had_transaction_policies=read.transaction_policies().ok();
    allocation_budget=0;auto moved=std::move(read);const auto move_budget=allocation_budget;allocation_budget=-1;
    Check(move_budget==0&&!read.ok()&&!read.authorization().ok()&&!read.configuration().ok()&&!read.agents().ok()&&
        !read.credentials().selection.ok()&&!read.agent_policies().ok()&&moved.agent_policies().ok()==policies_validated&&
        moved.ok()&&moved.agents().entries().size()==66&&
        AgentPayloadFingerprint(moved.agents())==agent_payload_fingerprint&&moved.configuration().configuration->memory_version_uuid==memory_version,
        "host prerequisite move transfers all governed backing and clears every old borrowed policy/grant view without allocation");
    Check(!read.authentication()&&moved.authentication()==authenticated_before_move,
        "move transfers the one engine decision with its fenced source and removes moved-from access");
    Check(!read.session()&&moved.session().has_value()==session_before_move.has_value()&&
        (!session_before_move||(moved.session()->session_uuid==session_before_move->session_uuid&&
          moved.session()->attachment_uuid==session_before_move->attachment_uuid&&moved.session()->engine_instance_uuid==engine_instance)),
        "native session ownership moves with retained authentication and invalidates moved-from access");
    Check(!read.catalogs().ok()&&moved.catalogs().ok()==had_catalogs,
        "catalog observation moves with the original authenticated owner");
    Check(!read.catalog_capture().ok()&&read.catalog_capture().rows().empty()&&moved.catalog_capture().ok()==had_capture,
        "complete row capture moves with original custody and clears every moved-from view");
    Check(!read.catalog_metadata().ok()&&!read.catalog_metadata().source&&moved.catalog_metadata().ok()==had_metadata,
        "engine metadata result moves with authenticated custody and clears the prior owner's view");
    Check(!read.catalog_capture().inventory()&&moved.catalog_capture().inventory().has_value()==had_inventory,
        "complete selected transaction inventory moves with catalog backing and clears the old owner");
    if(had_inventory)Check(moved.catalog_capture().inventory()->inventory.entries[0].identity.transaction_uuid.value==security_tx.value,
        "actual retained transaction identity survives host move without rereading or allocation");
    Check(!read.verification_policies().ok()&&!read.verification_policies().source&&moved.verification_policies().ok()==had_verification_policies,
        "verification policy declarations move with their capture owner and clear prior borrowed text views");
    Check(!read.management_definition().ok()&&!read.management_definition().source&&
        moved.management_definition().ok()==had_management_definition,
        "management descriptor selection moves with authenticated native source and invalidates the prior owner");
    Check(!read.transaction_policies().ok()&&!read.transaction_policies().source&&moved.transaction_policies().ok()==had_transaction_policies,
        "transaction policy closure transfers with authenticated native ownership and clears borrowed source views");
    if(had_verification_policies)Check(moved.verification_policies().evidence.required_search_keys.entries[0]=="SB-MGA-VERIFY",
        "evidence text still borrows retained immutable catalog after host move");
    if(profile==0&&indexed&&!death)InheritedCredentialRefusals([&](unsigned mode){
      if(mode==0)(void)moved.authentication();
      if(mode==1){auto inherited=std::move(moved);(void)inherited;}
      if(mode==2)(void)read.configuration();
      if(mode==3){auto inherited=std::move(read);(void)inherited;}
    });
    if(death){std::set_terminate([]{::_exit(73);});
      const auto mode=(death-1)%5+1;
      std::thread other([&]{
        if(death==11)(void)moved.authentication();
        if(death==12)(void)Host::AuthenticateStartupService(moved,provisioner.derived);
        if(mode==1)(void)moved.authorization();
        if(mode==2)(void)read.configuration();
        if(mode==3){auto foreign=std::move(moved);(void)foreign;}
        if(mode==4)moved.Reset();
        if(mode==5)(void)Host::RetainStartupObservation(moved);
      });other.join();::_exit(74);
    }
    read.Reset();Check(moved.ok(),"moved-from reset cannot clear actual host read reservation or source custody");
    Check(Host::AuthenticateStartupService(read,provisioner.derived).error==AuthenticationError::not_ready,
        "moved-from owner cannot manufacture or replay a service authentication decision");
    Check(Host::AttachStartupServiceSession(read).error==SessionError::not_ready,
        "moved-from owner cannot attach a native service session");
    if(session_before_move){
      session_stop.request_stop();
      Check(Host::AttachStartupServiceSession(moved).error==SessionError::cancelled&&!moved.session()&&moved.authentication(),
          "cancelled source retires live native session while preserving original authenticated cleanup custody");
    }
    Check(Host::InstallStartupAllocationBehavior(read).error==EnvelopeError::not_ready,
        "moved-from credential owner cannot install allocation behavior");
    Check(Host::ConstrainStartupMemory(read).error==EnvelopeError::not_ready,
        "moved-from credential owner cannot install a database memory constraint");
    Check(Host::RetainStartupObservation(read)==server::NativeHostedRuntimeError::closed,
        "moved-from owner cannot retire or reacquire selected source custody");
    Check(Host::Close(host.runtime).custody.writer.error==db::NativeWriterError::busy,
        "selected credential and actual operation custody block premature writer retirement");
    cleanup_read=&moved;cleanup_host=&host.runtime;cleanup_hook=ProbeCleanupCallback;
    moved.Reset();
    Check(!moved.authentication(),"reset retires engine authentication before borrowed grant or protected-source backing");
    Check(!moved.session(),"reset retires native session before borrowed authorization backing");
    const auto reset_callback_invoked=!cleanup_hook;cleanup_hook=nullptr;
    Check(reset_callback_invoked&&callback_close==server::NativeHostedRuntimeError::busy&&
        callback_retain==server::NativeHostedRuntimeError::busy&&!callback_read_ok,
        "reset invalidates observed access and retains a busy original host through resource cleanup callbacks");
    if(profile==0&&reopen==0) {
      stage_sync_fault=1;
      const auto failed_close=Host::Close(host.runtime);
      const auto receipts=host.runtime->close_receipts();
      Check(failed_close.error==server::NativeHostedRuntimeError::custody_failure&&
          failed_close.custody.writer.error==db::NativeWriterError::io_failure&&
          !failed_close.original_route_released&&receipts.size()==1&&receipts[0].synchronization&&
          receipts[0].synchronization->native_error==EIO&&!receipts[0].data.attempted,
          "real native sync failure retains exact member/native cause, writer, grant and original OS custody for retry");
      const auto still_owned=Host::Open(process,ownership,host_limits,secondary);
      Check(still_owned.error==server::NativeHostedRuntimeError::process_admission_failure&&!still_owned.runtime,
          "failed native close cannot release the actual single-database process admission");
    }
    if(held_memory){
      const db::NativeStorageMemoryBinding binding{Id(1),Id(240),Id(241),Id(242)};
      auto loan=db::AdoptNativeStorageMemory(binding,held_memory);
      Check(loan.ok()&&!held_memory,"actual database grant transfers to a native memory workspace");
      auto backing=loan.memory.CreateArena(binding,64);
      Check(backing.ok(),"actual database grant funds retained native arena bytes");
      auto payload=backing.arena.Allocate(16,alignof(std::uint64_t));
      Check(payload.ok(),"native arena retains a live payload across workspace retirement");
      std::memset(payload.pointer,0x5a,16);
      loan.memory={};
      const auto pending=Host::Close(host.runtime);
      Check(pending.error==server::NativeHostedRuntimeError::memory_envelope_failure&&
          pending.memory_envelope_error==EnvelopeError::busy&&!pending.original_route_released,
          "native close retains original ownership and database constraint while actual memory loans remain");
      Check(Host::Open(process,ownership,host_limits,secondary).error==server::NativeHostedRuntimeError::process_admission_failure,
          "an outstanding memory loan cannot make this database available to a second native host");
      const int probe=::open(path.c_str(),O_RDWR|O_CLOEXEC);
      Check(probe>=0,"independent file descriptor opens for kernel ownership probe");
      const auto lock_result=::flock(probe,LOCK_EX|LOCK_NB);const auto lock_error=errno;
      ::close(probe);
      Check(lock_result==-1&&(lock_error==EWOULDBLOCK||lock_error==EAGAIN),
          "original native file lock remains exclusive while an arena outlives its workspace");
      const auto* bytes=static_cast<const byte*>(payload.pointer);
      Check(std::all_of(bytes,bytes+16,[](byte value){return value==0x5a;}),
          "refused host retirement preserves every live payload byte");
      auto excessive=process.AcquireNativeBootstrapResource({Id(1),Id(240),Id(241),Id(242),
          64*1024*1024+1,server::NativeBootstrapMemoryKind::storage});
      Check(!excessive.ok(),"partial close preserves the actual selected database ceiling");
      backing.arena={};
      const auto premature_process_close=process.CloseAfterUserDrain();
      Check(premature_process_close.error==runtime::RuntimeCryptoPoolError::adapter_failure&&
          premature_process_close.adapter_error==scratchbird::core::hash::CryptoMemoryError::busy,
          "process provider waits for host constraint retirement after the last payload releases");
    }
    const auto closed=Host::Close(host.runtime);
    if(!closed.ok())std::cerr<<"native host close error="<<int(closed.error)<<" custody="<<int(closed.custody.error)
        <<" writer="<<int(closed.custody.writer.error)<<" crypto="<<int(closed.crypto.error)
        <<" adapter="<<int(closed.crypto.adapter_error)<<'\n';
    Check(closed.ok()&&host.runtime->engine_instance_uuid().is_nil(),"real native instance, writer, database provider and original route all retire after selected owners drain");
    Check(Host::Close(host.runtime).ok(),"production native close is idempotent after actual retirement");
    host.runtime.reset();
    if(memory_variant==1||memory_variant==11){
      const int probe=::open(path.c_str(),O_RDWR|O_CLOEXEC);
      Check(probe>=0,"independent file descriptor opens after final native close");
      const auto lock_result=::flock(probe,LOCK_EX|LOCK_NB);::close(probe);
      Check(lock_result==0,"completed native close releases the actual exclusive file lock");
      auto fresh=process.AcquireNativeBootstrapResource({Id(1),Id(240),Id(241),Id(242),
          64*1024*1024+1,server::NativeBootstrapMemoryKind::storage});
      Check(fresh.ok(),"after complete retirement a fresh bootstrap is not bound to the previous host's selected ceiling");
    }
    const auto restored=mem::DefaultMemoryManager().Snapshot();
    Check(restored.current_bytes==manager_baseline.current_bytes&&
        restored.reserved_capacity_bytes==manager_baseline.reserved_capacity_bytes&&
        restored.active_capacity_reservation_count==manager_baseline.active_capacity_reservation_count,
        "full native host returns all actual process-manager allocations and reservation charges before cold reopen");
  }
  Check(provisioner.source->Revoke({Id(231),Id(236),7,{},{}}).ok(),"actual custodian can revoke after both host lifetimes drain");
}
}
// SB_NATIVE_DATABASE_CREATE_PRODUCER (M1 step 2c), host acceptance stage 1:
// a database written by the production producer, opened cold by the
// production host. Its service authenticates, its management history,
// control authority and verification-family history verify, and the engine
// verifies its complete captured catalog, component by component. The calls
// and limits are the variant-0 sequence of EnrolledNativeSource. Management
// definition, policy resolution and the startup transaction are later stages,
// not claimed here.
void ProducerNativeSource(unsigned profile,server::ServerProcessCryptoOwner& process) {
  scratchbird::tests::OwnedTempDirectory artifacts;
  const auto path=(artifacts.path()/"producer-native-node").string();
  const u64 budget=db::NativeHostedSourceBackingBytes(sizes[profile]);
  db::NativeDatabaseCreateRequest create;
  create.policy_seed_pack_root=SB_DEFAULT_POLICY_PACK_ROOT;create.database_name="enrolled_native_node";
  create.database_default_language="en";create.page_size_profile_uuid=Profile(profile);create.page_size_bytes=sizes[profile];
  // As the variant-0 fixture provisions them, history-retaining 8 KiB included.
  create.total_pages=sizes[profile]==8192?2048:512;create.creation_utc_millis=1789357072000ULL;create.engine_uuid=Id(4);
  create.datatype_cohort={{platform::UuidKind::object,scratchbird::core::datatypes::kDatatypeCohortV11},11,11};
  create.bootstrap_principal_name="BootstrapAdmin";create.bootstrap_credential_fingerprint=verifier;
  create.service_principal_name="NodeService";create.service_credential_fingerprint=verifier;
  create.maximum_retained_image_bytes=budget;
  // Bound to the database being created: Id(1), as the fixture's.
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),Id(10)},{{},0,1000});
  disk::FileDevice created;
  Check(created.Open(path,disk::FileOpenMode::create_new).ok(),"owned empty primary for the producer");
  const auto made=db::CreateNativeDatabaseOnOpenDevice(created,create,issuer);
  if(!made.ok())std::cerr<<"producer error="<<int(made.error)<<" policy="<<int(made.policy_error)<<" workspace="
      <<int(made.workspace_error)<<" profile="<<profile<<'\n';
  Check(made.ok()&&created.Close().ok(),"the production producer writes the database and its descriptor retires");
  // SB_OPEN_VERIFICATION_READ_ONLY: nothing between create and the first
  // startup transaction may write the file. Compare every byte.
  const auto read_file=[&]{std::ifstream in(path,std::ios::binary);return std::vector<char>(std::istreambuf_iterator<char>(in),{});};
  const auto created_bytes=read_file();
  Check(!created_bytes.empty(),"the producer's file reads back after create");
  const auto& receipt=*made.receipt;
  Check(receipt.database_uuid==Id(1)&&receipt.workspace.free_pages>0,"the producer leaves free pages for later publication");

  EnrolledCredentialCustodian provisioner(receipt.database_uuid,receipt.service_principal_uuid,
      receipt.service_provider_uuid,receipt.service_credential_reference_uuid);
  const std::size_t credential_base_pages=db::NativeHostedCatalogBasePages(sizes[profile]);
  constexpr auto bound=db::kNativeHostedCatalogRelationMaximumRows;
  const db::NativeCatalogRelationMemoryLimits limits{Profile(profile),1,credential_base_pages,bound,bound,
      db::NativeHostedCatalogNavigationLimits(sizes[profile])};
  const runtime_security::NativeRuntimeAuthorizationLimits graph_limits{bound,128u,1000000u};
  server::DatabaseOwnershipRequest ownership;ownership.database_path=path;ownership.storage_access=disk::RouteStorageAccess::read_write;
  using Host=server::NativeHostedDatabaseRuntime;
  std::vector<std::unique_ptr<disk::FileDevice>> secondary;
  const server::NativeHostedRuntimeLimits host_limits{{64*1024,{budget,32*1024*1024}},limits,2*1024*1024,graph_limits};
  auto host=Host::Open(process,ownership,host_limits,secondary);
  if(!host.ok())std::cerr<<"producer host open error="<<int(host.error)<<'\n';
  Check(host.ok()&&host.runtime->database_uuid()==receipt.database_uuid&&host.runtime->primary_filespace_uuid()==receipt.filespace_uuid,
      "the production host opens the producer's database cold");
  server::NativeLocalServiceStartupCredentialsRequest request;
  request.credential.security.database_uuid=receipt.database_uuid;
  request.credential.security.runtime_binding_uuid=receipt.service_runtime_binding_uuid;
  request.credential.security.runtime_binding_generation=1;request.credential.security.maximum_retained_image_bytes=budget;
  request.credential.security.catalog_selection=runtime_security::NativeRuntimeCatalogSelection::discover_selected;
  request.source={Id(231),Id(235),7,server::StartupCredentialSource::Mutex::Clock::now()+std::chrono::seconds(10),{}};
  request.checkpoint_limits={budget,32*1024*1024};
  auto read=Host::ReadStartupCredentials(host.runtime,request,provisioner.source,provisioner.tag,provisioner.derived);
  if(!read.ok())std::cerr<<"producer credentials error="<<int(read.error)<<" credential="<<int(read.credentials().error)
      <<" configuration="<<int(read.configuration().error)<<" binding="<<int(read.configuration().failing_binding)
      <<" expected="<<read.configuration().expected_generation<<" observed="<<read.configuration().observed_generation
      <<" object="<<scratchbird::core::uuid::UuidToString(read.configuration().failing_object_uuid)
      <<" agents="<<int(read.agents().error)<<" authorization="<<int(read.authorization().error)<<'\n';
  Check(read.ok()&&read.credentials().ok(),"the producer's L5 service credential verifies through selected-root discovery");
  Check(read.agents().ok()&&read.agents().definition_count==66&&read.agents().registration_count==32&&
      read.agents().policy_count==34,"cold startup selects the producer's complete agent configuration");
  const auto retained=Host::RetainStartupObservation(read);
  if(retained!=server::NativeHostedRuntimeError::none)std::cerr<<"producer retain error="<<int(retained)
      <<" policies="<<int(read.agent_policies().error)<<'\n';
  Check(retained==server::NativeHostedRuntimeError::none,"the startup observation and agent policies are retained");
  auto envelope=Host::ConstrainStartupMemory(read);
  if(!envelope.ok())std::cerr<<"producer memory envelope error="<<int(envelope.error)<<" "<<envelope.diagnostic.diagnostic_code<<'\n';
  Check(envelope.ok(),"the producer's memory configuration installs its database envelope");
  const auto behavior=Host::InstallStartupAllocationBehavior(read);
  Check(behavior.ok(),"the selected allocation behavior installs");
  const auto authenticated=Host::AuthenticateStartupService(read,provisioner.derived);
  if(!authenticated.ok())std::cerr<<"producer authentication error="<<int(authenticated.error)<<'\n';
  Check(authenticated.ok()&&authenticated.observation.principal_uuid==receipt.service_principal_uuid,
      "the L5 service authenticates as the producer's principal");
  const db::NativeSelectedCheckpointMemoryLimits catalog_limits{budget,12*1024*1024};
  const db::NativeCatalogRelationMemoryLimits catalog_scan_limits{Profile(profile),1,db::NativeCatalogScanBasePages(sizes[profile]),
      db::kNativeCatalogScanRelationMaximumRows,db::kNativeCatalogScanRelationMaximumRows,
      db::NativeCatalogScanNavigationLimits(sizes[profile])};
  const auto counted=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits);
  if(counted!=server::NativeHostedRuntimeError::none)std::cerr<<"producer catalog scan error="<<int(counted)<<'\n';
  Check(counted==server::NativeHostedRuntimeError::none,"the complete catalog scans within the shared tiers");
  const db::NativeCatalogCaptureLimits capture_limits{4096,4*1024*1024,1,4,1024,128};
  const auto captured=Host::ReadStartupCatalogs(read,catalog_limits,catalog_scan_limits,capture_limits);
  if(captured!=server::NativeHostedRuntimeError::none)std::cerr<<"producer catalog capture error="<<int(captured)<<'\n';
  Check(captured==server::NativeHostedRuntimeError::none,"the complete catalog is captured");
  const auto metadata=Host::VerifyStartupCatalogMetadata(read);
  const auto& m=read.catalog_metadata();
  if(metadata!=server::NativeHostedRuntimeError::none)std::cerr<<"producer catalog metadata error="<<int(metadata)
      <<" names="<<int(m.names.error)<<" indexes="<<int(m.indexes.error)<<" role="<<m.indexes.role
      <<" namespaces="<<int(m.namespaces.error)<<" filespaces="<<int(m.filespaces.error)<<" audit="<<int(m.audit.error)
      <<" security="<<int(m.security.error)<<" naming_profiles="<<int(m.naming_profiles.error)
      <<" collisions="<<int(m.collisions.error)<<'\n';
  Check(metadata==server::NativeHostedRuntimeError::none&&m.ok()&&m.names.ok()&&m.indexes.ok()&&m.indexes.indexed_relations==5&&
      m.namespaces.ok()&&m.filespaces.ok()&&m.audit.ok()&&m.security.ok()&&m.naming_profiles.ok()&&m.collisions.ok(),
      "the engine verifies every component of the producer's catalog: names, indexes, namespaces, filespaces, audit, "
      "security references, naming profiles and collisions");
  // Stage 2: the coordinator's configuration from the producer's own rows.
  const auto management=Host::ResolveStartupManagementDefinition(read);
  if(management!=server::NativeHostedRuntimeError::none)std::cerr<<"producer management error="<<int(management)
      <<" definition="<<int(read.management_definition().error)<<'\n';
  Check(management==server::NativeHostedRuntimeError::none&&read.management_definition().ok(),
      "the management descriptor and every registered profile resolve from the producer's sys.management rows");
  const auto verification_policies=Host::ResolveConfiguredStartupVerificationPolicies(read);
  if(verification_policies!=server::NativeHostedRuntimeError::none)std::cerr<<"producer verification policies error="
      <<int(verification_policies)<<" selection="<<int(read.verification_policies().error)<<'\n';
  Check(verification_policies==server::NativeHostedRuntimeError::none&&read.verification_policies().ok(),
      "the coordinator's persisted startup verification references resolve to complete policy bodies");
  const auto attached=Host::AttachStartupServiceSession(read);
  if(!attached.ok())std::cerr<<"producer session error="<<int(attached.error)<<'\n';
  Check(attached.ok()&&read.session()&&read.session()->database_uuid==receipt.database_uuid&&
      read.session()->principal_uuid==receipt.service_principal_uuid,
      "the L5 service attaches its startup session as the producer's principal");
  const auto transaction_policies=Host::ResolveStartupTransactionPolicies(read);
  if(transaction_policies!=server::NativeHostedRuntimeError::none)std::cerr<<"producer transaction policies error="
      <<int(transaction_policies)<<" selection="<<int(read.transaction_policies().error)<<'\n';
  Check(transaction_policies==server::NativeHostedRuntimeError::none&&read.transaction_policies().ok(),
      "transaction, isolation, lock and evidence policies resolve from the producer's pack rows");

  // Stage 3: open verification (owner direction 2026-11-05), read-only,
  // before any transaction. Startup records nothing (Q6/Q7): there is no
  // startup transaction (SB_DRIFT_NATIVE_STARTUP_TRANSACTION_CEREMONY).
  using OpenError=server::NativeOpenVerificationError;using OpenStage=server::NativeOpenVerificationStage;
  const u64 open_image_budget=(create.total_pages==2048?32:8*create.total_pages/512)*512*u64{sizes[profile]};
  const server::NativeOpenVerificationLimits open_limits{{open_image_budget,64*1024*1024},8*1024*1024};
  {const auto short_map=Host::VerifyStartupDatabaseFile(read,{{open_image_budget,64*1024*1024},0});
   const auto over_policy=Host::VerifyStartupDatabaseFile(read,{{open_image_budget,SIZE_MAX/4},8*1024*1024});
   Check(short_map.error==OpenError::invalid_request&&over_policy.error==OpenError::policy_refused&&
       !read.open_verification()&&read_file()==created_bytes,
       "open verification refuses unusable limits before reading, retains nothing and writes nothing");}
  if(profile==0){
    // Damage one byte of the primary's page zero on disk. Verification must
    // report a finding, retain nothing and write nothing; restoring the byte
    // restores a clean verification.
    const int fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
    Check(fd>=0,"independent descriptor for the page-zero damage probe");
    char original=0;Check(::pread(fd,&original,1,200)==1,"read the page-zero byte to damage");
    const char damaged=char(original^0x5a);
    Check(::pwrite(fd,&damaged,1,200)==1&&::fsync(fd)==0,"damage one page-zero byte");
    auto expected=created_bytes;expected[200]=damaged;
    const auto refused=Host::VerifyStartupDatabaseFile(read,open_limits);
    if(refused.error!=OpenError::finding)std::cerr<<"producer damaged verify="<<int(refused.error)<<" stage="<<int(refused.stage)<<" source="<<int(refused.source_error)<<'\n';
    Check(refused.error==OpenError::finding&&refused.stage==OpenStage::identity&&!refused.observation&&
        !read.open_verification()&&read_file()==expected,
        "a damaged page zero is a finding at the identity stage; verification retains nothing and writes nothing");
    Check(::pwrite(fd,&original,1,200)==1&&::fsync(fd)==0&&::close(fd)==0&&read_file()==created_bytes,
        "restore the damaged page-zero byte");
    // Every read position: an injected I/O error is reported as io_failure,
    // never as a finding, and retains and writes nothing. The sweep ends at
    // the first position past the verification's last read.
    unsigned positions=0,misreported=0;
    for(unsigned fault=1;fault<100000;++fault){
      reads=0;track_reads=true;read_fault=fault;
      const auto faulted=Host::VerifyStartupDatabaseFile(read,open_limits);
      track_reads=false;
      if(read_fault){read_fault=0;Check(faulted.ok(),"the verification completes once no read is faulted");break;}
      ++positions;
      if(faulted.error!=OpenError::io_failure||read.open_verification()){
        if(!misreported++)std::cerr<<"producer read fault "<<fault<<" reported="<<int(faulted.error)<<" stage="<<int(faulted.stage)
            <<" source="<<int(faulted.source_error)<<'\n';
      }
    }
    Check(positions>0&&!misreported&&read_file()==created_bytes,
        "an I/O error at every verification read is reported as io_failure, never a finding, and writes nothing");
  }
  const auto opened=Host::VerifyStartupDatabaseFile(read,open_limits);
  if(!opened.ok())std::cerr<<"producer open verification="<<int(opened.error)<<" stage="<<int(opened.stage)<<'\n';
  Check(opened.ok()&&opened.stage==OpenStage::complete&&read.open_verification()&&
      opened.observation->identity.source.root_set_generation==1&&opened.observation->identity.filespaces==1&&
      opened.observation->identity.system_state.lifecycle==db::NativeSystemLifecycle::creating&&
      opened.observation->allocation.filespaces==1&&opened.observation->allocation.catalog_pages>0&&
      opened.observation->inventory.transactions==1&&opened.observation->inventory.active_transactions==0&&
      opened.observation->inventory.in_doubt_transactions==0&&opened.observation->inventory.horizon_records==3&&
      opened.observation->routes.routes==2,
      "the opened file verifies read-only before any startup transaction: identity, allocation, inventory, catalog routes");
  // Owner ruling 2026-11-06 (Q17): a creator page that no root, catalog page
  // or control page reaches is wasted space, reported and never refused. The
  // creator's original allocation is rebuilt from what the verification and
  // the capture retained; one extra creator page is then added.
  Check(opened.observation->allocation.wasted_pages==0,
      "a production create read through its selected checkpoint leaves no creator page unreachable");
  {
    namespace pg=scratchbird::storage::page;
    const auto& capture=read.catalog_capture();const auto& identity=opened.observation->identity;
    const auto captured=capture.inventory();
    Check(captured&&captured->inventory.entries.size()==1&&!capture.physical_pages().empty(),"the creator and its pages are retained");
    const auto creator=captured->inventory.entries.front().identity;
    std::vector<pg::NativeAllocationRecord> records;
    const auto put=[&](u64 page,const platform::Uuid& page_uuid,u64 generation,platform::u32 type,const platform::Uuid& owner){
      if(std::any_of(records.begin(),records.end(),[&](const auto& r){return r.page_number==page;}))return;
      pg::NativeAllocationRecord r;r.page_number=page;r.allocation_uuid=Id(226);r.page_uuid=page_uuid;r.owner_uuid=owner;
      r.creator_transaction_uuid=creator.transaction_uuid.value;r.creator_local_transaction_id=creator.local_id.value;
      r.page_generation=generation;r.page_type=type;records.push_back(r);};
    put(0,identity.primary_page_uuid,identity.primary_page_generation,1,identity.primary_filespace_uuid);
    for(const auto& p:capture.physical_pages())put(p.header.page_number,p.header.page_uuid,p.header.page_generation,p.header.page_type,p.owner_uuid);
    for(const auto& p:capture.initial_control_pages())put(p.header.page_number,p.header.page_uuid,p.header.page_generation,p.header.page_type,p.owner_uuid);
    for(const auto& r:capture.initial_control_roots())put(r.page.page_number,Id(227),r.page.page_generation,r.page_type,r.object_uuid);
    for(const auto& r:std::span{identity.primary_roots}.first(identity.primary_root_count))
      put(r.page_number,Id(227),r.page_generation,r.page_type,r.object_uuid);
    u64 last=0;for(const auto& r:records)last=std::max(last,r.page_number);
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
    std::vector<pg::NativeAllocationState> states(last+2,pg::NativeAllocationState::free);
    for(const auto& r:records)states[r.page_number]=pg::NativeAllocationState::allocated;
    std::array<db::NativeReadAllocationPage,1> pages{};auto& map=pages[0].map;
    map.header=capture.physical_pages().front().header;map.first_page=0;map.states=states;map.records=records;
    db::NativeReadAllocationChain chain;chain.error=pg::NativeAllocationError::none;chain.pages=pages;
    const auto base=runtime_security::CheckNativeStartupInitialControlAllocation(capture,identity,chain);
    Check(base.ok()&&base.wasted_pages==0&&base.control_pages==opened.observation->allocation.initial_control_pages,
        "the rebuilt original allocation verifies exactly as the file's own did");
    auto with_extra=records;
    pg::NativeAllocationRecord extra=records.back();extra.page_number=last+1;extra.page_uuid=Id(228);extra.owner_uuid=Id(229);extra.page_type=0x302;
    with_extra.push_back(extra);states[last+1]=pg::NativeAllocationState::allocated;map.records=with_extra;
    const auto wasted=runtime_security::CheckNativeStartupInitialControlAllocation(capture,identity,chain);
    Check(wasted.ok()&&wasted.wasted_pages==1&&wasted.control_pages==base.control_pages,
        "a creator page nothing reaches is reported as wasted space, not refused");
    auto moved=with_extra;
    const auto control=capture.initial_control_pages().front().header.page_number;
    for(auto& r:moved)if(r.page_number==control)r.owner_uuid=Id(229);
    map.records=moved;
    Check(runtime_security::CheckNativeStartupInitialControlAllocation(capture,identity,chain).error==
        runtime_security::NativeStartupInitialControlError::invalid_record,
        "a control page with the wrong owner is still refused");
    std::vector<pg::NativeAllocationRecord> missing;
    for(const auto& r:with_extra)if(r.page_number!=control)missing.push_back(r);
    states[control]=pg::NativeAllocationState::free;map.records=missing;
    Check(runtime_security::CheckNativeStartupInitialControlAllocation(capture,identity,chain).error==
        runtime_security::NativeStartupInitialControlError::missing_page,
        "a control page the creator did not allocate is still refused");
  }
  Check(read_file()==created_bytes,"open verification writes nothing: every byte of the producer's file is unchanged");
  Check(read_file()==created_bytes,
      "opening, verifying and resolving startup policies leave every byte of the producer's file unchanged");
  read.Reset();
  Check(Host::Close(host.runtime).ok(),"the producer's database closes");

  // SB_TRANSITIONAL_HOSTED_SOURCE (drift removal C3b): the same startup chain
  // on the same file, through the selected checkpoint and through page zero's
  // fixed roots. Everything it observes must be the same, except what only the
  // checkpoint layer supplies: the source identity and the checkpoint page
  // among the creation control pages.
  if(profile==0||profile==4){
    struct ChainSummary {
      std::size_t definitions=0,registrations=0,policies=0,indexed=0,filespaces=0;
      std::vector<std::array<u64,6>> relations;
      std::vector<std::pair<platform::Uuid,std::string>> rows;
      std::vector<u64> catalog_pages;
      std::vector<std::pair<u64,platform::u32>> control_pages;
      std::vector<std::pair<platform::Uuid,u64>> transactions;
      u64 next_local=0,next_commit=0,horizon_page=0;
      db::NativeReadSourceIdentity source;
      platform::Uuid session_principal;
    };
    // The whole chain to the startup transaction policies; empty when a stage refuses.
    const auto run=[&](server::NativeHostedReadSource mode,bool deny_drift)->std::optional<ChainSummary>{
      auto mode_limits=host_limits;mode_limits.read_source=mode;
      std::vector<std::unique_ptr<disk::FileDevice>> none;
      auto reopened=Host::Open(process,ownership,mode_limits,none);
      Check(reopened.ok(),"the host reopens the producer's database");
      // Adoption still reads the selected checkpoint (until C3d): deny the
      // checkpoint layer's pages only from the first startup read on.
      struct Disarm {~Disarm(){denied_page_count=0;}} disarm;
      if(deny_drift)denied_page_count=denied_page_total;
      std::optional<ChainSummary> out;
      {
        auto mode_request=request;
        mode_request.source.deadline=server::StartupCredentialSource::Mutex::Clock::now()+std::chrono::seconds(30);
        auto r=Host::ReadStartupCredentials(reopened.runtime,mode_request,provisioner.source,provisioner.tag,provisioner.derived);
        const auto chain=[&]{
          return r.ok()&&Host::RetainStartupObservation(r)==server::NativeHostedRuntimeError::none&&
              Host::ConstrainStartupMemory(r).ok()&&Host::InstallStartupAllocationBehavior(r).ok()&&
              Host::AuthenticateStartupService(r,provisioner.derived).ok()&&
              Host::ReadStartupCatalogs(r,catalog_limits,catalog_scan_limits,capture_limits)==server::NativeHostedRuntimeError::none&&
              Host::VerifyStartupCatalogMetadata(r)==server::NativeHostedRuntimeError::none&&
              Host::ResolveStartupManagementDefinition(r)==server::NativeHostedRuntimeError::none&&
              Host::ResolveConfiguredStartupVerificationPolicies(r)==server::NativeHostedRuntimeError::none&&
              Host::AttachStartupServiceSession(r).ok()&&
              Host::ResolveStartupTransactionPolicies(r)==server::NativeHostedRuntimeError::none;
        };
        if(chain()){
          ChainSummary sum;
          sum.definitions=r.agents().definition_count;sum.registrations=r.agents().registration_count;
          sum.policies=r.agents().policy_count;sum.indexed=r.catalog_metadata().indexes.indexed_relations;
          for(std::size_t i=0;i<r.catalogs().relation_count;++i){const auto& x=r.catalogs().relations[i];
            sum.relations.push_back({x.selector,x.role,x.versions,x.visible_rows,x.base_pages,x.navigation_pages});}
          const auto& c=r.catalog_capture();
          for(const auto& row:c.rows())
            sum.rows.push_back({row.row.metadata.record.header.object_uuid.value,std::string(row.row.metadata.record.payload)});
          for(const auto& page:c.physical_pages())sum.catalog_pages.push_back(page.header.page_number);
          for(const auto& page:c.initial_control_pages())sum.control_pages.push_back({page.header.page_number,page.header.page_type});
          for(const auto& root:c.initial_control_roots())if(root.role==2)sum.horizon_page=root.page.page_number;
          const auto inventory=c.inventory();
          Check(inventory.has_value()&&c.source().has_value()&&r.session().has_value(),"the chain retains its capture and session");
          for(const auto& e:inventory->inventory.entries)sum.transactions.push_back({e.identity.transaction_uuid.value,e.identity.local_id.value});
          sum.next_local=inventory->inventory.next_local_transaction_id;sum.next_commit=inventory->inventory.next_commit_sequence;
          sum.filespaces=c.filespaces().size();sum.source=*c.source();sum.session_principal=r.session()->principal_uuid;
          out=std::move(sum);
        }
        r.Reset();
      }
      Check(Host::Close(reopened.runtime).ok(),"the reopened database closes");
      return out;
    };
    // The checkpoint layer's pages: page-zero kinds 9 and 18-21.
    const auto zero=disk::DecodeFilespacePageZero(reinterpret_cast<const platform::byte*>(created_bytes.data()),sizes[profile]);
    Check(zero.ok(),"the producer's page zero decodes");
    std::vector<u64> drift_pages;
    for(const auto& root:zero.record->roots)if(root.kind==9||(root.kind>=18&&root.kind<=21))drift_pages.push_back(root.page_number);
    Check(drift_pages.size()==5,"page zero roots the checkpoint, both selectors and both watermarks");
    const auto selected=run(server::NativeHostedReadSource::selected_checkpoint,false);
    const auto fixed=run(server::NativeHostedReadSource::fixed_root,false);
    Check(selected.has_value()&&fixed.has_value(),"the startup chain completes on both sources");
    Check(selected->source.kind==db::NativeReadSourceKind::selected_checkpoint&&fixed->source.kind==db::NativeReadSourceKind::fixed_root&&
        selected->source.database_uuid==fixed->source.database_uuid&&selected->source.filespace_uuid==fixed->source.filespace_uuid&&
        selected->source.page_size_bytes==fixed->source.page_size_bytes&&!db::SameReadSource(selected->source,fixed->source),
        "both sources are the producer's file, under different source identities");
    Check(selected->definitions==fixed->definitions&&selected->registrations==fixed->registrations&&
        selected->policies==fixed->policies&&selected->indexed==fixed->indexed&&selected->filespaces==fixed->filespaces&&
        selected->relations==fixed->relations&&selected->rows==fixed->rows&&selected->catalog_pages==fixed->catalog_pages&&
        selected->transactions==fixed->transactions&&selected->next_local==fixed->next_local&&
        selected->next_commit==fixed->next_commit&&selected->session_principal==fixed->session_principal,
        "agents, catalog relations, every captured row, catalog pages, inventory and session are the same on both sources");
    auto without_checkpoint=selected->control_pages;
    std::erase_if(without_checkpoint,[](const auto& page){return page.second==0x300;});
    Check(selected->control_pages.size()==without_checkpoint.size()+1&&fixed->control_pages==without_checkpoint,
        "the creation control pages are the same except the checkpoint page, which page zero roots instead");
    // Deny every read of the checkpoint layer's pages and the horizon root.
    Check(selected->horizon_page&&std::find(drift_pages.begin(),drift_pages.end(),selected->horizon_page)==drift_pages.end(),
        "the checkpoint roots the horizon page, which page zero does not");
    drift_pages.push_back(selected->horizon_page);
    struct stat identity{};Check(::stat(path.c_str(),&identity)==0,"the producer's file identity");
    denied_device=identity.st_dev;denied_inode=identity.st_ino;denied_page_total=drift_pages.size();
    for(std::size_t i=0;i<drift_pages.size();++i){
      const auto first=static_cast<off_t>(drift_pages[i]*sizes[profile]);
      denied_pages[i]={first,first+static_cast<off_t>(sizes[profile])};
    }
    denied_page_reads=0;
    const auto fixed_denied=run(server::NativeHostedReadSource::fixed_root,true);
    Check(fixed_denied.has_value()&&denied_page_reads==0&&fixed_denied->rows==fixed->rows&&
        fixed_denied->control_pages==fixed->control_pages,
        "the fixed-root startup chain completes without reading any checkpoint, selector, watermark or horizon page");
    const auto selected_denied=run(server::NativeHostedReadSource::selected_checkpoint,true);
    Check(!selected_denied.has_value()&&denied_page_reads>0,
        "the selected-checkpoint chain does read them, so the denial is real");
    denied_page_total=0;
    Check(read_file()==created_bytes,"neither source's startup chain writes the producer's file");
  }
}

// Fixture shards (owner 2026-11-05: split the fixture into several ctest
// entries so -j runs them side by side). Shard k runs the cases numbered from
// kShardFirstCase[k] up to the next shard's first case; the last shard is
// open-ended, so a case added later still runs. Case numbers stay global, so a
// case keeps its number whichever shard runs it. The one-time checks (the
// affinity children and the pack family comparison) run in shard 0 only.
// Boundaries balance the measured standalone case times; see CMakeLists.txt.
constexpr unsigned kShardFirstCase[]={SB_ENROLLMENT_SHARD_FIRST_CASES};
constexpr unsigned kShardCount=sizeof(kShardFirstCase)/sizeof(kShardFirstCase[0]);
// Affinity children: each is re-executed with its mode number as the argument.
constexpr unsigned kAffinityModes=14;

bool ParseShardNumber(const char* text,unsigned& value){
  const std::string_view view(text);
  const auto parsed=std::from_chars(view.data(),view.data()+view.size(),value);
  return parsed.ec==std::errc{}&&parsed.ptr==view.data()+view.size();
}

int main(int argc,char** argv) {
  // Arguments: none (every case), "producer" (only the production-producer
  // acceptance stage, its own ctest entry so it neither lengthens nor depends
  // on the fixture cases), "shard <k> <count>" (one fixture shard), or an
  // affinity child's positive mode number. Anything else is refused, so a
  // mistyped argument cannot silently run the whole fixture instead.
  const bool producer=argc==2&&std::string_view(argv[1])=="producer";
  const bool sharded=argc==4&&std::string_view(argv[1])=="shard";
  unsigned shard=0,shard_count=0;
  if(sharded&&(!ParseShardNumber(argv[2],shard)||!ParseShardNumber(argv[3],shard_count)||
      shard_count!=kShardCount||shard>=kShardCount)){
    std::cerr<<"native enrolled source FAIL shard arguments must be <k> "<<kShardCount<<" with k below "<<kShardCount<<'\n';
    return 2;
  }
  unsigned death_mode=0;
  if(argc==2&&!producer&&(!ParseShardNumber(argv[1],death_mode)||death_mode==0||death_mode>kAffinityModes)){
    std::cerr<<"native enrolled source FAIL unknown argument "<<argv[1]<<'\n';return 2;
  }
  if(argc!=1&&argc!=2&&!sharded){std::cerr<<"native enrolled source FAIL unknown arguments\n";return 2;}
  const int death=int(death_mode);
  const unsigned first_case=sharded?kShardFirstCase[shard]:1;
  const unsigned end_case=sharded&&shard+1<kShardCount?kShardFirstCase[shard+1]:UINT_MAX;
  const bool once=!sharded||shard==0;
  server::ServerProcessCryptoOwner process;
  ParentOwnedArtifacts artifacts;int outcome=0;
  try {
  if(!death&&!producer&&once)for(unsigned mode=1;mode<=kAffinityModes;++mode){const auto child=::fork();Check(child>=0,"owned native host affinity fixture child");
    if(!child){const auto option=std::to_string(mode);::execl(argv[0],argv[0],option.c_str(),nullptr);::_exit(72);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==73,
        "foreign thread host prerequisite access/move/reset refuses before governed custody");
  }
    server::ServerBootstrapConfig config;config.database_policy_seed_pack_root=SB_DEFAULT_POLICY_PACK_ROOT;
    config.memory_enable_platform_memory_probe=false;
    const auto policy=server::ResolveServerMemoryAllocationPolicy(config);
    Check(policy.ok()&&mem::ConfigureDefaultMemoryManager(policy.policy,config.memory_policy_provenance).ok(),
        "actual selected production memory policy before native host/provider admission");
    Check(process.Start(config,policy).ok(),"production process owner installs before any provider use");
    Check(db::NativeCatalogCaptureWorkspaceBytes({1,1,1,SIZE_MAX})==0&&
        db::NativeCatalogCaptureWorkspaceBytes({1,1,1,1,SIZE_MAX})==0&&
        db::NativeCatalogCaptureWorkspaceBytes({1,1,1,1,1,SIZE_MAX})==0&&
        db::NativeCatalogCaptureWorkspaceBytes({SIZE_MAX,1,1,1})==0&&
        db::NativeCatalogCaptureWorkspaceBytes({1,SIZE_MAX,1,1})==0&&
        db::NativeCatalogCaptureWorkspaceBytes({1,1,SIZE_MAX,1})==0,
        "capture transaction/row/text/directory size arithmetic refuses overflow before allocation");
    const bool timing=std::getenv("SB_NATIVE_ENROLLMENT_TIMING")!=nullptr;
    unsigned case_number=0,cases_run=0;
    const auto run=[&](unsigned profile,bool indexed,server::ServerProcessCryptoOwner& owner,auto... variants){
      const auto number=++case_number;
      if(number<first_case||number>=end_case)return;
      ++cases_run;
      const auto started=std::chrono::steady_clock::now();
      if(timing)std::cerr<<"native_enrollment_case_start="<<number<<" death="<<death<<" profile="<<profile<<" indexed="<<indexed<<'\n';
      EnrolledNativeSource(profile,indexed,owner,variants...);
      if(timing)std::cerr<<"native_enrollment_case_end="<<number<<" milliseconds="<<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()<<'\n';
    };
    Check(process.WithProviderScope([&]{
      if(death){run(0,true,process,0,0,death);return;}
      if(producer){for(unsigned profile=0;profile<5;++profile)ProducerNativeSource(profile,process);return;}
      if(once)PackPolicyFamiliesMatchFixture();
      for(unsigned profile=0;profile<5;++profile)for(bool indexed:{false,true})run(profile,indexed,process);
      for(unsigned variant=1;variant<=10;++variant)run(0,true,process,variant);
      for(unsigned variant=1;variant<=2;++variant)run(0,true,process,0,variant);
      for(unsigned variant=1;variant<=14;++variant)run(0,true,process,0,0,0,variant);
      for(unsigned variant=1;variant<=13;++variant)run(0,true,process,0,0,0,0,variant);
      for(unsigned variant=1;variant<=29;++variant)run(0,true,process,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=3;++variant)run(0,true,process,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=6;++variant)run(0,true,process,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=6;++variant)run(0,true,process,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=9;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=4;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=24;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,0,0,variant);
      for(const unsigned variant:{1u,2u,3u,4u,6u,7u,8u,9u})run(0,true,process,0,0,0,0,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=41;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=5;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=16;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=22;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,variant);
      for(unsigned variant=1;variant<=6;++variant)run(0,true,process,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,variant);
    }).ok(),"actual production provider scope for construction and native hosting");
    if(sharded){
      Check(cases_run>0,"every fixture shard runs at least one case");
      std::cout<<"native enrolled source shard "<<shard<<" of "<<kShardCount<<" ran cases "<<first_case<<'-'
          <<(first_case+cases_run-1)<<" of "<<case_number<<'\n';
    }
  }catch(const std::exception& error){allocation_budget=-1;read_fault=0;track_reads=false;
    native_clock_failure=native_clock_regression=false;native_clock_advance_seconds=0;
    std::cerr<<"native enrolled source FAIL "<<error.what()<<" checks="<<checks<<'\n';outcome=1;}
  Check(process.CloseAfterUserDrain().ok(),"actual production process cleanup follows all native database/credential owners");
  artifacts.Cleanup();
  if(!outcome)std::cout<<"PASS native local-service enrolled source "<<checks<<" checks; runtime activation remains separate\n";
  return outcome;
}

#undef Check
