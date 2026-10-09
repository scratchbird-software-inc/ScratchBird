// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query/expression_api.hpp"
#include "mga_relation_store/stored_scalar_payload.hpp"
#include "mga_relation_store/stored_integer_descriptor.hpp"
#include "descriptor_value_runtime.hpp"
#include "executor_foundation.hpp"
#include "engine/public_abi_int64_payload.hpp"
#include "wire/public_result_packet.hpp"
#include "dml/direct_bulk_typed_row_codec.hpp"
#include "engine/sblr/native_row_field.hpp"
#include "../support/exact_datatype_descriptor_fixture.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace ex = scratchbird::engine::executor;
unsigned checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
api::EngineTypedValue Value(dt::CanonicalTypeId type, unsigned width,
                            std::int64_t number, unsigned occurrence) {
  api::EngineTypedValue value;
  value.descriptor = scratchbird::tests::ExactScalarDescriptorFixture(
      type, width == 4 ? "int32" : "int64",
      scratchbird::tests::FixtureUuid(32, occurrence), "nullability=nullable");
  value.setState(api::EngineValueState::value);
  for (unsigned byte = 0; byte < width; ++byte)
    value.binary_value.push_back(static_cast<std::uint8_t>(
        static_cast<std::uint64_t>(number) >> (8 * byte)));
  return value;
}
int main() {
  for (const unsigned width : {4u, 8u}) {
    const auto type = width == 4 ? dt::CanonicalTypeId::int32 : dt::CanonicalTypeId::int64;
    for (const bool nullable : {false, true}) {
      auto low = Value(type, width, -9, 901);
      low.descriptor.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
      auto high = Value(type, width, 12, 901);
      high.descriptor = low.descriptor;
      const auto batch = ex::MakeDescriptorBatch({{"key", low.descriptor, nullable}},
                                                 {{{high}}, {{low}}, {{high}}});
      ex::DescriptorRuntimeDiagnostic diagnostic;
      const auto filtered = ex::FilterDescriptorBatchByComparison(batch, 0,
          ex::DescriptorComparisonOperator::kGreaterThan, low, &diagnostic);
      Check(diagnostic.ok && filtered.rows.size() == 2 && ex::ValidateDescriptorBatch(filtered).ok,
            "integer comparison bound invented nullable authority");
      for (const bool specialized : {false, true}) {
        const auto counted = specialized
            ? ex::AggregateDescriptorCountByInt64(batch, 0, "count", &diagnostic)
            : ex::AggregateDescriptorCountByKey(batch, 0, "count", &diagnostic);
        Check(diagnostic.ok && counted.rows.size() == 2 && ex::ValidateDescriptorBatch(counted).ok,
              "integer grouping lost native width, occurrence or count nullability");
        std::int64_t total = 0;
        for (const auto& row : counted.rows) {
          Check(row.values[0].descriptor == low.descriptor && row.values[0].binary_value.size() == width,
                "integer grouping rebound the representative descriptor");
          total += ex::DecodeInt64Value(row.values[1]).value;
        }
        Check(total == 3, "integer grouping changed cardinality");
      }
      const auto numbered = ex::WindowDescriptorRowNumberByInt64(batch, 0, "position", true, &diagnostic);
      Check(diagnostic.ok && numbered.rows.size() == 3 && ex::ValidateDescriptorBatch(numbered).ok,
            "row numbering published invalid native count descriptors");
      for (std::size_t i = 0; i < numbered.rows.size(); ++i)
        Check(ex::DecodeInt64Value(numbered.rows[i].values[1]).value == static_cast<std::int64_t>(i + 1),
              "row numbering changed ordinal");
      auto stale = low;
      ++stale.descriptor.datatype_descriptor_generation;
      ex::FilterDescriptorBatchByComparison(batch, 0,
          ex::DescriptorComparisonOperator::kGreaterThan, stale, &diagnostic);
      Check(!diagnostic.ok, "integer bound bypassed stale generation admission");
      auto empty = batch;
      empty.rows.clear();
      for (const bool specialized : {false, true}) {
        const auto counted = specialized
            ? ex::AggregateDescriptorCountByInt64(empty, 0, "count", &diagnostic)
            : ex::AggregateDescriptorCountByKey(empty, 0, "count", &diagnostic);
        Check(diagnostic.ok && counted.rows.empty() && ex::ValidateDescriptorBatch(counted).ok,
              "empty integer grouping published an invalid descriptor");
      }
      const auto no_numbers = ex::WindowDescriptorRowNumberByInt64(empty, 0, "position", true, &diagnostic);
      Check(diagnostic.ok && no_numbers.rows.empty() && ex::ValidateDescriptorBatch(no_numbers).ok,
            "empty row numbering bypassed native descriptor admission");
      for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto invalid = low;
        if (mutation == 0) invalid.encoded_value = "-9";
        if (mutation == 1) invalid.binary_value.pop_back();
        if (mutation == 2) invalid.is_null = true;
        ex::FilterDescriptorBatchByComparison(batch, 0,
            ex::DescriptorComparisonOperator::kGreaterThan, invalid, &diagnostic);
        Check(!diagnostic.ok, "integer filter accepted malformed native bound");
      }
      if (nullable) {
        auto null = low;
        null.setState(api::EngineValueState::sql_null);
        null.binary_value.clear();
        const auto filtered_null = ex::FilterDescriptorBatchByComparison(batch, 0,
            ex::DescriptorComparisonOperator::kGreaterThan, null, &diagnostic);
        Check(diagnostic.ok && filtered_null.rows.empty(), "NULL integer bound lost UNKNOWN filtering");
      }
    }
  }
  namespace bulk = api::dml::detail;
  const std::array<std::uint64_t, 8> unsigned_values{
      0, 1, 255, 256, 65536, std::uint64_t{1} << 63,
      UINT64_MAX - 1, UINT64_MAX};
  for (const auto number : unsigned_values) {
    auto value = ex::EncodeUint64Value(number);
    value.descriptor.descriptor_kind = "scalar";
    value.descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(32, 90);
    std::uint64_t decoded = 0;
    std::string detail;
    Check(ex::DecodeBoundUint64Value(value, &decoded, &detail) && decoded == number,
          "native UINT64 round trip failed");
    const std::string stored(value.binary_value.begin(), value.binary_value.end());
    auto restored = value;
    restored.binary_value.clear();
    Check(api::RestoreStoredScalarPayloadV1(stored, api::EngineValueState::value, &restored) &&
              api::StoredScalarPayloadMatchesV1(restored, stored, api::EngineValueState::value) &&
              restored.binary_value == value.binary_value && restored.encoded_value.empty(),
          "stored UINT64 was rendered as text");
    auto nullable = value;
    nullable.descriptor.encoded_descriptor = "nullability=nullable";
    auto null = nullable;
    null.binary_value.clear();
    null.setState(api::EngineValueState::sql_null);
    auto batch = ex::MakeDescriptorBatch({{"u", nullable.descriptor, true, 1}},
                                        {{{nullable}}, {{null}}});
    Check(ex::ValidateDescriptorBatch(batch).ok, "native UINT64 batch was refused");
    batch.rows.clear();
    Check(ex::ValidateDescriptorBatch(batch).ok, "empty native UINT64 batch was refused");
    ++batch.columns[0].descriptor.datatype_descriptor_generation;
    Check(!ex::ValidateDescriptorBatch(batch).ok, "empty UINT64 batch bypassed binding admission");
    for (const auto placement : {ex::CanonicalDescriptorNullPlacement::first,
                                 ex::CanonicalDescriptorNullPlacement::last}) {
      for (const auto direction : {ex::CanonicalDescriptorOrderDirection::ascending,
                                   ex::CanonicalDescriptorOrderDirection::descending}) {
        ex::CanonicalDescriptorOrderTerm term;
        term.expression_descriptor_id = 1;
        term.null_placement = placement;
        term.direction = direction;
        const auto left = ex::CompareCanonicalDescriptorOrderValues(null, nullable, term);
        const auto right = ex::CompareCanonicalDescriptorOrderValues(nullable, null, term);
        const int expected = placement == ex::CanonicalDescriptorNullPlacement::first ? -1 : 1;
        Check(left.diagnostic.ok && right.diagnostic.ok && left.comparison == expected &&
                  right.comparison == -expected, "UINT64 NULL placement changed with direction");
      }
    }
    for (const auto other : unsigned_values) {
      auto right = ex::EncodeUint64Value(other);
      right.descriptor = value.descriptor;
      int comparison = 0;
      const int expected = number < other ? -1 : number > other ? 1 : 0;
      const bool compared = api::QowCompareCanonicalNonCollatedScalarsV1(value, right, &comparison, &detail);
      if (!compared || comparison != expected)
        std::cerr << "UINT64 " << number << " vs " << other << ": " << detail << '\n';
      Check(compared && comparison == expected, "UINT64 comparison used signed or textual order");
      for (const bool descending : {false, true}) {
        ex::CanonicalDescriptorOrderTerm term;
        term.expression_descriptor_id = 1;
        term.direction = descending ? ex::CanonicalDescriptorOrderDirection::descending
                                    : ex::CanonicalDescriptorOrderDirection::ascending;
        const auto order = ex::CompareCanonicalDescriptorOrderValues(value, right, term);
        Check(order.diagnostic.ok && order.comparison == (descending ? -expected : expected),
              "UINT64 physical order differs from unsigned oracle");
        const auto left_key = ex::MakeCanonicalDescriptorEqualityKey(value, term);
        const auto right_key = ex::MakeCanonicalDescriptorEqualityKey(right, term);
        Check(left_key.diagnostic.ok && right_key.diagnostic.ok &&
                  (left_key.equality_key == right_key.equality_key) == (number == other),
              "UINT64 equality key differs from exact native value");
      }
    }
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
      auto invalid = value;
      switch (mutation) {
        case 0: invalid.encoded_value = "1"; break;
        case 1: invalid.binary_value.pop_back(); break;
        case 2: ++invalid.descriptor.datatype_descriptor_generation; break;
        case 3: invalid.is_null = true; break;
        case 4: invalid.binary_value.clear(); invalid.encoded_value = "1"; break;
      }
      Check(!ex::DecodeBoundUint64Value(invalid, &decoded, &detail),
            "malformed UINT64 carrier or descriptor was admitted");
      ex::CanonicalDescriptorOrderTerm term;
      term.expression_descriptor_id = 1;
      Check(!ex::CompareCanonicalDescriptorOrderValues(invalid, value, term).diagnostic.ok,
            "malformed UINT64 was admitted to ordering");
    }
  }
  for (const auto width : {4u, 8u}) {
    namespace sblr = scratchbird::engine::sblr;
    for (unsigned bytes = 0; bytes <= 16; ++bytes) {
      sblr::SblrOperand operand;
      operand.name = "n";
      operand.type = width == 4 ? "row_new_field_binary16.int32" : "row_new_field_binary16.int64";
      operand.value_kind = sblr::SblrValueKind::literal_typed;
      operand.value_body.resize(40 + bytes, 0);
      operand.value_body[16] = 16 + bytes;
      const auto row = scratchbird::tests::FixtureUuid(32, 61);
      std::copy(row.bytes.begin(), row.bytes.end(), operand.value_body.begin() + 24);
      Check(sblr::DecodeNativeRowField(operand).has_value() == (bytes == width),
            "native SBLR row field admitted wrong signed integer width");
    }
    auto display = Value(width == 4 ? dt::CanonicalTypeId::int32 : dt::CanonicalTypeId::int64,
                         width, 1, 1);
    display.binary_value.clear(); display.encoded_value = "1";
    std::vector<std::uint8_t> packed;
    Check(!bulk::DirectPackTypedPayload(dt::CanonicalTypeId::int64, display, &packed),
          "integer source accepted display text as native data");
  }
  {
    api::EngineInsertRowsRequest request;
    api::BoundInsertRowTemplate row_template;
    row_template.max_inline_encoded_bytes = 1024;
    api::InsertRowEncoderPlan encoder;
    api::InsertRowEncoderColumnPlan narrow, wide;
    narrow.column_name = "narrow"; narrow.canonical_type_name = "int32";
    wide.column_name = "wide"; wide.canonical_type_name = "int64";
    encoder.columns = {narrow, wide};
    api::EngineRowValue source;
    source.requested_row_uuid = scratchbird::tests::FixtureUuid(32, 60);
    source.fields = {{"wide", Value(dt::CanonicalTypeId::int64, 8, INT64_MAX, 1)},
                     {"narrow", Value(dt::CanonicalTypeId::int64, 8, INT32_MIN, 2)}};
    const auto prepared = api::PrepareInsertRowForBatch(request, source, row_template, encoder);
    Check(prepared.values.size() == 2 && prepared.values[0].first == "narrow" &&
              prepared.values[0].second.bytes == std::string("\0\0\0\x80", 4) &&
              prepared.values[1].first == "wide" && prepared.values[1].second.bytes.size() == 8 &&
              prepared.encoded_bytes == 22,
          "reordered INSERT encoded against source order or accounted source width");
    source.fields.erase(source.fields.begin());
    const auto partial = api::PrepareInsertRowForBatch(request, source, row_template, encoder);
    Check(partial.values[0].first == "narrow" && partial.values[0].second.bytes.size() == 4,
          "subset INSERT lost destination integer width");
  }
  for (auto source_width : {4u, 8u}) for (auto target_width : {4u, 8u}) {
    for (std::int64_t integer : {INT64_MIN, std::int64_t(INT32_MIN) - 1,
          std::int64_t(INT32_MIN), std::int64_t(-1), std::int64_t(0),
          std::int64_t(1), std::int64_t(INT32_MAX),
          std::int64_t(INT32_MAX) + 1, INT64_MAX}) {
      if (source_width == 4 && (integer < INT32_MIN || integer > INT32_MAX)) continue;
      const auto source_type = source_width == 4 ? dt::CanonicalTypeId::int32 : dt::CanonicalTypeId::int64;
      const auto target_type = target_width == 4 ? dt::CanonicalTypeId::int32 : dt::CanonicalTypeId::int64;
      const auto source = Value(source_type, source_width, integer, 1);
      const bool fits = target_width == 8 || (integer >= INT32_MIN && integer <= INT32_MAX);
      std::vector<std::uint8_t> packed;
      Check(bulk::DirectPackTypedPayload(target_type, source, &packed) == fits,
            "integer storage conversion ignored target range");
      if (fits) {
        const auto expected = Value(target_type, target_width, integer, 2);
        const auto stored = bulk::DirectStoredValueForColumn(source, target_type);
        Check(packed == expected.binary_value && stored.isPresent() &&
                  stored.bytes == std::string(packed.begin(), packed.end()),
              "physical and retained integer payloads disagree");
        if (source_width < target_width) {
          api::EngineTypedValue promoted;
          std::string category, detail;
          Check(api::QowApplyCanonicalDescriptorCoercionV1(source, expected.descriptor,
                    false, &promoted, &category, &detail) &&
                    promoted.binary_value == expected.binary_value && promoted.encoded_value.empty(),
                "bound comparison widening lost native integer value");
        }
      } else {
        bool refused = false;
        try { (void)bulk::DirectStoredValueForColumn(source, target_type); }
        catch (const std::invalid_argument&) { refused = true; }
        Check(refused && packed.empty(), "invalid integer storage conversion published a value");
      }
    }
  }
  for (const auto target : {dt::CanonicalTypeId::int32, dt::CanonicalTypeId::int64}) {
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
      auto bad = Value(dt::CanonicalTypeId::int64, 8, -1, 1);
      if (mutation == 0) bad.encoded_value = "-1";
      if (mutation == 1) bad.descriptor.canonical_type_name = "int32";
      if (mutation == 2) bad.descriptor.canonical_type_name = "uint64";
      if (mutation == 3) bad.descriptor.canonical_type_name = "real64";
      if (mutation == 4) bad.binary_value.pop_back();
      std::vector<std::uint8_t> packed;
      Check(!bulk::DirectPackTypedPayload(target, bad, &packed) && packed.empty(),
            "malformed or overflowing source integer admitted by storage");
    }
  }
  for (const unsigned width : {4, 8}) {
    const auto type = width == 4 ? dt::CanonicalTypeId::int32 : dt::CanonicalTypeId::int64;
    const std::array<std::int64_t, 11> numbers = {
        width == 4 ? INT32_MIN : INT64_MIN, -65536, -256, -1, 0, 1, 127, 255, 256, 65536,
        width == 4 ? INT32_MAX : INT64_MAX};
    for (const auto a : numbers) for (const auto b : numbers) {
      const auto left = Value(type, width, a, 1), right = Value(type, width, b, 2);
      int comparison = 42;
      std::string detail;
      Check(api::QowCompareCanonicalNonCollatedScalarsV1(left, right, &comparison, &detail),
            "exact native integer comparison refused");
      Check(comparison == (a < b ? -1 : a > b ? 1 : 0),
            "native integer comparison differs from signed oracle");
      for (bool descending : {false, true}) {
        ex::CanonicalDescriptorOrderTerm term;
        term.expression_descriptor_id = 1;
        term.direction = descending ? ex::CanonicalDescriptorOrderDirection::descending
                                    : ex::CanonicalDescriptorOrderDirection::ascending;
        const auto ordered = ex::CompareCanonicalDescriptorOrderValues(left, right, term);
        Check(ordered.diagnostic.ok && ordered.comparison == (descending ? -comparison : comparison),
              "native integer physical ordering differs from signed oracle");
        const auto ka = ex::MakeCanonicalDescriptorEqualityKey(left, term);
        // Equality keys are occurrence-bound. SQL comparison above exercises
        // distinct occurrences; physical keys compare values in one column.
        auto same_column = right;
        same_column.descriptor = left.descriptor;
        const auto kb = ex::MakeCanonicalDescriptorEqualityKey(same_column, term);
        const auto plan = ex::PlanCanonicalDescriptorEqualityKey(left, term);
        Check(ka.diagnostic.ok && kb.diagnostic.ok && plan.diagnostic.ok &&
                  (ka.equality_key == kb.equality_key) == (a == b) &&
                  ka.equality_key.capacity() <= plan.retained_key_bytes,
              "native integer equality key or admitted size is incorrect");
      }
    }
    for (const auto number : numbers) {
      const auto original = Value(type, width, number, 1);
      const std::string bytes(original.binary_value.begin(), original.binary_value.end());
      auto restored = original;
      restored.encoded_value = "stale";
      restored.binary_value.clear();
      Check(api::RestoreStoredScalarPayloadV1(bytes, api::EngineValueState::value, &restored) &&
                restored.descriptor == original.descriptor && restored.encoded_value.empty() &&
                restored.binary_value == original.binary_value &&
                api::StoredScalarPayloadMatchesV1(restored, bytes, api::EngineValueState::value),
            "stored native integer restoration changed binding or bits");
      const auto decoded = ex::DecodeInt64Value(restored);
      Check(decoded.diagnostic.ok && decoded.value == number, "native integer decode changed value");
      std::array<char, 8> widened;
      std::string_view payload;
      Check(scratchbird::engine::PublicSignedIntegerScalarPayloadV1(restored, &widened, &payload),
            "native signed integer wire projection refused");
      namespace packet = scratchbird::wire::public_result;
      const packet::Field field{"integer", packet::Kind::signed_integer, std::string(payload)};
      Check(packet::AsSigned(field) == number && restored.binary_value == original.binary_value &&
                restored.descriptor == original.descriptor,
            "signed wire extension changed value or native carrier");
      for (unsigned length = 0; length <= 16; ++length) {
        if (length == width) continue;
        auto bad = original;
        Check(!api::RestoreStoredScalarPayloadV1(std::string(length, 'x'),
                  api::EngineValueState::value, &bad) &&
                  bad.descriptor == original.descriptor && bad.binary_value == original.binary_value &&
                  bad.encoded_value.empty(), "invalid stored width mutated destination");
      }
    }
    const auto present = Value(type, width, 0, 1);
    for (const auto a : numbers) for (const auto b : numbers)
    for (bool left_nullable : {false,true}) for (bool right_nullable : {false,true}) {
      auto left = Value(type,width,a,1), right = Value(type,width,b,2);
      left.descriptor.encoded_descriptor = left_nullable
          ? "nullability=nullable" : "nullability=non_null";
      right.descriptor.encoded_descriptor = right_nullable
          ? "nullability=nullable" : "nullability=non_null";
      const auto left_before = left, right_before = right;
      int comparison = 42;
      std::string detail;
      Check(api::QowCompareCanonicalNonCollatedScalarsV1(left,right,&comparison,&detail) &&
                comparison == (a < b ? -1 : a > b ? 1 : 0) && detail.empty(),
            "present integer order depends on containing slot nullability");
      Check(left.descriptor == left_before.descriptor && right.descriptor == right_before.descriptor &&
                left.binary_value == left_before.binary_value && right.binary_value == right_before.binary_value,
            "comparison rewrote independently admitted source descriptors or bytes");
      for (bool mutate_left : {false,true}) {
        auto bad_left = left, bad_right = right;
        auto& bad = mutate_left ? bad_left : bad_right;
        ++bad.descriptor.datatype_descriptor_generation;
        Check(!api::QowCompareCanonicalNonCollatedScalarsV1(bad_left,bad_right,&comparison,&detail) &&
                  comparison == 0 && !detail.empty(),
              "mixed-nullability integer comparison skipped independent binding validation");
      }
    }
    for (unsigned cardinality : {0, 1, 2}) {
      for (unsigned mutation = 0; mutation != 7; ++mutation) {
        ex::DescriptorBatch batch;
        ex::ExecutorColumnDescriptor column;
        column.descriptor = present.descriptor;
        column.descriptor_id = 1;
        column.stable_name = "integer";
        column.nullable = true;
        if (mutation == 1) ++column.descriptor.datatype_descriptor_generation;
        if (mutation == 2) column.descriptor.type_uuid = {};
        if (mutation == 3) column.descriptor.descriptor_uuid = {};
        if (mutation == 4) column.descriptor.encoded_descriptor = "nullability=non_null";
        if (mutation == 5) column.descriptor.encoded_descriptor += ";precision=8";
        if (mutation == 6) column.nullable = false;
        batch.columns.push_back(column);
        if (cardinality) {
          auto value = present;
          value.descriptor = column.descriptor;
          if (cardinality == 2) {
            value.binary_value.clear();
            value.setState(api::EngineValueState::sql_null);
          }
          batch.rows.push_back({{value}});
        }
        Check(ex::ValidateDescriptorBatch(batch).ok == (mutation == 0),
              "integer batch admission depends on row cardinality or NULL state");
      }
    }
    auto null = present;
    Check(api::RestoreStoredScalarPayloadV1({}, api::EngineValueState::sql_null, &null) &&
              null.is_null && null.binary_value.empty() && null.encoded_value.empty() &&
              api::StoredScalarPayloadMatchesV1(null, {}, api::EngineValueState::sql_null),
          "containing NULL retained a native integer payload");
    for (auto placement : {ex::CanonicalDescriptorNullPlacement::first,
                           ex::CanonicalDescriptorNullPlacement::last}) {
      for (auto direction : {ex::CanonicalDescriptorOrderDirection::ascending,
                             ex::CanonicalDescriptorOrderDirection::descending}) {
        ex::CanonicalDescriptorOrderTerm term;
        term.expression_descriptor_id = 1;
        term.direction = direction;
        term.null_placement = placement;
        const auto ordered = ex::CompareCanonicalDescriptorOrderValues(null, present, term);
        const auto reverse = ex::CompareCanonicalDescriptorOrderValues(present, null, term);
        const int expected = placement == ex::CanonicalDescriptorNullPlacement::first ? -1 : 1;
        const auto kn = ex::MakeCanonicalDescriptorEqualityKey(null, term);
        const auto kp = ex::MakeCanonicalDescriptorEqualityKey(present, term);
        Check(ordered.diagnostic.ok && reverse.diagnostic.ok && ordered.comparison == expected &&
                  reverse.comparison == -expected && kn.diagnostic.ok && kp.diagnostic.ok &&
                  kn.equality_key != kp.equality_key,
              "native integer NULL ordering changed with direction or aliases zero");
      }
    }
    for (bool mutate_left : {false, true}) for (unsigned mutation = 0; mutation < 14; ++mutation) {
      auto left = Value(type, width, -1, 1), right = Value(type, width, 1, 2);
      auto& bad = mutate_left ? left : right;
      switch (mutation) {
        case 0: bad.binary_value.pop_back(); break;
        case 1: bad.binary_value.push_back(0); break;
        case 2: bad.encoded_value = "1"; break;
        case 3: bad.binary_value.clear(); bad.encoded_value = "1"; break;
        case 4: bad.descriptor.datatype_descriptor_uuid = {}; break;
        case 5: ++bad.descriptor.datatype_descriptor_generation; break;
        case 6: bad.descriptor.type_uuid = {}; break;
        case 7: bad.descriptor.descriptor_uuid = {}; break;
        case 8: bad.descriptor.encoded_descriptor += ";precision=8"; break;
        case 9: bad.descriptor.charset_uuid = bad.descriptor.type_uuid; break;
        case 10: bad.descriptor.collation_uuid = bad.descriptor.type_uuid; break;
        case 11: bad.is_null = true; break;
        case 12: bad.setState(api::EngineValueState::sql_null); break;
        case 13: bad.binary_value.clear(); break;
      }
      int comparison = 42;
      std::string detail;
      Check(!api::QowCompareCanonicalNonCollatedScalarsV1(left, right, &comparison, &detail) &&
                comparison == 0 && !detail.empty(),
            "invalid native integer operand published comparison");
      if (mutation <= 3 || mutation >= 11) {
        std::array<char, 8> widened;
        widened.fill('!');
        const auto sentinel = widened;
        std::string_view payload = "unchanged";
        Check(!scratchbird::engine::PublicSignedIntegerScalarPayloadV1(bad, &widened, &payload) &&
                  widened == sentinel && payload == "unchanged",
              "invalid signed integer wire carrier changed output or was accepted");
      }
    }
  }
  std::cout << "native integer comparison checks=" << checks << '\n';
}
