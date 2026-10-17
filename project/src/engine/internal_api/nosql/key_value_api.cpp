// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "nosql/key_value_api.hpp"
#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "batch_point_lookup.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "observability/dml_summary_counters.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "catalog/name_resolution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/update_api.hpp"
#include "transaction/transaction_api.hpp"
#include "engine/statement_scratch_memory.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {
bool WellFormedUtf8(std::string_view);
bool UnsignedUtf8Less(const std::string&,const std::string&);

EngineApiDiagnostic KvError(const char* code,std::string detail) {
  return MakeEngineApiDiagnostic(code,"engine.nosql.key_value.refused",std::move(detail),true);
}
bool KvCancelled(const EngineRequestContext& context) noexcept {
  try {return context.query_cancellation_requested&&context.query_cancellation_requested();}
  catch (...) {return true;}
}
struct KvFailure {
  std::vector<EngineApiDiagnostic> diagnostics;
  explicit KvFailure(EngineApiDiagnostic diagnostic):diagnostics{std::move(diagnostic)}{}
  explicit KvFailure(const std::vector<EngineApiDiagnostic>& source):diagnostics(source){}
};
void KvRequire(bool condition,const char* code,const char* detail) {
  if(!condition)throw KvFailure{KvError(code,detail)};
}
void KvPoll(const EngineRequestContext& context) {
  KvRequire(!KvCancelled(context),"PROCESS.CANCELLED","key_value_cancelled_before_publication");
}
// This limits real allocations from the live receipt's physical memory owner;
// it is not a charged but unused reservation or an ungoverned map/cache.
class KvMemory final:public std::pmr::memory_resource {
 public:
  KvMemory(std::pmr::memory_resource* upstream,std::uint64_t limit):upstream_(upstream),limit_(limit){}
  std::uint64_t used() const noexcept {return used_;}
  void Restrict(std::uint64_t limit) {
    if(used_>limit)throw std::bad_alloc();
    limit_=std::min(limit_,limit);
  }
 private:
  void* do_allocate(std::size_t n,std::size_t alignment) override {
    if(n>limit_-used_)throw std::bad_alloc();
    void* p=upstream_->allocate(n,alignment);used_+=n;return p;
  }
  void do_deallocate(void* p,std::size_t n,std::size_t alignment) override {
    upstream_->deallocate(p,n,alignment);used_-=n;
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {return this==&other;}
  std::pmr::memory_resource* upstream_;std::uint64_t limit_,used_=0;
};
struct KvRecord {
  std::pmr::string value;
  std::size_t identity_index;
  EngineApiU64 expiry=0;
  KvRecord(std::pmr::memory_resource* memory,std::size_t index,std::string_view bytes,EngineApiU64 ttl)
      :value(bytes,memory),identity_index(index),expiry(ttl){}
};
struct KvWrite {
  std::size_t identity_index;std::pmr::string key,value;EngineApiU64 expiry;
  KvWrite(std::pmr::memory_resource* memory,std::size_t index,std::string_view k,std::string_view v,EngineApiU64 e)
      :identity_index(index),key(k,memory),value(v,memory),expiry(e){}
};
struct KvOutput {
  std::size_t identity_index;std::pmr::string key,value;EngineApiU64 expiry;
  bool duplicate=false;EngineApiU64 duplicate_ordinal=0;
  KvOutput(std::pmr::memory_resource* memory,std::size_t index,std::string_view k,std::string_view v,
      EngineApiU64 e,bool d,EngineApiU64 ordinal)
      :identity_index(index),key(k,memory),value(v,memory),expiry(e),duplicate(d),duplicate_ordinal(ordinal){}
};
struct KvPlan {
  std::pmr::memory_resource* memory;
  std::pmr::vector<EngineUuid> identities;
  std::pmr::vector<KvWrite> writes;
  std::pmr::vector<KvOutput> outputs;
  std::uint64_t retained_bytes=0;
  explicit KvPlan(std::pmr::memory_resource* source):memory(source),identities(source),writes(source),outputs(source){}
  void Charge(std::uint64_t n,const EngineKeyValueResourceLimits& limits) {
    KvRequire(n<=limits.maximum_decoded_bytes-retained_bytes,"RESOURCE.BUDGET_EXCEEDED",
              "key_value_staging_bytes_exceeded");
    retained_bytes+=n;
  }
  void Output(std::string_view key,const KvRecord& row,const EngineKeyValueResourceLimits& limits,
      bool duplicate=false,EngineApiU64 ordinal=0) {
    KvRequire(outputs.size()<limits.maximum_output_rows,"RESOURCE.BUDGET_EXCEEDED",
              "key_value_output_rows_exceeded");
    Charge(sizeof(KvOutput)+key.size()+row.value.size(),limits);
    outputs.emplace_back(memory,row.identity_index,key,row.value,row.expiry,duplicate,ordinal);
  }
  void Write(std::string_view key,const KvRecord& row,const EngineKeyValueResourceLimits& limits) {
    Charge(sizeof(KvWrite)+key.size()+row.value.size(),limits);
    writes.emplace_back(memory,row.identity_index,key,row.value,row.expiry);
  }
};
using KvRows=std::pmr::map<std::string_view,KvRecord,std::less<>>;
struct KvCollection {
  MgaVisibleHeapRelationReadResult read;
};
bool KvVisible(const KvRecord& row,EngineApiU64 reader) {return !row.expiry||reader<row.expiry;}

template<class Request>
void KvValidateInputBounds(const Request& request) {
  const auto& limits=request.resource_limits;
  std::uint64_t bytes=0;
  const auto operand=[&](std::string_view value) {
    KvRequire(value.size()<=limits.maximum_decoded_bytes-bytes,
        "RESOURCE.BUDGET_EXCEEDED","key_value_operand_bytes_exceeded");
    bytes+=value.size();
  };
  std::size_t operations=1;
  if constexpr(std::is_same_v<Request,EngineKeyValueGetRequest>)
    operand(request.prefix_scan||!request.prefix.empty()?request.prefix:request.key);
  else if constexpr(std::is_same_v<Request,EngineKeyValuePutRequest>) {
    operand(request.key);operand(request.value);
  } else if constexpr(std::is_same_v<Request,EngineKeyValueMultiGetRequest>) {
    operations=request.keys.size();
    KvRequire(operations<=limits.maximum_operations,"RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_exceeded");
    for(const auto& key:request.keys)operand(key);
  } else if constexpr(std::is_same_v<Request,EngineKeyValuePipelineRequest>) {
    KvRequire(request.puts.size()<=std::numeric_limits<std::size_t>::max()-request.get_keys.size(),
        "RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_overflow");
    operations=request.puts.size()+request.get_keys.size();
    KvRequire(operations<=limits.maximum_operations,"RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_exceeded");
    KvRequire(!request.max_admitted_operations||operations<=request.max_admitted_operations,
        kKeyValuePipelineAdmissionRefused,"key_value_pipeline_batch_limit_exceeded");
    for(const auto& put:request.puts){operand(put.key);operand(put.value);}
    for(const auto& key:request.get_keys)operand(key);
  } else if constexpr(std::is_same_v<Request,EngineKeyValueAtomicProgramRequest>) {
    operations=request.steps.size();
    KvRequire(operations<=limits.maximum_operations,"RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_exceeded");
    for(const auto& step:request.steps){operand(step.key);operand(step.operand);}
  }
  KvRequire(operations<=limits.maximum_operations,"RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_exceeded");
}

KvCollection LoadKvCollection(const EngineKeyValueRequest& request,const EngineRequestContext& context,
    const char* operation,bool mutation) {
  const auto& limits=request.resource_limits;
  KvPoll(context);
  KvPoll(request.context);
  KvRequire(core::uuid::IsEngineIdentityUuid(request.target_object.uuid),"SB_ENGINE_API_INVALID_REQUEST",
            "key_value_collection_identity_required");
  KvRequire(limits.maximum_operations&&limits.maximum_scanned_row_versions&&
      limits.maximum_decoded_bytes&&limits.maximum_output_rows&&limits.maximum_memory_bytes,
      "RESOURCE.BUDGET_EXCEEDED","key_value_resource_limits_required");
  for(const char* right:{"SELECT","INSERT","UPDATE"}) {
    if(!mutation&&std::string_view(right)!="SELECT")continue;
    const auto authorized=EvaluateMaterializedAuthorization(context,context.authorization_context,
        right,request.target_object.uuid);
    KvRequire(authorized.authorized&&!authorized.denied&&!authorized.policy_recheck_required&&
        authorized.diagnostics.empty(),"SECURITY.ACCESS_DENIED","key_value_collection_right_required");
  }
  const auto descriptor=LoadMgaRelationStorageDescriptor(context,request.target_object.uuid);
  if(!descriptor.ok)throw KvFailure{descriptor.diagnostic};
  const auto& d=descriptor.descriptor;
  KvRequire(d.database_uuid==context.database_uuid&&d.relation_uuid==request.target_object.uuid&&
      d.relation_kind=="table"&&d.storage_profile=="local_mga_rowstore_v1"&&d.columns.size()==3&&
      !ValidateMgaRelationStorageDescriptor(d).error,"DATATYPE.DESCRIPTOR.INVALID",
      "key_value_collection_storage_shape_invalid");
  constexpr std::array<std::string_view,3> names{"key","value","expires_after_local_transaction_id"};
  constexpr std::array<std::string_view,3> types{"text","text","uint64"};
  // Resolve the pair from one actual admitted catalog image. This avoids a
  // second complete node/resource read and cannot mix resource generations.
  const auto resources=LookupEngineTextResourcePair(context,d.columns[0].charset_uuid,d.columns[0].collation_uuid);
  const auto& charset=resources.charset;
  const auto& collation=resources.collation;
  KvRequire(resources.ok&&charset.present&&collation.present&&
      charset.canonical_name=="UTF-8"&&collation.canonical_name=="SB_UTF8_BINARY"&&
      collation.parent_resource_uuid==charset.resource_uuid,
      "DATATYPE.DESCRIPTOR.INVALID","key_value_utf8_binary_profile_unavailable");
  for(std::size_t i=0;i<3;++i) {
    const auto& c=d.columns[i];const auto& value=c.value_descriptor;
    const auto identity=core::datatypes::LookupDatatypeTypeCodecIdentityV3(
        context.datatype_catalog_snapshot_uuid,context.datatype_catalog_generation,
        context.datatype_registry_generation,value.datatype_descriptor_uuid,value.datatype_descriptor_generation);
    const auto expected=i<2?core::datatypes::CanonicalTypeId::character:core::datatypes::CanonicalTypeId::uint64;
    KvRequire(c.ordinal==i&&c.canonical_name_key==names[i]&&!c.nullable&&!c.generated&&!c.identity_column&&
        value.canonical_type_name==types[i]&&QowCanonicalDescriptorIdentityV1(value)&&identity.ok&&
        identity.row.legacy_fields.type_uuid==value.type_uuid&&
        identity.row.legacy_fields.canonical_binary_type_code==static_cast<std::uint32_t>(expected),
        "DATATYPE.DESCRIPTOR.INVALID","key_value_column_descriptor_invalid");
    if(i<2)KvRequire(c.charset_uuid==charset.resource_uuid&&
        c.collation_uuid==collation.resource_uuid,
        "DATATYPE.DESCRIPTOR.INVALID","key_value_column_requires_utf8_binary_profile");
  }
  // Inventory ordering is not a uniqueness constraint across independently
  // admitted MGA snapshots. The storage writer must have real exact-key
  // support, including conflicts with non-KV DML and other transactions.
  KvRequire(std::any_of(d.indexes.begin(),d.indexes.end(),[](const auto& index){
      return index.unique&&!index.approximate&&index.key_envelopes.size()==1&&index.key_envelopes.front()=="key"&&
          index.predicate_kind.empty();
    }),"CLI.SUPPORT_STRUCTURE_UNAVAILABLE","key_value_unique_key_support_required");
  MgaVisibleHeapRelationReadRequest read;
  read.relation_uuid=request.target_object.uuid;
  read.maximum_scanned_row_versions=limits.maximum_scanned_row_versions;
  read.maximum_decoded_bytes=limits.maximum_decoded_bytes;
  // Input materialization is bounded by the scanned-version and memory caps.
  // The public output cap applies to selected/duplicate result rows, not to
  // unrelated rows that must be examined to establish exact key equality.
  read.maximum_output_rows=limits.maximum_scanned_row_versions;
  read.maximum_memory_bytes=limits.maximum_memory_bytes;
  read.cancellation_requested=[&]{return KvCancelled(context)||KvCancelled(request.context);};
  KvCollection collection{ReadVisibleMgaHeapRelation(context,read)};
  if(!collection.read.ok)throw KvFailure{collection.read.diagnostic};
  KvRequire(collection.read.descriptor==d,"DATATYPE.DESCRIPTOR.INVALID","key_value_descriptor_changed_during_read");
  return collection;
}

template<class Callback>
KvPlan PlanKv(const EngineKeyValueRequest& request,const EngineRequestContext& context,
    const KvCollection& collection,KvMemory& memory,Callback&& callback) {
  KvPlan plan(&memory);
      KvRows rows(&memory);
      for(const auto& row:collection.read.visible_rows) {
        KvPoll(context);
        KvPoll(request.context);
        KvRequire(row.values.size()==3&&core::uuid::IsEngineIdentityUuid(row.row_uuid),
                  "DATATYPE.DESCRIPTOR.INVALID","key_value_stored_row_shape_invalid");
        std::array<const CrudStoredValue*,3> fields{};
        for(const auto& [name,value]:row.values) {
          std::size_t at=name=="key"?0:name=="value"?1:name=="expires_after_local_transaction_id"?2:3;
          KvRequire(at<3&&!fields[at]&&value.isPresent(),"DATATYPE.DESCRIPTOR.INVALID","key_value_stored_field_invalid");
          fields[at]=&value;
        }
        KvRequire(fields[0]&&fields[1]&&fields[2]&&fields[2]->bytes.size()==8&&
            WellFormedUtf8(fields[0]->bytes)&&WellFormedUtf8(fields[1]->bytes),
            "DATATYPE.DESCRIPTOR.INVALID","key_value_stored_native_value_invalid");
        EngineApiU64 expiry=0;for(unsigned i=0;i<8;++i)
          expiry|=static_cast<EngineApiU64>(static_cast<unsigned char>(fields[2]->bytes[i]))<<(i*8);
        const auto index=plan.identities.size();plan.Charge(sizeof(EngineUuid),request.resource_limits);
        plan.identities.push_back(row.row_uuid);
        KvRequire(rows.try_emplace(fields[0]->bytes,&memory,index,fields[1]->bytes,expiry).second,
            "SB_MODEL_KEY_VALUE_DUPLICATE_VISIBLE_KEY_REFUSED_V1","key_value_duplicate_visible_key");
      }
      callback(plan,rows,memory);
      KvPoll(context);
      KvPoll(request.context);
  return plan;
}
KvRecord& KvUpsertPlan(KvPlan& plan,KvRows& rows,std::pmr::memory_resource& memory,
    const EngineKeyValueRequest& request,std::string_view key,std::string_view value,EngineApiU64 expiry) {
  KvRequire(key.size()<=request.resource_limits.maximum_decoded_bytes&&
      value.size()<=request.resource_limits.maximum_decoded_bytes,"RESOURCE.BUDGET_EXCEEDED","key_value_operand_bytes_exceeded");
  KvRequire(WellFormedUtf8(key)&&WellFormedUtf8(value),"SB_MODEL_KEY_VALUE_TEXT_INVALID_V1","key_value_malformed_utf8");
  auto found=rows.find(key);
  if(found==rows.end()) {
    plan.Charge(sizeof(EngineUuid),request.resource_limits);
    const auto index=plan.identities.size();plan.identities.push_back({});
    found=rows.try_emplace(key,&memory,index,std::string_view{},0).first;
  }
  found->second.value.assign(value);found->second.expiry=expiry;
  plan.Write(key,found->second,request.resource_limits);return found->second;
}
void KvBatchRead(KvPlan& plan,const KvRows& rows,const EngineKeyValueRequest& request,
    const std::vector<std::string>& keys,EngineApiResult& result) {
  for(const auto& key:keys) {
    KvRequire(key.size()<=request.resource_limits.maximum_decoded_bytes,
              "RESOURCE.BUDGET_EXCEEDED","key_value_operand_bytes_exceeded");
    KvRequire(WellFormedUtf8(key),"SB_MODEL_KEY_VALUE_TEXT_INVALID_V1","key_value_malformed_utf8");
  }
  core::index::CandidateSetAuthorityContext authority;
  // Observations of THIS completed engine-native read, not supplied proof flags.
  authority.engine_mga_authoritative=true;authority.security_context_bound=true;
  authority.row_mga_recheck_required=true;authority.row_security_recheck_required=true;
  authority.exact_recheck_available=true;authority.exact_rerank_source_available=true;
  struct Source {KvPlan& plan;const KvRows& rows;const EngineKeyValueRequest& request;};
  Source source{plan,rows,request};
  const auto lookup=core::index::RunBoundedUniqueBatchPointLookupV1(
      keys,authority,plan.memory,&source,[](void* raw,std::string_view key){
    auto& actual=*static_cast<Source*>(raw);
    KvPoll(actual.request.context);
    core::index::BoundedUniqueBatchValueV1 row;
    const auto found=actual.rows.find(key);
    if(found==actual.rows.end()||!KvVisible(found->second,actual.request.context.local_transaction_id))return row;
    const auto identity=core::uuid::MakeDurableEngineIdentityUuid(core::platform::UuidKind::row,
        actual.plan.identities[found->second.identity_index]);
    KvRequire(identity.ok(),"DATATYPE.DESCRIPTOR.INVALID","key_value_stored_identity_unavailable");
    row.found=true;row.row_uuid=identity.value;row.payload=found->second.value;
    row.exact_match=true;row.mga_visible=true;row.security_authorized=true;
    return row;
  });
  if(!lookup.ok())throw KvFailure{KvError("SB_ENGINE_API_INVALID_REQUEST",lookup.diagnostic.diagnostic_code)};
  AddApiBehaviorEvidence(&result,"batch_point_lookup","batch_point_lookup.purpose=key_value");
  AddApiBehaviorEvidence(&result,"batch_point_lookup","batch_point_lookup.input_key_count="+std::to_string(keys.size()));
  AddApiBehaviorEvidence(&result,"batch_point_lookup","batch_point_lookup.unique_key_count="+std::to_string(lookup.unique_key_count));
  AddApiBehaviorEvidence(&result,"batch_point_lookup","batch_point_lookup.miss_count="+std::to_string(lookup.misses.size()));
  for(const auto& miss:lookup.misses) {
    plan.Charge(sizeof(EngineEvidenceReference)+miss.encoded_key.size()+64,request.resource_limits);
    AddApiBehaviorEvidence(&result,"batch_point_lookup_miss",
        std::to_string(miss.input_ordinal)+":"+std::string(miss.encoded_key)+":key_not_found");
  }
  for(const auto& row:lookup.rows)plan.Output(row.encoded_key,rows.find(row.encoded_key)->second,
      request.resource_limits,row.duplicate_key,row.duplicate_ordinal);
  AddApiBehaviorEvidence(&result,"kv_ordered_batch_lookup_primitive","ODF-092");
}

EngineTypedValue KvNativeValue(const EngineDescriptor& descriptor,std::string_view bytes) {
  EngineTypedValue value;value.descriptor=descriptor;value.encoded_value=bytes;return value;
}
EngineTypedValue KvNativeExpiry(const EngineDescriptor& descriptor,EngineApiU64 number) {
  EngineTypedValue value;value.descriptor=descriptor;
  for(unsigned i=0;i<8;++i)value.binary_value.push_back(static_cast<std::uint8_t>(number>>(i*8)));
  return value;
}
void ApplyKvWrites(const EngineKeyValueRequest& request,const EngineRequestContext& context,
    const KvCollection& collection,KvPlan& plan,EngineApiResult& result) {
  const auto& columns=collection.read.descriptor.columns;
  for(const auto& write:plan.writes) {
    KvPoll(context);
    KvPoll(request.context);
    auto& identity=plan.identities[write.identity_index];
    EngineRowValue row;
    row.fields={{"key",KvNativeValue(columns[0].value_descriptor,write.key)},
                {"value",KvNativeValue(columns[1].value_descriptor,write.value)},
                {"expires_after_local_transaction_id",KvNativeExpiry(columns[2].value_descriptor,write.expiry)}};
    if(identity.is_nil()) {
      EngineInsertRowsRequest insert;
      insert.context=context;insert.target_table=request.target_object;
      insert.input_rows.push_back(std::move(row));
      const auto written=EngineInsertRows(insert);
      AddDmlSummaryCounters(&result.dml_summary,written.dml_summary);
      if(!written.ok) {
        if(written.diagnostics.empty())throw KvFailure{KvError("SBLR.EXECUTION_FAILED","key_value_insert_failed")};
        throw KvFailure{written.diagnostics};
      }
      KvRequire(written.row_uuids.size()==1&&written.inserted_count==1&&
          core::uuid::IsEngineIdentityUuid(written.row_uuids.front()),
          "SBLR.EXECUTION_FAILED","key_value_insert_identity_missing");
      identity=written.row_uuids.front();
    } else {
      EngineUpdateRowsRequest update;
      update.context=context;
      update.target_table=request.target_object;update.update_predicate.predicate_kind="row_uuid_match";
      update.update_predicate.row_uuid=identity;update.assignments=std::move(row.fields);
      const auto written=EngineUpdateRows(update);
      AddDmlSummaryCounters(&result.dml_summary,written.dml_summary);
      if(!written.ok) {
        if(written.diagnostics.empty())throw KvFailure{KvError("SBLR.EXECUTION_FAILED","key_value_update_failed")};
        throw KvFailure{written.diagnostics};
      }
      // Native DML reports a validated equal-image update as matched without
      // manufacturing a row version. Only a missing/multiple target is stale.
      KvRequire(written.matched_count==1&&written.updated_count<=1,
          "MGA.TRANSACTION.STALE","key_value_update_target_changed");
    }
  }
}
void PublishKvRows(const EngineKeyValueRequest& request,const KvCollection& collection,
    const KvPlan& plan,EngineApiResult& result) {
  const auto& expiry=collection.read.descriptor.columns[2].value_descriptor;
  for(const auto& row:plan.outputs) {
    KvRequire(core::uuid::IsEngineIdentityUuid(plan.identities[row.identity_index]),
        "DATATYPE.DESCRIPTOR.INVALID","key_value_result_identity_missing");
    EngineRowValue output;
    // The result projects an actual stored row, not a new synthetic row. Apart
    // from preserving source identity this avoids a UUID allocation per hit.
    output.requested_row_uuid=plan.identities[row.identity_index];
    output.fields={{"surface",ApiBehaviorValue("key_value")},
        {"key_uuid",ApiBehaviorValue(output.requested_row_uuid)},
        {"row_uuid",ApiBehaviorValue(output.requested_row_uuid)},
        {"key",KvNativeValue(collection.read.descriptor.columns[0].value_descriptor,row.key)},
        {"value",KvNativeValue(collection.read.descriptor.columns[1].value_descriptor,row.value)},
        {"ttl_expires_after_tx",KvNativeExpiry(expiry,row.expiry)},
        {"state",ApiBehaviorValue(row.expiry&&result.local_transaction_id>=row.expiry?"expired":"active")},
        {"duplicate_key",ApiBehaviorBooleanValue(row.duplicate)},
        {"duplicate_ordinal",ApiBehaviorUnsignedValue(row.duplicate_ordinal)}};
    result.result_shape.rows.push_back(std::move(output));
    result.result_shape.result_kind="api_behavior_rows";
  }
  result.primary_object=request.target_object;
  result.dml_summary.visible_rows_scanned+=collection.read.scanned_row_version_count;
  AddEngineNoSqlSurfaceEvidence(&result,"key_value","native_mga_collection");
  AddApiBehaviorEvidence(&result,"kv_physical_access","bounded_native_mga_heap_exact_recheck");
  AddApiBehaviorEvidence(&result,"ttl_visibility_evidence","deterministic_local_transaction_id");
  AddApiBehaviorEvidence(&result,"mga_finality_authority","engine_transaction_inventory");
  AddApiBehaviorEvidence(&result,"client_autocommit_authority","false");
}

// Failure recovery runs before formatting a public diagnostic can allocate.
// The destructor also covers unexpected exception paths after marker creation.
// A failed/uncertain recovery is not retried as a fresh storage operation.
struct KvSavepointOwner {
  const EngineRequestContext& context;
  const std::string& name;
  bool active=false,attempted=false,rolled_back=false,recovery_exception=false;
  std::optional<EngineApiDiagnostic> recovery_diagnostic;
  void Recover() noexcept {
    if(!active||attempted)return;
    attempted=true;
    try {
      auto diagnostic=RollbackToMgaSavepointMarker(context,name);
      if(diagnostic.error) {recovery_diagnostic.emplace(std::move(diagnostic));return;}
      rolled_back=true;
      diagnostic=ReleaseMgaSavepointMarker(context,name);
      if(diagnostic.error)recovery_diagnostic.emplace(std::move(diagnostic));
      else active=false;
    } catch(...) {recovery_exception=true;}
  }
  ~KvSavepointOwner() noexcept {Recover();}
};

template<class Result,class Request,class Planner>
Result RunKv(const Request& request,const char* operation,bool mutation,Planner&& planner) {
  // Legacy proof booleans grant nothing. An explicit non-local scope remains
  // a routing demand, however; it must never silently select this local path.
  if constexpr(requires {request.physical_proof.provider_contract.scope;}) {
    const auto scope=request.physical_proof.provider_contract.scope;
    if(scope==EngineNoSqlProviderScope::kClusterOnly||scope==EngineNoSqlProviderScope::kDistributed)
      return EngineNoSqlClusterAuthorityUnavailable<Result>(request,operation);
    if(scope!=EngineNoSqlProviderScope::kLocal)
      return MakeApiBehaviorDiagnostic<Result>(request.context,operation,
          KvError("SB_ENGINE_API_INVALID_REQUEST","key_value_provider_scope_invalid"));
  }
  if(EngineNoSqlRequiresClusterAuthority(request)||request.context.cluster_transaction_active||
      request.context.route_fence_present)
    return EngineNoSqlClusterAuthorityUnavailable<Result>(request,operation);
  auto result=MakeApiBehaviorSuccess<Result>(request.context,operation);
  std::optional<KvFailure> execution_failure;
  std::optional<EngineApiDiagnostic> recovery_diagnostic;
  bool succeeded=false,recovery_attempted=false,rolled_back=false,recovery_required=false;
  enum class ExceptionKind {none,memory,execution};
  auto exception_kind=ExceptionKind::none;
  core::platform::DiagnosticRecord admission_cause;
  sb_engine_status_t admitted=SB_ENGINE_STATUS_OK;
  try {
    const auto& limits=request.resource_limits;
    KvRequire(limits.maximum_operations&&limits.maximum_scanned_row_versions&&
        limits.maximum_decoded_bytes&&limits.maximum_output_rows&&limits.maximum_memory_bytes,
        "RESOURCE.BUDGET_EXCEEDED","key_value_resource_limits_required");
    KvValidateInputBounds(request);
    KvPoll(request.context);
    // Actual session/receipt ownership precedes inventory ordering, matching
    // public dispatch. Direct API callers must never invert those locks.
    admitted=RunWithStatementOperationMemoryV1(request.context,limits.maximum_memory_bytes,
        [&](StatementScratchMemoryV1& scratch){
      // Security/visibility/policy come from the privately issued immutable
      // receipt, never replaced grant arrays in a copied request projection.
      const auto& actual=scratch.context();
      const auto ordering=AcquireTransactionInventoryGuard(actual.database_path);
      result.transaction_uuid=scratch.context().transaction_uuid;
      result.local_transaction_id=scratch.context().local_transaction_id;
      std::string savepoint;
      KvSavepointOwner savepoint_owner{actual,savepoint};
      try {
        if(actual.cluster_transaction_active||actual.route_fence_present) {
          result.cluster_authority_required=true;
          throw KvFailure{MakeClusterAuthorityUnavailableDiagnostic(operation)};
        }
        if(mutation) {
          const auto pending=ValidateMgaStatementSavepointFinality(actual);
          if(pending.error)throw KvFailure{pending};
        }
        auto collection=LoadKvCollection(request,actual,operation,mutation);
        KvRequire(collection.read.current_live_memory_bytes<limits.maximum_memory_bytes,
            "RESOURCE.BUDGET_EXCEEDED","key_value_source_memory_budget_exceeded");
        KvMemory memory(scratch.memory(),std::min(scratch.limit_bytes(),
            limits.maximum_memory_bytes-collection.read.current_live_memory_bytes));
        {
          auto plan=PlanKv(request,actual,collection,memory,[&](auto& p,auto& rows,auto& work){
            planner(p,rows,work,result);
          });
          if(mutation) {
            const auto identity=GenerateCrudEngineUuid("object");
            savepoint=MgaSavepointUuidKey(identity);
            AddApiBehaviorEvidence(&result,"kv_statement_savepoint_uuid",identity);
            auto created=CreateMgaStatementSavepointMarker(actual,identity);
            if(created.error)throw KvFailure{created};
            savepoint_owner.active=true;
            ApplyKvWrites(request,actual,collection,plan,result);
          }
          if constexpr(!std::is_same_v<Request,EngineKeyValuePipelineRequest>)
            PublishKvRows(request,collection,plan,result);
        }
        if constexpr(std::is_same_v<Request,EngineKeyValuePipelineRequest>) {
          // Drop the first materialization/plan before the second actual read.
          collection=KvCollection{};
          const auto fresh=LoadKvCollection(request,actual,operation,false);
          KvRequire(fresh.read.current_live_memory_bytes<limits.maximum_memory_bytes,
              "RESOURCE.BUDGET_EXCEEDED","key_value_source_memory_budget_exceeded");
          memory.Restrict(limits.maximum_memory_bytes-fresh.read.current_live_memory_bytes);
          auto reads=PlanKv(request,actual,fresh,memory,[&](auto& p,auto& rows,auto&){
            KvBatchRead(p,rows,request,request.get_keys,result);
          });
          PublishKvRows(request,fresh,reads,result);
        }
        KvPoll(actual);
        KvPoll(request.context);
        if(savepoint_owner.active) {
          auto released=ReleaseMgaSavepointMarker(actual,savepoint);
          if(released.error)throw KvFailure{released};
          savepoint_owner.active=false;
        }
        succeeded=true;
      } catch(KvFailure& failure) {execution_failure.emplace(std::move(failure));}
        catch(const std::bad_alloc&) {exception_kind=ExceptionKind::memory;}
        catch(...) {exception_kind=ExceptionKind::execution;}
      // Recovery precedes diagnostic formatting/allocation and remains inside
      // BOTH the actual receipt scope and inventory publication ordering.
      if(!succeeded) {
        savepoint_owner.Recover();
        recovery_attempted=savepoint_owner.attempted;
        rolled_back=savepoint_owner.rolled_back;
        recovery_required=savepoint_owner.active;
        if(savepoint_owner.recovery_diagnostic)
          recovery_diagnostic.emplace(std::move(*savepoint_owner.recovery_diagnostic));
        if(savepoint_owner.recovery_exception)exception_kind=ExceptionKind::execution;
      }
    },&admission_cause);
  } catch(KvFailure& failure) {execution_failure.emplace(std::move(failure));}
    catch(const std::bad_alloc&) {exception_kind=ExceptionKind::memory;}
    catch(...) {exception_kind=ExceptionKind::execution;}
  if(succeeded&&admitted==SB_ENGINE_STATUS_OK)return result;
  result.ok=false;
  result.result_shape={};
  if(execution_failure)result.diagnostics=std::move(execution_failure->diagnostics);
  else if(admitted!=SB_ENGINE_STATUS_OK)
    result.diagnostics.push_back(MakeEngineApiDiagnosticFromNative(admission_cause,
        admitted==SB_ENGINE_STATUS_RESOURCE_EXHAUSTED?"RESOURCE.BUDGET_EXCEEDED":"MGA.TRANSACTION.STALE",
        "engine.nosql.key_value.refused","key_value_live_statement_memory_admission_failed"));
  else result.diagnostics.push_back(exception_kind==ExceptionKind::memory?
      KvError("RESOURCE.BUDGET_EXCEEDED","key_value_memory_budget_exceeded"):
      KvError("SBLR.EXECUTION_FAILED","key_value_execution_exception"));
  if(recovery_diagnostic)result.diagnostics.push_back(std::move(*recovery_diagnostic));
  if(recovery_required&&!recovery_diagnostic)
    result.diagnostics.push_back(KvError("SBLR.EXECUTION_FAILED","key_value_recovery_exception"));
  if(rolled_back)result.dml_summary.rows_changed=0;
  if(recovery_attempted)AddApiBehaviorEvidence(&result,"kv_statement_atomicity",
      recovery_required?"recovery_required":"rolled_back");
  return result;
}
void KvOperationBound(std::size_t count,const EngineKeyValueRequest& request) {
  KvRequire(count<=request.resource_limits.maximum_operations,"RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_exceeded");
}
} // namespace

EngineKeyValueGetResult EngineKeyValueGet(const EngineKeyValueGetRequest& request) {
  return RunKv<EngineKeyValueGetResult>(request,"nosql.key_value_get",false,[&](auto& plan,auto& rows,auto&,auto&){
    const bool prefix=request.prefix_scan||!request.prefix.empty();
    KvRequire((prefix?request.prefix.size():request.key.size())<=request.resource_limits.maximum_decoded_bytes,
        "RESOURCE.BUDGET_EXCEEDED","key_value_operand_bytes_exceeded");
    if(prefix) {
      KvRequire(WellFormedUtf8(request.prefix),"SB_MODEL_KEY_VALUE_TEXT_INVALID_V1","key_value_malformed_utf8");
      for(const auto& [key,row]:rows)if(key.starts_with(request.prefix)&&KvVisible(row,request.context.local_transaction_id))
        plan.Output(key,row,request.resource_limits);
      std::ranges::sort(plan.outputs,[](const KvOutput& a,const KvOutput& b){
        return std::lexicographical_compare(a.key.begin(),a.key.end(),b.key.begin(),b.key.end(),
            [](unsigned char left,unsigned char right){return left<right;});
      });
    } else {
      KvRequire(WellFormedUtf8(request.key),"SB_MODEL_KEY_VALUE_TEXT_INVALID_V1","key_value_malformed_utf8");
      const auto found=rows.find(request.key);
      if(found!=rows.end()&&KvVisible(found->second,request.context.local_transaction_id))
        plan.Output(found->first,found->second,request.resource_limits);
    }
  });
}
EngineKeyValuePutResult EngineKeyValuePut(const EngineKeyValuePutRequest& request) {
  return RunKv<EngineKeyValuePutResult>(request,"nosql.key_value_put",true,[&](auto& plan,auto& rows,auto& memory,auto&){
    KvOperationBound(1,request);
    auto& row=KvUpsertPlan(plan,rows,memory,request,request.key,request.value,request.expires_after_local_transaction_id);
    plan.Output(request.key,row,request.resource_limits);
  });
}
EngineKeyValueMultiGetResult EngineKeyValueMultiGet(const EngineKeyValueMultiGetRequest& request) {
  return RunKv<EngineKeyValueMultiGetResult>(request,"nosql.key_value_multiget",false,[&](auto& plan,auto& rows,auto&,auto& result){
    KvOperationBound(request.keys.size(),request);KvBatchRead(plan,rows,request,request.keys,result);
  });
}
EngineKeyValuePipelineResult EngineKeyValuePipeline(const EngineKeyValuePipelineRequest& request) {
  return RunKv<EngineKeyValuePipelineResult>(request,"nosql.key_value_pipeline",!request.puts.empty(),[&](auto& plan,auto& rows,auto& memory,auto&){
    KvRequire(request.puts.size()<=std::numeric_limits<std::size_t>::max()-request.get_keys.size(),
        "RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_overflow");
    const auto count=request.puts.size()+request.get_keys.size();KvOperationBound(count,request);
    KvRequire(!request.max_admitted_operations||count<=request.max_admitted_operations,
        kKeyValuePipelineAdmissionRefused,"key_value_pipeline_batch_limit_exceeded");
    for(const auto& put:request.puts)KvUpsertPlan(plan,rows,memory,request,put.key,put.value,put.expires_after_local_transaction_id);
  });
}
EngineKeyValueAtomicProgramResult EngineKeyValueAtomicProgram(const EngineKeyValueAtomicProgramRequest& request) {
  return RunKv<EngineKeyValueAtomicProgramResult>(request,"nosql.key_value_atomic_program",true,[&](auto& plan,auto& rows,auto& memory,auto&){
    KvOperationBound(request.steps.size(),request);
    for(const auto& step:request.steps) {
      KvPoll(request.context);
      const auto prior=rows.find(step.key);
      const bool present=prior!=rows.end()&&KvVisible(prior->second,request.context.local_transaction_id);
      std::pmr::string value(present?std::string_view(prior->second.value):std::string_view{},&memory);
      const auto expiry=present?prior->second.expiry:0;
      if(step.opcode=="set")value=step.operand;
      else if(step.opcode=="append") {
        KvRequire(step.operand.size()<=request.resource_limits.maximum_decoded_bytes&&
            value.size()<=request.resource_limits.maximum_decoded_bytes-step.operand.size(),
            "RESOURCE.BUDGET_EXCEEDED","key_value_append_bytes_exceeded");
        value+=step.operand;
      } else if(step.opcode=="increment_i64") {
        std::int64_t lhs=0,rhs=0;
        const auto parse=[](std::string_view text,std::int64_t& out){
          const auto p=std::from_chars(text.data(),text.data()+text.size(),out);
          return !text.empty()&&p.ec==std::errc{}&&p.ptr==text.data()+text.size();
        };
        KvRequire((!present||parse(value,lhs))&&parse(step.operand,rhs),
            kKeyValueAtomicProgramRefused,"key_value_increment_requires_complete_int64");
        KvRequire(!(rhs>0&&lhs>INT64_MAX-rhs)&&!(rhs<0&&lhs<INT64_MIN-rhs),
            kKeyValueAtomicProgramRefused,"key_value_increment_int64_overflow");
        std::array<char,32> buffer;const auto formatted=std::to_chars(buffer.data(),buffer.data()+buffer.size(),lhs+rhs);
        value.assign(buffer.data(),formatted.ptr);
      } else throw KvFailure{KvError(kKeyValueAtomicProgramRefused,"key_value_unregistered_atomic_step")};
      auto& stored=KvUpsertPlan(plan,rows,memory,request,step.key,value,expiry);
      plan.Output(step.key,stored,request.resource_limits);
    }
  });
}

namespace {

constexpr const char* kBoundKeyValueOperation =
    "model.key_value.read.v1";

EngineBoundKeyValueReadResultV1 BoundKeyValueRefusal(
    const EngineBoundKeyValueReadRequestV1& request,
    const char* diagnostic_id,
    std::string detail) {
  auto result = MakeApiBehaviorDiagnostic<EngineBoundKeyValueReadResultV1>(
      request.context, kBoundKeyValueOperation,
      MakeEngineApiDiagnostic(diagnostic_id,
                              "engine.model.key_value.read.refused",
                              std::move(detail)));
  result.rows.clear();
  result.result_shape = {};
  return result;
}

bool CheckedAddU64(const std::uint64_t value, std::uint64_t* total) {
  if (total == nullptr ||
      value > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool CanonicalKeyValueUuid(const EngineUuid& value) {
  return core::uuid::IsEngineIdentityUuid(value);
}

bool WellFormedUtf8(const std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t code_point = 0;
    std::size_t continuation_count = 0;
    if (first <= 0x7f) {
      code_point = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1f;
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0f;
      continuation_count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07;
      continuation_count = 3;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1) return false;
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto next = static_cast<unsigned char>(value[offset + index]);
      if ((next & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool CanonicalKeyValueTimestamp(const std::string_view value,
                                const bool stored_expiry) {
  if (stored_expiry) {
    if (value.size() != 30 || value[19] != '.') return false;
  } else if (value.size() != 20 &&
             (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigits[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigits) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](const std::size_t begin,
                           const std::size_t count) {
    unsigned out = 0;
    for (std::size_t index = 0; index < count; ++index) {
      out = out * 10 + static_cast<unsigned>(value[begin + index] - '0');
    }
    return out;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDays[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDays[month];
  if (month == 2 &&
      ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    ++maximum_day;
  }
  return day != 0 && day <= maximum_day;
}

bool ExactKeyValueValueDescriptor(const EngineDescriptor& descriptor,
                                  const std::string_view expected_type,
                                  const EngineUuid& expected_type_uuid,
                                  const scratchbird::core::datatypes::
                                      DatatypeTypeCodecIdentityRowV1*
                                          expected_registry_identity,
                                  const EngineUuid& expected_column_uuid,
                                  const EngineUuid& expected_datatype_descriptor_uuid,
                                  std::uint64_t expected_datatype_generation,
                                  const bool expected_nullable) {
  const auto canonical_type = expected_type == "timestamp_tz" ? "timestamp" : expected_type;
  if (!QowCanonicalDescriptorIdentityV1(descriptor) ||
      (descriptor.descriptor_kind != "scalar" &&
       descriptor.descriptor_kind != "canonical_type_descriptor") ||
      descriptor.canonical_type_name != canonical_type ||
      descriptor.datatype_descriptor_uuid != expected_datatype_descriptor_uuid ||
      descriptor.datatype_descriptor_generation != expected_datatype_generation) {
    return false;
  }
  CatalogColumnMetadata metadata;
  if (!DecodeCatalogColumnMetadata(descriptor.encoded_descriptor, &metadata)) return false;
  const auto& fields = metadata.text;
  const auto& identities = metadata.identities;
  const bool contextual_text = expected_type == "text";
  const bool native_codec = expected_registry_identity != nullptr;
  if (fields.size() != (native_codec ? 8U : 2U) ||
      identities.size() != (native_codec ? (contextual_text ? 4U : 3U) : 1U) ||
      !fields.contains("canonical") || !identities.contains("type_uuid") ||
      !fields.contains("nullable") ||
      fields.at("canonical") != expected_type ||
      !CanonicalKeyValueUuid(identities.at("type_uuid")) ||
      identities.at("type_uuid") != expected_type_uuid ||
      descriptor.type_uuid != expected_type_uuid ||
      fields.at("nullable") != (expected_nullable ? "true" : "false") ||
      (contextual_text &&
       (!native_codec ||
        !identities.contains("column_uuid") ||
        identities.at("column_uuid") != expected_column_uuid)) ||
      (native_codec &&
       (!identities.contains("datatype_descriptor_uuid") ||
        identities.at("datatype_descriptor_uuid") !=
            expected_registry_identity->descriptor_uuid ||
        !fields.contains("datatype_descriptor_generation") ||
        fields.at("datatype_descriptor_generation") !=
            std::to_string(expected_registry_identity->descriptor_generation) ||
        !fields.contains("type_generation") ||
        fields.at("type_generation") !=
            std::to_string(expected_registry_identity->type_generation) ||
        !identities.contains("codec_uuid") ||
        identities.at("codec_uuid") != expected_registry_identity->codec_uuid ||
        !fields.contains("codec_id") ||
        fields.at("codec_id") != expected_registry_identity->codec_id ||
        !fields.contains("codec_version") ||
        fields.at("codec_version") !=
            std::to_string(expected_registry_identity->codec_version) ||
        !fields.contains("codec_generation") ||
        fields.at("codec_generation") !=
            std::to_string(expected_registry_identity->codec_generation) ||
        !fields.contains("null_encoding") ||
        fields.at("null_encoding") !=
            std::to_string(expected_registry_identity->null_encoding_code))) ||
      (!contextual_text && identities.contains("column_uuid"))) {
    return false;
  }
  return true;
}

bool ExactKeyValueStorageDescriptorImpl(
    const EngineRequestContext& context,
    const MgaRelationStorageDescriptor& descriptor) {
  static constexpr std::array<std::string_view, 3> kNames{
      "key", "value", "expires_at"};
  static constexpr std::array<std::string_view, 3> kTypes{
      "text", "text", "timestamp_tz"};
  static constexpr std::array<bool, 3> kNullable{false, false, true};
  if (descriptor.columns.size() != kNames.size() ||
      ValidateMgaRelationStorageDescriptor(descriptor).error) return false;
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return false;
  std::set<EngineUuid> column_uuids;
  std::set<EngineUuid> value_descriptor_uuids;
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto& column = descriptor.columns[ordinal];
    // TIMESTAMP_TZ is the signed key/value storage semantic carried by the
    // canonical engine timestamp catalog identity.  Resolve that identity
    // explicitly rather than treating the carrier spelling as a new type.
    const auto type_id = kTypes[ordinal] == "timestamp_tz"
                             ? scratchbird::core::datatypes::CanonicalTypeId::timestamp
                             : scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
                                   std::string(kTypes[ordinal]));
    const auto type_row = scratchbird::core::datatypes::LookupDatatypeCatalogRow(
        manifest.manifest, type_id);
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return false;
    }
    const auto& descriptor_row =
        type_row.manifest.descriptor_rows.front();
    const auto descriptor_uuid = descriptor_row.descriptor_uuid.value;
    const auto codec_identity =
        scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
            context.datatype_catalog_snapshot_uuid,
            context.datatype_catalog_generation, context.datatype_registry_generation, descriptor_uuid,
            descriptor_row.descriptor_epoch);
    const auto expected_type_uuid =
        codec_identity.ok ? codec_identity.row.type_uuid : descriptor_uuid;
    const auto* expected_registry_identity =
        codec_identity.ok ? &codec_identity.row : nullptr;
    if (kTypes[ordinal] == "text" && expected_registry_identity == nullptr) {
      return false;
    }
    if (column.ordinal != ordinal ||
        column.canonical_name_key != kNames[ordinal] ||
        column.value_descriptor.canonical_type_name !=
            (kTypes[ordinal] == "timestamp_tz" ? "timestamp" : kTypes[ordinal]) ||
        column.nullable != kNullable[ordinal] || column.generated ||
        column.identity_column || column.storage_class != "inline_row_value" ||
        column.max_inline_bytes != 4096 ||
        column.overflow_policy != "mga_large_value_locator" ||
        !CanonicalKeyValueUuid(column.column_uuid) ||
        !CanonicalKeyValueUuid(
            column.value_descriptor.descriptor_uuid) ||
        !column_uuids.insert(column.column_uuid).second ||
        !value_descriptor_uuids.insert(column.value_descriptor.descriptor_uuid).second ||
        !ExactKeyValueValueDescriptor(column.value_descriptor,
                                      kTypes[ordinal], expected_type_uuid,
                                      expected_registry_identity,
                                      column.column_uuid,
                                      descriptor_uuid, descriptor_row.descriptor_epoch,
                                      kNullable[ordinal])) {
      return false;
    }
  }
  return true;
}

std::string NormalizeStatementTimestamp(const std::string_view value) {
  if (!CanonicalKeyValueTimestamp(value, false)) return {};
  if (value.size() == 20) {
    return std::string(value.substr(0, 19)) + ".000000000Z";
  }
  const auto digits = value.size() - 21;
  std::string normalized(value.substr(0, value.size() - 1));
  normalized.append(9 - digits, '0');
  normalized.push_back('Z');
  return normalized;
}

bool UnsignedUtf8Less(const std::string& left, const std::string& right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](const char l, const char r) {
        return static_cast<unsigned char>(l) <
               static_cast<unsigned char>(r);
      });
}

}  // namespace

bool ExactKeyValueStorageDescriptorV1(
    const EngineRequestContext& context,
    const MgaRelationStorageDescriptor& descriptor) {
  return ExactKeyValueStorageDescriptorImpl(context, descriptor);
}

EngineBoundKeyValueReadResultV1 EngineBoundKeyValueReadV1(
    const EngineBoundKeyValueReadRequestV1& request) {
  bool data_access_observed = false;
  const auto refuse = [&](const char* diagnostic, std::string detail) {
    auto result =
        BoundKeyValueRefusal(request, diagnostic, std::move(detail));
    result.data_access_observed = data_access_observed;
    return result;
  };
  const auto cancelled = [&]() {
    try {
      return request.cancellation_requested &&
             request.cancellation_requested();
    } catch (...) {
      return true;
    }
  };
  const bool get =
      request.operation == EngineBoundKeyValueReadOperationV1::kGet;
  const bool multi_get =
      request.operation == EngineBoundKeyValueReadOperationV1::kMultiGet;
  const bool prefix =
      request.operation == EngineBoundKeyValueReadOperationV1::kPrefixRange;
  if (request.abi_version != 1 || (!get && !multi_get && !prefix) ||
      !CanonicalKeyValueUuid(request.object_uuid) ||
      request.expected_descriptor_generation == 0 ||
      !CanonicalKeyValueUuid(request.expected_descriptor_uuid) ||
      !CanonicalKeyValueUuid(request.selected_alternative_uuid) ||
      !CanonicalKeyValueUuid(request.capability_uuid) ||
      !CanonicalKeyValueUuid(request.provider_uuid) ||
      request.provider_generation == 0 ||
      !request.cancellation_requested) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "engine-bound key/value request identity is incomplete");
  }
  if (request.statement_timestamp.empty() ||
      request.statement_timestamp != request.context.statement_timestamp ||
      NormalizeStatementTimestamp(request.statement_timestamp).empty()) {
    return refuse("SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1",
                  "engine-bound key/value statement timestamp is invalid");
  }
  if (request.maximum_request_keys == 0 ||
      request.maximum_request_bytes == 0 ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_value_bytes == 0 ||
      request.maximum_result_bytes == 0 ||
      request.maximum_memory_bytes == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "key/value resource contract is incomplete");
  }
  if ((get || prefix) && request.request_values.size() != 1) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "key/value operation operand cardinality is invalid");
  }
  if (multi_get && request.request_values.empty()) {
    return refuse("SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1",
                  "key/value multi-get requires at least one key");
  }
  if (request.request_values.size() > request.maximum_request_keys) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "key/value request-key count exceeded its bound");
  }

  std::uint64_t request_bytes = 0;
  std::vector<std::string> first_distinct_values;
  std::unordered_set<std::string> distinct_values;
  first_distinct_values.reserve(request.request_values.size());
  for (const auto& value : request.request_values) {
    if (value.state != EngineValueState::value || value.is_null ||
        value.descriptor.canonical_type_name != "text" ||
        !value.binary_value.empty()) {
      return refuse("SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1",
                    "key/value request operand is not non-null TEXT");
    }
    if (!WellFormedUtf8(value.encoded_value)) {
      return refuse("SB_MODEL_KEY_VALUE_TEXT_INVALID_V1",
                    "key/value request operand is malformed UTF-8");
    }
    if (!CheckedAddU64(value.encoded_value.size(), &request_bytes) ||
        request_bytes > request.maximum_request_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "key/value request bytes exceeded their bound");
    }
    if (distinct_values.insert(value.encoded_value).second) {
      first_distinct_values.push_back(value.encoded_value);
    }
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value request normalization was cancelled");
    }
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "key/value execution was cancelled before data access");
  }
  const auto authorization = EvaluateMaterializedAuthorization(
      request.context, request.context.authorization_context, "SELECT",
      request.object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "key/value SELECT authorization was refused");
  }

  const auto preflight =
      LoadMgaRelationStorageDescriptor(request.context, request.object_uuid);
  if (!preflight.ok) {
    const auto detail =
        preflight.diagnostic.detail.empty()
            ? std::string("key/value persisted descriptor preflight failed")
            : preflight.diagnostic.detail;
    const auto mga_failure =
        detail.find("transaction") != std::string::npos ||
        detail.find("snapshot") != std::string::npos ||
        detail.find("visibility") != std::string::npos;
    return refuse(mga_failure ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                              : "SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  detail);
  }
  const auto& persisted = preflight.descriptor;
  if (persisted.relation_uuid != request.object_uuid ||
      persisted.database_uuid !=
          request.context.database_uuid ||
      persisted.schema_uuid.is_nil() ||
      persisted.relation_kind != "table" ||
      persisted.storage_profile != "local_mga_rowstore_v1" ||
      persisted.descriptor_uuid !=
          request.expected_descriptor_uuid ||
      persisted.descriptor_generation !=
          request.expected_descriptor_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "key/value persisted descriptor identity changed");
  }
  if (!ExactKeyValueStorageDescriptorV1(request.context, persisted)) {
    return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                  "key/value storage descriptor is not exact before data access");
  }

  MgaVisibleHeapRelationReadRequest read_request;
  read_request.relation_uuid = request.object_uuid;
  read_request.maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  read_request.maximum_decoded_bytes = request.maximum_decoded_bytes;
  read_request.maximum_output_rows = request.maximum_output_rows;
  read_request.cancellation_requested = request.cancellation_requested;
  data_access_observed = true;
  const auto read = ReadVisibleMgaHeapRelation(request.context, read_request);
  if (!read.ok) {
    if (read.cancellation_observed || cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value MGA-visible row read was cancelled");
    }
    const auto detail = read.diagnostic.detail.empty()
                            ? std::string(
                                  "bounded key/value MGA-visible row read failed")
                            : read.diagnostic.detail;
    const auto resource_failure =
        detail.find("maximum") != std::string::npos ||
        detail.find("resource") != std::string::npos ||
        detail.find("bound") != std::string::npos ||
        detail.find("decoded") != std::string::npos ||
        detail.find("overflow") != std::string::npos ||
        detail.find("large_value") != std::string::npos;
    const auto mga_failure =
        detail.find("transaction") != std::string::npos ||
        detail.find("snapshot") != std::string::npos ||
        detail.find("visibility") != std::string::npos;
    return refuse(resource_failure
                      ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                      : mga_failure ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                                    : "SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  detail);
  }

  EngineBoundKeyValueReadResultV1 result =
      MakeApiBehaviorSuccess<EngineBoundKeyValueReadResultV1>(
          request.context, kBoundKeyValueOperation);
  result.data_access_observed = true;
  result.exact_fallback_observed = request.exact_fallback_selected;
  result.scanned_row_version_count = read.scanned_row_version_count;
  result.selected_visible_row_count = read.visible_rows.size();
  result.descriptor_uuid = read.descriptor.descriptor_uuid;
  result.descriptor_generation = read.descriptor.descriptor_generation;
  result.selected_alternative_uuid = request.selected_alternative_uuid;
  result.capability_uuid = request.capability_uuid;
  result.provider_uuid = request.provider_uuid;
  result.provider_generation = request.provider_generation;
  if (result.descriptor_uuid != request.expected_descriptor_uuid ||
      result.descriptor_generation !=
          request.expected_descriptor_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "key/value relation descriptor generation changed");
  }

  if (!ExactKeyValueStorageDescriptorV1(request.context, read.descriptor)) {
    return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                  "key/value storage descriptor is not key/value/expires_at");
  }

  struct VisibleRow {
    EngineBoundKeyValueRowV1 public_row;
    std::optional<std::string> expires_at;
  };
  std::vector<VisibleRow> visible_rows;
  visible_rows.reserve(read.visible_rows.size());
  std::unordered_set<std::string> visible_keys;
  std::uint64_t retained_value_bytes = 0;
  const auto normalized_statement =
      NormalizeStatementTimestamp(request.statement_timestamp);
  for (const auto& row : read.visible_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value visible-row validation was cancelled");
    }
    if (!CanonicalKeyValueUuid(row.row_uuid)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "key/value row identity is not canonical");
    }
    if (row.values.size() != read.descriptor.columns.size()) {
      return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                    "key/value stored row width differs from its descriptor");
    }
    std::unordered_map<std::string, CrudStoredValue> fields;
    for (const auto& [name, encoded] : row.values) {
      if (!fields.emplace(name, encoded).second) {
        return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                      "key/value stored row has a duplicated field");
      }
    }
    const auto stored_key = fields.find("key");
    const auto stored_value = fields.find("value");
    const auto stored_expiry = fields.find("expires_at");
    if (stored_key == fields.end() || stored_value == fields.end() ||
        stored_expiry == fields.end() || fields.size() != 3 ||
        !stored_key->second.isPresent() || !stored_value->second.isPresent() ||
        !stored_expiry->second.valid() ||
        (!stored_expiry->second.isPresent() && !stored_expiry->second.isSqlNull())) {
      return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                    "key/value stored row has null or unknown fields");
    }
    if (!WellFormedUtf8(stored_key->second.bytes) ||
        !WellFormedUtf8(stored_value->second.bytes)) {
      return refuse("SB_MODEL_KEY_VALUE_TEXT_INVALID_V1",
                    "key/value stored row contains malformed UTF-8");
    }
    if (!visible_keys.insert(stored_key->second.bytes).second) {
      return refuse("SB_MODEL_KEY_VALUE_DUPLICATE_VISIBLE_KEY_REFUSED_V1",
                    "more than one MGA-visible logical row has the same key");
    }
    VisibleRow selected;
    selected.public_row = {row.row_uuid, stored_key->second.bytes,
                           stored_value->second.bytes};
    if (!stored_expiry->second.isSqlNull()) {
      if (!CanonicalKeyValueTimestamp(stored_expiry->second.bytes, true)) {
        return refuse("SB_MODEL_KEY_VALUE_EXPIRES_AT_INVALID_V1",
                      "key/value expires_at is not canonical TIMESTAMP_TZ");
      }
      selected.expires_at = stored_expiry->second.bytes;
    }
    if (!CheckedAddU64(selected.public_row.key.size(),
                       &retained_value_bytes) ||
        !CheckedAddU64(selected.public_row.value.size(),
                       &retained_value_bytes) ||
        retained_value_bytes > request.maximum_value_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "key/value selected value bytes exceeded their bound");
    }
    visible_rows.push_back(std::move(selected));
  }

  std::unordered_map<std::string, const VisibleRow*> unexpired;
  unexpired.reserve(visible_rows.size());
  for (const auto& row : visible_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value TTL evaluation was cancelled");
    }
    if (!row.expires_at.has_value() ||
        normalized_statement < *row.expires_at) {
      unexpired.emplace(row.public_row.key, &row);
    }
  }

  std::vector<EngineBoundKeyValueRowV1> output;
  if (get) {
    result.ordering_id = "key_value_unordered_v1";
    const auto found = unexpired.find(first_distinct_values.front());
    if (found != unexpired.end()) output.push_back(found->second->public_row);
  } else if (multi_get) {
    result.ordering_id = "first_distinct_request_order_v1";
    for (const auto& key : first_distinct_values) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "key/value multi-get lookup was cancelled");
      }
      const auto found = unexpired.find(key);
      if (found != unexpired.end()) output.push_back(found->second->public_row);
    }
  } else {
    result.ordering_id = "key_utf8_byte_ascending_v1";
    const auto& requested_prefix = first_distinct_values.front();
    for (const auto& [key, row] : unexpired) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "key/value prefix scan was cancelled");
      }
      if (key.starts_with(requested_prefix)) output.push_back(row->public_row);
    }
    std::ranges::sort(output, [](const auto& left, const auto& right) {
      return UnsignedUtf8Less(left.key, right.key);
    });
  }

  std::uint64_t result_bytes = 0;
  std::uint64_t memory_bytes = sizeof(result);
  if (output.size() > request.maximum_output_rows) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "key/value output row count exceeded its bound");
  }
  for (const auto& row : output) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value result materialization was cancelled");
    }
    const auto row_bytes = static_cast<std::uint64_t>(
        row.row_uuid.bytes.size() + row.key.size() + row.value.size() + 3);
    if (!CheckedAddU64(row_bytes, &result_bytes) ||
        !CheckedAddU64(sizeof(EngineBoundKeyValueRowV1), &memory_bytes) ||
        !CheckedAddU64(row_bytes, &memory_bytes) ||
        result_bytes > request.maximum_result_bytes ||
        memory_bytes > request.maximum_memory_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "key/value atomic result preflight exceeded its bound");
    }
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "key/value execution was cancelled before publication");
  }
  result.result_byte_count = result_bytes;
  result.rows = std::move(output);
  result.residual_recheck_complete = true;
  result.base_row_mga_recheck_complete = true;
  result.security_recheck_complete = true;
  AddApiBehaviorEvidence(&result, "key_value_visibility_authority",
                         "engine_mga_statement_snapshot_first");
  AddApiBehaviorEvidence(
      &result, "key_value_ttl_statement_timestamp",
      request.statement_timestamp);
  AddApiBehaviorEvidence(
      &result, "key_value_access_route",
      result.exact_fallback_observed
          ? "KEY_VALUE_ORDERED_STORE_SCAN_EXACT_V1"
          : "key_value.local.v1");
  AddApiBehaviorEvidence(&result, "key_value_ordering", result.ordering_id);
  return result;
}

}  // namespace scratchbird::engine::internal_api
