// SPDX-License-Identifier: MPL-2.0
// Reuse only component fixtures, not their success assertions as a new oracle.
#define main ExistingProducerRuntimeMain
#include "typed_result_producer_cursor_runtime_test.cpp"
#undef main
#include <cstdio>
#include <cstdlib>
#include <new>
namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
thread_local void* staged_pointer = nullptr;
thread_local bool staged_live = false;
thread_local bool watch_packet = false;
thread_local std::size_t largest_packet_allocation = 0;
thread_local std::size_t large_packet_allocations = 0;
void Arm(long n) { remaining = n; hit = false; }
void Off() { remaining = -1; }
}
#ifndef SB_PRODUCER_NO_ALLOC_OVERRIDE
void* operator new(std::size_t n) {
  if (fault::watch_packet)
    fault::largest_packet_allocation = std::max(fault::largest_packet_allocation, n);
  if (fault::watch_packet && n >= 1024) ++fault::large_packet_allocations;
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (auto p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void operator delete(void* p) noexcept {
  if (p && p == fault::staged_pointer) fault::staged_live = false;
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
namespace {
unsigned checks = 0, failures = 0, injected = 0;
void Check(bool okay, const char* why) {
  ++checks; if (!okay) { ++failures; std::fprintf(stderr, "FAIL %s\n", why); }
}
void Released(const std::shared_ptr<AuthorityControl>& a, const std::shared_ptr<ProducerControl>& p) {
  Check(a->statement_releases == 1 && a->mga_releases == 1 &&
        a->cancellation_releases == 1 && a->grant_releases == 1 && p->close_calls == 1,
        "consumed request did not release every owner exactly once");
}
#ifndef SB_PRODUCER_NO_ALLOC_OVERRIDE
void OpenFaults() {
  bool complete = false;
  for (long n = 0; n != 8192 && !complete; ++n) {
    auto a = std::make_shared<AuthorityControl>();
    auto p = std::make_shared<ProducerControl>();
    auto request = OpenRequest(a, p);
    api::TypedResultProducerOpenResultV1 result;
    bool escaped = false;
    fault::Arm(n);
    try { result = api::OpenTypedResultProducerCursorV1(std::move(request)); }
    catch (const std::bad_alloc&) { escaped = true; }
    fault::Off();
    Check(!escaped, "Open allocation failure escaped typed API");
    if (fault::hit) ++injected;
    else { complete = true; Check(result.ok(), "uninjected Open refused"); }
    result.carrier.reset();
    Released(a, p);
  }
  Check(complete, "Open allocation sweep incomplete");
}
void PullFaults(bool terminal) {
  bool complete = false;
  for (long n = 0; n != 8192 && !complete; ++n) {
    auto f = Opened();
    f.producer->staged.push_back(Batch({Row(0, std::string(1024, 'x'))}, terminal));
    auto request = PullRequest();
    api::TypedResultProducerPullResultV1 result;
    bool escaped = false;
    fault::Arm(n);
    try { result = api::PullTypedResultProducerCursorV1(*f.carrier, request); }
    catch (const std::bad_alloc&) { escaped = true; }
    fault::Off();
    Check(!escaped, "Pull allocation failure escaped typed API");
    auto snapshot = f.carrier->Snapshot();
    Check(snapshot.lifecycle != api::TypedResultProducerCursorLifecycleV1::pulling,
          "failed Pull left carrier stuck pulling");
    const bool published = result.ok() && result.outcome == api::TypedResultProducerPullOutcomeV1::batch;
    if (published) {
      Check(snapshot.row_position == 1 && snapshot.next_batch_ordinal == 1 &&
            f.producer->stage_commits == 1, "published Pull did not commit exactly once");
    } else {
      Check(snapshot.row_position == 0 && snapshot.next_batch_ordinal == 0 &&
            f.producer->stage_commits == 0 && result.row_data_packet.empty(),
            "failed Pull advanced or published partial result");
      if (snapshot.lifecycle == api::TypedResultProducerCursorLifecycleV1::open) {
        auto retry = api::PullTypedResultProducerCursorV1(*f.carrier, request);
        Check(retry.ok() && retry.row_count == 1 && f.producer->stage_commits == 1,
              "allocation failure prevented exact retry");
      }
    }
    if (fault::hit) ++injected; else { complete = true; Check(published, "uninjected Pull refused"); }
    f.carrier.reset(); Released(f.authority, f.producer);
  }
  Check(complete, "Pull allocation sweep incomplete");
}
#endif
}
struct Lifetime { int sources = 0, captures = 0, grants = 0, closes = 0; bool early_release = false; };
struct DescriptorCapture {
  std::shared_ptr<Lifetime> life;
  explicit DescriptorCapture(std::shared_ptr<Lifetime> p) : life(std::move(p)) { ++life->captures; }
  ~DescriptorCapture() { --life->captures; }
};
class OwnedSource final : public api::TypedResultProducerSourceV1 {
 public:
  OwnedSource(std::shared_ptr<Lifetime> life, std::shared_ptr<ProducerControl> p)
      : life_(std::move(life)), producer_(std::move(p)), storage_(131072, 0xa5) { ++life_->sources; }
  ~OwnedSource() override { --life_->sources; }
  api::TypedResultProducerStageResultV1 Stage(const api::TypedResultProducerStageRequestV1& r) override {
    auto result = producer_.Stage(r);
#ifndef SB_PRODUCER_NO_ALLOC_OVERRIDE
    if (!result.rows.empty()) {
      fault::staged_pointer = result.rows[0].cells[0].canonical_payload.data();
      fault::staged_live = true;
    }
#endif
    return result;
  }
  void Close(api::TypedResultProducerReleaseReasonV1 r) noexcept override { ++life_->closes; producer_.Close(r); }
 private:
  std::shared_ptr<Lifetime> life_;
  Producer producer_;
  std::vector<byte> storage_;
};
class OwnedGrant final : public api::TypedResultResourceGrantReceiptHandleV1 {
 public:
  OwnedGrant(std::shared_ptr<Lifetime> life, std::shared_ptr<AuthorityControl> a) : life_(std::move(life)), grant_(std::move(a)) {}
  api::TypedResultProducerGrantObservationV1 ObserveGrant(const wire::TypedResultUuid& id,
      std::uint64_t generation, std::uint64_t ceiling, std::uint64_t bytes) override {
    return grant_.ObserveGrant(id, generation, ceiling, bytes);
  }
  void Release(api::TypedResultProducerReleaseReasonV1 r) noexcept override {
    ++life_->grants;
    life_->early_release |= life_->sources != 0 || life_->captures != 0 || fault::staged_live;
    grant_.Release(r);
  }
 private:
  std::shared_ptr<Lifetime> life_;
  ResourceGrant grant_;
};
void TerminalStorageOwnership() {
  for (unsigned mode = 0; mode != 7; ++mode) {
    fault::staged_pointer = nullptr; fault::staged_live = false;
    auto a = std::make_shared<AuthorityControl>();
    auto p = std::make_shared<ProducerControl>();
    auto life = std::make_shared<Lifetime>();
    auto request = OpenRequest(a, p);
    request.producer_state = std::make_unique<OwnedSource>(life, p);
    request.resource_grant_receipt = std::make_unique<OwnedGrant>(life, a);
    request.descriptor_authority = [capture = std::make_shared<DescriptorCapture>(life)](const auto&) {
      wire::TypedResultDescriptorAuthorityDecision result;
      result.accepted = capture->life->captures == 1; return result;
    };
    if (mode == 4) request.version = 0;
    auto opened = api::OpenTypedResultProducerCursorV1(std::move(request));
    if (mode != 4) {
      Check(opened.ok(), "terminal ownership Open");
      if (mode == 0) {
        fault::Arm(0);
        const auto closed = api::CloseTypedResultProducerCursorV1(*opened.carrier, api::TypedResultProducerCloseReasonV1::explicit_close);
        bool allocated = fault::hit; fault::Off();
        Check(closed.ok() && !allocated, "terminal explicit close allocated");
      } else if (mode == 5) {
        fault::Arm(0); opened.carrier.reset();
        bool allocated = fault::hit; fault::Off();
        Check(!allocated, "terminal carrier destructor allocated");
      } else {
        p->staged.push_back(mode == 2 ? EmptyEos() : Batch({Row(0, "terminal-payload")}, true));
        if (mode == 3) p->on_stage = [a](const auto&) {
          a->cancellation_script = {api::TypedResultProducerCancellationObservationV1::requested};
          a->cancellation_index = 0;
        };
        if (mode == 6) p->on_stage = [a](const auto&) {
          a->owner = api::TypedResultProducerOwnerObservationV1::denied;
        };
        auto result = api::PullTypedResultProducerCursorV1(*opened.carrier, PullRequest());
        Check(mode == 3 ? result.status == api::TypedResultProducerCursorStatusV1::cancelled :
              mode == 6 ? result.status == api::TypedResultProducerCursorStatusV1::access_denied : result.ok() && result.end_of_cursor,
              "terminal Pull outcome");
      }
      if (opened.carrier) {
        auto snapshot = opened.carrier->Snapshot();
        Check(snapshot.retained_authority_released && snapshot.row_descriptor_uuid == Uuid(0x31) &&
              snapshot.row_descriptor_generation == 7, "terminal snapshot lost frozen binary identity");
        Check(opened.carrier->result_descriptor_vector().empty() &&
              opened.carrier->row_descriptor().columns.empty(), "terminal carrier retained descriptor storage");
      }
    } else Check(!opened.ok(), "malformed Open accepted");
    Check(life->grants == 1 && life->closes == 1 && life->sources == 0 &&
          life->captures == 0 && !life->early_release, "source or descriptor capture outlived grant release");
    opened.carrier.reset(); Released(a, p);
    Check(life->grants == 1 && life->closes == 1, "terminal destruction repeated cleanup");
  }
}
void ObserverAllocationFailures() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto a = std::make_shared<AuthorityControl>();
    auto p = std::make_shared<ProducerControl>();
    auto armed = std::make_shared<bool>(mode == 0);
    auto request = OpenRequest(a, p);
    request.descriptor_authority = [armed](const auto&) {
      if (*armed) throw std::bad_alloc();
      wire::TypedResultDescriptorAuthorityDecision result; result.accepted = true; return result;
    };
    auto opened = api::OpenTypedResultProducerCursorV1(std::move(request));
    if (mode == 0) Check(opened.status == api::TypedResultProducerCursorStatusV1::resource_budget_exceeded,
                         "Open observer OOM impersonated descriptor refusal");
    else {
      Check(opened.ok(), "observer failure fixture Open");
      p->staged.push_back(Batch({Row(0, "retry-data")}));
      if (mode == 1) *armed = true;
      else p->on_stage = [](const auto&) { throw std::bad_alloc(); };
      auto failed = api::PullTypedResultProducerCursorV1(*opened.carrier, PullRequest());
      Check(failed.status == api::TypedResultProducerCursorStatusV1::resource_budget_exceeded &&
            opened.carrier->Snapshot().lifecycle == api::TypedResultProducerCursorLifecycleV1::open,
            "observer/producer OOM impersonated stale authority or stranded cursor");
      *armed = false; p->on_stage = {};
      auto retried = api::PullTypedResultProducerCursorV1(*opened.carrier, PullRequest());
      Check(retried.ok() && retried.row_count == 1 && p->stage_commits == 1, "observer OOM prevented exact retry");
    }
    opened.carrier.reset(); Released(a, p);
  }
}

// Observe real encoder allocations only after the independent source has
// supplied its staged rows. Source allocation itself is a separate obligation.
class PacketAdmissionSource final : public api::TypedResultProducerSourceV1 {
 public:
  explicit PacketAdmissionSource(std::shared_ptr<ProducerControl> control)
      : source_(std::move(control)) {}
  api::TypedResultProducerStageResultV1 Stage(
      const api::TypedResultProducerStageRequestV1& request) override {
    auto result = source_.Stage(request);
    fault::largest_packet_allocation = 0;
    fault::watch_packet = true;
    return result;
  }
  void Close(api::TypedResultProducerReleaseReasonV1 reason) noexcept override {
    source_.Close(reason);
  }
 private:
  Producer source_;
};
void PacketAdmissionBeforeCopy() {
  for (bool revoke : {false, true}) {
    auto a = std::make_shared<AuthorityControl>();
    auto p = std::make_shared<ProducerControl>();
    auto request = OpenRequest(a, p);
    request.producer_state = std::make_unique<PacketAdmissionSource>(p);
    auto opened = api::OpenTypedResultProducerCursorV1(std::move(request));
    Check(opened.ok(), "packet allocation observer could not open producer");
    if (!opened.ok()) continue;
    p->staged.push_back(Batch({Row(0, std::string(8192, 'x'))}));
    if (revoke) p->on_stage = [a](const auto&) {
      a->owner = api::TypedResultProducerOwnerObservationV1::denied;
    };
    auto pull = PullRequest();
    pull.maximum_bytes = 512;
    auto refused = api::PullTypedResultProducerCursorV1(*opened.carrier, pull);
    fault::watch_packet = false;
    Check(fault::largest_packet_allocation < 1024,
          "producer did not pass admitted byte ceiling to encoder before copying");
    Check(refused.status == (revoke ? api::TypedResultProducerCursorStatusV1::access_denied
                                  : api::TypedResultProducerCursorStatusV1::resource_budget_exceeded),
          "packet admission changed retained-authority refusal priority");
    Check(refused.row_count == 0 && refused.row_data_packet.empty() &&
              p->stage_commits == 0 && p->stage_aborts == 1 && p->next == 0,
          "packet admission refusal consumed the staged source");
    if (!revoke) {
      auto retried = api::PullTypedResultProducerCursorV1(*opened.carrier, PullRequest());
      fault::watch_packet = false;
      Check(retried.ok() && retried.row_count == 1 &&
                retried.batch.batch_ordinal == 0 && p->stage_commits == 1 &&
                retried.batch.rows[0].cells[0].canonical_payload == std::vector<byte>(8192, 'x'),
            "larger admitted retry lost source values or batch ordinal");
    }
    opened.carrier.reset(); Released(a, p);
  }
}

void ConsumingCodecOwnership(const wire::TypedResultBatch& original,
                            const wire::TypedResultRowDescriptor& descriptor,
                            const wire::TypedResultCarrierBinding& binding) {
  const auto reference = wire::EncodeTypedResultBatch(original, descriptor, binding);
  Check(reference.ok() && reference.batch.rows[0].cells[0].canonical_payload.data() !=
            original.rows[0].cells[0].canonical_payload.data(),
        "const owning encoder retains independent copy semantics");
  for (unsigned refusal = 0; refusal != 5; ++refusal) {
    auto input = original;
    auto carrier = binding;
    auto schema = descriptor;
    if (refusal == 1) input.rows[0].row_ordinal = 9;
    if (refusal == 2) input.batch_evidence_sha256[0] ^= 1;
    if (refusal == 3) carrier.kind = static_cast<wire::TypedResultCarrierKind>(255);
    if (refusal == 4) ++schema.descriptor_generation;
    const auto* rows = input.rows.data();
    const auto* cells = input.rows[0].cells.data();
    const auto* payload = input.rows[0].cells[0].canonical_payload.data();
    const auto evidence = input.batch_evidence_sha256;
    const auto ordinal = input.rows[0].row_ordinal;
    auto result = wire::EncodeTypedResultBatch(std::move(input), schema, carrier,
        refusal == 0 ? 1 : 65536);
    Check(!result.ok() && result.encoded.empty() && result.batch.rows.empty(),
          "consuming encoder refusal exposes no partial packet or rows");
    Check(input.rows.data() == rows && input.rows[0].cells.data() == cells &&
              input.rows[0].cells[0].canonical_payload.data() == payload &&
              input.batch_evidence_sha256 == evidence && input.rows[0].row_ordinal == ordinal,
          "consuming encoder semantic refusal leaves all caller storage and evidence intact");
  }
  bool complete = false;
  for (long n = 0; n != 512 && !complete; ++n) {
    auto input = original;
    const auto* rows = input.rows.data();
    const auto* cells = input.rows[0].cells.data();
    const auto* payload = input.rows[0].cells[0].canonical_payload.data();
    wire::TypedResultBatchCodecResult result;
    bool threw = false;
    fault::Arm(n);
    try { result = wire::EncodeTypedResultBatch(std::move(input), descriptor, binding); }
    catch (const std::bad_alloc&) { threw = true; }
    fault::Off();
    if (fault::hit) ++injected;
    else complete = true;
    if (threw) {
      Check(input.rows.data() == rows && input.rows[0].cells.data() == cells &&
                input.rows[0].cells[0].canonical_payload.data() == payload &&
                input.rows[0].cells[0].canonical_payload == original.rows[0].cells[0].canonical_payload &&
                input.batch_evidence_sha256 == original.batch_evidence_sha256,
            "consuming encoder allocation failure leaves caller ownership intact");
      result = wire::EncodeTypedResultBatch(std::move(input), descriptor, binding);
    }
    Check(result.ok() && result.encoded == reference.encoded,
          "consuming encoder success or unchanged-input retry preserves exact packet");
    if (!result.ok() || result.batch.rows.empty()) continue;
    Check(result.batch.rows.data() == rows && result.batch.rows[0].cells.data() == cells &&
              result.batch.rows[0].cells[0].canonical_payload.data() == payload &&
              input.rows.empty(),
          "consuming encoder transfers all original row cell and payload storage");
    fault::Arm(0);
    result = {};
    fault::Off();
    Check(!fault::hit, "consumed batch and packet teardown allocate nothing");
  }
  Check(complete, "consuming encoder allocation sweep completes");
}

class TransferObservedSource final : public api::TypedResultProducerSourceV1 {
 public:
  TransferObservedSource(std::shared_ptr<ProducerControl> control,
                         const byte** payload)
      : source_(std::move(control)), payload_(payload) {}
  api::TypedResultProducerStageResultV1 Stage(
      const api::TypedResultProducerStageRequestV1& request) override {
    auto result = source_.Stage(request);
    *payload_ = result.rows.front().cells.front().canonical_payload.data();
    fault::large_packet_allocations = 0;
    fault::watch_packet = true;
    return result;
  }
  void Close(api::TypedResultProducerReleaseReasonV1 reason) noexcept override {
    source_.Close(reason);
  }
 private:
  Producer source_;
  const byte** payload_;
};

void ProducerTransfersValidatedPayload() {
  for (const bool terminal : {false, true}) {
    auto authority = std::make_shared<AuthorityControl>();
    auto source = std::make_shared<ProducerControl>();
    auto request = OpenRequest(authority, source);
    const auto descriptor = request.row_descriptor;
    const byte* staged_payload = nullptr;
    request.producer_state =
        std::make_unique<TransferObservedSource>(source, &staged_payload);
    auto opened = api::OpenTypedResultProducerCursorV1(std::move(request));
    Check(opened.ok(), "transfer fixture opened");
    if (!opened.ok()) continue;
    std::string payload(8192, 'z');
    payload[0] = '\0'; payload[4096] = '|'; payload.back() = '=';
    source->staged.push_back(Batch({Row(0, payload)}, terminal));
    const byte* gate_payload = nullptr;
    const byte* gate_packet = nullptr;
    const api::TypedResultProducerPrecommitGateV1 gate = [&](const auto& candidate) {
      if (candidate.batch.rows.size() == 1) {
        gate_payload = candidate.batch.rows[0].cells[0].canonical_payload.data();
        gate_packet = candidate.row_data_packet.data();
      }
      api::TypedResultProducerPrecommitDecisionV1 decision;
      decision.accepted = true;
      return decision;
    };
    auto published = api::PullTypedResultProducerCursorV1(
        *opened.carrier, PullRequest(), gate);
    fault::watch_packet = false;
    Check(published.ok() && published.row_count == 1,
          "transfer publishes the actual staged row");
    Check(gate_payload != nullptr && gate_payload != staged_payload,
          "precommit candidate storage is independent of source grant teardown");
    Check(fault::large_packet_allocations == 2,
          "actual producer allocates one encoded packet and one independent owning payload");
    if (!published.ok() || published.batch.rows.size() != 1) continue;
    Check(published.batch.rows[0].cells[0].canonical_payload.data() == gate_payload &&
              published.row_data_packet.data() == gate_packet,
          "publication transfers already-validated candidate storage without another copy");
    Check(published.batch.rows[0].cells[0].canonical_payload ==
              std::vector<byte>(payload.begin(), payload.end()),
          "transferred payload preserves every binary byte");
    Check(source->stage_commits == 1 && source->stage_aborts == 0 &&
              published.end_of_cursor == terminal,
          "transfer retains the actual source publication barrier");
    opened.carrier.reset();
    Released(authority, source);
    Check(published.batch.rows[0].cells[0].canonical_payload.data() == gate_payload &&
              published.batch.rows[0].cells[0].canonical_payload.back() == '=',
          "returned batch owns payload after cursor and retained authority cleanup");

    wire::TypedResultCarrierBinding binding;
    binding.kind = wire::TypedResultCarrierKind::ps_fetch_result_v1;
    binding.row_count = 1;
    binding.end_of_rowset = terminal;
    binding.execution_uuid = published.batch.execution_uuid;
    binding.result_set_uuid = published.batch.result_set_uuid;
    binding.snapshot_uuid = published.batch.snapshot_uuid;
    binding.cursor_uuid = published.batch.cursor_uuid;
    binding.cursor_stream_descriptor_uuid = Uuid(0x49);
    binding.cursor_stream_descriptor_version = 1;
    binding.cursor_stream_descriptor_generation = 10;
    wire::TypedResultCursorBatchState state;
    const auto decoded = wire::DecodeTypedResultBatch(
        published.row_data_packet, descriptor, binding, &state);
    Check(decoded.ok() && decoded.batch.rows[0].cells[0].canonical_payload ==
              published.batch.rows[0].cells[0].canonical_payload &&
              decoded.batch.batch_evidence_sha256 == published.batch.batch_evidence_sha256 &&
              decoded.batch.descriptor_evidence_sha256 == published.batch.descriptor_evidence_sha256,
          "transferred owning batch and verified packet have identical payload and evidence");
    if (!terminal) ConsumingCodecOwnership(published.batch, descriptor, binding);
  }
}

int main() {
#ifndef SB_PRODUCER_NO_ALLOC_OVERRIDE
  OpenFaults(); PullFaults(false); PullFaults(true);
  PacketAdmissionBeforeCopy();
  ProducerTransfersValidatedPayload();
#else
  Check(ExistingProducerRuntimeMain() == 0, "existing concurrent cursor lifecycle contracts");
#endif
  TerminalStorageOwnership(); ObserverAllocationFailures();
  std::cout << checks << " checks; " << injected << " allocation faults; " << failures << " failures\n";
  return failures ? 1 : 0;
}
