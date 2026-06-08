// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/spatial_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

enum class GeometryKind { point, linestring, polygon, collection };

struct Point {
  double x = 0.0;
  double y = 0.0;
  std::optional<double> z;
  std::optional<double> m;
};

struct Bounds {
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
};

struct Geometry {
  GeometryKind kind = GeometryKind::point;
  std::vector<Point> points;
  std::vector<Geometry> members;
  int srid = 0;
  std::string source_kind;
};

std::string Trim(std::string_view input) {
  std::size_t first = 0;
  while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) ++first;
  std::size_t last = input.size();
  while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) --last;
  return std::string(input.substr(first, last - first));
}

std::string UpperAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool IdIs(const std::string& id, std::initializer_list<std::string_view> names) {
  std::string leaf = id;
  if (leaf.rfind("sb.scalar.", 0) == 0) leaf = leaf.substr(10);
  if (leaf.rfind("spatial.", 0) == 0) leaf = leaf.substr(8);
  if (leaf.rfind("sb.fn.spatial.", 0) == 0) leaf = leaf.substr(14);
  const auto dot = leaf.rfind('.');
  const std::string suffix = dot == std::string::npos ? leaf : leaf.substr(dot + 1);
  for (const auto name : names) {
    const std::string text(name);
    if (id == text || id == "sb.scalar." + text || id == "spatial." + text ||
        id == "sb.fn.spatial." + text || leaf == text || suffix == text ||
        leaf.rfind(text + "_", 0) == 0 || suffix.rfind(text + "_", 0) == 0 ||
        (suffix == "construct_point" && (text == "st_makepoint" || text == "point" || text == "spatial_construct")) ||
        (suffix == "construct_line" && text == "st_makeline") ||
        (suffix == "construct_polygon" && text == "st_makepolygon") ||
        (suffix == "construct_collection" && text == "geom_collect") ||
        ((suffix == "construct_from_wkt" || suffix == "construct_geometry") && text == "st_geomfromtext") ||
        (suffix == "construct_from_geojson" && text == "st_geomfromgeojson") ||
        (suffix == "construct_from_wkb" && text == "st_geomfromwkb") ||
        (suffix == "measure_area" && text == "st_area") ||
        (suffix == "measure_azimuth" && text == "st_azimuth") ||
        (suffix == "measure_distance" && text == "st_distance") ||
        ((suffix == "measure_extent") && (text == "st_envelope" || text == "geom_extent")) ||
        (suffix == "measure_length" && text == "st_length") ||
        (suffix == "measure_generic" && text == "st_length") ||
        (suffix == "predicate_contains" && text == "st_contains") ||
        (suffix == "predicate_covers" && text == "st_covers") ||
        (suffix == "predicate_crosses" && text == "st_crosses") ||
        (suffix == "predicate_disjoint" && text == "st_disjoint") ||
        (suffix == "predicate_equals" && text == "st_equals") ||
        (suffix == "predicate_intersects" && text == "st_intersects") ||
        (suffix == "predicate_overlaps" && text == "st_overlaps") ||
        (suffix == "predicate_touches" && text == "st_touches") ||
        (suffix == "predicate_within" && text == "st_within") ||
        (suffix == "predicate_relate" && text == "st_relate") ||
        (suffix == "predicate_generic" && text == "st_intersects")) {
      return true;
    }
  }
  return false;
}

bool AnyNull(const FunctionCallRequest& request) {
  for (const auto& argument : request.arguments) {
    if (IsSqlNull(argument.value)) return true;
  }
  return false;
}

std::string FormatDouble(double value) {
  if (std::abs(value) < 0.000000000001) value = 0.0;
  std::ostringstream out;
  out << std::setprecision(12) << std::defaultfloat << value;
  std::string text = out.str();
  if (text.find('.') != std::string::npos && text.find('e') == std::string::npos &&
      text.find('E') == std::string::npos) {
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  return text.empty() ? "0" : text;
}

bool ParseFiniteDouble(std::string_view input, double* out) {
  const auto text = Trim(input);
  if (text.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) return false;
  *out = value;
  return true;
}

std::vector<double> ExtractNumbers(std::string_view input) {
  std::vector<double> values;
  const std::string text(input);
  const char* cursor = text.c_str();
  while (*cursor != '\0') {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(cursor, &end);
    if (end != cursor && errno != ERANGE && std::isfinite(value)) {
      values.push_back(value);
      cursor = end;
      continue;
    }
    ++cursor;
  }
  return values;
}

std::vector<Point> PointsFromNumbers(const std::vector<double>& numbers) {
  std::vector<Point> points;
  for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
    points.push_back(Point{numbers[i], numbers[i + 1]});
  }
  return points;
}

std::optional<std::vector<Point>> PointsFromNumbersWithDimension(const std::vector<double>& numbers,
                                                                 bool has_z,
                                                                 bool has_m) {
  const std::size_t stride = has_z && has_m ? 4 : (has_z || has_m ? 3 : 2);
  if (numbers.empty() || numbers.size() % stride != 0) return std::nullopt;
  std::vector<Point> points;
  for (std::size_t i = 0; i < numbers.size(); i += stride) {
    Point point{numbers[i], numbers[i + 1]};
    if (has_z && has_m) {
      point.z = numbers[i + 2];
      point.m = numbers[i + 3];
    } else if (has_z) {
      point.z = numbers[i + 2];
    } else if (has_m) {
      point.m = numbers[i + 2];
    }
    points.push_back(point);
  }
  return points;
}

std::string PointText(const Point& point) {
  std::string text = FormatDouble(point.x) + " " + FormatDouble(point.y);
  if (point.z) text += " " + FormatDouble(*point.z);
  if (point.m) text += " " + FormatDouble(*point.m);
  return text;
}

bool ExtractParenthesizedBody(std::string_view input, std::string* body) {
  const auto open = input.find('(');
  const auto close = input.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open) return false;
  *body = std::string(input.substr(open + 1, close - open - 1));
  return true;
}

void DetectWktDimensions(std::string_view input, std::string_view type_name, bool* has_z, bool* has_m) {
  *has_z = false;
  *has_m = false;
  const auto open = input.find('(');
  const auto header = UpperAscii(Trim(open == std::string::npos ? input : input.substr(0, open)));
  if (header.rfind(std::string(type_name), 0) != 0) return;
  const auto dimension = Trim(std::string_view(header).substr(type_name.size()));
  if (dimension == "Z") {
    *has_z = true;
  } else if (dimension == "M") {
    *has_m = true;
  } else if (dimension == "ZM") {
    *has_z = true;
    *has_m = true;
  }
}

bool ParseSridPrefix(std::string* text, int* srid) {
  const auto upper = UpperAscii(*text);
  if (upper.rfind("SRID=", 0) != 0) return true;
  const auto semi = text->find(';');
  if (semi == std::string::npos) return false;
  double parsed = 0.0;
  if (!ParseFiniteDouble(std::string_view(*text).substr(5, semi - 5), &parsed)) return false;
  if (parsed < 0 || parsed > static_cast<double>(std::numeric_limits<int>::max())) return false;
  *srid = static_cast<int>(parsed);
  *text = Trim(std::string_view(*text).substr(semi + 1));
  return true;
}

bool ParseWkt(std::string input, Geometry* geometry) {
  input = Trim(input);
  int srid = 0;
  if (!ParseSridPrefix(&input, &srid)) return false;
  const auto upper = UpperAscii(input);
  std::string body;
  if (!ExtractParenthesizedBody(input, &body)) return false;

  if (upper.rfind("POINT", 0) == 0) {
    bool has_z = false;
    bool has_m = false;
    DetectWktDimensions(input, "POINT", &has_z, &has_m);
    const auto points = has_z || has_m ? PointsFromNumbersWithDimension(ExtractNumbers(body), has_z, has_m)
                                       : std::optional<std::vector<Point>>(PointsFromNumbers(ExtractNumbers(body)));
    if (!points || points->size() != 1) return false;
    *geometry = Geometry{GeometryKind::point, *points, {}, srid, "wkt"};
    return true;
  }
  if (upper.rfind("LINESTRING", 0) == 0) {
    bool has_z = false;
    bool has_m = false;
    DetectWktDimensions(input, "LINESTRING", &has_z, &has_m);
    const auto points = has_z || has_m ? PointsFromNumbersWithDimension(ExtractNumbers(body), has_z, has_m)
                                       : std::optional<std::vector<Point>>(PointsFromNumbers(ExtractNumbers(body)));
    if (!points || points->size() < 2) return false;
    *geometry = Geometry{GeometryKind::linestring, *points, {}, srid, "wkt"};
    return true;
  }
  if (upper.rfind("POLYGON", 0) == 0) {
    bool has_z = false;
    bool has_m = false;
    DetectWktDimensions(input, "POLYGON", &has_z, &has_m);
    const auto points = has_z || has_m ? PointsFromNumbersWithDimension(ExtractNumbers(body), has_z, has_m)
                                       : std::optional<std::vector<Point>>(PointsFromNumbers(ExtractNumbers(body)));
    if (!points || points->size() < 3) return false;
    *geometry = Geometry{GeometryKind::polygon, *points, {}, srid, "wkt"};
    return true;
  }
  if (upper.rfind("GEOMETRYCOLLECTION", 0) == 0) {
    bool has_z = false;
    bool has_m = false;
    DetectWktDimensions(input, "GEOMETRYCOLLECTION", &has_z, &has_m);
    const auto points = has_z || has_m ? PointsFromNumbersWithDimension(ExtractNumbers(body), has_z, has_m)
                                       : std::optional<std::vector<Point>>(PointsFromNumbers(ExtractNumbers(body)));
    if (!points || points->empty()) return false;
    *geometry = Geometry{GeometryKind::collection, *points, {}, srid, "wkt"};
    return true;
  }
  return false;
}

std::optional<std::string> JsonStringForKey(std::string_view input, std::string_view key) {
  const std::string text(input);
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto pos = text.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  const auto colon = text.find(':', pos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  auto cursor = colon + 1;
  while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
  if (cursor >= text.size() || text[cursor] != '"') return std::nullopt;
  ++cursor;
  std::string value;
  bool escaped = false;
  for (; cursor < text.size(); ++cursor) {
    const char ch = text[cursor];
    if (escaped) {
      value.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return value;
    value.push_back(ch);
  }
  return std::nullopt;
}

bool ParseGeoJson(std::string_view input, Geometry* geometry) {
  const auto type = JsonStringForKey(input, "type");
  if (!type) return false;
  const auto lowered_type = LowerAscii(*type);
  const auto numbers = ExtractNumbers(input);
  const auto points = PointsFromNumbers(numbers);
  if (lowered_type == "point") {
    if (numbers.size() < 2 || numbers.size() > 4) return false;
    Point point{numbers[0], numbers[1]};
    if (numbers.size() >= 3) point.z = numbers[2];
    if (numbers.size() >= 4) point.m = numbers[3];
    *geometry = Geometry{GeometryKind::point, {point}, {}, 0, "geojson"};
    return true;
  }
  if (lowered_type == "linestring") {
    if (points.size() < 2) return false;
    *geometry = Geometry{GeometryKind::linestring, points, {}, 0, "geojson"};
    return true;
  }
  if (lowered_type == "polygon") {
    if (points.size() < 3) return false;
    *geometry = Geometry{GeometryKind::polygon, points, {}, 0, "geojson"};
    return true;
  }
  if (lowered_type == "geometrycollection") {
    if (points.empty()) return false;
    *geometry = Geometry{GeometryKind::collection, points, {}, 0, "geojson"};
    return true;
  }
  return false;
}

std::optional<std::uint8_t> HexByte(char high, char low) {
  auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  const int h = nibble(high);
  const int l = nibble(low);
  if (h < 0 || l < 0) return std::nullopt;
  return static_cast<std::uint8_t>((h << 4) | l);
}

std::vector<std::uint8_t> DecodeHex(std::string_view input) {
  std::vector<std::uint8_t> bytes;
  std::string hex;
  for (const char ch : input) {
    if (!std::isspace(static_cast<unsigned char>(ch))) hex.push_back(ch);
  }
  if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) hex = hex.substr(2);
  if (hex.size() % 2 != 0) return {};
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const auto byte = HexByte(hex[i], hex[i + 1]);
    if (!byte) return {};
    bytes.push_back(*byte);
  }
  return bytes;
}

bool ParseWkbPointHex(std::string_view input, Geometry* geometry) {
  const auto bytes = DecodeHex(input);
  if (bytes.size() < 21) return false;
  const bool little = bytes[0] == 1;
  if (!little && bytes[0] != 0) return false;
  auto read_u32 = [&](std::size_t offset) -> std::uint32_t {
    if (little) {
      return static_cast<std::uint32_t>(bytes[offset]) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
             (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
  };
  if (read_u32(1) != 1) return false;
  auto read_double = [&](std::size_t offset) -> double {
    std::array<std::uint8_t, 8> raw{};
    for (std::size_t i = 0; i < 8; ++i) raw[i] = bytes[offset + (little ? i : 7 - i)];
    double value = 0.0;
    static_assert(sizeof(value) == raw.size());
    std::memcpy(&value, raw.data(), raw.size());
    return value;
  };
  const double x = read_double(5);
  const double y = read_double(13);
  if (!std::isfinite(x) || !std::isfinite(y)) return false;
  *geometry = Geometry{GeometryKind::point, {Point{x, y}}, {}, 0, "wkb"};
  return true;
}

bool ParseGeometryText(std::string_view input, Geometry* geometry) {
  const auto text = Trim(input);
  if (text.empty()) return false;
  if (ParseWkt(text, geometry)) return true;
  if (!text.empty() && text.front() == '{' && ParseGeoJson(text, geometry)) return true;
  return false;
}

std::string GeometryTypeName(const Geometry& geometry) {
  switch (geometry.kind) {
    case GeometryKind::point:
      return "POINT";
    case GeometryKind::linestring:
      return "LINESTRING";
    case GeometryKind::polygon:
      return "POLYGON";
    case GeometryKind::collection:
      return "GEOMETRYCOLLECTION";
  }
  return "GEOMETRY";
}

std::string WktDimensionTokenForPoint(const Point& point) {
  if (point.z && point.m) return " ZM";
  if (point.z) return " Z";
  if (point.m) return " M";
  return "";
}

std::string WktDimensionToken(const Geometry& geometry) {
  for (const auto& point : geometry.points) {
    const auto token = WktDimensionTokenForPoint(point);
    if (!token.empty()) return token;
  }
  for (const auto& member : geometry.members) {
    const auto token = WktDimensionToken(member);
    if (!token.empty()) return token;
  }
  return "";
}

std::string GeometryToWktNoSrid(const Geometry& geometry) {
  std::ostringstream out;
  if (geometry.kind == GeometryKind::point) {
    out << "POINT" << WktDimensionToken(geometry) << "(" << PointText(geometry.points.front()) << ")";
  } else if (geometry.kind == GeometryKind::linestring) {
    out << "LINESTRING" << WktDimensionToken(geometry) << "(";
    for (std::size_t i = 0; i < geometry.points.size(); ++i) {
      if (i) out << ",";
      out << PointText(geometry.points[i]);
    }
    out << ")";
  } else if (geometry.kind == GeometryKind::polygon) {
    out << "POLYGON" << WktDimensionToken(geometry) << "((";
    for (std::size_t i = 0; i < geometry.points.size(); ++i) {
      if (i) out << ",";
      out << PointText(geometry.points[i]);
    }
    if (!geometry.points.empty() &&
        (geometry.points.front().x != geometry.points.back().x ||
         geometry.points.front().y != geometry.points.back().y)) {
      out << "," << PointText(geometry.points.front());
    }
    out << "))";
  } else {
    out << "GEOMETRYCOLLECTION(";
    for (std::size_t i = 0; i < geometry.members.size(); ++i) {
      if (i) out << ",";
      out << GeometryToWktNoSrid(geometry.members[i]);
    }
    if (geometry.members.empty()) {
      for (std::size_t i = 0; i < geometry.points.size(); ++i) {
        if (i) out << ",";
        out << "POINT" << WktDimensionTokenForPoint(geometry.points[i]) << "(" << PointText(geometry.points[i])
            << ")";
      }
    }
    out << ")";
  }
  return out.str();
}

std::string GeometryToWkt(const Geometry& geometry) {
  const auto wkt = GeometryToWktNoSrid(geometry);
  if (geometry.srid <= 0) return wkt;
  return "SRID=" + std::to_string(geometry.srid) + ";" + wkt;
}

Bounds GeometryBounds(const Geometry& geometry) {
  std::vector<Point> points = geometry.points;
  for (const auto& member : geometry.members) {
    points.insert(points.end(), member.points.begin(), member.points.end());
  }
  Bounds bounds{points.front().x, points.front().y, points.front().x, points.front().y};
  for (const auto& point : points) {
    bounds.min_x = std::min(bounds.min_x, point.x);
    bounds.min_y = std::min(bounds.min_y, point.y);
    bounds.max_x = std::max(bounds.max_x, point.x);
    bounds.max_y = std::max(bounds.max_y, point.y);
  }
  return bounds;
}

Geometry EnvelopeGeometry(const Geometry& geometry) {
  const auto b = GeometryBounds(geometry);
  return Geometry{GeometryKind::polygon,
                  {Point{b.min_x, b.min_y}, Point{b.max_x, b.min_y}, Point{b.max_x, b.max_y},
                   Point{b.min_x, b.max_y}, Point{b.min_x, b.min_y}},
                  {},
                  geometry.srid,
                  "envelope"};
}

double SegmentLength(const Point& a, const Point& b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}

double Length(const Geometry& geometry) {
  if (geometry.points.size() < 2) return 0.0;
  double sum = 0.0;
  for (std::size_t i = 1; i < geometry.points.size(); ++i) {
    sum += SegmentLength(geometry.points[i - 1], geometry.points[i]);
  }
  if (geometry.kind == GeometryKind::polygon &&
      (geometry.points.front().x != geometry.points.back().x ||
       geometry.points.front().y != geometry.points.back().y)) {
    sum += SegmentLength(geometry.points.back(), geometry.points.front());
  }
  return sum;
}

double PolygonArea(const Geometry& geometry) {
  if (geometry.kind != GeometryKind::polygon || geometry.points.size() < 3) return 0.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < geometry.points.size(); ++i) {
    const auto& a = geometry.points[i];
    const auto& b = geometry.points[(i + 1) % geometry.points.size()];
    sum += a.x * b.y - b.x * a.y;
  }
  return std::abs(sum) / 2.0;
}

Point Centroid(const Geometry& geometry) {
  if (geometry.kind == GeometryKind::polygon && geometry.points.size() >= 3) {
    double area2 = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    for (std::size_t i = 0; i < geometry.points.size(); ++i) {
      const auto& a = geometry.points[i];
      const auto& b = geometry.points[(i + 1) % geometry.points.size()];
      const double cross = a.x * b.y - b.x * a.y;
      area2 += cross;
      cx += (a.x + b.x) * cross;
      cy += (a.y + b.y) * cross;
    }
    if (std::abs(area2) > 0.000000000001) return Point{cx / (3.0 * area2), cy / (3.0 * area2)};
  }
  double x = 0.0;
  double y = 0.0;
  for (const auto& point : geometry.points) {
    x += point.x;
    y += point.y;
  }
  return Point{x / static_cast<double>(geometry.points.size()), y / static_cast<double>(geometry.points.size())};
}

bool BoundsEqual(const Bounds& a, const Bounds& b) {
  return a.min_x == b.min_x && a.min_y == b.min_y && a.max_x == b.max_x && a.max_y == b.max_y;
}

bool BoundsContains(const Bounds& outer, const Bounds& inner) {
  return outer.min_x <= inner.min_x && outer.min_y <= inner.min_y &&
         outer.max_x >= inner.max_x && outer.max_y >= inner.max_y;
}

bool BoundsIntersects(const Bounds& a, const Bounds& b) {
  return !(a.max_x < b.min_x || b.max_x < a.min_x || a.max_y < b.min_y || b.max_y < a.min_y);
}

bool BoundsInteriorIntersects(const Bounds& a, const Bounds& b) {
  return a.max_x > b.min_x && b.max_x > a.min_x && a.max_y > b.min_y && b.max_y > a.min_y;
}

bool BoundsTouches(const Bounds& a, const Bounds& b) {
  if (!BoundsIntersects(a, b) || BoundsInteriorIntersects(a, b)) return false;
  return true;
}

std::string JsonCoordinateFor(const Point& point) {
  std::ostringstream out;
  out << "[" << FormatDouble(point.x) << "," << FormatDouble(point.y);
  if (point.z) out << "," << FormatDouble(*point.z);
  if (point.m) out << "," << FormatDouble(*point.m);
  out << "]";
  return out.str();
}

std::string GeoJsonFor(const Geometry& geometry) {
  std::ostringstream out;
  out << "{\"type\":\"" << GeometryTypeName(geometry) << "\",\"coordinates\":";
  if (geometry.kind == GeometryKind::point) {
    out << JsonCoordinateFor(geometry.points[0]);
  } else if (geometry.kind == GeometryKind::polygon) {
    out << "[[";
    for (std::size_t i = 0; i < geometry.points.size(); ++i) {
      if (i) out << ",";
      out << JsonCoordinateFor(geometry.points[i]);
    }
    if (geometry.points.front().x != geometry.points.back().x ||
        geometry.points.front().y != geometry.points.back().y) {
      out << "," << JsonCoordinateFor(geometry.points.front());
    }
    out << "]]";
  } else {
    out << "[";
    for (std::size_t i = 0; i < geometry.points.size(); ++i) {
      if (i) out << ",";
      out << JsonCoordinateFor(geometry.points[i]);
    }
    out << "]";
  }
  if (geometry.srid > 0) out << ",\"srid\":" << geometry.srid;
  out << "}";
  return out.str();
}

std::string SvgFor(const Geometry& geometry) {
  if (geometry.kind == GeometryKind::point) {
    return "<circle cx=\"" + FormatDouble(geometry.points[0].x) + "\" cy=\"" +
           FormatDouble(geometry.points[0].y) + "\" r=\"1\"/>";
  }
  std::ostringstream out;
  out << "<path d=\"";
  for (std::size_t i = 0; i < geometry.points.size(); ++i) {
    out << (i == 0 ? "M " : " L ") << FormatDouble(geometry.points[i].x) << " "
        << FormatDouble(geometry.points[i].y);
  }
  if (geometry.kind == GeometryKind::polygon) out << " Z";
  out << "\"/>";
  return out.str();
}

std::string BinaryTextFor(const Geometry& geometry) {
  const auto wkt = GeometryToWkt(geometry);
  std::ostringstream out;
  out << "5342574B42";
  for (const unsigned char ch : wkt) {
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
  }
  return out.str();
}

FunctionCallResult ReturnNull(const FunctionCallRequest& request, std::string descriptor) {
  return MakeFunctionSuccess(request, {MakeNullValue(std::move(descriptor))});
}

FunctionCallResult ReturnGeometry(const FunctionCallRequest& request, const Geometry& geometry) {
  return MakeFunctionSuccess(request, {MakeTextValue("geometry", GeometryToWkt(geometry))});
}

bool ReadGeometryArgument(const FunctionCallRequest& request, std::size_t index, Geometry* geometry) {
  if (index >= request.arguments.size() || IsSqlNull(request.arguments[index].value)) return false;
  return ParseGeometryText(ValueAsText(request.arguments[index].value), geometry);
}

std::optional<std::int64_t> ReadIntArgument(const FunctionCallRequest& request, std::size_t index) {
  if (index >= request.arguments.size() || IsSqlNull(request.arguments[index].value)) return std::nullopt;
  const auto& value = request.arguments[index].value;
  if (value.has_int64_value) return value.int64_value;
  double parsed = 0.0;
  if (!ParseFiniteDouble(ValueAsText(value), &parsed)) return std::nullopt;
  return static_cast<std::int64_t>(parsed);
}

std::optional<double> ReadDoubleArgument(const FunctionCallRequest& request, std::size_t index) {
  if (index >= request.arguments.size() || IsSqlNull(request.arguments[index].value)) return std::nullopt;
  const auto& value = request.arguments[index].value;
  if (value.has_real64_value) return value.real64_value;
  if (value.has_int64_value) return static_cast<double>(value.int64_value);
  double parsed = 0.0;
  if (!ParseFiniteDouble(ValueAsText(value), &parsed)) return std::nullopt;
  return parsed;
}

bool ReadBooleanArgument(const FunctionCallRequest& request, std::size_t index, bool* out) {
  if (index >= request.arguments.size() || IsSqlNull(request.arguments[index].value)) return false;
  const auto& value = request.arguments[index].value;
  if (value.has_int64_value) {
    *out = value.int64_value != 0;
    return true;
  }
  const auto text = LowerAscii(Trim(ValueAsText(value)));
  if (text == "true" || text == "t" || text == "1") {
    *out = true;
    return true;
  }
  if (text == "false" || text == "f" || text == "0") {
    *out = false;
    return true;
  }
  return false;
}

Geometry EmptyGeometry(int srid) {
  return Geometry{GeometryKind::collection, {}, {}, srid, "empty"};
}

Geometry CollectionGeometry(const Geometry& a, const Geometry& b, std::string source_kind) {
  Geometry collection;
  collection.kind = GeometryKind::collection;
  collection.srid = a.srid;
  collection.source_kind = std::move(source_kind);
  collection.members = {a, b};
  collection.points = a.points;
  collection.points.insert(collection.points.end(), b.points.begin(), b.points.end());
  return collection;
}

Geometry ConvexHullGeometry(const Geometry& geometry) {
  if (geometry.kind == GeometryKind::point) return geometry;
  return EnvelopeGeometry(geometry);
}

bool ReadBoundsArgument(const FunctionCallRequest& request, std::size_t index, Bounds* bounds) {
  Geometry geometry;
  if (ReadGeometryArgument(request, index, &geometry)) {
    *bounds = GeometryBounds(geometry);
    return true;
  }
  if (index >= request.arguments.size() || IsSqlNull(request.arguments[index].value)) return false;
  const auto numbers = ExtractNumbers(ValueAsText(request.arguments[index].value));
  if (numbers.size() != 4) return false;
  *bounds = Bounds{std::min(numbers[0], numbers[2]), std::min(numbers[1], numbers[3]),
                   std::max(numbers[0], numbers[2]), std::max(numbers[1], numbers[3])};
  return true;
}

Point MvtPoint(const Point& point, const Bounds& bounds, double extent) {
  Point mapped = point;
  mapped.x = ((point.x - bounds.min_x) / (bounds.max_x - bounds.min_x)) * extent;
  mapped.y = ((bounds.max_y - point.y) / (bounds.max_y - bounds.min_y)) * extent;
  return mapped;
}

Geometry MvtGeometry(const Geometry& geometry, const Bounds& bounds, double extent) {
  Geometry mapped = geometry;
  mapped.srid = 0;
  mapped.source_kind = "mvtgeom";
  for (auto& point : mapped.points) {
    point = MvtPoint(point, bounds, extent);
  }
  for (auto& member : mapped.members) {
    member = MvtGeometry(member, bounds, extent);
  }
  return mapped;
}

FunctionCallResult Invalid(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionInvalidInput(request, std::move(detail));
}

}  // namespace

bool IsSpatialFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("spatial.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.spatial.", 0) == 0 ||
         request.context.function_id.rfind("sb.scalar.geom_", 0) == 0 ||
         request.context.function_id.rfind("sb.scalar.st_", 0) == 0;
}

FunctionCallResult DispatchSpatialFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (AnyNull(request)) {
    if (IdIs(id, {"st_x", "st_y", "st_z", "st_m", "st_distance", "st_area", "st_perimeter", "st_length",
                  "st_azimuth"})) {
      return ReturnNull(request, "real64");
    }
    if (IdIs(id, {"st_npoints", "st_numpoints", "st_srid"})) return ReturnNull(request, "int64");
    if (IdIs(id, {"st_contains", "st_covers", "st_crosses", "st_disjoint", "st_dwithin", "st_equals",
                  "st_intersects", "st_overlaps", "st_touches", "st_within"})) {
      return ReturnNull(request, "boolean");
    }
    if (IdIs(id, {"st_relate"})) return ReturnNull(request, "character");
    if (IdIs(id, {"st_asbinary"})) return ReturnNull(request, "bytea");
    if (IdIs(id, {"st_asgeojson"})) return ReturnNull(request, "json_document");
    if (IdIs(id, {"st_assvg", "st_astext", "st_geometrytype"})) return ReturnNull(request, "character");
    return ReturnNull(request, "geometry");
  }

  if (IdIs(id, {"st_makepoint", "point", "spatial_construct"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 4) {
      return Invalid(request, "st_makepoint expects x, y, and optional z and m");
    }
    const auto x = ReadDoubleArgument(request, 0);
    const auto y = ReadDoubleArgument(request, 1);
    if (!x || !y) return Invalid(request, "st_makepoint expects finite numeric coordinates");
    Point point{*x, *y};
    if (request.arguments.size() >= 3) {
      const auto z = ReadDoubleArgument(request, 2);
      if (!z) return Invalid(request, "st_makepoint z must be a finite numeric coordinate");
      point.z = *z;
    }
    if (request.arguments.size() >= 4) {
      const auto m = ReadDoubleArgument(request, 3);
      if (!m) return Invalid(request, "st_makepoint m must be a finite numeric coordinate");
      point.m = *m;
    }
    return ReturnGeometry(request, Geometry{GeometryKind::point, {point}, {}, 0, "constructed"});
  }

  if (IdIs(id, {"st_geogfromtext", "st_astext", "st_geometrytype", "st_x", "st_y", "st_srid",
                "st_z", "st_m", "st_area", "st_perimeter", "st_length", "st_npoints", "st_numpoints", "st_centroid",
                "st_envelope", "geom_extent", "st_asbinary", "st_asgeojson", "st_assvg",
                "st_simplify", "st_convexhull", "geom_union", "st_buffer"})) {
    if (request.arguments.empty()) return Invalid(request, id + " expects a geometry argument");
    Geometry geometry;
    if (!ReadGeometryArgument(request, 0, &geometry)) return Invalid(request, id + " expects WKT or GeoJSON geometry");

    if (IdIs(id, {"st_geogfromtext", "st_simplify", "geom_union"})) return ReturnGeometry(request, geometry);
    if (IdIs(id, {"st_astext"})) return MakeFunctionSuccess(request, {MakeTextValue("character", GeometryToWkt(geometry))});
    if (IdIs(id, {"st_geometrytype"})) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", GeometryTypeName(geometry))});
    }
    if (IdIs(id, {"st_x"})) {
      if (geometry.kind != GeometryKind::point) return Invalid(request, "st_x expects a POINT geometry");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", geometry.points[0].x)});
    }
    if (IdIs(id, {"st_y"})) {
      if (geometry.kind != GeometryKind::point) return Invalid(request, "st_y expects a POINT geometry");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", geometry.points[0].y)});
    }
    if (IdIs(id, {"st_z"})) {
      if (geometry.points.empty() || !geometry.points[0].z) return ReturnNull(request, "real64");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", *geometry.points[0].z)});
    }
    if (IdIs(id, {"st_m"})) {
      if (geometry.points.empty() || !geometry.points[0].m) return ReturnNull(request, "real64");
      return MakeFunctionSuccess(request, {MakeReal64Value("real64", *geometry.points[0].m)});
    }
    if (IdIs(id, {"st_srid"})) return MakeFunctionSuccess(request, {MakeInt64Value("int64", geometry.srid)});
    if (IdIs(id, {"st_area"})) return MakeFunctionSuccess(request, {MakeReal64Value("real64", PolygonArea(geometry))});
    if (IdIs(id, {"st_perimeter"})) return MakeFunctionSuccess(request, {MakeReal64Value("real64", Length(geometry))});
    if (IdIs(id, {"st_length"})) return MakeFunctionSuccess(request, {MakeReal64Value("real64", Length(geometry))});
    if (IdIs(id, {"st_npoints", "st_numpoints"})) {
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(geometry.points.size()))});
    }
    if (IdIs(id, {"st_centroid"})) {
      return ReturnGeometry(request, Geometry{GeometryKind::point, {Centroid(geometry)}, {}, geometry.srid, "centroid"});
    }
    if (IdIs(id, {"st_convexhull"})) return ReturnGeometry(request, ConvexHullGeometry(geometry));
    if (IdIs(id, {"st_envelope", "geom_extent"})) return ReturnGeometry(request, EnvelopeGeometry(geometry));
    if (IdIs(id, {"st_asbinary"})) {
      return MakeFunctionSuccess(request, {MakeTextValue("bytea", BinaryTextFor(geometry))});
    }
    if (IdIs(id, {"st_asgeojson"})) {
      return MakeFunctionSuccess(request, {MakeTextValue("json_document", GeoJsonFor(geometry))});
    }
    if (IdIs(id, {"st_assvg"})) return MakeFunctionSuccess(request, {MakeTextValue("character", SvgFor(geometry))});
    if (IdIs(id, {"st_buffer"})) {
      const double distance = request.arguments.size() >= 2 ? ReadDoubleArgument(request, 1).value_or(0.0) : 0.0;
      if (!std::isfinite(distance)) return Invalid(request, "st_buffer distance must be finite");
      const auto b = GeometryBounds(geometry);
      return ReturnGeometry(request, Geometry{GeometryKind::polygon,
                                              {Point{b.min_x - distance, b.min_y - distance},
                                               Point{b.max_x + distance, b.min_y - distance},
                                               Point{b.max_x + distance, b.max_y + distance},
                                               Point{b.min_x - distance, b.max_y + distance},
                                               Point{b.min_x - distance, b.min_y - distance}},
                                              {},
                                              geometry.srid,
                                              "buffer_bbox"});
    }
  }

  if (IdIs(id, {"st_geomfromtext"})) {
    if (request.arguments.empty() || request.arguments.size() > 2) {
      return Invalid(request, "st_geomfromtext expects WKT text and optional srid");
    }
    Geometry geometry;
    if (!ParseWkt(ValueAsText(request.arguments[0].value), &geometry)) {
      return Invalid(request, "st_geomfromtext expects deterministic WKT geometry");
    }
    if (request.arguments.size() == 2) {
      const auto srid = ReadIntArgument(request, 1);
      if (!srid || *srid < 0) return Invalid(request, "st_geomfromtext srid must be non-negative");
      geometry.srid = static_cast<int>(*srid);
    }
    return ReturnGeometry(request, geometry);
  }

  if (IdIs(id, {"st_asmvtgeom"})) {
    if (request.arguments.size() < 2 || request.arguments.size() > 5) {
      return Invalid(request, "st_asmvtgeom expects geometry, bbox, and optional extent/buffer/clip arguments");
    }
    Geometry geometry;
    Bounds bounds;
    if (!ReadGeometryArgument(request, 0, &geometry) || !ReadBoundsArgument(request, 1, &bounds)) {
      return Invalid(request, "st_asmvtgeom expects WKT or GeoJSON geometry and bbox geometry or four-number bounds");
    }
    double extent = 4096.0;
    if (request.arguments.size() >= 3) {
      const auto parsed_extent = ReadDoubleArgument(request, 2);
      if (!parsed_extent) return Invalid(request, "st_asmvtgeom extent must be positive");
      extent = *parsed_extent;
    }
    if (!std::isfinite(extent) || extent <= 0.0) return Invalid(request, "st_asmvtgeom extent must be positive");
    if (request.arguments.size() >= 4) {
      const auto buffer = ReadDoubleArgument(request, 3);
      if (!buffer || *buffer < 0.0) return Invalid(request, "st_asmvtgeom buffer must be non-negative");
    }
    if (request.arguments.size() >= 5) {
      bool clip = false;
      if (!ReadBooleanArgument(request, 4, &clip)) return Invalid(request, "st_asmvtgeom clip must be boolean");
      (void)clip;
    }
    if (bounds.max_x == bounds.min_x || bounds.max_y == bounds.min_y) {
      return Invalid(request, "st_asmvtgeom bbox must have non-zero width and height");
    }
    return ReturnGeometry(request, MvtGeometry(geometry, bounds, extent));
  }

  if (IdIs(id, {"st_setsrid", "st_transform"})) {
    if (request.arguments.size() != 2) return Invalid(request, id + " expects geometry and srid");
    Geometry geometry;
    const auto srid = ReadIntArgument(request, 1);
    if (!ReadGeometryArgument(request, 0, &geometry) || !srid || *srid < 0) {
      return Invalid(request, id + " expects valid geometry and non-negative srid");
    }
    geometry.srid = static_cast<int>(*srid);
    return ReturnGeometry(request, geometry);
  }

  if (IdIs(id, {"st_distance"})) {
    if (request.arguments.size() != 2) return Invalid(request, "st_distance expects two geometries");
    Geometry a;
    Geometry b;
    if (!ReadGeometryArgument(request, 0, &a) || !ReadGeometryArgument(request, 1, &b)) {
      return Invalid(request, "st_distance expects WKT or GeoJSON geometries");
    }
    const auto ca = Centroid(a);
    const auto cb = Centroid(b);
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", SegmentLength(ca, cb))});
  }

  if (IdIs(id, {"st_azimuth"})) {
    if (request.arguments.size() != 2) return Invalid(request, "st_azimuth expects two point geometries");
    Geometry a;
    Geometry b;
    if (!ReadGeometryArgument(request, 0, &a) || !ReadGeometryArgument(request, 1, &b)) {
      return Invalid(request, "st_azimuth expects WKT or GeoJSON geometries");
    }
    const auto ca = Centroid(a);
    const auto cb = Centroid(b);
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::atan2(cb.y - ca.y, cb.x - ca.x))});
  }

  if (IdIs(id, {"st_dwithin"})) {
    if (request.arguments.size() != 3) return Invalid(request, "st_dwithin expects two geometries and a distance");
    Geometry a;
    Geometry b;
    const auto distance = ReadDoubleArgument(request, 2);
    if (!ReadGeometryArgument(request, 0, &a) || !ReadGeometryArgument(request, 1, &b) || !distance ||
        *distance < 0.0) {
      return Invalid(request, "st_dwithin expects WKT or GeoJSON geometries and a non-negative finite distance");
    }
    const auto ca = Centroid(a);
    const auto cb = Centroid(b);
    const bool result = SegmentLength(ca, cb) <= *distance;
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", result ? 1 : 0)});
  }

  if (IdIs(id, {"st_contains", "st_covers", "st_crosses", "st_disjoint", "st_equals", "st_intersects",
                "st_overlaps", "st_touches", "st_within"})) {
    if (request.arguments.size() != 2) return Invalid(request, id + " expects two geometries");
    Geometry a;
    Geometry b;
    if (!ReadGeometryArgument(request, 0, &a) || !ReadGeometryArgument(request, 1, &b)) {
      return Invalid(request, id + " expects WKT or GeoJSON geometries");
    }
    const auto ba = GeometryBounds(a);
    const auto bb = GeometryBounds(b);
    bool result = false;
    if (IdIs(id, {"st_equals"})) {
      result = GeometryToWktNoSrid(a) == GeometryToWktNoSrid(b);
    } else if (IdIs(id, {"st_contains", "st_covers"})) {
      result = BoundsContains(ba, bb);
    } else if (IdIs(id, {"st_within"})) {
      result = BoundsContains(bb, ba);
    } else if (IdIs(id, {"st_intersects"})) {
      result = BoundsIntersects(ba, bb);
    } else if (IdIs(id, {"st_disjoint"})) {
      result = !BoundsIntersects(ba, bb);
    } else if (IdIs(id, {"st_touches"})) {
      result = BoundsTouches(ba, bb);
    } else if (IdIs(id, {"st_overlaps"})) {
      result = BoundsInteriorIntersects(ba, bb) && !BoundsContains(ba, bb) && !BoundsContains(bb, ba);
    } else if (IdIs(id, {"st_crosses"})) {
      result = BoundsInteriorIntersects(ba, bb) && !BoundsEqual(ba, bb);
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", result ? 1 : 0)});
  }

  if (IdIs(id, {"st_relate"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return Invalid(request, "st_relate expects two geometries and optional relation mask");
    }
    Geometry a;
    Geometry b;
    if (!ReadGeometryArgument(request, 0, &a) || !ReadGeometryArgument(request, 1, &b)) {
      return Invalid(request, "st_relate expects WKT or GeoJSON geometries");
    }
    const std::string relation = BoundsIntersects(GeometryBounds(a), GeometryBounds(b)) ? "T********" : "FF*FF****";
    if (request.arguments.size() == 3) {
      return MakeFunctionSuccess(request, {MakeInt64Value("boolean", relation == ValueAsText(request.arguments[2].value) ? 1 : 0)});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", relation)});
  }

  if (IdIs(id, {"st_intersection"})) {
    if (request.arguments.size() != 2) return Invalid(request, "st_intersection expects two geometries");
    Geometry a;
    Geometry b;
    if (!ReadGeometryArgument(request, 0, &a) || !ReadGeometryArgument(request, 1, &b)) {
      return Invalid(request, "st_intersection expects WKT or GeoJSON geometries");
    }
    const auto ba = GeometryBounds(a);
    const auto bb = GeometryBounds(b);
    if (!BoundsIntersects(ba, bb)) return ReturnGeometry(request, EmptyGeometry(a.srid));
    const Bounds i{std::max(ba.min_x, bb.min_x), std::max(ba.min_y, bb.min_y),
                   std::min(ba.max_x, bb.max_x), std::min(ba.max_y, bb.max_y)};
    return ReturnGeometry(request, Geometry{GeometryKind::polygon,
                                            {Point{i.min_x, i.min_y}, Point{i.max_x, i.min_y},
                                             Point{i.max_x, i.max_y}, Point{i.min_x, i.max_y},
                                             Point{i.min_x, i.min_y}},
                                            {},
                                            a.srid,
                                            "intersection_bbox"});
  }

  if (IdIs(id, {"st_difference", "st_symdifference", "st_union"})) {
    if (request.arguments.size() != 2) return Invalid(request, id + " expects two geometries");
    Geometry a;
    Geometry b;
    if (!ReadGeometryArgument(request, 0, &a) || !ReadGeometryArgument(request, 1, &b)) {
      return Invalid(request, id + " expects WKT or GeoJSON geometries");
    }
    const bool equal = GeometryToWktNoSrid(a) == GeometryToWktNoSrid(b);
    if (IdIs(id, {"st_union"})) {
      return ReturnGeometry(request, equal ? a : CollectionGeometry(a, b, "union"));
    }
    if (IdIs(id, {"st_difference"})) {
      if (equal || BoundsContains(GeometryBounds(b), GeometryBounds(a))) return ReturnGeometry(request, EmptyGeometry(a.srid));
      return ReturnGeometry(request, a);
    }
    if (equal) return ReturnGeometry(request, EmptyGeometry(a.srid));
    return ReturnGeometry(request, CollectionGeometry(a, b, "symdifference"));
  }

  if (IdIs(id, {"st_makeline"})) {
    if (request.arguments.empty()) return Invalid(request, "st_makeline expects point geometries or a coordinate array");
    std::vector<Point> points;
    int srid = 0;
    if (request.arguments.size() == 1) {
      Geometry geometry;
      if (ReadGeometryArgument(request, 0, &geometry)) {
        srid = geometry.srid;
        points = geometry.points;
      } else {
        const auto text = Trim(ValueAsText(request.arguments[0].value));
        if (text.empty() || text.front() != '[') return Invalid(request, "st_makeline expects geometry or coordinate array text");
        const auto numbers = ExtractNumbers(text);
        if (numbers.size() < 4 || numbers.size() % 2 != 0) {
          return Invalid(request, "st_makeline coordinate array must contain at least two x/y pairs");
        }
        points = PointsFromNumbers(numbers);
      }
    } else {
      for (std::size_t i = 0; i < request.arguments.size(); ++i) {
        Geometry geometry;
        if (!ReadGeometryArgument(request, i, &geometry) || geometry.points.empty()) {
          return Invalid(request, "st_makeline expects point or line geometry arguments");
        }
        if (i == 0) srid = geometry.srid;
        points.insert(points.end(), geometry.points.begin(), geometry.points.end());
      }
    }
    if (points.size() < 2) return Invalid(request, "st_makeline needs at least two points");
    return ReturnGeometry(request, Geometry{GeometryKind::linestring, points, {}, srid, "makeline"});
  }

  if (IdIs(id, {"geom_collect"})) {
    if (request.arguments.empty()) return Invalid(request, "geom_collect expects at least one geometry");
    Geometry collection;
    collection.kind = GeometryKind::collection;
    collection.source_kind = "collection";
    for (std::size_t i = 0; i < request.arguments.size(); ++i) {
      Geometry member;
      if (!ReadGeometryArgument(request, i, &member)) return Invalid(request, "geom_collect expects geometry arguments");
      collection.members.push_back(member);
      collection.points.insert(collection.points.end(), member.points.begin(), member.points.end());
    }
    return ReturnGeometry(request, collection);
  }

  if (IdIs(id, {"st_geomfromgeojson"})) {
    if (request.arguments.size() != 1) return Invalid(request, "st_geomfromgeojson expects GeoJSON text");
    Geometry geometry;
    if (!ParseGeoJson(ValueAsText(request.arguments[0].value), &geometry)) {
      return Invalid(request, "st_geomfromgeojson expects narrow Point/LineString/Polygon GeoJSON");
    }
    return ReturnGeometry(request, geometry);
  }

  if (IdIs(id, {"st_geomfromwkb"})) {
    if (request.arguments.empty() || request.arguments.size() > 2) return Invalid(request, "st_geomfromwkb expects wkb and optional srid");
    Geometry geometry;
    const auto text = ValueAsText(request.arguments[0].value);
    if (!ParseWkbPointHex(text, &geometry) && !ParseGeometryText(text, &geometry)) {
      return Invalid(request, "st_geomfromwkb expects point WKB hex or deterministic WKT text");
    }
    if (request.arguments.size() == 2) {
      const auto srid = ReadIntArgument(request, 1);
      if (!srid || *srid < 0) return Invalid(request, "st_geomfromwkb srid must be non-negative");
      geometry.srid = static_cast<int>(*srid);
    }
    return ReturnGeometry(request, geometry);
  }

  if (IdIs(id, {"st_makepolygon"})) {
    if (request.arguments.empty() || request.arguments.size() > 2) {
      return Invalid(request, "st_makepolygon expects a linestring and optional holes array");
    }
    Geometry line;
    if (!ReadGeometryArgument(request, 0, &line) || line.kind != GeometryKind::linestring || line.points.size() < 3) {
      return Invalid(request, "st_makepolygon expects a valid LINESTRING");
    }
    line.kind = GeometryKind::polygon;
    return ReturnGeometry(request, line);
  }

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_SPATIAL_FUNCTION_UNHANDLED",
                                      "spatial helper id is not handled by the activated spatial scalar surface");
}

}  // namespace scratchbird::engine::functions
