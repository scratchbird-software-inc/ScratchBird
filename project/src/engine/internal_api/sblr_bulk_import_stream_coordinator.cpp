#include "sblr_bulk_import_stream_coordinator.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

using Uuid = engine::sblr::BulkImportUuid;
using Sha = engine::sblr::BulkImportSha;
using Descriptor = engine::sblr::SblrBulkImportStreamDescriptorV1;

constexpr std::string_view kAuthorityDomain =
    "ScratchBird.BulkImportStreamPrivateAuthority.V1";
constexpr std::string_view kCopyImportExport =
    "SBSQL-465931ED7427";
constexpr std::string_view kCopyStatement =
    "SBSQL-4F912014EA85";

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

bool NonZero(const Uuid& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool NonZero(const Sha& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool HasTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

bool CanonicalUuid(std::string_view text, Uuid* output) {
  if (!output || text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::UuidToString(parsed.value) != text)
    return false;
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), output->begin());
  return NonZero(*output);
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index)
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index)
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

template <std::size_t N>
void PutFixed(std::vector<std::uint8_t>* out,
              const std::array<std::uint8_t, N>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

Sha AuthorityEvidence(const SblrBulkImportStreamAuthorityInputV1& value) {
  std::vector<std::uint8_t> material(kAuthorityDomain.begin(),
                                     kAuthorityDomain.end());
  PutFixed(&material, value.authenticated_receipt_uuid);
  PutU16(&material,
         static_cast<std::uint16_t>(value.admitted_command_surface_id.size()));
  material.insert(material.end(), value.admitted_command_surface_id.begin(),
                  value.admitted_command_surface_id.end());
  PutFixed(&material, value.binding_uuid);
  PutU64(&material, value.binding_generation);
  PutU64(&material, value.structural_occurrence);
  PutU32(&material, value.import_occurrence);
  PutFixed(&material, value.syntax_demand_sha256);
  PutFixed(&material, value.binding_evidence_sha256);
  PutFixed(&material, value.target_relation_uuid);
  PutU64(&material, value.target_relation_generation);
  PutFixed(&material, value.owning_transaction_uuid);
  PutU64(&material, value.owning_local_transaction_id);
  PutFixed(&material, value.statement_snapshot_uuid);
  PutFixed(&material, value.catalog_epoch_uuid);
  PutU64(&material, value.catalog_generation);
  PutFixed(&material, value.security_context_uuid);
  PutU64(&material, value.security_epoch);
  PutFixed(&material, value.policy_snapshot_uuid);
  PutU64(&material, value.policy_generation);
  PutFixed(&material, value.import_policy_bundle_sha256);
  PutFixed(&material, value.route_snapshot_uuid);
  PutU64(&material, value.route_generation);
  PutFixed(&material, value.row_shape_uuid);
  PutU64(&material, value.row_shape_generation);
  PutFixed(&material, value.column_descriptor_set_sha256);
  PutFixed(&material, value.resource_grant_uuid);
  PutU64(&material, value.resource_grant_generation);
  material.push_back(value.cluster_bound ? 1u : 0u);
  PutU64(&material, value.cluster_epoch);
  PutFixed(&material, value.cluster_fence_uuid);
  PutU64(&material, value.executor_availability_generation);
  PutU64(&material, value.effective_maximum_stream_bytes);
  PutU64(&material, value.effective_maximum_chunk_count);
  PutU32(&material, value.effective_maximum_chunk_bytes);
  PutU64(&material, value.effective_maximum_rows);
  PutU32(&material, value.effective_maximum_target_columns);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

void WriteU32(std::array<std::uint8_t, 368>* body, std::size_t offset,
              std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index)
    (*body)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8));
}

void WriteU64(std::array<std::uint8_t, 368>* body, std::size_t offset,
              std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index)
    (*body)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8));
}

template <std::size_t N>
void WriteFixed(std::array<std::uint8_t, 368>* body, std::size_t offset,
                const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), body->begin() + offset);
}

bool BuildDescriptor(const BulkImportStreamAllocation& allocation,
                     Descriptor* output) {
  if (!output) return false;
  Descriptor descriptor;
  WriteFixed(&descriptor.canonical_body, 0,
             allocation.authenticated_receipt_uuid);
  WriteU64(&descriptor.canonical_body, 16, allocation.structural_occurrence);
  WriteU32(&descriptor.canonical_body, 24, allocation.import_occurrence);
  WriteU32(&descriptor.canonical_body, 28,
           allocation.cluster_bound ? 1u : 0u);
  WriteFixed(&descriptor.canonical_body, 32, allocation.stream_uuid);
  WriteU64(&descriptor.canonical_body, 48, allocation.stream_generation);
  WriteFixed(&descriptor.canonical_body, 56, allocation.target_relation_uuid);
  WriteU64(&descriptor.canonical_body, 72,
           allocation.target_relation_generation);
  WriteFixed(&descriptor.canonical_body, 80,
             allocation.owning_transaction_uuid);
  WriteU64(&descriptor.canonical_body, 96,
           allocation.owning_local_transaction_id);
  WriteFixed(&descriptor.canonical_body, 104,
             allocation.statement_snapshot_uuid);
  WriteFixed(&descriptor.canonical_body, 120,
             allocation.catalog_epoch_uuid);
  WriteU64(&descriptor.canonical_body, 136, allocation.catalog_generation);
  WriteFixed(&descriptor.canonical_body, 144,
             allocation.security_context_uuid);
  WriteU64(&descriptor.canonical_body, 160, allocation.security_epoch);
  WriteFixed(&descriptor.canonical_body, 168,
             allocation.policy_snapshot_uuid);
  WriteU64(&descriptor.canonical_body, 184, allocation.policy_generation);
  WriteFixed(&descriptor.canonical_body, 192,
             allocation.route_snapshot_uuid);
  WriteU64(&descriptor.canonical_body, 208, allocation.route_generation);
  WriteFixed(&descriptor.canonical_body, 216,
             allocation.recovery_operation_uuid);
  WriteU64(&descriptor.canonical_body, 232, allocation.recovery_generation);
  WriteFixed(&descriptor.canonical_body, 240, allocation.row_shape_uuid);
  WriteU64(&descriptor.canonical_body, 256, allocation.row_shape_generation);
  WriteFixed(&descriptor.canonical_body, 264,
             allocation.column_descriptor_set_sha256);
  WriteFixed(&descriptor.canonical_body, 296,
             allocation.import_policy_bundle_sha256);
  WriteFixed(&descriptor.canonical_body, 328,
             allocation.resource_grant_uuid);
  WriteU64(&descriptor.canonical_body, 344,
           allocation.resource_grant_generation);
  WriteFixed(&descriptor.canonical_body, 352,
             allocation.cluster_fence_uuid);
  descriptor.evidence = allocation.descriptor_evidence;
  descriptor.availability_generation =
      allocation.executor_availability_generation;
  const auto wire = engine::sblr::EncodeSblrBulkImportStreamDescriptorV1(
      descriptor, false);
  Descriptor decoded;
  if (wire.size() != engine::sblr::BulkImportWireLayout::descriptor_size ||
      !engine::sblr::DecodeSblrBulkImportStreamDescriptorV1(
          wire.data(), wire.size(), &decoded, nullptr, false)) {
    return false;
  }
  *output = decoded;
  return true;
}

Uuid NewIdentity(std::uint64_t salt) {
  static std::atomic<std::uint64_t> ordinal{0};
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object,
      now + salt + ordinal.fetch_add(1, std::memory_order_relaxed));
  Uuid result{};
  if (generated.ok()) {
    std::copy(generated.value.value.bytes.begin(),
              generated.value.value.bytes.end(), result.begin());
  }
  return result;
}

bool ExactCommand(std::string_view value) {
  return value == kCopyImportExport || value == kCopyStatement;
}

bool AuthorityShape(const SblrBulkImportStreamAuthorityInputV1& value) {
  const bool cluster_shape =
      value.cluster_bound
          ? value.cluster_epoch != 0 && NonZero(value.cluster_fence_uuid)
          : value.cluster_epoch == 0 && !NonZero(value.cluster_fence_uuid);
  return ExactCommand(value.admitted_command_surface_id) &&
         value.admitted_command_surface_id.size() <= 0xffffu &&
         NonZero(value.authenticated_receipt_uuid) &&
         NonZero(value.binding_uuid) && value.binding_generation != 0 &&
         value.structural_occurrence != 0 && value.import_occurrence != 0 &&
         NonZero(value.syntax_demand_sha256) &&
         NonZero(value.binding_evidence_sha256) &&
         NonZero(value.target_relation_uuid) &&
         value.target_relation_generation != 0 &&
         NonZero(value.owning_transaction_uuid) &&
         value.owning_local_transaction_id != 0 &&
         NonZero(value.statement_snapshot_uuid) &&
         NonZero(value.catalog_epoch_uuid) && value.catalog_generation != 0 &&
         NonZero(value.security_context_uuid) && value.security_epoch != 0 &&
         NonZero(value.policy_snapshot_uuid) && value.policy_generation != 0 &&
         NonZero(value.import_policy_bundle_sha256) &&
         NonZero(value.route_snapshot_uuid) && value.route_generation != 0 &&
         NonZero(value.row_shape_uuid) && value.row_shape_generation != 0 &&
         NonZero(value.column_descriptor_set_sha256) &&
         NonZero(value.resource_grant_uuid) &&
         value.resource_grant_generation != 0 && cluster_shape &&
         value.effective_maximum_stream_bytes != 0 &&
         value.effective_maximum_stream_bytes <=
             kBulkImportStreamMaximumBytesV1 &&
         value.effective_maximum_chunk_count != 0 &&
         value.effective_maximum_chunk_count <=
             kBulkImportStreamMaximumChunksV1 &&
         value.effective_maximum_chunk_bytes != 0 &&
         value.effective_maximum_chunk_bytes <=
             kBulkImportStreamMaximumChunkBytesV1 &&
         value.effective_maximum_rows != 0 &&
         value.effective_maximum_rows <= kBulkImportStreamMaximumRowsV1 &&
         value.effective_maximum_target_columns != 0 &&
         value.effective_maximum_target_columns <= 65535u;
}

bool ContextMatches(const EngineRequestContext& context,
                    const SblrBulkImportStreamAuthorityInputV1& value) {
  Uuid receipt{};
  Uuid transaction{};
  Uuid snapshot{};
  Uuid catalog{};
  Uuid security{};
  Uuid resource{};
  return CanonicalUuid(context.statement_receipt_uuid.canonical, &receipt) &&
         receipt == value.authenticated_receipt_uuid &&
         CanonicalUuid(context.transaction_uuid.canonical, &transaction) &&
         transaction == value.owning_transaction_uuid &&
         context.local_transaction_id == value.owning_local_transaction_id &&
         CanonicalUuid(context.statement_snapshot_uuid.canonical, &snapshot) &&
         snapshot == value.statement_snapshot_uuid &&
         CanonicalUuid(context.catalog_epoch_uuid.canonical, &catalog) &&
         catalog == value.catalog_epoch_uuid &&
         context.catalog_generation_id == value.catalog_generation &&
         CanonicalUuid(context.authorization_context.authority_uuid.canonical,
                       &security) &&
         security == value.security_context_uuid &&
         context.authorization_context.security_epoch == value.security_epoch &&
         CanonicalUuid(context.resource_admission_uuid.canonical, &resource) &&
         resource == value.resource_grant_uuid &&
         context.resource_epoch == value.resource_grant_generation;
}

std::string DescriptorKey(const Sha& value) {
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

// Kept only until the terminal executor is switched to the durable registry.
std::mutex legacy_mutex;
std::map<std::string, Descriptor> legacy_live;
std::map<std::string, Descriptor> legacy_used;

}  // namespace

SblrBulkImportStreamCoordinationResult
CoordinateDurableSblrBulkImportStreamDescriptorV1(
    const EngineRequestContext& context,
    SblrBulkImportStreamRegistry& registry,
    const SblrBulkImportStreamAuthorityInputV1& authority) {
  SblrBulkImportStreamCoordinationResult result;
  const auto refuse = [&](std::string code, std::string key,
                          std::string detail = {}) {
    result.diagnostic = Diagnostic(std::move(code), std::move(key),
                                   std::move(detail));
    return result;
  };

  if (!context.security_context_present ||
      !context.authorization_context.present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !HasTag(context, "private_bulk_import_stream_compiler")) {
    return refuse("SECURITY.ACCESS_DENIED",
                  "sblr.bulk_import_stream.coordination_hidden");
  }
  if (!AuthorityShape(authority)) {
    return refuse("SBLR.OPERAND_INVALID",
                  "sblr.bulk_import_stream.authority_invalid");
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return refuse("MGA.TRANSACTION_INVALID",
                  "sblr.bulk_import_stream.transaction_invalid");
  }
  if (!ContextMatches(context, authority)) {
    return refuse("MGA.AUTHORITY_MISMATCH",
                  "sblr.bulk_import_stream.receipt_authority_mismatch");
  }
  if (authority.cluster_bound || context.cluster_authority_available ||
      context.cluster_transaction_active || context.route_fence_present) {
    return refuse("CLUSTER.WRITE_AUTHORITY_REQUIRED",
                  "sblr.bulk_import_stream.cluster_authority_required");
  }
  if (authority.executor_availability_generation == 0) {
    return refuse("SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                  "sblr.bulk_import_stream.executor_evidence_missing");
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED",
                  "sblr.bulk_import_stream.coordination_cancelled");
  }
  if (!registry.healthy()) {
    return refuse("BULK.IMPORT.RECOVERY_CONFLICT",
                  "sblr.bulk_import_stream.registry_unavailable",
                  registry.startup_error());
  }

  const Sha authority_evidence = AuthorityEvidence(authority);
  if (!NonZero(authority_evidence)) {
    return refuse("BULK.IMPORT.ABORTED",
                  "sblr.bulk_import_stream.authority_evidence_failed");
  }

  const auto allocated = registry.AllocateOrReplay(
      authority.authenticated_receipt_uuid, authority.structural_occurrence,
      authority.import_occurrence, authority_evidence,
      [&](BulkImportStreamAllocation* allocation) {
        if (!allocation) return false;
        allocation->authenticated_receipt_uuid =
            authority.authenticated_receipt_uuid;
        allocation->stream_uuid = NewIdentity(1);
        allocation->stream_generation = 1;
        allocation->structural_occurrence = authority.structural_occurrence;
        allocation->import_occurrence = authority.import_occurrence;
        allocation->authority_evidence_sha256 = authority_evidence;
        allocation->syntax_demand_sha256 = authority.syntax_demand_sha256;
        allocation->durable_spool_uuid = NewIdentity(2);
        allocation->durable_spool_generation = 1;
        allocation->target_relation_uuid = authority.target_relation_uuid;
        allocation->target_relation_generation =
            authority.target_relation_generation;
        allocation->owning_transaction_uuid =
            authority.owning_transaction_uuid;
        allocation->owning_local_transaction_id =
            authority.owning_local_transaction_id;
        allocation->statement_snapshot_uuid = authority.statement_snapshot_uuid;
        allocation->catalog_epoch_uuid = authority.catalog_epoch_uuid;
        allocation->catalog_generation = authority.catalog_generation;
        allocation->security_context_uuid = authority.security_context_uuid;
        allocation->security_epoch = authority.security_epoch;
        allocation->policy_snapshot_uuid = authority.policy_snapshot_uuid;
        allocation->policy_generation = authority.policy_generation;
        allocation->route_snapshot_uuid = authority.route_snapshot_uuid;
        allocation->route_generation = authority.route_generation;
        allocation->recovery_operation_uuid = NewIdentity(3);
        allocation->recovery_generation = 1;
        allocation->row_shape_uuid = authority.row_shape_uuid;
        allocation->row_shape_generation = authority.row_shape_generation;
        allocation->column_descriptor_set_sha256 =
            authority.column_descriptor_set_sha256;
        allocation->import_policy_bundle_sha256 =
            authority.import_policy_bundle_sha256;
        allocation->resource_grant_uuid = authority.resource_grant_uuid;
        allocation->resource_grant_generation =
            authority.resource_grant_generation;
        allocation->cluster_bound = authority.cluster_bound;
        allocation->cluster_epoch = authority.cluster_epoch;
        allocation->cluster_fence_uuid = authority.cluster_fence_uuid;
        allocation->executor_availability_generation =
            authority.executor_availability_generation;
        allocation->effective_maximum_stream_bytes =
            authority.effective_maximum_stream_bytes;
        allocation->effective_maximum_chunk_count =
            authority.effective_maximum_chunk_count;
        allocation->effective_maximum_chunk_bytes =
            authority.effective_maximum_chunk_bytes;
        allocation->effective_maximum_rows = authority.effective_maximum_rows;
        allocation->effective_maximum_target_columns =
            authority.effective_maximum_target_columns;
        Descriptor descriptor;
        if (!NonZero(allocation->stream_uuid) ||
            !NonZero(allocation->durable_spool_uuid) ||
            !NonZero(allocation->recovery_operation_uuid) ||
            allocation->stream_uuid == allocation->durable_spool_uuid ||
            allocation->stream_uuid == allocation->recovery_operation_uuid ||
            allocation->durable_spool_uuid ==
                allocation->recovery_operation_uuid ||
            !BuildDescriptor(*allocation, &descriptor)) {
          return false;
        }
        allocation->descriptor_evidence = descriptor.evidence;
        return NonZero(allocation->descriptor_evidence);
      });

  if (!allocated.ok) {
    if (allocated.error == "allocation_authority_conflict") {
      return refuse("BULK.IMPORT.RECOVERY_CONFLICT",
                    "sblr.bulk_import_stream.allocation_conflict");
    }
    return refuse("BULK.IMPORT.ABORTED",
                  "sblr.bulk_import_stream.allocation_failed",
                  allocated.error);
  }
  if (!BuildDescriptor(allocated.allocation, &result.descriptor) ||
      result.descriptor.evidence != allocated.allocation.descriptor_evidence) {
    return refuse("BULK.IMPORT.RECOVERY_CONFLICT",
                  "sblr.bulk_import_stream.descriptor_replay_invalid");
  }
  result.ok = true;
  result.replayed = allocated.replayed;
  result.allocation = allocated.allocation;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrBulkImportStreamCoordinationResult CompileSblrBulkImportStreamDescriptor(
    const EngineRequestContext& context, const std::string& receipt,
    std::uint64_t occurrence, std::uint32_t import_occurrence,
    std::uint64_t availability) {
  std::lock_guard lock(legacy_mutex);
  SblrBulkImportStreamCoordinationResult result;
  if (!context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !HasTag(context, "private_bulk_import_stream_compiler") ||
      receipt != context.statement_uuid.canonical || occurrence == 0 ||
      import_occurrence == 0 || availability == 0) {
    result.diagnostic =
        Diagnostic("SBLR.OPERAND_INVALID",
                   "sblr.bulk_import_stream.legacy_coordination_invalid");
    return result;
  }
  result.descriptor.canonical_body[0] = 1;
  result.descriptor.canonical_body[1] = static_cast<std::uint8_t>(occurrence);
  result.descriptor.canonical_body[2] =
      static_cast<std::uint8_t>(import_occurrence);
  result.descriptor.availability_generation = availability;
  const auto bytes = engine::sblr::EncodeSblrBulkImportStreamDescriptorV1(
      result.descriptor, false);
  if (bytes.empty() || !engine::sblr::DecodeSblrBulkImportStreamDescriptorV1(
                           bytes.data(), bytes.size(), &result.descriptor,
                           nullptr, false)) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.bulk_import_stream.legacy_descriptor_invalid");
    return result;
  }
  legacy_live[DescriptorKey(result.descriptor.evidence)] = result.descriptor;
  result.ok = true;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

SblrBulkImportStreamCoordinationResult ConsumeSblrBulkImportStreamDescriptor(
    const EngineRequestContext& context, const Descriptor& descriptor) {
  std::lock_guard lock(legacy_mutex);
  SblrBulkImportStreamCoordinationResult result;
  const auto key = DescriptorKey(descriptor.evidence);
  const auto found = legacy_live.find(key);
  if (!context.security_context_present ||
      !HasTag(context, "private_bulk_import_stream")) {
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                   "sblr.bulk_import_stream.hidden");
    return result;
  }
  if (found == legacy_live.end()) {
    result.diagnostic = legacy_used.count(key)
                            ? Diagnostic("MGA.AUTHORITY_MISMATCH",
                                         "sblr.bulk_import_stream.stale")
                            : Diagnostic("SECURITY.ACCESS_DENIED",
                                         "sblr.bulk_import_stream.hidden");
    return result;
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    result.diagnostic = Diagnostic("PROCESS.CANCELLED",
                                   "sblr.bulk_import_stream.cancelled");
    return result;
  }
  legacy_used[key] = descriptor;
  legacy_live.erase(found);
  result.ok = true;
  result.descriptor = descriptor;
  result.diagnostic = Diagnostic("OK", "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api
