// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_policy_config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>

namespace scratchbird::core::config {
namespace {
constexpr std::size_t kMaximumBytes = 262144;
constexpr std::size_t kMaximumFamilies = 1024;
using Fields = std::map<std::string, std::string, std::less<>>;
using Sections = std::map<std::string, Fields, std::less<>>;
constexpr std::array<std::string_view, 14> kFields = {
    "policy_name", "scope", "mode", "raw_retention_seconds",
    "rollup_retention_seconds", "rollup_grains", "purge_batch_limit",
    "max_cardinality", "overflow_behavior", "edit_right", "default_admin_group",
    "evidence_required", "read_right", "sensitive_labels"};

metrics::MetricValidationResult Invalid(std::string detail) {
  return {false, "METRIC.RETENTION_POLICY_INVALID", std::move(detail)};
}
MetricPolicyConfigResult Failure(std::string detail) {
  return {std::nullopt, Invalid(std::move(detail))};
}
std::string_view Trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
bool Identifier(std::string_view value, bool upper = false) {
  if (value.empty() || value.size() > 128) return false;
  return std::all_of(value.begin(), value.end(), [upper](char c) {
    return c == '_' || (c >= '0' && c <= '9') ||
           (upper ? c >= 'A' && c <= 'Z' : c >= 'a' && c <= 'z');
  });
}
bool Parse(std::string_view input, Sections* sections, std::string* detail) {
  if (input.size() > kMaximumBytes) { *detail = "config_too_large"; return false; }
  for (const unsigned char ch : input) {
    if ((ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') || ch > 126) {
      *detail = "invalid_config_byte"; return false;
    }
  }
  std::string section;
  std::size_t line_number = 0;
  while (!input.empty()) {
    ++line_number;
    const auto newline = input.find('\n');
    const auto line = Trim(input.substr(0, newline));
    input = newline == std::string_view::npos ? std::string_view{} : input.substr(newline + 1);
    const auto fail = [&](const char* reason) {
      *detail = std::string(reason) + ":line=" + std::to_string(line_number); return false;
    };
    if (line.size() > 2048) return fail("line_too_long");
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') {
      if (line.back() != ']' || line.size() < 3) return fail("invalid_section");
      section = std::string(line.substr(1, line.size() - 2));
      if (!Identifier(section) || sections->contains(section) ||
          sections->size() >= kMaximumFamilies + 1) return fail("invalid_or_duplicate_section");
      sections->emplace(section, Fields{});
    } else {
      const auto equal = line.find('=');
      if (section.empty() || equal == std::string_view::npos) return fail("expected_section_and_key");
      const auto key = Trim(line.substr(0, equal));
      const auto value = Trim(line.substr(equal + 1));
      if (!Identifier(key) || value.find('=') != std::string_view::npos ||
          !sections->at(section).emplace(key, value).second) return fail("invalid_or_duplicate_key");
    }
  }
  return true;
}
bool Number(const std::string& value, metrics::u64* out) {
  const auto result = std::from_chars(value.data(), value.data() + value.size(), *out);
  return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}
metrics::MetricValidationResult Validate(const MetricPolicyDefinition& policy) {
  const auto retention = metrics::ValidateMetricRetentionPolicyDefinition(policy.retention);
  if (!retention.ok) return retention;
  if (!Identifier(policy.read_right, true) || !policy.redact_sensitive_labels)
    return Invalid("invalid_read_right_or_sensitive_label_handling");
  if (policy.retention.scope == "cluster") return Invalid("cluster_policy_requires_cluster_authority");
  return {true, {}, {}};
}
metrics::MetricValidationResult Decode(const Fields& fields, MetricPolicyDefinition* policy) {
  for (const auto key : kFields)
    if (!fields.contains(key)) return Invalid("missing_field:" + std::string(key));
  for (const auto& [key, value] : fields)
    if (std::find(kFields.begin(), kFields.end(), key) == kFields.end())
      return Invalid("unknown_field:" + key);
  auto& p = policy->retention;
  p.policy_name = fields.at("policy_name"); p.scope = fields.at("scope");
  p.mode = metrics::MetricRetentionModeFromName(fields.at("mode"));
  if (!Number(fields.at("raw_retention_seconds"), &p.raw_retention_seconds) ||
      !Number(fields.at("rollup_retention_seconds"), &p.rollup_retention_seconds) ||
      !Number(fields.at("purge_batch_limit"), &p.purge_batch_limit) ||
      !Number(fields.at("max_cardinality"), &p.max_cardinality)) return Invalid("invalid_unsigned_integer");
  std::string_view grains = fields.at("rollup_grains");
  p.rollup_grains.clear();
  while (!grains.empty()) {
    const auto comma = grains.find(',');
    p.rollup_grains.push_back(metrics::MetricRollupGrainFromName(std::string(Trim(grains.substr(0, comma)))));
    if (comma == std::string_view::npos) break;
    grains.remove_prefix(comma + 1);
    if (grains.empty()) return Invalid("empty_rollup_grain");
  }
  p.overflow_behavior = fields.at("overflow_behavior"); p.edit_right = fields.at("edit_right");
  p.default_admin_group = fields.at("default_admin_group");
  if (fields.at("evidence_required") != "true") return Invalid("evidence_required_must_be_true");
  p.evidence_required = true;
  policy->read_right = fields.at("read_right");
  if (fields.at("sensitive_labels") != "redact") return Invalid("sensitive_labels_must_be_redacted");
  policy->redact_sensitive_labels = true;
  return Validate(*policy);
}
bool Read(const std::filesystem::path& path, std::string* contents) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  std::array<char, 4096> chunk{};
  while (input) {
    input.read(chunk.data(), chunk.size());
    if (contents->size() + static_cast<std::size_t>(input.gcount()) > kMaximumBytes) return false;
    contents->append(chunk.data(), static_cast<std::size_t>(input.gcount()));
  }
  return input.eof() && !input.bad();
}
}  // namespace

MetricPolicyConfigResult ParseMetricPolicyConfig(std::string_view defaults,
                                                std::string_view families) {
  Sections baseline, selected;
  std::string detail;
  if (!Parse(defaults, &baseline, &detail)) return Failure("defaults:" + detail);
  if (!Parse(families, &selected, &detail)) return Failure("families:" + detail);
  const Fields header{{"schema", "scratchbird_metric_policy_v1"}};
  if (!baseline.contains("config") || baseline.at("config") != header ||
      !selected.contains("config") || selected.at("config") != header)
    return Failure("invalid_schema_header");
  if (baseline.size() != 2 || !baseline.contains("operational_defaults"))
    return Failure("expected_operational_defaults_only");
  MetricPolicyConfig result;
  const auto& default_fields = baseline.at("operational_defaults");
  const auto valid = Decode(default_fields, &result.operational_defaults);
  if (!valid.ok) return Failure("defaults:" + valid.detail);
  if (selected.size() < 2) return Failure("empty_family_selection");
  for (const auto& [family, fields] : selected) {
    if (family == "config") continue;
    if (!fields.contains("profile")) return Failure("missing_family_profile:" + family);
    const auto& profile = fields.at("profile");
    if (profile != "operational" && profile != "explicit") return Failure("invalid_family_profile:" + family);
    Fields effective = profile == "operational" ? default_fields : Fields{};
    for (const auto& [key, value] : fields) if (key != "profile") effective[key] = value;
    MetricPolicyDefinition policy;
    const auto checked = Decode(effective, &policy);
    if (!checked.ok) return Failure("family:" + family + ":" + checked.detail);
    result.families.emplace(family, std::move(policy));
  }
  return {std::move(result), {true, {}, {}}};
}

MetricPolicyConfigResult LoadMetricPolicyConfig(const std::filesystem::path& defaults,
                                               const std::filesystem::path& families) {
  std::string default_text, family_text;
  if (!Read(defaults, &default_text)) return Failure("defaults:read_failed_or_size_limit");
  if (!Read(families, &family_text)) return Failure("families:read_failed_or_size_limit");
  return ParseMetricPolicyConfig(default_text, family_text);
}

MetricPolicySelection SelectMetricPolicyDefinition(
    const MetricPolicyConfig& config, std::string_view family,
    const MetricPolicyDefinition* governing_definition) {
  if (!Identifier(family)) return {std::nullopt, Invalid("invalid_family")};
  const auto found = config.families.find(family);
  const auto* policy = governing_definition;
  if (!policy && found != config.families.end()) policy = &found->second;
  if (!policy) return {std::nullopt, Invalid("family_not_explicitly_selected")};
  const auto valid = Validate(*policy);
  if (!valid.ok) return {std::nullopt, valid};
  return {*policy, valid};
}
}  // namespace scratchbird::core::config
