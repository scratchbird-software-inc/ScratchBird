// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PARSER_PACKAGE_LIFECYCLE

#include "parser_package_registry.hpp"

#include "agent_feature_gates.hpp"
#include "sb_udr_runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>

namespace scratchbird::server {

namespace {

namespace agents = scratchbird::core::agents;
namespace udr_runtime = scratchbird::udr::runtime;

std::string Trim(std::string value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 'a');
    return static_cast<char>(ch);
  });
  return value;
}

bool ParseBool(const std::string& value, bool fallback = false) {
  const auto lower = LowerAscii(value);
  if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") return true;
  if (lower == "false" || lower == "no" || lower == "off" || lower == "0") return false;
  return fallback;
}

std::uint64_t ParseU64(const std::string& value, std::uint64_t fallback) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::uint32_t ParseU32(const std::string& value, std::uint32_t fallback) {
  return static_cast<std::uint32_t>(ParseU64(value, fallback));
}

std::uint16_t ParseU16(const std::string& value, std::uint16_t fallback) {
  return static_cast<std::uint16_t>(ParseU64(value, fallback));
}

ServerDiagnostic ParserPackageDiagnostic(std::string code,
                                         std::string message,
                                         std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

std::string BytesToHex(const std::array<std::uint8_t, 32>& bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const auto byte : bytes) {
    out.push_back(hex[(byte >> 4u) & 0x0fu]);
    out.push_back(hex[byte & 0x0fu]);
  }
  return out;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

ParserPackageRegistry DefaultRegistry() {
  ParserPackageRegistry registry;
  registry.entries.push_back(ParserPackageRegistryEntry{});
  return registry;
}

std::map<std::string, std::string> ReadKeyValueFile(const std::filesystem::path& path,
                                                    std::vector<ServerDiagnostic>* diagnostics) {
  std::ifstream in(path);
  std::map<std::string, std::string> values;
  if (!in) {
    diagnostics->push_back(ParserPackageDiagnostic(
        "SERVER.PARSER.REGISTRY_UNREADABLE",
        "The parser package registry file could not be read.",
        {{"registry_path", path.string()}}));
    return values;
  }
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      diagnostics->push_back(ParserPackageDiagnostic(
          "SERVER.PARSER.REGISTRY_MALFORMED",
          "The parser package registry contains a malformed line."));
      return {};
    }
    values[LowerAscii(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
  }
  return values;
}

bool VersionInRange(std::uint16_t major,
                    std::uint16_t minor,
                    const ParserPackageRegistryEntry& entry) {
  if (major < entry.sbps_min_major || major > entry.sbps_max_major) return false;
  if (major == entry.sbps_min_major && minor < entry.sbps_min_minor) return false;
  if (major == entry.sbps_max_major && minor > entry.sbps_max_minor) return false;
  return true;
}

bool StateAllowsAdmission(const std::string& state) {
  return state == "enabled" || state == "registered";
}

agents::CapabilityEditionScope EditionScopeFromText(const std::string& value) {
  const auto lower = LowerAscii(value);
  if (lower == "cluster") return agents::CapabilityEditionScope::cluster;
  if (lower == "enterprise") return agents::CapabilityEditionScope::enterprise;
  if (lower == "private_build") return agents::CapabilityEditionScope::private_build;
  return agents::CapabilityEditionScope::community;
}

agents::CapabilityLifecycleState CapabilityStateForEntry(
    const ParserPackageRegistryEntry& entry) {
  if (!entry.capability_installed) return agents::CapabilityLifecycleState::retired;
  if (!entry.capability_enabled || entry.state == "disabled") {
    return agents::CapabilityLifecycleState::disabled;
  }
  if (entry.state == "quarantined") return agents::CapabilityLifecycleState::quarantined;
  return agents::CapabilityLifecycleState::enabled;
}

ServerDiagnostic CapabilityDiagnostic(const agents::FeatureGateDecision& decision,
                                      const ParserPackageRegistryEntry& entry) {
  return ParserPackageDiagnostic(
      decision.diagnostic_code.empty() ? "SERVER.PARSER.CAPABILITY_DENIED"
                                       : decision.diagnostic_code,
      "Parser package capability profile admission failed.",
      {{"parser_package_uuid", entry.parser_package_uuid},
       {"capability_id", entry.required_capability_id},
       {"decision", agents::FeatureGateDecisionClassName(decision.decision_class)},
       {"detail", decision.detail}});
}

agents::FeatureGateDecision EvaluateParserCapability(
    const ParserPackageRegistryEntry& entry,
    std::uint64_t registry_generation) {
  agents::InstalledCapabilityRecord record;
  record.capability_id = entry.required_capability_id;
  record.provider_id = entry.capability_provider_id;
  record.edition_scope = EditionScopeFromText(entry.capability_edition_scope);
  record.lifecycle_state = CapabilityStateForEntry(entry);
  record.capability_epoch = entry.capability_epoch;
  record.installed_policy_epoch = entry.capability_policy_generation;
  record.requires_parser_package = true;
  record.required_parser_package_id = entry.parser_package_uuid;
  record.cluster_authority_required = entry.cluster_capability_required;
  record.parser_package_installed = StateAllowsAdmission(entry.state);

  agents::FeatureGateRequest request;
  request.request_id = "server.parser_package_admission";
  request.capability_id = entry.required_capability_id;
  request.requested_edition_scope = agents::CapabilityEditionScope::community;
  request.observed_policy_epoch = registry_generation == 0 ? 1 : registry_generation;
  request.minimum_capability_epoch = 1;
  request.cluster_authority_available = false;
  request.parser_package_available = StateAllowsAdmission(entry.state);
  return agents::EvaluateFeatureGateRequest(record,
                                            request,
                                            registry_generation == 0 ? 1 : registry_generation);
}

std::optional<ServerDiagnostic> ValidateParserSupportUdrDependency(
    const ParserPackageRegistryEntry& entry) {
  if (!entry.parser_support_udr_required) return std::nullopt;
  if (!entry.parser_support_udr_available || entry.parser_support_udr_uuid.empty()) {
    return ParserPackageDiagnostic(
        "SERVER.PARSER.SUPPORT_UDR_MISSING",
        "A required parser-support UDR dependency is unavailable.",
        {{"parser_package_uuid", entry.parser_package_uuid},
         {"parser_support_udr_uuid",
          entry.parser_support_udr_uuid.empty() ? "missing" : entry.parser_support_udr_uuid}});
  }

  const auto state = udr_runtime::GetPackageState(entry.parser_support_udr_uuid);
  if (!state || !state->registered || !state->loaded) {
    return ParserPackageDiagnostic(
        "SERVER.PARSER.SUPPORT_UDR_MISSING",
        "The required parser-support UDR is not loaded in the trusted runtime.",
        {{"parser_package_uuid", entry.parser_package_uuid},
         {"parser_support_udr_uuid", entry.parser_support_udr_uuid},
         {"runtime_state", state ? "registered_not_loaded" : "not_registered"}});
  }

  if ((!entry.parser_support_udr_abi.empty() &&
       state->abi_version != entry.parser_support_udr_abi) ||
      (!entry.parser_support_udr_source_revision.empty() &&
       state->source_revision != entry.parser_support_udr_source_revision) ||
      (!entry.parser_support_udr_binary_hash.empty() &&
       state->binary_hash != entry.parser_support_udr_binary_hash) ||
      (!entry.parser_support_udr_signature_policy.empty() &&
       state->signature_policy != entry.parser_support_udr_signature_policy) ||
      (!entry.parser_support_udr_capability_role.empty() &&
       state->capability_role != entry.parser_support_udr_capability_role)) {
    return ParserPackageDiagnostic(
        "SERVER.PARSER.SUPPORT_UDR_REJECTED",
        "The parser-support UDR runtime descriptor does not match registry authority.",
        {{"parser_package_uuid", entry.parser_package_uuid},
         {"parser_support_udr_uuid", entry.parser_support_udr_uuid},
         {"runtime_abi", state->abi_version},
         {"runtime_source_revision", state->source_revision},
         {"runtime_capability_role", state->capability_role}});
  }

  return std::nullopt;
}

}  // namespace

ParserPackageRegistry LoadParserPackageRegistry(const ServerBootstrapConfig& config) {
  ParserPackageRegistry registry = DefaultRegistry();
  if (config.parser_registry_path.empty() || !std::filesystem::is_regular_file(config.parser_registry_path)) {
    return registry;
  }

  registry = {};
  registry.source = config.parser_registry_path.string();
  auto values = ReadKeyValueFile(config.parser_registry_path, &registry.diagnostics);
  if (!registry.diagnostics.empty()) return registry;
  if (values["format"] != "SBPPR1") {
    registry.diagnostics.push_back(ParserPackageDiagnostic(
        "SERVER.PARSER.REGISTRY_VERSION_UNSUPPORTED",
        "The parser package registry format is unsupported.",
        {{"format", values["format"]}}));
    return registry;
  }

  ParserPackageRegistryEntry entry;
  registry.generation = ParseU64(values["generation"], 1);
  registry.capability_policy_generation =
      ParseU64(values["capability_policy_generation"], 1);
  entry.parser_package_uuid = values.contains("parser_package_uuid") ? values["parser_package_uuid"] : entry.parser_package_uuid;
  entry.parser_family_uuid = values.contains("parser_family_uuid") ? values["parser_family_uuid"] : entry.parser_family_uuid;
  entry.dialect_profile_uuid = values.contains("dialect_profile_uuid") ? values["dialect_profile_uuid"] : entry.dialect_profile_uuid;
  entry.required_capability_id = values.contains("required_capability_id") ? values["required_capability_id"] : entry.required_capability_id;
  entry.capability_provider_id = values.contains("capability_provider_id") ? values["capability_provider_id"] : entry.capability_provider_id;
  entry.capability_edition_scope = values.contains("capability_edition_scope") ? LowerAscii(values["capability_edition_scope"]) : entry.capability_edition_scope;
  entry.resource_bundle_hash = values.contains("resource_bundle_hash") ? LowerAscii(values["resource_bundle_hash"]) : entry.resource_bundle_hash;
  entry.state = values.contains("state") ? LowerAscii(values["state"]) : entry.state;
  entry.capability_epoch = ParseU64(values["capability_epoch"], entry.capability_epoch);
  entry.capability_policy_generation = ParseU64(values["capability_policy_generation"], entry.capability_policy_generation);
  entry.parser_api_major = ParseU32(values["parser_api_major"], entry.parser_api_major);
  entry.parser_api_minor = ParseU32(values["parser_api_minor"], entry.parser_api_minor);
  entry.sbps_min_major = ParseU16(values["sbps_min_major"], entry.sbps_min_major);
  entry.sbps_min_minor = ParseU16(values["sbps_min_minor"], entry.sbps_min_minor);
  entry.sbps_max_major = ParseU16(values["sbps_max_major"], entry.sbps_max_major);
  entry.sbps_max_minor = ParseU16(values["sbps_max_minor"], entry.sbps_max_minor);
  entry.match_builtin_test_profile = ParseBool(values["match_builtin_test_profile"], entry.match_builtin_test_profile);
  entry.hash_required = ParseBool(values["hash_required"], entry.hash_required);
  entry.signature_required = ParseBool(values["signature_required"], entry.signature_required);
  entry.signature_present = ParseBool(values["signature_present"], entry.signature_present);
  entry.dev_hash_bypass = ParseBool(values["dev_hash_bypass"], entry.dev_hash_bypass);
  entry.parser_support_udr_required = ParseBool(values["parser_support_udr_required"], entry.parser_support_udr_required);
  entry.parser_support_udr_available = ParseBool(values["parser_support_udr_available"], entry.parser_support_udr_available);
  entry.parser_support_udr_uuid = values.contains("parser_support_udr_uuid") ? values["parser_support_udr_uuid"] : entry.parser_support_udr_uuid;
  entry.parser_support_udr_abi = values.contains("parser_support_udr_abi") ? values["parser_support_udr_abi"] : entry.parser_support_udr_abi;
  entry.parser_support_udr_source_revision = values.contains("parser_support_udr_source_revision") ? values["parser_support_udr_source_revision"] : entry.parser_support_udr_source_revision;
  entry.parser_support_udr_binary_hash = values.contains("parser_support_udr_binary_hash") ? values["parser_support_udr_binary_hash"] : entry.parser_support_udr_binary_hash;
  entry.parser_support_udr_signature_policy = values.contains("parser_support_udr_signature_policy") ? values["parser_support_udr_signature_policy"] : entry.parser_support_udr_signature_policy;
  entry.parser_support_udr_capability_role = values.contains("parser_support_udr_capability_role") ? values["parser_support_udr_capability_role"] : entry.parser_support_udr_capability_role;
  entry.capability_installed = ParseBool(values["capability_installed"], entry.capability_installed);
  entry.capability_enabled = ParseBool(values["capability_enabled"], entry.capability_enabled);
  entry.cluster_capability_required = ParseBool(values["cluster_capability_required"], entry.cluster_capability_required);
  entry.failure_count_10m = ParseU64(values["failure_count_10m"], entry.failure_count_10m);
  entry.failure_count_1h = ParseU64(values["failure_count_1h"], entry.failure_count_1h);
  if (ParserPackageShouldQuarantine(entry)) entry.state = "quarantined";
  registry.entries.push_back(std::move(entry));
  return registry;
}

ParserPackageAdmissionResult AdmitParserPackage(const ParserPackageRegistry& registry,
                                                const sbps::HelloRequest& hello,
                                                std::uint16_t protocol_major,
                                                std::uint16_t protocol_minor) {
  ParserPackageAdmissionResult result;
  result.registry_generation = registry.generation;
  if (!registry.diagnostics.empty()) {
    result.diagnostics = registry.diagnostics;
    return result;
  }
  if (registry.entries.empty()) {
    result.diagnostics.push_back(ParserPackageDiagnostic(
        "SERVER.PARSER.PACKAGE_REJECTED",
        "No parser package is registered for this endpoint."));
    return result;
  }

  for (const auto& entry : registry.entries) {
    if (entry.match_builtin_test_profile && !sbps::IsBuiltInTestHello(hello)) {
      continue;
    }
    result.entry = entry;
    if (!VersionInRange(protocol_major, protocol_minor, entry) ||
        hello.parser_api_major != entry.parser_api_major) {
      result.diagnostics.push_back(ParserPackageDiagnostic(
          "SERVER.VERSION.INCOMPATIBLE",
          "The parser package version is incompatible with this endpoint.",
          {{"parser_package_uuid", entry.parser_package_uuid}}));
      return result;
    }
    if (!StateAllowsAdmission(entry.state)) {
      result.diagnostics.push_back(ParserPackageDiagnostic(
          "SERVER.PARSER.PACKAGE_REJECTED",
          "The parser package state does not allow new channels.",
          {{"parser_package_uuid", entry.parser_package_uuid}, {"state", entry.state}}));
      return result;
    }
    const auto capability_decision =
        EvaluateParserCapability(entry, registry.capability_policy_generation);
    if (capability_decision.decision_class != agents::FeatureGateDecisionClass::allow) {
      result.diagnostics.push_back(CapabilityDiagnostic(capability_decision, entry));
      return result;
    }
    if (entry.dev_hash_bypass) {
      result.diagnostics.push_back(ParserPackageDiagnostic(
          "SERVER.PARSER.PACKAGE_ATTESTATION_REQUIRED",
          "Parser package development hash bypass is forbidden in server lifecycle admission.",
          {{"parser_package_uuid", entry.parser_package_uuid}}));
      return result;
    }
    if (entry.hash_required) {
      const auto actual_hash = BytesToHex(hello.resource_bundle_hash);
      if (entry.resource_bundle_hash != "builtin" && entry.resource_bundle_hash != actual_hash) {
        result.diagnostics.push_back(ParserPackageDiagnostic(
            "SERVER.PARSER.PACKAGE_ATTESTATION_REQUIRED",
            "The parser package resource bundle hash does not match registry authority.",
            {{"parser_package_uuid", entry.parser_package_uuid}}));
        return result;
      }
    }
    if (entry.signature_required && !entry.signature_present) {
      result.diagnostics.push_back(ParserPackageDiagnostic(
          "SERVER.PARSER.PACKAGE_ATTESTATION_REQUIRED",
          "The parser package signature is required but missing.",
          {{"parser_package_uuid", entry.parser_package_uuid}}));
      return result;
    }
    if (const auto support_udr_failure = ValidateParserSupportUdrDependency(entry)) {
      result.diagnostics.push_back(*support_udr_failure);
      return result;
    }
    result.admitted = true;
    return result;
  }

  result.diagnostics.push_back(ParserPackageDiagnostic(
      "SERVER.PARSER.PACKAGE_REJECTED",
      "The parser package manifest does not match any registered package."));
  return result;
}

ParserChildLaunchPolicy DefaultParserChildLaunchPolicy() {
  ParserChildLaunchPolicy policy;
  policy.env_whitelist = {"PATH", "LANG", "LC_ALL", "TZ", "SCRATCHBIRD_CONFIG"};
  policy.inherited_handles = {"stdin:closed", "stdout:server_log_pipe", "stderr:server_log_pipe", "sbps:endpoint_descriptor"};
  return policy;
}

std::uint64_t ParserChildRestartDelayMs(const std::string& package_uuid,
                                        std::uint64_t attempt_index) {
  std::uint64_t base = 1000;
  for (std::uint64_t i = 0; i < attempt_index && base < 60000; ++i) {
    base *= 2;
  }
  if (base > 60000) base = 60000;
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto ch : package_uuid) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  hash ^= attempt_index;
  return base + (hash % 251);
}

bool ParserPackageShouldQuarantine(const ParserPackageRegistryEntry& entry) {
  return entry.failure_count_10m >= 5 || entry.failure_count_1h >= 10;
}

std::string ParserPackageRegistryStatusJson(const ParserPackageRegistry& registry) {
  const auto child_policy = DefaultParserChildLaunchPolicy();
  std::ostringstream out;
  out << "{\"parser_package_registry\":{\"generation\":" << registry.generation
      << ",\"capability_policy_generation\":" << registry.capability_policy_generation
      << ",\"source\":\"" << JsonEscape(registry.source)
      << "\",\"package_count\":" << registry.entries.size()
      << ",\"diagnostic_count\":" << registry.diagnostics.size()
      << ",\"child_policy\":{\"memory_bytes\":" << child_policy.memory_bytes
      << ",\"open_handle_limit\":" << child_policy.open_handle_limit
      << ",\"restart_delay_attempt0_ms\":"
      << ParserChildRestartDelayMs(registry.entries.empty() ? "none" : registry.entries.front().parser_package_uuid, 0)
      << "},\"packages\":[";
  for (std::size_t i = 0; i < registry.entries.size(); ++i) {
    if (i != 0) out << ',';
    const auto& entry = registry.entries[i];
    out << "{\"parser_package_uuid\":\"" << JsonEscape(entry.parser_package_uuid)
        << "\",\"state\":\"" << JsonEscape(entry.state)
        << "\",\"required_capability_id\":\"" << JsonEscape(entry.required_capability_id)
        << "\",\"capability_policy_generation\":" << entry.capability_policy_generation
        << ",\"hash_required\":" << (entry.hash_required ? "true" : "false")
        << ",\"dev_hash_bypass\":" << (entry.dev_hash_bypass ? "true" : "false")
        << ",\"parser_support_udr_required\":" << (entry.parser_support_udr_required ? "true" : "false")
        << ",\"parser_support_udr_available\":" << (entry.parser_support_udr_available ? "true" : "false")
        << ",\"parser_support_udr_uuid\":\"" << JsonEscape(entry.parser_support_udr_uuid)
        << "\",\"parser_support_udr_abi\":\"" << JsonEscape(entry.parser_support_udr_abi)
        << "\",\"parser_support_udr_source_revision\":\""
        << JsonEscape(entry.parser_support_udr_source_revision)
        << "\",\"parser_support_udr_capability_role\":\""
        << JsonEscape(entry.parser_support_udr_capability_role)
        << "\",\"parser_support_udr_runtime_loaded\":";
    const auto support_udr_state = entry.parser_support_udr_uuid.empty()
                                       ? std::nullopt
                                       : udr_runtime::GetPackageState(entry.parser_support_udr_uuid);
    out << (support_udr_state && support_udr_state->loaded ? "true" : "false")
        << ",\"capability_enabled\":" << (entry.capability_enabled ? "true" : "false")
        << ",\"cluster_capability_required\":" << (entry.cluster_capability_required ? "true" : "false")
        << "}";
  }
  out << "]}}\n";
  return out.str();
}

}  // namespace scratchbird::server
