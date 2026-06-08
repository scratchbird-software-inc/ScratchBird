// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_aggregate_window_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
                                                "aggregate requires a finite numeric input"));
  }
  state->non_null_count += 1;
  state->numeric_sum += numeric;
  const long double count = static_cast<long double>(state->non_null_count);
  const long double delta = numeric - state->numeric_mean;
  state->numeric_mean += delta / count;
  const long double delta2 = numeric - state->numeric_mean;
  state->numeric_m2 += delta * delta2;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult ApplyCorrelationInput(SblrAggregateWindowState* state,
                                 const SblrAggregateUpdateRequest& request) {
  if (request.values.size() < 2) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_CORR_PAIR_REQUIRED",
                                                request.context,
                                                state,
                                                "statistical pair aggregate requires y and x inputs"));
  }
  const SblrValue& y_value = request.values[0];
  const SblrValue& x_value = request.values[1];
  if (y_value.is_null || x_value.is_null) {
    return MakeSblrSuccess(AggregateOperationId(*state));
  }
  long double y = 0.0L;
  long double x = 0.0L;
  if (!ParseLongDouble(y_value, &y) || !ParseLongDouble(x_value, &x)) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED",
                                                request.context,
                                                state,
                                                "statistical pair aggregate requires finite numeric y and x inputs"));
  }

  if (state->aggregate_kind == SblrAggregateFunctionKind::regr_count) {
    state->non_null_count += 1;
    return MakeSblrSuccess(AggregateOperationId(*state));
  }

  const long double next_count = static_cast<long double>(state->non_null_count + 1);
  const long double delta_x = x - state->corr_mean_x;
  state->corr_mean_x += delta_x / next_count;
  const long double delta_y = y - state->corr_mean_y;
  state->corr_mean_y += delta_y / next_count;
  state->corr_m2_x += delta_x * (x - state->corr_mean_x);
  state->corr_m2_y += delta_y * (y - state->corr_mean_y);
  state->corr_comoment += delta_x * (y - state->corr_mean_y);
  state->non_null_count += 1;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult ApplyBoolInput(SblrAggregateWindowState* state,
                          const SblrAggregateUpdateRequest& request,
                          const SblrValue& value) {
  if (value.is_null) return MakeSblrSuccess(AggregateOperationId(*state));
  bool truth = false;
  if (!Truthy(value, &truth)) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_BOOLEAN_INPUT_REQUIRED",
                                                request.context,
                                                state,
                                                "boolean aggregate requires a boolean-compatible input"));
  }
  state->non_null_count += 1;
  if (truth) {
    state->saw_true = true;
  } else {
    state->saw_false = true;
  }
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult ApplyBitInput(SblrAggregateWindowState* state,
                         const SblrAggregateUpdateRequest& request,
                         const SblrValue& value) {
  if (value.is_null) return MakeSblrSuccess(AggregateOperationId(*state));
  std::uint64_t bits = 0;
  if (!ParseUint64(value, &bits)) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_INTEGER_INPUT_REQUIRED",
                                                request.context,
                                                state,
                                                "bit aggregate requires an integer-compatible input"));
  }
  state->non_null_count += 1;
  if (!state->bit_initialized) {
    state->bit_accumulator = bits;
    state->bit_initialized = true;
    return MakeSblrSuccess(AggregateOperationId(*state));
  }
  switch (state->aggregate_kind) {
    case SblrAggregateFunctionKind::bit_and: state->bit_accumulator &= bits; break;
    case SblrAggregateFunctionKind::bit_or: state->bit_accumulator |= bits; break;
    case SblrAggregateFunctionKind::bit_xor: state->bit_accumulator ^= bits; break;
    default: break;
  }
  return MakeSblrSuccess(AggregateOperationId(*state));
}

void ApplyMinMaxInput(SblrAggregateWindowState* state, const SblrValue& value) {
  if (value.is_null) return;
  state->non_null_count += 1;
  if (state->non_null_count == 1) {
    state->min_value = value;
    state->max_value = value;
    return;
  }
  if (CompareValues(value, state->min_value) < 0) state->min_value = value;
  if (CompareValues(value, state->max_value) > 0) state->max_value = value;
}

std::vector<std::uint8_t> BytesFromValue(const SblrValue& value) {
  if (!value.binary_value.empty()) return value.binary_value;
  const std::string text = ValueText(value);
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream stream;
  stream << '"';
  for (const char c : input) {
    switch (c) {
      case '\\': stream << "\\\\"; break;
      case '"': stream << "\\\""; break;
      case '\b': stream << "\\b"; break;
      case '\f': stream << "\\f"; break;
      case '\n': stream << "\\n"; break;
      case '\r': stream << "\\r"; break;
      case '\t': stream << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(static_cast<unsigned char>(c)) << std::dec << std::setfill(' ');
        } else {
          stream << c;
        }
        break;
    }
  }
  stream << '"';
  return stream.str();
}

std::string JsonValueFromSblrValue(const SblrValue& value) {
  if (value.is_null) return "null";
  if (value.payload_kind == SblrValuePayloadKind::boolean) return value.int64_value == 0 ? "false" : "true";
  if (value.has_int64_value || value.has_uint64_value || value.has_real64_value ||
      value.payload_kind == SblrValuePayloadKind::high_precision_numeric_text) {
    return ValueText(value);
  }
  return JsonEscape(ValueText(value));
}

bool IsListAggFunctionId(std::string_view function_id) {
  const std::string id = Lower(function_id);
  return id == "listagg" || id == "data.aggregate.listagg" || id == "sb.aggregate.listagg" ||
         id.find("listagg(") != std::string::npos;
}

std::string JoinTextItems(const std::vector<std::string>& items,
                          std::size_t count,
                          std::string_view separator) {
  std::string out;
  const std::size_t bounded_count = std::min(count, items.size());
  for (std::size_t index = 0; index < bounded_count; ++index) {
    if (index != 0) out += separator;
    out += items[index];
  }
  return out;
}

std::string ListAggTruncationSuffix(const SblrAggregateOptions& options,
                                    std::uint64_t truncated_count) {
  std::string suffix = options.listagg_truncation_indicator.empty()
                           ? "..."
                           : options.listagg_truncation_indicator;
  if (options.listagg_with_count) {
    suffix += "(";
    suffix += std::to_string(truncated_count);
    suffix += ")";
  }
  return suffix;
}

SblrResult ApplyStringAggregateInput(SblrAggregateWindowState* state,
                                     const SblrAggregateUpdateRequest& request,
                                     const SblrValue& value) {
  if (value.is_null) return MakeSblrSuccess(AggregateOperationId(*state));
  const std::string separator = request.values.size() > 1 && !request.values[1].is_null
                                    ? ValueText(request.values[1])
                                    : ",";
  if (state->non_null_count == 0) state->separator = separator;
  const std::string text = ValueText(value);
  if (IsListAggFunctionId(state->function_id)) state->sketch_values.push_back(text);
  if (state->listagg_retained_count != 0) state->text_accumulator += state->separator;
  state->text_accumulator += text;
  state->listagg_retained_count += 1;
  state->non_null_count += 1;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

std::string ListDescriptorForState(const SblrAggregateWindowState& state) {
  if (!state.result_descriptor_id.empty()) return state.result_descriptor_id;
  return "list<any nullable>";
}

std::string ListElementFromValue(const SblrValue& value) {
  if (value.is_null) return "NULL";
  std::string descriptor = value.descriptor_id.empty() ? "any" : value.descriptor_id;
  return descriptor + ":" + ValueText(value);
}

SblrResult ApplyListAggregateInput(SblrAggregateWindowState* state,
                                   const SblrValue& value) {
  state->sketch_values.push_back(ListElementFromValue(value));
  state->listagg_retained_count += 1;
  if (!state->text_accumulator.empty()) state->text_accumulator += ";";
  state->text_accumulator += state->sketch_values.back();
  if (!value.is_null) state->non_null_count += 1;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult ApplyBinaryAggregateInput(SblrAggregateWindowState* state,
                                     const SblrValue& value) {
  if (value.is_null) return MakeSblrSuccess(AggregateOperationId(*state));
  auto bytes = BytesFromValue(value);
  state->binary_accumulator.insert(state->binary_accumulator.end(), bytes.begin(), bytes.end());
  state->non_null_count += 1;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult ApplyJsonArrayInput(SblrAggregateWindowState* state,
                               const SblrValue& value) {
  if (!state->text_accumulator.empty()) state->text_accumulator += ",";
  state->text_accumulator += JsonValueFromSblrValue(value);
  state->non_null_count += 1;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult ApplyJsonObjectInput(SblrAggregateWindowState* state,
                                const SblrAggregateUpdateRequest& request,
                                const SblrValue& key,
                                const SblrValue& value) {
  if (key.is_null) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_JSON_OBJECT_KEY_REQUIRED",
                                                request.context,
                                                state,
                                                "JSON object aggregate key cannot be NULL"));
  }
  const std::string key_text = ValueText(key);
  const auto existing = std::find(state->sketch_values.begin(),
                                  state->sketch_values.end(),
                                  key_text);
  if (existing != state->sketch_values.end()) {
    const auto offset = static_cast<std::size_t>(existing - state->sketch_values.begin());
    state->sketch_values.erase(existing);
    if (offset < state->distinct_values.size()) {
      state->distinct_values.erase(state->distinct_values.begin() + static_cast<std::ptrdiff_t>(offset));
    }
  }
  state->sketch_values.push_back(key_text);
  state->distinct_values.push_back(JsonEscape(key_text) + ":" + JsonValueFromSblrValue(value));
  state->text_accumulator.clear();
  for (std::size_t index = 0; index < state->distinct_values.size(); ++index) {
    if (index != 0) state->text_accumulator += ",";
    state->text_accumulator += state->distinct_values[index];
  }
  state->non_null_count += 1;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

void AddDistinctValue(SblrAggregateWindowState* state, std::string value) {
  if (std::find(state->distinct_values.begin(), state->distinct_values.end(), value) == state->distinct_values.end()) {
    state->distinct_values.push_back(std::move(value));
  }
}

void AddFrequencyValue(SblrAggregateWindowState* state, std::string value) {
  for (std::size_t index = 0; index < state->sketch_values.size(); ++index) {
    if (state->sketch_values[index] == value) {
      state->sketch_counts[index] += 1;
      return;
    }
  }
  state->sketch_values.push_back(std::move(value));
  state->sketch_counts.push_back(1);
}

SblrValue TextResultValue(const SblrAggregateWindowState& state, std::string value, std::string fallback_descriptor) {
  SblrValue out;
  out.descriptor_id = state.result_descriptor_id.empty() ? std::move(fallback_descriptor) : state.result_descriptor_id;
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::text;
  out.encoded_value = std::move(value);
  out.text_value = out.encoded_value;
  return out;
}

SblrResult ScalarResult(const SblrAggregateWindowState& state, SblrValue value);

SblrResult FinalizeListAggText(const SblrAggregateWindowState& state,
                               const SblrAggregateFinalizeRequest& request) {
  const std::string separator = state.separator.empty() ? "," : state.separator;
  const std::string full_text = state.sketch_values.empty()
                                    ? state.text_accumulator
                                    : JoinTextItems(state.sketch_values, state.sketch_values.size(), separator);
  const auto max_output_bytes = request.options.listagg_max_output_bytes;
  const auto mode = request.options.listagg_overflow_mode;
  if (mode == SblrListAggOverflowMode::none || max_output_bytes == 0 ||
      full_text.size() <= max_output_bytes) {
    return ScalarResult(state, TextResultValue(state, full_text, "text"));
  }

  if (mode == SblrListAggOverflowMode::error) {
    auto diagnostic = AggregateDiagnostic("SB_DIAG_AGGREGATE_LISTAGG_OVERFLOW",
                                          request.context,
                                          &state,
                                          "LISTAGG result exceeded ON OVERFLOW ERROR byte limit");
    diagnostic.fields.push_back({"overflow_policy", "ERROR"});
    diagnostic.fields.push_back({"max_output_bytes", std::to_string(max_output_bytes)});
    diagnostic.fields.push_back({"actual_output_bytes", std::to_string(full_text.size())});
    return AggregateFailure(SblrStatusCode::execution_failed, AggregateOperationId(state), std::move(diagnostic));
  }

  if (mode != SblrListAggOverflowMode::truncate) {
    return ScalarResult(state, TextResultValue(state, full_text, "text"));
  }

  if (state.sketch_values.empty()) {
    auto diagnostic = AggregateDiagnostic("SB_DIAG_AGGREGATE_LISTAGG_BOUNDARIES_REQUIRED",
                                          request.context,
                                          &state,
                                          "LISTAGG ON OVERFLOW TRUNCATE requires exact element boundaries");
    diagnostic.fields.push_back({"overflow_policy", "TRUNCATE"});
    return AggregateFailure(SblrStatusCode::execution_failed, AggregateOperationId(state), std::move(diagnostic));
  }

  for (std::size_t retained = state.sketch_values.size(); retained > 0; --retained) {
    const std::uint64_t truncated_count =
        static_cast<std::uint64_t>(state.sketch_values.size() - retained);
    if (truncated_count == 0) continue;
    std::string candidate = JoinTextItems(state.sketch_values, retained, separator);
    candidate += separator;
    candidate += ListAggTruncationSuffix(request.options, truncated_count);
    if (candidate.size() <= max_output_bytes) {
      return ScalarResult(state, TextResultValue(state, std::move(candidate), "text"));
    }
  }

  const std::uint64_t truncated_count = static_cast<std::uint64_t>(state.sketch_values.size());
  std::string suffix_only = ListAggTruncationSuffix(request.options, truncated_count);
  if (suffix_only.size() <= max_output_bytes) {
    return ScalarResult(state, TextResultValue(state, std::move(suffix_only), "text"));
  }

  auto diagnostic = AggregateDiagnostic("SB_DIAG_AGGREGATE_LISTAGG_TRUNCATION_INDICATOR_TOO_LARGE",
                                        request.context,
                                        &state,
                                        "LISTAGG truncation marker cannot fit inside the configured byte limit");
  diagnostic.fields.push_back({"overflow_policy", "TRUNCATE"});
  diagnostic.fields.push_back({"max_output_bytes", std::to_string(max_output_bytes)});
  diagnostic.fields.push_back({"marker_bytes", std::to_string(suffix_only.size())});
  return AggregateFailure(SblrStatusCode::execution_failed, AggregateOperationId(state), std::move(diagnostic));
}

SblrValue BinaryResultValue(const SblrAggregateWindowState& state) {
  SblrValue out;
  out.descriptor_id = state.result_descriptor_id.empty() ? "binary" : state.result_descriptor_id;
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::binary;
  out.binary_value = state.binary_accumulator;
  return out;
}

SblrValue ListResultValue(const SblrAggregateWindowState& state) {
  SblrValue out;
  out.descriptor_id = ListDescriptorForState(state);
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::descriptor_payload;
  out.encoded_value = "list[" + state.text_accumulator + "]";
  out.text_value = out.encoded_value;
  return out;
}

SblrValue JsonArrayResult(const SblrAggregateWindowState& state) {
  return TextResultValue(state, "[" + state.text_accumulator + "]", "json");
}

SblrValue JsonObjectResult(const SblrAggregateWindowState& state) {
  return TextResultValue(state, "{" + state.text_accumulator + "}", "json");
}

SblrValue TopKResult(const SblrAggregateWindowState& state) {
  std::vector<std::size_t> order(state.sketch_values.size());
  for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
  std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
    if (state.sketch_counts[left] != state.sketch_counts[right]) return state.sketch_counts[left] > state.sketch_counts[right];
    return state.sketch_values[left] < state.sketch_values[right];
  });
  std::ostringstream stream;
