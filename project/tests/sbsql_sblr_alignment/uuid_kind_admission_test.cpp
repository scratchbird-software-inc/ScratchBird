// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <utility>

namespace p = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
using p::UuidKind;
static_assert(sizeof(p::Uuid) == 16);
constexpr std::array kinds = {
    UuidKind::database, UuidKind::cluster, UuidKind::filespace, UuidKind::schema,
    UuidKind::object, UuidKind::row, UuidKind::page, UuidKind::transaction,
    UuidKind::session, UuidKind::principal};
unsigned checks = 0, failures = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition && ++failures <= 12) std::cerr << "FAIL " << message << '\n';
}

// The oracle enumerates the declared kinds; it does not call production
// classification or infer authority from a generated result.
bool Known(UuidKind kind) {
  return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

void CheckResult(const uuid::TypedUuidResult& result, bool admitted) {
  Check(result.ok() == admitted, "typed identity admission differs from kind oracle");
  if (!admitted) Check(!result.diagnostic.diagnostic_code.empty(), "missing refusal diagnostic");
}

void CheckCompare(p::TypedUuid left, p::TypedUuid right, UuidKind expected,
                  bool admitted, const char* refusal) {
  const auto result = uuid::CompareUuidV7ForIndex(left, right, expected);
  const int binary_order = left.value.bytes < right.value.bytes ? -1 :
                           right.value.bytes < left.value.bytes ? 1 : 0;
  Check(result.ok == admitted, "undefined index kind admitted");
  Check(result.specialized_comparator_used == admitted &&
            result.fallback_to_uncompressed_uuid != admitted,
        "incorrect specialized/fallback flags");
  Check(result.comparison == binary_order, "binary fallback or specialized order changed");
  Check(result.refusal_reason == refusal, "incorrect comparator refusal reason");
}

int main() {
  const auto millis = static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  if (!generated.ok()) return 2;
  const auto value = generated.value;
  auto greater = value;
  greater.bytes[15] = 255;
  auto lesser = value;
  lesser.bytes[15] = 0;
  for (unsigned raw = 0; raw < 256; ++raw) {
    const auto kind = static_cast<UuidKind>(raw);
    const bool known = Known(kind);
    const bool durable = known && kind != UuidKind::session;
    Check(uuid::IsEngineIdentityKind(kind) == known, "undefined engine kind classified as valid");
    Check(uuid::IsDurableEngineIdentityKind(kind) == durable, "durable classification changed");
    const auto typed = uuid::MakeTypedUuid(kind, value);
    CheckResult(typed, known);
    if (known) Check(typed.value.kind == kind && typed.value.value == value, "binary identity changed");
    CheckResult(uuid::ParseTypedUuid(kind, uuid::UuidToString(value)), known);
    CheckResult(uuid::ParseDurableEngineIdentityUuid(kind, uuid::UuidToString(value)), durable);
    CheckResult(uuid::MakeDurableEngineIdentityUuid(kind, value), durable);
    CheckResult(uuid::GenerateEngineIdentityV7(kind, millis), durable);
    for (const auto& pair : std::array{std::pair{lesser, greater},
                                      std::pair{greater, lesser}, std::pair{value, value}}) {
      CheckCompare({kind, pair.first}, {kind, pair.second}, kind, known,
                   known ? "" : "unsupported_kind");
    }
    if (kind != UuidKind::object) {
      CheckCompare({kind, lesser}, {UuidKind::object, greater}, UuidKind::object,
                   false, "kind_mismatch");
      CheckCompare({UuidKind::object, lesser}, {kind, greater}, UuidKind::object,
                   false, "kind_mismatch");
    }
    for (unsigned version = 0; version < 16; ++version) {
      for (unsigned variant = 0; variant < 4; ++variant) {
        auto candidate = value;
        candidate.bytes[6] = static_cast<p::byte>((candidate.bytes[6] & 15) | (version << 4));
        candidate.bytes[8] = static_cast<p::byte>((candidate.bytes[8] & 63) | (variant << 6));
        CheckResult(uuid::MakeTypedUuid(kind, candidate), known && version == 7 && variant == 2);
      }
    }
    CheckResult(uuid::MakeTypedUuid(kind, {}), false);
  }
  // Older UUID versions remain user values, never typed engine identity.
  // Actual datatype storage codecs are covered by descriptor_binary_uuid_authority.
  for (unsigned version = 1; version <= 7; ++version) {
    auto candidate = value;
    candidate.bytes[6] = static_cast<p::byte>((candidate.bytes[6] & 15) | (version << 4));
    Check(uuid::UuidVersionAllowed(candidate, {}), "ordinary user UUID version refused");
    const auto parsed = uuid::ParseUuid(uuid::UuidToString(candidate));
    Check(parsed.ok() && parsed.value == candidate, "ordinary UUID bytes rewritten");
  }
  std::cout << "checks=" << checks << " failures=" << failures
            << " kind_values=256 kind_version_variant_cases=16384\n";
  return failures == 0 ? 0 : 1;
}
