// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "agent_engine_lifecycle.hpp"
#include "agent_runtime_manager.hpp"
#include "../../drivers/tool/cli/binary_status_display.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace a = scratchbird::core::agents;
namespace w = scratchbird::wire;
using Uuid = scratchbird::core::platform::Uuid;
void Check(bool pass, const char* message) {
  if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}
Uuid Id(unsigned suffix) {
  Uuid id;
  id.bytes = {1, 0x9e, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0,
              static_cast<unsigned char>(suffix)};
  return id;
}
std::string Raw(const Uuid& id) {
  return {reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size()};
}
a::AgentRuntimeActivationEvidence Evidence() {
  a::AgentRuntimeActivationEvidence e;
  e.database_uuid = Raw(Id(1)); e.engine_instance_uuid = Raw(Id(2));
  e.policy_generation = e.catalog_generation = e.security_generation = 1;
  e.filespace_generation = e.agent_set_generation = e.health_generation = 1;
  e.tx1_bootstrap_visible = e.tx2_activation_committed = e.startup_admitted = true;
  e.health_publication_allowed = e.health_publication_persisted = true;
  return e;
}
a::AgentRuntimeManagerConfig Config() {
  a::InMemoryAgentRuntimeCatalog catalog;
  catalog.BootstrapDatabasePolicies(Evidence().database_uuid, 1);
  a::AgentRuntimeManagerConfig c;
  c.use_explicit_policy_state = true;
  c.policy_records = catalog.policies(); c.policy_attachments = catalog.attachments();
  return c;
}
const a::AgentRuntimeSelectionDecision& Decision(const a::AgentRuntimeManagerSnapshot& s,
                                                 std::string_view name) {
  const auto at = std::find_if(s.selection_decisions.begin(), s.selection_decisions.end(),
      [&](const auto& d) { return d.agent_type_id == name; });
  Check(at != s.selection_decisions.end(), "missing real selection decision");
  return *at;
}
void CheckPacket(const a::AgentRuntimeManagerSnapshot& s) {
  a::DatabaseEngineAgentHealthPublication health;
  health.database_uuid = Evidence().database_uuid;
  health.engine_instance_uuid = Evidence().engine_instance_uuid;
  health.selection_decisions = s.selection_decisions;
  const auto encoded = a::SerializeDatabaseEngineAgentHealthJson(health, true);
  std::vector<w::public_result::Field> fields;
  Check(w::binary_status::Decode(encoded, &fields), "health status is not canonical binary status");
  std::vector<std::string> identities;
  for (const auto& field : fields) {
    if (field.kind == w::public_result::Kind::uuid) {
      Check(field.value.size() == 16, "status UUID lost its binary16 carrier");
      identities.push_back(field.value);
    } else {
      Check(std::all_of(field.value.begin(), field.value.end(),
          [](unsigned char ch) { return ch < 128; }), "identity bytes leaked into status text");
    }
  }
  for (const auto& d : s.selection_decisions) {
    for (const auto* id : {&d.policy_uuid, &d.instance_uuid, &d.retirement_evidence_uuid}) {
      if (id->is_nil()) continue;
      Check(scratchbird::core::uuid::IsEngineIdentityUuid(*id), "selection identity is not system v7");
      const auto raw = Raw(*id);
      Check(std::find(identities.begin(), identities.end(), raw) != identities.end(),
            "selection reference missing from binary status atoms");
      Check(d.detail.find(raw) == std::string::npos, "diagnostic detail contains binary identity");
    }
  }
  const auto display = scratchbird::cli::RenderBinaryStatus(encoded);
  Check(display && std::all_of(display->begin(), display->end(),
      [](unsigned char ch) { return ch < 128; }), "client display is not textual JSON data");
}
int main() {
  const auto config = Config();
  const auto selected = a::SelectStandaloneDatabaseLocalAgents(Evidence(), config);
  Check(selected.status.ok && !selected.supervised_agents.empty(), "real baseline selection failed");
  for (const auto& instance : selected.supervised_agents) {
    const auto& d = Decision(selected, instance.agent_type_id);
    Check(d.selected && Raw(d.policy_uuid) == instance.policy_uuid &&
          Raw(d.instance_uuid) == instance.instance_uuid && d.retirement_evidence_uuid.is_nil(),
          "selection changed policy or instance UUID bits");
  }
  const auto& disabled = Decision(selected, "export_adapter_manager");
  Check(disabled.policy_disabled && !disabled.policy_uuid.is_nil() && disabled.instance_uuid.is_nil(),
        "disabled policy decision lost its identity or fabricated an instance");
  CheckPacket(selected);

  auto retired_config = config;
  auto retired = selected.supervised_agents.front();
  retired.state = a::AgentLifecycleState::retired;
  retired.retirement_evidence_uuid = Raw(Id(3));
  retired_config.persisted_instances = {retired};
  const auto retirement = a::SelectStandaloneDatabaseLocalAgents(Evidence(), retired_config);
  const auto& d = Decision(retirement, retired.agent_type_id);
  Check(retirement.status.ok && !d.selected && d.retirement_evidence_uuid == Id(3) &&
        Raw(d.instance_uuid) == retired.instance_uuid && Raw(d.policy_uuid) == retired.policy_uuid,
        "retirement references were lost or retired instance was selected");
  CheckPacket(retirement);
  retired_config.persisted_instances.front().retirement_evidence_uuid.clear();
  const auto missing = a::SelectStandaloneDatabaseLocalAgents(Evidence(), retired_config);
  const auto& missing_d = Decision(missing, retired.agent_type_id);
  Check(missing_d.failed_closed && !missing_d.selected && missing_d.retirement_evidence_uuid.is_nil() &&
        Raw(missing_d.instance_uuid) == retired.instance_uuid,
        "missing retirement evidence must retain instance identity without inventing evidence");
  CheckPacket(missing);

  std::vector<std::string> invalid{std::string(1, 'x'), std::string(15, 'x'),
      std::string(17, 'x'), "019e0000-0000-7000-8000-000000000003", std::string(16, '\0')};
  auto version4 = Id(3); version4.bytes[6] = 0x40; invalid.push_back(Raw(version4));
  auto variant = Id(3); variant.bytes[8] = 0x40; invalid.push_back(Raw(variant));
  for (const auto& bad : invalid) {
    auto c = config;
    const auto original = selected.supervised_agents.front().policy_uuid;
    for (auto& p : c.policy_records) if (p.policy_uuid == original) p.policy_uuid = bad;
    for (auto& p : c.policy_attachments) if (p.policy_uuid == original) p.policy_uuid = bad;
    const auto rejected = a::SelectStandaloneDatabaseLocalAgents(Evidence(), c);
    Check(!rejected.status.ok && rejected.status.diagnostic_code ==
          "ENGINE.AGENT_RUNTIME_MANAGER.IDENTITY_INVALID" &&
          rejected.selection_decisions.empty() && rejected.supervised_agents.empty() &&
          !rejected.ordinary_admission_allowed, "invalid policy UUID published an admitted cohort");
    c = retired_config; c.persisted_instances.front().retirement_evidence_uuid = bad;
    const auto bad_retirement = a::SelectStandaloneDatabaseLocalAgents(Evidence(), c);
    Check(!bad_retirement.status.ok && bad_retirement.selection_decisions.empty() &&
          bad_retirement.supervised_agents.empty(), "invalid retirement UUID published agent authority");
  }
  std::cout << "native selection identities, binary status, retirement and malformed references passed\n";
}
