// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "wire/parser_server_ipc/sbps_execution_request_payload_codec.hpp"

#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace ipc = scratchbird::parser::ipc;
using scratchbird::core::platform::byte;

static_assert(ipc::kPsExecuteRequestMessageTypeV1 == 42);
static_assert(ipc::kPsExecuteRequestSchemaIdV1 == 1042);
static_assert(ipc::kPsFetchRequestMessageTypeV1 == 44);
static_assert(ipc::kPsFetchRequestSchemaIdV1 == 1044);

void Require(bool condition, const std::string& detail) {
  if (!condition) throw std::runtime_error(detail);
}

ipc::PsRequestUuidV1 Uuid(byte discriminator) {
  ipc::PsRequestUuidV1 uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0x9d;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[15] = discriminator;
  return uuid;
}

ipc::PsExecuteRequestValidationContextV1 ExecuteContext() {
  ipc::PsExecuteRequestValidationContextV1 context;
  context.expected_session_uuid = Uuid(1);
  context.expected_catalog_generation = 101;
  context.expected_security_epoch = 102;
  context.expected_policy_generation = 103;
  context.authoritative_parameter_count = 0;
  return context;
}

ipc::PsExecuteRequestPayloadV1 DirectExecute() {
  ipc::PsExecuteRequestPayloadV1 request;
  request.session_uuid = Uuid(1);
  request.sblr_envelope = {0x53, 0x42, 0x4c, 0x52};
  request.transaction_request.request_kind =
      ipc::PsTransactionRequestKindV1::join_existing;
  request.transaction_request.transaction_uuid = Uuid(2);
  request.execution_options.result_mode =
      ipc::PsExecutionResultModeV1::row_batch;
  request.execution_options.cursor_mode =
      ipc::PsExecutionCursorModeV1::none;
  request.execution_options.max_rows = 64;
  request.execution_options.max_result_bytes = 4096;
  request.catalog_generation = 101;
  request.security_epoch = 102;
  request.policy_generation = 103;
  return request;
}

ipc::PsFetchRequestValidationContextV1 FetchContext() {
  ipc::PsFetchRequestValidationContextV1 context;
  context.expected_session_uuid = Uuid(1);
  context.expected_cursor_uuid = Uuid(3);
  context.expected_cursor_stream_descriptor_uuid = Uuid(4);
  context.expected_cursor_stream_descriptor_version = 1;
  context.expected_cursor_stream_descriptor_generation = 5;
  context.maximum_rows = 256;
  context.maximum_bytes = 8192;
  context.maximum_timeout_ms = 2000;
  return context;
}

ipc::PsFetchRequestPayloadV1 FetchRequest() {
  ipc::PsFetchRequestPayloadV1 request;
  request.session_uuid = Uuid(1);
  request.cursor_uuid = Uuid(3);
  request.max_rows = 32;
  request.max_bytes = 4096;
  request.timeout_ms = 1000;
  request.fetch_direction = ipc::PsFetchDirectionV1::forward;
  request.cursor_stream_descriptor_uuid = Uuid(4);
  request.cursor_stream_descriptor_version = 1;
  request.cursor_stream_descriptor_generation = 5;
  return request;
}

void PositiveExecuteAndCanonicalDecode() {
  const auto context = ExecuteContext();
  const auto request = DirectExecute();
  const auto encoded =
      ipc::EncodeAndValidatePsExecuteRequestV1Payload(request, context);
  Require(encoded.ok(), "1042 direct request did not encode");
  Require(encoded.canonical_payload.size() == 300 &&
              encoded.canonical_payload[0] == 1 &&
              encoded.canonical_payload[1] == 0,
          "1042 TLV revision drifted");
  const auto decoded = ipc::DecodeAndValidatePsExecuteRequestV1Payload(
      encoded.canonical_payload, context);
  Require(decoded.ok(), "1042 canonical request did not decode");
  Require(decoded.canonical_payload == encoded.canonical_payload,
          "1042 decode did not retain exact canonical bytes");
  Require(decoded.request.sblr_envelope == request.sblr_envelope &&
              decoded.request.transaction_request.transaction_uuid ==
                  request.transaction_request.transaction_uuid,
          "1042 request fields drifted");

  auto standalone = request;
  standalone.sblr_envelope.clear();
  standalone.transaction_request.request_kind =
      ipc::PsTransactionRequestKindV1::begin_explicit;
  standalone.transaction_request.transaction_uuid = {};
  standalone.transaction_request.requested_isolation_profile_uuid = Uuid(5);
  standalone.transaction_request.requested_sync_profile_uuid = Uuid(6);
  Require(ipc::EncodeAndValidatePsExecuteRequestV1Payload(standalone, context)
              .ok(),
          "1042 standalone begin request was refused");
}

void NegativeExecuteDrift() {
  const auto context = ExecuteContext();
  const auto request = DirectExecute();

  auto mixed = request;
  mixed.prepared_statement_uuid = Uuid(7);
  Require(!ipc::EncodeAndValidatePsExecuteRequestV1Payload(mixed, context).ok(),
          "1042 admitted simultaneous direct and prepared paths");

  auto partial = request;
  partial.execution_options.allow_partial_result = true;
  Require(!ipc::EncodeAndValidatePsExecuteRequestV1Payload(partial, context)
               .ok(),
          "1042 admitted partial results");

  auto donor_context = context;
  donor_context.admitted_donor_execution_profile_uuid = Uuid(8);
  Require(ipc::EncodeAndValidatePsExecuteRequestV1Payload(request,
                                                          donor_context)
              .ok(),
          "1042 required an optional donor execution profile");
  auto wrong_donor = request;
  wrong_donor.execution_options.donor_execution_profile_uuid = Uuid(9);
  Require(!ipc::EncodeAndValidatePsExecuteRequestV1Payload(wrong_donor,
                                                           donor_context)
               .ok(),
          "1042 admitted an unbound donor execution profile");

  auto wrong_generation = request;
  ++wrong_generation.security_epoch;
  const auto stale = ipc::EncodeAndValidatePsExecuteRequestV1Payload(
      wrong_generation, context);
  Require(!stale.ok() &&
              stale.outcome.diagnostic_code ==
                  "PARSER_SERVER_IPC.GENERATION_STALE",
          "1042 generation drift was not correlated");

  auto encoded =
      ipc::EncodeAndValidatePsExecuteRequestV1Payload(request, context);
  Require(encoded.ok(), "1042 negative fixture seed failed");
  encoded.canonical_payload[2] = 2;
  Require(!ipc::DecodeAndValidatePsExecuteRequestV1Payload(
               encoded.canonical_payload, context)
               .ok(),
          "1042 admitted an out-of-order first field");
}

void PositiveAndNegativeFetch() {
  const auto context = FetchContext();
  const auto request = FetchRequest();
  const auto encoded =
      ipc::EncodeAndValidatePsFetchRequestV1Payload(request, context);
  Require(encoded.ok() && encoded.canonical_payload.size() == 139,
          "1044 request did not encode to exactly 139 bytes");
  const auto decoded = ipc::DecodeAndValidatePsFetchRequestV1Payload(
      encoded.canonical_payload, context);
  Require(decoded.ok() &&
              decoded.canonical_payload == encoded.canonical_payload,
          "1044 canonical round trip failed");

  auto zero_timeout = request;
  zero_timeout.timeout_ms = 0;
  Require(!ipc::EncodeAndValidatePsFetchRequestV1Payload(zero_timeout, context)
               .ok(),
          "1044 admitted a zero timeout");

  auto descriptor_drift = request;
  descriptor_drift.cursor_stream_descriptor_generation = 6;
  Require(!ipc::EncodeAndValidatePsFetchRequestV1Payload(descriptor_drift,
                                                         context)
               .ok(),
          "1044 admitted cursor descriptor drift");

  auto trailing = encoded.canonical_payload;
  trailing.push_back(0);
  Require(!ipc::DecodeAndValidatePsFetchRequestV1Payload(trailing, context)
               .ok(),
          "1044 admitted trailing bytes");
}

}  // namespace

int main() {
  try {
    PositiveExecuteAndCanonicalDecode();
    NegativeExecuteDrift();
    PositiveAndNegativeFetch();
  } catch (const std::exception& error) {
    (void)error;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
