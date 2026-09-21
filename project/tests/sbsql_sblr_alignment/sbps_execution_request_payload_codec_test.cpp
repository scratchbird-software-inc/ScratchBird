// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "wire/parser_server_ipc/sbps_execution_request_payload_codec.hpp"

#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t allocation_limit = std::numeric_limits<std::size_t>::max();
std::size_t refused_allocations = 0;
}
void* operator new(std::size_t size) {
  if (size > allocation_limit) {
    ++refused_allocations;
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

namespace ipc = scratchbird::parser::ipc;
using scratchbird::core::platform::byte;

static_assert(ipc::kPsExecuteRequestMessageTypeV1 == 42);
static_assert(ipc::kPsExecuteRequestSchemaIdV1 == 1042);
static_assert(ipc::kPsFetchRequestMessageTypeV1 == 44);
static_assert(ipc::kPsFetchRequestSchemaIdV1 == 1044);

std::size_t checks = 0;
void Require(bool condition, const std::string& detail) {
  ++checks;
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

// Locate fixture fields independently of the production decoder. These tests
// change identity bytes without changing any other TLV structure.
std::size_t FieldOffset(const std::vector<byte>& bytes,
                        std::uint16_t wanted, std::size_t base = 0) {
  for (std::size_t offset = base + 2; offset + 6 <= bytes.size();) {
    const auto id = static_cast<std::uint16_t>(
        bytes[offset] | (static_cast<unsigned>(bytes[offset + 1]) << 8));
    std::uint32_t length = 0;
    for (unsigned i = 0; i != 4; ++i)
      length |= static_cast<std::uint32_t>(bytes[offset + 2 + i]) << (8 * i);
    Require(length <= bytes.size() - offset - 6, "invalid mutation fixture");
    if (id == wanted) return offset + 6;
    offset += 6 + length;
  }
  throw std::runtime_error("mutation fixture field absent");
}

void SystemIdentityVersions() {
  for (unsigned slot = 0; slot != 7; ++slot) {
    auto request = DirectExecute();
    auto context = ExecuteContext();
    ipc::PsRequestUuidV1* identity = nullptr;
    std::uint16_t field = 0, nested = 0;
    switch (slot) {
      case 0: identity = &request.session_uuid; field = 1; break;
      case 1:
        request.sblr_envelope.clear();
        identity = &request.prepared_statement_uuid; field = 2; break;
      case 2:
        identity = &request.transaction_request.transaction_uuid;
        field = 5; nested = 2; break;
      case 3:
        request.sblr_envelope.clear();
        request.transaction_request.request_kind =
            ipc::PsTransactionRequestKindV1::create_savepoint;
        identity = &request.transaction_request.savepoint_uuid;
        field = 5; nested = 3; break;
      case 4:
      case 5:
        request.sblr_envelope.clear();
        request.transaction_request = {};
        request.transaction_request.request_kind =
            ipc::PsTransactionRequestKindV1::begin_explicit;
        identity = slot == 4
            ? &request.transaction_request.requested_isolation_profile_uuid
            : &request.transaction_request.requested_sync_profile_uuid;
        field = 5; nested = slot == 4 ? 4 : 5; break;
      case 6:
        identity = &request.execution_options.donor_execution_profile_uuid;
        field = 6; nested = 6; break;
    }
    *identity = Uuid(static_cast<byte>(slot + 20));
    if (slot == 0) context.expected_session_uuid = *identity;
    if (slot == 6) context.admitted_donor_execution_profile_uuid = *identity;
    const auto valid = ipc::EncodeAndValidatePsExecuteRequestV1Payload(request, context);
    Require(valid.ok(), "valid identity profile refused");
    auto offset = FieldOffset(valid.canonical_payload, field);
    if (nested) offset = FieldOffset(valid.canonical_payload, nested, offset);
    const auto original = *identity;
    for (unsigned version = 0; version != 16; ++version) {
      for (unsigned variant = 0; variant != 4; ++variant) {
        *identity = original;
        (*identity)[6] = static_cast<byte>(version << 4);
        (*identity)[8] = static_cast<byte>(variant << 6);
        if (slot == 0) context.expected_session_uuid = *identity;
        if (slot == 6) context.admitted_donor_execution_profile_uuid = *identity;
        const bool expected = version == 7 && variant == 2;
        const auto encoded =
            ipc::EncodeAndValidatePsExecuteRequestV1Payload(request, context);
        Require(encoded.ok() == expected, "1042 accepted non-v7 system identity");
        auto bytes = valid.canonical_payload;
        std::copy(identity->begin(), identity->end(), bytes.begin() + offset);
        const auto decoded =
            ipc::DecodeAndValidatePsExecuteRequestV1Payload(bytes, context);
        Require(decoded.ok() == expected, "1042 decoded non-v7 system identity");
        if (!expected) {
          Require(encoded.canonical_payload.empty() &&
                      decoded.canonical_payload.empty() &&
                      decoded.request.session_uuid == ipc::PsRequestUuidV1{},
                  "invalid identity published an executable request");
        }
      }
    }
  }
  for (unsigned slot = 0; slot != 3; ++slot) {
    auto request = FetchRequest();
    auto context = FetchContext();
    auto* identity = slot == 0 ? &request.session_uuid
        : slot == 1 ? &request.cursor_uuid
                    : &request.cursor_stream_descriptor_uuid;
    auto* authority = slot == 0 ? &context.expected_session_uuid
        : slot == 1 ? &context.expected_cursor_uuid
                    : &context.expected_cursor_stream_descriptor_uuid;
    const auto valid = ipc::EncodeAndValidatePsFetchRequestV1Payload(request, context);
    Require(valid.ok(), "valid fetch identity profile refused");
    const auto offset = FieldOffset(valid.canonical_payload, slot == 2 ? 7 : slot + 1);
    const auto original = *identity;
    for (unsigned version = 0; version != 16; ++version) {
      for (unsigned variant = 0; variant != 4; ++variant) {
        *identity = original;
        (*identity)[6] = static_cast<byte>(version << 4);
        (*identity)[8] = static_cast<byte>(variant << 6);
        *authority = *identity;
        const bool expected = version == 7 && variant == 2;
        const auto encoded =
            ipc::EncodeAndValidatePsFetchRequestV1Payload(request, context);
        Require(encoded.ok() == expected, "1044 accepted non-v7 system identity");
        auto bytes = valid.canonical_payload;
        std::copy(identity->begin(), identity->end(), bytes.begin() + offset);
        const auto decoded =
            ipc::DecodeAndValidatePsFetchRequestV1Payload(bytes, context);
        Require(decoded.ok() == expected, "1044 decoded non-v7 system identity");
        if (!expected) {
          Require(encoded.canonical_payload.empty() &&
                      decoded.canonical_payload.empty() &&
                      decoded.request.session_uuid == ipc::PsRequestUuidV1{},
                  "invalid fetch identity published a request");
        }
      }
    }
  }
}

void RejectBeforeExecutableBlobCopy() {
  auto request = DirectExecute();
  request.sblr_envelope.resize(1024 * 1024, 0x61);
  const auto context = ExecuteContext();
  const auto encoded = ipc::EncodeAndValidatePsExecuteRequestV1Payload(request, context);
  Require(encoded.ok(), "large outer execution-payload fixture refused");
  for (unsigned failure = 0; failure != 8; ++failure) {
    auto bytes = encoded.canonical_payload;
    auto authority = context;
    switch (failure) {
      case 0: bytes[FieldOffset(bytes, 1) + 6] = 0x40; break;
      case 1: {
        const auto offset = FieldOffset(bytes, 2);
        const auto prepared = Uuid(55);
        std::copy(prepared.begin(), prepared.end(), bytes.begin() + offset);
        break;
      }
      case 2: bytes[FieldOffset(bytes, 2) + 6] = 0x40; break;
      case 3: bytes[FieldOffset(bytes, 2, FieldOffset(bytes, 5)) + 6] = 0x40; break;
      case 4: bytes[FieldOffset(bytes, 7, FieldOffset(bytes, 6))] = 1; break;
      case 5: bytes[FieldOffset(bytes, 7)] ^= 1; break;
      case 6: authority.authoritative_parameter_count = 1; break;
      case 7: authority.maximum_sblr_bytes = 4096; break;
    }
    const auto refused_before = refused_allocations;
    ipc::PsExecuteRequestPayloadCodecResultV1 result;
    struct Guard {
      Guard() { allocation_limit = 4096; }
      ~Guard() { allocation_limit = std::numeric_limits<std::size_t>::max(); }
    };
    {
      Guard guard;
      result = ipc::DecodeAndValidatePsExecuteRequestV1Payload(bytes, authority);
    }
    Require(!result.ok() && result.canonical_payload.empty() &&
                result.request.sblr_envelope.empty() &&
                refused_allocations == refused_before,
            "invalid metadata reached executable blob allocation or publication");
  }
  const auto decoded = ipc::DecodeAndValidatePsExecuteRequestV1Payload(
      encoded.canonical_payload, context);
  Require(decoded.ok() && decoded.request.sblr_envelope == request.sblr_envelope,
          "valid outer payload no longer preserved its opaque executable bytes");
}

}  // namespace

int main() {
  try {
    PositiveExecuteAndCanonicalDecode();
    NegativeExecuteDrift();
    PositiveAndNegativeFetch();
    SystemIdentityVersions();
    RejectBeforeExecutableBlobCopy();
    std::cout << "PASS execution request identity checks=" << checks << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
