// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_typed_row_codec.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "datatype_time.hpp"
#include "datatype_timestamp.hpp"
#include "datatype_interval.hpp"
#include "datatype_bit_string.hpp"
#include <iostream>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
thread_local long allocation_failure_after = -1;
thread_local std::size_t* allocation_counter = nullptr;
thread_local unsigned allocation_faults = 0;
}
void* operator new(std::size_t size) {
  if (allocation_counter) ++*allocation_counter;
  if (allocation_failure_after == 0) {
    allocation_failure_after = -1; // Fail once so diagnostic ownership can recover.
    ++allocation_faults;
    throw std::bad_alloc();
  }
  if (allocation_failure_after > 0) --allocation_failure_after;
  if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace api = scratchbird::engine::internal_api;
namespace bulk = api::dml::detail;
namespace dt = scratchbird::core::datatypes;
unsigned checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
api::EngineTypedValue Value(dt::CanonicalTypeId type, unsigned generation, std::uint64_t bits,
                          std::uint32_t fraction = 0) {
  api::EngineTypedValue value;
  const auto cohort = generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11;
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = entry.legacy_fields;
    if (row.catalog_snapshot_uuid != cohort || row.canonical_binary_type_code != static_cast<unsigned>(type)) continue;
    auto& descriptor = value.descriptor;
    descriptor.descriptor_uuid = descriptor.datatype_descriptor_uuid = row.descriptor_uuid;
    descriptor.datatype_descriptor_generation = row.descriptor_generation;
    descriptor.type_uuid = row.type_uuid;
    descriptor.datatype_cohort = {cohort, generation, generation};
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = type == dt::CanonicalTypeId::date ? "date" :
        type == dt::CanonicalTypeId::time ? "time" :
        type == dt::CanonicalTypeId::timestamp ? "timestamp" : "interval";
    descriptor.encoded_descriptor = "nullability=nullable";
    for (unsigned i = 0; i < (type == dt::CanonicalTypeId::date ? 4u : 8u); ++i)
      value.binary_value.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    if (type == dt::CanonicalTypeId::timestamp) {
      for (unsigned i = 0; i < 4; ++i)
        value.binary_value.push_back(static_cast<std::uint8_t>(fraction >> (8*i)));
      value.binary_value.resize(16, 0);
    }
    if (type == dt::CanonicalTypeId::interval) {
      value.binary_value.clear();
      for (const auto part : {bits, ~bits})
        for (unsigned i=0;i<4;++i)value.binary_value.push_back(static_cast<std::uint8_t>(part>>(8*i)));
      for (unsigned i=0;i<8;++i)value.binary_value.push_back(static_cast<std::uint8_t>(bits>>(8*i)));
    }
    return value;
  }
  throw std::runtime_error("explicit temporal fixture row unavailable");
}
api::InsertRowEncoderPlan Plan(const api::EngineTypedValue& value) {
  api::InsertRowEncoderPlan plan;
  api::InsertRowEncoderColumnPlan column;
  column.column_name = "v";
  column.canonical_type_name = value.descriptor.canonical_type_name;
  plan.columns.push_back(column);
  return plan;
}
void Refuses(dt::CanonicalTypeId type, const api::EngineTypedValue& value) {
  Check(!bulk::DirectValidateNativeTemporalValue(value, type), "invalid temporal carrier admitted");
  std::vector<std::uint8_t> output{0x5a};
  Check(!bulk::DirectPackTypedPayload(type, value, &output) && output == std::vector<std::uint8_t>{0x5a},
        "temporal packing refusal changed output");
  bool refused = false;
  try { (void)bulk::DirectStoredValueForColumn(value, type); } catch (const std::invalid_argument&) { refused = true; }
  Check(refused, "retained temporal path admitted invalid carrier");
  const auto plan = Plan(value);
  api::EngineRowValue row; row.fields.push_back({"v", value});
  const auto validation = bulk::BuildDirectFixedWidthPayloadValidationPlan(plan, &row);
  Check(!bulk::DirectFixedWidthTypedPayloadFailure(row, validation).empty(), "fixed-width fast path bypassed temporal validation");
  for (const auto* selected : {static_cast<const std::vector<bulk::DirectFixedWidthPayloadValidationColumnPlan>*>(nullptr), &validation}) {
    refused = false;
    try { (void)bulk::DirectPhysicalCellsFromTypedInputRow(row, plan, selected); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "physical temporal path admitted invalid carrier");
  }
}
void Run(dt::CanonicalTypeId type, unsigned generation) {
  const std::vector<std::uint64_t> cases = type == dt::CanonicalTypeId::date ?
      std::vector<std::uint64_t>{0, 1, 0xffffffffu, 0x80000000u, 0x7fffffffu} :
      type == dt::CanonicalTypeId::time ?
      std::vector<std::uint64_t>{0, 1, 255, 256, dt::kTimeMaximumNanosecondsV3} :
      type == dt::CanonicalTypeId::interval ?
      std::vector<std::uint64_t>{0,1,~std::uint64_t{0},0x80000000u,0x7fffffffu,
          0x8000000000000000ull,0x7fffffffffffffffull} :
      std::vector<std::uint64_t>{0, 1, 255, 256, ~std::uint64_t{0},
          static_cast<std::uint64_t>(dt::kTimestampMinimumCivilSecondV3),
          static_cast<std::uint64_t>(dt::kTimestampMaximumCivilSecondV3)};
  for (const auto bits : cases) {
    const auto value = Value(type, generation, bits);
    for (const bool null : {false, true}) {
      auto typed = value;
      if (null) { typed.state = api::EngineValueState::sql_null; typed.binary_value.clear(); }
      Check(bulk::DirectValidateNativeTemporalValue(typed, type), "exact native temporal refused");
      const auto stored = bulk::DirectStoredValueForColumn(typed, type);
      Check(stored.isSqlNull() == null && stored.bytes == std::string(typed.binary_value.begin(), typed.binary_value.end()),
            "retained temporal bytes changed");
      api::EngineRowValue row; row.fields.push_back({"v", typed});
      const auto plan = Plan(typed);
      const auto validation = bulk::BuildDirectFixedWidthPayloadValidationPlan(plan, &row);
      Check(bulk::DirectFixedWidthTypedPayloadFailure(row, validation).empty(), "valid temporal fixed-width admission failed");
      for (const auto* selected : {static_cast<const std::vector<bulk::DirectFixedWidthPayloadValidationColumnPlan>*>(nullptr), &validation}) {
        const auto physical = bulk::DirectPhysicalCellsFromTypedInputRow(row, plan, selected);
        Check(physical.cells.size() == 1 && physical.cells[0].value.type_id == type &&
            physical.cells[0].value.is_null == null && physical.cells[0].value.payload == typed.binary_value,
            "physical and retained temporal carriers diverged");
      }
      auto destination = typed.descriptor;
      Check(bulk::DirectValidateNativeTemporalValue(typed, type, &destination, true), "exact destination refused");
      destination.encoded_descriptor = "nullability=non_null";
      Check(bulk::DirectValidateNativeTemporalValue(typed, type, &destination, false) == !null,
            "destination nullability not enforced independently of source nullability");
      destination.datatype_cohort = generation == 10 ? api::EngineDatatypeCohort{dt::kDatatypeCohortV11,11,11} :
          api::EngineDatatypeCohort{dt::kDatatypeCohortV10,10,10};
      Check(!bulk::DirectValidateNativeTemporalValue(typed, type, &destination, false), "cross-cohort source relabeled");
      for (unsigned mutation = 0; mutation < 9; ++mutation) {
        destination = typed.descriptor;
        switch (mutation) {
          case 0: destination.datatype_descriptor_uuid.bytes[0] ^= 1; break;
          case 1: ++destination.datatype_descriptor_generation; break;
          case 2: destination.type_uuid.bytes[0] ^= 1; break;
          case 3: ++destination.datatype_cohort.catalog_generation; break;
          case 4: ++destination.datatype_cohort.registry_generation; break;
          case 5: destination.encoded_descriptor += ";precision=1"; break;
          case 6: destination.encoded_descriptor += ";unknown=1"; break;
          case 7: destination.charset_uuid = destination.type_uuid; break;
          case 8: destination.collation_uuid = destination.type_uuid; break;
        }
        api::EngineApiDiagnostic diagnostic;
        Check(!bulk::DirectValidateNativeTemporalValue(typed, type, &destination, true, &diagnostic) &&
                  diagnostic.error && !diagnostic.code.empty(),
              "invalid receiving descriptor admitted or diagnostic lost");
      }
      for (const bool source_nullable : {false, true}) {
        auto source = typed;
        source.descriptor.encoded_descriptor = source_nullable ? "nullability=nullable" : "nullability=non_null";
        for (const bool target_nullable : {false, true}) {
          destination = typed.descriptor;
          destination.encoded_descriptor = target_nullable ? "nullability=nullable" : "nullability=non_null";
          Check(bulk::DirectValidateNativeTemporalValue(source, type, &destination, target_nullable) ==
              (!null || (source_nullable && target_nullable)), "independent source/destination NULL contract changed");
        }
      }
    }
  }
  for (unsigned mutation = 0; mutation < 15; ++mutation) {
    auto value = Value(type, generation, 1);
    switch (mutation) {
      case 0: value.encoded_value = type == dt::CanonicalTypeId::date ? "1970-01-01" : "00:00:01"; value.binary_value.clear(); break;
      case 1: value.encoded_value = "ambiguous"; break;
      case 2: value.binary_value.pop_back(); break;
      case 3: value.binary_value.push_back(0); break;
      case 4: value.descriptor.datatype_cohort = {}; break;
      case 5: ++value.descriptor.datatype_cohort.registry_generation; break;
      case 6: value.descriptor.datatype_descriptor_uuid.bytes[0] ^= 1; break;
      case 7: ++value.descriptor.datatype_descriptor_generation; break;
      case 8: value.descriptor.type_uuid.bytes[0] ^= 1; break;
      case 9: value.descriptor.encoded_descriptor += ";precision=1"; break;
      case 10: value.descriptor.charset_uuid = value.descriptor.type_uuid; break;
      case 11: value.state = api::EngineValueState::sql_null; break;
      case 12: value.state = api::EngineValueState::missing; value.binary_value.clear(); break;
      case 13: value.descriptor.encoded_descriptor += ";unknown=1"; break;
      case 14: value.descriptor.collation_uuid = value.descriptor.type_uuid; break;
    }
    Refuses(type, value);
  }
  if (type == dt::CanonicalTypeId::time) {
    Refuses(type, Value(type, generation, dt::kTimeMaximumNanosecondsV3 + 1));
    Refuses(type, Value(type, generation, ~std::uint64_t{0}));
    api::EngineApiDiagnostic diagnostic;
    Check(!bulk::DirectValidateNativeTemporalValue(
        Value(type, generation, dt::kTimeMaximumNanosecondsV3 + 1), type, nullptr, true, &diagnostic) &&
        diagnostic.native_source && diagnostic.native_source->datatype_cause &&
        !diagnostic.native_source->datatype_cause->status.ok() &&
        diagnostic.native_source->datatype_cause->diagnostic_code == diagnostic.code &&
        diagnostic.native_source->datatype_cause->detail == diagnostic.detail,
        "native TIME component failure lost its typed cause");
  }
  if (type == dt::CanonicalTypeId::interval) {
    for (const auto* text : {"3600", "-3600", "P1DT2H3M4S", "P1Y2M", "P1M", "P0D", "1,2,3"}) {
      auto value=Value(type,generation,0);value.binary_value.clear();value.encoded_value=text;
      Refuses(type,value);
    }
    // Exhaustive independent component boundary combinations. No normalization
    // or conversion to seconds is authorized by this bulk carrier.
    for(const std::uint32_t months:{0u,1u,0xffffffffu,0x80000000u,0x7fffffffu})
      for(const std::uint32_t days:{0u,1u,0xffffffffu,0x80000000u,0x7fffffffu})
        for(const std::uint64_t nanos:{0ull,1ull,0xffffffffffffffffull,0x8000000000000000ull,0x7fffffffffffffffull}) {
          auto value=Value(type,generation,0);value.binary_value.clear();
          for(const auto part:{months,days})for(unsigned i=0;i<4;++i)
            value.binary_value.push_back(static_cast<std::uint8_t>(part>>(8*i)));
          for(unsigned i=0;i<8;++i)value.binary_value.push_back(static_cast<std::uint8_t>(nanos>>(8*i)));
          std::vector<std::uint8_t> output;
          Check(bulk::DirectPackTypedPayload(type,value,&output)&&output==value.binary_value,
                "INTERVAL component tuple normalized or lost bounds");
        }
    auto malformed=Value(type,generation,0);malformed.binary_value.pop_back();
    api::EngineApiDiagnostic diagnostic;
    Check(!bulk::DirectValidateNativeTemporalValue(malformed,type,nullptr,true,&diagnostic)&&
          diagnostic.native_source&&diagnostic.native_source->datatype_cause&&
          diagnostic.native_source->datatype_cause->diagnostic_code==diagnostic.code&&
          diagnostic.native_source->datatype_cause->detail==diagnostic.detail,
          "INTERVAL component failure lost typed cause");
  }
  if (type == dt::CanonicalTypeId::timestamp) {
    for (const auto* text : {"1970-01-01T00:00:00", "1970-01-01 01:00:00+01:00", "1970-01-01T00:00:00Z"}) {
      auto value = Value(type, generation, 0);
      value.binary_value.clear(); value.encoded_value = text;
      Refuses(type, value);
    }
    for (const auto fraction : {0u, 1u, 999999999u}) {
      for (const auto seconds : {dt::kTimestampMinimumCivilSecondV3, std::int64_t{-1},
          std::int64_t{0}, dt::kTimestampMaximumCivilSecondV3}) {
        const auto value = Value(type, generation, static_cast<std::uint64_t>(seconds), fraction);
        std::vector<std::uint8_t> payload;
        Check(bulk::DirectPackTypedPayload(type, value, &payload) && payload == value.binary_value,
              "TIMESTAMP fraction/extrema native bytes changed");
      }
    }
    for (const auto bits : {static_cast<std::uint64_t>(dt::kTimestampMinimumCivilSecondV3-1),
                           static_cast<std::uint64_t>(dt::kTimestampMaximumCivilSecondV3+1)})
      Refuses(type, Value(type, generation, bits));
    Refuses(type, Value(type, generation, 0, 1000000000));
    for (unsigned i = 12; i < 16; ++i) {
      auto value = Value(type, generation, 0); value.binary_value[i] = 1; Refuses(type, value);
    }
    api::EngineApiDiagnostic diagnostic;
    Check(!bulk::DirectValidateNativeTemporalValue(Value(type, generation, 0, 1000000000), type,
        nullptr, true, &diagnostic) && diagnostic.native_source && diagnostic.native_source->datatype_cause &&
        diagnostic.native_source->datatype_cause->diagnostic_code == diagnostic.code &&
        diagnostic.native_source->datatype_cause->detail == diagnostic.detail,
        "TIMESTAMP canonical failure lost typed diagnostic");
  }
}
void RetainedColumnBindings() {
  for (const unsigned generation : {10u, 11u}) {
    const auto date = Value(dt::CanonicalTypeId::date, generation, 0xffffffffu);
    const auto time = Value(dt::CanonicalTypeId::time, generation, dt::kTimeMaximumNanosecondsV3);
    const auto timestamp = Value(dt::CanonicalTypeId::timestamp, generation, ~std::uint64_t{0}, 999999999);
    const auto interval = Value(dt::CanonicalTypeId::interval, generation, 0x8000000000000000ull);
    auto plan = Plan(date);
    plan.columns[0].column_name = "day";
    auto middle = plan.columns[0]; middle.column_name = "opaque"; middle.canonical_type_name = "binary";
    auto last = plan.columns[0]; last.column_name = "clock"; last.canonical_type_name = "time";
    plan.columns.push_back(middle); plan.columns.push_back(last);
    auto stamp_column = last; stamp_column.column_name = "stamp"; stamp_column.canonical_type_name = "timestamp";
    plan.columns.push_back(stamp_column);
    auto interval_column=stamp_column;interval_column.column_name="span";interval_column.canonical_type_name="interval";
    plan.columns.push_back(interval_column);
    const auto day = bulk::DirectStoredValueForColumn(date, dt::CanonicalTypeId::date);
    const auto clock = bulk::DirectStoredValueForColumn(time, dt::CanonicalTypeId::time);
    const auto stamp = bulk::DirectStoredValueForColumn(timestamp, dt::CanonicalTypeId::timestamp);
    const auto span = bulk::DirectStoredValueForColumn(interval, dt::CanonicalTypeId::interval);
    for (const bool null : {false, true}) {
      const auto day_value = null ? api::CrudStoredValue::SqlNull() : day;
      const auto clock_value = null ? api::CrudStoredValue::SqlNull() : clock;
      const auto stamp_value = null ? api::CrudStoredValue::SqlNull() : stamp;
      const auto span_value = null ? api::CrudStoredValue::SqlNull() : span;
      // Retained input can be reordered or sparse. Its position is not the
      // physical column ordinal and must not select a different receiver.
      const api::CrudValueFields reordered{{"clock", clock_value}, {"day", day_value}};
      const auto cells = bulk::DirectPhysicalCells(reordered, &plan);
      Check(cells.size() == 2 && cells[0].column_ordinal == 3 && cells[1].column_ordinal == 1,
            "retained temporal ordinals followed input order");
      Check(cells[0].value.type_id == dt::CanonicalTypeId::time &&
                cells[1].value.type_id == dt::CanonicalTypeId::date &&
                cells[0].value.is_null == null && cells[1].value.is_null == null &&
                cells[0].value.payload == (null ? std::vector<std::uint8_t>{} : time.binary_value) &&
                cells[1].value.payload == (null ? std::vector<std::uint8_t>{} : date.binary_value),
            "retained temporal type, state or canonical payload changed");
      const auto sparse = bulk::DirectPhysicalCells({{"clock", clock_value}}, &plan);
      Check(sparse.size() == 1 && sparse[0].column_ordinal == 3 &&
                sparse[0].value.type_id == dt::CanonicalTypeId::time,
            "sparse retained temporal binding lost");
      bool refused = false;
      try { (void)bulk::DirectPhysicalCells({{"missing", clock_value}}, &plan); }
      catch (const std::invalid_argument&) { refused = true; }
      Check(refused, "missing retained receiving column accepted");
      for (bool sparse_timestamp : {false, true}) {
        const api::CrudValueFields fields = sparse_timestamp ? api::CrudValueFields{{"stamp", stamp_value}} :
            api::CrudValueFields{{"stamp", stamp_value}, {"day", day_value}, {"clock", clock_value}};
        const auto result = bulk::DirectPhysicalCells(fields, &plan);
        Check(result.size() == (sparse_timestamp ? 1 : 3) && result[0].column_ordinal == 4 &&
              result[0].value.type_id == dt::CanonicalTypeId::timestamp && result[0].value.is_null == null &&
              result[0].value.payload == (null ? std::vector<std::uint8_t>{} : timestamp.binary_value),
              "retained TIMESTAMP reordered/sparse/NULL binding lost");
        const api::CrudValueFields interval_fields=sparse_timestamp ? api::CrudValueFields{{"span",span_value}} :
            api::CrudValueFields{{"span",span_value},{"stamp",stamp_value},{"day",day_value}};
        const auto interval_result=bulk::DirectPhysicalCells(interval_fields,&plan);
        Check(interval_result.size()==(sparse_timestamp?1:3)&&interval_result[0].column_ordinal==5&&
              interval_result[0].value.type_id==dt::CanonicalTypeId::interval&&interval_result[0].value.is_null==null&&
              interval_result[0].value.payload==(null?std::vector<std::uint8_t>{}:interval.binary_value),
              "retained INTERVAL reordered/sparse/NULL binding lost");
      }
    }
    const std::string bytes{"\xff\0\x80", 3};
    const auto opaque = bulk::DirectPhysicalCells({{"opaque", api::CrudStoredValue(bytes)}}, &plan);
    Check(opaque.size() == 1 && opaque[0].column_ordinal == 2 &&
              opaque[0].value.type_id == dt::CanonicalTypeId::binary &&
              opaque[0].value.payload == std::vector<std::uint8_t>({255, 0, 128}),
          "opaque retained binary bytes were translated");
    bool refused = false;
    try { (void)bulk::DirectPhysicalCells({{"day", api::CrudStoredValue::SqlNull()}}, nullptr); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "profileless retained NULL accepted");
    auto oversized = plan;
    oversized.columns.resize(65536);
    oversized.columns.back() = last;
    oversized.columns.back().column_name = "overflow";
    refused = false;
    try { (void)bulk::DirectPhysicalCells({{"overflow", clock}}, &oversized); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "retained physical ordinal truncated");
  }
}
void ReceiverConstruction() {
  namespace page = scratchbird::storage::page;
  for (const auto type : {dt::CanonicalTypeId::date, dt::CanonicalTypeId::time,
                         dt::CanonicalTypeId::timestamp, dt::CanonicalTypeId::interval})
    for (const unsigned generation : {10u,11u}) for (const bool nullable : {false,true}) {
      auto value = Value(type,generation,0);
      auto& descriptor = value.descriptor;
      descriptor.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
      const auto& cohort = descriptor.datatype_cohort;
      dt::DatatypeStorageIdentityV3 identity;
      Check(dt::LookupDatatypeStorageIdentityV3(cohort.catalog_snapshot_uuid,cohort.catalog_generation,
          cohort.registry_generation,descriptor.datatype_descriptor_uuid,descriptor.datatype_descriptor_generation,
          &identity),"receiver exact identity unavailable");
      page::RowDataTemporalColumnBinding original;
      api::EngineApiDiagnostic diagnostic;
      Check(bulk::DirectBuildTemporalPageColumn(descriptor,identity,7,nullable,&original,&diagnostic),
            "receiver baseline failed");
      Check(original.column_ordinal==7&&original.null_allowed==nullable&&
          (bool(original.date)+bool(original.time)+bool(original.timestamp)+bool(original.interval))==1,
          "receiver shape or profile count invalid");
      if(original.date)Check(dt::ValidateDateProfileHandleV3(*original.date).ok(),"DATE receiver invalid");
      if(original.time)Check(dt::ValidateTimeProfileHandleV3(*original.time).ok(),"TIME receiver invalid");
      if(original.timestamp)Check(dt::ValidateTimestampProfileHandleV3(*original.timestamp).ok(),"TIMESTAMP receiver invalid");
      if(original.interval)Check(dt::ValidateIntervalProfileHandleV3(*original.interval).ok(),"INTERVAL receiver invalid");
      const auto unchanged = [&](const page::RowDataTemporalColumnBinding& actual) {
        return actual.column_ordinal==original.column_ordinal&&actual.null_allowed==original.null_allowed&&
            actual.date==original.date&&actual.time==original.time&&actual.timestamp==original.timestamp&&
            actual.interval==original.interval;
      };
      auto output=original;
      std::size_t allocations=0;
      allocation_counter=&allocations;
      const bool counted=bulk::DirectBuildTemporalPageColumn(descriptor,identity,9,nullable,&output,&diagnostic);
      allocation_counter=nullptr;
      Check(counted&&allocations>0&&allocations<1000,"receiver allocation sweep unbounded or empty");
      unsigned native_resource_causes=0;
      for(std::size_t position=0;position<allocations;++position) {
        output=original;
        diagnostic=api::EngineApiDiagnostic{};
        const auto before=allocation_faults;
        allocation_failure_after=static_cast<long>(position);
        bool accepted=false,threw=false;
        try { accepted=bulk::DirectBuildTemporalPageColumn(descriptor,identity,9,nullable,&output,&diagnostic); }
        catch(...) { threw=true; }
        allocation_failure_after=-1;
        Check(allocation_faults==before+1,"receiver allocation fault did not fire");
        if(accepted||threw||diagnostic.code!="RESOURCE.BUDGET_EXCEEDED")
          std::cerr<<"receiver allocation position="<<position<<" type="<<static_cast<unsigned>(type)
                   <<" code="<<diagnostic.code<<" detail="<<diagnostic.detail<<'\n';
        Check(!accepted&&!threw&&diagnostic.error&&diagnostic.code=="RESOURCE.BUDGET_EXCEEDED",
              "receiver allocation failure lost resource diagnosis");
        Check(unchanged(output),"receiver allocation failure replaced previous owner");
        if(diagnostic.native_source&&diagnostic.native_source->datatype_cause) {
          const auto& cause=*diagnostic.native_source->datatype_cause;
          Check(cause.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED"&&!cause.status.ok(),
                "receiver native resource cause changed");
          ++native_resource_causes;
        }
        Check(bulk::DirectBuildTemporalPageColumn(descriptor,identity,9,nullable,&output,&diagnostic)&&
              output.column_ordinal==9,"receiver fresh retry failed");
      }
      if(type!=dt::CanonicalTypeId::date)
        Check(native_resource_causes>0,"receiver sweep did not exercise typed profile allocation failure");
      for(const bool invalid_ordinal:{false,true}) {
        output=original;
        auto invalid=descriptor;
        if(!invalid_ordinal)++invalid.datatype_descriptor_generation;
        Check(!bulk::DirectBuildTemporalPageColumn(invalid,identity,invalid_ordinal?0:9,nullable,&output,&diagnostic)&&
              unchanged(output),"invalid receiver published a profile");
      }
      output=original;
      Check(!bulk::DirectBuildTemporalPageColumn(descriptor,identity,9,!nullable,&output,&diagnostic)&&
            unchanged(output),"receiver nullability contradiction accepted");
      const auto foreign=Value(type,generation==10?11:10,0);
      auto foreign_descriptor=foreign.descriptor;
      foreign_descriptor.encoded_descriptor=descriptor.encoded_descriptor;
      output=original;
      Check(!bulk::DirectBuildTemporalPageColumn(foreign_descriptor,identity,9,nullable,&output,&diagnostic)&&
            unchanged(output),"receiver relabeled another valid cohort");
      for(unsigned mutation=0;mutation<6;++mutation) {
        auto invalid=identity;
        switch(mutation) {
          case 0: invalid.descriptor_uuid.bytes.back()^=1;break;
          case 1: ++invalid.descriptor_generation;break;
          case 2: invalid.type_uuid.bytes.back()^=1;break;
          case 3: invalid.type_id=dt::CanonicalTypeId::int64;break;
          case 4: invalid.codec.reset();break;
          case 5: ++invalid.codec->native_fields.canonical_value_maximum_bytes;break;
        }
        output=original;
        Check(!bulk::DirectBuildTemporalPageColumn(descriptor,invalid,9,nullable,&output,&diagnostic)&&
              unchanged(output),"receiver admitted inconsistent storage/codec identity");
      }
    }
}
api::EngineTypedValue BitValue(unsigned generation, unsigned kind, bool nullable) {
  api::EngineTypedValue value;
  const auto cohort = generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11;
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = entry.legacy_fields;
    if (row.catalog_snapshot_uuid != cohort || row.canonical_binary_type_code != 302) continue;
    auto& descriptor = value.descriptor;
    descriptor.descriptor_uuid = descriptor.datatype_descriptor_uuid = row.descriptor_uuid;
    descriptor.datatype_descriptor_generation = row.descriptor_generation;
    descriptor.type_uuid = row.type_uuid;
    descriptor.datatype_cohort = {cohort, generation, generation};
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = "bit_string";
    descriptor.encoded_descriptor = std::string("nullability=") + (nullable ? "nullable" : "non_null") +
        ";bit_profile_kind=" + std::to_string(kind) + (kind ? ";length=9" : "");
    // Independently written count LE32 followed by exactly nine MSB0 bits.
    value.binary_value = {9,0,0,0,0xA5,0x80};
    return value;
  }
  throw std::runtime_error("explicit BIT identity unavailable");
}

void BitReceivers() {
  namespace page = scratchbird::storage::page;
  for (const unsigned generation : {10u,11u}) for (unsigned kind = 0; kind < 3; ++kind)
    for (const bool nullable : {false,true}) {
      auto value = BitValue(generation,kind,nullable);
      const auto& descriptor = value.descriptor;
      const auto& cohort = descriptor.datatype_cohort;
      dt::DatatypeStorageIdentityV3 identity;
      Check(dt::LookupDatatypeStorageIdentityV3(cohort.catalog_snapshot_uuid,cohort.catalog_generation,
          cohort.registry_generation,descriptor.datatype_descriptor_uuid,descriptor.datatype_descriptor_generation,
          &identity),"BIT exact identity unavailable");
      page::RowDataProfileColumnBinding original;
      api::EngineApiDiagnostic diagnostic;
      Check(bulk::DirectBuildBitStringPageColumn(descriptor,identity,7,nullable,&original,&diagnostic),
            "BIT receiver baseline failed");
      Check(original.column_ordinal==7 && original.null_allowed==nullable && original.bit_string &&
            !original.date && !original.time && !original.timestamp && !original.interval &&
            original.bit_string->length_bits == (kind ? 9u : dt::kBitStringMaximumLogicalBitsV3) &&
            original.bit_string->fixed_length == (kind==1) &&
            dt::ValidateBitStringDescriptorProfileNoAllocV3(*original.bit_string).ok(),
            "BIT receiver profile invalid");
      const auto unchanged = [&](const page::RowDataProfileColumnBinding& actual) {
        return actual.column_ordinal==original.column_ordinal && actual.null_allowed==original.null_allowed &&
            actual.bit_string==original.bit_string && !actual.date && !actual.time && !actual.timestamp && !actual.interval;
      };
      for (const bool null : {false,true}) {
        auto sample=value;
        if(null) { sample.state=api::EngineValueState::sql_null; sample.binary_value.clear(); }
        Check(bulk::DirectValidateNativeBitStringValue(sample,nullptr,true,&diagnostic)==(!null||nullable),
              "BIT source state admission disagrees");
        Check(bulk::DirectValidateNativeBitStringValue(sample,&descriptor,nullable,&diagnostic)==(!null||nullable),
              "BIT receiving state admission disagrees");
        for (const unsigned target_generation : {10u,11u}) for (unsigned target_kind=0;target_kind<3;++target_kind) {
          auto target=BitValue(target_generation,target_kind,nullable).descriptor;
          Check(bulk::DirectValidateNativeBitStringValue(sample,&target,nullable,&diagnostic)==
                (target_generation==generation && target_kind==kind && (!null||nullable)),
                "BIT source/target profile or cohort was silently converted");
        }
      }
      for(unsigned mutation=0;mutation<12;++mutation) {
        auto invalid=value;
        switch(mutation) {
          case 0: invalid.binary_value.back()|=1;break;
          case 1: invalid.binary_value.push_back(0);break;
          case 2: invalid.binary_value.pop_back();break;
          case 3: invalid.binary_value[0]=10;break;
          case 4: invalid.encoded_value="101001011";break;
          case 5: invalid.state=api::EngineValueState::lob_handle;break;
          case 6: invalid.state=api::EngineValueState::sql_null;break;
          case 7: ++invalid.descriptor.datatype_descriptor_generation;break;
          case 8: invalid.descriptor.type_uuid.bytes.back()^=1;break;
          case 9: invalid.descriptor.datatype_cohort.catalog_generation++;break;
          case 10: invalid.descriptor.encoded_descriptor+=";timezone_generation=1";break;
          case 11: invalid.descriptor.encoded_descriptor+=";precision=1";break;
        }
        // Ten bits with an all-zero tail is valid for variable BIT, so make
        // the alternate count exceed the complete carrier instead.
        if(mutation==3) invalid.binary_value[0]=17;
        Check(!bulk::DirectValidateNativeBitStringValue(invalid,&descriptor,nullable,&diagnostic),
              "malformed BIT carrier or authority admitted");
        if(mutation==0) Check(diagnostic.code=="CTB.BIT.CANONICAL_ENCODING_INVALID" && diagnostic.native_source,
                              "BIT component native diagnosis lost");
      }
      for(const auto metadata : {"", "length=9", "bit_profile_kind=1", "bit_profile_kind=2",
          "bit_profile_kind=3", "bit_profile_kind=01", "bit_profile_kind=0;length=0",
          "bit_profile_kind=1;length=0", "bit_profile_kind=2;length=16777217",
          "bit_profile_kind=2;length=-1", "bit_profile_kind=1;length=9;other=1"}) {
        auto invalid=descriptor;
        invalid.encoded_descriptor=std::string("nullability=")+(nullable?"nullable":"non_null")+";"+metadata;
        auto output=original;
        const bool accepted=bulk::DirectBuildBitStringPageColumn(invalid,identity,9,nullable,&output,&diagnostic);
        if(std::string_view(metadata).empty()) Check(accepted,"unqualified BIT default refused");
        else Check(!accepted && unchanged(output) && diagnostic.code=="CTB.BIT.DESCRIPTOR_INVALID",
                   "invalid BIT profile metadata admitted or output modified");
      }
      for(unsigned mutation=0;mutation<8;++mutation) {
        auto invalid=identity;
        switch(mutation) {
          case 0: invalid.descriptor_uuid.bytes.back()^=1;break;
          case 1: ++invalid.descriptor_generation;break;
          case 2: invalid.type_uuid.bytes.back()^=1;break;
          case 3: invalid.type_id=dt::CanonicalTypeId::binary;break;
          case 4: invalid.codec.reset();break;
          case 5: ++invalid.codec->native_fields.canonical_value_maximum_bytes;break;
          case 6: ++invalid.codec->legacy_fields.codec_generation;break;
          case 7: ++invalid.codec->legacy_fields.registry_generation;break;
        }
        auto output=original;
        Check(!bulk::DirectBuildBitStringPageColumn(descriptor,invalid,9,nullable,&output,&diagnostic) && unchanged(output),
              "inconsistent BIT identity admitted");
      }
      for(const bool zero_ordinal:{false,true}) {
        auto output=original;
        Check(!bulk::DirectBuildBitStringPageColumn(descriptor,identity,zero_ordinal?0:9,
                  zero_ordinal?nullable:!nullable,&output,&diagnostic) && unchanged(output),
              "BIT ordinal/nullability contradiction admitted");
      }
      auto output=original;
      std::size_t allocations=0;
      allocation_counter=&allocations;
      const bool counted=bulk::DirectBuildBitStringPageColumn(descriptor,identity,9,nullable,&output,&diagnostic);
      allocation_counter=nullptr;
      Check(counted && allocations>0 && allocations<1000,"BIT receiver allocation sweep unbounded");
      for(std::size_t position=0;position<allocations;++position) {
        output=original;
        diagnostic={};
        const auto before=allocation_faults;
        allocation_failure_after=static_cast<long>(position);
        bool accepted=false,threw=false;
        try { accepted=bulk::DirectBuildBitStringPageColumn(descriptor,identity,9,nullable,&output,&diagnostic); }
        catch(...) { threw=true; }
        allocation_failure_after=-1;
        Check(allocation_faults==before+1 && !accepted && !threw && unchanged(output) &&
              diagnostic.code=="RESOURCE.BUDGET_EXCEEDED", "BIT allocation failure lost atomicity or diagnosis");
        Check(bulk::DirectBuildBitStringPageColumn(descriptor,identity,9,nullable,&output,&diagnostic),
              "BIT receiver fresh retry failed");
      }
      for(const bool null:{false,true}) for(const bool receiving:{false,true}) {
        if(null&&!nullable) continue;
        auto sample=value;
        if(null) { sample.state=api::EngineValueState::sql_null; sample.binary_value.clear(); }
        const auto* target=receiving?&descriptor:nullptr;
        allocations=0;
        allocation_counter=&allocations;
        const bool valid=bulk::DirectValidateNativeBitStringValue(sample,target,nullable,&diagnostic);
        allocation_counter=nullptr;
        Check(valid && allocations>0 && allocations<1000,"BIT value allocation sweep unbounded");
        for(std::size_t position=0;position<allocations;++position) {
          diagnostic={};
          const auto before=allocation_faults;
          allocation_failure_after=static_cast<long>(position);
          bool accepted=false,threw=false;
          try { accepted=bulk::DirectValidateNativeBitStringValue(sample,target,nullable,&diagnostic); }
          catch(...) { threw=true; }
          allocation_failure_after=-1;
          if(accepted||threw||diagnostic.code!="RESOURCE.BUDGET_EXCEEDED")
            std::cerr<<"BIT value allocation="<<position<<" code="<<diagnostic.code<<" detail="<<diagnostic.detail<<'\n';
          Check(allocation_faults==before+1 && !accepted && !threw && diagnostic.code=="RESOURCE.BUDGET_EXCEEDED",
                "BIT value allocation failure misclassified");
          Check(bulk::DirectValidateNativeBitStringValue(sample,target,nullable,&diagnostic),"BIT value retry failed");
        }
      }
    }
}

int main() {
  for (const auto type : {dt::CanonicalTypeId::date, dt::CanonicalTypeId::time, dt::CanonicalTypeId::timestamp, dt::CanonicalTypeId::interval})
    for (const unsigned generation : {10u, 11u}) Run(type, generation);
  RetainedColumnBindings();
  ReceiverConstruction();
  BitReceivers();
  std::cout << "direct_bulk_temporal_payload checks=" << checks << '\n';
}
