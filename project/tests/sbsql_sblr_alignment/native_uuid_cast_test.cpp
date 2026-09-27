// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_seed_registry.hpp"
#include "dispatch/function_dispatch.hpp"
#include "common/function_result_helpers.hpp"
#include "sblr/sblr_special_forms.hpp"
#include "sblr/sblr_operator_runtime.hpp"
#include "sblr/sblr_projection_value_runtime.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace s = scratchbird::engine::sblr;
namespace f = scratchbird::engine::functions;
void Check(bool pass, const char* message) {
  if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}
s::SblrResult CallValues(const f::FunctionRegistry& registry, const char* name,
                         std::vector<s::SblrValue> values) {
  const auto* entry = registry.Lookup(name);
  Check(entry != nullptr, "cast absent from actual callable registry");
  f::FunctionCallRequest request;
  request.context.function_uuid = entry->function_uuid;
  request.context.security_allowed = request.context.policy_allowed = true;
  Check(registry.BindCallContext(request.context) != nullptr, "binary callable binding failed");
  for (auto& value : values) request.arguments.push_back({"arg", std::move(value)});
  return f::DispatchFunctionCall(registry, std::move(request)).result;
}
s::SblrResult Call(const f::FunctionRegistry& registry, const char* name,
                   const s::SblrValue& value, const char* target) {
  return CallValues(registry, name, {value, f::MakeTextValue("text", target)});
}
const s::SblrValue& Scalar(const s::SblrResult& result) {
  Check(result.ok() && result.scalar_values.size() == 1 && result.rows.empty(), "cast did not produce one actual scalar");
  return result.scalar_values.front();
}
void Published(const s::SblrValue& value, const std::vector<std::uint8_t>& expected,
               const char* type) {
  Check(value.descriptor_id == type && s::ProjectionSblrValueResolved(value), "native cast representation unresolved");
  Check(value.encoded_value.empty() && value.text_value.empty(), "native cast leaked UUID/binary through text");
  const auto published = s::EngineTypedValueFromSblrValue(value);
  Check(!published.is_null && published.binary_value == expected && published.encoded_value.empty(),
        "publication lost or reformatted exact 128 data bits");
}
void Truth(const s::SblrResult& result, bool expected) {
  const auto& value = Scalar(result);
  Check(!value.is_null && value.has_int64_value && value.int64_value == (expected ? 1 : 0),
        "native predicate returned the wrong SQL truth value");
}
void NativeComparisons() {
  std::vector<s::SblrUuid> ids(130);
  ids[1].bytes.fill(0xff);
  for (unsigned n = 2; n < ids.size(); ++n) ids[n].bytes[(n - 2) / 8] = std::uint8_t(1u << ((n - 2) % 8));
  for (const auto& lhs : ids) for (const auto& rhs : ids) {
    int order = 0;
    for (unsigned n = 0; n < 16; ++n) {
      if (lhs.bytes[n] == rhs.bytes[n]) continue;
      order = lhs.bytes[n] < rhs.bytes[n] ? -1 : 1;
      break;
    }
    const auto left = s::MakeSblrUuidValue(lhs), right = s::MakeSblrUuidValue(rhs);
    for (const auto& [operation, truth] : {
        std::pair{"op_eq", order == 0}, {"op_ne", order != 0},
        {"op_lt", order < 0}, {"op_le", order <= 0},
        {"op_gt", order > 0}, {"op_ge", order >= 0}, {"op_is_distinct", order != 0}})
      Truth(s::EvaluateSblrComparison(operation, left, right, {}), truth);
    Truth(s::EvaluateSblrInListForm("in", left, {right}), order == 0);
    const auto nullif = s::EvaluateSblrNullIfForm("nullif", left, right);
    Check(Scalar(nullif).is_null == (order == 0), "NULLIF compared empty UUID text fields");
    if (order != 0) Check(Scalar(nullif).uuid_value == lhs, "NULLIF changed retained UUID data");
    Truth(s::EvaluateSblrBetweenForm("between", left, right, right), order == 0);
  }
  const auto native = s::MakeSblrUuidValue(ids[2]), other = s::MakeSblrUuidValue(ids[3]);
  const auto null = f::MakeNullValue("uuid");
  Check(Scalar(s::EvaluateSblrComparison("op_eq", native, null, {})).is_null, "UUID = NULL is not UNKNOWN");
  Truth(s::EvaluateSblrComparison("op_is_distinct", native, null, {}), true);
  Truth(s::EvaluateSblrComparison("op_is_distinct", null, null, {}), false);
  Truth(s::EvaluateSblrComparison("op_is_null", native, other, {}), false);
  Truth(s::EvaluateSblrComparison("op_is_null", null, other, {}), true);
  Check(Scalar(s::EvaluateSblrInListForm("in", native, {other, null})).is_null, "IN lost UNKNOWN on an unmatched NULL candidate");
  Truth(s::EvaluateSblrInListForm("in", native, {other, null, native}), true);
  Check(Scalar(s::EvaluateSblrBetweenForm("between", native, null, native)).is_null, "BETWEEN lost NULL semantics");
  unsigned probes = 0;
  const auto thunk = [&](s::SblrValue value) { return [&, value] {
    ++probes; auto result = s::MakeSblrSuccess("thunk"); result.scalar_values.push_back(value); return result;
  }; };
  Truth(s::EvaluateSblrInListFormLazy("in", thunk(native),
      {thunk(other), thunk(native), []() -> s::SblrResult { throw std::logic_error("IN evaluated past its matching candidate"); }}, {}), true);
  Check(probes == 3, "lazy IN skipped or repeated an evaluated expression");
  for (const auto& bad : {f::MakeTextValue("text", ""), f::MakeBinaryValue("binary", std::vector<std::uint8_t>(16))}) {
    Check(!s::EvaluateSblrComparison("op_eq", native, bad, {}).ok(), "UUID implicitly compared to text/binary");
    Check(!s::EvaluateSblrNullIfForm("nullif", native, bad).ok(), "NULLIF implicitly compared UUID to text/binary");
    Check(!s::EvaluateSblrInListForm("in", native, {bad}).ok(), "IN implicitly compared UUID to text/binary");
    Check(!s::EvaluateSblrBetweenForm("between", native, bad, native).ok(), "BETWEEN implicitly compared UUID to text/binary");
  }
  const auto low = f::MakeBinaryValue("binary", {0x7f}), high = f::MakeBinaryValue("binary", {0x80});
  Truth(s::EvaluateSblrComparison("op_lt", low, high, {}), true);
  Truth(s::EvaluateSblrInListForm("in", low, {high}), false);
  Check(!Scalar(s::EvaluateSblrNullIfForm("nullif", low, high)).is_null, "NULLIF compared empty binary text fields");
}
int main() {
  NativeComparisons();
  const auto package = f::BuildStandardFunctionSeedPackage();
  const char* functions[] = {"data.scalar.cast", "sb.scalar.safe_cast", "sb.scalar.try_cast"};
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    s::SblrUuid id;
    if (pattern == 1) id.bytes.fill(0xff);
    if (pattern >= 2) id.bytes[(pattern - 2) / 8] = std::uint8_t(1u << ((pattern - 2) % 8));
    const std::vector<std::uint8_t> bytes(id.bytes.begin(), id.bytes.end());
    const auto uuid = s::MakeSblrUuidValue(id);
    const auto binary = f::MakeBinaryValue("binary", bytes);
    Published(Scalar(s::EvaluateSblrCastForm("cast", uuid, "uuid", {}, false, false)), bytes, "uuid");
    Published(Scalar(s::EvaluateSblrCastForm("cast", uuid, "binary", {}, true, false)), bytes, "binary");
    Published(Scalar(s::EvaluateSblrCastForm("cast", binary, "uuid", {}, true, false)), bytes, "uuid");
    Check(!s::EvaluateSblrCastForm("cast", uuid, "binary", {}, false, false).ok(), "implicit UUID/binary cast bypassed permission");
    Check(!s::EvaluateSblrCastForm("cast", binary, "uuid", {}, false, false).ok(), "implicit binary/UUID cast bypassed permission");
    for (const auto* function : functions) {
      Published(Scalar(Call(package.registry, function, uuid, "uuid")), bytes, "uuid");
      Published(Scalar(Call(package.registry, function, uuid, "binary")), bytes, "binary");
      Published(Scalar(Call(package.registry, function, binary, "uuid")), bytes, "uuid");
      Published(Scalar(Call(package.registry, function, uuid, "varbinary")), bytes, "varbinary");
    }
    const auto nil = s::MakeSblrUuidValue({});
    Published(Scalar(CallValues(package.registry, "sb.scalar.greatest", {nil, uuid})), bytes, "uuid");
    Published(Scalar(CallValues(package.registry, "sb.scalar.least", {uuid, nil})), std::vector<std::uint8_t>(16), "uuid");
    const auto nullif = CallValues(package.registry, "sb.scalar.nullif", {uuid, nil});
    Check(Scalar(nullif).is_null == (pattern == 0), "registered NULLIF compared empty UUID text fields");
    if (pattern != 0) Published(Scalar(nullif), bytes, "uuid");
  }
  const auto null = f::MakeNullValue("uuid");
  for (const auto* function : functions) {
    for (const auto* target : {"uuid", "binary", "varbinary"}) {
      const auto result = Call(package.registry, function, null, target);
      const auto& value = Scalar(result);
      Check(value.descriptor_id == target && s::SblrNullPayloadEmpty(value), "cast NULL acquired payload or lost target");
    }
  }
  s::SblrUuid id; id.bytes.fill(0x55);
  const auto native = s::MakeSblrUuidValue(id);
  for (const auto* function : functions)
    Check(!Call(package.registry, function, native, "not_a_type").ok(), "safe cast manufactured a NULL with an unknown target type");
  for (unsigned gate = 0; gate < 3; ++gate) {
    f::FunctionCallRequest request;
    request.context.function_uuid = package.registry.Lookup(functions[0])->function_uuid;
    request.context.security_allowed = gate != 0;
    request.context.policy_allowed = gate != 1;
    request.context.dependency_available = gate != 2;
    request.arguments = {{"value", native}, {"target", f::MakeTextValue("text", "uuid")}};
    const auto result = f::DispatchFunctionCall(package.registry, std::move(request));
    Check(!result.result.ok() && result.result.scalar_values.empty(), "native cast bypassed a function admission gate");
  }
  const auto text = f::MakeTextValue("text", "019f1122-3344-7566-8788-99aabbccddee");
  Check(!Call(package.registry, functions[0], text, "uuid").ok(), "engine CAST parsed UUID text");
  Check(!Call(package.registry, functions[0], native, "text").ok(), "engine CAST formatted UUID text");
  for (const auto* function : {functions[1], functions[2]}) {
    Check(s::SblrNullPayloadEmpty(Scalar(Call(package.registry, function, text, "uuid"))), "safe text conversion produced UUID data");
    Check(s::SblrNullPayloadEmpty(Scalar(Call(package.registry, function, native, "text"))), "safe UUID conversion produced text");
  }
  for (unsigned length : {0u, 1u, 15u, 17u, 36u}) {
    const auto binary = f::MakeBinaryValue("binary", std::vector<std::uint8_t>(length, 0x55));
    Check(!Call(package.registry, functions[0], binary, "uuid").ok(), "wrong UUID length cast successfully");
    for (const auto* function : {functions[1], functions[2]})
      Check(s::SblrNullPayloadEmpty(Scalar(Call(package.registry, function, binary, "uuid"))), "safe invalid-length cast published data");
  }
  for (unsigned fault = 0; fault < 11; ++fault) {
    auto value = native;
    if (fault == 0) value.text_value = "hidden";
    if (fault == 1) value.encoded_value = std::string(16, '\0');
    if (fault == 2) value.binary_value = {0};
    if (fault == 3) value.uuid_array_value = {id};
    if (fault == 4) value.charset_name = "UTF8";
    if (fault == 5) value.collation_name = "binary";
    if (fault == 6) value.has_int64_value = true;
    if (fault == 7) value.has_uint64_value = true;
    if (fault == 8) value.has_real64_value = true;
    if (fault == 9) value.payload_kind = s::SblrValuePayloadKind::uuid_text;
    if (fault == 10) value.is_null = true;
    Check(!s::ProjectionSblrValueResolved(value), "dirty UUID considered resolved");
    bool refused = false;
    try { (void)s::EngineTypedValueFromSblrValue(value); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "dirty UUID escaped projection publication");
    Check(!s::EvaluateSblrCastForm("cast", value, "uuid", {}, true, false).ok(), "dirty UUID cast source accepted");
    Check(!s::EvaluateSblrComparison("op_eq", value, native, {}).ok(), "dirty UUID comparison accepted");
    Check(!s::EvaluateSblrNullIfForm("nullif", value, native).ok(), "dirty UUID NULLIF accepted");
    Check(!s::EvaluateSblrInListForm("in", native, {value}).ok(), "dirty UUID IN candidate accepted");
    Check(!s::EvaluateSblrBetweenForm("between", native, value, native).ok(), "dirty UUID BETWEEN bound accepted");
    for (const auto* function : functions)
      Check(!Call(package.registry, function, value, "uuid").ok(), "safe or ordinary cast erased malformed representation");
    for (const auto* function : {"sb.scalar.nullif", "sb.scalar.greatest", "sb.scalar.least"})
      Check(!CallValues(package.registry, function, {value, native}).ok(), "registered selection accepted malformed UUID carrier");
  }
  bool refused = false;
  try { (void)f::MakeTextValue("uuid", "019f1122-3344-7566-8788-99aabbccddee"); }
  catch (const std::invalid_argument&) { refused = true; }
  Check(refused, "text factory still manufactures engine UUIDs");
  std::cout << "native UUID casts preserve all 128 bits through function and projection publication\n";
}
