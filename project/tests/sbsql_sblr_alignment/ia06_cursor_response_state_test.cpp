// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "parsers/sbsql_worker/wire/cursor_response_state.hpp"
#include <iostream>
#include <stdexcept>

namespace s = scratchbird::engine::sblr;
using scratchbird::parser::sbsql::CursorResponseState;
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  try {
    // Component inputs only: these encoded packets test parser response
    // correlation, not engine-issued identity or actual row production.
    s::SblrCursorHandleV1 h;
    h.cursor[0] = 1; h.plan[0] = 2; h.row_shape[0] = 3; h.session[0] = 4;
    h.cursor_generation = 10; h.plan_generation = 20; h.row_shape_generation = 30;
    h.position_generation = 1; h.availability_generation = 40;
    h.state = h.mode = h.hold = 1; h.fetch_size = 64; h.cursor_evidence[0] = 5;
    const auto open = s::EncodeSblrCursorHandleV1(h);
    Require(!open.empty(), "fixture open encoding");
    CursorResponseState state;
    auto reset = [&] { Require(state.AdmitOpen(open.data(), open.size()), "open"); };
    s::SblrCursorFetchResultV1 f;
    f.cursor = h.cursor; f.cursor_generation = h.cursor_generation;
    f.prior_position_generation = 1; f.resulting_position_generation = 2;
    f.availability_generation = 50; f.returned_rows = 2;
    f.row_batch_sha[0] = 6; f.refreshed_cursor_evidence[0] = 7;
    const auto fetch = s::EncodeSblrCursorFetchResultV1(f);
    Require(!fetch.empty(), "fixture fetch encoding");
    reset();
    Require(state.AdmitFetch(fetch.data(), fetch.size(), 50, 64), "first fetch");
    Require(state.PositionGeneration() == 2 &&
            state.CursorEvidence() == f.refreshed_cursor_evidence, "copy CFRS authority");
    Require(state.OpenedHandle().position_generation == 1 &&
            state.OpenedHandle().cursor_evidence == h.cursor_evidence,
            "original CUHD must not be fabricated or modified");
    Require(!state.AdmitFetch(fetch.data(), fetch.size(), 50, 64) &&
            !state.HasHandle(), "old-position result must invalidate local state");

    for (unsigned fault = 0; fault < 7; ++fault) {
      reset();
      auto bad = f;
      if (fault == 0) bad.cursor[1] ^= 1;
      if (fault == 1) ++bad.cursor_generation;
      if (fault == 2) { ++bad.prior_position_generation; ++bad.resulting_position_generation; }
      if (fault == 3) ++bad.availability_generation;
      if (fault == 4) bad.returned_rows = 65;
      auto bytes = s::EncodeSblrCursorFetchResultV1(bad);
      Require(!bytes.empty(), "valid-shaped mismatched fetch");
      if (fault == 5) bytes.back() ^= 1; // Corrupt result evidence hash.
      if (fault == 6) bytes.pop_back();
      Require(!state.AdmitFetch(bytes.data(), bytes.size(), 50, 64) &&
              !state.HasHandle(), "mismatched fetch must invalidate");
    }
    reset();
    Require(state.AdmitFetch(fetch.data(), fetch.size(), 50, 64), "fetch after reopen");
    auto eof = f;
    eof.prior_position_generation = 2; eof.resulting_position_generation = 3;
    eof.returned_rows = 0; eof.eof = 1; eof.refreshed_cursor_evidence[0] = 8;
    const auto eof_bytes = s::EncodeSblrCursorFetchResultV1(eof);
    Require(state.AdmitFetch(eof_bytes.data(), eof_bytes.size(), 50, 64) &&
            state.PositionGeneration() == 3 &&
            state.CursorEvidence() == eof.refreshed_cursor_evidence, "EOF advances authority");

    s::SblrCursorCloseResultV1 c;
    c.cursor = h.cursor; c.cursor_generation = h.cursor_generation;
    c.final_position_generation = 3; c.availability_generation = 60;
    c.final_state = 2; c.close_reason = 1; c.cleanup_evidence[0] = 9;
    const auto close = s::EncodeSblrCursorCloseResultV1(c);
    Require(!close.empty(), "fixture close encoding");
    Require(state.AdmitClose(close.data(), close.size(), 60, 1) &&
            !state.HasHandle(), "successful close retires authority");
    Require(!state.AdmitClose(close.data(), close.size(), 60, 1), "closed handle absent");
    reset();
    Require(state.PositionGeneration() == 1 && state.CursorEvidence() == h.cursor_evidence,
            "reopen cannot retain previous fetch authority");
    for (unsigned fault = 0; fault < 7; ++fault) {
      reset();
      auto bad = c; bad.final_position_generation = 1;
      if (fault == 0) bad.cursor[1] ^= 1;
      if (fault == 1) ++bad.cursor_generation;
      if (fault == 2) ++bad.final_position_generation;
      if (fault == 3) ++bad.availability_generation;
      if (fault == 4) bad.close_reason = 2;
      auto bytes = s::EncodeSblrCursorCloseResultV1(bad);
      Require(!bytes.empty(), "valid-shaped mismatched close");
      if (fault == 5) bytes.back() ^= 1;
      if (fault == 6) bytes.pop_back();
      Require(!state.AdmitClose(bytes.data(), bytes.size(), 60, 1) &&
              !state.HasHandle(), "mismatched close must invalidate");
    }
    reset();
    Require(!state.AdmitOpen(open.data(), open.size() - 1) && !state.HasHandle(),
            "malformed open invalidates");
    std::cout << "PASS binary response correlation and cursor retirement\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
