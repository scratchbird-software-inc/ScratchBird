// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Included in blob_input_test after the real native receiving helpers.
#include "base_blob_component_vectors.inc"
std::vector<platform::byte> LogicalHashGolden(unsigned index) {
  const auto hex=kBlobComponentGoldenVectors[index].component_hex;
  const auto nibble=[](char c)->platform::byte {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    throw std::runtime_error("invalid independent BLOB hash vector");
  };
  std::vector<platform::byte> out(hex.size()/2);
  for(std::size_t i=0;i<out.size();++i) out[i]=static_cast<platform::byte>((nibble(hex[2*i])<<4)|nibble(hex[2*i+1]));
  return out;
}
bool EmptyHash(const api::StatementBlobHashResultV3& result) {
  return std::all_of(result.digest.begin(),result.digest.end(),[](auto byte){return byte==0;});
}
void LogicalHashes(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  Statement statement(session,context); const auto baseline=mm::DefaultMemoryManager().Snapshot();
  for(bool logical:{false,true}) for(unsigned index=1;index<5;++index) {
    const auto frame=LogicalHashGolden(index);
    const auto bytes=std::span<const platform::byte>(frame).subspan(320);
    api::StatementBlobInputV3 input; auto bounds=Bounds(logical); bounds.operation=SB_BLOB_OP_HASH_V3;
    bounds.maximum_work[static_cast<std::size_t>(dt::BlobLifetimeResourceV3::sink_bytes_written)-1]=0;
    bounds.maximum_work[static_cast<std::size_t>(dt::BlobLifetimeResourceV3::sink_calls)-1]=0;
    bounds.maximum_work[static_cast<std::size_t>(dt::BlobLifetimeResourceV3::encoded_bytes_read)-1]=0;
    Check(Receive(statement,bytes,bounds,input)==SB_ENGINE_STATUS_OK,"actual native hash input");
    api::StatementBlobValueV3 source; auto descriptor=Descriptor(*input.profile());
    Check(input.RetainTyped(std::move(descriptor),source).ok(),"actual native hash typed source");
    const auto work_before=*input.WorkSnapshot();
    const auto before=mm::DefaultMemoryManager().Snapshot();
    const auto result=source.HashLogical();
    const auto after=mm::DefaultMemoryManager().Snapshot();
    const auto work_after=*input.WorkSnapshot();
    if(!result.ok() || result.bytes_visited!=bytes.size() ||
        !std::equal(result.digest.begin(),result.digest.end(),frame.begin()+248) ||
        after.allocation_count!=before.allocation_count+1 || after.current_bytes!=before.current_bytes ||
        after.active_allocation_count!=before.active_allocation_count)
      std::cerr<<"native hash logical="<<logical<<" index="<<index<<" failure="<<static_cast<unsigned>(result.failure)
          <<" lifetime="<<result.lifetime.diagnostic_code<<':'<<result.lifetime.reason
          <<" computation="<<static_cast<unsigned>(result.computation.failure)
          <<" resource="<<static_cast<unsigned>(result.resource)<<" required="<<result.required<<" available="<<result.available
          <<" visited="<<result.bytes_visited<<" allocations="<<(after.allocation_count-before.allocation_count)
          <<" current="<<before.current_bytes<<'/'<<after.current_bytes
          <<" active="<<before.active_allocation_count<<'/'<<after.active_allocation_count<<'\n';
    Check(result.ok() && result.bytes_visited==bytes.size() &&
        std::equal(result.digest.begin(),result.digest.end(),frame.begin()+248) &&
        after.allocation_count==before.allocation_count+1 && after.current_bytes==before.current_bytes &&
        after.active_allocation_count==before.active_allocation_count,
        "native logical hash lost exact independent digest, duplicated workspace or leaked allocation");
    const auto delta=[&](dt::BlobLifetimeResourceV3 resource) {
      const auto n=static_cast<std::size_t>(resource);
      return work_after[n].invoked-work_before[n].invoked;
    };
    Check(delta(dt::BlobLifetimeResourceV3::logical_bytes_read)==bytes.size() &&
        delta(dt::BlobLifetimeResourceV3::reader_calls)==(logical && !bytes.empty() ? 1 : 0) &&
        !delta(dt::BlobLifetimeResourceV3::encoded_bytes_read) &&
        !delta(dt::BlobLifetimeResourceV3::sink_bytes_written) && !delta(dt::BlobLifetimeResourceV3::sink_calls),
        "native hash replayed/copied/encoded content or used a sink instead of one guarded pass");
    input.Reset();
    const auto retired=source.HashLogical();
    Check(retired.ok() && retired.digest==result.digest,"retired input root revoked an already-retained hash operation");
    Check(source.Release().ok(),"actual native hash source cleanup");
    SameMemory(baseline);
  }
  // Multiple reader windows and genuinely independent retained leases share
  // only immutable content and the actual atomic ledger, not scratch/leases.
  {
    const std::vector<platform::byte> bytes(131079,0xb7);
    api::StatementBlobInputV3 input; auto bounds=Bounds(true); bounds.operation=SB_BLOB_OP_HASH_V3;
    Check(Receive(statement,bytes,bounds,input)==SB_ENGINE_STATUS_OK,"concurrent native hash receiving");
    api::StatementBlobValueV3 first,second; auto a=Descriptor(*input.profile()),b=a;
    Check(input.RetainTyped(std::move(a),first).ok() && input.RetainTyped(std::move(b),second).ok(),
        "concurrent native hash actual independent owners");
    std::array<api::StatementBlobHashResultV3,2> results;
    std::thread one([&]{results[0]=first.HashLogical();});
    std::thread two([&]{results[1]=second.HashLogical();});
    one.join(); two.join();
    Check(results[0].ok() && results[1].ok() && results[0].digest==results[1].digest &&
        results[0].bytes_visited==bytes.size() && results[1].bytes_visited==bytes.size(),
        "concurrent native hash changed bytes, borrowed scratch or lost source lifetime");
    Check(first.Release().ok() && second.Release().ok(),"concurrent native hash source cleanup");
    input.Reset(); SameMemory(baseline);
  }
}
void LogicalHashFailures(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  using Failure=api::StatementBlobHashFailureV3;
  Statement statement(session,context); const auto baseline=mm::DefaultMemoryManager().Snapshot();
  api::StatementBlobValueV3 absent;
  Check(absent.HashLogical().failure==Failure::source_unbound,"unbound native hash source admitted");
  for(unsigned which=0;which<5;++which) {
    const std::array<platform::byte,3> bytes{0,255,129};
    api::StatementBlobInputV3 input; auto bounds=Bounds(true); bounds.operation=SB_BLOB_OP_HASH_V3;
    if(which==0) bounds.operation=SB_BLOB_OP_READ_V3;
    if(which==1) bounds.state=dt::BlobValueStateV3::sql_null;
    const auto resource=which==2 ? dt::BlobLifetimeResourceV3::logical_bytes_read :
        which==3 ? dt::BlobLifetimeResourceV3::reader_calls : dt::BlobLifetimeResourceV3::temporary_bytes;
    if(which>=2) bounds.maximum_work[static_cast<std::size_t>(resource)-1]=0;
    Check(Receive(statement,which==1 ? std::span<const platform::byte>{} : std::span<const platform::byte>(bytes),
        bounds,input)==SB_ENGINE_STATUS_OK,"native hash failure input");
    api::StatementBlobValueV3 source; auto descriptor=Descriptor(*input.profile());
    Check(input.RetainTyped(std::move(descriptor),source).ok(),"native hash failure actual typed source");
    const auto before=mm::DefaultMemoryManager().Snapshot();
    const auto result=source.HashLogical();
    Check(!result.ok() && EmptyHash(result) && !result.bytes_visited &&
        result.failure==(which==0 ? Failure::operation_not_admitted :
            which==1 ? Failure::sql_null_has_no_content_hash : Failure::resource_preflight) &&
        (which<2 || (result.resource==resource && result.required>0 && result.available==0)) &&
        mm::DefaultMemoryManager().Snapshot().allocation_count==before.allocation_count,
        "native hash bypassed exact operation/state/zero-resource admission or invented hash/work");
    Check(source.Release().ok(),"native hash refusal cleanup");
    input.Reset(); SameMemory(baseline);
  }
  const auto golden=LogicalHashGolden(3);
  const auto fault_content=std::span<const platform::byte>(golden).subspan(320);
  unsigned fired=0,failed=0,telemetry_only=0; bool completed=false;
  for(long fault=0;fault<8192;++fault) {
    {
      api::StatementBlobInputV3 input; auto bounds=Bounds(true); bounds.operation=SB_BLOB_OP_HASH_V3;
      Check(Receive(statement,fault_content,bounds,input)==SB_ENGINE_STATUS_OK,"native hash fault receiving");
      api::StatementBlobValueV3 source; auto descriptor=Descriptor(*input.profile());
      Check(input.RetainTyped(std::move(descriptor),source).ok(),"native hash fault actual owner");
      // Last-source custody must also be safe if the reader consumes its
      // retained lease on a later failed native gate.
      input.Reset();
      const auto before=mm::DefaultMemoryManager().Snapshot();
      allocation_count=0; allocation_fault_triggered=false; allocation_budget=fault;
      const auto result=source.HashLogical();
      allocation_budget=-1; const bool faulted=allocation_fault_triggered;
      const auto after=mm::DefaultMemoryManager().Snapshot();
      if(faulted) {
        ++fired;
        if(result.ok()) {
          ++telemetry_only;
          // AllocateImpl commits actual ownership before optional telemetry.
          // A fault in that later publication must not fabricate an operational
          // failure, erase real work or leak its already-owned physical buffer.
          Check(after.telemetry_truncation_count>before.telemetry_truncation_count &&
              result.bytes_visited==fault_content.size() &&
              std::equal(result.digest.begin(),result.digest.end(),golden.begin()+248),
              "faulted native hash succeeded without exact independent digest and observed telemetry loss");
        } else {
          ++failed; Check(EmptyHash(result),"actual failed hash allocation exposed a digest");
        }
      }
      else { completed=true; Check(result.ok() && result.bytes_visited==fault_content.size() &&
          std::equal(result.digest.begin(),result.digest.end(),golden.begin()+248),"nonfaulted native hash failed"); }
      Check(after.current_bytes==before.current_bytes && after.active_allocation_count==before.active_allocation_count,
          "native hash allocation/telemetry fault stranded physical workspace");
      if(source.bound()) Check(source.Release().ok(),"native hash fault source cleanup");
    }
    SameMemory(baseline);
    if(completed) break;
  }
  Check(failed>0 && telemetry_only>0 && fired==failed+telemetry_only && completed,
      "native hash allocation sweep did not prove actual operational/telemetry faults and complete");
  std::cout<<"actual native hash allocation fault positions="<<fired
      <<" operational_refusals="<<failed<<" telemetry_only="<<telemetry_only<<'\n';
  bool refused=false,succeeded=false;
  for(std::uint64_t capacity=8192;capacity<=65536;capacity+=512) {
    {
      const std::array<platform::byte,3> bytes{0,255,129};
      api::StatementBlobInputV3 input; auto bounds=Bounds(true); bounds.operation=SB_BLOB_OP_HASH_V3;
      bounds.resident_bytes=capacity;
      if(Receive(statement,bytes,bounds,input)==SB_ENGINE_STATUS_OK) {
        api::StatementBlobValueV3 source; auto descriptor=Descriptor(*input.profile());
        Check(input.RetainTyped(std::move(descriptor),source).ok(),"bounded native hash actual owner");
        const auto result=source.HashLogical();
        if(result.ok()) succeeded=true;
        else {
          Check(result.failure==Failure::allocation_failed && EmptyHash(result) && !result.bytes_visited,
              "bounded physical hash did not preserve actual allocation refusal");
          refused=true;
        }
        if(source.bound()) Check(source.Release().ok(),"bounded native hash source cleanup");
        input.Reset();
      }
    }
    SameMemory(baseline);
    if(succeeded) break;
  }
  Check(refused && succeeded,"native hash capacity sweep did not prove actual physical refusal and success");
}
void LogicalHashRevocation(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  for(bool cancel:{false,true}) {
    Statement statement(session,context); const auto baseline=mm::DefaultMemoryManager().Snapshot();
    const std::array<platform::byte,3> bytes{0,255,129};
    api::StatementBlobInputV3 input; auto bounds=Bounds(true); bounds.operation=SB_BLOB_OP_HASH_V3;
    Check(Receive(statement,bytes,bounds,input)==SB_ENGINE_STATUS_OK,"native hash revocation receiving");
    api::StatementBlobValueV3 source; auto descriptor=Descriptor(*input.profile());
    Check(input.RetainTyped(std::move(descriptor),source).ok(),"native hash revocation actual owner");
    input.Reset();
    if(cancel) {
      std::optional<api::RetainedStatementCancellationV1> signal;
      Check(api::RetainStatementCancellationV1(statement.handle,statement.context.session_uuid.bytes,
          statement.context.statement_receipt_uuid.bytes,&signal)==SB_ENGINE_STATUS_OK && signal->Request(),
          "native hash actual cancellation request");
    } else mga::RevokePublishedSnapshotVector({platform::UuidKind::object,platform::Uuid{statement.context.statement_snapshot_uuid.bytes}});
    const auto result=source.HashLogical();
    Check(!result.ok() && result.failure==api::StatementBlobHashFailureV3::lifetime && result.lifetime_invoked &&
        EmptyHash(result) && !result.bytes_visited &&
        result.lifetime.diagnostic_code==(cancel ? "PROCESS.CANCELLED" : "CINL.LOB.HANDLE_EXPIRED"),
        "native hash ignored actual cancellation/MGA revocation or exposed a digest");
    if(source.bound()) Check(source.Release().ok(),"native hash revoked source cleanup");
    SameMemory(baseline);
  }
}
