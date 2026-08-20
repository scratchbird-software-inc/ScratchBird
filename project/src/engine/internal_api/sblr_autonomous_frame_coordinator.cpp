#include "sblr_autonomous_frame_coordinator.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "../sblr/sblr_autonomous_frame_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <unordered_map>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex mutex;
std::unordered_map<std::string, SblrAutonomousFrameSnapshot> rows;
std::unordered_map<std::string, SblrAutonomousBodyFrameProjectionV1> projections;
std::uint64_t generation = 0;

EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key), {});
}
bool HasTag(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}
std::string Identity(std::uint64_t value) {
  const auto millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, millis + value);
  return uuid.ok() ? scratchbird::core::uuid::UuidToString(uuid.value.value)
                   : std::string{};
}
bool Uuid(const std::string& text) {
  return scratchbird::core::uuid::ParseUuid(text).ok();
}
bool Sha(const std::string& text) {
  return text.size() == 71 && text.rfind("sha256:", 0) == 0;
}
std::string DescriptorEvidence(const SblrAutonomousFrameSnapshot& snapshot) {
  const auto& a = snapshot.authority;
  scratchbird::engine::sblr::SblrAutonomousFrameDescriptorV1 descriptor;
  const auto uuid = [](const std::string& text, auto* out) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (!parsed.ok()) return false;
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
    return true;
  };
  const auto sha = [](const std::string& text, auto* out) {
    if (text.size() != 71 || text.rfind("sha256:", 0) != 0) return false;
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    for (std::size_t i = 0; i < out->size(); ++i) {
      const int high = nibble(text[7 + i * 2]);
      const int low = nibble(text[8 + i * 2]);
      if (high < 0 || low < 0) return false;
      (*out)[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
  };
  if (!uuid(a.preliminary_receipt_uuid, &descriptor.receipt) ||
      !uuid(snapshot.frame_uuid, &descriptor.frame) ||
      !uuid(snapshot.child_transaction_uuid, &descriptor.child_transaction) ||
      !uuid(a.parent_transaction_uuid, &descriptor.parent_transaction) ||
      !uuid(a.parent_frame_uuid, &descriptor.parent_frame) ||
      !uuid(a.database_uuid, &descriptor.database) ||
      !uuid(a.attachment_uuid, &descriptor.attachment) ||
      !uuid(a.session_uuid, &descriptor.session) ||
      !uuid(a.principal_uuid, &descriptor.principal) ||
      !uuid(a.security_snapshot_uuid, &descriptor.security) ||
      !uuid(a.policy_snapshot_uuid, &descriptor.policy) ||
      !uuid(a.body_sblr_uuid, &descriptor.body) ||
      (!a.dynamic_statement_sblr_uuid.empty() &&
       !uuid(a.dynamic_statement_sblr_uuid, &descriptor.dynamic)) ||
      !sha(a.effect_set_sha256, &descriptor.effect_sha)) return {};
  descriptor.frame_generation = snapshot.frame_generation;
  descriptor.child_transaction_number = snapshot.child_transaction_number;
  descriptor.catalog_generation = a.catalog_generation;
  descriptor.capability_generation = a.capability_generation;
  descriptor.intent = a.intent;
  descriptor.depth = a.nesting_depth;
  descriptor.effect_count = a.effect_count;
  const auto canonical =
      scratchbird::engine::sblr::EncodeSblrAutonomousFrameDescriptorV1(descriptor);
  if (canonical.size() != 324) return {};
  scratchbird::engine::sblr::AfSha digest{};
  std::copy(canonical.begin() + 292, canonical.end(), digest.begin());
  static constexpr char hex[] = "0123456789abcdef";
  std::string out = "sha256:";
  for (auto byte : digest) { out.push_back(hex[byte >> 4]); out.push_back(hex[byte & 15]); }
  return out;
}
bool Valid(const EngineRequestContext& c,
           const SblrAutonomousBodyFrameProjectionV1& a) {
  return a.preliminary_receipt_uuid == c.statement_uuid.canonical &&
         a.parent_transaction_uuid == c.transaction_uuid.canonical &&
         a.database_uuid == c.database_uuid.canonical &&
         a.session_uuid == c.session_uuid.canonical &&
         a.principal_uuid == c.principal_uuid.canonical &&
         a.structural_occurrence_id && Uuid(a.parent_frame_uuid) &&
         Uuid(a.attachment_uuid) && Uuid(a.security_snapshot_uuid) &&
         Uuid(a.policy_snapshot_uuid) && a.catalog_generation &&
         a.capability_generation && Uuid(a.body_sblr_uuid) &&
         Sha(a.body_sblr_sha256) && a.intent >= 1 && a.intent <= 4 &&
         a.nesting_depth >= 1 && a.nesting_depth <= 8 &&
         a.effect_count <= 64 && Sha(a.effect_set_sha256) &&
         Sha(a.projection_evidence_sha256);
}
std::string ProjectionKey(const std::string& receipt, std::uint64_t occurrence) {
  return receipt + ":" + std::to_string(occurrence);
}
std::string ProjectionPath(const EngineRequestContext& c) {
  return c.database_path + ".sb.sblr_autonomous_body_frame_projection.v1";
}
std::string ShaMaterial(const std::string& material) {
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(
      std::vector<std::uint8_t>(material.begin(),material.end())).digest;
  static constexpr char hex[]="0123456789abcdef";std::string out="sha256:";
  for(auto byte:digest){out.push_back(hex[byte>>4]);out.push_back(hex[byte&15]);}
  return out;
}
bool AppendProjection(const EngineRequestContext& c, char event,
                      const SblrAutonomousBodyFrameProjectionV1& a) {
  std::ofstream out(ProjectionPath(c), std::ios::app);
  if (!out) return false;
  out << event << ' ' << std::quoted(a.preliminary_receipt_uuid) << ' '
      << a.structural_occurrence_id << ' ' << std::quoted(a.parent_transaction_uuid) << ' '
      << std::quoted(a.parent_frame_uuid) << ' ' << std::quoted(a.database_uuid) << ' '
      << std::quoted(a.attachment_uuid) << ' ' << std::quoted(a.session_uuid) << ' '
      << std::quoted(a.principal_uuid) << ' ' << std::quoted(a.security_snapshot_uuid) << ' '
      << std::quoted(a.policy_snapshot_uuid) << ' ' << a.catalog_generation << ' '
      << a.capability_generation << ' ' << std::quoted(a.body_sblr_uuid) << ' '
      << std::quoted(a.dynamic_statement_sblr_uuid) << ' '
      << std::quoted(a.body_sblr_sha256) << ' ' << unsigned(a.intent) << ' '
      << unsigned(a.nesting_depth) << ' ' << a.effect_count << ' '
      << std::quoted(a.effect_set_sha256) << ' '
      << std::quoted(a.projection_evidence_sha256) << '\n';
  out.flush(); return bool(out);
}
void LoadProjections(const EngineRequestContext& c) {
  std::ifstream in(ProjectionPath(c));
  char event; SblrAutonomousBodyFrameProjectionV1 a;
  unsigned intent, depth;
  while (in >> event >> std::quoted(a.preliminary_receipt_uuid) >> a.structural_occurrence_id
         >> std::quoted(a.parent_transaction_uuid) >> std::quoted(a.parent_frame_uuid)
         >> std::quoted(a.database_uuid) >> std::quoted(a.attachment_uuid)
         >> std::quoted(a.session_uuid) >> std::quoted(a.principal_uuid)
         >> std::quoted(a.security_snapshot_uuid) >> std::quoted(a.policy_snapshot_uuid)
         >> a.catalog_generation >> a.capability_generation >> std::quoted(a.body_sblr_uuid)
         >> std::quoted(a.dynamic_statement_sblr_uuid) >> std::quoted(a.body_sblr_sha256)
         >> intent >> depth >> a.effect_count >> std::quoted(a.effect_set_sha256)
         >> std::quoted(a.projection_evidence_sha256)) {
    a.intent=static_cast<std::uint8_t>(intent); a.nesting_depth=static_cast<std::uint8_t>(depth);
    const auto key=ProjectionKey(a.preliminary_receipt_uuid,a.structural_occurrence_id);
    if(event=='P') projections[key]=a; else if(event=='C') projections.erase(key);
  }
}
bool Publish(const EngineRequestContext& c,
             const SblrAutonomousFrameSnapshot& s) {
  std::ofstream out(c.database_path + ".sb.sblr_autonomous_frame.v1",
                    std::ios::app);
  if (!out) return false;
  out << "SBAF1\t" << s.frame_uuid << '\t' << s.frame_generation << '\t'
      << s.child_transaction_uuid << '\t' << s.child_transaction_number << '\t'
      << unsigned(s.state) << '\t' << s.finality_sequence << '\n';
  out.flush();
  return bool(out);
}
}  // namespace

EngineApiDiagnostic PublishSblrAutonomousBodyFrameProjection(
    const EngineRequestContext& c,
    const SblrAutonomousBodyFrameProjectionV1& authority) {
  std::lock_guard lock(mutex);
  if (!HasTag(c, "private_psql_autonomous_body_compiler") ||
      !c.statement_metadata_snapshot_engine_owned)
    return Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.projection_hidden");
  if (!Valid(c, authority))
    return Diagnostic("PSQL.AUTONOMOUS_DESCRIPTOR_INVALID", "sblr.psql_autonomous.projection_invalid");
  const auto key = ProjectionKey(authority.preliminary_receipt_uuid,
                                 authority.structural_occurrence_id);
  if (projections.contains(key))
    return Diagnostic("PSQL.AUTONOMOUS_DESCRIPTOR_INVALID", "sblr.psql_autonomous.projection_duplicate");
  LoadProjections(c);
  if (projections.contains(key))
    return Diagnostic("PSQL.AUTONOMOUS_DESCRIPTOR_INVALID", "sblr.psql_autonomous.projection_duplicate");
  if (!AppendProjection(c, 'P', authority))
    return Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED", "sblr.psql_autonomous.projection_publish_failed");
  projections.emplace(key, authority);
  return Diagnostic("OK", "ok");
}

EngineApiDiagnostic CompileAndPublishSblrAutonomousBodyFrameProjection(
    const EngineRequestContext& c, const std::string& receipt,
    std::uint64_t occurrence) {
  if (!HasTag(c, "private_psql_autonomous_body_compiler") ||
      !c.statement_metadata_snapshot_engine_owned || receipt != c.statement_uuid.canonical ||
      !occurrence || c.transaction_uuid.canonical.empty() ||
      c.database_uuid.canonical.empty() || c.session_uuid.canonical.empty() ||
      c.principal_uuid.canonical.empty())
    return Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.compiler_hidden");
  SblrAutonomousBodyFrameProjectionV1 a;
  a.preliminary_receipt_uuid=receipt;a.structural_occurrence_id=occurrence;
  a.parent_transaction_uuid=c.transaction_uuid.canonical;
  a.parent_frame_uuid=Identity(++generation);
  a.database_uuid=c.database_uuid.canonical;
  a.attachment_uuid=Identity(++generation);
  a.session_uuid=c.session_uuid.canonical;a.principal_uuid=c.principal_uuid.canonical;
  a.security_snapshot_uuid=Identity(++generation);a.policy_snapshot_uuid=Identity(++generation);
  a.catalog_generation=++generation;a.capability_generation=++generation;
  a.body_sblr_uuid=Identity(++generation);a.intent=1;a.nesting_depth=1;a.effect_count=0;
  const std::string material=receipt+":"+std::to_string(occurrence)+":"+a.body_sblr_uuid;
  a.body_sblr_sha256=ShaMaterial("ScratchBird.SblrPsqlAutonomousBody.V1"+material);
  a.effect_set_sha256=ShaMaterial("ScratchBird.SblrPsqlAutonomousAllowedEffectSet.V1");
  a.projection_evidence_sha256=ShaMaterial(
      "ScratchBird.SblrPsqlAutonomousBodyFrameProjection.V1"+material);
  return PublishSblrAutonomousBodyFrameProjection(c,a);
}

EngineApiDiagnostic RevokeSblrAutonomousBodyFrameProjection(
    const EngineRequestContext& c, const std::string& receipt) {
  std::lock_guard lock(mutex);
  if (!HasTag(c, "private_psql_autonomous_body_compiler"))
    return Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.projection_hidden");
  for (auto it = projections.begin(); it != projections.end();) {
    if (it->second.preliminary_receipt_uuid == receipt) {
      if (!AppendProjection(c, 'C', it->second))
        return Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED", "sblr.psql_autonomous.projection_revoke_failed");
      it = projections.erase(it);
    }
    else ++it;
  }
  return Diagnostic("OK", "ok");
}

SblrAutonomousFrameCoordinatorResult ReserveSblrAutonomousFrame(
    const EngineRequestContext& c, const std::string& receipt,
    std::uint64_t occurrence) {
  std::lock_guard lock(mutex);
  SblrAutonomousFrameCoordinatorResult out;
  if (!HasTag(c, "private_psql_autonomous_frame_coordination") ||
      !c.statement_metadata_snapshot_engine_owned) {
    out.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.hidden");
    return out;
  }
  auto projection = projections.find(ProjectionKey(receipt, occurrence));
  if (projection == projections.end()) {
    LoadProjections(c);
    projection = projections.find(ProjectionKey(receipt, occurrence));
  }
  if (projection == projections.end() || !Valid(c, projection->second)) {
    out.diagnostic = Diagnostic("PSQL.AUTONOMOUS_DESCRIPTOR_INVALID",
                                "sblr.psql_autonomous.projection_invalid");
    return out;
  }
  const auto authority = projection->second;
  if (!AppendProjection(c, 'C', authority)) {
    out.diagnostic = Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED",
                                "sblr.psql_autonomous.projection_consume_failed");
    return out;
  }
  projections.erase(projection);
  SblrAutonomousFrameSnapshot snapshot;
  snapshot.authority = authority;
  snapshot.frame_generation = ++generation;
  snapshot.child_transaction_number = ++generation;
  snapshot.recovery_generation = ++generation;
  snapshot.frame_uuid = Identity(snapshot.frame_generation);
  snapshot.child_transaction_uuid = Identity(snapshot.child_transaction_number);
  snapshot.recovery_token_uuid = Identity(snapshot.recovery_generation);
  snapshot.state = SblrAutonomousFrameState::reserved;
  snapshot.descriptor_evidence_sha256 = DescriptorEvidence(snapshot);
  if (snapshot.descriptor_evidence_sha256 == authority.effect_set_sha256 ||
      !Publish(c, snapshot)) {
    out.diagnostic = Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED",
                                "sblr.psql_autonomous.reserve_publish_failed");
    return out;
  }
  rows[snapshot.frame_uuid] = snapshot;
  out.ok = true; out.snapshot = snapshot; out.diagnostic = Diagnostic("OK", "ok");
  return out;
}

SblrAutonomousFrameCoordinatorResult FinalizeSblrAutonomousFrame(
    const EngineRequestContext& c, const std::string& id,
    std::uint64_t frame_generation, bool commit) {
  std::lock_guard lock(mutex);
  SblrAutonomousFrameCoordinatorResult out;
  if (!HasTag(c, "private_psql_autonomous_frame_coordination")) {
    out.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.hidden"); return out;
  }
  auto it = rows.find(id);
  if (it == rows.end() || it->second.authority.parent_transaction_uuid != c.transaction_uuid.canonical) {
    out.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.hidden"); return out;
  }
  if (it->second.frame_generation != frame_generation || it->second.state != SblrAutonomousFrameState::reserved) {
    out.diagnostic = Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED", "sblr.psql_autonomous.stale"); return out;
  }
  auto snapshot = it->second;
  snapshot.state = commit ? SblrAutonomousFrameState::committed : SblrAutonomousFrameState::rolled_back;
  snapshot.finality_sequence = ++generation;
  if (!Publish(c, snapshot)) { out.diagnostic = Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED", "sblr.psql_autonomous.finality_publish_failed"); return out; }
  it->second = snapshot; out.ok = true; out.snapshot = snapshot; out.diagnostic = Diagnostic("OK", "ok"); return out;
}

EngineApiDiagnostic RecoverSblrAutonomousFrameCoordinator(const EngineRequestContext& c) {
  std::lock_guard lock(mutex);
  if (!HasTag(c, "right:SBLR_PSQL_AUTONOMOUS_FRAME_ADMIN")) return Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.recovery_denied");
  for (auto& [_, snapshot] : rows) if (snapshot.state == SblrAutonomousFrameState::reserved) {
    snapshot.state = SblrAutonomousFrameState::revoked; snapshot.finality_sequence = ++generation;
    if (!Publish(c, snapshot)) return Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED", "sblr.psql_autonomous.recovery_publish_failed");
  }
  return Diagnostic("OK", "ok");
}
}  // namespace scratchbird::engine::internal_api
