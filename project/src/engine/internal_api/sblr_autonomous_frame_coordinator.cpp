#include "sblr_autonomous_frame_coordinator.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "../sblr/sblr_autonomous_frame_runtime.hpp"
#include "uuid.hpp"
#include "catalog/binary_catalog_metadata.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include <filesystem>
#include <charconv>
#include <limits>
#include <iterator>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <unordered_map>

namespace scratchbird::engine::internal_api {
namespace {
std::mutex mutex;
std::map<EngineUuid, SblrAutonomousFrameSnapshot> rows;
std::map<std::pair<EngineUuid, std::uint64_t>, SblrAutonomousBodyFrameProjectionV1> projections;
std::uint64_t generation = 0;

EngineApiDiagnostic Diagnostic(std::string code, std::string key) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key), {});
}
bool HasTag(const EngineRequestContext& context, const char* tag) {
  return context.security_context_present &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
             context.trace_tags.end();
}
EngineUuid Identity(std::uint64_t value) {
  const auto millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, millis + value);
  return uuid.ok() ? uuid.value.value : EngineUuid{};
}
bool Uuid(const EngineUuid& value) {
  return scratchbird::core::uuid::IsEngineIdentityUuid(value);
}
bool Sha(const std::string& text) {
  return text.size() == 71 && text.rfind("sha256:", 0) == 0;
}
std::string DescriptorEvidence(const SblrAutonomousFrameSnapshot& snapshot) {
  const auto& a = snapshot.authority;
  scratchbird::engine::sblr::SblrAutonomousFrameDescriptorV1 descriptor;
  const auto uuid = [](const EngineUuid& value, auto* out) {
    if (!Uuid(value)) return false;
    *out = value.bytes;
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
      (!a.dynamic_statement_sblr_uuid.is_nil() &&
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
  return a.preliminary_receipt_uuid == c.statement_uuid &&
         a.parent_transaction_uuid == c.transaction_uuid &&
         a.database_uuid == c.database_uuid &&
         a.session_uuid == c.session_uuid &&
         a.principal_uuid == c.principal_uuid &&
         a.structural_occurrence_id && Uuid(a.parent_frame_uuid) &&
         Uuid(a.attachment_uuid) && Uuid(a.security_snapshot_uuid) &&
         Uuid(a.policy_snapshot_uuid) && a.catalog_generation &&
         a.capability_generation && Uuid(a.body_sblr_uuid) &&
         Sha(a.body_sblr_sha256) && a.intent >= 1 && a.intent <= 4 &&
         a.nesting_depth >= 1 && a.nesting_depth <= 8 &&
         a.effect_count <= 64 && Sha(a.effect_set_sha256) &&
         Sha(a.projection_evidence_sha256);
}
auto ProjectionKey(const EngineUuid& receipt, std::uint64_t occurrence) {
  return std::pair{receipt, occurrence};
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
std::string EncodeProjection(const SblrAutonomousBodyFrameProjectionV1& a) {
  BinaryCatalogMetadata fields;
  fields.identities.emplace("preliminary_receipt_uuid", a.preliminary_receipt_uuid);
  fields.identities.emplace("parent_transaction_uuid", a.parent_transaction_uuid);
  fields.identities.emplace("parent_frame_uuid", a.parent_frame_uuid);
  fields.identities.emplace("database_uuid", a.database_uuid);
  fields.identities.emplace("attachment_uuid", a.attachment_uuid);
  fields.identities.emplace("session_uuid", a.session_uuid);
  fields.identities.emplace("principal_uuid", a.principal_uuid);
  fields.identities.emplace("security_snapshot_uuid", a.security_snapshot_uuid);
  fields.identities.emplace("policy_snapshot_uuid", a.policy_snapshot_uuid);
  fields.identities.emplace("body_sblr_uuid", a.body_sblr_uuid);
  fields.identities.emplace("dynamic_statement_sblr_uuid", a.dynamic_statement_sblr_uuid);
  fields.text.emplace("structural_occurrence_id", std::to_string(a.structural_occurrence_id));
  fields.text.emplace("catalog_generation", std::to_string(a.catalog_generation));
  fields.text.emplace("capability_generation", std::to_string(a.capability_generation));
  fields.text.emplace("intent", std::to_string(a.intent));
  fields.text.emplace("nesting_depth", std::to_string(a.nesting_depth));
  fields.text.emplace("effect_count", std::to_string(a.effect_count));
  fields.text.emplace("body_sblr_sha256", a.body_sblr_sha256);
  fields.text.emplace("effect_set_sha256", a.effect_set_sha256);
  fields.text.emplace("projection_evidence_sha256", a.projection_evidence_sha256);
  std::string bytes;
  return EncodeBinaryCatalogMetadata(fields, "autonomous.body_projection.v2", &bytes) ? bytes : std::string{};
}
bool DecodeProjection(std::string_view bytes, SblrAutonomousBodyFrameProjectionV1* output) {
  if (!output) return false;
  BinaryCatalogMetadata fields;
  if (!DecodeBinaryCatalogMetadata(bytes, "autonomous.body_projection.v2", &fields) ||
      fields.identities.size() != 11 || fields.text.size() != 9) return false;
  SblrAutonomousBodyFrameProjectionV1 a;
  const auto identity = [&](const char* name, EngineUuid* target, bool optional = false) {
    const auto it = fields.identities.find(name);
    if (it == fields.identities.end() ||
        (!Uuid(it->second) && !(optional && it->second.is_nil()))) return false;
    *target = it->second; return true;
  };
  const auto number = [&](const char* name, auto* target) {
    const auto it = fields.text.find(name);
    if (it == fields.text.end() || it->second.empty()) return false;
    std::uint64_t value = 0;
    const auto& text = it->second;
    const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(),value);
    using Number = std::remove_reference_t<decltype(*target)>;
    if (error != std::errc{} || end != text.data()+text.size() ||
        value > std::numeric_limits<Number>::max()) return false;
    *target = static_cast<Number>(value); return true;
  };
  if (!identity("preliminary_receipt_uuid", &a.preliminary_receipt_uuid)) return false;
  if (!identity("parent_transaction_uuid", &a.parent_transaction_uuid)) return false;
  if (!identity("parent_frame_uuid", &a.parent_frame_uuid)) return false;
  if (!identity("database_uuid", &a.database_uuid)) return false;
  if (!identity("attachment_uuid", &a.attachment_uuid)) return false;
  if (!identity("session_uuid", &a.session_uuid)) return false;
  if (!identity("principal_uuid", &a.principal_uuid)) return false;
  if (!identity("security_snapshot_uuid", &a.security_snapshot_uuid)) return false;
  if (!identity("policy_snapshot_uuid", &a.policy_snapshot_uuid)) return false;
  if (!identity("body_sblr_uuid", &a.body_sblr_uuid)) return false;
  if (!identity("dynamic_statement_sblr_uuid", &a.dynamic_statement_sblr_uuid, true)) return false;
  if (!number("structural_occurrence_id", &a.structural_occurrence_id)) return false;
  if (!number("catalog_generation", &a.catalog_generation)) return false;
  if (!number("capability_generation", &a.capability_generation)) return false;
  if (!number("intent", &a.intent)) return false;
  if (!number("nesting_depth", &a.nesting_depth)) return false;
  if (!number("effect_count", &a.effect_count)) return false;
  if (!fields.text.contains("body_sblr_sha256") || !Sha(fields.text.at("body_sblr_sha256"))) return false;
  a.body_sblr_sha256 = fields.text.at("body_sblr_sha256");
  if (!fields.text.contains("effect_set_sha256") || !Sha(fields.text.at("effect_set_sha256"))) return false;
  a.effect_set_sha256 = fields.text.at("effect_set_sha256");
  if (!fields.text.contains("projection_evidence_sha256") || !Sha(fields.text.at("projection_evidence_sha256"))) return false;
  a.projection_evidence_sha256 = fields.text.at("projection_evidence_sha256");
  if (!a.structural_occurrence_id || !a.catalog_generation || !a.capability_generation ||
      a.intent < 1 || a.intent > 4 || a.nesting_depth < 1 || a.nesting_depth > 8 || a.effect_count > 64)
    return false;
  *output = std::move(a); return true;
}
bool AppendProjection(const EngineRequestContext& c, char event,
                      const SblrAutonomousBodyFrameProjectionV1& a) {
  if (event != 'P' && event != 'C') return false;
  const auto projection = EncodeProjection(a);
  if (projection.empty()) return false;
  const auto bytes = EncodeMgaMetadataFields({"autonomous.projection.journal.v2", std::string(1,event), projection});
  if (bytes.empty()) return false;
  std::ofstream out(ProjectionPath(c), std::ios::binary | std::ios::app);
  if (!out) return false;
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.flush(); return bool(out);
}
bool LoadProjections(const EngineRequestContext& c) {
  std::error_code error;
  const auto path = ProjectionPath(c);
  const bool exists = std::filesystem::exists(path,error);
  if (error) return false;
  if (!exists) return true;
  const auto size = std::filesystem::file_size(path,error);
  if (error || size > kMgaMetadataMaximumBytes) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  if (in.bad() || bytes.size() != size) return false;
  std::vector<std::string> records;
  if (!DecodeMgaMetadataStream(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()), &records))
    return false;
  auto staged = projections;
  for (auto it = staged.begin(); it != staged.end();) {
    if (it->second.database_uuid == c.database_uuid) it = staged.erase(it);
    else ++it;
  }
  for (const auto& record : records) {
    std::vector<std::string> fields;
    SblrAutonomousBodyFrameProjectionV1 a;
    if (!DecodeMgaMetadataFields(record,&fields) || fields.size() != 3 ||
        fields[0] != "autonomous.projection.journal.v2" ||
        (fields[1] != "P" && fields[1] != "C") || !DecodeProjection(fields[2], &a) ||
        a.database_uuid != c.database_uuid) return false;
    const auto key = ProjectionKey(a.preliminary_receipt_uuid,a.structural_occurrence_id);
    if (fields[1] == "P") staged[key] = std::move(a);
    else staged.erase(key);
  }
  projections.swap(staged);
  return true;
}
bool Publish(const EngineRequestContext& c,
             const SblrAutonomousFrameSnapshot& s) {
  BinaryCatalogMetadata fields;
  fields.identities = {{"database_uuid", c.database_uuid},
                       {"frame_uuid", s.frame_uuid},
                       {"child_transaction_uuid", s.child_transaction_uuid},
                       {"recovery_token_uuid", s.recovery_token_uuid}};
  fields.text = {{"frame_generation", std::to_string(s.frame_generation)},
                 {"child_transaction_number", std::to_string(s.child_transaction_number)},
                 {"state", std::to_string(static_cast<unsigned>(s.state))},
                 {"finality_sequence", std::to_string(s.finality_sequence)}};
  std::string payload;
  if (!EncodeBinaryCatalogMetadata(fields, "autonomous.frame.journal.v2", &payload)) return false;
  const auto bytes = EncodeMgaMetadataFields({"autonomous.frame.journal.v2", payload});
  if (bytes.empty()) return false;
  std::ofstream out(c.database_path + ".sb.sblr_autonomous_frame.v1", std::ios::binary | std::ios::app);
  if (!out) return false;
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.flush(); return bool(out);
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
  if (!LoadProjections(c))
    return Diagnostic("PSQL.AUTONOMOUS_DESCRIPTOR_INVALID", "sblr.psql_autonomous.projection_journal_invalid");
  if (projections.contains(key))
    return Diagnostic("PSQL.AUTONOMOUS_DESCRIPTOR_INVALID", "sblr.psql_autonomous.projection_duplicate");
  if (!AppendProjection(c, 'P', authority))
    return Diagnostic("PSQL.AUTONOMOUS_TRANSACTION_REFUSED", "sblr.psql_autonomous.projection_publish_failed");
  projections.emplace(key, authority);
  return Diagnostic("OK", "ok");
}

EngineApiDiagnostic CompileAndPublishSblrAutonomousBodyFrameProjection(
    const EngineRequestContext& c, const EngineUuid& receipt,
    std::uint64_t occurrence) {
  if (!HasTag(c, "private_psql_autonomous_body_compiler") ||
      !c.statement_metadata_snapshot_engine_owned || receipt != c.statement_uuid ||
      !occurrence || c.transaction_uuid.is_nil() ||
      c.database_uuid.is_nil() || c.session_uuid.is_nil() ||
      c.principal_uuid.is_nil())
    return Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.compiler_hidden");
  SblrAutonomousBodyFrameProjectionV1 a;
  a.preliminary_receipt_uuid=receipt;a.structural_occurrence_id=occurrence;
  a.parent_transaction_uuid=c.transaction_uuid;
  a.parent_frame_uuid=Identity(++generation);
  a.database_uuid=c.database_uuid;
  a.attachment_uuid=Identity(++generation);
  a.session_uuid=c.session_uuid;a.principal_uuid=c.principal_uuid;
  a.security_snapshot_uuid=Identity(++generation);a.policy_snapshot_uuid=Identity(++generation);
  a.catalog_generation=++generation;a.capability_generation=++generation;
  a.body_sblr_uuid=Identity(++generation);a.intent=1;a.nesting_depth=1;a.effect_count=0;
  std::string material;
  material.append(reinterpret_cast<const char*>(receipt.bytes.data()), receipt.bytes.size());
  AppendBinaryU64(&material, occurrence);
  material.append(reinterpret_cast<const char*>(a.body_sblr_uuid.bytes.data()), a.body_sblr_uuid.bytes.size());
  a.body_sblr_sha256=ShaMaterial("ScratchBird.SblrPsqlAutonomousBody.V1"+material);
  a.effect_set_sha256=ShaMaterial("ScratchBird.SblrPsqlAutonomousAllowedEffectSet.V1");
  a.projection_evidence_sha256=ShaMaterial(
      "ScratchBird.SblrPsqlAutonomousBodyFrameProjection.V1"+material);
  return PublishSblrAutonomousBodyFrameProjection(c,a);
}

EngineApiDiagnostic RevokeSblrAutonomousBodyFrameProjection(
    const EngineRequestContext& c, const EngineUuid& receipt) {
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
    const EngineRequestContext& c, const EngineUuid& receipt,
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
    if (!LoadProjections(c)) {
      out.diagnostic = Diagnostic("PSQL.AUTONOMOUS_DESCRIPTOR_INVALID", "sblr.psql_autonomous.projection_journal_invalid");
      return out;
    }
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
    const EngineRequestContext& c, const EngineUuid& id,
    std::uint64_t frame_generation, bool commit) {
  std::lock_guard lock(mutex);
  SblrAutonomousFrameCoordinatorResult out;
  if (!HasTag(c, "private_psql_autonomous_frame_coordination")) {
    out.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED", "sblr.psql_autonomous.hidden"); return out;
  }
  auto it = rows.find(id);
  if (it == rows.end() || it->second.authority.parent_transaction_uuid != c.transaction_uuid) {
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
