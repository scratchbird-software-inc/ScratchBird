// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "direct_binary_result_frame.hpp"
#include "streaming_cursor_manager.hpp"
#include "vectorized_result_batch.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace wire = scratchbird::wire;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-060/061/062 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values,
              const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

scratchbird::core::platform::u64 EvidenceU64(
    const std::vector<std::string>& values,
    const std::string& key) {
  const std::string prefix = key + "=";
  for (const auto& value : values) {
    if (value.rfind(prefix, 0) != 0) {
      continue;
    }
    scratchbird::core::platform::u64 parsed = 0;
    for (std::size_t index = prefix.size(); index < value.size(); ++index) {
      const char ch = value[index];
      Require(ch >= '0' && ch <= '9', "non-numeric evidence value");
      parsed = (parsed * 10u) +
               static_cast<scratchbird::core::platform::u64>(ch - '0');
    }
    return parsed;
  }
  Fail("required numeric evidence missing");
}

wire::StreamingCursorState CursorState() {
  wire::StreamingCursorState state;
  state.cursor_id = "019b8000-0000-7000-8000-000000000060";
  state.plan_result_contract_hash = "fnv1a64:orh060-result-contract";
  state.catalog_epoch = 59;
  state.descriptor_epoch = 60;
  state.transaction_snapshot_class = "mga_statement_snapshot";
  state.transaction_uuid = "019b8000-0000-7000-8000-000000000061";
  state.local_transaction_id = 601;
  state.snapshot_visible_through_local_transaction_id = 599;
  state.security_epoch = 61;
  state.redaction_epoch = 62;
  state.route_kind = "embedded";
  state.frame_sequence = 7;
  state.expiry_deadline_unix_millis = 10'000;
  state.client_credit.frame_credit = 2;
  state.client_credit.row_credit = 4;
  state.client_credit.byte_credit = 4096;
  state.client_credit.backpressure_active = false;
  return state;
}

void CursorManagerTracksStateAndCredit() {
  wire::StreamingCursorManager manager;
  const auto opened =
      manager.OpenCursor({.state = CursorState(), .now_unix_millis = 100});
  Require(opened.ok(), "cursor open failed");
  const auto duplicate =
      manager.OpenCursor({.state = CursorState(), .now_unix_millis = 100});
  Require(!duplicate.ok(), "duplicate cursor open was admitted");
  Require(duplicate.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.DUPLICATE_CURSOR_ID",
          "duplicate cursor diagnostic mismatch");
  Require(opened.state.cursor_id == CursorState().cursor_id,
          "cursor id not retained");
  Require(opened.state.plan_result_contract_hash ==
              "fnv1a64:orh060-result-contract",
          "result contract hash not retained");
  Require(opened.state.catalog_epoch == 59,
          "catalog epoch not retained");
  Require(opened.state.descriptor_epoch == 60,
          "descriptor epoch not retained");
  Require(opened.state.transaction_snapshot_class ==
              "mga_statement_snapshot",
          "snapshot class not retained");
  Require(opened.state.transaction_uuid ==
              "019b8000-0000-7000-8000-000000000061",
          "transaction uuid not retained");
  Require(opened.state.local_transaction_id == 601,
          "local transaction id not retained");
  Require(opened.state.snapshot_visible_through_local_transaction_id == 599,
          "snapshot visible-through id not retained");
  Require(opened.state.security_epoch == 61,
          "security epoch not retained");
  Require(opened.state.redaction_epoch == 62,
          "redaction epoch not retained");
  Require(opened.state.route_kind == "embedded", "route kind not retained");
  Require(opened.state.frame_sequence == 7,
          "frame sequence not retained");
  Require(!opened.state.mga_visibility_or_finality_authority,
          "cursor claimed MGA finality authority");
  Require(opened.state.advisory_metadata_only,
          "cursor did not remain advisory metadata");
  Require(Contains(opened.evidence,
                   "cursor_mga_visibility_or_finality_authority=false"),
          "MGA authority evidence missing");

  const auto binding = wire::StreamingCursorBindingFromState(opened.state);
  const auto admitted =
      manager.ValidateFetch({.expected = binding, .now_unix_millis = 101});
  Require(admitted.ok(), "fetch was not admitted with credit");

  auto wrong_txn_uuid = binding;
  wrong_txn_uuid.transaction_uuid =
      "019b8000-0000-7000-8000-000000000062";
  const auto txn_uuid_refused = manager.ValidateFetch(
      {.expected = wrong_txn_uuid, .now_unix_millis = 101});
  Require(txn_uuid_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.TRANSACTION_UUID_MISMATCH",
          "manager transaction uuid mismatch diagnostic mismatch");

  auto wrong_local_tx = binding;
  wrong_local_tx.local_transaction_id = 602;
  const auto local_tx_refused = manager.ValidateFetch(
      {.expected = wrong_local_tx, .now_unix_millis = 101});
  Require(local_tx_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.LOCAL_TRANSACTION_ID_MISMATCH",
          "manager local transaction mismatch diagnostic mismatch");

  auto wrong_snapshot_visible = binding;
  wrong_snapshot_visible.snapshot_visible_through_local_transaction_id = 598;
  const auto snapshot_visible_refused = manager.ValidateFetch(
      {.expected = wrong_snapshot_visible, .now_unix_millis = 101});
  Require(snapshot_visible_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.SNAPSHOT_VISIBLE_THROUGH_MISMATCH",
          "manager snapshot visible-through mismatch diagnostic mismatch");

  auto wrong_expiry = binding;
  wrong_expiry.expiry_deadline_unix_millis = 10'001;
  const auto expiry_refused = manager.ValidateFetch(
      {.expected = wrong_expiry, .now_unix_millis = 101});
  Require(expiry_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.EXPIRY_MISMATCH",
          "manager expiry mismatch diagnostic mismatch");

  auto delivered = manager.RecordFrameDelivery(
      {.expected = binding,
       .row_count = 2,
       .byte_count = 300,
       .now_unix_millis = 102});
  Require(delivered.ok(), "frame delivery rejected");
  Require(delivered.state.frame_sequence == 8,
          "frame sequence did not advance");
  Require(delivered.state.client_credit.frame_credit == 1,
          "frame credit not consumed");
  Require(delivered.state.client_credit.row_credit == 2,
          "row credit not consumed");
  Require(delivered.state.client_credit.byte_credit == 3796,
          "byte credit not consumed");

  auto backpressured = manager.GrantCredit(
      CursorState().cursor_id,
      {.frame_credit = 0, .row_credit = 8, .byte_credit = 4096});
  Require(backpressured.ok(), "credit update failed");
  auto blocked_binding = binding;
  blocked_binding.frame_sequence = 8;
  const auto blocked = manager.ValidateFetch(
      {.expected = blocked_binding, .now_unix_millis = 103});
  Require(!blocked.ok(), "zero frame credit did not block fetch");
  Require(blocked.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.BACKPRESSURE",
          "backpressure diagnostic mismatch");

  manager.GrantCredit(CursorState().cursor_id,
                      {.frame_credit = 1, .row_credit = 2, .byte_credit = 512});
  const auto cancelled = manager.CancelCursor(CursorState().cursor_id);
  Require(cancelled.ok(), "cancel request failed");
  const auto refused = manager.ValidateFetch(
      {.expected = blocked_binding, .now_unix_millis = 104});
  Require(!refused.ok(), "cancelled cursor admitted fetch");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.CANCELLED",
          "cancel diagnostic mismatch");
}

void ContinuationTokensValidateBindingsAndTamper() {
  const auto binding = wire::StreamingCursorBindingFromState(CursorState());
  const wire::ContinuationTokenSecret secret{.key_id = "orh061.key.with.dot",
                                             .secret_material = "test-secret"};
  const auto issued = wire::IssueContinuationToken(binding, secret);
  Require(issued.ok(), "token issue failed");
  Require(!issued.token.empty(), "issued token was empty");
  const auto admitted =
      wire::ValidateContinuationToken(issued.token, binding, secret, 100);
  Require(admitted.ok(), "issued token was not admitted");
  Require(Contains(admitted.evidence,
                   "continuation_token_signature_algorithm=HMAC-SHA256"),
          "HMAC-SHA256 token evidence missing");
  Require(Contains(admitted.evidence,
                   "continuation_token_key_id_encoding=hex"),
          "hex key id encoding evidence missing");

  auto tampered_token = issued.token;
  tampered_token.back() = tampered_token.back() == '0' ? '1' : '0';
  const auto tampered =
      wire::ValidateContinuationToken(tampered_token, binding, secret, 100);
  Require(!tampered.ok(), "tampered token was admitted");
  Require(tampered.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.TAMPERED",
          "tampered token diagnostic mismatch");

  auto wrong_route = binding;
  wrong_route.route_kind = "ipc";
  const auto route_refused =
      wire::ValidateContinuationToken(issued.token, wrong_route, secret, 100);
  Require(route_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.ROUTE_MISMATCH",
          "route mismatch diagnostic mismatch");

  auto wrong_epoch = binding;
  wrong_epoch.security_epoch = 6001;
  const auto epoch_refused =
      wire::ValidateContinuationToken(issued.token, wrong_epoch, secret, 100);
  Require(epoch_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.SECURITY_EPOCH_MISMATCH",
          "security epoch mismatch diagnostic mismatch");

  auto wrong_catalog = binding;
  wrong_catalog.catalog_epoch = 5901;
  const auto catalog_refused =
      wire::ValidateContinuationToken(issued.token, wrong_catalog, secret, 100);
  Require(catalog_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.CATALOG_EPOCH_MISMATCH",
          "catalog epoch mismatch diagnostic mismatch");

  auto wrong_contract = binding;
  wrong_contract.plan_result_contract_hash = "fnv1a64:other-contract";
  const auto contract_refused = wire::ValidateContinuationToken(
      issued.token, wrong_contract, secret, 100);
  Require(contract_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.CONTRACT_MISMATCH",
          "contract mismatch diagnostic mismatch");

  auto wrong_token_txn_uuid = binding;
  wrong_token_txn_uuid.transaction_uuid =
      "019b8000-0000-7000-8000-000000000062";
  const auto token_txn_uuid_refused = wire::ValidateContinuationToken(
      issued.token, wrong_token_txn_uuid, secret, 100);
  Require(token_txn_uuid_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.TRANSACTION_UUID_MISMATCH",
          "token transaction uuid mismatch diagnostic mismatch");

  auto wrong_token_local_tx = binding;
  wrong_token_local_tx.local_transaction_id = 602;
  const auto token_local_tx_refused = wire::ValidateContinuationToken(
      issued.token, wrong_token_local_tx, secret, 100);
  Require(token_local_tx_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.LOCAL_TRANSACTION_ID_MISMATCH",
          "token local transaction mismatch diagnostic mismatch");

  auto wrong_token_snapshot_visible = binding;
  wrong_token_snapshot_visible.snapshot_visible_through_local_transaction_id =
      598;
  const auto token_snapshot_visible_refused =
      wire::ValidateContinuationToken(
          issued.token, wrong_token_snapshot_visible, secret, 100);
  Require(token_snapshot_visible_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.SNAPSHOT_VISIBLE_THROUGH_MISMATCH",
          "token snapshot visible-through mismatch diagnostic mismatch");

  auto wrong_token_expiry = binding;
  wrong_token_expiry.expiry_deadline_unix_millis = 10'001;
  const auto token_expiry_refused = wire::ValidateContinuationToken(
      issued.token, wrong_token_expiry, secret, 100);
  Require(token_expiry_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.EXPIRY_MISMATCH",
          "token expiry mismatch diagnostic mismatch");

  auto wrong_sequence = binding;
  wrong_sequence.frame_sequence = 8;
  const auto sequence_refused = wire::ValidateContinuationToken(
      issued.token, wrong_sequence, secret, 100);
  Require(sequence_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.SEQUENCE_MISMATCH",
          "sequence mismatch diagnostic mismatch");
}

exec::VectorizedResultBatch Batch(const std::vector<std::string>& values) {
  std::vector<scratchbird::core::platform::u64> offsets;
  std::vector<std::uint8_t> data;
  offsets.push_back(0);
  for (const auto& value : values) {
    data.insert(data.end(), value.begin(), value.end());
    offsets.push_back(static_cast<scratchbird::core::platform::u64>(
        data.size()));
  }
  exec::VectorizedResultBatchBuilder builder(values.size());
  builder.AddColumn(exec::MakeVariableWidthResultBatchColumn(
      "payload",
      values.size(),
      offsets,
      data,
      exec::MakeResultBatchValidityBitmap(values.size())));
  auto finalized = builder.Finalize();
  Require(finalized.ok(), "batch finalization failed");
  return finalized.batch;
}

scratchbird::core::platform::u64 FullFrameBytes(
    const std::vector<std::string>& values) {
  const auto built = wire::BuildDirectBinaryResultFrame(Batch(values));
  Require(built.ok(), "full frame build failed");
  return built.frame.bytes.size();
}

void FrameWindowUsesActualBytesCancellationAndBackpressure() {
  const std::vector<std::string> rows = {
      "a",
      "variable-row-two-is-longer",
      "variable-row-three-is-longer-than-two",
  };
  const auto two_row_bytes = FullFrameBytes({rows[0], rows[1]});
  const auto three_row_bytes = FullFrameBytes(rows);
  Require(three_row_bytes > two_row_bytes,
          "test fixture did not grow frame bytes");

  wire::DirectBinaryResultFrameWindowPolicy policy;
  policy.start_row = 0;
  policy.requested_rows = 3;
  policy.max_rows = 3;
  policy.max_frame_bytes = two_row_bytes;
  policy.client_credit_rows = 3;
  policy.client_credit_bytes = three_row_bytes;
  policy.frame_sequence = 9;
  const auto window = wire::BuildDirectBinaryResultFrameWindow(Batch(rows), policy);
  Require(window.ok(), "window frame build failed");
  Require(window.row_count == 2,
          "window did not stop at actual byte row boundary");
  Require(window.actual_frame_bytes == two_row_bytes,
          "window did not report actual serialized frame bytes");
  Require(window.next_start_row == 2,
          "window next row mismatch");
  Require(window.continuation_required,
          "window did not require continuation");
  Require(window.ordering_preserved,
          "ordered output was not preserved");
  Require(Contains(window.evidence,
                   "direct_binary_frame.actual_byte_accounting=true"),
          "actual byte accounting evidence missing");
  Require(EvidenceU64(window.evidence,
                      "direct_binary_frame.actual_byte_probe_count") <
              policy.requested_rows,
          "actual byte frame window used linear prefix probing");

  auto no_credit = policy;
  no_credit.client_credit_bytes = 0;
  const auto backpressured =
      wire::BuildDirectBinaryResultFrameWindow(Batch(rows), no_credit);
  Require(!backpressured.ok(), "zero byte credit was admitted");
  Require(backpressured.diagnostic.diagnostic_code ==
              "SB_DIRECT_BINARY_RESULT_FRAME.BACKPRESSURE",
          "frame backpressure diagnostic mismatch");

  auto cancelled_policy = policy;
  cancelled_policy.cancellation_requested = true;
  const auto cancelled =
      wire::BuildDirectBinaryResultFrameWindow(Batch(rows), cancelled_policy);
  Require(cancelled.ok(), "cancelled frame window should return handled state");
  Require(cancelled.cancelled, "cancelled frame window not marked cancelled");
  Require(cancelled.diagnostic.diagnostic_code ==
              "SB_DIRECT_BINARY_RESULT_FRAME.CANCELLED",
          "frame cancellation diagnostic mismatch");
}

}  // namespace

int main() {
  CursorManagerTracksStateAndCredit();
  ContinuationTokensValidateBindingsAndTamper();
  FrameWindowUsesActualBytesCancellationAndBackpressure();
  return EXIT_SUCCESS;
}
