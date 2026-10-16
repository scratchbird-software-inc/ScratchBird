// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/statement_blob_input.hpp"
#include "engine/statement_cancellation.hpp"
#include "engine/statement_monotonic_clock.hpp"
#include "core/uuid/uuid.hpp"
#include <algorithm>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <new>
#include <vector>

namespace scratchbird::core::datatypes {
namespace {
namespace api = engine::internal_api;
BlobUuid16V3 AbiUuid(const platform::Uuid& id) noexcept {
  BlobUuid16V3 out{}; std::copy(id.bytes.begin(), id.bytes.end(), out.bytes); return out;
}
BlobUuid16V3 AbiUuid(const wire::TypedResultUuid& id) noexcept {
  return AbiUuid(platform::Uuid{id});
}
bool Equal(const BlobUuid16V3& a, const BlobUuid16V3& b) noexcept {
  return std::equal(std::begin(a.bytes),std::end(a.bytes),std::begin(b.bytes));
}
bool Before(const BlobUuid16V3& a,const BlobUuid16V3& b) noexcept {
  return std::memcmp(a.bytes,b.bytes,sizeof(a.bytes))<0;
}
BlobLifetimeCallbackResultV3 Code(u8 code) noexcept { return {code,{}}; }
void Erase(void* p, std::size_t n) noexcept {
  auto* b = static_cast<volatile byte*>(p); while(n--) *b++=0;
}
bool Range(std::span<const byte> input) noexcept {
  return input.empty() || (input.data() && input.size() <= UINTPTR_MAX-reinterpret_cast<std::uintptr_t>(input.data()));
}
} // namespace

// The only production capability constructor. It owns COPIED statement input,
// never a locator or permission to read another object's persisted content.
// Its two Core factory pins retain the same physical owner, budget and services.
class BlobLifetimeReceiverHostV3Generation1 final {
  struct Ticket {
    BlobUuid16V3 uuid{};
    bool used=false, released=false;
    BlobLifetimeAccessTicketV3 access{};
  };
  std::unique_ptr<api::RetainedStatementMemoryV1> memory_;
  std::optional<api::RetainedStatementCancellationV1> cancellation_;
  std::optional<api::RetainedStatementMonotonicClockV1> clock_;
  std::optional<std::pmr::vector<byte>> content_, scratch_;
  std::optional<std::pmr::vector<Ticket>> tickets_;
  std::optional<std::pmr::vector<BlobUuid16V3>> access_ids_;
  std::atomic<u64> pins_{1};
  std::atomic<bool> closing_{false}, quarantined_{false};
  std::mutex mutex_;
  std::size_t next_ticket_=0, next_access_=0;
  std::size_t active_accesses_=0;
  std::array<BlobLifetimeInvariantFactV3,16> invariants_{};
  std::size_t invariant_count_=0;
  BlobValidatedProfileHandleV3 profile_{};
  api::StatementBlobInputBoundsV3 bounds_{};
  BlobLifetimeBudgetLedgerV3Generation1 ledger_;
  BlobLifetimeTokenV3 token_{};
  BlobLifetimeUseRequestV3 request_{};
  BlobCarrierBindingV3 carrier_{};
  BlobLifetimeAuthorityV3 authority_{};
  BaseBlobLifetimeReceiverServicesV3Generation1 services_{};

  BlobLifetimeReceiverHostV3Generation1() noexcept = default;
  ~BlobLifetimeReceiverHostV3Generation1() noexcept {
    if(content_) Erase(content_->data(),content_->size());
    if(scratch_) Erase(scratch_->data(),scratch_->size());
    if(tickets_) Erase(tickets_->data(),tickets_->size()*sizeof(Ticket));
    if(access_ids_) Erase(access_ids_->data(),access_ids_->size()*sizeof(BlobUuid16V3));
  }
  void Drop() noexcept {
    const auto before=pins_.fetch_sub(1,std::memory_order_acq_rel);
    if(before==0) std::terminate();
    if(before!=1) return;
    if(active_accesses_!=0 || (tickets_ && std::any_of(tickets_->begin(),tickets_->end(),
        [](const auto& ticket) { return ticket.used && !ticket.released; }))) std::terminate();
    // Payload/control destructors run while their exact physical grant lives.
    auto owner=std::move(memory_);
    auto* allocator=owner->memory();
    this->~BlobLifetimeReceiverHostV3Generation1();
    Erase(this,sizeof(*this));
    try { allocator->deallocate(this,sizeof(*this),alignof(BlobLifetimeReceiverHostV3Generation1)); }
    catch(...) { std::terminate(); }
    if(owner->Release()!=SB_ENGINE_STATUS_OK) std::terminate();
  }
  static BlobReceiverPinCloneResultV3 AcquirePin(void* raw) noexcept {
    auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    if(host.closing_.load(std::memory_order_acquire)) return {BlobReceiverPinAcquireStatusV3::draining,{},0};
    auto value=host.pins_.load(std::memory_order_relaxed);
    do {
      if(value==0 || value==UINT64_MAX) return {};
    } while(!host.pins_.compare_exchange_weak(value,value+1,std::memory_order_acq_rel));
    if(host.closing_.load(std::memory_order_acquire)) {
      host.Drop(); return {BlobReceiverPinAcquireStatusV3::draining,{},0};
    }
    return {BlobReceiverPinAcquireStatusV3::acquired,{},1};
  }
  static BlobReceiverPinCloneResultV3 ClonePin(void* raw,u64 cookie) noexcept {
    return cookie==1 ? AcquirePin(raw) : BlobReceiverPinCloneResultV3{};
  }
  static void DropPin(void* raw,u64 cookie) noexcept {
    if(cookie!=1) std::terminate();
    static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw)->Drop();
  }
  BlobReceiverLifetimePinOpsV3Generation1 Pins() noexcept {
    return {this,AcquirePin,ClonePin,DropPin};
  }
  u8 Live() const noexcept {
    if(quarantined_.load(std::memory_order_acquire)) return SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3;
    switch(memory_->Observe()) {
      case api::RetainedStatementMemoryObservationV1::live: return SB_BLOB_CALLBACK_OK_V3;
      case api::RetainedStatementMemoryObservationV1::receipt_revoked: return SB_BLOB_CALLBACK_REVOKED_V3;
      case api::RetainedStatementMemoryObservationV1::snapshot_revoked: return SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3;
      case api::RetainedStatementMemoryObservationV1::released: return SB_BLOB_CALLBACK_CLOSED_V3;
      default: return SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3;
    }
  }
  u8 Binding(const BlobLifetimeTokenV3* token,const BlobLifetimeUseRequestV3* request) const noexcept {
    if(!token || !request) return SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3;
    if(std::memcmp(token,&token_,sizeof(token_))!=0) return SB_BLOB_CALLBACK_UNKNOWN_TOKEN_V3;
    if(std::memcmp(request,&request_,sizeof(request_))!=0) return SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3;
    return Live();
  }
  Ticket* Find(const BlobLifetimeRetainTicketV3* ticket) noexcept {
    if(!ticket || !Equal(ticket->lifetime_token_uuid,token_.lifetime_token_uuid) ||
        ticket->lifetime_token_generation!=token_.lifetime_token_generation ||
        ticket->immutable_binding_generation!=token_.immutable_binding_generation) return nullptr;
    const auto end=tickets_->begin()+next_ticket_;
    const auto found=std::lower_bound(tickets_->begin(),end,ticket->retain_ticket_uuid,
        [](const auto& slot,const auto& id) { return Before(slot.uuid,id); });
    return found!=end && found->used && Equal(found->uuid,ticket->retain_ticket_uuid) ? &*found : nullptr;
  }
  static BlobLifetimeCallbackResultV3 RetainCallback(void* raw,const BlobLifetimeTokenV3* token,
      const BlobLifetimeUseRequestV3* request,BlobLifetimeRetainTicketV3* out) {
    if(!out) return Code(SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3);
    *out={}; auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    std::lock_guard lock(host.mutex_);
    const auto admitted=host.Binding(token,request);
    if(admitted) return Code(admitted);
    if(host.closing_.load()) return Code(SB_BLOB_CALLBACK_CLOSED_V3);
    if(host.next_ticket_==host.tickets_->size()) return Code(SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3);
    auto& slot=(*host.tickets_)[host.next_ticket_++];
    slot.used=true;
    *out={slot.uuid,host.token_.lifetime_token_uuid,host.token_.lifetime_token_generation,
          host.token_.immutable_binding_generation};
    const auto after=host.Live();
    if(after) {
      // A failed callback cannot leave a hidden retain obligation or dirty
      // output. The identity remains consumed and is never recycled.
      slot.released=true; *out={}; return Code(after);
    }
    return Code(SB_BLOB_CALLBACK_OK_V3);
  }
  static BlobLifetimeCallbackResultV3 ProbeCallback(void* raw,const BlobLifetimeTokenV3* token,
      const BlobLifetimeRetainTicketV3* ticket,const BlobLifetimeUseRequestV3* request) {
    auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    std::lock_guard lock(host.mutex_);
    const auto admitted=host.Binding(token,request); if(admitted) return Code(admitted);
    const auto* slot=host.Find(ticket);
    if(!slot) return Code(SB_BLOB_CALLBACK_RETAINED_TICKET_INVALID_V3);
    if(slot->released) return Code(SB_BLOB_CALLBACK_RETAINED_TICKET_CONSUMED_V3);
    return Code(host.Live());
  }
  static BlobLifetimeCallbackResultV3 BeginCallback(void* raw,const BlobLifetimeTokenV3* token,
      const BlobLifetimeRetainTicketV3* ticket,const BlobLifetimeUseRequestV3* request,
      BlobLifetimeAccessTicketV3* out) {
    if(!out) return Code(SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3);
    *out={}; auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    std::lock_guard lock(host.mutex_);
    const auto admitted=host.Binding(token,request); if(admitted) return Code(admitted);
    auto* slot=host.Find(ticket);
    if(!slot) return Code(SB_BLOB_CALLBACK_RETAINED_TICKET_INVALID_V3);
    if(slot->released) return Code(SB_BLOB_CALLBACK_RETAINED_TICKET_CONSUMED_V3);
    const BlobUuid16V3 nil{};
    if(!Equal(slot->access.access_ticket_uuid,nil)) return Code(SB_BLOB_CALLBACK_ACCESS_TICKET_INVALID_V3);
    if(host.next_access_==host.access_ids_->size()) return Code(SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3);
    slot->access={(*host.access_ids_)[host.next_access_++],slot->uuid,
        host.token_.lifetime_token_generation,host.token_.immutable_binding_generation};
    ++host.active_accesses_;
    *out=slot->access;
    const auto after=host.Live();
    if(after) {
      --host.active_accesses_; slot->access={}; *out={}; return Code(after);
    }
    return Code(SB_BLOB_CALLBACK_OK_V3);
  }
  static BlobLifetimeCallbackResultV3 EndCallback(void* raw,const BlobLifetimeTokenV3* token,
      const BlobLifetimeRetainTicketV3* ticket,BlobLifetimeAccessTicketV3* access,
      const BlobLifetimeUseRequestV3*) {
    auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    std::lock_guard lock(host.mutex_);
    // Cleanup deliberately ignores expiry, revocation, cancellation and closed
    // admission. It consumes only the exact actual outstanding obligation.
    if(!token || std::memcmp(token,&host.token_,sizeof(*token))!=0 || !access)
      return Code(SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3);
    auto* slot=host.Find(ticket); const BlobUuid16V3 nil{};
    if(!slot || slot->released || Equal(slot->access.access_ticket_uuid,nil) ||
        std::memcmp(access,&slot->access,sizeof(*access))!=0)
      return Code(SB_BLOB_CALLBACK_ACCESS_TICKET_INVALID_V3);
    if(host.active_accesses_==0) std::terminate();
    --host.active_accesses_;
    slot->access={}; *access={}; return Code(SB_BLOB_CALLBACK_OK_V3);
  }
  static BlobLifetimeCallbackResultV3 ReleaseCallback(void* raw,const BlobLifetimeTokenV3* token,
      BlobLifetimeRetainTicketV3* ticket) {
    auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    std::lock_guard lock(host.mutex_);
    if(!token || std::memcmp(token,&host.token_,sizeof(*token))!=0)
      return Code(SB_BLOB_CALLBACK_UNKNOWN_TOKEN_V3);
    auto* slot=host.Find(ticket); const BlobUuid16V3 nil{};
    if(!slot) return Code(SB_BLOB_CALLBACK_RETAINED_TICKET_INVALID_V3);
    if(slot->released) return Code(SB_BLOB_CALLBACK_RETAINED_TICKET_CONSUMED_V3);
    if(!Equal(slot->access.access_ticket_uuid,nil)) return Code(SB_BLOB_CALLBACK_ACCESS_TICKET_INVALID_V3);
    slot->released=true; *ticket={}; return Code(SB_BLOB_CALLBACK_OK_V3);
  }
  static BlobMonotonicReadResultV3 Clock(void* raw) noexcept {
    const auto sample=static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw)->clock_->Sample();
    return sample.ok() ? BlobMonotonicReadResultV3{BlobReceiverSampleStatusV3::ok,{},sample.nanoseconds} :
                       BlobMonotonicReadResultV3{};
  }
  static BlobCancellationSampleResultV3 Cancel(void* raw,u8,BlobLifetimePublicPhaseV3) noexcept {
    const auto observed=static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw)->cancellation_->Observe();
    if(observed==api::StatementCancellationObservationV1::source_unavailable) return {};
    return {BlobReceiverSampleStatusV3::ok,static_cast<u8>(observed==api::StatementCancellationObservationV1::requested),{}};
  }
  static BlobReceiverEffectResultV3 Invariant(void* raw,const BlobLifetimeBindingKeyV3& key,
      const BlobLifetimeInvariantFactV3& fact) {
    auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    std::lock_guard lock(host.mutex_);
    if(key.authority_instance_uuid!=platform::Uuid{host.carrier_.authority_instance_uuid} ||
        key.authority_instance_generation!=host.token_.authority_instance_generation ||
        key.lifetime_token_uuid!=host.carrier_.lifetime_token_uuid ||
        key.lifetime_token_generation!=host.token_.lifetime_token_generation ||
        key.immutable_binding_generation!=host.token_.immutable_binding_generation ||
        !ValidateBlobLifetimeInvariantFactDomainV3(key,fact)) return {};
    for(std::size_t n=0;n<host.invariant_count_;++n) {
      const auto& old=host.invariants_[n];
      if(old.event==fact.event && old.operation_enum==fact.operation_enum && old.phase==fact.phase &&
          old.callback_code_or_255==fact.callback_code_or_255 &&
          old.authority_instance_generation==fact.authority_instance_generation &&
          old.lifetime_token_generation==fact.lifetime_token_generation &&
          old.immutable_binding_generation==fact.immutable_binding_generation)
        return {BlobReceiverEffectStatusV3::already_applied,{}};
    }
    if(host.invariant_count_==host.invariants_.size()) return {};
    host.invariants_[host.invariant_count_++]=fact;
    host.quarantined_.store(true,std::memory_order_release);
    return {BlobReceiverEffectStatusV3::accepted,{}};
  }
  static BlobReaderResultV3 ReaderFailure(u8 live,u64 copied=0) noexcept {
    BlobReaderResultV3 failed; failed.actual_octets=copied;
    if(live==SB_BLOB_CALLBACK_REVOKED_V3 || live==SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3 ||
        live==SB_BLOB_CALLBACK_CLOSED_V3) {
      failed.fact_bits=static_cast<u32>(BlobReaderFactV3::snapshot_invalid);
      const u8 expiry=live==SB_BLOB_CALLBACK_REVOKED_V3 ? 5 :
          live==SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3 ? 3 : 6;
      failed.diagnostic_facts.lifetime_enum=expiry;
      failed.diagnostic_facts.expiry_reason_enum=expiry;
    } else {
      failed.fact_bits=static_cast<u32>(BlobReaderFactV3::authority_unavailable);
      failed.diagnostic_facts.reason_enum=22;
    }
    return failed;
  }
  static BlobReaderResultV3 ReadAt(void* raw,const BlobReaderRequestV3& request) {
    auto& host=*static_cast<BlobLifetimeReceiverHostV3Generation1*>(raw);
    std::lock_guard lock(host.mutex_);
    // Guard existence is maintained with actual begin/end effects, not a
    // caller flag or an O(ticket-capacity) scan for every input window.
    const bool guarded=host.active_accesses_!=0;
    if(!guarded || !request.destination || !request.requested_octets ||
        request.requested_octets>request.destination_capacity ||
        request.offset>host.content_->size() || request.requested_octets>host.content_->size()-request.offset) {
      BlobReaderResultV3 malformed; malformed.status=255; return malformed;
    }
    const auto live=host.Live();
    if(live!=SB_BLOB_CALLBACK_OK_V3) return ReaderFailure(live);
    std::memcpy(request.destination,host.content_->data()+request.offset,request.requested_octets);
    const auto after=host.Live();
    if(after!=SB_BLOB_CALLBACK_OK_V3) return ReaderFailure(after,request.requested_octets);
    BlobReaderResultV3 result; result.status=0; result.actual_octets=request.requested_octets;
    return result;
  }
  static bool Identity(platform::Uuid& id) noexcept {
    const auto issued=uuid::IssueRuntimeIdentityV7(); if(!issued) return false; id=*issued; return true;
  }
 public:
  static sb_engine_status_t Create(server_engine_bridge::StatementContextReceiptHandle handle,
      const wire::TypedResultUuid& session,const wire::TypedResultUuid& receipt,
      std::span<const byte> input,const api::StatementBlobInputBoundsV3& bounds,
      BlobLifetimeReceiverHostV3Generation1** output,api::StatementBlobInputAdmissionV3& observation,
      platform::DiagnosticRecord* diagnostic) noexcept {
    using Failure=api::StatementBlobInputAdmissionFailureV3;
    if(!handle || !output || *output || !Range(input) || !bounds.resident_bytes || !bounds.valid_for_ns ||
        !bounds.retain_tickets || bounds.operation<SB_BLOB_OP_VALIDATE_VIEW_V3 || bounds.operation>SB_BLOB_OP_RESTORE_V3 ||
        (bounds.carrier!=api::StatementBlobInputCarrierV3::materialized && bounds.carrier!=api::StatementBlobInputCarrierV3::logical_reader) ||
        (bounds.state!=BlobValueStateV3::value && bounds.state!=BlobValueStateV3::sql_null) ||
        (bounds.state==BlobValueStateV3::sql_null && !input.empty()) || input.size()>kBlobMaximumLogicalBytesV3 ||
        (bounds.carrier==api::StatementBlobInputCarrierV3::logical_reader && !input.empty() && !bounds.window_bytes) ||
        bounds.scratch_bytes>SIZE_MAX) return SB_ENGINE_STATUS_INVALID_ARGUMENT;
    BlobLifetimeReceiverHostV3Generation1* host=nullptr;
    try {
      // This issuer checks its native process claim before ANY inherited
      // registry/memory/receipt lock. Do not put memory admission before it.
      std::optional<api::RetainedStatementMonotonicClockV1> retained_clock;
      observation.failure=Failure::clock_admission;
      auto status=api::RetainStatementMonotonicClockV1(handle,session,receipt,&retained_clock);
      if(status!=SB_ENGINE_STATUS_OK) return status;
      std::optional<api::RetainedStatementCancellationV1> retained_cancellation;
      observation.failure=Failure::cancellation_admission;
      status=api::RetainStatementCancellationV1(handle,session,receipt,&retained_cancellation);
      if(status!=SB_ENGINE_STATUS_OK) return status;
      std::unique_ptr<api::RetainedStatementMemoryV1> memory;
      observation.failure=Failure::memory_admission;
      status=api::RetainStatementMemoryForResultV1(handle,session,receipt,bounds.resident_bytes,&memory,diagnostic);
      if(status!=SB_ENGINE_STATUS_OK) return status;
      auto* block=memory->memory()->allocate(sizeof(BlobLifetimeReceiverHostV3Generation1),alignof(BlobLifetimeReceiverHostV3Generation1));
      host=new(block) BlobLifetimeReceiverHostV3Generation1;
      host->memory_=std::move(memory);
      host->clock_=std::move(retained_clock); host->cancellation_=std::move(retained_cancellation);
      struct Cleanup {
        BlobLifetimeReceiverHostV3Generation1*& host;
        ~Cleanup() { if(host) host->Close(); }
      } cleanup{host};
      const auto& binding=host->memory_->binding();
      observation.failure=Failure::datatype_cohort;
      const auto profile=BuildCurrentBlobValidatedProfileHandleV3(kBlobV11ReceiptUuid);
      if(!profile.ok() || binding.datatype_catalog_snapshot!=profile.profile.receipt.catalog_snapshot_uuid.bytes ||
          binding.datatype_catalog_generation!=profile.profile.receipt.catalog_generation ||
          binding.datatype_registry_generation!=profile.profile.receipt.registry_generation) return SB_ENGINE_STATUS_CONFLICT;
      host->profile_=profile.profile; host->bounds_=bounds;
      const api::StatementCancellationBindingV1 actual{binding.database,binding.session,binding.transaction,binding.statement,binding.receipt};
      observation.failure=Failure::owner_binding;
      if(!host->cancellation_->binding() || *host->cancellation_->binding()!=actual ||
          !host->clock_->binding() || host->clock_->binding()->owner!=actual) return SB_ENGINE_STATUS_INVALID_HANDLE;
      const auto now=host->clock_->Sample();
      const auto cancellation=host->cancellation_->Observe();
      if(cancellation!=api::StatementCancellationObservationV1::not_requested) {
        observation.failure=cancellation==api::StatementCancellationObservationV1::requested ?
            Failure::cancellation_requested : Failure::cancellation_unavailable;
        return SB_ENGINE_STATUS_CONFLICT;
      }
      if(!now.ok()) { observation.failure=Failure::clock_unavailable; return SB_ENGINE_STATUS_CONFLICT; }
      if(bounds.valid_for_ns>UINT64_MAX-now.nanoseconds) {
        observation.failure=Failure::deadline_overflow; return SB_ENGINE_STATUS_INVALID_ARGUMENT;
      }
      u64 last_tick=now.nanoseconds;
      const auto preparing=[&] {
        const auto tick=host->clock_->Sample();
        const auto cancelled=host->cancellation_->Observe();
        const auto live=host->Live();
        if(cancelled!=api::StatementCancellationObservationV1::not_requested) {
          observation.failure=cancelled==api::StatementCancellationObservationV1::requested ?
              Failure::cancellation_requested : Failure::cancellation_unavailable; return false;
        }
        if(live!=SB_BLOB_CALLBACK_OK_V3) { observation.failure=Failure::source_unavailable; return false; }
        if(!tick.ok()) { observation.failure=Failure::clock_unavailable; return false; }
        if(tick.nanoseconds<last_tick) { observation.failure=Failure::clock_regression; return false; }
        last_tick=tick.nanoseconds;
        if(tick.nanoseconds>=now.nanoseconds+bounds.valid_for_ns) {
          observation.failure=Failure::deadline_expired; return false;
        }
        return true;
      };
      observation.failure=Failure::allocation_failed;
      auto* allocator=host->memory_->memory();
      host->content_.emplace(allocator); host->scratch_.emplace(allocator);
      host->tickets_.emplace(allocator); host->access_ids_.emplace(allocator);
      host->content_->resize(input.size()); host->scratch_->resize(bounds.scratch_bytes);
      host->tickets_->resize(bounds.retain_tickets); host->access_ids_->resize(bounds.access_tickets);
      for(std::size_t at=0;at<input.size();) {
        if(!preparing()) return observation.failure==Failure::deadline_expired ? SB_ENGINE_STATUS_TIMEOUT : SB_ENGINE_STATUS_CONFLICT;
        const auto n=std::min(std::size_t{65536},input.size()-at);
        std::memcpy(host->content_->data()+at,input.data()+at,n); at+=n; observation.copied_bytes+=n;
      }
      platform::Uuid authority,token,carrier,services;
      const auto issue=[&](platform::Uuid& id) {
        if(!Identity(id)) { observation.failure=Failure::identity_source; return false; }
        ++observation.identities_issued; return true;
      };
      if(!issue(authority) || !issue(token) || !issue(carrier) || !issue(services)) return SB_ENGINE_STATUS_CONFLICT;
      std::size_t issued_count=0;
      BlobUuid16V3 previous{}; bool previous_present=false;
      for(auto& ticket:*host->tickets_) {
        platform::Uuid id;
        if(((issued_count++ & 63)==0 && !preparing()) || !issue(id))
          return observation.failure==Failure::deadline_expired ? SB_ENGINE_STATUS_TIMEOUT : SB_ENGINE_STATUS_CONFLICT;
        ticket.uuid=AbiUuid(id);
        // Runtime issuance is strictly increasing within its owning thread.
        // Verify this indexing precondition once; callback lookup then uses
        // canonical unsigned-byte binary search, not a capacity-sized scan.
        if(previous_present && !Before(previous,ticket.uuid)) {
          observation.failure=Failure::identity_source; return SB_ENGINE_STATUS_CONFLICT;
        }
        previous=ticket.uuid; previous_present=true;
      }
      for(auto& ticket:*host->access_ids_) {
        platform::Uuid id;
        if(((issued_count++ & 63)==0 && !preparing()) || !issue(id))
          return observation.failure==Failure::deadline_expired ? SB_ENGINE_STATUS_TIMEOUT : SB_ENGINE_STATUS_CONFLICT;
        ticket=AbiUuid(id);
      }
      auto& t=host->token_; t.struct_bytes=sizeof(t); t.abi_major=3;
      t.lifetime_token_uuid=AbiUuid(token); t.lifetime_token_generation=binding.snapshot_generation;
      t.owner_class=SB_BLOB_OWNER_EXECUTION_BORROW_V3; t.authority_instance_uuid=AbiUuid(authority);
      t.authority_instance_generation=1; t.database_uuid=AbiUuid(binding.database); t.session_uuid=AbiUuid(binding.session);
      t.statement_uuid=AbiUuid(binding.statement); t.transaction_uuid=AbiUuid(binding.transaction);
      t.snapshot_uuid=AbiUuid(binding.snapshot); t.snapshot_generation=binding.snapshot_generation;
      t.security_context_uuid=AbiUuid(binding.security_context); t.security_generation=binding.security_generation;
      t.immutable_binding_generation=1; t.valid_until_monotonic_ns=now.nanoseconds+bounds.valid_for_ns;
      const bool logical=bounds.carrier==api::StatementBlobInputCarrierV3::logical_reader;
      t.mode_bits=SB_BLOB_MODE_IMMUTABLE_READ_V3|SB_BLOB_MODE_CONCURRENT_IMMUTABLE_READ_V3|
          (logical ? SB_BLOB_MODE_LOGICAL_READER_V3|SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3 : SB_BLOB_MODE_MATERIALIZED_BYTES_V3);
      auto& r=host->request_; r.struct_bytes=sizeof(r); r.abi_major=3; r.operation=bounds.operation;
      r.required_owner_class=t.owner_class; r.required_mode_bits=t.mode_bits;
      r.expected_authority_instance_uuid=t.authority_instance_uuid; r.expected_authority_instance_generation=1;
      r.expected_database_uuid=t.database_uuid; r.expected_session_uuid=t.session_uuid;
      r.expected_statement_uuid=t.statement_uuid; r.expected_transaction_uuid=t.transaction_uuid;
      r.expected_snapshot_uuid=t.snapshot_uuid; r.expected_snapshot_generation=t.snapshot_generation;
      r.expected_security_context_uuid=t.security_context_uuid; r.expected_security_generation=t.security_generation;
      r.absolute_deadline_monotonic_ns=t.valid_until_monotonic_ns;
      host->carrier_={authority,1,carrier,1,token,t.lifetime_token_generation,1,
          static_cast<u8>(logical ? SB_BLOB_MODE_LOGICAL_READER_V3 : SB_BLOB_MODE_MATERIALIZED_BYTES_V3)};
      host->authority_={sizeof(BlobLifetimeAuthorityV3),3,0,AbiUuid(authority),1,host,
          RetainCallback,ProbeCallback,BeginCallback,EndCallback,ReleaseCallback};
      const auto& clock=*host->clock_->binding();
      host->services_={services,1,{platform::Uuid{clock.clock},clock.clock_generation},host,Clock,Cancel,Invariant};
      BlobLifetimeBudgetConfigurationV3Generation1 budget;
      budget.limits=bounds.maximum_work; budget.scratch=*host->scratch_;
      budget.admitted_window_octets=bounds.window_bytes; budget.admitted_relative_timeout_ns=bounds.relative_timeout_ns;
      if(!host->ledger_.Configure(budget) || !host->ledger_.Freeze()) {
        observation.failure=Failure::execution_bounds; return SB_ENGINE_STATUS_INVALID_ARGUMENT;
      }
      if(!preparing()) return observation.failure==Failure::deadline_expired ? SB_ENGINE_STATUS_TIMEOUT : SB_ENGINE_STATUS_CONFLICT;
      *output=host; host=nullptr; observation.failure=Failure::none; return SB_ENGINE_STATUS_OK;
    } catch(const std::bad_alloc&) {
      observation.failure=Failure::allocation_failed; return SB_ENGINE_STATUS_RESOURCE_EXHAUSTED;
    } catch(...) { observation.failure=Failure::internal_failure; return SB_ENGINE_STATUS_INTERNAL_ERROR; }
  }
  void Close() noexcept { closing_.store(true,std::memory_order_release); Drop(); }
  const BlobValidatedProfileHandleV3& profile() const noexcept { return profile_; }
  api::StatementBlobInputRetainV3 Retain(BlobRetainedLifetimeLeaseV3& lease) noexcept {
    api::StatementBlobInputRetainV3 result;
    BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    const auto pins=Pins();
    BlobLifetimeBudgetControlCapabilityV3Generation1 budget(ledger_,pins);
    result.runtime_invoked=true;
    if(bounds_.carrier==api::StatementBlobInputCarrierV3::materialized) {
      BlobReceiverMaterializedCapabilityV3Generation1 capability(carrier_,bounds_.state,content_->data(),content_->size());
      BlobBoundMaterializedCarrierV3Generation1 carrier;
      result.runtime=BlobLifetimeRuntimeFactoryV3Generation1::AdmitMaterialized(bounds_.operation,profile_,&authority_,&services_,
          pins,budget,services_.monotonic_clock,services_.monotonic_clock,token_,request_,capability,admission,carrier);
      if(result.runtime.ok()) result.runtime=RetainBaseBlobLifetimeLeaseV3Generation1(profile_,std::move(admission),carrier,token_,request_,lease);
    } else {
      BlobReceiverLogicalReaderCapabilityV3Generation1 capability(carrier_,bounds_.state,content_->size(),this,ReadAt);
      BlobBoundLogicalReaderCarrierV3Generation1 carrier;
      result.runtime=BlobLifetimeRuntimeFactoryV3Generation1::AdmitLogicalReader(bounds_.operation,profile_,&authority_,&services_,
          pins,budget,services_.monotonic_clock,services_.monotonic_clock,token_,request_,capability,admission,carrier);
      if(result.runtime.ok()) result.runtime=RetainBaseBlobLifetimeLeaseV3Generation1(profile_,std::move(admission),carrier,token_,request_,lease);
    }
    result.status=result.runtime.ok() ? SB_ENGINE_STATUS_OK : SB_ENGINE_STATUS_CONFLICT;
    return result;
  }
};
} // namespace scratchbird::core::datatypes

namespace scratchbird::engine::internal_api {
StatementBlobInputV3::~StatementBlobInputV3() noexcept { Reset(); }
void StatementBlobInputV3::Reset() noexcept { auto* host=host_; host_=nullptr; if(host) host->Close(); }
const core::datatypes::BlobValidatedProfileHandleV3* StatementBlobInputV3::profile() const noexcept {
  return host_ ? &host_->profile() : nullptr;
}
StatementBlobInputRetainV3 StatementBlobInputV3::Retain(core::datatypes::BlobRetainedLifetimeLeaseV3& lease) noexcept {
  return host_ ? host_->Retain(lease) : StatementBlobInputRetainV3{};
}
StatementBlobInputAdmissionV3 ReceiveStatementBlobInputV3(server_engine_bridge::StatementContextReceiptHandle handle,
    const wire::TypedResultUuid& session,const wire::TypedResultUuid& receipt,std::span<const core::platform::byte> input,
    const StatementBlobInputBoundsV3& bounds,StatementBlobInputV3* output,core::platform::DiagnosticRecord* diagnostic) noexcept {
  if(diagnostic) *diagnostic={};
  StatementBlobInputAdmissionV3 result;
  if(!output) return result;
  if(output->bound()) { result.failure=StatementBlobInputAdmissionFailureV3::output_in_use; return result; }
  result.status=core::datatypes::BlobLifetimeReceiverHostV3Generation1::Create(handle,session,receipt,input,bounds,&output->host_,result,diagnostic);
  return result;
}
} // namespace scratchbird::engine::internal_api
