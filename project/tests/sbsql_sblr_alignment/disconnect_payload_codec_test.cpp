// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "wire/parser_server_ipc/sbps_disconnect_payload_codec.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>

namespace {
std::size_t allocation_calls = 0;
std::size_t fail_allocation = std::numeric_limits<std::size_t>::max();
}
void* operator new(std::size_t size) {
  const auto call = allocation_calls++;
  if (call == fail_allocation) throw std::bad_alloc();
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace ipc = scratchbird::parser::ipc;
using Bytes = std::vector<std::uint8_t>;
using Fields = std::array<Bytes, 5>;
using Status = ipc::PsDisconnectCodecStatusV1;
std::size_t checks = 0;
void Check(bool condition, std::string_view message) {
  ++checks;
  if (!condition) {
    std::cerr << "check " << checks << ": " << message << '\n';
    std::exit(1);
  }
}
void Number(Bytes& b, std::uint32_t n, unsigned width) {
  for (unsigned i = 0; i < width; ++i) b.push_back((n >> (8 * i)) & 255);
}
void Set(Bytes& b, std::size_t pos, std::uint32_t n, unsigned width) {
  for (unsigned i = 0; i < width; ++i) b[pos + i] = (n >> (8 * i)) & 255;
}
// Independent fixture writer: values and field table come from Core section5,
// not a captured runtime response or the codec under test.
Bytes Wire(const Fields& fields) {
  Bytes out{1, 0};
  for (unsigned i = 0; i != 5; ++i) {
    Number(out, i + 1, 2);
    Number(out, fields[i].size(), 4);
    out.insert(out.end(), fields[i].begin(), fields[i].end());
  }
  return out;
}
ipc::PsDisconnectUuidV1 Id(unsigned n) {
  ipc::PsDisconnectUuidV1 id{0x01, 0x9d, 0x21, 0x31, 0x41, 0x51,
                            0x70, 0x01, 0x80, 0x01};
  id[14] = static_cast<std::uint8_t>(n >> 8);
  id[15] = static_cast<std::uint8_t>(n);
  return id;
}
Fields Populated() {
  Fields f{Bytes{3}, Bytes{2}, Bytes{2, 0}, Bytes{2, 0}, Bytes{0xab, 0, 0xcd}};
  f[2].reserve(34);
  for (unsigned i = 1; i != 3; ++i) {
    const auto id = Id(i);
    f[2].insert(f[2].end(), id.begin(), id.end());
  }
  Number(f[3], 3, 4);
  f[3].insert(f[3].end(), {0, 0x80, 0xff});
  Number(f[3], 2, 4);
  f[3].insert(f[3].end(), {0x7f, 0});
  return f;
}
void EmptyOnError(const ipc::PsDisconnectDecodeResultV1& r) {
  Check(r.payload == ipc::PsDisconnectPayloadV1{}, "decode error published fields");
  Check(std::string_view(r.diagnostic.code()) ==
        (r.diagnostic.status == Status::resource_limit_exceeded
            ? "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED"
            : "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID"), "diagnostic identity");
}
void Invalid(const Bytes& wire, const ipc::PsDisconnectLimitsV1& limits = {}) {
  const auto before = allocation_calls;
  const auto decoded = ipc::DecodePsDisconnectPayloadV1(wire, limits);
  Check(!decoded.ok(), "malformed bytes accepted");
  Check(allocation_calls == before, "invalid decode allocated before validation");
  EmptyOnError(decoded);
}
void RoundTrip(const ipc::PsDisconnectPayloadV1& payload,
               const ipc::PsDisconnectLimitsV1& limits = {}) {
  const auto encoded = ipc::EncodePsDisconnectPayloadV1(payload, limits);
  Check(encoded.ok(), "valid encode refused");
  const auto decoded = ipc::DecodePsDisconnectPayloadV1(encoded.bytes, limits);
  Check(decoded.ok() && decoded.payload == payload, "round trip lost bytes");
}
void RefuseEncode(const ipc::PsDisconnectPayloadV1& payload,
                   const ipc::PsDisconnectLimitsV1& limits = {}) {
  const auto before = allocation_calls;
  const auto encoded = ipc::EncodePsDisconnectPayloadV1(payload, limits);
  Check(!encoded.ok() && encoded.bytes.empty(), "invalid encode published bytes");
  Check(allocation_calls == before, "invalid encode allocated");
}
}  // namespace

int main() {
  const Fields empty{Bytes{1}, Bytes{1}, Bytes{0, 0}, Bytes{0, 0}, Bytes{}};
  const Bytes minimal{
    1,0, 1,0,1,0,0,0,1, 2,0,1,0,0,0,1,
    3,0,2,0,0,0,0,0, 4,0,2,0,0,0,0,0, 5,0,0,0,0,0};
  Check(Wire(empty) == minimal, "independent empty wire literal");
  Check(ipc::EncodePsDisconnectPayloadV1({}).bytes == minimal,
        "encoder exact revision field ordering and empty vectors");
  RoundTrip({});
  const auto populated = Wire(Populated());
  auto decoded = ipc::DecodePsDisconnectPayloadV1(populated);
  Check(decoded.ok(), "populated envelope decode");
  Check(decoded.payload.reason == ipc::PsDisconnectReasonV1::drain &&
        decoded.payload.initiator == ipc::PsDisconnectInitiatorV1::server &&
        decoded.payload.active_requests == std::vector{Id(1), Id(2)} &&
        decoded.payload.finality_tokens == std::vector<Bytes>{{0,128,255},{127,0}} &&
        decoded.payload.message_vector_set == Bytes({171,0,205}),
        "independent binary field oracle");
  Check(ipc::EncodePsDisconnectPayloadV1(decoded.payload).bytes == populated,
        "canonical byte equality");
  const auto sample = decoded.payload;

  for (unsigned reason = 0; reason != 256; ++reason) {
    auto f = empty;
    f[0][0] = reason;
    auto p = ipc::PsDisconnectPayloadV1{};
    p.reason = static_cast<ipc::PsDisconnectReasonV1>(reason);
    if (reason >= 1 && reason <= 7) {
      Check(ipc::DecodePsDisconnectPayloadV1(Wire(f)).ok(), "closed reason admitted");
      RoundTrip(p);
    } else { Invalid(Wire(f)); RefuseEncode(p); }
  }
  for (unsigned initiator = 0; initiator != 256; ++initiator) {
    auto f = empty;
    f[1][0] = initiator;
    auto p = ipc::PsDisconnectPayloadV1{};
    p.initiator = static_cast<ipc::PsDisconnectInitiatorV1>(initiator);
    if (initiator >= 1 && initiator <= 4) {
      Check(ipc::DecodePsDisconnectPayloadV1(Wire(f)).ok(), "closed initiator admitted");
      RoundTrip(p);
    } else { Invalid(Wire(f)); RefuseEncode(p); }
  }
  for (const auto& wire : {minimal, populated}) {
    for (std::size_t n = 0; n < wire.size(); ++n)
      Invalid(Bytes(wire.begin(), wire.begin() + n));
    auto trailing = wire; trailing.push_back(0); Invalid(trailing);
    for (std::size_t i = 0; i < wire.size(); ++i) {
      for (unsigned v = 0; v != 256; ++v) {
        auto mutation = wire; mutation[i] = v;
        const auto before = allocation_calls;
        const auto r = ipc::DecodePsDisconnectPayloadV1(mutation);
        if (!r.ok()) {
          Check(allocation_calls == before, "mutation failure allocated");
          EmptyOnError(r);
        } else {
          const auto encoded = ipc::EncodePsDisconnectPayloadV1(r.payload);
          Check(encoded.ok() && encoded.bytes == mutation,
                "accepted mutation noncanonical or partially consumed");
        }
      }
    }
  }
  for (unsigned field = 0; field < 5; ++field) {
    for (unsigned id : {0u, 6u, 65535u}) {
      auto w = minimal;
      const std::array<std::size_t, 5> start{2,9,16,24,32};
      Set(w, start[field], id, 2); Invalid(w);
    }
    auto f = Populated(); f[field].clear();
    // The fifth blob is expressly permitted to be empty.
    if (field == 4) Check(ipc::DecodePsDisconnectPayloadV1(Wire(f)).ok(), "empty message blob");
    else Invalid(Wire(f));
  }

  for (unsigned version = 0; version != 16; ++version) {
    if (version == 7) continue;
    auto p = sample; p.active_requests[0][6] = version << 4;
    RefuseEncode(p);
    auto f = Populated(); f[2][8] = version << 4; Invalid(Wire(f));
  }
  for (unsigned variant : {0u, 0x40u, 0xc0u}) {
    auto p = sample; p.active_requests[0][8] = variant; RefuseEncode(p);
    auto f = Populated(); f[2][10] = variant; Invalid(Wire(f));
  }
  for (bool duplicate : {false, true}) {
    auto p = sample;
    p.active_requests[0] = duplicate ? Id(2) : Id(3);
    RefuseEncode(p);
    auto f = Populated();
    std::copy(p.active_requests[0].begin(), p.active_requests[0].end(), f[2].begin() + 2);
    Invalid(Wire(f));
  }
  {
    auto p = sample; p.active_requests[0] = {}; RefuseEncode(p);
    auto f = Populated(); std::fill_n(f[2].begin() + 2, 16, 0); Invalid(Wire(f));
  }
  for (unsigned count : {1u, 3u, 1025u, 65535u}) {
    for (unsigned field : {2u, 3u}) {
      auto f = Populated(); Set(f[field], 0, count, 2); Invalid(Wire(f));
    }
  }
  for (std::uint32_t size : {0u, 1u, 4u, 4097u, 0xffffffffu}) {
    auto f = Populated(); Set(f[3], 2, size, 4); Invalid(Wire(f));
  }
  {
    auto p = sample; p.finality_tokens[0].clear(); RefuseEncode(p);
    p.finality_tokens[0].resize(4097); RefuseEncode(p);
  }
  for (unsigned dimension = 0; dimension != 5; ++dimension) {
    ipc::PsDisconnectLimitsV1 limit;
    switch (dimension) {
      case 0: limit.maximum_payload_bytes = populated.size() - 1; break;
      case 1: limit.maximum_message_vector_bytes = 2; break;
      case 2: limit.maximum_active_requests = 1; break;
      case 3: limit.maximum_finality_tokens = 1; break;
      case 4: limit.maximum_finality_token_bytes = 2; break;
    }
    RefuseEncode(sample, limit); Invalid(populated, limit);
  }
  {
    ipc::PsDisconnectLimitsV1 exact;
    exact.maximum_payload_bytes = populated.size();
    exact.maximum_message_vector_bytes = 3;
    exact.maximum_active_requests = 2;
    exact.maximum_finality_tokens = 2;
    exact.maximum_finality_token_bytes = 3;
    RoundTrip(sample, exact);
    ipc::PsDisconnectLimitsV1 zero{38,0,0,0,0};
    RoundTrip({}, zero);
    zero.maximum_payload_bytes = 0; RefuseEncode({}, zero); Invalid(minimal, zero);
  }
  {
    ipc::PsDisconnectPayloadV1 max;
    for (unsigned i = 0; i != 1024; ++i) max.active_requests.push_back(Id(i));
    max.finality_tokens.resize(1024, Bytes(4096, 0xa5));
    max.message_vector_set.resize(16 * 1024 * 1024, 0x5a);
    RoundTrip(max);
    ipc::PsDisconnectLimitsV1 excessive;
    excessive.maximum_payload_bytes = std::numeric_limits<std::uint64_t>::max();
    excessive.maximum_message_vector_bytes = excessive.maximum_payload_bytes;
    excessive.maximum_active_requests = excessive.maximum_finality_tokens =
        excessive.maximum_finality_token_bytes = 0xffffffff;
    max.message_vector_set.push_back(0); RefuseEncode(max, excessive);
    max.message_vector_set.pop_back();
    max.active_requests.push_back(Id(1024)); RefuseEncode(max, excessive);
    max.active_requests.pop_back();
    max.finality_tokens.push_back({1}); RefuseEncode(max, excessive);
    max.finality_tokens.pop_back();
    max.finality_tokens[0].push_back(1); RefuseEncode(max, excessive);
  }
  // Allocation failure at every construction step must discard all fields.
  const auto before = allocation_calls;
  const auto successful = ipc::DecodePsDisconnectPayloadV1(populated);
  const auto allocations = allocation_calls - before;
  Check(successful.ok() && allocations > 0, "allocation probe");
  for (std::size_t i = 0; i < allocations; ++i) {
    fail_allocation = allocation_calls + i;
    const auto failed = ipc::DecodePsDisconnectPayloadV1(populated);
    fail_allocation = std::numeric_limits<std::size_t>::max();
    Check(failed.diagnostic.status == Status::resource_limit_exceeded,
          "decode allocation failure escaped");
    EmptyOnError(failed);
  }
  fail_allocation = allocation_calls;
  const auto failed_encode = ipc::EncodePsDisconnectPayloadV1(sample);
  fail_allocation = std::numeric_limits<std::size_t>::max();
  Check(failed_encode.diagnostic.status == Status::resource_limit_exceeded &&
        failed_encode.bytes.empty(), "encode allocation failure published prefix");

  std::cout << "disconnect_payload_codec checks=" << checks
            << " PASS schema=1074 outer_layout_only=true engine_execution=false\n";
}
