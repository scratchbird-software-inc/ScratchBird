// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_lifecycle.hpp"
#include "metric_builtin_definitions.hpp"
#include "metric_contracts.hpp"
#include "metric_label_key.hpp"
#include "metric_observation_queue.hpp"
#include "metric_value_codec.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace m = scratchbird::core::metrics;
namespace fs = scratchbird::storage::filespace;
namespace p = scratchbird::core::platform;
static_assert(!std::is_invocable_v<decltype(&m::PublishFilespaceHealthState), double,
    std::string,m::MetricUuid,m::MetricUuid,m::MetricUuid,std::string,std::string>);
namespace {
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
m::MetricUuid Id(unsigned value) {
  m::MetricUuid id;
  id.bytes = {1,159,0,0,0,0,112,0,128,0,0,0,0,0,
      static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
  return id;
}
constexpr std::array<const char*,17> names = {"unknown", "active_primary", "primary_shadow",
    "primary_snapshot", "primary_candidate", "secondary_data", "secondary_index",
    "secondary_overflow", "secondary_history", "secondary_shard", "archive_history",
    "archive_log", "archive_detached", "temporary", "import_candidate", "drop_pending", "forbidden"};
}
int main() {
  try {
    auto& registry = m::DefaultMetricRegistry();
    fs::FilespaceDescriptor filespace;
    filespace.database_uuid = {p::UuidKind::database, Id(1)};
    filespace.filespace_uuid = {p::UuidKind::filespace, Id(2)};
    filespace.state = fs::FilespaceState::online;
    Require(!registry.ObservationNodeForDatabase(Id(1)), "unbound registry invented a node");
    Require(!fs::PublishFilespaceRoleObservation(filespace).ok, "unbound role observation succeeded");
    auto made = m::MetricObservationQueue::Create({Id(1),Id(3),{}}, {64,1024*1024});
    Require(made.ok(), "create component queue");
    std::shared_ptr<m::MetricObservationQueue> queue = std::move(made.queue);
    Require(registry.BindObservationQueue(queue).ok, "bind component queue");
    Require(registry.ObservationNodeForDatabase(Id(1)) == Id(3) &&
        !registry.ObservationNodeForDatabase(Id(9)), "node owner selection crossed database");
    Require(!fs::PublishFilespaceRoleObservation(filespace).ok,
        "queue attachment invented a native descriptor or series");
    const auto definitions = m::BuiltinMetricDescriptorDefinitions();
    const auto found = std::find_if(definitions.begin(), definitions.end(),
        [](const auto& d) { return d.family == "sb_filespace_role_state"; });
    Require(found != definitions.end(), "missing role definition");
    m::MetricDescriptor descriptor;
    static_cast<m::MetricDescriptorDefinition&>(descriptor) = *found;
    descriptor.metric_uuid=Id(10); descriptor.descriptor_generation=2;
    descriptor.label_schema_uuid=Id(11); descriptor.label_schema_generation=3;
    descriptor.retention_policy_uuid=Id(12); descriptor.retention_policy_generation=4;
    descriptor.visibility_policy_uuid=Id(13); descriptor.visibility_policy_generation=5;
    descriptor.readiness=m::MetricReadiness::implemented;
    m::MetricRetentionPolicy policy;
    policy.policy_uuid=Id(12); policy.generation=4; policy.policy_name="component retained policy";
    Require(registry.RegisterDescriptor(descriptor).ok, "register component descriptor");
    const auto health_definition = std::find_if(definitions.begin(), definitions.end(),
        [](const auto& d) { return d.family == "sb_filespace_health_state"; });
    Require(health_definition != definitions.end(), "missing health definition");
    auto health = descriptor;
    static_cast<m::MetricDescriptorDefinition&>(health) = *health_definition;
    health.metric_uuid=Id(20); health.label_schema_uuid=Id(21);
    Require(registry.RegisterDescriptor(health).ok, "register health negative control");
    for (unsigned code=0; code<names.size(); ++code) {
      filespace.role=static_cast<fs::FilespaceRole>(code);
      Require(std::string(fs::FilespaceRoleName(filespace.role))==names[code], "role enum changed meaning");
      m::MetricLabelSet labels={{"component","storage.filespace"},{"database_uuid",Id(1)},
          {"filespace_uuid",Id(2)},{"node_uuid",Id(3)},{"filespace_role",names[code]},
          {"device_class","file"},{"state",names[code]}};
      m::MetricHistoryBinding binding;
      static_cast<m::MetricDescriptorBinding&>(binding)=descriptor;
      binding.database_uuid=Id(1); binding.node_uuid=Id(3);
      auto series=m::MakeMetricSeriesIdentity(descriptor,labels,policy,binding,Id(100+code),6);
      Require(series.ok() && registry.RegisterSeries(*series.record,policy).ok, "retain component series");
      auto health_labels=labels;
      for(auto& label:health_labels) if(label.key=="state") label.value=std::string("online");
      auto health_binding=binding;
      static_cast<m::MetricDescriptorBinding&>(health_binding)=health;
      auto health_series=m::MakeMetricSeriesIdentity(health,health_labels,policy,health_binding,Id(200+code),6);
      Require(health_series.ok() && registry.RegisterSeries(*health_series.record,policy).ok,
          "retain health negative-control series");
      Require(fs::PublishFilespaceRoleObservation(filespace).ok, "actual role producer rejected valid role");
      auto lease=queue->TryAcquire(); Require(lease.ok(), "role producer omitted queue bytes");
      const auto& envelope=*lease.lease.observation;
      Require(envelope.binding.database_uuid==Id(1) && envelope.binding.node_uuid==Id(3),
          "role observation lost raw16 owner");
      const auto decoded=m::DecodeMetricRawSample(descriptor,*series.record,envelope.bytes);
      Require(decoded.ok() &&
          m::MakeMetricSeriesKey(descriptor.family,decoded.record->value.labels)==
              m::MakeMetricSeriesKey(descriptor.family,labels) &&
          std::holds_alternative<m::MetricEnumValue>(decoded.record->value.value) &&
          std::get<m::MetricEnumValue>(decoded.record->value.value).code==code,
          "role was collapsed, rounded or relabeled in binary transport");
      Require(queue->TryRemove(lease.lease)==m::MetricQueueError::none, "drain checked component sample");
    }
    Require(queue->Stats().admitted==names.size(), "lifecycle observation invented health samples");
    const auto before=registry.SnapshotCurrent();
    for (unsigned fault=0; fault<6; ++fault) {
      auto bad=filespace;
      if(fault==0) bad.database_uuid.value=Id(9);
      if(fault==1) bad.database_uuid.kind=p::UuidKind::object;
      if(fault==2) bad.filespace_uuid.kind=p::UuidKind::object;
      if(fault==3) bad.filespace_uuid.value={};
      if(fault==4) bad.role=static_cast<fs::FilespaceRole>(17);
      if(fault==5) bad.database_uuid.value.bytes[6]=0x40;
      Require(!fs::PublishFilespaceRoleObservation(bad).ok, "invalid owner or role accepted");
    }
    Require(queue->Stats().admitted==names.size() && queue->Stats().queued==0 &&
        registry.SnapshotCurrent().size()==before.size(), "refusal published partial observation");
    const auto after=registry.SnapshotCurrent();
    for(std::size_t i=0;i<before.size();++i) {
      const auto a=m::EncodeMetricValue(descriptor,before[i]);
      const auto b=m::EncodeMetricValue(descriptor,after[i]);
      Require(a.ok() && b.ok() && a.bytes==b.bytes, "refusal changed a retained current value");
    }
    m::MetricLabelSet health_labels={{"component","storage.filespace"},{"database_uuid",Id(1)},
        {"filespace_uuid",Id(2)},{"node_uuid",Id(3)},{"filespace_role","active_primary"},
        {"device_class","file"},{"state","healthy"}};
    m::MetricHistoryBinding health_binding;
    static_cast<m::MetricDescriptorBinding&>(health_binding)=health;
    health_binding.database_uuid=Id(1); health_binding.node_uuid=Id(3);
    auto observed_health=m::MakeMetricSeriesIdentity(health,health_labels,policy,health_binding,Id(300),6);
    Require(observed_health.ok() && registry.RegisterSeries(*observed_health.record,policy).ok,
        "retain explicit health producer series");
    Require(m::PublishFilespaceHealthState(m::MetricEnumValue{1},"healthy",Id(1),Id(2),Id(3),
        "active_primary","file").ok, "explicit health enum producer failed");
    auto measured=queue->TryAcquire(); Require(measured.ok(), "health producer omitted sample");
    auto decoded_health=m::DecodeMetricRawSample(health,*observed_health.record,measured.lease.observation->bytes);
    Require(decoded_health.ok() && std::get<m::MetricEnumValue>(decoded_health.record->value.value).code==1,
        "health enum lost exact code");
    Require(queue->TryRemove(measured.lease)==m::MetricQueueError::none, "drain checked health sample");
    Require(!m::PublishFilespaceHealthState(m::MetricEnumValue{5},"healthy",Id(1),Id(2),Id(3),
        "active_primary","file").ok && queue->Stats().admitted==names.size()+1,
        "out-of-domain health code published a sample");
    std::cout << "filespace_role_observation=passed roles=17 native_activation_claimed=false\n";
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
