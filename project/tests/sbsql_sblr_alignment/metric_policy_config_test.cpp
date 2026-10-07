// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_policy_config.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

namespace {
namespace c = scratchbird::core::config;
namespace m = scratchbird::core::metrics;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::cerr << message << '\n'; }
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::string Replace(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  Check(at != std::string::npos, "fixture replacement missing");
  if (at != std::string::npos) text.replace(at, from.size(), to);
  return text;
}
void Rejected(const std::string& defaults, const std::string& families) {
  const auto r = c::ParseMetricPolicyConfig(defaults, families);
  Check(!r.ok() && !r.config && r.validation.diagnostic_code == "METRIC.RETENTION_POLICY_INVALID" &&
        !r.validation.detail.empty(), "invalid config did not fail atomically with diagnostics");
}
template<class T> concept HasUuid = requires(T t) { t.policy_uuid; };
static_assert(!HasUuid<c::MetricPolicyDefinition>);
static_assert(!HasUuid<m::MetricRetentionPolicyDefinition>);
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::filesystem::path directory(argv[1]);
  const auto dp = directory / "metric-policy-defaults.conf", fp = directory / "metric-policy-families.conf";
  const auto d = Read(dp), f = Read(fp);
  Check(!d.empty() && !f.empty(), "distributed configs missing");
  const auto loaded = c::LoadMetricPolicyConfig(dp, fp);
  Check(loaded.ok(), "distributed config did not load");
  if (!loaded.ok()) { std::cerr << loaded.validation.detail << '\n'; return 1; }
  Check(loaded.config->families.size() == 6, "bootstrap family inventory changed");
  for (const auto* family : {"sb_tx_begin_total", "sb_tx_commit_total",
                            "sb_mga_cleanup_horizon_local_transaction_id", "sb_metric_samples_rejected_total"}) {
    const auto s = c::SelectMetricPolicyDefinition(*loaded.config, family);
    Check(s.ok(), "default family not selected");
    if (!s.ok()) continue;
    const auto& p = *s.definition;
    Check(p.retention.raw_retention_seconds == 14ull * 86400 &&
          p.retention.rollup_retention_seconds == 400ull * 86400 &&
          p.retention.rollup_grains == std::vector<m::MetricRollupGrain>{
            m::MetricRollupGrain::one_minute, m::MetricRollupGrain::one_hour, m::MetricRollupGrain::one_day} &&
          p.read_right == "OBS_METRICS_READ_DATABASE" && p.redact_sensitive_labels, "approved defaults changed");
  }
  const auto active = c::SelectMetricPolicyDefinition(*loaded.config, "sb_tx_active_transactions");
  const auto storage = c::SelectMetricPolicyDefinition(*loaded.config, "sb_filespace_used_bytes");
  Check(active.ok() && active.definition->retention.max_cardinality == 1024, "transaction cardinality overwritten");
  Check(storage.ok() && storage.definition->retention.raw_retention_seconds == 30ull * 86400 &&
        storage.definition->read_right == "OBS_METRICS_READ_FAMILY", "storage requirements overwritten");
  for (const auto* family : {"", "sb_tx_begin_total_extra", "SB_TX_BEGIN_TOTAL", "sb_auth_failures_total",
                            "sb_agent_actions_total", "cluster", "*", "sb_tx_*"})
    Check(!c::SelectMetricPolicyDefinition(*loaded.config, family).ok(), "unknown family inherited defaults");
  auto governing = loaded.config->operational_defaults;
  governing.retention.raw_retention_seconds = 400ull * 86400; governing.read_right = "SEC_AUTH_METRICS_READ";
  const auto s = c::SelectMetricPolicyDefinition(*loaded.config, "sb_tx_begin_total", &governing);
  Check(s.ok() && s.definition->retention.raw_retention_seconds == 400ull * 86400 &&
        s.definition->read_right == "SEC_AUTH_METRICS_READ", "governing policy lost precedence");
  governing.retention.evidence_required = false;
  Check(!c::SelectMetricPolicyDefinition(*loaded.config, "sb_tx_begin_total", &governing).ok(), "invalid governing fallback");
  const auto changed = c::ParseMetricPolicyConfig(Replace(d, "raw_retention_seconds = 1209600", "raw_retention_seconds = 604800"), f);
  Check(changed.ok() && changed.config->families.at("sb_tx_begin_total").retention.raw_retention_seconds == 604800 &&
        changed.config->families.at("sb_tx_active_transactions").retention.raw_retention_seconds == 1209600 &&
        changed.config->families.at("sb_filespace_used_bytes").retention.raw_retention_seconds == 2592000, "explicit rows inherited defaults");
  const auto override = c::ParseMetricPolicyConfig(d, Replace(f, "[sb_tx_begin_total]\nprofile = operational",
      "[sb_tx_begin_total]\nprofile = operational\nmax_cardinality = 32"));
  Check(override.ok() && override.config->families.at("sb_tx_begin_total").retention.max_cardinality == 32, "override lost");
  Rejected("", f); Rejected(d, "");
  for (const auto* suffix : {"unknown = yes\n", "policy_uuid = textual_uuid\n", "scope = database\n", "[operational_defaults]\n"}) Rejected(d + suffix, f);
  for (const auto* suffix : {"[sb_tx_begin_total]\nprofile = operational\n", "[sb_*]\nprofile = operational\n", "oops\n"}) Rejected(d, f + suffix);
  for (const auto* replacement : {"profile = unknown", "profile = explicit", "unknown = operational"})
    Rejected(d, Replace(f, "profile = operational", replacement));
  Rejected(d, Replace(f, "[sb_tx_begin_total]", "[sb_tx_begin_total"));
  Rejected("schema = scratchbird_metric_policy_v1\n" + d, f);
  Rejected(Replace(d, "scratchbird_metric_policy_v1", "scratchbird_metric_policy_v2"), f);
  Rejected(d, Replace(f, "scratchbird_metric_policy_v1", "scratchbird_metric_policy_v2"));
  for (const auto* value : {"-1", "+1", "1.0", "1s", "", "18446744073709551616", "0"})
    Rejected(Replace(d, "raw_retention_seconds = 1209600", std::string("raw_retention_seconds = ") + value), f);
  const auto maximum = c::ParseMetricPolicyConfig(Replace(d, "raw_retention_seconds = 1209600", "raw_retention_seconds = 18446744073709551615"), f);
  Check(maximum.ok() && maximum.config->operational_defaults.retention.raw_retention_seconds == std::numeric_limits<m::u64>::max(), "uint64 maximum wrapped");
  for (const auto* value : {"1m,1m", "1m,", ",1h", "2m", "1m,,1h"})
    Rejected(Replace(d, "rollup_grains = 1m,1h,1d", std::string("rollup_grains = ") + value), f);
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
      {"mode = raw_and_rollup", "mode = bogus"}, {"scope = database", "scope = cluster"},
      {"evidence_required = true", "evidence_required = false"}, {"purge_batch_limit = 1024", "purge_batch_limit = 0"},
      {"max_cardinality = 4096", "max_cardinality = 0"}, {"sensitive_labels = redact", "sensitive_labels = expose"},
      {"read_right = OBS_METRICS_READ_DATABASE", "read_right = guessed right"}, {"overflow_behavior = reject_and_evidence", "overflow_behavior = drop"}})
    Rejected(Replace(d, from, to), f);
  for (const char byte : {'\0', '\x01', '\x7f', '\xff'}) Rejected(d + byte, f);
  Rejected(d + "#" + std::string(2048, 'x'), f);
  auto boundary = d; boundary.resize(262144, '\n');
  Check(c::ParseMetricPolicyConfig(boundary, f).ok(), "maximum file size rejected"); Rejected(boundary + '\n', f);
  auto many = std::string("[config]\nschema = scratchbird_metric_policy_v1\n");
  for (unsigned i = 0; i < 1024; ++i) many += "[family_" + std::to_string(i) + "]\nprofile = operational\n";
  Check(c::ParseMetricPolicyConfig(d, many).ok(), "maximum family inventory rejected");
  Rejected(d, many + "[overflow]\nprofile = operational\n");
  Check(!c::LoadMetricPolicyConfig(directory, fp).ok(), "directory accepted as config");
  Check(!c::LoadMetricPolicyConfig(dp / "missing", fp).ok(), "missing defaults silently replaced");
  Check(!c::LoadMetricPolicyConfig(dp, fp / "missing").ok(), "missing family config silently replaced");
  Check(c::ParseMetricPolicyConfig(d + "# trailing comment", f).ok(), "final comment rejected");
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
