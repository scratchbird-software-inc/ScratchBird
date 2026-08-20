// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_management_envelope.hpp"

#include "hash_digest.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace {

using Bytes = std::vector<std::uint8_t>;

struct FieldSpec {
  const char* name;
  bool required;
  bool uuid;
};

struct RecordSpec {
  SblrManagementEnvelopeKind kind;
  std::string_view operation;
  std::string_view opcode;
  std::uint16_t code;
  std::string_view slot;
  std::string_view type;
  std::vector<FieldSpec> fields;
};

const std::vector<RecordSpec>& Specs() {
  static const std::vector<RecordSpec> specs{
      {SblrManagementEnvelopeKind::operation, "engine.op.mgmt_operation", "SBLR_MGMT_OPERATION", 0x0d00, "operation", "management.operation.v1", {
          {"operation_uuid", true, true}, {"opcode", true, false}, {"opcode_version_major", true, false}, {"opcode_version_minor", true, false}, {"target_database_uuid", true, true}, {"target_filespace_uuid", false, true}, {"target_cluster_uuid", false, true}, {"target_node_uuid", false, true}, {"target_object_uuid", false, true}, {"security_context_uuid", true, true}, {"policy_snapshot_uuid", true, true}, {"policy_epoch", true, false}, {"request_source", true, false}, {"idempotency_key", true, false}, {"causal_transaction_id", false, false}, {"requested_snapshot", false, false}, {"wait_mode", true, false}, {"timeout_ms", true, false}, {"dry_run", true, false}, {"audit_reason", true, false}, {"diagnostic_locale", true, false}, {"disclosure_class", true, false}, {"operation_generation", true, false}}},
      {SblrManagementEnvelopeKind::payload, "engine.op.mgmt_payload", "SBLR_MGMT_PAYLOAD", 0x0d01, "payload", "management.payload.v1", {
          {"operation_uuid", true, true}, {"payload_schema_uuid", true, true}, {"payload_version_major", true, false}, {"payload_version_minor", true, false}, {"canonical_serialization_hash", true, false}, {"payload_body", true, false}}},
      {SblrManagementEnvelopeKind::result, "engine.op.mgmt_result", "SBLR_MGMT_RESULT", 0x0d02, "result", "management.result.v1", {
          {"operation_uuid", true, true}, {"terminal_state", true, false}, {"summary_code", true, false}, {"affected_object_counts", true, false}, {"durable_state_refs", true, false}, {"diagnostic_vector_uuid", true, true}, {"metric_snapshot_refs", true, false}, {"evidence_bundle_uuid", true, true}}},
      {SblrManagementEnvelopeKind::progress, "engine.op.mgmt_progress", "SBLR_MGMT_PROGRESS", 0x0d03, "progress", "management.progress.v1", {
          {"operation_uuid", true, true}, {"phase", true, false}, {"phase_generation", true, false}, {"total_work_estimate", true, false}, {"completed_work", true, false}, {"current_object_uuid", false, true}, {"retry_count", true, false}, {"last_safe_retry_point", true, false}}},
      {SblrManagementEnvelopeKind::diagnostic, "engine.op.mgmt_diagnostic", "SBLR_MGMT_DIAGNOSTIC", 0x0d04, "diagnostic", "management.diagnostic.v1", {
          {"operation_uuid", true, true}, {"diagnostic_code", true, false}, {"severity", true, false}, {"object_uuid", false, true}, {"search_key", true, false}, {"safe_human_text", true, false}, {"disclosure_class", true, false}, {"retry_class", true, false}}},
      {SblrManagementEnvelopeKind::metric_snapshot_ref, "engine.op.mgmt_metric_snapshot_ref", "SBLR_MGMT_METRIC_SNAPSHOT_REF", 0x0d05, "metric_snapshot_ref", "management.metric_snapshot_ref.v1", {
          {"operation_uuid", true, true}, {"metric_scope", true, false}, {"metric_path", true, false}, {"sample_window", true, false}, {"metric_row_refs", true, false}, {"classification", true, false}}}};
  return specs;
}

const RecordSpec* FindSpec(SblrManagementEnvelopeKind kind) {
  for (const auto& spec : Specs()) if (spec.kind == kind) return &spec;
  return nullptr;
}

const RecordSpec* FindSpec(std::string_view operation) {
  for (const auto& spec : Specs()) if (spec.operation == operation) return &spec;
  return nullptr;
}

void Append16(Bytes* out, std::uint16_t value) { out->push_back(value); out->push_back(value >> 8); }
void Append32(Bytes* out, std::uint32_t value) { for (unsigned s = 0; s != 32; s += 8) out->push_back(value >> s); }
std::uint16_t Load16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1]) << 8; }
std::uint32_t Load32(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
void Store32(Bytes* out, std::size_t offset, std::uint32_t value) { for (unsigned s = 0; s != 32; s += 8) (*out)[offset + s / 8] = value >> s; }

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit != 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & static_cast<std::uint32_t>(-(crc & 1u)));
  }
  return ~crc;
}

bool IsUuid(std::string_view v) {
  if (v.size() != 36 || v[8] != '-' || v[13] != '-' || v[18] != '-' || v[23] != '-') return false;
  bool nonzero = false;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    const char c = v[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    nonzero = nonzero || c != '0';
  }
  return nonzero;
}

bool IsText(const Bytes& v) {
  if (v.empty()) return false;
  for (auto c : v) if (c == 0 || c < 0x20) return false;
  return true;
}

bool ParseU64(std::string_view value, std::uint64_t* out) {
  if (value.empty() || value.size() > 20) return false;
  std::uint64_t parsed = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') return false;
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (parsed > (UINT64_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  *out = parsed;
  return true;
}

std::string FieldText(const SblrManagementEnvelopeRecord& record, std::string_view name) {
  for (const auto& field : record.fields) if (field.name == name) return {field.value.begin(), field.value.end()};
  return {};
}

SblrManagementEnvelopeCodecResult Failure(std::string id, std::string detail) {
  SblrManagementEnvelopeCodecResult result;
  result.diagnostic_id = std::move(id);
  result.detail = std::move(detail);
  return result;
}

SblrManagementEnvelopeCodecResult ValidateRecord(const SblrManagementEnvelopeRecord& record) {
  const auto* spec = FindSpec(record.kind);
  if (spec == nullptr) return Failure("MGA.CMO.ENVELOPE_INVALID", "record_kind_unknown");
  if (record.fields.size() > kManagementEnvelopeMaximumFields) return Failure("MGA.CMO.ENVELOPE_INVALID", "field_count_limit");
  if (record.fields.size() != spec->fields.size()) return Failure("MGA.CMO.ENVELOPE_INVALID", "field_count_not_exact");
  for (std::size_t i = 0; i < spec->fields.size(); ++i) {
    const auto& expected = spec->fields[i]; const auto& actual = record.fields[i];
    if (actual.name != expected.name) return Failure("MGA.CMO.ENVELOPE_INVALID", "field_order_or_name:" + actual.name);
    if (actual.value.size() > kManagementEnvelopeMaximumFieldBytes) return Failure("MGA.CMO.ENVELOPE_INVALID", "field_limit:" + actual.name);
    if (actual.value.empty() && expected.required) return Failure(record.kind == SblrManagementEnvelopeKind::payload ? "MGA.CMO.PAYLOAD_INVALID" : "MGA.CMO.ENVELOPE_INVALID", "required_field_empty:" + actual.name);
    if (!actual.value.empty() && actual.name != "payload_body" && !IsText(actual.value)) return Failure("MGA.CMO.ENVELOPE_INVALID", "text_invalid:" + actual.name);
    if (!actual.value.empty() && expected.uuid && !IsUuid(FieldText(record, actual.name))) return Failure("MGA.CMO.ENVELOPE_INVALID", "uuid_invalid:" + actual.name);
  }
  if (record.kind == SblrManagementEnvelopeKind::payload) {
    const auto body = FieldText(record, "payload_body");
    const auto hash = scratchbird::core::hash::ComputeSha256Digest(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    if (!hash.ok() || FieldText(record, "canonical_serialization_hash") != scratchbird::core::hash::HexLower(hash.digest)) return Failure("MGA.CMO.PAYLOAD_INVALID", "payload_body_sha256_mismatch");
  }
  SblrManagementEnvelopeCodecResult result; result.ok = true; result.record = record; return result;
}

struct OperationState { std::string operation_digest; std::string payload_digest; bool terminal = false; std::uint64_t generation = 0; std::uint64_t progress_work = 0; };
std::mutex& StateMutex() { static std::mutex mutex; return mutex; }
std::unordered_map<std::string, OperationState>& States() { static std::unordered_map<std::string, OperationState> states; return states; }

SblrManagementEnvelopeDispatchResult DispatchFailure(std::string id, std::string detail) {
  SblrManagementEnvelopeDispatchResult result; result.diagnostic_id = std::move(id); result.detail = std::move(detail); return result;
}

}  // namespace

bool IsManagementEnvelopeOperation(std::string_view operation_id) noexcept { return FindSpec(operation_id) != nullptr; }
std::string_view ManagementEnvelopeOperationId(SblrManagementEnvelopeKind kind) noexcept { const auto* spec = FindSpec(kind); return spec ? spec->operation : std::string_view{}; }
std::string_view ManagementEnvelopeOpcode(SblrManagementEnvelopeKind kind) noexcept { const auto* spec = FindSpec(kind); return spec ? spec->opcode : std::string_view{}; }
std::uint16_t ManagementEnvelopeOpcodeCode(SblrManagementEnvelopeKind kind) noexcept { const auto* spec = FindSpec(kind); return spec ? spec->code : 0; }

SblrManagementEnvelopeCodecResult EncodeSblrManagementEnvelopeRecord(const SblrManagementEnvelopeRecord& record) {
  auto validated = ValidateRecord(record); if (!validated.ok) return validated;
  Bytes out(16, 0); out[0] = 'S'; out[1] = 'B'; out[2] = 'M'; out[3] = 'G';
  out[4] = 1; out[6] = 0; out[8] = static_cast<std::uint8_t>(record.kind); out[10] = static_cast<std::uint8_t>(record.fields.size());
  for (std::size_t i = 0; i < record.fields.size(); ++i) { Append16(&out, static_cast<std::uint16_t>(i + 1)); Append16(&out, 0); Append32(&out, static_cast<std::uint32_t>(record.fields[i].value.size())); out.insert(out.end(), record.fields[i].value.begin(), record.fields[i].value.end()); }
  Store32(&out, 12, static_cast<std::uint32_t>(out.size() - 16));
  if (out.size() + 36 > kManagementEnvelopeMaximumBytes) return Failure("MGA.CMO.ENVELOPE_INVALID", "frame_limit");
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(out);
  if (!digest.ok()) return Failure("MGA.CMO.ENVELOPE_INVALID", "sha256_unavailable");
  out.insert(out.end(), digest.digest.begin(), digest.digest.end()); Append32(&out, Crc32c(out.data(), out.size()));
  validated.canonical_bytes = std::move(out); validated.sha256_hex = scratchbird::core::hash::HexLower(digest.digest); return validated;
}

SblrManagementEnvelopeCodecResult DecodeSblrManagementEnvelopeRecord(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < 52 || size > kManagementEnvelopeMaximumBytes) return Failure("MGA.CMO.ENVELOPE_INVALID", "frame_size");
  if (data[0] != 'S' || data[1] != 'B' || data[2] != 'M' || data[3] != 'G' || Load16(data + 4) != 1 || Load16(data + 6) != 0) return Failure("MGA.CMO.ENVELOPE_INVALID", "magic_or_version");
  const auto* spec = FindSpec(static_cast<SblrManagementEnvelopeKind>(Load16(data + 8)));
  const std::uint16_t count = Load16(data + 10); const std::uint32_t area = Load32(data + 12);
  if (spec == nullptr || count != spec->fields.size() || area + 16 + 36 != size) return Failure("MGA.CMO.ENVELOPE_INVALID", "kind_or_area");
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(data, size - 36);
  if (!digest.ok() || !std::equal(digest.digest.begin(), digest.digest.end(), data + size - 36)) return Failure("MGA.CMO.ENVELOPE_INVALID", "sha256_mismatch");
  if (Crc32c(data, size - 4) != Load32(data + size - 4)) return Failure("MGA.CMO.ENVELOPE_INVALID", "crc32c_mismatch");
  SblrManagementEnvelopeRecord record; record.kind = spec->kind; std::size_t offset = 16;
  for (std::size_t i = 0; i < count; ++i) {
    if (offset + 8 > size - 36 || Load16(data + offset) != i + 1 || Load16(data + offset + 2) != 0) return Failure("MGA.CMO.ENVELOPE_INVALID", "field_order_or_reserved");
    const auto bytes = Load32(data + offset + 4); offset += 8;
    if (bytes > kManagementEnvelopeMaximumFieldBytes || offset + bytes > size - 36) return Failure("MGA.CMO.ENVELOPE_INVALID", "field_size");
    record.fields.push_back({spec->fields[i].name, Bytes(data + offset, data + offset + bytes)}); offset += bytes;
  }
  if (offset != size - 36) return Failure("MGA.CMO.ENVELOPE_INVALID", "field_area_mismatch");
  auto validated = ValidateRecord(record); if (!validated.ok) return validated;
  validated.canonical_bytes.assign(data, data + size); validated.sha256_hex = scratchbird::core::hash::HexLower(digest.digest); return validated;
}

SblrManagementEnvelopeCodecResult DecodeSblrManagementEnvelopeOperand(const SblrOperationEnvelope& envelope) {
  const auto* spec = FindSpec(envelope.operation_id);
  if (spec == nullptr || envelope.opcode != spec->opcode || envelope.opcode_code != spec->code || envelope.operands.size() != 1) return Failure("MGA.CMO.ENVELOPE_INVALID", "sbop_identity_or_operand_count");
  const auto& operand = envelope.operands.front();
  if (operand.type != spec->type || operand.name != spec->slot || operand.ordinal != 1 || operand.value_kind != SblrValueKind::literal_typed || operand.value_body.size() < 24) return Failure("MGA.CMO.ENVELOPE_INVALID", "sbop_operand_carrier");
  std::uint64_t count = 0; for (unsigned i = 0; i != 8; ++i) count |= static_cast<std::uint64_t>(operand.value_body[16 + i]) << (i * 8);
  if (count != operand.value_body.size() - 24) return Failure("MGA.CMO.ENVELOPE_INVALID", "sbop_carrier_size");
  auto decoded = DecodeSblrManagementEnvelopeRecord(operand.value_body.data() + 24, static_cast<std::size_t>(count));
  if (decoded.ok && decoded.record.kind != spec->kind) return Failure("MGA.CMO.ENVELOPE_INVALID", "sbop_record_kind");
  return decoded;
}

SblrOperand MakeSblrManagementEnvelopeOperand(const SblrManagementEnvelopeCodecResult& encoded) {
  SblrOperand operand; if (!encoded.ok) return operand; const auto* spec = FindSpec(encoded.record.kind); if (!spec) return operand;
  operand.type = std::string(spec->type); operand.name = std::string(spec->slot); operand.ordinal = 1; operand.value_kind = SblrValueKind::literal_typed; operand.value_body.assign(24, 0); operand.value_body[0] = static_cast<std::uint8_t>(spec->kind);
  const auto n = encoded.canonical_bytes.size(); for (unsigned i = 0; i != 8; ++i) operand.value_body[16 + i] = static_cast<std::uint8_t>(n >> (i * 8)); operand.value_body.insert(operand.value_body.end(), encoded.canonical_bytes.begin(), encoded.canonical_bytes.end()); return operand;
}

SblrManagementEnvelopeDispatchResult DispatchSblrManagementEnvelope(const SblrOperationEnvelope& envelope, const scratchbird::engine::internal_api::EngineRequestContext& context) {
  if (!context.security_context_present) return DispatchFailure("MGA.CMO.AUTHORIZATION_DENIED", "security_context_absent");
  if (context.query_cancellation_requested && context.query_cancellation_requested()) return DispatchFailure("PROCESS.CANCELLED", "cancelled_before_management_envelope_publication");
  const auto registry = ValidateSblrOpcodeForEnvelope(envelope);
  if (!registry.ok) return DispatchFailure(registry.diagnostic_id, registry.detail);
  const auto decoded = DecodeSblrManagementEnvelopeOperand(envelope); if (!decoded.ok) return DispatchFailure(decoded.diagnostic_id, decoded.detail);
  const auto operation_uuid = FieldText(decoded.record, "operation_uuid");
  std::lock_guard lock(StateMutex()); auto& states = States(); const auto kind = decoded.record.kind;
  if (kind == SblrManagementEnvelopeKind::operation) {
    const auto existing = states.find(operation_uuid);
    if (existing != states.end()) { if (existing->second.operation_digest != decoded.sha256_hex) return DispatchFailure("MGA.CMO.IDEMPOTENT_PAYLOAD_MISMATCH", "operation_uuid_replay_digest_mismatch"); }
    else states.emplace(operation_uuid, OperationState{decoded.sha256_hex, {}, false, 1, 0});
  } else {
    auto found = states.find(operation_uuid); if (found == states.end()) return DispatchFailure("MGA.CMO.ENVELOPE_INVALID", "operation_not_admitted");
    auto& state = found->second; if (state.terminal) return DispatchFailure("MGA.CMO.ENVELOPE_INVALID", "publication_after_terminal");
    if (kind == SblrManagementEnvelopeKind::payload) { if (!state.payload_digest.empty() && state.payload_digest != decoded.sha256_hex) return DispatchFailure("MGA.CMO.IDEMPOTENT_PAYLOAD_MISMATCH", "payload_republication_mismatch"); state.payload_digest = decoded.sha256_hex; }
    if (kind == SblrManagementEnvelopeKind::progress) {
      std::uint64_t completed = 0;
      if (!ParseU64(FieldText(decoded.record, "completed_work"), &completed) ||
          completed < state.progress_work) {
        return DispatchFailure("MGA.CMO.ENVELOPE_INVALID",
                               "progress_not_monotonic");
      }
      state.progress_work = completed;
    }
    if (kind == SblrManagementEnvelopeKind::metric_snapshot_ref) { const auto path = FieldText(decoded.record, "metric_path"); const auto scope = FieldText(decoded.record, "metric_scope"); if ((scope == "cluster" && (!context.cluster_authority_available || !path.starts_with("cluster.sys.metrics."))) || (scope == "local" && !path.starts_with("sys.metrics."))) return DispatchFailure("MGA.CMO.METRIC_SCOPE_INVALID", "metric_scope_path"); }
    if (kind == SblrManagementEnvelopeKind::result) { const auto terminal = FieldText(decoded.record, "terminal_state"); if (terminal != "completed" && terminal != "failed_non_destructive" && terminal != "refused_by_policy") return DispatchFailure("MGA.CMO.ENVELOPE_INVALID", "result_not_terminal"); state.terminal = true; }
    ++state.generation;
  }
  SblrManagementEnvelopeDispatchResult result; result.accepted = true;
  result.evidence.push_back({"management-envelope", "executor_id=" + registry.entry->executor_id + ";opcode_code=" + std::to_string(envelope.opcode_code) + ";opcode_version=1.0;record_identity=" + std::string(ManagementEnvelopeOperationId(decoded.record.kind)) + ";frame_sha256=" + decoded.sha256_hex + ";operation_uuid=" + operation_uuid});
  return result;
}

}  // namespace scratchbird::engine::sblr
