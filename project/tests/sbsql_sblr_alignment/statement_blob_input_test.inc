// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
namespace scratchbird::core::datatypes {
class BlobLifetimeRuntimeConformanceAccessV3 final {
 public:
  static BlobTrustedInternalVisitorV3 Visitor(BlobTrustedInternalVisitorV3::Function f,void* context) noexcept {
    return BlobTrustedInternalVisitorV3(f,context);
  }
};
}
namespace blob_input_test {
namespace dt=scratchbird::core::datatypes;
struct Read {
  std::span<const platform::byte> expected;
  std::size_t at=0, calls=0;
  bool matches=true;
  static dt::BlobProtectedWorkResultV3 Visit(void* raw,std::span<const platform::byte> bytes) {
    auto& read=*static_cast<Read*>(raw); ++read.calls;
    read.matches=read.matches && read.at<=read.expected.size() && bytes.size()<=read.expected.size()-read.at;
    if(read.matches) read.matches=std::equal(bytes.begin(),bytes.end(),read.expected.begin()+read.at);
    read.at+=bytes.size();
    dt::BlobProtectedWorkResultV3 result; result.status=0; return result;
  }
};
api::StatementBlobInputBoundsV3 Bounds(bool logical=false) {
  api::StatementBlobInputBoundsV3 bounds;
  bounds.operation=SB_BLOB_OP_READ_V3;
  bounds.carrier=logical ? api::StatementBlobInputCarrierV3::logical_reader : api::StatementBlobInputCarrierV3::materialized;
  bounds.resident_bytes=1024*1024; bounds.valid_for_ns=60'000'000'000ULL;
  bounds.retain_tickets=16; bounds.access_tickets=128;
  bounds.maximum_work.fill(4*1024*1024);
  bounds.scratch_bytes=8192; bounds.window_bytes=4096; bounds.relative_timeout_ns=1'000'000'000;
  return bounds;
}
sb_engine_status_t Receive(Statement& statement,std::span<const platform::byte> bytes,
    const api::StatementBlobInputBoundsV3& bounds,api::StatementBlobInputV3& value) {
  const auto received=api::ReceiveStatementBlobInputV3(statement.handle,statement.context.session_uuid.bytes,
      statement.context.statement_receipt_uuid.bytes,bytes,bounds,&value);
  Check(received.ok()==(received.status==SB_ENGINE_STATUS_OK),"native BLOB admission status/fact mismatch");
  if(received.ok()) Check(received.copied_bytes==bytes.size() &&
      received.identities_issued==4ULL+bounds.retain_tickets+bounds.access_tickets,
      "native BLOB admission lost actual copied-byte/issued-identity observations");
  else Check(received.failure!=api::StatementBlobInputAdmissionFailureV3::none,
      "native BLOB admission lost its typed private failure");
  return received.status;
}
void SameMemory(const mm::MemoryAccountingSnapshot& before) {
  const auto after=mm::DefaultMemoryManager().Snapshot();
  Check(after.current_bytes==before.current_bytes && after.reserved_capacity_bytes==before.reserved_capacity_bytes &&
      after.active_capacity_reservation_count==before.active_capacity_reservation_count,
      "native BLOB input leaked actual physical memory/reservations");
}
void Values(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  Statement statement(session,context);
  std::vector<platform::byte> original(131079);
  for(std::size_t n=0;n<original.size();++n) original[n]=static_cast<platform::byte>(n*19);
  for(bool logical:{false,true}) for(unsigned state=0;state<3;++state) {
    const auto baseline=mm::DefaultMemoryManager().Snapshot();
    api::StatementBlobInputV3 value;
    auto bounds=Bounds(logical);
    if(state==2) bounds.state=dt::BlobValueStateV3::sql_null;
    auto input=state==0 ? original : std::vector<platform::byte>{};
    Check(Receive(statement,input,bounds,value)==SB_ENGINE_STATUS_OK && value.bound() && value.profile(),
        "native opaque receipt did not receive copied BLOB input");
    Check(value.profile()->receipt.catalog_snapshot_uuid.bytes==statement.context.datatype_catalog_snapshot_uuid.bytes &&
        value.profile()->receipt.catalog_generation==statement.context.datatype_catalog_generation &&
        value.profile()->receipt.registry_generation==statement.context.datatype_registry_generation,
        "native BLOB input did not retain the actual persisted cohort");
    std::fill(input.begin(),input.end(),0x5a);
    dt::BlobRetainedLifetimeLeaseV3 lease;
    allocation_count=0; allocation_budget=0;
    const auto retained=value.Retain(lease);
    const auto allocations=allocation_count; allocation_budget=-1;
    if(!retained.ok()) std::cerr<<"native BLOB retain: "<<retained.runtime.diagnostic_code<<':'<<retained.runtime.reason<<'\n';
    Check(retained.ok() && lease.retained() && allocations==0,"native BLOB retention allocated or failed");
    Read read{state==0 ? std::span<const platform::byte>(original) : std::span<const platform::byte>{}};
    auto visitor=dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(Read::Visit,&read);
    allocation_count=0; allocation_budget=0;
    // Like the owning Core scoped consumer, authenticate empty/NULL shortcuts
    // with a real probe; the low-level materialized visitor itself deliberately
    // does not choose that scalar consumer policy on its caller's behalf.
    const auto visited=state!=0 ? lease.Probe() : logical ? lease.VisitBoundReaderBytes(visitor) : lease.VisitBoundMaterializedBytes(visitor);
    const auto read_allocations=allocation_count; allocation_budget=-1;
    if(!visited.ok()) std::cerr<<"native BLOB visit: "<<visited.diagnostic_code<<':'<<visited.reason<<'\n';
    if(!visited.ok() || !read.matches || read.at!=(state==0 ? original.size() : 0) ||
        read_allocations!=0 || !(state==0 ? read.calls>0 : read.calls==0))
      std::cerr<<"native BLOB read state="<<state<<" logical="<<logical<<" matches="<<read.matches
          <<" bytes="<<read.at<<" calls="<<read.calls<<" allocations="<<read_allocations<<'\n';
    Check(visited.ok() && read.matches && read.at==(state==0 ? original.size() : 0) &&
        read_allocations==0 && (state==0 ? read.calls>0 : read.calls==0),
        "native guarded BLOB read lost bytes/NULL state or allocated");
    Check(lease.Release().ok(),"native BLOB exact retained cleanup failed");
    value.Reset(); Check(!value.bound() && !value.profile(),"native BLOB source reset did not disarm");
    SameMemory(baseline);
  }
}
void Retirement(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  Statement statement(session,context); const auto baseline=mm::DefaultMemoryManager().Snapshot();
  api::StatementBlobInputV3 value; const std::array<platform::byte,3> bytes{0,0xff,0x41};
  Check(Receive(statement,bytes,Bounds(),value)==SB_ENGINE_STATUS_OK,"native retired source admission");
  dt::BlobRetainedLifetimeLeaseV3 lease;
  Check(value.Retain(lease).ok(),"native source before public retirement");
  statement.Release();
  Check(lease.Probe().ok(),"ordinary public retirement invalidated actual retained BLOB input");
  value.Reset();
  Read read{bytes}; auto visitor=dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(Read::Visit,&read);
  Check(lease.VisitBoundMaterializedBytes(visitor).ok() && read.matches && read.at==bytes.size(),
      "source-root retirement reclaimed payload still owned by native factory pins");
  Check(lease.Release().ok(),"native retained source final pin cleanup");
  // Statement retirement also releases its separate native control backing;
  // compare the physical source grant independently while the statement lives
  // in Values(), rather than comparing this post-retirement total to baseline.
  Check(mm::DefaultMemoryManager().Snapshot().reserved_capacity_bytes<=baseline.reserved_capacity_bytes,
      "native retired source stranded physical capacity");
}
void Refusals(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  Statement statement(session,context);
  const std::array<platform::byte,1> bytes{0x71};
  const auto baseline=mm::DefaultMemoryManager().Snapshot();
  for(unsigned fault=0;fault<10;++fault) {
    auto bounds=Bounds(); api::StatementBlobInputV3 value;
    if(fault==0) bounds.operation=0;
    if(fault==1) bounds.operation=255;
    if(fault==2) bounds.carrier=static_cast<api::StatementBlobInputCarrierV3>(255);
    if(fault==3) bounds.state=static_cast<dt::BlobValueStateV3>(255);
    if(fault==4) bounds.state=dt::BlobValueStateV3::sql_null;
    if(fault==5) bounds.resident_bytes=0;
    if(fault==6) bounds.resident_bytes=1;
    if(fault==7) bounds.valid_for_ns=0;
    if(fault==8) bounds.retain_tickets=0;
    if(fault==9) bounds.valid_for_ns=UINT64_MAX;
    Check(Receive(statement,bytes,bounds,value)!=SB_ENGINE_STATUS_OK && !value.bound(),
        "invalid native BLOB source bounds published an owner");
    SameMemory(baseline);
  }
  for(std::size_t n=0;n<16;++n) {
    auto foreign=statement.context.session_uuid.bytes; foreign[n]^=1;
    api::StatementBlobInputV3 value;
    Check(api::ReceiveStatementBlobInputV3(statement.handle,foreign,statement.context.statement_receipt_uuid.bytes,
        bytes,Bounds(),&value).status==SB_ENGINE_STATUS_SECURITY_DENIED && !value.bound(),"foreign native BLOB session admitted");
    foreign=statement.context.statement_receipt_uuid.bytes; foreign[n]^=1;
    Check(api::ReceiveStatementBlobInputV3(statement.handle,statement.context.session_uuid.bytes,foreign,
        bytes,Bounds(),&value).status==SB_ENGINE_STATUS_INVALID_HANDLE && !value.bound(),"foreign native BLOB receipt admitted");
    SameMemory(baseline);
  }
  api::StatementBlobInputV3 bounded; auto bounds=Bounds(); bounds.retain_tickets=1;
  Check(Receive(statement,bytes,bounds,bounded)==SB_ENGINE_STATUS_OK,"bounded BLOB source admission");
  dt::BlobRetainedLifetimeLeaseV3 first,second;
  Check(bounded.Retain(first).ok() && first.Release().ok(),"bounded BLOB first actual ticket");
  Check(!bounded.Retain(second).ok() && !second.retained(),"consumed retain ticket was reused/reset");
  bounded.Reset(); SameMemory(baseline);
  api::StatementBlobInputV3 occupied;
  Check(Receive(statement,bytes,Bounds(),occupied)==SB_ENGINE_STATUS_OK,"native occupied source admission");
  const auto* profile=occupied.profile();
  const auto duplicate=api::ReceiveStatementBlobInputV3(statement.handle,statement.context.session_uuid.bytes,
      statement.context.statement_receipt_uuid.bytes,bytes,Bounds(),&occupied);
  Check(!duplicate.ok() && duplicate.failure==api::StatementBlobInputAdmissionFailureV3::output_in_use &&
      duplicate.copied_bytes==0 && duplicate.identities_issued==0 && occupied.profile()==profile,
      "native receiving overwrote an occupied source or fabricated work");
  occupied.Reset(); SameMemory(baseline);
}
void AllocationFailures(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  Statement statement(session,context);
  const std::array<platform::byte,3> bytes{0,0xff,0x41};
  // Warm native identity/provider state before measuring actual C++ admission
  // allocations. Every injected position must actually fire.
  api::StatementBlobInputV3 warm;
  Check(Receive(statement,bytes,Bounds(),warm)==SB_ENGINE_STATUS_OK,"native BLOB warm admission"); warm.Reset();
  const auto baseline=mm::DefaultMemoryManager().Snapshot();
  unsigned fired=0; bool completed=false;
  // Optional telemetry/provider paths can change their allocation count as
  // actual time advances. Sweep until an observed, non-faulted complete call;
  // count ONLY positions whose allocator actually fired, never pretend an
  // unreached calibrated ordinal was injected. Keep the existing deterministic
  // retained-memory admission sweep above unchanged.
  for(unsigned n=0;n<4096;++n) {
    api::StatementBlobInputV3 value;
    allocation_fault_triggered=false; allocation_budget=n;
    const auto status=Receive(statement,bytes,Bounds(),value);
    allocation_budget=-1;
    const bool faulted=allocation_fault_triggered;
    if(faulted) ++fired;
    else Check(status==SB_ENGINE_STATUS_OK && value.bound(),"non-faulted native BLOB admission failed");
    if(status==SB_ENGINE_STATUS_OK) {
      dt::BlobRetainedLifetimeLeaseV3 lease;
      Check(value.bound() && value.Retain(lease).ok() && lease.Release().ok(),
          "optional native allocation failure published incomplete BLOB source");
      value.Reset();
    } else Check(!value.bound(),"failed native BLOB source leaked a partial owner");
    SameMemory(baseline);
    if(!faulted) { completed=true; break; }
  }
  Check(fired>0 && completed,"native BLOB allocation sweep did not reach actual full publication");
  std::cout<<"actual native BLOB input allocation fault positions="<<fired<<'\n';
}
void IdentityCasts(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  namespace engine=scratchbird::engine;
  Statement statement(session,context);
  const auto baseline=mm::DefaultMemoryManager().Snapshot();
  const std::array<platform::byte,3> content{0,255,129};
  for(bool logical:{false,true}) for(unsigned state=0;state<3;++state) {
    api::StatementBlobInputV3 value; auto bounds=Bounds(logical); bounds.retain_tickets=4;
    if(state==2) bounds.state=dt::BlobValueStateV3::sql_null;
    Check(Receive(statement,state==0 ? std::span<const platform::byte>(content) : std::span<const platform::byte>{},bounds,value)==SB_ENGINE_STATUS_OK,
        "native identity source receiving");
    const auto profile=*value.profile();
    engine::ExecutionTypeDescriptor descriptor;
    std::copy(profile.identity.descriptor_uuid.bytes.begin(),profile.identity.descriptor_uuid.bytes.end(),descriptor.descriptor_uuid.bytes);
    descriptor.descriptor_epoch=profile.identity.descriptor_generation;
    descriptor.canonical_type_id=static_cast<std::uint32_t>(dt::CanonicalTypeId::blob);
    descriptor.family=engine::ExecutionTypeFamily::blob;
    descriptor.width_class=engine::ExecutionTypeWidthClass::descriptor_defined;
    auto target_descriptor=descriptor; target_descriptor.stable_name="localized-alias-is-data";
    dt::BlobRetainedLifetimeLeaseV3 source;
    std::array<dt::BlobRetainedLifetimeLeaseV3,3> targets;
    Check(value.Retain(source).ok(),"native identity source lease");
    const std::array contexts{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,dt::DatatypeCastContext::explicit_cast};
    for(std::size_t n=0;n<contexts.size();++n) {
      auto bad=target_descriptor; ++bad.descriptor_epoch;
      Check(!source.CastIdentityInto(profile,descriptor,bad,contexts[n],targets[n]).ok() && !targets[n].retained(),
          "native BLOB identity cast admitted a substituted datatype generation");
      allocation_count=0; allocation_budget=0;
      const auto cast=source.CastIdentityInto(profile,descriptor,target_descriptor,contexts[n],targets[n]);
      const auto allocated=allocation_count; allocation_budget=-1;
      Check(cast.ok() && targets[n].retained() && source.retained() && !allocated,
          "native BLOB identity cast lost distinct live ownership or allocated");
      Check(!source.CastIdentityInto(profile,descriptor,target_descriptor,contexts[n],targets[n]).ok() && targets[n].retained(),
          "native BLOB identity cast overwrote an armed destination");
    }
    Check(source.Release().ok(),"native identity original source release");
    value.Reset();
    for(auto& target:targets) {
      Check(target.Probe().ok(),"native identity result depended on released source wrapper");
      Check(target.Release().ok(),"native identity result final pin cleanup");
    }
    SameMemory(baseline);
  }
}
void Revocation(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  for(bool cancel:{false,true}) {
    Statement statement(session,context);
    const auto baseline=mm::DefaultMemoryManager().Snapshot();
    api::StatementBlobInputV3 value; const std::array<platform::byte,3> bytes{0,0xff,0x41};
    Check(Receive(statement,bytes,Bounds(true),value)==SB_ENGINE_STATUS_OK,"native revocation source");
    dt::BlobRetainedLifetimeLeaseV3 lease; Check(value.Retain(lease).ok(),"native revocation lease");
    if(cancel) {
      std::optional<api::RetainedStatementCancellationV1> signal;
      Check(api::RetainStatementCancellationV1(statement.handle,statement.context.session_uuid.bytes,
          statement.context.statement_receipt_uuid.bytes,&signal)==SB_ENGINE_STATUS_OK && signal->Request(),
          "native BLOB actual cancellation signal");
    } else mga::RevokePublishedSnapshotVector({platform::UuidKind::object,platform::Uuid{statement.context.statement_snapshot_uuid.bytes}});
    const auto refused=lease.Probe();
    Check(!refused.ok() && refused.diagnostic_code==(cancel ? "PROCESS.CANCELLED" : "CINL.LOB.HANDLE_EXPIRED"),
        "native BLOB lost actual cancellation versus MGA revocation classification");
    if(!cancel) Check(static_cast<unsigned>(refused.fact.parameters.lifetime_enum)==3 &&
        static_cast<unsigned>(refused.fact.parameters.expiry_reason_enum)==3,
        "native BLOB MGA expiry did not preserve exact stale-snapshot vector");
    if(lease.retained()) Check(lease.Release().ok(),"native revoked BLOB lease cleanup");
    value.Reset(); SameMemory(baseline);
  }
}
scratchbird::engine::ExecutionTypeDescriptor Descriptor(const dt::BlobValidatedProfileHandleV3& profile) {
  namespace engine=scratchbird::engine;
  engine::ExecutionTypeDescriptor descriptor;
  std::copy(profile.identity.descriptor_uuid.bytes.begin(),profile.identity.descriptor_uuid.bytes.end(),descriptor.descriptor_uuid.bytes);
  descriptor.descriptor_epoch=profile.identity.descriptor_generation;
  descriptor.canonical_type_id=static_cast<std::uint32_t>(dt::CanonicalTypeId::blob);
  descriptor.family=engine::ExecutionTypeFamily::blob;
  descriptor.width_class=engine::ExecutionTypeWidthClass::descriptor_defined;
  descriptor.stable_name="source presentation alias is not identity";
  return descriptor;
}
bool ExactDescriptor(const scratchbird::engine::ExecutionTypeDescriptor& a,
    const scratchbird::engine::ExecutionTypeDescriptor& b) {
  const auto uuid=[](const auto& x,const auto& y) { return std::equal(std::begin(x.bytes),std::end(x.bytes),std::begin(y.bytes)); };
  if(!uuid(a.descriptor_uuid,b.descriptor_uuid) || a.descriptor_epoch!=b.descriptor_epoch ||
      a.canonical_type_id!=b.canonical_type_id || a.family!=b.family || a.width_class!=b.width_class ||
      a.stable_name!=b.stable_name || a.bit_width!=b.bit_width || a.precision!=b.precision || a.scale!=b.scale ||
      a.length!=b.length || a.vector_dimensions!=b.vector_dimensions || a.container_rank!=b.container_rank ||
      a.modifier_flags!=b.modifier_flags || !uuid(a.domain_uuid,b.domain_uuid) || a.domain_stack.size()!=b.domain_stack.size() ||
      !uuid(a.charset_uuid,b.charset_uuid) || !uuid(a.collation_uuid,b.collation_uuid) || !uuid(a.timezone_uuid,b.timezone_uuid) ||
      !uuid(a.element_descriptor_uuid,b.element_descriptor_uuid) || !uuid(a.security_policy_uuid,b.security_policy_uuid) ||
      a.nullable_allowed!=b.nullable_allowed || a.descriptor_authoritative!=b.descriptor_authoritative ||
      a.parser_independent!=b.parser_independent) return false;
  for(std::size_t n=0;n<a.domain_stack.size();++n) if(!uuid(a.domain_stack[n],b.domain_stack[n])) return false;
  return true;
}
void TypedValues(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  Statement statement(session,context);
  const auto baseline=mm::DefaultMemoryManager().Snapshot();
  const std::array<platform::byte,3> content{0,255,129};
  for(bool logical:{false,true}) for(unsigned state=0;state<3;++state) {
    api::StatementBlobInputV3 input; auto bounds=Bounds(logical);
    if(state==2) bounds.state=dt::BlobValueStateV3::sql_null;
    const auto bytes=state==0 ? std::span<const platform::byte>{content} : std::span<const platform::byte>{};
    Check(Receive(statement,bytes,bounds,input)==SB_ENGINE_STATUS_OK,"typed native BLOB source receiving");
    auto expected_source=Descriptor(*input.profile());
    auto receiving_descriptor=expected_source;
    api::StatementBlobValueV3 source;
    // Every refusal preserves full caller metadata and produces no typed owner.
    for(unsigned fault=0;fault<8;++fault) {
      auto descriptor=expected_source;
      if(fault==0) ++descriptor.descriptor_epoch;
      if(fault==1) descriptor.canonical_type_id=static_cast<std::uint32_t>(dt::CanonicalTypeId::binary);
      if(fault==2) descriptor.family=scratchbird::engine::ExecutionTypeFamily::binary;
      if(fault==3) descriptor.width_class=scratchbird::engine::ExecutionTypeWidthClass::variable;
      if(fault==4) descriptor.domain_uuid.bytes[0]=1;
      if(fault==5) descriptor.charset_uuid.bytes[0]=1;
      if(fault==6) descriptor.descriptor_authoritative=false;
      if(fault==7) descriptor.parser_independent=false;
      const auto preserved=descriptor;
      api::StatementBlobValueV3 refused;
      const auto result=input.RetainTyped(std::move(descriptor),refused);
      Check(!result.ok() && result.failure==api::StatementBlobValueFailureV3::descriptor_invalid &&
          !result.lifetime.runtime_invoked && !refused.bound() && !refused.descriptor() && !refused.profile() &&
          !refused.state() && !refused.length() && ExactDescriptor(descriptor,preserved),
          "typed BLOB invalid descriptor reached lifetime or moved/published metadata");
    }
    allocation_count=0; allocation_budget=0;
    const auto retained=input.RetainTyped(std::move(receiving_descriptor),source);
    const auto allocations=allocation_count; allocation_budget=-1;
    Check(retained.ok() && source.bound() && !allocations && source.descriptor() &&
        ExactDescriptor(*source.descriptor(),expected_source) && source.state()==bounds.state && source.length()==bytes.size(),
        "typed native BLOB receiving lost exact owned descriptor/state/extent or allocated");
    std::array<api::StatementBlobValueV3,3> targets;
    const std::array contexts{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,dt::DatatypeCastContext::explicit_cast};
    for(std::size_t n=0;n<contexts.size();++n) {
      auto expected=expected_source; expected.stable_name="target localized display alias with independent owned storage";
      expected.nullable_allowed=state==2;
      auto bad=expected; ++bad.descriptor_epoch; const auto preserved=bad;
      const auto refused=source.CastIdentityInto(std::move(bad),contexts[n],targets[n]);
      Check(!refused.ok() && refused.lifetime.runtime_invoked && !targets[n].bound() &&
          !targets[n].descriptor() && ExactDescriptor(bad,preserved),"typed BLOB cast moved invalid target or published a value");
      auto target=expected;
      allocation_count=0; allocation_budget=0;
      const auto cast=source.CastIdentityInto(std::move(target),contexts[n],targets[n]);
      const auto allocated=allocation_count; allocation_budget=-1;
      Check(cast.ok() && cast.category==dt::DatatypeCastCategory::identity &&
          targets[n].bound() && source.bound() && !allocated && targets[n].descriptor() &&
          ExactDescriptor(*targets[n].descriptor(),expected) && targets[n].state()==bounds.state &&
          targets[n].length()==bytes.size(),"typed BLOB cast lost exact owned target/state/extent or allocated");
      // Mutating the caller's moved-from descriptor cannot mutate result data.
      target.stable_name="changed moved-from caller data";
      Check(ExactDescriptor(*targets[n].descriptor(),expected),"typed BLOB result borrowed caller metadata");
      auto replacement=expected;
      Check(!source.CastIdentityInto(std::move(replacement),contexts[n],targets[n]).ok() &&
          ExactDescriptor(replacement,expected) && ExactDescriptor(*targets[n].descriptor(),expected),
          "typed BLOB cast overwrote an armed destination or consumed failed target metadata");
    }
    auto bad_context_target=expected_source; api::StatementBlobValueV3 bad_context;
    Check(!source.CastIdentityInto(std::move(bad_context_target),static_cast<dt::DatatypeCastContext>(65535),bad_context).ok() &&
        ExactDescriptor(bad_context_target,expected_source) && !bad_context.bound(),"typed BLOB cast accepted undefined context");
    auto alias=expected_source;
    Check(!source.CastIdentityInto(std::move(alias),dt::DatatypeCastContext::implicit,source).ok() &&
        ExactDescriptor(alias,expected_source) && ExactDescriptor(*source.descriptor(),expected_source),"typed BLOB self-cast changed source");
    if(state==2) {
      auto nonnullable=expected_source; nonnullable.nullable_allowed=false;
      auto preserved=nonnullable; api::StatementBlobValueV3 refused;
      const auto receive=input.RetainTyped(std::move(nonnullable),refused);
      Check(!receive.ok() && receive.failure==api::StatementBlobValueFailureV3::sql_null_not_admitted &&
          !receive.lifetime.runtime_invoked && !refused.bound() && ExactDescriptor(nonnullable,preserved),
          "typed BLOB NULL receiving bypassed nullability or consumed metadata");
      const auto cast=source.CastIdentityInto(std::move(nonnullable),dt::DatatypeCastContext::assignment,refused);
      Check(!cast.ok() && cast.lifetime.runtime_invoked && cast.lifetime.runtime.diagnostic_code=="BLOB.STATE_INVALID" &&
          !refused.bound() && ExactDescriptor(nonnullable,preserved),"typed BLOB NULL cast lost actual nullability vector");
    }
    Check(source.Release().ok() && !source.bound() && !source.descriptor() && !source.profile() &&
        !source.state() && !source.length(),"typed BLOB source release retained public value metadata");
    auto consumed=expected_source;
    Check(!input.RetainTyped(std::move(consumed),source).ok() && ExactDescriptor(consumed,expected_source),
        "typed BLOB retired wrapper was rearmed or consumed caller metadata");
    input.Reset();
    for(auto& target:targets) {
      Read read{bytes}; auto visitor=dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(Read::Visit,&read);
      const auto result=state ? target.Probe() : logical ? target.VisitLogical(visitor) : target.VisitMaterialized(visitor);
      Check(result.ok() && read.matches && read.at==bytes.size() && target.descriptor() &&
          target.length()==bytes.size() && target.state()==bounds.state,
          "typed BLOB result depended on source wrapper or lost actual guarded contents");
      Check(target.Release().ok() && !target.bound() && !target.descriptor(),"typed BLOB result exact owner cleanup failed");
    }
    SameMemory(baseline);
  }
}
void ConcurrentReaders(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  Statement statement(session,context);
  const auto baseline=mm::DefaultMemoryManager().Snapshot();
  const std::vector<platform::byte> bytes(8193,0x93);
  for(bool logical:{false,true}) {
    api::StatementBlobInputV3 value; auto bounds=Bounds(logical); bounds.scratch_bytes=65536;
    Check(Receive(statement,bytes,bounds,value)==SB_ENGINE_STATUS_OK,"native concurrent BLOB source");
    std::barrier start(3); std::atomic<bool> failed{false};
    const auto worker=[&] {
      start.arrive_and_wait();
      for(unsigned n=0;n<4;++n) {
        dt::BlobRetainedLifetimeLeaseV3 lease;
        if(!value.Retain(lease).ok()) { failed=true; return; }
        Read read{bytes}; auto visitor=dt::BlobLifetimeRuntimeConformanceAccessV3::Visitor(Read::Visit,&read);
        const auto result=logical ? lease.VisitBoundReaderBytes(visitor) : lease.VisitBoundMaterializedBytes(visitor);
        if(!result.ok() || !read.matches || read.at!=bytes.size()) failed=true;
        if(lease.retained() && !lease.Release().ok()) failed=true;
      }
    };
    std::thread first(worker),second(worker); start.arrive_and_wait(); first.join(); second.join();
    Check(!failed,"native concurrent immutable BLOB sources crossed tickets, windows or bytes");
    value.Reset(); SameMemory(baseline);
  }
}
void ForeignProcess(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
#if defined(__linux__)
  Statement statement(session,context);
  std::barrier held(2),release(2); std::atomic<bool> entered{false};
  sb_engine_status_t scratch_status=SB_ENGINE_STATUS_INTERNAL_ERROR;
  std::thread holder([&] {
    scratch_status=api::RunWithStatementScratchMemoryV1(statement.context,[&](api::StatementScratchMemoryV1&) {
      entered=true; held.arrive_and_wait(); release.arrive_and_wait();
    });
    if(!entered) held.arrive_and_wait();
  });
  held.arrive_and_wait();
  if(!entered) { holder.join(); Check(false,"native fork lock fixture did not enter actual scratch scope"); }
  const auto child=::fork();
  if(child==0) {
    // The receipt mutex is held by a vanished parent thread in this process.
    // Refusal must precede that lock; inherited allocator owners are NEVER
    // destroyed in the child. Supported lifetime is fork followed by exec/exit.
    api::StatementBlobInputV3 value;
    const std::array<platform::byte,1> bytes{0x71};
    const auto received=api::ReceiveStatementBlobInputV3(statement.handle,statement.context.session_uuid.bytes,
        statement.context.statement_receipt_uuid.bytes,bytes,Bounds(),&value);
    ::_exit(received.status==SB_ENGINE_STATUS_INVALID_HANDLE &&
        received.failure==api::StatementBlobInputAdmissionFailureV3::clock_admission && !value.bound() &&
        !received.copied_bytes && !received.identities_issued ? 0 : 81);
  }
  // Keep the parent's mutex held while the child proves its early refusal.
  int status=0; const auto waited=child>0 ? ::waitpid(child,&status,0) : -1;
  release.arrive_and_wait(); holder.join();
  Check(child>0 && waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0 && scratch_status==SB_ENGINE_STATUS_OK,
      "foreign native BLOB receiving entered an inherited receipt lock");
  api::StatementBlobInputV3 parent; const std::array<platform::byte,1> bytes{0x71};
  Check(Receive(statement,bytes,Bounds(),parent)==SB_ENGINE_STATUS_OK,"parent source admission damaged by fork refusal");
#else
  (void)session; (void)context;
#endif
}
}
void NativeBlobInput(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  blob_input_test::Values(session,context);
  blob_input_test::Retirement(session,context);
  blob_input_test::Refusals(session,context);
  blob_input_test::AllocationFailures(session,context);
  blob_input_test::IdentityCasts(session,context);
  blob_input_test::TypedValues(session,context);
  blob_input_test::Revocation(session,context);
  blob_input_test::ConcurrentReaders(session,context);
  blob_input_test::ForeignProcess(session,context);
}
