// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_label_key.hpp"
#include "metric_producer.hpp"
#include "metric_observation_queue.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <limits>
#include <type_traits>
#include <atomic>
#include <new>
#include <thread>
#include <stdexcept>

namespace { thread_local long allocation_budget=-1; thread_local bool allocation_failed=false; }
void* operator new(std::size_t size) {
  if(allocation_budget==0){allocation_budget=-1;allocation_failed=true;throw std::bad_alloc();}
  if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(size?size:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace m = scratchbird::core::metrics;
namespace {
std::size_t checks = 0;
void Require(bool good, const char* detail) {
  ++checks;
  if (!good) { std::cerr << detail << '\n'; std::exit(1); }
}
m::MetricUuid Id() {
  m::MetricUuid value{};
  value.bytes = {1,159,0,0,0,0,112,0,128,0,0,0,0,0,0,1};
  return value;
}
m::MetricDescriptor Definition(unsigned tag) {
  m::MetricDescriptor d;
  d.family="sb_atomic_runtime_metric_"+std::to_string(tag);
  d.type=m::MetricType::counter;d.unit=m::MetricUnit::operations;
  d.namespace_path="sys.metrics.registration";d.producer_owner="registration_test";
  d.help="actual registry binary identity and atomic publication fixture";
  d.value_type=m::MetricScalarType::uint64;d.readiness=m::MetricReadiness::implemented;
  d.metric_uuid=Id();d.metric_uuid.bytes[14]=tag>>8;d.metric_uuid.bytes[15]=tag;
  d.descriptor_generation=1;d.retention_policy_uuid=Id();d.retention_policy_generation=1;
  d.visibility_policy_uuid=Id();d.visibility_policy_generation=1;
  d.aliases={d.family+"_alias_a_long_annotation",d.family+"_alias_b_long_annotation",d.family+"_alias_c_long_annotation"};
  return d;
}
void RegistryPublication() {
  m::MetricRegistry registry;
  const auto first=Definition(300),next=Definition(301);
  Require(registry.RegisterDescriptor(first).ok,"register actual binary descriptor");
  const auto* retained=registry.FindDescriptor(first.metric_uuid);
  Require(retained&&retained->metric_uuid==first.metric_uuid&&retained->descriptor_generation==1,"binary descriptor lookup lost identity/generation");
  Require(registry.FindDescriptor(first.family)==retained,"family annotation changed binary entry");
  for(const auto& alias:first.aliases)Require(registry.FindDescriptorOrAlias(alias)==retained&&!registry.FindDescriptor(alias),"alias lookup changed entry or became canonical family");
  for(unsigned fault=0;fault<7;++fault){auto candidate=next;
    if(fault==0)candidate.metric_uuid=first.metric_uuid;
    if(fault==1)candidate.family=first.family;
    if(fault==2)candidate.family=first.aliases[0];
    if(fault==3)candidate.aliases[1]=first.family;
    if(fault==4)candidate.aliases[1]=first.aliases[0];
    if(fault==5)candidate.aliases[1]=candidate.aliases[0];
    if(fault==6)candidate.aliases[1]=candidate.family;
    Require(!registry.RegisterDescriptor(candidate).ok,"descriptor identity/annotation collision admitted");
    Require(!registry.FindDescriptor(next.metric_uuid)&&!registry.FindDescriptor(next.family),"failed registration published descriptor");
    for(const auto& alias:next.aliases)Require(!registry.FindDescriptorOrAlias(alias),"failed registration published alias");
    Require(registry.FindDescriptor(first.metric_uuid)==retained&&registry.FindDescriptorOrAlias(first.aliases[0])==retained,"collision replaced existing descriptor");
  }
  unsigned faults=0;
  for(long budget=0;budget<1000;++budget){m::MetricRegistry fresh;Require(fresh.RegisterDescriptor(first).ok,"allocation baseline registration");
    const auto baseline=fresh.Descriptors().size();const auto* existing=fresh.FindDescriptor(first.metric_uuid);bool threw=false;
    allocation_budget=budget;
    try {const auto added=fresh.RegisterDescriptor(next);allocation_budget=-1;Require(added.ok,"valid registration returned false after allocation recovery");}
    catch(const std::bad_alloc&){allocation_budget=-1;threw=true;++faults;}
    Require(fresh.FindDescriptor(first.metric_uuid)==existing&&fresh.FindDescriptorOrAlias(first.aliases[0])==existing,"allocation failure changed prior pointer/mapping");
    if(!threw)break;
    Require(fresh.Descriptors().size()==baseline&&!fresh.FindDescriptor(next.metric_uuid)&&!fresh.FindDescriptor(next.family),"allocation exception exposed partial descriptor");
    for(const auto& alias:next.aliases)Require(!fresh.FindDescriptorOrAlias(alias),"allocation exception exposed alias");
    Require(fresh.RegisterDescriptor(next).ok,"allocation exception stranded an alias and prevented retry");
    const auto* added=fresh.FindDescriptor(next.metric_uuid);
    for(const auto& alias:next.aliases)Require(added&&fresh.FindDescriptorOrAlias(alias)==added,"retry failed complete atomic publication");
  }
  Require(faults>0&&faults<999,"registration allocation sweep did not exhaust all sites");
  std::cout<<"registry allocation sites="<<faults<<'\n';
  std::atomic<bool> start=false,good=true;
  std::vector<std::thread> workers;
  for(unsigned worker=0;worker<4;++worker)workers.emplace_back([&,worker]{
    while(!start.load(std::memory_order_acquire))std::this_thread::yield();
    for(unsigned n=0;n<64;++n){const auto candidate=Definition(1000+64*worker+n);
      if(!registry.RegisterDescriptor(candidate).ok)good=false;
      const auto* by_uuid=registry.FindDescriptor(candidate.metric_uuid);
      if(!by_uuid||by_uuid->metric_uuid!=candidate.metric_uuid||registry.FindDescriptor(candidate.family)!=by_uuid)good=false;
      for(const auto& alias:candidate.aliases)if(registry.FindDescriptorOrAlias(alias)!=by_uuid)good=false;
      if(registry.FindDescriptor(first.metric_uuid)!=retained||retained->family!=first.family)good=false;
      const auto snapshot=registry.Descriptors();for(const auto& row:snapshot)
        if(registry.FindDescriptor(row.metric_uuid)->family!=row.family)good=false;
    }
  });
  start.store(true,std::memory_order_release);for(auto& worker:workers)worker.join();
  Require(good,"concurrent registration/lookup changed an immutable entry");
  Require(registry.FindDescriptor(first.metric_uuid)==retained,"registration invalidated retained descriptor pointer");
}
struct ObservationFixture {
  m::MetricDescriptor descriptor=Definition(700);
  m::MetricRetentionPolicy policy;
  m::MetricSeriesIdentity series;
  m::MetricLabelSet labels;
  std::shared_ptr<m::MetricObservationQueue> queue;
  ObservationFixture(unsigned capacity=2){
    auto database=Id(),node=Id(),identity=Id();database.bytes[15]=20;node.bytes[15]=21;identity.bytes[15]=22;
    auto made=m::MetricObservationQueue::Create({database,node,{}},{capacity,8*m::kMetricSampleMaxBytes});
    Require(made.ok(),"construct node observation queue");queue=std::move(made.queue);
    policy.policy_uuid=descriptor.retention_policy_uuid;policy.generation=descriptor.retention_policy_generation;policy.policy_name="retained local policy";
    Bind(identity);
  }
  void Bind(m::MetricUuid identity){
    m::MetricHistoryBinding binding;static_cast<m::MetricDescriptorBinding&>(binding)=descriptor;
    binding.database_uuid=queue->binding().database_uuid;binding.node_uuid=queue->binding().node_uuid;
    auto made=m::MakeMetricSeriesIdentity(descriptor,labels,policy,binding,identity,7);
    Require(made.ok(),"construct exact retained series");series=std::move(*made.record);
  }
  void Register(m::MetricRegistry& registry){Require(registry.RegisterDescriptor(descriptor).ok&&registry.RegisterSeries(series,policy).ok,"register retained observation bindings");}
  auto Increment(m::MetricRegistry& registry,m::MetricScalar value=m::MetricScalar{m::u64{1}},m::MetricValue* receipt=nullptr){return registry.IncrementCounter(descriptor.family,labels,std::move(value),descriptor.producer_owner,receipt);}
  m::MetricRawSampleRecord Read(m::MetricObservationLease& lease){
    auto head=queue->TryAcquire();Require(head.ok(),"acquire actual registry observation");lease=head.lease;
    auto sample=m::DecodeMetricRawSample(descriptor,series,lease.observation->bytes);
    Require(sample.ok(),"decode actual registry SBMS bytes");return std::move(*sample.record);
  }
};
void QueuePublication(){
  ObservationFixture f(1);m::MetricRegistry registry(f.queue);
  Require(registry.RegisterDescriptor(f.descriptor).ok,"descriptor alone registration");
  Require(f.Increment(registry).diagnostic_code=="METRIC.OBSERVATION_SOURCE_UNAVAILABLE"&&registry.SnapshotCurrent().empty()&&f.queue->Stats().queued==0,"unbound series fabricated observation success");
  auto foreign=f.series;foreign.node_uuid.bytes[15]++;
  std::get<1>(foreign.series_key)=foreign.node_uuid;
  Require(!registry.RegisterSeries(foreign,f.policy).ok,"foreign-node series bound");
  auto bad_policy=f.policy;++bad_policy.generation;
  Require(!registry.RegisterSeries(f.series,bad_policy).ok,"wrong retained policy generation bound");
  Require(registry.RegisterSeries(f.series,f.policy).ok&&!registry.RegisterSeries(f.series,f.policy).ok,"series registration failed or replacement admitted");
  Require(f.Increment(registry).ok,"bound update not handed off");
  m::MetricObservationLease first_lease;const auto first=f.Read(first_lease);
  Require(first.series_uuid==f.series.series_uuid&&first.metric_uuid==f.descriptor.metric_uuid&&
    first.series_definition_generation==7&&first_lease.observation->series_definition_generation==7&&
    first.source_sequence==1&&first.sample_time_utc_ns>0&&first.sample_time_utc_ns==first.collection_time_utc_ns&&
    !first.publication_time_utc_ns&&first.clock_quality.empty()&&first.freshness_class.empty()&&
    m::MetricSystemUuidValid(first.sample_uuid)&&std::get<m::u64>(first.value.value)==1,"queued sample invented binding/finality/quality or changed exact counter");
  Require(f.Increment(registry,m::u64{9}).diagnostic_code=="METRIC.OBSERVATION_RESOURCE_EXHAUSTED","leased full queue did not refuse update");
  Require(registry.SnapshotCurrent().size()==1&&std::get<m::u64>(registry.SnapshotCurrent()[0].value)==1&&registry.SnapshotHistory().size()==1&&f.queue->Stats().queued==1,"queue refusal changed current/history or evicted head");
  // Component-test consumption, not a native recorder/commit receipt.
  Require(f.queue->TryRemove(first_lease)==m::MetricQueueError::none,"consume verified component observation");
  Require(f.Increment(registry,m::u64{2}).ok,"retry after capacity release failed");
  m::MetricObservationLease next_lease;const auto next=f.Read(next_lease);
  Require(next.source_sequence==3&&next.sample_uuid!=first.sample_uuid&&std::get<m::u64>(next.value.value)==3&&registry.SnapshotHistory().size()==2,"queue refusal hid source gap or committed rejected delta");
  Require(f.queue->TryRemove(next_lease)==m::MetricQueueError::none,"consume second component observation");
  Require(f.Increment(registry,std::numeric_limits<m::u64>::max()).diagnostic_code=="METRIC.AGGREGATE_OVERFLOW"&&f.queue->Stats().queued==0&&std::get<m::u64>(registry.SnapshotCurrent()[0].value)==3,"overflow published queue/current state");
  m::MetricRegistry unbound;Require(unbound.RegisterDescriptor(f.descriptor).ok&&!unbound.RegisterSeries(f.series,f.policy).ok&&!f.Increment(unbound).ok,"default registry invented a node queue or native series");
  for(unsigned kind=0;kind<3;++kind){ObservationFixture other;
    other.descriptor.type=kind==0?m::MetricType::gauge:kind==1?m::MetricType::histogram:m::MetricType::state;
    if(kind==1)other.descriptor.histogram_buckets={m::u64{5},m::u64{10}};
    if(kind==2){other.descriptor.value_type=m::MetricScalarType::enumeration;other.descriptor.enum_values={2,7};}
    other.Bind(other.series.series_uuid);m::MetricRegistry owned(other.queue);other.Register(owned);
    m::MetricValue receipt;
    const auto result=kind==0?owned.SetGauge(other.descriptor.family,{},m::u64{7},other.descriptor.producer_owner,&receipt):
      kind==1?owned.ObserveHistogram(other.descriptor.family,{},m::u64{7},other.descriptor.producer_owner,&receipt):
      owned.SetState(other.descriptor.family,{},m::MetricEnumValue{7},"ready",other.descriptor.producer_owner,&receipt);
    Require(result.ok,"typed update did not use actual registry queue path");m::MetricObservationLease lease;const auto stored=other.Read(lease);
    Require(stored.value.type==other.descriptor.type&&stored.series_uuid==other.series.series_uuid,"typed handoff changed class or series");
    const auto encoded_receipt=m::EncodeMetricValue(other.descriptor,receipt);
    const auto encoded_queued=m::EncodeMetricValue(other.descriptor,stored.value);
    Require(encoded_receipt.ok()&&encoded_queued.ok()&&encoded_receipt.bytes==encoded_queued.bytes,
            "typed receipt differs from the actually admitted observation");
    if(kind==1)Require(stored.value.count==1&&std::get<m::u64>(stored.value.sum)==7&&stored.value.buckets==std::vector<m::u64>{0,1,1},"histogram queue lost exact aggregates");
    if(kind==2)Require(std::get<m::MetricEnumValue>(stored.value.value).code==7&&stored.value.state_text=="ready","state queue lost enum or text");
  }
}
void DescriptorScopeAdmission() {
  for(const auto& key:{"database_uuid","node_uuid"}) {
    ObservationFixture f;
    f.descriptor.labels={{key,true,false,m::MetricLabelType::system_uuid}};
    f.descriptor.label_schema_uuid=Id();f.descriptor.label_schema_generation=1;
    const auto expected=std::string(key)=="database_uuid"?
        f.queue->binding().database_uuid:f.queue->binding().node_uuid;
    f.labels={{key,expected}};f.Bind(f.series.series_uuid);
    m::MetricRegistry owner(f.queue);
    Require(owner.RegisterDescriptor(f.descriptor).ok,"owner-labelled descriptor refused");
    auto foreign=f.series;auto other=expected;++other.bytes[15];
    foreign.labels[0].value=other;
    std::get<5>(foreign.series_key)=m::MakeMetricSeriesKey({},foreign.labels).second;
    Require(!owner.RegisterSeries(foreign,f.policy).ok&&owner.SnapshotCurrent().empty()&&
        owner.SnapshotHistory().empty()&&f.queue->Stats().queued==0,
        "foreign owner label registered or changed observations");
    Require(owner.RegisterSeries(f.series,f.policy).ok&&f.Increment(owner).ok,
        "matching binary owner failed actual publication after refused forged series");
    m::MetricObservationLease lease;const auto sample=f.Read(lease);
    Require(sample.database_uuid==f.series.database_uuid&&sample.node_uuid==f.series.node_uuid&&
        m::MakeMetricSeriesKey({},sample.labels)==m::MakeMetricSeriesKey({},f.labels),
        "publication changed owner or reserved UUID label");
    auto crossed=f.labels;crossed[0].value=other;
    Require(!owner.IncrementCounter(f.descriptor.family,crossed,m::u64{1},f.descriptor.producer_owner).ok&&
        owner.SnapshotCurrent().size()==1&&owner.SnapshotHistory().size()==1&&f.queue->Stats().queued==1,
        "cross-scope update changed retained observation");
    Require(f.queue->TryRemove(lease)==m::MetricQueueError::none,"remove verified scope observation");
  }
  ObservationFixture fixture;
  m::MetricRegistry registry(fixture.queue);
  const std::vector<std::string> invalid_local = {
      "sys.metrics", "sys.metrics.", "sys.metricsevil.test", "sys.metrics_extra.test",
      "cluster.sys.metrics.test", "cluster.sys.metrics", "other.sys.metrics.test",
      std::string("sys.metrics.good\0hidden",23), "sys.metrics." + std::string(4096,'x')};
  for (const auto& path : invalid_local) {
    auto descriptor=fixture.descriptor;descriptor.namespace_path=path;
    Require(!registry.RegisterDescriptor(descriptor).ok,
            "local descriptor accepted an empty, forged, or cluster namespace root");
    Require(!m::MakeMetricSeriesIdentity(descriptor, fixture.labels, fixture.policy,
        fixture.series, fixture.series.series_uuid, fixture.series.series_definition_generation).ok(),
        "series construction accepted a mismatched namespace scope");
    Require(registry.Descriptors().empty() && registry.SnapshotCurrent().empty() &&
            fixture.queue->Stats().queued==0,"invalid scope mutated live state");
  }
  for (const auto& path : {"cluster.sys.metrics", "cluster.sys.metrics.",
                           "cluster.sys.metricsevil.test", "sys.metrics.test"}) {
    auto descriptor=fixture.descriptor;descriptor.cluster_only=true;descriptor.namespace_path=path;
    Require(!registry.RegisterDescriptor(descriptor).ok,"cluster descriptor accepted a forged or local namespace root");
  }
  auto invalid=fixture.descriptor;invalid.readiness=static_cast<m::MetricReadiness>(255);
  Require(!registry.RegisterDescriptor(invalid).ok,"unknown readiness became an admitted runtime descriptor");
  fixture.Register(registry);
  Require(fixture.Increment(registry).ok,"valid local descriptor/series stopped publishing");
  m::MetricObservationLease lease;
  const auto sample=fixture.Read(lease);
  Require(sample.database_uuid==fixture.series.database_uuid && sample.node_uuid==fixture.series.node_uuid &&
      sample.cluster_uuid.is_nil(),"valid local observation changed scope");
  auto cluster=Definition(702);cluster.cluster_only=true;cluster.namespace_path="cluster.sys.metrics.test";
  Require(registry.RegisterDescriptor(cluster).ok,"valid cluster definition rejected rather than retained as metadata");
  Require(!registry.IncrementCounter(cluster.family,{},m::u64{1},cluster.producer_owner).ok,
          "cluster definition manufactured a local series/source");
  Require(fixture.queue->Stats().queued==1 && registry.SnapshotCurrent().size()==1 &&
      registry.Descriptors(false).size()==1 && registry.Descriptors(true).size()==2,
      "cluster metadata crossed local publication or descriptor projection scope");
}
void RejectionObservationPublication() {
  ObservationFixture fixture;
  fixture.descriptor.family="sb_metric_samples_rejected_total";
  fixture.descriptor.producer_owner="metrics_registry_manager";
  fixture.descriptor.labels={{"metric_family",true},{"reason",true}};
  fixture.descriptor.label_schema_uuid=Id();fixture.descriptor.label_schema_generation=1;
  fixture.labels={{"metric_family","sb_bad_producer"},{"reason","invalid_label"}};
  fixture.Bind(fixture.series.series_uuid);
  auto& registry=m::DefaultMetricRegistry();
  Require(registry.BindObservationQueue(fixture.queue).ok,"bind rejection counter node queue");
  fixture.Register(registry);
  Require(m::RejectSample("sb_bad_producer","invalid_label","bad_producer").ok,
          "explicit rejection failed exact counter type/registered producer admission");
  m::MetricObservationLease lease;
  auto sample=fixture.Read(lease);
  Require(std::get<m::u64>(sample.value.value)==1 && sample.metric_uuid==fixture.descriptor.metric_uuid,
          "explicit rejection did not retain exact counter observation");
  Require(fixture.queue->TryRemove(lease)==m::MetricQueueError::none,"remove inspected rejection sample");

  auto rejected=Definition(703);rejected.family="sb_bad_producer";rejected.producer_owner="bad_producer";
  rejected.labels={{"required",true}};rejected.label_schema_uuid=Id();rejected.label_schema_generation=1;
  Require(registry.RegisterDescriptor(rejected).ok,"bind rejected-producer definition");
  const m::MetricLabelSet automatic_labels={{"metric_family","sb_bad_producer"},
      {"reason","SB-METRICS-LABEL-REQUIRED-MISSING"}};
  auto series_uuid=fixture.series.series_uuid;series_uuid.bytes[14]=77;
  const auto automatic=m::MakeMetricSeriesIdentity(fixture.descriptor,automatic_labels,fixture.policy,
      fixture.series,series_uuid,fixture.series.series_definition_generation);
  Require(automatic.ok() && registry.RegisterSeries(*automatic.record,fixture.policy).ok,
          "bind retained automatic rejection series");
  const auto refusal=registry.IncrementCounter(rejected.family,{},m::u64{1},rejected.producer_owner);
  Require(!refusal.ok && refusal.diagnostic_code=="SB-METRICS-LABEL-REQUIRED-MISSING",
          "rejection observation changed original producer refusal");
  auto queued=fixture.queue->TryAcquire();Require(queued.ok(),"automatic rejection did not publish queue bytes");
  const auto decoded=m::DecodeMetricRawSample(fixture.descriptor,*automatic.record,queued.lease.observation->bytes);
  Require(decoded.ok() && std::get<m::u64>(decoded.record->value.value)==1,
          "automatic rejection counter has wrong value/type");
  Require(fixture.queue->TryRemove(queued.lease)==m::MetricQueueError::none,"remove automatic rejection sample");
  for(const auto& current:registry.SnapshotCurrent())
    Require(current.family==fixture.descriptor.family,"invalid original producer exposed current state");
  const auto before=fixture.queue->Stats().admitted;
  Require(!m::RejectSample("sb_bad_producer","","bad_producer").ok &&
      fixture.queue->Stats().admitted==before && fixture.queue->Stats().queued==0,
      "invalid rejection sample recursively published or falsely succeeded");
  // Telemetry backpressure must not replace the original producer diagnostic
  // or publish the rejected value, even when the rejection series exists.
  for (unsigned n=0;n<2;++n)
    Require(m::RejectSample("sb_bad_producer","invalid_label","bad_producer").ok,
            "fill actual rejection queue");
  const auto full_admitted=fixture.queue->Stats().admitted;
  const auto full_history=registry.SnapshotHistory().size();
  const auto pressured=registry.IncrementCounter(rejected.family,{},m::u64{1},rejected.producer_owner);
  Require(!pressured.ok && pressured.diagnostic_code==refusal.diagnostic_code &&
      pressured.detail==refusal.detail && fixture.queue->Stats().queued==2 &&
      fixture.queue->Stats().admitted==full_admitted && registry.SnapshotHistory().size()==full_history,
      "rejection queue backpressure replaced original refusal or published state");
  for (m::u64 expected=2;expected<=3;++expected) {
    m::MetricObservationLease retained;
    const auto value=fixture.Read(retained);
    Require(std::get<m::u64>(value.value.value)==expected,
            "failed automatic rejection mutated retained counter observations");
    Require(fixture.queue->TryRemove(retained)==m::MetricQueueError::none,
            "remove retained rejection under backpressure");
  }
  // Count only the allocations needed to establish the original diagnostic,
  // then fail the first allocation made by its secondary telemetry path.
  allocation_budget=1000;
  const auto measured=registry.ValidateLabels(rejected,{});
  const long diagnostic_allocations=1000-allocation_budget;
  allocation_budget=-1;
  Require(!measured.ok && diagnostic_allocations>0 && diagnostic_allocations<1000,
          "could not isolate original diagnostic allocations");
  m::MetricValidationResult allocation_refusal;
  bool telemetry_threw=false;
  allocation_failed=false;
  allocation_budget=diagnostic_allocations;
  try {
    allocation_refusal=registry.IncrementCounter(rejected.family,{},m::u64{1},rejected.producer_owner);
  } catch (const std::bad_alloc&) {
    telemetry_threw=true;
  }
  allocation_budget=-1;
  Require(allocation_failed && !telemetry_threw && !allocation_refusal.ok &&
      allocation_refusal.diagnostic_code==refusal.diagnostic_code && allocation_refusal.detail==refusal.detail &&
      fixture.queue->Stats().queued==0 && fixture.queue->Stats().admitted==full_admitted &&
      registry.SnapshotHistory().size()==full_history,
      "rejection telemetry allocation failure replaced original refusal or published state");
}
void OneNodeQueueBinding() {
  ObservationFixture fixture;
  m::MetricRegistry registry;
  const auto& owner = fixture.queue->binding();
  Require(!registry.ObservationOwnerMatches(owner.database_uuid, owner.node_uuid),
          "unbound registry claimed a node owner");
  Require(!registry.BindObservationQueue({}).ok, "nil node queue admitted");
  Require(registry.RegisterDescriptor(fixture.descriptor).ok,
          "pre-bind descriptor registration failed");
  Require(registry.BindObservationQueue(fixture.queue).ok &&
          registry.ObservationOwnerMatches(owner.database_uuid, owner.node_uuid),
          "explicit local node queue did not bind");
  auto foreign = owner.database_uuid;
  foreign.bytes[14] ^= 0x40;
  Require(!registry.ObservationOwnerMatches(foreign, owner.node_uuid) &&
          !registry.ObservationOwnerMatches(owner.database_uuid, foreign),
          "foreign database/node matched retained queue");
  auto replacement = m::MetricObservationQueue::Create(
      {foreign, owner.node_uuid, {}}, {2, 8*m::kMetricSampleMaxBytes});
  Require(replacement.ok(), "replacement queue setup");
  Require(!registry.BindObservationQueue(std::move(replacement.queue)).ok &&
          !registry.BindObservationQueue(fixture.queue).ok &&
          registry.ObservationOwnerMatches(owner.database_uuid, owner.node_uuid),
          "queue rebinding replaced retained owner");
  m::MetricRegistry local_only;
  auto clustered = m::MetricObservationQueue::Create(
      {owner.database_uuid, owner.node_uuid, foreign}, {2, 8*m::kMetricSampleMaxBytes});
  Require(clustered.ok() && !local_only.BindObservationQueue(std::move(clustered.queue)).ok &&
          !local_only.ObservationOwnerMatches(owner.database_uuid, owner.node_uuid),
          "local registry accepted a cluster queue");
  auto constructor_queue = m::MetricObservationQueue::Create(
      {owner.database_uuid, owner.node_uuid, foreign}, {2, 8*m::kMetricSampleMaxBytes});
  Require(constructor_queue.ok(), "cluster constructor queue setup");
  std::shared_ptr<m::MetricObservationQueue> retained_cluster_queue = std::move(constructor_queue.queue);
  bool refused_constructor = false;
  try {
    m::MetricRegistry incorrectly_local(retained_cluster_queue);
  } catch (const std::invalid_argument&) {
    refused_constructor = true;
  }
  Require(refused_constructor && retained_cluster_queue->Stats().queued == 0 &&
          retained_cluster_queue->Stats().admitted == 0,
          "constructor bypassed local ownership admission or modified cluster queue");
  Require(registry.RegisterSeries(fixture.series, fixture.policy).ok &&
          fixture.Increment(registry).ok && fixture.queue->Stats().queued == 1,
          "one-time binding did not retain actual observation handoff");
  m::MetricRegistry racing;
  auto other = m::MetricObservationQueue::Create(
      {foreign, owner.node_uuid, {}}, {2, 8*m::kMetricSampleMaxBytes});
  Require(other.ok(), "competing queue setup failed");
  std::shared_ptr<m::MetricObservationQueue> second = std::move(other.queue);
  std::atomic<bool> start{false};
  bool first_won = false, second_won = false;
  std::thread first([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    first_won = racing.BindObservationQueue(fixture.queue).ok;
  });
  std::thread second_thread([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    second_won = racing.BindObservationQueue(second).ok;
  });
  start.store(true, std::memory_order_release);
  first.join(); second_thread.join();
  Require(first_won != second_won &&
          racing.ObservationOwnerMatches(owner.database_uuid, owner.node_uuid) == first_won &&
          racing.ObservationOwnerMatches(foreign, owner.node_uuid) == second_won,
          "concurrent binding admitted multiple owners or lost the winner");
}
void QueuePublicationFaults(){
  for(bool with_receipt:{false,true}) for(bool existing:{false,true}){unsigned faults=0;bool complete=false;
    for(long budget=0;budget<1000;++budget){ObservationFixture f;m::MetricRegistry registry(f.queue);f.Register(registry);
      if(existing)Require(f.Increment(registry).ok,"initial observation for fault baseline");
      const auto baseline=registry.SnapshotHistory().size(),queued=f.queue->Stats().queued;
      m::MetricValue receipt;receipt.value=m::u64{999};
      bool ok=false;allocation_failed=false;allocation_budget=budget;
      try{ok=f.Increment(registry,m::u64{1},with_receipt?&receipt:nullptr).ok;}catch(const std::bad_alloc&){}
      const bool failed=allocation_failed;allocation_budget=-1;
      if(!failed){Require(ok,"observation refused without allocation fault");
        if(with_receipt)Require(std::get<m::u64>(receipt.value)==(existing?2u:1u),"receipt lost exact published successor");
        complete=true;break;}
      ++faults;Require(!ok&&registry.SnapshotHistory().size()==baseline&&f.queue->Stats().queued==queued,"allocation failure published current/history or queue prefix");
      Require(std::get<m::u64>(receipt.value)==999,"allocation failure modified publication receipt");
      const auto current=registry.SnapshotCurrent();Require(existing?(current.size()==1&&std::get<m::u64>(current[0].value)==1):current.empty(),"allocation fault changed current value");
      Require(f.Increment(registry).ok,"observation retry after allocation failure refused");
      Require(std::get<m::u64>(registry.SnapshotCurrent()[0].value)==(existing?2u:1u)&&f.queue->Stats().queued==queued+1,"retry committed failed observation twice");
    }
    Require(complete&&faults>0,"observation fault sweep incomplete");std::cout<<"registry observation existing="<<existing<<" receipt="<<with_receipt<<" allocation sites="<<faults<<'\n';
  }
}
void SeriesBindingAndConcurrency(){
  ObservationFixture typed(256);
  typed.descriptor.labels={{"tenant",true,false,m::MetricLabelType::text},{"user_uuid",true,false,m::MetricLabelType::uuid_value}};
  typed.descriptor.label_schema_uuid=Id();typed.descriptor.label_schema_uuid.bytes[15]=31;typed.descriptor.label_schema_generation=1;
  auto user_id=Id();user_id.bytes[6]=0x10;typed.labels={{"tenant",std::string("tenant\0alpha",12)},{"user_uuid",user_id}};
  typed.Bind(typed.series.series_uuid);m::MetricRegistry registry(typed.queue);typed.Register(registry);
  auto different=typed.series;different.series_uuid.bytes[15]++;
  Require(!registry.RegisterSeries(different,typed.policy).ok,"second UUID replaced an occupied typed key");
  auto other_labels=typed.labels;other_labels[0].value=std::string("different tenant");
  const auto other=m::MakeMetricSeriesIdentity(typed.descriptor,other_labels,typed.policy,typed.series,typed.series.series_uuid,1);
  Require(other.ok()&&!registry.RegisterSeries(*other.record,typed.policy).ok,"same series UUID bound two typed keys");
  std::atomic<bool> start=false,good=true;std::vector<std::thread> producers;
  std::array<std::array<m::u64,32>,4> receipts{};
  for(unsigned t=0;t<4;++t)producers.emplace_back([&,t]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();
    for(unsigned n=0;n<32;++n){m::MetricValue receipt;
      if(!typed.Increment(registry,m::u64{1},&receipt).ok)good=false;
      else receipts[t][n]=std::get<m::u64>(receipt.value);}});
  start.store(true,std::memory_order_release);for(auto& thread:producers)thread.join();
  Require(good&&typed.queue->Stats().queued==128&&registry.SnapshotHistory().size()==128&&std::get<m::u64>(registry.SnapshotCurrent()[0].value)==128,"concurrent actual updates lost queue/current/history effects");
  std::set<m::u64> successors;
  for(const auto& thread:receipts){m::u64 previous=0;for(const auto value:thread){
    Require(value>previous&&successors.insert(value).second,"concurrent receipts returned later snapshots or duplicates");previous=value;}}
  Require(successors.size()==128&&*successors.begin()==1&&*successors.rbegin()==128,"concurrent receipt coverage lost an admitted update");
  m::MetricUuid previous;
  for(m::u64 n=1;n<=128;++n){m::MetricObservationLease lease;const auto sample=typed.Read(lease);
    Require(sample.source_sequence==n&&std::get<m::u64>(sample.value.value)==n&&sample.sample_uuid!=previous&&
      std::get<m::MetricUuid>(sample.value.labels[1].value)==user_id&&std::get<std::string>(sample.value.labels[0].value)==std::string("tenant\0alpha",12),"concurrent handoff reordered state or coerced typed UUID/text");
    previous=sample.sample_uuid;Require(typed.queue->TryRemove(lease)==m::MetricQueueError::none,"consume concurrent component observation");
  }
  unsigned faults=0;bool complete=false;
  for(long budget=0;budget<1000;++budget){ObservationFixture f;m::MetricRegistry fresh(f.queue);Require(fresh.RegisterDescriptor(f.descriptor).ok,"series fault descriptor");
    bool ok=false;allocation_failed=false;allocation_budget=budget;
    try{ok=fresh.RegisterSeries(f.series,f.policy).ok;}catch(const std::bad_alloc&){}
    const bool failed=allocation_failed;allocation_budget=-1;
    if(!failed){Require(ok,"series registration failed without allocation fault");complete=true;break;}
    ++faults;Require(!ok&&!f.Increment(fresh).ok&&fresh.SnapshotCurrent().empty()&&f.queue->Stats().queued==0,"failed series registration became observable");
    Require(fresh.RegisterSeries(f.series,f.policy).ok&&f.Increment(fresh).ok,"series allocation failure stranded a binding");
  }
  Require(complete&&faults>0,"series registration allocation sweep incomplete");std::cout<<"registry series allocation sites="<<faults<<'\n';
}
}  // namespace

int main() {
  RegistryPublication();
  QueuePublication();
  OneNodeQueueBinding();
  DescriptorScopeAdmission();
  RejectionObservationPublication();
  QueuePublicationFaults();
  SeriesBindingAndConcurrency();
  static_assert(sizeof(m::MetricUuid) == 16);
  static_assert(std::is_same_v<std::variant_alternative_t<1, m::MetricLabelValue::Base>, m::MetricUuid>);
  m::MetricDescriptor descriptor;
  descriptor.family = "sb_binary_label_test";
  descriptor.labels = {{"object_uuid", true, false, m::MetricLabelType::system_uuid},
                        {"mode", true, false, m::MetricLabelType::text},
                        {"secret", false, true, m::MetricLabelType::system_uuid}};
  const m::MetricLabelSet labels{{"object_uuid", Id()}, {"mode", "insert"}};
  const auto produced = m::Labels({{"object_uuid", Id()}, {"mode", "insert"}});
  Require(m::MakeMetricSeriesKey(descriptor.family, produced) ==
          m::MakeMetricSeriesKey(descriptor.family, labels), "producer flattened binary label");
  const auto invalid_produced = m::Labels({{"object_uuid", m::MetricUuid{}}, {"mode", ""}});
  Require(invalid_produced.size() == 2 && !m::ValidateMetricLabelSet(descriptor, invalid_produced).ok,
          "producer silently discarded invalid labels");
  auto confidential = labels; confidential.push_back({"secret", Id()});
  std::vector<std::string> omitted;
  const auto redacted = m::RedactSensitiveLabels(descriptor, confidential, false, &omitted);
  Require(std::get<m::MetricUuid>(redacted[0].value) == Id(), "redaction changed public binary identity");
  Require(std::equal(redacted.begin(), redacted.end(), labels.begin(), labels.end(),
              [](const auto& a, const auto& b) { return a.key == b.key && a.value == b.value; }) &&
          omitted == std::vector<std::string>{"secret"},
          "redaction did not omit sensitive identity with explicit metadata");
  Require(std::get<m::MetricUuid>(m::RedactSensitiveLabels(descriptor, confidential, true).back().value) == Id(),
          "authorized binary label lost");
  Require(std::get<m::MetricUuid>(confidential.back().value) == Id(), "redaction mutated source identity");
  Require(m::MakeMetricSeriesKey(descriptor.family, redacted) !=
          m::MakeMetricSeriesKey(descriptor.family, confidential), "projection retained confidential series key");
  m::RedactSensitiveLabels(descriptor, confidential, true, &omitted);
  Require(omitted.empty(), "authorized projection retained stale omission metadata");
  Require(m::ValidateMetricLabelSet(descriptor, labels).ok, "valid typed labels refused");
  const auto original = m::MakeMetricSeriesKey(descriptor.family, labels);
  Require(original.first == descriptor.family && original.second.size() == 2, "series key shape");
  Require(std::get<m::MetricUuid>(original.second[1].second) == Id(), "UUID was not retained as raw16");
  auto reversed = labels; std::reverse(reversed.begin(), reversed.end());
  Require(m::MakeMetricSeriesKey(descriptor.family, reversed) == original, "label order changed series");
  for (std::size_t n = 0; n < 16; ++n) for (unsigned bit = 0; bit < 8; ++bit) {
    auto changed = labels;
    std::get<m::MetricUuid>(changed[0].value).bytes[n] ^= 1u << bit;
    Require(m::MakeMetricSeriesKey(descriptor.family, changed) != original, "UUID bit omitted from metric series key");
  }
  for (unsigned version = 0; version < 256; ++version) {
    auto changed = labels;
    std::get<m::MetricUuid>(changed[0].value).bytes[6] = version;
    Require(m::ValidateMetricLabelSet(descriptor, changed).ok == ((version & 0xf0) == 0x70), "system UUID version admission");
  }
  for (unsigned variant = 0; variant < 256; ++variant) {
    auto changed = labels;
    std::get<m::MetricUuid>(changed[0].value).bytes[8] = variant;
    Require(m::ValidateMetricLabelSet(descriptor, changed).ok == ((variant & 0xc0) == 0x80), "system UUID variant admission");
  }
  auto data_descriptor = descriptor;
  data_descriptor.labels[0].value_type = m::MetricLabelType::uuid_value;
  for (unsigned version = 0; version < 256; ++version) {
    auto data = labels;
    std::get<m::MetricUuid>(data[0].value).bytes[6] = version;
    Require(m::ValidateMetricLabelSet(data_descriptor, data).ok ==
        ((version >> 4) >= 1 && (version >> 4) <= 7), "user UUID version policy");
    Require(std::get<m::MetricUuid>(m::MakeMetricSeriesKey(descriptor.family, data).second[1].second).bytes[6] == version,
            "user UUID was rewritten to a system identity");
  }
  auto bad = labels; bad[0].value = m::MetricUuid{};
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "nil UUID admitted");
  bad[0].value = std::string("019f0000-0000-7000-8000-000000000001");
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "text UUID admitted");
  Require(m::MakeMetricSeriesKey(descriptor.family, bad) != original, "text UUID aliases binary identity");
  bad = labels; bad[1].value = Id();
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "UUID bypassed text label schema");
  bad = labels; bad.push_back(labels[0]);
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "duplicate label admitted");
  bad = labels; bad.pop_back();
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "required label absent");
  bad = labels; bad[1].key = "unknown";
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "unknown label admitted");
  bad = labels; bad[1].value = std::string{};
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "empty text label admitted");
  m::MetricLabelSet left{{"a", "b|c=d"}}, right{{"a", "b"}, {"c", "d"}};
  Require(m::MakeMetricSeriesKey("sb_x", left) != m::MakeMetricSeriesKey("sb_x", right), "delimiter series collision");
  left = {{"b", "c"}}; right = {{"a|b", "c"}};
  Require(m::MakeMetricSeriesKey("sb_x|a", left) != m::MakeMetricSeriesKey("sb_x", right), "family label delimiter collision");
  left = {{"a", std::string("b\0c", 3)}}; right = {{"a", "b"}};
  Require(m::MakeMetricSeriesKey("sb_x", left) != m::MakeMetricSeriesKey("sb_x", right), "embedded NUL truncated");
  std::map<m::MetricSeriesKey, unsigned> series;
  for (unsigned n = 0; n != 256; ++n) {
    auto modified = labels;
    std::get<m::MetricUuid>(modified[0].value).bytes[15] = n;
    Require(series.emplace(m::MakeMetricSeriesKey(descriptor.family, modified), n).second, "binary series aliased in map");
  }
  for (unsigned n = 0; n != 256; ++n) {
    auto modified = labels;
    std::get<m::MetricUuid>(modified[0].value).bytes[15] = n;
    Require(series.at(m::MakeMetricSeriesKey(descriptor.family, modified)) == n, "binary map lookup changed series");
  }
  std::cout << "metric_binary_label_key=passed checks=" << checks << '\n';
}
