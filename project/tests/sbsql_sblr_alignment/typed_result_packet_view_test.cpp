// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ExistingTypedResultFixtureMain
#include "typed_result_transport_codec_test.cpp"
#undef main
#include "wire/typed_result_packet_view.hpp"
#include <array>
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

void ExistingOwningDecodeCopies() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, std::vector<byte>(65537, 'x'))})});
  const auto binding = ExecuteBinding(batch);
  const auto encoded = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  for (unsigned kind = 0; kind != 2; ++kind) {
    buffer_fault::exact = kind == 0 ? encoded.encoded.size() : 65537;
    buffer_fault::count = 0; buffer_fault::watch = true;
    const auto decoded = wire::DecodeTypedResultBatch(encoded.encoded, descriptor, binding);
    buffer_fault::Off();
    Check(decoded.ok() && decoded.encoded == encoded.encoded &&
          decoded.batch.rows[0].cells[0].canonical_payload == batch.rows[0].cells[0].canonical_payload,
          "owning packet decoder changed bytes");
    Check(buffer_fault::count == 1,
          kind == 0 ? "owning decoder rebuilt full packet before returning its owned copy" :
                      "owning decoder cloned row payloads more than once");
  }
}
}
namespace {

void UnknownCarrierKinds() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))})});
  const auto encoded = wire::EncodeTypedResultBatch(batch, descriptor, ExecuteBinding(batch));
  for (unsigned kind = 3; kind != 256; ++kind) {
    auto binding = ExecuteBinding(batch);
    binding.kind = static_cast<wire::TypedResultCarrierKind>(kind);
    Check(!wire::EncodeTypedResultBatch(batch, descriptor, binding).ok(), "unknown carrier kind encoded");
    const auto view = wire::DecodeTypedResultPacketView(encoded.encoded.data(), encoded.encoded.size(), descriptor, binding);
    Check(!view.ok() && view.view.data() == nullptr && !view.has_cursor_state, "unknown carrier kind exposed view");
    Check(!wire::DecodeTypedResultBatch(encoded.encoded, descriptor, binding).ok(), "unknown carrier kind decoded");
  }
}
void BorrowedRowsAndNoCopies() {
  const auto descriptor = Descriptor({TextColumn(0, "same", 0, 0x11),
      TextColumn(1, "same", 1, 0x12), Int128Column(2, "wide", 0, 0x13)});
  std::vector<byte> min(16, 0); min[15] = 0x80;
  std::vector<byte> max(16, 0xff); max[15] = 0x7f;
  const auto batch = Batch(descriptor, {
      Row(0, {Present(0, 0, std::vector<byte>(65537, 'x')), Null(1, 1), Present(2, 0, min)}),
      Row(1, {Present(0, 0, {}), Present(1, 1, {'a', 0, ';', '='}), Present(2, 0, max)})});
  const auto binding = ExecuteBinding(batch);
  const auto encoded = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  Check(encoded.ok(), "multirow independent packet setup");
  buffer_fault::reject_size = 1024;
  bool escaped = false;
  wire::TypedResultPacketViewResult parsed;
  try { parsed = wire::DecodeTypedResultPacketView(encoded.encoded.data(), encoded.encoded.size(), descriptor, binding); }
  catch (...) { escaped = true; }
  buffer_fault::Off();
  Check(!escaped && parsed.ok(), "borrowed packet decode allocated proportional bytes");
  if (!parsed.ok()) return;
  Check(parsed.view.data() == encoded.encoded.data() && parsed.view.size() == encoded.encoded.size() &&
        parsed.view.header().row_count == 2 && parsed.view.header().column_count == 3 &&
        parsed.view.header().execution_uuid == batch.execution_uuid &&
        parsed.view.header().snapshot_uuid == batch.snapshot_uuid &&
        parsed.view.header().batch_evidence_sha256 == encoded.batch.batch_evidence_sha256,
        "borrowed packet metadata or exact original storage changed");
  auto rows = parsed.view.rows();
  wire::TypedResultPacketRowView row;
  Check(!rows.Next(nullptr), "null row destination consumed a row");
  std::size_t offset = 224;
  for (unsigned r = 0; r != 2; ++r) {
    buffer_fault::Arm(0);
    const bool next = rows.Next(&row), allocated = buffer_fault::hit;
    buffer_fault::Off();
    Check(next && !allocated && row.row_ordinal() == r && row.cell_count() == 3,
          "verified row iteration allocated or changed shape");
    offset += 16;
    auto cells = row.cells();
    wire::TypedResultPacketCellView cell;
    Check(!cells.Next(nullptr), "null cell destination consumed a cell");
    for (unsigned c = 0; c != 3; ++c) {
      buffer_fault::Arm(0);
      const bool found = cells.Next(&cell), hit = buffer_fault::hit;
      buffer_fault::Off();
      const auto& wanted = batch.rows[r].cells[c];
      Check(found && !hit && cell.column_ordinal == c && cell.name_occurrence == wanted.name_occurrence &&
            cell.state == wanted.state && cell.value.type_id == descriptor.columns[c].canonical_type_id &&
            cell.value.is_null == (wanted.state == wire::TypedResultValueState::sql_null) &&
            cell.value.payload_data == encoded.encoded.data() + offset + 20 + 32 &&
            cell.value.payload_bytes == wanted.canonical_payload.size() &&
            std::equal(wanted.canonical_payload.begin(), wanted.canonical_payload.end(), cell.value.payload_data),
            "borrowed cell lost exact ordinal/NULL/type/payload address");
      offset += 20 + 32 + wanted.canonical_payload.size();
    }
    Check(!cells.Next(&cell), "cell iterator exceeded verified count");
  }
  Check(!rows.Next(&row) && offset == encoded.encoded.size(), "row iterator exceeded verified packet");
  wire::TypedResultPacketView empty;
  Check(empty.data() == nullptr && empty.size() == 0 && !empty.rows().Next(&row), "default view exposed rows");
}
void BorrowedCorruptions() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))})});
  const auto binding = ExecuteBinding(batch);
  const auto encoded = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  auto Refused = [&](const byte* data, std::size_t size) {
    const auto parsed = wire::DecodeTypedResultPacketView(data, size, descriptor, binding);
    wire::TypedResultPacketRowView row;
    Check(!parsed.ok() && parsed.view.data() == nullptr && parsed.view.size() == 0 &&
          !parsed.view.rows().Next(&row) && !parsed.has_cursor_state && parsed.next_cursor_state.seen_batch_uuids.empty(),
          "malformed packet exposed provisional rows or state");
  };
  for (std::size_t size = 0; size < encoded.encoded.size(); ++size) Refused(encoded.encoded.data(), size);
  Refused(nullptr, 0); Refused(nullptr, encoded.encoded.size());
  Refused(encoded.encoded.data(), 16u * 1024u * 1024u + 1);
  for (unsigned offset : {0u, 8u, 10u, 12u, 16u, 80u, 96u, 104u, 136u, 140u, 144u,
                          184u, 224u, 228u, 232u, 240u, 244u, 248u, 252u, 253u, 254u,
                          256u, 260u, 268u, 272u, 280u, 292u}) {
    auto changed = encoded.encoded;
    changed[offset] ^= offset == 12 ? 0x80 : 1;
    Rehash(&changed, kBatchEvidenceOffset, kBatchDomain);
    const auto before = changed;
    Refused(changed.data(), changed.size());
    Check(changed == before, "borrowed refusal modified input");
  }
  auto trailing = encoded.encoded; trailing.push_back(0);
  platform::StoreLittle64(trailing.data() + 16, trailing.size());
  platform::StoreLittle64(trailing.data() + 144, trailing.size() - 224);
  Rehash(&trailing, kBatchEvidenceOffset, kBatchDomain);
  Refused(trailing.data(), trailing.size());
  const auto two_rows = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))}),
                                          Row(1, {Present(0, 0, Bytes("def"))})});
  const auto two_binding = ExecuteBinding(two_rows);
  auto bad_tail = wire::EncodeTypedResultBatch(two_rows, descriptor, two_binding).encoded;
  bad_tail.back() ^= 1;
  Rehash(&bad_tail, kBatchEvidenceOffset, kBatchDomain);
  const auto refused_tail = wire::DecodeTypedResultPacketView(bad_tail.data(), bad_tail.size(), descriptor, two_binding);
  wire::TypedResultPacketRowView first_row;
  Check(!refused_tail.ok() && refused_tail.view.data() == nullptr && !refused_tail.view.rows().Next(&first_row),
        "malformed second row exposed a valid first-row prefix");
}

}
namespace {

bool SameCursor(const wire::TypedResultCursorBatchState& a, const wire::TypedResultCursorBatchState& b) {
  return a.initialized == b.initialized && a.terminal == b.terminal &&
      a.cursor_uuid == b.cursor_uuid && a.execution_uuid == b.execution_uuid &&
      a.result_set_uuid == b.result_set_uuid && a.snapshot_uuid == b.snapshot_uuid &&
      a.cursor_stream_descriptor_uuid == b.cursor_stream_descriptor_uuid &&
      a.cursor_stream_descriptor_version == b.cursor_stream_descriptor_version &&
      a.cursor_stream_descriptor_generation == b.cursor_stream_descriptor_generation &&
      a.row_descriptor_uuid == b.row_descriptor_uuid &&
      a.row_descriptor_generation == b.row_descriptor_generation &&
      a.descriptor_evidence_sha256 == b.descriptor_evidence_sha256 &&
      a.next_batch_ordinal == b.next_batch_ordinal && a.seen_batch_uuids == b.seen_batch_uuids;
}
void CursorStagingAndAllocationFailures() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto first = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))})}, 0, false, true);
  const auto second = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("def"))})}, 1, true, true);
  const auto first_binding = FetchBinding(first), second_binding = FetchBinding(second);
  const auto first_bytes = wire::EncodeTypedResultBatch(first, descriptor, first_binding).encoded;
  const auto second_bytes = wire::EncodeTypedResultBatch(second, descriptor, second_binding).encoded;
  wire::TypedResultCursorBatchState empty;
  const auto initial = wire::DecodeTypedResultPacketView(first_bytes.data(), first_bytes.size(), descriptor, first_binding, &empty);
  Check(initial.ok() && initial.has_cursor_state && !empty.initialized && empty.seen_batch_uuids.empty() &&
        initial.next_cursor_state.initialized && initial.next_cursor_state.next_batch_ordinal == 1 &&
        !initial.next_cursor_state.terminal, "borrowed cursor validation committed or lost provisional state");
  if (!initial.ok()) return;
  const auto prior = initial.next_cursor_state;
  const auto terminal = wire::DecodeTypedResultPacketView(second_bytes.data(), second_bytes.size(), descriptor, second_binding, &prior);
  Check(terminal.ok() && terminal.has_cursor_state && terminal.next_cursor_state.terminal &&
        terminal.next_cursor_state.next_batch_ordinal == 2 && terminal.next_cursor_state.seen_batch_uuids.size() == 2,
        "borrowed terminal cursor replacement missing");
  if (!terminal.ok()) return;
  const auto after_terminal = wire::DecodeTypedResultPacketView(second_bytes.data(), second_bytes.size(), descriptor,
      second_binding, &terminal.next_cursor_state);
  Check(!after_terminal.ok() && !after_terminal.has_cursor_state && after_terminal.view.data() == nullptr,
        "post-terminal view publication");
  Check(!wire::DecodeTypedResultPacketView(first_bytes.data(), first_bytes.size(), descriptor, first_binding).ok(),
        "fetch view bypassed required cursor state");
  for (unsigned mode = 0; mode != 2; ++mode) {
    bool complete = false;
    for (long point = 0; point != 1024 && !complete; ++point) {
      auto state = prior;
      std::optional<wire::TypedResultPacketViewResult> view;
      std::optional<wire::TypedResultBatchCodecResult> owning;
      buffer_fault::Arm(point);
      try {
        if (mode == 0) view.emplace(wire::DecodeTypedResultPacketView(
            second_bytes.data(), second_bytes.size(), descriptor, second_binding, &state));
        else owning.emplace(wire::DecodeTypedResultBatch(second_bytes, descriptor, second_binding, &state));
      } catch (const std::bad_alloc&) {}
      const bool hit = buffer_fault::hit; buffer_fault::Off();
      const bool published = mode == 0 ? (view && view->ok()) : (owning && owning->ok());
      if (hit) ++injected;
      else { complete = true; Check(published, "uninjected packet decode failed"); }
      Check(SameCursor(state, mode == 1 && published ? terminal.next_cursor_state : prior),
            "allocation/refusal advanced caller state before owning publication");
      if (mode == 0 && published)
        Check(SameCursor(view->next_cursor_state, terminal.next_cursor_state) &&
              view->view.data() == second_bytes.data(), "view lost staged cursor or borrowed storage");
      if (mode == 0 && view && !view->ok())
        Check(!view->has_cursor_state && view->view.data() == nullptr, "refused view published partial state");
      if (mode == 1 && owning && !owning->ok())
        Check(owning->encoded.empty() && owning->batch.rows.empty(), "refused owning decode published partial packet");
      buffer_fault::Arm(0); view.reset(); owning.reset();
      const bool release_allocated = buffer_fault::hit; buffer_fault::Off();
      Check(!release_allocated, "packet result teardown allocated under pressure");
      if (!published) {
        const auto retry = wire::DecodeTypedResultBatch(second_bytes, descriptor, second_binding, &state);
        Check(retry.ok() && SameCursor(state, terminal.next_cursor_state), "faulted packet could not retry on retained state");
      }
    }
    Check(complete, "packet allocation fault sweep incomplete");
  }
}

}
int main() {
  Check(ExistingTypedResultFixtureMain() == 0, "independent existing packet fixtures");
  ExistingOwningDecodeCopies();
  UnknownCarrierKinds(); BorrowedRowsAndNoCopies(); BorrowedCorruptions();
  CursorStagingAndAllocationFailures();
  std::cout << "packet view checks=" << checks << " faults=" << injected
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
