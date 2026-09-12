// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "typed_result_transport_codec.hpp"
#include "datatype_binary_view.hpp"

namespace scratchbird::wire {

// Immutable borrowed packet views. The caller retains the encoded storage and
// must not modify it while any view or iterator exists. A successful decode
// validates the entire packet before exposing a row; these objects supply no
// session, descriptor-catalog, cursor, grant or MGA authority.
struct TypedResultPacketHeader {
  TypedResultUuid execution_uuid{}, result_set_uuid{}, batch_uuid{};
  u64 batch_ordinal = 0;
  bool end_of_rowset = false, cursor_bound = false;
  TypedResultUuid row_descriptor_uuid{};
  u64 row_descriptor_generation = 0;
  TypedResultEvidenceHash descriptor_evidence_sha256{};
  TypedResultUuid snapshot_uuid{}, cursor_uuid{};
  TypedResultEvidenceHash batch_evidence_sha256{};
  u32 row_count = 0, column_count = 0;
};

struct TypedResultPacketCellView {
  u32 column_ordinal = 0, name_occurrence = 0;
  TypedResultValueState state = TypedResultValueState::value_present;
  core::datatypes::DatatypeBinaryValueView value;
};

class TypedResultPacketRowView;
class TypedResultPacketCellCursor {
 public:
  TypedResultPacketCellCursor() = default;
  bool Next(TypedResultPacketCellView* cell) noexcept;
 private:
  TypedResultPacketCellCursor(const byte* next, u32 count) : next_(next), remaining_(count) {}
  const byte* next_ = nullptr;
  u32 remaining_ = 0;
  friend class TypedResultPacketRowView;
};

class TypedResultPacketRowCursor;
class TypedResultPacketRowView {
 public:
  u64 row_ordinal() const noexcept { return ordinal_; }
  u32 cell_count() const noexcept { return cell_count_; }
  TypedResultPacketCellCursor cells() const noexcept { return {cells_, cell_count_}; }
 private:
  u64 ordinal_ = 0;
  u32 cell_count_ = 0;
  const byte* cells_ = nullptr;
  friend class TypedResultPacketRowCursor;
};

class TypedResultPacketView;
class TypedResultPacketRowCursor {
 public:
  TypedResultPacketRowCursor() = default;
  bool Next(TypedResultPacketRowView* row) noexcept;
 private:
  TypedResultPacketRowCursor(const byte* next, u32 count) : next_(next), remaining_(count) {}
  const byte* next_ = nullptr;
  u32 remaining_ = 0;
  friend class TypedResultPacketView;
};

struct TypedResultPacketViewResult;
class TypedResultPacketView {
 public:
  const TypedResultPacketHeader& header() const noexcept { return header_; }
  const byte* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  TypedResultPacketRowCursor rows() const noexcept {
    return {data_ == nullptr ? nullptr : data_ + kTypedResultBatchHeaderBytes, header_.row_count};
  }
 private:
  const byte* data_ = nullptr;
  std::size_t size_ = 0;
  TypedResultPacketHeader header_;
  friend TypedResultPacketViewResult DecodeTypedResultPacketView(
      const byte*, std::size_t, const TypedResultRowDescriptor&,
      const TypedResultCarrierBinding&, const TypedResultCursorBatchState*);
};

struct TypedResultPacketViewResult {
  TypedResultCodecStatus status = TypedResultCodecStatus::invalid_argument;
  std::string diagnostic_code, detail;
  TypedResultPacketView view;
  bool has_cursor_state = false;
  TypedResultCursorBatchState next_cursor_state;
  bool ok() const { return status == TypedResultCodecStatus::ok; }
};

// Exact canonical validation without packet/row/cell-payload materialization.
// Descriptor validation and cursor history still own their control metadata.
// cursor_state is read-only. On success the returned next_cursor_state is a
// provisional replacement, committed only after the caller's publication work.
// The owning decoder delays that commit until all its copies exist.
TypedResultPacketViewResult DecodeTypedResultPacketView(
    const byte* encoded, std::size_t encoded_bytes,
    const TypedResultRowDescriptor& expected_descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    const TypedResultCursorBatchState* cursor_state = nullptr);

}  // namespace scratchbird::wire
