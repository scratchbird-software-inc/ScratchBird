// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_operations.hpp"
#include "datatype_wire_metadata.hpp"
#include "domain_support/domain_store.hpp"
#include "catalog/sys_information_projection.hpp"
#include "catalog/wire_driver_metadata_api.hpp"
#include "metric_contracts.hpp"
#include "metric_registry.hpp"
#include "query/expression_api.hpp"
#include "runtime_capabilities.hpp"
#include "sbl_numeric.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace info = scratchbird::engine::internal_api;
namespace metrics = scratchbird::core::metrics;
namespace numeric = scratchbird::libraries::sbl_numeric;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

dt::WireUuidBytes WireUuid(std::uint8_t seed) {
  dt::WireUuidBytes value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

dt::NullableText PresentText(std::string text) {
  dt::NullableText value;
  value.state = dt::WireNullableTextState::present;
  value.text = std::move(text);
  return value;
}

dt::CanonicalTypeRef TypeRefFor(dt::CanonicalTypeId type_id, std::uint8_t uuid_seed) {
  const auto descriptor = dt::LookupDatatypeDescriptor(type_id);
  Require(descriptor.ok(), "test requested unknown descriptor");
  dt::CanonicalTypeRef type_ref;
  type_ref.canonical_type_id = dt::WireTypeIdForCanonicalTypeId(type_id);
  type_ref.descriptor_uuid = WireUuid(uuid_seed);
  type_ref.descriptor_version = 1;
  if (descriptor.descriptor.default_precision != 0) {
    type_ref.modifier_bitmap |= dt::CanonicalTypeRefModifierBit(
        dt::CanonicalTypeRefModifier::precision);
    type_ref.precision = descriptor.descriptor.default_precision;
  }
  type_ref.modifier_bitmap |= dt::CanonicalTypeRefModifierBit(
      dt::CanonicalTypeRefModifier::scale);
  type_ref.scale = descriptor.descriptor.default_scale;
  if (descriptor.descriptor.requires_mandatory_library) {
    type_ref.modifier_bitmap |= dt::CanonicalTypeRefModifierBit(
        dt::CanonicalTypeRefModifier::backend_profile_required);
  }
  return type_ref;
}

std::string ProjectionField(const info::SysInformationProjectionRow& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return field.second; }
  }
  return {};
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TempDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sb_p3_datatype_numeric_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

dt::DatatypeOperationValue Value(dt::CanonicalTypeId type, std::string encoded) {
  return {type, std::move(encoded), false};
}

void RequireMetricOk(const metrics::MetricValidationResult& result, std::string_view message) {
  if (!result.ok) {
    std::cerr << result.diagnostic_code << ":" << result.detail << '\n';
  }
  Require(result.ok, message);
}

void RequireDiagnosticDetail(const api::EngineApiResult& result,
                             std::string_view expected_detail,
                             std::string_view message) {
  Require(!result.ok, message);
  Require(!result.diagnostics.empty(), "expected diagnostic vector was empty");
  const auto& actual = result.diagnostics.front().detail;
  const bool matches = actual == expected_detail ||
                       (actual.size() >= expected_detail.size() &&
                        actual.compare(actual.size() - expected_detail.size(),
                                       expected_detail.size(),
                                       expected_detail) == 0);
  if (!matches) {
    std::cerr << "expected=" << expected_detail
              << " actual=" << actual << '\n';
  }
  Require(matches, message);
}

void TestMandatoryDatatypeCapabilities() {
  const auto platform_check = platform::CheckMandatoryRuntimeCapabilities();
  if (!platform_check.ok()) {
    for (const auto& diagnostic : platform_check.diagnostics) {
      std::cerr << diagnostic.diagnostic_code << '\n';
    }
  }
  Require(platform_check.ok(), "mandatory runtime capabilities are not present");

  const auto datatype_check = dt::CheckDatatypeMandatoryCapabilities(platform_check.manifest);
  if (!datatype_check.ok()) {
    for (const auto& diagnostic : datatype_check.diagnostics) {
      std::cerr << diagnostic.diagnostic_code << '\n';
    }
  }
  Require(datatype_check.ok(), "mandatory datatype capabilities are not present");

  std::set<std::string> required;
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    const auto validated = dt::ValidateDatatypeDescriptor(descriptor);
    Require(validated.ok(), "builtin datatype descriptor is invalid");
    if (descriptor.requires_mandatory_library) {
      required.insert(descriptor.required_capability_key);
    }
  }
  Require(required.count("numeric.int128") == 1, "int128 capability was not required");
  Require(required.count("numeric.uint128") == 1, "uint128 capability was not required");
  Require(required.count("numeric.decimal") == 1, "decimal capability was not required");
  Require(required.count("numeric.decimal_float") == 1, "decimal_float capability was not required");
  Require(required.count("numeric.real128") == 1, "real128 capability was not required");
}

void TestNumericBackend() {
  numeric::NumericRequest request;
  request.type = numeric::NumericType::decimal;
  request.operation = numeric::NumericOperation::canonicalize;
  request.left = {numeric::NumericType::decimal, "123.455", false};
  request.context.precision = 38;
  request.context.scale = 2;
  request.context.rounding = numeric::RoundingMode::half_even;
  auto result = numeric::ApplyNumericOperation(request);
  Require(result.status == numeric::NumericStatusCode::ok, "decimal canonicalize failed");
  Require(result.value.encoded == "123.46", "decimal half-even canonicalization mismatch");

  request.operation = numeric::NumericOperation::add;
  request.left.encoded = "99999999999999999999999999999999999999";
  request.right = {numeric::NumericType::decimal, "1", false};
  request.context.scale = 0;
  result = numeric::ApplyNumericOperation(request);
  Require(result.status == numeric::NumericStatusCode::overflow, "decimal overflow was not detected");

  request.operation = numeric::NumericOperation::divide;
  request.left.encoded = "10";
  request.right.encoded = "0";
  result = numeric::ApplyNumericOperation(request);
  Require(result.status == numeric::NumericStatusCode::divide_by_zero, "divide by zero was not detected");

  request.type = numeric::NumericType::decimal_float;
  request.operation = numeric::NumericOperation::canonicalize;
  request.left = {numeric::NumericType::decimal_float, "nan", false};
  request.context.allow_special_values = true;
  result = numeric::ApplyNumericOperation(request);
  Require(result.status == numeric::NumericStatusCode::ok, "decimal_float NaN canonicalize failed");
  Require(result.value.encoded == "NaN", "decimal_float NaN canonical form mismatch");

  request.type = numeric::NumericType::real128;
  request.operation = numeric::NumericOperation::add;
  request.left = {numeric::NumericType::real128, "1.25", false};
  request.right = {numeric::NumericType::real128, "2.5", false};
  result = numeric::ApplyNumericOperation(request);
  Require(result.status == numeric::NumericStatusCode::ok, "real128 backend add failed");
  Require(result.value.encoded.find("3.75") == 0, "real128 add result mismatch");
}

void TestDatatypeOperations() {
  dt::DatatypeNumericOperationRequest numeric_request;
  numeric_request.operation = dt::DatatypeNumericOperationKind::multiply;
  numeric_request.type_id = dt::CanonicalTypeId::decimal;
  numeric_request.left = Value(dt::CanonicalTypeId::decimal, "12.50");
  numeric_request.right = Value(dt::CanonicalTypeId::decimal, "2");
  numeric_request.context.precision = 38;
  numeric_request.context.scale = 2;
  auto numeric_result = dt::ApplyNumericOperation(numeric_request);
  Require(numeric_result.ok(), "datatype numeric operation failed");
  Require(numeric_result.value.encoded_value == "25.00", "datatype numeric backend result mismatch");

  dt::DatatypeCastRequest cast;
  cast.value = Value(dt::CanonicalTypeId::character, "170141183460469231731687303715884105727");
  cast.target_type_id = dt::CanonicalTypeId::int128;
  cast.explicit_cast = true;
  auto cast_result = dt::CastDatatypeValue(cast);
  Require(cast_result.ok(), "int128 max cast failed");
  Require(cast_result.value.encoded_value == "170141183460469231731687303715884105727",
          "int128 max cast value mismatch");

  cast.value.encoded_value = "170141183460469231731687303715884105728";
  cast_result = dt::CastDatatypeValue(cast);
  Require(!cast_result.ok(), "int128 overflow cast was accepted");

  cast.value.encoded_value = "340282366920938463463374607431768211455";
  cast.target_type_id = dt::CanonicalTypeId::uint128;
  cast_result = dt::CastDatatypeValue(cast);
  Require(cast_result.ok(), "uint128 max cast failed");

  cast.value.encoded_value = "340282366920938463463374607431768211456";
  cast_result = dt::CastDatatypeValue(cast);
  Require(!cast_result.ok(), "uint128 overflow cast was accepted");

  dt::DatatypeComparisonRequest compare;
  compare.left = Value(dt::CanonicalTypeId::character, "Alpha");
  compare.right = Value(dt::CanonicalTypeId::character, "alpha");
  compare.case_insensitive_character_compare = true;
  compare.text_seed.active = true;
  compare.text_seed.seed_pack_name = "initial-resource-pack";
  compare.text_seed.seed_pack_version = "1";
  compare.text_seed.charset_name = "UTF-8";
  compare.text_seed.collation_name = "unicode_ci";
  compare.text_seed.collation_case_insensitive = true;
  auto comparison = dt::CompareDatatypeValues(compare);
  Require(comparison.ok(), "case-insensitive comparison with seed authority failed");
  Require(comparison.comparison == 0, "case-insensitive comparison result mismatch");

  compare.text_seed.active = false;
  comparison = dt::CompareDatatypeValues(compare);
  Require(!comparison.ok(), "case-insensitive comparison without seed authority was accepted");
}

void TestDsr023WireTypeAndMetadataRoundTrip() {
  const auto int128_wire = dt::WireTypeIdForCanonicalTypeId(dt::CanonicalTypeId::int128);
  Require(int128_wire.type_family ==
              static_cast<std::uint16_t>(dt::CanonicalWireTypeFamily::signed_integer),
          "int128 wire family mismatch");
  Require(int128_wire.type_code == 5, "int128 wire code mismatch");
  Require(int128_wire.type_version == 1, "int128 wire version mismatch");
  Require(dt::CanonicalTypeIdFromWireTypeId(int128_wire) == dt::CanonicalTypeId::int128,
          "int128 wire reverse mapping mismatch");

  const auto uint128_wire = dt::WireTypeIdForCanonicalTypeId(dt::CanonicalTypeId::uint128);
  Require(uint128_wire.type_family ==
              static_cast<std::uint16_t>(dt::CanonicalWireTypeFamily::unsigned_integer),
          "uint128 wire family mismatch");
  Require(uint128_wire.type_code == 5, "uint128 wire code mismatch");

  const auto real128_wire = dt::WireTypeIdForCanonicalTypeId(dt::CanonicalTypeId::real128);
  Require(real128_wire.type_family ==
              static_cast<std::uint16_t>(dt::CanonicalWireTypeFamily::approximate_numeric),
          "real128 wire family mismatch");
  Require(real128_wire.type_code == 4, "real128 wire code mismatch");

  auto type_ref = TypeRefFor(dt::CanonicalTypeId::real128, 0x20);
  type_ref.modifier_bitmap |= dt::CanonicalTypeRefModifierBit(
      dt::CanonicalTypeRefModifier::timezone_policy);
  type_ref.timezone_policy = 1;
  type_ref.modifier_bitmap |= dt::CanonicalTypeRefModifierBit(
      dt::CanonicalTypeRefModifier::calendar_policy);
  type_ref.calendar_policy = 1;
  type_ref.modifier_bitmap |= dt::CanonicalTypeRefModifierBit(
      dt::CanonicalTypeRefModifier::numeric_locale_policy);
  type_ref.numeric_locale_policy = 1;
  const auto encoded = dt::EncodeCanonicalTypeRef(type_ref);
  Require(encoded.size() == dt::kCanonicalTypeRefBytes, "CanonicalTypeRef byte size mismatch");
  const auto decoded = dt::DecodeCanonicalTypeRef(encoded.data(), encoded.size());
  Require(decoded.ok, "CanonicalTypeRef decode failed");
  Require(decoded.type_ref.precision == 113, "real128 precision metadata mismatch");
  Require((decoded.type_ref.modifier_bitmap &
           dt::CanonicalTypeRefModifierBit(dt::CanonicalTypeRefModifier::backend_profile_required)) != 0,
          "real128 backend profile metadata bit missing");

  auto reserved = int128_wire;
  reserved.type_flags = 1u << 15;
  const auto invalid = dt::ValidateCanonicalWireTypeId(reserved);
  Require(!invalid.ok &&
              invalid.diagnostic_code == "NATIVE_WIRE.TYPE.CANONICAL_ID_RESERVED_BITS",
          "reserved CanonicalTypeId flags were not refused");
}

void TestDsr023ParameterValueStates() {
  dt::ParameterDataPacket packet;
  packet.slot_descriptors = {
      {.ordinal = 1,
       .direction = dt::WireParameterDirection::in,
       .type_ref = TypeRefFor(dt::CanonicalTypeId::character, 0x31),
       .name = PresentText("p_text")},
      {.ordinal = 2,
       .direction = dt::WireParameterDirection::in,
       .type_ref = TypeRefFor(dt::CanonicalTypeId::int128, 0x41),
       .name = PresentText("p_default")},
      {.ordinal = 3,
       .direction = dt::WireParameterDirection::out,
       .type_ref = TypeRefFor(dt::CanonicalTypeId::uint128, 0x51),
       .name = PresentText("p_out")}};

  dt::ParameterRowValueFrame row;
  row.row_ordinal = 0;
  row.values.push_back({.slot_ordinal = 1,
                        .value_state = dt::WireValueState::value_present,
                        .payload_encoding = dt::WirePayloadEncoding::utf8_text,
                        .payload = {}});
  row.values.push_back({.slot_ordinal = 2,
                        .value_state = dt::WireValueState::default_requested,
                        .payload_encoding = dt::WirePayloadEncoding::canonical_binary,
                        .payload = {}});
  row.values.push_back({.slot_ordinal = 3,
                        .value_state = dt::WireValueState::out_unbound,
                        .payload_encoding = dt::WirePayloadEncoding::canonical_binary,
                        .payload = {}});
  packet.row_value_frames.push_back(std::move(row));

  const auto encoded = dt::EncodeParameterDataPacket(packet);
  Require(encoded.ok, "ParameterDataPacket encode failed");
  const auto decoded = dt::DecodeParameterDataPacket(encoded.bytes);
  Require(decoded.ok, "ParameterDataPacket decode failed");
  Require(decoded.packet.row_value_frames.front().values.front().value_state ==
              dt::WireValueState::value_present,
          "empty string was not preserved as value_present");
  Require(decoded.packet.row_value_frames.front().values[1].value_state ==
              dt::WireValueState::default_requested,
          "default_requested state did not round-trip");
  Require(decoded.packet.row_value_frames.front().values[2].value_state ==
              dt::WireValueState::out_unbound,
          "OUT parameter out_unbound state did not round-trip");

  auto bad_packet = packet;
  bad_packet.row_value_frames.front().values[2].value_state = dt::WireValueState::value_present;
  bad_packet.row_value_frames.front().values[2].payload = {'x'};
  const auto bad = dt::EncodeParameterDataPacket(bad_packet);
  Require(!bad.ok && bad.diagnostic_code == "NATIVE_WIRE.PARAMETER.OUT_BIND_HAS_VALUE",
          "OUT-only input value was not refused");
}

void TestDsr023RowDescriptionDiscriminators() {
  dt::RowDescriptionPacket generated;
  generated.result_set_kind = dt::WireResultSetKind::generated_keys;
  dt::ResultColumnDescriptor generated_column;
  generated_column.ordinal = 1;
  generated_column.column_class = dt::WireResultColumnClass::generated_key;
  generated_column.nullability = dt::WireNullability::not_nullable;
  generated_column.metadata_bitmap =
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::display_label_present) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::type_name_present) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::precision_valid) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::scale_valid) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::identity_autoincrement_metadata_present) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::generated_key_discriminator_active);
  generated_column.type_ref = TypeRefFor(dt::CanonicalTypeId::int128, 0x61);
  generated_column.generated_value_kind = dt::WireGeneratedValueKind::identity;
  generated_column.display_label = PresentText("id");
  generated_column.type_name = PresentText("int128");
  generated.columns.push_back(generated_column);
  const auto generated_encoded = dt::EncodeRowDescriptionPacket(generated);
  Require(generated_encoded.ok, "generated-key RowDescription encode failed");
  const auto generated_decoded = dt::DecodeRowDescriptionPacket(generated_encoded.bytes);
  Require(generated_decoded.ok, "generated-key RowDescription decode failed");
  Require(generated_decoded.packet.result_set_kind == dt::WireResultSetKind::generated_keys,
          "generated-key result_set_kind did not round-trip");

  dt::RowDescriptionPacket out_row;
  out_row.result_set_kind = dt::WireResultSetKind::out_parameter_row;
  dt::ResultColumnDescriptor out_column;
  out_column.ordinal = 1;
  out_column.column_class = dt::WireResultColumnClass::out_parameter;
  out_column.metadata_bitmap =
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::display_label_present) |
      dt::WireMetadataBit(dt::WireMetadataBitmapBit::out_inout_return_discriminator_active);
  out_column.type_ref = TypeRefFor(dt::CanonicalTypeId::uint128, 0x71);
  out_column.source_parameter_ordinal = 3;
  out_column.parameter_direction = dt::WireParameterDirection::inout;
  out_column.display_label = PresentText("p_out");
  out_row.columns.push_back(out_column);
  const auto out_encoded = dt::EncodeRowDescriptionPacket(out_row);
  Require(out_encoded.ok, "OUT/INOUT RowDescription encode failed");

  generated.columns.front().metadata_bitmap &=
      ~dt::WireMetadataBit(dt::WireMetadataBitmapBit::generated_key_discriminator_active);
  const auto bad_generated = dt::EncodeRowDescriptionPacket(generated);
  Require(!bad_generated.ok &&
              bad_generated.diagnostic_code ==
                  "NATIVE_WIRE.RESULT.GENERATED_KEYS_DISCRIMINATOR_MISSING",
          "generated key discriminator omission was not refused");
}

void TestDsr023DriverAndSysInformationMetadata() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "descriptor-int128";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int128";
  descriptor.encoded_descriptor = "canonical=int128;precision=128;scale=0;nullable=false";
  const auto metadata = api::RenderWireDriverMetadata(descriptor);
  Require(metadata.canonical_type_family == "signed_integer",
          "driver metadata canonical family mismatch");
  Require(metadata.canonical_type_code == "int128", "driver metadata canonical code mismatch");
  Require(metadata.canonical_type_code_id == 5, "driver metadata wire code mismatch");
  Require(metadata.precision == 128, "driver metadata int128 precision mismatch");
  Require(metadata.scale == 0, "driver metadata int128 scale mismatch");
  Require(metadata.backend_profile == "sbl_numeric:numeric.int128",
          "driver metadata int128 backend profile mismatch");
  Require(metadata.nullability == "NO", "driver metadata nullability mismatch");
  Require(metadata.metadata_projection_source ==
              "sys.information.scratchbird_datatype_descriptors",
          "driver metadata projection source mismatch");

  info::SysInformationProjectionContext context;
  context.catalog_display_name = "DriverDB";
  context.visible_catalog_generation_id = 10;
  info::SysInformationDatatypeDescriptorSource visible;
  visible.type_catalog = "DriverDB";
  visible.type_schema = "sys";
  visible.type_name = "int128";
  visible.standard_type_name = "int128";
  visible.canonical_type_family = "signed_integer";
  visible.canonical_type_code = "int128";
  visible.driver_family = "native";
  visible.native_type_code = "2:5";
  visible.precision = "128";
  visible.scale = "0";
  visible.display_size = "40";
  visible.numeric_precision_radix = "2";
  visible.is_nullable = "NO";
  visible.is_signed = "YES";
  visible.is_case_sensitive = "NO";
  visible.is_searchable = "BASIC";
  visible.is_currency = "NO";
  visible.is_auto_increment_capable = "YES";
  visible.compatibility_class = "native_or_better";
  visible.support_state = "supported";
  visible.backend_profile = "sbl_numeric:numeric.int128";
  visible.catalog_generation_id = 2;

  auto hidden = visible;
  hidden.type_name = "hidden_int128";
  hidden.hidden = true;
  std::vector<info::SysInformationDatatypeDescriptorSource> sources = {visible, hidden};
  const std::vector<info::SysInformationCommentSource> comments;
  const auto projected = info::BuildSysInformationProjection(
      "sys.information_schema.scratchbird_datatype_descriptors",
      context,
      {},
      {},
      comments,
      sources);
  Require(projected.ok, "scratchbird datatype descriptor projection failed");
  Require(projected.rows.size() == 1, "hidden datatype descriptor row was not omitted");
  Require(ProjectionField(projected.rows.front(), "type_name") == "int128",
          "datatype descriptor visible row mismatch");
  Require(ProjectionField(projected.rows.front(), "backend_profile") ==
              "sbl_numeric:numeric.int128",
          "datatype descriptor backend profile mismatch");
  for (const auto& field : projected.rows.front().fields) {
    Require(!info::SysInformationProjectionColumnNameExposesUuid(field.first),
            "datatype descriptor projection exposed UUID-shaped column");
    Require(field.second.find("descriptor-int128") == std::string::npos,
            "datatype descriptor projection exposed descriptor identity");
  }
}

std::string CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(platform::UuidKind::database, 1779810301000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, 1779810301001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810301002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "datatype/domain database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext BaseDomainContext(const std::filesystem::path& path,
                                            const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "p3-domain-method";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = "principal-p3-domain";
  context.session_uuid.canonical = "session-p3-domain";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("role:ROOT");
  return context;
}

api::EngineRequestContext BeginDomainContext(const std::filesystem::path& path,
                                             const std::string& database_uuid) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseDomainContext(path, database_uuid);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "domain transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

api::DomainRecord Domain(std::uint64_t creator_tx,
                         std::string uuid,
                         std::string name,
                         std::string method_binding) {
  api::DomainRecord record;
  record.creator_tx = creator_tx;
  record.domain_uuid = std::move(uuid);
  record.catalog_row_uuid = "row-" + record.domain_uuid;
  record.schema_uuid = "schema-p3-domain";
  record.default_name = std::move(name);
  record.base_descriptor_uuid = "descriptor-character";
  record.base_descriptor_kind = "scalar";
  record.base_canonical_type_name = "character";
  record.base_encoded_descriptor = "canonical=character";
  record.nullable = false;
  record.method_binding_envelope = std::move(method_binding);
  record.validation_hook_status = "builtin";
  return record;
}

void TestDomainMethodBinding(const std::filesystem::path& path, const std::string& database_uuid) {
  const auto context = BeginDomainContext(path, database_uuid);

  const auto no_method_domain = Domain(context.local_transaction_id, "domain-no-method", "no_method_domain", {});
  auto diagnostic = api::AppendDomainEvent(context, api::MakeDomainCreateEvent(no_method_domain));
  Require(!diagnostic.error, "domain without method seed failed");
  Require(api::FindVisibleDomain(context, no_method_domain.domain_uuid, context.local_transaction_id).has_value(),
          "seeded domain without method is not visible");

  api::EngineInvokeDomainMethodRequest invoke;
  invoke.context = context;
  invoke.domain_descriptor = api::DomainDescriptor(no_method_domain);
  invoke.input_value.descriptor.canonical_type_name = "character";
  invoke.input_value.encoded_value = "alpha";
  invoke.method_name = "upper";
  auto invoked = api::EngineInvokeDomainMethod(invoke);
  RequireDiagnosticDetail(invoked, "domain_method_not_declared",
                          "domain method call without binding did not fail closed");

  const auto upper_domain = Domain(context.local_transaction_id, "domain-upper-method", "upper_domain", "builtin:upper");
  diagnostic = api::AppendDomainEvent(context, api::MakeDomainCreateEvent(upper_domain));
  Require(!diagnostic.error, "domain with builtin method seed failed");
  invoke.domain_descriptor = api::DomainDescriptor(upper_domain);
  invoked = api::EngineInvokeDomainMethod(invoke);
  Require(invoked.ok, "domain builtin method invocation failed");
  Require(invoked.value.encoded_value == "ALPHA", "domain upper method result mismatch");

  RequireMetricOk(metrics::RecordDomainMethodInvocation("domain-upper-method", "upper", "ok", "none"),
                  "domain method metric failed");
}

void TestDatatypeMetrics() {
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_datatype_operation_total") != nullptr,
          "datatype operation metric descriptor missing");
  Require(metrics::DefaultMetricRegistry().FindDescriptor("sb_datatype_catalog_descriptors") != nullptr,
          "datatype catalog metric descriptor missing");
  RequireMetricOk(metrics::RecordDatatypeOperation("decimal", "canonicalize", "ok", "none"),
                  "datatype operation metric publish failed");
  RequireMetricOk(metrics::RecordDatatypeCast("character", "int128", "overflow", "integer_out_of_range"),
                  "datatype cast metric publish failed");
  RequireMetricOk(metrics::RecordDatatypeNumericBackend("sbl_numeric", "real128", "add", "ok", "none"),
                  "datatype numeric backend metric publish failed");
  RequireMetricOk(metrics::PublishDatatypeCatalogDescriptorCount(
                      static_cast<double>(dt::BuiltinDatatypeDescriptors().size()), "ok"),
                  "datatype catalog descriptor metric publish failed");

  bool saw_operation = false;
  bool saw_backend = false;
  bool saw_catalog = false;
  bool saw_domain = false;
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    saw_operation = saw_operation || value.family == "sb_datatype_operation_total";
    saw_backend = saw_backend || value.family == "sb_datatype_numeric_backend_total";
    saw_catalog = saw_catalog || value.family == "sb_datatype_catalog_descriptors";
    saw_domain = saw_domain || value.family == "sb_domain_method_invocation_total";
  }
  Require(saw_operation, "datatype operation metric snapshot missing");
  Require(saw_backend, "datatype numeric backend metric snapshot missing");
  Require(saw_catalog, "datatype catalog metric snapshot missing");
  Require(saw_domain, "domain method metric snapshot missing");
}

}  // namespace

int main() {
  const auto path = TempDatabasePath();
  const auto database_uuid = CreateDatabase(path);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + ".sb.crud_events", ignored);
      std::filesystem::remove(path.string() + ".sb.domain_events", ignored);
    }
  } cleanup{path};

  TestMandatoryDatatypeCapabilities();
  TestNumericBackend();
  TestDatatypeOperations();
  TestDsr023WireTypeAndMetadataRoundTrip();
  TestDsr023ParameterValueStates();
  TestDsr023RowDescriptionDiscriminators();
  TestDsr023DriverAndSysInformationMetadata();
  TestDomainMethodBinding(path, database_uuid);
  TestDatatypeMetrics();
  return EXIT_SUCCESS;
}
