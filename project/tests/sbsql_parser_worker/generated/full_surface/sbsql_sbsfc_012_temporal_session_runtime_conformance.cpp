// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

scratchbird::engine::sblr::SblrResult Run(const FunctionRegistry& registry,
                                          std::string function_id,
                                          std::vector<SblrValue> values = {}) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.statement_timestamp = "2026-05-12T14:23:45Z";
  request.context.sblr_context.transaction_timestamp = "2026-05-12T13:00:00Z";
  request.context.sblr_context.current_timestamp = "2026-05-12T14:23:46Z";
  request.context.sblr_context.user_uuid = "019e2f00-0000-7000-8000-000000000001";
  request.context.sblr_context.database_uuid = "019e2f00-0000-7000-8000-000000000002";
  request.context.sblr_context.current_schema_uuid = "019e2f00-0000-7000-8000-000000000003";
  request.context.sblr_context.deterministic_uuid_text =
      "550e8400-e29b-41d4-a716-446655440000";
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

bool HasDiagnosticDetail(const scratchbird::engine::sblr::SblrResult& result,
                         std::string_view id,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id && diagnostic.detail == detail) return true;
  }
  return false;
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected one successful scalar result\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.descriptor_id != "int64" ||
      value.int64_value != expected) {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << ", got "
              << value.descriptor_id << "\n";
    return false;
  }
  return true;
}

bool ExpectInvalidArity(std::string_view case_id, const scratchbird::engine::sblr::SblrResult& result) {
  if (result.status == SblrStatusCode::ok || !HasDiagnostic(result, "SB_DIAG_FUNCTION_INVALID_INPUT")) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_INVALID_INPUT\n";
    return false;
  }
  return true;
}

bool ExpectFailureDetail(std::string_view case_id,
                         const scratchbird::engine::sblr::SblrResult& result,
                         std::string_view detail) {
  if (result.status == SblrStatusCode::ok ||
      !HasDiagnosticDetail(result, "SB_DIAG_FUNCTION_INVALID_INPUT", detail)) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_INVALID_INPUT detail " << detail << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("statement_timestamp",
                  Run(registry, "sb.temporal.statement_timestamp"),
                  "2026-05-12T14:23:45Z",
                  "timestamp_tz") && ok;
  ok = ExpectText("transaction_timestamp",
                  Run(registry, "sb.temporal.transaction_timestamp"),
                  "2026-05-12T13:00:00Z",
                  "timestamp_tz") && ok;
  ok = ExpectText("clock_timestamp",
                  Run(registry, "sb.temporal.clock_timestamp"),
                  "2026-05-12T14:23:46Z",
                  "timestamp_tz") && ok;
  ok = ExpectText("timeofday",
                  Run(registry, "sb.temporal.timeofday"),
                  "2026-05-12T14:23:46Z",
                  "character") && ok;
  ok = ExpectText("localtime",
                  Run(registry, "sb.temporal.localtime"),
                  "14:23:46",
                  "time") && ok;
  ok = ExpectText("localtimestamp",
                  Run(registry, "sb.temporal.localtimestamp"),
                  "2026-05-12T14:23:46",
                  "timestamp") && ok;
  ok = ExpectText("sb_temporal_now_wrapper",
                  Run(registry, "sb.temporal.now"),
                  "2026-05-12T14:23:46Z",
                  "timestamp_tz") && ok;
  ok = ExpectText("sb_temporal_current_timestamp_wrapper",
                  Run(registry, "sb.temporal.current_timestamp"),
                  "2026-05-12T14:23:46Z",
                  "timestamp_tz") && ok;
  ok = ExpectText("sb_temporal_current_date_wrapper",
                  Run(registry, "sb.temporal.current_date"),
                  "2026-05-12",
                  "date") && ok;
  ok = ExpectText("sb_temporal_current_time_wrapper",
                  Run(registry, "sb.temporal.current_time"),
                  "14:23:46",
                  "time") && ok;
  ok = ExpectText("qualified_current_user",
                  Run(registry, "sb.session.current_user"),
                  "019e2f00-0000-7000-8000-000000000001",
                  "uuid") && ok;
  ok = ExpectText("qualified_current_catalog",
                  Run(registry, "sb.session.current_catalog"),
                  "019e2f00-0000-7000-8000-000000000002",
                  "uuid") && ok;
  ok = ExpectText("qualified_current_schema",
                  Run(registry, "sb.session.current_schema"),
                  "019e2f00-0000-7000-8000-000000000003",
                  "uuid") && ok;
  ok = ExpectText("uuid_v1_provider",
                  Run(registry, "sb.uuid.v1"),
                  "550e8400-e29b-41d4-a716-446655440000",
                  "uuid") && ok;
  ok = ExpectText("uuid_v4_provider",
                  Run(registry, "sb.uuid.v4"),
                  "550e8400-e29b-41d4-a716-446655440000",
                  "uuid") && ok;
  ok = ExpectText("uuid_v7_provider",
                  Run(registry, "sb.uuid.v7"),
                  "550e8400-e29b-41d4-a716-446655440000",
                  "uuid") && ok;

  ok = ExpectInt64("dow_date_signature",
                   Run(registry, "sb.temporal.dow", {TextValue("date", "2026-05-11")}),
                   1) && ok;
  ok = ExpectInt64("dow_bare_alias",
                   Run(registry, "sb.temporal.dow", {TextValue("date", "2026-05-11")}),
                   1) && ok;
  ok = ExpectInt64("doy_date_signature",
                   Run(registry, "sb.temporal.doy", {TextValue("date", "2026-05-11")}),
                   131) && ok;
  ok = ExpectInt64("doy_bare_alias",
                   Run(registry, "sb.temporal.doy", {TextValue("date", "2026-05-11")}),
                   131) && ok;
  ok = ExpectInt64("quarter_date_signature",
                   Run(registry, "sb.temporal.quarter", {TextValue("date", "2026-05-11")}),
                   2) && ok;
  ok = ExpectInt64("quarter_bare_alias",
                   Run(registry, "sb.temporal.quarter", {TextValue("date", "2026-05-11")}),
                   2) && ok;
  ok = ExpectInt64("isodow_date_signature",
                   Run(registry, "sb.temporal.isodow", {TextValue("date", "2026-05-11")}),
                   1) && ok;
  ok = ExpectInt64("isodow_bare_alias",
                   Run(registry, "sb.temporal.isodow", {TextValue("date", "2026-05-11")}),
                   1) && ok;
  ok = ExpectInt64("week_date_signature",
                   Run(registry, "sb.temporal.week", {TextValue("date", "2026-05-11")}),
                   20) && ok;
  ok = ExpectInt64("week_bare_alias",
                   Run(registry, "sb.temporal.week", {TextValue("date", "2026-05-11")}),
                   20) && ok;
  ok = ExpectNull("week_null",
                  Run(registry, "sb.temporal.week", {NullValue("date")}),
                  "int64") && ok;
  ok = ExpectText("add_months_bare_alias",
                  Run(registry, "sb.temporal.add_months", {
                      TextValue("date", "2026-05-11"),
                      Int64Value(3),
                  }),
                  "2026-08-11",
                  "date") && ok;
  ok = ExpectText("add_months_signature_clamp",
                  Run(registry, "sb.temporal.add_months", {
                      TextValue("date", "2026-01-31"),
                      Int64Value(1),
                  }),
                  "2026-02-28",
                  "date") && ok;
  ok = ExpectNull("add_months_null",
                  Run(registry, "sb.temporal.add_months", {
                      NullValue("date"),
                      Int64Value(1),
                  }),
                  "date") && ok;
  ok = ExpectText("last_day_bare_alias",
                  Run(registry, "sb.temporal.last_day", {TextValue("date", "2026-05-11")}),
                  "2026-05-31",
                  "date") && ok;
  ok = ExpectText("last_day_signature_leap",
                  Run(registry, "sb.temporal.last_day", {TextValue("date", "2024-02-15")}),
                  "2024-02-29",
                  "date") && ok;
  ok = ExpectNull("last_day_null",
                  Run(registry, "sb.temporal.last_day", {NullValue("date")}),
                  "date") && ok;

  ok = ExpectInt64("date_part_timestamp_year",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "year"),
                       TextValue("timestamp", "2026-05-11T14:23:45"),
                   }),
                   2026) && ok;
  ok = ExpectInt64("date_part_bare_alias_year",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "year"),
                       TextValue("timestamp", "2026-05-11T14:23:45"),
                   }),
                   2026) && ok;
  ok = ExpectInt64("qualified_date_part_alias_year",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "year"),
                       TextValue("timestamp", "2026-05-11T14:23:45"),
                   }),
                   2026) && ok;
  ok = ExpectInt64("date_part_timestamp_epoch",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "epoch"),
                       TextValue("timestamp", "2026-05-11T14:23:45"),
                   }),
                   1778509425) && ok;
  ok = ExpectInt64("SBSQL-A71E6FCE948C SBSFC012-extract-generic-year",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "year"),
                       TextValue("timestamp", "2026-05-11T14:23:45"),
                   }),
                   2026) && ok;
  ok = ExpectInt64("SBSQL-FAD1979C420A SBSFC012-extract-part-temporal",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "month"),
                       TextValue("timestamp", "2026-05-11T14:23:45"),
                   }),
                   5) && ok;
  ok = ExpectInt64("SBSQL-35DD945088A4 SBSFC012-special-extract-year",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "year"),
                       TextValue("timestamp", "2026-05-11T14:23:45"),
                   }),
                   2026) && ok;
  ok = ExpectInt64("date_part_interval_hour",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "hour"),
                       TextValue("interval", "P1Y2M3DT4H5M6S"),
                   }),
                   4) && ok;
  ok = ExpectInt64("date_part_interval_epoch_day_time",
                   Run(registry, "sb.temporal.date_part", {
                       TextValue("character", "epoch"),
                       TextValue("interval", "P3DT4H5M6S"),
                   }),
                   273906) && ok;
  ok = ExpectFailureDetail("date_part_interval_invalid",
                           Run(registry, "sb.temporal.date_part", {
                               TextValue("character", "hour"),
                               TextValue("interval", "1 day"),
                           }),
                           "date_part interval text must match ISO-8601 duration grammar P[nY][nM][nD][T[nH][nM][nS]]") && ok;

  ok = ExpectText("SBSFC012-make_date-pos[SBSQL-91BF8C8E4A0B]",
                  Run(registry, "sb.temporal.make_date", {
                      Int64Value(2026),
                      Int64Value(5),
                      Int64Value(11),
                  }),
                  "2026-05-11",
                  "date") && ok;
  ok = ExpectNull("SBSFC012-make_date-null[SBSQL-91BF8C8E4A0B]",
                  Run(registry, "sb.temporal.make_date", {
                      NullValue("int64"),
                      Int64Value(5),
                      Int64Value(11),
                  }),
                  "date") && ok;
  ok = ExpectText("SBSFC012-make_date-stale-signature[SBSQL-A8EA58D8BB54]",
                  Run(registry, "sb.temporal.make_date", {
                      Int64Value(2026),
                      Int64Value(12),
                      Int64Value(31),
                  }),
                  "2026-12-31",
                  "date") && ok;
  ok = ExpectText("SBSFC012-make_time-pos[SBSQL-1F63F7A3A3E8]",
                  Run(registry, "sb.temporal.make_time", {
                      Int64Value(14),
                      Int64Value(23),
                      Int64Value(45),
                  }),
                  "14:23:45",
                  "time") && ok;
  ok = ExpectNull("SBSFC012-make_time-null[SBSQL-1F63F7A3A3E8]",
                  Run(registry, "sb.temporal.make_time", {
                      NullValue("int64"),
                      Int64Value(23),
                      Int64Value(45),
                  }),
                  "time") && ok;
  ok = ExpectText("SBSFC012-make_time-stale-signature[SBSQL-0A161EBD898D]",
                  Run(registry, "sb.temporal.make_time", {
                      Int64Value(23),
                      Int64Value(59),
                      Int64Value(58),
                  }),
                  "23:59:58",
                  "time") && ok;
  ok = ExpectText("SBSFC012-make_timestamp-pos[SBSQL-18C4CF502CEF]",
                  Run(registry, "sb.temporal.make_timestamp", {
                      TextValue("date", "2026-05-11"),
                      TextValue("time", "14:23:45"),
                  }),
                  "2026-05-11T14:23:45",
                  "timestamp") && ok;
  ok = ExpectNull("SBSFC012-make_timestamp-null[SBSQL-18C4CF502CEF]",
                  Run(registry, "sb.temporal.make_timestamp", {
                      NullValue("date"),
                      TextValue("time", "14:23:45"),
                  }),
                  "timestamp") && ok;
  ok = ExpectText("SBSFC012-make_timestamp-stale-sixint[SBSQL-0D13739AA895]",
                  Run(registry, "sb.temporal.make_timestamp", {
                      Int64Value(2026),
                      Int64Value(5),
                      Int64Value(11),
                      Int64Value(14),
                      Int64Value(23),
                      Int64Value(45),
                  }),
                  "2026-05-11T14:23:45",
                  "timestamp") && ok;
  ok = ExpectNull("SBSFC012-make_timestamp-stale-sixint-null[SBSQL-0D13739AA895]",
                  Run(registry, "sb.temporal.make_timestamp", {
                      Int64Value(2026),
                      Int64Value(5),
                      NullValue("int64"),
                      Int64Value(14),
                      Int64Value(23),
                      Int64Value(45),
                  }),
                  "timestamp") && ok;
  ok = ExpectFailureDetail("SBSFC012-make_timestamp-stale-sixint-type[SBSQL-0D13739AA895]",
                           Run(registry, "sb.temporal.make_timestamp", {
                               TextValue("character", "2026"),
                               Int64Value(5),
                               Int64Value(11),
                               Int64Value(14),
                               Int64Value(23),
                               Int64Value(45),
                           }),
                           "make_timestamp six-argument constructor requires int64 arguments") && ok;
  ok = ExpectFailureDetail("SBSFC012-make_timestamp-stale-sixint-arity[SBSQL-0D13739AA895]",
                           Run(registry, "sb.temporal.make_timestamp", {
                               Int64Value(2026),
                               Int64Value(5),
                               Int64Value(11),
                               Int64Value(14),
                               Int64Value(23),
                           }),
                           "make_timestamp expects date/time text or year month day hour minute second") && ok;
  ok = ExpectText("SBSFC012-make_timestamptz-default[SBSQL-B4F7A13FA126]",
                  Run(registry, "sb.temporal.make_timestamptz", {
                      TextValue("date", "2026-05-11"),
                      TextValue("time", "14:23:45"),
                  }),
                  "2026-05-11T14:23:45Z",
                  "timestamp_tz") && ok;
  ok = ExpectText("SBSFC012-make_timestamptz-with_tz[SBSQL-5C6D5D612785]",
                  Run(registry, "sb.temporal.make_timestamptz", {
                      TextValue("date", "2026-05-11"),
                      TextValue("time", "14:23:45"),
                      TextValue("character", "+05:30"),
                  }),
                  "2026-05-11T14:23:45+05:30",
                  "timestamp_tz") && ok;
  ok = ExpectNull("SBSFC012-make_timestamptz-null[SBSQL-B4F7A13FA126]",
                  Run(registry, "sb.temporal.make_timestamptz", {
                      NullValue("date"),
                      TextValue("time", "14:23:45"),
                  }),
                  "timestamp_tz") && ok;

  ok = ExpectText("date_trunc_day",
                  Run(registry, "sb.temporal.date_trunc", {
                      TextValue("character", "day"),
                      TextValue("timestamp", "2026-05-11T14:23:45"),
                  }),
                  "2026-05-11T00:00:00",
                  "timestamp") && ok;
  ok = ExpectText("date_trunc_bare_alias_day",
                  Run(registry, "sb.temporal.date_trunc", {
                      TextValue("character", "day"),
                      TextValue("timestamp", "2026-05-11T14:23:45"),
                  }),
                  "2026-05-11T00:00:00",
                  "timestamp") && ok;
  ok = ExpectText("qualified_date_trunc_alias_day",
                  Run(registry, "sb.temporal.date_trunc", {
                      TextValue("character", "day"),
                      TextValue("timestamp", "2026-05-11T14:23:45"),
                  }),
                  "2026-05-11T00:00:00",
                  "timestamp") && ok;
  ok = ExpectText("date_trunc_utc_timezone",
                  Run(registry, "sb.temporal.date_trunc", {
                      TextValue("character", "hour"),
                      TextValue("timestamp_tz", "2026-05-11T14:23:45Z"),
                      TextValue("character", "UTC"),
                  }),
                  "2026-05-11T14:00:00",
                  "timestamp") && ok;
  ok = ExpectFailureDetail("date_trunc_unsupported_timezone",
                           Run(registry, "sb.temporal.date_trunc", {
                               TextValue("character", "hour"),
                               TextValue("timestamp_tz", "2026-05-11T14:23:45Z"),
                               TextValue("character", "America/Toronto"),
                           }),
                           "date_trunc timezone support is limited to UTC in this runtime slice") && ok;

  SblrValue extra_arg;
  extra_arg.descriptor_id = "character";
  extra_arg.encoded_value = "unexpected";
  extra_arg.text_value = "unexpected";
  ok = ExpectInvalidArity("statement_timestamp_arity",
                          Run(registry, "sb.temporal.statement_timestamp", {std::move(extra_arg)})) && ok;
  ok = ExpectInvalidArity("qualified_current_user_arity",
                          Run(registry, "sb.session.current_user",
                              {TextValue("character", "unexpected")})) && ok;
  ok = ExpectInvalidArity("qualified_current_catalog_arity",
                          Run(registry, "sb.session.current_catalog",
                              {TextValue("character", "unexpected")})) && ok;
  ok = ExpectInvalidArity("qualified_current_schema_arity",
                          Run(registry, "sb.session.current_schema",
                              {TextValue("character", "unexpected")})) && ok;

  return ok ? 0 : 1;
}
