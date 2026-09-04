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

struct RouteBinding {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

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

ServerSessionRegistry MakeRegistry(RouteBinding* route) {
  Require(route != nullptr, "COPY stream route binding is required");
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  route->connection_uuid = session.connection_uuid;
  route->session_uuid = session.session_uuid;

  ServerSessionRegistry registry;
  registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  registry.channel_state = scratchbird::server::ServerChannelState::kReady;
  return registry;
}

const ServerCursorRecord& FindCursor(
    const ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& cursor_uuid) {
  const auto found = registry.cursors_by_uuid.find(
      scratchbird::server::UuidBytesToText(cursor_uuid));
  Require(found != registry.cursors_by_uuid.end(),
          "COPY stream cursor fixture is missing");
  return found->second;
}

std::array<std::uint8_t, 16> InstallCopyStreamCursor(
    ServerSessionRegistry* registry,
    const RouteBinding& route,
    std::uint64_t total_rows,
    std::uint64_t rejected_rows) {
  Require(registry != nullptr && rejected_rows <= total_rows,
          "COPY stream fixture counts are invalid");

  ServerCursorRecord cursor;
  cursor.cursor_uuid = sbps::MakeUuidV7Bytes();
  cursor.request_uuid = sbps::MakeUuidV7Bytes();
  cursor.session_uuid = route.session_uuid;
  cursor.operation_id = "engine.op.bulk_import_stream";
  cursor.bulk_stream_kind = "copy_import";
  cursor.bulk_total_rows = total_rows;
  cursor.bulk_rejected_rows = rejected_rows;
  cursor.total_row_count = rejected_rows + 3;
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
  statement_context.session_uuid = route.session_uuid;
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
  Require(digest.ok(), "COPY stream descriptor binding digest failed");
  cursor.stream_descriptor_receipt_binding_sha256 = digest.digest;
  cursor.stream_descriptor_live = true;

  const auto cursor_uuid = cursor.cursor_uuid;
  registry->cursors_by_uuid.emplace(
      scratchbird::server::UuidBytesToText(cursor_uuid), std::move(cursor));
  return cursor_uuid;
}

sbps::Frame FetchFrame(const RouteBinding& route,
                       const ServerCursorRecord& cursor,
                       std::uint64_t max_rows) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = route.connection_uuid;
  frame.header.session_uuid = route.session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      route.session_uuid, cursor.cursor_uuid, max_rows, cursor.max_chunk_bytes);
  frame.payload.insert(frame.payload.end(), cursor.stream_descriptor_uuid.begin(),
                       cursor.stream_descriptor_uuid.end());
  PutU16(&frame.payload, cursor.stream_descriptor_version);
  PutU64(&frame.payload, cursor.stream_descriptor_generation);
  return frame;
}

}  // namespace

int main() {
  RouteBinding route;
  auto registry = MakeRegistry(&route);
  const auto cursor_uuid =
      InstallCopyStreamCursor(&registry, route, 10, 2);

  const auto fetch1 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(route, FindCursor(registry, cursor_uuid), 2));
  const auto payload1 =
      scratchbird::server::DecodeFetchResultForTest(fetch1.payload);
  Require(fetch1.accepted && payload1.has_value() &&
              payload1->row_count == 2 && !payload1->end_of_cursor,
          "COPY first fetch did not return progress plus first reject");
  Require(Contains(payload1->row_packet, "\"event\":\"progress\"") &&
              Contains(payload1->row_packet, "\"rows_processed\":10") &&
              Contains(payload1->row_packet, "\"event\":\"reject_record\"") &&
              Contains(payload1->row_packet, "\"source_row_number\":1"),
          "COPY first fetch missing progress or first reject record");

  const auto fetch2 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(route, FindCursor(registry, cursor_uuid), 2));
  const auto payload2 =
      scratchbird::server::DecodeFetchResultForTest(fetch2.payload);
  Require(fetch2.accepted && payload2.has_value() &&
              payload2->row_count == 2 && !payload2->end_of_cursor,
          "COPY second fetch did not return second reject plus summary");
  Require(Contains(payload2->row_packet, "\"source_row_number\":2") &&
              Contains(payload2->row_packet, "\"event\":\"bulk_summary\"") &&
              Contains(payload2->row_packet, "\"accepted_rows\":8") &&
              Contains(payload2->row_packet, "\"rejected_rows\":2"),
          "COPY second fetch missing reject record or bulk summary");

  const auto fetch3 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(route, FindCursor(registry, cursor_uuid), 1));
  const auto payload3 =
      scratchbird::server::DecodeFetchResultForTest(fetch3.payload);
  Require(fetch3.accepted && payload3.has_value() &&
              payload3->row_count == 1 && payload3->end_of_cursor,
          "COPY final fetch did not return final status");
  Require(Contains(payload3->row_packet, "\"event\":\"final_status\"") &&
              Contains(payload3->row_packet,
                       "\"status\":\"completed_with_rejects\"") &&
              Contains(payload3->detail, "\"end_of_cursor\":true"),
          "COPY final fetch missing final status or cursor metadata");

  std::cout << "sbsql_copy_streaming_conformance=passed\n";
  return EXIT_SUCCESS;
}
