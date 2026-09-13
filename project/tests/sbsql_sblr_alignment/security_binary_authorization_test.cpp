// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "security/security_model.hpp"
#include "uuid.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
long allocation_budget = -1;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok && failures++ < 20) std::cerr << "FAIL " << message << '\n';
}
}
void* operator new(std::size_t size) {
  if (allocation_budget == 0) throw std::bad_alloc();
  if (allocation_budget > 0) --allocation_budget;
  if (void* result = std::malloc(size ? size : 1)) return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace api = scratchbird::engine::internal_api;
api::EngineUuid Id() {
  const auto result = scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(1789310000000ULL);
  if (!result.ok()) throw std::runtime_error("fixture UUID generation failed");
  return result.value;
}
struct Fixture {
  api::EngineUuid authority = Id(), principal = Id(), role = Id(), group = Id(), target = Id(), other = Id();
  api::DurableAuthorizationState state;
  api::DurableAuthorizationMaterializeRequest request;
  api::EngineRequestContext context;
  Fixture() {
    state.authority_uuid = authority;
    state.security_context_generation = 3;
    state.security_epoch = 7;
    state.policy_epoch = 11;
    state.catalog_generation_id = 13;
    state.principals.push_back({principal, "principal", true, 7});
    state.roles.push_back({role, true, 7});
    state.groups.push_back({group, true, 7});
    state.memberships.push_back({principal, "principal", role, "role", true, 7});
    state.memberships.push_back({role, "role", group, "group", true, 7});
    state.grants.push_back({Id(), group, "group", target, "SELECT", false, true, 7});
    request.principal_uuid = principal;
    request.observed_security_epoch = 7;
    request.observed_policy_epoch = 11;
    request.observed_catalog_generation_id = 13;
    context.principal_uuid = principal;
    context.security_context_present = true;
    context.security_epoch = 7;
    context.catalog_generation_id = 13;
  }
};
void ClosureAndDecision() {
  Fixture f;
  const auto materialized = api::MaterializeDurableAuthorizationContext(f.state, f.request);
  Check(materialized.ok && materialized.context.effective_subjects.size() == 3,
        "transitive closure executes with binary subjects");
  Check(materialized.context.grants.size() == 1 &&
        materialized.context.grants.front().grant_uuid == f.state.grants.front().grant_uuid,
        "durable grant identity is exact");
  Check(api::EvaluateMaterializedAuthorization(f.context, materialized.context, "SELECT", f.target).authorized,
        "nested group grant authorizes its binary target");
  Check(!api::EvaluateMaterializedAuthorization(f.context, materialized.context, "SELECT", f.other).authorized,
        "grant does not authorize a different binary target");
  Check(!api::EvaluateMaterializedAuthorization(f.context, materialized.context, "SELECT", {}).authorized,
        "object grant does not become a global grant");
  auto state = f.state;
  state.memberships.push_back({f.group, "group", f.role, "role", true, 7});
  const auto cycle = api::MaterializeDurableAuthorizationContext(state, f.request);
  Check(!cycle.ok && !cycle.context.present && !cycle.diagnostics.empty() &&
        cycle.diagnostics.front().code == "SECURITY.AUTHORIZATION.MEMBERSHIP_CYCLE",
        "membership cycle fails without partial publication");
  state = f.state; state.roles.front().active = false;
  Check(!api::MaterializeDurableAuthorizationContext(state, f.request).ok, "inactive role fails closure");
  state = f.state; state.memberships.front().security_epoch = 6;
  Check(!api::MaterializeDurableAuthorizationContext(state, f.request).ok, "stale membership fails closure");
  state = f.state; state.principals.push_back(state.principals.front());
  Check(!api::MaterializeDurableAuthorizationContext(state, f.request).ok, "duplicate principal is not first-row authority");
  state = f.state; state.roles.push_back(state.roles.front());
  Check(!api::MaterializeDurableAuthorizationContext(state, f.request).ok, "duplicate role fails closure");
  state = f.state; state.groups.push_back(state.groups.front());
  Check(!api::MaterializeDurableAuthorizationContext(state, f.request).ok, "duplicate group fails closure");
  state = f.state; state.grants.front().target_uuid = {};
  const auto global = api::MaterializeDurableAuthorizationContext(state, f.request);
  Check(global.ok && api::EvaluateMaterializedAuthorization(f.context, global.context, "SELECT", f.other).authorized,
        "explicit global target grants wider scope");
  state.grants.push_back({Id(), f.principal, "principal", f.target, "SELECT", true, true, 7});
  const auto denied = api::MaterializeDurableAuthorizationContext(state, f.request);
  const auto decision = api::EvaluateMaterializedAuthorization(f.context, denied.context, "SELECT", f.target);
  Check(denied.ok && !decision.authorized && decision.denied, "explicit principal deny overrides group allow");
  state = f.state;
  api::DurableAuthorizationPolicyRecord policy;
  policy.policy_uuid = Id(); policy.subject_uuid = f.principal; policy.subject_kind = "principal";
  policy.target_uuid = f.target; policy.right = "SELECT"; policy.policy_kind = "test_policy";
  policy.policy_epoch = 11; policy.requires_runtime_recheck = true;
  state.policies.push_back(policy);
  const auto policy_context = api::MaterializeDurableAuthorizationContext(state, f.request);
  const auto recheck = api::EvaluateMaterializedAuthorization(f.context, policy_context.context, "SELECT", f.target);
  Check(recheck.authorized && recheck.policy_recheck_required, "policy recheck survives materialization");
  state.policies.front().deny = true;
  const auto denied_policy = api::MaterializeDurableAuthorizationContext(state, f.request);
  Check(api::EvaluateMaterializedAuthorization(f.context, denied_policy.context, "SELECT", f.target).denied,
        "policy deny overrides grant");
}
void BootstrapAndTrace() {
  Fixture f;
  f.state.grants.clear();
  auto ordinary = api::MaterializeDurableAuthorizationContext(f.state, f.request);
  Check(ordinary.ok && !api::EvaluateMaterializedAuthorization(f.context, ordinary.context, "SELECT", f.target).authorized,
        "role membership alone is not bootstrap authority");
  f.state.engine_owned_sysarch_role_uuid = f.role;
  auto bootstrap = api::MaterializeDurableAuthorizationContext(f.state, f.request);
  Check(bootstrap.ok && bootstrap.context.grants.empty() &&
        bootstrap.context.engine_owned_bootstrap_role_uuid == f.role,
        "bootstrap retains real role without fabricated grant identities");
  Check(api::EvaluateMaterializedAuthorization(f.context, bootstrap.context, "SELECT", f.target).authorized,
        "catalog bootstrap bundle executes");
  f.state.grants.push_back({Id(), f.principal, "principal", f.target, "SELECT", true, true, 7});
  bootstrap = api::MaterializeDurableAuthorizationContext(f.state, f.request);
  Check(api::EvaluateMaterializedAuthorization(f.context, bootstrap.context, "SELECT", f.target).denied,
        "deny wins over bootstrap bundle");
  for (auto mode : {api::EngineTrustMode::embedded_in_process, api::EngineTrustMode::server_isolated}) {
    auto context = f.context; context.trust_mode = mode;
    context.trace_tags = {"security.fixture_trace_authority", "right:SELECT", "role_uuid:fake", "group_uuid:fake"};
    Check(!api::SecurityTraceAuthorizationFallbackAllowed(context) &&
          !api::SecurityContextHasRight(context, "SELECT", f.target), "trace tags never grant rights");
    api::EngineApiRequest request; request.context = context;
    const auto record = api::ConnectionSecurityContextFromRequest(request);
    Check(record.active_roles.empty() && record.effective_groups.empty(), "trace strings cannot create binary memberships");
    request.context.authorization_context = ordinary.context;
    const auto actual = api::ConnectionSecurityContextFromRequest(request);
    Check(actual.active_roles == std::vector<api::EngineUuid>{f.role} &&
          actual.effective_groups == std::vector<api::EngineUuid>{f.group}, "connection memberships retain binary identities");
  }
}
void ScopeMatrix() {
  Fixture f;
  const api::EngineUuid scopes[] = {{}, f.target, f.other};
  for (unsigned mask = 0; mask < 8; ++mask) {
    for (const auto& allow_scope : scopes) {
      for (const auto& deny_scope : scopes) {
        auto state = f.state;
        state.grants.clear();
        if (mask & 1) state.grants.push_back({Id(), f.group, "group", allow_scope, "SELECT", false, true, 7});
        if (mask & 2) state.grants.push_back({Id(), f.principal, "principal", deny_scope, "SELECT", true, true, 7});
        if (mask & 4) {
          api::DurableAuthorizationPolicyRecord policy;
          policy.policy_uuid = Id(); policy.subject_uuid = f.role; policy.subject_kind = "role";
          policy.target_uuid = f.target; policy.right = "SELECT"; policy.policy_kind = "scope_policy";
          policy.policy_epoch = 11; policy.deny = true;
          state.policies.push_back(policy);
        }
        const auto materialized = api::MaterializeDurableAuthorizationContext(state, f.request);
        Check(materialized.ok, "scope matrix materializes");
        for (const auto& probe : scopes) {
          const bool expected = (mask & 1) && (allow_scope.is_nil() || allow_scope == probe) &&
              !((mask & 2) && (deny_scope.is_nil() || deny_scope == probe)) &&
              !((mask & 4) && probe == f.target);
          const auto decision = api::EvaluateMaterializedAuthorization(f.context, materialized.context, "SELECT", probe);
          Check(decision.authorized == expected, "binary allow deny policy scope matrix");
        }
      }
    }
  }
}
void InvalidContexts() {
  Fixture f;
  const auto normal = api::MaterializeDurableAuthorizationContext(f.state, f.request);
  for (unsigned variant = 0; variant < 13; ++variant) {
    auto context = normal.context;
    auto request = f.context;
    auto target = f.target;
    if (variant == 0) context.authority_uuid = {};
    if (variant == 1) context.security_context_generation = 0;
    if (variant == 2) context.principal_uuid = f.other;
    if (variant == 3) context.grants.front().grant_uuid = {};
    if (variant == 4) context.grants.front().grant_uuid.bytes[6] = 0x40;
    if (variant == 5) context.grants.front().security_epoch = 6;
    if (variant == 6) context.grants.front().target_uuid.bytes[8] = 0;
    if (variant == 7) context.effective_subjects.front().subject_uuid = {};
    if (variant == 8) context.effective_subjects.front().subject_kind = "unknown";
    if (variant == 9) context.engine_owned_bootstrap_role_uuid = f.other;
    if (variant == 10) target.bytes[6] = 0x40;
    if (variant == 11) request.security_context_present = false;
    if (variant == 12) request.security_epoch = 6;
    const auto result = api::EvaluateMaterializedAuthorization(request, context, "SELECT", target);
    Check(!result.authorized && !result.diagnostics.empty(), "malformed/stale context never authorizes");
  }
  for (unsigned version = 1; version <= 7; ++version) {
    auto state = f.state;
    state.grants.front().grant_uuid.bytes[6] = static_cast<std::uint8_t>(version << 4);
    Check(api::MaterializeDurableAuthorizationContext(state, f.request).ok == (version == 7),
          "system grant identity requires v7");
  }
  auto with_policy = normal.context;
  api::EngineMaterializedAuthorizationPolicy policy;
  policy.policy_uuid = Id(); policy.subject_uuid = f.principal; policy.subject_kind = "principal";
  policy.target_uuid = f.target; policy.right = "SELECT"; policy.policy_kind = "row_policy";
  policy.policy_epoch = 11; policy.requires_runtime_recheck = true;
  policy.source_policy_generation = 4; policy.update_policy_phase = 1;
  policy.effective_policy_uuid = Id(); policy.effective_policy_generation = 5;
  policy.effective_expression_uuid = Id(); policy.effective_expression_generation = 6;
  policy.effective_expression_evidence_sha256[0] = 1;
  with_policy.policies.push_back(policy);
  Check(api::EvaluateMaterializedAuthorization(f.context, with_policy, "SELECT", f.target).policy_recheck_required,
        "complete native row policy requests actual recheck");
  for (unsigned variant = 0; variant < 8; ++variant) {
    auto invalid = with_policy;
    auto& changed = invalid.policies.front();
    if (variant == 0) changed.source_policy_generation = 0;
    if (variant == 1) changed.update_policy_phase = 3;
    if (variant == 2) changed.effective_policy_uuid = {};
    if (variant == 3) changed.effective_policy_generation = 0;
    if (variant == 4) changed.effective_expression_uuid.bytes[6] = 0x40;
    if (variant == 5) changed.effective_expression_generation = 0;
    if (variant == 6) changed.effective_expression_evidence_sha256 = {};
    if (variant == 7) changed.policy_epoch = 10;
    Check(!api::EvaluateMaterializedAuthorization(f.context, invalid, "SELECT", f.target).authorized,
          "incomplete or stale native row policy never authorizes");
  }
}
void AllocationFailure() {
  Fixture f;
  unsigned faults = 0;
  bool success = false;
  for (long budget = 0; budget < 200; ++budget) {
    allocation_budget = budget;
    try {
      const auto result = api::MaterializeDurableAuthorizationContext(f.state, f.request);
      allocation_budget = -1;
      Check(result.ok && result.context.grants.size() == 1 && result.context.effective_subjects.size() == 3,
            "allocation sweep materializes complete context");
      success = true;
    } catch (const std::bad_alloc&) {
      allocation_budget = -1;
      ++faults;
    }
    Check(f.state.grants.size() == 1 && f.state.memberships.size() == 2,
          "allocation failure never mutates input authority");
    if (success) break;
  }
  Check(success && faults > 5, "closure allocation failures exercised");
  std::cout << "cpp_allocation_faults=" << faults << '\n';
}
}
int main() {
  ClosureAndDecision(); BootstrapAndTrace(); ScopeMatrix(); InvalidContexts(); AllocationFailure();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
