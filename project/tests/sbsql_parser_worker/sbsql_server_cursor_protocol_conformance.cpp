// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hash_digest.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scratchbird::server::ServerDiagnostic;
using scratchbird::server::ServerCursorRecord;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

constexpr std::uint32_t kFetchFlagScroll = 1;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  for (const ServerDiagnostic& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void PutUuid(std::vector<std::uint8_t>* out,
             const std::array<std::uint8_t, 16>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const ServerCursorRecord& cursor,
                       std::uint64_t max_rows,
                       std::uint64_t max_bytes = 0,
                       std::uint32_t fetch_flags = 0) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor.cursor_uuid, max_rows, max_bytes, fetch_flags);
  frame.payload.insert(frame.payload.end(), cursor.stream_descriptor_uuid.begin(),
                       cursor.stream_descriptor_uuid.end());
  PutU16(&frame.payload, cursor.stream_descriptor_version);
  PutU64(&frame.payload, cursor.stream_descriptor_generation);
  return frame;
}

sbps::Frame CloseFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeCloseCursorPayloadForTest(session_uuid, cursor_uuid);
  return frame;
}

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_server_cursor_protocol_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-0000000000b2";
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

std::array<std::uint8_t, 16> OpenCursor(ServerSessionRegistry* registry,
                                        const std::array<std::uint8_t, 16>& session_uuid,
                                        std::uint64_t rows) {
  ServerCursorRecord cursor;
  cursor.cursor_uuid = sbps::MakeUuidV7Bytes();
  cursor.request_uuid = sbps::MakeUuidV7Bytes();
  cursor.session_uuid = session_uuid;
  cursor.operation_id = "query.execute";
  cursor.total_row_count = rows;
  cursor.max_chunk_rows = 4;
  cursor.max_chunk_bytes = 65536;
  cursor.stream_descriptor_uuid = sbps::MakeUuidV7Bytes();
  cursor.stream_descriptor_version = 1;
  cursor.stream_descriptor_generation = 1;
  cursor.execution_uuid = sbps::MakeUuidV7Bytes();
  cursor.result_set_uuid = sbps::MakeUuidV7Bytes();
  cursor.row_descriptor_uuid = sbps::MakeUuidV7Bytes();
  cursor.snapshot_uuid = sbps::MakeUuidV7Bytes();
  cursor.statement_context_statement_uuid =
      scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes());

  scratchbird::server::ServerStatementContextRecord statement_context;
  statement_context.session_uuid = session_uuid;
  statement_context.statement_uuid = cursor.statement_context_statement_uuid;
  statement_context.receipt.opaque_id = 42;
  registry->statement_contexts_by_statement_uuid.emplace(
      statement_context.statement_uuid, statement_context);

  std::vector<std::uint8_t> binding;
  constexpr std::string_view kBindingDomain =
      "ScratchBird.CursorStreamDescriptor.ReceiptBinding.V1";
  binding.insert(binding.end(), kBindingDomain.begin(), kBindingDomain.end());
  PutU64(&binding, statement_context.receipt.opaque_id);
  PutUuid(&binding, cursor.stream_descriptor_uuid);
  PutU16(&binding, cursor.stream_descriptor_version);
  PutU64(&binding, cursor.stream_descriptor_generation);
  PutUuid(&binding, cursor.cursor_uuid);
  PutUuid(&binding, cursor.execution_uuid);
  PutUuid(&binding, cursor.result_set_uuid);
  PutUuid(&binding, cursor.row_descriptor_uuid);
  PutUuid(&binding, cursor.snapshot_uuid);
  PutU64(&binding, cursor.max_chunk_rows);
  PutU64(&binding, cursor.max_chunk_bytes);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(binding);
  Require(digest.ok(), "could not bind cursor stream descriptor to receipt");
  cursor.stream_descriptor_receipt_binding_sha256 = digest.digest;
  cursor.stream_descriptor_live = true;

  const auto cursor_uuid = cursor.cursor_uuid;
  registry->cursors_by_uuid.emplace(
      scratchbird::server::UuidBytesToText(cursor_uuid), std::move(cursor));
  return cursor_uuid;
}

const ServerCursorRecord& FindCursor(
    const ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& cursor_uuid) {
  const auto found = registry.cursors_by_uuid.find(
      scratchbird::server::UuidBytesToText(cursor_uuid));
  Require(found != registry.cursors_by_uuid.end(), "cursor fixture is missing");
  return found->second;
}

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);

  const auto cursor_uuid = OpenCursor(&registry, session_uuid, 3);
  const auto fetch1 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, cursor_uuid), 2, 4096));
  Require(fetch1.accepted, "metadata fetch rejected");
  const auto fetch1_payload = scratchbird::server::DecodeFetchResultForTest(fetch1.payload);
  Require(fetch1_payload.has_value(), "metadata fetch payload malformed");
  Require(fetch1_payload->row_count == 2 && !fetch1_payload->end_of_cursor,
          "metadata fetch did not return first bounded chunk");
  Require(Contains(fetch1_payload->detail, "\"fetch_count\":1") &&
              Contains(fetch1_payload->detail, "\"next_row_index\":2") &&
              Contains(fetch1_payload->detail, "\"total_rows\":3") &&
              Contains(fetch1_payload->detail, "\"capability\":\"forward_only\""),
          "metadata fetch did not include cursor metadata");

  const auto too_small = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, cursor_uuid), 1, 8));
  Require(!too_small.accepted && HasDiagnostic(too_small, "SERVER.STREAM.BYTES_TOO_SMALL"),
          "too-small max_bytes did not fail before advancing cursor");

  const auto fetch2 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, cursor_uuid), 1, 4096));
  Require(fetch2.accepted, "final fetch rejected after too-small byte limit");
  const auto fetch2_payload = scratchbird::server::DecodeFetchResultForTest(fetch2.payload);
  Require(fetch2_payload.has_value() && fetch2_payload->row_count == 1 && fetch2_payload->end_of_cursor,
          "final fetch did not preserve cursor position after byte-limit refusal");
  Require(Contains(fetch2_payload->detail, "\"fetch_count\":2") &&
              Contains(fetch2_payload->detail, "\"end_of_cursor\":true"),
          "final fetch metadata missing exhausted state");

  const auto eos = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, cursor_uuid), 1, 4096));
  Require(!eos.accepted && HasDiagnostic(eos, "SERVER.STREAM.DESCRIPTOR_STALE"),
          "post-EOS fetch did not refuse the released stream descriptor");

  const auto bytes_cursor = OpenCursor(&registry, session_uuid, 1);
  const auto too_large_bytes = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, bytes_cursor), 1, 65537));
  Require(!too_large_bytes.accepted && HasDiagnostic(too_large_bytes, "SERVER.STREAM.BYTES_TOO_LARGE"),
          "oversized max_bytes did not fail closed");

  const auto scroll_cursor = OpenCursor(&registry, session_uuid, 1);
  const auto scroll = scratchbird::server::HandleFetch(
      &registry,
      FetchFrame(session_uuid, FindCursor(registry, scroll_cursor), 1, 4096,
                 kFetchFlagScroll));
  Require(!scroll.accepted && HasDiagnostic(scroll, "SERVER.CURSOR.SCROLL_UNSUPPORTED"),
          "unsupported cursor scroll flag did not fail closed");

  const auto rows_cursor = OpenCursor(&registry, session_uuid, 1);
  const auto too_many_rows = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, rows_cursor), 5, 4096));
  Require(!too_many_rows.accepted && HasDiagnostic(too_many_rows, "SERVER.STREAM.CHUNK_TOO_LARGE"),
          "oversized max_rows did not fail closed");

  const auto close = scratchbird::server::HandleCloseCursor(&registry, CloseFrame(session_uuid, cursor_uuid));
  Require(close.accepted, "close rejected");

  std::cout << "sb_server_cursor_protocol_conformance=passed\n";
  return EXIT_SUCCESS;
}
