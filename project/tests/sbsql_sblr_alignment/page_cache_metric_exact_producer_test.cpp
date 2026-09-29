// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_contracts.hpp"
#include "metric_page_cache_definitions.hpp"
#include "metric_observation_queue.hpp"
#include "metric_value_update.hpp"
#include "optimizer_storage_metrics.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace m = scratchbird::core::metrics;
namespace {
void Require(bool value, const char* detail) {
  if (!value) throw std::runtime_error(detail);
}
m::MetricUuid Id(unsigned tag) {
  m::MetricUuid id;
  id.bytes = {1,159,0,0,0,0,112,0,128,0,0,0,0,0,
              static_cast<unsigned char>(tag >> 8), static_cast<unsigned char>(tag)};
  return id;
}
using Counter = m::MetricValidationResult (*)(m::u64, m::MetricUuid, m::MetricUuid,
    std::string, std::string, std::string, std::string);
constexpr std::array<Counter,5> counters = {m::RecordPageCacheContextAdmission,
    m::RecordPageCacheContextReuse, m::RecordPageCacheContextEviction,
    m::RecordPageCacheContextProtectedNormalHotSkip, m::RecordPageCacheContextRefusal};
constexpr std::array<const char*,9> families = {
    "sb_page_cache_context_resident_pages", "sb_page_cache_context_resident_bytes",
    "sb_page_cache_context_pinned_pages", "sb_page_cache_context_dirty_pages",
    "sb_page_cache_context_admissions_total", "sb_page_cache_context_reuses_total",
    "sb_page_cache_context_evictions_total", "sb_page_cache_context_protected_normal_hot_skips_total",
    "sb_page_cache_context_refusals_total"};
constexpr m::u64 large = (m::u64{1} << 53) + 1;

struct Fixture {
  m::MetricRegistry& registry = m::DefaultMetricRegistry();
  std::shared_ptr<m::MetricObservationQueue> queue;
  std::map<m::MetricUuid, std::pair<m::MetricDescriptor,m::MetricSeriesIdentity>> retained;
  Fixture() {
    Require(registry.Descriptors().empty(), "registry acquired unbound descriptor authority");
    for (auto counter : counters) {
      Require(!counter(0, Id(1), Id(2), "all", "normal", "ok", "test").ok,
              "zero counter manufactured unbound publication success");
      Require(!counter(large, Id(1), Id(2), "all", "normal", "ok", "test").ok,
              "producer manufactured descriptor/series binding");
    }
    Require(!m::PublishPageCacheContextSnapshot(0,0,0,0,Id(1),Id(2),"all","normal","ok","test").ok,
            "snapshot succeeded without native activation");
    Require(registry.Descriptors().empty() && registry.SnapshotCurrent().empty(),
            "producer lazily registered identity-less templates");
    auto made = m::MetricObservationQueue::Create({Id(1),Id(3),{}}, {64,1024*1024});
    Require(made.ok(), "node queue creation");
    queue = std::move(made.queue);
    Require(registry.BindObservationQueue(queue).ok, "bind actual observation queue");
    auto definitions = m::PageCacheContextMetricDefinitions();
    Require(definitions.size() == families.size(), "page-cache definition inventory");
    for (unsigned i=0; i<definitions.size(); ++i) {
      const auto& definition = definitions[i];
      Require(definition.family == families[i] && definition.value_type == m::MetricScalarType::uint64 &&
          definition.type == (i<4 ? m::MetricType::gauge : m::MetricType::counter) &&
          definition.unit == (i==1 ? m::MetricUnit::bytes : i<4 ? m::MetricUnit::pages : m::MetricUnit::events),
          "independent exact family/class/unit oracle");
      Require(m::ValidateStoredMetricValueDescriptor(definition), "valid exact scalar definition");
      Require(!definition.cluster_only && definition.labels.size()==7, "local exact label schema");
      for (unsigned label=0; label<definition.labels.size(); ++label)
        Require(definition.labels[label].required && definition.labels[label].value_type ==
            (label==1 || label==2 ? m::MetricLabelType::system_uuid : m::MetricLabelType::text),
            "required raw16 scope label schema");
      Bind(definition, i);
    }
    // Aggregate page-cache producers share the same exact integer contract.
    constexpr std::array<const char*,5> aggregate = {"sb_page_cache_resident_pages",
        "sb_page_cache_resident_bytes", "sb_page_cache_pinned_pages", "sb_page_cache_dirty_pages",
        "sb_page_cache_evictions_total"};
    for(unsigned i=0;i<aggregate.size();++i) {
      auto definition=definitions[i]; definition.family=aggregate[i];
      definition.labels.resize(4);
      if(i==4)definition.labels.push_back({"result",true});
      Bind(definition,9+i);
    }
  }
  void Bind(const m::MetricDescriptorDefinition& definition, unsigned ordinal) {
    m::MetricDescriptor descriptor;
    static_cast<m::MetricDescriptorDefinition&>(descriptor)=definition;
    descriptor.metric_uuid=Id(100+ordinal);descriptor.descriptor_generation=7;
    descriptor.label_schema_uuid=Id(200+ordinal);descriptor.label_schema_generation=3;
    descriptor.retention_policy_uuid=Id(4);descriptor.retention_policy_generation=2;
    descriptor.visibility_policy_uuid=Id(5);descriptor.visibility_policy_generation=9;
    descriptor.readiness=m::MetricReadiness::implemented;
    m::MetricRetentionPolicy policy;
    policy.policy_name="component fixture retained policy";policy.policy_uuid=Id(4);policy.generation=2;
    m::MetricLabelSet labels={{"component","storage.page_cache"},{"database_uuid",Id(1)},
        {"filespace_uuid",Id(2)},{"page_family","all"}};
    if(ordinal<9) {
      labels.push_back({"context","normal"});labels.push_back({"result","ok"});
      labels.push_back({"reason","test"});
    } else if(ordinal==13)labels.push_back({"result","ok"});
    m::MetricHistoryBinding binding;
    static_cast<m::MetricDescriptorBinding&>(binding)=descriptor;
    binding.database_uuid=Id(1);binding.node_uuid=Id(3);
    auto series=m::MakeMetricSeriesIdentity(descriptor,labels,policy,binding,Id(300+ordinal),11);
    Require(series.ok(),"exact component series binding");
    Require(registry.RegisterDescriptor(descriptor).ok && registry.RegisterSeries(*series.record,policy).ok,
            "register actual retained component series");
    retained.emplace(descriptor.metric_uuid,std::make_pair(descriptor,*series.record));
  }
  void Read(unsigned ordinal, m::u64 expected) {
    auto acquired=queue->TryAcquire();Require(acquired.ok(),"producer did not publish actual queue bytes");
    const auto& observation=*acquired.lease.observation;
    const auto& [descriptor,series]=retained.at(Id(100+ordinal));
    Require(observation.binding.metric_uuid==descriptor.metric_uuid && observation.series_uuid==series.series_uuid &&
        observation.binding.database_uuid==Id(1) && observation.binding.node_uuid==Id(3),
        "binary queue ownership or retained identities changed");
    const auto decoded=m::DecodeMetricRawSample(descriptor,series,observation.bytes);
    Require(decoded.ok() && std::holds_alternative<m::u64>(decoded.record->value.value) &&
        std::get<m::u64>(decoded.record->value.value)==expected && !decoded.record->value.arithmetic_inexact,
        "producer narrowed or changed the exact unsigned sample");
    // This component discards its observation after checking it; no native
    // transaction or history durability is claimed by this queue drain.
    Require(queue->TryRemove(acquired.lease)==m::MetricQueueError::none,"drain checked fixture observation");
  }
};
}  // namespace

int main() {
  try {
    Fixture f;
    const m::u64 maximum=std::numeric_limits<m::u64>::max();
    Require(m::PublishPageCacheContextSnapshot(large,maximum,0,large+2,Id(1),Id(2),"all","normal","ok","test").ok,
            "exact context snapshot publication");
    f.Read(0,large);f.Read(1,maximum);f.Read(2,0);f.Read(3,large+2);
    Require(m::PublishPageCacheSnapshot(large,maximum,0,large+2,Id(1),Id(2),"all").ok,
            "exact aggregate snapshot publication");
    f.Read(9,large);f.Read(10,maximum);f.Read(11,0);f.Read(12,large+2);
    Require(m::RecordPageCacheEviction(Id(1),Id(2),"all","ok").ok,"typed aggregate eviction");
    f.Read(13,1);
    scratchbird::storage::page::OptimizerStorageMetricSample storage;
    storage.scope_uuid=Id(6);storage.database_uuid=Id(1);storage.filespace_uuid=Id(2);storage.node_uuid=Id(3);
    storage.route_label="component";storage.page_family="all";storage.page_class="mixed";
    storage.evidence_digest="component retained cache observation";storage.source_generation=1;
    storage.authority.storage_page_manager_authoritative=true;
    storage.authority.filespace_identity_authoritative=true;storage.authority.engine_scope_bound=true;
    storage.page_count=storage.resident_pages=3;storage.pinned_pages=1;storage.dirty_pages=2;
    auto published=scratchbird::storage::page::PublishOptimizerStorageMetrics(storage);
    Require(!published.ok && published.detail=="optimizer.storage_metrics.required_field_missing:resident_bytes" &&
        f.queue->Stats().queued==0,"missing measured bytes became a fabricated zero or fixed-page-size observation");
    storage.resident_bytes=114688; // 16KiB + 32KiB + 64KiB, not three 4KiB pages.
    published=scratchbird::storage::page::PublishOptimizerStorageMetrics(storage);
    // Other optimizer families are deliberately not bound in this component;
    // the overall publication must not claim their success.
    Require(!published.ok,"unbound optimizer families claimed successful publication");
    f.Read(9,3);f.Read(10,114688);f.Read(11,1);f.Read(12,2);
    for(unsigned i=0;i<counters.size();++i) {
      auto counter=counters[i];
      Require(counter(large,Id(1),Id(2),"all","normal","ok","test").ok,"exact context counter");f.Read(i+4,large);
      Require(counter(0,Id(1),Id(2),"all","normal","ok","test").ok,"bound zero observation");f.Read(i+4,large);
      Require(counter(maximum-large,Id(1),Id(2),"all","normal","ok","test").ok,"full uint64 counter range");f.Read(i+4,maximum);
      Require(!counter(1,Id(1),Id(2),"all","normal","ok","test").ok,"counter overflow accepted");
      Require(!counter(0,{},Id(2),"all","normal","ok","test").ok,"zero hid invalid database identity");
      Require(!counter(0,Id(6),Id(2),"all","normal","ok","test").ok,"zero hid foreign owner/absent series");
      Require(!counter(1,Id(1),Id(2),"all","normal","ok","").ok,"missing required reason accepted");
      Require(f.queue->Stats().queued==0,"failed counter published queue bytes");
      bool found=false;
      for(const auto& current:f.registry.SnapshotCurrent())if(current.family==families[i+4]) {
        found=true;Require(std::get<m::u64>(current.value)==maximum,"failed counter mutated current value");
      }
      Require(found,"published current counter missing");
    }
    std::cout<<"page_cache_exact_producers=passed native_bootstrap_claimed=false\n";
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
  }
}
