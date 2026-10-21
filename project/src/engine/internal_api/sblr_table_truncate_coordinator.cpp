#include "sblr_table_truncate_coordinator.hpp"
#include "../../core/uuid/uuid.hpp"
#include "api_diagnostics.hpp"
#include <string_view>
#include <array>
#include <vector>
#include "../../core/hash/hash_digest.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
namespace scratchbird::engine::internal_api{namespace{std::mutex m;std::unordered_map<std::string,scratchbird::engine::sblr::SblrTableTruncateDescriptorV1>live,used;std::string k(const auto&a){static const char*h="0123456789abcdef";std::string s;for(auto b:a){s+=h[b>>4];s+=h[b&15];}return s;}bool tag(const EngineRequestContext&c,const char*t){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),t)!=c.trace_tags.end();}auto d(std::string c,std::string x){return MakeEngineApiDiagnostic(std::move(c),std::move(x),{});}}
// SBSQL-TRUNCATE-TABLE-V1 section 6.1, SBLR_TABLE_TRUNCATE_CANONICAL_FIELDS_V1.
// Absolute wire offsets; the canonical body starts at wire offset 16, so body
// index = offset - 16. Every value below comes from the authenticated receipt
// context, the coordinator's admitted arguments, the engine-resolved target, or
// an engine-issued identity. Nothing is defaulted to satisfy a validator: the
// encoder revalidates the whole body and a refusal here is a refusal, not an ok
// with an unencodable frame.
namespace {
constexpr std::size_t kBodyBase = 16;
void PutUuid(std::array<std::uint8_t,368>& body, std::size_t offset,
             const EngineUuid& value) {
  std::copy(value.bytes.begin(), value.bytes.end(),
            body.begin() + (offset - kBodyBase));
}
void PutUint(std::array<std::uint8_t,368>& body, std::size_t offset,
             std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    body[offset - kBodyBase + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void PutSha(std::array<std::uint8_t,368>& body, std::size_t offset,
            const std::array<std::uint8_t,32>& digest) {
  std::copy(digest.begin(), digest.end(), body.begin() + (offset - kBodyBase));
}
// Domain-tagged so a digest computed for one field can never be accepted as
// another, and so copying a digest between carriers cannot pass.
bool Digest(std::string_view domain, const std::vector<std::uint8_t>& material,
            std::array<std::uint8_t,32>* out) {
  std::vector<std::uint8_t> tagged(domain.begin(), domain.end());
  tagged.insert(tagged.end(), material.begin(), material.end());
  const auto hashed = scratchbird::core::hash::ComputeSha256Digest(tagged);
  if (!hashed.ok() || hashed.digest_bytes != out->size()) return false;
  *out = hashed.digest;
  return true;
}
void AppendUuid(std::vector<std::uint8_t>& out, const EngineUuid& value) {
  out.insert(out.end(), value.bytes.begin(), value.bytes.end());
}
void AppendUint(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
bool Identity(const EngineUuid& value) {
  return scratchbird::core::uuid::IsEngineIdentityUuid(value);
}
}  // namespace

SblrTableTruncateCoordinationResult CompileSblrTableTruncateDescriptor(
    const EngineRequestContext& c, const EngineUuid& r, std::uint64_t o,
    std::uint32_t u, std::uint64_t a,
    const SblrTableTruncateResolvedTargetV1& target) {
  std::lock_guard l(m);
  SblrTableTruncateCoordinationResult x;
  if (!tag(c, "private_table_truncate_compiler") ||
      !c.statement_metadata_snapshot_engine_owned || !Identity(r) ||
      r != c.statement_uuid || !o || !u || !a) {
    x.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.table_truncate.coordination_invalid");
    return x;
  }
  // The engine-resolved target. Section 6.1: a carrier that satisfies count
  // checks without a matching frozen record is not authority, so a missing
  // target cannot be papered over.
  if (!Identity(target.persisted_descriptor_uuid) ||
      target.persisted_descriptor_generation == 0) {
    x.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.table_truncate.resolved_target_required");
    return x;
  }
  // The authenticated tuple the carrier must match at issuance and execution.
  if (!Identity(c.database_uuid) || !Identity(c.session_uuid) ||
      !Identity(c.transaction_uuid) || !Identity(c.statement_uuid) ||
      !Identity(c.statement_snapshot_uuid)) {
    x.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.table_truncate.authenticated_tuple_required");
    return x;
  }
  if (c.catalog_generation_id == 0 || c.security_epoch == 0) {
    x.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.table_truncate.captured_epochs_required");
    return x;
  }

  // Engine-issued identities for this issuance: the immutable descriptor and
  // its four proofs/intents. Distinct offsets keep them distinct values.
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto issued = [&](unsigned slot) {
    return scratchbird::core::uuid::GenerateEngineIdentityV7(
        scratchbird::core::platform::UuidKind::object, now + slot);
  };
  const auto descriptor_identity = issued(0);
  const auto security_proof = issued(1);
  const auto resource_proof = issued(2);
  const auto recovery_intent = issued(3);
  const auto cleanup_intent = issued(4);
  if (!descriptor_identity.ok() || !security_proof.ok() ||
      !resource_proof.ok() || !recovery_intent.ok() || !cleanup_intent.ok()) {
    x.diagnostic = d("SBLR.EXECUTION_FAILED",
                     "sblr.table_truncate.issued_identity_unavailable");
    return x;
  }

  // Section 2 defaults, and the only form this version resolves: one explicit
  // target, CONTINUE IDENTITY, RESTRICT, local scope, outer transaction scope.
  // The bind refuses RESTART/CASCADE/multi-target before reaching here.
  constexpr std::uint64_t kDescriptorGeneration = 1;
  constexpr std::uint8_t kIdentityContinue = 1;
  constexpr std::uint8_t kDependencyRestrict = 1;
  constexpr std::uint16_t kLocalScopeFlags = 0;
  constexpr std::uint32_t kExplicitTargets = 1;
  constexpr std::uint32_t kFinalTargets = 1;

  auto& body = x.descriptor.canonical_body;
  body.fill(0);
  PutUuid(body, 16, r);                             // preliminary receipt
  PutUuid(body, 32, c.database_uuid);               // database/node
  PutUuid(body, 48, c.session_uuid);                // session
  PutUuid(body, 64, c.transaction_uuid);            // transaction
  PutUuid(body, 80, c.statement_uuid);              // statement
  PutUuid(body, 96, c.statement_snapshot_uuid);     // statement snapshot
  PutUuid(body, 112, descriptor_identity.value.value);  // engine-owned descriptor
  PutUint(body, 128, kDescriptorGeneration, 8);
  PutUint(body, 136, o, 8);                         // structural occurrence
  PutUint(body, 144, u, 4);                         // truncate occurrence
  PutUint(body, 148, kIdentityContinue, 1);
  PutUint(body, 149, kDependencyRestrict, 1);
  PutUint(body, 150, kLocalScopeFlags, 2);
  PutUint(body, 152, kExplicitTargets, 4);
  PutUint(body, 156, kFinalTargets, 4);
  PutUint(body, 160, 0, 4);                         // FK edge count
  PutUint(body, 164, 0, 4);                         // partition edge count
  PutUint(body, 168, 0, 4);                         // ordered trigger effects
  PutUint(body, 172, 0, 4);                         // identity resets: zero for CONTINUE
  PutUint(body, 176, c.catalog_generation_id, 8);   // captured catalog epoch
  PutUint(body, 184, c.security_epoch, 8);          // captured policy epoch
  PutUint(body, 192, 0, 8);                         // route epoch: zero for local
  PutUint(body, 200, 0, 8);                         // savepoint ordinal: outer scope
  PutUuid(body, 272, security_proof.value.value);
  PutUuid(body, 288, resource_proof.value.value);
  PutUuid(body, 304, recovery_intent.value.value);
  PutUuid(body, 320, cleanup_intent.value.value);
  // 368..383 remain the required reserved zeros.

  // The closure digest covers the resolved target set and the dependency
  // decision that produced it, so a different target or action cannot reuse it.
  std::vector<std::uint8_t> closure;
  AppendUuid(closure, target.persisted_descriptor_uuid);
  AppendUint(closure, target.persisted_descriptor_generation);
  AppendUint(closure, kExplicitTargets);
  AppendUint(closure, kFinalTargets);
  AppendUint(closure, kDependencyRestrict);
  std::array<std::uint8_t,32> closure_digest{};
  // The semantic digest covers the normalized descriptor: the authenticated
  // tuple, the occurrences, the actions and the captured epochs.
  std::vector<std::uint8_t> semantic;
  AppendUuid(semantic, r);
  AppendUuid(semantic, c.database_uuid);
  AppendUuid(semantic, c.session_uuid);
  AppendUuid(semantic, c.transaction_uuid);
  AppendUuid(semantic, c.statement_uuid);
  AppendUuid(semantic, c.statement_snapshot_uuid);
  AppendUuid(semantic, descriptor_identity.value.value);
  AppendUint(semantic, o);
  AppendUint(semantic, u);
  AppendUint(semantic, kIdentityContinue);
  AppendUint(semantic, kDependencyRestrict);
  AppendUint(semantic, c.catalog_generation_id);
  AppendUint(semantic, c.security_epoch);
  std::array<std::uint8_t,32> semantic_digest{};
  if (!Digest("ScratchBird.SblrTableTruncateClosure.V1", closure,
              &closure_digest) ||
      !Digest("ScratchBird.SblrTableTruncateSemantic.V1", semantic,
              &semantic_digest)) {
    x.diagnostic = d("SBLR.EXECUTION_FAILED",
                     "sblr.table_truncate.descriptor_digest_unavailable");
    return x;
  }
  PutSha(body, 208, closure_digest);
  PutSha(body, 336, semantic_digest);
  // The frozen-payload digest binds the record as a whole, so it is computed
  // over the body with its own slot still zero and the proofs already placed.
  std::vector<std::uint8_t> frozen(body.begin(), body.end());
  std::array<std::uint8_t,32> frozen_digest{};
  if (!Digest("ScratchBird.SblrTableTruncateFrozenRecord.V1", frozen,
              &frozen_digest)) {
    x.diagnostic = d("SBLR.EXECUTION_FAILED",
                     "sblr.table_truncate.descriptor_digest_unavailable");
    return x;
  }
  PutSha(body, 240, frozen_digest);
  x.descriptor.availability_generation = a;

  // The encoder recomputes the evidence digest and revalidates every field of
  // section 6.1. An empty frame means the body is not a descriptor, so this
  // refuses instead of storing one and reporting ok.
  const auto encoded =
      scratchbird::engine::sblr::EncodeSblrTableTruncateDescriptorV1(
          x.descriptor, false);
  if (encoded.empty()) {
    x.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.table_truncate.descriptor_encode_failed");
    return x;
  }
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrTableTruncateDescriptorV1(
          encoded.data(), encoded.size(), &x.descriptor, &detail, false)) {
    x.diagnostic = d("SBLR.OPERAND_INVALID",
                     "sblr.table_truncate.descriptor_decode_failed");
    return x;
  }
  live[k(x.descriptor.evidence)] = x.descriptor;
  x.ok = true;
  x.diagnostic = d("OK", "ok");
  return x;
}
SblrTableTruncateCoordinationResult ConsumeSblrTableTruncateDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrTableTruncateDescriptorV1&v){std::lock_guard l(m);SblrTableTruncateCoordinationResult x;auto q=k(v.evidence);auto i=live.find(q);if(!tag(c,"private_table_truncate")){x.diagnostic=d("SECURITY.ACCESS_DENIED","sblr.table_truncate.hidden");return x;}if(i==live.end()){x.diagnostic=used.count(q)?d("MGA.TRANSACTION.STALE","sblr.table_truncate.stale"):d("SECURITY.ACCESS_DENIED","sblr.table_truncate.hidden");return x;}if(c.query_cancellation_requested&&c.query_cancellation_requested()){x.diagnostic=d("PROCESS.CANCELLED","sblr.table_truncate.cancelled");return x;}used[q]=v;live.erase(i);x.ok=true;x.descriptor=v;x.diagnostic=d("OK","ok");return x;}}
