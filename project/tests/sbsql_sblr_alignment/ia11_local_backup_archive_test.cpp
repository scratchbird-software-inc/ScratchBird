#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_local_backup_archive.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <iostream>

namespace {
using namespace scratchbird::engine::sblr;

const char* Operation(std::uint16_t opcode) {
  static constexpr const char* kOperations[] = {
      "engine.op.backup_start", "engine.op.backup_finish", "engine.op.restore_backup",
      "engine.op.archive_export", "engine.op.archive_verify"};
  return kOperations[opcode - 0x0a00];
}
const char* OpcodeName(std::uint16_t opcode) {
  static constexpr const char* kNames[] = {
      "SBLR_BACKUP_START", "SBLR_BACKUP_FINISH", "SBLR_RESTORE_BACKUP",
      "SBLR_ARCHIVE_EXPORT", "SBLR_ARCHIVE_VERIFY"};
  return kNames[opcode - 0x0a00];
}
const char* OperandType(std::uint16_t opcode) {
  static constexpr const char* kTypes[] = {
      "backup.start.v1", "backup.finish.v1", "backup.restore.v1",
      "archive.export.v1", "archive.verify.v1"};
  return kTypes[opcode - 0x0a00];
}

SblrDispatchRequest RequestFor(std::uint16_t opcode, const std::vector<std::uint8_t>& frame) {
  auto envelope = MakeSblrEnvelope(Operation(opcode), OpcodeName(opcode), "ia11");
  envelope.opcode_code = opcode;
  envelope.requires_transaction_context = opcode != 0x0a04;
  envelope.parser_package_uuid = "11111111-1111-1111-1111-111111111111";
  envelope.registry_snapshot_uuid = "22222222-2222-2222-2222-222222222222";
  SblrOperand operand;
  operand.type = OperandType(opcode);
  operand.name = "request";
  operand.ordinal = 1;
  operand.value_kind = SblrValueKind::literal_typed;
  operand.value_body.assign(24, 0);
  operand.value_body[0] = 1;
  for (unsigned index = 0; index < 8; ++index) operand.value_body[16 + index] = frame.size() >> (8 * index);
  operand.value_body.insert(operand.value_body.end(), frame.begin(), frame.end());
  envelope.operands = {operand};
  SblrDispatchRequest request;
  request.envelope = std::move(envelope);
  request.context.security_context_present = true;
  request.context.local_transaction_id = opcode == 0x0a04 ? 0 : 7;
  request.context.authorization_context.present = true;
  request.context.authorization_context.policy_epoch = 1;
  return request;
}

bool RejectedAs(const SblrDispatchResult& result, std::string_view id) {
  return !result.accepted && !result.diagnostics.empty() && result.diagnostics.front().code == id;
}
void PrintDiagnostics(const SblrDispatchResult& result) {
  std::cerr << "accepted=" << result.accepted;
  for (const auto& diagnostic : result.diagnostics) std::cerr << ' ' << diagnostic.code << ':' << diagnostic.message;
  std::cerr << '\n';
}
bool SameEvidence(const std::vector<scratchbird::engine::internal_api::EngineEvidenceReference>& left,
                  const std::vector<scratchbird::engine::internal_api::EngineEvidenceReference>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].evidence_kind != right[index].evidence_kind || left[index].evidence_id != right[index].evidence_id) return false;
  }
  return true;
}
}  // namespace

int main() {
  using namespace scratchbird::engine::sblr;
  const std::vector<std::uint8_t> payload(32, 1);

  // CSC-TEST-003342: all five typed frames enforce their exact digest.
  for (unsigned opcode = 0x0a00; opcode <= 0x0a04; ++opcode) {
    auto encoded = EncodeSblrLocalBackupArchiveFrame(opcode, payload);
    if (!encoded.ok || !DecodeSblrLocalBackupArchiveFrame(opcode, encoded.bytes.data(), encoded.bytes.size()).ok) return 1;
    const auto* row = LookupSblrOpcodeCode(opcode);
    if (!row || !row->executor_evidence_required || !row->executor_evidence_accepted) return 2;
    encoded.bytes.back() ^= 1;
    if (DecodeSblrLocalBackupArchiveFrame(opcode, encoded.bytes.data(), encoded.bytes.size()).ok) return 3;
  }

  // CSC-TEST-003343: local-only executor requires engine authority and exact epoch.
  auto start_frame = EncodeSblrLocalBackupArchiveFrame(0x0a00, payload);
  auto start = RequestFor(0x0a00, start_frame.bytes);
  auto no_authority = start;
  no_authority.context.authorization_context.present = false;
  const auto no_authority_result = DispatchSblrOperation(no_authority);
  if (!RejectedAs(no_authority_result, "MGA.BACKUP.AUTHORIZATION_DENIED")) { PrintDiagnostics(no_authority_result); return 4; }
  auto wrong_epoch = start;
  wrong_epoch.context.authorization_context.policy_epoch = 2;
  if (!RejectedAs(DispatchSblrOperation(wrong_epoch), "MGA.BACKUP.POLICY_REFUSED")) return 5;

  // CSC-TEST-003345: start/finish seal and exact finish replay publish immutable evidence.
  const auto started = DispatchSblrOperation(start);
  if (!started.accepted || started.api_result.evidence.size() != 10) return 6;
  auto finish_frame = EncodeSblrLocalBackupArchiveFrame(0x0a01, payload);
  auto finish = RequestFor(0x0a01, finish_frame.bytes);
  const auto sealed = DispatchSblrOperation(finish);
  if (!sealed.accepted || sealed.api_result.evidence.size() != 10) return 7;
  const auto replay = DispatchSblrOperation(finish);
  if (!replay.accepted || !SameEvidence(replay.api_result.evidence, sealed.api_result.evidence)) return 8;
  auto restore = RequestFor(0x0a02, EncodeSblrLocalBackupArchiveFrame(0x0a02, payload).bytes);
  if (!DispatchSblrOperation(restore).accepted) return 9;
  auto verify = RequestFor(0x0a04, EncodeSblrLocalBackupArchiveFrame(0x0a04, payload).bytes);
  if (!DispatchSblrOperation(verify).accepted) return 10;

  // CSC-TEST-003344: cancel is before artifact access; failed finality is recovery-suppressed.
  auto export_request = RequestFor(0x0a03, EncodeSblrLocalBackupArchiveFrame(0x0a03, payload).bytes);
  export_request.context.query_cancellation_requested = [] { return true; };
  if (!RejectedAs(DispatchSblrOperation(export_request), "PROCESS.CANCELLED")) return 11;
  auto recovery = RequestFor(0x0a03, EncodeSblrLocalBackupArchiveFrame(0x0a03, payload).bytes);
  recovery.context.local_transaction_id = 8;
  if (!RejectedAs(DispatchSblrOperation(recovery), "MGA.BACKUP.POLICY_REFUSED")) return 12;
  return 0;
}
