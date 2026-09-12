#include "engine/internal_api/sblr_cursor_open_coordinator.hpp"
#include "hash_digest.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace fs = std::filesystem;

namespace {
void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class FixtureDirectory {
 public:
  FixtureDirectory() {
    std::random_device random;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      const auto candidate = fs::temp_directory_path() /
          ("sb-cursor-recovery-fault-" + std::to_string(random()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      if (fs::create_directory(candidate)) {
        path = candidate;
        return;
      }
    }
    throw std::runtime_error("cannot create isolated fixture directory");
  }
  ~FixtureDirectory() {
    // Only the uniquely created directory owned by this fixture is removed.
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  fs::path path;
};

std::string AdmittedEvidence(const api::SblrCursorOpenSnapshot& cursor) {
  const auto& value = cursor.cursor_evidence_sha256;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      std::vector<std::uint8_t>(value.begin(), value.end())).digest;
  return "sha256:" + scratchbird::core::hash::HexLower(digest);
}

api::SblrCursorOpenSnapshot PublishDescriptor(api::EngineRequestContext& context,
                                             std::uint64_t occurrence) {
  // Legacy coordinator setup only. This constructor currently synthesizes plan
  // receipts; this test does NOT establish executable-plan or public E2E proof.
  const auto result = api::CompileAndPublishSblrExecutablePlanReceipt(
      context, context.statement_uuid.canonical, occurrence, 1, 1, 64, 1);
  Require(result.ok, "fixture descriptor publication failed");
  return result.snapshot;
}

api::SblrCursorOpenSnapshot Open(api::EngineRequestContext& context,
                                const api::SblrCursorOpenSnapshot& descriptor) {
  const auto result = api::OpenSblrCursor(
      context, descriptor.descriptor_uuid, descriptor.descriptor_generation,
      descriptor.descriptor_evidence_sha256, 1);
  Require(result.ok, "descriptor missing or unusable after failed recovery");
  return result.snapshot;
}

void Close(api::EngineRequestContext& context,
           const api::SblrCursorOpenSnapshot& cursor) {
  const auto result = api::CloseSblrCursor(
      context, cursor.cursor_uuid, cursor.cursor_generation,
      cursor.position_generation, AdmittedEvidence(cursor), 1, 1);
  Require(result.ok, "cursor missing or changed after failed recovery");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "expected open_failure or flush_failure");
    const std::string mode = argv[1];
    Require(mode == "open_failure" || mode == "flush_failure", "unknown fault");
    FixtureDirectory fixture;
    api::EngineRequestContext context;
    context.database_path = (fixture.path / "database").string();
    context.database_uuid.canonical = "019d0000-0000-7000-8000-000000006387";
    context.security_context_present = true;
    context.statement_metadata_snapshot_engine_owned = true;
    context.statement_uuid.canonical = "019d0000-0000-7000-8000-000000005849";
    context.session_uuid.canonical = "019d0000-0000-7000-8000-000000005850";
    context.principal_uuid.canonical = "019d0000-0000-7000-8000-000000005851";
    context.transaction_uuid.canonical = "019d0000-0000-7000-8000-000000005852";
    context.trace_tags = {"private_executable_plan_receipt_compiler",
                          "private_cursor_open", "private_cursor_close",
                          "right:SBLR_CURSOR_ADMIN"};
    const auto first = Open(context, PublishDescriptor(context, 1));
    const auto second = Open(context, PublishDescriptor(context, 2));
    const auto pending = PublishDescriptor(context, 3);

    auto unauthorized = context;
    unauthorized.trace_tags.clear();
    Require(api::RecoverSblrOpenCursors(unauthorized).code ==
                "SECURITY.ACCESS_DENIED",
            "recovery must require administrative authority");

    const fs::path journal = context.database_path + ".sb.sblr_cursor_open.v1";
    const fs::path saved = fixture.path / "saved-journal";
    fs::rename(journal, saved);
    if (mode == "open_failure") {
      fs::create_directory(journal);  // Real open failure; no mocked I/O.
    } else {
      // Registered only on Linux: the device accepts open but fails writes.
      fs::create_symlink("/dev/full", journal);
    }
    const auto failure = api::RecoverSblrOpenCursors(context);
    Require(failure.error && failure.code == "CURSOR.CLOSE_FAILED",
            "failed recovery journal publication must not return success");

    // Restore the exact fixture journal before probing unchanged authority.
    fs::remove(journal);
    fs::rename(saved, journal);
    Close(context, first);
    Close(context, second);
    Close(context, Open(context, pending));
    std::cout << "PASS " << mode
              << ": failure reported; both cursors and pending descriptor retained\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
