// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/narrow_query_profile_source.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace datatypes = scratchbird::core::datatypes;
namespace wire = scratchbird::wire;

constexpr std::string_view kOperation = "query.narrow_profile_source";
constexpr std::string_view kTextDescriptorUuid =
    "019d0000-0000-7000-8000-00000000d718";
constexpr std::string_view kTextTypeUuid =
    "019d0000-0000-7000-8000-00000000d719";
constexpr std::string_view kTextCodecId = "datatype.text.utf8.v1";
constexpr std::uint64_t kTextMaximumBytes = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMinimumStorageHeadroomBytes = 64ull * 1024ull;

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

EngineApiDiagnostic Diagnostic(std::string code,
                               std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

std::string StageDetail(const EngineApiDiagnostic& diagnostic) {
  std::string detail = diagnostic.code.empty()
                           ? std::string("SBLR.EXECUTION_FAILED")
                           : diagnostic.code;
  if (!diagnostic.message_key.empty()) {
    detail.push_back(':');
    detail.append(diagnostic.message_key);
  }
  if (!diagnostic.detail.empty()) {
    detail.push_back(':');
    detail.append(diagnostic.detail);
  }
  return detail;
}

bool CheckedAdd(std::uint64_t left,
                std::uint64_t right,
                std::uint64_t* result) {
  if (result == nullptr ||
      left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedMultiply(std::uint64_t left,
                     std::uint64_t right,
                     std::uint64_t* result) {
  if (result == nullptr ||
      (left != 0 &&
       right > std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

template <typename T>
bool AddCapacityBytes(const std::vector<T>& values,
                      std::uint64_t* total) {
  std::uint64_t bytes = 0;
  if (!CheckedMultiply(static_cast<std::uint64_t>(values.capacity()),
                       static_cast<std::uint64_t>(sizeof(T)), &bytes)) {
    return false;
  }
  return CheckedAdd(*total, bytes, total);
}

bool AddStringCapacity(const std::string& value, std::uint64_t* total) {
  return CheckedAdd(*total, static_cast<std::uint64_t>(value.capacity()),
                    total);
}

bool UuidPresent(const wire::NarrowQueryUuid& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

std::string UuidText(const wire::NarrowQueryUuid& value) {
  if (!UuidPresent(value)) return {};
  scratchbird::core::platform::Uuid parsed{};
  std::copy(value.begin(), value.end(), parsed.bytes.begin());
  return scratchbird::core::uuid::UuidToString(parsed);
}

bool SameOccurrence(const wire::NarrowQueryUuid& left_uuid,
                    std::uint64_t left_generation,
                    const wire::NarrowQueryUuid& right_uuid,
                    std::uint64_t right_generation) {
  return left_uuid == right_uuid && left_generation == right_generation;
}

bool StrictShortestUtf8(std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<std::uint8_t>(value[offset++]);
    if (first <= 0x7fu) continue;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    std::size_t continuation = 0;
    if ((first & 0xe0u) == 0xc0u) {
      codepoint = first & 0x1fu;
      minimum = 0x80u;
      continuation = 1;
    } else if ((first & 0xf0u) == 0xe0u) {
      codepoint = first & 0x0fu;
      minimum = 0x800u;
      continuation = 2;
    } else if ((first & 0xf8u) == 0xf0u) {
      codepoint = first & 0x07u;
      minimum = 0x10000u;
      continuation = 3;
    } else {
      return false;
    }
    if (continuation > value.size() - offset) return false;
    for (std::size_t index = 0; index < continuation; ++index) {
      const auto byte = static_cast<std::uint8_t>(value[offset++]);
      if ((byte & 0xc0u) != 0x80u) return false;
      codepoint = (codepoint << 6u) | (byte & 0x3fu);
    }
    if (codepoint < minimum || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
      return false;
    }
  }
  return true;
}

struct StoredFieldLookup {
  bool found = false;
  bool duplicate = false;
  std::string_view value;
};

StoredFieldLookup LookupStoredField(const CrudRowVersionRecord& row,
                                    std::string_view key) {
  StoredFieldLookup result;
  for (const auto& [name, value] : row.values) {
    if (name != key) continue;
    if (result.found) {
      result.duplicate = true;
      continue;
    }
    result.found = true;
    result.value = value;
  }
  return result;
}

struct CanonicalCell {
  bool sql_null = false;
  datatypes::CanonicalTypeId type_id =
      datatypes::CanonicalTypeId::unknown;
  std::string canonical_text;
  std::vector<std::uint8_t> canonical_payload;
};

struct BoundColumn {
  std::string column_uuid;
  std::uint32_t ordinal = 0;
  std::string storage_key;
  EngineDescriptor descriptor;
  bool nullable = false;
  std::string charset_uuid;
  std::string collation_uuid;
  std::uint64_t maximum_inline_bytes = 0;
  datatypes::DatatypeTypeCodecIdentityRowV1 datatype;
};

struct SourceRow {
  std::string row_uuid;
  std::string version_uuid;
  std::vector<CanonicalCell> cells;
  std::vector<std::string> ordering_keys;
};

struct SourceState {
  wire::NarrowQuerySourceOccurrence occurrence;
  MgaRelationStorageDescriptor descriptor;
  std::vector<BoundColumn> columns;
  std::vector<SourceRow> rows;
  std::uint64_t exact_cardinality = 0;
  std::uint64_t delivery_count = 0;
  std::uint64_t delivered_rows = 0;
  std::uint64_t retain_begin = 0;
  std::uint64_t retain_count = 0;
  std::uint64_t second_pass_scanned_row_version_count = 0;
  std::uint64_t second_pass_decoded_byte_count = 0;
  std::uint64_t second_pass_storage_bytes_read = 0;
  bool complete_value_delivery = false;
  bool delivery_stopped_by_bound = false;
};

struct OutputRuntime {
  const wire::NarrowQueryOutputOccurrence* output = nullptr;
  std::size_t source_index = 0;
  std::size_t cell_index = 0;
};

struct OrderingRuntime {
  const wire::NarrowQueryOrderingTerm* term = nullptr;
  std::size_t source_index = 0;
  std::size_t cell_index = 0;
  datatypes::DatatypeTextSeedAuthority text_seed;
};

bool CellMemory(const CanonicalCell& cell, std::uint64_t* total) {
  return AddStringCapacity(cell.canonical_text, total) &&
         AddCapacityBytes(cell.canonical_payload, total);
}

bool RowMemory(const SourceRow& row, std::uint64_t* total) {
  if (!AddStringCapacity(row.row_uuid, total) ||
      !AddStringCapacity(row.version_uuid, total) ||
      !AddCapacityBytes(row.cells, total) ||
      !AddCapacityBytes(row.ordering_keys, total)) {
    return false;
  }
  for (const auto& cell : row.cells) {
    if (!CellMemory(cell, total)) return false;
  }
  for (const auto& key : row.ordering_keys) {
    if (!AddStringCapacity(key, total)) return false;
  }
  return true;
}

bool DatatypeMemory(
    const datatypes::DatatypeTypeCodecIdentityRowV1& datatype,
    std::uint64_t* total) {
  return AddStringCapacity(datatype.catalog_snapshot_uuid, total) &&
         AddStringCapacity(datatype.descriptor_uuid, total) &&
         AddStringCapacity(datatype.type_uuid, total) &&
         AddStringCapacity(datatype.codec_id, total) &&
         AddStringCapacity(datatype.canonical_name, total) &&
         AddStringCapacity(datatype.codec_uuid, total) &&
         AddStringCapacity(datatype.canonical_byte_order, total) &&
         AddStringCapacity(datatype.canonical_representation, total) &&
         AddStringCapacity(datatype.canonical_charset, total) &&
         AddStringCapacity(datatype.invalid_encoding_diagnostic_id, total);
}

bool BindingMemory(const wire::NarrowQueryBinding& binding,
                   std::uint64_t* total) {
  if (!AddCapacityBytes(binding.sources, total) ||
      !AddCapacityBytes(binding.outputs, total) ||
      !AddCapacityBytes(binding.ordering_terms, total)) {
    return false;
  }
  for (const auto& source : binding.sources) {
    if (!AddStringCapacity(source.alias, total)) return false;
  }
  for (const auto& output : binding.outputs) {
    if (!AddStringCapacity(output.name, total) ||
        !AddStringCapacity(output.codec_id, total)) {
      return false;
    }
  }
  return true;
}

bool SourceMemory(const SourceState& source, std::uint64_t* total) {
  if (!AddStringCapacity(source.occurrence.alias, total) ||
      !AddStringCapacity(source.descriptor.descriptor_uuid.canonical, total) ||
      !AddStringCapacity(source.descriptor.database_uuid.canonical, total) ||
      !AddStringCapacity(source.descriptor.schema_uuid.canonical, total) ||
      !AddStringCapacity(source.descriptor.relation_uuid.canonical, total) ||
      !AddCapacityBytes(source.columns, total) ||
      !AddCapacityBytes(source.rows, total)) {
    return false;
  }
  for (const auto& column : source.columns) {
    if (!AddStringCapacity(column.column_uuid, total) ||
        !AddStringCapacity(column.storage_key, total) ||
        !AddStringCapacity(column.descriptor.descriptor_uuid.canonical,
                           total) ||
        !AddStringCapacity(column.descriptor.descriptor_kind, total) ||
        !AddStringCapacity(column.descriptor.canonical_type_name, total) ||
        !AddStringCapacity(column.descriptor.encoded_descriptor, total) ||
        !AddStringCapacity(column.charset_uuid, total) ||
        !AddStringCapacity(column.collation_uuid, total) ||
        !DatatypeMemory(column.datatype, total)) {
      return false;
    }
  }
  for (const auto& row : source.rows) {
    if (!RowMemory(row, total)) return false;
  }
  return true;
}

bool OutputMatchesDatatype(
    const wire::NarrowQueryOutputOccurrence& output,
    const datatypes::DatatypeTypeCodecIdentityRowV1& datatype,
    bool nullable) {
  return UuidText(output.datatype_descriptor_uuid) ==
             datatype.descriptor_uuid &&
         output.datatype_descriptor_generation ==
             datatype.descriptor_generation &&
         UuidText(output.datatype_type_uuid) == datatype.type_uuid &&
         output.datatype_type_generation == datatype.type_generation &&
         output.datatype_binary_type_code ==
             datatype.canonical_binary_type_code &&
         output.codec_id == datatype.codec_id &&
         output.codec_version == datatype.codec_version &&
         output.codec_generation == datatype.codec_generation &&
         output.canonical_value_bytes == datatype.canonical_value_bytes &&
         output.null_encoding == 1 &&
         output.nullability == (nullable ? 1u : 0u);
}

std::optional<std::size_t> FindSourceIndex(
    const wire::NarrowQueryBinding& binding,
    const wire::NarrowQueryUuid& occurrence_uuid,
    std::uint64_t occurrence_generation) {
  for (std::size_t index = 0; index < binding.sources.size(); ++index) {
    const auto& source = binding.sources[index];
    if (SameOccurrence(source.source_occurrence_uuid,
                       source.source_occurrence_generation,
                       occurrence_uuid, occurrence_generation)) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FindColumnIndex(
    const SourceState& source,
    const wire::NarrowQueryUuid& column_uuid,
    std::uint32_t column_ordinal) {
  const auto text = UuidText(column_uuid);
  for (std::size_t index = 0; index < source.columns.size(); ++index) {
    if (source.columns[index].column_uuid == text &&
        source.columns[index].ordinal == column_ordinal) {
      return index;
    }
  }
  return std::nullopt;
}

bool EncodeSignedLittleEndian(std::int64_t value,
                             std::size_t width,
                             std::vector<std::uint8_t>* encoded) {
  if (encoded == nullptr || (width != 4 && width != 8)) return false;
  encoded->assign(width, 0);
  const auto bits = static_cast<std::uint64_t>(value);
  for (std::size_t index = 0; index < width; ++index) {
    (*encoded)[index] =
        static_cast<std::uint8_t>((bits >> (index * 8u)) & 0xffu);
  }
  return true;
}

bool CanonicalizeStoredCell(const BoundColumn& column,
                            const CrudRowVersionRecord& row,
                            CanonicalCell* cell,
                            EngineApiDiagnostic* diagnostic) {
  if (cell == nullptr || diagnostic == nullptr) return false;
  const auto stored = LookupStoredField(row, column.storage_key);
  if (stored.duplicate) {
    *diagnostic = Diagnostic(
        "RESULT_SET.SHAPE_INVALID",
        "sblr.query_execute.duplicate_storage_field",
        column.column_uuid);
    return false;
  }
  if (!stored.found) {
    *diagnostic = Diagnostic(
        "RESULT_SET.SHAPE_INVALID",
        "sblr.query_execute.storage_field_missing",
        column.column_uuid);
    return false;
  }
  const bool is_null = stored.value == "<NULL>";
  if (is_null) {
    if (!column.nullable) {
      *diagnostic = Diagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.query_execute.nonnull_storage_value_missing",
          column.column_uuid);
      return false;
    }
    cell->sql_null = true;
    cell->type_id = static_cast<datatypes::CanonicalTypeId>(
        column.datatype.canonical_binary_type_code);
    return true;
  }

  const auto type_id = static_cast<datatypes::CanonicalTypeId>(
      column.datatype.canonical_binary_type_code);
  cell->type_id = type_id;
  if (type_id == datatypes::CanonicalTypeId::character) {
    if (column.datatype.descriptor_uuid != kTextDescriptorUuid ||
        column.datatype.type_uuid != kTextTypeUuid ||
        column.datatype.codec_id != kTextCodecId ||
        !column.datatype.canonical_value_variable_width ||
        !column.datatype.shortest_form_utf8_required ||
        column.datatype.implicit_normalization_allowed ||
        stored.value.size() > kTextMaximumBytes ||
        !StrictShortestUtf8(stored.value)) {
      *diagnostic = Diagnostic(
          "CTB.TEXT.INVALID_ENCODING",
          "sblr.query_execute.text_value_invalid",
          column.column_uuid);
      return false;
    }
    cell->canonical_text.assign(stored.value);
    cell->canonical_payload.assign(stored.value.begin(), stored.value.end());
    return true;
  }
  if (type_id != datatypes::CanonicalTypeId::int32 &&
      type_id != datatypes::CanonicalTypeId::int64) {
    *diagnostic = Diagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.query_execute.narrow_datatype_unsupported",
        column.datatype.canonical_name);
    return false;
  }

  datatypes::DatatypeCastRequest cast;
  cast.value.type_id = type_id;
  cast.value.encoded_value.assign(stored.value);
  cast.value.is_null = false;
  cast.target_type_id = type_id;
  cast.explicit_cast = false;
  const auto canonical = datatypes::CastDatatypeValue(cast);
  if (!canonical.ok() || canonical.value.is_null) {
    *diagnostic = Diagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.query_execute.numeric_value_invalid",
        column.column_uuid);
    return false;
  }
  std::int64_t parsed = 0;
  const auto* begin = canonical.value.encoded_value.data();
  const auto* end = begin + canonical.value.encoded_value.size();
  const auto converted = std::from_chars(begin, end, parsed);
  if (converted.ec != std::errc{} || converted.ptr != end ||
      !EncodeSignedLittleEndian(
          parsed,
          type_id == datatypes::CanonicalTypeId::int32 ? 4u : 8u,
          &cell->canonical_payload)) {
    *diagnostic = Diagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.query_execute.numeric_value_invalid",
        column.column_uuid);
    return false;
  }
  cell->canonical_text = canonical.value.encoded_value;
  return true;
}

std::uint64_t LogicalOutputCount(std::uint64_t cardinality,
                                 const wire::NarrowQueryBinding& binding) {
  const auto offset = binding.row_offset_present ? binding.row_offset : 0;
  if (offset >= cardinality) return 0;
  const auto remaining = cardinality - offset;
  return binding.row_limit_present
             ? std::min(remaining, binding.row_limit)
             : remaining;
}

class NarrowQueryProfileOccurrenceSource final
    : public NarrowQueryTypedResultOccurrenceSourceV1 {
 private:
  struct PublicationCursorState {
    std::vector<std::size_t> join_indices;
    std::optional<NarrowQueryTypedResultOccurrenceRowV1> pending_row;
    std::uint64_t result_index = 0;
    std::uint64_t join_combination_ordinal = 0;
    std::uint64_t generated_rows = 0;
    std::uint64_t published_rows = 0;
    std::uint64_t next_batch_ordinal = 0;
    bool join_initialized = false;
    bool pending_row_combination_charged = false;
    bool terminal = false;
    bool terminal_eos = false;
  };

  struct PublicationLeaseControl {
    PublicationLeaseControl()
        : committed(std::make_unique<PublicationCursorState>()) {}

    std::unique_ptr<PublicationCursorState> committed;
    std::uint64_t active_token = 0;
    std::uint64_t next_token = 1;
    bool closed = false;
  };

  class PublicationStageLeaseAction final
      : public TypedResultProducerStageLeaseActionV1 {
   public:
    PublicationStageLeaseAction(
        std::shared_ptr<PublicationLeaseControl> control,
        std::uint64_t token,
        std::unique_ptr<PublicationCursorState> staged,
        EngineNarrowQueryBindingAuthorityHandleV1* authority,
        const EngineRequestContext* context,
        std::uint64_t result_rows,
        std::uint64_t batch_rows)
        : control_(std::move(control)),
          token_(token),
          staged_(std::move(staged)),
          authority_(authority),
          context_(context),
          result_rows_(result_rows),
          batch_rows_(batch_rows) {}

    CommitStatus Commit() noexcept override {
      if (!control_ || control_->closed ||
          control_->active_token != token_ || !staged_ ||
          authority_ == nullptr || context_ == nullptr) {
        return CommitStatus::stale;
      }
      const auto charged = CommitNarrowQueryPublicationChargeV1(
          authority_, *context_, result_rows_, batch_rows_);
      switch (charged) {
        case EngineNarrowQueryPublicationChargeStatusV1::cancelled:
          return CommitStatus::cancelled;
        case EngineNarrowQueryPublicationChargeStatusV1::stale:
          return CommitStatus::stale;
        case EngineNarrowQueryPublicationChargeStatusV1::
            resource_budget_exceeded:
          return CommitStatus::resource_budget_exceeded;
        case EngineNarrowQueryPublicationChargeStatusV1::committed:
          break;
      }

      // The authority pair is now committed.  Only no-fail ownership swaps and
      // scalar stores may follow in the source/carrier publication barrier.
      control_->committed.swap(staged_);
      control_->active_token = 0;
      return CommitStatus::committed;
    }

    void Abort() noexcept override {
      if (control_ && control_->active_token == token_) {
        control_->active_token = 0;
      }
      staged_.reset();
    }

   private:
    std::shared_ptr<PublicationLeaseControl> control_;
    std::uint64_t token_ = 0;
    std::unique_ptr<PublicationCursorState> staged_;
    EngineNarrowQueryBindingAuthorityHandleV1* authority_ = nullptr;
    const EngineRequestContext* context_ = nullptr;
    std::uint64_t result_rows_ = 0;
    std::uint64_t batch_rows_ = 0;
  };

 public:
  NarrowQueryProfileOccurrenceSource(
      EngineRequestContext context,
      EngineNarrowQueryBindingAuthoritySnapshotV1 snapshot,
      EngineNarrowQueryBindingAuthorityHandleV1 authority)
      : context_(std::move(context)),
        binding_(std::move(snapshot.binding)),
        grant_(std::move(snapshot.resource_grant)),
        authority_(std::move(authority)),
        publication_control_(std::make_shared<PublicationLeaseControl>()) {
    sources_.reserve(binding_.sources.size());
    for (const auto& occurrence : binding_.sources) {
      SourceState source;
      source.occurrence = occurrence;
      sources_.push_back(std::move(source));
    }
  }

  ~NarrowQueryProfileOccurrenceSource() override {
    publication_control_->closed = true;
    Release();
  }

  NarrowQueryTypedResultOccurrenceStageResultV1 Stage(
      const TypedResultProducerStageRequestV1& request) override {
    NarrowQueryTypedResultOccurrenceStageResultV1 result;
    auto& publication = *publication_control_;
    const auto& committed = *publication.committed;
    if (closed_ || publication.closed || committed.terminal) {
      result.detail =
          "MGA.TRANSACTION.STALE:sblr.query_execute.source_closed";
      return result;
    }
    if (publication.active_token != 0) {
      result.detail =
          "SBLR.EXECUTION_FAILED:sblr.query_execute.stage_lease_active";
      return result;
    }
    if (request.maximum_rows == 0 || request.maximum_bytes == 0 ||
        request.maximum_rows > grant_.maximum_batch_rows ||
        request.next_batch_ordinal != committed.next_batch_ordinal ||
        request.row_position != committed.published_rows ||
        !request.cancellation_requested) {
      result.detail =
          "SBLR.OPERAND.INVALID:sblr.query_execute.stage_request_invalid";
      return result;
    }
    if (Cancelled(request)) {
      return CancelledStage();
    }
    const auto live = Observe(EngineNarrowQueryWorkClassV1::liveness_only,
                              0, 0);
    if (live.error) return FailedStage(live);

    if (!prepared_) {
      if (!Prepare(request)) {
        if (preparation_cancelled_) return CancelledStage();
        return FailedStage(preparation_diagnostic_);
      }
    }
    std::unique_ptr<PublicationCursorState> staged_cursor;
    try {
      staged_cursor =
          std::make_unique<PublicationCursorState>(*publication.committed);
    } catch (...) {
      return FailedStage(Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.stage_cursor_allocation_failed"));
    }
    if (ResultExhausted(*staged_cursor)) {
      result.outcome = TypedResultProducerStageOutcomeV1::empty_eos;
      result.end_of_cursor = true;
      staged_cursor->terminal = true;
      staged_cursor->terminal_eos = true;
      if (!AttachLease(&result, std::move(staged_cursor), 0, 0)) {
        return FailedStage(Diagnostic(
            "RESOURCE.BUDGET_EXCEEDED",
            "sblr.query_execute.stage_lease_allocation_failed"));
      }
      return result;
    }

    const auto maximum_rows = std::min(request.maximum_rows,
                                       grant_.maximum_batch_rows);
    std::uint64_t encoded_bytes = 0;
    try {
      result.rows.reserve(static_cast<std::size_t>(maximum_rows));
    } catch (...) {
      return FailedStage(Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.stage_row_allocation_failed"));
    }
    while (result.rows.size() < maximum_rows &&
           !ResultExhausted(*staged_cursor)) {
      if (Cancelled(request)) return CancelledStage();
      const auto boundary = Observe(
          EngineNarrowQueryWorkClassV1::liveness_only, 0, 0);
      if (boundary.error) return FailedStage(boundary);

      NarrowQueryTypedResultOccurrenceRowV1 row;
      bool combination_charged = false;
      if (!BuildNextRow(staged_cursor.get(), &row,
                        &combination_charged)) {
        return FailedStage(preparation_diagnostic_);
      }
      std::uint64_t row_bytes = 0;
      if (!OccurrenceRowBytes(row, &row_bytes) ||
          row_bytes > request.maximum_bytes) {
        return FailedStage(Diagnostic(
            "RESOURCE.BUDGET_EXCEEDED",
            "sblr.query_execute.result_row_exceeds_batch_bytes"));
      }
      if (!result.rows.empty() &&
          encoded_bytes > request.maximum_bytes - row_bytes) {
        staged_cursor->pending_row = std::move(row);
        staged_cursor->pending_row_combination_charged =
            combination_charged;
        break;
      }
      if (result.rows.empty() && encoded_bytes > request.maximum_bytes -
                                                   row_bytes) {
        return FailedStage(Diagnostic(
            "RESOURCE.BUDGET_EXCEEDED",
            "sblr.query_execute.result_batch_bytes_exhausted"));
      }
      row.row_ordinal = result.rows.size();
      encoded_bytes += row_bytes;
      result.rows.push_back(std::move(row));
    }
    if (result.rows.empty()) {
      return FailedStage(Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.result_batch_cannot_progress"));
    }
    const auto row_count = static_cast<std::uint64_t>(result.rows.size());
    staged_cursor->published_rows += row_count;
    ++staged_cursor->next_batch_ordinal;
    result.outcome = TypedResultProducerStageOutcomeV1::batch;
    result.end_of_cursor = ResultExhausted(*staged_cursor);
    if (result.end_of_cursor) {
      staged_cursor->terminal = true;
      staged_cursor->terminal_eos = true;
    }
    if (!AttachLease(&result, std::move(staged_cursor), row_count,
                     row_count)) {
      return FailedStage(Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.stage_lease_allocation_failed"));
    }
    return result;
  }

  void Close(TypedResultProducerReleaseReasonV1) noexcept override {
    if (closed_) return;
    closed_ = true;
    publication_control_->closed = true;
    (void)Observe(EngineNarrowQueryWorkClassV1::liveness_only, 0, 0);
    Release();
  }

  bool CopyTerminalMetrics(
      EngineNarrowQueryProfileSourceExecutionMetricsV1* metrics) const {
    const auto& committed = *publication_control_->committed;
    if (metrics == nullptr || !committed.terminal ||
        !committed.terminal_eos || !prepared_ || closed_ || released_ ||
        !authority_.valid()) {
      return false;
    }
    EngineNarrowQueryProfileSourceExecutionMetricsV1 copy;
    try {
      copy.sources.reserve(sources_.size());
      for (std::size_t index = 0; index < sources_.size(); ++index) {
        const auto& source = sources_[index];
        EngineNarrowQueryProfileSourceOccurrenceMetricsV1 occurrence;
        occurrence.source_ordinal = static_cast<std::uint32_t>(index);
        occurrence.visible_row_count = source.exact_cardinality;
        occurrence.delivered_row_count = source.delivered_rows;
        occurrence.second_pass_scanned_row_version_count =
            source.second_pass_scanned_row_version_count;
        occurrence.second_pass_decoded_byte_count =
            source.second_pass_decoded_byte_count;
        occurrence.second_pass_storage_bytes_read =
            source.second_pass_storage_bytes_read;
        occurrence.complete_value_delivery =
            source.complete_value_delivery;
        occurrence.delivery_stopped_by_bound =
            source.delivery_stopped_by_bound;
        copy.sources.push_back(std::move(occurrence));
      }
    } catch (...) {
      return false;
    }
    *metrics = std::move(copy);
    return true;
  }

 private:
  bool AttachLease(
      NarrowQueryTypedResultOccurrenceStageResultV1* result,
      std::unique_ptr<PublicationCursorState> staged,
      std::uint64_t result_rows,
      std::uint64_t batch_rows) {
    if (result == nullptr || !staged || publication_control_->closed ||
        publication_control_->active_token != 0 ||
        publication_control_->next_token == 0 ||
        publication_control_->next_token ==
            std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
    const auto token = publication_control_->next_token;
    std::unique_ptr<TypedResultProducerStageLeaseActionV1> action;
    try {
      action = std::make_unique<PublicationStageLeaseAction>(
          publication_control_, token, std::move(staged), &authority_,
          &context_, result_rows, batch_rows);
    } catch (...) {
      return false;
    }
    result->lease = TypedResultProducerStageLeaseV1(std::move(action));
    publication_control_->active_token = token;
    ++publication_control_->next_token;
    return true;
  }

  EngineApiDiagnostic Observe(EngineNarrowQueryWorkClassV1 work_class,
                              std::uint32_t source_ordinal,
                              std::uint64_t amount) {
    const auto observed = ObserveNarrowQueryBindingLivenessV1(
        &authority_, context_, work_class, source_ordinal, amount);
    return observed.ok ? OkDiagnostic() : observed.diagnostic;
  }

  bool Cancelled(const TypedResultProducerStageRequestV1& request) {
    try {
      if (request.cancellation_requested()) return true;
      if (context_.query_cancellation_requested &&
          context_.query_cancellation_requested()) {
        return true;
      }
    } catch (...) {
      return true;
    }
    return false;
  }

  void Release() noexcept {
    if (released_) return;
    released_ = true;
    (void)ReleaseNarrowQueryBindingAuthorityV1(&authority_);
  }

  NarrowQueryTypedResultOccurrenceStageResultV1 CancelledStage() {
    NarrowQueryTypedResultOccurrenceStageResultV1 result;
    result.outcome = TypedResultProducerStageOutcomeV1::cancelled;
    result.detail =
        "PROCESS.CANCELLED:sblr.query_execute.narrow_source_cancelled";
    return result;
  }

  NarrowQueryTypedResultOccurrenceStageResultV1 FailedStage(
      const EngineApiDiagnostic& diagnostic) {
    NarrowQueryTypedResultOccurrenceStageResultV1 result;
    if (diagnostic.code == "PROCESS.CANCELLED") {
      return CancelledStage();
    }
    result.outcome = TypedResultProducerStageOutcomeV1::refused;
    result.detail = StageDetail(diagnostic);
    return result;
  }

  std::uint64_t RetainedMemory() const {
    const auto& committed = *publication_control_->committed;
    std::uint64_t total = sizeof(*this);
    if (!CheckedAdd(total, sizeof(PublicationLeaseControl), &total) ||
        !CheckedAdd(total, sizeof(PublicationCursorState), &total) ||
        !BindingMemory(binding_, &total) ||
        !AddCapacityBytes(sources_, &total) ||
        !AddCapacityBytes(outputs_, &total) ||
        !AddCapacityBytes(ordering_, &total) ||
        !AddCapacityBytes(committed.join_indices, &total)) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    for (const auto& source : sources_) {
      if (!SourceMemory(source, &total)) {
        return std::numeric_limits<std::uint64_t>::max();
      }
    }
    for (const auto& runtime : ordering_) {
      if (!AddStringCapacity(runtime.text_seed.seed_pack_name, &total) ||
          !AddStringCapacity(runtime.text_seed.seed_pack_version, &total) ||
          !AddStringCapacity(runtime.text_seed.charset_name, &total) ||
          !AddStringCapacity(runtime.text_seed.collation_name, &total)) {
        return std::numeric_limits<std::uint64_t>::max();
      }
    }
    if (committed.pending_row.has_value()) {
      if (!AddCapacityBytes(committed.pending_row->cells, &total)) {
        return std::numeric_limits<std::uint64_t>::max();
      }
      for (const auto& cell : committed.pending_row->cells) {
        if (!AddCapacityBytes(cell.canonical_payload, &total)) {
          return std::numeric_limits<std::uint64_t>::max();
        }
      }
    }
    return total;
  }

  bool ChargeRetainedMemory() {
    const auto current = RetainedMemory();
    if (current == std::numeric_limits<std::uint64_t>::max() ||
        current < charged_memory_bytes_) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.memory_receipt_invalid");
      return false;
    }
    if (current == charged_memory_bytes_) return true;
    const auto charged = Observe(
        EngineNarrowQueryWorkClassV1::sort_memory_bytes, 0,
        current - charged_memory_bytes_);
    if (charged.error) {
      preparation_diagnostic_ = charged;
      return false;
    }
    charged_memory_bytes_ = current;
    return true;
  }

  bool ValidateAndBindDescriptor(std::size_t source_index,
                                 const MgaRelationStorageDescriptor& descriptor) {
    if (source_index >= sources_.size()) return false;
    auto& state = sources_[source_index];
    const auto& occurrence = state.occurrence;
    if (descriptor.descriptor_uuid.canonical !=
            UuidText(occurrence.relation_descriptor_uuid) ||
        descriptor.descriptor_generation !=
            occurrence.relation_descriptor_generation ||
        descriptor.relation_uuid.canonical !=
            UuidText(occurrence.relation_object_uuid) ||
        descriptor.schema_uuid.canonical !=
            UuidText(occurrence.schema_uuid) ||
        descriptor.database_uuid.canonical != context_.database_uuid.canonical ||
        occurrence.validated_resource_epoch != context_.resource_epoch ||
        descriptor.relation_kind != "table" ||
        descriptor.storage_profile != "local_mga_rowstore_v1") {
      preparation_diagnostic_ = Diagnostic(
          "MGA.TRANSACTION.STALE",
          "sblr.query_execute.source_descriptor_stale",
          std::to_string(source_index));
      return false;
    }
    state.descriptor = descriptor;

    using ColumnKey = std::pair<std::string, std::uint32_t>;
    std::set<ColumnKey> required;
    for (const auto& output : binding_.outputs) {
      if (!SameOccurrence(output.source_occurrence_uuid,
                          output.source_occurrence_generation,
                          occurrence.source_occurrence_uuid,
                          occurrence.source_occurrence_generation)) {
        continue;
      }
      required.emplace(UuidText(output.source_column_uuid),
                       output.source_column_ordinal);
    }
    for (const auto& term : binding_.ordering_terms) {
      if (!SameOccurrence(term.source_occurrence_uuid,
                          term.source_occurrence_generation,
                          occurrence.source_occurrence_uuid,
                          occurrence.source_occurrence_generation)) {
        continue;
      }
      required.emplace(UuidText(term.source_column_uuid),
                       term.source_column_ordinal);
    }
    if (required.empty()) {
      preparation_diagnostic_ = Diagnostic(
          "PROJECTION.EXPRESSION_VECTOR_INVALID",
          "sblr.query_execute.source_has_no_bound_columns");
      return false;
    }

    state.columns.clear();
    state.columns.reserve(required.size());
    for (const auto& key : required) {
      const MgaRelationColumnStorageDescriptor* found = nullptr;
      for (const auto& column : descriptor.columns) {
        if (column.column_uuid.canonical != key.first ||
            column.ordinal != key.second) {
          continue;
        }
        if (found != nullptr) {
          preparation_diagnostic_ = Diagnostic(
              "DATATYPE.DESCRIPTOR_INVALID",
              "sblr.query_execute.source_column_identity_duplicated",
              key.first);
          return false;
        }
        found = &column;
      }
      if (found == nullptr || found->column_generation == 0 ||
          found->canonical_name_key.empty()) {
        preparation_diagnostic_ = Diagnostic(
            "MGA.TRANSACTION.STALE",
            "sblr.query_execute.source_column_stale", key.first);
        return false;
      }
      const auto datatype = datatypes::LookupDatatypeTypeCodecIdentityV1(
          context_.datatype_catalog_snapshot_uuid.canonical,
          context_.datatype_catalog_generation,
          context_.datatype_registry_generation,
          found->value_descriptor.descriptor_uuid.canonical, 1);
      if (!datatype.ok ||
          (datatype.row.canonical_binary_type_code !=
               static_cast<std::uint32_t>(datatypes::CanonicalTypeId::int32) &&
           datatype.row.canonical_binary_type_code !=
               static_cast<std::uint32_t>(datatypes::CanonicalTypeId::int64) &&
           datatype.row.canonical_binary_type_code !=
               static_cast<std::uint32_t>(datatypes::CanonicalTypeId::character))) {
        preparation_diagnostic_ = Diagnostic(
            "DATATYPE.DESCRIPTOR_INVALID",
            "sblr.query_execute.source_datatype_unavailable", key.first);
        return false;
      }
      BoundColumn bound;
      bound.column_uuid = found->column_uuid.canonical;
      bound.ordinal = found->ordinal;
      bound.storage_key = found->canonical_name_key;
      bound.descriptor = found->value_descriptor;
      bound.nullable = found->nullable;
      bound.charset_uuid = found->charset_uuid;
      bound.collation_uuid = found->collation_uuid;
      bound.maximum_inline_bytes = found->max_inline_bytes;
      bound.datatype = datatype.row;
      state.columns.push_back(std::move(bound));
    }

    for (const auto& output : binding_.outputs) {
      if (!SameOccurrence(output.source_occurrence_uuid,
                          output.source_occurrence_generation,
                          occurrence.source_occurrence_uuid,
                          occurrence.source_occurrence_generation)) {
        continue;
      }
      const auto cell = FindColumnIndex(state, output.source_column_uuid,
                                        output.source_column_ordinal);
      if (!cell.has_value() ||
          !OutputMatchesDatatype(output, state.columns[*cell].datatype,
                                 state.columns[*cell].nullable)) {
        preparation_diagnostic_ = Diagnostic(
            "DATATYPE.DESCRIPTOR_INVALID",
            "sblr.query_execute.output_datatype_stale",
            std::to_string(output.output_ordinal));
        return false;
      }
    }
    return true;
  }

  bool ResolveRuntimeBindings() {
    outputs_.clear();
    outputs_.reserve(binding_.outputs.size());
    for (const auto& output : binding_.outputs) {
      const auto source = FindSourceIndex(
          binding_, output.source_occurrence_uuid,
          output.source_occurrence_generation);
      if (!source.has_value()) return false;
      const auto cell = FindColumnIndex(sources_[*source],
                                        output.source_column_uuid,
                                        output.source_column_ordinal);
      if (!cell.has_value()) return false;
      outputs_.push_back(OutputRuntime{&output, *source, *cell});
    }

    ordering_.clear();
    ordering_.reserve(binding_.ordering_terms.size());
    for (const auto& term : binding_.ordering_terms) {
      const auto source = FindSourceIndex(
          binding_, term.source_occurrence_uuid,
          term.source_occurrence_generation);
      if (!source.has_value()) return false;
      const auto cell = FindColumnIndex(sources_[*source],
                                        term.source_column_uuid,
                                        term.source_column_ordinal);
      if (!cell.has_value()) return false;
      OrderingRuntime runtime;
      runtime.term = &term;
      runtime.source_index = *source;
      runtime.cell_index = *cell;
      const auto& column = sources_[*source].columns[*cell];
      const auto type = static_cast<datatypes::CanonicalTypeId>(
          column.datatype.canonical_binary_type_code);
      if (type == datatypes::CanonicalTypeId::character) {
        const auto collation_uuid = UuidText(term.collation_uuid);
        if (collation_uuid.empty() ||
            collation_uuid != column.collation_uuid ||
            term.collation_generation == 0) {
          preparation_diagnostic_ = Diagnostic(
              "SORT.COLLATION_PROFILE_INVALID",
              "sblr.query_execute.text_collation_binding_invalid");
          return false;
        }
        EngineUuid resource_uuid{collation_uuid};
        const auto resource = LookupEngineResourceDescriptorByUuid(
            context_, resource_uuid, "collation");
        if (!resource.ok || !resource.resource_descriptor.present ||
            resource.resource_descriptor.resource_uuid.canonical !=
                collation_uuid ||
            resource.resource_descriptor.resource_epoch !=
                context_.resource_epoch ||
            resource.resource_descriptor.family_epoch !=
                term.collation_generation ||
            resource.resource_descriptor.parent_resource_uuid.canonical !=
                column.charset_uuid) {
          preparation_diagnostic_ = Diagnostic(
              "SORT.COLLATION_PROFILE_INVALID",
              "sblr.query_execute.text_collation_stale");
          return false;
        }
        runtime.text_seed.active = true;
        runtime.text_seed.seed_pack_name =
            resource.resource_descriptor.seed_pack_name;
        runtime.text_seed.seed_pack_version =
            resource.resource_descriptor.seed_pack_version;
        runtime.text_seed.charset_name =
            resource.resource_descriptor.parent_canonical_name;
        runtime.text_seed.collation_name =
            resource.resource_descriptor.canonical_name;
        runtime.text_seed.collation_case_insensitive =
            resource.resource_descriptor.case_insensitive;
        runtime.text_seed.collation_accent_insensitive =
            resource.resource_descriptor.accent_insensitive;
      } else if (UuidPresent(term.collation_uuid) ||
                 term.collation_generation != 0) {
        preparation_diagnostic_ = Diagnostic(
            "SORT.COLLATION_PROFILE_INVALID",
            "sblr.query_execute.nontext_collation_forbidden");
        return false;
      }
      ordering_.push_back(std::move(runtime));
    }
    return true;
  }

  bool BuildOrderingKeys(SourceRow* row) {
    if (row == nullptr) return false;
    row->ordering_keys.clear();
    row->ordering_keys.reserve(ordering_.size());
    for (const auto& runtime : ordering_) {
      if (runtime.source_index != 0 ||
          runtime.cell_index >= row->cells.size()) {
        return false;
      }
      const auto& cell = row->cells[runtime.cell_index];
      if (cell.sql_null) {
        row->ordering_keys.emplace_back();
        continue;
      }
      datatypes::DatatypeSortKeyRequest request;
      request.value.type_id = cell.type_id;
      request.value.encoded_value = cell.canonical_text;
      request.value.is_null = false;
      request.null_ordering =
          runtime.term->null_placement == wire::NarrowQueryNullPlacement::first
              ? datatypes::DatatypeNullOrdering::nulls_first
              : datatypes::DatatypeNullOrdering::nulls_last;
      request.case_insensitive_character_compare =
          runtime.text_seed.collation_case_insensitive;
      request.text_seed = runtime.text_seed;
      const auto key = datatypes::MakeDatatypeSortKey(request);
      if (!key.ok()) {
        preparation_diagnostic_ = Diagnostic(
            "SORT.COLLATION_PROFILE_INVALID",
            "sblr.query_execute.order_key_refused",
            key.diagnostic.diagnostic_code);
        return false;
      }
      row->ordering_keys.push_back(key.sort_key);
    }
    return true;
  }

  bool ConsumeSourceRow(std::size_t source_index,
                        const CrudRowVersionRecord& stored) {
    auto& source = sources_[source_index];
    const auto ordinal = source.delivered_rows++;
    if (ordinal < source.retain_begin ||
        ordinal - source.retain_begin >= source.retain_count) {
      return true;
    }
    const auto before = RetainedMemory();
    SourceRow row;
    row.row_uuid = stored.row_uuid;
    row.version_uuid = stored.version_uuid;
    row.cells.reserve(source.columns.size());
    for (const auto& column : source.columns) {
      CanonicalCell cell;
      if (!CanonicalizeStoredCell(column, stored, &cell,
                                  &preparation_diagnostic_)) {
        return false;
      }
      row.cells.push_back(std::move(cell));
    }
    if (binding_.profile == wire::NarrowQueryProfile::ordered_projection &&
        !BuildOrderingKeys(&row)) {
      return false;
    }
    source.rows.push_back(std::move(row));
    const auto after = RetainedMemory();
    if (after == std::numeric_limits<std::uint64_t>::max() || after < before ||
        after - before > active_growth_bound_) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.source_row_growth_exceeded");
      return false;
    }
    return ChargeRetainedMemory();
  }

  bool PrepareSource(std::size_t source_index,
                     const TypedResultProducerStageRequestV1& stage) {
    auto& state = sources_[source_index];
    const auto relation_uuid = UuidText(state.occurrence.relation_object_uuid);
    // EngineApiDiagnostic defaults to error=true.  This is an optional
    // observation slot, so initialize it explicitly to success; otherwise an
    // unrelated store refusal before the callback runs can be masked by an
    // empty diagnostic at the source boundary.
    EngineApiDiagnostic liveness_failure = OkDiagnostic();
    bool callback_cancelled = false;
    const std::function<bool()> cancellation = [&]() {
      if (Cancelled(stage)) {
        callback_cancelled = true;
        return true;
      }
      const auto observed = Observe(
          EngineNarrowQueryWorkClassV1::liveness_only, 0, 0);
      if (observed.error) {
        liveness_failure = observed;
        callback_cancelled = observed.code == "PROCESS.CANCELLED";
        return true;
      }
      return false;
    };

    MgaVisibleHeapRelationStreamRequest request;
    request.borrowed_relation_uuid = &relation_uuid;
    request.maximum_decoded_bytes_per_pass =
        grant_.maximum_mga_relation_decoded_bytes_per_pass;
    request.maximum_memory_bytes = grant_.maximum_sort_memory_bytes;
    request.borrowed_cancellation_requested = &cancellation;
    const auto offset = binding_.row_offset_present ? binding_.row_offset : 0;
    if (binding_.row_limit_present && binding_.row_limit == 0) {
      request.maximum_delivered_visible_rows = 0;
    } else if (binding_.profile !=
                   wire::NarrowQueryProfile::ordered_projection &&
               binding_.row_limit_present) {
      std::uint64_t required_source_rows = 0;
      if (!CheckedAdd(offset, binding_.row_limit, &required_source_rows)) {
        preparation_diagnostic_ = Diagnostic(
            "RESOURCE.BUDGET_EXCEEDED",
            "sblr.query_execute.source_delivery_bound_overflow");
        return false;
      }
      request.maximum_delivered_visible_rows = required_source_rows;
    }
    request.prepare_consumer_for_visible_rows =
        [&](const MgaRelationStorageDescriptor& descriptor,
            std::uint64_t exact_cardinality,
            std::uint64_t* maximum_growth) {
          if (maximum_growth == nullptr) return false;
          const auto revalidated =
              RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
                  authority_, context_,
                  static_cast<std::uint32_t>(source_index), descriptor);
          if (!revalidated.ok) {
            preparation_diagnostic_ = revalidated.diagnostic;
            return false;
          }
          if (!ValidateAndBindDescriptor(source_index, descriptor)) {
            return false;
          }
          state.exact_cardinality = exact_cardinality;
          state.delivery_count =
              request.maximum_delivered_visible_rows.has_value()
                  ? std::min(exact_cardinality,
                             *request.maximum_delivered_visible_rows)
                  : exact_cardinality;
          if (binding_.profile ==
              wire::NarrowQueryProfile::projection_occurrence) {
            const auto count = LogicalOutputCount(exact_cardinality, binding_);
            if (count > grant_.maximum_result_rows) {
              preparation_diagnostic_ = Diagnostic(
                  "RESOURCE.BUDGET_EXCEEDED",
                  "sblr.query_execute.projection_result_rows_exceeded");
              return false;
            }
            state.retain_begin = state.delivery_count == 0
                                     ? 0
                                     : std::min(offset, exact_cardinality);
            state.retain_count = count;
          } else if (binding_.profile ==
                     wire::NarrowQueryProfile::ordered_projection) {
            const auto count = LogicalOutputCount(exact_cardinality, binding_);
            if (count > grant_.maximum_result_rows) {
              preparation_diagnostic_ = Diagnostic(
                  "RESOURCE.BUDGET_EXCEEDED",
                  "sblr.query_execute.ordered_result_rows_exceeded");
              return false;
            }
            state.retain_begin = 0;
            state.retain_count = state.delivery_count;
          } else {
            state.retain_begin = 0;
            state.retain_count = state.delivery_count;
          }
          if (state.retain_begin > state.delivery_count ||
              state.retain_count >
                  state.delivery_count - state.retain_begin) {
            preparation_diagnostic_ = Diagnostic(
                "SBLR.PLAN_TREE.INVALID_HANDLE",
                "sblr.query_execute.source_retention_extent_invalid");
            return false;
          }
          const auto source_charge = Observe(
              EngineNarrowQueryWorkClassV1::source_rows,
              static_cast<std::uint32_t>(source_index),
              state.delivery_count);
          if (source_charge.error) {
            preparation_diagnostic_ = source_charge;
            return false;
          }
          if (state.retain_count >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::size_t>::max())) {
            preparation_diagnostic_ = Diagnostic(
                "RESOURCE.BUDGET_EXCEEDED",
                "sblr.query_execute.source_row_extent_unrepresentable");
            return false;
          }
          state.rows.reserve(
              static_cast<std::size_t>(state.retain_count));
          // Ordered rows need their exact bound comparison terms before the
          // first value is delivered.  The other profiles can resolve all
          // output occurrences after every source descriptor is present.
          if (binding_.profile ==
                  wire::NarrowQueryProfile::ordered_projection &&
              !ResolveRuntimeBindings()) {
            preparation_diagnostic_ = Diagnostic(
                "SBLR.PLAN_TREE.INVALID_HANDLE",
                "sblr.query_execute.runtime_binding_invalid");
            return false;
          }
          if (!ChargeRetainedMemory()) return false;

          if (state.delivery_count == 0) {
            *maximum_growth = 0;
            active_growth_bound_ = 0;
            return true;
          }

          const auto retained = RetainedMemory();
          const auto memory_ceiling = grant_.maximum_sort_memory_bytes;
          const auto headroom = std::max(
              kMinimumStorageHeadroomBytes, memory_ceiling / 2u);
          if (retained >= memory_ceiling ||
              headroom >= memory_ceiling - retained) {
            preparation_diagnostic_ = Diagnostic(
                "RESOURCE.BUDGET_EXCEEDED",
                "sblr.query_execute.source_memory_headroom_exhausted");
            return false;
          }
          const auto consumer_budget = memory_ceiling - retained - headroom;
          const auto divisor = std::max<std::uint64_t>(1, state.retain_count);
          *maximum_growth = consumer_budget / divisor;
          if (*maximum_growth == 0) {
            preparation_diagnostic_ = Diagnostic(
                "RESOURCE.BUDGET_EXCEEDED",
                "sblr.query_execute.source_row_growth_unavailable");
            return false;
          }
          active_growth_bound_ = *maximum_growth;
          return true;
        };
    request.consumer_retained_memory_bytes = [&]() {
      return RetainedMemory();
    };
    request.consume_visible_row =
        [&](std::uint64_t, const CrudRowVersionRecord& row) {
          return ConsumeSourceRow(source_index, row);
        };

    const auto streamed = StreamVisibleMgaHeapRelation(context_, request);
    active_growth_bound_ = 0;
    if (streamed.ok && streamed.second_pass_decoded_byte_count >
                           streamed.decoded_byte_count) {
      preparation_diagnostic_ = Diagnostic(
          "SBLR.EXECUTION_FAILED",
          "sblr.query_execute.mga_stream_decoded_byte_counters_invalid");
      return false;
    }
    if (streamed.ok) {
      const auto first_pass_decoded_bytes =
          streamed.decoded_byte_count -
          streamed.second_pass_decoded_byte_count;
      const auto first_pass_charge = Observe(
          EngineNarrowQueryWorkClassV1::mga_relation_decoded_bytes_per_pass,
          static_cast<std::uint32_t>(source_index), first_pass_decoded_bytes);
      if (first_pass_charge.error) {
        preparation_diagnostic_ = first_pass_charge;
        return false;
      }
      if (streamed.second_pass_decoded_byte_count != 0) {
        const auto second_pass_charge = Observe(
            EngineNarrowQueryWorkClassV1::mga_relation_decoded_bytes_per_pass,
            static_cast<std::uint32_t>(source_index),
            streamed.second_pass_decoded_byte_count);
        if (second_pass_charge.error) {
          preparation_diagnostic_ = second_pass_charge;
          return false;
        }
      }
    }
    const bool expected_complete_delivery =
        state.delivery_count == state.exact_cardinality;
    const bool expected_bound_stop =
        state.delivery_count < state.exact_cardinality;
    if (!streamed.ok || !streamed.complete_mga_chain_validation ||
        !streamed.exact_segment_extent_revalidated ||
        !streamed.memory_receipt_complete ||
        streamed.visible_row_count != state.exact_cardinality ||
        streamed.delivered_row_count != state.delivered_rows ||
        state.delivered_rows != state.delivery_count ||
        state.rows.size() != state.retain_count ||
        streamed.complete_value_delivery != expected_complete_delivery ||
        streamed.delivery_stopped_by_bound != expected_bound_stop) {
      if (callback_cancelled) {
        preparation_cancelled_ = true;
        preparation_diagnostic_ = Diagnostic(
            "PROCESS.CANCELLED",
            "sblr.query_execute.narrow_source_cancelled");
      } else if (liveness_failure.error) {
        preparation_diagnostic_ = liveness_failure;
      } else if (!preparation_diagnostic_.error &&
                 streamed.failure_category ==
                     MgaHeapReadFailureCategoryV1::kCancellation) {
        preparation_cancelled_ = true;
        preparation_diagnostic_ = Diagnostic(
            "PROCESS.CANCELLED",
            "sblr.query_execute.narrow_source_cancelled");
      } else if (!preparation_diagnostic_.error &&
                 streamed.failure_category ==
                     MgaHeapReadFailureCategoryV1::kResource) {
        preparation_diagnostic_ = Diagnostic(
            "RESOURCE.BUDGET_EXCEEDED",
            "sblr.query_execute.mga_stream_resource_exhausted",
            streamed.diagnostic.detail);
      } else if (!preparation_diagnostic_.error) {
        preparation_diagnostic_ = streamed.diagnostic.error
                                      ? streamed.diagnostic
                                      : Diagnostic(
                                            "SBLR.EXECUTION_FAILED",
                                            "sblr.query_execute.mga_stream_incomplete");
      }
      return false;
    }
    state.second_pass_scanned_row_version_count =
        streamed.second_pass_scanned_row_version_count;
    state.second_pass_decoded_byte_count =
        streamed.second_pass_decoded_byte_count;
    state.second_pass_storage_bytes_read =
        streamed.second_pass_storage_bytes_read;
    state.complete_value_delivery = streamed.complete_value_delivery;
    state.delivery_stopped_by_bound = streamed.delivery_stopped_by_bound;
    return true;
  }

  std::optional<int> CompareOrderedRows(
      const TypedResultProducerStageRequestV1& stage,
      const SourceRow& left,
      const SourceRow& right) {
    // Core requires cancellation/liveness observation before every sort
    // comparison.  A failed observation terminates the sort; it is never
    // converted into an arbitrary comparator result.
    if (Cancelled(stage)) {
      preparation_cancelled_ = true;
      preparation_diagnostic_ = Diagnostic(
          "PROCESS.CANCELLED",
          "sblr.query_execute.narrow_source_cancelled");
      return std::nullopt;
    }
    const auto observed = Observe(
        EngineNarrowQueryWorkClassV1::liveness_only, 0, 0);
    if (observed.error) {
      preparation_cancelled_ = observed.code == "PROCESS.CANCELLED";
      preparation_diagnostic_ = observed;
      return std::nullopt;
    }
    for (std::size_t index = 0; index < ordering_.size(); ++index) {
      const auto& runtime = ordering_[index];
      if (runtime.term == nullptr ||
          runtime.cell_index >= left.cells.size() ||
          runtime.cell_index >= right.cells.size() ||
          index >= left.ordering_keys.size() ||
          index >= right.ordering_keys.size()) {
        preparation_diagnostic_ = Diagnostic(
            "SORT.ORDERING_VECTOR_INVALID",
            "sblr.query_execute.order_comparison_binding_invalid");
        return std::nullopt;
      }
      const auto& left_cell = left.cells[runtime.cell_index];
      const auto& right_cell = right.cells[runtime.cell_index];
      if (left_cell.sql_null != right_cell.sql_null) {
        const bool nulls_first =
            runtime.term->null_placement ==
            wire::NarrowQueryNullPlacement::first;
        return left_cell.sql_null == nulls_first ? -1 : 1;
      }
      if (left_cell.sql_null) continue;
      const auto& left_key = left.ordering_keys[index];
      const auto& right_key = right.ordering_keys[index];
      if (left_key == right_key) continue;
      const bool left_less = left_key < right_key;
      const bool ascending =
          runtime.term->direction ==
          wire::NarrowQueryDirection::ascending;
      return left_less == ascending ? -1 : 1;
    }
    // Fully equal terms preserve the exact MGA input-stream order.  UUID,
    // name, clock and physical-location tie breakers are forbidden.
    return 0;
  }

  bool StableSortRows(const TypedResultProducerStageRequestV1& stage,
                      std::vector<SourceRow>* rows) {
    if (rows == nullptr) return false;
    const auto count = rows->size();
    if (count < 2) return true;
    std::uint64_t index_bytes = 0;
    std::uint64_t row_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;
    if (!CheckedMultiply(static_cast<std::uint64_t>(count),
                         2u * sizeof(std::size_t), &index_bytes) ||
        !CheckedMultiply(static_cast<std::uint64_t>(count),
                         sizeof(SourceRow), &row_bytes) ||
        !CheckedAdd(index_bytes, row_bytes, &auxiliary_bytes)) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.sort_extent_unrepresentable");
      return false;
    }
    const auto charged = Observe(
        EngineNarrowQueryWorkClassV1::sort_memory_bytes, 0,
        auxiliary_bytes);
    if (charged.error) {
      preparation_diagnostic_ = charged;
      return false;
    }
    try {
      std::vector<std::size_t> order(count);
      std::vector<std::size_t> scratch(count);
      std::iota(order.begin(), order.end(), std::size_t{0});
      for (std::size_t width = 1; width < count;) {
        for (std::size_t begin = 0; begin < count;) {
          const auto middle = std::min(count, begin + width);
          const auto end = std::min(count, middle + width);
          std::size_t left = begin;
          std::size_t right = middle;
          std::size_t output = begin;
          while (left < middle && right < end) {
            const auto compared = CompareOrderedRows(
                stage, (*rows)[order[left]], (*rows)[order[right]]);
            if (!compared.has_value()) return false;
            // The left input wins equality, preserving input order.
            scratch[output++] = *compared <= 0
                                    ? order[left++]
                                    : order[right++];
          }
          while (left < middle) scratch[output++] = order[left++];
          while (right < end) scratch[output++] = order[right++];
          begin = end;
        }
        order.swap(scratch);
        if (width > count / 2) break;
        width *= 2;
      }
      std::vector<SourceRow> sorted;
      sorted.reserve(count);
      for (const auto index : order) {
        sorted.push_back(std::move((*rows)[index]));
      }
      rows->swap(sorted);
    } catch (...) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.sort_allocation_failed");
      return false;
    }
    return true;
  }

  bool Prepare(const TypedResultProducerStageRequestV1& stage) {
    preparation_diagnostic_ = OkDiagnostic();
    preparation_cancelled_ = false;
    for (std::size_t index = 0; index < sources_.size(); ++index) {
      if (!PrepareSource(index, stage)) return false;
    }
    if (!ResolveRuntimeBindings()) {
      preparation_diagnostic_ = Diagnostic(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "sblr.query_execute.runtime_binding_invalid");
      return false;
    }

    if (binding_.profile == wire::NarrowQueryProfile::ordered_projection) {
      auto& rows = sources_.front().rows;
      if (!StableSortRows(stage, &rows)) return false;
      publication_control_->committed->result_index =
          binding_.row_offset_present
              ? std::min(binding_.row_offset,
                         sources_.front().exact_cardinality)
              : 0;
      total_result_rows_ =
          LogicalOutputCount(sources_.front().exact_cardinality, binding_);
    } else if (binding_.profile ==
               wire::NarrowQueryProfile::projection_occurrence) {
      publication_control_->committed->result_index = 0;
      total_result_rows_ = sources_.front().rows.size();
    } else if (!PrepareJoinCardinality()) {
      return false;
    }
    prepared_ = true;
    return ChargeRetainedMemory();
  }

  bool PrepareJoinCardinality() {
    const auto offset = binding_.row_offset_present ? binding_.row_offset : 0;
    const bool bounded = binding_.row_limit_present;
    std::uint64_t needed = 0;
    if (bounded && !CheckedAdd(offset, binding_.row_limit, &needed)) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.join_bound_overflow");
      return false;
    }
    if (std::any_of(sources_.begin(), sources_.end(),
                    [](const SourceState& source) {
                      return source.exact_cardinality == 0;
                    })) {
      combinations_to_explore_ = 0;
      total_result_rows_ = 0;
      publication_control_->committed->join_indices.assign(
          sources_.size(), 0);
      publication_control_->committed->join_initialized = false;
      return true;
    }
    std::uint64_t product = 1;
    bool product_overflow = false;
    for (const auto& source : sources_) {
      std::uint64_t next = 0;
      if (!CheckedMultiply(product, source.exact_cardinality, &next)) {
        product_overflow = true;
        product = bounded ? needed : 0;
        break;
      }
      product = next;
      if (bounded && product >= needed) {
        product = needed;
        break;
      }
    }
    if (!bounded && product_overflow) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.join_cardinality_overflow");
      return false;
    }
    combinations_to_explore_ = bounded ? std::min(product, needed) : product;
    if (combinations_to_explore_ > grant_.maximum_join_combinations) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.join_combination_grant_exceeded");
      return false;
    }
    total_result_rows_ = combinations_to_explore_ <= offset
                             ? 0
                             : combinations_to_explore_ - offset;
    if (bounded) {
      total_result_rows_ = std::min(total_result_rows_, binding_.row_limit);
    }
    if (total_result_rows_ > grant_.maximum_result_rows) {
      preparation_diagnostic_ = Diagnostic(
          "RESOURCE.BUDGET_EXCEEDED",
          "sblr.query_execute.join_result_grant_exceeded");
      return false;
    }
    publication_control_->committed->join_indices.assign(
        sources_.size(), 0);
    publication_control_->committed->join_initialized =
        combinations_to_explore_ != 0;
    return true;
  }

  bool OccurrenceRowBytes(
      const NarrowQueryTypedResultOccurrenceRowV1& row,
      std::uint64_t* bytes) const {
    if (bytes == nullptr) return false;
    std::uint64_t total = 224;
    for (const auto& cell : row.cells) {
      std::uint64_t cell_bytes = 96;
      if (!CheckedAdd(cell_bytes, cell.canonical_payload.size(),
                      &cell_bytes) ||
          !CheckedAdd(total, cell_bytes, &total)) {
        return false;
      }
    }
    *bytes = total;
    return true;
  }

  bool AppendOutputCell(const OutputRuntime& runtime,
                        const SourceRow& source_row,
                        NarrowQueryTypedResultOccurrenceRowV1* row) {
    if (runtime.output == nullptr || row == nullptr ||
        runtime.cell_index >= source_row.cells.size()) {
      return false;
    }
    const auto& cell = source_row.cells[runtime.cell_index];
    NarrowQueryTypedResultOccurrenceCellV1 output;
    output.output_occurrence_uuid = runtime.output->output_occurrence_uuid;
    output.output_occurrence_generation =
        runtime.output->output_occurrence_generation;
    output.state = cell.sql_null ? wire::TypedResultValueState::sql_null
                                 : wire::TypedResultValueState::value_present;
    if (!cell.sql_null) output.canonical_payload = cell.canonical_payload;
    row->cells.push_back(std::move(output));
    return true;
  }

  bool BuildProjectedRow(const SourceRow& source_row,
                         NarrowQueryTypedResultOccurrenceRowV1* row) {
    row->cells.clear();
    row->cells.reserve(outputs_.size());
    for (const auto& output : outputs_) {
      if (output.source_index != 0 ||
          !AppendOutputCell(output, source_row, row)) {
        return false;
      }
    }
    return true;
  }

  bool AdvanceJoin(PublicationCursorState* cursor) {
    if (cursor == nullptr || !cursor->join_initialized) return false;
    for (std::size_t reverse = sources_.size(); reverse != 0; --reverse) {
      const auto index = reverse - 1;
      ++cursor->join_indices[index];
      if (cursor->join_indices[index] < sources_[index].rows.size()) {
        return true;
      }
      cursor->join_indices[index] = 0;
    }
    cursor->join_initialized = false;
    return false;
  }

  bool BuildJoinRow(const PublicationCursorState& cursor,
                    NarrowQueryTypedResultOccurrenceRowV1* row) {
    if (row == nullptr || !cursor.join_initialized) return false;
    row->cells.clear();
    row->cells.reserve(outputs_.size());
    for (const auto& output : outputs_) {
      if (output.source_index >= sources_.size() ||
          cursor.join_indices[output.source_index] >=
              sources_[output.source_index].rows.size() ||
          !AppendOutputCell(
              output,
              sources_[output.source_index]
                  .rows[cursor.join_indices[output.source_index]],
              row)) {
        return false;
      }
    }
    return true;
  }

  bool BuildNextRow(PublicationCursorState* cursor,
                    NarrowQueryTypedResultOccurrenceRowV1* row,
                    bool* combination_charged) {
    if (cursor == nullptr || row == nullptr || combination_charged == nullptr) {
      return false;
    }
    *combination_charged = false;
    if (cursor->pending_row.has_value()) {
      *row = std::move(*cursor->pending_row);
      cursor->pending_row.reset();
      *combination_charged = cursor->pending_row_combination_charged;
      cursor->pending_row_combination_charged = false;
      return true;
    }
    if (binding_.profile !=
        wire::NarrowQueryProfile::alias_distinct_self_join) {
      if (cursor->result_index >= sources_.front().rows.size()) return false;
      if (!BuildProjectedRow(sources_.front().rows[cursor->result_index], row)) {
        preparation_diagnostic_ = Diagnostic(
            "RESULT_SET.SHAPE_INVALID",
            "sblr.query_execute.projection_occurrence_invalid");
        return false;
      }
      ++cursor->result_index;
      ++cursor->generated_rows;
      return true;
    }

    const auto offset = binding_.row_offset_present ? binding_.row_offset : 0;
    while (cursor->join_combination_ordinal < combinations_to_explore_ &&
           cursor->join_initialized) {
      if (cursor->join_combination_ordinal >=
          reserved_join_combinations_) {
        const auto charged = Observe(
            EngineNarrowQueryWorkClassV1::join_combinations, 0, 1);
        if (charged.error) {
          preparation_diagnostic_ = charged;
          return false;
        }
        ++reserved_join_combinations_;
      }
      const auto current = cursor->join_combination_ordinal++;
      const bool publish = current >= offset;
      if (publish && !BuildJoinRow(*cursor, row)) {
        preparation_diagnostic_ = Diagnostic(
            "RESULT_SET.SHAPE_INVALID",
            "sblr.query_execute.join_projection_invalid");
        return false;
      }
      (void)AdvanceJoin(cursor);
      if (publish) {
        *combination_charged = true;
        ++cursor->generated_rows;
        return true;
      }
    }
    return false;
  }

  bool ResultExhausted(const PublicationCursorState& cursor) const {
    return cursor.generated_rows >= total_result_rows_ &&
           !cursor.pending_row.has_value();
  }

  EngineRequestContext context_;
  wire::NarrowQueryBinding binding_;
  EngineNarrowQueryResourceGrantV1 grant_;
  EngineNarrowQueryBindingAuthorityHandleV1 authority_;
  std::shared_ptr<PublicationLeaseControl> publication_control_;
  std::vector<SourceState> sources_;
  std::vector<OutputRuntime> outputs_;
  std::vector<OrderingRuntime> ordering_;
  EngineApiDiagnostic preparation_diagnostic_;
  std::uint64_t charged_memory_bytes_ = 0;
  std::uint64_t active_growth_bound_ = 0;
  std::uint64_t total_result_rows_ = 0;
  std::uint64_t combinations_to_explore_ = 0;
  // Join work is an actual-work charge.  Aborted publication retries replay
  // the same logical combinations without consuming the work budget twice.
  std::uint64_t reserved_join_combinations_ = 0;
  bool prepared_ = false;
  bool preparation_cancelled_ = false;
  bool closed_ = false;
  bool released_ = false;
};

EngineNarrowQueryProfileSourceStatusV1 StatusForDiagnostic(
    const EngineApiDiagnostic& diagnostic,
    EngineNarrowQueryProfileSourceStatusV1 fallback) {
  if (diagnostic.code == "SECURITY.ACCESS_DENIED") {
    return EngineNarrowQueryProfileSourceStatusV1::access_denied;
  }
  if (diagnostic.code == "MGA.TRANSACTION.STALE") {
    return EngineNarrowQueryProfileSourceStatusV1::binding_stale;
  }
  if (diagnostic.code == "PROCESS.CANCELLED") {
    return EngineNarrowQueryProfileSourceStatusV1::cancelled;
  }
  if (diagnostic.code == "RESOURCE.BUDGET_EXCEEDED") {
    return EngineNarrowQueryProfileSourceStatusV1::resource_budget_exceeded;
  }
  if (diagnostic.code == "DATATYPE.DESCRIPTOR_INVALID" ||
      diagnostic.code == "CTB.TEXT.INVALID_ENCODING") {
    return EngineNarrowQueryProfileSourceStatusV1::datatype_invalid;
  }
  if (diagnostic.code == "SORT.COLLATION_PROFILE_INVALID") {
    return EngineNarrowQueryProfileSourceStatusV1::collation_invalid;
  }
  return fallback;
}

}  // namespace

bool InspectNarrowQueryProfileSourceExecutionMetricsV1(
    const NarrowQueryTypedResultOccurrenceSourceV1* source,
    EngineNarrowQueryProfileSourceExecutionMetricsV1* metrics) {
  if (source == nullptr || metrics == nullptr) return false;
  const auto* exact =
      dynamic_cast<const NarrowQueryProfileOccurrenceSource*>(source);
  return exact != nullptr && exact->CopyTerminalMetrics(metrics);
}

EngineNarrowQueryProfileSourceOpenResultV1 OpenNarrowQueryProfileSourceV1(
    EngineNarrowQueryProfileSourceOpenRequestV1 request) {
  EngineNarrowQueryProfileSourceOpenResultV1 result;
  if (request.version != kNarrowQueryProfileSourceVersionV1 ||
      !request.binding_authority.valid()) {
    result.diagnostic = Diagnostic(
        "SBLR.OPERAND.INVALID",
        "sblr.query_execute.narrow_source_open_invalid");
    return result;
  }
  EngineNarrowQueryBindingAuthoritySnapshotV1 snapshot;
  if (!CopyNarrowQueryBindingAuthoritySnapshotV1(
          request.binding_authority, &snapshot, &result.diagnostic)) {
    result.status = StatusForDiagnostic(
        result.diagnostic,
        EngineNarrowQueryProfileSourceStatusV1::binding_stale);
    return result;
  }
  if (snapshot.binding.profile != wire::NarrowQueryProfile::ordered_projection &&
      snapshot.binding.profile !=
          wire::NarrowQueryProfile::projection_occurrence &&
      snapshot.binding.profile !=
          wire::NarrowQueryProfile::alias_distinct_self_join) {
    result.status =
        EngineNarrowQueryProfileSourceStatusV1::profile_unavailable;
    result.diagnostic = Diagnostic(
        "SBLR.OPCODE.UNSUPPORTED",
        "sblr.query_execute.narrow_profile_unavailable");
    (void)ReleaseNarrowQueryBindingAuthorityV1(
        &request.binding_authority);
    return result;
  }
  const auto& grant = snapshot.resource_grant;
  if (grant.grant_generation == 0 ||
      grant.maximum_source_rows_per_occurrence == 0 ||
      grant.maximum_cumulative_source_rows == 0 ||
      grant.maximum_result_rows == 0 ||
      grant.maximum_join_combinations == 0 ||
      grant.maximum_sort_memory_bytes == 0 ||
      grant.maximum_batch_rows == 0 ||
      grant.maximum_mga_relation_decoded_bytes_per_pass == 0) {
    result.status =
        EngineNarrowQueryProfileSourceStatusV1::resource_budget_exceeded;
    result.diagnostic = Diagnostic(
        "RESOURCE.BUDGET_EXCEEDED",
        "sblr.query_execute.narrow_source_grant_invalid");
    (void)ReleaseNarrowQueryBindingAuthorityV1(
        &request.binding_authority);
    return result;
  }
  const auto observed = ObserveNarrowQueryBindingLivenessV1(
      &request.binding_authority, request.context,
      EngineNarrowQueryWorkClassV1::liveness_only, 0, 0);
  if (!observed.ok) {
    result.status = StatusForDiagnostic(
        observed.diagnostic,
        EngineNarrowQueryProfileSourceStatusV1::binding_stale);
    result.diagnostic = observed.diagnostic;
    (void)ReleaseNarrowQueryBindingAuthorityV1(
        &request.binding_authority);
    return result;
  }
  try {
    result.source = std::make_unique<NarrowQueryProfileOccurrenceSource>(
        std::move(request.context), std::move(snapshot),
        std::move(request.binding_authority));
  } catch (...) {
    result.status =
        EngineNarrowQueryProfileSourceStatusV1::resource_budget_exceeded;
    result.diagnostic = Diagnostic(
        "RESOURCE.BUDGET_EXCEEDED",
        "sblr.query_execute.narrow_source_allocation_failed");
    (void)ReleaseNarrowQueryBindingAuthorityV1(
        &request.binding_authority);
    return result;
  }
  result.status = EngineNarrowQueryProfileSourceStatusV1::ok;
  result.diagnostic = OkDiagnostic();
  return result;
}

const char* EngineNarrowQueryProfileSourceStatusNameV1(
    EngineNarrowQueryProfileSourceStatusV1 status) {
  switch (status) {
    case EngineNarrowQueryProfileSourceStatusV1::ok:
      return "ok";
    case EngineNarrowQueryProfileSourceStatusV1::invalid_argument:
      return "invalid_argument";
    case EngineNarrowQueryProfileSourceStatusV1::access_denied:
      return "access_denied";
    case EngineNarrowQueryProfileSourceStatusV1::profile_unavailable:
      return "profile_unavailable";
    case EngineNarrowQueryProfileSourceStatusV1::binding_stale:
      return "binding_stale";
    case EngineNarrowQueryProfileSourceStatusV1::descriptor_invalid:
      return "descriptor_invalid";
    case EngineNarrowQueryProfileSourceStatusV1::datatype_invalid:
      return "datatype_invalid";
    case EngineNarrowQueryProfileSourceStatusV1::collation_invalid:
      return "collation_invalid";
    case EngineNarrowQueryProfileSourceStatusV1::resource_budget_exceeded:
      return "resource_budget_exceeded";
    case EngineNarrowQueryProfileSourceStatusV1::cancelled:
      return "cancelled";
    case EngineNarrowQueryProfileSourceStatusV1::storage_refused:
      return "storage_refused";
  }
  return "invalid_argument";
}

}  // namespace scratchbird::engine::internal_api
