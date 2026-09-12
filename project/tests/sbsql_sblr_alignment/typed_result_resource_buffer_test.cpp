// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ExistingTypedResultFixtureMain
#include "typed_result_transport_codec_test.cpp"
#undef main
#include "reservation_backed_memory_resource.hpp"
#include "wire/typed_result_resource_buffer.hpp"
#include "wire/typed_result_packet_view.hpp"
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <optional>
#include <type_traits>

namespace buffer_fault {
thread_local long remaining = -1;
thread_local bool hit = false, watch = false;
thread_local std::size_t exact = 0, count = 0, reject_size = 0;
void Arm(long point) { remaining = point; hit = false; }
void Off() { remaining = -1; watch = false; reject_size = 0; }
}
void* operator new(std::size_t bytes) {
  if (buffer_fault::watch && bytes == buffer_fault::exact) ++buffer_fault::count;
  if ((buffer_fault::remaining >= 0 && buffer_fault::remaining-- == 0) ||
      (buffer_fault::reject_size && bytes >= buffer_fault::reject_size)) {
    buffer_fault::remaining = 0; buffer_fault::hit = true; throw std::bad_alloc();
  }
  if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  return ::operator new(bytes, std::nothrow);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
unsigned checks = 0, failures = 0, injected = 0;
void Check(bool good, const char* why) {
  ++checks;
  if (!good) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
void ActualOwningScratch() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  for (const std::size_t bytes : {257u, 4097u, 65537u}) {
    const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, std::vector<byte>(bytes, 'x'))})});
    const auto binding = ExecuteBinding(batch);
    // Independent required layout:16-byte row +20-byte cell +32-byte scalar.
    buffer_fault::exact = 16 + 20 + 32 + bytes;
    buffer_fault::count = 0; buffer_fault::watch = true;
    const auto encoded = wire::EncodeTypedResultBatch(batch, descriptor, binding);
    buffer_fault::Off();
    Check(encoded.ok(), "owning single-buffer batch serialization");
    Check(buffer_fault::count == 0, "actual batch still allocates a separate whole row-area scratch");
    Check(encoded.encoded.size() == 224 + 16 + 20 + 32 + bytes, "independent complete packet extent");
  }
}
}
namespace {

namespace mem = scratchbird::core::memory;
mem::MemoryBinaryUuid OwnerId(unsigned n) {
  mem::MemoryBinaryUuid id{}; id[0] = 1; id[6] = 0x70; id[8] = 0x80;
  id[15] = static_cast<byte>(n); return id;
}
mem::AllocationPolicy PacketPolicy(mem::u64 limit) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.hard_limit_bytes = limit; policy.per_context_limit_bytes = 0;
  policy.zero_memory_on_allocate = true; policy.zero_memory_on_release = true;
  return policy;
}
mem::ReservationBackedMemoryResourceAcquireResult PacketGrant(
    mem::MemoryManager& manager, mem::HierarchicalMemoryBudgetLedger& ledger,
    mem::u64 bytes) {
  mem::ReservationBackedMemoryResourceRequest request;
  request.memory_manager = &manager; request.reservation_ledger = &ledger;
  request.category = mem::MemoryCategory::executor_query_reserved;
  request.consumer_kind = mem::ReservationBackedMemoryConsumerKind::result_frame;
  request.route_label = "typed packet"; request.operation_id = "serialize result packet";
  request.requested_bytes = bytes;
  request.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  request.provenance.source_label = "actual allocator qualification policy";
  for (unsigned i = 0; i != 7; ++i) request.binary_ownership.scopes[i] = OwnerId(i + 1);
  using Kind = mem::HierarchicalMemoryScopeKind;
  request.scope_chain.push_back({Kind::process, {}, OwnerId(20)});
  for (const auto kind : {Kind::database, Kind::session, Kind::transaction, Kind::statement, Kind::query}) {
    const auto binary_kind = mem::HierarchicalMemoryBinaryScopeKind(kind);
    request.scope_chain.push_back({kind, {}, request.binary_ownership[binary_kind]});
  }
  for (const auto& scope : request.scope_chain)
    Check(ledger.SetBudget({scope, bytes, 0, request.provenance}).ok(), "real binary parent budget");
  return mem::AcquireReservationBackedMemoryResource(std::move(request));
}
void PhysicalCharge(const mem::MemoryManager& manager, mem::u64 live, mem::u64 unused) {
  const auto snapshot = manager.Snapshot();
  Check(snapshot.current_bytes == live && snapshot.sharded_accounting_current_bytes == live &&
        snapshot.reserved_capacity_bytes == unused &&
        snapshot.active_allocation_count == (live ? 1u : 0u),
        "packet storage not charged to the real physical allocator");
  if (live) {
    for (unsigned i = 0; i != 7; ++i) {
      const mem::MemoryBinaryScopeKey wanted{static_cast<mem::MemoryBinaryScopeKind>(i), OwnerId(i + 1)};
      auto found = std::find_if(snapshot.contexts.begin(), snapshot.contexts.end(), [&](const auto& row) {
        return row.binary_scope && *row.binary_scope == wanted;
      });
      Check(found != snapshot.contexts.end() && found->scope_id.empty() && found->current_bytes == live,
            "actual packet lost a binary physical owner");
    }
  }
}
void ResourcePacketLifetime() {
  static_assert(!std::is_copy_constructible_v<wire::TypedResultResourceBuffer>);
  static_assert(std::is_nothrow_move_constructible_v<wire::TypedResultResourceBuffer>);
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, std::vector<byte>(65537, 'x'))})});
  const auto binding = ExecuteBinding(batch);
  const auto reference = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  Check(reference.ok(), "reference real packet");
  const auto bytes = reference.encoded.size();
  mem::MemoryManager manager(PacketPolicy(bytes));
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  auto grant = PacketGrant(manager, ledger, bytes);
  Check(grant.ok(), "exact packet capacity grant");
  if (!grant.ok()) return;
  {
  mem::ReservationBackedPmrMemoryResource resource(grant.resource.get(), "typed packet");
  PhysicalCharge(manager, 0, bytes);
  std::optional<wire::TypedResultResourceBuffer> packet;
  // The actual allocator uses its own physical backend; an accidental PMR
  // fallback must not succeed by charging an unrelated default resource.
  auto* previous_default = std::pmr::set_default_resource(std::pmr::null_memory_resource());
  buffer_fault::reject_size = 1024;
  bool escaped = false;
  try { packet.emplace(wire::EncodeTypedResultBatchBuffer(batch, descriptor, binding, resource)); }
  catch (...) { escaped = true; }
  buffer_fault::Off();
  std::pmr::set_default_resource(previous_default);
  Check(!escaped && packet && packet->ok(), "resource packet still makes a proportional ordinary allocation");
  if (!packet || !packet->ok()) return;
  Check(packet->encoded.get_allocator().resource() == &resource &&
        std::equal(packet->encoded.begin(), packet->encoded.end(), reference.encoded.begin(), reference.encoded.end()),
        "resource packet allocator or exact bytes changed");
  Check(packet->descriptor_evidence_sha256 == reference.batch.descriptor_evidence_sha256 &&
        packet->batch_evidence_sha256 == reference.batch.batch_evidence_sha256, "resource evidence differs");
  PhysicalCharge(manager, bytes, 0);
  Check(resource.Snapshot().allocation_count == 1 && resource.Snapshot().allocated_bytes == bytes,
        "serialization used more than one real packet buffer");
  {
  buffer_fault::reject_size = 1024;
  bool view_escaped = false;
  wire::TypedResultPacketViewResult viewed;
  try { viewed = wire::DecodeTypedResultPacketView(packet->encoded.data(), packet->encoded.size(), descriptor, binding); }
  catch (...) { view_escaped = true; }
  buffer_fault::Off();
  Check(!view_escaped && viewed.ok() && viewed.view.data() == packet->encoded.data() &&
        viewed.view.size() == bytes, "view of physical packet copied or detached storage");
  wire::TypedResultPacketRowView viewed_row;
  wire::TypedResultPacketCellView viewed_cell;
  Check(viewed.view.rows().Next(&viewed_row) && viewed_row.cells().Next(&viewed_cell) &&
        viewed_cell.value.payload_data == packet->encoded.data() + 224 + 16 + 20 + 32 &&
        viewed_cell.value.payload_bytes == 65537, "view payload is not the actual grant-owned buffer");
  }
  auto ordinary = manager.Allocate(1, 0, {});
  Check(!ordinary.ok(), "ordinary allocation stole live packet capacity");
  if (ordinary.ok()) manager.allocator()->DeallocateNoAlloc(ordinary.pointer);
  auto* pointer = packet->encoded.data();
  buffer_fault::Arm(0);
  auto moved = std::move(*packet);
  packet.reset();
  const bool move_allocated = buffer_fault::hit; buffer_fault::Off();
  Check(!move_allocated && moved.encoded.data() == pointer, "moving packet allocated or detached storage");
  // Parent revocation cannot free bytes while the exported packet still owns them.
  const auto revoked = ledger.CleanupOwner(OwnerId(2));
  Check(!revoked.ok() && !grant.resource->active(), "live parent revocation lost retained packet");
  PhysicalCharge(manager, bytes, 0);
  Check(std::equal(moved.encoded.begin(), moved.encoded.end(), reference.encoded.begin(), reference.encoded.end()),
        "revocation changed exported bytes");
  buffer_fault::Arm(0);
  { auto last = std::move(moved); }
  const bool release_allocated = buffer_fault::hit; buffer_fault::Off();
  Check(!release_allocated, "packet teardown allocated under pressure");
  PhysicalCharge(manager, 0, bytes);
  }
  grant.resource.reset();
  PhysicalCharge(manager, 0, 0);
  Check(ledger.Snapshot().current_bytes == 0, "packet owner retained hierarchy charge after teardown");
}
void RefusalAndRetry() {
  auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  auto original = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))})});
  const auto binding = ExecuteBinding(original);
  const auto reference = wire::EncodeTypedResultBatch(original, descriptor, binding);
  const auto bytes = reference.encoded.size();
  mem::MemoryManager manager(PacketPolicy(bytes));
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  auto grant = PacketGrant(manager, ledger, bytes);
  Check(grant.ok(), "refusal grant setup"); if (!grant.ok()) return;
  mem::ReservationBackedPmrMemoryResource resource(grant.resource.get(), "refused packet");
  for (unsigned mode = 0; mode != 7; ++mode) {
    auto batch = original;
    auto descriptor_case = descriptor;
    if (mode == 0) batch.rows[0].row_ordinal = 1;
    if (mode == 1) batch.rows[0].cells[0].column_ordinal = 1;
    if (mode == 2) batch.rows[0].cells[0].state = wire::TypedResultValueState::sql_null;
    if (mode == 3) batch.batch_evidence_sha256.fill(0x55);
    if (mode == 4) batch.rows[0].cells[0].canonical_payload[0] = 0xff;
    if (mode == 5) batch.execution_uuid[6] = 0x40;
    if (mode == 6) descriptor_case.columns[0].descriptor_generation = 0;
    const auto owning = wire::EncodeTypedResultBatch(batch, descriptor_case, binding);
    const auto refused = wire::EncodeTypedResultBatchBuffer(batch, descriptor_case, binding, resource);
    Check(!refused.ok() && refused.encoded.empty() && refused.status == owning.status &&
          refused.diagnostic_code == owning.diagnostic_code && refused.detail == owning.detail,
          "resource route weakened original refusal or published partial packet");
    PhysicalCharge(manager, 0, bytes);
    { const auto retry = wire::EncodeTypedResultBatchBuffer(original, descriptor, binding, resource);
      Check(retry.ok(), "failed serialization poisoned exact-capacity retry"); }
    PhysicalCharge(manager, 0, bytes);
  }
  for (mem::u64 limit = 0; limit < bytes; ++limit) {
    const auto refused = wire::EncodeTypedResultBatchBuffer(original, descriptor, binding,
        *std::pmr::null_memory_resource(), limit);
    Check(refused.status == wire::TypedResultCodecStatus::resource_limit_exceeded && refused.encoded.empty(),
          "byte cap did not refuse before packet allocation");
  }
  bool exhausted = false;
  try { auto packet = wire::EncodeTypedResultBatchBuffer(original, descriptor, binding,
              *std::pmr::null_memory_resource()); }
  catch (const std::bad_alloc&) { exhausted = true; }
  Check(exhausted, "refusing packet resource was bypassed");
  PhysicalCharge(manager, 0, bytes);
}
void AllocationFailureCleanup() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto original = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))})});
  const auto binding = ExecuteBinding(original);
  const auto reference = wire::EncodeTypedResultBatch(original, descriptor, binding);
  const auto bytes = reference.encoded.size();
  for (unsigned mode = 0; mode != 3; ++mode) {
  auto batch = original;
  if (mode == 1) batch.rows[0].cells[0].canonical_payload[0] = 0xff;
  if (mode == 2) batch.batch_evidence_sha256.fill(0x55);
  bool complete = false;
  for (long point = 0; point != 2048 && !complete; ++point) {
    mem::MemoryManager manager(PacketPolicy(bytes));
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    auto grant = PacketGrant(manager, ledger, bytes);
    Check(grant.ok(), "fault grant setup"); if (!grant.ok()) return;
    {
    mem::ReservationBackedPmrMemoryResource resource(grant.resource.get(), "fault packet");
    std::optional<wire::TypedResultResourceBuffer> packet;
    buffer_fault::Arm(point);
    try { packet.emplace(wire::EncodeTypedResultBatchBuffer(batch, descriptor, binding, resource)); }
    catch (const std::bad_alloc&) {}
    const bool hit = buffer_fault::hit; buffer_fault::Off();
    if (hit) ++injected;
    else { complete = true; Check(packet && packet->ok() == (mode == 0), "uninjected serializer outcome changed"); }
    const bool published = packet && packet->ok();
    PhysicalCharge(manager, published ? bytes : 0, published ? 0 : bytes);
    buffer_fault::Arm(0); packet.reset();
    const bool release_allocated = buffer_fault::hit; buffer_fault::Off();
    Check(!release_allocated, "fault-path packet destruction allocated");
    PhysicalCharge(manager, 0, bytes);
    { const auto retry = wire::EncodeTypedResultBatchBuffer(original, descriptor, binding, resource);
      Check(retry.ok() && std::equal(retry.encoded.begin(), retry.encoded.end(),
          reference.encoded.begin(), reference.encoded.end()), "fault-path byte-exact retry failed"); }
    }
    grant.resource.reset();
    PhysicalCharge(manager, 0, 0);
    Check(ledger.Snapshot().current_bytes == 0, "fault-path hierarchy leak");
  }
  Check(complete, "allocation sweep did not reach an uninjected success");
  }
}

void ExactGrantContention() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))})});
  const auto binding = ExecuteBinding(batch);
  const auto reference = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  const auto bytes = reference.encoded.size();
  for (unsigned short_by : {0u, 1u}) {
    mem::MemoryManager manager(PacketPolicy(bytes));
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    auto grant = PacketGrant(manager, ledger, bytes - short_by);
    Check(grant.ok(), "real short grant setup"); if (!grant.ok()) return;
    mem::ReservationBackedPmrMemoryResource resource(grant.resource.get(), "bounded packet");
    std::optional<wire::TypedResultResourceBuffer> first;
    if (short_by == 0)
      first.emplace(wire::EncodeTypedResultBatchBuffer(batch, descriptor, binding, resource));
    bool refused = false;
    try { const auto second = wire::EncodeTypedResultBatchBuffer(batch, descriptor, binding, resource); }
    catch (const std::bad_alloc&) { refused = true; }
    Check(refused, "actual insufficient or occupied physical grant was bypassed");
    PhysicalCharge(manager, first ? bytes : 0, first ? 0 : bytes - short_by);
    if (first) {
      Check(std::equal(first->encoded.begin(), first->encoded.end(), reference.encoded.begin(),
            reference.encoded.end()), "competing serialization damaged retained packet");
      first.reset();
      const auto retry = wire::EncodeTypedResultBatchBuffer(batch, descriptor, binding, resource);
      Check(retry.ok(), "released exact packet capacity not reusable");
    }
  }
}

void ScalarBytesInResourcePackets() {
  auto uuid_column = TextColumn(2, "same", 2, 0x13);
  uuid_column.canonical_type_id = datatypes::CanonicalTypeId::uuid;
  uuid_column.codec_id = "datatype.uuid.v1";
  uuid_column.canonical_value_bytes = 16;
  const auto descriptor = Descriptor({TextColumn(0, "same", 0, 0x11),
      Int128Column(1, "same", 1, 0x12), uuid_column});
  mem::MemoryManager manager(PacketPolicy(65536));
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  auto grant = PacketGrant(manager, ledger, 65536);
  Check(grant.ok(), "scalar packet grant"); if (!grant.ok()) return;
  mem::ReservationBackedPmrMemoryResource resource(grant.resource.get(), "scalar packet");
  for (unsigned version = 1; version <= 7; ++version)
    for (unsigned text_kind = 0; text_kind != 3; ++text_kind)
      for (unsigned integer_kind = 0; integer_kind != 3; ++integer_kind) {
        auto uuid = std::vector<byte>(16, 0x11);
        uuid[6] = static_cast<byte>(version << 4); uuid[8] = 0x80;
        auto integer = std::vector<byte>(16, integer_kind == 1 ? 0 : 0xff);
        integer[15] = integer_kind == 1 ? 0x80 : 0x7f;
        auto text = text_kind == 0 ? Null(0, 0) :
            Present(0, 0, text_kind == 1 ? Bytes("") : std::vector<byte>{'a', 0, ';', '=', 'b'});
        auto number = integer_kind == 0 ? Null(1, 1) : Present(1, 1, integer);
        const auto batch = Batch(descriptor, {Row(0, {text, number, Present(2, 2, uuid)})});
        const auto binding = ExecuteBinding(batch);
        const auto reference = wire::EncodeTypedResultBatch(batch, descriptor, binding);
        {
          const auto packet = wire::EncodeTypedResultBatchBuffer(batch, descriptor, binding, resource);
          Check(packet.ok() && reference.ok() && std::equal(packet.encoded.begin(), packet.encoded.end(),
                reference.encoded.begin(), reference.encoded.end()),
                "resource encoding changed NULL/empty/delimiter/int128/user UUID data bytes");
          PhysicalCharge(manager, packet.encoded.size(), 65536 - packet.encoded.size());
          const std::vector<byte> bytes(packet.encoded.begin(), packet.encoded.end());
          const auto decoded = wire::DecodeTypedResultBatch(bytes, descriptor, binding);
          Check(decoded.ok() && decoded.batch.rows[0].cells[2].canonical_payload == uuid &&
                decoded.batch.rows[0].cells[0].state == text.state &&
                decoded.batch.rows[0].cells[0].canonical_payload == text.canonical_payload &&
                decoded.batch.rows[0].cells[1].canonical_payload == number.canonical_payload,
                "resource packet failed actual canonical decoder or lost duplicate-name cells");
        }
        PhysicalCharge(manager, 0, 65536);
      }
}

}
int main() {
  Check(ExistingTypedResultFixtureMain() == 0, "independent existing wire fixtures");
  ActualOwningScratch();
  ResourcePacketLifetime(); RefusalAndRetry(); AllocationFailureCleanup(); ExactGrantContention();
  ScalarBytesInResourcePackets();
  std::cout << "resource buffer checks=" << checks << " faults=" << injected
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
