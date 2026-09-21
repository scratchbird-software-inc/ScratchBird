// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "security/security_model.hpp"
#include "security/authorization_api.hpp"
#include "agents/agent_authorization_context.hpp"
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
void AuthorizationResponse() {
  Fixture f;
  api::EngineAuthorizeRequest request;
  request.context = f.context;
  request.context.authorization_context =
      api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  request.required_right = "SELECT";
  request.target_object.uuid = f.target;
  auto check_target = [&](const api::EngineAuthorizeResult& result,
                          const api::EngineUuid& expected) {
    const api::EngineTypedValue* field = nullptr;
    if (result.result_shape.rows.size() == 1)
      for (const auto& [name, value] : result.result_shape.rows.front().fields)
        if (name == "target_uuid") field = &value;
    Check(field != nullptr && field->descriptor.canonical_type_name == "uuid" &&
          field->encoded_value.empty() &&
          field->binary_value == std::vector<std::uint8_t>(expected.bytes.begin(), expected.bytes.end()),
          "authorization response retains exact raw16 target");
  };
  const auto allowed = api::EngineAuthorize(request);
  Check(allowed.ok && allowed.authorized && allowed.decision == "allow",
        "actual authorization API executes materialized allow");
  check_target(allowed, f.target);
  request.target_object.uuid = f.other;
  const auto denied = api::EngineAuthorize(request);
  Check(!denied.ok && !denied.authorized && denied.decision == "deny",
        "actual authorization API refuses different target");
  check_target(denied, f.other);
  request.target_object.uuid = f.target;
  f.state.grants.push_back({Id(), f.principal, "principal", f.target, "SELECT", true, true, 7});
  request.context.authorization_context =
      api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  const auto explicit_deny = api::EngineAuthorize(request);
  Check(!explicit_deny.ok && !explicit_deny.authorized,
        "response migration does not bypass explicit deny");
  check_target(explicit_deny, f.target);
  request.require_cluster_authority = true;
  const auto cluster = api::EngineAuthorize(request);
  Check(!cluster.ok && cluster.cluster_authority_required && cluster.result_shape.rows.empty(),
        "unavailable cluster authority never produces an allow row");

  api::EngineApiResult values;
  const std::string user_text = "019d0000-0000-7000-8000-000000000001";
  api::AddSecurityRow(&values, {{"uuid", f.target}, {"target_uuid", user_text}});
  const auto& fields = values.result_shape.rows.front().fields;
  Check(fields[0].second.binary_value == std::vector<std::uint8_t>(f.target.bytes.begin(), f.target.bytes.end()) &&
        fields[0].second.encoded_value.empty(), "security row transports binary UUID");
  Check(fields[1].second.descriptor.canonical_type_name == "text" &&
        fields[1].second.encoded_value == user_text && fields[1].second.binary_value.empty(),
        "UUID-looking TEXT remains TEXT even under identity-like field name");
  api::AddSecurityRow(&values, std::vector<std::pair<std::string, std::string>>{{"text", user_text}});
  Check(values.result_shape.rows.back().fields.front().second.encoded_value == user_text,
        "explicit TEXT vector overload remains supported");
  api::AddSecurityEvidence(&values, "identity", f.target);
  api::AddSecurityEvidence(&values, "text", user_text);
  Check(std::get<api::EngineUuid>(values.evidence[0].evidence_id) == f.target &&
        std::get<std::string>(values.evidence[1].evidence_id) == user_text,
        "security evidence preserves UUID and TEXT alternatives");
  for (unsigned version = 1; version <= 7; ++version) {
    auto user_uuid = f.target;
    user_uuid.bytes[6] = static_cast<std::uint8_t>((version << 4) | 0x0d);
    api::EngineApiResult data;
    api::AddSecurityRow(&data, {{"user_uuid_value", user_uuid}});
    Check(data.result_shape.rows.front().fields.front().second.binary_value ==
          std::vector<std::uint8_t>(user_uuid.bytes.begin(), user_uuid.bytes.end()),
          "response value transport preserves earlier-version user UUID data");
  }
}
void AgentAuthorizationProjection() {
  Fixture f;
  f.state.grants.front().right = "OBS_AGENT_STATE_READ";
  f.context.database_uuid = f.target;
  const auto materialize = [&]() {
    const auto result = api::MaterializeDurableAuthorizationContext(f.state, f.request);
    Check(result.ok, "agent fixture uses actual authorization materialization");
    f.context.authorization_context = result.context;
  };
  materialize();
  f.context.trace_tags = {"right:OBS_AGENT_CONTROL", "group:ROOT", "deny:OBS_AGENT_STATE_READ",
                          "security.fixture_trace_authority", "request.observed", "request.observed"};
  scratchbird::core::agents::AgentRuntimeContext runtime;
  runtime.read_only_mode = true;
  runtime.monotonic_now_microseconds = 73;
  const auto project = [&]() {
    api::agent_authorization::PopulateAgentRuntimeSecurityContext(
        f.context, &runtime, api::agent_authorization::Scope::local_node);
  };
  project();
  Check(runtime.security_context_present && !runtime.fixture_authorization_authority &&
        runtime.rights == std::vector<std::string>{"OBS_AGENT_STATE_READ"},
        "agent projection grants only evaluated materialized rights");
  Check(runtime.groups.empty() && runtime.effective_group_uuids == std::vector<api::EngineUuid>{f.group},
        "agent group identity is exact binary16, not fixture display-name authority");
  Check(runtime.trace_tags == std::vector<std::string>{"security.fixture_trace_authority", "request.observed"},
        "right group and deny trace strings cannot alter materialized authorization");
  Check(runtime.read_only_mode && runtime.monotonic_now_microseconds == 73,
        "security projection preserves unrelated runtime controls");

  f.state.grants.clear(); materialize(); project();
  Check(runtime.security_context_present && runtime.rights.empty(),
        "reusing runtime cannot retain revoked grants");
  runtime.rights = {"OBS_AGENT_CONTROL"}; runtime.groups = {"ROOT"};
  runtime.fixture_authorization_authority = true;
  f.context.principal_uuid = f.other; project();
  Check(!runtime.security_context_present && runtime.rights.empty() && runtime.groups.empty() &&
        runtime.effective_group_uuids.empty() && runtime.trace_tags.empty() &&
        !runtime.fixture_authorization_authority,
        "principal mismatch revokes old runtime and fixture authority");
  f.context.principal_uuid = f.principal;
  f.context.authorization_context.security_context_generation = 0; project();
  Check(!runtime.security_context_present, "zero authority generation cannot populate agent context");
  materialize(); f.context.security_epoch = 8; project();
  Check(!runtime.security_context_present, "stale agent security epoch rejected");
  f.context.security_epoch = 7;
  for (unsigned version = 1; version <= 8; ++version) {
    if (version == 7) continue;
    materialize();
    f.context.authorization_context.authority_uuid.bytes[6] =
        static_cast<std::uint8_t>(version << 4);
    project();
    Check(!runtime.security_context_present, "non-v7 agent system authority rejected");
  }
  materialize();
  f.context.authorization_context.effective_subjects.back().subject_uuid = {};
  project();
  Check(!runtime.security_context_present && runtime.effective_group_uuids.empty(),
        "nil effective subject does not partially publish group projection");

  f.state.grants.push_back({Id(), f.group, "group", f.target, "OBS_AGENT_STATE_READ", false, true, 7});
  f.state.grants.push_back({Id(), f.principal, "principal", f.target, "OBS_AGENT_STATE_READ", true, true, 7});
  materialize(); project();
  Check(runtime.rights.empty(), "agent projection preserves deny over group allow");
  f.state.grants.pop_back();
  api::DurableAuthorizationPolicyRecord policy;
  policy.policy_uuid = Id(); policy.subject_uuid = f.principal; policy.subject_kind = "principal";
  policy.target_uuid = f.target; policy.right = "OBS_AGENT_STATE_READ";
  policy.policy_kind = "agent_scope_policy"; policy.policy_epoch = 11;
  policy.requires_runtime_recheck = true;
  f.state.policies.push_back(policy); materialize(); project();
  Check(runtime.rights.empty(), "pending policy recheck never becomes an unconditional agent right");
  f.state.policies.clear();
  // Distinguish binary groups differing at every octet, including embedded zeros.
  for (unsigned byte = 0; byte < 16; ++byte) {
    auto other_group = f.group; other_group.bytes[byte] ^= 1;
    f.state.groups = {{f.group, true, 7}, {other_group, true, 7}};
    f.state.memberships.push_back({f.principal, "principal", other_group, "group", true, 7});
    materialize(); project();
    Check(runtime.effective_group_uuids.size() == 2 &&
          std::find(runtime.effective_group_uuids.begin(), runtime.effective_group_uuids.end(), f.group) !=
              runtime.effective_group_uuids.end() &&
          std::find(runtime.effective_group_uuids.begin(), runtime.effective_group_uuids.end(), other_group) !=
              runtime.effective_group_uuids.end(),
          "agent group projection retains all sixteen identity octets");
    f.state.memberships.pop_back();
  }
  f.state.groups = {{f.group, true, 7}}; materialize();
  unsigned faults = 0; bool success = false;
  for (long budget = 0; budget < 256; ++budget) {
    runtime.security_context_present = true;
    runtime.fixture_authorization_authority = true;
    runtime.rights = {"OBS_AGENT_CONTROL"}; runtime.groups = {"ROOT"};
    runtime.effective_group_uuids = {f.other};
    allocation_budget = budget;
    try {
      project(); allocation_budget = -1; success = true;
      Check(runtime.security_context_present && !runtime.fixture_authorization_authority &&
            runtime.rights == std::vector<std::string>{"OBS_AGENT_STATE_READ"} &&
            runtime.effective_group_uuids == std::vector<api::EngineUuid>{f.group},
            "agent allocation sweep publishes only complete authorization projection");
    } catch (const std::bad_alloc&) {
      allocation_budget = -1; ++faults;
      Check(!runtime.security_context_present && !runtime.fixture_authorization_authority &&
            runtime.rights.empty() && runtime.groups.empty() &&
            runtime.effective_group_uuids.empty() && runtime.trace_tags.empty(),
            "allocation failure leaves reused agent runtime without partial or stale authority");
    }
    if (success) break;
  }
  Check(success && faults > 5, "agent projection allocation-failure paths exercised");
  std::cout << "agent_projection_allocation_faults=" << faults << '\n';
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(
      f.context, nullptr, api::agent_authorization::Scope::local_node);
}
void AgentAuthorizationScopeAndBootstrap() {
  using Scope = api::agent_authorization::Scope;
  Fixture f;
  const auto cluster = Id();
  f.context.database_uuid = f.target;
  f.context.cluster_uuid = cluster;
  f.context.cluster_authority_available = false;
  const auto target = [&](unsigned selector) {
    if (selector == 1) return f.target;
    if (selector == 2) return cluster;
    if (selector == 3) return f.other;
    return api::EngineUuid{};
  };
  const auto applies = [](unsigned selector, Scope scope) {
    return selector == 0 || selector == (scope == Scope::local_node ? 1u : 2u);
  };
  for (const auto scope : {Scope::local_node, Scope::cluster}) {
    for (unsigned grant = 0; grant < 4; ++grant) {
      for (unsigned deny = 0; deny < 5; ++deny) {
        for (unsigned recheck = 0; recheck < 5; ++recheck) {
          f.state.grants = {{Id(), f.group, "group", target(grant), "OBS_AGENT_STATE_READ", false, true, 7}};
          if (deny != 4) {
            f.state.grants.push_back({Id(), f.principal, "principal", target(deny),
                                      "OBS_AGENT_STATE_READ", true, true, 7});
          }
          f.state.policies.clear();
          if (recheck != 4) {
            api::DurableAuthorizationPolicyRecord policy;
            policy.policy_uuid = Id(); policy.subject_uuid = f.principal; policy.subject_kind = "principal";
            policy.target_uuid = target(recheck); policy.right = "OBS_AGENT_STATE_READ";
            policy.policy_kind = "agent_scope_policy"; policy.policy_epoch = 11;
            policy.requires_runtime_recheck = true;
            f.state.policies.push_back(policy);
          }
          const auto materialized = api::MaterializeDurableAuthorizationContext(f.state, f.request);
          Check(materialized.ok, "agent scope fixture materializes exact target grants and policies");
          f.context.authorization_context = materialized.context;
          scratchbird::core::agents::AgentRuntimeContext runtime;
          api::agent_authorization::PopulateAgentRuntimeSecurityContext(f.context, &runtime, scope);
          const bool expected = applies(grant, scope) && !applies(deny, scope) && !applies(recheck, scope);
          Check(api::agent_authorization::RightAllowed(f.context, scope, "OBS_AGENT_STATE_READ") == expected &&
                (runtime.rights == std::vector<std::string>{"OBS_AGENT_STATE_READ"}) == expected &&
                (expected || runtime.rights.empty()),
                "local and cluster grants denies and rechecks remain in their exact target scope");
          Check(scratchbird::core::agents::EvaluateAgentCommandGrant(
                    runtime, scratchbird::core::agents::AgentSecurityCommandFamily::state_read,
                    "agents.list", false, false).allowed == expected,
                "actual agent command evaluator consumes the scoped rights projection");
          Check(runtime.security_context_present &&
                runtime.authorization_cluster_scope == (scope == Scope::cluster) &&
                runtime.authorization_target_uuid == (scope == Scope::cluster ? cluster : f.target),
                "agent projection publishes the exact binary evaluated target");
        }
      }
    }
  }
  // Every right in the normative agent grant matrix must materialize and
  // evaluate; right vocabulary alone must not authorize any operation.
  const std::vector<std::string> agent_rights{
      "OBS_AGENT_STATE_READ", "OBS_AGENT_EVIDENCE_READ", "OBS_AGENT_RECOMMENDATION_READ",
      "OBS_AGENT_CONTROL", "OBS_AGENT_ACTION_APPROVE", "OBS_AGENT_ACTION_CANCEL",
      "OBS_AGENT_OVERRIDE", "OBS_SUPPORT_BUNDLE_READ", "OBS_POLICY_READ", "OBS_POLICY_SIMULATE",
      "OBS_POLICY_EDIT_DRAFT", "OBS_POLICY_VALIDATE", "OBS_POLICY_APPROVE", "OBS_POLICY_APPLY",
      "OBS_POLICY_ROLLBACK", "OBS_POLICY_DELETE", "OBS_CLUSTER_HEALTH_INSPECT",
      "OBS_CLUSTER_TOPOLOGY_INSPECT", "OBS_CLUSTER_CONTROL", "SEC_AUTH_METRICS_READ",
      "SEC_REDACTION_POLICY_EDIT", "SEC_EXPORT_POLICY_APPROVE"};
  f.state.policies.clear();
  for (const auto& right : agent_rights) {
    f.state.grants = {{Id(), f.group, "group", f.target, right, false, true, 7}};
    const auto materialized = api::MaterializeDurableAuthorizationContext(f.state, f.request);
    f.context.authorization_context = materialized.context;
    Check(api::IsKnownSecurityRight(right) && api::KnownSecurityRights().count(right) == 1 &&
          materialized.ok && api::agent_authorization::RightAllowed(f.context, Scope::local_node, right) &&
          !api::agent_authorization::RightAllowed(f.context, Scope::cluster, right),
          "all normative agent rights evaluate only at their granted target");
  }
  f.state.grants.clear();
  f.state.engine_owned_sysarch_role_uuid = f.role;
  f.context.authorization_context = api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  scratchbird::core::agents::AgentRuntimeContext runtime;
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(f.context, &runtime, Scope::local_node);
  Check(runtime.rights.size() == api::KnownSecurityRights().size() &&
        f.context.authorization_context.grants.empty(),
        "admitted immutable bootstrap role projects rights without invented grant records");
  f.state.grants.push_back({Id(), f.principal, "principal", f.target, "OBS_AGENT_CONTROL", true, true, 7});
  f.context.authorization_context = api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(f.context, &runtime, Scope::local_node);
  Check(std::find(runtime.rights.begin(), runtime.rights.end(), "OBS_AGENT_CONTROL") == runtime.rights.end() &&
        runtime.rights.size() + 1 == api::KnownSecurityRights().size(),
        "explicit deny remains stronger than bootstrap agent projection");
  f.context.cluster_uuid = {};
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(f.context, &runtime, Scope::cluster);
  Check(runtime.security_context_present && runtime.authorization_target_uuid.is_nil() &&
        runtime.authorization_cluster_scope && runtime.rights.size() == api::KnownSecurityRights().size() &&
        !f.context.cluster_authority_available,
        "global bootstrap authorization can reach absent-provider route without inventing cluster identity");
  Check(scratchbird::core::agents::EvaluateAgentCommandGrant(
            runtime, scratchbird::core::agents::AgentSecurityCommandFamily::cluster_inspect,
            "cluster.sys.agents", false, false).allowed,
        "actual cluster inspect permission check leaves absent-provider routing to gateway");
  f.state.engine_owned_sysarch_role_uuid = {};
  f.state.grants = {{Id(), f.principal, "principal", f.target, "OBS_AGENT_CONTROL", false, true, 7}};
  f.context.authorization_context = api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(f.context, &runtime, Scope::cluster);
  Check(runtime.rights.empty(), "node-specific permission cannot authorize a missing cluster target");
  f.context.database_uuid = {};
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(f.context, &runtime, Scope::local_node);
  Check(!runtime.security_context_present && runtime.rights.empty() && runtime.authorization_target_uuid.is_nil(),
        "missing node identity cannot create a global local-operation grant");
  f.context.database_uuid = f.target;
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(f.context, &runtime, static_cast<Scope>(9));
  Check(!runtime.security_context_present && runtime.rights.empty(), "unknown authorization scope rejected");
}
void AgentFixtureLabelsNeverGrant() {
  namespace agents = scratchbird::core::agents;
  std::vector<std::string> rights(api::KnownSecurityRights().begin(), api::KnownSecurityRights().end());
  rights.push_back("OBS_AGENT_INTERNAL");
  rights.push_back("SB_AGENT_INTERNAL_TRACE");
  rights.push_back("unregistered_fixture_right");
  for (const bool fixture_hint : {false, true}) {
    for (const auto* group : {"ROOT", "OPS", "SUP", "AUD", "DBA", "SEC", "ETL", "SCH", "PUBLIC"}) {
      for (const auto& right : rights) {
        agents::AgentRuntimeContext runtime;
        runtime.security_context_present = true;
        runtime.fixture_authorization_authority = fixture_hint;
        runtime.groups = {group};
        runtime.trace_tags = {"right:" + right, "group:" + std::string(group),
                              "engine.internal", "agent.internal", "internal"};
        Check(!agents::AgentContextHasRight(runtime, right),
              "actual agent evaluator never grants from fixture hints group names or trace strings");
      }
    }
  }
  Fixture f;
  f.context.database_uuid = f.target;
  f.state.grants.front().right = "OBS_AGENT_CONTROL";
  f.context.authorization_context = api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  agents::AgentRuntimeContext runtime;
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(
      f.context, &runtime, api::agent_authorization::Scope::local_node);
  runtime.fixture_authorization_authority = true;
  runtime.groups = {"ROOT", "OPS"};
  runtime.trace_tags = {"deny:OBS_AGENT_CONTROL", "right:OBS_AGENT_OVERRIDE"};
  Check(agents::AgentContextHasRight(runtime, "OBS_AGENT_CONTROL") &&
        !agents::AgentContextHasRight(runtime, "OBS_AGENT_OVERRIDE"),
        "display and trace metadata neither adds nor removes a materialized right");
  f.state.grants.clear();
  f.context.authorization_context = api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(
      f.context, &runtime, api::agent_authorization::Scope::local_node);
  Check(!runtime.effective_group_uuids.empty() &&
        !agents::AgentContextHasRight(runtime, "OBS_AGENT_CONTROL"),
        "binary group membership alone is not an unconditional privilege bundle");
  f.state.engine_owned_sysarch_role_uuid = f.role;
  f.context.authorization_context = api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  api::agent_authorization::PopulateAgentRuntimeSecurityContext(
      f.context, &runtime, api::agent_authorization::Scope::local_node);
  Check(agents::AgentContextHasRight(runtime, "OBS_AGENT_CONTROL"),
        "actual immutable bootstrap role still grants through materialization and evaluated projection");
}
void AuthorizationResponseAllocationFailure() {
  Fixture f;
  api::EngineAuthorizeRequest request;
  request.context = f.context;
  request.context.authorization_context =
      api::MaterializeDurableAuthorizationContext(f.state, f.request).context;
  request.required_right = "SELECT";
  request.target_object.uuid = f.target;
  const auto grant_uuid = request.context.authorization_context.grants.front().grant_uuid;
  bool success = false;
  unsigned faults = 0;
  for (long budget = 0; budget < 256; ++budget) {
    allocation_budget = budget;
    try {
      const auto result = api::EngineAuthorize(request);
      allocation_budget = -1;
      Check(result.ok && result.authorized && result.result_shape.rows.size() == 1,
            "allocation sweep returns only complete authorization response");
      success = true;
    } catch (const std::bad_alloc&) {
      allocation_budget = -1;
      ++faults;
    }
    Check(request.target_object.uuid == f.target &&
          request.context.authorization_context.grants.size() == 1 &&
          request.context.authorization_context.grants.front().grant_uuid == grant_uuid,
          "authorization response allocation failure preserves input authority");
    if (success) break;
  }
  Check(success && faults > 5, "actual authorization response allocation faults exercised");
  std::cout << "authorization_response_allocation_faults=" << faults << '\n';
}
}
int main() {
  ClosureAndDecision(); BootstrapAndTrace(); ScopeMatrix(); InvalidContexts(); AllocationFailure(); AuthorizationResponse();
  AuthorizationResponseAllocationFailure();
  AgentAuthorizationProjection();
  AgentAuthorizationScopeAndBootstrap();
  AgentFixtureLabelsNeverGrant();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
