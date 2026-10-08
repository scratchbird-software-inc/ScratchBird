// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_bit_string.hpp"
#include "datatype_blob.hpp"
#include "datatype_catalog_manifest.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace dt = scratchbird::core::datatypes;
namespace engine = scratchbird::engine;
namespace platform = scratchbird::core::platform;

unsigned checks = 0;

enum class ClosedCastExpectation : std::uint8_t {
  forbidden,
  identity,
  explicit_admitted,
  descriptor_unresolved,
};
struct ClosedCastPolicyFixture {
  std::string_view row_id;
  std::string_view source_label;
  std::string_view target_label;
  bool identities_resolved;
  std::array<ClosedCastExpectation, 3> contexts;
};
// Compiled reproduction of all ordered rows in
// BASE-BIT-STRING-CAST-POLICY-V1. This test has no runtime dependency on the
// evidence package.
static constexpr ClosedCastPolicyFixture kClosedCastPolicy[] = {
  {"BIT-CAST-001", "base.aggregate_state", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-002", "base.array", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-003", "base.bfloat16", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-004", "base.binary", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-005", "base.binary_json_document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-006", "base.binary_vector", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-007", "base.bit_string", "base.aggregate_state", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-008", "base.bit_string", "base.array", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-009", "base.bit_string", "base.bfloat16", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-010", "base.bit_string", "base.binary", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-011", "base.bit_string", "base.binary_json_document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-012", "base.bit_string", "base.binary_vector", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-013", "base.bit_string", "base.bit_string", true, {ClosedCastExpectation::identity, ClosedCastExpectation::identity, ClosedCastExpectation::identity}},
  {"BIT-CAST-014", "base.bit_string", "base.blob", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-015", "base.bit_string", "base.bloom_filter", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-016", "base.bit_string", "base.boolean", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-017", "base.bit_string", "base.bridge_handle", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-018", "base.bit_string", "base.bson_document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-019", "base.bit_string", "base.character", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-020", "base.bit_string", "base.columnar_segment", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-021", "base.bit_string", "base.composite", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-022", "base.bit_string", "base.cursor", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-023", "base.bit_string", "base.cursor_handle", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-024", "base.bit_string", "base.date", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-025", "base.bit_string", "base.decimal", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-026", "base.bit_string", "base.decimal_float", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-027", "base.bit_string", "base.dense_vector", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-028", "base.bit_string", "base.document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-029", "base.bit_string", "base.enum_value", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-030", "base.bit_string", "base.external_file_locator", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-031", "base.bit_string", "base.flattened_object_document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-032", "base.bit_string", "base.geography", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-033", "base.bit_string", "base.geometry", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-034", "base.bit_string", "base.graph_edge", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-035", "base.bit_string", "base.graph_node", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-036", "base.bit_string", "base.graph_path", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-037", "base.bit_string", "base.histogram_sketch", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-038", "base.bit_string", "base.hll_sketch", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-039", "base.bit_string", "base.hstore_document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-040", "base.bit_string", "base.int128", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-041", "base.bit_string", "base.int16", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-042", "base.bit_string", "base.int32", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-043", "base.bit_string", "base.int64", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-044", "base.bit_string", "base.int8", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-045", "base.bit_string", "base.interval", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-046", "base.bit_string", "base.ip_address", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-047", "base.bit_string", "base.json_document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-048", "base.bit_string", "base.list", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-049", "base.bit_string", "base.lob_locator", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-050", "base.bit_string", "base.mac_address", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-051", "base.bit_string", "base.map", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-052", "base.bit_string", "base.multirange", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-053", "base.bit_string", "base.network_prefix", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-054", "base.bit_string", "base.null", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-055", "base.bit_string", "base.object_document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-056", "base.bit_string", "base.opaque_extension", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-057", "base.bit_string", "base.point", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-058", "base.bit_string", "base.quantile_sketch", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-059", "base.bit_string", "base.quantized_vector", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-060", "base.bit_string", "base.range", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-061", "base.bit_string", "base.ranking_summary", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-062", "base.bit_string", "base.raster", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-063", "base.bit_string", "base.real128", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-064", "base.bit_string", "base.real16", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-065", "base.bit_string", "base.real32", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-066", "base.bit_string", "base.real64", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-067", "base.bit_string", "base.remote_object_locator", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-068", "base.bit_string", "base.result_set", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-069", "base.bit_string", "base.row", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-070", "base.bit_string", "base.search_completion", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-071", "base.bit_string", "base.search_percolator", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-072", "base.bit_string", "base.search_query", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-073", "base.bit_string", "base.search_rank_feature", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-074", "base.bit_string", "base.set_value", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-075", "base.bit_string", "base.shape", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-076", "base.bit_string", "base.sparse_vector", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-077", "base.bit_string", "base.system_reference", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-078", "base.bit_string", "base.table_value", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-079", "base.bit_string", "base.time", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-080", "base.bit_string", "base.time_series_value", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-081", "base.bit_string", "base.timestamp", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-082", "base.bit_string", "base.token_stream", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-083", "base.bit_string", "base.uint128", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-084", "base.bit_string", "base.uint16", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-085", "base.bit_string", "base.uint32", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-086", "base.bit_string", "base.uint64", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-087", "base.bit_string", "base.uint8", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-088", "base.bit_string", "base.uuid", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-089", "base.bit_string", "base.variant", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-090", "base.bit_string", "base.vector", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-091", "base.bit_string", "base.vector_summary", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-092", "base.bit_string", "base.xml_document", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-093", "base.bit_string", "spec_only.column_segment_value", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-094", "base.bit_string", "spec_only.compressed_column_value", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-095", "base.bit_string", "spec_only.day_time_interval", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-096", "base.bit_string", "spec_only.dictionary_encoded", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-097", "base.bit_string", "spec_only.field_value", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-098", "base.bit_string", "spec_only.fixed_duration", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-099", "base.bit_string", "spec_only.graph_label", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-100", "base.bit_string", "spec_only.graph_property_map", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-101", "base.bit_string", "spec_only.instant", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-102", "base.bit_string", "spec_only.ip_network", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-103", "base.bit_string", "spec_only.ip_range", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-104", "base.bit_string", "spec_only.low_cardinality", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-105", "base.bit_string", "spec_only.measurement", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-106", "base.bit_string", "spec_only.mixed_interval", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-107", "base.bit_string", "spec_only.money_currency", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-108", "base.bit_string", "spec_only.nested_column", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-109", "base.bit_string", "spec_only.nullable_wrapper", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-110", "base.bit_string", "spec_only.rollup_state", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-111", "base.bit_string", "spec_only.series_key", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-112", "base.bit_string", "spec_only.tag_set", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-113", "base.bit_string", "spec_only.time_bucket", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-114", "base.bit_string", "spec_only.time_with_zone", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-115", "base.bit_string", "spec_only.timestamp_with_zone", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-116", "base.bit_string", "spec_only.vectorized_batch_value", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-117", "base.bit_string", "spec_only.year_month_interval", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-118", "base.blob", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-119", "base.bloom_filter", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-120", "base.boolean", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-121", "base.bridge_handle", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-122", "base.bson_document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-123", "base.character", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-124", "base.columnar_segment", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-125", "base.composite", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-126", "base.cursor", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-127", "base.cursor_handle", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-128", "base.date", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-129", "base.decimal", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-130", "base.decimal_float", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-131", "base.dense_vector", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-132", "base.document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-133", "base.enum_value", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-134", "base.external_file_locator", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-135", "base.flattened_object_document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-136", "base.geography", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-137", "base.geometry", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-138", "base.graph_edge", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-139", "base.graph_node", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-140", "base.graph_path", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-141", "base.histogram_sketch", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-142", "base.hll_sketch", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-143", "base.hstore_document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-144", "base.int128", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-145", "base.int16", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-146", "base.int32", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-147", "base.int64", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-148", "base.int8", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-149", "base.interval", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-150", "base.ip_address", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-151", "base.json_document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-152", "base.list", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-153", "base.lob_locator", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-154", "base.mac_address", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-155", "base.map", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-156", "base.multirange", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-157", "base.network_prefix", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-158", "base.null", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-159", "base.object_document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-160", "base.opaque_extension", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-161", "base.point", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-162", "base.quantile_sketch", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-163", "base.quantized_vector", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-164", "base.range", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-165", "base.ranking_summary", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-166", "base.raster", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-167", "base.real128", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-168", "base.real16", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-169", "base.real32", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-170", "base.real64", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-171", "base.remote_object_locator", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-172", "base.result_set", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-173", "base.row", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-174", "base.search_completion", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-175", "base.search_percolator", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-176", "base.search_query", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-177", "base.search_rank_feature", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-178", "base.set_value", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-179", "base.shape", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-180", "base.sparse_vector", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-181", "base.system_reference", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-182", "base.table_value", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-183", "base.time", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-184", "base.time_series_value", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-185", "base.timestamp", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-186", "base.token_stream", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-187", "base.uint128", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-188", "base.uint16", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-189", "base.uint32", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-190", "base.uint64", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-191", "base.uint8", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::explicit_admitted}},
  {"BIT-CAST-192", "base.uuid", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-193", "base.variant", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-194", "base.vector", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-195", "base.vector_summary", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-196", "base.xml_document", "base.bit_string", true, {ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden, ClosedCastExpectation::forbidden}},
  {"BIT-CAST-197", "spec_only.column_segment_value", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-198", "spec_only.compressed_column_value", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-199", "spec_only.day_time_interval", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-200", "spec_only.dictionary_encoded", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-201", "spec_only.field_value", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-202", "spec_only.fixed_duration", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-203", "spec_only.graph_label", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-204", "spec_only.graph_property_map", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-205", "spec_only.instant", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-206", "spec_only.ip_network", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-207", "spec_only.ip_range", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-208", "spec_only.low_cardinality", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-209", "spec_only.measurement", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-210", "spec_only.mixed_interval", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-211", "spec_only.money_currency", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-212", "spec_only.nested_column", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-213", "spec_only.nullable_wrapper", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-214", "spec_only.rollup_state", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-215", "spec_only.series_key", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-216", "spec_only.tag_set", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-217", "spec_only.time_bucket", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-218", "spec_only.time_with_zone", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-219", "spec_only.timestamp_with_zone", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-220", "spec_only.vectorized_batch_value", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
  {"BIT-CAST-221", "spec_only.year_month_interval", "base.bit_string", false, {ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved, ClosedCastExpectation::descriptor_unresolved}},
};

struct SealedSortKeyFixture {
  std::string_view name;
  std::string_view bits;
  bool is_null;
  dt::BitStringSortDirectionV3 direction;
  dt::BitStringNullModeV3 null_mode;
  std::string_view hex;
};
static constexpr SealedSortKeyFixture kSealedSortKeys[] = {
  {"ascending_nulls_first_0", "0", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e3010000000000000000000100000000000100"},
  {"ascending_nulls_first_00", "00", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000000010000000000010100"},
  {"ascending_nulls_first_01", "01", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000000010000000000010200"},
  {"ascending_nulls_first_1", "1", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e3010000000000000000000100000000000200"},
  {"ascending_nulls_first_10", "10", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000000010000000000020100"},
  {"ascending_nulls_first_11", "11", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000000010000000000020200"},
  {"ascending_nulls_first_NULL", "", true, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000000000000000000"},
  {"ascending_nulls_first_empty", "", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e30100000000000000000001000000000000"},
  {"ascending_nulls_last_0", "0", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e3010000000000000000010100000000000100"},
  {"ascending_nulls_last_00", "00", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000001010000000000010100"},
  {"ascending_nulls_last_01", "01", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000001010000000000010200"},
  {"ascending_nulls_last_1", "1", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e3010000000000000000010100000000000200"},
  {"ascending_nulls_last_10", "10", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000001010000000000020100"},
  {"ascending_nulls_last_11", "11", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000001010000000000020200"},
  {"ascending_nulls_last_NULL", "", true, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000001020000000000"},
  {"ascending_nulls_last_empty", "", false, dt::BitStringSortDirectionV3::ascending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e30100000000000000000101000000000000"},
  {"descending_nulls_first_0", "0", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100010000000000feff"},
  {"descending_nulls_first_00", "00", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100010000000000fefeff"},
  {"descending_nulls_first_01", "01", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100010000000000fefdff"},
  {"descending_nulls_first_1", "1", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100010000000000fdff"},
  {"descending_nulls_first_10", "10", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100010000000000fdfeff"},
  {"descending_nulls_first_11", "11", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100010000000000fdfdff"},
  {"descending_nulls_first_NULL", "", true, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100000000000000"},
  {"descending_nulls_first_empty", "", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_first, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000100010000000000ff"},
  {"descending_nulls_last_0", "0", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101010000000000feff"},
  {"descending_nulls_last_00", "00", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101010000000000fefeff"},
  {"descending_nulls_last_01", "01", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101010000000000fefdff"},
  {"descending_nulls_last_1", "1", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101010000000000fdff"},
  {"descending_nulls_last_10", "10", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101010000000000fdfeff"},
  {"descending_nulls_last_11", "11", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101010000000000fdfdff"},
  {"descending_nulls_last_NULL", "", true, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101020000000000"},
  {"descending_nulls_last_empty", "", false, dt::BitStringSortDirectionV3::descending, dt::BitStringNullModeV3::nulls_last, "53424249544b3031019d000000007000800000000000d7100a000000000000000a00000000000000e1860523736b5b688bfb921c3eac0e786d20b639683d6d51cd4ce1515e961c8901a0ff2727177a54bdbca3c1fee7c5e301000000000000000101010000000000ff"},
};


[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}
void Check(bool value, std::string_view message) {
  ++checks;
  if (!value) Fail(message);
}

dt::BitStringDescriptorProfileV3 Profile(
    dt::BitStringSurfaceProfileKindV3 kind =
        dt::BitStringSurfaceProfileKindV3::unqualified,
    std::uint32_t length = dt::kBitStringMaximumLogicalBitsV3) {
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV10, 10, 10,
      scratchbird::tests::FixtureUuidLiteral(
          "019d0000-0000-7000-8000-00000000d829"), 1);
  Check(identity.ok &&
            dt::IsExactCanonicalBitStringTypeCodecIdentityV3(identity.row),
        "exact D710 bit identity lookup failed");
  const auto historical = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV6, 6, 6,
      scratchbird::tests::FixtureUuidLiteral(
          "019d0000-0000-7000-8000-00000000d829"), 1);
  Check(historical.ok &&
            !dt::IsExactCanonicalBitStringTypeCodecIdentityV3(historical.row),
        "d706 bit identity did not remain historical-only");
  const auto historical_profile = dt::BuildBitStringDescriptorProfileV3(
      {{scratchbird::tests::FixtureUuid(9902, 2), dt::kDatatypeCohortV6,
        6, 6}, historical.row, kind, length});
  Check(!historical_profile.ok(),
        "d706 historical identity established a current bit profile");
  dt::BitStringAuthorityReceiptV3 receipt{
      scratchbird::tests::FixtureUuid(9902, 1), dt::kDatatypeCohortV10, 10, 10};
  const auto profile = dt::BuildBitStringDescriptorProfileV3(
      {receipt, identity.row, kind, length});
  Check(profile.ok(), "bit profile construction failed");
  return profile.profile;
}

std::vector<platform::byte> Pack(std::string_view bits) {
  std::vector<platform::byte> bytes((bits.size() + 7) / 8, 0);
  for (std::size_t i = 0; i < bits.size(); ++i) {
    Check(bits[i] == '0' || bits[i] == '1', "invalid bit fixture");
    if (bits[i] == '1') bytes[i / 8] |= static_cast<platform::byte>(0x80u >> (i % 8));
  }
  return bytes;
}

std::vector<platform::byte> Hex(std::string_view text) {
  auto nibble=[](char c)->unsigned{return c<='9'?c-'0':10+(c|32)-'a';};
  std::vector<platform::byte> out(text.size()/2);
  for(std::size_t i=0;i<out.size();++i)
    out[i]=static_cast<platform::byte>((nibble(text[2*i])<<4)|nibble(text[2*i+1]));
  return out;
}

dt::BitStringValueViewV3 View(const dt::BitStringDescriptorProfileV3& profile,
                              std::string_view bits,
                              std::vector<platform::byte>* storage) {
  *storage = Pack(bits);
  return {&profile, dt::BitStringValueStateV3::present,
          static_cast<std::uint32_t>(bits.size()), *storage,
          dt::BitStringOwnershipV3::borrowed};
}

bool IsBits(const dt::BitStringOwnedValueV3& value, std::string_view bits) {
  if (value.state != dt::BitStringValueStateV3::present ||
      value.logical_bit_count != bits.size()) return false;
  const auto expected = Pack(bits);
  return value.packed_msb0 == expected;
}

engine::ExecutionTypeDescriptor Descriptor(dt::CanonicalTypeId type) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "catalog manifest unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type);
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        "catalog descriptor lookup failed");
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto result = dt::LookupExecutionTypeDescriptorFromCatalog(type, metadata);
  Check(result.ok(), std::string("execution descriptor construction failed: ") +
                        dt::CanonicalTypeName(type));
  return result.descriptor;
}

dt::DatatypeOperationValue Scalar(dt::CanonicalTypeId type,
                                  std::string bytes, bool is_null = false) {
  dt::DatatypeOperationValue value{type, std::move(bytes), is_null};
  value.descriptor = Descriptor(type);
  return value;
}

dt::CanonicalTypeId ResolvedClosedCastType(std::string_view label,
                                           std::size_t row_index) {
  if (!label.starts_with("base.") || label == "base.null")
    return static_cast<dt::CanonicalTypeId>(0xf0000000u + row_index);
  return dt::CanonicalTypeIdFromStableName(std::string(label.substr(5)));
}

dt::DatatypeOperationValue ClosedCastScalar(dt::CanonicalTypeId type) {
  std::string encoded;
  switch (type) {
    case dt::CanonicalTypeId::character:
      encoded = "0b1";
      break;
    case dt::CanonicalTypeId::int8:
      Check(dt::EncodeCanonicalInt8Value("1", &encoded), "int8 fixture");
      break;
    case dt::CanonicalTypeId::uint8:
      Check(dt::EncodeCanonicalUint8Value("1", &encoded), "uint8 fixture");
      break;
    case dt::CanonicalTypeId::int16:
      Check(dt::EncodeCanonicalInt16Value(1, &encoded), "int16 fixture");
      break;
    case dt::CanonicalTypeId::uint16:
      Check(dt::EncodeCanonicalUint16Value(1, &encoded), "uint16 fixture");
      break;
    case dt::CanonicalTypeId::int32:
      Check(dt::EncodeCanonicalInt32Value(1, &encoded), "int32 fixture");
      break;
    case dt::CanonicalTypeId::uint32:
      Check(dt::EncodeCanonicalUint32Value(1, &encoded), "uint32 fixture");
      break;
    case dt::CanonicalTypeId::int64:
      Check(dt::EncodeCanonicalInt64Value(1, &encoded), "int64 fixture");
      break;
    case dt::CanonicalTypeId::uint64:
      Check(dt::EncodeCanonicalUint64Value(1, &encoded), "uint64 fixture");
      break;
    case dt::CanonicalTypeId::int128:
      Check(dt::EncodeCanonicalInt128Value("1", &encoded), "int128 fixture");
      break;
    case dt::CanonicalTypeId::uint128:
      Check(dt::EncodeCanonicalUint128Value("1", &encoded), "uint128 fixture");
      break;
    default:
      break;
  }
  return Scalar(type, std::move(encoded));
}

void Operations() {
  auto profile = Profile();
  std::vector<platform::byte> a_bytes, b_bytes;
  const auto a = View(profile, "10101", &a_bytes);
  const auto b = View(profile, "11000", &b_bytes);
  Check(dt::BitStringLogicalLengthV3(a, true).unsigned_value == 5,
        "logical length failed");
  Check(dt::BitStringCountV3(a, true).unsigned_value == 3,
        "population count failed");
  Check(dt::BitStringAtV3(a, true, 0).boolean_value &&
            !dt::BitStringAtV3(a, true, 1).boolean_value,
        "zero-based bit access failed");
  Check(!dt::BitStringAtV3(a, true, 5).ok(),
        "out-of-range bit access was admitted");
  Check(IsBits(dt::BitStringSetV3(a, true, 1, true).value, "11101"),
        "bit set failed");
  Check(IsBits(dt::BitStringNotV3(a, true).value, "01010"), "NOT failed");
  Check(IsBits(dt::BitStringAndV3(a, b, true).value, "10000"), "AND failed");
  Check(IsBits(dt::BitStringOrV3(a, b, true).value, "11101"), "OR failed");
  Check(IsBits(dt::BitStringXorV3(a, b, true).value, "01101"), "XOR failed");
  Check(IsBits(dt::BitStringShiftLeftV3(a, true, 2).value, "10100"),
        "left shift failed");
  Check(IsBits(dt::BitStringShiftRightV3(a, true, 2).value, "00101"),
        "right shift failed");
  Check(IsBits(dt::BitStringShiftLeftV3(a, true, 99).value, "00000"),
        "overshift did not zero-fill");
  Check(IsBits(dt::BitStringSliceV3(a, profile, true, 1, 3).value, "010"),
        "contained slice failed");
  Check(IsBits(dt::BitStringConcatenateV3(a, b, profile, true).value,
               "1010111000"),
        "concatenation failed");
  std::vector<platform::byte> needle_bytes, absent_bytes, empty_bytes;
  const auto needle = View(profile, "101", &needle_bytes);
  const auto absent = View(profile, "111", &absent_bytes);
  const auto empty = View(profile, "", &empty_bytes);
  const auto found = dt::SearchBitStringValueNoAllocV3(a, needle, true);
  Check(found.ok() && found.found && found.zero_based_position == 0,
        "allocation-free logical search did not find first match");
  const auto missing = dt::SearchBitStringValueNoAllocV3(a, absent, true);
  Check(missing.ok() && !missing.found,
        "allocation-free logical search reported absent needle");
  Check(!dt::SearchBitStringValueNoAllocV3(a, empty, true).ok(),
        "allocation-free logical search admitted empty needle");

  dt::BitStringValueViewV3 typed_null{
      &profile, dt::BitStringValueStateV3::sql_null, 0, {},
      dt::BitStringOwnershipV3::borrowed};
  std::array<platform::byte, 1> dirty_tail{0x81};
  dt::BitStringValueViewV3 dirty_present{
      &profile, dt::BitStringValueStateV3::present, 1, dirty_tail,
      dt::BitStringOwnershipV3::borrowed};
  for (const auto result : {
           dt::BitStringAndV3(typed_null, dirty_present, true),
           dt::BitStringOrV3(typed_null, dirty_present, true),
           dt::BitStringXorV3(typed_null, dirty_present, true)})
    Check(result.ok() &&
              result.value.state == dt::BitStringValueStateV3::sql_null &&
              result.value.packed_msb0.empty(),
          "strict binary NULL inspected a PRESENT peer payload");
  const auto null_concat =
      dt::BitStringConcatenateV3(typed_null, dirty_present, profile, true);
  Check(null_concat.ok() &&
            null_concat.value.state == dt::BitStringValueStateV3::sql_null,
        "strict concatenate NULL inspected a PRESENT peer payload");
  const auto null_search =
      dt::SearchBitStringValueNoAllocV3(typed_null, dirty_present, true);
  Check(null_search.ok() && null_search.is_null,
        "strict search NULL inspected a PRESENT peer payload");
  auto invalid_profile = profile;
  invalid_profile.receipt.registry_generation = 5;
  auto invalid_profile_present = dirty_present;
  invalid_profile_present.profile = &invalid_profile;
  Check(!dt::BitStringAndV3(typed_null, invalid_profile_present, true).ok(),
        "strict binary NULL masked invalid peer profile");
  auto invalid_null = typed_null;
  invalid_null.packed_msb0 = dirty_tail;
  Check(dt::BitStringAndV3(invalid_null, dirty_present, true)
                .diagnostic.diagnostic_code == "DATATYPE.NULL_STATE.INVALID",
        "invalid strict NULL state did not precede PRESENT payload");
  Check(dt::DecodeCanonicalBitStringComponentNoAllocV3(
                invalid_profile, dt::BitStringValueStateV3::sql_null, true,
                dirty_tail)
                .diagnostic.diagnostic_code == "CTB.BIT.DESCRIPTOR_INVALID",
        "component decode state masked invalid profile");

  const auto render = dt::RenderBitStringValueV3(a, false);
  const auto export_render = dt::RenderBitStringValueV3(a, true);
  Check(render.ok() && render.text == "0b10101", "display rendering failed");
  Check(export_render.ok() && export_render.text == "B'10101'",
        "export rendering failed");
}

struct MetricCapture {
  unsigned calls = 0;
  bool accept = true;
  dt::BitStringMetricRecordV3 last;
};
bool CaptureMetric(const dt::BitStringMetricRecordV3& record,
                   void* context) noexcept {
  auto* capture = static_cast<MetricCapture*>(context);
  ++capture->calls;
  capture->last = record;
  return capture->accept;
}

void TypedMetricApi() {
  MetricCapture capture;
  dt::SetBitStringMetricSinkV3(CaptureMetric, &capture);
  dt::BitStringMetricRecordV3 record;
  record.database_uuid = scratchbird::tests::FixtureUuid(9902, 40);
  record.node_uuid = scratchbird::tests::FixtureUuid(9902, 41);
  record.represented_event_committed = true;
  record.metric = dt::BitStringMetricV3::operation_success;
  record.update = dt::BitStringMetricUpdateV3::counter_add;
  record.value = 1;
  record.operation = dt::BitStringMetricOperationV3::bit_count;
  Check(dt::RecordBitStringMetricAfterCommitV3(record) ==
            dt::BitStringMetricRecordDispositionV3::recorded &&
            capture.calls == 1,
        "typed operation metric was not recorded after commit");

  for (unsigned id = 0;
       id <= static_cast<unsigned>(dt::BitStringMetricV3::operation_input_bits);
       ++id) {
    dt::BitStringMetricRecordV3 row;
    row.database_uuid = record.database_uuid;
    row.node_uuid = record.node_uuid;
    row.represented_event_committed = true;
    row.metric = static_cast<dt::BitStringMetricV3>(id);
    row.value = 1;
    switch (row.metric) {
      case dt::BitStringMetricV3::descriptor_admissions:
        row.result = dt::BitStringMetricResultLabelV3::admitted; break;
      case dt::BitStringMetricV3::values_admitted:
        row.value_state = dt::BitStringMetricValueStateLabelV3::present; break;
      case dt::BitStringMetricV3::owned_bytes:
        row.update = dt::BitStringMetricUpdateV3::gauge_set;
        row.allocation_class = dt::BitStringMetricAllocationClassV3::owned_value; break;
      case dt::BitStringMetricV3::descriptor_refusals:
      case dt::BitStringMetricV3::padding_refusals:
      case dt::BitStringMetricV3::canonical_encoding_refusals:
      case dt::BitStringMetricV3::statistics_stale:
      case dt::BitStringMetricV3::merge_manual_review:
        row.reason = dt::BitStringMetricReasonV3::policy; break;
      case dt::BitStringMetricV3::length_refusals:
      case dt::BitStringMetricV3::operation_attempts:
      case dt::BitStringMetricV3::operation_success:
        row.operation = dt::BitStringMetricOperationV3::validate; break;
      case dt::BitStringMetricV3::operation_input_bits:
        row.operation = dt::BitStringMetricOperationV3::validate;
        row.update = dt::BitStringMetricUpdateV3::histogram_observe; break;
      case dt::BitStringMetricV3::logical_length:
        row.update = dt::BitStringMetricUpdateV3::histogram_observe; break;
      case dt::BitStringMetricV3::operation_refusals:
        row.operation = dt::BitStringMetricOperationV3::validate;
        row.reason = dt::BitStringMetricReasonV3::policy; break;
      case dt::BitStringMetricV3::index_admission_refusals:
        row.index_family = dt::BitStringMetricIndexFamilyV3::btree;
        row.reason = dt::BitStringMetricReasonV3::policy; break;
      case dt::BitStringMetricV3::compatibility_mapping_misses:
      case dt::BitStringMetricV3::transport_refusals:
        row.lane_class = dt::BitStringMetricLaneClassV3::canonical_sblr;
        row.reason = dt::BitStringMetricReasonV3::unsupported; break;
      case dt::BitStringMetricV3::serialization_refusals:
        row.boundary = dt::BitStringMetricBoundaryV3::component;
        row.reason = dt::BitStringMetricReasonV3::profile_missing; break;
      case dt::BitStringMetricV3::protection_refusals:
        row.layer = dt::BitStringMetricLayerV3::inner_encryption;
        row.reason = dt::BitStringMetricReasonV3::unsupported; break;
      default: break;
    }
    Check(dt::RecordBitStringMetricAfterCommitV3(row) ==
              dt::BitStringMetricRecordDispositionV3::recorded,
          "one of the exact 21 metric rows was refused");
  }

  const unsigned committed_calls = capture.calls;
  auto missing_result = record;
  missing_result.metric = dt::BitStringMetricV3::descriptor_admissions;
  missing_result.operation = dt::BitStringMetricOperationV3::not_applicable;
  Check(dt::RecordBitStringMetricAfterCommitV3(missing_result) ==
            dt::BitStringMetricRecordDispositionV3::label_combination_invalid,
        "descriptor admission metric accepted missing result label");
  missing_result.result = dt::BitStringMetricResultLabelV3::admitted;
  missing_result.reason = dt::BitStringMetricReasonV3::policy;
  Check(dt::RecordBitStringMetricAfterCommitV3(missing_result) ==
            dt::BitStringMetricRecordDispositionV3::label_combination_invalid,
        "descriptor admission metric accepted an undeclared extra label");
  auto invalid = record;
  invalid.reason = dt::BitStringMetricReasonV3::policy;
  Check(dt::RecordBitStringMetricAfterCommitV3(invalid) ==
            dt::BitStringMetricRecordDispositionV3::label_combination_invalid &&
            capture.calls == committed_calls,
        "undeclared metric-label Cartesian product was admitted");
  invalid = record;
  invalid.represented_event_committed = false;
  Check(dt::RecordBitStringMetricAfterCommitV3(invalid) ==
            dt::BitStringMetricRecordDispositionV3::event_not_committed,
        "metric advanced before represented event committed");
  invalid = record;
  invalid.cluster_series = true;
  Check(dt::RecordBitStringMetricAfterCommitV3(invalid) ==
            dt::BitStringMetricRecordDispositionV3::cluster_series_forbidden,
        "datatype runtime admitted a cluster metric series");
  invalid = record;
  invalid.metric = dt::BitStringMetricV3::logical_length;
  invalid.operation = dt::BitStringMetricOperationV3::not_applicable;
  invalid.update = dt::BitStringMetricUpdateV3::histogram_observe;
  invalid.value = dt::kBitStringMaximumLogicalBitsV3 + 1u;
  Check(dt::RecordBitStringMetricAfterCommitV3(invalid) ==
            dt::BitStringMetricRecordDispositionV3::update_invalid,
        "logical-length histogram admitted an out-of-domain observation");

  capture.accept = false;
  Check(dt::RecordBitStringMetricAfterCommitV3(record) ==
            dt::BitStringMetricRecordDispositionV3::sink_failure_isolated,
        "metric sink failure was not isolated");
  dt::SetBitStringMetricSinkV3(nullptr, nullptr);
}

void ComparisonHashAndKeys() {
  auto profile = Profile();
  std::vector<platform::byte> a_bytes, prefix_bytes, one_bytes;
  const auto a = View(profile, "10101", &a_bytes);
  const auto prefix = View(profile, "101", &prefix_bytes);
  const auto one = View(profile, "1", &one_bytes);
  const auto compare = dt::CompareBitStringValuesV3(
      prefix, a, dt::BitStringComparisonModeV3::ordered);
  Check(compare.ok() && compare.comparison < 0,
        "shorter exact prefix did not sort first");
  const auto equal = dt::CompareBitStringValuesV3(
      a, a, dt::BitStringComparisonModeV3::scalar_3vl);
  Check(equal.ok() && !equal.is_null && equal.boolean_value,
        "scalar equality failed");

  dt::BitStringValueViewV3 null_value{
      &profile, dt::BitStringValueStateV3::sql_null, 0, {},
      dt::BitStringOwnershipV3::borrowed};
  Check(dt::CompareBitStringValuesV3(
            a, null_value, dt::BitStringComparisonModeV3::scalar_3vl).is_null,
        "scalar NULL comparison did not return typed NULL state");
  Check(dt::CompareBitStringValuesV3(
            null_value, null_value,
            dt::BitStringComparisonModeV3::distinct).boolean_value,
        "two typed NULLs were not distinct-equivalent");
  Check(dt::CompareBitStringValuesV3(
            null_value, null_value,
            dt::BitStringComparisonModeV3::grouping).boolean_value,
        "grouping did not make two typed NULLs equivalent");
  Check(dt::CompareBitStringValuesV3(
            null_value, a,
            dt::BitStringComparisonModeV3::ordered).comparison < 0 &&
            dt::CompareBitStringValuesV3(
                a, null_value,
                dt::BitStringComparisonModeV3::ordered).comparison > 0,
        "ordered NULL placement drifted");
  Check(!dt::CompareBitStringValuesV3(
             null_value, a,
             dt::BitStringComparisonModeV3::distinct).boolean_value &&
            !dt::CompareBitStringValuesV3(
                 null_value, a,
                 dt::BitStringComparisonModeV3::grouping).boolean_value,
        "distinct/grouping NULL-versus-present modes drifted");
  Check(!dt::CompareBitStringValuesV3(
             null_value, null_value,
             dt::BitStringComparisonModeV3::present_only).ok(),
        "present-only comparator admitted NULL");
  std::array<platform::byte, 1> dirty_compare_tail{0x81};
  dt::BitStringValueViewV3 dirty_compare_present{
      &profile, dt::BitStringValueStateV3::present, 1, dirty_compare_tail,
      dt::BitStringOwnershipV3::borrowed};
  Check(dt::CompareBitStringValuesV3(
            null_value, dirty_compare_present,
            dt::BitStringComparisonModeV3::scalar_3vl).is_null &&
            !dt::CompareBitStringValuesV3(
                 null_value, dirty_compare_present,
                 dt::BitStringComparisonModeV3::distinct).boolean_value &&
            !dt::CompareBitStringValuesV3(
                 null_value, dirty_compare_present,
                 dt::BitStringComparisonModeV3::grouping).boolean_value &&
            dt::CompareBitStringValuesV3(
                 null_value, dirty_compare_present,
                 dt::BitStringComparisonModeV3::ordered).comparison < 0,
        "NULL comparison inspected a PRESENT peer payload");

  const auto hash = dt::HashBitStringValueV3(a);
  Check(hash.ok() && hash.bytes.size() == 32,
        "stable value hash failed");
  const auto null_hash = dt::HashBitStringValueV3(null_value);
  Check(null_hash.ok() && null_hash.bytes ==
            Hex("888ed669517b0e02c7c2b54aa15d9645bc2abbf71fa2cc8a73b35c85d5ffae17"),
        "sealed SQL NULL value hash drifted");
  const auto key = dt::MakeBitStringSortKeyV3(
      one, dt::BitStringSortDirectionV3::ascending,
      dt::BitStringNullModeV3::nulls_first);
  Check(key.ok() && key.bytes.size() == 106 &&
            std::equal(key.bytes.begin(), key.bytes.begin() + 8,
                       reinterpret_cast<const platform::byte*>("SBBITK01")) &&
            key.bytes[98] == 1 && key.bytes[104] == 2 && key.bytes[105] == 0,
        "SBBITK01 present key bytes drifted");
  const auto null_key = dt::MakeBitStringSortKeyV3(
      null_value, dt::BitStringSortDirectionV3::descending,
      dt::BitStringNullModeV3::nulls_last);
  Check(null_key.ok() && null_key.bytes.size() == 104 &&
            null_key.bytes[96] == 1 && null_key.bytes[97] == 1 &&
            null_key.bytes[98] == 2,
        "SBBITK01 NULL key bytes drifted");

  unsigned key_count=0;
  for(const auto& item:kSealedSortKeys){
    std::vector<platform::byte> packed=Pack(item.bits);
    dt::BitStringValueViewV3 value{&profile,item.is_null?dt::BitStringValueStateV3::sql_null:dt::BitStringValueStateV3::present,item.is_null?0u:static_cast<std::uint32_t>(item.bits.size()),item.is_null?std::span<const platform::byte>{}:std::span<const platform::byte>{packed},dt::BitStringOwnershipV3::borrowed};
      const auto actual=dt::MakeBitStringSortKeyV3(value,item.direction,item.null_mode);Check(actual.ok(),"one of 32 sort-key vectors was refused");
      Check(actual.bytes == Hex(item.hex),
            std::string(item.name) + " did not match exact sealed key bytes");
      const auto decoded_key=dt::DecodeBitStringSortKeyNoAllocV3(profile,actual.bytes);Check(decoded_key.ok()&&decoded_key.value.state==value.state,"one of 32 sort-key vectors did not decode");
      const std::size_t expected_size=104+(item.is_null?0:item.bits.size()+1);Check(actual.bytes.size()==expected_size,"sort-key vector extent drifted");
      Check(actual.bytes[96]==static_cast<unsigned>(item.direction)&&actual.bytes[97]==static_cast<unsigned>(item.null_mode),"sort-key direction/null mode drifted");
      const platform::byte rank=item.is_null?(item.null_mode==dt::BitStringNullModeV3::nulls_first?0:2):1;Check(actual.bytes[98]==rank,"sort-key state rank drifted");
      if(!item.is_null){for(std::size_t i=0;i<item.bits.size();++i){const platform::byte expected=item.direction==dt::BitStringSortDirectionV3::ascending?(item.bits[i]=='1'?2:1):(item.bits[i]=='1'?0xfd:0xfe);Check(actual.bytes[104+i]==expected,"sort-key logical symbol drifted");}Check(actual.bytes.back()==(item.direction==dt::BitStringSortDirectionV3::ascending?0:0xff),"sort-key terminator drifted");}
      ++key_count;
  }
  Check(key_count==32,"did not execute all 32 sealed sort-key vectors");
  auto valid_key=dt::MakeBitStringSortKeyV3(one,dt::BitStringSortDirectionV3::ascending,dt::BitStringNullModeV3::nulls_first).bytes;
  for(const std::size_t offset:{0u,8u,24u,32u,40u,72u,88u,98u,99u,104u,105u}){auto changed=valid_key;changed[offset]^=1;Check(!dt::DecodeBitStringSortKeyNoAllocV3(profile,changed).ok(),"mutated SBBITK01 field was admitted");}
  for (const std::size_t offset : {96u, 97u}) {
    auto changed = valid_key;
    changed[offset] = 0xff;
    Check(!dt::DecodeBitStringSortKeyNoAllocV3(profile, changed).ok(),
          "invalid SBBITK01 direction/null mode was admitted");
  }
  auto excess=valid_key;excess.push_back(0);Check(!dt::DecodeBitStringSortKeyNoAllocV3(profile,excess).ok(),"SBBITK01 trailing excess was admitted");
  auto fixed_profile = Profile(dt::BitStringSurfaceProfileKindV3::fixed, 8);
  auto varying_profile = Profile(dt::BitStringSurfaceProfileKindV3::varying, 1);
  Check(!dt::DecodeBitStringSortKeyNoAllocV3(fixed_profile, valid_key).ok(),
        "SBBITK01 reopened a fixed-profile count mismatch");
  Check(!dt::DecodeBitStringSortKeyNoAllocV3(varying_profile, valid_key).ok(),
        "SBBITK01 reopened a varying-profile bound mismatch");

  struct HashVector { dt::BitStringSurfaceProfileKindV3 kind;std::uint32_t bound;std::string_view bits;std::string_view hash; };
  static constexpr HashVector hashes[]={{dt::BitStringSurfaceProfileKindV3::unqualified,1,"","a600d648dff45bdbe9f423d558fceb4b3663fab00b9f1a36896085c5bfe1feba"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"0","cd5ab73c4975a3406b2a98bca43c3bfb3b2b06cf01c57c1742e9094ccf638748"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"1","bbfa485e1176188263e8d8d82073d359fefe8d0cd53f2770fb4e00573619539a"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"10","807c77f36f260b934c72c266bc5ae6121a210218d8b7e0ccffcd8fcf4c8523a0"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"101","316fe4769d04474ec0e2116cc32c8b881efaa284618278c89792f737723febc0"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"1010","e83ebc7ce38b05b477d941fc0e21a1aa9991fe508fba3911095cb073b4c6f3fc"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"10101","89ad692cde042165d5951b59bd5b9ede37d4ae733d89e66439a6f5f2e9549d8f"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"101010","64bbd0c16d4eed3d594003a379adfdad3a0730503481475a4afc257f55f76eae"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"1010101","ee116c3379e611190e6fca46d74d07203be56e423db4281a8f4f2b052d935b4a"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"10101010","30828dcc8cc52283512eb3e5ffe29b0a1208cb0321e879cacc1c43ca3fec5a3f"},{dt::BitStringSurfaceProfileKindV3::unqualified,1,"101010101","86af072f7f04ff9a400c948248a32a0362de06384aacce31f4dfb02082fe251a"},{dt::BitStringSurfaceProfileKindV3::fixed,8,"10100000","087cd3501d15fbdec40fa81f5a55e05b19c244676f5a96da1f862c1e6a1f416e"},{dt::BitStringSurfaceProfileKindV3::fixed,8,"11001010","b7c634d48cf69234c148976c850b5a96724bc4c1849ccb5014d2d66577416e53"},{dt::BitStringSurfaceProfileKindV3::varying,8,"","6d2c5a1f57faafc9acb93a6e2dd6d3a7b52495df85a9b5439f681b0a0bbdffdf"},{dt::BitStringSurfaceProfileKindV3::varying,8,"00110101","d7de01118d3726a446bf24b5eaf573abf0f04f1538945442979ed60fdfe1ee9d"}};
  for(const auto& fixture:hashes){auto p=Profile(fixture.kind,fixture.bound);auto packed=Pack(fixture.bits);dt::BitStringValueViewV3 value{&p,dt::BitStringValueStateV3::present,static_cast<std::uint32_t>(fixture.bits.size()),packed,dt::BitStringOwnershipV3::borrowed};const auto actual=dt::HashBitStringValueV3(value);Check(actual.ok()&&actual.bytes==Hex(fixture.hash),"sealed value-hash vector drifted");}

  std::vector<platform::byte> maximum(2'097'152,0xff);dt::BitStringValueViewV3 maximum_value{&profile,dt::BitStringValueStateV3::present,dt::kBitStringMaximumLogicalBitsV3,maximum,dt::BitStringOwnershipV3::borrowed};const auto maximum_hash=dt::HashBitStringValueV3(maximum_value);Check(maximum_hash.ok()&&maximum_hash.bytes==Hex("7dd37e8abfa3587147537577e141b45ab7f3fed8abf134bff9bacfd1ed32bfe2"),"maximum value hash drifted");const auto maximum_key=dt::MakeBitStringSortKeyV3(maximum_value,dt::BitStringSortDirectionV3::ascending,dt::BitStringNullModeV3::nulls_first);Check(maximum_key.ok()&&maximum_key.bytes.size()==dt::kBitStringMaximumSortKeyBytesV3,"maximum SBBITK01 extent drifted");Check(dt::DecodeBitStringSortKeyNoAllocV3(profile,maximum_key.bytes).ok(),"maximum SBBITK01 did not decode allocation-free");
}

void CastsAndClosedRegistry() {
  auto profile = Profile();
  auto fixed8 = Profile(dt::BitStringSurfaceProfileKindV3::fixed, 8);
  auto varying8 = Profile(dt::BitStringSurfaceProfileKindV3::varying, 8);
  auto text = Scalar(dt::CanonicalTypeId::character, "0b101");
  dt::BitStringCastRequestV3 to_bit;
  to_bit.scalar_source = &text;
  to_bit.bit_target = &profile;
  to_bit.context = dt::DatatypeCastContext::explicit_cast;
  auto cast = dt::CastBitStringValueV3(to_bit);
  Check(cast.ok() && IsBits(cast.bit_value, "101"),
        "strict character-to-bit cast failed");
  const auto expect_descriptor_refusal = [&](auto mutate,
                                             std::string_view message) {
    auto changed = text;
    mutate(changed.descriptor);
    to_bit.scalar_source = &changed;
    const auto result = dt::CastBitStringValueV3(to_bit);
    Check(!result.ok() && result.diagnostic.diagnostic_code ==
                              "CTB.BIT.DESCRIPTOR_INVALID", message);
  };
  expect_descriptor_refusal([](auto& d){d.descriptor_uuid.bytes[0]^=1;},"mutated scalar descriptor UUID admitted");
  expect_descriptor_refusal([](auto& d){++d.descriptor_epoch;},"mutated scalar descriptor epoch admitted");
  expect_descriptor_refusal([](auto& d){++d.canonical_type_id;},"mutated scalar canonical type admitted");
  expect_descriptor_refusal([](auto& d){d.family=engine::ExecutionTypeFamily::unknown;},"mutated scalar family admitted");
  expect_descriptor_refusal([](auto& d){d.width_class=engine::ExecutionTypeWidthClass::unknown;},"mutated scalar width class admitted");
  auto localized_alias = text;
  localized_alias.descriptor.stable_name = "chaine_de_caracteres";
  to_bit.scalar_source = &localized_alias;
  Check(dt::CastBitStringValueV3(to_bit).ok(),
        "presentation/localized scalar alias was treated as identity");
  to_bit.scalar_source = &text;
  expect_descriptor_refusal([](auto& d){++d.bit_width;},"mutated scalar bit width admitted");
  expect_descriptor_refusal([](auto& d){++d.precision;},"mutated scalar precision admitted");
  expect_descriptor_refusal([](auto& d){++d.scale;},"mutated scalar scale admitted");
  expect_descriptor_refusal([](auto& d){d.length=16'777'217;d.modifier_flags=engine::ExecutionTypeModifierFlagBit(engine::ExecutionTypeModifierFlag::length);},"out-of-bound scalar length admitted");
  expect_descriptor_refusal([](auto& d){++d.vector_dimensions;},"mutated scalar vector dimensions admitted");
  expect_descriptor_refusal([](auto& d){++d.container_rank;},"mutated scalar container rank admitted");
  expect_descriptor_refusal([](auto& d){d.modifier_flags=~std::uint64_t{0};},"mutated scalar modifier flags admitted");
  expect_descriptor_refusal([](auto& d){d.domain_uuid.bytes[0]=1;},"mutated scalar domain admitted");
  expect_descriptor_refusal([](auto& d){engine::Uuid u{};u.bytes[0]=1;d.domain_stack.push_back(u);},"mutated scalar domain stack admitted");
  expect_descriptor_refusal([](auto& d){d.charset_uuid.bytes[0]=1;},"mutated scalar charset admitted");
  expect_descriptor_refusal([](auto& d){d.collation_uuid.bytes[0]=1;},"mutated scalar collation admitted");
  expect_descriptor_refusal([](auto& d){d.timezone_uuid.bytes[0]=1;},"mutated scalar timezone admitted");
  expect_descriptor_refusal([](auto& d){d.element_descriptor_uuid.bytes[0]=1;},"mutated scalar element descriptor admitted");
  expect_descriptor_refusal([](auto& d){d.security_policy_uuid.bytes[0]=1;},"mutated scalar security policy admitted");
  expect_descriptor_refusal([](auto& d){d.nullable_allowed=false;},"mutated scalar nullability admitted");
  expect_descriptor_refusal([](auto& d){d.descriptor_authoritative=false;},"non-authoritative scalar descriptor admitted");
  expect_descriptor_refusal([](auto& d){d.parser_independent=false;},"parser-dependent scalar descriptor admitted");
  auto short_declared = text;
  short_declared.descriptor.length = 4;
  short_declared.descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(engine::ExecutionTypeModifierFlag::length);
  to_bit.scalar_source = &short_declared;
  const auto too_long = dt::CastBitStringValueV3(to_bit);
  Check(!too_long.ok() && too_long.diagnostic.diagnostic_code ==
                            "CTB.BIT.LENGTH_EXCEEDED",
        "character source declared length was ignored");
  for (std::string bad : {"0B1", " 0b1", "0b2", "0b1_0", "0b\xc2\xb9"}) {
    auto value = Scalar(dt::CanonicalTypeId::character, bad);
    to_bit.scalar_source = &value;
    const auto refused = dt::CastBitStringValueV3(to_bit);
    Check(!refused.ok() && refused.diagnostic.diagnostic_code ==
                               "CTB.BIT.CAST_TEXT_INVALID",
          "malformed strict bit text was admitted or misdiagnosed");
  }
  to_bit.scalar_source = &text;
  to_bit.context = dt::DatatypeCastContext::implicit;
  Check(!dt::CastBitStringValueV3(to_bit).ok(),
        "character-to-bit implicit cast was admitted");

  std::string int_bytes;
  Check(dt::EncodeCanonicalUint64Value(5, &int_bytes),
        "uint64 fixture encoding failed");
  auto integer = Scalar(dt::CanonicalTypeId::uint64, int_bytes);
  to_bit.scalar_source = &integer;
  to_bit.bit_target = &fixed8;
  to_bit.context = dt::DatatypeCastContext::explicit_cast;
  cast = dt::CastBitStringValueV3(to_bit);
  Check(cast.ok() && IsBits(cast.bit_value, "00000101"),
        "integer-to-fixed-bit left extension failed");

  std::vector<platform::byte> bits;
  const auto bit5 = View(profile, "101", &bits);
  std::array<platform::byte, 1> dirty_cast_tail{0x81};
  dt::BitStringValueViewV3 dirty_cast_source{
      &profile, dt::BitStringValueStateV3::present, 1, dirty_cast_tail,
      dt::BitStringOwnershipV3::borrowed};
  dt::BitStringCastRequestV3 forbidden_dirty_cast;
  forbidden_dirty_cast.bit_source = &dirty_cast_source;
  forbidden_dirty_cast.scalar_target = dt::CanonicalTypeId::boolean;
  forbidden_dirty_cast.scalar_target_descriptor =
      Descriptor(dt::CanonicalTypeId::boolean);
  forbidden_dirty_cast.context = dt::DatatypeCastContext::explicit_cast;
  Check(dt::CastBitStringValueV3(forbidden_dirty_cast)
                .diagnostic.diagnostic_code == "DATATYPE.CAST_FORBIDDEN",
        "forbidden pair classification inspected PRESENT payload");
  dt::BitStringCastRequestV3 forbidden_dirty_profile_cast;
  forbidden_dirty_profile_cast.bit_source = &dirty_cast_source;
  forbidden_dirty_profile_cast.bit_target = &fixed8;
  forbidden_dirty_profile_cast.context = dt::DatatypeCastContext::implicit;
  Check(dt::CastBitStringValueV3(forbidden_dirty_profile_cast)
                .diagnostic.diagnostic_code == "DATATYPE.CAST_FORBIDDEN",
        "profile cast context classification inspected PRESENT payload");
  dt::BitStringCastRequestV3 to_integer;
  to_integer.bit_source = &bit5;
  to_integer.scalar_target = dt::CanonicalTypeId::uint64;
  to_integer.scalar_target_descriptor = Descriptor(dt::CanonicalTypeId::uint64);
  to_integer.context = dt::DatatypeCastContext::explicit_cast;
  const auto integer_result = dt::CastBitStringValueV3(to_integer);
  std::uint64_t decoded = 0;
  Check(integer_result.ok() &&
            dt::DecodeCanonicalUint64Value(
                integer_result.scalar_value.encoded_value, &decoded) &&
            decoded == 5,
        "bit-to-integer magnitude cast failed");
  std::string negative_bytes;
  Check(dt::EncodeCanonicalInt8Value("-1", &negative_bytes),
        "negative int8 fixture encoding failed");
  auto negative_integer =
      Scalar(dt::CanonicalTypeId::int8, std::move(negative_bytes));
  auto negative_request = to_bit;
  negative_request.scalar_source = &negative_integer;
  negative_request.bit_target = &profile;
  negative_request.context = dt::DatatypeCastContext::explicit_cast;
  Check(dt::CastBitStringValueV3(negative_request)
                .diagnostic.diagnostic_code == "SCALAR.OUT_OF_RANGE",
        "negative integer cast lost its range diagnostic");
  std::vector<platform::byte> empty_integer_bits;
  const auto empty_integer = View(profile, "", &empty_integer_bits);
  auto empty_integer_request = to_integer;
  empty_integer_request.bit_source = &empty_integer;
  Check(dt::CastBitStringValueV3(empty_integer_request)
                .diagnostic.diagnostic_code == "SCALAR.OUT_OF_RANGE",
        "empty bit-to-integer cast lost its range diagnostic");
  dt::BitStringCastRequestV3 to_character;
  to_character.bit_source = &bit5;
  to_character.scalar_target = dt::CanonicalTypeId::character;
  to_character.scalar_target_descriptor = Descriptor(dt::CanonicalTypeId::character);
  to_character.context = dt::DatatypeCastContext::explicit_cast;
  Check(dt::CastBitStringValueV3(to_character).ok(),
        "bit-to-character cast failed");
  to_character.scalar_target_descriptor.length = 4;
  to_character.scalar_target_descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(engine::ExecutionTypeModifierFlag::length);
  const auto target_short = dt::CastBitStringValueV3(to_character);
  Check(!target_short.ok() && target_short.diagnostic.diagnostic_code ==
                                  "CTB.BIT.LENGTH_EXCEEDED",
        "character target declared length was ignored");

  std::vector<dt::DatatypeOperationValue> integer_sources;
  for (const auto type : {dt::CanonicalTypeId::int8, dt::CanonicalTypeId::int16,
                          dt::CanonicalTypeId::int32, dt::CanonicalTypeId::int64,
                          dt::CanonicalTypeId::int128, dt::CanonicalTypeId::uint8,
                          dt::CanonicalTypeId::uint16, dt::CanonicalTypeId::uint32,
                          dt::CanonicalTypeId::uint64, dt::CanonicalTypeId::uint128}) {
    std::string encoded;
    bool ok = false;
    switch (type) {
      case dt::CanonicalTypeId::int8: ok=dt::EncodeCanonicalInt8Value("5",&encoded);break;
      case dt::CanonicalTypeId::uint8: ok=dt::EncodeCanonicalUint8Value("5",&encoded);break;
      case dt::CanonicalTypeId::int16: ok=dt::EncodeCanonicalInt16Value(5,&encoded);break;
      case dt::CanonicalTypeId::uint16: ok=dt::EncodeCanonicalUint16Value(5,&encoded);break;
      case dt::CanonicalTypeId::int32: ok=dt::EncodeCanonicalInt32Value(5,&encoded);break;
      case dt::CanonicalTypeId::uint32: ok=dt::EncodeCanonicalUint32Value(5,&encoded);break;
      case dt::CanonicalTypeId::int64: ok=dt::EncodeCanonicalInt64Value(5,&encoded);break;
      case dt::CanonicalTypeId::uint64: ok=dt::EncodeCanonicalUint64Value(5,&encoded);break;
      case dt::CanonicalTypeId::int128: ok=dt::EncodeCanonicalInt128Value("5",&encoded);break;
      case dt::CanonicalTypeId::uint128: ok=dt::EncodeCanonicalUint128Value("5",&encoded);break;
      default: break;
    }
    Check(ok,"integer cast fixture encoding failed");
    integer_sources.push_back(Scalar(type,std::move(encoded)));
  }
  unsigned integer_directions=0;
  for (const auto& source_integer : integer_sources) {
    dt::BitStringCastRequestV3 incoming;
    incoming.scalar_source=&source_integer;incoming.bit_target=&profile;
    incoming.context=dt::DatatypeCastContext::explicit_cast;
    const auto as_bits=dt::CastBitStringValueV3(incoming);
    Check(as_bits.ok()&&IsBits(as_bits.bit_value,"101"),"one of ten explicit integer-to-bit directions failed");
    for(auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment}){incoming.context=context;Check(!dt::CastBitStringValueV3(incoming).ok(),"integer-to-bit non-explicit context admitted");}
    incoming.context=dt::DatatypeCastContext::explicit_cast;
    dt::BitStringCastRequestV3 outgoing;
    const auto as_bits_view=as_bits.bit_value.view();outgoing.bit_source=&as_bits_view;
    outgoing.scalar_target=source_integer.type_id;outgoing.scalar_target_descriptor=source_integer.descriptor;outgoing.context=dt::DatatypeCastContext::explicit_cast;
    Check(dt::CastBitStringValueV3(outgoing).ok(),"one of ten explicit bit-to-integer directions failed");
    for(auto context:{dt::DatatypeCastContext::implicit,dt::DatatypeCastContext::assignment}){outgoing.context=context;Check(!dt::CastBitStringValueV3(outgoing).ok(),"bit-to-integer non-explicit context admitted");}
    integer_directions+=2;
  }
  Check(integer_directions==20,"did not execute all 20 explicit integer directions");

  dt::BitStringCastRequestV3 profile_cast;
  profile_cast.bit_source = &bit5;
  profile_cast.bit_target = &fixed8;
  profile_cast.context = dt::DatatypeCastContext::explicit_cast;
  const auto padded = dt::CastBitStringValueV3(profile_cast);
  Check(padded.ok() && IsBits(padded.bit_value, "10100000"),
        "ordinary fixed profile right padding failed");
  profile_cast.bit_target = &varying8;
  Check(dt::CastBitStringValueV3(profile_cast).ok(),
        "explicit varying profile conversion failed");

  Check(std::size(kClosedCastPolicy) == dt::kBitStringClosedCastPolicyRowsV3,
        "compiled closed cast fixture row count drifted");
  const std::array contexts{
      dt::DatatypeCastContext::implicit,
      dt::DatatypeCastContext::assignment,
      dt::DatatypeCastContext::explicit_cast};
  unsigned executed_paths = 0;
  const auto blob_profile = dt::BuildCurrentBlobValidatedProfileHandleV3(
      dt::kBlobV11ReceiptUuid);
  Check(blob_profile.ok(), "native BLOB peer profile unavailable");
  const dt::BlobMaterializedValueViewV3 blob_source{
      &blob_profile.profile, dt::BlobValueStateV3::value, 0, {}};
  for (std::size_t row_index = 0; row_index < std::size(kClosedCastPolicy);
       ++row_index) {
    const auto& row = kClosedCastPolicy[row_index];
    const bool source_is_bit = row.source_label == "base.bit_string";
    const bool target_is_bit = row.target_label == "base.bit_string";
    Check(source_is_bit != target_is_bit ||
              (source_is_bit && target_is_bit),
          std::string(row.row_id) + " is not incident to bit_string");
    const auto source_type = ResolvedClosedCastType(row.source_label, row_index);
    const auto target_type = ResolvedClosedCastType(row.target_label, row_index);
    if (row.identities_resolved) {
      Check((source_is_bit || source_type != dt::CanonicalTypeId::unknown) &&
                (target_is_bit || target_type != dt::CanonicalTypeId::unknown),
            std::string(row.row_id) + " failed compiled identity resolution");
    }
    dt::DatatypeOperationValue scalar_source;
    if (!source_is_bit && source_type != dt::CanonicalTypeId::blob) {
      if (row.identities_resolved)
        scalar_source = ClosedCastScalar(source_type);
      else
        scalar_source.type_id = source_type;
    }
    for (std::size_t context_index = 0; context_index < contexts.size();
         ++context_index) {
      if (source_type == dt::CanonicalTypeId::blob || target_type == dt::CanonicalTypeId::blob) {
        dt::BitStringBlobClosedCastRequestV3 native;
        native.context = contexts[context_index];
        if (source_is_bit) {
          native.bit_source = &bit5; native.blob_target = &blob_profile.profile;
        } else {
          native.blob_source = &blob_source; native.bit_target = &profile;
        }
        const auto actual = dt::AdmitBitStringBlobClosedCastV3(native);
        const auto* fact = std::get_if<dt::BitStringBlobCastForbiddenV3>(&actual.diagnostic);
        Check(row.contexts[context_index] == ClosedCastExpectation::forbidden &&
                  !actual.status.ok() && fact && fact->cast_context == context_index + 1 &&
                  fact->reason == 84 && fact->source_type.kind == 1 && fact->target_type.kind == 1 &&
                  fact->source_type.type_generation == 1 && fact->target_type.type_generation == 1 &&
                  fact->source_type.type_uuid == (source_is_bit ? profile.identity.legacy_fields.type_uuid : blob_profile.profile.identity.type_uuid) &&
                  fact->target_type.type_uuid == (source_is_bit ? blob_profile.profile.identity.type_uuid : profile.identity.legacy_fields.type_uuid),
              "native BLOB closed matrix cell failed");
        ++executed_paths;
        continue;
      }
      dt::BitStringCastRequestV3 request;
      request.context = contexts[context_index];
      if (source_is_bit)
        request.bit_source = &bit5;
      else
        request.scalar_source = &scalar_source;
      if (target_is_bit) {
        request.bit_target = &profile;
      } else {
        request.scalar_target = target_type;
        if (row.identities_resolved)
          request.scalar_target_descriptor = Descriptor(target_type);
      }
      const auto actual = dt::CastBitStringValueV3(request);
      const auto expected = row.contexts[context_index];
      const auto message = std::string(row.row_id) + " context " +
          std::to_string(context_index) + " disagreed with closed registry; actual=" +
          std::string(actual.diagnostic.diagnostic_code);
      if (expected == ClosedCastExpectation::identity) {
        Check(actual.ok() &&
                  actual.category == dt::DatatypeCastCategory::identity &&
                  actual.produced_bit_string && IsBits(actual.bit_value, "101"),
              message);
      } else if (expected == ClosedCastExpectation::explicit_admitted) {
        Check(actual.ok() &&
                  actual.category ==
                      dt::DatatypeCastCategory::lossless_explicit,
              message);
      } else if (expected == ClosedCastExpectation::descriptor_unresolved) {
        Check(!actual.ok() && actual.diagnostic.diagnostic_code ==
                                  "CTB.BIT.DESCRIPTOR_INVALID",
              message);
      } else {
        Check(!actual.ok() && actual.diagnostic.diagnostic_code ==
                                  "DATATYPE.CAST_FORBIDDEN",
              message);
      }
      ++executed_paths;
    }
  }
  Check(executed_paths == 221u * 3u,
        "not every closed cast row/context was executed");
}

bool CancelOnSecondCheck(void* state) noexcept {
  auto* calls = static_cast<unsigned*>(state);
  return ++*calls >= 2;
}

void NativeBlobClosedPair() {
  const auto profile = Profile();
  const auto built = dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid);
  Check(built.ok(), "native BLOB closed-pair profile");
  auto blob_profile = built.profile;
  std::vector<platform::byte> bits;
  auto bit = View(profile, "101", &bits);
  const auto original_bits = bits;
  dt::BlobMaterializedValueViewV3 blob{
      &blob_profile, dt::BlobValueStateV3::value, 0, {}};
  dt::BitStringBlobClosedCastRequestV3 incoming, outgoing;
  incoming.blob_source = &blob; incoming.bit_target = &profile;
  outgoing.bit_source = &bit; outgoing.blob_target = &blob_profile;
  const auto refused = [&](const dt::BitStringBlobClosedCastRequestV3& request,
                           std::string_view expected) {
    const auto result = dt::AdmitBitStringBlobClosedCastV3(request);
    Check(!result.status.ok() && bits == original_bits,
          "native BLOB pair status or input changed");
    using K = dt::BitStringBlobAdmissionFailureKindV3;
    if (const auto* failure = std::get_if<dt::BitStringBlobAdmissionFailureV3>(&result.diagnostic)) {
      if (expected == "shape") Check(failure->kind == K::shape, "native request shape gate");
      else if (expected == "context") Check(failure->kind == K::context, "native context gate");
      else if (expected == "bit_profile") Check(failure->kind == K::bit_profile, "native bit profile gate");
      else if (expected == "bit_state") Check(failure->kind == K::bit_state, "native bit state gate");
      else {
        auto diagnostic = dt::BlobStructuralDiagnosticV3::descriptor_invalid;
        if (expected == "BLOB.STATE_INVALID") diagnostic = dt::BlobStructuralDiagnosticV3::state_invalid;
        else if (expected == "BLOB.LENGTH_EXCEEDED") diagnostic = dt::BlobStructuralDiagnosticV3::length_exceeded;
        else if (expected == "extent") diagnostic = dt::BlobStructuralDiagnosticV3::canonical_encoding_invalid;
        else Check(expected == "CINL.LOB.DESCRIPTOR_INVALID", "unexpected admission refusal");
        Check(failure->kind == K::blob_structure && failure->blob_diagnostic == diagnostic,
              "native BLOB structural diagnostic lost");
      }
    } else if (const auto* fact = std::get_if<dt::BitStringBlobCastForbiddenV3>(&result.diagnostic)) {
      Check(expected == "DATATYPE.CAST_FORBIDDEN", "unexpected closed-pair refusal");
      const auto context = request.context == dt::DatatypeCastContext::implicit ? 1 :
                           request.context == dt::DatatypeCastContext::assignment ? 2 : 3;
      Check(fact->source_type.kind == 1 && fact->target_type.kind == 1 &&
                fact->source_type.type_uuid == (request.blob_source ? blob_profile.identity.type_uuid : profile.identity.legacy_fields.type_uuid) &&
                fact->target_type.type_uuid == (request.blob_source ? profile.identity.legacy_fields.type_uuid : blob_profile.identity.type_uuid) &&
                fact->source_type.type_generation == 1 && fact->target_type.type_generation == 1 &&
                fact->cast_context == context && fact->reason == 84,
            "native typed CastTypeRefV1/context/reason vector");
    } else if (const auto* fact = std::get_if<dt::BitStringBlobNullNotAdmittedV3>(&result.diagnostic)) {
      Check(expected == "DATATYPE.NULL_NOT_ADMITTED" &&
                fact->descriptor_ref.descriptor_uuid == (request.blob_source ? profile.identity.legacy_fields.descriptor_uuid : blob_profile.identity.descriptor_uuid) &&
                fact->descriptor_ref.descriptor_generation == 1 && fact->boundary == 1 && fact->reason == 1,
            "native typed NULL descriptor reference vector");
    } else {
      const auto* cancelled = std::get_if<dt::BitStringBlobCastCancelledV3>(&result.diagnostic);
      Check(expected == "PROCESS.CANCELLED" && cancelled &&
                cancelled->operation_enum == 7 && cancelled->phase_enum == 1,
            "native typed pre-retain cancellation vector");
    }
  };
  const auto both = [&](std::string_view code) {
    refused(incoming, code); refused(outgoing, code);
  };
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    incoming.context = outgoing.context = context;
    incoming.control.maximum_allocation_bytes = outgoing.control.maximum_allocation_bytes = 0;
    both("DATATYPE.CAST_FORBIDDEN");
    blob.state = dt::BlobValueStateV3::sql_null;
    bit.state = dt::BitStringValueStateV3::sql_null;
    bit.logical_bit_count = 0; bit.packed_msb0 = {};
    both("DATATYPE.CAST_FORBIDDEN");
    incoming.target_null_allowed = outgoing.target_null_allowed = false;
    both("DATATYPE.NULL_NOT_ADMITTED");
    incoming.target_null_allowed = outgoing.target_null_allowed = true;
    blob.state = dt::BlobValueStateV3::value;
    bit = View(profile, "101", &bits);
  }
  for (std::size_t byte = 0; byte < 16; ++byte) {
    blob_profile.identity.descriptor_uuid.bytes[byte] ^= 1;
    both("CINL.LOB.DESCRIPTOR_INVALID"); blob_profile = built.profile;
    blob_profile.receipt.catalog_snapshot_uuid.bytes[byte] ^= 1;
    both("CINL.LOB.DESCRIPTOR_INVALID"); blob_profile = built.profile;
    blob_profile.profile_uuid.bytes[byte] ^= 1;
    both("CINL.LOB.DESCRIPTOR_INVALID"); blob_profile = built.profile;
  }
  for (std::size_t index = 0; index < dt::kBlobPolicyBindingCountV3; ++index) {
    blob_profile.policy_bindings[index].generation += 1;
    both("CINL.LOB.DESCRIPTOR_INVALID"); blob_profile = built.profile;
    blob_profile.policy_bindings[index].uuid.bytes[0] ^= 1;
    both("CINL.LOB.DESCRIPTOR_INVALID"); blob_profile = built.profile;
  }
  for (auto* generation : {&blob_profile.receipt.catalog_generation,
                           &blob_profile.receipt.registry_generation,
                           &blob_profile.identity.descriptor_generation,
                           &blob_profile.profile_generation}) {
    ++*generation; both("CINL.LOB.DESCRIPTOR_INVALID"); --*generation;
  }
  for (std::size_t byte = 0; byte < blob_profile.profile_fingerprint.size(); ++byte) {
    blob_profile.profile_fingerprint[byte] ^= 1;
    both("CINL.LOB.DESCRIPTOR_INVALID"); blob_profile = built.profile;
  }
  auto invalid_bit_profile = profile;
  invalid_bit_profile.receipt.catalog_generation += 1;
  incoming.bit_target = &invalid_bit_profile;
  refused(incoming, "bit_profile"); incoming.bit_target = &profile;
  bit.profile = &invalid_bit_profile;
  refused(outgoing, "bit_profile"); bit.profile = &profile;
  bit.state = static_cast<dt::BitStringValueStateV3>(99);
  refused(outgoing, "bit_state");
  bit.state = dt::BitStringValueStateV3::sql_null;
  refused(outgoing, "bit_state"); // Dirty NULL retains original count/payload.
  bit.state = dt::BitStringValueStateV3::present;
  const auto owned_fact = dt::AdmitBitStringBlobClosedCastV3(incoming);
  blob_profile.identity.type_uuid.bytes[0] ^= 1;
  Check(std::get<dt::BitStringBlobCastForbiddenV3>(owned_fact.diagnostic).source_type.type_uuid ==
            built.profile.identity.type_uuid, "native refusal borrowed mutable identity");
  blob_profile = built.profile;
  blob.profile = nullptr;
  refused(incoming, "CINL.LOB.DESCRIPTOR_INVALID"); blob.profile = &blob_profile;
  blob.state = static_cast<dt::BlobValueStateV3>(99);
  refused(incoming, "BLOB.STATE_INVALID");
  blob.state = dt::BlobValueStateV3::sql_null; blob.logical_length = 1;
  refused(incoming, "BLOB.STATE_INVALID");
  blob.state = dt::BlobValueStateV3::value;
  refused(incoming, "extent");
  const std::array<platform::byte, 1> raw_blob{0x55};
  blob.bytes = raw_blob;
  refused(incoming, "CINL.LOB.DESCRIPTOR_INVALID"); // No lifetime authority.
  Check(raw_blob[0] == 0x55, "closed cast touched raw BLOB content");
  blob.logical_length = dt::kBlobMaximumLogicalBytesV3 + 1;
  refused(incoming, "BLOB.LENGTH_EXCEEDED");
  blob.logical_length = 0; blob.bytes = {};
  unsigned cancellation_calls = 0;
  const auto cancel = [](void* context) noexcept {
    ++*static_cast<unsigned*>(context); return true;
  };
  incoming.control.cancelled = outgoing.control.cancelled = cancel;
  incoming.control.cancellation_context = outgoing.control.cancellation_context = &cancellation_calls;
  both("PROCESS.CANCELLED");
  Check(cancellation_calls == 2, "closed pair cancellation not checked exactly once");
  incoming.control.cancelled = outgoing.control.cancelled = nullptr;
  incoming.bit_source = &bit;
  refused(incoming, "shape"); incoming.bit_source = nullptr;
  outgoing.blob_source = &blob;
  refused(outgoing, "shape"); outgoing.blob_source = nullptr;
  incoming.context = outgoing.context = static_cast<dt::DatatypeCastContext>(99);
  both("context");
}
struct CancelAtCall {
  unsigned calls = 0;
  unsigned cancel_at = 1;
};
bool CancelAt(void* state) noexcept {
  auto* control = static_cast<CancelAtCall*>(state);
  return ++control->calls >= control->cancel_at;
}

void ResourceCancellationAndGenericRefusal() {
  auto profile = Profile();
  std::vector<platform::byte> storage((65'537 + 7) / 8, 0xaa);
  storage.back() &= 0x80;
  dt::BitStringValueViewV3 large{
      &profile, dt::BitStringValueStateV3::present, 65'537, storage,
      dt::BitStringOwnershipV3::borrowed};
  unsigned calls = 0;
  dt::BitStringExecutionControlV3 control;
  control.cancelled = CancelOnSecondCheck;
  control.cancellation_context = &calls;
  const auto cancelled = dt::BitStringNotV3(large, true, control);
  Check(!cancelled.ok() && cancelled.value.packed_msb0.empty() &&
            cancelled.diagnostic.diagnostic_code == "PROCESS.CANCELLED",
        "cancellation did not preserve atomic publication");
  std::vector<platform::byte> exact_storage(65'536 / 8, 0xaa);
  dt::BitStringValueViewV3 exact{
      &profile, dt::BitStringValueStateV3::present, 65'536, exact_storage,
      dt::BitStringOwnershipV3::borrowed};
  const auto require_atomic_checkpoint =
      [&](auto operation, std::string_view message) {
        CancelAtCall state{0, 2};
        dt::BitStringExecutionControlV3 c;
        c.cancelled = CancelAt;
        c.cancellation_context = &state;
        const auto result = operation(c);
        Check(!result.ok() && result.diagnostic.diagnostic_code ==
                                  "PROCESS.CANCELLED" &&
                  result.value.packed_msb0.empty() && state.calls == 2,
              message);
      };
  require_atomic_checkpoint(
      [&](const auto& c) { return dt::BitStringSetV3(exact, true, 0, true, c); },
      "bit set missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) {
        return dt::BitStringConcatenateV3(exact, exact, profile, true, c);
      },
      "concatenate missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) {
        return dt::BitStringSliceV3(exact, profile, true, 0, 65'536, c);
      },
      "slice missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) { return dt::BitStringNotV3(exact, true, c); },
      "NOT missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) { return dt::BitStringAndV3(exact, exact, true, c); },
      "AND missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) { return dt::BitStringOrV3(exact, exact, true, c); },
      "OR missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) { return dt::BitStringXorV3(exact, exact, true, c); },
      "XOR missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) { return dt::BitStringShiftLeftV3(exact, true, 1, c); },
      "left shift missed exact cancellation/atomicity");
  require_atomic_checkpoint(
      [&](const auto& c) { return dt::BitStringShiftRightV3(exact, true, 1, c); },
      "right shift missed exact cancellation/atomicity");
  CancelAtCall count_state{0, 2};
  dt::BitStringExecutionControlV3 count_control;
  count_control.cancelled = CancelAt;
  count_control.cancellation_context = &count_state;
  Check(dt::BitStringCountV3(exact, true, count_control)
                .diagnostic.diagnostic_code == "PROCESS.CANCELLED" &&
            count_state.calls == 2,
        "bit count missed the exact 65,536-bit checkpoint");
  CancelAtCall materialize_state{0, 2};
  auto materialize_control = count_control;
  materialize_control.cancellation_context = &materialize_state;
  const auto materialize_cancelled =
      dt::MaterializeBitStringValueV3(exact, true, materialize_control);
  Check(!materialize_cancelled.ok() &&
            materialize_cancelled.value.packed_msb0.empty() &&
            materialize_state.calls == 2,
        "materialization missed the exact 65,536-bit checkpoint");
  CancelAtCall component_state{0, 2};
  auto component_control = count_control;
  component_control.cancellation_context = &component_state;
  const auto component_cancelled =
      dt::EncodeCanonicalBitStringComponentV3(exact, component_control);
  Check(!component_cancelled.ok() && component_cancelled.bytes.empty() &&
            component_state.calls == 2,
        "component encode missed the exact 65,536-bit checkpoint");

  dt::BitStringCastRequestV3 bit_to_character;
  bit_to_character.bit_source = &exact;
  bit_to_character.scalar_target = dt::CanonicalTypeId::character;
  bit_to_character.scalar_target_descriptor =
      Descriptor(dt::CanonicalTypeId::character);
  bit_to_character.context = dt::DatatypeCastContext::explicit_cast;
  bit_to_character.control.maximum_allocation_bytes = 65'537;
  const auto cast_budget = dt::CastBitStringValueV3(bit_to_character);
  Check(!cast_budget.ok() && cast_budget.diagnostic.diagnostic_code ==
                                 "RESOURCE.BUDGET_EXCEEDED" &&
            cast_budget.scalar_value.encoded_value.empty(),
        "bit-to-character cast ignored its allocation grant");
  CancelAtCall outgoing_cast_state{0, 2};
  bit_to_character.control.maximum_allocation_bytes = ~std::uint64_t{0};
  bit_to_character.control.cancelled = CancelAt;
  bit_to_character.control.cancellation_context = &outgoing_cast_state;
  const auto outgoing_cast_cancelled =
      dt::CastBitStringValueV3(bit_to_character);
  Check(!outgoing_cast_cancelled.ok() &&
            outgoing_cast_cancelled.diagnostic.diagnostic_code ==
                "PROCESS.CANCELLED" &&
            outgoing_cast_cancelled.scalar_value.encoded_value.empty() &&
            outgoing_cast_state.calls == 2,
        "bit-to-character cast missed its exact checkpoint");
  CancelAtCall preallocation_cast_state{0, 1};
  bit_to_character.control.cancellation_context = &preallocation_cast_state;
  const auto preallocation_cast_cancelled =
      dt::CastBitStringValueV3(bit_to_character);
  Check(!preallocation_cast_cancelled.ok() &&
            preallocation_cast_cancelled.diagnostic.diagnostic_code ==
                "PROCESS.CANCELLED" &&
            preallocation_cast_cancelled.scalar_value.encoded_value.empty() &&
            preallocation_cast_state.calls == 1,
        "cast preallocation cancellation published output");

  std::string large_text = "0b" + std::string(65'536, '1');
  auto large_character =
      Scalar(dt::CanonicalTypeId::character, std::move(large_text));
  dt::BitStringCastRequestV3 character_to_bit;
  character_to_bit.scalar_source = &large_character;
  character_to_bit.bit_target = &profile;
  character_to_bit.context = dt::DatatypeCastContext::explicit_cast;
  character_to_bit.control.maximum_allocation_bytes = 8'191;
  const auto incoming_budget = dt::CastBitStringValueV3(character_to_bit);
  Check(!incoming_budget.ok() && incoming_budget.diagnostic.diagnostic_code ==
                                     "RESOURCE.BUDGET_EXCEEDED" &&
            incoming_budget.bit_value.packed_msb0.empty(),
        "character-to-bit cast ignored its allocation grant");
  CancelAtCall incoming_cast_state{0, 4};
  character_to_bit.control.maximum_allocation_bytes = ~std::uint64_t{0};
  character_to_bit.control.cancelled = CancelAt;
  character_to_bit.control.cancellation_context = &incoming_cast_state;
  const auto incoming_cast_cancelled =
      dt::CastBitStringValueV3(character_to_bit);
  Check(!incoming_cast_cancelled.ok() &&
            incoming_cast_cancelled.diagnostic.diagnostic_code ==
                "PROCESS.CANCELLED" &&
            incoming_cast_cancelled.bit_value.packed_msb0.empty() &&
            incoming_cast_state.calls == 4,
        "character-to-bit publication missed its exact checkpoint");

  dt::DatatypeOperationValue raw{dt::CanonicalTypeId::bit_string,
                                 std::string(1, static_cast<char>(0x80)), false};
  dt::DatatypeCastRequest generic_cast;
  generic_cast.value = raw;
  generic_cast.target_type_id = dt::CanonicalTypeId::character;
  generic_cast.context = dt::DatatypeCastContext::explicit_cast;
  Check(!dt::CastDatatypeValue(generic_cast).ok(),
        "generic cast accepted raw bit bytes");
  Check(!dt::CompareDatatypeValues({raw, raw}).ok(),
        "generic comparison accepted raw bit bytes");
  Check(!dt::MakeDatatypeSortKey({raw}).ok(),
        "generic sort key accepted raw bit bytes");
  Check(!dt::HashDatatypeValue({raw}).ok(),
        "generic hash accepted raw bit bytes");
  Check(!dt::SerializeDatatypeValue({raw}).ok(),
        "generic SBDV1 serializer accepted raw bit bytes");
  Check(!dt::RenderDatatypeValueForDisplay({raw}).ok(),
        "generic render accepted raw bit bytes");
  auto dirty_null = raw;
  dirty_null.is_null = true;
  const auto dirty_cast = dt::CastDatatypeValue(
      {dirty_null, dt::CanonicalTypeId::character,
       dt::DatatypeCastContext::explicit_cast});
  Check(!dirty_cast.ok() && dirty_cast.diagnostic.diagnostic_code ==
                              "DATATYPE.NULL_STATE.INVALID" &&
            dt::CompareDatatypeValues({dirty_null, raw}).diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            dt::MakeDatatypeSortKey({dirty_null}).diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            dt::HashDatatypeValue({dirty_null}).diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            dt::SerializeDatatypeValue({dirty_null}).diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID",
        "raw payload-bearing bit NULL lost common state precedence");
  auto bad_descriptor_dirty_null = dirty_null;
  bad_descriptor_dirty_null.descriptor =
      Descriptor(dt::CanonicalTypeId::bit_string);
  ++bad_descriptor_dirty_null.descriptor.descriptor_epoch;
  const auto bad_descriptor_cast = dt::CastDatatypeValue(
      {bad_descriptor_dirty_null, dt::CanonicalTypeId::character,
       dt::DatatypeCastContext::explicit_cast});
  Check(!bad_descriptor_cast.ok() &&
            bad_descriptor_cast.diagnostic.diagnostic_code ==
                "CTB.BIT.DESCRIPTOR_INVALID" &&
            dt::CompareDatatypeValues({bad_descriptor_dirty_null, raw})
                    .diagnostic.diagnostic_code ==
                "CTB.BIT.DESCRIPTOR_INVALID" &&
            dt::MakeDatatypeSortKey({bad_descriptor_dirty_null})
                    .diagnostic.diagnostic_code ==
                "CTB.BIT.DESCRIPTOR_INVALID" &&
            dt::HashDatatypeValue({bad_descriptor_dirty_null})
                    .diagnostic.diagnostic_code ==
                "CTB.BIT.DESCRIPTOR_INVALID" &&
            dt::SerializeDatatypeValue({bad_descriptor_dirty_null})
                    .diagnostic.diagnostic_code ==
                "CTB.BIT.DESCRIPTOR_INVALID" &&
            dt::RenderDatatypeValueForDisplay({bad_descriptor_dirty_null})
                    .diagnostic.diagnostic_code ==
                "CTB.BIT.DESCRIPTOR_INVALID",
        "raw bit descriptor did not precede dirty-NULL state");
}

}  // namespace

int main() {
  Operations();
  ComparisonHashAndKeys();
  CastsAndClosedRegistry();
  NativeBlobClosedPair();
  ResourceCancellationAndGenericRefusal();
  TypedMetricApi();
  std::cout << "base.bit_string operation/cast checks: " << checks << '\n';
  return EXIT_SUCCESS;
}
