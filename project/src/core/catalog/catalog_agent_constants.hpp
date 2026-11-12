// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_CATALOG_AGENT_CONSTANTS (owner Q68 = 1, Q69 = 1, Q70 = 2,
// 2026-11-12). Agent types, agent policy families and metric families are
// coding-time identifiers: each has a code fixed forever (append-only, never
// renumbered) and a permanent UUIDv7 (generated once 2026-11-12, recorded
// before use), its identity in the names table. `tag` is the boundary
// spelling only (the text the current code writes, mapped once at the
// boundary); engine code compares the constant. Constants only: nothing is
// wired to these yet.
//
// GENERATED from 28d (Claude-temp-directory/2026-10-30-full-run/
// firebird-mga-research/28d-agent-metric-uuids.md,
// sha256 37e19cadd3679b6a4a950d4fbc33633bf53f8391aeb884a00d417e22c299b97a);
// never edit by hand. Hand-written small classifications follow the
// generated part.
#include "runtime_platform.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {

// ---- GENERATED from 28d; never edit by hand ----

// agent types (32)
enum class CatalogAgentType : scratchbird::core::platform::u16 {
  none = 0,  // no listed value; never stored
  node_resource_agent = 1,  // 01a1d1d7-7448-7f66-b994-4c738406156a
  metrics_registry_manager = 2,  // 01a1d1d7-7449-7a18-8974-c16f4666c165
  storage_health_manager = 3,  // 01a1d1d7-744a-7a33-b8d6-32edcef40471
  filespace_capacity_manager = 4,  // 01a1d1d7-744b-70b6-bf84-c12df2418b4b
  page_allocation_manager = 5,  // 01a1d1d7-744c-7c5c-b903-462bc6156af2
  memory_governor = 6,  // 01a1d1d7-744d-704e-9e98-a099debfeb57
  index_health_manager = 7,  // 01a1d1d7-744e-79f5-a27c-567b948dd6c8
  cluster_autoscale_manager = 8,  // 01a1d1d7-744f-7a82-9cef-e390a9dad379
  admission_control_manager = 9,  // 01a1d1d7-7450-715d-9d3f-710a92a4c0cb
  parser_interface_manager = 10,  // 01a1d1d7-7451-7037-9eee-754a834314ba
  transaction_pressure_manager = 11,  // 01a1d1d7-7452-7f3e-a4fd-886f8a5c9c7d
  storage_version_cleanup_agent = 12,  // 01a1d1d7-7453-7528-ad86-97d6df4e5cf1
  cleanup_archive_manager = 13,  // 01a1d1d7-7454-7840-adac-c8c9939e9893
  policy_recommendation_manager = 14,  // 01a1d1d7-7455-750e-a960-317e3df6f329
  distributed_query_metrics_agent = 15,  // 01a1d1d7-7456-7386-b77a-28858f3964ab
  remote_query_routing_agent = 16,  // 01a1d1d7-7457-7f29-a68e-1bede7beffa2
  runtime_learning_agent = 17,  // 01a1d1d7-7458-7aec-b558-2f4b9ef33136
  support_bundle_triage_agent = 18,  // 01a1d1d7-7459-7084-ae36-3883d61e0e3c
  cluster_scheduler_manager = 19,  // 01a1d1d7-745a-70e5-a727-b489c2db5880
  job_control_manager = 20,  // 01a1d1d7-745b-71d5-9583-307e35f256c0
  backup_manager = 21,  // 01a1d1d7-745c-7867-a8d4-382f5e619b03
  archive_manager = 22,  // 01a1d1d7-745d-70bb-b094-599d1caccde7
  restore_drill_manager = 23,  // 01a1d1d7-745e-7be8-922f-09554dfc506c
  pitr_manager = 24,  // 01a1d1d7-745f-7145-bbed-7ab1b365fe63
  identity_manager = 25,  // 01a1d1d7-7460-78b0-a1ce-a4bf94aa7d18
  session_control_manager = 26,  // 01a1d1d7-7461-7ffb-b633-767f7da72e1c
  alert_manager = 27,  // 01a1d1d7-7462-7433-af49-70ca6801ea1d
  export_adapter_manager = 28,  // 01a1d1d7-7463-7c78-bcae-b358eeab9075
  cluster_upgrade_manager = 29,  // 01a1d1d7-7464-751d-8628-201cc076bbc0
  agent_coordinator = 30,  // 01a1d1d7-7465-708b-bffa-57df41cbb4b2
  startup_agent = 31,  // 01a1d1d7-7466-712b-b50b-0fe65413677c
  shutdown_agent = 32,  // 01a1d1d7-7467-78bc-90b9-8094899c0d34
};
struct CatalogAgentTypeIdentity {
  CatalogAgentType type;
  scratchbird::core::platform::Uuid uuid;
  std::string_view tag;  // boundary spelling only
};
inline constexpr std::array<CatalogAgentTypeIdentity, 32> kCatalogAgentTypes{{
    CatalogAgentTypeIdentity{CatalogAgentType::node_resource_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x48,0x7f,0x66,0xb9,0x94,0x4c,0x73,0x84,0x06,0x15,0x6a}}, "node_resource_agent"},
    CatalogAgentTypeIdentity{CatalogAgentType::metrics_registry_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x49,0x7a,0x18,0x89,0x74,0xc1,0x6f,0x46,0x66,0xc1,0x65}}, "metrics_registry_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::storage_health_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x4a,0x7a,0x33,0xb8,0xd6,0x32,0xed,0xce,0xf4,0x04,0x71}}, "storage_health_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::filespace_capacity_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x4b,0x70,0xb6,0xbf,0x84,0xc1,0x2d,0xf2,0x41,0x8b,0x4b}}, "filespace_capacity_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::page_allocation_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x4c,0x7c,0x5c,0xb9,0x03,0x46,0x2b,0xc6,0x15,0x6a,0xf2}}, "page_allocation_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::memory_governor, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x4d,0x70,0x4e,0x9e,0x98,0xa0,0x99,0xde,0xbf,0xeb,0x57}}, "memory_governor"},
    CatalogAgentTypeIdentity{CatalogAgentType::index_health_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x4e,0x79,0xf5,0xa2,0x7c,0x56,0x7b,0x94,0x8d,0xd6,0xc8}}, "index_health_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::cluster_autoscale_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x4f,0x7a,0x82,0x9c,0xef,0xe3,0x90,0xa9,0xda,0xd3,0x79}}, "cluster_autoscale_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::admission_control_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x50,0x71,0x5d,0x9d,0x3f,0x71,0x0a,0x92,0xa4,0xc0,0xcb}}, "admission_control_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::parser_interface_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x51,0x70,0x37,0x9e,0xee,0x75,0x4a,0x83,0x43,0x14,0xba}}, "parser_interface_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::transaction_pressure_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x52,0x7f,0x3e,0xa4,0xfd,0x88,0x6f,0x8a,0x5c,0x9c,0x7d}}, "transaction_pressure_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::storage_version_cleanup_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x53,0x75,0x28,0xad,0x86,0x97,0xd6,0xdf,0x4e,0x5c,0xf1}}, "storage_version_cleanup_agent"},
    CatalogAgentTypeIdentity{CatalogAgentType::cleanup_archive_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x54,0x78,0x40,0xad,0xac,0xc8,0xc9,0x93,0x9e,0x98,0x93}}, "cleanup_archive_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::policy_recommendation_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x55,0x75,0x0e,0xa9,0x60,0x31,0x7e,0x3d,0xf6,0xf3,0x29}}, "policy_recommendation_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::distributed_query_metrics_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x56,0x73,0x86,0xb7,0x7a,0x28,0x85,0x8f,0x39,0x64,0xab}}, "distributed_query_metrics_agent"},
    CatalogAgentTypeIdentity{CatalogAgentType::remote_query_routing_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x57,0x7f,0x29,0xa6,0x8e,0x1b,0xed,0xe7,0xbe,0xff,0xa2}}, "remote_query_routing_agent"},
    CatalogAgentTypeIdentity{CatalogAgentType::runtime_learning_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x58,0x7a,0xec,0xb5,0x58,0x2f,0x4b,0x9e,0xf3,0x31,0x36}}, "runtime_learning_agent"},
    CatalogAgentTypeIdentity{CatalogAgentType::support_bundle_triage_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x59,0x70,0x84,0xae,0x36,0x38,0x83,0xd6,0x1e,0x0e,0x3c}}, "support_bundle_triage_agent"},
    CatalogAgentTypeIdentity{CatalogAgentType::cluster_scheduler_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x5a,0x70,0xe5,0xa7,0x27,0xb4,0x89,0xc2,0xdb,0x58,0x80}}, "cluster_scheduler_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::job_control_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x5b,0x71,0xd5,0x95,0x83,0x30,0x7e,0x35,0xf2,0x56,0xc0}}, "job_control_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::backup_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x5c,0x78,0x67,0xa8,0xd4,0x38,0x2f,0x5e,0x61,0x9b,0x03}}, "backup_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::archive_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x5d,0x70,0xbb,0xb0,0x94,0x59,0x9d,0x1c,0xac,0xcd,0xe7}}, "archive_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::restore_drill_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x5e,0x7b,0xe8,0x92,0x2f,0x09,0x55,0x4d,0xfc,0x50,0x6c}}, "restore_drill_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::pitr_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x5f,0x71,0x45,0xbb,0xed,0x7a,0xb1,0xb3,0x65,0xfe,0x63}}, "pitr_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::identity_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x60,0x78,0xb0,0xa1,0xce,0xa4,0xbf,0x94,0xaa,0x7d,0x18}}, "identity_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::session_control_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x61,0x7f,0xfb,0xb6,0x33,0x76,0x7f,0x7d,0xa7,0x2e,0x1c}}, "session_control_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::alert_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x62,0x74,0x33,0xaf,0x49,0x70,0xca,0x68,0x01,0xea,0x1d}}, "alert_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::export_adapter_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x63,0x7c,0x78,0xbc,0xae,0xb3,0x58,0xee,0xab,0x90,0x75}}, "export_adapter_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::cluster_upgrade_manager, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x64,0x75,0x1d,0x86,0x28,0x20,0x1c,0xc0,0x76,0xbb,0xc0}}, "cluster_upgrade_manager"},
    CatalogAgentTypeIdentity{CatalogAgentType::agent_coordinator, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x65,0x70,0x8b,0xbf,0xfa,0x57,0xdf,0x41,0xcb,0xb4,0xb2}}, "agent_coordinator"},
    CatalogAgentTypeIdentity{CatalogAgentType::startup_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x66,0x71,0x2b,0xb5,0x0b,0x0f,0xe6,0x54,0x13,0x67,0x7c}}, "startup_agent"},
    CatalogAgentTypeIdentity{CatalogAgentType::shutdown_agent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x67,0x78,0xbc,0x90,0xb9,0x80,0x94,0x89,0x9c,0x0d,0x34}}, "shutdown_agent"},
}};
constexpr bool IsKnownCatalogAgentType(CatalogAgentType value) noexcept {
  for (const auto& r : kCatalogAgentTypes)
    if (r.type == value) return true;
  return false;
}
constexpr std::optional<scratchbird::core::platform::Uuid> CatalogAgentTypeIdentityOf(CatalogAgentType value) noexcept {
  for (const auto& r : kCatalogAgentTypes)
    if (r.type == value) return r.uuid;
  return std::nullopt;
}
// Boundary adapter: the constant a text tag names; nullopt for one with no
// constant (refused by the caller, never defaulted).
constexpr std::optional<CatalogAgentType> CatalogAgentTypeForTag(std::string_view tag) noexcept {
  for (const auto& r : kCatalogAgentTypes)
    if (r.tag == tag) return r.type;
  return std::nullopt;
}

// agent policy families (36)
enum class CatalogAgentPolicyFamily : scratchbird::core::platform::u16 {
  none = 0,  // no listed value; never stored
  node_resource_policy = 1,  // 01a1d1d7-7468-78b3-9f81-04860e4dfac6
  metric_registry_policy = 2,  // 01a1d1d7-7469-7218-8a2c-dd01c64c0797
  storage_health_policy = 3,  // 01a1d1d7-746a-7d56-b7bb-7a31bd06b6d0
  memory_governor_policy = 4,  // 01a1d1d7-746b-731f-99ca-9acc37da3615
  index_health_policy = 5,  // 01a1d1d7-746c-7528-a296-825615cd7a9c
  cluster_autoscale_policy = 6,  // 01a1d1d7-746d-7909-9b1d-2515c186218d
  admission_control_policy = 7,  // 01a1d1d7-746e-7cae-90cc-fe8a299804d9
  parser_health_policy = 8,  // 01a1d1d7-746f-7a94-8597-46292a26aad6
  long_transaction_policy = 9,  // 01a1d1d7-7470-7a1d-b369-b5ab9b99db3b
  storage_version_cleanup_policy = 10,  // 01a1d1d7-7471-7596-b45f-5a0e925740e7
  cleanup_archive_policy = 11,  // 01a1d1d7-7472-7582-b68e-38c4c5bb0265
  policy_recommendation_policy = 12,  // 01a1d1d7-7473-710f-b3a4-3273d6804bf8
  distributed_query_policy = 13,  // 01a1d1d7-7474-78cd-b3a6-7b2753a8a389
  remote_routing_policy = 14,  // 01a1d1d7-7475-7db3-a1e3-9e9ee8cc4de7
  optimizer_learning_policy = 15,  // 01a1d1d7-7476-7538-82b0-249cd24d46d8
  support_bundle_policy = 16,  // 01a1d1d7-7477-7b5c-8e3b-b204b300575c
  cluster_scheduler_policy = 17,  // 01a1d1d7-7478-7599-b58c-725d569ad2e5
  job_control_policy = 18,  // 01a1d1d7-7479-7803-9a07-82ad2c7bdb45
  backup_policy = 19,  // 01a1d1d7-747a-7bfe-abf7-761005d024ef
  archive_policy = 20,  // 01a1d1d7-747b-74d2-899c-b462ea7e44dc
  restore_drill_policy = 21,  // 01a1d1d7-747c-755f-91a2-a2a6c5331731
  pitr_policy = 22,  // 01a1d1d7-747d-7d78-a377-2b471bd192b8
  identity_lifecycle_policy = 23,  // 01a1d1d7-747e-7dd3-99b8-2fb5dcd7ae07
  session_control_policy = 24,  // 01a1d1d7-747f-717d-905f-a616f5d9b2a3
  alerting_baseline = 25,  // 01a1d1d7-7480-78f5-8674-07648699b5ec
  export_default_baseline = 26,  // 01a1d1d7-7481-786d-87d3-b9318ef48731
  upgrade_policy = 27,  // 01a1d1d7-7482-7b24-8e8f-4ae263b8cd52
  agent_coordinator_policy = 28,  // 01a1d1d7-7483-79a4-9380-3effba41cb7e
  agent_startup_policy = 29,  // 01a1d1d7-7484-7cf3-bf34-dc2b73997950
  agent_shutdown_policy = 30,  // 01a1d1d7-7485-71ff-a09b-337b9b01dd7d
  coordinator_policy = 31,  // 01a1d1d7-7486-79f9-9587-f81dc4d3dad0
  cleanup_cycle_policy = 32,  // 01a1d1d7-7487-7c91-a028-793d695835cb
  filespace_capacity_policy = 33,  // 01a1d1d7-7488-7255-bdd2-b7d50e3d4df1
  filespace_shadow_promotion_policy = 34,  // 01a1d1d7-7489-7e07-9ea8-94c89dc4dde1
  page_preallocation_policy = 35,  // 01a1d1d7-748a-7255-88d4-69290e0e95ee
  page_relocation_policy = 36,  // 01a1d1d7-748b-731b-9ba7-19a88b6e7f85
};
struct CatalogAgentPolicyFamilyIdentity {
  CatalogAgentPolicyFamily family;
  scratchbird::core::platform::Uuid uuid;
  std::string_view tag;  // boundary spelling only
};
inline constexpr std::array<CatalogAgentPolicyFamilyIdentity, 36> kCatalogAgentPolicyFamilies{{
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::node_resource_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x68,0x78,0xb3,0x9f,0x81,0x04,0x86,0x0e,0x4d,0xfa,0xc6}}, "node_resource_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::metric_registry_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x69,0x72,0x18,0x8a,0x2c,0xdd,0x01,0xc6,0x4c,0x07,0x97}}, "metric_registry_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::storage_health_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x6a,0x7d,0x56,0xb7,0xbb,0x7a,0x31,0xbd,0x06,0xb6,0xd0}}, "storage_health_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::memory_governor_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x6b,0x73,0x1f,0x99,0xca,0x9a,0xcc,0x37,0xda,0x36,0x15}}, "memory_governor_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::index_health_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x6c,0x75,0x28,0xa2,0x96,0x82,0x56,0x15,0xcd,0x7a,0x9c}}, "index_health_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::cluster_autoscale_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x6d,0x79,0x09,0x9b,0x1d,0x25,0x15,0xc1,0x86,0x21,0x8d}}, "cluster_autoscale_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::admission_control_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x6e,0x7c,0xae,0x90,0xcc,0xfe,0x8a,0x29,0x98,0x04,0xd9}}, "admission_control_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::parser_health_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x6f,0x7a,0x94,0x85,0x97,0x46,0x29,0x2a,0x26,0xaa,0xd6}}, "parser_health_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::long_transaction_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x70,0x7a,0x1d,0xb3,0x69,0xb5,0xab,0x9b,0x99,0xdb,0x3b}}, "long_transaction_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::storage_version_cleanup_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x71,0x75,0x96,0xb4,0x5f,0x5a,0x0e,0x92,0x57,0x40,0xe7}}, "storage_version_cleanup_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::cleanup_archive_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x72,0x75,0x82,0xb6,0x8e,0x38,0xc4,0xc5,0xbb,0x02,0x65}}, "cleanup_archive_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::policy_recommendation_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x73,0x71,0x0f,0xb3,0xa4,0x32,0x73,0xd6,0x80,0x4b,0xf8}}, "policy_recommendation_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::distributed_query_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x74,0x78,0xcd,0xb3,0xa6,0x7b,0x27,0x53,0xa8,0xa3,0x89}}, "distributed_query_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::remote_routing_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x75,0x7d,0xb3,0xa1,0xe3,0x9e,0x9e,0xe8,0xcc,0x4d,0xe7}}, "remote_routing_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::optimizer_learning_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x76,0x75,0x38,0x82,0xb0,0x24,0x9c,0xd2,0x4d,0x46,0xd8}}, "optimizer_learning_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::support_bundle_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x77,0x7b,0x5c,0x8e,0x3b,0xb2,0x04,0xb3,0x00,0x57,0x5c}}, "support_bundle_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::cluster_scheduler_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x78,0x75,0x99,0xb5,0x8c,0x72,0x5d,0x56,0x9a,0xd2,0xe5}}, "cluster_scheduler_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::job_control_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x79,0x78,0x03,0x9a,0x07,0x82,0xad,0x2c,0x7b,0xdb,0x45}}, "job_control_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::backup_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x7a,0x7b,0xfe,0xab,0xf7,0x76,0x10,0x05,0xd0,0x24,0xef}}, "backup_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::archive_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x7b,0x74,0xd2,0x89,0x9c,0xb4,0x62,0xea,0x7e,0x44,0xdc}}, "archive_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::restore_drill_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x7c,0x75,0x5f,0x91,0xa2,0xa2,0xa6,0xc5,0x33,0x17,0x31}}, "restore_drill_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::pitr_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x7d,0x7d,0x78,0xa3,0x77,0x2b,0x47,0x1b,0xd1,0x92,0xb8}}, "pitr_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::identity_lifecycle_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x7e,0x7d,0xd3,0x99,0xb8,0x2f,0xb5,0xdc,0xd7,0xae,0x07}}, "identity_lifecycle_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::session_control_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x7f,0x71,0x7d,0x90,0x5f,0xa6,0x16,0xf5,0xd9,0xb2,0xa3}}, "session_control_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::alerting_baseline, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x80,0x78,0xf5,0x86,0x74,0x07,0x64,0x86,0x99,0xb5,0xec}}, "alerting_baseline"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::export_default_baseline, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x81,0x78,0x6d,0x87,0xd3,0xb9,0x31,0x8e,0xf4,0x87,0x31}}, "export_default_baseline"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::upgrade_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x82,0x7b,0x24,0x8e,0x8f,0x4a,0xe2,0x63,0xb8,0xcd,0x52}}, "upgrade_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::agent_coordinator_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x83,0x79,0xa4,0x93,0x80,0x3e,0xff,0xba,0x41,0xcb,0x7e}}, "agent_coordinator_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::agent_startup_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x84,0x7c,0xf3,0xbf,0x34,0xdc,0x2b,0x73,0x99,0x79,0x50}}, "agent_startup_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::agent_shutdown_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x85,0x71,0xff,0xa0,0x9b,0x33,0x7b,0x9b,0x01,0xdd,0x7d}}, "agent_shutdown_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::coordinator_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x86,0x79,0xf9,0x95,0x87,0xf8,0x1d,0xc4,0xd3,0xda,0xd0}}, "coordinator_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::cleanup_cycle_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x87,0x7c,0x91,0xa0,0x28,0x79,0x3d,0x69,0x58,0x35,0xcb}}, "cleanup_cycle_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::filespace_capacity_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x88,0x72,0x55,0xbd,0xd2,0xb7,0xd5,0x0e,0x3d,0x4d,0xf1}}, "filespace_capacity_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::filespace_shadow_promotion_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x89,0x7e,0x07,0x9e,0xa8,0x94,0xc8,0x9d,0xc4,0xdd,0xe1}}, "filespace_shadow_promotion_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::page_preallocation_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x8a,0x72,0x55,0x88,0xd4,0x69,0x29,0x0e,0x0e,0x95,0xee}}, "page_preallocation_policy"},
    CatalogAgentPolicyFamilyIdentity{CatalogAgentPolicyFamily::page_relocation_policy, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x8b,0x73,0x1b,0x9b,0xa7,0x19,0xa8,0x8b,0x6e,0x7f,0x85}}, "page_relocation_policy"},
}};
constexpr bool IsKnownCatalogAgentPolicyFamily(CatalogAgentPolicyFamily value) noexcept {
  for (const auto& r : kCatalogAgentPolicyFamilies)
    if (r.family == value) return true;
  return false;
}
constexpr std::optional<scratchbird::core::platform::Uuid> CatalogAgentPolicyFamilyIdentityOf(CatalogAgentPolicyFamily value) noexcept {
  for (const auto& r : kCatalogAgentPolicyFamilies)
    if (r.family == value) return r.uuid;
  return std::nullopt;
}
// Boundary adapter: the constant a text tag names; nullopt for one with no
// constant (refused by the caller, never defaulted).
constexpr std::optional<CatalogAgentPolicyFamily> CatalogAgentPolicyFamilyForTag(std::string_view tag) noexcept {
  for (const auto& r : kCatalogAgentPolicyFamilies)
    if (r.tag == tag) return r.family;
  return std::nullopt;
}

// metric families (426)
enum class CatalogMetricFamily : scratchbird::core::platform::u16 {
  none = 0,  // no listed value; never stored
  sb_index_candidates_total = 1,  // 01a1d1d7-748c-7529-b838-c19d29d5a2f4
  sb_index_visible_candidates_total = 2,  // 01a1d1d7-748d-7059-80e6-ab662e033713
  sb_index_rechecks_total = 3,  // 01a1d1d7-748e-7228-8d16-2c209eb00682
  sb_index_fallback_sorts_total = 4,  // 01a1d1d7-748f-7f3b-a484-d6e558bcbfde
  sb_index_pages_read_total = 5,  // 01a1d1d7-7490-7f39-be9a-523b86140a89
  sb_index_pages_written_total = 6,  // 01a1d1d7-7491-7569-83af-65ba8e8fe8e6
  sb_index_splits_observed_total = 7,  // 01a1d1d7-7492-728b-8af5-45494d782d02
  sb_index_merges_observed_total = 8,  // 01a1d1d7-7493-74aa-954d-66d74d17b62b
  sb_index_depth = 9,  // 01a1d1d7-7494-71ce-a451-e69b956e5e4e
  sb_index_density_ratio = 10,  // 01a1d1d7-7495-7127-a285-dfd2198530f7
  sb_index_fragmentation_ratio = 11,  // 01a1d1d7-7496-75ea-89b5-8b0df93712c0
  sb_index_maintenance_operations_total = 12,  // 01a1d1d7-7497-7a59-82c1-2f20d5a2b6c7
  sb_index_verify_failures_total = 13,  // 01a1d1d7-7498-7f99-b21c-5b54a36fce16
  sb_index_repair_actions_total = 14,  // 01a1d1d7-7499-74ad-924a-f23c60d52927
  sb_index_stale_resources_total = 15,  // 01a1d1d7-749a-77ff-9bb8-beaae83fdccd
  sb_index_quarantine_events_total = 16,  // 01a1d1d7-749b-7d59-a1ca-59f0b5bb90be
  sb_index_maintenance_progress_percent = 17,  // 01a1d1d7-749c-7701-8a8d-d2c4ffe22cf8
  sb_index_optimizer_estimate_error_ratio = 18,  // 01a1d1d7-749d-75e0-b0b3-e25d83d5144d
  sb_index_optimizer_stale_stats_total = 19,  // 01a1d1d7-749e-79cb-afbb-d012dfa0ada6
  sb_index_optimizer_invalidations_total = 20,  // 01a1d1d7-749f-7b83-95f6-66c443abb887
  sb_index_optimizer_fallback_refusals_total = 21,  // 01a1d1d7-74a0-7564-bcbf-73310903c53d
  sb_index_reference_profile_hits_total = 22,  // 01a1d1d7-74a1-7170-a98a-e98633ac722e
  sb_index_reference_profile_refusals_total = 23,  // 01a1d1d7-74a2-7774-acae-de7c13efe683
  sb_index_reference_rechecks_total = 24,  // 01a1d1d7-74a3-7172-9e33-762976422928
  sb_index_reference_fallback_sorts_total = 25,  // 01a1d1d7-74a4-71f0-9e98-f912cfa777a8
  sb_index_reference_order_proofs_total = 26,  // 01a1d1d7-74a5-7580-8a0e-7bf8579cf1c9
  sb_index_reference_catalog_projections_total = 27,  // 01a1d1d7-74a6-7516-bc26-344ef356f4f7
  sb_index_reference_compatibility_diagnostics_total = 28,  // 01a1d1d7-74a7-7027-9544-db1a92224966
  sb_index_resident_bytes = 29,  // 01a1d1d7-74a8-7c24-b89e-df4524f39f15
  sb_index_residency_hits_total = 30,  // 01a1d1d7-74a9-718a-99ff-2553e9d2c3d0
  sb_index_residency_misses_total = 31,  // 01a1d1d7-74aa-706f-af79-becba8a0df7f
  sb_index_residency_evictions_total = 32,  // 01a1d1d7-74ab-7578-a216-7abb2cc6249d
  sb_index_residency_pressure_score = 33,  // 01a1d1d7-74ac-7cc1-a402-a15eebf995df
  sb_index_residency_degraded_total = 34,  // 01a1d1d7-74ad-7237-a8b9-178260ef2fc5
  sb_index_residency_refused_total = 35,  // 01a1d1d7-74ae-7060-9566-92b40edc02c1
  sb_index_page_allocation_requests_total = 36,  // 01a1d1d7-74af-72b3-ae78-0920df185b8e
  sb_index_page_relocation_requests_total = 37,  // 01a1d1d7-74b0-7c6f-8ddc-d421b7540fb1
  sb_index_filespace_shrink_ready_bytes = 38,  // 01a1d1d7-74b1-7116-8826-9aceb9315eed
  sb_index_operation_probe_total = 39,  // 01a1d1d7-74b2-7a43-b941-c32c79991bb4
  sb_index_operation_insert_total = 40,  // 01a1d1d7-74b3-7c74-ba91-8a7a6216b270
  sb_index_operation_delete_total = 41,  // 01a1d1d7-74b4-761b-b98b-06910873d693
  sb_index_operation_split_total = 42,  // 01a1d1d7-74b5-7e6b-bc45-1bec01c2af98
  sb_index_operation_merge_total = 43,  // 01a1d1d7-74b6-720e-b036-898209306971
  sb_index_operation_collision_total = 44,  // 01a1d1d7-74b7-7c17-a52e-3d26542b3d6c
  sb_index_operation_recheck_total = 45,  // 01a1d1d7-74b8-7c8b-b1e4-e7ed4425c0c3
  sb_index_operation_false_positive_total = 46,  // 01a1d1d7-74b9-78a5-8030-592e59a4bb5b
  sb_index_operation_cleanup_total = 47,  // 01a1d1d7-74ba-73c1-bf6a-fcaca52ab55a
  sb_index_operation_corruption_total = 48,  // 01a1d1d7-74bb-78a7-a6c4-9504a2000f1c
  sb_index_operation_recovery_total = 49,  // 01a1d1d7-74bc-767c-a72d-4ee63ae9c1fa
  sb_index_operation_benchmark_total = 50,  // 01a1d1d7-74bd-748d-9f50-23d7568de3e0
  sb_optimizer_index_selectivity = 51,  // 01a1d1d7-74be-77b9-bada-90cbfc6eb6d2
  sb_optimizer_index_false_positive_ratio = 52,  // 01a1d1d7-74bf-714e-8461-f067bdf89e3f
  sb_optimizer_index_recheck_count = 53,  // 01a1d1d7-74c0-7ce6-b103-d4425c9a8b50
  sb_optimizer_index_backlog_entries = 54,  // 01a1d1d7-74c1-790e-b3d5-3b5b9b5aeb3a
  sb_optimizer_btree_depth = 55,  // 01a1d1d7-74c2-7144-bd51-8a29b3b9a365
  sb_optimizer_btree_leaf_pages = 56,  // 01a1d1d7-74c3-760d-b327-3f75d91e36ec
  sb_optimizer_hash_collision_depth = 57,  // 01a1d1d7-74c4-7ad5-ac4b-8c1e36b280cc
  sb_optimizer_hash_overflow_depth = 58,  // 01a1d1d7-74c5-7a43-9e03-cc83098b6b88
  sb_optimizer_bitmap_density = 59,  // 01a1d1d7-74c6-7087-a940-3df26643bd27
  sb_optimizer_bloom_observed_fpr = 60,  // 01a1d1d7-74c7-782c-85d4-199e07473afd
  sb_optimizer_zone_prune_selectivity = 61,  // 01a1d1d7-74c8-7ebf-8716-3c69875a1e33
  sb_optimizer_text_posting_length = 62,  // 01a1d1d7-74c9-7d5f-98e4-dd2e2501f441
  sb_optimizer_text_blockmax_skips = 63,  // 01a1d1d7-74ca-7bf2-a2c3-c6434a2d3609
  sb_optimizer_vector_recall_observed = 64,  // 01a1d1d7-74cb-765d-a045-9b66d8d107ff
  sb_optimizer_vector_rerank_count = 65,  // 01a1d1d7-74cc-7291-8f4d-81050f019974
  sb_optimizer_vector_tombstone_ratio = 66,  // 01a1d1d7-74cd-7c69-b3fd-ba529aa5c833
  sb_optimizer_graph_frontier_width = 67,  // 01a1d1d7-74ce-7b60-b9ab-4e7cfee9ba61
  sb_optimizer_graph_adjacency_degree = 68,  // 01a1d1d7-74cf-7c8f-9dbd-39c96740917f
  sb_optimizer_document_path_selectivity = 69,  // 01a1d1d7-74d0-7a57-a9d2-0be9ef46b244
  sb_cluster_decision_request_total = 70,  // 01a1d1d7-74d1-74e4-b12e-5e2abc72e8da
  sb_cluster_decision_proof_projection_total = 71,  // 01a1d1d7-74d2-70cb-b3d2-294937fe3fcc
  sb_cluster_route_publish_total = 72,  // 01a1d1d7-74d3-795c-90ce-f13c9d0b07b7
  sb_cluster_route_binding_state = 73,  // 01a1d1d7-74d4-7db9-8a9b-69cda176cd1c
  sb_cluster_fence_token_state = 74,  // 01a1d1d7-74d5-7db5-87e6-c654250fff81
  sb_cluster_shard_placement_state = 75,  // 01a1d1d7-74d6-7a67-b2ad-e4d35e68f099
  sb_cluster_replica_topology_generation = 76,  // 01a1d1d7-74d7-7179-90a0-9ef5a6e0b741
  sb_cluster_cleanup_low_water_lag_transactions = 77,  // 01a1d1d7-74d8-7dee-92d2-3ba66f2aad56
  sb_cluster_limbo_reconciliation_total = 78,  // 01a1d1d7-74d9-77ea-8731-bf3af94952a5
  sb_cluster_security_binding_state = 79,  // 01a1d1d7-74da-79a3-ba55-4274f561d628
  sb_cluster_metric_profile_binding_state = 80,  // 01a1d1d7-74db-755d-8235-06fc0224dfe2
  sb_cluster_authority_provenance_gap_total = 81,  // 01a1d1d7-74dc-7b0a-be57-5e9c0dfc87b7
  sb_cluster_provider_boundary_refusal_total = 82,  // 01a1d1d7-74dd-7c33-9a3f-1dbe9df98743
  sb_dml_insert_batch_started_total = 83,  // 01a1d1d7-74de-7e95-a61b-6ff21c03b74b
  sb_dml_insert_batch_fallback_total = 84,  // 01a1d1d7-74df-7dc1-9297-b639500316fa
  sb_dml_insert_batch_fallback_reason_total = 85,  // 01a1d1d7-74e0-7f94-a7c6-14fdd8c7c1a2
  sb_dml_insert_rows_inserted_total = 86,  // 01a1d1d7-74e1-7967-8db8-dafcca5fb857
  sb_dml_insert_rows_per_batch = 87,  // 01a1d1d7-74e2-79d5-88d6-8e2cad60eac0
  sb_dml_insert_row_template_cache_hit_total = 88,  // 01a1d1d7-74e3-7959-b5db-a2055f9fdd55
  sb_dml_insert_prepared_descriptor_cache_total = 89,  // 01a1d1d7-74e4-723b-a75c-b3a2ea96cd9c
  sb_dml_insert_relation_state_full_load_total = 90,  // 01a1d1d7-74e5-7768-a498-0ea1c89e2bcc
  sb_dml_insert_relation_state_scoped_load_total = 91,  // 01a1d1d7-74e6-7044-9539-58d840c2528d
  sb_dml_insert_adaptive_batch_requested_rows = 92,  // 01a1d1d7-74e7-7db8-b324-1cde144a97f4
  sb_dml_insert_adaptive_batch_admitted_rows = 93,  // 01a1d1d7-74e8-7a54-8964-ec10f6976bd9
  sb_dml_insert_adaptive_batch_admitted_bytes = 94,  // 01a1d1d7-74e9-7744-946a-87b4ead8afae
  sb_dml_insert_adaptive_batch_resize_total = 95,  // 01a1d1d7-74ea-7061-9f08-587307212ca8
  sb_dml_insert_allocation_stall_microseconds = 96,  // 01a1d1d7-74eb-7a18-85cb-7079a66f8519
  sb_dml_insert_slow_path_total = 97,  // 01a1d1d7-74ec-71a0-bf49-01ee44e87d22
  sb_dml_insert_identity_range_reserved_total = 98,  // 01a1d1d7-74ed-7e1d-91ce-0c8bca3da09a
  sb_page_insert_page_reuse_total = 99,  // 01a1d1d7-74ee-77fe-bd54-97b8576573d4
  sb_page_insert_page_full_retry_total = 100,  // 01a1d1d7-74ef-72d2-b471-525ed1cd6389
  sb_page_insert_page_split_total = 101,  // 01a1d1d7-74f0-7b8a-bf73-6b5b2af7f4cc
  sb_page_insert_preallocated_pages_total = 102,  // 01a1d1d7-74f1-70e7-aa9e-65794051ea04
  sb_page_allocation_reserve_low_total = 103,  // 01a1d1d7-74f2-7b90-84da-c4a44185798d
  sb_page_reservation_admitted_total = 104,  // 01a1d1d7-74f3-723f-8f20-ecf70b4bc570
  sb_page_reservation_refused_total = 105,  // 01a1d1d7-74f4-7608-8907-318589b1425f
  sb_page_reservation_expired_total = 106,  // 01a1d1d7-74f5-77db-97d8-3a9c3b6d8000
  sb_page_reservation_consumed_total = 107,  // 01a1d1d7-74f6-7040-aca4-3745188df11b
  sb_page_reservation_released_total = 108,  // 01a1d1d7-74f7-7411-b5e8-9e27f23d3af6
  sb_page_reservation_recovered_total = 109,  // 01a1d1d7-74f8-7a21-a745-e698d63660a4
  sb_index_insert_synchronous_total = 110,  // 01a1d1d7-74f9-74aa-a7ea-6b96932581b4
  sb_index_insert_delta_ledger_total = 111,  // 01a1d1d7-74fa-750c-aded-4f469ea3a6fb
  sb_index_insert_delta_merge_total = 112,  // 01a1d1d7-74fb-7ee4-a7fb-784b65facf31
  sb_index_insert_unique_preflight_hit_total = 113,  // 01a1d1d7-74fc-75bd-90b3-8fb5ed8c81cb
  sb_index_insert_unique_physical_probe_total = 114,  // 01a1d1d7-74fd-777a-bc0c-3b950f4d6448
  sb_index_insert_sorted_run_total = 115,  // 01a1d1d7-74fe-7ee3-8a83-675efe90ccb2
  sb_filespace_insert_growth_request_total = 116,  // 01a1d1d7-74ff-793f-bafa-8cf4f2d2c8c0
  sb_filespace_insert_growth_wait_microseconds = 117,  // 01a1d1d7-7500-7852-8d22-78a20bc1e18a
  sb_dml_insert_cancel_total = 118,  // 01a1d1d7-7501-7fb8-b47a-38ca6df533a9
  sb_dml_insert_trace_event_total = 119,  // 01a1d1d7-7502-72e5-96a1-b7d28558b0d2
  sb_dml_update_batch_started_total = 120,  // 01a1d1d7-7503-7be9-9541-8ec161846697
  sb_dml_update_batch_fallback_total = 121,  // 01a1d1d7-7504-7c2e-9cae-49f42b2cd203
  sb_dml_update_rows_updated_total = 122,  // 01a1d1d7-7505-72aa-aaee-0d139987fc18
  sb_dml_update_cancel_total = 123,  // 01a1d1d7-7506-767e-bd9b-3745d6ebddb4
  sb_dml_update_trace_event_total = 124,  // 01a1d1d7-7507-7f9e-aae6-ed179e7a0c9f
  sb_dml_delete_batch_started_total = 125,  // 01a1d1d7-7508-7ba8-a546-3e19a8ff8a67
  sb_dml_delete_batch_fallback_total = 126,  // 01a1d1d7-7509-73bd-b88f-c58f9345e885
  sb_dml_delete_rows_deleted_total = 127,  // 01a1d1d7-750a-762c-abb1-84f72d116612
  sb_dml_delete_cancel_total = 128,  // 01a1d1d7-750b-77e0-ba13-e00d669b9ff6
  sb_dml_delete_trace_event_total = 129,  // 01a1d1d7-750c-733a-9797-ed458de751a9
  sb_dml_merge_rows_total = 130,  // 01a1d1d7-750d-78c5-ae31-32657b21e2f9
  sb_mga_relation_state_load_total = 131,  // 01a1d1d7-750e-7297-9f35-bf6d26eb6b53
  sb_mga_relation_state_rows_materialized_total = 132,  // 01a1d1d7-750f-7200-99d8-168efb725e8b
  sb_mga_relation_state_bytes_materialized_total = 133,  // 01a1d1d7-7510-7b45-9e45-c7addc30076c
  sb_mga_relation_state_allocation_units_materialized_total = 134,  // 01a1d1d7-7511-7470-be3c-28d0f3639b96
  sb_memory_allocated_bytes = 135,  // 01a1d1d7-7512-79ca-8536-50e70508fd81
  sb_memory_allocation_latency_microseconds = 136,  // 01a1d1d7-7513-7d87-bdab-c7c6946cd539
  sb_memory_allocation_failures_total = 137,  // 01a1d1d7-7514-7005-af6d-cfa914a7412f
  sb_memory_emergency_reserve_bytes = 138,  // 01a1d1d7-7515-71e9-82fd-73f572506591
  sb_memory_background_reclamation_runs_total = 139,  // 01a1d1d7-7516-7255-91da-c36f2e05eaa6
  sb_memory_background_reclaimed_bytes_total = 140,  // 01a1d1d7-7517-7098-82a8-a2ccb0bf0c31
  sb_memory_background_reclamation_retained_items = 141,  // 01a1d1d7-7518-7801-9c74-355ee4c54297
  sb_llvm_foreign_memory_reservations_total = 142,  // 01a1d1d7-7519-77b1-9287-2c1eb56f6def
  sb_llvm_foreign_memory_reserved_bytes = 143,  // 01a1d1d7-751a-7471-b649-a7f99175318c
  sb_llvm_foreign_memory_refusals_total = 144,  // 01a1d1d7-751b-7af4-b418-eb32ecefb1ae
  sb_storage_device_read_latency_microseconds = 145,  // 01a1d1d7-751c-7048-9933-a3a28f2623c9
  sb_storage_device_write_latency_microseconds = 146,  // 01a1d1d7-751d-7562-8bd3-992444c31817
  sb_storage_fsync_latency_microseconds = 147,  // 01a1d1d7-751e-70f9-9d5f-ceb98a46e43b
  sb_storage_checksum_failures_total = 148,  // 01a1d1d7-751f-7a19-ba87-12f980d4d4e3
  sb_storage_unknown_pages_total = 149,  // 01a1d1d7-7520-7f57-a459-ccf2fd61889b
  sb_storage_device_errors_total = 150,  // 01a1d1d7-7521-7a42-888a-d1b64f1c471a
  sb_filespace_total_bytes = 151,  // 01a1d1d7-7522-7c27-9e23-a6bcc748e329
  sb_filespace_used_bytes = 152,  // 01a1d1d7-7523-7629-95ad-02a7fe530219
  sb_filespace_free_bytes = 153,  // 01a1d1d7-7524-76d1-9d6a-89164d8d0361
  sb_filespace_reserved_bytes = 154,  // 01a1d1d7-7525-7d85-8673-4d7d2a0e2b2d
  sb_filespace_expandable_bytes = 155,  // 01a1d1d7-7526-7183-ac85-101be3f45857
  sb_filespace_growth_rate_bytes_per_second = 156,  // 01a1d1d7-7527-7578-ab17-c12ed600ea2e
  sb_filespace_depletion_eta_seconds = 157,  // 01a1d1d7-7528-7a94-9e4c-044cbc933938
  sb_filespace_shrink_candidate_bytes = 158,  // 01a1d1d7-7529-7b76-a2f7-91a1973ae7f0
  sb_filespace_truncate_ready_bytes = 159,  // 01a1d1d7-752a-7055-a819-fde2a7834a14
  sb_filespace_pending_page_relocation_bytes = 160,  // 01a1d1d7-752b-7e9d-90f0-d294138a4ee5
  sb_filespace_shrink_blocker_count = 161,  // 01a1d1d7-752c-7b0d-8a20-a60626e770cd
  sb_filespace_device_read_latency_microseconds = 162,  // 01a1d1d7-752d-74b2-bdc2-8fcca070785c
  sb_filespace_device_write_latency_microseconds = 163,  // 01a1d1d7-752e-77ce-bd62-75bd376b09f5
  sb_filespace_fsync_latency_microseconds = 164,  // 01a1d1d7-752f-7863-bead-1cd824179b11
  sb_filespace_device_error_total = 165,  // 01a1d1d7-7530-752b-95dd-839596a84341
  sb_filespace_health_state = 166,  // 01a1d1d7-7531-7318-8eff-22aff5d4a171
  sb_filespace_role_state = 167,  // 01a1d1d7-7532-758f-b4ce-e908a561b5f7
  sb_storage_filespace_lifecycle_total = 168,  // 01a1d1d7-7533-79f0-a20e-47291c862b8c
  sb_storage_filespace_active_pins = 169,  // 01a1d1d7-7534-7c80-b17b-15b8a7c10010
  sb_cloud_filespace_operation_total = 170,  // 01a1d1d7-7535-738f-a291-bb6c268ae1d2
  sb_foreign_filespace_quarantine_total = 171,  // 01a1d1d7-7536-7276-994d-7bf13079cf05
  sb_filespace_move_request_total = 172,  // 01a1d1d7-7537-7071-a833-7ac23a7647f7
  sb_filespace_expand_request_total = 173,  // 01a1d1d7-7538-7d9b-b49b-40e32358d2a1
  sb_filespace_shrink_request_total = 174,  // 01a1d1d7-7539-76f0-87e2-6aa4dda212e1
  sb_storage_pressure_total = 175,  // 01a1d1d7-753a-7983-a9ee-91880aab536e
  sb_temp_workspace_bytes = 176,  // 01a1d1d7-753b-7926-bd0a-2d7111b98c67
  sb_index_build_workspace_bytes = 177,  // 01a1d1d7-753c-79b0-b7ed-ed7a458cbee4
  sb_storage_support_redaction_total = 178,  // 01a1d1d7-753d-796d-9647-5300ed70357c
  sb_page_free_count = 179,  // 01a1d1d7-753e-79c1-862c-92b56dd8799f
  sb_page_allocated_count = 180,  // 01a1d1d7-753f-75ae-b2c1-87a0fc762c14
  sb_page_released_free_count = 181,  // 01a1d1d7-7540-7f43-976d-ba84c49e7146
  sb_page_reserved_count = 182,  // 01a1d1d7-7541-7fad-aa32-f86d47771343
  sb_page_preallocated_count = 183,  // 01a1d1d7-7542-7585-83a0-f0e7cdcf3e74
  sb_page_preallocation_target_count = 184,  // 01a1d1d7-7543-7215-84fc-4da6b7bf7b88
  sb_page_preallocation_deficit_count = 185,  // 01a1d1d7-7544-7c94-9f71-f8ace3b53c61
  sb_page_allocation_latency_microseconds = 186,  // 01a1d1d7-7545-7ce7-bd2a-657a887c74df
  sb_page_allocation_failures_total = 187,  // 01a1d1d7-7546-77c4-b737-efaf26899964
  sb_page_cache_resident_pages = 188,  // 01a1d1d7-7547-73c7-88f1-a294a76d79a2
  sb_page_cache_resident_bytes = 189,  // 01a1d1d7-7548-7720-92f0-41f867ea3fd3
  sb_page_cache_pinned_pages = 190,  // 01a1d1d7-7549-750f-9b8f-bb137479f59f
  sb_page_cache_dirty_pages = 191,  // 01a1d1d7-754a-72b0-a1fd-c4ffaf6f5fec
  sb_page_cache_evictions_total = 192,  // 01a1d1d7-754b-76e5-b7fb-609379b556cb
  sb_page_fragmentation_ratio = 193,  // 01a1d1d7-754c-7f4b-905a-5073a67aac44
  sb_page_relocation_backlog_count = 194,  // 01a1d1d7-754d-78f9-9a13-7fc8c876f9ea
  sb_page_relocation_latency_microseconds = 195,  // 01a1d1d7-754e-7665-86be-efd95401d247
  sb_page_relocation_blocked_total = 196,  // 01a1d1d7-754f-7ed6-970e-c2ac9fa74c4c
  sb_page_relocatable_bytes = 197,  // 01a1d1d7-7550-7ebf-ab50-87381c093c67
  sb_page_unmovable_bytes = 198,  // 01a1d1d7-7551-7748-b918-5fd6ee347359
  sb_page_family_growth_rate_pages_per_second = 199,  // 01a1d1d7-7552-752d-9a7e-07d2af48cbb1
  sb_page_family_depletion_eta_seconds = 200,  // 01a1d1d7-7553-7ce1-9393-c5d170cd1f5d
  sb_page_relocation_ready_for_filespace_shrink = 201,  // 01a1d1d7-7554-7323-a96a-7b907343f055
  sb_index_lookup_latency_microseconds = 202,  // 01a1d1d7-7555-7604-9c86-2308e8a820d5
  sb_index_splits_total = 203,  // 01a1d1d7-7556-717f-b441-d06b9256bdde
  sb_index_read_amplification_ratio = 204,  // 01a1d1d7-7557-7c60-91d8-6ce401db9938
  sb_datatype_operation_total = 205,  // 01a1d1d7-7558-7e7e-8f43-ae70b52681af
  sb_datatype_cast_total = 206,  // 01a1d1d7-7559-7ea1-add5-51f090d4ce3b
  sb_datatype_numeric_backend_total = 207,  // 01a1d1d7-755a-7923-8e7a-eb650712ce58
  sb_datatype_catalog_descriptors = 208,  // 01a1d1d7-755b-7069-a457-8e5247b9354f
  sb_datatype_physical_encoding_total = 209,  // 01a1d1d7-755c-7909-9eaa-334811fa5f14
  sb_datatype_chunk_event_total = 210,  // 01a1d1d7-755d-7b15-8e09-2f148a077017
  sb_datatype_comparison_total = 211,  // 01a1d1d7-755e-786a-89a0-2305773dddff
  sb_datatype_locator_event_total = 212,  // 01a1d1d7-755f-7393-8077-e879882d0a9a
  sb_datatype_redaction_total = 213,  // 01a1d1d7-7560-7f0a-9ec4-9f93af097221
  sb_domain_method_invocation_total = 214,  // 01a1d1d7-7561-70c2-b86e-29668455f9e6
  sb_optimizer_plan_estimate_error_ratio = 215,  // 01a1d1d7-7562-7819-9794-8f85cec9498c
  sb_optimizer_feedback_estimated_rows = 216,  // 01a1d1d7-7563-7166-9e0c-7e2d0dae3c6f
  sb_optimizer_feedback_actual_rows = 217,  // 01a1d1d7-7564-7d08-8c37-9956b5d1b6a9
  sb_optimizer_feedback_estimated_pages = 218,  // 01a1d1d7-7565-7d4b-abb7-a8140adfb9a4
  sb_optimizer_feedback_actual_pages = 219,  // 01a1d1d7-7566-7d9d-b812-8492bd566737
  sb_optimizer_feedback_estimated_io_operations = 220,  // 01a1d1d7-7567-7851-b1fc-c6639cf261ed
  sb_optimizer_feedback_actual_io_operations = 221,  // 01a1d1d7-7568-781d-a1bc-bd851fd4f461
  sb_optimizer_feedback_estimated_visibility_recheck_rows = 222,  // 01a1d1d7-7569-7899-bccc-94e615a4de39
  sb_optimizer_feedback_actual_visibility_recheck_rows = 223,  // 01a1d1d7-756a-7427-a824-52b3f82f8555
  sb_optimizer_feedback_estimated_spill_bytes = 224,  // 01a1d1d7-756b-7154-b5c0-1517bc96d4e3
  sb_optimizer_feedback_actual_spill_bytes = 225,  // 01a1d1d7-756c-70c3-bbc6-e929725720da
  sb_optimizer_feedback_memory_grant_bytes = 226,  // 01a1d1d7-756d-7e7e-b8cf-911163c6df9d
  sb_optimizer_feedback_peak_memory_bytes = 227,  // 01a1d1d7-756e-7110-a5b2-e006ba46cc5a
  sb_optimizer_feedback_recommended_memory_grant_bytes = 228,  // 01a1d1d7-756f-7c11-a711-8d8a2c249735
  sb_optimizer_feedback_estimated_latency_microseconds = 229,  // 01a1d1d7-7570-7749-811a-b8b3b12485d4
  sb_optimizer_feedback_actual_latency_microseconds = 230,  // 01a1d1d7-7571-7e9d-9bda-f1360a4d8f7a
  sb_optimizer_feedback_estimated_resource_units = 231,  // 01a1d1d7-7572-7c00-8007-ca6916b0254d
  sb_optimizer_feedback_actual_resource_units = 232,  // 01a1d1d7-7573-7133-89da-155140f4ba8a
  sb_lock_latch_contention_wait_total = 233,  // 01a1d1d7-7574-7c67-a68d-02f8f8c36f60
  sb_lock_latch_contention_wait_microseconds = 234,  // 01a1d1d7-7575-74a1-9775-1cd7df5cb418
  sb_native_compile_jit_compile_total = 235,  // 01a1d1d7-7576-74f3-b290-a3ea752baaae
  sb_native_compile_jit_compile_seconds = 236,  // 01a1d1d7-7577-79d2-a27d-ac45173eeb9f
  sb_native_compile_jit_cache_hit_total = 237,  // 01a1d1d7-7578-742d-833b-49af96b40b2c
  sb_native_compile_fallback_total = 238,  // 01a1d1d7-7579-72f5-8617-d116ef8ec8cb
  sb_native_compile_aot_load_total = 239,  // 01a1d1d7-757a-7f55-bf3f-4989c6d1425b
  sb_native_compile_aot_invalid_total = 240,  // 01a1d1d7-757b-7aad-9549-0da41a910c7b
  sb_native_compile_execution_fault_total = 241,  // 01a1d1d7-757c-7674-a7c1-57a574fbd3d4
  sb_native_compile_invalidation_total = 242,  // 01a1d1d7-757d-7f10-91be-06879297e6cd
  sb_native_compile_equivalence_failure_total = 243,  // 01a1d1d7-757e-7718-9abf-707a1f3549e1
  sb_native_compile_quarantine_total = 244,  // 01a1d1d7-757f-7650-ad5b-f57f96567451
  sb_gpu_device_available = 245,  // 01a1d1d7-7580-7bac-a264-c57ba4791524
  sb_gpu_kernel_compile_total = 246,  // 01a1d1d7-7581-7268-be9b-c51732dea728
  sb_gpu_kernel_compile_seconds = 247,  // 01a1d1d7-7582-72a7-873f-4d264e74c7f4
  sb_gpu_artifact_load_total = 248,  // 01a1d1d7-7583-7067-8d13-0a12c0036b69
  sb_gpu_artifact_invalid_total = 249,  // 01a1d1d7-7584-79ae-a4e4-f60687a713d0
  sb_gpu_execution_total = 250,  // 01a1d1d7-7585-71c5-aa17-d81db1a32077
  sb_gpu_execution_seconds = 251,  // 01a1d1d7-7586-72ac-a049-0eab16d2fcc8
  sb_gpu_fallback_total = 252,  // 01a1d1d7-7587-7198-9650-d9da4d00299a
  sb_gpu_runtime_fault_total = 253,  // 01a1d1d7-7588-7ab1-a758-02a4a1906b9b
  sb_gpu_quarantine_total = 254,  // 01a1d1d7-7589-729a-88e1-7b3ce4e884fa
  sb_gpu_memory_bytes = 255,  // 01a1d1d7-758a-7fa6-bcd3-24672d057cbc
  sb_gpu_transfer_bytes_total = 256,  // 01a1d1d7-758b-776c-8826-bfb8a7e1f018
  sb_gpu_cluster_placement_refused_total = 257,  // 01a1d1d7-758c-7cb0-abe4-e46725ab4fa9
  sb_udr_registration_total = 258,  // 01a1d1d7-758d-7370-acd3-090a45306340
  sb_udr_load_total = 259,  // 01a1d1d7-758e-779f-be1e-107764c41a85
  sb_udr_unload_total = 260,  // 01a1d1d7-758f-7bca-ae73-c12d4ea4c4dc
  sb_udr_inspect_total = 261,  // 01a1d1d7-7590-7101-833b-5452322f19b9
  sb_udr_invocation_total = 262,  // 01a1d1d7-7591-7402-93f9-974fa145c635
  sb_udr_resource_refused_total = 263,  // 01a1d1d7-7592-77ad-92c8-0cc1faca0b22
  sb_udr_non_cpp_refusal_total = 264,  // 01a1d1d7-7593-73d1-82d8-0f69ff30520b
  sb_security_deep_enforcement_total = 265,  // 01a1d1d7-7594-7f76-91b0-a6a40159969c
  sb_security_deep_refusal_total = 266,  // 01a1d1d7-7595-7ad9-8c98-c59e24e88dee
  sb_security_audit_before_success_total = 267,  // 01a1d1d7-7596-7f30-937c-0870d3b6728b
  sb_security_masking_applied_total = 268,  // 01a1d1d7-7597-770d-b798-667748f58505
  sb_security_rls_filter_total = 269,  // 01a1d1d7-7598-76bf-ac69-1825c0fa8325
  sb_optimizer_remote_fragment_latency_microseconds = 270,  // 01a1d1d7-7599-7bd4-a42e-fae6f84888cd
  sb_query_fragment_queue_delay_microseconds = 271,  // 01a1d1d7-759a-76c5-a1b8-1cd00a945d54
  sb_query_fragment_propagation_delay_microseconds = 272,  // 01a1d1d7-759b-70ea-8de6-5cb90ddca6bb
  sb_query_fragment_local_connection_delay_microseconds = 273,  // 01a1d1d7-759c-786c-a4de-57f80c63aa0f
  sb_query_fragment_sample_freshness_microseconds = 274,  // 01a1d1d7-759d-765e-a540-282d59b82dcb
  sb_parser_sessions_active = 275,  // 01a1d1d7-759e-7000-b97c-ce7b1d8d3d1f
  sb_parser_failures_total = 276,  // 01a1d1d7-759f-7dc3-9462-ee418bc4a80a
  sb_parser_crashes_total = 277,  // 01a1d1d7-75a0-7eb6-a60a-0a3e32f366d8
  sb_parser_policy_attach_latency_microseconds = 278,  // 01a1d1d7-75a1-764d-93c4-44c18123078a
  sb_listener_sessions_active = 279,  // 01a1d1d7-75a2-7353-9f84-51f07715e3ce
  sb_listener_rejects_total = 280,  // 01a1d1d7-75a3-770c-804f-9f27e38ca9d8
  sb_listener_queue_depth = 281,  // 01a1d1d7-75a4-7e38-b153-3468a40787cd
  sb_listener_queue_delay_microseconds = 282,  // 01a1d1d7-75a5-7f71-bbb9-c908c42beb2a
  sb_management_frontend_requests_total = 283,  // 01a1d1d7-75a6-7db7-ba66-d61dac00137b
  sb_management_frontend_latency_microseconds = 284,  // 01a1d1d7-75a7-76a2-a773-e88d81253a23
  sb_tx_begin_total = 285,  // 01a1d1d7-75a8-7b2f-bbac-f8e6778deb14
  sb_tx_commit_total = 286,  // 01a1d1d7-75a9-7d16-94ed-05b232b4d2c1
  sb_tx_rollback_total = 287,  // 01a1d1d7-75aa-79ef-9575-c037dc81c566
  sb_tx_abort_total = 288,  // 01a1d1d7-75ab-7b62-bea9-5d2505795bb5
  sb_tx_failure_total = 289,  // 01a1d1d7-75ac-7888-90a5-3f488c1ccb91
  sb_tx_cluster_fail_closed_total = 290,  // 01a1d1d7-75ad-74b2-b733-396d9ba38a7e
  sb_tx_active_transactions = 291,  // 01a1d1d7-75ae-75d6-905f-f8cdb3d26577
  sb_tx_oldest_snapshot_age_microseconds = 292,  // 01a1d1d7-75af-7e27-9ad1-8851bdde297d
  sb_tx_runtime_policy_violation_total = 293,  // 01a1d1d7-75b0-798c-91ca-b3b810230e8c
  sb_tx_lock_decisions_total = 294,  // 01a1d1d7-75b1-7bc1-ac77-74f77ad3622c
  sb_tx_locks_held = 295,  // 01a1d1d7-75b2-7d54-be29-3f5d0d9d190b
  sb_tx_lock_waiters = 296,  // 01a1d1d7-75b3-7395-9e31-901d84672b61
  sb_tx_savepoint_operations_total = 297,  // 01a1d1d7-75b4-7847-bfd2-e7bbd90ac0de
  sb_mga_row_versions_reclaimed_total = 298,  // 01a1d1d7-75b5-79ef-931d-537f6a15f37d
  sb_mga_cleanup_horizon_local_transaction_id = 299,  // 01a1d1d7-75b6-7f31-994e-2eed16e96813
  sb_mga_cleanup_blocked_total = 300,  // 01a1d1d7-75b7-7bdc-a8da-4b6527fbff67
  sb_mga_cleanup_retained_row_versions = 301,  // 01a1d1d7-75b8-7a60-866b-240876fbda34
  sb_mga_cleanup_retention_age_microseconds = 302,  // 01a1d1d7-75b9-7595-8abf-b68d13217f7f
  sb_mga_temporary_on_commit_delete_rows_total = 303,  // 01a1d1d7-75ba-79d3-bdac-4f480bc2aaa3
  sb_archive_lag_bytes = 304,  // 01a1d1d7-75bb-7589-a17e-4413f45f454c
  sb_archive_slice_count = 305,  // 01a1d1d7-75bc-758c-9117-04c46171bf5e
  sb_archive_slice_bytes = 306,  // 01a1d1d7-75bd-7a76-9517-2e30ba5b54d8
  sb_archive_slice_age_microseconds = 307,  // 01a1d1d7-75be-76fe-a773-bb75a9bedcca
  sb_archive_slice_max_age_microseconds = 308,  // 01a1d1d7-75bf-7ff2-9afc-dcf525545f60
  sb_archive_health_state = 309,  // 01a1d1d7-75c0-7541-b5db-cb792f53ec25
  sb_archive_queue_depth = 310,  // 01a1d1d7-75c1-7437-9615-804cb25e238f
  sb_archive_delta_lag_transactions = 311,  // 01a1d1d7-75c2-7bb7-ac14-278f7aa983d0
  sb_archive_delta_apply_lag_transactions = 312,  // 01a1d1d7-75c3-7671-bd9b-4ad42ffea8b3
  sb_archive_checksum_failures_total = 313,  // 01a1d1d7-75c4-7d2b-88c0-b8418f7a457c
  sb_archive_restore_refusals_total = 314,  // 01a1d1d7-75c5-7c4b-b5b3-eef8cf5851bd
  sb_backup_in_progress = 315,  // 01a1d1d7-75c6-7e31-89ad-04c821bfcf08
  sb_backup_progress_percent = 316,  // 01a1d1d7-75c7-7eec-ba7d-27aa062e40fb
  sb_restore_drill_duration_microseconds = 317,  // 01a1d1d7-75c8-7791-a375-15b7fb5cc53b
  sb_pitr_window_available_seconds = 318,  // 01a1d1d7-75c9-7d58-80a0-583ec6dbc190
  sb_auth_failures_total = 319,  // 01a1d1d7-75ca-738f-8fc3-33485447b615
  sb_identity_auth_attempts_total = 320,  // 01a1d1d7-75cb-7d0f-83d3-28f5190e2d38
  sb_identity_sessions_active = 321,  // 01a1d1d7-75cc-7574-be5b-302057c8e926
  sb_identity_users_online = 322,  // 01a1d1d7-75cd-79f8-a31d-5a68e5014b73
  sb_policy_evaluations_total = 323,  // 01a1d1d7-75ce-76f6-8d70-28c5a8ea56d6
  sb_agent_actions_total = 324,  // 01a1d1d7-75cf-755f-949c-97e65f06a80c
  sb_agent_decision_latency_microseconds = 325,  // 01a1d1d7-75d0-7d54-90b0-3ea80e8dc2ca
  sb_agent_runtime_service_events_total = 326,  // 01a1d1d7-75d1-7b9f-9fe3-dcc950daa1ff
  sb_agent_runtime_service_leases = 327,  // 01a1d1d7-75d2-7ae0-a776-24d0aef19f27
  sb_agent_runtime_service_actions = 328,  // 01a1d1d7-75d3-7345-821a-984e25316583
  sb_agent_runtime_service_history_records = 329,  // 01a1d1d7-75d4-7449-b61c-99a6d8269236
  sb_agent_runtime_service_catalog_generation = 330,  // 01a1d1d7-75d5-711d-b191-01f55643e8c8
  sb_agent_filespace_capacity_requests_total = 331,  // 01a1d1d7-75d6-74d0-97a0-3a8b8866476f
  sb_agent_filespace_free_reserve_pages = 332,  // 01a1d1d7-75d7-7ee3-a431-8b464e802616
  sb_agent_filespace_decision_latency_microseconds = 333,  // 01a1d1d7-75d8-7cfb-a09d-3c05fff306e5
  sb_agent_page_allocation_requests_total = 334,  // 01a1d1d7-75d9-71c2-8c0a-6d903b65ee34
  sb_agent_page_allocation_preallocated_pages = 335,  // 01a1d1d7-75da-7337-8fd2-497d0c3ed7de
  sb_agent_page_allocation_relocated_pages_total = 336,  // 01a1d1d7-75db-7c0c-87d8-05309cc9ba38
  sb_alerts_fired_total = 337,  // 01a1d1d7-75dc-790d-966a-a34441f0a1bf
  sb_cluster_node_cpu_feature_available = 338,  // 01a1d1d7-75dd-794f-a5df-d80862da2c06
  sb_cluster_node_role_state = 339,  // 01a1d1d7-75de-7268-b4a8-b63d9efed823
  sb_cluster_node_saturation_ratio = 340,  // 01a1d1d7-75df-7cb7-af9b-802f2db5a8b1
  sb_cluster_admission_denied_total = 341,  // 01a1d1d7-75e0-70f3-837f-c2a68aa02528
  sb_cluster_limbo_transactions = 342,  // 01a1d1d7-75e1-71c6-9714-a6f578333adf
  sb_cluster_rolling_upgrade_readiness_state = 343,  // 01a1d1d7-75e2-7e56-a070-04c955035831
  sb_cluster_scheduler_queue_depth = 344,  // 01a1d1d7-75e3-78fa-b4ba-07ff0bc0a30e
  sb_cluster_insert_route_checks_total = 345,  // 01a1d1d7-75e4-70c8-87fc-4361f5645aae
  sb_cluster_insert_stale_route_rejections_total = 346,  // 01a1d1d7-75e5-70dc-99c7-b40097b6ef7b
  sb_cluster_insert_participant_admissions_total = 347,  // 01a1d1d7-75e6-7971-8b01-437fd3870d30
  sb_cluster_insert_remote_requests_total = 348,  // 01a1d1d7-75e7-7249-891d-9bb8aad4f795
  sb_cluster_insert_finality_wait_microseconds = 349,  // 01a1d1d7-75e8-728a-9943-daf3eeeb3919
  sb_cluster_insert_rows_mutated_total = 350,  // 01a1d1d7-75e9-7dee-9da9-fd920cefc0d9
  sb_cluster_insert_fail_closed_total = 351,  // 01a1d1d7-75ea-7d47-83b9-e9a033494d97
  sb_cluster_insert_bad_stats_suppressed_total = 352,  // 01a1d1d7-75eb-7603-b9a9-fd5aa34c2d35
  sb_event_channels = 353,  // 01a1d1d7-75ec-75cb-8a94-b17c864049ab
  sb_event_subscriptions_active = 354,  // 01a1d1d7-75ed-7541-952d-4661148d2e98
  sb_event_subscribe_attempts_total = 355,  // 01a1d1d7-75ee-7c6e-8879-36429d871de1
  sb_event_publish_attempts_total = 356,  // 01a1d1d7-75ef-771e-8ec3-824012ed6baa
  sb_event_publications_committed_total = 357,  // 01a1d1d7-75f0-76c5-ab2e-0f2f79b6acc4
  sb_event_publications_rolled_back_total = 358,  // 01a1d1d7-75f1-7ffd-8429-b37d7882006e
  sb_event_queued = 359,  // 01a1d1d7-75f2-7eb0-9789-a6ea3e2f54df
  sb_event_queue_bytes = 360,  // 01a1d1d7-75f3-7655-8f8e-4abf5a03c1bf
  sb_event_delivered_total = 361,  // 01a1d1d7-75f4-7a1c-ba71-6b942877b7d9
  sb_event_delivery_latency_microseconds = 362,  // 01a1d1d7-75f5-7167-bbfa-d54addea2cfd
  sb_event_backpressure_total = 363,  // 01a1d1d7-75f6-7733-8ff7-f6cc4d40277a
  sb_event_redacted_total = 364,  // 01a1d1d7-75f7-7e20-9e5c-1abeabfe72e8
  sb_cluster_event_route_attempts_total = 365,  // 01a1d1d7-75f8-75aa-aae7-442b9aa561d4
  sb_query_fragment_route_generation = 366,  // 01a1d1d7-75f9-72e9-bd05-e777f057e505
  sb_scheduler_queue_depth = 367,  // 01a1d1d7-75fa-7297-a0ec-5a00b08556aa
  sb_job_control_actions_total = 368,  // 01a1d1d7-75fb-753b-aa0d-5a186dda37ef
  sb_support_bundle_completeness_ratio = 369,  // 01a1d1d7-75fc-729a-85d9-ae3bef18cddd
  sb_export_adapter_queue_depth = 370,  // 01a1d1d7-75fd-734a-814d-1491e5238a3b
  sb_export_adapter_failures_total = 371,  // 01a1d1d7-75fe-741d-aeda-cabb7571c247
  sb_export_adapter_retries_total = 372,  // 01a1d1d7-75ff-7aad-ac6c-5b85fe1d063a
  sb_metric_samples_rejected_total = 373,  // 01a1d1d7-7600-70f5-9e23-df495c918905
  sb_metric_export_shed_total = 374,  // 01a1d1d7-7601-7f84-b3d4-cbf2e9d48f82
  sb_idle_capacity_ratio = 375,  // 01a1d1d7-7602-7766-8143-8ff075c8756b
  sb_workload_slo_burn_rate = 376,  // 01a1d1d7-7603-73d1-b14e-0e9e202ce031
  sb_mga_concurrency_latch_waits = 377,  // 01a1d1d7-7604-71c9-8bb8-ddae8593c7f1
  sb_mga_concurrency_latch_timeouts = 378,  // 01a1d1d7-7605-739a-8574-1ab3eca4b9f8
  sb_mga_concurrency_fail_safe_releases = 379,  // 01a1d1d7-7606-7ca0-a81c-17be5ff7f3d2
  sb_mga_concurrency_latch_wait_duration_us = 380,  // 01a1d1d7-7607-78bc-8ab8-9d8e0d3a0c6c
  sb_page_cache_context_resident_pages = 381,  // 01a1d1d7-7608-7824-8b61-2fbdd993ca84
  sb_page_cache_context_resident_bytes = 382,  // 01a1d1d7-7609-7074-aef9-023bcc78d6f3
  sb_page_cache_context_pinned_pages = 383,  // 01a1d1d7-760a-75e2-ba27-5b6951083e02
  sb_page_cache_context_dirty_pages = 384,  // 01a1d1d7-760b-7e95-aa14-373c00350b03
  sb_page_cache_context_admissions_total = 385,  // 01a1d1d7-760c-7fe6-ba8a-2877a99b0dce
  sb_page_cache_context_reuses_total = 386,  // 01a1d1d7-760d-7446-840e-770c00ce37d2
  sb_page_cache_context_evictions_total = 387,  // 01a1d1d7-760e-77d0-bb38-682822c3d743
  sb_page_cache_context_protected_normal_hot_skips_total = 388,  // 01a1d1d7-760f-7461-bb8f-0e1d8f24495f
  sb_page_cache_context_refusals_total = 389,  // 01a1d1d7-7610-7cbe-ab32-6da814f06afd
  sb_optimizer_operator_actual_rows = 390,  // 01a1d1d7-7611-7b7c-8ee4-1260b384dbba
  sb_optimizer_operator_rows_examined = 391,  // 01a1d1d7-7612-73d4-a1ed-0d694857b018
  sb_optimizer_operator_rows_filtered = 392,  // 01a1d1d7-7613-739d-b498-00130d0e6d40
  sb_optimizer_operator_loop_count = 393,  // 01a1d1d7-7614-7920-8a9d-98f92fb90851
  sb_optimizer_operator_cpu_time = 394,  // 01a1d1d7-7615-7791-a86f-c1bca46448a9
  sb_optimizer_spill_passes = 395,  // 01a1d1d7-7616-7513-a006-1d84ffadbbe1
  sb_lifecycle_operation_total = 396,  // 01a1d1d7-7617-7460-9a06-2e4248b1f624
  sb_lifecycle_diagnostic_total = 397,  // 01a1d1d7-7618-7193-a071-5665ef45c64a
  sb_lifecycle_audit_event_total = 398,  // 01a1d1d7-7619-791c-8506-de793fc211fa
  sb_lifecycle_cache_invalidation_total = 399,  // 01a1d1d7-761a-7be6-9141-f2196c9f3fe0
  sb_optimizer_route_plan_hash = 400,  // 01a1d1d7-761b-7339-9c5d-7919e3daeeb7
  sb_optimizer_route_result_hash = 401,  // 01a1d1d7-761c-7721-b22a-19a2c811bd21
  sb_optimizer_explain_digest = 402,  // 01a1d1d7-761d-7b01-b781-1f579d80102c
  sb_optimizer_route_equivalence_status = 403,  // 01a1d1d7-761e-7c8f-af0a-703b22c6aee5
  sb_optimizer_driver_visible_route_count = 404,  // 01a1d1d7-761f-7e50-a0bf-11a922692158
  sb_optimizer_candidate_set_cardinality = 405,  // 01a1d1d7-7620-741a-ac80-6896fc26cfa8
  sb_optimizer_candidate_set_density = 406,  // 01a1d1d7-7621-7c9f-be00-d5cc4a933c00
  sb_optimizer_candidate_set_recheck_ratio = 407,  // 01a1d1d7-7622-708e-82a0-259af169b33a
  sb_optimizer_specialized_exact_recheck_rows = 408,  // 01a1d1d7-7623-7acc-9c44-b0f3d5d7a167
  sb_optimizer_specialized_false_positive_ratio = 409,  // 01a1d1d7-7624-7e4c-b9e5-250d63086c41
  sb_optimizer_time_series_bucket_count = 410,  // 01a1d1d7-7625-7884-a82f-5a7181c1a4ef
  sb_optimizer_time_series_rollup_selectivity = 411,  // 01a1d1d7-7626-7f55-8edb-c55370f3e353
  sb_optimizer_page_cache_hit_miss = 412,  // 01a1d1d7-7627-7a22-adc1-e6d6062e514f
  sb_optimizer_prefetch_usefulness = 413,  // 01a1d1d7-7628-7686-a55d-9da5f1cf63ab
  sb_optimizer_page_count = 414,  // 01a1d1d7-7629-7816-b41c-96a3494c9e8c
  sb_optimizer_page_cache_dirty_pressure = 415,  // 01a1d1d7-762a-7c93-b47d-86b45d0033e5
  sb_optimizer_page_cache_pin_pressure = 416,  // 01a1d1d7-762b-7773-a867-8ac2e903b4b8
  sb_optimizer_writeback_pressure = 417,  // 01a1d1d7-762c-7752-a5c8-07aa09b1b0ce
  sb_optimizer_sequential_page_cost = 418,  // 01a1d1d7-762d-7ea5-8d0a-e66d5d1a9ff3
  sb_optimizer_random_page_cost = 419,  // 01a1d1d7-762e-7d2f-94e8-d47dc6dfdbf0
  sb_optimizer_filespace_pressure = 420,  // 01a1d1d7-762f-74a5-ab84-4c67dc58f55b
  sb_optimizer_mga_cleanup_debt = 421,  // 01a1d1d7-7630-7d87-b56e-59b23ab89c72
  sb_optimizer_mga_retained_dead_bytes = 422,  // 01a1d1d7-7631-7fdd-8f0a-659c5d50bb3b
  sb_optimizer_mga_chain_depth = 423,  // 01a1d1d7-7632-77b3-81b9-49d98a2f86a0
  sb_optimizer_mga_chain_scatter = 424,  // 01a1d1d7-7633-7a59-8cc6-fd9dcdac9f0f
  sb_optimizer_same_page_update_ratio = 425,  // 01a1d1d7-7634-78cd-80a3-dec6ee05173e
  sb_optimizer_commit_fence_pressure = 426,  // 01a1d1d7-7635-7d08-a904-945027ae120d
};
struct CatalogMetricFamilyIdentity {
  CatalogMetricFamily family;
  scratchbird::core::platform::Uuid uuid;
  std::string_view tag;  // boundary spelling only
};
inline constexpr std::array<CatalogMetricFamilyIdentity, 426> kCatalogMetricFamilies{{
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_candidates_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x8c,0x75,0x29,0xb8,0x38,0xc1,0x9d,0x29,0xd5,0xa2,0xf4}}, "sb_index_candidates_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_visible_candidates_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x8d,0x70,0x59,0x80,0xe6,0xab,0x66,0x2e,0x03,0x37,0x13}}, "sb_index_visible_candidates_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_rechecks_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x8e,0x72,0x28,0x8d,0x16,0x2c,0x20,0x9e,0xb0,0x06,0x82}}, "sb_index_rechecks_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_fallback_sorts_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x8f,0x7f,0x3b,0xa4,0x84,0xd6,0xe5,0x58,0xbc,0xbf,0xde}}, "sb_index_fallback_sorts_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_pages_read_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x90,0x7f,0x39,0xbe,0x9a,0x52,0x3b,0x86,0x14,0x0a,0x89}}, "sb_index_pages_read_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_pages_written_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x91,0x75,0x69,0x83,0xaf,0x65,0xba,0x8e,0x8f,0xe8,0xe6}}, "sb_index_pages_written_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_splits_observed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x92,0x72,0x8b,0x8a,0xf5,0x45,0x49,0x4d,0x78,0x2d,0x02}}, "sb_index_splits_observed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_merges_observed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x93,0x74,0xaa,0x95,0x4d,0x66,0xd7,0x4d,0x17,0xb6,0x2b}}, "sb_index_merges_observed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x94,0x71,0xce,0xa4,0x51,0xe6,0x9b,0x95,0x6e,0x5e,0x4e}}, "sb_index_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_density_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x95,0x71,0x27,0xa2,0x85,0xdf,0xd2,0x19,0x85,0x30,0xf7}}, "sb_index_density_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_fragmentation_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x96,0x75,0xea,0x89,0xb5,0x8b,0x0d,0xf9,0x37,0x12,0xc0}}, "sb_index_fragmentation_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_maintenance_operations_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x97,0x7a,0x59,0x82,0xc1,0x2f,0x20,0xd5,0xa2,0xb6,0xc7}}, "sb_index_maintenance_operations_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_verify_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x98,0x7f,0x99,0xb2,0x1c,0x5b,0x54,0xa3,0x6f,0xce,0x16}}, "sb_index_verify_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_repair_actions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x99,0x74,0xad,0x92,0x4a,0xf2,0x3c,0x60,0xd5,0x29,0x27}}, "sb_index_repair_actions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_stale_resources_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x9a,0x77,0xff,0x9b,0xb8,0xbe,0xaa,0xe8,0x3f,0xdc,0xcd}}, "sb_index_stale_resources_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_quarantine_events_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x9b,0x7d,0x59,0xa1,0xca,0x59,0xf0,0xb5,0xbb,0x90,0xbe}}, "sb_index_quarantine_events_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_maintenance_progress_percent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x9c,0x77,0x01,0x8a,0x8d,0xd2,0xc4,0xff,0xe2,0x2c,0xf8}}, "sb_index_maintenance_progress_percent"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_optimizer_estimate_error_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x9d,0x75,0xe0,0xb0,0xb3,0xe2,0x5d,0x83,0xd5,0x14,0x4d}}, "sb_index_optimizer_estimate_error_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_optimizer_stale_stats_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x9e,0x79,0xcb,0xaf,0xbb,0xd0,0x12,0xdf,0xa0,0xad,0xa6}}, "sb_index_optimizer_stale_stats_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_optimizer_invalidations_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0x9f,0x7b,0x83,0x95,0xf6,0x66,0xc4,0x43,0xab,0xb8,0x87}}, "sb_index_optimizer_invalidations_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_optimizer_fallback_refusals_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa0,0x75,0x64,0xbc,0xbf,0x73,0x31,0x09,0x03,0xc5,0x3d}}, "sb_index_optimizer_fallback_refusals_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_reference_profile_hits_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa1,0x71,0x70,0xa9,0x8a,0xe9,0x86,0x33,0xac,0x72,0x2e}}, "sb_index_reference_profile_hits_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_reference_profile_refusals_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa2,0x77,0x74,0xac,0xae,0xde,0x7c,0x13,0xef,0xe6,0x83}}, "sb_index_reference_profile_refusals_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_reference_rechecks_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa3,0x71,0x72,0x9e,0x33,0x76,0x29,0x76,0x42,0x29,0x28}}, "sb_index_reference_rechecks_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_reference_fallback_sorts_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa4,0x71,0xf0,0x9e,0x98,0xf9,0x12,0xcf,0xa7,0x77,0xa8}}, "sb_index_reference_fallback_sorts_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_reference_order_proofs_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa5,0x75,0x80,0x8a,0x0e,0x7b,0xf8,0x57,0x9c,0xf1,0xc9}}, "sb_index_reference_order_proofs_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_reference_catalog_projections_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa6,0x75,0x16,0xbc,0x26,0x34,0x4e,0xf3,0x56,0xf4,0xf7}}, "sb_index_reference_catalog_projections_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_reference_compatibility_diagnostics_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa7,0x70,0x27,0x95,0x44,0xdb,0x1a,0x92,0x22,0x49,0x66}}, "sb_index_reference_compatibility_diagnostics_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_resident_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa8,0x7c,0x24,0xb8,0x9e,0xdf,0x45,0x24,0xf3,0x9f,0x15}}, "sb_index_resident_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_residency_hits_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xa9,0x71,0x8a,0x99,0xff,0x25,0x53,0xe9,0xd2,0xc3,0xd0}}, "sb_index_residency_hits_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_residency_misses_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xaa,0x70,0x6f,0xaf,0x79,0xbe,0xcb,0xa8,0xa0,0xdf,0x7f}}, "sb_index_residency_misses_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_residency_evictions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xab,0x75,0x78,0xa2,0x16,0x7a,0xbb,0x2c,0xc6,0x24,0x9d}}, "sb_index_residency_evictions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_residency_pressure_score, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xac,0x7c,0xc1,0xa4,0x02,0xa1,0x5e,0xeb,0xf9,0x95,0xdf}}, "sb_index_residency_pressure_score"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_residency_degraded_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xad,0x72,0x37,0xa8,0xb9,0x17,0x82,0x60,0xef,0x2f,0xc5}}, "sb_index_residency_degraded_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_residency_refused_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xae,0x70,0x60,0x95,0x66,0x92,0xb4,0x0e,0xdc,0x02,0xc1}}, "sb_index_residency_refused_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_page_allocation_requests_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xaf,0x72,0xb3,0xae,0x78,0x09,0x20,0xdf,0x18,0x5b,0x8e}}, "sb_index_page_allocation_requests_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_page_relocation_requests_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb0,0x7c,0x6f,0x8d,0xdc,0xd4,0x21,0xb7,0x54,0x0f,0xb1}}, "sb_index_page_relocation_requests_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_filespace_shrink_ready_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb1,0x71,0x16,0x88,0x26,0x9a,0xce,0xb9,0x31,0x5e,0xed}}, "sb_index_filespace_shrink_ready_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_probe_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb2,0x7a,0x43,0xb9,0x41,0xc3,0x2c,0x79,0x99,0x1b,0xb4}}, "sb_index_operation_probe_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_insert_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb3,0x7c,0x74,0xba,0x91,0x8a,0x7a,0x62,0x16,0xb2,0x70}}, "sb_index_operation_insert_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_delete_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb4,0x76,0x1b,0xb9,0x8b,0x06,0x91,0x08,0x73,0xd6,0x93}}, "sb_index_operation_delete_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_split_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb5,0x7e,0x6b,0xbc,0x45,0x1b,0xec,0x01,0xc2,0xaf,0x98}}, "sb_index_operation_split_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_merge_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb6,0x72,0x0e,0xb0,0x36,0x89,0x82,0x09,0x30,0x69,0x71}}, "sb_index_operation_merge_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_collision_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb7,0x7c,0x17,0xa5,0x2e,0x3d,0x26,0x54,0x2b,0x3d,0x6c}}, "sb_index_operation_collision_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_recheck_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb8,0x7c,0x8b,0xb1,0xe4,0xe7,0xed,0x44,0x25,0xc0,0xc3}}, "sb_index_operation_recheck_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_false_positive_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xb9,0x78,0xa5,0x80,0x30,0x59,0x2e,0x59,0xa4,0xbb,0x5b}}, "sb_index_operation_false_positive_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_cleanup_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xba,0x73,0xc1,0xbf,0x6a,0xfc,0xac,0xa5,0x2a,0xb5,0x5a}}, "sb_index_operation_cleanup_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_corruption_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xbb,0x78,0xa7,0xa6,0xc4,0x95,0x04,0xa2,0x00,0x0f,0x1c}}, "sb_index_operation_corruption_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_recovery_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xbc,0x76,0x7c,0xa7,0x2d,0x4e,0xe6,0x3a,0xe9,0xc1,0xfa}}, "sb_index_operation_recovery_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_operation_benchmark_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xbd,0x74,0x8d,0x9f,0x50,0x23,0xd7,0x56,0x8d,0xe3,0xe0}}, "sb_index_operation_benchmark_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_index_selectivity, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xbe,0x77,0xb9,0xba,0xda,0x90,0xcb,0xfc,0x6e,0xb6,0xd2}}, "sb_optimizer_index_selectivity"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_index_false_positive_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xbf,0x71,0x4e,0x84,0x61,0xf0,0x67,0xbd,0xf8,0x9e,0x3f}}, "sb_optimizer_index_false_positive_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_index_recheck_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc0,0x7c,0xe6,0xb1,0x03,0xd4,0x42,0x5c,0x9a,0x8b,0x50}}, "sb_optimizer_index_recheck_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_index_backlog_entries, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc1,0x79,0x0e,0xb3,0xd5,0x3b,0x5b,0x9b,0x5a,0xeb,0x3a}}, "sb_optimizer_index_backlog_entries"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_btree_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc2,0x71,0x44,0xbd,0x51,0x8a,0x29,0xb3,0xb9,0xa3,0x65}}, "sb_optimizer_btree_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_btree_leaf_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc3,0x76,0x0d,0xb3,0x27,0x3f,0x75,0xd9,0x1e,0x36,0xec}}, "sb_optimizer_btree_leaf_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_hash_collision_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc4,0x7a,0xd5,0xac,0x4b,0x8c,0x1e,0x36,0xb2,0x80,0xcc}}, "sb_optimizer_hash_collision_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_hash_overflow_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc5,0x7a,0x43,0x9e,0x03,0xcc,0x83,0x09,0x8b,0x6b,0x88}}, "sb_optimizer_hash_overflow_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_bitmap_density, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc6,0x70,0x87,0xa9,0x40,0x3d,0xf2,0x66,0x43,0xbd,0x27}}, "sb_optimizer_bitmap_density"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_bloom_observed_fpr, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc7,0x78,0x2c,0x85,0xd4,0x19,0x9e,0x07,0x47,0x3a,0xfd}}, "sb_optimizer_bloom_observed_fpr"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_zone_prune_selectivity, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc8,0x7e,0xbf,0x87,0x16,0x3c,0x69,0x87,0x5a,0x1e,0x33}}, "sb_optimizer_zone_prune_selectivity"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_text_posting_length, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xc9,0x7d,0x5f,0x98,0xe4,0xdd,0x2e,0x25,0x01,0xf4,0x41}}, "sb_optimizer_text_posting_length"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_text_blockmax_skips, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xca,0x7b,0xf2,0xa2,0xc3,0xc6,0x43,0x4a,0x2d,0x36,0x09}}, "sb_optimizer_text_blockmax_skips"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_vector_recall_observed, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xcb,0x76,0x5d,0xa0,0x45,0x9b,0x66,0xd8,0xd1,0x07,0xff}}, "sb_optimizer_vector_recall_observed"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_vector_rerank_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xcc,0x72,0x91,0x8f,0x4d,0x81,0x05,0x0f,0x01,0x99,0x74}}, "sb_optimizer_vector_rerank_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_vector_tombstone_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xcd,0x7c,0x69,0xb3,0xfd,0xba,0x52,0x9a,0xa5,0xc8,0x33}}, "sb_optimizer_vector_tombstone_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_graph_frontier_width, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xce,0x7b,0x60,0xb9,0xab,0x4e,0x7c,0xfe,0xe9,0xba,0x61}}, "sb_optimizer_graph_frontier_width"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_graph_adjacency_degree, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xcf,0x7c,0x8f,0x9d,0xbd,0x39,0xc9,0x67,0x40,0x91,0x7f}}, "sb_optimizer_graph_adjacency_degree"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_document_path_selectivity, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd0,0x7a,0x57,0xa9,0xd2,0x0b,0xe9,0xef,0x46,0xb2,0x44}}, "sb_optimizer_document_path_selectivity"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_decision_request_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd1,0x74,0xe4,0xb1,0x2e,0x5e,0x2a,0xbc,0x72,0xe8,0xda}}, "sb_cluster_decision_request_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_decision_proof_projection_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd2,0x70,0xcb,0xb3,0xd2,0x29,0x49,0x37,0xfe,0x3f,0xcc}}, "sb_cluster_decision_proof_projection_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_route_publish_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd3,0x79,0x5c,0x90,0xce,0xf1,0x3c,0x9d,0x0b,0x07,0xb7}}, "sb_cluster_route_publish_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_route_binding_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd4,0x7d,0xb9,0x8a,0x9b,0x69,0xcd,0xa1,0x76,0xcd,0x1c}}, "sb_cluster_route_binding_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_fence_token_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd5,0x7d,0xb5,0x87,0xe6,0xc6,0x54,0x25,0x0f,0xff,0x81}}, "sb_cluster_fence_token_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_shard_placement_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd6,0x7a,0x67,0xb2,0xad,0xe4,0xd3,0x5e,0x68,0xf0,0x99}}, "sb_cluster_shard_placement_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_replica_topology_generation, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd7,0x71,0x79,0x90,0xa0,0x9e,0xf5,0xa6,0xe0,0xb7,0x41}}, "sb_cluster_replica_topology_generation"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_cleanup_low_water_lag_transactions, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd8,0x7d,0xee,0x92,0xd2,0x3b,0xa6,0x6f,0x2a,0xad,0x56}}, "sb_cluster_cleanup_low_water_lag_transactions"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_limbo_reconciliation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xd9,0x77,0xea,0x87,0x31,0xbf,0x3a,0xf9,0x49,0x52,0xa5}}, "sb_cluster_limbo_reconciliation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_security_binding_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xda,0x79,0xa3,0xba,0x55,0x42,0x74,0xf5,0x61,0xd6,0x28}}, "sb_cluster_security_binding_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_metric_profile_binding_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xdb,0x75,0x5d,0x82,0x35,0x06,0xfc,0x02,0x24,0xdf,0xe2}}, "sb_cluster_metric_profile_binding_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_authority_provenance_gap_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xdc,0x7b,0x0a,0xbe,0x57,0x5e,0x9c,0x0d,0xfc,0x87,0xb7}}, "sb_cluster_authority_provenance_gap_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_provider_boundary_refusal_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xdd,0x7c,0x33,0x9a,0x3f,0x1d,0xbe,0x9d,0xf9,0x87,0x43}}, "sb_cluster_provider_boundary_refusal_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_batch_started_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xde,0x7e,0x95,0xa6,0x1b,0x6f,0xf2,0x1c,0x03,0xb7,0x4b}}, "sb_dml_insert_batch_started_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_batch_fallback_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xdf,0x7d,0xc1,0x92,0x97,0xb6,0x39,0x50,0x03,0x16,0xfa}}, "sb_dml_insert_batch_fallback_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_batch_fallback_reason_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe0,0x7f,0x94,0xa7,0xc6,0x14,0xfd,0xd8,0xc7,0xc1,0xa2}}, "sb_dml_insert_batch_fallback_reason_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_rows_inserted_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe1,0x79,0x67,0x8d,0xb8,0xda,0xfc,0xca,0x5f,0xb8,0x57}}, "sb_dml_insert_rows_inserted_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_rows_per_batch, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe2,0x79,0xd5,0x88,0xd6,0x8e,0x2c,0xad,0x60,0xea,0xc0}}, "sb_dml_insert_rows_per_batch"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_row_template_cache_hit_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe3,0x79,0x59,0xb5,0xdb,0xa2,0x05,0x5f,0x9f,0xdd,0x55}}, "sb_dml_insert_row_template_cache_hit_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_prepared_descriptor_cache_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe4,0x72,0x3b,0xa7,0x5c,0xb3,0xa2,0xea,0x96,0xcd,0x9c}}, "sb_dml_insert_prepared_descriptor_cache_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_relation_state_full_load_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe5,0x77,0x68,0xa4,0x98,0x0e,0xa1,0xc8,0x9e,0x2b,0xcc}}, "sb_dml_insert_relation_state_full_load_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_relation_state_scoped_load_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe6,0x70,0x44,0x95,0x39,0x58,0xd8,0x40,0xc2,0x52,0x8d}}, "sb_dml_insert_relation_state_scoped_load_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_adaptive_batch_requested_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe7,0x7d,0xb8,0xb3,0x24,0x1c,0xde,0x14,0x4a,0x97,0xf4}}, "sb_dml_insert_adaptive_batch_requested_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_adaptive_batch_admitted_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe8,0x7a,0x54,0x89,0x64,0xec,0x10,0xf6,0x97,0x6b,0xd9}}, "sb_dml_insert_adaptive_batch_admitted_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_adaptive_batch_admitted_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xe9,0x77,0x44,0x94,0x6a,0x87,0xb4,0xea,0xd8,0xaf,0xae}}, "sb_dml_insert_adaptive_batch_admitted_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_adaptive_batch_resize_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xea,0x70,0x61,0x9f,0x08,0x58,0x73,0x07,0x21,0x2c,0xa8}}, "sb_dml_insert_adaptive_batch_resize_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_allocation_stall_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xeb,0x7a,0x18,0x85,0xcb,0x70,0x79,0xa6,0x6f,0x85,0x19}}, "sb_dml_insert_allocation_stall_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_slow_path_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xec,0x71,0xa0,0xbf,0x49,0x01,0xee,0x44,0xe8,0x7d,0x22}}, "sb_dml_insert_slow_path_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_identity_range_reserved_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xed,0x7e,0x1d,0x91,0xce,0x0c,0x8b,0xca,0x3d,0xa0,0x9a}}, "sb_dml_insert_identity_range_reserved_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_insert_page_reuse_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xee,0x77,0xfe,0xbd,0x54,0x97,0xb8,0x57,0x65,0x73,0xd4}}, "sb_page_insert_page_reuse_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_insert_page_full_retry_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xef,0x72,0xd2,0xb4,0x71,0x52,0x5e,0xd1,0xcd,0x63,0x89}}, "sb_page_insert_page_full_retry_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_insert_page_split_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf0,0x7b,0x8a,0xbf,0x73,0x6b,0x5b,0x2a,0xf7,0xf4,0xcc}}, "sb_page_insert_page_split_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_insert_preallocated_pages_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf1,0x70,0xe7,0xaa,0x9e,0x65,0x79,0x40,0x51,0xea,0x04}}, "sb_page_insert_preallocated_pages_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_allocation_reserve_low_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf2,0x7b,0x90,0x84,0xda,0xc4,0xa4,0x41,0x85,0x79,0x8d}}, "sb_page_allocation_reserve_low_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_reservation_admitted_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf3,0x72,0x3f,0x8f,0x20,0xec,0xf7,0x0b,0x4b,0xc5,0x70}}, "sb_page_reservation_admitted_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_reservation_refused_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf4,0x76,0x08,0x89,0x07,0x31,0x85,0x89,0xb1,0x42,0x5f}}, "sb_page_reservation_refused_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_reservation_expired_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf5,0x77,0xdb,0x97,0xd8,0x3a,0x9c,0x3b,0x6d,0x80,0x00}}, "sb_page_reservation_expired_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_reservation_consumed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf6,0x70,0x40,0xac,0xa4,0x37,0x45,0x18,0x8d,0xf1,0x1b}}, "sb_page_reservation_consumed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_reservation_released_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf7,0x74,0x11,0xb5,0xe8,0x9e,0x27,0xf2,0x3d,0x3a,0xf6}}, "sb_page_reservation_released_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_reservation_recovered_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf8,0x7a,0x21,0xa7,0x45,0xe6,0x98,0xd6,0x36,0x60,0xa4}}, "sb_page_reservation_recovered_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_insert_synchronous_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xf9,0x74,0xaa,0xa7,0xea,0x6b,0x96,0x93,0x25,0x81,0xb4}}, "sb_index_insert_synchronous_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_insert_delta_ledger_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xfa,0x75,0x0c,0xad,0xed,0x4f,0x46,0x9e,0xa3,0xa6,0xfb}}, "sb_index_insert_delta_ledger_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_insert_delta_merge_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xfb,0x7e,0xe4,0xa7,0xfb,0x78,0x4b,0x65,0xfa,0xcf,0x31}}, "sb_index_insert_delta_merge_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_insert_unique_preflight_hit_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xfc,0x75,0xbd,0x90,0xb3,0x8f,0xb5,0xed,0x8c,0x81,0xcb}}, "sb_index_insert_unique_preflight_hit_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_insert_unique_physical_probe_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xfd,0x77,0x7a,0xbc,0x0c,0x3b,0x95,0x0f,0x4d,0x64,0x48}}, "sb_index_insert_unique_physical_probe_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_insert_sorted_run_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xfe,0x7e,0xe3,0x8a,0x83,0x67,0x5e,0xfe,0x90,0xcc,0xb2}}, "sb_index_insert_sorted_run_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_insert_growth_request_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x74,0xff,0x79,0x3f,0xba,0xfa,0x8c,0xf4,0xf2,0xd2,0xc8,0xc0}}, "sb_filespace_insert_growth_request_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_insert_growth_wait_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x00,0x78,0x52,0x8d,0x22,0x78,0xa2,0x0b,0xc1,0xe1,0x8a}}, "sb_filespace_insert_growth_wait_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_cancel_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x01,0x7f,0xb8,0xb4,0x7a,0x38,0xca,0x6d,0xf5,0x33,0xa9}}, "sb_dml_insert_cancel_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_insert_trace_event_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x02,0x72,0xe5,0x96,0xa1,0xb7,0xd2,0x85,0x58,0xb0,0xd2}}, "sb_dml_insert_trace_event_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_update_batch_started_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x03,0x7b,0xe9,0x95,0x41,0x8e,0xc1,0x61,0x84,0x66,0x97}}, "sb_dml_update_batch_started_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_update_batch_fallback_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x04,0x7c,0x2e,0x9c,0xae,0x49,0xf4,0x2b,0x2c,0xd2,0x03}}, "sb_dml_update_batch_fallback_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_update_rows_updated_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x05,0x72,0xaa,0xaa,0xee,0x0d,0x13,0x99,0x87,0xfc,0x18}}, "sb_dml_update_rows_updated_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_update_cancel_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x06,0x76,0x7e,0xbd,0x9b,0x37,0x45,0xd6,0xeb,0xdd,0xb4}}, "sb_dml_update_cancel_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_update_trace_event_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x07,0x7f,0x9e,0xaa,0xe6,0xed,0x17,0x9e,0x7a,0x0c,0x9f}}, "sb_dml_update_trace_event_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_delete_batch_started_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x08,0x7b,0xa8,0xa5,0x46,0x3e,0x19,0xa8,0xff,0x8a,0x67}}, "sb_dml_delete_batch_started_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_delete_batch_fallback_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x09,0x73,0xbd,0xb8,0x8f,0xc5,0x8f,0x93,0x45,0xe8,0x85}}, "sb_dml_delete_batch_fallback_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_delete_rows_deleted_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x0a,0x76,0x2c,0xab,0xb1,0x84,0xf7,0x2d,0x11,0x66,0x12}}, "sb_dml_delete_rows_deleted_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_delete_cancel_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x0b,0x77,0xe0,0xba,0x13,0xe0,0x0d,0x66,0x9b,0x9f,0xf6}}, "sb_dml_delete_cancel_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_delete_trace_event_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x0c,0x73,0x3a,0x97,0x97,0xed,0x45,0x8d,0xe7,0x51,0xa9}}, "sb_dml_delete_trace_event_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_dml_merge_rows_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x0d,0x78,0xc5,0xae,0x31,0x32,0x65,0x7b,0x21,0xe2,0xf9}}, "sb_dml_merge_rows_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_relation_state_load_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x0e,0x72,0x97,0x9f,0x35,0xbf,0x6d,0x26,0xeb,0x6b,0x53}}, "sb_mga_relation_state_load_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_relation_state_rows_materialized_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x0f,0x72,0x00,0x99,0xd8,0x16,0x8e,0xfb,0x72,0x5e,0x8b}}, "sb_mga_relation_state_rows_materialized_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_relation_state_bytes_materialized_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x10,0x7b,0x45,0x9e,0x45,0xc7,0xad,0xdc,0x30,0x07,0x6c}}, "sb_mga_relation_state_bytes_materialized_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_relation_state_allocation_units_materialized_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x11,0x74,0x70,0xbe,0x3c,0x28,0xd0,0xf3,0x63,0x9b,0x96}}, "sb_mga_relation_state_allocation_units_materialized_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_memory_allocated_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x12,0x79,0xca,0x85,0x36,0x50,0xe7,0x05,0x08,0xfd,0x81}}, "sb_memory_allocated_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_memory_allocation_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x13,0x7d,0x87,0xbd,0xab,0xc7,0xc6,0x94,0x6c,0xd5,0x39}}, "sb_memory_allocation_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_memory_allocation_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x14,0x70,0x05,0xaf,0x6d,0xcf,0xa9,0x14,0xa7,0x41,0x2f}}, "sb_memory_allocation_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_memory_emergency_reserve_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x15,0x71,0xe9,0x82,0xfd,0x73,0xf5,0x72,0x50,0x65,0x91}}, "sb_memory_emergency_reserve_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_memory_background_reclamation_runs_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x16,0x72,0x55,0x91,0xda,0xc3,0x6f,0x2e,0x05,0xea,0xa6}}, "sb_memory_background_reclamation_runs_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_memory_background_reclaimed_bytes_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x17,0x70,0x98,0x82,0xa8,0xa2,0xcc,0xb0,0xbf,0x0c,0x31}}, "sb_memory_background_reclaimed_bytes_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_memory_background_reclamation_retained_items, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x18,0x78,0x01,0x9c,0x74,0x35,0x5e,0xe4,0xc5,0x42,0x97}}, "sb_memory_background_reclamation_retained_items"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_llvm_foreign_memory_reservations_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x19,0x77,0xb1,0x92,0x87,0x2c,0x1e,0xb5,0x6f,0x6d,0xef}}, "sb_llvm_foreign_memory_reservations_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_llvm_foreign_memory_reserved_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x1a,0x74,0x71,0xb6,0x49,0xa7,0xf9,0x91,0x75,0x31,0x8c}}, "sb_llvm_foreign_memory_reserved_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_llvm_foreign_memory_refusals_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x1b,0x7a,0xf4,0xb4,0x18,0xeb,0x32,0xec,0xef,0xb1,0xae}}, "sb_llvm_foreign_memory_refusals_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_device_read_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x1c,0x70,0x48,0x99,0x33,0xa3,0xa2,0x8f,0x26,0x23,0xc9}}, "sb_storage_device_read_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_device_write_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x1d,0x75,0x62,0x8b,0xd3,0x99,0x24,0x44,0xc3,0x18,0x17}}, "sb_storage_device_write_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_fsync_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x1e,0x70,0xf9,0x9d,0x5f,0xce,0xb9,0x8a,0x46,0xe4,0x3b}}, "sb_storage_fsync_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_checksum_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x1f,0x7a,0x19,0xba,0x87,0x12,0xf9,0x80,0xd4,0xd4,0xe3}}, "sb_storage_checksum_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_unknown_pages_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x20,0x7f,0x57,0xa4,0x59,0xcc,0xf2,0xfd,0x61,0x88,0x9b}}, "sb_storage_unknown_pages_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_device_errors_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x21,0x7a,0x42,0x88,0x8a,0xd1,0xb6,0x4f,0x1c,0x47,0x1a}}, "sb_storage_device_errors_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_total_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x22,0x7c,0x27,0x9e,0x23,0xa6,0xbc,0xc7,0x48,0xe3,0x29}}, "sb_filespace_total_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_used_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x23,0x76,0x29,0x95,0xad,0x02,0xa7,0xfe,0x53,0x02,0x19}}, "sb_filespace_used_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_free_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x24,0x76,0xd1,0x9d,0x6a,0x89,0x16,0x4d,0x8d,0x03,0x61}}, "sb_filespace_free_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_reserved_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x25,0x7d,0x85,0x86,0x73,0x4d,0x7d,0x2a,0x0e,0x2b,0x2d}}, "sb_filespace_reserved_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_expandable_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x26,0x71,0x83,0xac,0x85,0x10,0x1b,0xe3,0xf4,0x58,0x57}}, "sb_filespace_expandable_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_growth_rate_bytes_per_second, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x27,0x75,0x78,0xab,0x17,0xc1,0x2e,0xd6,0x00,0xea,0x2e}}, "sb_filespace_growth_rate_bytes_per_second"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_depletion_eta_seconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x28,0x7a,0x94,0x9e,0x4c,0x04,0x4c,0xbc,0x93,0x39,0x38}}, "sb_filespace_depletion_eta_seconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_shrink_candidate_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x29,0x7b,0x76,0xa2,0xf7,0x91,0xa1,0x97,0x3a,0xe7,0xf0}}, "sb_filespace_shrink_candidate_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_truncate_ready_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x2a,0x70,0x55,0xa8,0x19,0xfd,0xe2,0xa7,0x83,0x4a,0x14}}, "sb_filespace_truncate_ready_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_pending_page_relocation_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x2b,0x7e,0x9d,0x90,0xf0,0xd2,0x94,0x13,0x8a,0x4e,0xe5}}, "sb_filespace_pending_page_relocation_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_shrink_blocker_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x2c,0x7b,0x0d,0x8a,0x20,0xa6,0x06,0x26,0xe7,0x70,0xcd}}, "sb_filespace_shrink_blocker_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_device_read_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x2d,0x74,0xb2,0xbd,0xc2,0x8f,0xcc,0xa0,0x70,0x78,0x5c}}, "sb_filespace_device_read_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_device_write_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x2e,0x77,0xce,0xbd,0x62,0x75,0xbd,0x37,0x6b,0x09,0xf5}}, "sb_filespace_device_write_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_fsync_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x2f,0x78,0x63,0xbe,0xad,0x1c,0xd8,0x24,0x17,0x9b,0x11}}, "sb_filespace_fsync_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_device_error_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x30,0x75,0x2b,0x95,0xdd,0x83,0x95,0x96,0xa8,0x43,0x41}}, "sb_filespace_device_error_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_health_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x31,0x73,0x18,0x8e,0xff,0x22,0xaf,0xf5,0xd4,0xa1,0x71}}, "sb_filespace_health_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_role_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x32,0x75,0x8f,0xb4,0xce,0xe9,0x08,0xa5,0x61,0xb5,0xf7}}, "sb_filespace_role_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_filespace_lifecycle_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x33,0x79,0xf0,0xa2,0x0e,0x47,0x29,0x1c,0x86,0x2b,0x8c}}, "sb_storage_filespace_lifecycle_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_filespace_active_pins, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x34,0x7c,0x80,0xb1,0x7b,0x15,0xb8,0xa7,0xc1,0x00,0x10}}, "sb_storage_filespace_active_pins"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cloud_filespace_operation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x35,0x73,0x8f,0xa2,0x91,0xbb,0x6c,0x26,0x8a,0xe1,0xd2}}, "sb_cloud_filespace_operation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_foreign_filespace_quarantine_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x36,0x72,0x76,0x99,0x4d,0x7b,0xf1,0x30,0x79,0xcf,0x05}}, "sb_foreign_filespace_quarantine_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_move_request_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x37,0x70,0x71,0xa8,0x33,0x7a,0xc2,0x3a,0x76,0x47,0xf7}}, "sb_filespace_move_request_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_expand_request_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x38,0x7d,0x9b,0xb4,0x9b,0x40,0xe3,0x23,0x58,0xd2,0xa1}}, "sb_filespace_expand_request_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_filespace_shrink_request_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x39,0x76,0xf0,0x87,0xe2,0x6a,0xa4,0xdd,0xa2,0x12,0xe1}}, "sb_filespace_shrink_request_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_pressure_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x3a,0x79,0x83,0xa9,0xee,0x91,0x88,0x0a,0xab,0x53,0x6e}}, "sb_storage_pressure_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_temp_workspace_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x3b,0x79,0x26,0xbd,0x0a,0x2d,0x71,0x11,0xb9,0x8c,0x67}}, "sb_temp_workspace_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_build_workspace_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x3c,0x79,0xb0,0xb7,0xed,0xed,0x7a,0x45,0x8c,0xbe,0xe4}}, "sb_index_build_workspace_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_storage_support_redaction_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x3d,0x79,0x6d,0x96,0x47,0x53,0x00,0xed,0x70,0x35,0x7c}}, "sb_storage_support_redaction_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_free_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x3e,0x79,0xc1,0x86,0x2c,0x92,0xb5,0x6d,0xd8,0x79,0x9f}}, "sb_page_free_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_allocated_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x3f,0x75,0xae,0xb2,0xc1,0x87,0xa0,0xfc,0x76,0x2c,0x14}}, "sb_page_allocated_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_released_free_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x40,0x7f,0x43,0x97,0x6d,0xba,0x84,0xc4,0x9e,0x71,0x46}}, "sb_page_released_free_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_reserved_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x41,0x7f,0xad,0xaa,0x32,0xf8,0x6d,0x47,0x77,0x13,0x43}}, "sb_page_reserved_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_preallocated_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x42,0x75,0x85,0x83,0xa0,0xf0,0xe7,0xcd,0xcf,0x3e,0x74}}, "sb_page_preallocated_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_preallocation_target_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x43,0x72,0x15,0x84,0xfc,0x4d,0xa6,0xb7,0xbf,0x7b,0x88}}, "sb_page_preallocation_target_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_preallocation_deficit_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x44,0x7c,0x94,0x9f,0x71,0xf8,0xac,0xe3,0xb5,0x3c,0x61}}, "sb_page_preallocation_deficit_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_allocation_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x45,0x7c,0xe7,0xbd,0x2a,0x65,0x7a,0x88,0x7c,0x74,0xdf}}, "sb_page_allocation_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_allocation_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x46,0x77,0xc4,0xb7,0x37,0xef,0xaf,0x26,0x89,0x99,0x64}}, "sb_page_allocation_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_resident_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x47,0x73,0xc7,0x88,0xf1,0xa2,0x94,0xa7,0x6d,0x79,0xa2}}, "sb_page_cache_resident_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_resident_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x48,0x77,0x20,0x92,0xf0,0x41,0xf8,0x67,0xea,0x3f,0xd3}}, "sb_page_cache_resident_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_pinned_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x49,0x75,0x0f,0x9b,0x8f,0xbb,0x13,0x74,0x79,0xf5,0x9f}}, "sb_page_cache_pinned_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_dirty_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x4a,0x72,0xb0,0xa1,0xfd,0xc4,0xff,0xaf,0x6f,0x5f,0xec}}, "sb_page_cache_dirty_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_evictions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x4b,0x76,0xe5,0xb7,0xfb,0x60,0x93,0x79,0xb5,0x56,0xcb}}, "sb_page_cache_evictions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_fragmentation_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x4c,0x7f,0x4b,0x90,0x5a,0x50,0x73,0xa6,0x7a,0xac,0x44}}, "sb_page_fragmentation_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_relocation_backlog_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x4d,0x78,0xf9,0x9a,0x13,0x7f,0xc8,0xc8,0x76,0xf9,0xea}}, "sb_page_relocation_backlog_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_relocation_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x4e,0x76,0x65,0x86,0xbe,0xef,0xd9,0x54,0x01,0xd2,0x47}}, "sb_page_relocation_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_relocation_blocked_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x4f,0x7e,0xd6,0x97,0x0e,0xc2,0xac,0x9f,0xa7,0x4c,0x4c}}, "sb_page_relocation_blocked_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_relocatable_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x50,0x7e,0xbf,0xab,0x50,0x87,0x38,0x1c,0x09,0x3c,0x67}}, "sb_page_relocatable_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_unmovable_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x51,0x77,0x48,0xb9,0x18,0x5f,0xd6,0xee,0x34,0x73,0x59}}, "sb_page_unmovable_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_family_growth_rate_pages_per_second, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x52,0x75,0x2d,0x9a,0x7e,0x07,0xd2,0xaf,0x48,0xcb,0xb1}}, "sb_page_family_growth_rate_pages_per_second"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_family_depletion_eta_seconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x53,0x7c,0xe1,0x93,0x93,0xc5,0xd1,0x70,0xcd,0x1f,0x5d}}, "sb_page_family_depletion_eta_seconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_relocation_ready_for_filespace_shrink, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x54,0x73,0x23,0xa9,0x6a,0x7b,0x90,0x73,0x43,0xf0,0x55}}, "sb_page_relocation_ready_for_filespace_shrink"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_lookup_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x55,0x76,0x04,0x9c,0x86,0x23,0x08,0xe8,0xa8,0x20,0xd5}}, "sb_index_lookup_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_splits_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x56,0x71,0x7f,0xb4,0x41,0xd0,0x6b,0x92,0x56,0xbd,0xde}}, "sb_index_splits_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_index_read_amplification_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x57,0x7c,0x60,0x91,0xd8,0x6c,0xe4,0x01,0xdb,0x99,0x38}}, "sb_index_read_amplification_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_operation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x58,0x7e,0x7e,0x8f,0x43,0xae,0x70,0xb5,0x26,0x81,0xaf}}, "sb_datatype_operation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_cast_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x59,0x7e,0xa1,0xad,0xd5,0x51,0xf0,0x90,0xd4,0xce,0x3b}}, "sb_datatype_cast_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_numeric_backend_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x5a,0x79,0x23,0x8e,0x7a,0xeb,0x65,0x07,0x12,0xce,0x58}}, "sb_datatype_numeric_backend_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_catalog_descriptors, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x5b,0x70,0x69,0xa4,0x57,0x8e,0x52,0x47,0xb9,0x35,0x4f}}, "sb_datatype_catalog_descriptors"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_physical_encoding_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x5c,0x79,0x09,0x9e,0xaa,0x33,0x48,0x11,0xfa,0x5f,0x14}}, "sb_datatype_physical_encoding_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_chunk_event_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x5d,0x7b,0x15,0x8e,0x09,0x2f,0x14,0x8a,0x07,0x70,0x17}}, "sb_datatype_chunk_event_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_comparison_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x5e,0x78,0x6a,0x89,0xa0,0x23,0x05,0x77,0x3d,0xdd,0xff}}, "sb_datatype_comparison_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_locator_event_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x5f,0x73,0x93,0x80,0x77,0xe8,0x79,0x88,0x2d,0x0a,0x9a}}, "sb_datatype_locator_event_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_datatype_redaction_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x60,0x7f,0x0a,0x9e,0xc4,0x9f,0x93,0xaf,0x09,0x72,0x21}}, "sb_datatype_redaction_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_domain_method_invocation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x61,0x70,0xc2,0xb8,0x6e,0x29,0x66,0x84,0x55,0xf9,0xe6}}, "sb_domain_method_invocation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_plan_estimate_error_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x62,0x78,0x19,0x97,0x94,0x8f,0x85,0xce,0xc9,0x49,0x8c}}, "sb_optimizer_plan_estimate_error_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_estimated_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x63,0x71,0x66,0x9e,0x0c,0x7e,0x2d,0x0d,0xae,0x3c,0x6f}}, "sb_optimizer_feedback_estimated_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_actual_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x64,0x7d,0x08,0x8c,0x37,0x99,0x56,0xb5,0xd1,0xb6,0xa9}}, "sb_optimizer_feedback_actual_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_estimated_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x65,0x7d,0x4b,0xab,0xb7,0xa8,0x14,0x0a,0xdf,0xb9,0xa4}}, "sb_optimizer_feedback_estimated_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_actual_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x66,0x7d,0x9d,0xb8,0x12,0x84,0x92,0xbd,0x56,0x67,0x37}}, "sb_optimizer_feedback_actual_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_estimated_io_operations, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x67,0x78,0x51,0xb1,0xfc,0xc6,0x63,0x9c,0xf2,0x61,0xed}}, "sb_optimizer_feedback_estimated_io_operations"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_actual_io_operations, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x68,0x78,0x1d,0xa1,0xbc,0xbd,0x85,0x1f,0xd4,0xf4,0x61}}, "sb_optimizer_feedback_actual_io_operations"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_estimated_visibility_recheck_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x69,0x78,0x99,0xbc,0xcc,0x94,0xe6,0x15,0xa4,0xde,0x39}}, "sb_optimizer_feedback_estimated_visibility_recheck_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_actual_visibility_recheck_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x6a,0x74,0x27,0xa8,0x24,0x52,0xb3,0xf8,0x2f,0x85,0x55}}, "sb_optimizer_feedback_actual_visibility_recheck_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_estimated_spill_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x6b,0x71,0x54,0xb5,0xc0,0x15,0x17,0xbc,0x96,0xd4,0xe3}}, "sb_optimizer_feedback_estimated_spill_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_actual_spill_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x6c,0x70,0xc3,0xbb,0xc6,0xe9,0x29,0x72,0x57,0x20,0xda}}, "sb_optimizer_feedback_actual_spill_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_memory_grant_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x6d,0x7e,0x7e,0xb8,0xcf,0x91,0x11,0x63,0xc6,0xdf,0x9d}}, "sb_optimizer_feedback_memory_grant_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_peak_memory_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x6e,0x71,0x10,0xa5,0xb2,0xe0,0x06,0xba,0x46,0xcc,0x5a}}, "sb_optimizer_feedback_peak_memory_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_recommended_memory_grant_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x6f,0x7c,0x11,0xa7,0x11,0x8d,0x8a,0x2c,0x24,0x97,0x35}}, "sb_optimizer_feedback_recommended_memory_grant_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_estimated_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x70,0x77,0x49,0x81,0x1a,0xb8,0xb3,0xb1,0x24,0x85,0xd4}}, "sb_optimizer_feedback_estimated_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_actual_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x71,0x7e,0x9d,0x9b,0xda,0xf1,0x36,0x0a,0x4d,0x8f,0x7a}}, "sb_optimizer_feedback_actual_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_estimated_resource_units, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x72,0x7c,0x00,0x80,0x07,0xca,0x69,0x16,0xb0,0x25,0x4d}}, "sb_optimizer_feedback_estimated_resource_units"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_feedback_actual_resource_units, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x73,0x71,0x33,0x89,0xda,0x15,0x51,0x40,0xf4,0xba,0x8a}}, "sb_optimizer_feedback_actual_resource_units"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_lock_latch_contention_wait_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x74,0x7c,0x67,0xa6,0x8d,0x02,0xf8,0xf8,0xc3,0x6f,0x60}}, "sb_lock_latch_contention_wait_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_lock_latch_contention_wait_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x75,0x74,0xa1,0x97,0x75,0x1c,0xd7,0xdf,0x5c,0xb4,0x18}}, "sb_lock_latch_contention_wait_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_jit_compile_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x76,0x74,0xf3,0xb2,0x90,0xa3,0xea,0x75,0x2b,0xaa,0xae}}, "sb_native_compile_jit_compile_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_jit_compile_seconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x77,0x79,0xd2,0xa2,0x7d,0xac,0x45,0x17,0x3e,0xeb,0x9f}}, "sb_native_compile_jit_compile_seconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_jit_cache_hit_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x78,0x74,0x2d,0x83,0x3b,0x49,0xaf,0x96,0xb4,0x0b,0x2c}}, "sb_native_compile_jit_cache_hit_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_fallback_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x79,0x72,0xf5,0x86,0x17,0xd1,0x16,0xef,0x8e,0xc8,0xcb}}, "sb_native_compile_fallback_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_aot_load_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x7a,0x7f,0x55,0xbf,0x3f,0x49,0x89,0xc6,0xd1,0x42,0x5b}}, "sb_native_compile_aot_load_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_aot_invalid_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x7b,0x7a,0xad,0x95,0x49,0x0d,0xa4,0x1a,0x91,0x0c,0x7b}}, "sb_native_compile_aot_invalid_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_execution_fault_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x7c,0x76,0x74,0xa7,0xc1,0x57,0xa5,0x74,0xfb,0xd3,0xd4}}, "sb_native_compile_execution_fault_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_invalidation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x7d,0x7f,0x10,0x91,0xbe,0x06,0x87,0x92,0x97,0xe6,0xcd}}, "sb_native_compile_invalidation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_equivalence_failure_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x7e,0x77,0x18,0x9a,0xbf,0x70,0x7a,0x1f,0x35,0x49,0xe1}}, "sb_native_compile_equivalence_failure_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_native_compile_quarantine_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x7f,0x76,0x50,0xad,0x5b,0xf5,0x7f,0x96,0x56,0x74,0x51}}, "sb_native_compile_quarantine_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_device_available, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x80,0x7b,0xac,0xa2,0x64,0xc5,0x7b,0xa4,0x79,0x15,0x24}}, "sb_gpu_device_available"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_kernel_compile_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x81,0x72,0x68,0xbe,0x9b,0xc5,0x17,0x32,0xde,0xa7,0x28}}, "sb_gpu_kernel_compile_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_kernel_compile_seconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x82,0x72,0xa7,0x87,0x3f,0x4d,0x26,0x4e,0x74,0xc7,0xf4}}, "sb_gpu_kernel_compile_seconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_artifact_load_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x83,0x70,0x67,0x8d,0x13,0x0a,0x12,0xc0,0x03,0x6b,0x69}}, "sb_gpu_artifact_load_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_artifact_invalid_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x84,0x79,0xae,0xa4,0xe4,0xf6,0x06,0x87,0xa7,0x13,0xd0}}, "sb_gpu_artifact_invalid_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_execution_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x85,0x71,0xc5,0xaa,0x17,0xd8,0x1d,0xb1,0xa3,0x20,0x77}}, "sb_gpu_execution_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_execution_seconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x86,0x72,0xac,0xa0,0x49,0x0e,0xab,0x16,0xd2,0xfc,0xc8}}, "sb_gpu_execution_seconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_fallback_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x87,0x71,0x98,0x96,0x50,0xd9,0xda,0x4d,0x00,0x29,0x9a}}, "sb_gpu_fallback_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_runtime_fault_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x88,0x7a,0xb1,0xa7,0x58,0x02,0xa4,0xa1,0x90,0x6b,0x9b}}, "sb_gpu_runtime_fault_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_quarantine_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x89,0x72,0x9a,0x88,0xe1,0x7b,0x3c,0xe4,0xe8,0x84,0xfa}}, "sb_gpu_quarantine_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_memory_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x8a,0x7f,0xa6,0xbc,0xd3,0x24,0x67,0x2d,0x05,0x7c,0xbc}}, "sb_gpu_memory_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_transfer_bytes_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x8b,0x77,0x6c,0x88,0x26,0xbf,0xb8,0xa7,0xe1,0xf0,0x18}}, "sb_gpu_transfer_bytes_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_gpu_cluster_placement_refused_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x8c,0x7c,0xb0,0xab,0xe4,0xe4,0x67,0x25,0xab,0x4f,0xa9}}, "sb_gpu_cluster_placement_refused_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_udr_registration_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x8d,0x73,0x70,0xac,0xd3,0x09,0x0a,0x45,0x30,0x63,0x40}}, "sb_udr_registration_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_udr_load_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x8e,0x77,0x9f,0xbe,0x1e,0x10,0x77,0x64,0xc4,0x1a,0x85}}, "sb_udr_load_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_udr_unload_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x8f,0x7b,0xca,0xae,0x73,0xc1,0x2d,0x4e,0xa4,0xc4,0xdc}}, "sb_udr_unload_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_udr_inspect_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x90,0x71,0x01,0x83,0x3b,0x54,0x52,0x32,0x2f,0x19,0xb9}}, "sb_udr_inspect_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_udr_invocation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x91,0x74,0x02,0x93,0xf9,0x97,0x4f,0xa1,0x45,0xc6,0x35}}, "sb_udr_invocation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_udr_resource_refused_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x92,0x77,0xad,0x92,0xc8,0x0c,0xc1,0xfa,0xca,0x0b,0x22}}, "sb_udr_resource_refused_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_udr_non_cpp_refusal_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x93,0x73,0xd1,0x82,0xd8,0x0f,0x69,0xff,0x30,0x52,0x0b}}, "sb_udr_non_cpp_refusal_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_security_deep_enforcement_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x94,0x7f,0x76,0x91,0xb0,0xa6,0xa4,0x01,0x59,0x96,0x9c}}, "sb_security_deep_enforcement_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_security_deep_refusal_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x95,0x7a,0xd9,0x8c,0x98,0xc5,0x9e,0x24,0xe8,0x8d,0xee}}, "sb_security_deep_refusal_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_security_audit_before_success_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x96,0x7f,0x30,0x93,0x7c,0x08,0x70,0xd3,0xb6,0x72,0x8b}}, "sb_security_audit_before_success_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_security_masking_applied_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x97,0x77,0x0d,0xb7,0x98,0x66,0x77,0x48,0xf5,0x85,0x05}}, "sb_security_masking_applied_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_security_rls_filter_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x98,0x76,0xbf,0xac,0x69,0x18,0x25,0xc0,0xfa,0x83,0x25}}, "sb_security_rls_filter_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_remote_fragment_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x99,0x7b,0xd4,0xa4,0x2e,0xfa,0xe6,0xf8,0x48,0x88,0xcd}}, "sb_optimizer_remote_fragment_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_query_fragment_queue_delay_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x9a,0x76,0xc5,0xa1,0xb8,0x1c,0xd0,0x0a,0x94,0x5d,0x54}}, "sb_query_fragment_queue_delay_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_query_fragment_propagation_delay_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x9b,0x70,0xea,0x8d,0xe6,0x5c,0xb9,0x0d,0xdc,0xa6,0xbb}}, "sb_query_fragment_propagation_delay_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_query_fragment_local_connection_delay_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x9c,0x78,0x6c,0xa4,0xde,0x57,0xf8,0x0c,0x63,0xaa,0x0f}}, "sb_query_fragment_local_connection_delay_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_query_fragment_sample_freshness_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x9d,0x76,0x5e,0xa5,0x40,0x28,0x2d,0x59,0xb8,0x2d,0xcb}}, "sb_query_fragment_sample_freshness_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_parser_sessions_active, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x9e,0x70,0x00,0xb9,0x7c,0xce,0x7b,0x1d,0x8d,0x3d,0x1f}}, "sb_parser_sessions_active"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_parser_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0x9f,0x7d,0xc3,0x94,0x62,0xee,0x41,0x8b,0xc4,0xa8,0x0a}}, "sb_parser_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_parser_crashes_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa0,0x7e,0xb6,0xa6,0x0a,0x0a,0x3e,0x32,0xf3,0x66,0xd8}}, "sb_parser_crashes_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_parser_policy_attach_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa1,0x76,0x4d,0x93,0xc4,0x44,0xc1,0x81,0x23,0x07,0x8a}}, "sb_parser_policy_attach_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_listener_sessions_active, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa2,0x73,0x53,0x9f,0x84,0x51,0xf0,0x77,0x15,0xe3,0xce}}, "sb_listener_sessions_active"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_listener_rejects_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa3,0x77,0x0c,0x80,0x4f,0x9f,0x27,0xe3,0x8c,0xa9,0xd8}}, "sb_listener_rejects_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_listener_queue_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa4,0x7e,0x38,0xb1,0x53,0x34,0x68,0xa4,0x07,0x87,0xcd}}, "sb_listener_queue_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_listener_queue_delay_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa5,0x7f,0x71,0xbb,0xb9,0xc9,0x08,0xc4,0x2b,0xeb,0x2a}}, "sb_listener_queue_delay_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_management_frontend_requests_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa6,0x7d,0xb7,0xba,0x66,0xd6,0x1d,0xac,0x00,0x13,0x7b}}, "sb_management_frontend_requests_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_management_frontend_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa7,0x76,0xa2,0xa7,0x73,0xe8,0x8d,0x81,0x25,0x3a,0x23}}, "sb_management_frontend_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_begin_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa8,0x7b,0x2f,0xbb,0xac,0xf8,0xe6,0x77,0x8d,0xeb,0x14}}, "sb_tx_begin_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_commit_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xa9,0x7d,0x16,0x94,0xed,0x05,0xb2,0x32,0xb4,0xd2,0xc1}}, "sb_tx_commit_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_rollback_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xaa,0x79,0xef,0x95,0x75,0xc0,0x37,0xdc,0x81,0xc5,0x66}}, "sb_tx_rollback_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_abort_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xab,0x7b,0x62,0xbe,0xa9,0x5d,0x25,0x05,0x79,0x5b,0xb5}}, "sb_tx_abort_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_failure_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xac,0x78,0x88,0x90,0xa5,0x3f,0x48,0x8c,0x1c,0xcb,0x91}}, "sb_tx_failure_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_cluster_fail_closed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xad,0x74,0xb2,0xb7,0x33,0x39,0x6d,0x9b,0xa3,0x8a,0x7e}}, "sb_tx_cluster_fail_closed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_active_transactions, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xae,0x75,0xd6,0x90,0x5f,0xf8,0xcd,0xb3,0xd2,0x65,0x77}}, "sb_tx_active_transactions"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_oldest_snapshot_age_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xaf,0x7e,0x27,0x9a,0xd1,0x88,0x51,0xbd,0xde,0x29,0x7d}}, "sb_tx_oldest_snapshot_age_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_runtime_policy_violation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb0,0x79,0x8c,0x91,0xca,0xb3,0xb8,0x10,0x23,0x0e,0x8c}}, "sb_tx_runtime_policy_violation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_lock_decisions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb1,0x7b,0xc1,0xac,0x77,0x74,0xf7,0x7a,0xd3,0x62,0x2c}}, "sb_tx_lock_decisions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_locks_held, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb2,0x7d,0x54,0xbe,0x29,0x3f,0x5d,0x0d,0x9d,0x19,0x0b}}, "sb_tx_locks_held"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_lock_waiters, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb3,0x73,0x95,0x9e,0x31,0x90,0x1d,0x84,0x67,0x2b,0x61}}, "sb_tx_lock_waiters"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_tx_savepoint_operations_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb4,0x78,0x47,0xbf,0xd2,0xe7,0xbb,0xd9,0x0a,0xc0,0xde}}, "sb_tx_savepoint_operations_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_row_versions_reclaimed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb5,0x79,0xef,0x93,0x1d,0x53,0x7f,0x6a,0x15,0xf3,0x7d}}, "sb_mga_row_versions_reclaimed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_cleanup_horizon_local_transaction_id, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb6,0x7f,0x31,0x99,0x4e,0x2e,0xed,0x16,0xe9,0x68,0x13}}, "sb_mga_cleanup_horizon_local_transaction_id"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_cleanup_blocked_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb7,0x7b,0xdc,0xa8,0xda,0x4b,0x65,0x27,0xfb,0xff,0x67}}, "sb_mga_cleanup_blocked_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_cleanup_retained_row_versions, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb8,0x7a,0x60,0x86,0x6b,0x24,0x08,0x76,0xfb,0xda,0x34}}, "sb_mga_cleanup_retained_row_versions"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_cleanup_retention_age_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xb9,0x75,0x95,0x8a,0xbf,0xb6,0x8d,0x13,0x21,0x7f,0x7f}}, "sb_mga_cleanup_retention_age_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_temporary_on_commit_delete_rows_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xba,0x79,0xd3,0xbd,0xac,0x4f,0x48,0x0b,0xc2,0xaa,0xa3}}, "sb_mga_temporary_on_commit_delete_rows_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_lag_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xbb,0x75,0x89,0xa1,0x7e,0x44,0x13,0xf4,0x5f,0x45,0x4c}}, "sb_archive_lag_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_slice_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xbc,0x75,0x8c,0x91,0x17,0x04,0xc4,0x61,0x71,0xbf,0x5e}}, "sb_archive_slice_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_slice_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xbd,0x7a,0x76,0x95,0x17,0x2e,0x30,0xba,0x5b,0x54,0xd8}}, "sb_archive_slice_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_slice_age_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xbe,0x76,0xfe,0xa7,0x73,0xbb,0x75,0xa9,0xbe,0xdc,0xca}}, "sb_archive_slice_age_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_slice_max_age_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xbf,0x7f,0xf2,0x9a,0xfc,0xdc,0xf5,0x25,0x54,0x5f,0x60}}, "sb_archive_slice_max_age_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_health_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc0,0x75,0x41,0xb5,0xdb,0xcb,0x79,0x2f,0x53,0xec,0x25}}, "sb_archive_health_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_queue_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc1,0x74,0x37,0x96,0x15,0x80,0x4c,0xb2,0x5e,0x23,0x8f}}, "sb_archive_queue_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_delta_lag_transactions, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc2,0x7b,0xb7,0xac,0x14,0x27,0x8f,0x7a,0xa9,0x83,0xd0}}, "sb_archive_delta_lag_transactions"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_delta_apply_lag_transactions, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc3,0x76,0x71,0xbd,0x9b,0x4a,0xd4,0x2f,0xfe,0xa8,0xb3}}, "sb_archive_delta_apply_lag_transactions"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_checksum_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc4,0x7d,0x2b,0x88,0xc0,0xb8,0x41,0x8f,0x7a,0x45,0x7c}}, "sb_archive_checksum_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_archive_restore_refusals_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc5,0x7c,0x4b,0xb5,0xb3,0xee,0xf8,0xcf,0x58,0x51,0xbd}}, "sb_archive_restore_refusals_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_backup_in_progress, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc6,0x7e,0x31,0x89,0xad,0x04,0xc8,0x21,0xbf,0xcf,0x08}}, "sb_backup_in_progress"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_backup_progress_percent, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc7,0x7e,0xec,0xba,0x7d,0x27,0xaa,0x06,0x2e,0x40,0xfb}}, "sb_backup_progress_percent"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_restore_drill_duration_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc8,0x77,0x91,0xa3,0x75,0x15,0xb7,0xfb,0x5c,0xc5,0x3b}}, "sb_restore_drill_duration_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_pitr_window_available_seconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xc9,0x7d,0x58,0x80,0xa0,0x58,0x3e,0xc6,0xdb,0xc1,0x90}}, "sb_pitr_window_available_seconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_auth_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xca,0x73,0x8f,0x8f,0xc3,0x33,0x48,0x54,0x47,0xb6,0x15}}, "sb_auth_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_identity_auth_attempts_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xcb,0x7d,0x0f,0x83,0xd3,0x28,0xf5,0x19,0x0e,0x2d,0x38}}, "sb_identity_auth_attempts_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_identity_sessions_active, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xcc,0x75,0x74,0xbe,0x5b,0x30,0x20,0x57,0xc8,0xe9,0x26}}, "sb_identity_sessions_active"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_identity_users_online, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xcd,0x79,0xf8,0xa3,0x1d,0x5a,0x68,0xe5,0x01,0x4b,0x73}}, "sb_identity_users_online"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_policy_evaluations_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xce,0x76,0xf6,0x8d,0x70,0x28,0xc5,0xa8,0xea,0x56,0xd6}}, "sb_policy_evaluations_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_actions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xcf,0x75,0x5f,0x94,0x9c,0x97,0xe6,0x5f,0x06,0xa8,0x0c}}, "sb_agent_actions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_decision_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd0,0x7d,0x54,0x90,0xb0,0x3e,0xa8,0x0e,0x8d,0xc2,0xca}}, "sb_agent_decision_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_runtime_service_events_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd1,0x7b,0x9f,0x9f,0xe3,0xdc,0xc9,0x50,0xda,0xa1,0xff}}, "sb_agent_runtime_service_events_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_runtime_service_leases, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd2,0x7a,0xe0,0xa7,0x76,0x24,0xd0,0xae,0xf1,0x9f,0x27}}, "sb_agent_runtime_service_leases"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_runtime_service_actions, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd3,0x73,0x45,0x82,0x1a,0x98,0x4e,0x25,0x31,0x65,0x83}}, "sb_agent_runtime_service_actions"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_runtime_service_history_records, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd4,0x74,0x49,0xb6,0x1c,0x99,0xa6,0xd8,0x26,0x92,0x36}}, "sb_agent_runtime_service_history_records"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_runtime_service_catalog_generation, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd5,0x71,0x1d,0xb1,0x91,0x01,0xf5,0x56,0x43,0xe8,0xc8}}, "sb_agent_runtime_service_catalog_generation"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_filespace_capacity_requests_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd6,0x74,0xd0,0x97,0xa0,0x3a,0x8b,0x88,0x66,0x47,0x6f}}, "sb_agent_filespace_capacity_requests_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_filespace_free_reserve_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd7,0x7e,0xe3,0xa4,0x31,0x8b,0x46,0x4e,0x80,0x26,0x16}}, "sb_agent_filespace_free_reserve_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_filespace_decision_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd8,0x7c,0xfb,0xa0,0x9d,0x3c,0x05,0xff,0xf3,0x06,0xe5}}, "sb_agent_filespace_decision_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_page_allocation_requests_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xd9,0x71,0xc2,0x8c,0x0a,0x6d,0x90,0x3b,0x65,0xee,0x34}}, "sb_agent_page_allocation_requests_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_page_allocation_preallocated_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xda,0x73,0x37,0x8f,0xd2,0x49,0x7d,0x0c,0x3e,0xd7,0xde}}, "sb_agent_page_allocation_preallocated_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_agent_page_allocation_relocated_pages_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xdb,0x7c,0x0c,0x87,0xd8,0x05,0x30,0x9c,0xc9,0xba,0x38}}, "sb_agent_page_allocation_relocated_pages_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_alerts_fired_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xdc,0x79,0x0d,0x96,0x6a,0xa3,0x44,0x41,0xf0,0xa1,0xbf}}, "sb_alerts_fired_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_node_cpu_feature_available, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xdd,0x79,0x4f,0xa5,0xdf,0xd8,0x08,0x62,0xda,0x2c,0x06}}, "sb_cluster_node_cpu_feature_available"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_node_role_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xde,0x72,0x68,0xb4,0xa8,0xb6,0x3d,0x9e,0xfe,0xd8,0x23}}, "sb_cluster_node_role_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_node_saturation_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xdf,0x7c,0xb7,0xaf,0x9b,0x80,0x2f,0x2d,0xb5,0xa8,0xb1}}, "sb_cluster_node_saturation_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_admission_denied_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe0,0x70,0xf3,0x83,0x7f,0xc2,0xa6,0x8a,0xa0,0x25,0x28}}, "sb_cluster_admission_denied_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_limbo_transactions, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe1,0x71,0xc6,0x97,0x14,0xa6,0xf5,0x78,0x33,0x3a,0xdf}}, "sb_cluster_limbo_transactions"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_rolling_upgrade_readiness_state, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe2,0x7e,0x56,0xa0,0x70,0x04,0xc9,0x55,0x03,0x58,0x31}}, "sb_cluster_rolling_upgrade_readiness_state"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_scheduler_queue_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe3,0x78,0xfa,0xb4,0xba,0x07,0xff,0x0b,0xc0,0xa3,0x0e}}, "sb_cluster_scheduler_queue_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_route_checks_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe4,0x70,0xc8,0x87,0xfc,0x43,0x61,0xf5,0x64,0x5a,0xae}}, "sb_cluster_insert_route_checks_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_stale_route_rejections_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe5,0x70,0xdc,0x99,0xc7,0xb4,0x00,0x97,0xb6,0xef,0x7b}}, "sb_cluster_insert_stale_route_rejections_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_participant_admissions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe6,0x79,0x71,0x8b,0x01,0x43,0x7f,0xd3,0x87,0x0d,0x30}}, "sb_cluster_insert_participant_admissions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_remote_requests_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe7,0x72,0x49,0x89,0x1d,0x9b,0xb8,0xaa,0xd4,0xf7,0x95}}, "sb_cluster_insert_remote_requests_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_finality_wait_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe8,0x72,0x8a,0x99,0x43,0xda,0xf3,0xee,0xeb,0x39,0x19}}, "sb_cluster_insert_finality_wait_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_rows_mutated_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xe9,0x7d,0xee,0x9d,0xa9,0xfd,0x92,0x0c,0xef,0xc0,0xd9}}, "sb_cluster_insert_rows_mutated_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_fail_closed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xea,0x7d,0x47,0x83,0xb9,0xe9,0xa0,0x33,0x49,0x4d,0x97}}, "sb_cluster_insert_fail_closed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_insert_bad_stats_suppressed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xeb,0x76,0x03,0xb9,0xa9,0xfd,0x5a,0xa3,0x4c,0x2d,0x35}}, "sb_cluster_insert_bad_stats_suppressed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_channels, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xec,0x75,0xcb,0x8a,0x94,0xb1,0x7c,0x86,0x40,0x49,0xab}}, "sb_event_channels"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_subscriptions_active, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xed,0x75,0x41,0x95,0x2d,0x46,0x61,0x14,0x8d,0x2e,0x98}}, "sb_event_subscriptions_active"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_subscribe_attempts_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xee,0x7c,0x6e,0x88,0x79,0x36,0x42,0x9d,0x87,0x1d,0xe1}}, "sb_event_subscribe_attempts_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_publish_attempts_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xef,0x77,0x1e,0x8e,0xc3,0x82,0x40,0x12,0xed,0x6b,0xaa}}, "sb_event_publish_attempts_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_publications_committed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf0,0x76,0xc5,0xab,0x2e,0x0f,0x2f,0x79,0xb6,0xac,0xc4}}, "sb_event_publications_committed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_publications_rolled_back_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf1,0x7f,0xfd,0x84,0x29,0xb3,0x7d,0x78,0x82,0x00,0x6e}}, "sb_event_publications_rolled_back_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_queued, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf2,0x7e,0xb0,0x97,0x89,0xa6,0xea,0x3e,0x2f,0x54,0xdf}}, "sb_event_queued"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_queue_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf3,0x76,0x55,0x8f,0x8e,0x4a,0xbf,0x5a,0x03,0xc1,0xbf}}, "sb_event_queue_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_delivered_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf4,0x7a,0x1c,0xba,0x71,0x6b,0x94,0x28,0x77,0xb7,0xd9}}, "sb_event_delivered_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_delivery_latency_microseconds, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf5,0x71,0x67,0xbb,0xfa,0xd5,0x4a,0xdd,0xea,0x2c,0xfd}}, "sb_event_delivery_latency_microseconds"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_backpressure_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf6,0x77,0x33,0x8f,0xf7,0xf6,0xcc,0x4d,0x40,0x27,0x7a}}, "sb_event_backpressure_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_event_redacted_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf7,0x7e,0x20,0x9e,0x5c,0x1a,0xbe,0xab,0xfe,0x72,0xe8}}, "sb_event_redacted_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_cluster_event_route_attempts_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf8,0x75,0xaa,0xaa,0xe7,0x44,0x2b,0x9a,0xa5,0x61,0xd4}}, "sb_cluster_event_route_attempts_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_query_fragment_route_generation, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xf9,0x72,0xe9,0xbd,0x05,0xe7,0x77,0xf0,0x57,0xe5,0x05}}, "sb_query_fragment_route_generation"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_scheduler_queue_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xfa,0x72,0x97,0xa0,0xec,0x5a,0x00,0xb0,0x85,0x56,0xaa}}, "sb_scheduler_queue_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_job_control_actions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xfb,0x75,0x3b,0xaa,0x0d,0x5a,0x18,0x6d,0xda,0x37,0xef}}, "sb_job_control_actions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_support_bundle_completeness_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xfc,0x72,0x9a,0x85,0xd9,0xae,0x3b,0xef,0x18,0xcd,0xdd}}, "sb_support_bundle_completeness_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_export_adapter_queue_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xfd,0x73,0x4a,0x81,0x4d,0x14,0x91,0xe5,0x23,0x8a,0x3b}}, "sb_export_adapter_queue_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_export_adapter_failures_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xfe,0x74,0x1d,0xae,0xda,0xca,0xbb,0x75,0x71,0xc2,0x47}}, "sb_export_adapter_failures_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_export_adapter_retries_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x75,0xff,0x7a,0xad,0xac,0x6c,0x5b,0x85,0xfe,0x1d,0x06,0x3a}}, "sb_export_adapter_retries_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_metric_samples_rejected_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x00,0x70,0xf5,0x9e,0x23,0xdf,0x49,0x5c,0x91,0x89,0x05}}, "sb_metric_samples_rejected_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_metric_export_shed_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x01,0x7f,0x84,0xb3,0xd4,0xcb,0xf2,0xe9,0xd4,0x8f,0x82}}, "sb_metric_export_shed_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_idle_capacity_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x02,0x77,0x66,0x81,0x43,0x8f,0xf0,0x75,0xc8,0x75,0x6b}}, "sb_idle_capacity_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_workload_slo_burn_rate, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x03,0x73,0xd1,0xb1,0x4e,0x0e,0x9e,0x20,0x2c,0xe0,0x31}}, "sb_workload_slo_burn_rate"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_concurrency_latch_waits, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x04,0x71,0xc9,0x8b,0xb8,0xdd,0xae,0x85,0x93,0xc7,0xf1}}, "sb_mga_concurrency_latch_waits"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_concurrency_latch_timeouts, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x05,0x73,0x9a,0x85,0x74,0x1a,0xb3,0xec,0xa4,0xb9,0xf8}}, "sb_mga_concurrency_latch_timeouts"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_concurrency_fail_safe_releases, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x06,0x7c,0xa0,0xa8,0x1c,0x17,0xbe,0x5f,0xf7,0xf3,0xd2}}, "sb_mga_concurrency_fail_safe_releases"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_mga_concurrency_latch_wait_duration_us, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x07,0x78,0xbc,0x8a,0xb8,0x9d,0x8e,0x0d,0x3a,0x0c,0x6c}}, "sb_mga_concurrency_latch_wait_duration_us"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_resident_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x08,0x78,0x24,0x8b,0x61,0x2f,0xbd,0xd9,0x93,0xca,0x84}}, "sb_page_cache_context_resident_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_resident_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x09,0x70,0x74,0xae,0xf9,0x02,0x3b,0xcc,0x78,0xd6,0xf3}}, "sb_page_cache_context_resident_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_pinned_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x0a,0x75,0xe2,0xba,0x27,0x5b,0x69,0x51,0x08,0x3e,0x02}}, "sb_page_cache_context_pinned_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_dirty_pages, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x0b,0x7e,0x95,0xaa,0x14,0x37,0x3c,0x00,0x35,0x0b,0x03}}, "sb_page_cache_context_dirty_pages"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_admissions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x0c,0x7f,0xe6,0xba,0x8a,0x28,0x77,0xa9,0x9b,0x0d,0xce}}, "sb_page_cache_context_admissions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_reuses_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x0d,0x74,0x46,0x84,0x0e,0x77,0x0c,0x00,0xce,0x37,0xd2}}, "sb_page_cache_context_reuses_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_evictions_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x0e,0x77,0xd0,0xbb,0x38,0x68,0x28,0x22,0xc3,0xd7,0x43}}, "sb_page_cache_context_evictions_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_protected_normal_hot_skips_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x0f,0x74,0x61,0xbb,0x8f,0x0e,0x1d,0x8f,0x24,0x49,0x5f}}, "sb_page_cache_context_protected_normal_hot_skips_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_page_cache_context_refusals_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x10,0x7c,0xbe,0xab,0x32,0x6d,0xa8,0x14,0xf0,0x6a,0xfd}}, "sb_page_cache_context_refusals_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_operator_actual_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x11,0x7b,0x7c,0x8e,0xe4,0x12,0x60,0xb3,0x84,0xdb,0xba}}, "sb_optimizer_operator_actual_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_operator_rows_examined, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x12,0x73,0xd4,0xa1,0xed,0x0d,0x69,0x48,0x57,0xb0,0x18}}, "sb_optimizer_operator_rows_examined"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_operator_rows_filtered, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x13,0x73,0x9d,0xb4,0x98,0x00,0x13,0x0d,0x0e,0x6d,0x40}}, "sb_optimizer_operator_rows_filtered"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_operator_loop_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x14,0x79,0x20,0x8a,0x9d,0x98,0xf9,0x2f,0xb9,0x08,0x51}}, "sb_optimizer_operator_loop_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_operator_cpu_time, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x15,0x77,0x91,0xa8,0x6f,0xc1,0xbc,0xa4,0x64,0x48,0xa9}}, "sb_optimizer_operator_cpu_time"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_spill_passes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x16,0x75,0x13,0xa0,0x06,0x1d,0x84,0xff,0xad,0xbb,0xe1}}, "sb_optimizer_spill_passes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_lifecycle_operation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x17,0x74,0x60,0x9a,0x06,0x2e,0x42,0x48,0xb1,0xf6,0x24}}, "sb_lifecycle_operation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_lifecycle_diagnostic_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x18,0x71,0x93,0xa0,0x71,0x56,0x65,0xef,0x45,0xc6,0x4a}}, "sb_lifecycle_diagnostic_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_lifecycle_audit_event_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x19,0x79,0x1c,0x85,0x06,0xde,0x79,0x3f,0xc2,0x11,0xfa}}, "sb_lifecycle_audit_event_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_lifecycle_cache_invalidation_total, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x1a,0x7b,0xe6,0x91,0x41,0xf2,0x19,0x6c,0x9f,0x3f,0xe0}}, "sb_lifecycle_cache_invalidation_total"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_route_plan_hash, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x1b,0x73,0x39,0x9c,0x5d,0x79,0x19,0xe3,0xda,0xee,0xb7}}, "sb_optimizer_route_plan_hash"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_route_result_hash, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x1c,0x77,0x21,0xb2,0x2a,0x19,0xa2,0xc8,0x11,0xbd,0x21}}, "sb_optimizer_route_result_hash"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_explain_digest, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x1d,0x7b,0x01,0xb7,0x81,0x1f,0x57,0x9d,0x80,0x10,0x2c}}, "sb_optimizer_explain_digest"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_route_equivalence_status, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x1e,0x7c,0x8f,0xaf,0x0a,0x70,0x3b,0x22,0xc6,0xae,0xe5}}, "sb_optimizer_route_equivalence_status"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_driver_visible_route_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x1f,0x7e,0x50,0xa0,0xbf,0x11,0xa9,0x22,0x69,0x21,0x58}}, "sb_optimizer_driver_visible_route_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_candidate_set_cardinality, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x20,0x74,0x1a,0xac,0x80,0x68,0x96,0xfc,0x26,0xcf,0xa8}}, "sb_optimizer_candidate_set_cardinality"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_candidate_set_density, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x21,0x7c,0x9f,0xbe,0x00,0xd5,0xcc,0x4a,0x93,0x3c,0x00}}, "sb_optimizer_candidate_set_density"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_candidate_set_recheck_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x22,0x70,0x8e,0x82,0xa0,0x25,0x9a,0xf1,0x69,0xb3,0x3a}}, "sb_optimizer_candidate_set_recheck_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_specialized_exact_recheck_rows, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x23,0x7a,0xcc,0x9c,0x44,0xb0,0xf3,0xd5,0xd7,0xa1,0x67}}, "sb_optimizer_specialized_exact_recheck_rows"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_specialized_false_positive_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x24,0x7e,0x4c,0xb9,0xe5,0x25,0x0d,0x63,0x08,0x6c,0x41}}, "sb_optimizer_specialized_false_positive_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_time_series_bucket_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x25,0x78,0x84,0xa8,0x2f,0x5a,0x71,0x81,0xc1,0xa4,0xef}}, "sb_optimizer_time_series_bucket_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_time_series_rollup_selectivity, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x26,0x7f,0x55,0x8e,0xdb,0xc5,0x53,0x70,0xf3,0xe3,0x53}}, "sb_optimizer_time_series_rollup_selectivity"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_page_cache_hit_miss, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x27,0x7a,0x22,0xad,0xc1,0xe6,0xd6,0x06,0x2e,0x51,0x4f}}, "sb_optimizer_page_cache_hit_miss"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_prefetch_usefulness, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x28,0x76,0x86,0xa5,0x5d,0x9d,0xa5,0xf1,0xcf,0x63,0xab}}, "sb_optimizer_prefetch_usefulness"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_page_count, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x29,0x78,0x16,0xb4,0x1c,0x96,0xa3,0x49,0x4c,0x9e,0x8c}}, "sb_optimizer_page_count"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_page_cache_dirty_pressure, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x2a,0x7c,0x93,0xb4,0x7d,0x86,0xb4,0x5d,0x00,0x33,0xe5}}, "sb_optimizer_page_cache_dirty_pressure"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_page_cache_pin_pressure, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x2b,0x77,0x73,0xa8,0x67,0x8a,0xc2,0xe9,0x03,0xb4,0xb8}}, "sb_optimizer_page_cache_pin_pressure"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_writeback_pressure, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x2c,0x77,0x52,0xa5,0xc8,0x07,0xaa,0x09,0xb1,0xb0,0xce}}, "sb_optimizer_writeback_pressure"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_sequential_page_cost, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x2d,0x7e,0xa5,0x8d,0x0a,0xe6,0x6d,0x5d,0x1a,0x9f,0xf3}}, "sb_optimizer_sequential_page_cost"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_random_page_cost, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x2e,0x7d,0x2f,0x94,0xe8,0xd4,0x7d,0xc6,0xdf,0xdb,0xf0}}, "sb_optimizer_random_page_cost"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_filespace_pressure, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x2f,0x74,0xa5,0xab,0x84,0x4c,0x67,0xdc,0x58,0xf5,0x5b}}, "sb_optimizer_filespace_pressure"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_mga_cleanup_debt, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x30,0x7d,0x87,0xb5,0x6e,0x59,0xb2,0x3a,0xb8,0x9c,0x72}}, "sb_optimizer_mga_cleanup_debt"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_mga_retained_dead_bytes, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x31,0x7f,0xdd,0x8f,0x0a,0x65,0x9c,0x5d,0x50,0xbb,0x3b}}, "sb_optimizer_mga_retained_dead_bytes"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_mga_chain_depth, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x32,0x77,0xb3,0x81,0xb9,0x49,0xd9,0x8a,0x2f,0x86,0xa0}}, "sb_optimizer_mga_chain_depth"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_mga_chain_scatter, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x33,0x7a,0x59,0x8c,0xc6,0xfd,0x9d,0xcd,0xac,0x9f,0x0f}}, "sb_optimizer_mga_chain_scatter"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_same_page_update_ratio, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x34,0x78,0xcd,0x80,0xa3,0xde,0xc6,0xee,0x05,0x17,0x3e}}, "sb_optimizer_same_page_update_ratio"},
    CatalogMetricFamilyIdentity{CatalogMetricFamily::sb_optimizer_commit_fence_pressure, scratchbird::core::platform::Uuid{std::array<scratchbird::core::platform::byte, 16>{0x01,0xa1,0xd1,0xd7,0x76,0x35,0x7d,0x08,0xa9,0x04,0x94,0x50,0x27,0xae,0x12,0x0d}}, "sb_optimizer_commit_fence_pressure"},
}};
constexpr bool IsKnownCatalogMetricFamily(CatalogMetricFamily value) noexcept {
  for (const auto& r : kCatalogMetricFamilies)
    if (r.family == value) return true;
  return false;
}
constexpr std::optional<scratchbird::core::platform::Uuid> CatalogMetricFamilyIdentityOf(CatalogMetricFamily value) noexcept {
  for (const auto& r : kCatalogMetricFamilies)
    if (r.family == value) return r.uuid;
  return std::nullopt;
}
// Boundary adapter: the constant a text tag names; nullopt for one with no
// constant (refused by the caller, never defaulted).
constexpr std::optional<CatalogMetricFamily> CatalogMetricFamilyForTag(std::string_view tag) noexcept {
  for (const auto& r : kCatalogMetricFamilies)
    if (r.tag == tag) return r.family;
  return std::nullopt;
}
// ---- end GENERATED from 28d ----

// ---- small agent classifications (coding-time constants, hand-written) ----
// Values follow the order the code defines them. Each has a boundary ForTag
// adapter mapping the exact text the current code writes (agent manifest,
// bootstrap declarations, native policy reader); nullopt for any other text.
template <class E>
struct CatalogAgentTagEntry {
  E value;
  std::string_view tag;  // boundary spelling only
};
template <class E, std::size_t N>
constexpr std::optional<E> CatalogAgentTagLookup(const std::array<CatalogAgentTagEntry<E>, N>& table,
                                                 std::string_view tag) noexcept {
  for (const auto& r : table)
    if (r.tag == tag) return r.value;
  return std::nullopt;
}

// AgentRuntimeLayer (agent_runtime.hpp).
enum class CatalogAgentRuntimeLayer : scratchbird::core::platform::u16 {
  none = 0, l1_observation = 1, l2_recorder = 2, l3_dispatcher = 3, l4_worker = 4, l5_coordinator = 5 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentRuntimeLayer>, 5> kCatalogAgentRuntimeLayerTags{{
    {CatalogAgentRuntimeLayer::l1_observation, "l1_observation"},
    {CatalogAgentRuntimeLayer::l2_recorder, "l2_recorder"},
    {CatalogAgentRuntimeLayer::l3_dispatcher, "l3_dispatcher"},
    {CatalogAgentRuntimeLayer::l4_worker, "l4_worker"},
    {CatalogAgentRuntimeLayer::l5_coordinator, "l5_coordinator"}}};
constexpr std::optional<CatalogAgentRuntimeLayer> CatalogAgentRuntimeLayerForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentRuntimeLayerTags, tag);
}

// AgentDeployment (agent_runtime.hpp).
enum class CatalogAgentDeployment : scratchbird::core::platform::u16 { none = 0, local = 1, both = 2, cluster = 3 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentDeployment>, 3> kCatalogAgentDeploymentTags{{
    {CatalogAgentDeployment::local, "local"},
    {CatalogAgentDeployment::both, "both"},
    {CatalogAgentDeployment::cluster, "cluster"}}};
constexpr std::optional<CatalogAgentDeployment> CatalogAgentDeploymentForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentDeploymentTags, tag);
}

// AgentAuthorityClass (agent_runtime.hpp).
enum class CatalogAgentAuthorityClass : scratchbird::core::platform::u16 {
  none = 0, observe_only = 1, recommend_only = 2, request_action = 3, direct_bounded_action = 4 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentAuthorityClass>, 4> kCatalogAgentAuthorityClassTags{{
    {CatalogAgentAuthorityClass::observe_only, "observe_only"},
    {CatalogAgentAuthorityClass::recommend_only, "recommend_only"},
    {CatalogAgentAuthorityClass::request_action, "request_action"},
    {CatalogAgentAuthorityClass::direct_bounded_action, "direct_bounded_action"}}};
constexpr std::optional<CatalogAgentAuthorityClass> CatalogAgentAuthorityClassForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentAuthorityClassTags, tag);
}

// AgentActivationProfile (agent_runtime.hpp).
enum class CatalogAgentActivationProfile : scratchbird::core::platform::u16 {
  none = 0, disabled = 1, observe_only = 2, recommend_only = 3, dry_run = 4, live_action = 5 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentActivationProfile>, 5> kCatalogAgentActivationProfileTags{{
    {CatalogAgentActivationProfile::disabled, "disabled"},
    {CatalogAgentActivationProfile::observe_only, "observe_only"},
    {CatalogAgentActivationProfile::recommend_only, "recommend_only"},
    {CatalogAgentActivationProfile::dry_run, "dry_run"},
    {CatalogAgentActivationProfile::live_action, "live_action"}}};
constexpr std::optional<CatalogAgentActivationProfile> CatalogAgentActivationProfileForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentActivationProfileTags, tag);
}

// Policy action_mode: every literal the native policy reader accepts
// (native_local_service_agent_policy.cpp), in its order; a superset of
// ActivationActionMode and BaselineDefaultModeForPolicyFamily (agent_runtime.cpp).
enum class CatalogAgentActionMode : scratchbird::core::platform::u16 {
  none = 0, disabled = 1, observe_only = 2, recommend_only = 3, request_action = 4, direct_bounded = 5,
  dry_run = 6, live_action = 7, operator_only = 8 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentActionMode>, 8> kCatalogAgentActionModeTags{{
    {CatalogAgentActionMode::disabled, "disabled"},
    {CatalogAgentActionMode::observe_only, "observe_only"},
    {CatalogAgentActionMode::recommend_only, "recommend_only"},
    {CatalogAgentActionMode::request_action, "request_action"},
    {CatalogAgentActionMode::direct_bounded, "direct_bounded"},
    {CatalogAgentActionMode::dry_run, "dry_run"},
    {CatalogAgentActionMode::live_action, "live_action"},
    {CatalogAgentActionMode::operator_only, "operator_only"}}};
constexpr std::optional<CatalogAgentActionMode> CatalogAgentActionModeForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentActionModeTags, tag);
}

// Bootstrap policy_class (agent_bootstrap_configuration.hpp).
enum class CatalogAgentPolicyClass : scratchbird::core::platform::u16 {
  none = 0, runtime_lifecycle_agent = 1, operational_agent = 2 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentPolicyClass>, 2> kCatalogAgentPolicyClassTags{{
    {CatalogAgentPolicyClass::runtime_lifecycle_agent, "runtime_lifecycle_agent"},
    {CatalogAgentPolicyClass::operational_agent, "operational_agent"}}};
constexpr std::optional<CatalogAgentPolicyClass> CatalogAgentPolicyClassForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentPolicyClassTags, tag);
}

// Bootstrap registration state (agent_bootstrap_configuration.hpp).
enum class CatalogAgentRegistrationState : scratchbird::core::platform::u16 { none = 0, registered = 1, disabled = 2 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentRegistrationState>, 2> kCatalogAgentRegistrationStateTags{{
    {CatalogAgentRegistrationState::registered, "registered"},
    {CatalogAgentRegistrationState::disabled, "disabled"}}};
constexpr std::optional<CatalogAgentRegistrationState> CatalogAgentRegistrationStateForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentRegistrationStateTags, tag);
}

// Bootstrap registration managed_by (agent_bootstrap_configuration.hpp).
enum class CatalogAgentManagedBy : scratchbird::core::platform::u16 { none = 0, ipc_server = 1, agent_coordinator = 2 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentManagedBy>, 2> kCatalogAgentManagedByTags{{
    {CatalogAgentManagedBy::ipc_server, "ipc_server"},
    {CatalogAgentManagedBy::agent_coordinator, "agent_coordinator"}}};
constexpr std::optional<CatalogAgentManagedBy> CatalogAgentManagedByForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentManagedByTags, tag);
}

// Bootstrap registration launch_trigger (agent_bootstrap_configuration.hpp).
enum class CatalogAgentLaunchTrigger : scratchbird::core::platform::u16 {
  none = 0, server_open = 1, coordinator_startup = 2, coordinator_shutdown = 3, policy_schedule = 4 };
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentLaunchTrigger>, 4> kCatalogAgentLaunchTriggerTags{{
    {CatalogAgentLaunchTrigger::server_open, "server_open"},
    {CatalogAgentLaunchTrigger::coordinator_startup, "coordinator_startup"},
    {CatalogAgentLaunchTrigger::coordinator_shutdown, "coordinator_shutdown"},
    {CatalogAgentLaunchTrigger::policy_schedule, "policy_schedule"}}};
constexpr std::optional<CatalogAgentLaunchTrigger> CatalogAgentLaunchTriggerForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentLaunchTriggerTags, tag);
}

// Agent scope as a bit set: one flag per path segment that appears in the
// manifest scope literals (agent_runtime_manifest.def) and the lifecycle scope
// (agent_engine_lifecycle.cpp), each listed once in first-appearance order.
enum class CatalogAgentScope : scratchbird::core::platform::u64 {
  none = 0,
  node = 1ull << 0,
  database = 1ull << 1,
  cluster = 1ull << 2,
  filespace = 1ull << 3,
  page_family = 1ull << 4,
  page_type = 1ull << 5,
  session = 1ull << 6,
  workload = 1ull << 7,
  index = 1ull << 8,
  parser = 1ull << 9,
  interface_ = 1ull << 10,  // segment "interface" (a macro in some Windows headers)
  row_version = 1ull << 11,
  query = 1ull << 12,
  route = 1ull << 13,
  optimizer = 1ull << 14,
  support = 1ull << 15,
  jobs = 1ull << 16,
  backup = 1ull << 17,
  archive = 1ull << 18,
  restore = 1ull << 19,
  pitr = 1ull << 20,
  security = 1ull << 21,
  export_ = 1ull << 22,  // segment "export"
  upgrade = 1ull << 23,
  runtime_lifecycle = 1ull << 24,
};
constexpr CatalogAgentScope operator|(CatalogAgentScope a, CatalogAgentScope b) noexcept {
  return static_cast<CatalogAgentScope>(static_cast<scratchbird::core::platform::u64>(a) |
                                        static_cast<scratchbird::core::platform::u64>(b));
}
constexpr CatalogAgentScope operator&(CatalogAgentScope a, CatalogAgentScope b) noexcept {
  return static_cast<CatalogAgentScope>(static_cast<scratchbird::core::platform::u64>(a) &
                                        static_cast<scratchbird::core::platform::u64>(b));
}
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentScope>, 25> kCatalogAgentScopeTags{{
    {CatalogAgentScope::node, "node"},
    {CatalogAgentScope::database, "database"},
    {CatalogAgentScope::cluster, "cluster"},
    {CatalogAgentScope::filespace, "filespace"},
    {CatalogAgentScope::page_family, "page_family"},
    {CatalogAgentScope::page_type, "page_type"},
    {CatalogAgentScope::session, "session"},
    {CatalogAgentScope::workload, "workload"},
    {CatalogAgentScope::index, "index"},
    {CatalogAgentScope::parser, "parser"},
    {CatalogAgentScope::interface_, "interface"},
    {CatalogAgentScope::row_version, "row_version"},
    {CatalogAgentScope::query, "query"},
    {CatalogAgentScope::route, "route"},
    {CatalogAgentScope::optimizer, "optimizer"},
    {CatalogAgentScope::support, "support"},
    {CatalogAgentScope::jobs, "jobs"},
    {CatalogAgentScope::backup, "backup"},
    {CatalogAgentScope::archive, "archive"},
    {CatalogAgentScope::restore, "restore"},
    {CatalogAgentScope::pitr, "pitr"},
    {CatalogAgentScope::security, "security"},
    {CatalogAgentScope::export_, "export"},
    {CatalogAgentScope::upgrade, "upgrade"},
    {CatalogAgentScope::runtime_lifecycle, "runtime_lifecycle"}}};
// One segment.
constexpr std::optional<CatalogAgentScope> CatalogAgentScopeForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentScopeTags, tag);
}
// A whole "a/b/c" scope path; nullopt if empty, if any segment is empty or
// unknown, or if a segment repeats.
constexpr std::optional<CatalogAgentScope> CatalogAgentScopeForPath(std::string_view path) noexcept {
  if (path.empty()) return std::nullopt;
  auto result = CatalogAgentScope::none;
  while (true) {
    const auto slash = path.find('/');
    const auto segment = path.substr(0, slash);
    const auto flag = CatalogAgentScopeForTag(segment);
    if (!flag || (result & *flag) != CatalogAgentScope::none) return std::nullopt;
    result = result | *flag;
    if (slash == std::string_view::npos) return result;
    path.remove_prefix(slash + 1);
  }
}

// Invalid-policy behaviour: one value per distinct text returned by
// DefaultInvalidPolicyBehavior (agent_runtime.cpp, in its order, ending with
// its fall-through), then the lifecycle value (agent_engine_lifecycle.cpp).
// The English text is a comment only (it moves to the comments table later);
// it is NOT stored. The ForTag table holds it solely as the boundary spelling.
enum class CatalogAgentInvalidPolicyBehavior : scratchbird::core::platform::u16 {
  none = 0,
  mark_suitability_unknown = 1,               // "mark suitability unknown"
  reject_invalid_samples = 2,                 // "reject invalid samples"
  health_unknown_deny_destructive = 3,        // "health unknown and deny destructive actions"
  conservative_grants = 4,                    // "conservative grants"
  suppress_recommendations = 5,               // "suppress recommendations"
  conservative_throttle = 6,                  // "conservative throttle"
  deny_new_parser_assignment = 7,             // "deny new parser assignment"
  warning_only = 8,                           // "warning only"
  deny_cleanup = 9,                           // "cleanup denied"
  no_recommendation = 10,                     // "no recommendation"
  disable_correction = 11,                    // "disable correction"
  deny_support_bundle = 12,                   // "deny bundle"
  deny_job_control_action = 13,               // "deny control action"
  progress_unknown_or_deny_start = 14,        // "progress unknown or deny start"
  hold_archive_slice = 15,                    // "hold slice"
  deny_restore_drill = 16,                    // "deny drill"
  report_pitr_unreachable = 17,               // "report unreachable"
  security_unknown_or_deny_action = 18,       // "security unknown or deny action"
  deny_session_action = 19,                   // "deny session action"
  local_event_only = 20,                      // "local event only"
  deny_export = 21,                           // "deny export"
  deny_filespace_mutation_recommend_only = 22,  // "deny filespace mutation; emit recommendation/refusal only"
  no_promotion_recommendation = 23,           // "no promotion recommendation"
  suppress_live_preallocation = 24,           // "suppress live preallocation"
  deny_relocation_publish_blockers_only = 25,  // "deny relocation; publish blockers only"
  fail_closed = 26,                           // "fail closed" (DefaultInvalidPolicyBehavior fall-through)
  deny_lifecycle_admission = 27,              // "deny_lifecycle_admission" (lifecycle agents)
};
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentInvalidPolicyBehavior>, 27>
    kCatalogAgentInvalidPolicyBehaviorTags{{
        {CatalogAgentInvalidPolicyBehavior::mark_suitability_unknown, "mark suitability unknown"},
        {CatalogAgentInvalidPolicyBehavior::reject_invalid_samples, "reject invalid samples"},
        {CatalogAgentInvalidPolicyBehavior::health_unknown_deny_destructive, "health unknown and deny destructive actions"},
        {CatalogAgentInvalidPolicyBehavior::conservative_grants, "conservative grants"},
        {CatalogAgentInvalidPolicyBehavior::suppress_recommendations, "suppress recommendations"},
        {CatalogAgentInvalidPolicyBehavior::conservative_throttle, "conservative throttle"},
        {CatalogAgentInvalidPolicyBehavior::deny_new_parser_assignment, "deny new parser assignment"},
        {CatalogAgentInvalidPolicyBehavior::warning_only, "warning only"},
        {CatalogAgentInvalidPolicyBehavior::deny_cleanup, "cleanup denied"},
        {CatalogAgentInvalidPolicyBehavior::no_recommendation, "no recommendation"},
        {CatalogAgentInvalidPolicyBehavior::disable_correction, "disable correction"},
        {CatalogAgentInvalidPolicyBehavior::deny_support_bundle, "deny bundle"},
        {CatalogAgentInvalidPolicyBehavior::deny_job_control_action, "deny control action"},
        {CatalogAgentInvalidPolicyBehavior::progress_unknown_or_deny_start, "progress unknown or deny start"},
        {CatalogAgentInvalidPolicyBehavior::hold_archive_slice, "hold slice"},
        {CatalogAgentInvalidPolicyBehavior::deny_restore_drill, "deny drill"},
        {CatalogAgentInvalidPolicyBehavior::report_pitr_unreachable, "report unreachable"},
        {CatalogAgentInvalidPolicyBehavior::security_unknown_or_deny_action, "security unknown or deny action"},
        {CatalogAgentInvalidPolicyBehavior::deny_session_action, "deny session action"},
        {CatalogAgentInvalidPolicyBehavior::local_event_only, "local event only"},
        {CatalogAgentInvalidPolicyBehavior::deny_export, "deny export"},
        {CatalogAgentInvalidPolicyBehavior::deny_filespace_mutation_recommend_only,
         "deny filespace mutation; emit recommendation/refusal only"},
        {CatalogAgentInvalidPolicyBehavior::no_promotion_recommendation, "no promotion recommendation"},
        {CatalogAgentInvalidPolicyBehavior::suppress_live_preallocation, "suppress live preallocation"},
        {CatalogAgentInvalidPolicyBehavior::deny_relocation_publish_blockers_only, "deny relocation; publish blockers only"},
        {CatalogAgentInvalidPolicyBehavior::fail_closed, "fail closed"},
        {CatalogAgentInvalidPolicyBehavior::deny_lifecycle_admission, "deny_lifecycle_admission"}}};
constexpr std::optional<CatalogAgentInvalidPolicyBehavior> CatalogAgentInvalidPolicyBehaviorForTag(
    std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kCatalogAgentInvalidPolicyBehaviorTags, tag);
}
}  // namespace scratchbird::core::catalog
