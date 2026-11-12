// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SEARCH_KEY: SB_CATALOG_AGENT_CONSTANTS (owner Q68 = 1, Q69 = 1, Q70 = 2).
// Agent types, agent policy families and metric families as coding-time
// constants (code + permanent UUIDv7), plus the small agent classifications.
// Iterates the code's own registries and the bootstrap declarations and
// requires every text they carry to map to a listed constant through the
// boundary ForTag adapters; checks codes are contiguous and UUIDs distinct
// engine-identity UUIDv7s. Constants only; not wired.
#include "core/catalog/catalog_agent_constants.hpp"
#include "core/catalog/catalog_security_constants.hpp"
#include "core/agents/agent_bootstrap_configuration.hpp"
#include "core/agents/agent_policy_schema.hpp"
#include "core/agents/agent_runtime.hpp"
#include "core/index/index_metrics.hpp"
#include "core/index/index_optimizer_runtime_metrics.hpp"
#include "core/metrics/cluster_metric_descriptors.hpp"
#include "core/metrics/metric_builtin_definitions.hpp"
#include "core/metrics/metric_concurrency_definitions.hpp"
#include "core/metrics/metric_page_cache_definitions.hpp"
#include "core/uuid/uuid.hpp"
#include "engine/executor/executor_operator_metrics.hpp"
#include "engine/internal_api/observability/lifecycle_metric_definitions.hpp"
#include "engine/optimizer/optimizer_route_metrics.hpp"
#include "engine/optimizer/specialized_workload_metrics.hpp"
#include "storage/page/optimizer_storage_metrics.hpp"
#include "transaction/mga/optimizer_mga_pressure_metrics.hpp"

#include <array>
#include <iostream>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace a = scratchbird::core::agents;
namespace c = scratchbird::core::catalog;
namespace m = scratchbird::core::metrics;
using scratchbird::core::platform::Uuid;

int failures = 0;
void Check(bool ok, std::string_view what, std::source_location where = std::source_location::current()) {
  if (ok) return;
  ++failures;
  std::cerr << "FAIL line " << where.line() << ": " << what << "\n";
}

std::string UuidKey(const Uuid& u) { return std::string(reinterpret_cast<const char*>(u.bytes.data()), 16); }

// Codes 1..N in table order, tags distinct, UUIDs valid engine-identity v7,
// IdentityOf agrees with the table, `none` is never known.
template <class Table, class Member, class IsKnown, class IdentityOf, class ForTag>
void CheckTable(const Table& table, Member member, IsKnown is_known, IdentityOf identity_of, ForTag for_tag,
                std::set<std::string>& all_uuids, std::string_view title) {
  std::set<std::string_view> tags;
  std::size_t expected = 1;
  for (const auto& row : table) {
    const auto value = row.*member;
    Check(static_cast<std::size_t>(value) == expected, std::string(title) + " code contiguous: " + std::string(row.tag));
    ++expected;
    Check(tags.insert(row.tag).second, std::string(title) + " tag distinct: " + std::string(row.tag));
    Check(scratchbird::core::uuid::IsEngineIdentityUuid(row.uuid) &&
              scratchbird::core::uuid::UuidVersion(row.uuid) == 7,
          std::string(title) + " uuid is v7: " + std::string(row.tag));
    Check(all_uuids.insert(UuidKey(row.uuid)).second, std::string(title) + " uuid distinct: " + std::string(row.tag));
    Check(is_known(value), std::string(title) + " known: " + std::string(row.tag));
    const auto id = identity_of(value);
    Check(id.has_value() && *id == row.uuid, std::string(title) + " identity: " + std::string(row.tag));
    const auto back = for_tag(row.tag);
    Check(back.has_value() && *back == value, std::string(title) + " tag round trip: " + std::string(row.tag));
  }
  using E = std::remove_cvref_t<decltype(table[0].*member)>;
  Check(!is_known(E::none) && !identity_of(E::none).has_value(), std::string(title) + " none unknown");
  Check(!for_tag("").has_value() && !for_tag("none").has_value() && !for_tag("NOT_A_TAG").has_value(),
        std::string(title) + " unknown tag refused");
}

std::vector<std::string> SplitTokens(std::string_view text, char sep) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto at = text.find(sep);
    out.emplace_back(text.substr(0, at));
    if (at == std::string_view::npos) break;
    text.remove_prefix(at + 1);
  }
  return out;
}

std::string_view Property(std::string_view body, std::string_view key) {
  while (!body.empty()) {
    const auto end = body.find('\n');
    const auto line = body.substr(0, end);
    const auto eq = line.find('=');
    if (eq != std::string_view::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    if (end == std::string_view::npos) break;
    body.remove_prefix(end + 1);
  }
  return {};
}

bool HasProperty(std::string_view body, std::string_view key) {
  const std::string probe = std::string(key) + "=";
  return body.starts_with(probe) || body.find("\n" + probe) != std::string_view::npos;
}

template <class Range>
void CheckMetricRegistry(const Range& definitions, std::string_view source, std::set<c::CatalogMetricFamily>& seen) {
  Check(!definitions.empty(), std::string(source) + " registry non-empty");
  for (const auto& d : definitions) {
    const auto f = c::CatalogMetricFamilyForTag(d.family);
    Check(f.has_value(), std::string(source) + " metric family listed: " + d.family);
    if (f) seen.insert(*f);
  }
}
}  // namespace

int main() {
  std::set<std::string> all_uuids;
  CheckTable(c::kCatalogAgentTypes, &c::CatalogAgentTypeIdentity::type, c::IsKnownCatalogAgentType,
             c::CatalogAgentTypeIdentityOf, c::CatalogAgentTypeForTag, all_uuids, "agent type");
  CheckTable(c::kCatalogAgentPolicyFamilies, &c::CatalogAgentPolicyFamilyIdentity::family,
             c::IsKnownCatalogAgentPolicyFamily, c::CatalogAgentPolicyFamilyIdentityOf,
             c::CatalogAgentPolicyFamilyForTag, all_uuids, "policy family");
  CheckTable(c::kCatalogMetricFamilies, &c::CatalogMetricFamilyIdentity::family, c::IsKnownCatalogMetricFamily,
             c::CatalogMetricFamilyIdentityOf, c::CatalogMetricFamilyForTag, all_uuids, "metric family");
  // No collision with the previously recorded security-right identities (28c).
  for (const auto& r : c::kCatalogSecurityRights)
    Check(!all_uuids.contains(UuidKey(r.uuid)), "no collision with security right " + std::string(r.tag));

  // ---- agent types: manifest + lifecycle, exactly the listed set ----
  auto agents = a::CanonicalAgentRegistry();
  const auto lifecycle = a::RuntimeLifecycleAgentDescriptors();
  Check(agents.size() == 29 && lifecycle.size() == 3, "registry sizes 29 + 3");
  agents.insert(agents.end(), lifecycle.begin(), lifecycle.end());
  std::set<c::CatalogAgentType> agent_seen;
  for (const auto& d : agents) {
    const auto t = c::CatalogAgentTypeForTag(d.type_id);
    Check(t.has_value(), "agent type listed: " + d.type_id);
    if (t) agent_seen.insert(*t);
    Check(c::CatalogAgentRuntimeLayerForTag(a::AgentRuntimeLayerName(d.layer)).has_value(), "layer: " + d.type_id);
    Check(c::CatalogAgentDeploymentForTag(a::AgentDeploymentName(d.deployment)).has_value(), "deployment: " + d.type_id);
    Check(c::CatalogAgentAuthorityClassForTag(a::AgentAuthorityClassName(d.authority)).has_value(), "authority: " + d.type_id);
    Check(c::CatalogAgentActivationProfileForTag(a::AgentActivationProfileName(d.default_activation)).has_value(),
          "activation: " + d.type_id);
    Check(c::CatalogAgentScopeForPath(d.scope).has_value(), "scope path: " + d.type_id + " " + d.scope);
    const auto lifecycle_family = a::RuntimeLifecycleAgentPolicyFamily(d.type_id);
    const auto families = lifecycle_family.empty() ? a::RequiredPolicyFamiliesForAgent(d)
                                                   : std::vector<std::string>{lifecycle_family};
    Check(!families.empty(), "agent has a policy family: " + d.type_id);
    for (const auto& f : families)
      Check(c::CatalogAgentPolicyFamilyForTag(f).has_value(), "required policy family listed: " + f);
  }
  Check(agent_seen.size() == c::kCatalogAgentTypes.size(), "every listed agent type is in the registries");

  // Every value of the code's own enums maps (not only those in use).
  for (const auto v : {a::AgentRuntimeLayer::l1_observation, a::AgentRuntimeLayer::l2_recorder,
                       a::AgentRuntimeLayer::l3_dispatcher, a::AgentRuntimeLayer::l4_worker,
                       a::AgentRuntimeLayer::l5_coordinator})
    Check(c::CatalogAgentRuntimeLayerForTag(a::AgentRuntimeLayerName(v)).has_value(), a::AgentRuntimeLayerName(v));
  for (const auto v : {a::AgentDeployment::local, a::AgentDeployment::both, a::AgentDeployment::cluster})
    Check(c::CatalogAgentDeploymentForTag(a::AgentDeploymentName(v)).has_value(), a::AgentDeploymentName(v));
  for (const auto v : {a::AgentAuthorityClass::observe_only, a::AgentAuthorityClass::recommend_only,
                       a::AgentAuthorityClass::request_action, a::AgentAuthorityClass::direct_bounded_action})
    Check(c::CatalogAgentAuthorityClassForTag(a::AgentAuthorityClassName(v)).has_value(), a::AgentAuthorityClassName(v));
  for (const auto v : {a::AgentActivationProfile::disabled, a::AgentActivationProfile::observe_only,
                       a::AgentActivationProfile::recommend_only, a::AgentActivationProfile::dry_run,
                       a::AgentActivationProfile::live_action})
    Check(c::CatalogAgentActivationProfileForTag(a::AgentActivationProfileName(v)).has_value(),
          a::AgentActivationProfileName(v));

  // ---- policy families: the schema map, exactly the listed set ----
  std::set<c::CatalogAgentPolicyFamily> family_seen;
  for (const auto& f : a::AgentPolicySchemaFamilies()) {
    const auto p = c::CatalogAgentPolicyFamilyForTag(f);
    Check(p.has_value(), "schema policy family listed: " + f);
    if (p) family_seen.insert(*p);
  }
  Check(family_seen.size() == c::kCatalogAgentPolicyFamilies.size(), "every listed policy family is in the schema map");

  // ---- metric families: every descriptor registry + agent contracts ----
  std::set<c::CatalogMetricFamily> metric_seen;
  CheckMetricRegistry(scratchbird::core::index::IndexMetricDescriptorDefinitions(), "IndexMetric", metric_seen);
  CheckMetricRegistry(scratchbird::core::index::IndexOperationMetricDescriptorDefinitions(), "IndexOperation", metric_seen);
  CheckMetricRegistry(scratchbird::core::index::IndexOptimizerRuntimeMetricDescriptorDefinitions(), "IndexOptimizerRuntime",
                      metric_seen);
  CheckMetricRegistry(m::BuiltinClusterMetricDescriptorManifests(), "BuiltinCluster", metric_seen);
  for (const auto& f : m::RequiredClusterMetricFamilies())
    Check(c::CatalogMetricFamilyForTag(f).has_value(), "required cluster metric family listed: " + f);
  CheckMetricRegistry(m::BuiltinMetricDescriptorDefinitions(), "Builtin", metric_seen);
  const std::array<scratchbird::core::platform::u64, 2> bounds{1, 2};
  const auto concurrency = m::ConcurrencyWaitMetricDefinitions(bounds);
  Check(concurrency.validation.ok, "concurrency definitions built");
  CheckMetricRegistry(concurrency.definitions, "ConcurrencyWait", metric_seen);
  CheckMetricRegistry(m::PageCacheContextMetricDefinitions(), "PageCacheContext", metric_seen);
  CheckMetricRegistry(scratchbird::engine::executor::ExecutorOperatorActualsMetricDescriptorDefinitions(),
                      "ExecutorOperatorActuals", metric_seen);
  CheckMetricRegistry(scratchbird::engine::internal_api::LifecycleMetricDefinitions(), "Lifecycle", metric_seen);
  CheckMetricRegistry(scratchbird::engine::optimizer::OptimizerRouteMetricDescriptorDefinitions(), "OptimizerRoute",
                      metric_seen);
  CheckMetricRegistry(scratchbird::engine::optimizer::SpecializedWorkloadMetricDescriptorDefinitions(),
                      "SpecializedWorkload", metric_seen);
  CheckMetricRegistry(scratchbird::storage::page::OptimizerStorageMetricDescriptorDefinitions(), "OptimizerStorage",
                      metric_seen);
  CheckMetricRegistry(scratchbird::transaction::mga::OptimizerMgaPressureMetricDescriptorDefinitions(),
                      "OptimizerMgaPressure", metric_seen);
  for (const auto& row : a::AgentMetricDependencyContractRegistry()) {
    const auto f = c::CatalogMetricFamilyForTag(row.dependency.metric_family);
    Check(f.has_value(), "agent contract metric family listed: " + row.dependency.metric_family);
    Check(c::CatalogAgentTypeForTag(row.agent_type_id).has_value(), "agent contract agent listed: " + row.agent_type_id);
    if (f) metric_seen.insert(*f);
  }
  Check(metric_seen.size() == c::kCatalogMetricFamilies.size(), "every listed metric family is defined by the code");

  // ---- bootstrap declarations: every small-enum text the builder writes ----
  std::set<c::CatalogAgentInvalidPolicyBehavior> behavior_seen;
  std::size_t policies = 0, registrations = 0;
  for (const auto& d : a::DefaultAgentBootstrapDeclarations()) {
    const std::string_view p = d.properties;
    Check(c::CatalogAgentTypeForTag(d.agent_type).has_value(), "bootstrap agent_type listed: " + d.agent_type);
    Check(c::CatalogAgentTypeForTag(Property(p, "agent_type")).has_value(), "bootstrap agent_type property");
    if (d.kind == c::AgentBootstrapDefinitionKind::policy) {
      ++policies;
      Check(c::CatalogAgentPolicyClassForTag(Property(p, "policy_class")).has_value(), "policy_class");
      Check(c::CatalogAgentPolicyFamilyForTag(Property(p, "policy_family")).has_value(), "policy_family");
      Check(c::CatalogAgentRuntimeLayerForTag(Property(p, "layer")).has_value(), "layer");
      Check(c::CatalogAgentDeploymentForTag(Property(p, "deployment")).has_value(), "deployment");
      Check(c::CatalogAgentScopeForPath(Property(p, "scope")).has_value(),
            "scope: " + std::string(Property(p, "scope")));
      Check(c::CatalogAgentAuthorityClassForTag(Property(p, "authority")).has_value(), "authority");
      Check(c::CatalogAgentActivationProfileForTag(Property(p, "activation")).has_value(), "activation");
      Check(c::CatalogAgentActionModeForTag(Property(p, "action_mode")).has_value(),
            "action_mode: " + std::string(Property(p, "action_mode")));
      const auto behavior = c::CatalogAgentInvalidPolicyBehaviorForTag(Property(p, "invalid_policy_behavior"));
      Check(behavior.has_value(), "invalid_policy_behavior: " + std::string(Property(p, "invalid_policy_behavior")));
      if (behavior) behavior_seen.insert(*behavior);
      Check(HasProperty(p, "required_metric_families"), "required_metric_families present");
      if (!Property(p, "required_metric_families").empty())
        for (const auto& f : SplitTokens(Property(p, "required_metric_families"), ','))
          Check(c::CatalogMetricFamilyForTag(f).has_value(), "policy required metric family listed: " + f);
    } else {
      ++registrations;
      Check(c::CatalogAgentRegistrationStateForTag(Property(p, "state")).has_value(), "state");
      Check(c::CatalogAgentManagedByForTag(Property(p, "managed_by")).has_value(), "managed_by");
      Check(c::CatalogAgentLaunchTriggerForTag(Property(p, "launch_trigger")).has_value(), "launch_trigger");
      for (const auto& f : SplitTokens(Property(p, "policy_families"), ','))
        Check(c::CatalogAgentPolicyFamilyForTag(f).has_value(), "registration policy family listed: " + f);
    }
  }
  Check(registrations == 32 && policies >= 32, "bootstrap declares every agent");
  Check(behavior_seen.size() == c::kCatalogAgentInvalidPolicyBehaviorTags.size(),
        "every invalid-policy behaviour is one the builder writes");

  // Native reader's accepted action modes (native_local_service_agent_policy.cpp).
  for (const auto mode : {"disabled", "observe_only", "recommend_only", "request_action", "direct_bounded", "dry_run",
                          "live_action", "operator_only"})
    Check(c::CatalogAgentActionModeForTag(mode).has_value(), mode);

  // Scope bit set: segments distinct single bits; path parsing refuses bad input.
  scratchbird::core::platform::u64 bits = 0;
  for (const auto& r : c::kCatalogAgentScopeTags) {
    const auto v = static_cast<scratchbird::core::platform::u64>(r.value);
    Check(v != 0 && (v & (v - 1)) == 0 && (bits & v) == 0, "scope single distinct bit: " + std::string(r.tag));
    bits |= v;
  }
  Check(c::CatalogAgentScopeForPath("node/database/session/workload") ==
            (c::CatalogAgentScope::node | c::CatalogAgentScope::database | c::CatalogAgentScope::session |
             c::CatalogAgentScope::workload),
        "scope path value");
  for (const auto bad : {"", "/", "node/", "/node", "node//database", "node/node", "node/unknown"})
    Check(!c::CatalogAgentScopeForPath(bad).has_value(), std::string("scope path refused: ") + bad);

  // Small enums: codes 1..N in table order.
  auto contiguous = [](const auto& table, std::string_view title) {
    std::size_t expected = 1;
    for (const auto& r : table) {
      Check(static_cast<std::size_t>(r.value) == expected++, std::string(title) + " contiguous: " + std::string(r.tag));
    }
  };
  contiguous(c::kCatalogAgentRuntimeLayerTags, "layer");
  contiguous(c::kCatalogAgentDeploymentTags, "deployment");
  contiguous(c::kCatalogAgentAuthorityClassTags, "authority");
  contiguous(c::kCatalogAgentActivationProfileTags, "activation");
  contiguous(c::kCatalogAgentActionModeTags, "action_mode");
  contiguous(c::kCatalogAgentPolicyClassTags, "policy_class");
  contiguous(c::kCatalogAgentRegistrationStateTags, "state");
  contiguous(c::kCatalogAgentManagedByTags, "managed_by");
  contiguous(c::kCatalogAgentLaunchTriggerTags, "launch_trigger");
  contiguous(c::kCatalogAgentInvalidPolicyBehaviorTags, "invalid_policy_behavior");

  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "catalog_agent_constants: " << c::kCatalogAgentTypes.size() << " agent types, "
            << c::kCatalogAgentPolicyFamilies.size() << " policy families, " << c::kCatalogMetricFamilies.size()
            << " metric families: ok\n";
  return 0;
}
