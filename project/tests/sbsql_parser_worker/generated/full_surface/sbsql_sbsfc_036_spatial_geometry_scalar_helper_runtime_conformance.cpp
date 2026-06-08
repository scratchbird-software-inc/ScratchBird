// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
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
using scratchbird::engine::sblr::SblrResult;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.text_value = std::move(input);
  value.encoded_value = value.text_value;
  value.charset_name = "UTF-8";
  value.collation_name = "unicode_root";
  value.is_null = false;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

SblrValue Real64Value(double input) {
  SblrValue value;
  value.descriptor_id = "real64";
  value.payload_kind = SblrValuePayloadKind::real64;
  value.has_real64_value = true;
  value.real64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

SblrResult Run(const FunctionRegistry& registry,
               std::string function_id,
               std::vector<SblrValue> values = {}) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-036-spatial-geometry-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-036-spatial-geometry-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.local_transaction_id = 36036;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1 ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected one successful non-mutating scalar result"
              << "; status=" << static_cast<int>(result.status)
              << "; scalar_count=" << result.scalar_values.size()
              << "; mutation_attempted=" << result.mutation_attempted
              << "; mutation_committed=" << result.mutation_committed << "\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  diagnostic=" << diagnostic.diagnostic_id << "\n";
      for (const auto& field : diagnostic.fields) {
        std::cerr << "    " << field.key << "=" << field.value << "\n";
      }
    }
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.encoded_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectReal(std::string_view case_id, const SblrResult& result, double expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "real64" || !value.has_real64_value ||
      std::abs(value.real64_value - expected) > 0.0000001) {
    std::cerr << case_id << ": expected real64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInt(std::string_view case_id, const SblrResult& result, std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "int64" || !value.has_int64_value ||
      value.int64_value != expected) {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBool(std::string_view case_id, const SblrResult& result, bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.descriptor_id != "boolean" || !value.has_int64_value ||
      value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << expected_int << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInvalidInput(std::string_view case_id, const SblrResult& result) {
  if (result.ok() || result.status != SblrStatusCode::execution_failed ||
      result.diagnostics.empty() ||
      result.diagnostics.front().diagnostic_id != "SB_DIAG_FUNCTION_INVALID_INPUT" ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_INVALID_INPUT refusal\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  const auto point = TextValue("geometry", "POINT(3 4)");
  const auto point12 = TextValue("geometry", "POINT(1 2)");
  const auto point00 = TextValue("geometry", "POINT(0 0)");
  const auto point34 = TextValue("geometry", "POINT(3 4)");
  const auto line = TextValue("geometry", "LINESTRING(0 0,1 1,2 1)");
  const auto extent_line = TextValue("geometry", "LINESTRING(0 0,2 3)");
  const auto square = TextValue("geometry", "POLYGON((0 0,2 0,2 2,0 2,0 0))");
  const auto big_square = TextValue("geometry", "POLYGON((0 0,4 0,4 4,0 4,0 0))");
  const auto shifted_square = TextValue("geometry", "POLYGON((1 1,3 1,3 3,1 3,1 1))");
  const auto touch_left = TextValue("geometry", "POLYGON((0 0,1 0,1 1,0 1,0 0))");
  const auto touch_right = TextValue("geometry", "POLYGON((1 0,2 0,2 1,1 1,1 0))");

  ok = ExpectReal("SBSQL-007BD17BDF55 SBSFC036-st-x",
                  Run(registry, "sb.scalar.st_x", {point}), 3.0) && ok;
  ok = ExpectText("SBSQL-01C6BF2303B1 SBSFC036-st-makepoint",
                  Run(registry, "sb.scalar.st_makepoint", {Real64Value(3), Real64Value(4)}),
                  "geometry", "POINT(3 4)") && ok;
  ok = ExpectBool("SBSQL-064CC33574E2 SBSFC036-st-crosses-signature",
                  Run(registry, "sb.scalar.st_crosses_g1_g2", {square, shifted_square}), true) && ok;
  ok = ExpectText("SBSQL-14816C2A7E33 SBSFC036-st-simplify",
                  Run(registry, "sb.scalar.st_simplify", {line}), "geometry",
                  "LINESTRING(0 0,1 1,2 1)") && ok;
  ok = ExpectText("SBSQL-14CD6B2AA8E3 SBSFC036-st-geometrytype",
                  Run(registry, "sb.scalar.st_geometrytype_geometry",
                      {TextValue("geometry", "LINESTRING(0 0,1 1)")}),
                  "character", "LINESTRING") && ok;
  ok = ExpectText("SBSQL-16705FD6AD8C SBSFC036-st-geogfromtext",
                  Run(registry, "sb.scalar.st_geogfromtext", {TextValue("character", "POINT(1 2)")}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectBool("SBSQL-1817177FD841 SBSFC036-st-contains-signature",
                  Run(registry, "sb.scalar.st_contains_g1_g2",
                      {big_square, TextValue("geometry", "POINT(2 2)")}),
                  true) && ok;
  ok = ExpectText("SBSQL-19B4EE69BD6A SBSFC036-geom-extent",
                  Run(registry, "sb.scalar.geom_extent_geometry", {extent_line}), "geometry",
                  "POLYGON((0 0,2 0,2 3,0 3,0 0))") && ok;
  ok = ExpectReal("SBSQL-19C49EFCE56D SBSFC036-st-y",
                  Run(registry, "sb.scalar.st_y", {point}), 4.0) && ok;
  ok = ExpectBool("SBSQL-20DB96AF4D98 SBSFC036-st-crosses",
                  Run(registry, "sb.scalar.st_crosses", {square, shifted_square}), true) && ok;
  ok = ExpectBool("SBSQL-2ED82920C391 SBSFC036-st-disjoint",
                  Run(registry, "sb.scalar.st_disjoint",
                      {TextValue("geometry", "POINT(0 0)"), TextValue("geometry", "POINT(2 2)")}),
                  true) && ok;
  ok = ExpectText("SBSQL-34B32A5FF887 SBSFC036-st-transform",
                  Run(registry, "sb.scalar.st_transform_geometry_target_srid", {point12, Int64Value(3857)}),
                  "geometry", "SRID=3857;POINT(1 2)") && ok;
  ok = ExpectInt("SBSQL-37FDE5D4CA38 SBSFC036-st-numpoints-signature",
                 Run(registry, "sb.scalar.st_numpoints_geometry", {line}), 3) && ok;
  ok = ExpectText("SBSQL-39EC9401DD7F SBSFC036-st-asbinary",
                  Run(registry, "sb.scalar.st_asbinary", {point12}), "bytea",
                  "5342574B42504F494E542831203229") && ok;
  ok = ExpectText("SBSQL-3B96D65453D5 SBSFC036-st-simplify-signature",
                  Run(registry, "sb.scalar.st_simplify_geometry_tolerance", {line, Real64Value(0.5)}),
                  "geometry", "LINESTRING(0 0,1 1,2 1)") && ok;
  ok = ExpectText("SBSQL-3E576350E9B0 SBSFC036-st-assvg-signature",
                  Run(registry, "sb.scalar.st_assvg_geometry", {point12}), "character",
                  "<circle cx=\"1\" cy=\"2\" r=\"1\"/>") && ok;
  ok = ExpectText("SBSQL-3F84A2CEBD71 SBSFC036-geom-union",
                  Run(registry, "sb.scalar.geom_union_geometry", {point12}), "geometry", "POINT(1 2)") && ok;
  ok = ExpectReal("SBSQL-4016AFBC31B8 SBSFC036-st-perimeter-signature",
                  Run(registry, "sb.scalar.st_perimeter_geometry", {square}), 8.0) && ok;
  ok = ExpectText("SBSQL-48C47B22CD64 SBSFC036-st-envelope",
                  Run(registry, "sb.scalar.st_envelope", {extent_line}), "geometry",
                  "POLYGON((0 0,2 0,2 3,0 3,0 0))") && ok;
  ok = ExpectReal("SBSQL-4976BE206EC9 SBSFC036-st-distance",
                  Run(registry, "sb.scalar.st_distance", {point00, point34}), 5.0) && ok;
  ok = ExpectReal("SBSQL-53EF6CC1B84B SBSFC036-st-x-point",
                  Run(registry, "sb.scalar.st_x_point", {point}), 3.0) && ok;
  ok = ExpectReal("SBSQL-549915041FC5 SBSFC036-st-distance-signature",
                  Run(registry, "sb.scalar.st_distance_g1_g2", {point00, point34}), 5.0) && ok;
  ok = ExpectText("SBSQL-56C21F337176 SBSFC036-st-envelope-signature",
                  Run(registry, "sb.scalar.st_envelope_geometry", {extent_line}), "geometry",
                  "POLYGON((0 0,2 0,2 3,0 3,0 0))") && ok;
  ok = ExpectText("SBSQL-577953487165 SBSFC036-st-astext",
                  Run(registry, "sb.scalar.st_astext", {point12}), "character", "POINT(1 2)") && ok;
  ok = ExpectText("SBSQL-581DB27EE2F3 SBSFC036-st-astext-signature",
                  Run(registry, "sb.scalar.st_astext_geometry", {point12}), "character", "POINT(1 2)") && ok;
  ok = ExpectText("SBSQL-5C008F9218F5 SBSFC036-st-asbinary-signature",
                  Run(registry, "sb.scalar.st_asbinary_geometry", {point12}), "bytea",
                  "5342574B42504F494E542831203229") && ok;
  ok = ExpectBool("SBSQL-610EB642822F SBSFC036-st-touches",
                  Run(registry, "sb.scalar.st_touches_g1_g2", {touch_left, touch_right}), true) && ok;
  ok = ExpectText("SBSQL-63555A174F42 SBSFC036-st-setsrid",
                  Run(registry, "sb.scalar.st_setsrid_geometry_srid", {point12, Int64Value(4326)}),
                  "geometry", "SRID=4326;POINT(1 2)") && ok;
  ok = ExpectReal("SBSQL-65C7DA8D048B SBSFC036-st-perimeter",
                  Run(registry, "sb.scalar.st_perimeter", {square}), 8.0) && ok;
  ok = ExpectText("SBSQL-6B3AC153575D SBSFC036-st-assvg",
                  Run(registry, "sb.scalar.st_assvg", {point12}), "character",
                  "<circle cx=\"1\" cy=\"2\" r=\"1\"/>") && ok;
  ok = ExpectText("SBSQL-6CC46392ADAC SBSFC036-st-buffer-signature",
                  Run(registry, "sb.scalar.st_buffer_geometry_distance",
                      {TextValue("geometry", "POINT(1 1)"), Real64Value(1)}),
                  "geometry", "POLYGON((0 0,2 0,2 2,0 2,0 0))") && ok;
  ok = ExpectInt("SBSQL-6E042C3D9DA7 SBSFC036-st-npoints",
                 Run(registry, "sb.scalar.st_npoints", {line}), 3) && ok;
  ok = ExpectInt("SBSQL-6EE712BB2CA2 SBSFC036-st-numpoints",
                 Run(registry, "sb.scalar.st_numpoints", {line}), 3) && ok;
  ok = ExpectBool("SBSQL-71E8B706D3F5 SBSFC036-st-overlaps",
                  Run(registry, "sb.scalar.st_overlaps", {square, shifted_square}), true) && ok;
  ok = ExpectBool("SBSQL-7387E0B53393 SBSFC036-st-intersects",
                  Run(registry, "sb.scalar.st_intersects",
                      {TextValue("geometry", "POINT(1 1)"), square}),
                  true) && ok;
  ok = ExpectText("SBSQL-774E359ADAF4 SBSFC036-st-geomfromwkb-signature",
                  Run(registry, "sb.scalar.st_geomfromwkb_wkb_srid",
                      {TextValue("bytea", "0101000000000000000000F03F0000000000000040"), Int64Value(4326)}),
                  "geometry", "SRID=4326;POINT(1 2)") && ok;
  ok = ExpectBool("SBSQL-779284739DB2 SBSFC036-st-equals",
                  Run(registry, "sb.scalar.st_equals_g1_g2", {point12, point12}), true) && ok;
  ok = ExpectText("SBSQL-78846923611D SBSFC036-st-geomfromwkb",
                  Run(registry, "sb.scalar.st_geomfromwkb",
                      {TextValue("bytea", "0101000000000000000000F03F0000000000000040")}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectText("SBSQL-7B1A7ED9A65B SBSFC036-st-makepolygon",
                  Run(registry, "sb.scalar.st_makepolygon_linestring_holesarray",
                      {TextValue("geometry", "LINESTRING(0 0,1 0,1 1,0 0)")}),
                  "geometry", "POLYGON((0 0,1 0,1 1,0 0))") && ok;
  ok = ExpectInt("SBSQL-7C81986B79D9 SBSFC036-st-srid",
                 Run(registry, "sb.scalar.st_srid_geometry",
                     {TextValue("geometry", "SRID=4326;POINT(1 2)")}),
                 4326) && ok;
  ok = ExpectText("SBSQL-7E7F908D3782 SBSFC036-st-geomfromgeojson",
                  Run(registry, "sb.scalar.st_geomfromgeojson_text",
                      {TextValue("json_document", "{\"type\":\"Point\",\"coordinates\":[1,2]}")}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectText("SBSQL-7F1AA7BC1C1B SBSFC036-st-centroid",
                  Run(registry, "sb.scalar.st_centroid", {square}), "geometry", "POINT(1 1)") && ok;
  ok = ExpectBool("SBSQL-81134D15580F SBSFC036-st-contains",
                  Run(registry, "sb.scalar.st_contains",
                      {big_square, TextValue("geometry", "POINT(2 2)")}),
                  true) && ok;
  ok = ExpectText("SBSQL-8126547CB199 SBSFC036-st-intersection",
                  Run(registry, "sb.scalar.st_intersection", {square, shifted_square}),
                  "geometry", "POLYGON((1 1,2 1,2 2,1 2,1 1))") && ok;
  ok = ExpectReal("SBSQL-82098A2E3A54 SBSFC036-st-area",
                  Run(registry, "sb.scalar.st_area_geometry", {square}), 4.0) && ok;
  ok = ExpectBool("SBSQL-83D324A5BD04 SBSFC036-st-within",
                  Run(registry, "sb.scalar.st_within_g1_g2",
                      {TextValue("geometry", "POINT(2 2)"), big_square}),
                  true) && ok;
  ok = ExpectText("SBSQL-8A2191CBD1FF SBSFC036-st-buffer",
                  Run(registry, "sb.scalar.st_buffer",
                      {TextValue("geometry", "POINT(1 1)"), Real64Value(1)}),
                  "geometry", "POLYGON((0 0,2 0,2 2,0 2,0 0))") && ok;
  ok = ExpectText("SBSQL-918FBB8B8F9A SBSFC036-geom-collect",
                  Run(registry, "sb.scalar.geom_collect", {point12, TextValue("geometry", "POINT(3 4)")}),
                  "geometry", "GEOMETRYCOLLECTION(POINT(1 2),POINT(3 4))") && ok;
  ok = ExpectText("SBSQL-9589706D80E8 SBSFC036-st-asgeojson",
                  Run(registry, "sb.scalar.st_asgeojson_geometry_maxdecimaldigits", {point12}),
                  "json_document", "{\"type\":\"POINT\",\"coordinates\":[1,2]}") && ok;
  ok = ExpectBool("SBSQL-9639CB5F0B9A SBSFC036-st-intersects-signature",
                  Run(registry, "sb.scalar.st_intersects_g1_g2",
                      {TextValue("geometry", "POINT(1 1)"), square}),
                  true) && ok;
  ok = ExpectInvalidInput("SBSFC036-st-x-invalid",
                          Run(registry, "sb.scalar.st_x",
                              {TextValue("geometry", "NOT_A_GEOMETRY")})) && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
