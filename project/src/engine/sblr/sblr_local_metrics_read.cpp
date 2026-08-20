// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_local_metrics_read.hpp"

#include "hash_digest.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {
namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kHeaderBytes = 96;
constexpr std::size_t kDigestBytes = 32;

void Put16(Bytes* out, std::size_t at, std::uint16_t value) {
  (*out)[at] = static_cast<std::uint8_t>(value);
  (*out)[at + 1] = static_cast<std::uint8_t>(value >> 8);
}
void Put32(Bytes* out, std::size_t at, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) (*out)[at + shift / 8] = static_cast<std::uint8_t>(value >> shift);
}
void Put64(Bytes* out, std::size_t at, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) (*out)[at + shift / 8] = static_cast<std::uint8_t>(value >> shift);
}
std::uint16_t Get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1]) << 8; }
std::uint32_t Get32(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint64_t Get64(const std::uint8_t* p) { std::uint64_t value = 0; for (unsigned shift = 0; shift != 64; shift += 8) value |= static_cast<std::uint64_t>(p[shift / 8]) << shift; return value; }

bool IsZero(const std::uint8_t* p, std::size_t size) { return std::all_of(p, p + size, [](std::uint8_t byte) { return byte == 0; }); }
bool IsNonzero(const std::array<std::uint8_t, 16>& uuid) { return !IsZero(uuid.data(), uuid.size()); }
std::string Hex16(const std::array<std::uint8_t, 16>& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(32);
  for (const auto byte : value) {
    output.push_back(kHex[byte >> 4]);
    output.push_back(kHex[byte & 0x0f]);
  }
  return output;
}
bool HasForbiddenText(const std::string& value) {
  return value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c == 0 || c < 0x20 || c >= 0x80; });
}
bool IsLocalSelector(const std::string& selector) {
  if (HasForbiddenText(selector) || selector.size() > 256 || selector.rfind("sys.metrics.", 0) != 0) return false;
  if (selector.find("cluster.") != std::string::npos) return false;
  const auto wildcard = selector.find('*');
  return wildcard == std::string::npos || (wildcard + 1 == selector.size() && wildcard > 0 && selector[wildcard - 1] == '.');
}
bool IsQueryClass(std::uint8_t value) { return value >= 1 && value <= 4; }
std::array<std::uint8_t, 16> PolicyDigest(const std::string& selector, SblrLocalMetricsQueryClass query_class) {
  const std::string policy = "SBMR-POLICY-V1:" + std::to_string(static_cast<unsigned>(query_class)) + ":" + selector;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(reinterpret_cast<const std::uint8_t*>(policy.data()), policy.size());
  std::array<std::uint8_t, 16> output{};
  if (digest.ok()) std::copy_n(digest.digest.begin(), output.size(), output.begin());
  return output;
}
SblrLocalMetricsReadCodecResult Failure(std::string id, std::string detail) {
  SblrLocalMetricsReadCodecResult result; result.diagnostic_id = std::move(id); result.detail = std::move(detail); return result;
}
SblrLocalMetricsReadDispatchResult DispatchFailure(std::string id, std::string detail) {
  SblrLocalMetricsReadDispatchResult result; result.diagnostic_id = std::move(id); result.detail = std::move(detail); return result;
}
SblrLocalMetricsReadCodecResult ValidateRequest(const SblrLocalMetricsReadRequest& request) {
  if (!IsQueryClass(static_cast<std::uint8_t>(request.query_class)) || request.page_size == 0 || request.page_size > 1024 || !IsNonzero(request.request_uuid)) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "query_class_page_size_or_uuid");
  if (!IsLocalSelector(request.selector)) return Failure("OBSERVABILITY_METRICS.POLICY_REFUSED", "selector_not_local_or_canonical");
  const bool temporal = request.query_class == SblrLocalMetricsQueryClass::history || request.query_class == SblrLocalMetricsQueryClass::rollup;
  if (temporal ? (request.start_time_ns == 0 || request.end_time_ns < request.start_time_ns) : (request.start_time_ns != 0 || request.end_time_ns != 0)) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "time_range_for_query_class");
  if (!IsZero(request.cursor_digest.data(), request.cursor_digest.size())) return Failure("OBSERVABILITY_METRICS.POLICY_REFUSED", "cursor_not_issued_by_local_page");
  SblrLocalMetricsReadCodecResult result; result.ok = true; result.request = request; return result;
}

}  // namespace

bool IsSblrLocalMetricsReadOperation(std::string_view operation_id) noexcept { return operation_id == "engine.op.read_metrics"; }

SblrLocalMetricsReadCodecResult EncodeSblrLocalMetricsReadRequest(const SblrLocalMetricsReadRequest& request) {
  auto validated = ValidateRequest(request); if (!validated.ok) return validated;
  const std::size_t size = kHeaderBytes + request.selector.size() + kDigestBytes;
  if (size < 129 || size > kSblrLocalMetricsReadMaximumBytes) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "frame_size");
  Bytes frame(size, 0); frame[0] = 'S'; frame[1] = 'B'; frame[2] = 'M'; frame[3] = 'R'; Put16(&frame, 4, 1); Put16(&frame, 6, 0);
  frame[8] = static_cast<std::uint8_t>(request.query_class); frame[9] = 0; Put16(&frame, 10, kHeaderBytes); Put32(&frame, 12, static_cast<std::uint32_t>(size)); Put32(&frame, 16, request.page_size); Put32(&frame, 20, static_cast<std::uint32_t>(request.selector.size())); Put64(&frame, 24, request.start_time_ns); Put64(&frame, 32, request.end_time_ns); Put64(&frame, 40, request.registry_epoch);
  std::copy(request.request_uuid.begin(), request.request_uuid.end(), frame.begin() + 48); std::copy(request.cursor_digest.begin(), request.cursor_digest.end(), frame.begin() + 64);
  const auto policy = PolicyDigest(request.selector, request.query_class); std::copy(policy.begin(), policy.end(), frame.begin() + 80);
  std::copy(request.selector.begin(), request.selector.end(), frame.begin() + kHeaderBytes);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(frame.data(), size - kDigestBytes); if (!digest.ok()) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "sha256_unavailable");
  std::copy(digest.digest.begin(), digest.digest.end(), frame.begin() + size - kDigestBytes);
  validated.canonical_bytes = std::move(frame); validated.sha256_hex = scratchbird::core::hash::HexLower(digest.digest); return validated;
}

SblrLocalMetricsReadCodecResult DecodeSblrLocalMetricsReadRequest(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < 129 || size > kSblrLocalMetricsReadMaximumBytes) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "frame_size");
  if (data[0] != 'S' || data[1] != 'B' || data[2] != 'M' || data[3] != 'R' || Get16(data + 4) != 1 || Get16(data + 6) != 0 || data[9] != 0 || Get16(data + 10) != kHeaderBytes || Get32(data + 12) != size) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "frame_header");
  const std::uint32_t selector_bytes = Get32(data + 20); if (selector_bytes == 0 || selector_bytes > 256 || kHeaderBytes + selector_bytes + kDigestBytes != size) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "selector_length");
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(data, size - kDigestBytes); if (!digest.ok() || !std::equal(digest.digest.begin(), digest.digest.end(), data + size - kDigestBytes)) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "sha256_mismatch");
  SblrLocalMetricsReadRequest request; request.query_class = static_cast<SblrLocalMetricsQueryClass>(data[8]); request.page_size = Get32(data + 16); request.start_time_ns = Get64(data + 24); request.end_time_ns = Get64(data + 32); request.registry_epoch = Get64(data + 40); std::copy_n(data + 48, 16, request.request_uuid.begin()); std::copy_n(data + 64, 16, request.cursor_digest.begin()); request.selector.assign(reinterpret_cast<const char*>(data + kHeaderBytes), selector_bytes);
  auto validated = ValidateRequest(request); if (!validated.ok) return validated;
  const auto expected_policy = PolicyDigest(request.selector, request.query_class); if (!std::equal(expected_policy.begin(), expected_policy.end(), data + 80)) return Failure("OBSERVABILITY_METRICS.POLICY_REFUSED", "selector_policy_digest");
  validated.canonical_bytes.assign(data, data + size); validated.sha256_hex = scratchbird::core::hash::HexLower(digest.digest); return validated;
}

SblrLocalMetricsReadCodecResult DecodeSblrLocalMetricsReadOperand(const SblrOperationEnvelope& envelope) {
  if (!IsSblrLocalMetricsReadOperation(envelope.operation_id) || envelope.opcode != "SBLR_READ_METRICS" || envelope.opcode_code != 0x0c01 || envelope.operands.size() != 1) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "sbop_identity_or_operand_count");
  const auto& operand = envelope.operands.front(); if (operand.type != "metrics.read_request.v1" || operand.name != "request" || operand.ordinal != 1 || operand.value_kind != SblrValueKind::literal_typed || operand.value_body.size() < 24) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "sbop_operand_carrier");
  std::uint64_t bytes = 0; for (unsigned i = 0; i != 8; ++i) bytes |= static_cast<std::uint64_t>(operand.value_body[16 + i]) << (i * 8); if (bytes != operand.value_body.size() - 24) return Failure("OBSERVABILITY_METRICS.REQUEST_INVALID", "sbop_carrier_size");
  return DecodeSblrLocalMetricsReadRequest(operand.value_body.data() + 24, static_cast<std::size_t>(bytes));
}

SblrOperand MakeSblrLocalMetricsReadOperand(const SblrLocalMetricsReadCodecResult& encoded) {
  SblrOperand operand; if (!encoded.ok) return operand; operand.type = "metrics.read_request.v1"; operand.name = "request"; operand.ordinal = 1; operand.value_kind = SblrValueKind::literal_typed; operand.value_body.assign(24, 0); operand.value_body[0] = 1; const auto size = encoded.canonical_bytes.size(); for (unsigned i = 0; i != 8; ++i) operand.value_body[16 + i] = static_cast<std::uint8_t>(size >> (i * 8)); operand.value_body.insert(operand.value_body.end(), encoded.canonical_bytes.begin(), encoded.canonical_bytes.end()); return operand;
}

SblrLocalMetricsReadDispatchResult DispatchSblrLocalMetricsRead(const SblrOperationEnvelope& envelope, const scratchbird::engine::internal_api::EngineRequestContext& context) {
  const auto registry = ValidateSblrOpcodeForEnvelope(envelope); if (!registry.ok) return DispatchFailure(registry.diagnostic_id, registry.detail);
  const auto decoded = DecodeSblrLocalMetricsReadOperand(envelope); if (!decoded.ok) return DispatchFailure(decoded.diagnostic_id, decoded.detail);
  if (!context.security_context_present) return DispatchFailure("SB_DIAG_SBLR_SECURITY_CONTEXT_REQUIRED", "engine.op.read_metrics");
  if (context.query_cancellation_requested && context.query_cancellation_requested()) return DispatchFailure("PROCESS.CANCELLED", "cancelled_before_local_metrics_projection");
  SblrLocalMetricsReadDispatchResult result; result.accepted = true; result.request = decoded.request;
  result.evidence = {{"executor_id", "engine.op.read_metrics"}, {"opcode_code", "0x0C01"}, {"opcode_version", "1.0"}, {"request_uuid", Hex16(decoded.request.request_uuid)}, {"registry_epoch", std::to_string(decoded.request.registry_epoch)}, {"request_sha256", decoded.sha256_hex}, {"next_cursor_present", "false"}};
  return result;
}

}  // namespace scratchbird::engine::sblr
