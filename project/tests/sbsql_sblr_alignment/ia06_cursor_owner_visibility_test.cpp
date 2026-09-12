// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/sblr_cursor_open_coordinator.hpp"
#include "hash_digest.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace fs = std::filesystem;

namespace {
void Require(bool value, const std::string& detail) {
  if (!value) throw std::runtime_error(detail);
}

struct FixtureDirectory {
  fs::path path;
  FixtureDirectory() {
    std::random_device random;
    for (unsigned attempt = 0; attempt != 32; ++attempt) {
      const auto candidate = fs::temp_directory_path() /
          ("sb-cursor-owner-" + std::to_string(random()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      if (fs::create_directory(candidate)) {
        path = candidate;
        return;
      }
    }
    throw std::runtime_error("could not create isolated fixture directory");
  }
  ~FixtureDirectory() {
    // Only this fixture's uniquely created directory is removed.
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

std::string ReadJournal(const api::EngineRequestContext& context) {
  std::ifstream stream(context.database_path + ".sb.sblr_cursor_open.v1",
                       std::ios::binary);
  Require(stream.is_open(), "journal missing");
  const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
  Require(!stream.bad(), "journal read failed");
  return bytes;
}

std::string Evidence(const api::SblrCursorOpenSnapshot& snapshot) {
  const auto& value = snapshot.cursor_evidence_sha256;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      std::vector<std::uint8_t>(value.begin(), value.end()));
  Require(digest.ok(), "fixture evidence hash failed");
  return "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}

api::SblrCursorOpenResult Close(const api::EngineRequestContext& context,
                              const api::SblrCursorOpenSnapshot& cursor,
                              std::uint8_t reason) {
  return api::CloseSblrCursor(context, cursor.cursor_uuid,
      cursor.cursor_generation, cursor.position_generation, Evidence(cursor),
      1, reason);
}

void RequireHidden(const api::SblrCursorOpenResult& result) {
  Require(!result.ok && result.diagnostic.error &&
              result.diagnostic.code == "SECURITY.ACCESS_DENIED" &&
              result.diagnostic.message_key == "sblr.cursor.hidden" &&
              result.snapshot.cursor_uuid.empty() &&
              result.snapshot.cursor_evidence_sha256.empty() &&
              result.snapshot.cursor_generation == 0,
          "foreign caller distinguished a retired cursor from hidden identity");
}

void OwnerVisibility(std::uint8_t reason, const fs::path& directory) {
  api::EngineRequestContext owner;
  owner.database_path = (directory / ("database-" + std::to_string(reason))).string();
  owner.database_uuid.canonical = "019d0000-0000-7000-8000-000000006001";
  owner.statement_uuid.canonical = "019d0000-0000-7000-8000-000000006002";
  owner.session_uuid.canonical = "019d0000-0000-7000-8000-000000006003";
  owner.principal_uuid.canonical = "019d0000-0000-7000-8000-000000006004";
  owner.transaction_uuid.canonical = "019d0000-0000-7000-8000-000000006005";
  owner.security_context_present = true;
  owner.statement_metadata_snapshot_engine_owned = true;
  owner.trace_tags = {"private_executable_plan_receipt_compiler",
                      "private_cursor_open", "private_cursor_close"};

  // Known legacy synthetic-plan constructor: this fixture tests the actual
  // coordinator's ownership/refusal behavior, not public SBsql or real rows.
  const auto descriptor = api::CompileAndPublishSblrExecutablePlanReceipt(
      owner, owner.statement_uuid.canonical, reason, 1, 1, 64, 1);
  Require(descriptor.ok, "fixture descriptor publication failed");
  const auto opened = api::OpenSblrCursor(owner,
      descriptor.snapshot.descriptor_uuid,
      descriptor.snapshot.descriptor_generation,
      descriptor.snapshot.descriptor_evidence_sha256, 1);
  Require(opened.ok, "fixture cursor open failed");

  auto foreign = owner;
  foreign.session_uuid.canonical = "019d0000-0000-7000-8000-000000006006";
  unsigned foreign_cancellation_checks = 0;
  foreign.query_cancellation_requested = [&] {
    ++foreign_cancellation_checks;
    return true;
  };
  const auto before_close = ReadJournal(owner);
  RequireHidden(Close(foreign, opened.snapshot, reason));
  Require(ReadJournal(owner) == before_close,
          "foreign live probe modified the journal");

  const auto closed = Close(owner, opened.snapshot, reason);
  Require(closed.ok, "foreign live probe consumed the owner's cursor");
  const auto after_close = ReadJournal(owner);
  const auto expected_event = "C\t" + opened.snapshot.descriptor_uuid + "\t" +
      opened.snapshot.cursor_uuid + "\t" +
      std::to_string(opened.snapshot.cursor_generation) + "\n";
  Require(after_close == before_close + expected_event,
          "owning close did not append its real journal event");

  auto unknown = opened.snapshot;
  unknown.cursor_uuid = "019d0000-0000-7000-8000-00000000ffff";
  RequireHidden(Close(owner, unknown, reason));
  RequireHidden(Close(foreign, unknown, reason));
  for (unsigned probe = 0; probe != 32; ++probe) {
    // Both same-principal and different-principal foreign sessions stay hidden.
    foreign.principal_uuid.canonical = (probe % 2 == 0)
        ? owner.principal_uuid.canonical
        : "019d0000-0000-7000-8000-000000006007";
    RequireHidden(Close(foreign, opened.snapshot, reason));
    Require(ReadJournal(owner) == after_close,
            "foreign retired probe modified the journal");
  }

  auto unauthenticated = owner;
  unauthenticated.security_context_present = false;
  RequireHidden(Close(unauthenticated, opened.snapshot, reason));
  auto unprivileged = owner;
  unprivileged.trace_tags.clear();
  RequireHidden(Close(unprivileged, opened.snapshot, reason));
  const auto replay = Close(owner, opened.snapshot, reason);
  Require(!replay.ok && replay.diagnostic.code == "CURSOR.STALE" &&
              replay.diagnostic.message_key == "sblr.cursor.stale" &&
              replay.snapshot.cursor_uuid.empty() &&
              ReadJournal(owner) == after_close,
          "owned retired replay changed state or lost its stale diagnostic");
  Require(foreign_cancellation_checks == 0,
          "foreign identity check did not precede cancellation observation");
}
}  // namespace

int main() {
  try {
    FixtureDirectory fixture;
    for (std::uint8_t reason = 1; reason <= 4; ++reason) {
      OwnerVisibility(reason, fixture.path);
    }
    std::cout << "cursor retired-owner visibility component regression passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
