// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_seed_registry.hpp"
#include "dispatch/function_dispatch.hpp"
#include "common/function_result_helpers.hpp"
#include "sblr/sblr_special_forms.hpp"
#include "sblr/sblr_projection_value_runtime.hpp"
#include "executor/descriptor_value_runtime.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cstdlib>

namespace {
thread_local bool track_cast_allocations=false;
thread_local std::size_t cast_allocations=0, fail_cast_allocation=0;
}
void* operator new(std::size_t bytes) {
  if(track_cast_allocations && ++cast_allocations==fail_cast_allocation) {
    track_cast_allocations=false;
    throw std::bad_alloc();
  }
  if(void* allocation=std::malloc(bytes?bytes:1)) return allocation;
  throw std::bad_alloc();
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation,std::size_t) noexcept { std::free(allocation); }

namespace s = scratchbird::engine::sblr;
namespace f = scratchbird::engine::functions;
namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* detail) {
  ++checks;
  if (!condition) throw std::runtime_error(detail);
}
std::shared_ptr<const api::EngineDescriptor> Binding(const char* name, bool nullable = true) {
  auto descriptor = ex::MakeExecutorDescriptor(name);
  descriptor.descriptor_kind = "scalar";
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2110, name[0] == 'i' ? 1 : 2);
  descriptor.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
  return std::make_shared<const api::EngineDescriptor>(std::move(descriptor));
}
s::SblrValue Integer(std::int64_t number) {
  auto value = f::MakeInt64Value("int64", number);
  value.projection_descriptor = Binding("int64");
  return value;
}
s::SblrValue Real(std::uint64_t bits) {
  s::SblrValue value;
  value.descriptor_id = "real64";
  value.payload_kind = s::SblrValuePayloadKind::real64;
  value.real64_value = std::bit_cast<double>(bits);
  value.has_real64_value = true;
  value.is_null = false;
  value.projection_descriptor = Binding("real64");
  return value;
}
s::SblrResult Cast(const f::FunctionRegistry& registry, const s::SblrValue& value,
                   const std::shared_ptr<const api::EngineDescriptor>& target, bool callable) {
  if (!callable)
    return s::EvaluateSblrCastForm("native.cast", value, target->canonical_type_name,
                                 {}, true, false, target);
  const auto* entry = registry.Lookup("data.scalar.cast");
  Check(entry != nullptr, "registered CAST is missing");
  f::FunctionCallRequest request;
  request.context.function_uuid = entry->function_uuid;
  request.context.security_allowed = request.context.policy_allowed = true;
  Check(registry.BindCallContext(request.context) != nullptr, "binary callable binding failed");
  request.result_descriptor = target;
  request.arguments = {{"source", value},
      {"target", f::MakeTextValue("text", target->canonical_type_name)}};
  return f::DispatchFunctionCall(registry, std::move(request)).result;
}
const s::SblrValue& Scalar(const s::SblrResult& result) {
  if (!result.ok()) for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.diagnostic_id << ':' << diagnostic.detail << '\n';
    for (const auto& field : diagnostic.fields)
      if (const auto* text = std::get_if<std::string>(&field.value))
        std::cerr << field.key << '=' << *text << '\n';
  }
  Check(result.ok() && result.scalar_values.size() == 1 && result.rows.empty() &&
        !result.mutation_attempted && !result.mutation_committed, "cast did not return one scalar");
  return result.scalar_values.front();
}
void Published(const s::SblrValue& value, std::uint64_t bits,
               const std::shared_ptr<const api::EngineDescriptor>& target) {
  Check(value.projection_descriptor == target && value.encoded_value.empty() &&
        value.text_value.empty() && value.binary_value.empty() &&
        (target->canonical_type_name == "int64" ? s::SblrInt64PayloadValid(value)
                                               : s::SblrReal64PayloadValid(value)),
        "cast lost binding or published a text shadow");
  const auto output = s::EngineTypedValueFromSblrValue(value);
  Check(output.descriptor == *target && output.encoded_value.empty() && output.binary_value.size() == 8,
        "projection adapter lost exact descriptor/native width");
  for (unsigned byte = 0; byte < 8; ++byte)
    Check(output.binary_value[byte] == static_cast<std::uint8_t>(bits >> (8 * byte)),
          "projection adapter changed a native result bit");
}
void Refused(const s::SblrResult& result) {
  Check(!result.ok() && !result.diagnostics.empty() && result.scalar_values.empty() &&
        result.rows.empty() && !result.mutation_attempted && !result.mutation_committed,
        "invalid cast produced a value or lacked diagnostics");
}
void ConversionRefused(const s::SblrResult& result, const char* code,
                       bool overflow, bool invalid, bool inexact) {
  Refused(result);
  Check(result.diagnostics.front().diagnostic_id == code &&
        result.numeric_facts.overflow == overflow &&
        result.numeric_facts.invalid == invalid &&
        result.numeric_facts.inexact == inexact,
        "conversion failed for the wrong reason or lost its numeric facts");
}

void NativeSignedCharacterCasts() {
  namespace dt = scratchbird::core::datatypes;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "signed cast catalog unavailable");
  const auto descriptor = [&](dt::CanonicalTypeId type) {
    const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type);
    Check(row.ok() && row.manifest.descriptor_rows.size()==1, "signed cast catalog row missing");
    dt::CatalogExecutionTypeMetadata metadata;
    metadata.descriptor_uuid=row.manifest.descriptor_rows.front().descriptor_uuid;
    metadata.descriptor_epoch=row.manifest.descriptor_rows.front().descriptor_epoch;
    const auto built=dt::LookupExecutionTypeDescriptorFromCatalog(type,metadata);
    Check(built.ok(), "signed cast descriptor unavailable");
    return built.descriptor;
  };
  struct Profile { dt::CanonicalTypeId type; const char* name; unsigned width;
    std::int64_t min,max; const char* underflow; const char* overflow; };
  const Profile profiles[] = {
    {dt::CanonicalTypeId::int16,"int16",2,-32768,32767,"-32769","32768"},
    {dt::CanonicalTypeId::int32,"int32",4,-2147483648LL,2147483647,"-2147483649","2147483648"},
    {dt::CanonicalTypeId::int64,"int64",8,std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max(),"-9223372036854775809","9223372036854775808"}
  };
  const auto character=descriptor(dt::CanonicalTypeId::character);
  const auto text_binding=ex::MakeExecutorDescriptor("character","nullability=nullable");
  for(const auto& profile:profiles) {
    const auto integer=descriptor(profile.type);
    const auto integer_binding=ex::MakeExecutorDescriptor(profile.name,"nullability=nullable");
    Check(dt::ClassifyDatatypeCast(profile.type,dt::CanonicalTypeId::character)==dt::DatatypeCastCategory::lossless_explicit &&
          dt::ClassifyDatatypeCast(dt::CanonicalTypeId::character,profile.type)==dt::DatatypeCastCategory::lossless_explicit,
          "signed character cast classification");
    std::vector<std::int64_t> samples{profile.min,profile.min+1,-1,0,1,profile.max-1,profile.max};
    // Exercise signed byte transitions near zero in addition to each width's
    // independently declared endpoints.
    for(std::int64_t number=-128;number<=127;++number) samples.push_back(number);
    for(const auto number:samples) {
      const auto text=std::to_string(number);
      std::string bytes(profile.width,'\0');
      for(unsigned byte=0;byte<profile.width;++byte)
        bytes[byte]=static_cast<char>(static_cast<std::uint64_t>(number)>>(8*byte));
      dt::DatatypeCastRequest request;
      request.value={dt::CanonicalTypeId::character,text,false,character};
      request.target_type_id=profile.type;request.target_descriptor=integer;
      request.context=dt::DatatypeCastContext::explicit_cast;
      const auto parsed=dt::CastDatatypeValue(request);
      Check(parsed.ok() && parsed.value.type_id==profile.type && !parsed.value.is_null &&
            parsed.value.encoded_value==bytes,"Core signed text cast must publish exact native bytes");
      request.value={profile.type,bytes,false,integer};
      request.target_type_id=dt::CanonicalTypeId::character;request.target_descriptor=character;
      const auto rendered=dt::CastDatatypeValue(request);
      Check(rendered.ok() && rendered.value.encoded_value==text,"Core signed decimal render roundtrip");
      ex::DescriptorRuntimeDiagnostic diagnostic;
      const auto encoded=ex::CastDescriptorValue(ex::MakeExecutorValue(text_binding,text,false),integer_binding,&diagnostic);
      Check(diagnostic.ok && encoded.descriptor==integer_binding && encoded.encoded_value.empty() &&
            encoded.binary_value==std::vector<std::uint8_t>(bytes.begin(),bytes.end()),"executor signed native cast");
      const auto decoded=ex::CastDescriptorValue(encoded,text_binding,&diagnostic);
      Check(diagnostic.ok && decoded.descriptor==text_binding && decoded.binary_value.empty() &&
            decoded.encoded_value==text,"executor signed text roundtrip");
    }
    for(const auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment,
                           dt::DatatypeCastContext::explicit_cast})
      for(bool explicit_flag:{false,true}) for(bool null:{false,true}) {
        dt::DatatypeCastRequest request;
        request.value={dt::CanonicalTypeId::character,null?"":"+00042",null,character};
        request.target_type_id=profile.type;request.target_descriptor=integer;
        request.context=context;request.explicit_cast=explicit_flag;
        const auto result=dt::CastDatatypeValue(request);
        const bool admitted=explicit_flag || context==dt::DatatypeCastContext::explicit_cast;
        Check(result.ok()==admitted,"signed cast explicit/assignment/implicit admission");
        if(admitted) Check(result.value.is_null==null && result.value.encoded_value.size()==(null?0:profile.width),
                           "signed cast NULL/native width");
        else Check(result.value.encoded_value.empty() && result.diagnostic.diagnostic_code=="DATATYPE.CAST_FORBIDDEN",
                   "signed implicit refusal must not publish value");
      }
    for(const std::string text:std::vector<std::string>{std::string(profile.underflow),std::string(profile.overflow),
                               "", " 1", "1 ","1.0","1e2","+-1","--1",std::string("1\0",2)}) {
      dt::DatatypeCastRequest request;
      request.value={dt::CanonicalTypeId::character,text,false,character};
      request.target_type_id=profile.type;request.target_descriptor=integer;
      request.context=dt::DatatypeCastContext::explicit_cast;
      const auto result=dt::CastDatatypeValue(request);
      const bool overflow=text==profile.underflow || text==profile.overflow;
      Check(!result.ok() && result.value.encoded_value.empty() && result.value.type_id==dt::CanonicalTypeId::unknown &&
            result.diagnostic.diagnostic_code==(overflow?"NUMERIC.CAST.OUT_OF_RANGE":"DATATYPE.CAST_FORBIDDEN"),
            "signed cast grammar/range must be checked without partial result");
    }
    for(bool null:{false,true}) for(bool source:{false,true}) for(unsigned mutation=0;mutation<3;++mutation) {
      dt::DatatypeCastRequest request;
      request.value={dt::CanonicalTypeId::character,null?"":"42",null,character};
      request.target_type_id=profile.type;request.target_descriptor=integer;
      request.context=dt::DatatypeCastContext::explicit_cast;
      auto& changed=source?request.value.descriptor:request.target_descriptor;
      if(mutation==0) ++changed.descriptor_epoch;
      if(mutation==1) changed.descriptor_authoritative=false;
      if(mutation==2) changed.security_policy_uuid.bytes[0]^=1;
      const auto result=dt::CastDatatypeValue(request);
      Check(!result.ok() && result.value.encoded_value.empty(),"signed cast cannot discard endpoint authority");
    }
    dt::DatatypeCastRequest null_request;
    null_request.value={dt::CanonicalTypeId::character,{},true,character};
    null_request.target_type_id=profile.type;null_request.target_descriptor=integer;
    null_request.target_descriptor.nullable_allowed=false;
    null_request.context=dt::DatatypeCastContext::explicit_cast;
    Check(dt::CastDatatypeValue(null_request).diagnostic.diagnostic_code=="DATATYPE.NULL_NOT_ADMITTED",
          "signed cast non-null target must refuse NULL");
    null_request.target_descriptor=integer;null_request.value.encoded_value="dirty";
    Check(!dt::CastDatatypeValue(null_request).ok(),"signed cast refuses payload-bearing NULL");
  }
  // INT8 already has a native Core codec/cast route. Its executor adapter must
  // delegate to it rather than returning decimal text as an integer payload.
  const auto int8_binding=ex::MakeExecutorDescriptor("int8","nullability=nullable");
  for(int number=-128;number<=127;++number) {
    ex::DescriptorRuntimeDiagnostic diagnostic;
    const auto result=ex::CastDescriptorValue(
        ex::MakeExecutorValue(text_binding,std::to_string(number),false),int8_binding,&diagnostic);
    Check(diagnostic.ok && result.encoded_value.empty() && result.descriptor==int8_binding &&
          result.binary_value==std::vector<std::uint8_t>{static_cast<std::uint8_t>(number)},
          "INT8 executor cast preserves native octet");
    const auto text=ex::CastDescriptorValue(result,text_binding,&diagnostic);
    Check(diagnostic.ok && text.binary_value.empty() && text.encoded_value==std::to_string(number),
          "INT8 executor character roundtrip");
  }
  for(const auto cohort:{api::EngineDatatypeCohort{dt::kDatatypeCohortV5,5,5},
                         api::EngineDatatypeCohort{dt::kDatatypeCohortV10,10,10},
                         api::EngineDatatypeCohort{dt::kDatatypeCohortV11,11,11}})
    for(const auto name:{"int8","int16","int32","int64"}) {
      const auto text=ex::MakeExecutorDescriptor("character","nullability=nullable",cohort);
      const auto integer=ex::MakeExecutorDescriptor(name,"nullability=nullable",cohort);
      ex::DescriptorRuntimeDiagnostic diagnostic;
      for(bool null:{false,true}) {
        const auto value=ex::CastDescriptorValue(ex::MakeExecutorValue(text,null?"":"42",null),integer,&diagnostic);
        Check(diagnostic.ok && value.descriptor==integer && value.isSqlNull()==null && value.encoded_value.empty(),
              "executor signed cast retains exact source cohort");
        const auto rendered=ex::CastDescriptorValue(value,text,&diagnostic);
        Check(diagnostic.ok && rendered.descriptor==text && rendered.isSqlNull()==null &&
              rendered.binary_value.empty() && rendered.encoded_value==(null?"":"42"),
              "executor signed reverse cast retains exact source cohort");
        for(bool reverse:{false,true}) for(bool destination:{false,true}) for(unsigned change=0;change<4;++change) {
          auto input=reverse?value:ex::MakeExecutorValue(text,null?"":"42",null);
          auto target=reverse?text:integer;
          auto& altered=destination?target:input.descriptor;
          if(change==0) altered.datatype_cohort={cohort.catalog_generation==5?dt::kDatatypeCohortV10:dt::kDatatypeCohortV5,
                                               cohort.catalog_generation==5?10u:5u,cohort.catalog_generation==5?10u:5u};
          if(change==1) ++altered.datatype_descriptor_generation;
          if(change==2) altered.type_uuid.bytes[0]^=1;
          if(change==3) altered.encoded_descriptor+=";unknown=1";
          const auto failed=ex::CastDescriptorValue(input,target,&diagnostic);
          Check(!diagnostic.ok && failed.binary_value.empty() && failed.encoded_value.empty(),
                "executor signed cast must reject foreign or modified endpoint");
        }
      }
      const auto valid=ex::CastDescriptorValue(ex::MakeExecutorValue(text,"42",false),integer,&diagnostic);
      Check(diagnostic.ok,"integer carrier refusal control");
      for(unsigned mutation=0;mutation<5;++mutation) {
        auto malformed=valid;
        if(mutation==0) malformed.binary_value.pop_back();
        if(mutation==1) malformed.binary_value.push_back(0);
        if(mutation==2) malformed.encoded_value="42";
        if(mutation==3) {malformed.binary_value.clear();malformed.encoded_value="42";}
        if(mutation==4) {malformed.setState(api::EngineValueState::sql_null);malformed.binary_value.clear();
          malformed.descriptor.encoded_descriptor="nullability=non_null";}
        const auto failed=ex::CastDescriptorValue(malformed,text,&diagnostic);
        Check(!diagnostic.ok && failed.binary_value.empty() && failed.encoded_value.empty(),
              "integer text/dual/wrong-width or forbidden NULL carrier admitted");
      }
      if(std::string_view(name)=="int8") for(const auto text_number:{"-129","128"}) {
        const auto failed=ex::CastDescriptorValue(ex::MakeExecutorValue(text,text_number,false),integer,&diagnostic);
        Check(!diagnostic.ok && failed.binary_value.empty() && failed.encoded_value.empty(),
              "INT8 executor overflow admitted");
      }
    }
}
void SblrSignedCharacterCasts(const f::FunctionRegistry& registry) {
  const auto bridge = [&](const s::SblrValue& value) {
    Check(s::ProjectionSblrValueResolved(value), "bound signed CHARACTER result must be projection-resolved");
    const auto output = s::EngineTypedValueFromSblrValue(value);
    Check(output.descriptor==*value.projection_descriptor && output.isSqlNull()==value.is_null,
          "projection bridge must preserve exact signed CHARACTER descriptor and NULL");
    api::EngineProjectionFunctionArgument argument;
    argument.type_name=value.descriptor_id; argument.descriptor=output.descriptor;
    argument.state=output.state; argument.is_null=output.is_null;
    argument.binary_value=output.binary_value; argument.encoded_value=output.encoded_value;
    Check(s::ProjectionArgumentEncodingValid(argument), "signed CHARACTER projection argument validation");
    const auto decoded=s::SblrValueFromProjectionArgument(argument);
    Check(s::ProjectionSblrValueResolved(decoded) && decoded.projection_descriptor &&
          *decoded.projection_descriptor==output.descriptor && decoded.is_null==value.is_null &&
          decoded.int64_value==value.int64_value && decoded.text_value==value.text_value &&
          decoded.encoded_value==value.encoded_value, "signed CHARACTER projection roundtrip");
    const unsigned width=s::SblrSignedIntegerWidth(value.descriptor_id);
    if(width) {
      Check(output.encoded_value.empty() && output.binary_value.size()==(value.is_null?0u:width),
            "signed projection must use native width and no text");
      if(!value.is_null) for(unsigned i=0;i<width;++i)
        Check(output.binary_value[i]==static_cast<std::uint8_t>(static_cast<std::uint64_t>(value.int64_value)>>(8*i)),
              "signed projection independent byte oracle");
    } else Check(output.binary_value.empty() && output.encoded_value==value.text_value,
                 "CHARACTER projection exact decimal text");
    auto dirty=argument; dirty.is_null=!dirty.is_null;
    Check(!s::ProjectionArgumentEncodingValid(dirty), "projection NULL flag/state disagreement refused");
    dirty=argument; dirty.descriptor.canonical_type_name="binary";
    Check(!s::ProjectionArgumentEncodingValid(dirty), "projection conflicting endpoint name refused");
    if(width || value.is_null) {
      dirty=argument;dirty.encoded_value="42";
      Check(!s::ProjectionArgumentEncodingValid(dirty), "projection textual integer or dirty NULL refused");
    }
    dirty=argument;dirty.binary_value.push_back(0);
    Check(!s::ProjectionArgumentEncodingValid(dirty), "projection extra native byte refused");
    for(const auto wrong_type : {"decimal","uint16"}) {
      auto binding=ex::MakeExecutorDescriptor(wrong_type,"nullability=nullable");
      binding.canonical_type_name=value.descriptor_id;
      dirty=argument;dirty.descriptor=binding;
      Check(!s::ProjectionArgumentEncodingValid(dirty) &&
            !s::ProjectionSblrValueResolved(s::SblrValueFromProjectionArgument(dirty)),
            "renamed DECIMAL/UINT16 argument binding cannot bypass strict profile admission");
      auto bad=value;bad.projection_descriptor=std::make_shared<const api::EngineDescriptor>(binding);
      Check(!s::ProjectionSblrValueResolved(bad), "renamed DECIMAL/UINT16 result binding refused");
      bool refused=false;
      try { (void)s::EngineTypedValueFromSblrValue(bad); }
      catch(const std::invalid_argument&) { refused=true; }
      Check(refused, "renamed DECIMAL/UINT16 binding cannot publish via signed CHARACTER bridge");
    }
  };
  const auto text = std::make_shared<const api::EngineDescriptor>(
      ex::MakeExecutorDescriptor("character", "nullability=nullable"));
  const auto character = [&](std::string bytes) {
    auto value = f::MakeTextValue("character", std::move(bytes));
    value.projection_descriptor = text;
    return value;
  };
  struct Profile { const char* name; std::int64_t minimum, maximum; const char* below; const char* above; };
  for (const auto& p : {
      Profile{"int8",-128,127,"-129","128"},
      Profile{"int16",-32768,32767,"-32769","32768"},
      Profile{"int32",-2147483648LL,2147483647,"-2147483649","2147483648"},
      Profile{"int64",std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max(),
              "-9223372036854775809","9223372036854775808"}}) {
    const auto integer = std::make_shared<const api::EngineDescriptor>(
        ex::MakeExecutorDescriptor(p.name, "nullability=nullable"));
    for (bool callable : {false,true}) {
      std::vector<std::int64_t> samples{p.minimum,p.minimum+1,-1,0,1,p.maximum-1,p.maximum};
      for (std::int64_t n=-128;n<=127;++n) samples.push_back(n);
      for (const auto number : samples) {
        const auto parsed = Cast(registry, character(std::to_string(number)), integer, callable);
        const auto& native = Scalar(parsed);
        Check(native.projection_descriptor==integer && native.descriptor_id==p.name &&
              native.payload_kind==s::SblrValuePayloadKind::signed_integer && native.has_int64_value &&
              native.int64_value==number && !native.is_null && !native.has_uint64_value && !native.has_real64_value &&
              native.encoded_value.empty() && native.text_value.empty() && native.binary_value.empty(),
              "SBLR signed CHARACTER parse must retain binding and machine integer without text");
        bridge(native);
        const auto rendered = Cast(registry, native, text, callable);
        const auto& out = Scalar(rendered);
        Check(out.projection_descriptor==text && !out.is_null && !out.has_int64_value &&
              out.payload_kind==s::SblrValuePayloadKind::text && out.binary_value.empty() &&
              out.encoded_value==std::to_string(number) && out.text_value==out.encoded_value,
              "SBLR signed CHARACTER render roundtrip");
        bridge(out);
      }
      for (const auto bad : {p.below,p.above,"","1.2","1e2"," 1","1 ","+","--1"})
        Refused(Cast(registry,character(bad),integer,callable));
      auto null = s::SblrValue{};
      null.descriptor_id="character"; null.is_null=true; null.projection_descriptor=text;
      const auto null_cast=Cast(registry,null,integer,callable);
      Check(Scalar(null_cast).is_null && Scalar(null_cast).projection_descriptor==integer &&
            s::SblrNullPayloadEmpty(Scalar(null_cast)), "SBLR signed NULL binding and empty carrier");
      bridge(Scalar(null_cast));
      const auto null_back=Cast(registry,Scalar(null_cast),text,callable);
      Check(Scalar(null_back).is_null && Scalar(null_back).projection_descriptor==text,
            "SBLR signed NULL render preserves binding");
      bridge(Scalar(null_back));
      auto nonnull=*integer; nonnull.encoded_descriptor="nullability=non_null";
      Refused(Cast(registry,null,std::make_shared<const api::EngineDescriptor>(nonnull),callable));
      null.text_value="0";
      Refused(Cast(registry,null,integer,callable));
      auto input=character("42"); input.projection_descriptor.reset();
      Refused(Cast(registry,input,integer,callable));
      input=character("42"); input.binary_value={42};
      Refused(Cast(registry,input,integer,callable));
      input=character("42"); input.text_value="43";
      Refused(Cast(registry,input,integer,callable));
      for (unsigned defect=0;defect<6;++defect) {
        auto bad=*integer;
        if(defect==0) bad.type_uuid={};
        if(defect==1) bad.datatype_descriptor_generation++;
        if(defect==2) bad.datatype_cohort.catalog_snapshot_uuid.bytes[15]^=1;
        if(defect==3) bad.encoded_descriptor="nullable=true;rounding=truncate";
        if(defect==4) bad.charset_uuid=scratchbird::tests::FixtureUuid(2120,3);
        if(defect==5) bad.datatype_descriptor_uuid={};
        Refused(Cast(registry,character("42"),std::make_shared<const api::EngineDescriptor>(bad),callable));
      }
      const auto native_result=Cast(registry,character("42"),integer,callable);
      auto native=Scalar(native_result); native.encoded_value="42";
      Refused(Cast(registry,native,text,callable));
      native=Scalar(native_result); native.has_uint64_value=true;
      Refused(Cast(registry,native,text,callable));
      if(std::string_view(p.name)!="int64") {
        native=Scalar(native_result); native.int64_value=p.maximum+1;
        Refused(Cast(registry,native,text,callable));
        native.int64_value=p.minimum-1;
        Refused(Cast(registry,native,text,callable));
      }
    }
    Refused(s::EvaluateSblrCastForm("native.cast",character("42"),p.name,{},false,false,integer));
    Refused(s::EvaluateSblrCastForm("native.cast",character("42"),p.name,{},true,true,integer));
    for(bool null:{false,true}) for(bool reverse:{false,true}) {
      auto input=character("42");
      auto target=integer;
      if(reverse) { input=Scalar(Cast(registry,input,integer,false));target=text; }
      if(null) {
        const auto descriptor=input.projection_descriptor;
        const auto name=input.descriptor_id;
        input=s::SblrValue{};input.descriptor_id=name;input.projection_descriptor=descriptor;input.is_null=true;
      }
      const s::SblrExecutionContext context;
      cast_allocations=0;fail_cast_allocation=0;track_cast_allocations=true;
      const auto measured=s::EvaluateSblrCastForm("native.cast",input,target->canonical_type_name,context,true,false,target);
      track_cast_allocations=false;
      const auto allocations=cast_allocations;
      Check(measured.ok() && allocations>0,"direct signed cast allocation measurement");
      for(std::size_t fault=1;fault<=allocations;++fault) {
        cast_allocations=0;fail_cast_allocation=fault;track_cast_allocations=true;
        const auto failed=s::EvaluateSblrCastForm("native.cast",input,target->canonical_type_name,context,true,false,target);
        const bool injected=!track_cast_allocations;
        track_cast_allocations=false;fail_cast_allocation=0;
        Refused(failed);
        Check(injected && (failed.diagnostics.front().diagnostic_id=="DATATYPE.RESOURCE_EXHAUSTED" ||
                          failed.diagnostics.front().diagnostic_id=="RESOURCE.BUDGET_EXCEEDED"),
              "signed cast injected allocation failure must retain resource diagnostic without value");
        Check(Cast(registry,input,target,false).ok(),"signed cast retry after allocation failure");
      }
    }
  }
}
}  // namespace

int main() try {
  NativeSignedCharacterCasts();
  const auto package = f::BuildStandardFunctionSeedPackage();
  SblrSignedCharacterCasts(package.registry);
  const auto integer = Binding("int64"), real = Binding("real64");
  for (const bool callable : {false, true}) {
    for (const auto number : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                              std::int64_t{0}, std::int64_t{42},
                              std::numeric_limits<std::int64_t>::max()}) {
      const auto result = Cast(package.registry, Integer(number), integer, callable);
      Published(Scalar(result), std::bit_cast<std::uint64_t>(number), integer);
    }
    // Independent bit-pattern oracles, including subnormal, signed zero and
    // exact finite endpoints; no decimal-format round trip in the test oracle.
    for (const auto bits : {0ULL, 0x8000000000000000ULL, 1ULL, 0x8000000000000001ULL,
                            0x3ff0000000000000ULL, 0x7fefffffffffffffULL,
                            0xffefffffffffffffULL}) {
      const auto result = Cast(package.registry, Real(bits), real, callable);
      Published(Scalar(result), bits, real);
    }
    auto result = Cast(package.registry, Integer(42), real, callable);
    Published(Scalar(result), 0x4045000000000000ULL, real);
    Check(!result.numeric_facts.inexact && !result.numeric_facts.invalid,
          "exact conversion manufactured numeric facts");
    auto nonnullable = Integer(42);
    nonnullable.projection_descriptor = Binding("int64", false);
    result = Cast(package.registry, nonnullable, real, callable);
    Published(Scalar(result), 0x4045000000000000ULL, real);
    result = Cast(package.registry, Real(0xc045000000000000ULL), integer, callable);
    Published(Scalar(result), std::bit_cast<std::uint64_t>(std::int64_t{-42}), integer);
    result = Cast(package.registry, Real(0xc3e0000000000000ULL), integer, callable);
    Published(Scalar(result), 0x8000000000000000ULL, integer);
    const auto rounding = std::fegetround();
    std::fesetround(FE_UPWARD);
    result = Cast(package.registry, Integer(9007199254740993LL), real, callable);
    std::fesetround(rounding);
    Published(Scalar(result), 0x4340000000000000ULL, real);
    Check(result.numeric_facts.inexact && !result.numeric_facts.invalid &&
          !result.numeric_facts.overflow, "successful rounding lost its inexact fact");
    ConversionRefused(Cast(package.registry, Real(0x3ff8000000000000ULL), integer, callable),
                      "NUMERIC.REAL64.INVALID", false, true, true);
    for (const auto bits : {0x43e0000000000000ULL, 0xc3e0000000000001ULL})
      ConversionRefused(Cast(package.registry, Real(bits), integer, callable),
                        "NUMERIC.REAL64.OVERFLOW", true, false, false);
    for (const auto bits : {0x7ff0000000000000ULL, 0x7ff8000000000001ULL})
      ConversionRefused(Cast(package.registry, Real(bits), integer, callable),
                        "NUMERIC.REAL64.INVALID", false, true, false);
    for (const char* source : {"int64", "real64"}) {
      auto null = f::MakeNullValue(source);
      null.projection_descriptor = Binding(source);
      for (const auto& target : {integer, real}) {
        result = Cast(package.registry, null, target, callable);
        Check(s::SblrNullPayloadEmpty(Scalar(result)) &&
              Scalar(result).projection_descriptor == target, "typed NULL lost binding/state");
        Refused(Cast(package.registry, null, Binding(target->canonical_type_name.c_str(), false), callable));
      }
    }
    for (unsigned variant = 0; variant < 13; ++variant) {
      auto bad = Real(0x3ff0000000000000ULL);
      auto binding = *real;
      switch (variant) {
        case 0: bad.text_value = "1"; break;
        case 1: bad.encoded_value = "1"; break;
        case 2: bad.binary_value = {0}; break;
        case 3: bad.has_int64_value = true; break;
        case 4: bad.has_uint64_value = true; break;
        case 5: bad.has_real64_value = false; break;
        case 6: bad.charset_name = "UTF-8"; break;
        case 7: bad.projection_descriptor.reset(); break;
        case 8: binding.type_uuid = {}; break;
        case 9: ++binding.datatype_descriptor_generation; break;
        case 10: binding.canonical_type_name = "int64"; break;
        case 11: binding.encoded_descriptor += ";precision=12"; break;
        case 12: bad.is_null = true; break;
      }
      if (variant >= 8 && variant <= 11)
        bad.projection_descriptor = std::make_shared<const api::EngineDescriptor>(binding);
      Refused(Cast(package.registry, bad, integer, callable));
    }
    auto bad_target = *real; bad_target.type_uuid = {};
    Refused(Cast(package.registry, Integer(1), std::make_shared<const api::EngineDescriptor>(bad_target), callable));
  }
  std::cout << "native numeric cast checks=" << checks << " failures=0\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
