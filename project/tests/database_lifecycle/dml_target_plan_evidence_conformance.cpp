// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/dml_target_plan_identity_evidence.hpp"
#include "typed_update_carrier_codec.hpp"
#include <array>
#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace wire = scratchbird::wire;
int main() try {
  const auto require = [](bool ok) {
    if (!ok) throw std::runtime_error("native target plan evidence oracle failed");
  };
  const auto id = [](unsigned n) {
    return api::EngineUuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,static_cast<std::uint8_t>(n)}};
  };
  api::DmlTargetAccessPlan plan;
  plan.database_uuid=id(1); plan.relation_uuid=id(2); plan.row_uuid=id(3); plan.index_uuid=id(4);
  plan.row_uuids={id(5),id(6),id(5)};
  const std::array<std::string,7> suffixes{
      "database_uuid","relation_uuid","row_uuid","index_uuid","row_uuids.0","row_uuids.1","row_uuids.2"};
  const std::array<api::EngineUuid,7> expected{id(1),id(2),id(3),id(4),id(5),id(6),id(5)};
  std::size_t controls=0;
  for (const auto prefix : {"dml_target_access_plan", "merge_target_access_plan"}) {
    std::vector<api::EngineEvidenceReference> output{{"sentinel",id(9)}};
    const auto source = std::string_view(prefix)=="merge_target_access_plan"
        ? std::optional<std::uint64_t>{7} : std::nullopt;
    const std::string scope=std::string(prefix)+(source ? ".7" : "");
    api::AppendDmlTargetPlanIdentityEvidence(plan,prefix,&output,source);
    require(output.size()==8);
    for (std::size_t n=0;n<expected.size();++n)
      require(output[n+1].evidence_kind==scope+"."+suffixes[n] &&
              std::get<api::EngineUuid>(output[n+1].evidence_id)==expected[n]);
    const auto hashes = [&](const auto& rows) {
      std::vector<wire::TypedUpdateResultEvidenceReference> refs;
      for (const auto& row:rows) refs.push_back({row.evidence_kind,{},std::get<api::EngineUuid>(row.evidence_id).bytes});
      wire::TypedUpdateResultCarrier result;
      result.update_descriptor_uuid=result.operation_uuid=result.owning_transaction_uuid=result.relation_uuid=id(8).bytes;
      std::pair<wire::TypedUpdateHash,wire::TypedUpdateHash> value;
      wire::TypedUpdateCarrierError error;
      require(wire::ComputeTypedUpdateResultInnerEvidence(result,refs,&value.first,&value.second,&error));
      return value;
    };
    const auto original=hashes(output);
    if (source) {
      std::vector<api::EngineEvidenceReference> other{{"sentinel",id(9)}};
      api::AppendDmlTargetPlanIdentityEvidence(plan,prefix,&other,8);
      require(hashes(other)!=original);
    }
    for (std::size_t n=1;n<output.size();++n) {
      auto changed=output;
      std::get<api::EngineUuid>(changed[n].evidence_id).bytes.back()^=0x40;
      const auto altered=hashes(changed);
      require(original.first!=altered.first && original.second!=altered.second);
      ++controls;
    }
    auto swapped=output;
    std::swap(swapped[5].evidence_id,swapped[6].evidence_id);
    require(hashes(swapped)!=original);
    auto duplicate_lost=output; duplicate_lost.pop_back();
    require(hashes(duplicate_lost)!=original);
  }
  const auto reject=[&](const api::DmlTargetAccessPlan& invalid,std::string_view prefix) {
    std::vector<api::EngineEvidenceReference> output{{"sentinel",id(9)}};
    bool refused=false;
    try { api::AppendDmlTargetPlanIdentityEvidence(invalid,prefix,&output); }
    catch(const std::invalid_argument&) { refused=true; }
    require(refused && output.size()==1 && output[0].evidence_kind=="sentinel" &&
            std::get<api::EngineUuid>(output[0].evidence_id)==id(9));
    ++controls;
  };
  for (const auto prefix : {"", "other", "dml_target_access_plan.uuid", "merge_target_access_plan"}) reject(plan,prefix);
  for (unsigned field=0;field<7;++field) {
    for(unsigned bit=0;bit<128;++bit) {
      auto changed=plan;
      auto* target=field==0?&changed.database_uuid:field==1?&changed.relation_uuid:
                   field==2?&changed.row_uuid:field==3?&changed.index_uuid:&changed.row_uuids[field-4];
      target->bytes[bit/8]^=static_cast<std::uint8_t>(1u<<(bit%8));
      // Independent exact RFC variant and system version oracle.
      if ((target->bytes[6]&0xf0)!=0x70 || (target->bytes[8]&0xc0)!=0x80) {
        reject(changed,"dml_target_access_plan");
      } else {
        std::vector<api::EngineEvidenceReference> output;
        api::AppendDmlTargetPlanIdentityEvidence(changed,"dml_target_access_plan",&output);
        require(output.size()==7 && std::get<api::EngineUuid>(output[field].evidence_id)==*target);
        ++controls;
      }
    }
  }
  auto nil_member=plan; nil_member.row_uuids[2]={}; reject(nil_member,"dml_target_access_plan");
  std::vector<api::EngineEvidenceReference> empty;
  api::AppendDmlTargetPlanIdentityEvidence({},"dml_target_access_plan",&empty);
  require(empty.empty());
  bool null_refused=false;
  try { api::AppendDmlTargetPlanIdentityEvidence(plan,"dml_target_access_plan",nullptr); }
  catch(const std::invalid_argument&) {null_refused=true;}
  require(null_refused);
  std::cout<<"native_target_plan_evidence=passed controls="<<controls<<'\n';
  return 0;
} catch(const std::exception& error) {
  std::cerr<<error.what()<<'\n'; return 1;
}
