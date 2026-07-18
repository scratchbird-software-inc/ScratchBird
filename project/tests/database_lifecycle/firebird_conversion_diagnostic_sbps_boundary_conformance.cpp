// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_worker_session.hpp"
#include "parser_server_client.hpp"
#include "sbps.hpp"
#include "sblr_dispatch_server.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::server::ServerDiagnosticField;

constexpr std::size_t kMessageVectorHeaderBytes = 64;
constexpr std::size_t kMessageVectorRecordHeaderBytes = 112;

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

std::uint16_t ReadLeU16(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1]) << 8u;
}

std::uint32_t ReadLeU32(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         static_cast<std::uint32_t>(bytes[offset + 1]) << 8u |
         static_cast<std::uint32_t>(bytes[offset + 2]) << 16u |
         static_cast<std::uint32_t>(bytes[offset + 3]) << 24u;
}

std::uint64_t ReadLeU64(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
  }
  return value;
}

void WriteLeU32(std::vector<std::uint8_t>* bytes,
                std::size_t offset,
                std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*bytes)[offset++] = static_cast<std::uint8_t>(value >> shift);
  }
}

void AppendLeU16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8u));
}

void AppendLeU64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendUuid(std::vector<std::uint8_t>* bytes,
                const std::array<std::uint8_t, 16>& uuid) {
  bytes->insert(bytes->end(), uuid.begin(), uuid.end());
}

void AppendString(std::vector<std::uint8_t>* bytes, std::string_view value) {
  AppendLeU16(bytes, static_cast<std::uint16_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void AppendBytes(std::vector<std::uint8_t>* bytes,
                 const std::vector<std::uint8_t>& value) {
  AppendLeU64(bytes, value.size());
  bytes->insert(bytes->end(), value.begin(), value.end());
}

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(0u - (crc & 1u));
      crc = (crc >> 1u) ^ (0x82f63b78u & mask);
    }
  }
  return ~crc;
}

bool HasDiagnosticCode(
    const scratchbird::parser::ipc::MessageVectorSet& messages,
    std::string_view code) {
  return std::any_of(
      messages.diagnostics.begin(), messages.diagnostics.end(),
      [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::optional<std::size_t> FindMessageVectorTrailer(
    const std::vector<std::uint8_t>& payload) {
  constexpr std::array<std::uint8_t, 4> kMagic{0x53, 0x42, 0x4d, 0x56};
  for (std::size_t offset = 8; offset + kMagic.size() <= payload.size();
       ++offset) {
    if (std::equal(kMagic.begin(), kMagic.end(), payload.begin() + offset) &&
        ReadLeU64(payload, offset - 8) == payload.size() - offset) {
      return offset;
    }
  }
  return std::nullopt;
}

bool RewriteMessageVectorChecksums(std::vector<std::uint8_t>* payload,
                                   std::size_t base,
                                   bool rewrite_record_crc,
                                   bool rewrite_records_crc,
                                   bool rewrite_header_crc) {
  if (payload == nullptr || base > payload->size() ||
      payload->size() - base < kMessageVectorHeaderBytes) {
    return false;
  }
  const std::size_t set_size = payload->size() - base;
  const std::uint32_t vector_count = ReadLeU32(*payload, base + 12);
  std::size_t record = base + kMessageVectorHeaderBytes;
  for (std::uint32_t index = 0; index < vector_count; ++index) {
    if (record + kMessageVectorRecordHeaderBytes > payload->size()) {
      return false;
    }
    const std::uint32_t record_bytes = ReadLeU32(*payload, record);
    if (record_bytes < kMessageVectorRecordHeaderBytes ||
        record_bytes > payload->size() - record) {
      return false;
    }
    if (rewrite_record_crc) {
      WriteLeU32(payload, record + 4, 0);
      WriteLeU32(payload, record + 4,
                 Crc32c(payload->data() + record, record_bytes));
    }
    record += record_bytes;
  }
  if (record != payload->size()) return false;
  if (rewrite_records_crc) {
    WriteLeU32(payload, base + 20, 0);
    if (vector_count != 0) {
      WriteLeU32(payload, base + 20,
                 Crc32c(payload->data() + base + kMessageVectorHeaderBytes,
                        set_size - kMessageVectorHeaderBytes));
    }
  }
  if (rewrite_header_crc) {
    WriteLeU32(payload, base + 52, 0);
    WriteLeU32(payload, base + 52,
               Crc32c(payload->data() + base,
                      kMessageVectorHeaderBytes));
  }
  return true;
}

std::uint32_t ReadXdrU32(const std::vector<std::uint8_t>& bytes,
                         std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

bool ReadXdrString(const std::vector<std::uint8_t>& bytes,
                   std::size_t* offset,
                   std::string* value) {
  if (offset == nullptr || value == nullptr || *offset + 4 > bytes.size()) {
    return false;
  }
  const std::uint32_t length = ReadXdrU32(bytes, *offset);
  *offset += 4;
  const std::size_t padded =
      static_cast<std::size_t>(length) + ((4u - (length & 3u)) & 3u);
  if (*offset + padded > bytes.size()) return false;
  value->assign(reinterpret_cast<const char*>(bytes.data() + *offset),
                length);
  *offset += padded;
  return true;
}

struct DiagnosticFrameFixture {
  scratchbird::server::sbps::FrameHeader header;
  std::vector<std::uint8_t> payload;
};

DiagnosticFrameFixture MakeRegisteredDiagnosticFrame(
    std::string_view code,
    const std::vector<ServerDiagnosticField>& engine_fields) {
  const auto registered =
      scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
          code, engine_fields);
  std::vector<ServerDiagnosticField> public_fields(registered.begin(),
                                                   registered.end());
  // This field proves that the real server SBPS redaction gate participates
  // in the boundary. It is never expected to reach the parser client.
  public_fields.push_back({"internal_path", "/tmp/secret"});

  auto diagnostic = scratchbird::server::sbps::IpcDiagnostic(
      std::string(code), std::string(code), "must not be parsed",
      std::move(public_fields));
  DiagnosticFrameFixture fixture;
  fixture.header.message_type = static_cast<std::uint16_t>(
      scratchbird::server::sbps::MessageType::kDiagnostic);
  fixture.header.flags = scratchbird::server::sbps::kFlagResponse |
                         scratchbird::server::sbps::kFlagError |
                         scratchbird::server::sbps::kFlagFinal;
  fixture.header.payload_schema_id =
      scratchbird::server::sbps::kSchemaMessageVectorSetV1;
  fixture.header.request_uuid =
      scratchbird::server::sbps::MakeUuidV7Bytes();
  fixture.payload = scratchbird::server::sbps::EncodeMessageVectorSet(
      {diagnostic}, fixture.header.request_uuid);
  return fixture;
}

scratchbird::parser::ipc::MessageVectorSet RoundTripRegisteredDiagnostic(
    std::string_view code,
    const std::vector<ServerDiagnosticField>& engine_fields,
    bool* ok) {
  const auto registered =
      scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
          code, engine_fields);
  const auto fixture = MakeRegisteredDiagnosticFrame(code, engine_fields);
  const auto frame = scratchbird::server::sbps::EncodeFrame(
      fixture.header, fixture.payload);

  scratchbird::parser::ipc::MessageVectorSet decoded;
  const bool decoded_ok = scratchbird::parser::ipc::
      DecodeDiagnosticFrameForTest(frame, &decoded);
  if (ok != nullptr) {
    *ok = Expect(decoded_ok && decoded.diagnostics.size() == 1,
                 "SBPS diagnostic frame did not cross the production decoder") &&
          *ok;
    if (decoded_ok && decoded.diagnostics.size() == 1) {
      *ok = Expect(decoded.diagnostics.front().fields.size() ==
                       registered.size(),
                   "SBPS decoded diagnostic field shape drifted from the exact registry") &&
            *ok;
      if (registered.size() == 1 &&
          decoded.diagnostics.front().fields.size() == 1) {
        *ok = Expect(
                  decoded.diagnostics.front().fields.front().name ==
                          "conversion_input_text" &&
                      decoded.diagnostics.front().fields.front().value ==
                          registered.front().value,
                  "SBPS decoded conversion field changed name or value") &&
              *ok;
      }
      for (const auto& field : decoded.diagnostics.front().fields) {
        *ok = Expect(field.name != "internal_path",
                     "SBPS public-field gate leaked an internal path") &&
              *ok;
      }
    }
  }
  return decoded;
}

scratchbird::parser::ipc::MessageVectorSet RoundTripTypedRejectedDiagnostic(
    std::string_view code,
    const std::vector<ServerDiagnosticField>& registered_fields,
    bool* ok) {
  scratchbird::server::sbps::Frame request;
  request.header.payload_schema_id = 4011;
  request.header.request_uuid =
      scratchbird::server::sbps::MakeUuidV7Bytes();
  request.header.session_uuid =
      scratchbird::server::sbps::MakeUuidV7Bytes();
  auto public_fields = registered_fields;
  public_fields.push_back({"internal_path", "/tmp/secret"});
  const auto rejected = scratchbird::server::RejectExecuteSblrBeforeEngine(
      request, std::string(code), "typed_statement_rejected",
      std::move(public_fields));

  scratchbird::parser::ipc::ServerExecutionResult decoded_result;
  scratchbird::parser::ipc::MessageVectorSet decoded;
  const bool decoded_ok = scratchbird::parser::ipc::
      DecodeExecuteResultPayloadV2ForTest(
          rejected.payload, &decoded_result, &decoded);
  if (ok != nullptr) {
    *ok = Expect(!rejected.accepted && rejected.response_schema_id == 4012 &&
                     (rejected.frame_flags &
                      scratchbird::server::sbps::kFlagError) == 0 &&
                     decoded_ok && !decoded_result.accepted &&
                     decoded_result.finality_state ==
                         scratchbird::parser::ipc::
                             ParserTransactionFinality::kNotApplicable &&
                     decoded_result.transaction_diagnostic_code == code &&
                     decoded.diagnostics.size() == 1 &&
                     decoded.diagnostics.front().code == code,
                 "typed V2 statement rejection lost its outcome or diagnostic") &&
          *ok;
    if (decoded.diagnostics.size() == 1) {
      *ok = Expect(decoded.diagnostics.front().fields.size() ==
                       registered_fields.size(),
                   "typed V2 diagnostic trailer bypassed public-field redaction") &&
            *ok;
      for (const auto& field : decoded.diagnostics.front().fields) {
        *ok = Expect(field.name != "internal_path",
                     "typed V2 diagnostic trailer leaked an internal path") &&
              *ok;
      }
    }
  }
  return decoded;
}

struct SelectedTypedRejectionFixture {
  scratchbird::server::SessionOperationResult operation;
  scratchbird::server::ServerTransactionState selected;
};

SelectedTypedRejectionFixture MakeSelectedTypedRejection(bool* ok) {
  SelectedTypedRejectionFixture fixture;
  fixture.selected.local_transaction_id = 941;
  fixture.selected.snapshot_visible_through_local_transaction_id = 937;
  fixture.selected.transaction_uuid =
      "019f0000-0000-7000-8000-000000000941";
  fixture.selected.transaction_timestamp = "2026-07-15T12:00:00Z";

  const auto session_uuid =
      scratchbird::server::sbps::MakeUuidV7Bytes();
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = session_uuid;
  session.connection_uuid = session_uuid;
  session.server_channel_uuid =
      scratchbird::server::sbps::MakeUuidV7Bytes();
  session.transaction_routing_v2_negotiated = true;
  session.local_transaction_id = fixture.selected.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      fixture.selected.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = fixture.selected.transaction_uuid;
  session.transaction_timestamp = fixture.selected.transaction_timestamp;
  session.default_local_transaction_id =
      fixture.selected.local_transaction_id;
  session.transactions_by_local_id.emplace(
      fixture.selected.local_transaction_id, fixture.selected);

  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid.emplace(
      scratchbird::server::UuidBytesToText(session_uuid),
      std::move(session));

  scratchbird::server::sbps::Frame request;
  request.header.message_type = static_cast<std::uint16_t>(
      scratchbird::server::sbps::MessageType::kExecuteSblr);
  request.header.payload_schema_id = 4011;
  request.header.connection_uuid = session_uuid;
  request.header.session_uuid = session_uuid;
  request.header.request_uuid =
      scratchbird::server::sbps::MakeUuidV7Bytes();
  AppendUuid(&request.payload, session_uuid);
  AppendUuid(&request.payload, {});
  request.payload.push_back(0);  // no cursor
  request.payload.push_back(1);  // exact selected transaction route
  AppendLeU64(&request.payload, fixture.selected.local_transaction_id);
  AppendString(&request.payload, fixture.selected.transaction_uuid);
  AppendString(&request.payload,
               "not-a-canonical-sblr-envelope");
  AppendBytes(&request.payload, {});

  const scratchbird::server::HostedEngineState engine_state;
  fixture.operation = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, request);

  scratchbird::parser::ipc::ServerExecutionResult decoded;
  scratchbird::parser::ipc::MessageVectorSet messages;
  const bool decoded_ok = scratchbird::parser::ipc::
      DecodeExecuteResultPayloadV2ForTest(
          fixture.operation.payload, &decoded, &messages);
  if (ok != nullptr) {
    const bool server_selector_exact =
        fixture.operation.transaction_state.has_value() &&
        fixture.operation.transaction_state->selected_present &&
        fixture.operation.transaction_state->selected.local_transaction_id ==
            fixture.selected.local_transaction_id &&
        fixture.operation.transaction_state->selected.transaction_uuid ==
            fixture.selected.transaction_uuid &&
        fixture.operation.transaction_state->selected
                .snapshot_visible_through_local_transaction_id ==
            fixture.selected.snapshot_visible_through_local_transaction_id &&
        fixture.operation.transaction_state->finality ==
            scratchbird::server::ServerTransactionResponseState::Finality::
                kNotApplicable &&
        !fixture.operation.transaction_state->finalized_present &&
        !fixture.operation.transaction_state->replacement_present &&
        fixture.operation.transaction_state->replacement_reason ==
            scratchbird::server::ServerTransactionResponseState::
                ReplacementReason::kNone;
    const bool parser_selector_exact =
        decoded.selected_transaction_present &&
        decoded.selected_transaction.local_transaction_id ==
            fixture.selected.local_transaction_id &&
        decoded.selected_transaction.transaction_uuid ==
            fixture.selected.transaction_uuid;
    *ok = Expect(!fixture.operation.accepted &&
                     fixture.operation.response_schema_id == 4012 &&
                     (fixture.operation.frame_flags &
                      scratchbird::server::sbps::kFlagError) == 0 &&
                     server_selector_exact && decoded_ok && !decoded.accepted &&
                     parser_selector_exact &&
                     decoded.finality_state ==
                         scratchbird::parser::ipc::
                             ParserTransactionFinality::kNotApplicable &&
                     !decoded.finality_applied &&
                     !decoded.finalized_transaction_present &&
                     !decoded.replacement_transaction_present &&
                     decoded.replacement_reason ==
                         scratchbird::parser::ipc::
                             ParserTransactionReplacementReason::kNone &&
                     !decoded.catalog_invalidation_applied &&
                     !decoded.transaction_state_present &&
                     !messages.diagnostics.empty(),
                 "typed V2 statement rejection did not preserve the exact selected MGA selector as still open") &&
          *ok;
  }
  return fixture;
}

bool ExpectMalformedTypedTrailerRejected(
    const std::vector<std::uint8_t>& payload,
    std::string_view source_diagnostic_code,
    std::string_view label) {
  scratchbird::parser::ipc::ServerExecutionResult result;
  scratchbird::parser::ipc::MessageVectorSet messages;
  const bool decoded = scratchbird::parser::ipc::
      DecodeExecuteResultPayloadV2ForTest(payload, &result, &messages);
  const std::string failure = std::string(label) +
                              " was not rejected before parser rendering";
  return Expect(!decoded &&
                    HasDiagnosticCode(
                        messages,
                        "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID") &&
                    !HasDiagnosticCode(messages, source_diagnostic_code),
                failure);
}

bool VerifyMalformedTypedTrailersFailClosed(
    const SelectedTypedRejectionFixture& fixture) {
  bool ok = true;
  const auto trailer = FindMessageVectorTrailer(fixture.operation.payload);
  ok = Expect(trailer.has_value(),
              "typed V2 rejection did not contain a message-vector trailer") &&
       ok;
  if (!trailer || !fixture.operation.transaction_state.has_value()) {
    return false;
  }
  const std::size_t base = *trailer;
  const std::string& diagnostic_code =
      fixture.operation.transaction_state->diagnostic_code;
  ok = Expect(!diagnostic_code.empty(),
              "typed V2 rejection omitted its canonical diagnostic code") &&
       ok;

  {
    auto malformed = fixture.operation.payload;
    WriteLeU32(&malformed, base + 16,
               ReadLeU32(malformed, base + 16) - 1);
    ok = Expect(RewriteMessageVectorChecksums(
                    &malformed, base, false, false, true),
                "could not prepare the inner-length mutation") &&
         ok;
    ok = ExpectMalformedTypedTrailerRejected(
             malformed, diagnostic_code, "inner total_bytes mismatch") &&
         ok;
  }
  {
    auto malformed = fixture.operation.payload;
    malformed[base + 52] ^= 0x01u;
    ok = ExpectMalformedTypedTrailerRejected(
             malformed, diagnostic_code, "message-vector header CRC mismatch") &&
         ok;
  }
  {
    auto malformed = fixture.operation.payload;
    const std::size_t record = base + kMessageVectorHeaderBytes;
    malformed[record + 4] ^= 0x01u;
    ok = Expect(RewriteMessageVectorChecksums(
                    &malformed, base, false, true, true),
                "could not prepare the record-CRC mutation") &&
         ok;
    ok = ExpectMalformedTypedTrailerRejected(
             malformed, diagnostic_code, "message-vector record CRC mismatch") &&
         ok;
  }
  {
    auto malformed = fixture.operation.payload;
    malformed[base + 56] = 1;
    ok = Expect(RewriteMessageVectorChecksums(
                    &malformed, base, false, false, true),
                "could not prepare the set-reserved mutation") &&
         ok;
    ok = ExpectMalformedTypedTrailerRejected(
             malformed, diagnostic_code, "nonzero set reserved value") &&
         ok;
  }
  {
    auto malformed = fixture.operation.payload;
    const std::size_t record = base + kMessageVectorHeaderBytes;
    malformed[record + 110] = 1;
    ok = Expect(RewriteMessageVectorChecksums(
                    &malformed, base, true, true, true),
                "could not prepare the record-reserved mutation") &&
         ok;
    ok = ExpectMalformedTypedTrailerRejected(
             malformed, diagnostic_code, "nonzero record reserved value") &&
         ok;
  }
  {
    auto malformed = fixture.operation.payload;
    const std::size_t record_local = kMessageVectorHeaderBytes;
    const std::uint16_t language_bytes =
        ReadLeU16(malformed, base + record_local + 92);
    const std::size_t language_end =
        record_local + kMessageVectorRecordHeaderBytes + language_bytes;
    const std::size_t padded_end = (language_end + 3u) & ~std::size_t{3u};
    ok = Expect(padded_end > language_end,
                "message-vector fixture did not expose string padding") &&
         ok;
    if (padded_end > language_end) {
      malformed[base + language_end] = 1;
      ok = Expect(RewriteMessageVectorChecksums(
                      &malformed, base, true, true, true),
                  "could not prepare the nonzero-padding mutation") &&
           ok;
      ok = ExpectMalformedTypedTrailerRejected(
               malformed, diagnostic_code, "nonzero variable-field padding") &&
           ok;
    }
  }
  {
    auto malformed = fixture.operation.payload;
    const std::size_t record = base + kMessageVectorHeaderBytes;
    malformed[record + 48] ^= 0x01u;
    ok = Expect(RewriteMessageVectorChecksums(
                    &malformed, base, true, true, true),
                "could not prepare the request-UUID mutation") &&
         ok;
    ok = ExpectMalformedTypedTrailerRejected(
             malformed, diagnostic_code, "message-vector request UUID mismatch") &&
         ok;
  }
  {
    auto malformed = fixture.operation.payload;
    const std::uint16_t outcome_bytes = ReadLeU16(malformed, 0);
    ok = Expect(outcome_bytes == 8,
                "typed rejection fixture has an unexpected outcome encoding") &&
         ok;
    if (outcome_bytes == 8) {
      constexpr std::string_view kAccepted = "accepted";
      std::copy(kAccepted.begin(), kAccepted.end(), malformed.begin() + 2);
      ok = ExpectMalformedTypedTrailerRejected(
               malformed, diagnostic_code,
               "accepted result carrying rejection diagnostics") &&
           ok;
    }
  }
  return ok;
}

bool VerifyOuterFrameRequestCorrelation(
    std::string_view code,
    const std::vector<ServerDiagnosticField>& fields) {
  auto fixture = MakeRegisteredDiagnosticFrame(code, fields);
  fixture.header.request_uuid.front() ^= 0x01u;
  const auto frame = scratchbird::server::sbps::EncodeFrame(
      fixture.header, fixture.payload);
  scratchbird::parser::ipc::MessageVectorSet messages;
  const bool decoded = scratchbird::parser::ipc::
      DecodeDiagnosticFrameForTest(frame, &messages);
  return Expect(decoded &&
                    HasDiagnosticCode(messages,
                                      "PARSER_SERVER_IPC.REQUEST_FAILED") &&
                    !HasDiagnosticCode(messages, code),
                "outer SBPS request UUID mismatch exposed a diagnostic from a different request");
}

bool ExpectConversionStatus(
    const scratchbird::parser::firebird::FirebirdConversionErrorPresentation&
        presentation,
    std::string_view input) {
  const auto& bytes = presentation.encoded_status_vector;
  std::size_t offset = 0;
  if (bytes.size() < 20 || ReadXdrU32(bytes, offset) != 1) return false;
  offset += 4;
  if (ReadXdrU32(bytes, offset) != 335544334u) return false;
  offset += 4;
  if (ReadXdrU32(bytes, offset) != 2) return false;
  offset += 4;
  std::string status_input;
  if (!ReadXdrString(bytes, &offset, &status_input) || status_input != input) {
    return false;
  }
  if (offset + 4 > bytes.size() || ReadXdrU32(bytes, offset) != 19) {
    return false;
  }
  offset += 4;
  std::string sqlstate;
  if (!ReadXdrString(bytes, &offset, &sqlstate) || sqlstate != "22018") {
    return false;
  }
  return offset + 4 == bytes.size() && ReadXdrU32(bytes, offset) == 0;
}

}  // namespace

int main() {
  constexpr std::string_view kCode =
      "SB_DIAG_FUNCTION_CONVERSION_INPUT";
  bool ok = true;

  for (const std::string_view input :
       {std::string_view("1"), std::string_view("29.2.2002"),
        std::string_view("9:11:60")}) {
    const std::vector<ServerDiagnosticField> candidates{
        {"conversion_input_text", std::string(input)}};
    const auto registered =
        scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
            kCode, candidates);
    ok = Expect(registered.size() == 1 &&
                    registered.front().key == "conversion_input_text" &&
                    registered.front().value == input,
                "neutral diagnostic registry rejected an exact conversion field") &&
         ok;
    const auto decoded = RoundTripRegisteredDiagnostic(kCode, candidates, &ok);
    const auto presentation = scratchbird::parser::firebird::
        PresentFirebirdConversionInputDiagnostic(decoded, "op_execute");
    ok = Expect(presentation.has_value(),
                "Firebird worker rejected the decoded SBPS conversion field") &&
         ok;
    if (presentation) {
      ok = Expect(presentation->conversion_input_text == input &&
                      presentation->response_json.find("\"sqlstate\":\"22018\"") !=
                          std::string::npos &&
                      presentation->response_json.find("must not be parsed") ==
                          std::string::npos &&
                      presentation->response_json.find("\"row") ==
                          std::string::npos &&
                      ExpectConversionStatus(*presentation, input),
                  "decoded SBPS conversion presentation drifted") &&
           ok;
    }
    const auto typed_decoded = RoundTripTypedRejectedDiagnostic(
        kCode, registered, &ok);
    const auto typed_presentation = scratchbird::parser::firebird::
        PresentFirebirdConversionInputDiagnostic(typed_decoded, "op_execute");
    ok = Expect(typed_presentation.has_value() &&
                    typed_presentation->conversion_input_text == input &&
                    ExpectConversionStatus(*typed_presentation, input),
                "typed V2 rejection did not reach Firebird conversion presentation") &&
         ok;
  }

  const auto selected_rejection = MakeSelectedTypedRejection(&ok);
  ok = VerifyMalformedTypedTrailersFailClosed(selected_rejection) && ok;
  ok = VerifyOuterFrameRequestCorrelation(
           kCode, {{"conversion_input_text", "29.2.2002"}}) &&
       ok;

  const std::vector<std::pair<std::string,
                              std::vector<ServerDiagnosticField>>>
      rejected{
          {std::string(kCode), {}},
          {std::string(kCode),
           {{"conversion_input_text", "1"},
            {"conversion_input_text", "2"}}},
          {std::string(kCode), {{"other", "1"}}},
          {std::string(kCode), {{"conversion_input_text", ""}}},
          {std::string(kCode),
           {{"conversion_input_text", std::string(1025, 'x')}}},
          {std::string(kCode),
           {{"conversion_input_text", std::string("a\0b", 3)}}},
          {std::string(kCode),
           {{"conversion_input_text", std::string("\xC0\xAF", 2)}}},
          {"SB_DIAG_FUNCTION_INVALID_INPUT",
           {{"conversion_input_text", "29.2.2002"}}},
      };
  for (const auto& [code, candidates] : rejected) {
    ok = Expect(
             scratchbird::server::RegisteredEngineDiagnosticFieldsForTest(
                 code, candidates)
                 .empty(),
             "neutral diagnostic registry forwarded a malformed or unrelated field") &&
         ok;
    const auto decoded = RoundTripRegisteredDiagnostic(code, candidates, &ok);
    ok = Expect(!scratchbird::parser::firebird::
                     PresentFirebirdConversionInputDiagnostic(
                         decoded, "op_execute")
                     .has_value(),
                "malformed or unrelated field became a Firebird conversion") &&
         ok;
  }

  return ok ? 0 : 1;
}
