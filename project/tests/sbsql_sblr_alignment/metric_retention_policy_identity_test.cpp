// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_retention_policy.hpp"
#include "canonical_diagnostic_catalog.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {
namespace m = scratchbird::core::metrics;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::cerr << message << '\n'; }
}
template<class T> concept HasIdentity = requires(T value) { value.policy_uuid; };
static_assert(!HasIdentity<m::MetricRetentionPolicyDefinition>);
static_assert(std::is_same_v<decltype(m::MetricRetentionPolicy::policy_uuid), m::MetricUuid>);
static_assert(!std::is_convertible_v<m::MetricRetentionPolicyDefinition, m::MetricRetentionPolicy>);

m::MetricRetentionPolicy Policy() {
  m::MetricRetentionPolicy policy;
  policy.policy_name = "test-policy";
  policy.policy_uuid.bytes[6] = 0x70;
  policy.policy_uuid.bytes[8] = 0x80;
  policy.generation = 1;
  return policy;
}
void Rejected(const m::MetricRetentionPolicy& policy,
              const char* expected = "METRIC.RETENTION_POLICY_INVALID") {
  const auto result = m::ValidateMetricRetentionPolicy(policy);
  Check(!result.ok && result.diagnostic_code == expected, "policy failure identity mismatch");
  const auto* code = scratchbird::core::diagnostics::FindCanonicalDiagnosticCode(result.diagnostic_code);
  Check(code && code->is_failure && code->retry_class != "not_specified" &&
            code->required_outcome != "not_specified", "policy diagnostic is not fully registered");
}
void Identity() {
  Check(m::ValidateMetricRetentionPolicy(Policy()).ok, "valid policy rejected");
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    auto policy = Policy(); policy.policy_uuid.bytes[6] = version << 4;
    Rejected(policy, "UUID.ENGINE_IDENTITY_NOT_V7");
  }
  for (const unsigned variant : {0u, 0x40u, 0xc0u}) {
    auto policy = Policy(); policy.policy_uuid.bytes[8] = variant;
    Rejected(policy, "UUID.ENGINE_IDENTITY_NOT_V7");
  }
  auto policy = Policy(); policy.policy_uuid = {};
  Rejected(policy, "UUID.ENGINE_IDENTITY_NOT_V7");
  policy = Policy(); policy.generation = 0; Rejected(policy);
  policy.generation = std::numeric_limits<std::uint64_t>::max();
  Check(m::ValidateMetricRetentionPolicy(policy).ok, "maximum generation rejected");
  const auto definitions = m::MetricRetentionPolicyDefinitions();
  Check(definitions.size() == 6, "definition inventory drifted");
  for (const auto& definition : definitions) {
    Check(m::ValidateMetricRetentionPolicyDefinition(definition).ok, "invalid compiled definition");
    const auto* found = m::FindMetricRetentionPolicyDefinition(definition.policy_name);
    Check(found && found->policy_name == definition.policy_name, "definition lookup lost exact name");
  }
  for (const auto name : {"", "seed-metrics-current-only", "METRICS_CURRENT_ONLY",
                         "metrics_current_only ", "not_a_policy"})
    Check(!m::FindMetricRetentionPolicyDefinition(name), "definition lookup inferred identity or alias");
}
void Definitions() {
  auto policy = Policy();
  for (auto member : {&m::MetricRetentionPolicyDefinition::policy_name,
                      &m::MetricRetentionPolicyDefinition::edit_right,
                      &m::MetricRetentionPolicyDefinition::default_admin_group}) {
    policy = Policy(); policy.*member = ""; Rejected(policy);
    policy = Policy(); policy.*member = std::string("a\0b", 3); Rejected(policy);
  }
  policy = Policy(); policy.scope = "invalid"; Rejected(policy);
  policy = Policy(); policy.purge_batch_limit = 0; Rejected(policy);
  policy = Policy(); policy.max_cardinality = 0; Rejected(policy);
  policy = Policy(); policy.evidence_required = false; Rejected(policy);
  policy = Policy(); policy.overflow_behavior = "drop_silently"; Rejected(policy);
  for (const auto mode : {m::MetricRetentionMode::invalid,
                          static_cast<m::MetricRetentionMode>(-1),
                          static_cast<m::MetricRetentionMode>(100)}) {
    policy = Policy(); policy.mode = mode; Rejected(policy);
    Check(std::string(m::MetricRetentionModeName(mode)) == "invalid", "invalid mode rendered as valid");
  }
  for (const auto name : {"", "CURRENT_ONLY", " current_only", "current_only ", "raw", "1m"})
    Check(m::MetricRetentionModeFromName(name) == m::MetricRetentionMode::invalid,
          "unknown mode silently defaulted");
  for (const auto name : {"current_only", "raw_and_rollup", "rollup_only"})
    Check(name == std::string(m::MetricRetentionModeName(m::MetricRetentionModeFromName(name))),
          "exact mode did not round trip");
  for (const auto name : {"", "1M", "1m ", "minute", "0", "30d"})
    Check(m::MetricRollupGrainFromName(name) == m::MetricRollupGrain::invalid,
          "unknown grain silently defaulted");
  for (const auto name : {"1m", "1h", "1d", "long_summary"}) {
    const auto grain = m::MetricRollupGrainFromName(name);
    Check(name == std::string(m::MetricRollupGrainName(grain)) &&
              m::MetricRollupGrainWindowSeconds(grain) != 0, "exact grain did not round trip");
  }
  policy = Policy(); policy.raw_retention_seconds = 1; Rejected(policy);
  policy = Policy(); policy.rollup_retention_seconds = 1; Rejected(policy);
  policy = Policy(); policy.rollup_grains = {m::MetricRollupGrain::one_minute}; Rejected(policy);
  policy = Policy(); policy.mode = m::MetricRetentionMode::raw_and_rollup; Rejected(policy);
  policy.raw_retention_seconds = 1;
  Check(m::ValidateMetricRetentionPolicy(policy).ok, "valid raw-only collection rejected");
  policy.rollup_grains = {m::MetricRollupGrain::one_minute}; Rejected(policy);
  policy.rollup_retention_seconds = 1;
  Check(m::ValidateMetricRetentionPolicy(policy).ok, "valid raw/rollup policy rejected");
  policy.rollup_grains.push_back(m::MetricRollupGrain::one_minute); Rejected(policy);
  policy.rollup_grains = {m::MetricRollupGrain::invalid}; Rejected(policy);
  Check(m::MetricRollupGrainWindowSeconds(static_cast<m::MetricRollupGrain>(100)) == 0,
        "invalid grain acquired a window");
  policy = Policy(); policy.mode = m::MetricRetentionMode::rollup_only; Rejected(policy);
  policy.rollup_retention_seconds = 1; Rejected(policy);
  policy.rollup_grains = {m::MetricRollupGrain::one_hour};
  Check(m::ValidateMetricRetentionPolicy(policy).ok, "valid rollup-only policy rejected");
  policy.raw_retention_seconds = 1; Rejected(policy);
}
bool Oracle(std::uint64_t observed, std::uint64_t now, std::uint64_t seconds) {
  if (!seconds || now <= observed || seconds > std::numeric_limits<std::uint64_t>::max() / 1000000)
    return false;
  return now - observed > seconds * 1000000;
}
void RetentionTime() {
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  const std::array<std::uint64_t, 13> values{
      0, 1, 59, 60, 999999, 1000000, 1000001, 2000000,
      max / 1000000, max / 1000000 + 1, max - 1000000, max - 1, max};
  for (const auto observed : values) for (const auto now : values) for (const auto seconds : values)
    Check(m::MetricRetentionTimeExpired(observed, now, seconds) == Oracle(observed, now, seconds),
          "retention boundary or overflow changed expiration");
  std::uint64_t state = 0x123456789abcdef;
  auto next = [&] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
  for (unsigned i = 0; i < 10000; ++i) {
    const auto observed = next(), now = next();
    const auto seconds = i % 2 ? next() : next() % (max / 1000000 + 1);
    Check(m::MetricRetentionTimeExpired(observed, now, seconds) == Oracle(observed, now, seconds),
          "retention randomized arithmetic mismatch");
  }
}
}
int main() {
  Identity(); Definitions(); RetentionTime();
  std::cout << "metric_retention_policy_identity checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
