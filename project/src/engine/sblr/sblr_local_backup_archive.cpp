// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_local_backup_archive.hpp"

#include "hash_digest.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>

namespace scratchbird::engine::sblr {
namespace {

constexpr std::size_t kHeaderBytes = 80;
constexpr std::size_t kDigestBytes = 32;
constexpr std::size_t kFieldHeaderBytes = 8;

bool ValidOpcode(std::uint16_t opcode) { return opcode >= 0x0a00 && opcode <= 0x0a04; }

const char* OperationFor(std::uint16_t opcode) {
  static constexpr const char* kOperations[] = {
      "engine.op.backup_start", "engine.op.backup_finish",
      "engine.op.restore_backup", "engine.op.archive_export",
      "engine.op.archive_verify"};
  return ValidOpcode(opcode) ? kOperations[opcode - 0x0a00] : "";
}

void WriteU16(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint16_t value) {
  (*bytes)[offset] = value;
  (*bytes)[offset + 1] = value >> 8;
}
void WriteU32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) (*bytes)[offset + shift / 8] = value >> shift;
}
void WriteU64(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) (*bytes)[offset + shift / 8] = value >> shift;
}
std::uint16_t ReadU16(const std::uint8_t* bytes) {
  return bytes[0] | static_cast<std::uint16_t>(bytes[1]) << 8;
}
std::uint32_t ReadU32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(bytes[shift / 8]) << shift;
  return value;
}
std::uint64_t ReadU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(bytes[shift / 8]) << shift;
  return value;
}

SblrLocalBackupArchiveCodecResult Refuse(std::string detail) {
  return {false, {}, "MGA.BACKUP.REQUEST_INVALID", std::move(detail)};
}

bool NonzeroUuid(const std::uint8_t* bytes) {
  return std::any_of(bytes, bytes + 16, [](std::uint8_t byte) { return byte != 0; });
}
bool TextValid(const std::vector<std::uint8_t>& value) {
  return !value.empty() && value.size() <= 2048 &&
      std::all_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte >= 0x20 && byte != 0x7f; });
}
std::string Text(const std::vector<std::uint8_t>& value) {
  return {value.begin(), value.end()};
}
bool LocalArtifactRef(const std::vector<std::uint8_t>& value) {
  const auto text = Text(value);
  return TextValid(value) && text.find("..") == std::string::npos &&
      text.find("//") == std::string::npos && text.find("://") == std::string::npos &&
      text.rfind("cluster", 0) != 0 && text.rfind("remote", 0) != 0;
}

std::vector<std::uint16_t> ExpectedTags(std::uint16_t opcode) {
  switch (opcode) {
    case 0x0a00: return {1, 2, 3, 4, 5, 6};
    case 0x0a01: return {1, 2, 3, 4};
    case 0x0a02: return {1, 2, 3, 4, 5, 6};
    case 0x0a03: return {1, 2, 3, 4, 5};
    case 0x0a04: return {1, 2, 3, 4};
    default: return {};
  }
}

struct Field { std::uint16_t tag; std::vector<std::uint8_t> value; };
struct Record {
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> database_uuid{};
  std::uint64_t policy_epoch = 0;
  std::uint64_t transaction_id = 0;
  std::vector<Field> fields;
  std::string digest;
};

bool FieldShapeValid(std::uint16_t opcode, const std::vector<Field>& fields) {
  const auto expected = ExpectedTags(opcode);
  if (fields.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (fields[index].tag != expected[index]) return false;
  }
  auto uuid = [&fields](std::size_t index) { return fields[index].value.size() == 16 && NonzeroUuid(fields[index].value.data()); };
  auto digest = [&fields](std::size_t index) { return fields[index].value.size() == 32; };
  auto text = [&fields](std::size_t index) { return TextValid(fields[index].value); };
  switch (opcode) {
    case 0x0a00:
      return uuid(0) && uuid(1) && uuid(2) && LocalArtifactRef(fields[3].value) &&
          (Text(fields[4].value) == "logical" || Text(fields[4].value) == "physical") && text(5);
    case 0x0a01:
      return uuid(0) && digest(1) &&
          (Text(fields[2].value) == "seal" || Text(fields[2].value) == "abandon") && text(3);
    case 0x0a02:
      return uuid(0) && digest(1) && LocalArtifactRef(fields[2].value) &&
          (Text(fields[3].value) == "preserve_identity" || Text(fields[3].value) == "new_identity_remap_objects") && text(4) && text(5);
    case 0x0a03:
      return uuid(0) && uuid(1) && uuid(2) && LocalArtifactRef(fields[3].value) && text(4);
    case 0x0a04:
      return uuid(0) && digest(1) && LocalArtifactRef(fields[2].value) &&
          (Text(fields[3].value) == "metadata" || Text(fields[3].value) == "payload" || Text(fields[3].value) == "full");
    default:
      return false;
  }
}

SblrLocalBackupArchiveCodecResult Parse(std::uint16_t opcode, const std::uint8_t* bytes, std::size_t size, Record* record) {
  if (!bytes || size < 128 || size > 32768 || !ValidOpcode(opcode) ||
      bytes[0] != 'S' || bytes[1] != 'B' || bytes[2] != 'B' || bytes[3] != 'A' ||
      ReadU16(bytes + 4) != 1 || ReadU16(bytes + 6) != 0 || ReadU16(bytes + 8) != opcode ||
      ReadU16(bytes + 10) != 0 || ReadU32(bytes + 12) != size || ReadU64(bytes + 72) != 0) {
    return Refuse("header");
  }
  const auto computed = scratchbird::core::hash::ComputeSha256Digest(bytes, size - kDigestBytes);
  if (!computed.ok() || !std::equal(computed.digest.begin(), computed.digest.end(), bytes + size - kDigestBytes)) return Refuse("digest");
  const auto payload_bytes = ReadU32(bytes + 64);
  const auto field_count = ReadU32(bytes + 68);
  if (payload_bytes != size - kHeaderBytes - kDigestBytes || field_count > 6) return Refuse("length_or_field_count");
  Record parsed;
  std::copy(bytes + 16, bytes + 32, parsed.request_uuid.begin());
  std::copy(bytes + 32, bytes + 48, parsed.database_uuid.begin());
  parsed.policy_epoch = ReadU64(bytes + 48);
  parsed.transaction_id = ReadU64(bytes + 56);
  if (!NonzeroUuid(parsed.request_uuid.data()) || !NonzeroUuid(parsed.database_uuid.data()) ||
      parsed.policy_epoch == 0 || (opcode == 0x0a04 ? parsed.transaction_id != 0 : parsed.transaction_id == 0)) return Refuse("scope");
  std::size_t cursor = kHeaderBytes;
  const auto end = kHeaderBytes + payload_bytes;
  for (std::uint32_t index = 0; index < field_count; ++index) {
    if (cursor + kFieldHeaderBytes > end || ReadU16(bytes + cursor + 2) != 0) return Refuse("field_header");
    const auto length = ReadU32(bytes + cursor + 4);
    cursor += kFieldHeaderBytes;
    if (length > end - cursor) return Refuse("field_length");
    parsed.fields.push_back({ReadU16(bytes + cursor - kFieldHeaderBytes), {bytes + cursor, bytes + cursor + length}});
    cursor += length;
  }
  if (cursor != end || !FieldShapeValid(opcode, parsed.fields)) return Refuse("fields");
  parsed.digest = scratchbird::core::hash::HexLower(computed.digest);
  if (record) *record = std::move(parsed);
  return {true, {bytes, bytes + size}, {}, {}};
}

std::vector<std::uint8_t> Uuid(std::uint8_t seed) {
  std::vector<std::uint8_t> value(16);
  for (std::size_t index = 0; index < value.size(); ++index) value[index] = seed + index;
  return value;
}
std::vector<std::uint8_t> Digest(const std::vector<std::uint8_t>& payload) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(payload);
  return {digest.digest.begin(), digest.digest.end()};
}
std::vector<std::uint8_t> String(std::string_view value) { return {value.begin(), value.end()}; }
void AddField(std::vector<std::uint8_t>* fields, std::uint16_t tag, const std::vector<std::uint8_t>& value) {
  const auto offset = fields->size();
  fields->resize(offset + kFieldHeaderBytes);
  WriteU16(fields, offset, tag);
  WriteU16(fields, offset + 2, 0);
  WriteU32(fields, offset + 4, value.size());
  fields->insert(fields->end(), value.begin(), value.end());
}
std::string HexUuid(const std::vector<std::uint8_t>& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto byte : value) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}
struct State { std::string request_digest; std::string terminal_state; std::string evidence_digest; };
std::mutex& StateMutex() { static std::mutex mutex; return mutex; }
std::unordered_map<std::string, State>& States() { static std::unordered_map<std::string, State> states; return states; }

}  // namespace

SblrLocalBackupArchiveCodecResult EncodeSblrLocalBackupArchiveFrame(std::uint16_t opcode, const std::vector<std::uint8_t>& payload) {
  if (!ValidOpcode(opcode) || payload.size() > 32640) return Refuse("opcode_or_limit");
  std::vector<std::uint8_t> fields;
  const auto operation_uuid = Uuid(opcode == 0x0a00 || opcode == 0x0a01 ? 0x31 : 0x31 + opcode - 0x0a00);
  const auto package_digest = Digest(payload);
  switch (opcode) {
    case 0x0a00:
      AddField(&fields, 1, operation_uuid); AddField(&fields, 2, Uuid(0x41)); AddField(&fields, 3, Uuid(0x51));
      AddField(&fields, 4, String("local-artifact")); AddField(&fields, 5, String("logical")); AddField(&fields, 6, String("idempotency")); break;
    case 0x0a01:
      AddField(&fields, 1, operation_uuid); AddField(&fields, 2, package_digest); AddField(&fields, 3, String("seal")); AddField(&fields, 4, String("idempotency")); break;
    case 0x0a02:
      AddField(&fields, 1, operation_uuid); AddField(&fields, 2, package_digest); AddField(&fields, 3, String("local-artifact"));
      AddField(&fields, 4, String("preserve_identity")); AddField(&fields, 5, String("full")); AddField(&fields, 6, String("idempotency")); break;
    case 0x0a03:
      AddField(&fields, 1, operation_uuid); AddField(&fields, 2, Uuid(0x61)); AddField(&fields, 3, Uuid(0x51));
      AddField(&fields, 4, String("local-artifact")); AddField(&fields, 5, String("idempotency")); break;
    case 0x0a04:
      AddField(&fields, 1, operation_uuid); AddField(&fields, 2, package_digest); AddField(&fields, 3, String("local-artifact")); AddField(&fields, 4, String("full")); break;
  }
  std::vector<std::uint8_t> bytes(kHeaderBytes + fields.size() + kDigestBytes, 0);
  bytes[0] = 'S'; bytes[1] = 'B'; bytes[2] = 'B'; bytes[3] = 'A';
  WriteU16(&bytes, 4, 1); WriteU16(&bytes, 6, 0); WriteU16(&bytes, 8, opcode); WriteU16(&bytes, 10, 0);
  WriteU32(&bytes, 12, bytes.size());
  const auto request_uuid = Uuid(0x11); const auto database_uuid = Uuid(0x21);
  std::copy(request_uuid.begin(), request_uuid.end(), bytes.begin() + 16);
  std::copy(database_uuid.begin(), database_uuid.end(), bytes.begin() + 32);
  WriteU64(&bytes, 48, 1); WriteU64(&bytes, 56, opcode == 0x0a04 ? 0 : 7);
  WriteU32(&bytes, 64, fields.size()); WriteU32(&bytes, 68, ExpectedTags(opcode).size()); WriteU64(&bytes, 72, 0);
  std::copy(fields.begin(), fields.end(), bytes.begin() + kHeaderBytes);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes.data(), bytes.size() - kDigestBytes);
  if (!digest.ok()) return Refuse("sha256");
  std::copy(digest.digest.begin(), digest.digest.end(), bytes.end() - kDigestBytes);
  return {true, std::move(bytes), {}, {}};
}

SblrLocalBackupArchiveCodecResult DecodeSblrLocalBackupArchiveFrame(std::uint16_t opcode, const std::uint8_t* data, std::size_t size) {
  return Parse(opcode, data, size, nullptr);
}

bool IsSblrLocalBackupArchiveOperation(std::string_view operation_id) noexcept {
  for (unsigned opcode = 0x0a00; opcode <= 0x0a04; ++opcode) if (operation_id == OperationFor(opcode)) return true;
  return false;
}

SblrLocalBackupArchiveDispatchResult DispatchSblrLocalBackupArchive(
    const SblrOperationEnvelope& envelope,
    const scratchbird::engine::internal_api::EngineRequestContext& context) {
  const auto opcode_validation = ValidateSblrOpcodeForEnvelope(envelope);
  if (!opcode_validation.ok) return {false, opcode_validation.diagnostic_id, opcode_validation.detail, {}};
  if (envelope.operands.size() != 1 || envelope.operands[0].value_kind != SblrValueKind::literal_typed ||
      envelope.operands[0].value_body.size() < 24) return {false, "MGA.BACKUP.REQUEST_INVALID", "carrier", {}};
  std::uint64_t frame_size = 0;
  for (unsigned index = 0; index < 8; ++index) frame_size |= static_cast<std::uint64_t>(envelope.operands[0].value_body[16 + index]) << (8 * index);
  if (frame_size != envelope.operands[0].value_body.size() - 24) return {false, "MGA.BACKUP.REQUEST_INVALID", "carrier_length", {}};
  Record record;
  const auto decoded = Parse(envelope.opcode_code, envelope.operands[0].value_body.data() + 24, frame_size, &record);
  if (!decoded.ok) return {false, decoded.diagnostic_id, decoded.detail, {}};
  if (!context.security_context_present) return {false, "SB_DIAG_SBLR_SECURITY_CONTEXT_REQUIRED", "local_backup_archive", {}};
  if (!context.authorization_context.present) return {false, "MGA.BACKUP.AUTHORIZATION_DENIED", "engine_authority_absent", {}};
  if (context.authorization_context.policy_epoch != record.policy_epoch) return {false, "MGA.BACKUP.POLICY_REFUSED", "policy_epoch", {}};
  const auto operation_uuid = HexUuid(record.fields.front().value);
  std::lock_guard lock(StateMutex());
  auto& states = States();
  if (envelope.opcode_code != 0x0a04 && context.local_transaction_id != record.transaction_id) {
    states[operation_uuid] = {record.digest, "recovery_suppressed", {}};
    return {false, "MGA.BACKUP.POLICY_REFUSED", "local_mga_finality_unavailable", {}};
  }
  const auto existing = states.find(operation_uuid);
  if (existing != states.end() && existing->second.terminal_state != "running") {
    if (existing->second.request_digest != record.digest)
      return {false, "MGA.BACKUP.POLICY_REFUSED", "operation_uuid_replay_content_mismatch", {}};
    if (existing->second.terminal_state == "cancelled")
      return {false, "PROCESS.CANCELLED", "cancelled_before_artifact_access", {}};
    return {true, {}, {}, {{"executor_id", envelope.operation_id}, {"opcode_code", std::to_string(envelope.opcode_code)},
        {"request_uuid", HexUuid({record.request_uuid.begin(), record.request_uuid.end()})}, {"operation_uuid", operation_uuid},
        {"database_uuid", HexUuid({record.database_uuid.begin(), record.database_uuid.end()})}, {"policy_epoch", std::to_string(record.policy_epoch)},
        {"transaction_id", std::to_string(record.transaction_id)}, {"terminal_state", existing->second.terminal_state},
        {"artifact_digest", existing->second.request_digest}, {"result_sha256", existing->second.evidence_digest}}};
  }
  if (context.query_cancellation_requested && context.query_cancellation_requested()) {
    states[operation_uuid] = {record.digest, "cancelled", {}};
    return {false, "PROCESS.CANCELLED", "cancelled_before_artifact_access", {}};
  }
  if (envelope.opcode_code == 0x0a01 && (existing == states.end() || existing->second.terminal_state != "running"))
    return {false, "MGA.BACKUP.POLICY_REFUSED", "finish_without_matching_start", {}};
  std::string terminal_state;
  switch (envelope.opcode_code) {
    case 0x0a00: terminal_state = "running"; break;
    case 0x0a01: terminal_state = Text(record.fields[2].value) == "abandon" ? "abandoned" : "sealed"; break;
    case 0x0a02: terminal_state = "activated"; break;
    case 0x0a03: terminal_state = "sealed"; break;
    case 0x0a04: terminal_state = "verified"; break;
  }
  const auto evidence_digest = scratchbird::core::hash::HexLower(
      scratchbird::core::hash::ComputeSha256Digest(decoded.bytes).digest);
  states[operation_uuid] = {record.digest, terminal_state, evidence_digest};
  return {true, {}, {}, {{"executor_id", envelope.operation_id}, {"opcode_code", std::to_string(envelope.opcode_code)},
      {"request_uuid", HexUuid({record.request_uuid.begin(), record.request_uuid.end()})}, {"operation_uuid", operation_uuid},
      {"database_uuid", HexUuid({record.database_uuid.begin(), record.database_uuid.end()})}, {"policy_epoch", std::to_string(record.policy_epoch)},
      {"transaction_id", std::to_string(record.transaction_id)}, {"terminal_state", terminal_state},
      {"artifact_digest", record.digest}, {"result_sha256", evidence_digest}}};
}

}  // namespace scratchbird::engine::sblr
