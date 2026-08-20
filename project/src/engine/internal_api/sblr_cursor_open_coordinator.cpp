#include "sblr_cursor_open_coordinator.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include "sblr_cursor_open_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex coordinator_mutex;
std::unordered_map<std::string, SblrCursorOpenSnapshot> descriptors;
std::unordered_map<std::string, SblrCursorOpenSnapshot> cursors;
std::unordered_map<std::string, SblrCursorOpenSnapshot> retired_cursors;
std::uint64_t next_generation = 0;

EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key), {});
}

bool HasTag(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}

std::string NewIdentity() {
  const auto millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object,
      millis + (++next_generation));
  return generated.ok()
             ? scratchbird::core::uuid::UuidToString(generated.value.value)
             : std::string{};
}

std::string Hash(const std::string& material) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
                          std::vector<std::uint8_t>(material.begin(),
                                                    material.end()))
                          .digest;
  return "sha256:" + scratchbird::core::hash::HexLower(digest);
}

bool AppendJournal(const EngineRequestContext& context, char event,
                   const SblrCursorOpenSnapshot& snapshot) {
  std::ofstream output(context.database_path + ".sb.sblr_cursor_open.v1",
                       std::ios::app);
  if (!output) return false;
  output << event << '\t' << snapshot.descriptor_uuid << '\t'
         << snapshot.cursor_uuid << '\t' << snapshot.cursor_generation << '\n';
  output.flush();
  return static_cast<bool>(output);
}
}  // namespace

SblrCursorOpenResult CompileAndPublishSblrExecutablePlanReceipt(
    const EngineRequestContext& context, const std::string& receipt_uuid,
    std::uint64_t occurrence, std::uint8_t mode, std::uint8_t hold,
    std::uint32_t fetch_size, std::uint64_t availability_generation) {
  std::lock_guard lock(coordinator_mutex);
  SblrCursorOpenResult result;
  if (!HasTag(context, "private_executable_plan_receipt_compiler") ||
      !context.statement_metadata_snapshot_engine_owned) {
    result.diagnostic =
        Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.plan_receipt_hidden");
    return result;
  }
  if (receipt_uuid != context.statement_uuid.canonical || !occurrence ||
      mode != 1 || hold < 1 || hold > 2 || !fetch_size ||
      fetch_size > 65535 || !availability_generation ||
      context.principal_uuid.canonical.empty()) {
    result.diagnostic =
        Diagnostic("SBLR.OPERAND_INVALID", "sblr.cursor.plan_receipt_invalid");
    return result;
  }
  SblrCursorOpenSnapshot snapshot;
  snapshot.receipt_uuid = receipt_uuid;
  snapshot.occurrence = occurrence;
  snapshot.descriptor_uuid = NewIdentity();
  snapshot.plan_uuid = NewIdentity();
  snapshot.row_shape_uuid = NewIdentity();
  snapshot.transaction_uuid = context.transaction_uuid.canonical;
  snapshot.session_uuid = context.session_uuid.canonical;
  snapshot.security_uuid = context.principal_uuid.canonical;
  snapshot.descriptor_generation = ++next_generation;
  snapshot.plan_generation = ++next_generation;
  snapshot.row_shape_generation = ++next_generation;
  snapshot.catalog_generation = std::max<std::uint64_t>(
      1, context.statement_metadata_snapshot_visible_through_local_transaction_id);
  snapshot.mode = mode;
  snapshot.hold = hold;
  snapshot.fetch_size = fetch_size;
  snapshot.availability_generation = availability_generation;
  snapshot.plan_evidence_sha256 = Hash(
      "ScratchBird.SblrExecutablePlanReceipt.V1" + snapshot.plan_uuid);
  scratchbird::engine::sblr::SblrCursorOpenDescriptorV1 wire;
  const auto copy_uuid = [&](const std::string& text, auto* target) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (!parsed.ok()) return false;
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), target->begin());
    return true;
  };
  const auto copy_sha = [](const std::string& text, auto* target) {
    if (text.size() != 71 || text.substr(0, 7) != "sha256:") return false;
    for (std::size_t i = 0; i < 32; ++i)
      (*target)[i] = static_cast<std::uint8_t>(
          std::stoul(text.substr(7 + i * 2, 2), nullptr, 16));
    return true;
  };
  if (!copy_uuid(snapshot.descriptor_uuid, &wire.descriptor) ||
      !copy_uuid(snapshot.plan_uuid, &wire.plan) ||
      !copy_uuid(snapshot.row_shape_uuid, &wire.row_shape) ||
      (!snapshot.transaction_uuid.empty() &&
       !copy_uuid(snapshot.transaction_uuid, &wire.transaction)) ||
      !copy_uuid(snapshot.session_uuid, &wire.session) ||
      !copy_uuid(snapshot.security_uuid, &wire.security) ||
      !copy_sha(snapshot.plan_evidence_sha256, &wire.plan_evidence)) {
    result.diagnostic = Diagnostic("CURSOR.OPEN_FAILED",
                                   "sblr.cursor.plan_receipt_identity_invalid");
    return result;
  }
  wire.descriptor_generation = snapshot.descriptor_generation;
  wire.plan_generation = snapshot.plan_generation;
  wire.row_shape_generation = snapshot.row_shape_generation;
  wire.catalog_generation = snapshot.catalog_generation;
  wire.mode = mode;
  wire.hold = hold;
  wire.fetch_size = fetch_size;
  wire.availability_generation = availability_generation;
  const auto canonical =
      scratchbird::engine::sblr::EncodeSblrCursorOpenDescriptorV1(wire);
  if (canonical.size() != 232) {
    result.diagnostic = Diagnostic("CURSOR.OPEN_FAILED",
                                   "sblr.cursor.plan_receipt_evidence_failed");
    return result;
  }
  snapshot.descriptor_evidence_sha256 = "sha256:";
  static constexpr char hex[] = "0123456789abcdef";
  for (std::size_t i = 192; i < 224; ++i) {
    snapshot.descriptor_evidence_sha256.push_back(hex[canonical[i] >> 4]);
    snapshot.descriptor_evidence_sha256.push_back(hex[canonical[i] & 15]);
  }
  if (!AppendJournal(context, 'P', snapshot)) {
    result.diagnostic =
        Diagnostic("CURSOR.OPEN_FAILED", "sblr.cursor.plan_receipt_publish_failed");
    return result;
  }
  descriptors[snapshot.descriptor_uuid] = snapshot;
  result.ok = true;
  result.snapshot = snapshot;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrCursorOpenResult OpenSblrCursor(
    const EngineRequestContext& context, const std::string& descriptor_uuid,
    std::uint64_t descriptor_generation,
    const std::string& descriptor_evidence_sha256,
    std::uint64_t availability_generation) {
  std::lock_guard lock(coordinator_mutex);
  SblrCursorOpenResult result;
  if (!HasTag(context, "private_cursor_open")) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.hidden");
    return result;
  }
  const auto found = descriptors.find(descriptor_uuid);
  if (found == descriptors.end() ||
      found->second.session_uuid != context.session_uuid.canonical) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.hidden");
    return result;
  }
  auto snapshot = found->second;
  if (snapshot.descriptor_generation != descriptor_generation ||
      snapshot.descriptor_evidence_sha256 != descriptor_evidence_sha256) {
    result.diagnostic = Diagnostic("CURSOR.PLAN_STALE", "sblr.cursor.plan_stale");
    return result;
  }
  snapshot.availability_generation = availability_generation;
  snapshot.cursor_uuid = NewIdentity();
  snapshot.cursor_generation = ++next_generation;
  snapshot.position_generation = 1;
  snapshot.cursor_evidence_sha256 = Hash(
      "ScratchBird.SblrCursorHandle.V1" + snapshot.cursor_uuid +
      snapshot.plan_uuid + std::to_string(snapshot.cursor_generation));
  if (!AppendJournal(context, 'O', snapshot)) {
    result.diagnostic =
        Diagnostic("CURSOR.OPEN_FAILED", "sblr.cursor.open_publish_failed");
    return result;
  }
  cursors[snapshot.cursor_uuid] = snapshot;
  descriptors.erase(found);
  result.ok = true;
  result.snapshot = snapshot;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

EngineApiDiagnostic RecoverSblrOpenCursors(
    const EngineRequestContext& context) {
  std::lock_guard lock(coordinator_mutex);
  if (!HasTag(context, "right:SBLR_CURSOR_ADMIN")) {
    return Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.recovery_denied");
  }
  for (const auto& [unused, snapshot] : cursors) {
    (void)unused;
    AppendJournal(context, 'X', snapshot);
  }
  cursors.clear();
  descriptors.clear();
  return Diagnostic("OK", "ok");
}

SblrCursorOpenResult FetchSblrCursor(
    const EngineRequestContext& context, const std::string& cursor_uuid,
    std::uint64_t cursor_generation, std::uint64_t position_generation,
    const std::string& admitted_evidence, std::uint64_t availability_generation,
    std::uint32_t maximum_rows) {
  std::lock_guard lock(coordinator_mutex);
  SblrCursorOpenResult result;
  if (!HasTag(context, "private_cursor_fetch")) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.hidden");
    return result;
  }
  const auto found = cursors.find(cursor_uuid);
  if (found == cursors.end() || found->second.session_uuid != context.session_uuid.canonical) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.hidden");
    return result;
  }
  auto snapshot = found->second;
  const auto wire_evidence = Hash(snapshot.cursor_evidence_sha256);
  if (snapshot.cursor_generation != cursor_generation ||
      snapshot.position_generation != position_generation ||
      wire_evidence != admitted_evidence) {
    result.diagnostic = Diagnostic("CURSOR.STALE", "sblr.cursor.position_stale");
    return result;
  }
  if (!availability_generation || !maximum_rows || maximum_rows > 65535) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID", "sblr.cursor.fetch_invalid");
    return result;
  }
  snapshot.position_generation++;
  snapshot.availability_generation = availability_generation;
  snapshot.cursor_evidence_sha256 = Hash(
      "ScratchBird.SblrCursorFetchEvidence.V1" + snapshot.cursor_uuid +
      std::to_string(snapshot.position_generation));
  if (!AppendJournal(context, 'F', snapshot)) {
    result.diagnostic = Diagnostic("CURSOR.FETCH_FAILED", "sblr.cursor.fetch_publish_failed");
    return result;
  }
  found->second = snapshot;
  result.ok = true;
  result.snapshot = snapshot;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrCursorOpenResult CloseSblrCursor(
    const EngineRequestContext& context, const std::string& cursor_uuid,
    std::uint64_t cursor_generation, std::uint64_t position_generation,
    const std::string& admitted_evidence, std::uint64_t availability_generation,
    std::uint8_t close_reason) {
  std::lock_guard lock(coordinator_mutex);
  SblrCursorOpenResult result;
  if (!HasTag(context, "private_cursor_close")) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.hidden");
    return result;
  }
  const auto found = cursors.find(cursor_uuid);
  if (found == cursors.end()) {
    if (retired_cursors.find(cursor_uuid) != retired_cursors.end())
      result.diagnostic = Diagnostic("CURSOR.STALE", "sblr.cursor.stale");
    else
      result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.hidden");
    return result;
  }
  if (found->second.session_uuid != context.session_uuid.canonical) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.cursor.hidden");
    return result;
  }
  auto snapshot = found->second;
  if (snapshot.cursor_generation != cursor_generation ||
      snapshot.position_generation != position_generation ||
      Hash(snapshot.cursor_evidence_sha256) != admitted_evidence) {
    result.diagnostic = Diagnostic("CURSOR.STALE", "sblr.cursor.stale");
    return result;
  }
  if (!availability_generation || close_reason < 1 || close_reason > 4) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID", "sblr.cursor.close_invalid");
    return result;
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic("PROCESS.CANCELLED", "sblr.cursor.close_cancelled");
    return result;
  }
  snapshot.availability_generation = availability_generation;
  snapshot.cursor_evidence_sha256 = Hash(
      "ScratchBird.SblrCursorCloseEvidence.V1" + snapshot.cursor_uuid +
      std::to_string(snapshot.position_generation) + std::to_string(close_reason));
  if (!AppendJournal(context, 'C', snapshot)) {
    result.diagnostic = Diagnostic("CURSOR.CLOSE_FAILED", "sblr.cursor.close_publish_failed");
    return result;
  }
  retired_cursors[cursor_uuid] = snapshot;
  cursors.erase(found);
  result.ok = true;
  result.snapshot = snapshot;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}
}  // namespace scratchbird::engine::internal_api
