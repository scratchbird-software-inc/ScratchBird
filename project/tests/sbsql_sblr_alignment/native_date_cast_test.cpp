// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr/sblr_special_forms.hpp"
#include "executor/descriptor_value_runtime.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "datatype_date.hpp"
#include "registry/function_seed_registry.hpp"
#include "dispatch/function_dispatch.hpp"
#include "common/function_result_helpers.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <bit>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <cstdlib>
#include <new>

namespace allocation_fault {
thread_local std::int64_t remaining = -1;
thread_local bool fired = false;
thread_local bool counting = false;
thread_local unsigned count = 0;
}
void* operator new(std::size_t size) {
  if (allocation_fault::counting) ++allocation_fault::count;
  if (allocation_fault::remaining >= 0 && allocation_fault::remaining-- == 0) {
    allocation_fault::fired = true;
    throw std::bad_alloc();
  }
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace s = scratchbird::engine::sblr;
namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace f = scratchbird::engine::functions;
namespace {
unsigned checks = 0;
bool callable = false;
void Check(bool ok, const char* detail) {
  ++checks;
  if (!ok) throw std::runtime_error(detail);
}
api::EngineDatatypeCohort Cohort(unsigned generation) {
  if (generation == 11) return {dt::kDatatypeCohortV11, 11, 11};
  return {scratchbird::core::platform::Uuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,static_cast<std::uint8_t>(generation==10?0x10:generation)}},generation,generation};
}
std::shared_ptr<const api::EngineDescriptor> Binding(const char* name, unsigned generation,
                                                   bool nullable = true) {
  auto binding = ex::MakeExecutorDescriptor(name,
      nullable ? "nullability=nullable" : "nullability=non_null", Cohort(generation));
  binding.descriptor_uuid = scratchbird::tests::FixtureUuid(2120, name[0]);
  return std::make_shared<const api::EngineDescriptor>(std::move(binding));
}
s::SblrValue Native(std::int32_t day, unsigned generation) {
  s::SblrValue value;
  value.descriptor_id = "date";
  value.projection_descriptor = Binding("date", generation);
  value.payload_kind = s::SblrValuePayloadKind::binary;
  value.is_null = false;
  value.binary_value.resize(4);
  const auto bits = std::bit_cast<std::uint32_t>(day);
  for (unsigned i=0;i<4;++i) value.binary_value[i] = bits >> (8*i);
  return value;
}
s::SblrValue Text(std::string text, unsigned generation) {
  s::SblrValue value;
  value.descriptor_id = "character";
  value.projection_descriptor = Binding("character", generation);
  value.payload_kind = s::SblrValuePayloadKind::text;
  value.is_null = false;
  value.text_value = value.encoded_value = std::move(text);
  return value;
}
s::SblrResult Cast(const s::SblrValue& value,
                  const std::shared_ptr<const api::EngineDescriptor>& target,
                  bool explicit_cast = true) {
  if (callable && explicit_cast) {
    static const auto package = f::BuildStandardFunctionSeedPackage();
    const auto* entry = package.registry.Lookup("data.scalar.cast");
    Check(entry != nullptr, "CAST registration missing");
    f::FunctionCallRequest request;
    request.context.function_uuid = entry->function_uuid;
    request.context.security_allowed = request.context.policy_allowed = true;
    Check(package.registry.BindCallContext(request.context) != nullptr, "CAST binary binding failed");
    request.result_descriptor = target;
    request.arguments = {{"source", value}, {"target", f::MakeTextValue("text", target->canonical_type_name)}};
    return f::DispatchFunctionCall(package.registry, std::move(request)).result;
  }
  static const s::SblrExecutionContext context;
  return s::EvaluateSblrCastForm("native.date.cast", value,
      target->canonical_type_name, context, explicit_cast, false, target);
}
const s::SblrValue& Scalar(const s::SblrResult& r) {
  if (!r.ok()) for (const auto& d:r.diagnostics)
    std::cerr << d.diagnostic_id << ':' << d.detail << '\n';
  Check(r.ok() && r.scalar_values.size()==1 && r.rows.empty() &&
        !r.mutation_attempted && !r.mutation_committed, "native date cast failed");
  return r.scalar_values.front();
}
void Refused(const s::SblrResult& r, const char* code = nullptr) {
  Check(!r.ok() && r.scalar_values.empty() && r.rows.empty() &&
        !r.diagnostics.empty() && !r.mutation_attempted && !r.mutation_committed,
        "invalid native date cast published output");
  if (code) Check(r.diagnostics.front().diagnostic_id == code, "wrong refusal diagnostic");
}
unsigned AllocationSweep(const s::SblrValue& source,
                         const std::shared_ptr<const api::EngineDescriptor>& target) {
  allocation_fault::count=0;allocation_fault::counting=true;
  const auto baseline=Cast(source,target);
  allocation_fault::counting=false;
  const auto extent=allocation_fault::count;
  Scalar(baseline);
  Check(extent>0 && extent<4096,"allocation oracle is empty or exceeds the bounded sweep");
  for (unsigned position=0;position<=extent;++position) {
    allocation_fault::remaining=position;allocation_fault::fired=false;
    const auto result=Cast(source,target);
    allocation_fault::remaining=-1;
    Check(allocation_fault::fired==(position<extent),"allocation sweep did not cover the measured normal-new extent");
    if (position==extent) {
      const auto& output=Scalar(result);
      const auto& expected=baseline.scalar_values.front();
      Check(output.is_null==expected.is_null && output.binary_value==expected.binary_value &&
            output.text_value==expected.text_value && output.projection_descriptor==target,
            "allocation sweep changed the complete successful result");
    } else {
      Refused(result,"RESOURCE.BUDGET_EXCEEDED");
      Check(result.status==s::SblrStatusCode::resource_exhausted,"allocation failure lost resource classification");
    }
  }
  return extent;
}
}
void CoreNullability(unsigned generation) {
  const auto binding=Binding("date",generation), character=Binding("character",generation);
  const auto& cohort=binding->datatype_cohort;
  const auto* identity=dt::FindDatatypeTypeCodecIdentityV3(cohort.catalog_snapshot_uuid,
      cohort.catalog_generation,cohort.registry_generation,binding->datatype_descriptor_uuid,binding->datatype_descriptor_generation);
  const auto* peer=dt::FindDatatypeTypeCodecIdentityV3(cohort.catalog_snapshot_uuid,
      cohort.catalog_generation,cohort.registry_generation,character->datatype_descriptor_uuid,character->datatype_descriptor_generation);
  Check(identity && peer,"direct DATE identity fixture missing");
  const auto built=dt::BuildDateValidatedProfileHandleV3({cohort.catalog_snapshot_uuid,cohort.catalog_snapshot_uuid,
      cohort.catalog_generation,cohort.registry_generation},*identity);
  Check(built.ok(),"direct DATE profile fixture failed");
  const std::shared_ptr<const dt::DateValidatedProfileHandleV3> profile=
      std::make_shared<const dt::DateValidatedProfileHandleV3>(built.profile);
  scratchbird::engine::ExecutionTypeDescriptor target, text_descriptor;
  std::string detail;
  Check(ex::BuildBoundExecutionTypeDescriptor(*binding,dt::CanonicalTypeId::date,&target,&detail) &&
        ex::BuildBoundExecutionTypeDescriptor(*character,dt::CanonicalTypeId::character,&text_descriptor,&detail),"direct DATE execution binding failed");
  dt::DatatypeOperationValue scalar;
  scalar.type_id=dt::CanonicalTypeId::null_type;scalar.is_null=true;
  dt::DateCastRequestV3 request;
  request.one_based_policy_row=1;request.scalar_source=&scalar;
  request.date_target=&profile;request.date_target_descriptor=&target;
  target.nullable_allowed=false;request.target_null_allowed=true;
  Check(dt::CastDateValueV3(request).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED",
        "direct contextual NULL flag relaxed DATE descriptor");
  request.one_based_policy_row=24;request.context=dt::DatatypeCastContext::explicit_cast;
  request.scalar_source_identity=peer;scalar.type_id=dt::CanonicalTypeId::character;scalar.descriptor=text_descriptor;
  Check(dt::CastDateValueV3(request).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED",
        "direct parse NULL flag relaxed DATE descriptor");
  target.nullable_allowed=true;scalar.descriptor.nullable_allowed=false;
  Check(dt::CastDateValueV3(request).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED",
        "direct DATE cast accepted nonnullable character NULL");
  dt::DateTextOperandV3 operand;
  operand.identity=peer;operand.descriptor=&scalar.descriptor;operand.state=dt::DateValueStateV3::sql_null;
  Check(dt::ParseCanonicalDateOperandV3(profile,operand).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED",
        "direct DATE parse accepted nonnullable character NULL");
  operand.state=dt::DateValueStateV3::value;operand.bytes="1970-01-01";operand.extent=10;
  const auto parsed=dt::ParseCanonicalDateOperandV3(profile,operand);
  Check(parsed.ok() && parsed.value.day==0,"direct DATE parse rejected nonnullable PRESENT");
}
int main() try {
  for(unsigned generation : {7u,8u,9u,10u,11u}) CoreNullability(generation);
  for (const bool route : {true,false}) {
  callable = route;
  for (unsigned generation : {7u,8u,9u,10u,11u}) {
    const auto date=Binding("date",generation), character=Binding("character",generation);
    struct Case { std::int32_t day; const char* text; };
    for (const auto& c : {Case{0,"1970-01-01"}, Case{-1,"1969-12-31"},
        Case{INT32_MIN,"-5877641-06-23"}, Case{INT32_MAX,"+5881580-07-11"}}) {
      const auto source=Native(c.day,generation);
      const auto render=Cast(source,character);
      const auto& rendered=Scalar(render);
      Check(rendered.text_value==c.text && rendered.binary_value.empty() &&
            rendered.projection_descriptor==character, "native render or binding changed");
      const auto parsed=Cast(Text(c.text,generation),date);
      const auto& native=Scalar(parsed);
      Check(native.binary_value==source.binary_value && native.text_value.empty() &&
            native.encoded_value.empty() && native.projection_descriptor==date &&
            native.payload_kind==s::SblrValuePayloadKind::binary, "parse did not retain exact native bytes");
      const auto identity=Cast(source,date,false);
      Check(Scalar(identity).binary_value==source.binary_value, "identity cast changed native bytes");
      Refused(Cast(source,character,false),"DATATYPE.CAST_FORBIDDEN");
      Refused(Cast(Text(c.text,generation),date,false),"DATATYPE.CAST_FORBIDDEN");
    }
    auto source=Native(0,generation);
    source.binary_value.push_back(0); Refused(Cast(source,character));
    source=Native(0,generation); source.binary_value.pop_back(); Refused(Cast(source,character));
    source=Native(0,generation); source.text_value="1970-01-01"; Refused(Cast(source,character));
    source=Native(0,generation); source.projection_descriptor.reset(); Refused(Cast(source,character));
    source=Native(0,generation); source.descriptor_id="date_tz"; Refused(Cast(source,date));
    Refused(Cast(Text("1970-01-01Z",generation),date),"CTI.TEMPORAL.INVALID_LITERAL");
    Refused(Cast(Text("2026-02-30",generation),date));
    source=Native(0,generation);
    Refused(Cast(source,Binding("character",generation==10?11:10)),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
    auto altered=std::make_shared<api::EngineDescriptor>(*source.projection_descriptor);
    ++altered->datatype_descriptor_generation; source.projection_descriptor=altered;
    Refused(Cast(source,character));
    source=Native(0,generation); source.is_null=true; source.binary_value.clear();
    source.payload_kind=s::SblrValuePayloadKind::none;
    Check(Scalar(Cast(source,character)).is_null, "bound native NULL not preserved");
    Refused(Cast(source,Binding("character",generation,false)),"DATATYPE.NULL_NOT_ADMITTED");
    Refused(Cast(source,Binding("date",generation==10?11:10)));
    source.binary_value.push_back(0); Refused(Cast(source,character));
    s::SblrValue contextual;
    contextual.descriptor_id="null_type";
    Check(Scalar(Cast(contextual,date)).is_null, "contextual NULL missing native binding");
    Refused(Cast(contextual,Binding("date",generation,false)),"DATATYPE.NULL_NOT_ADMITTED");
    contextual.text_value="dirty"; Refused(Cast(contextual,date));
    auto unknown_modifier=std::make_shared<api::EngineDescriptor>(*date);
    unknown_modifier->encoded_descriptor+=";unsupported=true";
    Refused(Cast(Text("1970-01-01",generation),unknown_modifier));
    auto short_text=std::make_shared<api::EngineDescriptor>(*character);
    short_text->encoded_descriptor+=";length=5";
    Refused(Cast(Native(0,generation),short_text),"CTB.TEXT.LENGTH_EXCEEDED");
    auto short_source=Text("1970-01-01",generation);
    auto short_source_binding=std::make_shared<api::EngineDescriptor>(*short_source.projection_descriptor);
    short_source_binding->encoded_descriptor+=";length=5";
    short_source.projection_descriptor=short_source_binding;
    Refused(Cast(short_source,date),"CTI.TEMPORAL.RANGE_EXCEEDED");
    for (unsigned mutation=0;mutation<8;++mutation) for (bool null_value : {false,true}) {
      auto bad=std::make_shared<api::EngineDescriptor>(*date);
      switch(mutation) {
        case 0:bad->descriptor_uuid={};break;
        case 1:bad->type_uuid.bytes[15]^=0x7f;break;
        case 2:bad->datatype_descriptor_uuid.bytes[15]^=0x7f;break;
        case 3:++bad->datatype_descriptor_generation;break;
        case 4:bad->datatype_cohort.catalog_snapshot_uuid.bytes[15]^=0x7f;break;
        case 5:++bad->datatype_cohort.catalog_generation;break;
        case 6:++bad->datatype_cohort.registry_generation;break;
        case 7:bad->canonical_type_name="time";break;
      }
      auto bad_source=Native(0,generation);
      bad_source.projection_descriptor=bad;
      auto parse_source=Text("1970-01-01",generation);
      if(null_value) {
        bad_source.is_null=true;bad_source.binary_value.clear();bad_source.payload_kind=s::SblrValuePayloadKind::none;
        parse_source.is_null=true;parse_source.text_value.clear();parse_source.encoded_value.clear();parse_source.payload_kind=s::SblrValuePayloadKind::none;
      }
      Refused(Cast(bad_source,character),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
      // The advertised target label is checked independently by dispatch;
      // a non-temporal label is a forbidden pair, not an exact binding.
      Refused(Cast(parse_source,bad),mutation==7?nullptr:"CTI.TEMPORAL.DESCRIPTOR_INVALID");
    }
    for (bool from_nullable : {false,true}) for (bool to_nullable : {false,true}) {
      auto identity_source=Native(0,generation);
      identity_source.projection_descriptor=Binding("date",generation,from_nullable);
      const auto identity_target=Binding("date",generation,to_nullable);
      Check(Scalar(Cast(identity_source,identity_target)).binary_value==identity_source.binary_value,
            "PRESENT identity nullability combination failed");
      identity_source.is_null=true;identity_source.binary_value.clear();identity_source.payload_kind=s::SblrValuePayloadKind::none;
      if(from_nullable && to_nullable) Check(Scalar(Cast(identity_source,identity_target)).is_null,"NULL identity changed state");
      else Refused(Cast(identity_source,identity_target),"DATATYPE.NULL_NOT_ADMITTED");
    }
    auto conflicting=Text("1970-01-01",generation);
    conflicting.encoded_value="2000-01-01";Refused(Cast(conflicting,date));
    for (unsigned peer_generation : {generation})
    for (bool from_nullable : {false,true}) for (bool to_nullable : {false,true}) {
      auto input=Native(0,generation);
      input.projection_descriptor=Binding("date",generation,from_nullable);
      const auto text_target=Binding("character",peer_generation,to_nullable);
      Check(Scalar(Cast(input,text_target)).text_value=="1970-01-01", "PRESENT nonnullable render failed");
      auto text_input=Text("1970-01-01",peer_generation);
      text_input.projection_descriptor=Binding("character",peer_generation,from_nullable);
      Check(Scalar(Cast(text_input,Binding("date",generation,to_nullable))).binary_value==input.binary_value,
            "PRESENT nonnullable parse failed");
      input.is_null=true; input.binary_value.clear();input.payload_kind=s::SblrValuePayloadKind::none;
      if (from_nullable && to_nullable) Check(Scalar(Cast(input,text_target)).is_null,"NULL matrix render failed");
      else Refused(Cast(input,text_target),"DATATYPE.NULL_NOT_ADMITTED");
      text_input.is_null=true;text_input.text_value.clear();text_input.encoded_value.clear();text_input.payload_kind=s::SblrValuePayloadKind::none;
      const auto native_target=Binding("date",generation,to_nullable);
      if (from_nullable && to_nullable) Check(Scalar(Cast(text_input,native_target)).is_null,"NULL matrix parse failed");
      else Refused(Cast(text_input,native_target),"DATATYPE.NULL_NOT_ADMITTED");
    }
  }
  }
  callable=false;
  const auto fault_source=Native(0,11);
  unsigned failures=AllocationSweep(fault_source,Binding("character",11));
  failures+=AllocationSweep(fault_source,Binding("date",11));
  failures+=AllocationSweep(Text("1970-01-01",11),Binding("date",11));
  s::SblrValue contextual;contextual.descriptor_id="null_type";
  failures+=AllocationSweep(contextual,Binding("date",11));
  std::cout << "native date cast checks=" << checks << " allocation_failures=" << failures << '\n';
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
