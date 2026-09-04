#include "core/hash/hash_digest.hpp"
#include "engine/internal_api/sblr_bulk_import_stream_coordinator.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <unistd.h>

namespace {

namespace api = scratchbird::engine::internal_api;
using Uuid = scratchbird::engine::sblr::BulkImportUuid;
using Sha = scratchbird::engine::sblr::BulkImportSha;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "bulk_import_stream_cancellation: " << detail << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool value, std::string_view detail) {
  if (!value) Fail(detail);
}

Uuid Id(const char* text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(text);
  Require(parsed.ok(), "fixture UUID was invalid");
  Uuid result{};
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            result.begin());
  return result;
}

Sha Hash(const char* text) {
  return scratchbird::core::hash::ComputeSha256Digest(
             reinterpret_cast<const std::uint8_t*>(text), std::strlen(text))
      .digest;
}

std::filesystem::path UniqueRoot() {
  std::array<char, 64> pattern{};
  constexpr std::string_view prefix =
      "/tmp/sb_bulk_import_cancel_ia07_XXXXXX";
  std::copy(prefix.begin(), prefix.end(), pattern.begin());
  Require(::mkdtemp(pattern.data()) != nullptr, "mkdtemp failed");
  return pattern.data();
}

api::SblrBulkImportStreamAuthorityInputV1 Authority() {
  api::SblrBulkImportStreamAuthorityInputV1 authority;
  authority.authenticated_receipt_uuid =
      Id("10000000-0000-4000-8000-000000000001");
  authority.admitted_command_surface_id = "SBSQL-465931ED7427";
  authority.binding_uuid = Id("10000000-0000-4000-8000-000000000002");
  authority.binding_generation = 3;
  authority.structural_occurrence = 1;
  authority.import_occurrence = 1;
  authority.syntax_demand_sha256 = Hash("syntax");
  authority.binding_evidence_sha256 = Hash("binding");
  authority.target_relation_uuid =
      Id("10000000-0000-4000-8000-000000000003");
  authority.target_relation_generation = 4;
  authority.owning_transaction_uuid =
      Id("10000000-0000-4000-8000-000000000004");
  authority.owning_local_transaction_id = 5;
  authority.statement_snapshot_uuid =
      Id("10000000-0000-4000-8000-000000000005");
  authority.catalog_epoch_uuid =
      Id("10000000-0000-4000-8000-000000000006");
  authority.catalog_generation = 7;
  authority.security_context_uuid =
      Id("10000000-0000-4000-8000-000000000007");
  authority.security_epoch = 8;
  authority.policy_snapshot_uuid =
      Id("10000000-0000-4000-8000-000000000008");
  authority.policy_generation = 9;
  authority.import_policy_bundle_sha256 = Hash("policy");
  authority.route_snapshot_uuid =
      Id("10000000-0000-4000-8000-000000000009");
  authority.route_generation = 10;
  authority.row_shape_uuid = Id("10000000-0000-4000-8000-00000000000a");
  authority.row_shape_generation = 11;
  authority.column_descriptor_set_sha256 = Hash("columns");
  authority.resource_grant_uuid =
      Id("10000000-0000-4000-8000-00000000000b");
  authority.resource_grant_generation = 12;
  authority.executor_availability_generation = 13;
  authority.effective_maximum_stream_bytes = 1024;
  authority.effective_maximum_chunk_count = 16;
  authority.effective_maximum_chunk_bytes = 128;
  authority.effective_maximum_rows = 64;
  authority.effective_maximum_target_columns = 8;
  return authority;
}

api::EngineRequestContext Context(
    const api::SblrBulkImportStreamAuthorityInputV1& authority) {
  api::EngineRequestContext context;
  context.statement_receipt_uuid.canonical =
      "10000000-0000-4000-8000-000000000001";
  context.transaction_uuid.canonical =
      "10000000-0000-4000-8000-000000000004";
  context.local_transaction_id = authority.owning_local_transaction_id;
  context.statement_snapshot_uuid.canonical =
      "10000000-0000-4000-8000-000000000005";
  context.catalog_epoch_uuid.canonical =
      "10000000-0000-4000-8000-000000000006";
  context.catalog_generation_id = authority.catalog_generation;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "10000000-0000-4000-8000-000000000007";
  context.authorization_context.security_epoch = authority.security_epoch;
  context.resource_admission_uuid.canonical =
      "10000000-0000-4000-8000-00000000000b";
  context.resource_epoch = authority.resource_grant_generation;
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_bulk_import_stream_compiler"};
  return context;
}

}  // namespace

int main() {
  const auto root = UniqueRoot();
  const auto authority = Authority();
  auto context = Context(authority);
  {
    api::SblrBulkImportStreamRegistry registry(root);
    Require(registry.healthy(), "durable registry was unavailable");

    context.query_cancellation_requested = [] { return true; };
    const auto cancelled =
        api::CoordinateDurableSblrBulkImportStreamDescriptorV1(
            context, registry, authority);
    Require(!cancelled.ok && cancelled.diagnostic.code == "PROCESS.CANCELLED",
            "cancellation did not refuse at the pre-allocation boundary");
    Require(std::distance(std::filesystem::directory_iterator(root),
                          std::filesystem::directory_iterator()) == 1,
            "cancelled coordination published durable stream state");

    context.query_cancellation_requested = [] { return false; };
    const auto admitted =
        api::CoordinateDurableSblrBulkImportStreamDescriptorV1(
            context, registry, authority);
    Require(admitted.ok && !admitted.replayed,
            "retry after cancellation did not allocate exactly once");
    const auto replay =
        api::CoordinateDurableSblrBulkImportStreamDescriptorV1(
            context, registry, authority);
    Require(replay.ok && replay.replayed &&
                replay.descriptor.canonical_body ==
                    admitted.descriptor.canonical_body &&
                replay.descriptor.evidence == admitted.descriptor.evidence,
            "exact post-cancellation retry was not byte-identical");
  }
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  Require(!cleanup_error, "fixture cleanup failed");
  return EXIT_SUCCESS;
}
