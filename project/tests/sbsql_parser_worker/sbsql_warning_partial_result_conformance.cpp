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
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::server::ServerCursorRecord;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
namespace sbps = scratchbird::server::sbps;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
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

ServerSessionRegistry MakeRegistry(
    std::array<std::uint8_t, 16>* session_uuid) {
  Require(session_uuid != nullptr, "warning-stream session output is required");
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  *session_uuid = session.session_uuid;
  ServerSessionRegistry registry;
  registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

const ServerCursorRecord& FindCursor(
    const ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& cursor_uuid) {
  const auto found = registry.cursors_by_uuid.find(
      scratchbird::server::UuidBytesToText(cursor_uuid));
  Require(found != registry.cursors_by_uuid.end(),
          "warning-stream cursor fixture is missing");
  return found->second;
}

std::array<std::uint8_t, 16> InstallWarningStreamCursor(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint64_t partial_rows,
    std::uint64_t warning_count) {
  Require(registry != nullptr && partial_rows != 0,
          "warning-stream cursor fixture is invalid");
  ServerCursorRecord cursor;
  cursor.cursor_uuid = sbps::MakeUuidV7Bytes();
  cursor.request_uuid = sbps::MakeUuidV7Bytes();
  cursor.session_uuid = session_uuid;
  cursor.operation_id = "query.execute";
  cursor.warning_stream_kind = "partial_result_warning_chain";
  cursor.partial_result_rows = partial_rows;
  cursor.warning_count = warning_count;
  cursor.total_row_count = partial_rows + warning_count + 1;
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
  Require(digest.ok(), "warning-stream descriptor binding digest failed");
  cursor.stream_descriptor_receipt_binding_sha256 = digest.digest;
  cursor.stream_descriptor_live = true;

  const auto cursor_uuid = cursor.cursor_uuid;
  registry->cursors_by_uuid.emplace(
      scratchbird::server::UuidBytesToText(cursor_uuid), std::move(cursor));
  return cursor_uuid;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const ServerCursorRecord& cursor,
                       std::uint64_t max_rows) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor.cursor_uuid, max_rows, cursor.max_chunk_bytes);
  frame.payload.insert(frame.payload.end(), cursor.stream_descriptor_uuid.begin(),
                       cursor.stream_descriptor_uuid.end());
  PutU16(&frame.payload, cursor.stream_descriptor_version);
  PutU64(&frame.payload, cursor.stream_descriptor_generation);
  return frame;
}

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto cursor_uuid =
      InstallWarningStreamCursor(&registry, session_uuid, 3, 2);

  const auto fetch1 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, cursor_uuid), 3));
  const auto payload1 =
      scratchbird::server::DecodeFetchResultForTest(fetch1.payload);
  Require(fetch1.accepted && payload1.has_value() &&
              payload1->row_count == 3 && !payload1->end_of_cursor,
          "first warning stream fetch did not return partial rows");
  Require(Contains(payload1->row_packet,
                   "\"event\":\"partial_result_row\"") &&
              Contains(payload1->row_packet, "\"row_index\":0") &&
              Contains(payload1->row_packet, "\"partial_result\":true"),
          "first warning stream fetch missing partial-result rows");

  const auto fetch2 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, cursor_uuid), 2));
  const auto payload2 =
      scratchbird::server::DecodeFetchResultForTest(fetch2.payload);
  Require(fetch2.accepted && payload2.has_value() &&
              payload2->row_count == 2 && !payload2->end_of_cursor,
          "second warning stream fetch did not return warning chain");
  Require(Contains(payload2->row_packet, "\"event\":\"warning\"") &&
              Contains(payload2->row_packet,
                       "\"diagnostic_code\":\"STREAM.WARNING.0\"") &&
              Contains(payload2->row_packet, "\"does_not_abort\":true"),
          "second warning stream fetch missing non-aborting warning chain");

  const auto fetch3 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, FindCursor(registry, cursor_uuid), 1));
  const auto payload3 =
      scratchbird::server::DecodeFetchResultForTest(fetch3.payload);
  Require(fetch3.accepted && payload3.has_value() &&
              payload3->row_count == 1 && payload3->end_of_cursor,
          "final warning stream fetch did not return finality");
  Require(Contains(payload3->row_packet,
                   "\"event\":\"partial_result_finality\"") &&
              Contains(payload3->row_packet,
                       "\"status\":\"completed_with_warnings\"") &&
              Contains(payload3->detail, "\"end_of_cursor\":true"),
          "final warning stream fetch missing warning finality or cursor metadata");

  std::cout << "sbsql_warning_partial_result_conformance=passed\n";
  return EXIT_SUCCESS;
}
