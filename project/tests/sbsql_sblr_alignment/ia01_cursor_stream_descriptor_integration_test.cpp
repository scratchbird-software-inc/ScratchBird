// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch_server.hpp"
#include "hash_digest.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "IA-01 cursor stream descriptor: " << detail << '\n';
  std::exit(EXIT_FAILURE);
}
void Require(bool value, std::string_view detail) {
  if (!value) Fail(detail);
}
bool HasDiagnostic(const server::SessionOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}
void U16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(value); out->push_back(value >> 8);
}
void U64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i) out->push_back(value >> (8 * i));
}
void Uuid(std::vector<std::uint8_t>* out,
          const std::array<std::uint8_t, 16>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

struct Fixture {
  server::ServerSessionRegistry registry;
  std::array<std::uint8_t, 16> session = sbps::MakeUuidV7Bytes();
  std::array<std::uint8_t, 16> cursor = sbps::MakeUuidV7Bytes();
  std::array<std::uint8_t, 16> descriptor = sbps::MakeUuidV7Bytes();
  Fixture() {
    server::ServerSessionRecord session_record;
    session_record.session_uuid = session;
    session_record.auth_context_uuid = sbps::MakeUuidV7Bytes();
    session_record.principal_uuid = sbps::MakeUuidV7Bytes();
    session_record.effective_user_uuid = session_record.principal_uuid;
    registry.sessions_by_uuid[server::UuidBytesToText(session)] = session_record;

    server::ServerCursorRecord record;
    record.cursor_uuid = cursor;
    record.session_uuid = session;
    record.request_uuid = sbps::MakeUuidV7Bytes();
    record.operation_id = "query.execute";
    record.total_row_count = 1;
    record.row_packet = "row[0]=descriptor-bound\n";
    record.max_chunk_rows = 4;
    record.max_chunk_bytes = 4096;
    record.stream_descriptor_uuid = descriptor;
    record.stream_descriptor_version = 1;
    record.stream_descriptor_generation = 1;
    record.execution_uuid = sbps::MakeUuidV7Bytes();
    record.result_set_uuid = sbps::MakeUuidV7Bytes();
    record.row_descriptor_uuid = sbps::MakeUuidV7Bytes();
    record.snapshot_uuid = sbps::MakeUuidV7Bytes();
    record.statement_context_statement_uuid =
        server::UuidBytesToText(sbps::MakeUuidV7Bytes());
    server::ServerStatementContextRecord statement_context;
    statement_context.session_uuid = session;
    statement_context.statement_uuid = record.statement_context_statement_uuid;
    statement_context.receipt.opaque_id = 42;
    registry.statement_contexts_by_statement_uuid.emplace(
        statement_context.statement_uuid, statement_context);
    std::vector<std::uint8_t> binding;
    constexpr std::string_view domain =
        "ScratchBird.CursorStreamDescriptor.ReceiptBinding.V1";
    binding.insert(binding.end(), domain.begin(), domain.end());
    U64(&binding, statement_context.receipt.opaque_id);
    Uuid(&binding, record.stream_descriptor_uuid);
    U16(&binding, record.stream_descriptor_version);
    U64(&binding, record.stream_descriptor_generation);
    Uuid(&binding, record.cursor_uuid);
    Uuid(&binding, record.execution_uuid);
    Uuid(&binding, record.result_set_uuid);
    Uuid(&binding, record.row_descriptor_uuid);
    Uuid(&binding, record.snapshot_uuid);
    U64(&binding, record.max_chunk_rows);
    U64(&binding, record.max_chunk_bytes);
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(binding);
    Require(digest.ok(), "could not create receipt binding fixture");
    record.stream_descriptor_receipt_binding_sha256 = digest.digest;
    record.stream_descriptor_live = true;
    registry.cursors_by_uuid[server::UuidBytesToText(cursor)] = record;
  }
  sbps::Frame Fetch(bool include_descriptor, std::uint16_t version = 1,
                    std::uint64_t generation = 1,
                    std::uint64_t rows = 1,
                    std::uint64_t bytes = 1024) const {
    sbps::Frame frame;
    frame.header.message_type =
        static_cast<std::uint16_t>(sbps::MessageType::kFetch);
    frame.header.request_uuid = sbps::MakeUuidV7Bytes();
    frame.header.session_uuid = session;
    frame.payload = server::EncodeFetchPayloadForTest(
        session, cursor, rows, bytes, 0);
    if (include_descriptor) {
      frame.payload.insert(frame.payload.end(), descriptor.begin(),
                           descriptor.end());
      U16(&frame.payload, version);
      U64(&frame.payload, generation);
    }
    return frame;
  }
};
}  // namespace

int main() {
  {
    Fixture fixture;
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true));
    Require(result.accepted,
            "published descriptor did not survive through first fetch");
  }
  {
    Fixture fixture;
    fixture.session = sbps::MakeUuidV7Bytes();
    fixture.cursor = sbps::MakeUuidV7Bytes();
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(false));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_REQUIRED"),
            "absent descriptor plus unknown session/cursor did not prefer REQUIRED");
  }
  {
    Fixture fixture;
    fixture.session = sbps::MakeUuidV7Bytes();
    fixture.cursor = sbps::MakeUuidV7Bytes();
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true, 2));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_INVALID"),
            "malformed descriptor plus unknown session/cursor did not prefer INVALID");
  }
  {
    Fixture fixture;
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(false));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_REQUIRED"),
            "missing descriptor did not take REQUIRED precedence");
  }
  {
    Fixture fixture;
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true, 2));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_INVALID"),
            "wrong descriptor version did not take INVALID precedence");
  }
  {
    Fixture fixture;
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true, 1, 2));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_STALE"),
            "wrong descriptor generation did not take STALE precedence");
  }
  {
    Fixture fixture;
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true, 1, 1, 5));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.CHUNK_TOO_LARGE"),
            "row maximum excess did not take CHUNK_TOO_LARGE precedence");
  }
  {
    Fixture fixture;
    const auto result = server::HandleFetch(
        &fixture.registry, fixture.Fetch(true, 1, 1, 1, 4097));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.BYTES_TOO_LARGE"),
            "byte maximum excess did not take BYTES_TOO_LARGE precedence");
  }
  {
    Fixture fixture;
    auto& cursor = fixture.registry.cursors_by_uuid[
        server::UuidBytesToText(fixture.cursor)];
    (void)server::ReleaseAndClearServerCursorResources(&fixture.registry,
                                                       &cursor);
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_STALE"),
            "revoked descriptor replay was not stale");
  }
  {
    Fixture fixture;
    fixture.registry.statement_contexts_by_statement_uuid.clear();
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_STALE"),
            "released receipt did not revoke descriptor");
  }
  {
    Fixture fixture;
    auto& cursor = fixture.registry.cursors_by_uuid[
        server::UuidBytesToText(fixture.cursor)];
    cursor.stream_descriptor_receipt_binding_sha256[0] ^= 0x80;
    const auto result = server::HandleFetch(&fixture.registry,
                                             fixture.Fetch(true));
    Require(!result.accepted &&
                HasDiagnostic(result, "SERVER.STREAM.DESCRIPTOR_STALE"),
            "tampered receipt binding did not revoke descriptor");
  }
  return EXIT_SUCCESS;
}
