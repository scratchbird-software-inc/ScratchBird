#include "engine/internal_api/sblr_bulk_import_stream_coordinator.hpp"
#include "core/hash/hash_digest.hpp"
#include "uuid.hpp"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
using namespace scratchbird::engine::internal_api;
using Uuid = scratchbird::engine::sblr::BulkImportUuid;
using Sha = scratchbird::engine::sblr::BulkImportSha;

Uuid Id(const char* text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(text);
  assert(parsed.ok());
  Uuid out{};
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out.begin());
  return out;
}

Sha Hash(const char* text) {
  return scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)).digest;
}

std::filesystem::path Root() {
  std::array<char, 64> name{};
  std::copy_n("/tmp/sb_bulk_import_coord_XXXXXX", 32, name.begin());
  assert(::mkdtemp(name.data()) != nullptr);
  return name.data();
}

SblrBulkImportStreamAuthorityInputV1 Authority() {
  SblrBulkImportStreamAuthorityInputV1 a;
  a.authenticated_receipt_uuid = Id("10000000-0000-4000-8000-000000000001");
  a.admitted_command_surface_id = "SBSQL-465931ED7427";
  a.binding_uuid = Id("10000000-0000-4000-8000-000000000002");
  a.binding_generation = 3;
  a.structural_occurrence = 1;
  a.import_occurrence = 1;
  a.syntax_demand_sha256 = Hash("syntax");
  a.binding_evidence_sha256 = Hash("binding");
  a.target_relation_uuid = Id("10000000-0000-4000-8000-000000000003");
  a.target_relation_generation = 4;
  a.owning_transaction_uuid = Id("10000000-0000-4000-8000-000000000004");
  a.owning_local_transaction_id = 5;
  a.statement_snapshot_uuid = Id("10000000-0000-4000-8000-000000000005");
  a.catalog_epoch_uuid = Id("10000000-0000-4000-8000-000000000006");
  a.catalog_generation = 7;
  a.security_context_uuid = Id("10000000-0000-4000-8000-000000000007");
  a.security_epoch = 8;
  a.policy_snapshot_uuid = Id("10000000-0000-4000-8000-000000000008");
  a.policy_generation = 9;
  a.import_policy_bundle_sha256 = Hash("policy");
  a.route_snapshot_uuid = Id("10000000-0000-4000-8000-000000000009");
  a.route_generation = 10;
  a.row_shape_uuid = Id("10000000-0000-4000-8000-00000000000a");
  a.row_shape_generation = 11;
  a.column_descriptor_set_sha256 = Hash("columns");
  a.resource_grant_uuid = Id("10000000-0000-4000-8000-00000000000b");
  a.resource_grant_generation = 12;
  a.executor_availability_generation = 13;
  a.effective_maximum_stream_bytes = 1024;
  a.effective_maximum_chunk_count = 16;
  a.effective_maximum_chunk_bytes = 128;
  a.effective_maximum_rows = 64;
  a.effective_maximum_target_columns = 8;
  return a;
}

EngineRequestContext Context(const SblrBulkImportStreamAuthorityInputV1& a) {
  EngineRequestContext c;
  c.statement_receipt_uuid.canonical =
      "10000000-0000-4000-8000-000000000001";
  c.transaction_uuid.canonical = "10000000-0000-4000-8000-000000000004";
  c.local_transaction_id = a.owning_local_transaction_id;
  c.statement_snapshot_uuid.canonical = "10000000-0000-4000-8000-000000000005";
  c.catalog_epoch_uuid.canonical = "10000000-0000-4000-8000-000000000006";
  c.catalog_generation_id = a.catalog_generation;
  c.authorization_context.present = true;
  c.authorization_context.authority_uuid.canonical =
      "10000000-0000-4000-8000-000000000007";
  c.authorization_context.security_epoch = a.security_epoch;
  c.resource_admission_uuid.canonical =
      "10000000-0000-4000-8000-00000000000b";
  c.resource_epoch = a.resource_grant_generation;
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.trace_tags = {"private_bulk_import_stream_compiler"};
  return c;
}
}

int main() {
  const auto root = Root();
  auto authority = Authority();
  auto context = Context(authority);
  SblrBulkImportStreamRegistry registry(root);
  assert(registry.healthy());

  const auto first = CoordinateDurableSblrBulkImportStreamDescriptorV1(
      context, registry, authority);
  assert(first.ok && !first.replayed && first.descriptor.evidence != Sha{});
  const auto replay = CoordinateDurableSblrBulkImportStreamDescriptorV1(
      context, registry, authority);
  assert(replay.ok && replay.replayed &&
         replay.descriptor.canonical_body == first.descriptor.canonical_body &&
         replay.descriptor.evidence == first.descriptor.evidence &&
         replay.allocation.stream_uuid == first.allocation.stream_uuid);

  auto conflicting = authority;
  conflicting.binding_evidence_sha256[0] ^= 1;
  const auto conflict = CoordinateDurableSblrBulkImportStreamDescriptorV1(
      context, registry, conflicting);
  assert(!conflict.ok && conflict.diagnostic.code == "BULK.IMPORT.RECOVERY_CONFLICT");

  auto hidden = context;
  hidden.security_context_present = false;
  assert(!CoordinateDurableSblrBulkImportStreamDescriptorV1(
              hidden, registry, authority).ok);
  auto transaction_mismatch = context;
  transaction_mismatch.transaction_uuid.canonical =
      "10000000-0000-4000-8000-000000000099";
  assert(!CoordinateDurableSblrBulkImportStreamDescriptorV1(
              transaction_mismatch, registry, authority).ok);
  auto cluster = context;
  cluster.cluster_authority_available = true;
  assert(!CoordinateDurableSblrBulkImportStreamDescriptorV1(
              cluster, registry, authority).ok);
  auto evidence_missing = authority;
  evidence_missing.executor_availability_generation = 0;
  assert(!CoordinateDurableSblrBulkImportStreamDescriptorV1(
              context, registry, evidence_missing).ok);

  const auto cancellation_root = Root();
  SblrBulkImportStreamRegistry cancellation_registry(cancellation_root);
  auto cancelled = context;
  cancelled.query_cancellation_requested = [] { return true; };
  assert(!CoordinateDurableSblrBulkImportStreamDescriptorV1(
              cancelled, cancellation_registry, authority).ok);
  assert(std::distance(std::filesystem::directory_iterator(cancellation_root),
                       std::filesystem::directory_iterator()) == 1);
  return 0;
}
