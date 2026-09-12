// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_source_map_descriptor_registry.hpp"
#include "uuid.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
static unsigned checks = 0;
static void Check(bool ok, const char* detail) {
  ++checks;
  if (!ok) throw std::runtime_error(detail);
}
static std::string Identity(platform::UuidKind kind) {
  static auto time = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  if (uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto id = uuid::GenerateEngineIdentityV7(kind, ++time);
    Check(id.ok(), "fixture durable system identity generation failed");
    return uuid::UuidToString(id.value.value);
  }
  const auto raw = uuid::GenerateCompatibilityUnixTimeV7(++time);
  Check(raw.ok(), "fixture process-local v7 generation failed");
  const auto typed = uuid::MakeTypedUuid(kind, raw.value);
  Check(typed.ok(), "fixture process-local identity typing failed");
  return uuid::UuidToString(typed.value.value);
}
struct Files {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("sb-source-map-" + Identity(platform::UuidKind::object));
  Files() { Check(std::filesystem::create_directory(directory), "fixture directory collision"); }
  ~Files() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};

int main() {
  try {
    Files files;
    EngineRequestContext context;
    context.database_path = (files.directory / "registry").string();
    context.database_uuid.canonical = Identity(platform::UuidKind::database);
    context.session_uuid.canonical = Identity(platform::UuidKind::session);
    context.transaction_uuid.canonical = Identity(platform::UuidKind::transaction);
    context.security_context_present = true;
    context.statement_metadata_snapshot_engine_owned = true;
    context.trace_tags = {"private_source_map_registry"};
    const auto receipt = Identity(platform::UuidKind::object);
    const auto snapshot = Identity(platform::UuidKind::object);
    const auto artifact = Identity(platform::UuidKind::object);
    scratchbird::engine::sblr::SblrSourceMapEntryV1 entry;
    entry.node_id = 1;
    entry.source_artifact_uuid = uuid::ParseUuid(artifact).value.bytes;
    entry.source_artifact_generation = 1;
    entry.byte_length = 4;
    const auto issued = IssueSblrSourceMapDescriptorV1(
        context, receipt, "sha256:" + std::string(64, 'a'), snapshot, 1, {entry});
    Check(issued.ok && !issued.diagnostic.error && issued.diagnostic.code == "OK" &&
              !issued.snapshot.canonical_smvd.empty(),
          "successful source-map issuance carried an error diagnostic");
    const auto found = LookupSblrSourceMapDescriptorV1(
        context, receipt, issued.snapshot.descriptor_uuid, 1,
        issued.snapshot.bound_ast_sha256, snapshot, issued.snapshot.registry_generation);
    Check(found.ok && !found.diagnostic.error, "source-map live lookup failed");
    const auto stale = LookupSblrSourceMapDescriptorV1(
        context, receipt, issued.snapshot.descriptor_uuid, 1,
        "sha256:" + std::string(64, 'b'), snapshot, issued.snapshot.registry_generation);
    Check(!stale.ok && stale.diagnostic.error &&
              stale.diagnostic.code == "SBLR.SOURCE_MAP.STALE",
          "stale source-map binding was accepted");
    auto wrong = context;
    wrong.session_uuid.canonical = Identity(platform::UuidKind::session);
    Check(!LookupSblrSourceMapDescriptorV1(
              wrong, receipt, issued.snapshot.descriptor_uuid, 1,
              issued.snapshot.bound_ast_sha256, snapshot,
              issued.snapshot.registry_generation).ok,
          "foreign session read source-map descriptor");
    const auto path = context.database_path + ".sb.sblr_source_map_registry.v1";
    const auto before = std::filesystem::file_size(path);
    auto denied = context;
    denied.security_context_present = false;
    const auto denied_revoke = RevokeSblrSourceMapDescriptorsV1(denied, receipt, "receipt.release");
    Check(denied_revoke.error && denied_revoke.code == "SECURITY.ACCESS_DENIED" &&
              std::filesystem::file_size(path) == before,
          "unauthorized source-map revocation mutated registry");
    const auto revoked = RevokeSblrSourceMapDescriptorsV1(context, receipt, "receipt.release");
    Check(!revoked.error && revoked.code == "OK" && std::filesystem::file_size(path) > before,
          "successful durable source-map revocation carried an error diagnostic");
    Check(!LookupSblrSourceMapDescriptorV1(
              context, receipt, issued.snapshot.descriptor_uuid, 1,
              issued.snapshot.bound_ast_sha256, snapshot,
              issued.snapshot.registry_generation).ok,
          "revoked source-map descriptor remained live");
    const auto absent_revoke = RevokeSblrSourceMapDescriptorsV1(context, receipt, "receipt.release");
    Check(absent_revoke.error && absent_revoke.code == "SECURITY.ACCESS_DENIED",
          "general source-map revocation lost absent-object hiding");

    const auto live_receipt = Identity(platform::UuidKind::object);
    const auto live_issue = IssueSblrSourceMapDescriptorV1(
        context, live_receipt, "sha256:" + std::string(64, 'c'), snapshot, 1, {entry});
    Check(live_issue.ok && !live_issue.diagnostic.error, "recovery precondition issuance failed");
    auto admin = context;
    admin.trace_tags = {"right:SBLR_SOURCE_MAP_REGISTRY_ADMIN"};
    const auto recovered = RecoverSblrSourceMapDescriptorRegistryV1(admin);
    Check(!recovered.error && recovered.code == "OK", "successful recovery carried an error diagnostic");
    Check(!LookupSblrSourceMapDescriptorV1(
              context, live_receipt, live_issue.snapshot.descriptor_uuid, 1,
              live_issue.snapshot.bound_ast_sha256, snapshot,
              live_issue.snapshot.registry_generation).ok,
          "recovery did not invalidate source-map descriptor");
    std::cout << "SOURCE_MAP real registry " << checks << " checks PASS\n";
  } catch (const std::exception& error) {
    std::cerr << "SOURCE_MAP registry check " << checks << ": " << error.what() << '\n';
    return 1;
  }
}
