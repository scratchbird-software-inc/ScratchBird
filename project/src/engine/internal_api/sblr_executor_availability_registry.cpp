#include "sblr_executor_availability_registry.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::string_view kMagic = "SBEXAV1";
constexpr std::string_view kRegistryId =
    "engine.sblr_executor_availability_registry.v1";

std::recursive_mutex& RegistryMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

std::string StorePath(const EngineRequestContext& context,
                      const SblrExecutorAvailabilityRowIdentity& identity) {
  const auto suffix = identity.executor_id == kSblrParameterExecutorId
                          ? ".parameter"
                          : "";
  return context.database_path + ".sb.sblr_executor_availability_registry.v1" +
         suffix;
}

void AddField(std::string* out, std::string_view key, std::string_view value) {
  out->append(std::to_string(key.size()));
  out->push_back(':');
  out->append(key);
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string Sha256(std::string_view value) {
  const auto* bytes = reinterpret_cast<const scratchbird::core::platform::byte*>(
      value.data());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes, value.size());
  if (!digest.ok() ||
      digest.digest_bytes != scratchbird::core::hash::kSha256DigestBytes) {
    return {};
  }
  return "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}

bool ExactLiteralIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrLiteralExecutorId &&
         row.opcode_code == kSblrLiteralOpcodeCode &&
         row.opcode_version == kSblrLiteralOpcodeVersion &&
         row.operand_descriptor_id == kSblrLiteralOperandDescriptorId &&
         row.result_descriptor_id == kSblrLiteralResultDescriptorId &&
         row.result_descriptor_version == kSblrLiteralResultDescriptorVersion;
}
bool ExactParameterIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrParameterExecutorId &&
         row.opcode_code == kSblrParameterOpcodeCode &&
         row.opcode_version == kSblrParameterOpcodeVersion &&
         row.operand_descriptor_id == kSblrParameterOperandDescriptorId &&
         row.result_descriptor_id == kSblrParameterResultDescriptorId &&
         row.result_descriptor_version == kSblrParameterResultDescriptorVersion;
}
bool ExactAdmittedIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return ExactLiteralIdentity(row) || ExactParameterIdentity(row);
}

std::string StateName(SblrExecutorAvailabilityState state) {
  switch (state) {
    case SblrExecutorAvailabilityState::installed: return "installed";
    case SblrExecutorAvailabilityState::revoked: return "revoked";
    case SblrExecutorAvailabilityState::unavailable: return "unavailable";
  }
  return {};
}

bool ParseState(std::string_view value,
                SblrExecutorAvailabilityState* state) {
  if (state == nullptr) return false;
  if (value == "installed") {
    *state = SblrExecutorAvailabilityState::installed;
    return true;
  }
  if (value == "revoked") {
    *state = SblrExecutorAvailabilityState::revoked;
    return true;
  }
  if (value == "unavailable") {
    *state = SblrExecutorAvailabilityState::unavailable;
    return true;
  }
  return false;
}

bool SafeReason(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  for (unsigned char c : value) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
          c == '-')) return false;
  }
  return true;
}

bool ValidIdentityUuid(std::string_view value,
                       scratchbird::core::platform::UuidKind kind) {
  return !value.empty() &&
      scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
          kind, std::string(value)).ok();
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto end = line.find('\t', start);
    fields.push_back(line.substr(start, end == std::string::npos
                                           ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return fields;
}

std::uint64_t ParseU64(std::string_view value) {
  std::uint64_t result = 0;
  if (value.empty()) return 0;
  for (char c : value) {
    if (c < '0' || c > '9') return 0;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (result > (UINT64_MAX - digit) / 10) return 0;
    result = result * 10 + digit;
  }
  return result;
}

std::string DecisionPayload(const std::string& database_uuid,
                            const std::string& prior_snapshot_uuid,
                            std::uint64_t prior_generation,
                            const SblrExecutorAvailabilitySnapshot& next,
                            std::string_view reason_code) {
  std::string payload;
  AddField(&payload, "registry_id", kRegistryId);
  AddField(&payload, "database_uuid", database_uuid);
  AddField(&payload, "prior_snapshot_uuid", prior_snapshot_uuid);
  AddField(&payload, "prior_generation", std::to_string(prior_generation));
  AddField(&payload, "snapshot_uuid", next.snapshot_uuid);
  AddField(&payload, "generation", std::to_string(next.generation));
  AddField(&payload, "row_identity_sha256", next.row_identity_sha256);
  AddField(&payload, "installed", next.installed ? "true" : "false");
  AddField(&payload, "availability_state", StateName(next.availability_state));
  AddField(&payload, "reason_code", reason_code);
  return payload;
}

std::string JoinRecord(std::string_view kind,
                       const SblrExecutorAvailabilitySnapshot& snapshot,
                       std::string_view prior_snapshot_uuid,
                       std::uint64_t prior_generation,
                       std::string_view reason_code) {
  std::ostringstream out;
  out << kMagic << '\t' << kind << '\t' << kRegistryId << '\t'
      << snapshot.database_uuid << '\t' << prior_snapshot_uuid << '\t'
      << prior_generation << '\t' << snapshot.snapshot_uuid << '\t'
      << snapshot.generation << '\t' << snapshot.row_identity_sha256 << '\t'
      << (snapshot.installed ? "1" : "0") << '\t'
      << StateName(snapshot.availability_state) << '\t'
      << snapshot.decision_evidence_sha256 << '\t' << reason_code;
  return out.str();
}

bool DurableAppend(const std::string& path, const std::string& line) {
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    out << line << '\n';
    out.flush();
    if (!out) return false;
  }
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const bool ok = FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  return ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

EngineApiDiagnostic RegistryDiagnostic(std::string code,
                                       std::string key,
                                       std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

std::string NewSnapshotUuid(std::uint64_t generation) {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, now + generation);
  return generated.ok()
      ? scratchbird::core::uuid::UuidToString(generated.value.value)
      : std::string{};
}

struct DecodedPair {
  SblrExecutorAvailabilitySnapshot snapshot;
  std::string prior_snapshot_uuid;
  std::uint64_t prior_generation{0};
  std::string reason_code;
};

bool DecodeRecord(const std::vector<std::string>& fields,
                  std::string_view expected_kind,
                  DecodedPair* decoded) {
  if (decoded == nullptr || fields.size() != 13 || fields[0] != kMagic ||
      fields[1] != expected_kind || fields[2] != kRegistryId ||
      !ValidIdentityUuid(fields[3],
                         scratchbird::core::platform::UuidKind::database) ||
      !ValidIdentityUuid(fields[6],
                         scratchbird::core::platform::UuidKind::object) ||
      ParseU64(fields[7]) == 0 ||
      fields[8].size() != 71 || !fields[8].starts_with("sha256:") ||
      (fields[9] != "0" && fields[9] != "1") ||
      fields[11].size() != 71 || !fields[11].starts_with("sha256:") ||
      !SafeReason(fields[12])) return false;
  decoded->snapshot.database_uuid = fields[3];
  decoded->prior_snapshot_uuid = fields[4];
  decoded->prior_generation = ParseU64(fields[5]);
  decoded->snapshot.snapshot_uuid = fields[6];
  decoded->snapshot.generation = ParseU64(fields[7]);
  decoded->snapshot.row_identity_sha256 = fields[8];
  decoded->snapshot.installed = fields[9] == "1";
  if (!ParseState(fields[10], &decoded->snapshot.availability_state)) return false;
  decoded->snapshot.decision_evidence_sha256 = fields[11];
  decoded->reason_code = fields[12];
  return decoded->snapshot.installed ==
      (decoded->snapshot.availability_state ==
       SblrExecutorAvailabilityState::installed);
}

SblrExecutorAvailabilityLoadResult LoadLocked(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& identity,
    bool allow_bootstrap);

bool PublishPair(const std::string& path,
                 const DecodedPair& pair) {
  if (!DurableAppend(path, JoinRecord("EVIDENCE", pair.snapshot,
                                      pair.prior_snapshot_uuid,
                                      pair.prior_generation,
                                      pair.reason_code))) return false;
  return DurableAppend(path, JoinRecord("SNAPSHOT", pair.snapshot,
                                        pair.prior_snapshot_uuid,
                                        pair.prior_generation,
                                        pair.reason_code));
}

SblrExecutorAvailabilityLoadResult BootstrapLocked(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& identity) {
  SblrExecutorAvailabilityLoadResult result;
  DecodedPair pair;
  pair.snapshot.snapshot_uuid = NewSnapshotUuid(1);
  pair.snapshot.generation = 1;
  pair.snapshot.database_uuid = context.database_uuid.canonical;
  pair.snapshot.row_identity_sha256 =
      ComputeSblrExecutorAvailabilityRowIdentitySha256(identity);
  pair.snapshot.installed = true;
  pair.snapshot.availability_state = SblrExecutorAvailabilityState::installed;
  pair.reason_code = ExactParameterIdentity(identity)
                         ? "bootstrap.admitted_parameter.v1"
                         : "bootstrap.admitted_literal.v1";
  pair.snapshot.decision_evidence_sha256 = Sha256(DecisionPayload(
      pair.snapshot.database_uuid, {}, 0, pair.snapshot, pair.reason_code));
  if (pair.snapshot.snapshot_uuid.empty() ||
      pair.snapshot.row_identity_sha256.empty() ||
      pair.snapshot.decision_evidence_sha256.empty() ||
      !PublishPair(StorePath(context, identity), pair)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.bootstrap_failed", "durable bootstrap failed");
    return result;
  }
  result.ok = true;
  result.snapshot = pair.snapshot;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  return result;
}

SblrExecutorAvailabilityLoadResult LoadLocked(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& identity,
    bool allow_bootstrap) {
  SblrExecutorAvailabilityLoadResult result;
  const std::string path = StorePath(context, identity);
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error || context.database_path.empty() ||
      !ValidIdentityUuid(context.database_uuid.canonical,
                         scratchbird::core::platform::UuidKind::database)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.database_identity_invalid", "fail closed");
    return result;
  }
  if (!exists) {
    return allow_bootstrap ? BootstrapLocked(context, identity) : result;
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  if (!input.eof() || lines.empty() || (lines.size() % 2) != 0) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.torn_or_missing_evidence", "fail closed");
    return result;
  }
  SblrExecutorAvailabilitySnapshot prior;
  std::set<std::uint64_t> generations;
  for (std::size_t i = 0; i < lines.size(); i += 2) {
    DecodedPair evidence;
    DecodedPair snapshot;
    if (!DecodeRecord(SplitTabs(lines[i]), "EVIDENCE", &evidence) ||
        !DecodeRecord(SplitTabs(lines[i + 1]), "SNAPSHOT", &snapshot) ||
        JoinRecord("EVIDENCE", evidence.snapshot, evidence.prior_snapshot_uuid,
                   evidence.prior_generation, evidence.reason_code) != lines[i] ||
        JoinRecord("SNAPSHOT", snapshot.snapshot, snapshot.prior_snapshot_uuid,
                   snapshot.prior_generation, snapshot.reason_code) != lines[i + 1] ||
        evidence.snapshot.snapshot_uuid != snapshot.snapshot.snapshot_uuid ||
        evidence.snapshot.generation != snapshot.snapshot.generation ||
        evidence.snapshot.database_uuid != snapshot.snapshot.database_uuid ||
        evidence.snapshot.row_identity_sha256 !=
            snapshot.snapshot.row_identity_sha256 ||
        evidence.snapshot.installed != snapshot.snapshot.installed ||
        evidence.snapshot.availability_state !=
            snapshot.snapshot.availability_state ||
        evidence.snapshot.decision_evidence_sha256 !=
            snapshot.snapshot.decision_evidence_sha256 ||
        evidence.prior_snapshot_uuid != snapshot.prior_snapshot_uuid ||
        evidence.prior_generation != snapshot.prior_generation ||
        evidence.reason_code != snapshot.reason_code ||
        evidence.snapshot.database_uuid != context.database_uuid.canonical ||
        evidence.snapshot.row_identity_sha256 !=
            ComputeSblrExecutorAvailabilityRowIdentitySha256(identity) ||
        evidence.snapshot.generation != prior.generation + 1 ||
        evidence.prior_generation != prior.generation ||
        evidence.prior_snapshot_uuid != prior.snapshot_uuid ||
        !generations.emplace(evidence.snapshot.generation).second ||
        Sha256(DecisionPayload(evidence.snapshot.database_uuid,
                               evidence.prior_snapshot_uuid,
                               evidence.prior_generation, evidence.snapshot,
                               evidence.reason_code)) !=
            evidence.snapshot.decision_evidence_sha256) {
      result.diagnostic = RegistryDiagnostic(
          "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "sblr.executor_registry.contradictory_evidence", "fail closed");
      return result;
    }
    prior = evidence.snapshot;
  }
  result.ok = true;
  result.snapshot = prior;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  return result;
}

bool HasAdminAuthority(const EngineRequestContext& context) {
  if (!context.security_context_present) return false;
  for (const auto& tag : context.trace_tags) {
    if (tag == "right:SBLR_EXECUTOR_AVAILABILITY_ADMIN") return true;
  }
  return false;
}

}  // namespace

std::string ComputeSblrExecutorAvailabilityRowIdentitySha256(
    const SblrExecutorAvailabilityRowIdentity& identity) {
  if (!ExactAdmittedIdentity(identity)) return {};
  std::string payload;
  AddField(&payload, "executor_id", identity.executor_id);
  AddField(&payload, "opcode_code", std::to_string(identity.opcode_code));
  AddField(&payload, "opcode_version", identity.opcode_version);
  AddField(&payload, "operand_descriptor_id", identity.operand_descriptor_id);
  AddField(&payload, "result_descriptor_id", identity.result_descriptor_id);
  AddField(&payload, "result_descriptor_version",
           std::to_string(identity.result_descriptor_version));
  return Sha256(payload);
}

SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context) {
  return LoadSblrExecutorAvailabilitySnapshot(
      context, SblrExecutorAvailabilityRowIdentity{});
}

SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity) {
  std::lock_guard<std::recursive_mutex> guard(RegistryMutex());
  if (!ExactAdmittedIdentity(exact_row_identity)) {
    SblrExecutorAvailabilityLoadResult result;
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPERAND_INVALID", "sblr.executor_registry.row_invalid",
        "exact admitted executor row required");
    return result;
  }
  return LoadLocked(context, exact_row_identity, true);
}

SblrExecutorAvailabilitySetResult SetSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySetRequest& request) {
  SblrExecutorAvailabilitySetResult result;
  if (!HasAdminAuthority(context)) {
    result.diagnostic = RegistryDiagnostic(
        "SECURITY.ACCESS_DENIED", "security.access_denied",
        "executor availability administration not admitted");
    return result;
  }
  if (request.database_uuid != context.database_uuid.canonical ||
      !ExactAdmittedIdentity(request.exact_row_identity) ||
      StateName(request.requested_state).empty() ||
      !SafeReason(request.reason_code)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPERAND_INVALID", "sblr.executor_registry.set_invalid",
        "exact database row identity state and reason required");
    return result;
  }
  std::lock_guard<std::recursive_mutex> guard(RegistryMutex());
  const auto loaded = LoadLocked(context, request.exact_row_identity, true);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  if (loaded.snapshot.snapshot_uuid != request.expected_snapshot_uuid ||
      loaded.snapshot.generation != request.expected_generation) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
        "sblr.executor_registry.compare_failed", "snapshot changed");
    return result;
  }
  DecodedPair next;
  next.prior_snapshot_uuid = loaded.snapshot.snapshot_uuid;
  next.prior_generation = loaded.snapshot.generation;
  next.reason_code = request.reason_code;
  next.snapshot.snapshot_uuid = NewSnapshotUuid(next.prior_generation + 1);
  next.snapshot.generation = next.prior_generation + 1;
  next.snapshot.database_uuid = request.database_uuid;
  next.snapshot.row_identity_sha256 =
      ComputeSblrExecutorAvailabilityRowIdentitySha256(
          request.exact_row_identity);
  next.snapshot.availability_state = request.requested_state;
  next.snapshot.installed =
      request.requested_state == SblrExecutorAvailabilityState::installed;
  next.snapshot.decision_evidence_sha256 = Sha256(DecisionPayload(
      request.database_uuid, next.prior_snapshot_uuid, next.prior_generation,
      next.snapshot, next.reason_code));
  if (next.snapshot.snapshot_uuid.empty() ||
      next.snapshot.decision_evidence_sha256.empty() ||
      !PublishPair(StorePath(context, request.exact_row_identity), next)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.publish_failed", "durable evidence or snapshot failed");
    return result;
  }
  result.ok = true;
  result.snapshot = next.snapshot;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  result.evidence = {{"registry_id", std::string(kRegistryId)},
                     {"snapshot_uuid", next.snapshot.snapshot_uuid},
                     {"generation", std::to_string(next.snapshot.generation)},
                     {"row_identity_sha256", next.snapshot.row_identity_sha256},
                     {"availability_state", StateName(next.snapshot.availability_state)},
                     {"decision_evidence_sha256",
                      next.snapshot.decision_evidence_sha256}};
  return result;
}

EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot) {
  return RevalidateSblrExecutorAvailability(
      context, SblrExecutorAvailabilityRowIdentity{}, admitted_snapshot,
      current_snapshot);
}

EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot) {
  const auto loaded = LoadSblrExecutorAvailabilitySnapshot(
      context, exact_row_identity);
  if (!loaded.ok) return loaded.diagnostic;
  if (current_snapshot != nullptr) *current_snapshot = loaded.snapshot;
  if (loaded.snapshot.availability_state ==
          SblrExecutorAvailabilityState::revoked) {
    return RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.opcode.executor_evidence_missing", "executor row revoked");
  }
  if (loaded.snapshot.availability_state ==
      SblrExecutorAvailabilityState::unavailable) {
    return RegistryDiagnostic("SBLR.OPCODE.EXECUTOR_UNAVAILABLE",
                              "sblr.opcode.executor_unavailable",
                              "executor row unavailable");
  }
  if (!loaded.snapshot.installed) {
    return RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.opcode.executor_evidence_missing", "executor row absent");
  }
  if (admitted_snapshot.database_uuid != loaded.snapshot.database_uuid ||
      admitted_snapshot.snapshot_uuid != loaded.snapshot.snapshot_uuid ||
      admitted_snapshot.generation != loaded.snapshot.generation ||
      admitted_snapshot.row_identity_sha256 !=
          loaded.snapshot.row_identity_sha256 ||
      !admitted_snapshot.installed ||
      admitted_snapshot.availability_state !=
          SblrExecutorAvailabilityState::installed) {
    return RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
        "sblr.opcode.executor_evidence_stale", "installed snapshot changed");
  }
  return MakeEngineApiDiagnostic("OK", "ok", {}, false);
}

}  // namespace scratchbird::engine::internal_api
