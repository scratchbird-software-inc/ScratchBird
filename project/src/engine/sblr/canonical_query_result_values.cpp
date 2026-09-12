// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_query_result_values.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "datatype_binary.hpp"
#include "datatype_binary_view.hpp"
#include "datatype_catalog_manifest.hpp"
#include "sbl_numeric.hpp"
#include "uuid.hpp"
#include "canonical_utf8.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <new>
#include <type_traits>

namespace scratchbird::engine::sblr {
namespace api = internal_api;
namespace dt = core::datatypes;
namespace numeric = libraries::sbl_numeric;
namespace {

bool SameUuid(const api::EngineUuid& identity, const wire::TypedResultUuid& binary) {
  return core::uuid::IsEngineIdentityUuid(identity) && identity.bytes == binary;
}

template <class Integer>
bool EncodeInteger(std::string_view text, std::vector<std::uint8_t>* payload) {
  Integer value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      std::to_string(value) != text) return false;
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto bits = static_cast<Unsigned>(value);
  payload->resize(sizeof(Integer));
  for (std::size_t i = 0; i < sizeof(Integer); ++i)
    (*payload)[i] = static_cast<std::uint8_t>(bits >> (8 * i));
  return true;
}

bool MaterializePayload(const api::EngineTypedValue& value,
                        const api::EngineQueryResultColumnV1& column,
                        std::uint64_t maximum_payload,
                        std::vector<std::uint8_t>* payload) {
  if (!value.binary_value.empty() && !value.encoded_value.empty()) return false;
  if (value.binary_value.size() > maximum_payload) return false;
  const bool binary = !value.binary_value.empty();
  const auto type = column.transport.canonical_type_id;
  if (type == dt::CanonicalTypeId::character) {
    if (value.encoded_value.size() > maximum_payload) return false;
  } else if (value.encoded_value.size() > 128) {
    return false;
  }
  if (binary) *payload = value.binary_value;
  switch (type) {
    case dt::CanonicalTypeId::boolean:
      if (!binary) {
        if (value.encoded_value != "true" && value.encoded_value != "false") return false;
        payload->assign(1, value.encoded_value == "true" ? 1 : 0);
      }
      break;
    case dt::CanonicalTypeId::int32:
      if (!binary && !EncodeInteger<std::int32_t>(value.encoded_value, payload)) return false;
      break;
    case dt::CanonicalTypeId::int64:
      if (!binary && !EncodeInteger<std::int64_t>(value.encoded_value, payload)) return false;
      break;
    case dt::CanonicalTypeId::int128:
      if (!binary) {
        auto encoded = numeric::EncodeInt128LittleEndian(value.encoded_value);
        if (encoded.status != numeric::NumericStatusCode::ok) return false;
        *payload = std::move(encoded.payload);
      }
      break;
    case dt::CanonicalTypeId::decimal: {
      auto decimal = binary
          ? numeric::DecodeExactDecimalLittleEndian(payload->data(), payload->size())
          : numeric::EncodeExactDecimalLittleEndian(value.encoded_value);
      if (!decimal.ok || (!binary && decimal.canonical_lexical != value.encoded_value)) return false;
      // Value precision describes the canonical coefficient, not the declared
      // output precision. Exact zero has canonical scale0 under this codec.
      if (!column.precision || !column.scale || *column.precision == 0 ||
          *column.precision > 38 || *column.scale > *column.precision ||
          decimal.scale > *column.scale ||
          (decimal.canonical_lexical == "0" ? 0U :
              static_cast<std::uint32_t>(decimal.precision - decimal.scale)) >
              *column.precision - *column.scale)
        return false;
      if (!binary) payload->assign(decimal.canonical_bytes.begin(), decimal.canonical_bytes.end());
      break;
    }
    case dt::CanonicalTypeId::character: {
      if (!binary) payload->assign(value.encoded_value.begin(), value.encoded_value.end());
      std::uint64_t scalar_count = 0;
      if (!dt::ValidateCanonicalUtf8(payload->data(), payload->size(), &scalar_count) ||
          (column.width && scalar_count > *column.width)) return false;
      break;
    }
    default: return false;  // No invented codec for unregistered datatype work.
  }
  if (payload->size() > maximum_payload ||
      (column.transport.canonical_value_bytes != 0 &&
       payload->size() != column.transport.canonical_value_bytes)) return false;
  return dt::ValidateDatatypeBinaryValueView(
      {type, false, false, payload->data(), payload->size()}).ok();
}

}  // namespace

bool PreserveCanonicalQueryResultValuesV1(
    const api::EngineRequestContext& context,
    api::EngineResultShape* shape,
    std::string* diagnostic_code,
    std::string* detail) {
  if (shape) shape->query_values.reset();
  if (!diagnostic_code || !detail) return false;
  diagnostic_code->clear();
  detail->clear();
  const auto refuse = [&](const char* why, const char* code = "DATATYPE.DESCRIPTOR.INVALID") {
    *diagnostic_code = code;
    *detail = why;
    return false;
  };
  if (!shape || shape->result_kind != "rows" || !shape->query_metadata)
    return refuse("query values require an engine-owned output schema");
  try {
    const auto cancelled = [&] { return context.query_cancellation_requested &&
                                      context.query_cancellation_requested(); };
    if (cancelled()) return refuse("query value publication cancelled", "PROCESS.CANCELLED");
    const auto metadata = shape->query_metadata;
    if (!SameUuid(context.statement_receipt_uuid, metadata->statement_receipt_uuid) ||
        !SameUuid(context.statement_snapshot_uuid, metadata->statement_snapshot_uuid) ||
        !SameUuid(context.datatype_catalog_snapshot_uuid, metadata->datatype_catalog_snapshot_uuid) ||
        context.datatype_catalog_generation == 0 || context.datatype_registry_generation == 0 ||
        metadata->datatype_catalog_generation != context.datatype_catalog_generation ||
        metadata->datatype_registry_generation != context.datatype_registry_generation)
      return refuse("query value schema belongs to another receipt/catalog cohort");
    if (metadata->columns.empty() || metadata->columns.size() > 16384 ||
        shape->columns.size() != metadata->columns.size())
      return refuse("query value schema has inconsistent column extent");
    const auto ceiling = context.maximum_typed_result_transport_bytes_per_packet;
    if (ceiling < api::kMinimumTypedResultTransportBytesPerPacket ||
        ceiling > api::kMaximumTypedResultTransportBytesPerPacket)
      return refuse("query value packet ceiling is not engine admitted", "RESOURCE.BUDGET_EXCEEDED");
    std::map<std::string, std::uint32_t> occurrences;
    std::uint64_t descriptor_bytes = wire::kTypedResultRowDescriptorHeaderBytes;
    for (std::size_t i = 0; i < metadata->columns.size(); ++i) {
      if (cancelled()) return refuse("query value publication cancelled", "PROCESS.CANCELLED");
      const auto& column = metadata->columns[i];
      const auto& transport = column.transport;
      if (transport.ordinal != i || !wire::ValidTypedResultColumnName(transport.name) ||
          transport.name_occurrence != occurrences[transport.name]++ ||
          static_cast<unsigned>(transport.nullability) > 2 ||
          !SameUuid(shape->columns[i].descriptor_uuid, column.bound_descriptor_uuid) ||
          !SameUuid(shape->columns[i].type_uuid, transport.type_uuid) ||
          (column.collation_uuid.has_value()
               ? !SameUuid(shape->columns[i].collation_uuid, *column.collation_uuid)
               : !shape->columns[i].collation_uuid.is_nil()))
        return refuse("query value column identity differs from engine schema");
      std::size_t matches = 0;
      for (const auto& identity : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
        if (identity.catalog_snapshot_uuid != context.datatype_catalog_snapshot_uuid ||
            identity.catalog_generation != context.datatype_catalog_generation ||
            identity.registry_generation != context.datatype_registry_generation ||
            !SameUuid(identity.descriptor_uuid, transport.descriptor_uuid)) continue;
        ++matches;
        if (identity.descriptor_generation != transport.descriptor_generation ||
            !SameUuid(identity.type_uuid, transport.type_uuid) ||
            identity.type_generation != transport.type_generation ||
            identity.canonical_binary_type_code != static_cast<std::uint32_t>(transport.canonical_type_id) ||
            identity.codec_id != transport.codec_id || identity.codec_version != transport.codec_version ||
            identity.codec_generation != transport.codec_generation ||
            identity.canonical_value_bytes != transport.canonical_value_bytes)
          return refuse("query value datatype codec differs from exact registry authority");
      }
      if (matches != 1) return refuse("query value datatype descriptor is absent or ambiguous");
      // Descriptor validity is independent of cardinality and SQL NULLs.
      if (transport.canonical_type_id == dt::CanonicalTypeId::decimal &&
          (!column.precision || !column.scale || *column.precision == 0 ||
           *column.precision > 38 || *column.scale > *column.precision))
        return refuse("query decimal output precision or scale is invalid");
      descriptor_bytes += wire::kTypedResultColumnDescriptorPrefixBytes + transport.name.size() + transport.codec_id.size();
      if (descriptor_bytes > ceiling)
        return refuse("query value descriptor exceeds live packet ceiling", "RESOURCE.BUDGET_EXCEEDED");
    }
    auto staged = std::make_shared<api::EngineQueryResultValuesV1>();
    staged->metadata = metadata;
    staged->rows.reserve(shape->rows.size());
    for (const auto& row : shape->rows) {
      if (cancelled()) return refuse("query value publication cancelled", "PROCESS.CANCELLED");
      if (row.fields.size() != metadata->columns.size()) return refuse("query row width differs from schema");
      wire::TypedResultRow typed_row;
      typed_row.row_ordinal = staged->rows.size();
      // A row cannot be split across packets; the total query may exceed this
      // ceiling and must then stream. Never apply the ceiling cumulatively.
      std::uint64_t row_bytes = wire::kTypedResultBatchHeaderBytes + 16;
      for (std::size_t i = 0; i < row.fields.size(); ++i) {
        if (cancelled()) return refuse("query value publication cancelled", "PROCESS.CANCELLED");
        const auto& [name, value] = row.fields[i];
        const auto& column = metadata->columns[i];
        if (name != column.transport.name ||
            !SameExactEngineDescriptorV1(value.descriptor, shape->columns[i]) ||
            (value.state != api::EngineValueState::value && value.state != api::EngineValueState::sql_null))
          return refuse("query cell identity or state differs from output schema");
        if (row_bytes > ceiling || ceiling - row_bytes < 52)
          return refuse("query row cannot fit the live packet ceiling", "RESOURCE.BUDGET_EXCEEDED");
        wire::TypedResultCell cell;
        cell.column_ordinal = column.transport.ordinal;
        cell.name_occurrence = column.transport.name_occurrence;
        if (value.isSqlNull()) {
          if (!value.encoded_value.empty() || !value.binary_value.empty() ||
              column.transport.nullability == wire::TypedResultNullability::not_null)
            return refuse("query SQL NULL has payload or a nonnullable descriptor");
          cell.state = wire::TypedResultValueState::sql_null;
        } else {
          const auto maximum_payload = ceiling - row_bytes - 52;
          // Text is already canonical bytes; numeric lexical form may be
          // longer than its fixed binary payload and is bounded separately.
          if (column.transport.canonical_value_bytes > maximum_payload ||
              value.binary_value.size() > maximum_payload ||
              (column.transport.canonical_type_id == dt::CanonicalTypeId::character &&
               value.encoded_value.size() > maximum_payload))
            return refuse("query cell cannot fit the live packet ceiling", "RESOURCE.BUDGET_EXCEEDED");
          if (!MaterializePayload(value, column, maximum_payload, &cell.canonical_payload))
            return refuse("query execution value is not canonical for its exact datatype codec");
        }
        row_bytes += 52 + cell.canonical_payload.size();
        typed_row.cells.push_back(std::move(cell));
      }
      staged->rows.push_back(std::move(typed_row));
    }
    if (cancelled()) return refuse("query value publication cancelled", "PROCESS.CANCELLED");
    shape->query_values = std::move(staged);
    return true;
  } catch (const std::bad_alloc&) {
    return refuse("query value allocation failed", "RESOURCE.BUDGET_EXCEEDED");
  } catch (...) {
    return refuse("query value materialization failed", "SBLR.EXECUTION_FAILED");
  }
}

}  // namespace scratchbird::engine::sblr
