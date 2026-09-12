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
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Directory {
  fs::path path;
  Directory() {
    std::random_device random;
    for (unsigned n = 0; n != 32; ++n) {
      auto candidate = fs::temp_directory_path() /
          ("sb-cursor-database-" + std::to_string(random()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      if (fs::create_directory(candidate)) { path = candidate; return; }
    }
    throw std::runtime_error("fixture directory creation failed");
  }
  ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
api::EngineRequestContext Context(const fs::path& directory, bool second) {
  api::EngineRequestContext context;
  context.database_path = (directory / (second ? "database-b" : "database-a")).string();
  context.database_uuid.canonical = second ? "019d0000-0000-7000-8000-000000006382"
                                           : "019d0000-0000-7000-8000-000000006381";
  // Deliberately identical session/principal/transaction: database is the only
  // ownership difference. Injecting both contexts into one private component is
  // adversarial defense-in-depth testing, NOT an admitted multi-database server.
  // Deployment isolation requires distinct processes and no shared runtime
  // memory/caches. No SQL, parser or real query producer is exercised here.
  context.session_uuid.canonical = "019d0000-0000-7000-8000-000000006383";
  context.principal_uuid.canonical = "019d0000-0000-7000-8000-000000006384";
  context.statement_uuid.canonical = "019d0000-0000-7000-8000-000000006385";
  context.transaction_uuid.canonical = "019d0000-0000-7000-8000-000000006386";
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_executable_plan_receipt_compiler", "private_cursor_open",
      "private_cursor_fetch", "private_cursor_close", "right:SBLR_CURSOR_ADMIN"};
  return context;
}
std::string Journal(const api::EngineRequestContext& context) {
  std::ifstream input(context.database_path + ".sb.sblr_cursor_open.v1", std::ios::binary);
  if (!input.is_open()) return {};
  const std::string bytes{std::istreambuf_iterator<char>(input), {}};
  Require(!input.bad(), "journal read failed");
  return bytes;
}
api::SblrCursorOpenSnapshot Publish(const api::EngineRequestContext& context,
                                   std::uint64_t occurrence = 1) {
  // Known synthetic legacy constructor, used solely to seed real registry state.
  const auto result = api::CompileAndPublishSblrExecutablePlanReceipt(
      context, context.statement_uuid.canonical, occurrence, 1, 1, 64, 1);
  Require(result.ok, "fixture descriptor failed");
  return result.snapshot;
}
api::SblrCursorOpenResult Open(const api::EngineRequestContext& context,
                              const api::SblrCursorOpenSnapshot& descriptor) {
  return api::OpenSblrCursor(context, descriptor.descriptor_uuid,
      descriptor.descriptor_generation, descriptor.descriptor_evidence_sha256, 1);
}
std::string Evidence(const api::SblrCursorOpenSnapshot& cursor) {
  const auto& text = cursor.cursor_evidence_sha256;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      std::vector<std::uint8_t>(text.begin(), text.end()));
  Require(digest.ok(), "fixture hash failed");
  return "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}
api::SblrCursorOpenResult Fetch(const api::EngineRequestContext& context,
                               const api::SblrCursorOpenSnapshot& cursor) {
  return api::FetchSblrCursor(context, cursor.cursor_uuid, cursor.cursor_generation,
      cursor.position_generation, Evidence(cursor), 1, 64);
}
api::SblrCursorOpenResult Close(const api::EngineRequestContext& context,
                               const api::SblrCursorOpenSnapshot& cursor) {
  return api::CloseSblrCursor(context, cursor.cursor_uuid, cursor.cursor_generation,
      cursor.position_generation, Evidence(cursor), 1, 1);
}
void Hidden(const api::SblrCursorOpenResult& result) {
  Require(!result.ok && result.diagnostic.error &&
      result.diagnostic.code == "SECURITY.ACCESS_DENIED" &&
      result.diagnostic.message_key == "sblr.cursor.hidden" &&
      result.snapshot.cursor_uuid.empty() && result.snapshot.descriptor_uuid.empty(),
      "foreign database accessed or distinguished cursor authority");
}
void Access(const fs::path& directory) {
  auto a = Context(directory, false), b = Context(directory, true);
  const auto descriptor = Publish(a);
  const auto before = Journal(a);
  Hidden(Open(b, descriptor));
  Require(Journal(a) == before && Journal(b).empty(), "foreign OPEN wrote a journal");
  // Sharing a path must never confer identity ownership.
  b.database_path = a.database_path;
  Hidden(Open(b, descriptor));
  auto alias = a;
  alias.database_uuid.canonical = "019D0000-0000-7000-8000-000000006381";
  auto opened = Open(alias, descriptor);
  Require(opened.ok, "foreign OPEN consumed descriptor");
  const auto live = Journal(a);
  Hidden(Fetch(b, opened.snapshot));
  Hidden(Close(b, opened.snapshot));
  Require(Journal(a) == live, "foreign live operation mutated owner journal");
  const auto fetched = Fetch(a, opened.snapshot);
  Require(fetched.ok && fetched.snapshot.position_generation ==
      opened.snapshot.position_generation + 1, "foreign FETCH consumed position");
  Require(Close(a, fetched.snapshot).ok, "owner CLOSE failed");
  const auto retired = Journal(a);
  Hidden(Close(b, fetched.snapshot));
  Require(Close(a, fetched.snapshot).diagnostic.code == "CURSOR.STALE",
          "owner retired replay lost stale diagnostic");
  Require(Journal(a) == retired, "retired replay wrote journal");
}
void Recovery(const fs::path& directory) {
  auto a = Context(directory, false), b = Context(directory, true);
  const auto live_a = Open(a, Publish(a));
  const auto pending_a = Publish(a, 2);
  const auto live_b = Open(b, Publish(b));
  const auto pending_b = Publish(b, 2);
  const auto retired_a = Open(a, Publish(a, 3));
  Require(retired_a.ok && Close(a, retired_a.snapshot).ok, "fixture retirement failed");
  Require(live_a.ok && live_b.ok, "fixture OPEN failed");
  const auto before_a = Journal(a), before_b = Journal(b);
  auto unauthorized = b;
  unauthorized.trace_tags.clear();
  Require(api::RecoverSblrOpenCursors(unauthorized).code == "SECURITY.ACCESS_DENIED",
          "recovery ignored capability");
  Require(api::RecoverSblrOpenCursors(b).code == "OK", "database recovery failed");
  Require(Journal(a) == before_a, "recovery wrote foreign database journal");
  const auto expected = "X\t" + live_b.snapshot.descriptor_uuid + "\t" +
      live_b.snapshot.cursor_uuid + "\t" + std::to_string(live_b.snapshot.cursor_generation) + "\n";
  Require(Journal(b) == before_b + expected,
          "recovery journal includes another database's cursor");
  Hidden(Fetch(b, live_b.snapshot));
  Hidden(Open(b, pending_b));
  Hidden(Close(b, retired_a.snapshot));
  Require(Close(a, retired_a.snapshot).diagnostic.code == "CURSOR.STALE",
          "foreign recovery changed owning retirement visibility");
  Require(Close(a, live_a.snapshot).ok, "recovery destroyed foreign live cursor");
  const auto remaining = Open(a, pending_a);
  Require(remaining.ok && Close(a, remaining.snapshot).ok,
          "recovery destroyed foreign pending descriptor");
  const auto recovered = Journal(b);
  Require(api::RecoverSblrOpenCursors(b).code == "OK" && Journal(b) == recovered,
          "repeated empty recovery changed journal");
}
void InvalidIdentity(const fs::path& directory) {
  const auto owner = Context(directory, false);
  const auto pending = Publish(owner);
  const auto live = Open(owner, Publish(owner, 2));
  Require(live.ok, "fixture OPEN failed");
  const auto before = Journal(owner);
  for (const std::string identity : {"", "malformed", "00000000-0000-0000-0000-000000000000",
       "019d0000-0000-4000-8000-000000006381", "019d0000-0000-7000-0000-000000006381"}) {
    auto invalid = owner;
    invalid.database_uuid.canonical = identity;
    Require(api::RecoverSblrOpenCursors(invalid).code == "SECURITY.ACCESS_DENIED",
            "invalid database identity accepted for recovery");
    Hidden(Open(invalid, pending));
    Hidden(Fetch(invalid, live.snapshot));
    Hidden(Close(invalid, live.snapshot));
    const auto publication = api::CompileAndPublishSblrExecutablePlanReceipt(
        invalid, invalid.statement_uuid.canonical, 3, 1, 1, 64, 1);
    Require(!publication.ok && publication.diagnostic.code == "SECURITY.ACCESS_DENIED" &&
        publication.snapshot.descriptor_uuid.empty(), "invalid database identity published descriptor");
    Require(Journal(owner) == before, "invalid database identity mutated journal");
  }
  Require(Close(owner, live.snapshot).ok, "invalid identity consumed live cursor");
  const auto opened = Open(owner, pending);
  Require(opened.ok && Close(owner, opened.snapshot).ok, "invalid identity consumed descriptor");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    Require(argc == 2, "expected access recovery or invalid");
    Directory directory;
    const std::string mode = argv[1];
    if (mode == "access") Access(directory.path);
    else if (mode == "recovery") Recovery(directory.path);
    else if (mode == "invalid") InvalidIdentity(directory.path);
    else throw std::runtime_error("unknown test mode");
    std::cout << "PASS cursor database ownership " << mode << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
