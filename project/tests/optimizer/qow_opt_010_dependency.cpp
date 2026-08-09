// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define main qow_opt_009_dependency_fixture_main
#include "qow_opt_009.cpp"
#undef main

namespace {

using Mutation010 = std::pair<
    std::string_view,
    std::function<void(cache::CanonicalExecutablePlanCacheKey&)>>;

bool RequireDependency010(const bool condition,
                          const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-OPT-010-DEPENDENCY-V1: " << detail << '\n';
  }
  return condition;
}

std::vector<Mutation010> DependencyMutations010() {
  return {
      {"catalog", [](auto& key) { ++key.catalog_generation; }},
      {"security", [](auto& key) { ++key.security_epoch; }},
      {"redaction", [](auto& key) {
         key.redaction_policy_digest = std::string(64, '1');
       }},
      {"policy", [](auto& key) { ++key.policy_epoch; }},
      {"statistics", [](auto& key) {
         ++key.statistics_generation;
         ++key.statistics_generations.front().generation;
       }},
      {"capability", [](auto& key) {
         ++key.capability_generations.front().generation;
       }},
      {"configuration", [](auto& key) {
         ++key.optimizer_configuration_generation;
       }},
      {"resource", [](auto& key) { ++key.resource_epoch; }},
      {"route", [](auto& key) {
         ++key.route_generation;
         ++key.route_generations.front().generation;
       }},
      {"filespace", [](auto& key) {
         ++key.filespace_placement_generation;
         ++key.filespace_generations.front().generation;
       }},
      {"cluster", [](auto& key) {
         key.standalone_database = false;
         key.cluster_uuid = Uuid(1601);
         key.cluster_epoch = 1;
       }},
      {"format", [](auto& key) { ++key.engine_format_generation; }},
      {"parser", [](auto& key) {
         ++key.parser_compatibility_generation;
       }},
      {"donor", [](auto& key) {
         key.donor_compatibility_profile_uuid = Uuid(1602);
         key.donor_compatibility_generation = 1;
       }},
      {"object", [](auto& key) {
         ++key.object_generations.front().generation;
       }},
      {"function", [](auto& key) {
         ++key.function_generations.front().generation;
       }},
      {"index", [](auto& key) {
         ++key.index_generations.front().generation;
       }},
      {"datatype", [](auto& key) {
         ++key.datatype_generations.front().generation;
       }},
      {"domain", [](auto& key) {
         const auto found = std::ranges::find_if(
             key.physical_dependencies, [](const auto& dependency) {
               return dependency.dependency_kind ==
                      cache::CanonicalPreparedPlanDependencyKind::kDomain;
             });
         if (found != key.physical_dependencies.end()) ++found->generation;
       }},
      {"collation", [](auto& key) {
         ++key.collation_generations.front().generation;
       }},
      {"descriptor", [](auto& key) {
         ++key.metadata_generations.front().generation;
       }},
      {"schema", [](auto& key) { key.result_schema_uuid = Uuid(1603); }},
      {"physical dependency", [](auto& key) {
         key.physical_dependencies.front().definition_digest =
             std::string(64, 'f');
       }},
      {"physical plan identity", [](auto& key) {
         key.selected_plan_uuid = Uuid(1604);
       }},
      {"database", [](auto& key) { key.database_uuid = Uuid(1605); }},
  };
}

bool ValidateDependencyMatrix010() {
  bool passed = true;
  for (const auto& [name, mutate] : DependencyMutations010()) {
    Fixture009 fixture;
    if (!fixture.ready) return false;
    auto current = fixture.key;
    mutate(current);
    const auto first = fixture.executable_cache.InvalidateIfStale(
        fixture.key.prepared_plan_uuid, current, true);
    const auto duplicate = fixture.executable_cache.InvalidateIfStale(
        fixture.key.prepared_plan_uuid, current, true);

    cache::CanonicalExecutablePlanCacheLookupRequest lookup;
    lookup.current_key = fixture.key;
    lookup.parameter_bindings = {{fixture.prepared_plan->parameters.front(),
                                  &fixture.parameter_value}};
    lookup.mga_authority = Authority009(FreshStatement009(1610));
    lookup.engine_lookup_authorized = true;
    lookup.engine_security_revalidated = true;
    lookup.engine_policy_revalidated = true;
    lookup.engine_authorization_revalidated = true;
    lookup.authorization_revalidation_receipt_uuid = Uuid(1614);
    const auto stale = fixture.executable_cache.LookupAndBind(lookup);
    passed &= RequireDependency010(
        first.invalidated && !first.duplicate_invalidation &&
            first.invalidation_generation != 0 &&
            !first.stale_execution_observed && duplicate.invalidated &&
            duplicate.duplicate_invalidation &&
            duplicate.invalidation_generation ==
                first.invalidation_generation &&
            fixture.executable_cache.Size() == 0 && !stale.accepted &&
            stale.reprepare_required && stale.issues.size() == 1 &&
            stale.issues.front().diagnostic_id ==
                "QOW-DIAG-OPT-010-DEPENDENCY-REFUSAL-V1" &&
            stale.execution_physical_dag.nodes.empty(),
        std::string("stale dependency was not atomically refused: ") +
            std::string(name));
  }
  return passed;
}

bool ValidateDependencySecurityPrecedenceAndExactness010() {
  Fixture009 fixture;
  if (!fixture.ready) return false;
  const auto unchanged = fixture.executable_cache.InvalidateIfStale(
      fixture.key.prepared_plan_uuid, fixture.key, true);
  auto current = fixture.key;
  ++current.security_epoch;
  current.object_generations.front().identity_uuid = Uuid(1699);
  const auto protected_result = fixture.executable_cache.InvalidateIfStale(
      fixture.key.prepared_plan_uuid, current, true);
  return RequireDependency010(
      !unchanged.invalidated && protected_result.invalidated &&
          protected_result.protected_detail &&
          protected_result.field_id == "protected_generation" &&
          !protected_result.stale_execution_observed,
      "security-safe precedence or exact vector comparison failed");
}

}  // namespace

// QOW-TEST-OPT-010-DEPENDENCY-V1
int main() {
  return ValidateDependencyMatrix010() &&
                 ValidateDependencySecurityPrecedenceAndExactness010()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
