// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "security/native_runtime_security_observation.hpp"
#include "security/security_model.hpp"
#include "database_lifecycle.hpp"
#include <cstring>
#include <memory>

namespace scratchbird::engine::internal_api {

// This is a bounded projection of DATA, not authentication, provider/config
// activation, a security admission receipt, or permission to start a transaction.
// The eventual security owner must retain/fence the actual native source and
// credential proof. No public observation can manufacture that authority.
enum class NativeRuntimeAuthorizationError {
  none, invalid_limits, backing_unavailable, resource_exhausted, source_failure,
  invalid_binding, invalid_record, duplicate_identity, generation_mismatch,
  principal_invalid, subject_missing, membership_cycle, unknown_right,
  malformed_membership, malformed_grant, work_limit, depth_limit, input_overlap
};
enum class NativeRuntimeAuthorizationSubjectKind : core::platform::u8 {
  invalid, principal, role, group
};
struct NativeRuntimeAuthorizationLimits {
  core::platform::usize maximum_records=0, maximum_depth=0;
  core::platform::u64 maximum_work=0;
};
struct NativeRuntimeAuthorizationSubject {
  EngineUuid subject_uuid, row_uuid, version_uuid;
  core::platform::u64 definition_generation=0, catalog_generation=0, security_epoch=0;
  // These payload event generations are NOT configuration epochs.
  std::optional<core::platform::u64> security_generation, policy_generation;
  NativeRuntimeAuthorizationSubjectKind kind=NativeRuntimeAuthorizationSubjectKind::invalid;
};
struct NativeRuntimeAuthorizationMembership {
  EngineUuid membership_uuid, member_uuid, parent_uuid, version_uuid;
  NativeRuntimeAuthorizationSubjectKind member_kind=NativeRuntimeAuthorizationSubjectKind::invalid;
  NativeRuntimeAuthorizationSubjectKind parent_kind=NativeRuntimeAuthorizationSubjectKind::invalid;
  core::platform::u64 definition_generation=0, security_epoch=0;
  core::platform::u64 source_security_epoch=0, catalog_generation=0;
};
struct NativeRuntimeAuthorizationGrant {
  EngineUuid grant_uuid, subject_uuid, target_uuid, version_uuid;
  NativeRuntimeAuthorizationSubjectKind subject_kind=NativeRuntimeAuthorizationSubjectKind::invalid;
  std::string_view right; // governed copy of the actual registered vocabulary
  bool deny=false;
  core::platform::u64 definition_generation=0, security_epoch=0;
  core::platform::u64 source_security_epoch=0, catalog_generation=0;
};
class NativeRuntimeAuthorizationPreparedMemory;
struct NativeRuntimeAuthorizationPreparedResult;
struct NativeRuntimeAuthorizationResult;
inline NativeRuntimeAuthorizationResult ProjectNativeRuntimeAuthorization(
    const NativeRuntimeSecurityObservationResult&,NativeRuntimeAuthorizationPreparedMemory&) noexcept;

class NativeRuntimeAuthorizationProjection {
 public:
  bool present() const noexcept {return !subjects_.empty();}
  const core::catalog::CatalogRuntimeAuthorityBinding& binding() const noexcept {return binding_;}
  const NativeRuntimeServicePrincipalObservation& principal() const noexcept {return principal_;}
  std::span<const NativeRuntimeAuthorizationSubject> subjects() const noexcept {return subjects_;}
  std::span<const NativeRuntimeAuthorizationMembership> memberships() const noexcept {return memberships_;}
  std::span<const NativeRuntimeAuthorizationGrant> grants() const noexcept {return grants_;}
  EngineUuid engine_owned_bootstrap_role_uuid() const noexcept {return bootstrap_;}
  // Base-right projection ONLY. Row/column/domain/runtime/cluster gates remain
  // mandatory at their owning operation. Explicit global/target denies precede
  // both grants and the genuinely resolved engine-owned bootstrap bundle.
  bool Allows(std::string_view right,const EngineUuid& target={}) const noexcept {
    if(!present()||(!target.is_nil()&&!core::uuid::IsEngineIdentityUuid(target)))return false;
    if(std::find(rights_.begin(),rights_.end(),right)==rights_.end())return false;
    bool allow=!bootstrap_.is_nil();
    for(const auto& g:grants_)if(g.right==right&&(g.target_uuid.is_nil()||g.target_uuid==target)){
      if(g.deny)return false;allow=true;
    }
    return allow;
  }
 private:
  core::catalog::CatalogRuntimeAuthorityBinding binding_;
  NativeRuntimeServicePrincipalObservation principal_;
  EngineUuid bootstrap_;
  std::span<const NativeRuntimeAuthorizationSubject> subjects_;
  std::span<const NativeRuntimeAuthorizationMembership> memberships_;
  std::span<const NativeRuntimeAuthorizationGrant> grants_;
  std::span<const std::string_view> rights_;
  friend NativeRuntimeAuthorizationResult ProjectNativeRuntimeAuthorization(
      const NativeRuntimeSecurityObservationResult&,NativeRuntimeAuthorizationPreparedMemory&) noexcept;
};
struct NativeRuntimeAuthorizationResult {
  NativeRuntimeAuthorizationError error=NativeRuntimeAuthorizationError::invalid_binding;
  core::platform::usize failing_record=std::numeric_limits<core::platform::usize>::max();
  core::platform::u64 work_performed=0;
  // Preserve the entire fixed actual source failure, without borrowing a usable
  // record prefix or reconstructing native errors from detail strings.
  NativeRuntimeSecurityReadError source_error=NativeRuntimeSecurityReadError::none;
  RuntimeAuthorityBindingReadError binding_error=RuntimeAuthorityBindingReadError::none;
  storage::database::NativePinnedCatalogReadError catalog_error=storage::database::NativePinnedCatalogReadError::none;
  storage::database::NativeCatalogRelationLeaseError relation_error=storage::database::NativeCatalogRelationLeaseError::none;
  core::catalog::CatalogValueError record_error=core::catalog::CatalogValueError::none;
  transaction::mga::MgaObservationStatus visibility_diagnostic;
  std::optional<storage::disk::BoundedIoResult> io_failure;
  EngineUuid io_failure_filespace_uuid;
  NativeRuntimeAuthorizationProjection projection;
  bool ok() const noexcept {return error==NativeRuntimeAuthorizationError::none&&projection.present();}
};

namespace native_authorization_detail {
using Size=core::platform::usize;
inline constexpr Size absent=std::numeric_limits<Size>::max();
using Kind=NativeRuntimeAuthorizationSubjectKind;
inline Kind KindOf(std::string_view s) noexcept {
  return s=="principal"?Kind::principal:s=="role"?Kind::role:s=="group"?Kind::group:Kind::invalid;
}
inline Kind KindOf(core::catalog::CatalogRecordKind k) noexcept {
  using K=core::catalog::CatalogRecordKind;
  return k==K::user_account?Kind::principal:k==K::role_account?Kind::role:k==K::group_account?Kind::group:Kind::invalid;
}
struct Node {NativeRuntimeAuthorizationSubject value;Size first=absent;core::platform::u8 color=0;bool active=false,bootstrap=false;};
struct Edge {NativeRuntimeAuthorizationMembership value;Size member=absent,parent=absent,next=absent;};
struct Grant {NativeRuntimeAuthorizationGrant value;Size subject=absent;};
struct Frame {Size node=absent,next=absent;};
inline bool Active(const NativeRuntimeSecurityRecordObservation& r) noexcept {
  return !r.deleted&&r.lifecycle==core::catalog::CatalogObjectLifecycle::active&&
      r.status==core::catalog::CatalogObjectStatus::active&&r.record.Boolean("active")!=false&&
      r.record.Boolean("created_disabled")!=true;
}
inline bool Overlap(const void* a,Size n,const void* b,Size m) noexcept {
  if(!n||!m)return false;
  const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
  return x<=y?y-x<n:x-y<m;
}
inline bool Add(Size& bytes,Size count,Size width,Size alignment) noexcept {
  const auto max=std::numeric_limits<Size>::max();
  if(count>(max-(alignment-1))/width)return false;
  const auto extra=count*width+alignment-1;if(bytes>max-extra)return false;bytes+=extra;return true;
}
inline bool Limits(const NativeRuntimeAuthorizationLimits& l) noexcept {
  return l.maximum_records&&l.maximum_depth&&l.maximum_depth<=l.maximum_records&&l.maximum_work;
}
} // namespace native_authorization_detail

// All storage including vocabulary bytes is charged to this actual native grant.
// Prepare and destroy outside source guards. Result views borrow this owner;
// keep it alive and do not move/reuse it while any view is in use. Projection
// reuse explicitly invalidates earlier views. No source strings are retained.
class NativeRuntimeAuthorizationPreparedMemory {
 public:
  NativeRuntimeAuthorizationPreparedMemory() noexcept=default;
  NativeRuntimeAuthorizationPreparedMemory(const NativeRuntimeAuthorizationPreparedMemory&)=delete;
  NativeRuntimeAuthorizationPreparedMemory& operator=(const NativeRuntimeAuthorizationPreparedMemory&)=delete;
  NativeRuntimeAuthorizationPreparedMemory(NativeRuntimeAuthorizationPreparedMemory&&) noexcept=default;
  NativeRuntimeAuthorizationPreparedMemory& operator=(NativeRuntimeAuthorizationPreparedMemory&&) noexcept=default;
  explicit operator bool() const noexcept {return bool(arena_)&&!raw_.empty()&&!rights_.empty();}
 private:
  storage::database::NativeStorageArena arena_;
  NativeRuntimeAuthorizationLimits limits_;
  std::span<core::platform::byte> raw_;
  std::span<std::string_view> rights_;
  std::array<core::catalog::CatalogValueSchemaView,4> schemas_;
  std::span<core::platform::usize> order_;
  std::span<native_authorization_detail::Node> nodes_;
  std::span<native_authorization_detail::Edge> edges_;
  std::span<native_authorization_detail::Grant> grants_;
  std::span<native_authorization_detail::Frame> frames_;
  std::span<NativeRuntimeAuthorizationSubject> subjects_out_;
  std::span<NativeRuntimeAuthorizationMembership> edges_out_;
  std::span<NativeRuntimeAuthorizationGrant> grants_out_;
  friend NativeRuntimeAuthorizationPreparedResult PrepareNativeRuntimeAuthorizationProjection(
      NativeRuntimeAuthorizationLimits,storage::database::NativeStorageMemory&,
      const storage::database::NativeStorageMemoryBinding&) noexcept;
  friend NativeRuntimeAuthorizationResult ProjectNativeRuntimeAuthorization(
      const NativeRuntimeSecurityObservationResult&,NativeRuntimeAuthorizationPreparedMemory&) noexcept;
};
inline core::platform::usize NativeRuntimeAuthorizationWorkspaceBytes(NativeRuntimeAuthorizationLimits limits) noexcept {
  using namespace native_authorization_detail;
  try {
    if(!Limits(limits))return 0;Size bytes=0;
    const auto& rights=KnownSecurityRights();
    if(rights.empty()||!Add(bytes,rights.size(),sizeof(std::string_view),alignof(std::string_view)))return 0;
    for(const auto& r:rights)if(!Add(bytes,r.size(),1,1))return 0;
    // Copy the real registered field schemas, too: no lazy registry setup under
    // native guards, and no hand-maintained shadow of the native scalar schema.
    using K=core::catalog::CatalogRecordKind;
    for(auto kind:{K::user_account,K::role_account,K::group_account,K::grant_record}){
      const auto& s=core::catalog::CatalogSecurityRecordSchema(kind);
      if(!Add(bytes,s.fields.size(),sizeof(core::catalog::CatalogValueFieldSchema),alignof(core::catalog::CatalogValueFieldSchema)))return 0;
    }
    const auto n=limits.maximum_records;
    if(!Add(bytes,n,sizeof(Size),alignof(Size))||!Add(bytes,n,sizeof(Node),alignof(Node))||
        !Add(bytes,n,sizeof(Edge),alignof(Edge))||!Add(bytes,n,sizeof(Grant),alignof(Grant))||
        !Add(bytes,limits.maximum_depth,sizeof(Frame),alignof(Frame))||
        !Add(bytes,n,sizeof(NativeRuntimeAuthorizationSubject),alignof(NativeRuntimeAuthorizationSubject))||
        !Add(bytes,n,sizeof(NativeRuntimeAuthorizationMembership),alignof(NativeRuntimeAuthorizationMembership))||
        !Add(bytes,n,sizeof(NativeRuntimeAuthorizationGrant),alignof(NativeRuntimeAuthorizationGrant)))return 0;
    return bytes;
  }catch(...){return 0;}
}
struct NativeRuntimeAuthorizationPreparedResult {
  NativeRuntimeAuthorizationError error=NativeRuntimeAuthorizationError::invalid_limits;
  storage::database::NativeStorageMemoryError memory_error=storage::database::NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeRuntimeAuthorizationPreparedMemory workspace;
  bool ok() const noexcept {return error==NativeRuntimeAuthorizationError::none&&bool(workspace);}
};
inline NativeRuntimeAuthorizationPreparedResult PrepareNativeRuntimeAuthorizationProjection(
    NativeRuntimeAuthorizationLimits limits,storage::database::NativeStorageMemory& memory,
    const storage::database::NativeStorageMemoryBinding& binding) noexcept {
  using namespace native_authorization_detail;NativeRuntimeAuthorizationPreparedResult out;
  try {
    // Initialize the owning registries while failures still have an explicit
    // preparation resource cause, never for the first time under a source lock.
    (void)KnownSecurityRights();
    using InitialKind=core::catalog::CatalogRecordKind;
    for(auto kind:{InitialKind::user_account,InitialKind::role_account,InitialKind::group_account,InitialKind::grant_record})
      (void)core::catalog::CatalogSecurityRecordSchema(kind);
    const auto bytes=NativeRuntimeAuthorizationWorkspaceBytes(limits);if(!bytes)return out;
    auto allocated=memory.CreateArena(binding,bytes,alignof(std::max_align_t));
    if(!allocated.ok()){
      out.error=NativeRuntimeAuthorizationError::backing_unavailable;out.memory_error=allocated.error;
      out.allocation_status=allocated.backing.status;out.allocation_diagnostic=std::move(allocated.backing.diagnostic);return out;
    }
    NativeRuntimeAuthorizationPreparedMemory w;w.arena_=std::move(allocated.arena);w.limits_=limits;
    auto block=w.arena_.Allocate(bytes,alignof(std::max_align_t));
    if(!block.ok()){out.error=NativeRuntimeAuthorizationError::resource_exhausted;return out;}
    w.raw_={static_cast<core::platform::byte*>(block.pointer),bytes};
    void* cursor=block.pointer;Size left=bytes;
    const auto slots=[&]<class T>(Size count)->std::span<T>{
      void* p=std::align(alignof(T),count*sizeof(T),cursor,left);if(!p)return {};
      auto* typed=static_cast<T*>(p);std::uninitialized_value_construct_n(typed,count);
      cursor=static_cast<core::platform::byte*>(p)+count*sizeof(T);left-=count*sizeof(T);return {typed,count};
    };
    const auto& rights=KnownSecurityRights();w.rights_=slots.template operator()<std::string_view>(rights.size());
    if(w.rights_.size()!=rights.size()){out.error=NativeRuntimeAuthorizationError::resource_exhausted;return out;}
    Size ordinal=0;
    for(const auto& r:rights){auto copy=slots.template operator()<char>(r.size());
      if(copy.size()!=r.size()){out.error=NativeRuntimeAuthorizationError::resource_exhausted;return out;}
      std::memcpy(copy.data(),r.data(),r.size());w.rights_[ordinal++]={copy.data(),copy.size()};}
    using K=core::catalog::CatalogRecordKind;ordinal=0;
    for(auto kind:{K::user_account,K::role_account,K::group_account,K::grant_record}){
      const auto& s=core::catalog::CatalogSecurityRecordSchema(kind);
      auto fields=slots.template operator()<core::catalog::CatalogValueFieldSchema>(s.fields.size());
      if(fields.size()!=s.fields.size()){out.error=NativeRuntimeAuthorizationError::resource_exhausted;return out;}
      std::copy(s.fields.begin(),s.fields.end(),fields.begin());w.schemas_[ordinal++]={s.id,s.version,fields};
    }
    const auto n=limits.maximum_records;
    w.order_=slots.template operator()<Size>(n);w.nodes_=slots.template operator()<Node>(n);
    w.edges_=slots.template operator()<Edge>(n);w.grants_=slots.template operator()<Grant>(n);
    w.frames_=slots.template operator()<Frame>(limits.maximum_depth);
    w.subjects_out_=slots.template operator()<NativeRuntimeAuthorizationSubject>(n);
    w.edges_out_=slots.template operator()<NativeRuntimeAuthorizationMembership>(n);
    w.grants_out_=slots.template operator()<NativeRuntimeAuthorizationGrant>(n);
    if(w.order_.size()!=n||w.nodes_.size()!=n||w.edges_.size()!=n||w.grants_.size()!=n||
        w.frames_.size()!=limits.maximum_depth||w.subjects_out_.size()!=n||w.edges_out_.size()!=n||w.grants_out_.size()!=n){
      out.error=NativeRuntimeAuthorizationError::resource_exhausted;return out;
    }
    out.workspace=std::move(w);out.error=NativeRuntimeAuthorizationError::none;
  }catch(const std::bad_alloc&){out.error=NativeRuntimeAuthorizationError::resource_exhausted;}
   catch(...){out.error=NativeRuntimeAuthorizationError::backing_unavailable;}
  return out;
}

inline NativeRuntimeAuthorizationResult ProjectNativeRuntimeAuthorization(
    const NativeRuntimeSecurityObservationResult& source,NativeRuntimeAuthorizationPreparedMemory& w) noexcept {
  namespace c=core::catalog;using namespace native_authorization_detail;
  using E=NativeRuntimeAuthorizationError;NativeRuntimeAuthorizationResult out;
  const auto fail=[&](E e,Size n=absent){out.error=e;out.failing_record=n;out.projection={};return out;};
  bool exhausted=false;
  const auto spend=[&](core::platform::u64 amount=1){
    if(amount>w.limits_.maximum_work-out.work_performed){exhausted=true;return false;}out.work_performed+=amount;return true;
  };
  const auto id=[](const EngineUuid& u){return core::uuid::IsEngineIdentityUuid(u);};
  if(!source.ok()||source.binding_error!=RuntimeAuthorityBindingReadError::none||
      source.source_error!=storage::database::NativePinnedCatalogReadError::none||
      source.relation_error!=storage::database::NativeCatalogRelationLeaseError::none||
      source.record_error!=c::CatalogValueError::none||source.io_failure){
    out.source_error=source.error;out.binding_error=source.binding_error;out.catalog_error=source.source_error;
    out.relation_error=source.relation_error;out.record_error=source.record_error;
    out.visibility_diagnostic=source.visibility_diagnostic;out.io_failure=source.io_failure;
    out.io_failure_filespace_uuid=source.io_failure_filespace_uuid;return fail(E::source_failure);
  }
  if(!w)return fail(E::backing_unavailable);
  const auto& binding=*source.binding;const auto& principal=*source.principal;
  if(!c::runtime_binding_detail::Valid(binding)||binding.authority_mode!=c::RuntimeAuthorityMode::database_local||
      !id(source.binding_version_uuid)||w.arena_.binding().database_uuid!=binding.database_uuid||
      principal.principal_uuid!=binding.service_principal_uuid||!id(principal.principal_uuid)||!id(principal.version_uuid)||
      !principal.definition_generation||!principal.security_context_generation||!principal.security_generation||!principal.policy_generation)
    return fail(E::invalid_binding);
  const auto count=source.records.size();if(count>w.limits_.maximum_records||
      count>std::numeric_limits<Size>::max()/sizeof(source.records[0]))return fail(E::resource_exhausted);
  if(!source.records.data()||count*sizeof(source.records[0])>
      std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(source.records.data()))return fail(E::invalid_record);
  if(Overlap(source.records.data(),count*sizeof(source.records[0]),w.raw_.data(),w.raw_.size()))return fail(E::input_overlap);
  // Deterministic allocation-free heapsort: O(n log n), bounded comparisons,
  // constant stack. Input ordinal only breaks equal identity ties, which are
  // subsequently refused regardless of input order.
  for(Size n=0;n<count;++n){if(!spend())return fail(E::work_limit,n);w.order_[n]=n;}
  const auto less=[&](Size a,Size b){
    if(!spend())return false;const auto& x=source.records[a].object_uuid;const auto& y=source.records[b].object_uuid;
    return x==y?a<b:x<y;
  };
  const auto sift=[&](Size root,Size length){
    while(root<length/2){Size child=2*root+1;
      if(child+1<length){const bool right=less(w.order_[child],w.order_[child+1]);if(exhausted)return false;if(right)++child;}
      const bool promote=less(w.order_[root],w.order_[child]);if(exhausted)return false;if(!promote)break;
      std::swap(w.order_[root],w.order_[child]);root=child;
    }return true;
  };
  for(Size n=count/2;n;--n)if(!sift(n-1,count))return fail(E::work_limit);
  for(Size n=count;n>1;--n){std::swap(w.order_[0],w.order_[n-1]);if(!sift(0,n-1))return fail(E::work_limit);}
  Size nodes=0,edges=0,grants=0,selected=absent;
  for(Size ordered=0;ordered<count;++ordered){
    const auto n=w.order_[ordered];const auto& r=source.records[n];const auto& body=r.record;
    if(!spend())return fail(E::work_limit,n);
    if(ordered&&source.records[w.order_[ordered-1]].object_uuid==r.object_uuid)return fail(E::duplicate_identity,n);
    const auto kind=KindOf(body.kind);const bool grant=body.kind==c::CatalogRecordKind::grant_record;
    if((kind==Kind::invalid&&!grant)||!id(r.object_uuid)||!id(r.row_uuid)||r.object_uuid==r.row_uuid||
        !id(r.version_uuid)||(!r.previous_version_uuid.is_nil()&&!id(r.previous_version_uuid))||
        !id(r.origin_transaction_uuid)||!id(r.creator_transaction_uuid)||!r.origin_local_transaction_id||
        !r.creator_local_transaction_id||r.origin_local_transaction_id>r.creator_local_transaction_id||
        !r.definition_generation||!r.catalog_generation||!r.security_epoch||
        body.Identity(c::CatalogSecurityPrimaryIdentityName(body.kind))!=r.object_uuid||
        body.Identity("origin_transaction_uuid")!=r.origin_transaction_uuid||
        body.Unsigned("creator_tx")!=r.origin_local_transaction_id||
        (body.Unsigned("created_txn")&&body.Unsigned("created_txn")!=body.Unsigned("creator_tx"))||
        (r.definition_generation==1&&(r.origin_transaction_uuid!=r.creator_transaction_uuid||r.origin_local_transaction_id!=r.creator_local_transaction_id))||
        (!r.retired_transaction_uuid.is_nil()&&(!id(r.retired_transaction_uuid)||r.retired_transaction_uuid!=r.creator_transaction_uuid||r.retired_local_transaction_id!=r.creator_local_transaction_id))||
        (r.retired_transaction_uuid.is_nil()&&r.retired_local_transaction_id)||
        static_cast<unsigned>(r.lifecycle)<1||static_cast<unsigned>(r.lifecycle)>10||
        static_cast<unsigned>(r.status)<1||static_cast<unsigned>(r.status)>9)return fail(E::invalid_record,n);
    const Size schema_index=grant?3:kind==Kind::principal?0:kind==Kind::role?1:2;
    const auto& schema=w.schemas_[schema_index];
    // Validate every supplied slot against the real pre-copied native schema,
    // including inactive/unreachable records, rather than last-wins defaults.
    std::array<c::CatalogValueFieldView,48> fields;
    std::array<std::array<core::platform::byte,8>,37> scalars{};Size field_count=0;
    for(Size i=0;i<body.identities.size();++i){
      if(!spend())return fail(E::work_limit,n);
      if(body.identities[i]){
        if(i<10&&body.identities[i]==body.identities[10])return fail(E::invalid_record,n);
        fields[field_count++]={static_cast<core::platform::u16>(i+1),c::CatalogValueType::engine_identity,
            i==10?core::platform::UuidKind::transaction:core::platform::UuidKind::object,body.identities[i]->bytes};
      }
    }
    for(Size i=0;i<body.attributes.size();++i){
      if(!spend())return fail(E::work_limit,n);
      if(!body.attributes[i])continue;
      const auto& a=*body.attributes[i];
      c::CatalogValueFieldView field;field.id=static_cast<core::platform::u16>(i+32);
      if(const auto* text=std::get_if<std::string_view>(&a)){
        const auto definition=std::find_if(schema.fields.begin(),schema.fields.end(),[&](const auto& f){return f.id==field.id;});
        if(definition==schema.fields.end()||text->size()>definition->maximum_bytes||(!text->empty()&&!text->data()))return fail(E::invalid_record,n);
        if(Overlap(text->data(),text->size(),w.raw_.data(),w.raw_.size()))return fail(E::input_overlap,n);
        if(!spend(text->size()))return fail(E::work_limit,n);
        field.type=c::CatalogValueType::utf8_text;field.bytes={reinterpret_cast<const core::platform::byte*>(text->data()),text->size()};
      }else if(const auto* value=std::get_if<core::platform::u64>(&a)){
        for(unsigned j=0;j<8;++j)scalars[i][j]=static_cast<core::platform::byte>(*value>>(j*8));
        field.type=c::CatalogValueType::unsigned_integer;field.bytes=scalars[i];
      }else{
        scalars[i][0]=std::get<bool>(a)?1:0;field.type=c::CatalogValueType::boolean;field.bytes={scalars[i].data(),1};
      }fields[field_count++]=field;
    }
    const auto structural=c::ValidateCatalogValueFields(schema,std::span(fields).first(field_count));
    if(structural!=c::CatalogValueError::none){out.record_error=structural;return fail(E::invalid_record,n);}
    // Metadata epochs describe WHEN THIS VERSION became visible, not the
    // current admitted snapshot epoch. Unchanged historical active versions
    // remain valid; a record from a future catalog/security selection does not.
    if(r.catalog_generation>binding.catalog_generation||r.security_epoch>binding.security_epoch)return fail(E::generation_mismatch,n);
    const bool active=Active(r);
    if(!grant){
      auto& node=w.nodes_[nodes];node={};node.active=active;
      node.value={r.object_uuid,r.row_uuid,r.version_uuid,r.definition_generation,r.catalog_generation,r.security_epoch,
          body.Unsigned("security_generation"),body.Unsigned("policy_generation"),kind};
      if(kind==Kind::role&&r.object_uuid==storage::database::kCanonicalSysarchRoleIdentity){
        const auto context_generation=body.Unsigned("security_context_generation"),policy_generation=body.Unsigned("policy_generation");
        if(body.Attribute("role_code")!="ROLE_SYSARCH"||body.Attribute("authority_class")!="engine_owned_sysarch"||
            body.Attribute("identity_authority")!="uuid"||body.Boolean("engine_owned")!=true||body.Boolean("immutable")!=true||
            body.Boolean("create_time_only")!=true||body.Boolean("active")!=true||body.Unsigned("creator_tx")!=1||
            body.Unsigned("security_context_authority_version")!=1||!context_generation||!*context_generation||
            !policy_generation||!*policy_generation||*policy_generation>std::numeric_limits<core::platform::u32>::max())return fail(E::invalid_record,n);
        node.bootstrap=true;
      }
      if(r.object_uuid==principal.principal_uuid){
        if(kind!=Kind::principal||!active||body.Boolean("active")!=true||body.Boolean("bootstrap_principal")!=false||
            body.Attribute("kind")!="service"||body.Attribute("principal_kind")!="service"||
            body.Unsigned("security_context_authority_version")!=1||
            body.Unsigned("security_context_generation")!=principal.security_context_generation||
            body.Unsigned("security_generation")!=principal.security_generation||body.Unsigned("policy_generation")!=principal.policy_generation||
            r.version_uuid!=principal.version_uuid||r.definition_generation!=principal.definition_generation)return fail(E::principal_invalid,n);
        selected=nodes;
      }
      ++nodes;continue;
    }
    // Active membership rows may omit effect: owning native bootstrap/import
    // schema uses positive links. An explicit effect must be exactly allow;
    // membership-deny semantics cannot silently turn into an allow edge.
    const auto cls=body.Attribute("grant_class");
    if(cls=="role_membership"||cls=="group_membership"){
      const auto member=body.Identity("member_uuid"),parent=body.Identity("parent_uuid");
      const auto mk=KindOf(body.Attribute("member_kind").value_or("")),pk=KindOf(body.Attribute("parent_kind").value_or(""));
      if(!id(member)||!id(parent)||mk==Kind::invalid||pk!=(cls=="role_membership"?Kind::role:Kind::group)||
          (body.Attribute("effect")&&body.Attribute("effect")!="allow")||
          (!body.Identity("membership_uuid").is_nil()&&body.Identity("membership_uuid")!=r.object_uuid)||
          (!body.Identity("principal_uuid").is_nil()&&(mk!=Kind::principal||body.Identity("principal_uuid")!=member))||
          (!body.Identity("role_uuid").is_nil()&&(pk!=Kind::role||body.Identity("role_uuid")!=parent))||
          !body.Identity("subject_uuid").is_nil()||!body.Identity("target_uuid").is_nil()||body.Attribute("right"))return fail(E::malformed_membership,n);
      if(active)w.edges_[edges++]={{r.object_uuid,member,parent,r.version_uuid,mk,pk,r.definition_generation,
          binding.security_epoch,r.security_epoch,r.catalog_generation}};
    }else if(cls=="privilege"){
      const auto subject=body.Identity("subject_uuid"),target=body.Identity("target_uuid");
      const auto sk=KindOf(body.Attribute("subject_kind").value_or(""));
      const auto effect=body.Attribute("effect"),right=body.Attribute("right");
      if(!id(subject)||(!target.is_nil()&&!id(target))||sk==Kind::invalid||!effect||(*effect!="allow"&&*effect!="deny")||!right||right->empty()||
          !body.Identity("member_uuid").is_nil()||!body.Identity("parent_uuid").is_nil()||!body.Identity("membership_uuid").is_nil()||
          !body.Identity("principal_uuid").is_nil()||!body.Identity("role_uuid").is_nil()||body.Attribute("member_kind")||body.Attribute("parent_kind"))return fail(E::malformed_grant,n);
      auto registered=w.rights_.end();
      for(auto candidate=w.rights_.begin();candidate!=w.rights_.end();++candidate){
        if(!spend())return fail(E::work_limit,n);
        if(*candidate==*right){registered=candidate;break;}
      }
      if(registered==w.rights_.end())return fail(E::unknown_right,n);
      if(active)w.grants_[grants++]={{r.object_uuid,subject,target,r.version_uuid,sk,*registered,*effect=="deny",r.definition_generation,
          binding.security_epoch,r.security_epoch,r.catalog_generation}};
    }else return fail(E::malformed_grant,n);
  }
  if(selected==absent)return fail(E::principal_invalid);
  const auto find=[&](const EngineUuid& u,Kind kind)->Size{
    Size lo=0,hi=nodes;
    while(lo<hi){if(!spend())return absent;const auto mid=lo+(hi-lo)/2;
      if(w.nodes_[mid].value.subject_uuid<u)lo=mid+1;else hi=mid;}
    return lo<nodes&&w.nodes_[lo].value.subject_uuid==u&&w.nodes_[lo].value.kind==kind?lo:absent;
  };
  for(Size e=0;e<edges;++e){auto& edge=w.edges_[e];
    edge.member=find(edge.value.member_uuid,edge.value.member_kind);edge.parent=find(edge.value.parent_uuid,edge.value.parent_kind);
    if(exhausted)return fail(E::work_limit);
    // Missing parents only refuse if reached, like the existing materializer.
    if(edge.member!=absent){edge.next=w.nodes_[edge.member].first;w.nodes_[edge.member].first=e;}
  }
  for(Size g=0;g<grants;++g){auto& grant=w.grants_[g];grant.subject=find(grant.value.subject_uuid,grant.value.subject_kind);
    if(exhausted)return fail(E::work_limit);}
  Size depth=1;w.nodes_[selected].color=1;w.frames_[0]={selected,w.nodes_[selected].first};
  while(depth){
    if(!spend())return fail(E::work_limit);auto& frame=w.frames_[depth-1];
    if(frame.next==absent){w.nodes_[frame.node].color=2;--depth;continue;}
    const auto& edge=w.edges_[frame.next];frame.next=edge.next;
    if(edge.parent==absent||!w.nodes_[edge.parent].active)return fail(E::subject_missing);
    auto& parent=w.nodes_[edge.parent];if(parent.color==1)return fail(E::membership_cycle);
    if(parent.color==2)continue;if(depth==w.limits_.maximum_depth)return fail(E::depth_limit);
    parent.color=1;w.frames_[depth++]={edge.parent,parent.first};
  }
  Size subject_count=0,edge_count=0,grant_count=0;EngineUuid bootstrap;
  for(Size n=0;n<nodes;++n){if(!spend())return fail(E::work_limit);
    if(w.nodes_[n].color==2){w.subjects_out_[subject_count++]=w.nodes_[n].value;if(w.nodes_[n].bootstrap)bootstrap=w.nodes_[n].value.subject_uuid;}}
  for(Size e=0;e<edges;++e){if(!spend())return fail(E::work_limit);
    if(w.edges_[e].member!=absent&&w.nodes_[w.edges_[e].member].color==2)w.edges_out_[edge_count++]=w.edges_[e].value;}
  for(Size g=0;g<grants;++g){if(!spend())return fail(E::work_limit);
    if(w.grants_[g].subject!=absent&&w.nodes_[w.grants_[g].subject].color==2)w.grants_out_[grant_count++]=w.grants_[g].value;}
  auto& result=out.projection;result.binding_=binding;result.principal_=principal;result.bootstrap_=bootstrap;
  result.subjects_=w.subjects_out_.first(subject_count);result.memberships_=w.edges_out_.first(edge_count);
  result.grants_=w.grants_out_.first(grant_count);result.rights_=w.rights_;out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
