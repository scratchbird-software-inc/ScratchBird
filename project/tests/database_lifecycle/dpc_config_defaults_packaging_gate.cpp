// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_optimization_surface.hpp"
#include "management/support_bundle_api.hpp"

// SEARCH_KEY: DPC_CONFIG_PACKAGING
// SEARCH_KEY: DPC_CONFIG_DEFAULTS_PACKAGING_GATE

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

constexpr std::string_view kPrecedence =
    "admin_override > cli_option > environment > config_file > packaged_default";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

const api::PerformanceOptimizationConfigSurfaceField& ConfigField(
    std::string_view name) {
  for (const auto& field : api::PerformanceOptimizationConfigSurface()) {
    if (field.surface_name == name) {
      return field;
    }
  }
  Fail(std::string("DPC-068 config field missing: ") + std::string(name));
}

const api::PerformanceOptimizationConfigEffectiveValue& EffectiveField(
    const api::PerformanceOptimizationConfigResolution& resolution,
    std::string_view name) {
  for (const auto& field : resolution.fields) {
    if (field.metadata.surface_name == name) {
      return field;
    }
  }
  Fail(std::string("DPC-068 effective field missing: ") + std::string(name));
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field_name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name) {
        return field.second.encoded_value;
      }
    }
  }
  return {};
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.request_id = "dpc068-config-defaults-packaging";
  context.catalog_generation_id = 68;
  context.security_epoch = 68;
  context.resource_epoch = 68;
  context.trace_tags = {
      "right:OBS_MANAGEMENT_INSPECT",
      "right:OBS_CONFIG_INSPECT",
      "dpc_config_defaults_packaging_gate"};
  return context;
}

void TestPackagedDefaultsAndMetadata() {
  const std::map<std::string, std::string> expected_defaults = {
      {"optimizer_enabled", "true"},
      {"copy_append_batching_enabled", "true"},
      {"native_ingest_enabled", "false"},
      {"plan_cache_enabled", "true"},
      {"descriptor_metadata_cache_enabled", "true"},
      {"statistics_enabled", "true"},
      {"summary_prune_enabled", "true"},
      {"agent_workers_enabled", "true"},
      {"resource_governor_enabled", "true"},
      {"page_filespace_preallocation_enabled", "true"},
      {"cancellation_enabled", "true"},
      {"backpressure_enabled", "true"},
      {"copy_batch_rows_configured", "1024"},
  };

  Require(api::PerformanceOptimizationConfigSurface().size() ==
              expected_defaults.size(),
          "DPC-068 config surface field count drifted");

  std::set<std::string> names;
  for (const auto& [name, expected_default] : expected_defaults) {
    const auto& field = ConfigField(name);
    Require(names.insert(field.surface_name).second,
            "DPC-068 duplicate config surface field");
    Require(field.packaged_default == expected_default,
            std::string("DPC-068 packaged default drifted: ") + name);
    Require(!field.config_key.empty() && !field.env_var.empty() &&
                !field.cli_option.empty() &&
                !field.admin_override_operation.empty() &&
                !field.admin_override_key.empty() &&
                !field.management_view_field.empty() &&
                !field.support_bundle_field.empty(),
            std::string("DPC-068 incomplete override metadata: ") + name);
    Require(!field.disabled_behavior.empty() &&
                field.disabled_behavior != "none",
            std::string("DPC-068 disabled behavior missing: ") + name);
    Require(!Contains(field.support_bundle_field, "/"),
            "DPC-068 support-bundle field exposed a path");
    Require(!Contains(field.management_view_field, "/"),
            "DPC-068 management field exposed a path");
  }
}

void TestPrecedenceResolution() {
  const auto resolution = api::ResolvePerformanceOptimizationConfigSurface({
      {"plan_cache_enabled", "config_file", "false"},
      {"plan_cache_enabled", "environment", "true"},
      {"plan_cache_enabled", "cli_option", "false"},
      {"plan_cache_enabled", "admin_override", "true"},
  });
  const auto& plan_cache = EffectiveField(resolution, "plan_cache_enabled");
  Require(plan_cache.effective_value == "true",
          "DPC-068 admin override did not win precedence");
  Require(plan_cache.value_source == "admin_override",
          "DPC-068 admin override source not retained");
  Require(plan_cache.configured_value == "admin_override:true",
          "DPC-068 configured value did not record winning source");
  Require(plan_cache.precedence_order == kPrecedence,
          "DPC-068 precedence order drifted");

  const auto lower_resolution = api::ResolvePerformanceOptimizationConfigSurface({
      {"optimizer_enabled", "config_file", "false"},
      {"optimizer_enabled", "environment", "true"},
      {"optimizer_enabled", "cli_option", "false"},
  });
  const auto& optimizer = EffectiveField(lower_resolution, "optimizer_enabled");
  Require(optimizer.effective_value == "false",
          "DPC-068 CLI override did not beat env/config");
  Require(optimizer.value_source == "cli_option",
          "DPC-068 CLI value source not retained");
}

void TestPolicyDeniedHigherPrecedenceOverride() {
  const auto resolution = api::ResolvePerformanceOptimizationConfigSurface({
      {"statistics_enabled", "config_file", "false"},
      {"statistics_enabled",
       "admin_override",
       "true",
       false,
       "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY",
       "admin override requires OBS_CONFIG_OVERRIDE",
       "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY|OBS_CONFIG_OVERRIDE_REQUIRED|"
       "surface=statistics_enabled"},
  });
  const auto& statistics = EffectiveField(resolution, "statistics_enabled");
  Require(statistics.effective_value == "false",
          "DPC-068 denied admin override changed effective value");
  Require(statistics.value_source == "config_file",
          "DPC-068 denied admin override replaced lower source");
  Require(statistics.override_refusal_code ==
              "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY",
          "DPC-068 refusal code drifted");
  Require(statistics.override_refusal_reason ==
              "admin override requires OBS_CONFIG_OVERRIDE",
          "DPC-068 refusal reason drifted");
  Require(statistics.override_refusal_message_vector ==
              "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY|"
              "OBS_CONFIG_OVERRIDE_REQUIRED|surface=statistics_enabled",
          "DPC-068 refusal message vector drifted");

  const auto json =
      api::SerializePerformanceOptimizationConfigResolutionJson(resolution);
  Require(Contains(json, "\"configured_value\":\"config_file:false\""),
          "DPC-068 JSON missing configured value");
  Require(Contains(json, "\"effective_value\":\"false\""),
          "DPC-068 JSON missing effective value");
  Require(Contains(json, "\"value_source\":\"config_file\""),
          "DPC-068 JSON missing value source");
  Require(Contains(json, "\"packaged_default\":\"true\""),
          "DPC-068 JSON missing packaged default");
  Require(Contains(json, "OBS_CONFIG_OVERRIDE_REQUIRED"),
          "DPC-068 JSON missing exact refusal vector");
}

void TestManagementAndSupportBundleVisibility() {
  const auto denied_resolution = api::ResolvePerformanceOptimizationConfigSurface({
      {"statistics_enabled", "config_file", "false"},
      {"statistics_enabled",
       "admin_override",
       "true",
       false,
       "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY",
       "admin override requires OBS_CONFIG_OVERRIDE",
       "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY|OBS_CONFIG_OVERRIDE_REQUIRED|"
       "surface=statistics_enabled"},
  });
  const auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  const auto management_json =
      api::SerializePerformanceOptimizationManagementJson(snapshot,
                                                          denied_resolution);
  const auto support_json =
      api::SerializePerformanceOptimizationSupportBundleJson(snapshot,
                                                             denied_resolution);

  for (const auto& json : {management_json, support_json}) {
    Require(Contains(json, "\"config_defaults\""),
            "DPC-068 config metadata missing from JSON surface");
    Require(Contains(json, "\"surface_name\":\"statistics_enabled\""),
            "DPC-068 statistics config metadata missing from JSON surface");
    Require(Contains(json, "\"configured_value\":\"config_file:false\""),
            "DPC-068 configured value missing from JSON surface");
    Require(Contains(json, "\"effective_value\":\"false\""),
            "DPC-068 effective value missing from JSON surface");
    Require(Contains(json, "\"value_source\":\"config_file\""),
            "DPC-068 source missing from JSON surface");
    Require(Contains(json, "\"override_refusal_code\":"
                           "\"DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY\""),
            "DPC-068 refusal code missing from JSON surface");
    Require(Contains(json, "OBS_CONFIG_OVERRIDE_REQUIRED"),
            "DPC-068 refusal vector missing from JSON surface");
    Require(!Contains(json, "password="),
            "DPC-068 JSON leaked a secret-shaped field");
    Require(!Contains(json, "/home/"),
            "DPC-068 JSON leaked a local home path");
  }

  api::EngineInspectPerformanceOptimizationSurfaceRequest request;
  request.context = Context();
  request.config_overrides = {
      {"statistics_enabled", "config_file", "false"},
      {"statistics_enabled",
       "admin_override",
       "true",
       false,
       "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY",
       "admin override requires OBS_CONFIG_OVERRIDE",
       "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY|OBS_CONFIG_OVERRIDE_REQUIRED|"
       "surface=statistics_enabled"},
  };
  const auto result = api::EngineInspectPerformanceOptimizationSurface(request);
  Require(result.ok, "DPC-068 inspect surface refused config override route");
  Require(HasEvidence(result, "config_defaults_packaging_gate", "DPC-068"),
          "DPC-068 engine result missing packaging evidence");
  Require(HasEvidence(result,
                      "support_bundle_config_defaults",
                      "performance_optimization_config"),
          "DPC-068 engine result missing support-bundle config evidence");
  Require(HasEvidence(result, "config_override_resolution", "request_overrides"),
          "DPC-068 engine result missing request override route evidence");
  Require(FieldValue(result, "config_defaults_packaging_ready") == "true",
          "DPC-068 result row missing packaging-ready flag");
  Require(FieldValue(result, "config_precedence_order") == kPrecedence,
          "DPC-068 result row missing precedence order");
  Require(Contains(result.support_bundle_json,
                   "\"surface_name\":\"copy_append_batching_enabled\""),
          "DPC-068 default support bundle missing config field metadata");
  Require(Contains(result.management_api_json,
                   "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY"),
          "DPC-068 inspect management JSON missing denied override route");
  Require(Contains(result.support_bundle_json,
                   "OBS_CONFIG_OVERRIDE_REQUIRED"),
          "DPC-068 inspect support bundle missing denied override route");

  api::EnginePrepareSupportBundleRequest support_request;
  support_request.context = Context();
  support_request.option_envelopes = {
      "engine_authorized_support_export",
      "retention_policy_ref:support.bundle.default_retention.v1",
      "redaction_profile_ref:server.support_bundle.default_redaction.v1",
  };
  support_request.performance_optimization_snapshot =
      api::DefaultPerformanceOptimizationSurfaceSnapshot();
  support_request.performance_optimization_snapshot_present = true;
  support_request.performance_optimization_config_overrides =
      request.config_overrides;
  const auto support_result = api::EnginePrepareSupportBundle(support_request);
  Require(support_result.ok,
          "DPC-068 prepare support bundle refused config override route");
  Require(HasEvidence(support_result,
                      "support_bundle_config_defaults",
                      "performance_optimization_config"),
          "DPC-068 support bundle API missing config defaults evidence");
  Require(HasEvidence(support_result,
                      "config_override_resolution",
                      "request_overrides"),
          "DPC-068 support bundle API missing override route evidence");
  Require(Contains(support_result.support_bundle_json,
                   "\"configured_value\":\"config_file:false\""),
          "DPC-068 support bundle API missing configured override value");
  Require(Contains(support_result.support_bundle_json,
                   "\"effective_value\":\"false\""),
          "DPC-068 support bundle API missing effective override value");
  Require(Contains(support_result.support_bundle_json,
                   "OBS_CONFIG_OVERRIDE_REQUIRED"),
          "DPC-068 support bundle API missing exact denied message vector");
}

}  // namespace

int main() {
  TestPackagedDefaultsAndMetadata();
  TestPrecedenceResolution();
  TestPolicyDeniedHigherPrecedenceOverride();
  TestManagementAndSupportBundleVisibility();
  std::cout << "dpc_config_defaults_packaging_gate=passed\n";
  return EXIT_SUCCESS;
}
