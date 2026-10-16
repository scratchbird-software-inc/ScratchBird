// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/statement_blob_input.hpp"
#include "engine/statement_blob_component.hpp"
#include "engine/statement_blob_encoded_input.hpp"
#include "engine/statement_blob_hash.hpp"
#include "engine/statement_blob_row_value.hpp"
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
  enum class Role : u8 { logical_value, encoded_artifact };
  Role role_=Role::logical_value;
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
      platform::DiagnosticRecord* diagnostic,Role role=Role::logical_value) noexcept {
    using Failure=api::StatementBlobInputAdmissionFailureV3;
    const bool encoded=role==Role::encoded_artifact;
    if(!handle || !output || *output || !Range(input) || !bounds.resident_bytes || !bounds.valid_for_ns ||
        !bounds.retain_tickets || bounds.operation<(encoded ? SB_BLOB_OP_COMPONENT_DECODE_V3 : SB_BLOB_OP_VALIDATE_VIEW_V3) ||
        bounds.operation>SB_BLOB_OP_RESTORE_V3 ||
        (bounds.carrier!=api::StatementBlobInputCarrierV3::materialized && bounds.carrier!=api::StatementBlobInputCarrierV3::logical_reader) ||
        (bounds.state!=BlobValueStateV3::value && bounds.state!=BlobValueStateV3::sql_null) ||
        (bounds.state==BlobValueStateV3::sql_null && !input.empty()) || (!encoded && input.size()>kBlobMaximumLogicalBytesV3) ||
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
      host->role_=role;
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
      t.owner_class=encoded ? SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3 : SB_BLOB_OWNER_EXECUTION_BORROW_V3;
      t.authority_instance_uuid=AbiUuid(authority);
      t.authority_instance_generation=1; t.database_uuid=AbiUuid(binding.database); t.session_uuid=AbiUuid(binding.session);
      t.statement_uuid=AbiUuid(binding.statement); t.transaction_uuid=AbiUuid(binding.transaction);
      t.snapshot_uuid=AbiUuid(binding.snapshot); t.snapshot_generation=binding.snapshot_generation;
      t.security_context_uuid=AbiUuid(binding.security_context); t.security_generation=binding.security_generation;
      t.immutable_binding_generation=1; t.valid_until_monotonic_ns=now.nanoseconds+bounds.valid_for_ns;
      const bool logical=bounds.carrier==api::StatementBlobInputCarrierV3::logical_reader;
      t.mode_bits=SB_BLOB_MODE_IMMUTABLE_READ_V3|SB_BLOB_MODE_CONCURRENT_IMMUTABLE_READ_V3|
          (encoded ? SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3 : logical ?
              SB_BLOB_MODE_LOGICAL_READER_V3|SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3 : SB_BLOB_MODE_MATERIALIZED_BYTES_V3);
      auto& r=host->request_; r.struct_bytes=sizeof(r); r.abi_major=3; r.operation=bounds.operation;
      r.required_owner_class=t.owner_class; r.required_mode_bits=t.mode_bits;
      r.expected_authority_instance_uuid=t.authority_instance_uuid; r.expected_authority_instance_generation=1;
      r.expected_database_uuid=t.database_uuid; r.expected_session_uuid=t.session_uuid;
      r.expected_statement_uuid=t.statement_uuid; r.expected_transaction_uuid=t.transaction_uuid;
      r.expected_snapshot_uuid=t.snapshot_uuid; r.expected_snapshot_generation=t.snapshot_generation;
      r.expected_security_context_uuid=t.security_context_uuid; r.expected_security_generation=t.security_generation;
      r.absolute_deadline_monotonic_ns=t.valid_until_monotonic_ns;
      host->carrier_={authority,1,carrier,1,token,t.lifetime_token_generation,1,
          static_cast<u8>(encoded ? SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3 :
              logical ? SB_BLOB_MODE_LOGICAL_READER_V3 : SB_BLOB_MODE_MATERIALIZED_BYTES_V3)};
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
  static sb_engine_status_t CreateEncoded(server_engine_bridge::StatementContextReceiptHandle handle,
      const wire::TypedResultUuid& session,const wire::TypedResultUuid& receipt,std::span<const byte> input,
      const api::StatementBlobEncodedInputBoundsV3& bounds,BlobLifetimeReceiverHostV3Generation1** output,
      api::StatementBlobInputAdmissionV3& observation,platform::DiagnosticRecord* diagnostic) noexcept {
    api::StatementBlobInputBoundsV3 common;
    common.operation=bounds.operation; common.carrier=api::StatementBlobInputCarrierV3::logical_reader;
    common.resident_bytes=bounds.resident_bytes; common.valid_for_ns=bounds.valid_for_ns;
    common.retain_tickets=bounds.retain_tickets; common.access_tickets=bounds.access_tickets;
    common.maximum_work=bounds.maximum_work; common.scratch_bytes=bounds.scratch_bytes;
    common.window_bytes=bounds.window_bytes; common.relative_timeout_ns=bounds.relative_timeout_ns;
    return Create(handle,session,receipt,input,common,output,observation,diagnostic,Role::encoded_artifact);
  }
  void Close() noexcept { closing_.store(true,std::memory_order_release); Drop(); }
  const BlobValidatedProfileHandleV3& profile() const noexcept { return profile_; }
  auto WorkSnapshot() noexcept { return ledger_.Snapshot().counters; }
  u64 extent() const noexcept { return content_->size(); }
  api::StatementBlobInputRetainV3 Retain(BlobRetainedLifetimeLeaseV3& lease) noexcept {
    api::StatementBlobInputRetainV3 result;
    BlobLifetimeAuthorityAdmissionV3Generation1 admission;
    const auto pins=Pins();
    BlobLifetimeBudgetControlCapabilityV3Generation1 budget(ledger_,pins);
    result.runtime_invoked=true;
    if(role_==Role::encoded_artifact) {
      BlobReceiverEncodedReaderCapabilityV3Generation1 capability(carrier_,content_->size(),this,ReadAt);
      BlobBoundEncodedReaderCarrierV3Generation1 carrier;
      result.runtime=BlobLifetimeRuntimeFactoryV3Generation1::AdmitEncodedReader(bounds_.operation,profile_,&authority_,&services_,
          pins,budget,services_.monotonic_clock,services_.monotonic_clock,token_,request_,capability,admission,carrier);
      if(result.runtime.ok()) result.runtime=RetainBaseBlobLifetimeLeaseV3Generation1(profile_,std::move(admission),carrier,token_,request_,lease);
    } else if(bounds_.carrier==api::StatementBlobInputCarrierV3::materialized) {
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
  api::StatementBlobValueResultV3 RetainTyped(engine::ExecutionTypeDescriptor&& descriptor,
      api::StatementBlobValueV3& destination) noexcept {
    using Failure=api::StatementBlobValueFailureV3;
    api::StatementBlobValueResultV3 result;
    result.lifetime.status=SB_ENGINE_STATUS_INVALID_ARGUMENT;
    if(role_!=Role::logical_value) { result.failure=Failure::encoded_artifact_not_value; return result; }
    if(destination.used_) { result.failure=Failure::destination_used; return result; }
    if(!ValidateBlobExecutionDescriptorV3(profile_,descriptor).ok()) {
      result.failure=Failure::descriptor_invalid; return result;
    }
    if(bounds_.state==BlobValueStateV3::sql_null && !descriptor.nullable_allowed) {
      result.failure=Failure::sql_null_not_admitted; return result;
    }
    result.lifetime=Retain(destination.lease_);
    if(!result.lifetime.ok()) { result.failure=Failure::lifetime; return result; }
    destination.descriptor_=std::move(descriptor);
    destination.profile_=profile_;
    destination.state_=bounds_.state;
    destination.length_=content_->size();
    destination.host_=this;
    destination.used_=true;
    result.failure=Failure::none; result.category=DatatypeCastCategory::identity;
    return result;
  }
 private:
  struct EncodeWork {
    BlobComponentDigestPassV3 digest;
    BlobComponentHeaderV3 header{};
    BlobLifetimeReceiverHostV3Generation1* host;
    api::StatementBlobComponentResultV3* result;
    byte* staging;
    std::size_t extent, offset=0;
    bool header_ready=false;
    EncodeWork(BlobLifetimeReceiverHostV3Generation1* h,api::StatementBlobComponentResultV3* r,
        byte* s,std::size_t n) noexcept : host(h),result(r),staging(s),extent(n) {}
    ~EncodeWork() noexcept { Erase(header.data(),header.size()); }
  };
  static bool SinkCharge(EncodeWork& work,BlobLifetimeResourceV3 resource,u64 amount) noexcept {
    u64 available=0;
    if(work.host->ledger_.Charge(resource,amount,&available)) return true;
    work.result->resource=resource; work.result->required=amount; work.result->available=available;
    return false;
  }
  static bool SinkAttempt(EncodeWork& work) noexcept {
    // This is an actual invocation of the closed native writer lifecycle,
    // not a fabricated external sink callback or successful publication.
    ++work.result->sink_attempts;
    return SinkCharge(work,BlobLifetimeResourceV3::sink_calls,1);
  }
  static bool SinkWrite(EncodeWork& work,std::span<const byte> bytes) noexcept {
    if(!SinkAttempt(work)) return false;
    if(work.offset>work.extent || bytes.size()>work.extent-work.offset) return false;
    if(!SinkCharge(work,BlobLifetimeResourceV3::sink_bytes_written,bytes.size())) return false;
    std::memcpy(work.staging+work.offset,bytes.data(),bytes.size());
    work.offset+=bytes.size(); work.result->staged_bytes+=bytes.size(); return true;
  }
  static BlobProtectedWorkResultV3 EncodeFailure(EncodeWork& work) noexcept {
    const auto& result=*work.result;
    if(result.required<=result.available || work.host->ledger_.terminal_quarantined()) return {};
    // Only the actual shared ledger shortage can produce this public fact.
    // Internal codec/schedule errors return the private terminal fact instead.
    BlobLifetimeTypedFactV3 fact;
    fact.fact_class=BlobLifetimeFactClassV3::resource_budget;
    fact.diagnostic=BlobLifetimeDiagnosticCodeV3::resource_budget_exceeded;
    fact.stage=BlobLifetimeFactStageV3::callback_return;
    fact.diagnostic_uuid=platform::Uuid{{0xde,0xdc,0x1c,0x9b,0x38,0x79,0x57,0x70,
        0x8f,0x8e,0xc0,0xc9,0x27,0xa0,0xc3,0x4d}};
    fact.diagnostic_generation=1;
    fact.parameters.present=blob_parameter_resource|blob_parameter_required|blob_parameter_available;
    fact.parameters.resource_enum=result.resource; fact.parameters.required_u64=result.required;
    fact.parameters.available_u64=result.available;
    return {1,{},fact};
  }
  static BlobProtectedWorkResultV3 DigestInput(void* raw,std::span<const byte> bytes) noexcept {
    auto& work=*static_cast<EncodeWork*>(raw);
    work.result->first_pass_bytes+=bytes.size();
    work.result->component=work.digest.UpdateContent(bytes);
    return work.result->component.ok() ? BlobProtectedWorkResultV3{0,{},{}} : EncodeFailure(work);
  }
  static bool FinishHeader(EncodeWork& work) noexcept {
    if(work.header_ready) return true;
    work.result->component=work.digest.FinishEncode(work.header);
    if(!work.result->component.ok() || !SinkWrite(work,work.header)) return false;
    work.header_ready=true; return true;
  }
  static BlobProtectedWorkResultV3 StageInput(void* raw,std::span<const byte> bytes) noexcept {
    auto& work=*static_cast<EncodeWork*>(raw);
    work.result->second_pass_bytes+=bytes.size();
    if(!FinishHeader(work) || !SinkWrite(work,bytes)) return EncodeFailure(work);
    return {0,{},{}};
  }
 public:
  static api::StatementBlobComponentResultV3 EncodeComponent(api::StatementBlobValueV3& source,
      api::StatementBlobComponentV3& output) noexcept {
    using Failure=api::StatementBlobComponentFailureV3;
    api::StatementBlobComponentResultV3 result;
    if(output.bound()) { result.failure=Failure::output_in_use; return result; }
    if(!source.bound() || !source.host_) return result;
    auto& host=*source.host_;
    if(host.bounds_.operation!=SB_BLOB_OP_COMPONENT_ENCODE_V3 ||
        host.bounds_.carrier!=api::StatementBlobInputCarrierV3::logical_reader) {
      result.failure=Failure::operation_not_admitted; return result;
    }
    const auto acquired=AcquirePin(&host);
    if(acquired.status!=BlobReceiverPinAcquireStatusV3::acquired) {
      result.failure=Failure::admission_closed; return result;
    }
    struct Pin {
      BlobLifetimeReceiverHostV3Generation1* host;
      ~Pin() { if(host) host->Drop(); }
    } pin{&host};
    const auto probe=[&] {
      result.lifetime_invoked=true; result.lifetime=source.lease_.Probe();
      if(!result.lifetime.ok()) { result.failure=Failure::lifetime; return false; }
      return true;
    };
    if(!probe()) return result;
    const auto length=source.length_;
    if(length>SIZE_MAX-kBlobComponentHeaderBytesV3-sizeof(EncodeWork) ||
        (length && !host.bounds_.window_bytes)) {
      result.failure=Failure::extent_invalid; return result;
    }
    const auto extent=static_cast<std::size_t>(length)+kBlobComponentHeaderBytesV3;
    const u64 windows=length ? length/host.bounds_.window_bytes+(length%host.bounds_.window_bytes!=0) : 0;
    const u64 fixed_temporary=extent+sizeof(EncodeWork);
    const u64 window=std::min(length,host.bounds_.window_bytes);
    if(window>UINT64_MAX-fixed_temporary) { result.failure=Failure::extent_invalid; return result; }
    const auto budget=host.ledger_.Snapshot();
    const auto need=[&](BlobLifetimeResourceV3 resource,u64 amount) {
      const auto& counter=budget.counters[static_cast<std::size_t>(resource)];
      const bool valid=budget.frozen && !budget.terminal_quarantined &&
          counter.invoked<=counter.limit && counter.reserved_work<=counter.limit-counter.invoked &&
          counter.reserved_cleanup<=counter.limit-counter.invoked-counter.reserved_work;
      const auto available=valid ? counter.limit-counter.invoked-counter.reserved_work-counter.reserved_cleanup : 0;
      if(amount<=available) return true;
      result.failure=Failure::resource_preflight; result.resource=resource;
      result.required=amount; result.available=available; return false;
    };
    if(!need(BlobLifetimeResourceV3::logical_bytes_read,2*length) ||
        !need(BlobLifetimeResourceV3::reader_calls,2*windows) ||
        !need(BlobLifetimeResourceV3::sink_bytes_written,extent) ||
        !need(BlobLifetimeResourceV3::sink_calls,3+windows) ||
        !need(BlobLifetimeResourceV3::temporary_bytes,fixed_temporary+window)) return result;
    u64 available=0;
    if(!host.ledger_.Charge(BlobLifetimeResourceV3::temporary_bytes,extent+sizeof(EncodeWork),&available)) {
      result.failure=Failure::resource_preflight; result.resource=BlobLifetimeResourceV3::temporary_bytes;
      result.required=extent+sizeof(EncodeWork); result.available=available; return result;
    }
    auto* allocator=host.memory_->memory();
    byte* allocation=nullptr; EncodeWork* work=nullptr;
    struct Cleanup {
      std::pmr::memory_resource* allocator; byte*& allocation; EncodeWork*& work; std::size_t allocated;
      void Workspace() noexcept {
        if(!work) return;
        work->~EncodeWork(); Erase(work,sizeof(EncodeWork));
        work=nullptr;
      }
      ~Cleanup() {
        Workspace();
        if(allocation) { Erase(allocation,allocated); try { allocator->deallocate(allocation,allocated,alignof(EncodeWork)); } catch(...) { std::terminate(); } }
      }
    } cleanup{allocator,allocation,work,extent+sizeof(EncodeWork)};
    try {
      // One actually charged allocation avoids duplicating physical grant,
      // binary-owner/accounting-node and sharded-accounting registration work.
      // The destroyed/scrubbed workspace prefix remains accounted padding of
      // this allocation until artifact cleanup; never report it as released.
      allocation=static_cast<byte*>(allocator->allocate(cleanup.allocated,alignof(EncodeWork)));
      auto* bytes=allocation+sizeof(EncodeWork);
      work=new(allocation) EncodeWork(&host,&result,bytes,extent);
      result.component=work->digest.BeginEncode(source.profile_,source.state_,length);
      if(!result.component.ok()) { result.failure=Failure::component_failed; return result; }
      if(!SinkAttempt(*work)) { result.failure=Failure::resource_preflight; return result; }
      const BlobTrustedInternalVisitorV3 first(DigestInput,work),second(StageInput,work);
      result.lifetime=source.lease_.VisitBoundReplayableReaderBytes(first,second);
      if(!result.lifetime.ok()) { result.failure=Failure::lifetime; return result; }
      // The Core NULL/empty path authenticates a probe without a payload visit.
      if(!FinishHeader(*work)) { result.failure=result.component.ok() ? Failure::resource_preflight : Failure::component_failed; return result; }
      if(work->offset!=extent || result.first_pass_bytes!=length || result.second_pass_bytes!=length) {
        result.failure=Failure::internal_failure; return result;
      }
      // Commit is this closed writer's real finalization attempt. Its quota
      // admission, all workspace destruction and the complete native return
      // gate precede irreversible output publication. No later sample exists.
      if(!SinkAttempt(*work)) { result.failure=Failure::resource_preflight; return result; }
      cleanup.Workspace();
      if(!probe()) return result;
      output.allocation_=allocation; output.allocated_=cleanup.allocated;
      output.data_=bytes; output.size_=extent; output.host_=&host;
      allocation=nullptr; pin.host=nullptr;
      result.failure=Failure::none; return result;
    } catch(const std::bad_alloc&) { result.failure=Failure::allocation_failed; return result; }
    catch(...) { result.failure=Failure::internal_failure; return result; }
  }
  static void DestroyComponent(api::StatementBlobComponentV3& value) noexcept {
    auto* host=value.host_; if(!host) return;
    auto* allocation=value.allocation_; const auto size=value.allocated_;
    value.host_=nullptr; value.allocation_=nullptr; value.data_=nullptr; value.size_=0; value.allocated_=0;
    Erase(allocation,size);
    try { host->memory_->memory()->deallocate(allocation,size,alignof(EncodeWork)); } catch(...) { std::terminate(); }
    host->Drop();
  }
 private:
  struct DecodeWork {
    BlobComponentDigestPassV3 digest;
    BlobValueStateV3 state=BlobValueStateV3::value;
    std::size_t length=0;
  };
  struct DecodeContext {
    BlobLifetimeReceiverHostV3Generation1* host;
    api::StatementBlobDecodeResultV3* result;
    bool nullable;
    byte* allocation=nullptr;
    DecodeWork* work=nullptr;
    std::size_t allocated=0;
    u64 reserved_bytes=0,reserved_calls=0;
    ~DecodeContext() noexcept {
      if((reserved_bytes || reserved_calls) && !host->ledger_.CancelSinkWork(reserved_bytes,reserved_calls)) std::terminate();
      DestroyWork(); if(allocation) Free(allocation,allocated);
    }
    void DestroyWork() noexcept {
      if(work) { work->~DecodeWork(); Erase(work,sizeof(DecodeWork)); work=nullptr; }
    }
    void Free(byte* bytes,std::size_t count) noexcept {
      Erase(bytes,count);
      try { host->memory_->memory()->deallocate(bytes,count,alignof(DecodeWork)); } catch(...) { std::terminate(); }
    }
    bool Need(BlobLifetimeResourceV3 resource,u64 amount) noexcept {
      const auto snapshot=host->ledger_.Snapshot();
      const auto& counter=snapshot.counters[static_cast<std::size_t>(resource)];
      const bool valid=snapshot.frozen && !snapshot.terminal_quarantined && counter.invoked<=counter.limit &&
          counter.reserved_cleanup<=counter.limit-counter.invoked &&
          counter.reserved_work<=counter.limit-counter.invoked-counter.reserved_cleanup;
      const u64 available=valid ? counter.limit-counter.invoked-counter.reserved_cleanup-counter.reserved_work : 0;
      if(amount<=available) return true;
      result->failure=api::StatementBlobDecodeFailureV3::resource_preflight;
      result->resource=resource; result->required=amount; result->available=available; return false;
    }
    bool Charge(BlobLifetimeResourceV3 resource,u64 amount) noexcept {
      u64 available=0;
      if(host->ledger_.Charge(resource,amount,&available)) return true;
      result->failure=api::StatementBlobDecodeFailureV3::resource_preflight;
      result->resource=resource; result->required=amount; result->available=available; return false;
    }
    bool SinkAttempt(u64 bytes=0) noexcept {
      ++result->sink_attempts;
      if(bytes>reserved_bytes || !reserved_calls || !host->ledger_.ConsumeReservedSinkCall(bytes)) {
        result->failure=api::StatementBlobDecodeFailureV3::internal_failure; return false;
      }
      reserved_bytes-=bytes; --reserved_calls; return true;
    }
  };
  static BlobProtectedWorkResultV3 DecodeHeader(void* raw,std::span<const byte> header) noexcept {
    auto& context=*static_cast<DecodeContext*>(raw); auto& result=*context.result;
    using Failure=api::StatementBlobDecodeFailureV3;
    result.header_bytes_visited+=header.size();
    // This closed native reader returns the complete exact requested range.
    // Do not accept fragmented header delivery as a complete header admission.
    result.component=ValidateBlobComponentHeaderNoAllocV3(context.host->profile_,context.host->content_->size(),header,context.nullable);
    if(!result.component.ok()) { result.failure=Failure::component_failed; return {0,{},{}}; }
    u64 length=0;
    for(unsigned n=0;n<8;++n) length|=static_cast<u64>(header[32+n])<<(8*n);
    if(length>SIZE_MAX-sizeof(DecodeWork)) { result.failure=Failure::extent_invalid; return {0,{},{}}; }
    const u64 window=std::min(length,context.host->bounds_.window_bytes);
    const u64 calls=length ? length/context.host->bounds_.window_bytes+(length%context.host->bounds_.window_bytes!=0) : 0;
    const u64 allocation=length+sizeof(DecodeWork);
    if(window>UINT64_MAX-allocation) { result.failure=Failure::extent_invalid; return {0,{},{}}; }
    // Only after the complete actual shape/identity/nullability gate succeeds
    // may body reads, output, hash workspace or sink work be admitted.
    if(!context.Need(BlobLifetimeResourceV3::encoded_bytes_read,length) ||
        !context.Need(BlobLifetimeResourceV3::reader_calls,calls) ||
        !context.Need(BlobLifetimeResourceV3::sink_bytes_written,length) ||
        !context.Need(BlobLifetimeResourceV3::sink_calls,2+calls) ||
        !context.Need(BlobLifetimeResourceV3::temporary_bytes,allocation+window) ||
        !context.Charge(BlobLifetimeResourceV3::temporary_bytes,allocation)) return {0,{},{}};
    BlobLifetimeResourceV3 failed; u64 available=0;
    if(!context.host->ledger_.ReserveSinkWork(length,2+calls,failed,available)) {
      result.failure=Failure::resource_preflight; result.resource=failed;
      result.required=failed==BlobLifetimeResourceV3::sink_calls ? 2+calls : length;
      result.available=available; return {0,{},{}};
    }
    context.reserved_bytes=length; context.reserved_calls=2+calls;
    try {
      context.allocated=static_cast<std::size_t>(allocation);
      context.allocation=static_cast<byte*>(context.host->memory_->memory()->allocate(context.allocated,alignof(DecodeWork)));
      context.work=new(context.allocation) DecodeWork;
      context.work->length=static_cast<std::size_t>(length);
      context.work->state=static_cast<BlobValueStateV3>(header[24]);
      result.component=context.work->digest.BeginDecode(context.host->profile_,context.host->content_->size(),header,context.nullable);
      if(!result.component.ok()) { result.failure=Failure::component_failed; return {0,{},{}}; }
      if(!context.SinkAttempt()) return {0,{},{}};
      result.failure=Failure::none;
    } catch(const std::bad_alloc&) { result.failure=Failure::allocation_failed; }
    catch(...) { result.failure=Failure::internal_failure; }
    // This visitor reports the actual bounded reader lifetime work only. Data,
    // allocation and format outcomes remain in the owning codec result, never
    // fake C-ABI callback facts or authority-quarantine for malformed user data.
    return {0,{},{}};
  }
  static BlobProtectedWorkResultV3 DecodeContent(void* raw,std::span<const byte> bytes) noexcept {
    auto& context=*static_cast<DecodeContext*>(raw); auto& result=*context.result;
    if(result.failure!=api::StatementBlobDecodeFailureV3::none) return {};
    const auto offset=result.content_bytes_visited;
    result.content_bytes_visited+=bytes.size();
    if(offset>context.work->length || bytes.size()>context.work->length-offset) {
      result.failure=api::StatementBlobDecodeFailureV3::internal_failure; return {};
    }
    result.component=context.work->digest.UpdateContent(bytes);
    if(!result.component.ok()) { result.failure=api::StatementBlobDecodeFailureV3::component_failed; return {}; }
    if(!context.SinkAttempt(bytes.size())) return {};
    std::memcpy(context.allocation+sizeof(DecodeWork)+offset,bytes.data(),bytes.size()); result.staged_bytes+=bytes.size();
    if(result.content_bytes_visited==context.work->length) {
      result.component=context.work->digest.FinishDecode();
      if(!result.component.ok()) result.failure=api::StatementBlobDecodeFailureV3::component_failed;
    }
    return {0,{},{}};
  }
 public:
  api::StatementBlobDecodeResultV3 DecodeComponent(engine::ExecutionTypeDescriptor&& target,
      api::StatementBlobDecodedComponentV3& output) noexcept {
    using Failure=api::StatementBlobDecodeFailureV3;
    api::StatementBlobDecodeResultV3 result;
    if(output.bound()) { result.failure=Failure::output_in_use; return result; }
    if(!ValidateBlobExecutionDescriptorV3(profile_,target).ok()) { result.failure=Failure::descriptor_invalid; return result; }
    if(role_!=Role::encoded_artifact || bounds_.operation!=SB_BLOB_OP_COMPONENT_DECODE_V3) {
      result.failure=Failure::operation_not_admitted; return result;
    }
    const auto acquired=AcquirePin(this);
    if(acquired.status!=BlobReceiverPinAcquireStatusV3::acquired) { result.failure=Failure::admission_closed; return result; }
    struct Pin { BlobLifetimeReceiverHostV3Generation1* host; ~Pin(){if(host)host->Drop();} } pin{this};
    DecodeContext context{this,&result,target.nullable_allowed};
    BlobRetainedLifetimeLeaseV3 source;
    const auto observe=[&](const BlobLifetimeRuntimeResultV3& value) {
      result.lifetime_invoked=true; result.lifetime=value;
      if(!value.ok()) { result.failure=Failure::lifetime; return false; } return true;
    };
    const auto finish=[&]() {
      if(source.retained()) {
        const auto released=source.Release();
        if(!released.ok()) { result.failure=Failure::lifetime; result.lifetime=released; }
      }
      return result;
    };
    const auto retained=Retain(source);
    if(!observe(retained.runtime)) return finish();
    if(content_->size()<kBlobComponentHeaderBytesV3) {
      // Closed copied-input ownership establishes the actual missing header
      // extent. Authenticate that owner, but never read past its real bounds.
      if(!observe(source.Probe())) return finish();
      result.component=ValidateBlobComponentHeaderNoAllocV3(profile_,content_->size(),{});
      result.component.failure_offset=content_->size();
      result.failure=Failure::component_failed; return finish();
    }
    const BlobTrustedInternalVisitorV3 header(DecodeHeader,&context);
    if(!observe(source.VisitBoundEncodedRangeBytes(0,kBlobComponentHeaderBytesV3,header))) return finish();
    if(result.failure!=Failure::none || !context.work) return finish();
    const auto length=context.work->length; const auto state=context.work->state;
    const BlobTrustedInternalVisitorV3 body(DecodeContent,&context);
    if(!observe(source.VisitBoundEncodedRangeBytes(kBlobComponentHeaderBytesV3,length,body))) return finish();
    if(result.failure!=Failure::none) return finish();
    if(!length) {
      result.component=context.work->digest.FinishDecode();
      if(!result.component.ok()) { result.failure=Failure::component_failed; return finish(); }
    }
    if(result.content_bytes_visited!=length || result.staged_bytes!=length) {
      result.failure=Failure::internal_failure; return finish();
    }
    if(!context.SinkAttempt()) return finish();
    context.DestroyWork();
    if(!observe(source.Probe())) return finish();
    result=finish();
    if(result.failure!=Failure::none) return result;
    output.allocation_=context.allocation; output.allocated_=context.allocated;
    output.data_=context.allocation+sizeof(DecodeWork); output.size_=length;
    output.descriptor_=std::move(target); output.profile_=profile_; output.state_=state; output.host_=this;
    context.allocation=nullptr; pin.host=nullptr;
    return result;
  }
  static void DestroyDecoded(api::StatementBlobDecodedComponentV3& value) noexcept {
    auto* host=value.host_; if(!host) return;
    auto* allocation=value.allocation_; const auto bytes=value.allocated_;
    value.host_=nullptr; value.allocation_=nullptr; value.data_=nullptr; value.size_=0; value.allocated_=0;
    value.descriptor_={}; value.profile_={}; value.state_=BlobValueStateV3::sql_null;
    Erase(allocation,bytes);
    try { host->memory_->memory()->deallocate(allocation,bytes,alignof(DecodeWork)); } catch(...) { std::terminate(); }
    host->Drop();
  }
  static api::StatementBlobComponentResultV3 SerializeAndRetire(api::StatementBlobValueV3& source,
      api::StatementBlobSerializedComponentV3& output) noexcept {
    using Failure=api::StatementBlobComponentFailureV3;
    api::StatementBlobComponentResultV3 result;
    if(output.frame_.bound() || output.value_.used_) { result.failure=Failure::output_in_use; return result; }
    result=EncodeComponent(source,output.frame_);
    if(!result.ok()) return result;
    // The real, qualified explicit transfer owns the original retain ticket,
    // pins and shared budget. No new authority or fake metadata clone exists.
    result.lifetime=output.value_.lease_.MoveReplaceFrom(std::move(source.lease_));
    if(!result.lifetime.ok()) { output.frame_.Reset(); result.failure=Failure::lifetime; return result; }
    output.value_.descriptor_=std::move(source.descriptor_);
    output.value_.profile_=source.profile_; output.value_.state_=source.state_; output.value_.length_=source.length_;
    output.value_.host_=source.host_; output.value_.used_=true;
    source.descriptor_={}; source.profile_={}; source.state_=BlobValueStateV3::sql_null; source.length_=0;
    source.host_=nullptr; source.used_=true;
    return result;
  }
 private:
  struct RowStageContext;
  struct RowStageWork { RowStageContext* context; u64 offset=0; };
  struct RowStageContext {
    BlobLifetimeReceiverHostV3Generation1* host;
    api::StatementBlobRowResultV3* result;
    byte* allocation=nullptr;
    std::size_t allocated=0;
    RowStageWork* work=nullptr;
    u64 reserved_bytes=0,reserved_calls=0;
    void DestroyWork() noexcept {
      if(work){work->~RowStageWork();Erase(work,sizeof(RowStageWork));work=nullptr;}
    }
    ~RowStageContext() noexcept {
      if((reserved_bytes||reserved_calls)&&!host->ledger_.CancelSinkWork(reserved_bytes,reserved_calls))std::terminate();
      DestroyWork();
      if(allocation){Erase(allocation,allocated);
        try{host->memory_->memory()->deallocate(allocation,allocated,alignof(RowStageWork));}catch(...){std::terminate();}}
    }
    bool Sink(u64 bytes=0) noexcept {
      ++result->sink_attempts;
      if(bytes>reserved_bytes||!reserved_calls||!host->ledger_.ConsumeReservedSinkCall(bytes)){
        result->failure=api::StatementBlobRowFailureV3::internal_failure;return false;}
      reserved_bytes-=bytes;--reserved_calls;return true;
    }
  };
  static BlobProtectedWorkResultV3 StageRowBytes(void* raw,std::span<const byte> bytes) noexcept {
    auto& work=*static_cast<RowStageWork*>(raw);auto& context=*work.context;
    if(context.result->failure!=api::StatementBlobRowFailureV3::none)return {0,{},{}};
    const auto extent=context.allocated-sizeof(RowStageWork);
    if(work.offset>extent||bytes.size()>extent-work.offset||!context.Sink(bytes.size())){
      context.result->failure=api::StatementBlobRowFailureV3::internal_failure;return {0,{},{}};}
    if(!bytes.empty())std::memcpy(context.allocation+sizeof(RowStageWork)+work.offset,bytes.data(),bytes.size());
    work.offset+=bytes.size();context.result->bytes_visited+=bytes.size();return {0,{},{}};
  }
 public:
  static api::StatementBlobRowResultV3 StageForRowAndRetire(api::StatementBlobValueV3& source,
      api::StatementBlobRowValueV3& output) noexcept {
    using Failure=api::StatementBlobRowFailureV3;
    api::StatementBlobRowResultV3 result;
    if(output.host_||output.value_.used_){result.failure=Failure::output_in_use;return result;}
    if(!source.bound()||!source.host_)return result;
    auto& host=*source.host_;
    if(host.bounds_.operation!=SB_BLOB_OP_READ_V3&&host.bounds_.operation!=SB_BLOB_OP_HASH_V3){
      result.failure=Failure::operation_not_admitted;return result;}
    // Cleanup custody for an already-retained input, not a fresh factory
    // admission. A root's retirement does not invalidate an independent lease.
    auto pins=host.pins_.load(std::memory_order_acquire);
    do{if(!pins||pins==UINT64_MAX){result.failure=Failure::admission_closed;return result;}}
    while(!host.pins_.compare_exchange_weak(pins,pins+1,std::memory_order_acq_rel));
    struct Pin{BlobLifetimeReceiverHostV3Generation1* host;~Pin(){if(host)host->Drop();}}pin{&host};
    const auto probe=[&]{result.lifetime_invoked=true;result.lifetime=source.lease_.Probe();
      if(result.lifetime.ok())return true;result.failure=Failure::lifetime;return false;};
    if(!probe())return result;
    const u64 length=source.length_;
    const bool reader=host.bounds_.carrier==api::StatementBlobInputCarrierV3::logical_reader;
    const u64 window=reader?std::min(length,host.bounds_.window_bytes):0;
    if(length>SIZE_MAX-sizeof(RowStageWork)||(reader&&length&&!window)){
      result.failure=Failure::extent_invalid;return result;}
    const u64 source_calls=window?length/window+(length%window!=0):0;
    const u64 sink_calls=(length?(reader?source_calls:1):0)+2;
    const u64 allocated=length+sizeof(RowStageWork);
    if(window>UINT64_MAX-allocated){result.failure=Failure::extent_invalid;return result;}
    const auto budget=host.ledger_.Snapshot();
    const auto need=[&](BlobLifetimeResourceV3 resource,u64 amount){
      const auto& counter=budget.counters[static_cast<std::size_t>(resource)];
      const bool valid=budget.frozen&&!budget.terminal_quarantined&&counter.invoked<=counter.limit&&
          counter.reserved_work<=counter.limit-counter.invoked&&
          counter.reserved_cleanup<=counter.limit-counter.invoked-counter.reserved_work;
      const u64 available=valid?counter.limit-counter.invoked-counter.reserved_work-counter.reserved_cleanup:0;
      if(valid&&amount<=available)return true;
      result.failure=Failure::resource_preflight;result.resource=resource;result.required=amount;result.available=available;return false;};
    if(!need(BlobLifetimeResourceV3::logical_bytes_read,length)||!need(BlobLifetimeResourceV3::reader_calls,source_calls)||
        !need(BlobLifetimeResourceV3::temporary_bytes,allocated+window)||
        !need(BlobLifetimeResourceV3::sink_bytes_written,length)||!need(BlobLifetimeResourceV3::sink_calls,sink_calls))return result;
    u64 available=0;
    if(!host.ledger_.Charge(BlobLifetimeResourceV3::temporary_bytes,allocated,&available)){
      result.failure=Failure::resource_preflight;result.resource=BlobLifetimeResourceV3::temporary_bytes;
      result.required=allocated;result.available=available;return result;}
    RowStageContext context{&host,&result};BlobLifetimeResourceV3 failed;
    if(!host.ledger_.ReserveSinkWork(length,sink_calls,failed,available)){
      result.failure=Failure::resource_preflight;result.resource=failed;
      result.required=failed==BlobLifetimeResourceV3::sink_calls?sink_calls:length;result.available=available;return result;}
    context.reserved_bytes=length;context.reserved_calls=sink_calls;
    try{
      context.allocated=static_cast<std::size_t>(allocated);
      context.allocation=static_cast<byte*>(host.memory_->memory()->allocate(context.allocated,alignof(RowStageWork)));
      context.work=new(context.allocation)RowStageWork{&context};
      result.failure=Failure::none;
      if(!context.Sink())return result;
      result.lifetime=length?(reader?source.lease_.VisitBoundReaderBytes(BlobTrustedInternalVisitorV3(StageRowBytes,context.work)):
          source.lease_.VisitBoundMaterializedBytes(BlobTrustedInternalVisitorV3(StageRowBytes,context.work))):source.lease_.Probe();
      if(!result.lifetime.ok()){result.failure=Failure::lifetime;return result;}
      if(result.failure!=Failure::none)return result;
      if(context.work->offset!=length||result.bytes_visited!=length){result.failure=Failure::internal_failure;return result;}
      if(!context.Sink())return result;
      context.DestroyWork();
      if(!probe())return result;
      // One exact owner transfer, not metadata reconstruction or a permission
      // boolean. All source observations and staged bytes precede publication.
      output.value_.used_=true;
      result.lifetime=output.value_.lease_.MoveReplaceFrom(std::move(source.lease_));
      if(!result.lifetime.ok()){result.failure=Failure::lifetime;return result;}
      output.value_.descriptor_=std::move(source.descriptor_);output.value_.profile_=source.profile_;
      output.value_.state_=source.state_;output.value_.length_=source.length_;output.value_.host_=source.host_;
      source.descriptor_={};source.profile_={};source.state_=BlobValueStateV3::sql_null;source.length_=0;source.host_=nullptr;source.used_=true;
      output.host_=&host;output.allocation_=context.allocation;output.allocated_=context.allocated;
      output.origin_=&host.memory_->binding();
      output.data_=context.allocation+sizeof(RowStageWork);output.size_=static_cast<std::size_t>(length);
      context.allocation=nullptr;pin.host=nullptr;return result;
    }catch(const std::bad_alloc&){result.failure=Failure::allocation_failed;return result;}
    catch(...){result.failure=Failure::internal_failure;return result;}
  }
  static void DestroyRowBytes(api::StatementBlobRowValueV3& output) noexcept {
    auto* host=output.host_;if(!host)return;
    auto* allocation=output.allocation_;const auto bytes=output.allocated_;
    output.host_=nullptr;output.allocation_=nullptr;output.data_=nullptr;output.size_=0;output.allocated_=0;
    output.origin_=nullptr;
    Erase(allocation,bytes);
    try{host->memory_->memory()->deallocate(allocation,bytes,alignof(RowStageWork));}catch(...){std::terminate();}
    host->Drop();
  }
 private:
  struct LogicalHashWork {
    BlobLogicalDigestPassV3 digest;
    hash::Blake3Digest staged{};
    api::StatementBlobHashResultV3* result=nullptr;
    ~LogicalHashWork() noexcept { Erase(staged.data(),staged.size()); }
  };
  static BlobProtectedWorkResultV3 LogicalHashInput(void* raw,std::span<const byte> bytes) noexcept {
    auto& work=*static_cast<LogicalHashWork*>(raw);
    work.result->bytes_visited+=bytes.size();
    work.result->computation=work.digest.Update(bytes);
    // A failure here is an impossible private computation/schedule defect, not
    // a fabricated reader IO or persisted-integrity error.
    return work.result->computation.ok() ? BlobProtectedWorkResultV3{0,{},{}} : BlobProtectedWorkResultV3{};
  }
 public:
  static api::StatementBlobHashResultV3 HashLogical(api::StatementBlobValueV3& source) noexcept {
    using Failure=api::StatementBlobHashFailureV3;
    api::StatementBlobHashResultV3 result;
    if(!source.bound() || !source.host_) return result;
    auto& host=*source.host_;
    if(host.bounds_.operation!=SB_BLOB_OP_HASH_V3) { result.failure=Failure::operation_not_admitted; return result; }
    // Cleanup custody for this already-retained operation, not a new factory
    // admission or persistent owner. It remains legal after root retirement;
    // real source gates still determine whether any computation may proceed.
    auto pins=host.pins_.load(std::memory_order_acquire);
    do {
      if(!pins || pins==UINT64_MAX) { result.failure=Failure::admission_closed; return result; }
    } while(!host.pins_.compare_exchange_weak(pins,pins+1,std::memory_order_acq_rel));
    struct Pin { BlobLifetimeReceiverHostV3Generation1* host; ~Pin() { host->Drop(); } } pin{&host};
    const auto probe=[&] {
      result.lifetime_invoked=true; result.lifetime=source.lease_.Probe();
      if(result.lifetime.ok()) return true;
      result.failure=Failure::lifetime; Erase(result.digest.data(),result.digest.size()); return false;
    };
    if(!probe()) return result;
    if(source.state_!=BlobValueStateV3::value) { result.failure=Failure::sql_null_has_no_content_hash; return result; }
    const auto length=source.length_;
    const bool reader=host.bounds_.carrier==api::StatementBlobInputCarrierV3::logical_reader;
    const u64 window=reader ? std::min(length,host.bounds_.window_bytes) : 0;
    const u64 calls=window ? length/window+(length%window!=0) : 0;
    const auto budget=host.ledger_.Snapshot();
    const auto need=[&](BlobLifetimeResourceV3 resource,u64 amount) {
      const auto& counter=budget.counters[static_cast<std::size_t>(resource)];
      const bool valid=budget.frozen && !budget.terminal_quarantined &&
          counter.invoked<=counter.limit && counter.reserved_work<=counter.limit-counter.invoked &&
          counter.reserved_cleanup<=counter.limit-counter.invoked-counter.reserved_work;
      const auto available=valid ? counter.limit-counter.invoked-counter.reserved_work-counter.reserved_cleanup : 0;
      if(valid && amount<=available) return true;
      result.failure=Failure::resource_preflight; result.resource=resource; result.required=amount; result.available=available;
      return false;
    };
    if(!need(BlobLifetimeResourceV3::logical_bytes_read,length) ||
        !need(BlobLifetimeResourceV3::reader_calls,calls) ||
        !need(BlobLifetimeResourceV3::temporary_bytes,sizeof(LogicalHashWork)+window)) return result;
    u64 available=0;
    if(!host.ledger_.Charge(BlobLifetimeResourceV3::temporary_bytes,sizeof(LogicalHashWork),&available)) {
      result.failure=Failure::resource_preflight; result.resource=BlobLifetimeResourceV3::temporary_bytes;
      result.required=sizeof(LogicalHashWork); result.available=available; return result;
    }
    struct Workspace {
      std::pmr::memory_resource* allocator; LogicalHashWork* work=nullptr;
      void Reset() noexcept {
        if(!work) return;
        work->~LogicalHashWork(); Erase(work,sizeof(LogicalHashWork));
        try { allocator->deallocate(work,sizeof(LogicalHashWork),alignof(LogicalHashWork)); } catch(...) { std::terminate(); }
        work=nullptr;
      }
      ~Workspace() noexcept { Reset(); }
    } workspace{host.memory_->memory()};
    try {
      auto* block=workspace.allocator->allocate(sizeof(LogicalHashWork),alignof(LogicalHashWork));
      workspace.work=new(block) LogicalHashWork;
      auto& work=*workspace.work; work.result=&result;
      result.computation=work.digest.Begin(source.profile_,length);
      if(!result.computation.ok()) { result.failure=Failure::digest_failed; return result; }
      const BlobTrustedInternalVisitorV3 visitor(LogicalHashInput,&work);
      result.lifetime=length ? reader ? source.lease_.VisitBoundReaderBytes(visitor) :
          source.lease_.VisitBoundMaterializedBytes(visitor) : source.lease_.Probe();
      if(!result.lifetime.ok()) { result.failure=Failure::lifetime; return result; }
      if(!result.computation.ok()) { result.failure=Failure::digest_failed; return result; }
      if(result.bytes_visited!=length) { result.failure=Failure::internal_failure; return result; }
      result.computation=work.digest.Finish(work.staged);
      if(!result.computation.ok()) { result.failure=Failure::digest_failed; return result; }
      result.digest=work.staged;
      // No source pin or payload allocation escapes this operation. The
      // existing retained value keeps the actual owner through these gates.
      workspace.Reset();
      if(!probe()) return result;
      result.failure=Failure::none; return result;
    } catch(const std::bad_alloc&) { result.failure=Failure::allocation_failed; return result; }
    catch(...) { result.failure=Failure::internal_failure; return result; }
  }
};
} // namespace scratchbird::core::datatypes

namespace scratchbird::engine::internal_api {
StatementBlobInputV3::~StatementBlobInputV3() noexcept { Reset(); }
void StatementBlobInputV3::Reset() noexcept { auto* host=host_; host_=nullptr; if(host) host->Close(); }
const core::datatypes::BlobValidatedProfileHandleV3* StatementBlobInputV3::profile() const noexcept {
  return host_ ? &host_->profile() : nullptr;
}
std::optional<std::array<core::datatypes::BlobLifetimeCounterV3,12>> StatementBlobInputV3::WorkSnapshot() noexcept {
  return host_ ? std::optional{host_->WorkSnapshot()} : std::nullopt;
}
StatementBlobInputRetainV3 StatementBlobInputV3::Retain(core::datatypes::BlobRetainedLifetimeLeaseV3& lease) noexcept {
  return host_ ? host_->Retain(lease) : StatementBlobInputRetainV3{};
}
StatementBlobValueResultV3 StatementBlobInputV3::RetainTyped(
    ExecutionTypeDescriptor&& descriptor,StatementBlobValueV3& destination) noexcept {
  return host_ ? host_->RetainTyped(std::move(descriptor),destination) : StatementBlobValueResultV3{};
}
StatementBlobValueResultV3 StatementBlobValueV3::CastIdentityInto(
    ExecutionTypeDescriptor&& target,core::datatypes::DatatypeCastContext context,
    StatementBlobValueV3& destination) noexcept {
  using Failure=StatementBlobValueFailureV3;
  StatementBlobValueResultV3 result;
  if(!bound()) return result;
  result.lifetime.status=SB_ENGINE_STATUS_INVALID_ARGUMENT;
  if(this==&destination) { result.failure=Failure::self_alias; return result; }
  if(destination.used_) { result.failure=Failure::destination_used; return result; }
  result.lifetime.runtime_invoked=true;
  result.lifetime.runtime=lease_.CastIdentityInto(profile_,descriptor_,target,context,destination.lease_);
  result.lifetime.status=result.lifetime.runtime.ok() ? SB_ENGINE_STATUS_OK : SB_ENGINE_STATUS_CONFLICT;
  if(!result.lifetime.ok()) { result.failure=Failure::lifetime; return result; }
  destination.descriptor_=std::move(target);
  destination.profile_=profile_; destination.state_=state_; destination.length_=length_;
  destination.host_=host_;
  destination.used_=true;
  result.failure=Failure::none; result.category=core::datatypes::DatatypeCastCategory::identity;
  return result;
}
core::datatypes::BlobLifetimeRuntimeResultV3 StatementBlobValueV3::Release() noexcept {
  const auto result=lease_.Release();
  if(!lease_.retained()) {
    descriptor_={}; profile_={}; state_=core::datatypes::BlobValueStateV3::sql_null; length_=0; host_=nullptr;
    // Even a failed retention can leave an internal terminal lease. Do not
    // promise that a retired typed wrapper can be rearmed as a fresh receiver.
    used_=true;
  }
  return result;
}
StatementBlobComponentResultV3 StatementBlobValueV3::EncodeComponent(StatementBlobComponentV3& output) noexcept {
  return core::datatypes::BlobLifetimeReceiverHostV3Generation1::EncodeComponent(*this,output);
}
StatementBlobComponentResultV3 StatementBlobValueV3::SerializeAndRetire(StatementBlobSerializedComponentV3& output) noexcept {
  return core::datatypes::BlobLifetimeReceiverHostV3Generation1::SerializeAndRetire(*this,output);
}
StatementBlobHashResultV3 StatementBlobValueV3::HashLogical() noexcept {
  return core::datatypes::BlobLifetimeReceiverHostV3Generation1::HashLogical(*this);
}
StatementBlobRowResultV3 StatementBlobValueV3::StageForRowAndRetire(StatementBlobRowValueV3& output) noexcept {
  return core::datatypes::BlobLifetimeReceiverHostV3Generation1::StageForRowAndRetire(*this,output);
}
StatementBlobRowValueV3::~StatementBlobRowValueV3() noexcept {(void)Release();}
core::datatypes::BlobLifetimeRuntimeResultV3 StatementBlobRowValueV3::Release() noexcept {
  const auto result=value_.Release();
  core::datatypes::BlobLifetimeReceiverHostV3Generation1::DestroyRowBytes(*this);
  return result;
}
StatementBlobHashResultV3 StatementBlobRowValueV3::HashLogical() noexcept {return value_.HashLogical();}
StatementBlobComponentV3::~StatementBlobComponentV3() noexcept { Reset(); }
void StatementBlobComponentV3::Reset() noexcept {
  core::datatypes::BlobLifetimeReceiverHostV3Generation1::DestroyComponent(*this);
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
StatementBlobEncodedInputV3::~StatementBlobEncodedInputV3() noexcept { Reset(); }
void StatementBlobEncodedInputV3::Reset() noexcept { auto* host=host_; host_=nullptr; if(host) host->Close(); }
const core::datatypes::BlobValidatedProfileHandleV3* StatementBlobEncodedInputV3::profile() const noexcept {
  return host_ ? &host_->profile() : nullptr;
}
std::optional<std::uint64_t> StatementBlobEncodedInputV3::extent() const noexcept {
  return host_ ? std::optional{host_->extent()} : std::nullopt;
}
std::optional<std::array<core::datatypes::BlobLifetimeCounterV3,12>> StatementBlobEncodedInputV3::WorkSnapshot() noexcept {
  return host_ ? std::optional{host_->WorkSnapshot()} : std::nullopt;
}
StatementBlobInputRetainV3 StatementBlobEncodedInputV3::Retain(core::datatypes::BlobRetainedLifetimeLeaseV3& lease) noexcept {
  return host_ ? host_->Retain(lease) : StatementBlobInputRetainV3{};
}
StatementBlobDecodeResultV3 StatementBlobEncodedInputV3::DecodeComponent(
    ExecutionTypeDescriptor&& target,StatementBlobDecodedComponentV3& output) noexcept {
  return host_ ? host_->DecodeComponent(std::move(target),output) : StatementBlobDecodeResultV3{};
}
StatementBlobDecodedComponentV3::~StatementBlobDecodedComponentV3() noexcept { Reset(); }
void StatementBlobDecodedComponentV3::Reset() noexcept {
  core::datatypes::BlobLifetimeReceiverHostV3Generation1::DestroyDecoded(*this);
}
StatementBlobInputAdmissionV3 ReceiveStatementBlobEncodedInputV3(server_engine_bridge::StatementContextReceiptHandle handle,
    const wire::TypedResultUuid& session,const wire::TypedResultUuid& receipt,std::span<const core::platform::byte> input,
    const StatementBlobEncodedInputBoundsV3& bounds,StatementBlobEncodedInputV3* output,core::platform::DiagnosticRecord* diagnostic) noexcept {
  if(diagnostic) *diagnostic={};
  StatementBlobInputAdmissionV3 result;
  if(!output) return result;
  if(output->bound()) { result.failure=StatementBlobInputAdmissionFailureV3::output_in_use; return result; }
  result.status=core::datatypes::BlobLifetimeReceiverHostV3Generation1::CreateEncoded(handle,session,receipt,input,bounds,
      &output->host_,result,diagnostic);
  return result;
}
} // namespace scratchbird::engine::internal_api
