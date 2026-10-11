// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_cleanup.hpp"
#include "transaction_metrics.hpp"
#include "metric_builtin_definitions.hpp"
#include "metric_history.hpp"
#include "metric_observation_queue.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

thread_local long allocation_budget=-1;
void* operator new(std::size_t bytes) {
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}
  if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(bytes?bytes:1))return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}

namespace m = scratchbird::core::metrics;
namespace t = scratchbird::transaction::mga;
namespace p = scratchbird::core::platform;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
m::MetricUuid Id(unsigned n) {
  m::MetricUuid id;
  id.bytes={1,159,0,0,0,0,112,0,128,0,0,0,0,0,
      static_cast<unsigned char>(n>>8),static_cast<unsigned char>(n)};
  return id;
}
constexpr std::array<const char*,5> families={"sb_mga_row_versions_reclaimed_total",
    "sb_mga_cleanup_horizon_local_transaction_id", "sb_mga_cleanup_retained_row_versions",
    "sb_mga_cleanup_retention_age_microseconds", "sb_mga_cleanup_blocked_total"};
constexpr m::u64 large=(m::u64{1}<<53)+1;
struct Fixture {
  std::shared_ptr<m::MetricObservationQueue> queue;
  m::MetricRegistry registry;
  std::map<std::string,std::pair<m::MetricDescriptor,m::MetricSeriesIdentity>> bindings;
  Fixture() {
    auto created=m::MetricObservationQueue::Create({Id(1),Id(2),{}},{64,1024*1024});
    Check(created.ok(),"owned component queue");queue=std::move(created.queue);
    Check(registry.BindObservationQueue(queue).ok,"retain component queue");
    for(unsigned i=0;i<families.size();++i) {
      m::MetricLabelSet labels={{"component","transaction.mga.cleanup"}};
      labels.push_back(i==0?m::MetricLabel{"result","reclaimed"}:
          i==4?m::MetricLabel{"reason","row_version_horizon"}:
               m::MetricLabel{"authority","local_inventory"});
      Bind(families[i],std::move(labels),registry);
    }
  }
  void Bind(const std::string& family,m::MetricLabelSet labels,m::MetricRegistry& target) {
    static const auto definitions=m::BuiltinMetricDescriptorDefinitions();
    const auto found=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.family==family;});
    Check(found!=definitions.end(),"exact compiled family definition");
    m::MetricDescriptor descriptor;
    static_cast<m::MetricDescriptorDefinition&>(descriptor)=*found;
    const auto ordinal=static_cast<unsigned>(bindings.size());
    descriptor.metric_uuid=Id(100+ordinal);descriptor.descriptor_generation=1;
    descriptor.label_schema_uuid=Id(200+ordinal);descriptor.label_schema_generation=1;
    descriptor.retention_policy_uuid=Id(3);descriptor.retention_policy_generation=1;
    descriptor.visibility_policy_uuid=Id(4);descriptor.visibility_policy_generation=1;
    descriptor.readiness=m::MetricReadiness::implemented;
    m::MetricRetentionPolicy policy;policy.policy_name="component-only explicit policy";
    policy.policy_uuid=Id(3);policy.generation=1;
    m::MetricHistoryBinding binding;
    static_cast<m::MetricDescriptorBinding&>(binding)=descriptor;
    binding.database_uuid=Id(1);binding.node_uuid=Id(2);
    const auto series=m::MakeMetricSeriesIdentity(descriptor,labels,policy,binding,Id(300+ordinal),1);
    Check(series.ok(),"component series payload");
    Check(target.RegisterDescriptor(descriptor).ok&&target.RegisterSeries(*series.record,policy).ok,
          "explicit component registration; not native catalog activation");
    bindings.emplace(family,std::make_pair(descriptor,*series.record));
  }
  void Read(const std::string& family,m::MetricScalar expected) {
    const auto acquired=queue->TryAcquire();Check(acquired.ok(),"actual observation missing");
    const auto& [descriptor,series]=bindings.at(family);
    const auto& sample=*acquired.lease.observation;
    Check(sample.binding.metric_uuid==descriptor.metric_uuid&&sample.binding.database_uuid==Id(1)&&
          sample.binding.node_uuid==Id(2)&&sample.series_uuid==series.series_uuid,"raw16 owner/series preserved");
    const auto decoded=m::DecodeMetricRawSample(descriptor,series,sample.bytes);
    Check(decoded.ok()&&decoded.record->value.value==expected&&!decoded.record->value.arithmetic_inexact,
          "exact native sample and scalar alternative");
    Check(queue->TryRemove(acquired.lease)==m::MetricQueueError::none,"drain checked component sample");
  }
};
t::LocalCleanupWorksetResult Result(m::u64 count) {
  t::LocalCleanupWorksetResult result;
  result.status={p::StatusCode::ok,p::Severity::info,p::Subsystem::transaction_mga};
  result.cleanup_horizon_authoritative=true;
  result.reclaimed_row_version_count=count;result.retained_row_version_count=count;
  result.horizon_blocked_row_version_count=count;
  result.authoritative_cleanup_horizon_local_transaction_id=count;
  result.oldest_retained_row_version_local_transaction_id=1;
  return result;
}
t::LocalCleanupWorksetRequest Request(m::u64 horizon) {
  t::LocalCleanupWorksetRequest request;
  t::TransactionInventoryEntry first,last;
  first.identity.local_id.value=1;first.begin_unix_epoch_millis=1000;
  last.identity.local_id.value=horizon;last.begin_unix_epoch_millis=1007;
  request.inventory.entries={first,last};
  return request;
}
void ExactCleanup() {
  for(const auto value:{large,std::numeric_limits<m::u64>::max()}) {
    Fixture f;
    auto request=Request(value);auto result=Result(value);
    const auto published=t::PublishLocalCleanupWorksetMetrics(request,result,f.registry);
    for(const auto& receipt:published)Check(receipt.attempted&&receipt.publication.ok,"all five actual observations");
    for(unsigned i=0;i<5;++i)f.Read(families[i],i==3?m::MetricScalar{7000.0}:m::MetricScalar{value});
    const auto overflow=t::PublishLocalCleanupWorksetMetrics(request,Result(1),f.registry);
    if(value==std::numeric_limits<m::u64>::max()) {
      Check(overflow[0].attempted&&!overflow[0].publication.ok&&
            overflow[0].publication.diagnostic_code=="METRIC.AGGREGATE_OVERFLOW"&&
            overflow[1].publication.ok&&overflow[2].publication.ok&&
            !overflow[4].publication.ok,"counter overflow retains later independent gauge receipts");
      f.Read(families[1],m::u64{1});f.Read(families[2],m::u64{1});f.Read(families[3],0.0);
      Check(!f.queue->TryAcquire().ok(),"overflow counters produce no observations");
      const auto values=f.registry.SnapshotCurrent();
      for(const auto& v:values)if(v.type==m::MetricType::counter)
        Check(std::get<m::u64>(v.value)==value,"overflow leaves exact previous counter unchanged");
    } else {
      for(const auto& receipt:overflow)Check(receipt.publication.ok,"nonoverflow successor admitted");
      for(unsigned i=0;i<5;++i)f.Read(families[i],i==3?m::MetricScalar{0.0}:
          m::MetricScalar{i==0||i==4?value+1:m::u64{1}});
    }
  }
}
void AgeAndRefusal() {
  for(unsigned fault=0;fault<6;++fault) {
    Fixture f;auto request=Request(large);const auto result=Result(large);
    if(fault==0)request.inventory.entries.pop_back();
    if(fault==1)request.inventory.entries.front().begin_unix_epoch_millis=0;
    if(fault==2)request.inventory.entries.back().begin_unix_epoch_millis=999;
    if(fault==3)request.inventory.entries.push_back(request.inventory.entries.front());
    if(fault==4)request.inventory.entries.push_back(request.inventory.entries.back());
    if(fault==5)request.inventory.entries.back().begin_unix_epoch_millis=0;
    const auto published=t::PublishLocalCleanupWorksetMetrics(request,result,f.registry);
    Check(!published[3].attempted&&!published[3].publication.ok&&
        std::string_view(published[3].diagnostic_code())=="METRIC.OBSERVATION_SOURCE_UNAVAILABLE",
        "missing/ambiguous/nonmonotonic timestamps never fabricated from transaction distance");
    for(unsigned i:{0,1,2,4}) {Check(published[i].publication.ok,"independent count observations survive unavailable age");f.Read(families[i],large);}
    Check(!f.queue->TryAcquire().ok(),"unknown age emitted no sample");
  }
  Fixture f;auto request=Request(large);auto result=Result(0);
  result.oldest_retained_row_version_local_transaction_id=0;
  result.authoritative_cleanup_horizon_local_transaction_id=large;
  const auto zero=t::PublishLocalCleanupWorksetMetrics(request,result,f.registry);
  Check(!zero[4].attempted,"no blocked event for zero blocked count");
  f.Read(families[0],m::u64{0});f.Read(families[1],large);f.Read(families[2],m::u64{0});f.Read(families[3],0.0);
  result.cleanup_horizon_authoritative=false;
  for(const auto& outcome:t::PublishLocalCleanupWorksetMetrics(request,result,f.registry))
    Check(!outcome.attempted,"failed workset emits no completion measurements");
  Check(!f.queue->TryAcquire().ok(),"failed workset has no metric effects");
  result.cleanup_horizon_authoritative=true;
  result.status.code=p::StatusCode::platform_required_feature_missing;
  for(const auto& outcome:t::PublishLocalCleanupWorksetMetrics(request,result,f.registry))
    Check(!outcome.attempted,"non-OK primary status has no completion observations");
  m::MetricRegistry empty;
  result=Result(large);
  for(const auto& outcome:t::PublishLocalCleanupWorksetMetrics(request,result,empty))
    Check(outcome.attempted&&!outcome.publication.ok,"unbound publication failure retained per field");
  Check(empty.Descriptors().empty()&&empty.SnapshotCurrent().empty(),"no invented runtime activation");
  request.inventory_authoritative=true;request.inventory=t::MakeEmptyLocalTransactionInventory();
  const auto real=t::ApplyLocalCleanupWithAuthoritativeInventory(request);
  Check(real.ok()&&!real.physical_storage_mutated,"real empty cleanup candidate unchanged by observation failures");
  for(unsigned i=0;i<4;++i)Check(real.metric_outcomes[i].attempted&&!real.metric_outcomes[i].publication.ok,
      "actual workset retains unbound default registry diagnostics");
}
void MeasuredAgeBounds() {
  for(const auto timestamp:{m::u64{1000},std::numeric_limits<m::u64>::max()}) {
    Fixture f;auto request=Request(large);auto result=Result(large);
    request.inventory.entries.back().begin_unix_epoch_millis=timestamp;
    const auto published=t::PublishLocalCleanupWorksetMetrics(request,result,f.registry);
    for(const auto& receipt:published)Check(receipt.publication.ok,"measured age boundary admitted");
    for(unsigned i=0;i<5;++i)f.Read(families[i],i==3?
        m::MetricScalar{static_cast<double>(timestamp-1000)*1000.0}:m::MetricScalar{large});
  }
}
void AllocationOutcomes() {
  unsigned faults=0,partial=0;
  for(long at=0;at<20000;++at) {
    Fixture f;const auto request=Request(large);const auto result=Result(large);
    allocation_budget=at;
    const auto outcomes=t::PublishLocalCleanupWorksetMetrics(request,result,f.registry);
    const bool injected=allocation_budget<0;
    allocation_budget=-1;
    unsigned accepted=0;
    for(unsigned i=0;i<5;++i) {
      Check(outcomes[i].attempted,"allocation failure must not lose or suppress independent outcomes");
      if(outcomes[i].publication.ok) {
        Check(outcomes[i].failure==t::LocalCleanupMetricFailure::none,"accepted result mislabeled");
        ++accepted;f.Read(families[i],i==3?m::MetricScalar{7000.0}:m::MetricScalar{large});
      } else {
        Check(outcomes[i].diagnostic_code()[0]!='\0',"allocation refusal requires retained diagnostic");
      }
    }
    Check(!f.queue->TryAcquire().ok(),"no accepted sample may be lost from receipts");
    for(const auto& values:{f.registry.SnapshotCurrent(),f.registry.SnapshotHistory()}) {
      Check(values.size()==accepted,"actual effects must agree with retained per-field outcomes");
      std::array<bool,5> seen{};
      for(const auto& value:values) {
        const auto found=std::find(families.begin(),families.end(),value.family);
        Check(found!=families.end(),"unexpected metric in retained values");
        const auto i=static_cast<unsigned>(found-families.begin());
        Check(!seen[i]&&outcomes[i].publication.ok&&value.value==
            (i==3?m::MetricScalar{7000.0}:m::MetricScalar{large}),"retained per-family values must match receipts");
        seen[i]=true;
      }
    }
    Check(request.inventory.entries[1].begin_unix_epoch_millis==1007&&
        result.reclaimed_row_version_count==large,"producer must not mutate retained cleanup inputs");
    if(!injected){Check(accepted==5&&faults>0&&partial>0,"nonvacuous complete allocation sweep");
      std::cout<<"cleanup allocation sites="<<faults<<" partial publications="<<partial<<'\n';return;}
    ++faults;if(accepted>0&&accepted<5)++partial;
  }
  throw std::runtime_error("allocation sweep exceeded its explicit bound");
}
void DefaultRegistryAllocation() {
  const auto request=Request(large);const auto result=Result(large);
  allocation_budget=0;
  const auto failed=t::PublishLocalCleanupWorksetMetrics(request,result);
  const bool injected=allocation_budget<0;allocation_budget=-1;
  Check(injected,"first-use registry allocation fault reached");
  for(const auto& outcome:failed)Check(!outcome.attempted&&!outcome.publication.ok&&
      outcome.failure==t::LocalCleanupMetricFailure::resource_exhausted,
      "source acquisition failure must retain primary result without inventing an observation");
  // Retry source acquisition, not a previously accepted observation/workset.
  for(const auto& outcome:t::PublishLocalCleanupWorksetMetrics(request,result))
    Check(outcome.attempted&&!outcome.publication.ok,
          "successful registry construction is still unbound, not producer activation");
}
void TransactionCounters() {
  Fixture f;
  auto& registry=m::DefaultMetricRegistry();
  Check(registry.Descriptors().empty()&&registry.BindObservationQueue(f.queue).ok,"component explicit default queue");
  constexpr std::array<const char*,6> names={"sb_tx_begin_total","sb_tx_commit_total","sb_tx_rollback_total",
      "sb_tx_abort_total","sb_tx_failure_total","sb_tx_cluster_fail_closed_total"};
  constexpr std::array<const char*,6> operations={"begin","commit","rollback","abort","failure","cluster_fail_closed"};
  for(unsigned i=0;i<names.size();++i)f.Bind(names[i],{{"component","transaction.mga"},
      {"operation",operations[i]},{"result","ok"}},registry);
  f.Bind("sb_tx_active_transactions",{{"component","transaction.mga"}},registry);
  t::TransactionMetrics metrics;
  const auto begin=[&]{metrics.RecordBegin();f.Read(names[0],metrics.Snapshot().begin_total);
      f.Read("sb_tx_active_transactions",metrics.Snapshot().active_current);};
  begin();begin();
  metrics.RecordCommit();f.Read(names[1],m::u64{1});f.Read("sb_tx_active_transactions",m::u64{1});
  metrics.RecordRollback();f.Read(names[2],m::u64{1});f.Read("sb_tx_active_transactions",m::u64{0});
  begin();metrics.RecordAbort();f.Read(names[3],m::u64{1});f.Read("sb_tx_active_transactions",m::u64{0});
  metrics.RecordFailure();f.Read(names[4],m::u64{1});
  metrics.RecordClusterFailClosed();f.Read(names[5],m::u64{1});
  Check(!f.queue->TryAcquire().ok(),"no extra transaction samples");
}
}
int main() {
  try {DefaultRegistryAllocation();ExactCleanup();AgeAndRefusal();MeasuredAgeBounds();AllocationOutcomes();TransactionCounters();}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
  std::cout<<"exact native transaction and cleanup observations passed\n";
}
