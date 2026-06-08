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

SblrValue BoolValue(bool input) {
  SblrValue value = Int64Value(input ? 1 : 0);
  value.descriptor_id = "boolean";
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
  request.context.sblr_context.database_uuid = "SBSFC-038-spatial-tail-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-038-spatial-tail-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.local_transaction_id = 38038;
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

  const auto point12 = TextValue("geometry", "POINT(1 2)");
  const auto point34 = TextValue("geometry", "POINT(3 4)");
  const auto point00 = TextValue("geometry", "POINT(0 0)");
  const auto point55 = TextValue("geometry", "POINT(5 5)");
  const auto line34 = TextValue("geometry", "LINESTRING(0 0,3 4)");
  const auto extent_line = TextValue("geometry", "LINESTRING(0 0,2 3)");
  const auto line_three = TextValue("geometry", "LINESTRING(0 0,1 1,2 1)");
  const auto square = TextValue("geometry", "POLYGON((0 0,2 0,2 2,0 2,0 0))");
  const auto big_square = TextValue("geometry", "POLYGON((0 0,4 0,4 4,0 4,0 0))");
  const auto shifted_square = TextValue("geometry", "POLYGON((1 1,3 1,3 3,1 3,1 1))");
  const auto touch_left = TextValue("geometry", "POLYGON((0 0,1 0,1 1,0 1,0 0))");
  const auto touch_right = TextValue("geometry", "POLYGON((1 0,2 0,2 1,1 1,1 0))");

  ok = ExpectText("SBSQL-9689873CEFCA SBSFC038-st-setsrid",
                  Run(registry, "sb.scalar.st_setsrid_geometry_srid", {point12, Int64Value(4326)}),
                  "geometry", "SRID=4326;POINT(1 2)") && ok;
  ok = ExpectBool("SBSQL-A01836D957A0 SBSFC038-st-dwithin-signature",
                  Run(registry, "sb.scalar.st_dwithin", {point00, point34, Real64Value(5)}),
                  true) && ok;
  ok = ExpectReal("SBSQL-A0BCD0E4C3DC SBSFC038-st-m",
                  Run(registry, "sb.scalar.st_m",
                      {TextValue("geometry", "POINT M(1 2 7)")}),
                  7.0) && ok;
  ok = ExpectBool("SBSQL-A57555BEE95E SBSFC038-st-overlaps-signature",
                  Run(registry, "sb.scalar.st_overlaps", {square, shifted_square}), true) && ok;
  ok = ExpectText("SBSQL-A5BDCC976DD0 SBSFC038-st-difference-signature",
                  Run(registry, "sb.scalar.st_difference", {point12, point34}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectReal("SBSQL-A5D10A16CCFA SBSFC038-st-z",
                  Run(registry, "sb.scalar.st_z",
                      {TextValue("geometry", "POINT Z(1 2 9)")}),
                  9.0) && ok;
  ok = ExpectReal("SBSQL-A8D99D74565F SBSFC038-st-area",
                  Run(registry, "sb.scalar.st_area_geometry", {square}), 4.0) && ok;
  ok = ExpectText("SBSQL-AD4F92702329 SBSFC038-st-asmvtgeom",
                  Run(registry, "sb.scalar.st_asmvtgeom",
                      {point55, TextValue("character", "0 0 10 10")}),
                  "geometry", "POINT(2048 2048)") && ok;
  ok = ExpectText("SBSQL-AEFECB9626BB SBSFC038-st-difference",
                  Run(registry, "sb.scalar.st_difference", {point12, point34}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectReal("SBSQL-B1718AA4E4B6 SBSFC038-st-length",
                  Run(registry, "sb.scalar.st_length", {line34}), 5.0) && ok;
  ok = ExpectText("SBSQL-B26EC3DF7AFB SBSFC038-geom-union",
                  Run(registry, "sb.scalar.geom_union_geometry", {point12}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectText("SBSQL-B288AFD4ECE5 SBSFC038-geom-collect-geometry",
                  Run(registry, "sb.scalar.geom_collect", {point12}),
                  "geometry", "GEOMETRYCOLLECTION(POINT(1 2))") && ok;
  ok = ExpectText("SBSQL-B5825D1638CA SBSFC038-st-makepoint-xyzm",
                  Run(registry, "sb.scalar.st_makepoint",
                      {Real64Value(1), Real64Value(2), Real64Value(3), Real64Value(4)}),
                  "geometry", "POINT ZM(1 2 3 4)") && ok;
  ok = ExpectBool("SBSQL-BA4115A6DBA5 SBSFC038-st-equals",
                  Run(registry, "sb.scalar.st_equals_g1_g2", {point12, point12}), true) && ok;
  ok = ExpectText("SBSQL-BD9DD4BBECA7 SBSFC038-st-intersection-signature",
                  Run(registry, "sb.scalar.st_intersection", {square, shifted_square}),
                  "geometry", "POLYGON((1 1,2 1,2 2,1 2,1 1))") && ok;
  ok = ExpectText("SBSQL-C03FDC7E09D0 SBSFC038-st-centroid-geometry",
                  Run(registry, "sb.scalar.st_centroid", {square}),
                  "geometry", "POINT(1 1)") && ok;
  ok = ExpectText("SBSQL-C44E7F61A475 SBSFC038-st-geometrytype",
                  Run(registry, "sb.scalar.st_geometrytype_geometry", {point12}),
                  "character", "POINT") && ok;
  ok = ExpectText("SBSQL-C557FC25C1DF SBSFC038-st-geomfromgeojson",
                  Run(registry, "sb.scalar.st_geomfromgeojson_text",
                      {TextValue("json_document", "{\"type\":\"Point\",\"coordinates\":[3,4]}")}),
                  "geometry", "POINT(3 4)") && ok;
  ok = ExpectText("SBSQL-C5B5E28021D3 SBSFC038-st-makeline-signature",
                  Run(registry, "sb.scalar.st_makeline",
                      {point00, TextValue("geometry", "POINT(1 1)")}),
                  "geometry", "LINESTRING(0 0,1 1)") && ok;
  ok = ExpectText("SBSQL-C6D14CCCA2D1 SBSFC038-st-geomfromtext-signature",
                  Run(registry, "sb.scalar.st_geomfromtext",
                      {TextValue("character", "POINT(1 2)"), Int64Value(4326)}),
                  "geometry", "SRID=4326;POINT(1 2)") && ok;
  ok = ExpectText("SBSQL-CBD9B6358B34 SBSFC038-geom-extent",
                  Run(registry, "sb.scalar.geom_extent_geometry", {extent_line}),
                  "geometry", "POLYGON((0 0,2 0,2 3,0 3,0 0))") && ok;
  ok = ExpectText("SBSQL-CBE14326BD0B SBSFC038-st-symdifference-signature",
                  Run(registry, "sb.scalar.st_symdifference", {point12, point34}),
                  "geometry", "GEOMETRYCOLLECTION(POINT(1 2),POINT(3 4))") && ok;
  ok = ExpectText("SBSQL-CF31B52FAA1F SBSFC038-st-asgeojson",
                  Run(registry, "sb.scalar.st_asgeojson_geometry_maxdecimaldigits", {point12}),
                  "json_document", "{\"type\":\"POINT\",\"coordinates\":[1,2]}") && ok;
  ok = ExpectBool("SBSQL-CFE56EE1BAC3 SBSFC038-st-dwithin",
                  Run(registry, "sb.scalar.st_dwithin", {point00, point34, Real64Value(5)}),
                  true) && ok;
  ok = ExpectBool("SBSQL-D3C5EA9765BE SBSFC038-st-touches",
                  Run(registry, "sb.scalar.st_touches_g1_g2", {touch_left, touch_right}), true) && ok;
  ok = ExpectText("SBSQL-D5BEA7309046 SBSFC038-st-transform",
                  Run(registry, "sb.scalar.st_transform_geometry_target_srid", {point12, Int64Value(3857)}),
                  "geometry", "SRID=3857;POINT(1 2)") && ok;
  ok = ExpectBool("SBSQL-DB22C5B8D6E6 SBSFC038-st-covers-signature",
                  Run(registry, "sb.scalar.st_covers",
                      {big_square, TextValue("geometry", "POINT(2 2)")}),
                  true) && ok;
  ok = ExpectInt("SBSQL-E211ACCD957F SBSFC038-st-srid",
                 Run(registry, "sb.scalar.st_srid_geometry",
                     {TextValue("geometry", "SRID=4326;POINT(1 2)")}),
                 4326) && ok;
  ok = ExpectBool("SBSQL-E43632706687 SBSFC038-st-disjoint-signature",
                  Run(registry, "sb.scalar.st_disjoint", {point00, point55}), true) && ok;
  ok = ExpectText("SBSQL-E4EB3BEDAA0A SBSFC038-st-convexhull-geometry",
                  Run(registry, "sb.scalar.st_convexhull", {extent_line}),
                  "geometry", "POLYGON((0 0,2 0,2 3,0 3,0 0))") && ok;
  ok = ExpectReal("SBSQL-E73C186D5991 SBSFC038-st-length-geometry",
                  Run(registry, "sb.scalar.st_length", {line34}), 5.0) && ok;
  ok = ExpectText("SBSQL-E8E12B064114 SBSFC038-st-convexhull",
                  Run(registry, "sb.scalar.st_convexhull", {extent_line}),
                  "geometry", "POLYGON((0 0,2 0,2 3,0 3,0 0))") && ok;
  ok = ExpectInt("SBSQL-F053EEAC95CD SBSFC038-st-npoints-geometry",
                 Run(registry, "sb.scalar.st_npoints", {line_three}), 3) && ok;
  ok = ExpectText("SBSQL-F1B58755A174 SBSFC038-st-makeline",
                  Run(registry, "sb.scalar.st_makeline",
                      {TextValue("character", "[[0,0],[1,1]]")}),
                  "geometry", "LINESTRING(0 0,1 1)") && ok;
  ok = ExpectText("SBSQL-F21F901FC2AF SBSFC038-st-makepolygon",
                  Run(registry, "sb.scalar.st_makepolygon_linestring_holesarray",
                      {TextValue("geometry", "LINESTRING(0 0,1 0,1 1,0 0)")}),
                  "geometry", "POLYGON((0 0,1 0,1 1,0 0))") && ok;
  ok = ExpectText("SBSQL-F3C89846D91C SBSFC038-st-geomfromtext",
                  Run(registry, "sb.scalar.st_geomfromtext",
                      {TextValue("character", "POINT(1 2)")}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectBool("SBSQL-F4AE1FA62237 SBSFC038-st-within",
                  Run(registry, "sb.scalar.st_within_g1_g2",
                      {TextValue("geometry", "POINT(2 2)"), big_square}),
                  true) && ok;
  ok = ExpectText("SBSQL-F763191B3241 SBSFC038-st-symdifference",
                  Run(registry, "sb.scalar.st_symdifference", {point12, point34}),
                  "geometry", "GEOMETRYCOLLECTION(POINT(1 2),POINT(3 4))") && ok;
  ok = ExpectBool("SBSQL-F7D5231CA0E4 SBSFC038-st-covers",
                  Run(registry, "sb.scalar.st_covers",
                      {big_square, TextValue("geometry", "POINT(2 2)")}),
                  true) && ok;
  ok = ExpectText("SBSQL-F8050BCAF06D SBSFC038-st-union",
                  Run(registry, "sb.scalar.st_union", {point12, point34}),
                  "geometry", "GEOMETRYCOLLECTION(POINT(1 2),POINT(3 4))") && ok;
  ok = ExpectText("SBSQL-F985930BDD2F SBSFC038-st-geogfromtext-wkt",
                  Run(registry, "sb.scalar.st_geogfromtext",
                      {TextValue("character", "POINT(1 2)")}),
                  "geometry", "POINT(1 2)") && ok;
  ok = ExpectText("SBSQL-FB46F964CAA5 SBSFC038-st-union-signature",
                  Run(registry, "sb.scalar.st_union", {point12, point34}),
                  "geometry", "GEOMETRYCOLLECTION(POINT(1 2),POINT(3 4))") && ok;
  ok = ExpectText("SBSQL-FF57FEDF9747 SBSFC038-st-asmvtgeom-signature",
                  Run(registry, "sb.scalar.st_asmvtgeom",
                      {point55, TextValue("character", "0 0 10 10"), Real64Value(10),
                       Real64Value(0), BoolValue(true)}),
                  "geometry", "POINT(5 5)") && ok;
  ok = ExpectInvalidInput("SBSFC038-st-geomfromtext-invalid",
                          Run(registry, "sb.scalar.st_geomfromtext",
                              {TextValue("character", "NOT_A_GEOMETRY")})) && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
