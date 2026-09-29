// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crud_support/native_value_payload.hpp"
#include "crud_support/retained_row_value_codec.hpp"
#include "crud_support/composite_logical_key.hpp"
#include "engine/optimizer/bulk_placement_order.hpp"
#include <iostream>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace {
void Require(bool condition, const char* detail) {
  if (!condition) throw std::runtime_error(detail);
}
template<class F> void Refuses(F run, const char* detail) {
  bool refused = false;
  try { run(); } catch (const std::invalid_argument&) { refused = true; }
  Require(refused, detail);
}
// Independent frame encoder, deliberately not using the production primitives.
void U32(std::string& out, std::size_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 255));
}
void Blob(std::string& out, const std::string& value) {
  U32(out, value.size());
  out += value;
}
std::string ValuesOracle(const api::CrudValueFields& fields) {
  std::string out = "SBVALS01";
  U32(out, fields.size());
  for (const auto& [name, value] : fields) {
    Blob(out, name);
    out.push_back(static_cast<char>(value.state));
    Blob(out, value.bytes);
  }
  return out;
}
void CheckValues(const api::CrudValueFields& fields) {
  const auto expected = ValuesOracle(fields);
  Require(api::EncodeCrudValues(fields) == expected, "value frame differs from independent oracle");
  const auto decoded = api::DecodeCrudValues(expected);
  Require(decoded && *decoded == fields, "value state or payload lost on decode");
  for (std::size_t width = 0; width < expected.size(); ++width)
    Require(!api::DecodeCrudValues(std::string_view(expected).substr(0, width)), "truncated frame accepted");
  Require(!api::DecodeCrudValues(expected + '\0'), "trailing bytes accepted");
}
}

int main() {
  try {
    std::string all_bytes;
    for (unsigned byte = 0; byte < 256; ++byte) all_bytes.push_back(static_cast<char>(byte));
    const api::CrudValueFields fields = {
        {"null_spelling", "<NULL>"}, {"default_spelling", "<DEFAULT>"},
        {"empty", ""}, {"binary", all_bytes}, {"nil_uuid", std::string(16, '\0')},
        {"null", api::CrudStoredValue::SqlNull()}, {"absent", api::CrudStoredValue::Missing()},
        {"default", api::CrudStoredValue::DefaultRequested()}};
    CheckValues({});
    CheckValues(fields);
    for (unsigned state = 0; state < 256; ++state) {
      const auto tag = static_cast<api::EngineValueState>(state);
      api::CrudValueFields candidate{{"x", {tag, {}}}};
      if (state <= 7) CheckValues(candidate);
      else {
        Require(!api::DecodeCrudValues(ValuesOracle(candidate)), "unknown state decoded");
        Refuses([&] { api::EncodeCrudValues(candidate); }, "unknown state encoded");
      }
      candidate[0].second.bytes = all_bytes;
      const bool has_payload = state == 0 || state == 5 || state == 6 || state == 7;
      if (has_payload) CheckValues(candidate);
      else {
        Require(!api::DecodeCrudValues(ValuesOracle(candidate)), "payload-free state accepted bytes");
        Refuses([&] { api::EncodeCrudValues(candidate); }, "payload-free state serialized bytes");
      }
    }
    const api::CrudValueFields duplicate{{"x", "a"}, {"x", "b"}};
    Require(!api::DecodeCrudValues(ValuesOracle(duplicate)), "duplicate field accepted");
    Refuses([&] { api::EncodeCrudValues(duplicate); }, "duplicate field encoded");

    for (const auto& [name, stored] : fields) {
      api::EngineTypedValue typed;
      typed.descriptor.canonical_type_name = "binary";
      typed.setState(stored.state);
      typed.binary_value.assign(stored.bytes.begin(), stored.bytes.end());
      Require(api::CrudTypedValuePayload(typed) == stored, "typed adapter changed value state or bytes");
      if (stored.isPresent()) {
        const auto compared = api::CompareCrudPredicateValue(stored, typed,
            [](const auto& a, const auto& b) { return a.compare(b); });
        Require(compared && *compared == 0, "present binary comparison lost value");
      } else Require(!api::CrudPredicateValuePayload(typed), "unresolved/NULL predicate admitted");
    }
    api::EngineTypedValue null_with_payload;
    null_with_payload.setState(api::EngineValueState::sql_null);
    null_with_payload.encoded_value = "<NULL>";
    Refuses([&] { api::CrudTypedValuePayload(null_with_payload); }, "NULL carrying bytes accepted");

    // Every scalar key is framed, so even bytes spelling an entire NULL frame
    // are unambiguously a present value. Compound components retain state too.
    const auto null_key = api::EncodeStoredLogicalKey({api::CrudStoredValue::SqlNull()});
    for (const auto& value : std::vector<api::CrudStoredValue>{"", "<NULL>", "<DEFAULT>", null_key, all_bytes}) {
      const std::vector<api::CrudStoredValue> components{value, api::CrudStoredValue::SqlNull(), value};
      std::string expected = "SBCLKEY2";
      U32(expected, components.size());
      for (const auto& cell : components) {
        expected.push_back(static_cast<char>(cell.state)); Blob(expected, cell.bytes);
      }
      Require(api::EncodeStoredLogicalKey(components) == expected, "key frame differs from oracle");
      const auto decoded = api::DecodeStoredLogicalKey(expected, 3);
      Require(decoded && *decoded == components, "compound key lost NULL/payload distinction");
      Require(api::EncodeStoredLogicalKey({value}) != null_key, "present key collided with NULL");
      Require(api::StoredLogicalKeyHasNull(expected) == true, "compound NULL key lost NULL evidence");
      Require(api::StoredLogicalKeyHasNull(api::EncodeStoredLogicalKey({value})) == false,
              "present marker key acquired NULL evidence");
      for (std::size_t width = 0; width < expected.size(); ++width)
        Require(!api::DecodeStoredLogicalKey(std::string_view(expected).substr(0, width), 3), "truncated key accepted");
      Require(!api::DecodeStoredLogicalKey(expected, 2), "wrong key arity accepted");
    }
    for (const auto state : {api::EngineValueState::missing, api::EngineValueState::default_requested,
                             api::EngineValueState::unknown, api::EngineValueState::error,
                             api::EngineValueState::lob_handle, api::EngineValueState::protected_value})
      Refuses([&] { api::EncodeStoredLogicalKey({{state, {}}}); }, "unresolved logical key encoded");
    scratchbird::engine::optimizer::BulkPlacementOrderRequest placement;
    placement.ordered_ingest_requested = true;
    placement.placement_key_column = "k";
    placement.rows = {{0, {}, "<NULL>"}, {1, {}, ""}, {2, {}, "", true, true}, {3, {}, ""}};
    const auto ordered = scratchbird::engine::optimizer::PlanBulkPlacementOrder(placement);
    Require(ordered.ok && ordered.source_ordinals_in_apply_order == std::vector<std::uint64_t>{2, 1, 3, 0} &&
            ordered.placement_key_run_count == 3, "placement order conflated NULL, empty or marker data");
    placement.rows[1].placement_key_present = false;
    Require(!scratchbird::engine::optimizer::PlanBulkPlacementOrder(placement).ok,
            "absent placement key was treated as empty data");
    placement.rows[1].placement_key_present = true;
    placement.rows[2].placement_key = "payload";
    Require(!scratchbird::engine::optimizer::PlanBulkPlacementOrder(placement).ok,
            "NULL placement key accepted payload");
    std::cout << "PASS retained value states, independent frames, malformed inputs and NULL-safe key identity\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n'; return 1;
  }
}
