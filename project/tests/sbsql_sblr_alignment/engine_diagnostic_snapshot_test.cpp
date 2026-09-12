// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server_engine_bridge/diagnostic_fields.hpp"
#include "server/sbps.hpp"
#include "engine/internal_api/security/database_local_security_event_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <type_traits>

namespace { long fail_after = -1; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace bridge = scratchbird::server_engine_bridge;
using Snapshot = bridge::EngineDiagnosticSnapshot;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::cerr << message << '\n'; }
}
bool Equal(const Snapshot& a, const Snapshot& b) {
  if (a.canonical_metadata.has_value() != b.canonical_metadata.has_value() ||
      a.native_source.has_value() != b.native_source.has_value()) return false;
  if (a.canonical_metadata) {
    const auto& x = *a.canonical_metadata;
    const auto& y = *b.canonical_metadata;
    if (x.code != y.code || x.severity != y.severity || x.is_failure != y.is_failure ||
        x.sqlstate != y.sqlstate || x.numeric_binding != y.numeric_binding ||
        x.retry_class != y.retry_class || x.required_outcome != y.required_outcome ||
        x.diagnostic_class != y.diagnostic_class) return false;
  }
  if (a.occurrence_uuid != b.occurrence_uuid || a.numeric_code != b.numeric_code ||
      a.severity != b.severity || a.code != b.code || a.message_key != b.message_key ||
      a.safe_detail != b.safe_detail || a.fields.size() != b.fields.size()) return false;
  for (std::size_t i = 0; i < a.fields.size(); ++i)
    if (a.fields[i].key != b.fields[i].key || a.fields[i].value != b.fields[i].value)
      return false;
  return true;
}
}

int main() {
  static_assert(std::is_nothrow_move_assignable_v<Snapshot>);
  sb_engine_handle_t engine = nullptr;
  sb_engine_result_t result = nullptr;
  Check(sb_engine_open(nullptr, &engine, &result) == SB_ENGINE_STATUS_INVALID_ARGUMENT &&
            engine == nullptr && result != nullptr, "actual null-parameter guard failed");
  Snapshot first;
  Check(bridge::CopyEngineDiagnosticSnapshot(result, 0, &first), "snapshot failed");
  Check(first.code == "ENGINE.ABI.PARAMETER_NULL" && first.numeric_code == 1004 &&
            first.message_key == "engine.abi.parameter_null" &&
            first.severity == SB_ENGINE_DIAGNOSTIC_ERROR && first.safe_detail.empty() &&
            first.fields.empty(), "actual diagnostic source fields changed");
  Check(first.canonical_metadata &&
            first.canonical_metadata->code == "ENGINE.ABI.PARAMETER_NULL" &&
            first.canonical_metadata->severity ==
                scratchbird::core::diagnostics::CanonicalSeverity::error &&
            first.canonical_metadata->is_failure &&
            first.canonical_metadata->retry_class == "false" &&
            first.canonical_metadata->required_outcome == "not_specified" &&
            first.canonical_metadata->numeric_binding == "not_applicable" &&
            first.canonical_metadata->sqlstate == "not_applicable" &&
            !first.native_source,
        "ABI emission metadata lost, guessed or confused with wrapper numeric status");
  Check((first.occurrence_uuid[6] & 0xf0) == 0x70 &&
            (first.occurrence_uuid[8] & 0xc0) == 0x80, "source identity is not binary UUIDv7");
  Snapshot again;
  Check(bridge::CopyEngineDiagnosticSnapshot(result, 0, &again) && Equal(first, again),
        "repeated snapshot changed source identity or content");
  namespace server = scratchbird::server;
  server::ServerDiagnostic target;
  target.code = first.code;
  target.message_key = "server.private.template.pending";
  target.safe_message = "safe caller text";
  target.fields = {{"visible", "safe"}};
  const auto untouched = target;
  const auto same_target = [&](const server::ServerDiagnostic& value) {
    return value.code == untouched.code && value.message_key == untouched.message_key &&
        value.safe_message == untouched.safe_message && value.fields.size() == 1 &&
        value.fields[0].key == "visible" && value.fields[0].value == "safe" &&
        value.occurrence_uuid == untouched.occurrence_uuid && !value.engine_source_snapshot;
  };
  Check(server::AdoptEngineDiagnosticSource(first, &target) &&
        target.occurrence_uuid == first.occurrence_uuid && target.engine_source_snapshot &&
        Equal(*target.engine_source_snapshot, first) &&
        target.message_key == untouched.message_key && target.fields[0].value == "safe",
        "actual server source adoption lost metadata or changed presentation fields");
  const auto adopted_text = server::ToMessageVectorJsonLine(target);
  Check(adopted_text.find(first.message_key) == std::string::npos &&
        adopted_text.find("not_specified") == std::string::npos,
        "private source metadata leaked through legacy public renderer");
  const auto owned_target = target;
  if (target.engine_source_snapshot)
    target.engine_source_snapshot->message_key = "changed private copy";
  Check(owned_target.engine_source_snapshot && Equal(*owned_target.engine_source_snapshot, first),
        "adopted server source aliases mutable snapshot");
  for (unsigned version = 0; version != 16; ++version) {
    for (unsigned variant = 0; variant != 4; ++variant) {
      auto input = first;
      input.occurrence_uuid[6] = static_cast<std::uint8_t>(version << 4);
      input.occurrence_uuid[8] = static_cast<std::uint8_t>(variant << 6);
      target = untouched;
      const bool accepted = server::AdoptEngineDiagnosticSource(input, &target);
      Check(accepted == (version == 7 && variant == 2), "wrong system identity admission");
      Check(accepted ? target.occurrence_uuid == input.occurrence_uuid : same_target(target),
            "system identity adoption or rejection changed unrelated output");
    }
  }
  for (unsigned bad_case = 0; bad_case != 3; ++bad_case) {
    auto input = first;
    if (bad_case == 0) input.code.clear();
    if (bad_case == 1) input.code = "DIFFERENT.WRAPPER";
    if (bad_case == 2) {
      input.canonical_metadata.emplace();
      input.canonical_metadata->code = "DIFFERENT.METADATA";
    }
    target = untouched;
    Check(!server::AdoptEngineDiagnosticSource(input, &target) && same_target(target),
          "source/code mismatch adopted or partially published");
  }
  Check(!server::AdoptEngineDiagnosticSource(first, nullptr), "null adoption output accepted");
  unsigned adoption_allocation_failures = 0;
  bool adoption_completed = false;
  for (long i = 0; i < 128; ++i) {
    target = untouched;
    fail_after = i;
    bool adopted = false;
    try { adopted = server::AdoptEngineDiagnosticSource(first, &target); }
    catch (...) { fail_after = -1; Check(false, "adoption leaked allocation exception"); break; }
    fail_after = -1;
    if (adopted) {
      Check(target.engine_source_snapshot && Equal(*target.engine_source_snapshot, first),
            "allocation campaign successful adoption lost source");
      adoption_completed = true;
      break;
    }
    ++adoption_allocation_failures;
    Check(same_target(target), "allocation failure partially adopted source");
  }
  Check(adoption_completed && adoption_allocation_failures >= 5,
        "actual adoption allocations not exercised");
  Snapshot sentinel = first;
  sentinel.code = "unchanged";
  sentinel.fields = {{"sentinel", "must remain intact"}};
  Snapshot output = sentinel;
  Check(!bridge::CopyEngineDiagnosticSnapshot(nullptr, 0, &output) && Equal(output, sentinel),
        "null result modified output");
  Check(!bridge::CopyEngineDiagnosticSnapshot(result, 1, &output) && Equal(output, sentinel),
        "out-of-range diagnostic modified output");
  Check(!bridge::CopyEngineDiagnosticSnapshot(result, 0, nullptr), "null output accepted");
  std::vector<bridge::EngineDiagnosticField> fields{{"sentinel", "keep"}};
  Check(!bridge::CopyEngineDiagnosticFields(nullptr, 0, &fields) && fields.size() == 1 &&
            fields[0].key == "sentinel", "legacy field copy modified output on failure");
  unsigned allocation_failures = 0;
  bool completed = false;
  for (long i = 0; i < 64; ++i) {
    output = sentinel;
    fail_after = i;
    bool ok = false;
    try { ok = bridge::CopyEngineDiagnosticSnapshot(result, 0, &output); }
    catch (...) { fail_after = -1; Check(false, "snapshot leaked allocation exception"); break; }
    fail_after = -1;
    if (ok) {
      Check(Equal(output, first), "successful allocation sweep changed snapshot");
      completed = true;
      break;
    }
    ++allocation_failures;
    Check(Equal(output, sentinel), "allocation failure partially published snapshot");
  }
  Check(completed && allocation_failures >= 2, "actual string allocations were not exercised");
  Check(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK && Equal(first, again),
        "owned snapshot did not survive result release");
  result = nullptr;
  Check(sb_engine_open(nullptr, &engine, &result) == SB_ENGINE_STATUS_INVALID_ARGUMENT,
        "second actual source did not refuse");
  Snapshot second;
  Check(bridge::CopyEngineDiagnosticSnapshot(result, 0, &second) &&
            second.code == first.code && second.occurrence_uuid != first.occurrence_uuid,
        "separate actual emissions share identity");
  Check(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK, "second result release failed");

  // A real security-store consumer must keep its private storage failure as a
  // distinct source, not append the cause code to the wrapper's detail text.
  // Only an isolated empty test directory is touched; no database is created.
  namespace fs = std::filesystem;
  namespace api = scratchbird::engine::internal_api;
  std::string suffix;
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : first.occurrence_uuid) {
    suffix.push_back(hex[byte >> 4]);
    suffix.push_back(hex[byte & 15]);
  }
  const auto fixture = fs::temp_directory_path() / ("sb_diagnostic_snapshot_" + suffix);
  const bool created = fs::create_directory(fixture);
  Check(created, "isolated security fixture creation failed");
  if (!created) return 1;  // Never adopt or remove a pre-existing directory.
  struct FixtureCleanup {
    fs::path path;
    ~FixtureCleanup() { std::error_code error; fs::remove_all(path, error); }
  } cleanup{fixture};
  api::EngineRequestContext context;
  context.database_path = (fixture / "absent.sbdb").string();
  const auto missing = api::LoadDatabaseLocalSecurityEventStoreV1(context);
  Check(!missing.ok && missing.diagnostic.error &&
            missing.diagnostic.code == api::kDatabaseLocalSecurityDiagnosticCorrupt &&
            missing.diagnostic.detail == "bootstrap_security_catalog_invalid" &&
            missing.diagnostic.fields.empty(), "security wrapper or disclosure boundary changed");
  Check(missing.diagnostic.native_source &&
            !missing.diagnostic.native_source->record.status.ok() &&
            !missing.diagnostic.native_source->record.diagnostic_code.empty() &&
            missing.diagnostic.native_source->record.diagnostic_code != missing.diagnostic.code,
        "actual security-store storage cause was lost");
  Check(!fs::exists(context.database_path), "read-only refusal created database");
  std::error_code cleanup_error;
  fs::remove_all(fixture, cleanup_error);
  Check(!cleanup_error && !fs::exists(fixture), "owned security fixture cleanup failed");

  // Exercise the actual server source and encoder, not a manually copied
  // engine-to-server adapter. Full engine/server dispatch is a separate gate.
  namespace sbps = scratchbird::server::sbps;
  const auto refusal = sbps::DecodeFrameBytes({}, 4096);
  Check(!refusal.frame && refusal.diagnostics.size() == 1, "actual SBPS refusal changed");
  if (refusal.diagnostics.size() == 1) {
    const auto request = sbps::MakeUuidV7Bytes();
    const auto wire = sbps::EncodeMessageVectorSet(refusal.diagnostics, request);
    const auto wire_again = sbps::EncodeMessageVectorSet(refusal.diagnostics, request);
    Check(wire.size() >= 176 && wire_again.size() >= 176, "actual record missing");
    if (wire.size() >= 176 && wire_again.size() >= 176) {
      const auto& source = refusal.diagnostics.front().occurrence_uuid;
      Check(std::equal(source.begin(), source.end(), wire.begin() + 96) &&
                std::equal(source.begin(), source.end(), wire_again.begin() + 96),
            "wire regenerated or lost source occurrence");
      Check(!std::equal(wire.begin() + 80, wire.begin() + 96, wire_again.begin() + 80),
            "separate message records share vector identity");
      Check(std::equal(request.begin(), request.end(), wire.begin() + 112),
            "source UUID overwrote adjacent request identity");
    }
  }
  std::cout << "checks=" << checks << " allocation_failures=" << allocation_failures
            << " adoption_allocation_failures=" << adoption_allocation_failures
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
