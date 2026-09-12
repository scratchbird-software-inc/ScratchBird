// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/sblr/sblr_cursor_open_runtime.hpp"
#include "engine/sblr/sblr_cursor_fetch_runtime.hpp"
#include "engine/sblr/sblr_cursor_close_runtime.hpp"

#include <limits>
#include <optional>

namespace scratchbird::parser::sbsql {

// Parser-owned copies of validated engine responses, not engine identity
// authority. Never re-encode a modified CUHD or invent a refreshed receipt.
// The original CUHD stays unchanged; only CFRS-issued fields feed later operands.
class CursorResponseState {
 public:
  using Handle = scratchbird::engine::sblr::SblrCursorHandleV1;
  using FetchResult = scratchbird::engine::sblr::SblrCursorFetchResultV1;
  using CloseResult = scratchbird::engine::sblr::SblrCursorCloseResultV1;

  bool HasHandle() const { return opened_.has_value(); }
  const Handle& OpenedHandle() const { return opened_.value(); }
  std::uint64_t PositionGeneration() const {
    return fetched_ ? fetched_->resulting_position_generation
                    : OpenedHandle().position_generation;
  }
  const auto& CursorEvidence() const {
    return fetched_ ? fetched_->refreshed_cursor_evidence
                    : OpenedHandle().cursor_evidence;
  }

  bool AdmitOpen(const std::uint8_t* data, std::size_t size) {
    Handle handle;
    std::string detail;
    Reset();
    if (!scratchbird::engine::sblr::DecodeSblrCursorHandleV1(
            data, size, &handle, &detail)) return false;
    opened_ = handle;
    return true;
  }

  // Call only for a successful authenticated server response to the outstanding
  // request. A corrupt/mismatched success leaves position unknowable: invalidate
  // locally instead of replaying potentially consumed authority.
  bool AdmitFetch(const std::uint8_t* data, std::size_t size,
                  std::uint64_t executor_generation, std::uint32_t maximum_rows) {
    FetchResult result;
    std::string detail;
    if (!opened_ ||
        !scratchbird::engine::sblr::DecodeSblrCursorFetchResultV1(
            data, size, &result, &detail) ||
        result.cursor != opened_->cursor ||
        result.cursor_generation != opened_->cursor_generation ||
        result.prior_position_generation != PositionGeneration() ||
        PositionGeneration() == std::numeric_limits<std::uint64_t>::max() ||
        result.resulting_position_generation != PositionGeneration() + 1 ||
        result.availability_generation != executor_generation ||
        result.returned_rows > maximum_rows) {
      Reset();
      return false;
    }
    fetched_ = result;
    return true;
  }

  bool AdmitClose(const std::uint8_t* data, std::size_t size,
                  std::uint64_t executor_generation, std::uint8_t reason) {
    CloseResult result;
    std::string detail;
    const bool accepted = opened_ &&
        scratchbird::engine::sblr::DecodeSblrCursorCloseResultV1(
            data, size, &result, &detail) &&
        result.cursor == opened_->cursor &&
        result.cursor_generation == opened_->cursor_generation &&
        result.final_position_generation == PositionGeneration() &&
        result.availability_generation == executor_generation &&
        result.final_state == 2 && result.close_reason == reason;
    // Success retires the handle. Malformed success cannot safely be retried.
    Reset();
    return accepted;
  }

 private:
  void Reset() {
    opened_.reset();
    fetched_.reset();
  }
  std::optional<Handle> opened_;
  std::optional<FetchResult> fetched_;
};
}  // namespace scratchbird::parser::sbsql
