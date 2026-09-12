// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/catalog/name_resolution_api.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "server/diagnostics.hpp"
#include "server/sbps.hpp"

#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using Id = std::array<std::uint8_t, 16>;
unsigned checks = 0, failures = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition && ++failures <= 16) std::cerr << message << '\n';
}
std::uint64_t Millis() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}
template<class Diagnostic>
void VerifyActualEmission(const Diagnostic& first, const Diagnostic& second,
                          const char* expected_code) {
  Check(first.code == expected_code && second.code == expected_code,
        "actual source refusal changed its code");
  const auto& id = first.occurrence_uuid;
  Check((id[6] & 0xf0) == 0x70 && (id[8] & 0xc0) == 0x80,
        "actual source refusal did not allocate binary UUIDv7");
  Check(id != second.occurrence_uuid,
        "separate actual source refusals shared an occurrence");
  const auto copy = first;
  Check(copy.occurrence_uuid == id && copy.code == first.code,
        "actual source refusal copy lost identity or code");
}
void VerifyActualSources() {
  namespace api = scratchbird::engine::internal_api;
  const auto first = api::LookupEngineResourceDescriptorByUuid({}, {}, "charset");
  const auto second = api::LookupEngineResourceDescriptorByUuid({}, {}, "charset");
  Check(!first.ok && !second.ok && !first.resource_descriptor.present &&
            !second.resource_descriptor.present, "resource UUID guard falsely succeeded");
  VerifyActualEmission(first.diagnostic, second.diagnostic, "CATALOG.RESOURCE.UUID_REQUIRED");
  const auto decoded = scratchbird::engine::sblr::DecodeSblrEnvelope({});
  const auto repeated = scratchbird::engine::sblr::DecodeSblrEnvelope({});
  Check(!decoded.ok && !repeated.ok && decoded.diagnostics.size() == 1 &&
            repeated.diagnostics.size() == 1, "actual SBLR short-header guard changed");
  if (decoded.diagnostics.size() == 1 && repeated.diagnostics.size() == 1)
    VerifyActualEmission(decoded.diagnostics.front(), repeated.diagnostics.front(),
                         "SBLR.OPERATION.HEADER_INVALID");
  const auto frame = scratchbird::server::sbps::DecodeFrameBytes({}, 4096);
  const auto again = scratchbird::server::sbps::DecodeFrameBytes({}, 4096);
  Check(!frame.frame && !again.frame && frame.diagnostics.size() == 1 &&
            again.diagnostics.size() == 1, "actual SBPS short-header guard changed");
  if (frame.diagnostics.size() == 1 && again.diagnostics.size() == 1)
    VerifyActualEmission(frame.diagnostics.front(), again.diagnostics.front(),
                         "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID");
}
template<class Diagnostic> void VerifySourceRecords(const char* source) {
  if constexpr (!requires(Diagnostic diagnostic) { diagnostic.occurrence_uuid; }) {
    Check(false, source);
  } else {
    static_assert(std::is_same_v<decltype(Diagnostic{}.occurrence_uuid), Id>);
    const auto before = Millis();
    Diagnostic original;
    original.code = "SECURITY.ACCESS_DENIED";
    const auto after = Millis();
    const auto identity = original.occurrence_uuid;
    Check((identity[6] & 0xf0) == 0x70 && (identity[8] & 0xc0) == 0x80,
          "source did not emit a binary UUIDv7 occurrence");
    std::uint64_t timestamp = 0;
    for (unsigned i = 0; i < 6; ++i) timestamp = (timestamp << 8) | identity[i];
    Check(timestamp >= before && timestamp <= after, "occurrence invented its timestamp");
    const Diagnostic copy(original);
    Check(copy.occurrence_uuid == identity && copy.code == original.code,
          "copy construction regenerated source identity");
    Diagnostic assigned;
    assigned = original;
    Check(assigned.occurrence_uuid == identity, "copy assignment regenerated source identity");
    Diagnostic moved(std::move(assigned));
    Check(moved.occurrence_uuid == identity, "move construction lost source identity");
    assigned = std::move(moved);
    Check(assigned.occurrence_uuid == identity, "move assignment lost source identity");
    std::vector<Diagnostic> records{original};
    records.reserve(1024);
    Check(records.front().occurrence_uuid == identity, "vector relocation changed occurrence");
    Diagnostic same_code;
    same_code.code = original.code;
    Check(same_code.occurrence_uuid != identity, "code identity substituted for occurrence");
    constexpr unsigned workers = 8, count = 1024;
    std::barrier ready(workers);
    std::array<std::vector<Id>, workers> samples;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) threads.emplace_back([&, i] {
      samples[i].reserve(count);
      ready.arrive_and_wait();
      for (unsigned j = 0; j < count; ++j) {
        Diagnostic diagnostic;
        diagnostic.code = original.code;
        samples[i].push_back(diagnostic.occurrence_uuid);
      }
    });
    for (auto& thread : threads) thread.join();
    std::set<Id> unique{identity, same_code.occurrence_uuid};
    for (const auto& batch : samples) for (const auto& id : batch) {
      Check((id[6] & 0xf0) == 0x70 && (id[8] & 0xc0) == 0x80,
            "concurrent source identity has invalid version/variant");
      Check(unique.insert(id).second, "concurrent sources emitted duplicate occurrences");
    }
  }
}
}
int main() {
  VerifyActualSources();
  VerifySourceRecords<scratchbird::engine::internal_api::EngineApiDiagnostic>(
      "engine API diagnostic has no source occurrence identity");
  VerifySourceRecords<scratchbird::engine::sblr::SblrEnvelopeDiagnostic>(
      "SBLR diagnostic has no source occurrence identity");
  VerifySourceRecords<scratchbird::server::ServerDiagnostic>(
      "server diagnostic has no source occurrence identity");
  std::cout << "diagnostic_occurrence_identity checks=" << checks
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
