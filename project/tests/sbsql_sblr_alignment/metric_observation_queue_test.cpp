// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define METRIC_SAMPLE_CODEC_MAIN MetricQueueSampleBaseMain
#include "metric_sample_codec_test.cpp"
#undef METRIC_SAMPLE_CODEC_MAIN
#include "metric_observation_queue.hpp"
#include <thread>
#include <atomic>
#include <set>
namespace {
using QE=m::MetricQueueError;
auto Queue(const SampleFixture& f,std::size_t count=4,std::size_t bytes=4*m::kMetricSampleMaxBytes){
  auto r=m::MetricObservationQueue::Create({f.sample.database_uuid,f.sample.node_uuid,f.sample.cluster_uuid},{count,bytes});
  Check(r.ok(),"valid queue construction");return std::move(r.queue);
}
void QueueConstruction(){
  const auto f=Sample();const m::MetricQueueBinding original{f.sample.database_uuid,f.sample.node_uuid,{}};
  for(unsigned n=0;n<8;++n){auto binding=original;m::MetricQueueLimits limits{4,10000};
    if(n==0)binding.database_uuid={};if(n==1)binding.node_uuid=Id(10,4);if(n==2)binding.cluster_uuid=Id(2,1);
    if(n==3)limits.maximum_samples=0;if(n==4)limits.maximum_samples=65537;
    if(n==5)limits.maximum_wire_bytes=0;if(n==6)limits.maximum_wire_bytes=64*1024*1024+1;
    if(n==7)binding.database_uuid.bytes[8]=0;
    const auto r=m::MetricObservationQueue::Create(binding,limits);Check(!r.ok()&&!r.queue&&r.error==QE::invalid_configuration,"invalid queue constructed");
  }
  auto q=Queue(f);auto r=q->TryAcquire();Check(r.error==QE::empty&&!r.lease.token&&!r.lease.observation,"empty queue invented observation");
  Check(q->TryRemove({})==QE::stale_lease,"empty removal succeeded");
}
void QueueIdentityAndLeases(){
  const auto f=Sample();const auto expected=GoldenSample(f);auto q=Queue(f,2,expected.size()*2);
  Check(q->TryEnqueue(f.descriptor,f.series,f.sample)==QE::none,"observation not admitted");
  auto first=q->TryAcquire();Check(first.ok()&&first.lease.observation->bytes==expected&&
      first.lease.observation->binding==static_cast<const m::MetricHistoryBinding&>(f.sample)&&
      first.lease.observation->sample_uuid==f.sample.sample_uuid&&first.lease.observation->series_uuid==f.sample.series_uuid&&
      first.lease.observation->series_definition_generation==17&&
      first.lease.observation->source_sequence==f.sample.source_sequence,"queue changed exact sample binding/bytes");
  Check(q->TryAcquire().error==QE::busy,"head acquired twice");
  Check(q->TryEnqueue(f.descriptor,f.series,f.sample)==QE::duplicate,"duplicate queued identity accepted");
  auto other=Queue(f);Check(other->TryEnqueue(f.descriptor,f.series,f.sample)==QE::none,"second node-local queue fixture admission");
  auto foreign=other->TryAcquire();Check(foreign.ok()&&foreign.lease.token==first.lease.token,"equal queue-local tokens fixture");
  Check(q->TryRemove(foreign.lease)==QE::stale_lease,"foreign same-token lease removed head");
  auto forged=first.lease;forged.observation=std::make_shared<m::MetricQueuedObservation>(*first.lease.observation);
  Check(q->TryRemove(forged)==QE::stale_lease,"copied observation forged a live lease");
  Check(q->TryRelease(first.lease)==QE::none,"head release failed");auto retry=q->TryAcquire();
  Check(retry.ok()&&retry.lease.token>first.lease.token&&retry.lease.observation==first.lease.observation,"release did not retry exact retained head");
  Check(q->TryRemove(first.lease)==QE::stale_lease,"stale retry token removed head");
  Check(q->TryRemove(retry.lease)==QE::none,"head removal failed");
  Check(q->TryRemove(retry.lease)==QE::stale_lease,"double remove succeeded");
  Check(q->Stats().queued==0&&q->Stats().wire_bytes==0&&q->Stats().admitted==1&&q->Stats().removed==1,"quiescent queue counters incorrect");
  q.reset();Check(first.lease.observation->bytes==expected,"lease bytes died with queue");
}
void QueueCapacityAndScope(){
  auto f=Sample();const auto bytes=GoldenSample(f).size();auto q=Queue(f,2,2*bytes);
  auto second=f.sample;second.sample_uuid=Id(50);auto third=f.sample;third.sample_uuid=Id(51);
  Check(q->TryEnqueue(f.descriptor,f.series,f.sample)==QE::none&&q->TryEnqueue(f.descriptor,f.series,second)==QE::none,"exact queue capacity refused");
  auto head=q->TryAcquire();Check(head.ok(),"capacity fixture acquire");
  Check(q->TryEnqueue(f.descriptor,f.series,third)==QE::full,"leased head stopped counting toward capacity");
  Check(q->Stats().queued==2&&q->Stats().wire_bytes==2*bytes,"full queue evicted data");
  Check(q->TryRemove(head.lease)==QE::none&&q->TryEnqueue(f.descriptor,f.series,third)==QE::none,"released capacity not reusable");
  auto next=q->TryAcquire();Check(next.ok()&&next.lease.observation->sample_uuid==second.sample_uuid,"queue reordered FIFO");
  Check(q->TryRemove(next.lease)==QE::none,"second remove");next=q->TryAcquire();
  Check(next.ok()&&next.lease.observation->sample_uuid==third.sample_uuid,"queue lost third sample");
  auto small=Queue(f,3,bytes-1);Check(small->TryEnqueue(f.descriptor,f.series,f.sample)==QE::full&&small->Stats().queued==0,"oversized sample partially enqueued");
  for(unsigned n=0;n<7;++n){auto sample=f.sample;
    if(n==0)sample.database_uuid=Id(55);if(n==1)sample.node_uuid=Id(55);
    if(n==2)sample.cluster_uuid=Id(55);if(n==3)sample.publication_time_utc_ns=9;
    if(n==4)sample.descriptor_generation++;if(n==5)sample.value.value=std::monostate{};
    if(n==6)sample.series_definition_generation++;
    Check(q->TryEnqueue(f.descriptor,f.series,sample)==QE::invalid_observation,"invalid scope/published/binding sample enqueued");
  }
  const auto local=Queue(f);f.descriptor.cluster_only=true;f.descriptor.namespace_path="cluster.sys.metrics.test";
  f.series.cluster_uuid=f.sample.cluster_uuid=Id(56);f.series.scope_class="cluster";f.series.namespace_path=f.descriptor.namespace_path;
  f.series.series_key={f.series.database_uuid,f.series.node_uuid,f.series.cluster_uuid,f.series.metric_uuid,f.series.label_schema_uuid,
      std::get<5>(f.series.series_key)};
  auto cluster=Queue(f);Check(cluster->TryEnqueue(f.descriptor,f.series,f.sample)==QE::none,"configured cluster scope rejected");
  Check(local->TryEnqueue(f.descriptor,f.series,f.sample)==QE::invalid_observation,"unconfigured local queue admitted cluster");
}
void QueueTypes(){
  for(const auto& value:std::vector<m::MetricScalar>{U(-1),std::numeric_limits<I>::min(),-0.,Binary("1e4000"),
      Decimal("9999999999999999999999999999999999",6111),true,std::string("x\0y",3),Id(31,1),m::MetricEnumValue{U(-1)}}){
    const auto f=Sample(value);auto q=Queue(f);Check(q->TryEnqueue(f.descriptor,f.series,f.sample)==QE::none,"typed queue admission failed");
    const auto lease=q->TryAcquire();Check(lease.ok()&&lease.lease.observation->bytes==GoldenSample(f),"queue narrowed exact scalar");
  }
}
void QueueAllocationFaults(){
  const auto f=Sample();const auto expected=GoldenSample(f);unsigned faults=0;bool complete=false;
  for(long n=0;n<1000;++n){auto q=Queue(f);codec_fault::remaining=n;codec_fault::fired=false;
    const auto status=q->TryEnqueue(f.descriptor,f.series,f.sample);const bool fired=codec_fault::fired;codec_fault::remaining=-1;
    if(fired){++faults;Check(status==QE::resource_exhausted&&q->Stats().queued==0&&q->Stats().wire_bytes==0&&q->Stats().admitted==0,"allocation failure partially admitted");}
    else {Check(status==QE::none,"unfaulted queue admission failed");complete=true;break;}
    Check(q->TryEnqueue(f.descriptor,f.series,f.sample)==QE::none,"allocation retry failed");
    const auto result=q->TryAcquire();Check(result.ok()&&result.lease.observation->bytes==expected,"allocation retry changed bytes");
  }
  Check(complete&&faults,"queue allocation sweep incomplete");std::cout<<"queue admission allocation sites="<<faults<<'\n';
  // On the Linux standard library this crosses a deque block boundary. Other
  // layouts still exercise fault atomicity with previously retained entries.
  unsigned growth_faults=0;bool growth_complete=false;
  for(long point=0;point<1000;++point){auto q=Queue(f,64,64*expected.size());
    for(unsigned i=0;i<31;++i){auto sample=f.sample;sample.sample_uuid=Id(100+i);
      Check(q->TryEnqueue(f.descriptor,f.series,sample)==QE::none,"growth fixture prefill");}
    codec_fault::remaining=point;codec_fault::fired=false;
    const auto status=q->TryEnqueue(f.descriptor,f.series,f.sample);const bool fired=codec_fault::fired;codec_fault::remaining=-1;
    if(fired){++growth_faults;Check(status==QE::resource_exhausted&&q->Stats().queued==31&&q->Stats().admitted==31&&
        q->Stats().wire_bytes==31*expected.size(),"deque growth failure mutated existing queue");}
    else {Check(status==QE::none&&q->Stats().queued==32,"unfaulted growth failed");growth_complete=true;}
    for(unsigned i=0;i<31;++i){const auto r=q->TryAcquire();Check(r.ok()&&r.lease.observation->sample_uuid==Id(100+i),"growth fault damaged prior FIFO");
      Check(q->TryRemove(r.lease)==QE::none,"growth fixture drain failed");}
    if(growth_complete)break;
  }
  Check(growth_complete&&growth_faults,"populated queue allocation sweep incomplete");std::cout<<"queue growth allocation sites="<<growth_faults<<'\n';
  for(long n=0;n<100;++n){codec_fault::remaining=n;codec_fault::fired=false;
    const auto r=m::MetricObservationQueue::Create({f.sample.database_uuid,f.sample.node_uuid,{}},{4,10000});
    const bool fired=codec_fault::fired;codec_fault::remaining=-1;
    if(fired)Check(!r.ok()&&!r.queue&&r.error==QE::resource_exhausted,"queue construction failure exposed partial queue");
    else{Check(r.ok(),"unfaulted queue constructor failed");break;}
  }
}
void QueueConcurrency(){
  const auto f=Sample();auto q=Queue(f,7,7*m::kMetricSampleMaxBytes);
  constexpr unsigned producers=4,each=80;std::atomic<unsigned> done{0},errors{0};std::vector<std::thread> workers;
  for(unsigned p=0;p<producers;++p)workers.emplace_back([&,p](){
    for(unsigned i=0;i<each;++i){auto sample=f.sample;sample.sample_uuid=Id(i+1);sample.sample_uuid.bytes[14]=p+1;
      sample.source_sequence=i+1;
      for(;;){const auto status=q->TryEnqueue(f.descriptor,f.series,sample);if(status==QE::none)break;
        if(status!=QE::full&&status!=QE::busy){++errors;break;}std::this_thread::yield();}
    }++done;
  });
  std::set<m::MetricUuid> seen;unsigned consumed=0;std::array<U,producers> last{};
  while(done.load()!=producers||q->Stats().queued){
    const auto r=q->TryAcquire();if(r.error==QE::busy||r.error==QE::empty){std::this_thread::yield();continue;}
    if(!r.ok()){++errors;break;}const auto& observation=*r.lease.observation;
    const auto decoded=m::DecodeMetricRawSample(f.descriptor,f.series,observation.bytes);
    if(!decoded.ok()||!seen.insert(observation.sample_uuid).second)++errors;
    const auto p=unsigned(observation.sample_uuid.bytes[14])-1;
    if(p>=producers||observation.source_sequence!=last[p]+1)++errors;else last[p]=observation.source_sequence;
    for(;;){const auto status=q->TryRemove(r.lease);if(status==QE::none)break;if(status!=QE::busy){++errors;break;}std::this_thread::yield();}
    ++consumed;
  }
  for(auto& worker:workers)worker.join();
  Check(!errors&&consumed==producers*each&&q->Stats().admitted==consumed&&q->Stats().removed==consumed&&
      !q->Stats().queued&&!q->Stats().wire_bytes,"concurrent queue lost/reordered/duplicated an admitted observation");
}
}
int main(){MetricQueueSampleBaseMain();const auto before=checks;QueueConstruction();QueueIdentityAndLeases();QueueCapacityAndScope();QueueTypes();QueueAllocationFaults();QueueConcurrency();
  numeric::ReleaseReal128ThreadCache();std::cout<<"metric queue checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
