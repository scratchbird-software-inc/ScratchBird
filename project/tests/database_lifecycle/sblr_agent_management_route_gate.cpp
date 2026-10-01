#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_evidence_fixture.hpp"
#include "../support/component_authorization_fixture.hpp"
#include "database_lifecycle_test_memory.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_action_hooks_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"
#include "storage/storage_management_api.hpp"
#include "uuid.hpp"
#include "filespace_bootstrap.hpp"
#include "behavior_support/api_behavior_record_codec.hpp"
#include "catalog/binary_catalog_metadata.hpp"
#include "agent_runtime.hpp"
#include "metric_builtin_definitions.hpp"
#include "metric_contracts.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <unistd.h>
namespace {
unsigned growth_fault=0, growth_syncs=0, growth_reads=0, growth_writes=0;
int growth_fd=-1;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t bytes,off_t offset) {
  if(growth_fault && offset==0 && bytes==256) {
    growth_fd=fd; ++growth_reads;
    if(growth_fault==4 && growth_syncs==2) { errno=EIO; return -1; }
  }
  return __real_pread(fd,data,bytes,offset);
}
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t bytes,off_t offset) {
  if(growth_fault && fd==growth_fd) {
    ++growth_writes;
    if(growth_fault==2 && offset==0) { errno=EIO; return -1; }
  }
  return __real_pwrite(fd,data,bytes,offset);
}
extern "C" int __wrap_fsync(int fd) {
  if(growth_fault && fd==growth_fd) {
    ++growth_syncs;
    if((growth_fault==1 && growth_syncs==1) || (growth_fault==3 && growth_syncs==2)) {
      errno=EIO; return -1;
    }
  }
  return __real_fsync(fd);
}
#endif

namespace {

namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

void RegisterComponentMetricDescriptors() {
  // Explicit component inputs, not proof of an active producer or of native
  // metric catalog publication. Snapshot inputs remain in the requests below.
  namespace metrics=scratchbird::core::metrics;
  const auto definitions=metrics::BuiltinMetricDescriptorDefinitions();
  unsigned ordinal=1;
  for(const auto* name:{"filespace_capacity_manager","page_allocation_manager"}) {
    const auto agent=scratchbird::core::agents::FindAgentType(name);
    Require(agent.has_value(),"storage agent descriptor missing");
    for(const auto& dependency:agent->metric_dependencies) {
      if(dependency.cluster_only || metrics::DefaultMetricRegistry().FindDescriptorOrAlias(dependency.metric_family))
        continue;
      const auto found=std::find_if(definitions.begin(),definitions.end(),
          [&](const auto& definition){return definition.family==dependency.metric_family;});
      Require(found!=definitions.end(),"storage metric builtin missing");
      metrics::MetricDescriptor descriptor;
      static_cast<metrics::MetricDescriptorDefinition&>(descriptor)=*found;
      descriptor.metric_uuid=scratchbird::tests::FixtureUuid(2513,ordinal++);
      descriptor.descriptor_generation=1;
      descriptor.label_schema_uuid=scratchbird::tests::FixtureUuid(2513,ordinal++);
      descriptor.label_schema_generation=1;
      descriptor.retention_policy_uuid=scratchbird::tests::FixtureUuid(2513,1000);
      descriptor.retention_policy_generation=1;
      descriptor.visibility_policy_uuid=scratchbird::tests::FixtureUuid(2513,1001);
      descriptor.visibility_policy_generation=1;
      descriptor.readiness=metrics::MetricReadiness::contract_ready_unwired;
      const auto registered=metrics::DefaultMetricRegistry().RegisterDescriptor(descriptor);
      Require(registered.ok,"component storage metric registration failed: "+registered.diagnostic_code+":"+registered.detail);
    }
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "PFAR-013 UUID generation failed");
  return generated.value;
}

platform::Uuid MakeIdentity(platform::UuidKind kind, platform::u64 salt) {
  return MakeUuid(kind, salt).value;
}

std::string IdentityBytes(const platform::Uuid& identity) {
  return {reinterpret_cast<const char*>(identity.bytes.data()), identity.bytes.size()};
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  platform::Uuid database_uuid;
  platform::Uuid filespace_uuid;
  platform::Uuid transaction_uuid;
  platform::Uuid policy_uuid;
  platform::Uuid agent_uuid;
  platform::Uuid principal_uuid;
  platform::u64 local_transaction_id=9001, resource_epoch=17;

  Fixture()=default;
  Fixture(const Fixture&)=delete;
  Fixture& operator=(const Fixture&)=delete;
  Fixture(Fixture&& other) noexcept
      : dir(std::exchange(other.dir,{})), database_path(std::move(other.database_path)),
        database_uuid(other.database_uuid), filespace_uuid(other.filespace_uuid),
        transaction_uuid(other.transaction_uuid), policy_uuid(other.policy_uuid),
        agent_uuid(other.agent_uuid), principal_uuid(other.principal_uuid),
        local_transaction_id(other.local_transaction_id), resource_epoch(other.resource_epoch) {}

  ~Fixture() {
    std::error_code ignored;
    if(!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

Fixture MakeFixture(std::string_view name, platform::u64 salt) {
  Fixture fixture;
  const auto base = std::filesystem::temp_directory_path() /
                ("scratchbird_pfar013_" + std::string(name) + "_" +
                 std::to_string(NowMillis() + salt));
  for(unsigned attempt=0;attempt<1024;++attempt) {
    const auto candidate=base.string()+"_"+std::to_string(attempt);
    std::error_code error;
    if(std::filesystem::create_directory(candidate,error)) {fixture.dir=candidate;break;}
    Require(!error || error==std::errc::file_exists,"cannot create isolated fixture directory");
  }
  Require(!fixture.dir.empty(),"isolated fixture directory attempts exhausted");
  fixture.database_path = fixture.dir / "pfar013.sbdb";
  fixture.database_uuid = MakeIdentity(platform::UuidKind::database, salt + 1);
  fixture.filespace_uuid = MakeIdentity(platform::UuidKind::filespace, salt + 2);
  fixture.transaction_uuid = MakeIdentity(platform::UuidKind::transaction, salt + 3);
  fixture.policy_uuid = MakeIdentity(platform::UuidKind::object, salt + 4);
  fixture.agent_uuid = MakeIdentity(platform::UuidKind::object, salt + 5);
  fixture.principal_uuid = MakeIdentity(platform::UuidKind::principal, salt + 6);
  return fixture;
}

api::EngineRequestContext Context(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.principal_uuid = fixture.principal_uuid;
  context.session_uuid = scratchbird::tests::FixtureUuid(1208, 2401);
  context.transaction_uuid = fixture.transaction_uuid;
  context.local_transaction_id = fixture.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id = fixture.local_transaction_id;
  context.security_context_present = true;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.catalog_generation_id = 11;
  context.security_epoch = 13;
  context.resource_epoch = fixture.resource_epoch;
  scratchbird::tests::MaterializeComponentAuthorization(context,
      {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL", "FILESPACE_LIFECYCLE_CONTROL"});
  return context;
}

void SeedFilespaceCatalogDescriptor(Fixture& fixture) {
  namespace db=scratchbird::storage::database;
  db::DatabaseCreateConfig create;
  create.path=fixture.database_path.string();
  create.database_uuid={platform::UuidKind::database,fixture.database_uuid};
  create.filespace_uuid=MakeUuid(platform::UuidKind::filespace,20000);
  create.page_size=16384;
  create.creation_unix_epoch_millis=NowMillis();
  create.resource_seed_pack_root=(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/
      "resources/seed-packs/initial-resource-pack").string();
  create.allow_minimal_resource_bootstrap=false;
  create.require_resource_seed_pack=true;
  const auto created=db::CreateDatabaseFile(create);
  Require(created.ok(),"real filespace catalog fixture creation failed: "+created.diagnostic.diagnostic_code+
      ":"+created.diagnostic.message_key);
  fixture.resource_epoch=created.state.resource_seed_catalog.resource_epoch;
  auto inventory=db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  Require(inventory.ok() && inventory.inventory.publication_base.has_value(),
          "real filespace catalog inventory missing");
  const auto begun=scratchbird::transaction::mga::BeginLocalTransaction(std::move(inventory.inventory),
      {platform::UuidKind::transaction,fixture.transaction_uuid},NowMillis());
  Require(begun.ok(),"real filespace catalog transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(fixture.database_path.string(),begun.inventory).ok(),
          "real filespace catalog transaction publication failed");
  fixture.local_transaction_id=begun.entry.identity.local_id.value;
  api::EngineFilespaceLifecycleRequest request;
  request.context = Context(fixture, "seed-filespace-catalog-descriptor");
  request.operation_id = "filespace.create";
  request.target_object.uuid = fixture.filespace_uuid;
  request.target_object.object_kind = "filespace";
  request.option_envelopes.push_back("filespace.path:" +
                                     (fixture.dir / "fixture.filespace").string());
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.total_pages:64");
  request.option_envelopes.push_back("filespace.free_pages:32");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  const auto result = api::EngineFilespaceLifecycleOperation(request);
  Require(result.ok, "PFAR-013 filespace catalog descriptor seed failed");
}

api::EngineObjectReference FilespaceTarget(const Fixture& fixture) {
  api::EngineObjectReference target;
  target.uuid = fixture.filespace_uuid;
  target.object_kind = "filespace";
  return target;
}

void AddCommonAgentFields(api::EngineAgentActionHookRequest* request,
                          const Fixture& fixture,
                          std::string agent_type,
                          std::string action_class) {
  request->agent_type = std::move(agent_type);
  request->action_class = std::move(action_class);
  request->agent_uuid = fixture.agent_uuid;
  request->policy_snapshot_uuid = fixture.policy_uuid;
  request->target_filespace = FilespaceTarget(fixture);
  request->safety_fence_result = "passed";
  request->policy_authorized = true;
  request->evidence_sink_available = true;
  request->metrics_fresh = true;
  request->option_envelopes.push_back("wall_now_us:100");
  request->option_envelopes.push_back("monotonic_now_us:100");
  request->option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request->option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request->option_envelopes.push_back("agent_metric_snapshot_trust_provenance:test_metric_registry");
  request->option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" + IdentityBytes(fixture.database_uuid));
  request->option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  request->option_envelopes.push_back("agent_metric_snapshot_source_id:sblr-agent-route-source:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_source_sequence:1");
  request->option_envelopes.push_back("agent_metric_snapshot_digest:sha256:sblr-agent-route:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_value_digest:sha256:sblr-agent-route-value:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_schema_digest:sha256:sblr-agent-route-schema:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:sblr-agent-route-key:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:sblr-agent-route-attest:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  request->option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_provenance_record:sblr-agent-route-provenance:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_id:sblr-agent-route:" +
                                      agent_type);
  request->option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" +
                                      IdentityBytes(fixture.agent_uuid));
}

void AddObservedMetricSnapshotFields(api::EngineApiRequest* request,
                                     const Fixture& fixture,
                                     std::string_view agent_type) {
  request->option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request->option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request->option_envelopes.push_back("agent_metric_snapshot_trust_provenance:test_metric_registry");
  request->option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" +
                                      IdentityBytes(fixture.database_uuid));
  request->option_envelopes.push_back("agent_metric_snapshot_source_count:2");
  request->option_envelopes.push_back("agent_metric_snapshot_source_id:sblr-agent-route-source:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_source_sequence:1");
  request->option_envelopes.push_back("agent_metric_snapshot_digest:sha256:sblr-agent-route:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_value_digest:sha256:sblr-agent-route-value:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back(
      "agent_metric_snapshot_schema_digest:sha256:sblr-agent-route-schema:" +
      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_key_id:sblr-agent-route-key:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_digest:sha256:sblr-agent-route-attest:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_attestation_verified:true");
  request->option_envelopes.push_back("agent_metric_snapshot_redacted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_provenance_record:sblr-agent-route-provenance:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_id:sblr-agent-route:" +
                                      std::string(agent_type));
  request->option_envelopes.push_back("agent_metric_snapshot_evidence_uuid:" +
                                      IdentityBytes(fixture.agent_uuid));
}

api::EngineRequestPagePreallocationRequest PageRequest(const Fixture& fixture,
                                                       std::string request_id) {
  api::EngineRequestPagePreallocationRequest request;
  request.context = Context(fixture, std::move(request_id));
  AddCommonAgentFields(&request, fixture, "page_allocation_manager", "page_preallocation_request");
  request.page_family = "data";
  request.page_type = "relation";
  request.requested_pages = 6;
  return request;
}

api::EngineRequestFilespaceGrowthRequest FilespaceRequest(const Fixture& fixture,
                                                          std::string request_id,
                                                          platform::u32 page_size=16384) {
  api::EngineRequestFilespaceGrowthRequest request;
  request.context = Context(fixture, std::move(request_id));
  AddCommonAgentFields(&request, fixture, "filespace_capacity_manager", "filespace_growth_request");
  request.requested_bytes = 12 * page_size;
  request.option_envelopes.push_back("filespace.page_size_bytes:"+std::to_string(page_size));
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  return request;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) {
      continue;
    }
    if (id.empty() || scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, id)) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result, std::string_view field) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == field) {
        return value.encoded_value;
      }
    }
  }
  return {};
}

void RequireField(const api::EngineApiResult& result,
                  std::string_view field,
                  std::string_view expected) {
  const auto actual = FieldValue(result, field);
  Require(actual == expected,
          std::string(field) + " mismatch: expected " + std::string(expected) +
              " got " + actual);
}

void RequireUuidField(const api::EngineApiResult& result, std::string_view field) {
  for(const auto& row:result.result_shape.rows) for(const auto& [name,value]:row.fields) {
    if(name!=field) continue;
    Require(value.encoded_value.empty() && value.binary_value.size()==16 &&
                value.descriptor.canonical_type_name=="uuid" && !value.isNull(),
            std::string(field)+" is not a native binary UUID value");
    platform::Uuid identity;
    std::copy(value.binary_value.begin(),value.binary_value.end(),identity.bytes.begin());
    Require(uuid::IsEngineIdentityUuid(identity),std::string(field)+" is not an engine identity");
    return;
  }
  Fail(std::string(field)+" missing");
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

void DumpDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
  }
}

sblr::SblrDispatchResult DispatchWithContext(api::EngineRequestContext context,
                                             std::string operation_id,
                                             std::string opcode,
                                             api::EngineApiRequest api_request,
                                             bool requires_security = true,
                                             bool requires_transaction = true) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "pfar-013");
  const auto* registry = sblr::LookupSblrOperation(envelope.operation_id);
  Require(registry != nullptr,
          "SBLR agent route canonical registry row missing: " +
              envelope.operation_id);
  Require(registry->opcode == envelope.opcode,
          "SBLR agent route canonical opcode mismatch");
  envelope.opcode_code = registry->code;
  envelope.parser_package_uuid = MakeIdentity(platform::UuidKind::object, 97);
  envelope.registry_snapshot_uuid = MakeIdentity(platform::UuidKind::object, 98);
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_transaction_context = requires_transaction;
  envelope.requires_security_context = requires_security;
  const sblr::SblrDispatchRequest request{std::move(context), envelope, std::move(api_request)};
  auto result = sblr::DispatchSblrOperation(request);
  if (!result.api_result.ok) {
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  return result;
}

sblr::SblrDispatchResult Dispatch(const Fixture& fixture,
                                  std::string operation_id,
                                  std::string opcode,
                                  api::EngineApiRequest api_request,
                                  std::string request_id) {
  return DispatchWithContext(Context(fixture, std::move(request_id)),
                             std::move(operation_id),
                             std::move(opcode),
                             std::move(api_request));
}

api::EngineApiRequest PageSblrApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.related_objects.push_back(FilespaceTarget(fixture));
  request.option_envelopes.push_back("agent_uuid:" + IdentityBytes(fixture.agent_uuid));
  request.option_envelopes.push_back("policy_snapshot_uuid:" + IdentityBytes(fixture.policy_uuid));
  request.option_envelopes.push_back("policy_authorized:true");
  request.option_envelopes.push_back("evidence_sink_available:true");
  request.option_envelopes.push_back("metrics_fresh:true");
  request.option_envelopes.push_back("safety_fence_result:passed");
  request.option_envelopes.push_back("page_family:data");
  request.option_envelopes.push_back("page_type:relation");
  request.option_envelopes.push_back("requested_pages:4");
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  AddObservedMetricSnapshotFields(&request, fixture, "page_allocation_manager");
  return request;
}

api::EngineApiRequest FilespaceSblrApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.related_objects.push_back(FilespaceTarget(fixture));
  request.option_envelopes.push_back("agent_uuid:" + IdentityBytes(fixture.agent_uuid));
  request.option_envelopes.push_back("policy_snapshot_uuid:" + IdentityBytes(fixture.policy_uuid));
  request.option_envelopes.push_back("policy_authorized:true");
  request.option_envelopes.push_back("evidence_sink_available:true");
  request.option_envelopes.push_back("metrics_fresh:true");
  request.option_envelopes.push_back("safety_fence_result:passed");
  request.option_envelopes.push_back("requested_bytes:196608");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  AddObservedMetricSnapshotFields(&request, fixture, "filespace_capacity_manager");
  return request;
}

api::EngineApiRequest FilespaceSblrPagesApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.related_objects.push_back(FilespaceTarget(fixture));
  request.option_envelopes.push_back("agent_uuid:" + IdentityBytes(fixture.agent_uuid));
  request.option_envelopes.push_back("policy_snapshot_uuid:" + IdentityBytes(fixture.policy_uuid));
  request.option_envelopes.push_back("policy_authorized:true");
  request.option_envelopes.push_back("evidence_sink_available:true");
  request.option_envelopes.push_back("metrics_fresh:true");
  request.option_envelopes.push_back("safety_fence_result:passed");
  request.option_envelopes.push_back("requested_pages:5");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("wall_now_us:100");
  request.option_envelopes.push_back("monotonic_now_us:100");
  AddObservedMetricSnapshotFields(&request, fixture, "filespace_capacity_manager");
  return request;
}

api::EngineApiRequest FilespacePreallocateSblrApiRequest(const Fixture& fixture) {
  api::EngineApiRequest request;
  request.target_object = FilespaceTarget(fixture);
  request.option_envelopes.push_back("requested_pages:12");
  request.option_envelopes.push_back("filespace.page_size_bytes:16384");
  request.option_envelopes.push_back("filespace.current_pages:64");
  request.option_envelopes.push_back("filespace.preallocated_pages:4");
  request.option_envelopes.push_back("filespace.maximum_pages:256");
  request.option_envelopes.push_back("evidence_sink_available:true");
  return request;
}

void TestApiPagePreallocationStorageMutation() {
  const auto fixture = MakeFixture("api_page", 1000);
  const auto result = api::EngineRequestPagePreallocation(PageRequest(fixture, "api-page-live"));
  if (!result.ok) {
    DumpDiagnostics(result);
  }
  Require(result.ok, "API page preallocation failed");
  Require(HasEvidence(result, "agent_hook", "agents.request_page_preallocation"),
          "API page hook evidence missing");
  Require(HasEvidence(result, "storage_executor", "PreallocatePageFamilyPool"),
          "API page storage executor evidence missing");
  RequireUuidField(result, "page_preallocation_allocation_uuid");
  RequireField(result, "storage_execution", "completed");
  RequireField(result, "page_preallocation_ledger_mutated", "true");
  RequireField(result, "page_preallocation_state", "preallocated");
  RequireField(result, "page_preallocation_diagnostic", "SB-STORAGE-PAGE-PREALLOCATION-PREALLOCATED");
  RequireField(result, "page_preallocation_evidence_action", "preallocate_page_family_pool");
  RequireField(result, "page_preallocation_durable_state_changed", "true");
  RequireField(result, "page_preallocation_capacity_evidence_accepted", "true");
}

void TestSblrPagePreallocationStorageMutation() {
  const auto fixture = MakeFixture("sblr_page", 2000);
  const auto result = Dispatch(fixture,
                               "agents.request_page_preallocation",
                               "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION",
                               PageSblrApiRequest(fixture),
                               "sblr-page-live");
  Require(result.accepted && result.dispatched_to_api, "SBLR page route was not dispatched");
  Require(result.api_result.ok, "SBLR page preallocation API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "PreallocatePageFamilyPool"),
          "SBLR page storage executor evidence missing");
  RequireUuidField(result.api_result, "page_preallocation_allocation_uuid");
  RequireField(result.api_result, "page_preallocation_ledger_mutated", "true");
  RequireField(result.api_result, "page_preallocation_state", "preallocated");
}

void TestApiFilespaceGrowthStorageMutation() {
  const auto fixture = MakeFixture("api_filespace", 3000);
  const auto result = api::EngineRequestFilespaceGrowth(FilespaceRequest(fixture, "api-filespace-live"));
  if (!result.ok) {
    DumpDiagnostics(result);
  }
  Require(result.ok, "API filespace growth failed");
  Require(result.storage_result && result.storage_result->ok() &&
              result.storage_result->operation.database_uuid.value==fixture.database_uuid &&
              result.storage_result->operation.filespace_uuid.value==fixture.filespace_uuid,
          "API lost typed storage success or native owner identities");
  Require(HasEvidence(result, "storage_executor", "ExecuteFilespacePhysicalGrowth"),
          "API filespace storage executor evidence missing");
  RequireUuidField(result, "filespace_growth_operation_uuid");
  RequireField(result, "storage_execution", "completed");
  RequireField(result, "filespace_growth_ledger_mutated", "true");
  RequireField(result, "filespace_growth_state", "completed");
  RequireField(result, "filespace_growth_diagnostic", "ok");
  RequireField(result, "filespace_growth_evidence_action", "filespace_physical_growth_commit");
  RequireField(result, "filespace_growth_requested_pages", "12");
  RequireField(result, "filespace_growth_grown_pages", "12");
  RequireField(result, "filespace_growth_durable_state_changed", "true");
  RequireField(result, "filespace_growth_physical_extension_completed", "true");
  RequireField(result, "filespace_growth_physical_extension_synced", "true");
  RequireField(result, "filespace_growth_physical_header_updated", "true");
  RequireField(result, "filespace_growth_metadata_after_physical_extension", "true");
  RequireField(result, "filespace_growth_page_allocation_authority_bypassed", "false");
  std::ifstream events(fixture.database_path.string()+".sb.api_events.v2",std::ios::binary);
  api::ApiBehaviorRecord record;
  Require(api::ReadApiBehaviorRecord(events,&record) && record.object_uuid==fixture.agent_uuid &&
              record.target_database_uuid==fixture.database_uuid &&
              record.target_object_uuid==fixture.filespace_uuid && record.state=="observed",
          "actual hook evidence is not a framed binary owner/target record");
  api::BinaryCatalogMetadata payload;
  Require(api::DecodeBinaryCatalogMetadata(record.payload,"agent.hook.event.v2",&payload) &&
              payload.identities.at("target_uuid")==fixture.filespace_uuid &&
              payload.identities.at("policy_snapshot_uuid")==fixture.policy_uuid,
          "persisted hook payload lost native identities");
}

void TestApiFilespaceGrowthRetainsEvidenceFailure() {
  const auto fixture=MakeFixture("growth_evidence_failure",12000);
  const auto request=FilespaceRequest(fixture,"growth-evidence-failure");
  // A directory at the event-file path produces a real append error, after
  // successful member extension. Do not replace this with a fake result.
  std::filesystem::create_directory(fixture.database_path.string()+".sb.api_events.v2");
  const auto failed=api::EngineRequestFilespaceGrowth(request);
  Require(!failed.ok && failed.storage_result && failed.storage_result->ok() &&
              failed.storage_result->operation.physical_extension_synced &&
              failed.storage_result->operation.physical_header_updated &&
              !failed.diagnostics.empty(),
          "later evidence failure erased an actual successful storage outcome");
  const auto& operation=failed.storage_result->operation;
  Require(operation.admitted_request &&
              std::filesystem::file_size(operation.admitted_request->member_capacity.physical_path)==80*16384,
          "evidence failure did not retain real member extension");

  const auto denied_fixture=MakeFixture("growth_trace_not_authority",12001);
  auto denied=FilespaceRequest(denied_fixture,"trace-not-authority");
  denied.context.authorization_context={};
  denied.context.trace_tags={"security.fixture_trace_authority","right:OBS_AGENT_CONTROL",
                            "right:FILESPACE_LIFECYCLE_CONTROL"};
  const auto refused=api::EngineRequestFilespaceGrowth(denied);
  Require(!refused.ok && !refused.storage_result && std::filesystem::is_empty(denied_fixture.dir),
          "trace strings bypassed materialized authorization or touched storage");
}

void TestApiFilespaceGrowthRetainsFailure() {
#if defined(__linux__)
  namespace fs=scratchbird::storage::filespace;
  for(const auto& profile:scratchbird::storage::disk::kCanonicalFilespacePageProfiles)
  for(unsigned fault=1;fault<=4;++fault) {
    const auto fixture=MakeFixture("growth_retained_failure",10000+fault);
    auto request=FilespaceRequest(fixture,"growth-before-failure",profile.page_size_bytes);
    // A real successful action establishes the member. Injection then targets
    // extension, not fixture creation or an unrelated catalog write.
    const auto initial=api::EngineRequestFilespaceGrowth(request);
    Require(initial.ok && initial.storage_result && initial.storage_result->ok() &&
                initial.storage_result->operation.admitted_request.has_value(),
            "failure fixture did not actually grow");
    const auto path=initial.storage_result->operation.admitted_request->member_capacity.physical_path;
    const auto size_before=std::filesystem::file_size(path);
    request.context.request_id="growth-with-failure";
    growth_fault=fault; growth_fd=-1; growth_reads=growth_writes=growth_syncs=0;
    const auto failed=api::EngineRequestFilespaceGrowth(request);
    growth_fault=0;
    Require(!failed.ok && failed.storage_result && !failed.storage_result->ok(),
            "failed engine action lost the typed storage failure");
    const auto& original=failed.storage_result->operation;
    Require(original.state==fs::FilespacePhysicalGrowthState::quarantine &&
                original.growth_operation_id.valid() && original.request_uuid.valid() &&
                original.admitted_request && original.database_uuid.value==fixture.database_uuid &&
                original.filespace_uuid.value==fixture.filespace_uuid,
            "engine action lost original binary operation/intent or quarantine");
    Require(original.physical_extension_completed &&
                original.physical_extension_synced==(fault!=1) &&
                original.physical_header_updated==(fault==4) &&
                !original.metadata_commit_after_physical_extension,
            "engine action collapsed retained physical effects");
    Require(std::filesystem::file_size(path)==size_before+12*profile.page_size_bytes,
            "injected failure did not leave actual physical growth");
    const auto read_bytes=[&] {std::ifstream in(path,std::ios::binary);
      Require(in.good(),"open independent growth image");
      std::vector<char> bytes(std::istreambuf_iterator<char>(in),{});
      Require(!in.bad() && bytes.size()==std::filesystem::file_size(path),
              "independent member image is incomplete");
      return bytes;};
    const auto retained_bytes=read_bytes();
    const auto diagnostic=failed.storage_result->diagnostic.diagnostic_code;
    Require(!diagnostic.empty() && HasDiagnostic(failed,diagnostic),
            "engine action lost original physical diagnostic");
    Require(failed.diagnostics.front().native_source.has_value() &&
                failed.diagnostics.front().native_source->record.diagnostic_code==diagnostic &&
                failed.diagnostics.front().native_source->record.message_key==
                    failed.storage_result->diagnostic.message_key &&
                failed.diagnostics.front().native_source->record.arguments.size()==
                    failed.storage_result->diagnostic.arguments.size(),
            "engine adapter lost the original structured native cause");
    const auto& cause=failed.diagnostics.front().native_source->record;
    for(std::size_t n=0;n<cause.arguments.size();++n)
      Require(cause.arguments[n].key==failed.storage_result->diagnostic.arguments[n].key &&
                  cause.arguments[n].value==failed.storage_result->diagnostic.arguments[n].value,
              "native diagnostic operand changed across the engine boundary");
    for(unsigned retry=0;retry<2;++retry) {
      if(retry) request.context.request_id="new-worker-tick";
      growth_fault=fault; growth_fd=-1; growth_reads=growth_writes=growth_syncs=0;
      const auto blocked=api::EngineRequestFilespaceGrowth(request);
      growth_fault=0;
      Require(!blocked.ok && blocked.storage_result &&
                  blocked.storage_result->diagnostic.diagnostic_code=="filespace_growth_quarantine" &&
                  blocked.storage_result->operation.state==fs::FilespacePhysicalGrowthState::quarantine &&
                  blocked.storage_result->operation.growth_operation_id.value==original.growth_operation_id.value &&
                  blocked.storage_result->operation.request_uuid.value==original.request_uuid.value &&
                  blocked.storage_result->operation.admitted_request &&
                  blocked.storage_result->operation.admitted_request->request_uuid.value==original.request_uuid.value,
              "retry invented a replacement operation or lost failed intent");
      Require(!growth_reads && !growth_writes && !growth_syncs,
              "unreconciled adapter retry reached physical member I/O");
      Require(read_bytes()==retained_bytes,"blocked retry changed actual member bytes");
    }
  }
#endif
}

void TestSblrFilespaceGrowthStorageMutation() {
  const auto fixture = MakeFixture("sblr_filespace", 4000);
  const auto result = Dispatch(fixture,
                               "agents.request_filespace_growth",
                               "SBLR_AGENT_REQUEST_FILESPACE_GROWTH",
                               FilespaceSblrApiRequest(fixture),
                               "sblr-filespace-live");
  Require(result.accepted && result.dispatched_to_api, "SBLR filespace route was not dispatched");
  Require(result.api_result.ok, "SBLR filespace growth API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "ExecuteFilespacePhysicalGrowth"),
          "SBLR filespace storage executor evidence missing");
  RequireUuidField(result.api_result, "filespace_growth_operation_uuid");
  RequireField(result.api_result, "filespace_growth_ledger_mutated", "true");
  RequireField(result.api_result, "filespace_growth_state", "completed");
  RequireField(result.api_result, "filespace_growth_grown_pages", "12");
  RequireField(result.api_result, "filespace_growth_physical_extension_completed", "true");
  RequireField(result.api_result, "filespace_growth_metadata_after_physical_extension", "true");
}

void TestSblrFilespaceGrowthAcceptsRequestedPages() {
  const auto fixture = MakeFixture("sblr_filespace_pages", 4500);
  const auto result = Dispatch(fixture,
                               "agents.request_filespace_growth",
                               "SBLR_AGENT_REQUEST_FILESPACE_GROWTH",
                               FilespaceSblrPagesApiRequest(fixture),
                               "sblr-filespace-pages-live");
  Require(result.accepted && result.dispatched_to_api,
          "SBLR filespace pages route was not dispatched");
  Require(result.api_result.ok, "SBLR filespace pages growth API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "ExecuteFilespacePhysicalGrowth"),
          "SBLR filespace pages storage executor evidence missing");
  RequireField(result.api_result, "filespace_growth_ledger_mutated", "true");
  RequireField(result.api_result, "filespace_growth_grown_pages", "5");
}

void TestSblrFilespacePreallocateStorageMutation() {
  auto fixture = MakeFixture("sblr_preallocate", 4600);
  SeedFilespaceCatalogDescriptor(fixture);
  const auto result = Dispatch(fixture,
                               "engine.op.filespace_preallocate",
                               "SBLR_FILESPACE_PREALLOCATE",
                               FilespacePreallocateSblrApiRequest(fixture),
                               "sblr-filespace-preallocate-live");
  Require(result.accepted && result.dispatched_to_api,
          "SBLR filespace preallocate was not dispatched");
  Require(result.api_result.ok, "SBLR filespace preallocate API failed");
  Require(HasEvidence(result.api_result, "storage_executor", "PreallocateFilespace"),
          "SBLR filespace preallocate storage executor evidence missing");
  RequireUuidField(result.api_result, "filespace_preallocation_operation_uuid");
  RequireField(result.api_result, "storage_execution", "completed");
  RequireField(result.api_result, "filespace_preallocation_ledger_mutated", "true");
  RequireField(result.api_result, "filespace_preallocation_state", "completed");
  RequireField(result.api_result, "filespace_preallocation_diagnostic", "ok");
  RequireField(result.api_result, "filespace_preallocation_evidence_action",
               "filespace_preallocate_commit");
  RequireField(result.api_result, "filespace_preallocation_requested_pages", "12");
  RequireField(result.api_result, "filespace_preallocation_pages", "12");
  RequireField(result.api_result, "filespace_preallocation_durable_state_changed", "true");
}

void TestSblrFilespacePreallocateNegativeCasesDoNotMutate() {
  auto fixture = MakeFixture("sblr_preallocate_negative", 4700);
  SeedFilespaceCatalogDescriptor(fixture);

  auto missing_security_context = Context(fixture, "preallocate-missing-security");
  missing_security_context.security_context_present = false;
  const auto missing_security = DispatchWithContext(
      missing_security_context,
      "engine.op.filespace_preallocate",
      "SBLR_FILESPACE_PREALLOCATE",
      FilespacePreallocateSblrApiRequest(fixture),
      false,
      true);
  Require(!missing_security.dispatched_to_api && !missing_security.accepted &&
              std::any_of(missing_security.diagnostics.begin(),missing_security.diagnostics.end(),
                  [](const auto& d){return d.code=="SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED";}),
          "missing security bypassed the canonical dispatcher boundary");
  api::EngineFilespacePreallocateRequest direct;
  static_cast<api::EngineApiRequest&>(direct)=FilespacePreallocateSblrApiRequest(fixture);
  direct.context=missing_security_context;
  const auto security_api=api::EngineFilespacePreallocate(direct);
  Require(!security_api.ok,"direct API accepted missing security");
  Require(HasDiagnostic(security_api, "AGENT.SECURITY_CONTEXT_REQUIRED"),
          "missing security diagnostic mismatch");
  RequireField(security_api,
               "filespace_preallocation_ledger_mutated",
               "false");

  auto missing_transaction_context = Context(fixture, "preallocate-missing-transaction");
  missing_transaction_context.local_transaction_id = 0;
  const auto missing_transaction = DispatchWithContext(
      missing_transaction_context,
      "engine.op.filespace_preallocate",
      "SBLR_FILESPACE_PREALLOCATE",
      FilespacePreallocateSblrApiRequest(fixture),
      true,
      false);
  Require(missing_transaction.dispatched_to_api && !missing_transaction.api_result.ok &&
              !missing_transaction.api_result.diagnostics.empty() &&
              missing_transaction.api_result.diagnostics.front().detail.find("local_transaction_id_required")!=
                  std::string::npos,
          "partial transaction identity did not reach and fail API admission");
  auto absent_transaction_context=missing_transaction_context;
  absent_transaction_context.transaction_uuid={};
  const auto absent_transaction=DispatchWithContext(absent_transaction_context,
      "engine.op.filespace_preallocate","SBLR_FILESPACE_PREALLOCATE",
      FilespacePreallocateSblrApiRequest(fixture),true,false);
  Require(!absent_transaction.dispatched_to_api && !absent_transaction.accepted &&
              std::any_of(absent_transaction.diagnostics.begin(),absent_transaction.diagnostics.end(),
                  [](const auto& d){return d.code=="SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED";}),
          "absent transaction bypassed the canonical dispatcher boundary");
  direct.context=missing_transaction_context;
  const auto transaction_api=api::EngineFilespacePreallocate(direct);
  Require(!transaction_api.ok,"direct API accepted missing transaction");
  Require(!transaction_api.diagnostics.empty() &&
              transaction_api.diagnostics.front().detail.find("local_transaction_id_required") !=
                  std::string::npos,
          "missing transaction diagnostic mismatch");

  auto insufficient_capacity = FilespacePreallocateSblrApiRequest(fixture);
  insufficient_capacity.option_envelopes.clear();
  insufficient_capacity.option_envelopes.push_back("requested_pages:12");
  insufficient_capacity.option_envelopes.push_back("filespace.page_size_bytes:16384");
  insufficient_capacity.option_envelopes.push_back("filespace.current_pages:64");
  insufficient_capacity.option_envelopes.push_back("filespace.preallocated_pages:4");
  insufficient_capacity.option_envelopes.push_back("filespace.maximum_pages:70");
  insufficient_capacity.option_envelopes.push_back("evidence_sink_available:true");
  const auto capacity = Dispatch(fixture,
                                 "engine.op.filespace_preallocate",
                                 "SBLR_FILESPACE_PREALLOCATE",
                                 insufficient_capacity,
                                 "preallocate-insufficient-capacity");
  Require(capacity.dispatched_to_api, "capacity refusal did not dispatch");
  Require(!capacity.api_result.ok, "insufficient capacity preallocate succeeded");
  Require(!capacity.api_result.diagnostics.empty() &&
              capacity.api_result.diagnostics.front().code ==
                  "filespace_preallocate_insufficient_capacity",
          "insufficient capacity diagnostic mismatch");

  const auto live = Dispatch(fixture,
                             "engine.op.filespace_preallocate",
                             "SBLR_FILESPACE_PREALLOCATE",
                             FilespacePreallocateSblrApiRequest(fixture),
                             "preallocate-live-after-negative");
  Require(live.api_result.ok, "live preallocate after negative cases failed");
  RequireField(live.api_result, "filespace_preallocation_evidence_sequence", "1");
}

void TestDryRunAndValidationFailuresDoNotMutateBeforeLiveRoute() {
  auto fixture = MakeFixture("negative_page", 5000);
  auto dry_run = PageRequest(fixture, "page-dry-run");
  dry_run.dry_run = true;
  const auto dry = api::EngineRequestPagePreallocation(dry_run);
  Require(dry.ok, "dry-run page route failed");
  RequireField(dry, "storage_execution", "dry_run");
  RequireField(dry, "page_preallocation_ledger_mutated", "false");
  Require(!HasEvidence(dry, "storage_executor", "PreallocatePageFamilyPool"),
          "dry-run emitted live storage executor evidence");

  auto missing_security = PageRequest(fixture, "page-missing-security");
  missing_security.context.security_context_present = false;
  const auto refused = api::EngineRequestPagePreallocation(missing_security);
  Require(!refused.ok, "missing security page route succeeded");
  Require(refused.refusal_reason == "security_context_required",
          "missing security refusal mismatch: " + refused.refusal_reason);

  auto missing_transaction = PageRequest(fixture, "page-missing-transaction");
  missing_transaction.context.local_transaction_id = 0;
  const auto no_transaction = api::EngineRequestPagePreallocation(missing_transaction);
  Require(!no_transaction.ok, "missing transaction page route succeeded");
  Require(no_transaction.refusal_reason == "local_transaction_id_required",
          "missing transaction refusal mismatch: " + no_transaction.refusal_reason);

  auto missing_evidence = PageRequest(fixture, "page-missing-evidence");
  missing_evidence.evidence_sink_available = false;
  const auto no_evidence = api::EngineRequestPagePreallocation(missing_evidence);
  Require(!no_evidence.ok, "missing evidence page route succeeded");
  Require(no_evidence.refusal_reason == "evidence_sink_required",
          "missing evidence refusal mismatch: " + no_evidence.refusal_reason);

  auto missing_metrics = PageRequest(fixture, "page-missing-metrics");
  missing_metrics.metrics_fresh = false;
  const auto no_metrics = api::EngineRequestPagePreallocation(missing_metrics);
  Require(!no_metrics.ok, "missing metrics page route succeeded");
  Require(no_metrics.refusal_reason == "metric_freshness_required",
          "missing metrics refusal mismatch: " + no_metrics.refusal_reason);

  const auto live = api::EngineRequestPagePreallocation(PageRequest(fixture, "page-live-after-negative"));
  Require(live.ok, "live page route after negative cases failed");
  RequireField(live, "page_preallocation_evidence_sequence", "1");

  const auto filespace_fixture = MakeFixture("negative_filespace", 6000);
  auto missing_policy = FilespaceRequest(filespace_fixture, "filespace-missing-policy");
  missing_policy.policy_authorized = false;
  const auto denied = api::EngineRequestFilespaceGrowth(missing_policy);
  Require(!denied.ok, "missing policy filespace route succeeded");
  Require(denied.refusal_reason == "policy_authorization_required",
          "missing policy refusal mismatch: " + denied.refusal_reason);

  auto dry_growth = FilespaceRequest(filespace_fixture, "filespace-dry-run");
  dry_growth.dry_run = true;
  const auto dry_growth_result = api::EngineRequestFilespaceGrowth(dry_growth);
  Require(dry_growth_result.ok, "dry-run filespace route failed");
  RequireField(dry_growth_result, "storage_execution", "dry_run");
  RequireField(dry_growth_result, "filespace_growth_ledger_mutated", "false");

  const auto live_growth = api::EngineRequestFilespaceGrowth(
      FilespaceRequest(filespace_fixture, "filespace-live-after-negative"));
  Require(live_growth.ok, "live filespace route after negative cases failed");
  RequireField(live_growth, "filespace_growth_evidence_sequence", "1");
}

}  // namespace

int main(int argc,char** argv) try {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("sblr_agent_management_route_gate");
  RegisterComponentMetricDescriptors();
  if(argc==2 && std::string_view(argv[1])=="--growth-retention-only") {
    TestApiFilespaceGrowthStorageMutation();
    TestApiFilespaceGrowthRetainsFailure();
    TestApiFilespaceGrowthRetainsEvidenceFailure();
    std::cout<<"PASS storage adapter actual growth failure/retry retention across five page profiles\n";
    return EXIT_SUCCESS;
  }
  Require(argc==1,"unexpected test arguments");
  TestApiPagePreallocationStorageMutation();
  TestSblrPagePreallocationStorageMutation();
  TestApiFilespaceGrowthStorageMutation();
  TestApiFilespaceGrowthRetainsFailure();
  TestApiFilespaceGrowthRetainsEvidenceFailure();
  TestSblrFilespaceGrowthStorageMutation();
  TestSblrFilespaceGrowthAcceptsRequestedPages();
  TestSblrFilespacePreallocateStorageMutation();
  TestSblrFilespacePreallocateNegativeCasesDoNotMutate();
  TestDryRunAndValidationFailuresDoNotMutateBeforeLiveRoute();
  return EXIT_SUCCESS;
} catch(const std::exception& error) {
#if defined(__linux__)
  growth_fault=0;
#endif
  std::cerr<<error.what()<<'\n';
  return EXIT_FAILURE;
}
