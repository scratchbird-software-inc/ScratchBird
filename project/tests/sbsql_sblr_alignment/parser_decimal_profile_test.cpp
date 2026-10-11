// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Parser binding/lowering/verification evidence, not live receipt admission.
#include "binder/descriptor_authority.hpp"
#include "lowering/lowering.hpp"
#include "cst/cst.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "wire/contextual_operand_freeze.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace p = scratchbird::parser::sbsql;
namespace dt = scratchbird::core::datatypes;
namespace api = scratchbird::engine::internal_api;
namespace s = scratchbird::engine::sblr;
using Uuid = scratchbird::core::platform::Uuid;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* detail) {
  ++checks;
  if (!ok) throw std::runtime_error(detail);
}
constexpr Uuid Id(unsigned n) {
  return {{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,
      static_cast<unsigned char>(n >> 8),static_cast<unsigned char>(n)}}};
}
p::NativeRelationalBindingContext Context(unsigned cohort) {
  p::NativeRelationalBindingContext c;
  c.bound_ast_uuid = Id(101);
  c.catalog_epoch_uuid = Id(102);
  c.security_context_uuid = Id(103);
  c.statement_uuid = Id(104);
  c.owning_transaction_uuid = Id(105);
  c.statement_snapshot_uuid = Id(106);
  c.statement_metadata_snapshot_uuid = Id(107);
  c.local_transaction_id = 73;
  c.snapshot_visible_through_local_transaction_id = 72;
  auto& a = c.engine_statement_authority;
  a.statement_uuid=c.statement_uuid; a.transaction_uuid=c.owning_transaction_uuid;
  a.statement_snapshot_uuid=c.statement_snapshot_uuid;
  a.statement_metadata_snapshot_uuid=c.statement_metadata_snapshot_uuid;
  a.catalog_epoch_uuid=c.catalog_epoch_uuid;
  a.local_transaction_id=73; a.snapshot_visible_through_local_transaction_id=72;
  const dt::DatatypeTypeCodecIdentityRowV1* row = nullptr;
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (entry.legacy_fields.catalog_generation == cohort &&
        entry.legacy_fields.canonical_binary_type_code ==
            static_cast<unsigned>(dt::CanonicalTypeId::decimal)) row=&entry.legacy_fields;
  }
  Check(row != nullptr, "missing exact DECIMAL fixture cohort");
  for (unsigned slot : {1U,2U}) {
    p::NativeDescriptorBindingInput d;
    d.descriptor_id=slot; d.descriptor_uuid=Id(200+slot); d.type_uuid=row->type_uuid;
    d.nullability=p::BoundNullability::kNonNull;
    d.width_precision_scale.precision=slot == 1 ? 18 : 50;
    d.width_precision_scale.scale=slot == 1 ? 2 : 5;
    d.descriptor_generation=row->descriptor_generation;
    d.type_generation=row->type_generation;
    d.codec_id=row->codec_id; d.codec_version=row->codec_version;
    d.codec_generation=row->codec_generation; d.statement_receipt_uuid=Id(301);
    d.datatype_catalog_snapshot_uuid=row->catalog_snapshot_uuid;
    d.datatype_catalog_generation=row->catalog_generation;
    d.datatype_registry_generation=row->registry_generation;
    d.decimal_codec_profile=dt::DecimalCodecProfile{
        slot == 1 ? dt::kExactDecimal24CodecUuid : dt::kExactDecimal40CodecUuid,1};
    c.descriptors.push_back(d);
  }
  c.expressions={{1,1,std::nullopt,std::nullopt,1},
                 {2,2,std::nullopt,std::nullopt,2}};
  c.outputs={{1,1,"column_1",1,true,0},{2,2,"column_2",2,true,1}};
  return c;
}
p::SessionContext Session() {
  p::SessionContext session;
  session.authenticated=true; session.session_uuid=Id(402);
  session.connection_uuid=Id(403); session.database_uuid=Id(404);
  session.dialect_profile_uuid=Id(405);
  session.catalog_epoch=7; session.security_policy_epoch=11; session.descriptor_epoch=13;
  session.admitted_parser_package_uuid=Id(401);
  return session;
}
p::BoundStatement Bind(const p::NativeRelationalBindingContext& context) {
  const auto cst=p::BuildCst("VALUES (1, 2);");
  p::ParserConfig config;
  config.parser_uuid=Id(401); config.bundle_contract_id="sbp_sbsql@decimal-profile";
  config.build_id="decimal-profile";
  auto bound=p::BindAst(p::BuildAst(cst),cst,config,Session(),{},&context);
  // Component fixture supplies the command-registry projection; it cannot
  // create a live engine statement receipt or authorize query execution.
  bound.command_registry_snapshot_uuid=context.catalog_epoch_uuid;
  return bound;
}
p::SblrEnvelope Lower(const p::BoundStatement& b) {
  return p::LowerToSblr(b,p::BuildCst("VALUES (1, 2);"),Session());
}
void Test(unsigned cohort) {
  const auto c=Context(cohort);
  unsigned profile_pairs=0;
  for (unsigned precision=1;precision<=76;++precision) {
    for(unsigned scale=0;scale<=precision;++scale) {
      auto d=c.descriptors[0];
      d.width_precision_scale.precision=precision;
      d.width_precision_scale.scale=scale;
      d.decimal_codec_profile=dt::DecimalCodecProfile{
          precision<=38 ? dt::kExactDecimal24CodecUuid : dt::kExactDecimal40CodecUuid,1};
      Check(p::MatchesNativeDecimalOccurrenceProfile(d,precision,scale),
            "valid precision/scale profile rejected");
      d.decimal_codec_profile->uuid=precision<=38
          ? dt::kExactDecimal40CodecUuid : dt::kExactDecimal24CodecUuid;
      Check(!p::MatchesNativeDecimalOccurrenceProfile(d,precision,scale),
            "opposite width accepted for precision/scale");
      ++profile_pairs;
    }
  }
  Check(profile_pairs==3002,"incomplete precision/scale matrix");
  for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& native=row.legacy_fields;
    if(native.catalog_generation!=cohort || native.canonical_binary_type_code==
        static_cast<unsigned>(dt::CanonicalTypeId::decimal))continue;
    auto d=c.descriptors[0];
    d.type_uuid=native.type_uuid;d.type_generation=native.type_generation;
    d.descriptor_generation=native.descriptor_generation;d.codec_id=native.codec_id;
    d.codec_version=native.codec_version;d.codec_generation=native.codec_generation;
    Check(!p::MatchesNativeDecimalOccurrenceProfile(d,18,2),
          "profile admitted on a valid non-DECIMAL base tuple");
  }
  scratchbird::parser::ipc::ParserStatementContext projected_context;
  projected_context.literal_preliminary_receipt_uuid=c.descriptors[0].statement_receipt_uuid;
  projected_context.literal_catalog_snapshot_uuid=c.descriptors[0].datatype_catalog_snapshot_uuid;
  projected_context.literal_catalog_generation=cohort;
  for(const auto& original:c.descriptors) {
    auto d=original;
    Check(p::PreserveNativeDescriptorAuthority(&d,projected_context) &&
          d.decimal_codec_profile==original.decimal_codec_profile,
          "complete supplied profile not preserved");
    auto stale=d;stale.decimal_codec_profile->generation=2;
    Check(!p::PreserveNativeDescriptorAuthority(&stale,projected_context) &&
          stale.decimal_codec_profile->generation==2 && stale.codec_id==d.codec_id,
          "invalid complete profile repaired or mutated");
    scratchbird::parser::ipc::ParserStatementContext::DescriptorProfile issued;
    issued.descriptor_uuid=d.descriptor_uuid;issued.type_uuid=d.type_uuid;
    issued.precision=*d.width_precision_scale.precision;
    issued.scale=*d.width_precision_scale.scale;
    projected_context.descriptor_profiles.push_back(issued);
    d.descriptor_generation=0;d.type_generation=0;d.codec_id.clear();
    d.codec_version=0;d.codec_generation=0;d.statement_receipt_uuid={};
    d.datatype_catalog_snapshot_uuid={};d.datatype_catalog_generation=0;
    d.datatype_registry_generation=0;
    auto invalid=d;invalid.decimal_codec_profile->generation=2;
    Check(!p::PreserveNativeDescriptorAuthority(&invalid,projected_context) &&
          invalid.codec_id.empty() && invalid.statement_receipt_uuid.is_nil(),
          "failed profile projection published partial authority");
    Check(p::PreserveNativeDescriptorAuthority(&d,projected_context) &&
          d.decimal_codec_profile==original.decimal_codec_profile &&
          d.codec_id==original.codec_id,"issued descriptor lost explicit profile");
  }
  const auto b=Bind(c);
  Check(b.bound && b.native_relational.bound,"explicit profiles failed native binding");
  Check(b.native_relational.descriptors.size()==2,"descriptor count changed");
  for (unsigned n=0;n<2;++n)
    Check(b.native_relational.descriptors[n].decimal_codec_profile ==
          c.descriptors[n].decimal_codec_profile,"binder dropped profile");
  const auto e=Lower(b);
  Check(e.operation_id=="query.execute","wrong canonical query route");
  const auto verified=p::VerifySblrEnvelope(e);
  if (!verified.admitted) for (const auto& d:verified.messages.diagnostics)
    std::cerr<<d.code<<":"<<d.message<<'\n';
  Check(verified.admitted,"explicit profiles failed canonical verifier");
  unsigned seen=0;
  for (const auto& operand:e.operands) {
    if (operand.type!="relational_descriptor_v3") continue;
    api::RelationalTypeDescriptor d;
    Check(operand.value.empty() && s::DecodeRelationalTypeDescriptorV1(
        operand.canonical_value_body.data(),operand.canonical_value_body.size(),&d),
        "descriptor transport not binary");
    Check(d.descriptor_id>=1 && d.descriptor_id<=2,"unexpected descriptor handle");
    Check(d.decimal_codec_profile==c.descriptors[d.descriptor_id-1].decimal_codec_profile,
          "lowering lost or interchanged parameterized codec");
    Check(p::MatchesNativeNumericDescriptorRecord(d,c.descriptors[d.descriptor_id-1]),
          "numeric proof lost matching binary profile");
    auto lost=d;
    lost.decimal_codec_profile.reset();
    Check(!p::MatchesNativeNumericDescriptorRecord(lost,c.descriptors[d.descriptor_id-1]),
          "numeric proof allowed profile loss");
    ++seen;
  }
  Check(seen==2,"descriptor transport count");
  auto aliased=c;
  aliased.descriptors[1].descriptor_uuid=aliased.descriptors[0].descriptor_uuid;
  Check(!Bind(aliased).bound,"same UUID admitted conflicting profile bindings");
  auto aliased_bound=b;
  aliased_bound.native_relational.descriptors[1].descriptor_uuid=
      aliased_bound.native_relational.descriptors[0].descriptor_uuid;
  Check(Lower(aliased_bound).messages.has_errors(),"lowerer allowed conflicting shared UUID");
  // Matching profiles permit distinct DAG handles for one immutable identity.
  aliased.descriptors[1].decimal_codec_profile=aliased.descriptors[0].decimal_codec_profile;
  aliased.descriptors[1].width_precision_scale.precision=25;
  Check(Bind(aliased).bound,"same profile distinct occurrence handles refused");
  Check(p::VerifySblrEnvelope(Lower(Bind(aliased))).admitted,
        "matching shared identity profile failed lowering");
  for (unsigned change=0;change<6;++change) {
    auto altered=e;
    auto operand=std::ranges::find_if(altered.operands,[](const auto& o) {
      return o.type=="relational_descriptor_v3" && o.name=="slot_1";
    });
    Check(operand!=altered.operands.end(),"missing descriptor for wire substitution");
    api::RelationalTypeDescriptor d;
    Check(s::DecodeRelationalTypeDescriptorV1(operand->canonical_value_body.data(),
          operand->canonical_value_body.size(),&d),"wire mutation baseline decode");
    switch(change) {
      case 0:d.decimal_codec_profile->uuid=dt::kExactDecimal40CodecUuid;break;
      case 1:d.decimal_codec_profile->generation=2;break;
      case 2:d.decimal_codec_profile->uuid.bytes[15]^=1;break;
      case 3:d.type_uuid=Id(0xd712);break;
      case 4:d.datatype_registry_generation=999;break;
      case 5:d.descriptor_uuid=c.descriptors[1].descriptor_uuid;break;
    }
    Check(s::EncodeRelationalTypeDescriptorV1(d,&operand->canonical_value_body),
          "structural codec unexpectedly rejected semantic mutation");
    Check(!p::VerifySblrEnvelope(altered).admitted,
          "verifier ignored structurally valid profile substitution");
  }
  const auto frozen=p::FreezeContextualReservationSkeletonV2(b,e,{});
  Check(frozen.has_value(),"reservation freeze failed");
  for (unsigned change=0;change<12;++change) {
    auto invalid=c;
    auto& d=invalid.descriptors[0];
    switch(change) {
      case 0:d.decimal_codec_profile->uuid=dt::kExactDecimal40CodecUuid;break;
      case 1:d.decimal_codec_profile->generation=2;break;
      case 2:d.decimal_codec_profile->uuid={};break;
      case 3:d.width_precision_scale.precision.reset();break;
      case 4:d.width_precision_scale.scale.reset();break;
      case 5:d.width_precision_scale.precision=0;break;
      case 6:d.width_precision_scale.scale=19;break;
      case 7:d.width_precision_scale.precision=77;break;
      case 8:d.datatype_catalog_generation=999;break;
      case 9:d.codec_generation=2;break;
      case 10:d.statement_receipt_uuid={};break;
      case 11:d.type_uuid=Id(0xd712);break;
    }
    const auto refused=Bind(invalid);
    Check(!refused.bound && refused.native_relational.descriptors.empty(),
          "binder admitted invalid profile or leaked staged records");
    auto altered=b;
    auto& target=altered.native_relational.descriptors[0];
    target.decimal_codec_profile=d.decimal_codec_profile;
    target.width_precision_scale=d.width_precision_scale;
    target.datatype_catalog_generation=d.datatype_catalog_generation;
    target.codec_generation=d.codec_generation;
    target.statement_receipt_uuid=d.statement_receipt_uuid;
    target.type_uuid=d.type_uuid;
    Check(Lower(altered).messages.has_errors(),"lowerer admitted altered profile");
    Check(p::FreezeContextualReservationSkeletonV2(altered,e,{})!=frozen,
          "reservation ignored descriptor mutation");
  }
  for (unsigned bit=0;bit<128;++bit) {
    auto altered=b;
    altered.native_relational.descriptors[0].decimal_codec_profile->uuid.bytes[bit/8]^=1U<<(bit%8);
    Check(p::FreezeContextualReservationSkeletonV2(altered,e,{})!=frozen,
          "reservation ignored binary profile identity bit");
  }
  auto dropped=b;
  dropped.native_relational.descriptors[0].decimal_codec_profile.reset();
  Check(p::FreezeContextualReservationSkeletonV2(dropped,e,{})!=frozen,
        "reservation ignored profile removal");
  // Legacy descriptors remain profile-free; absence never invents cast authority.
  auto legacy=c;
  for(auto& d:legacy.descriptors)d.decimal_codec_profile.reset();
  const auto legacy_bound=Bind(legacy);
  Check(legacy_bound.bound,"legacy profile-free binding regressed");
  Check(p::VerifySblrEnvelope(Lower(legacy_bound)).admitted,"legacy lowering regressed");
}
}
int main() try {
  for(unsigned cohort:{5U,10U,11U})Test(cohort);
  std::cout<<"PASS parser DECIMAL profile checks="<<checks<<"; component only\n";
  return 0;
} catch(const std::exception& e) {
  std::cerr<<"FAIL checks="<<checks<<": "<<e.what()<<'\n';return 1;
}
