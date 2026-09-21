// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_label_key.hpp"
#include "metric_producer.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <type_traits>
#include <atomic>
#include <new>
#include <thread>

namespace { thread_local long allocation_budget=-1; }
void* operator new(std::size_t size) {
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}
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
}  // namespace

int main() {
  RegistryPublication();
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
  const auto redacted = m::RedactSensitiveLabels(descriptor, confidential, false);
  Require(std::get<m::MetricUuid>(redacted[0].value) == Id(), "redaction changed public binary identity");
  Require(std::get<std::string>(redacted.back().value) == "<redacted>", "redaction leaked sensitive identity");
  Require(std::get<m::MetricUuid>(m::RedactSensitiveLabels(descriptor, confidential, true).back().value) == Id(),
          "authorized binary label lost");
  Require(std::get<m::MetricUuid>(confidential.back().value) == Id(), "redaction mutated source identity");
  Require(!m::ValidateMetricLabelSet(descriptor, redacted).ok, "presentation redaction became identity authority");
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
