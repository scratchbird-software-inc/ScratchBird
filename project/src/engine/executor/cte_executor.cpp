// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

//
// SBSQL bounded source-layout anchor. Runtime behavior for this family is
// implemented by the active dispatcher, executor, planner, or function modules
// linked beside this translation unit and covered by the corresponding proof
// gates. Keep family-specific growth in this bounded area or in the named
// shared runtime module, not in broad catch-all files.

#include "descriptor_value_runtime.hpp"

#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic RecursiveCteWorkingRefusal(std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code =
      "QOW-DIAG-QRY-014-WORKING-REFUSAL-V1";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

DescriptorRuntimeDiagnostic RecursiveCteMemoryRefusal(std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code =
      "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

const PhysicalNodeRecord* FindPhysicalNode(const TypedPhysicalNodeDag& dag,
                                           const std::uint64_t node_id) {
  for (const auto& node : dag.nodes) {
    if (node.physical_node_id == node_id) return &node;
  }
  return nullptr;
}

bool IsCanonicalCteEvidenceUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

std::string CanonicalCoreDatatypeUuid(const std::string_view stable_name) {
  static const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows, [&](const auto& row) {
        return row.stable_name == stable_name;
      });
  return found == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : scratchbird::core::uuid::UuidToString(
                   found->descriptor_uuid.value);
}

struct RecursiveCteCancellationPoll {
  DescriptorRuntimeDiagnostic diagnostic;
  bool cancelled = false;
  std::size_t iteration_ordinal = 0;
};

RecursiveCteCancellationPoll PollRecursiveCteCancellation(
    const CanonicalRecursiveCteCancellationProbe& probe,
    const std::size_t iteration_ordinal,
    const std::string_view phase) {
  RecursiveCteCancellationPoll result;
  result.iteration_ordinal = iteration_ordinal;
  if (!probe) return result;
  try {
    if (!probe(iteration_ordinal)) return result;
    result.cancelled = true;
    result.diagnostic = RecursiveCteWorkingRefusal(
        "recursive CTE cancellation observed " + std::string(phase) +
        " at iteration " + std::to_string(iteration_ordinal));
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-CANCELLATION-REFUSAL-V1";
    return result;
  } catch (const std::exception& error) {
    result.diagnostic = RecursiveCteWorkingRefusal(
        "recursive CTE cancellation probe failed " + std::string(phase) +
        ":" + error.what());
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-CANCELLATION-PROBE-V1";
    return result;
  } catch (...) {
    result.diagnostic = RecursiveCteWorkingRefusal(
        "recursive CTE cancellation probe failed " + std::string(phase));
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-CANCELLATION-PROBE-V1";
    return result;
  }
}

struct RecursiveCteBatchValidation {
  DescriptorRuntimeDiagnostic diagnostic;
  std::optional<RecursiveCteCancellationPoll> cancellation;
};

RecursiveCteBatchValidation ValidateRecursiveCteBatch(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& output_descriptor_ids,
    const CanonicalRecursiveCteCancellationProbe& probe,
    const std::size_t iteration_ordinal,
    const std::string_view phase) {
  RecursiveCteBatchValidation result;
  bool validation_cancelled = false;
  result.diagnostic = ValidateCanonicalDescriptorBatch(
      batch, output_descriptor_ids,
      [&]() {
        auto cancellation = PollRecursiveCteCancellation(
            probe, iteration_ordinal, phase);
        if (cancellation.diagnostic.ok) return false;
        result.cancellation = std::move(cancellation);
        return true;
      },
      &validation_cancelled);
  if (!validation_cancelled) result.cancellation.reset();
  return result;
}

bool RecursiveCteCancellationEvidenceBound(
    const TypedPhysicalNodeDag& dag,
    const CanonicalRecursiveCteCancellationProbe& probe,
    const std::string& evidence_uuid) {
  if (!probe) return evidence_uuid.empty();
  const PhysicalAdmissionEvidence* policy_evidence = nullptr;
  for (const auto& evidence : dag.admission_evidence) {
    if (evidence.stage != PhysicalAdmissionStage::kPolicyCapability) continue;
    if (policy_evidence != nullptr) return false;
    policy_evidence = &evidence;
  }
  return !evidence_uuid.empty() && policy_evidence != nullptr &&
         policy_evidence->evidence_uuid == evidence_uuid;
}

bool SameCanonicalCteColumns(
    const std::vector<ExecutorColumnDescriptor>& left,
    const std::vector<ExecutorColumnDescriptor>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto& left_column = left[index];
    const auto& right_column = right[index];
    if (left_column.descriptor_id != right_column.descriptor_id ||
        left_column.stable_name != right_column.stable_name ||
        left_column.nullable != right_column.nullable ||
        left_column.descriptor.descriptor_uuid.canonical !=
            right_column.descriptor.descriptor_uuid.canonical ||
        left_column.descriptor.descriptor_kind !=
            right_column.descriptor.descriptor_kind ||
        left_column.descriptor.canonical_type_name !=
            right_column.descriptor.canonical_type_name ||
        left_column.descriptor.encoded_descriptor !=
            right_column.descriptor.encoded_descriptor) {
      return false;
    }
  }
  return true;
}

struct RecursiveCtePayloadMeasurement {
  bool ok = false;
  std::size_t bytes = 0;
  std::string detail;
  std::optional<RecursiveCteCancellationPoll> cancellation;
};

bool AddRecursiveCteTuplePayload(const DescriptorTuple& row,
                                 std::size_t* bytes) {
  if (bytes == nullptr) return false;
  for (const auto& value : row.values) {
    if (value.encoded_value.size() >
            std::numeric_limits<std::size_t>::max() - *bytes ||
        value.binary_value.size() >
            std::numeric_limits<std::size_t>::max() - *bytes -
                value.encoded_value.size()) {
      return false;
    }
    *bytes += value.encoded_value.size();
    *bytes += value.binary_value.size();
  }
  return true;
}

std::size_t RecursiveCteUnsignedDecimalWidth(std::uint64_t value) {
  std::size_t width = 1;
  while (value >= 10) {
    value /= 10;
    ++width;
  }
  return width;
}

template <typename T>
bool ReserveRecursiveCteVector(std::vector<T>* values,
                               const std::size_t capacity,
                               const std::string_view label,
                               std::string* detail) {
  if (values == nullptr || detail == nullptr ||
      capacity > values->max_size()) {
    if (detail != nullptr) {
      *detail = "recursive CTE " + std::string(label) +
                " capacity exceeds the container limit";
    }
    return false;
  }
  try {
    values->reserve(capacity);
  } catch (const std::bad_alloc&) {
    *detail = "recursive CTE " + std::string(label) +
              " capacity allocation was refused";
    return false;
  } catch (const std::length_error&) {
    *detail = "recursive CTE " + std::string(label) +
              " capacity exceeds the container limit";
    return false;
  }
  if (values->capacity() != capacity) {
    *detail = "recursive CTE " + std::string(label) +
              " capacity differs from its charged logical capacity";
    return false;
  }
  return true;
}

bool AppendNormalizedRecursiveCteTuple(
    const DescriptorTuple& source,
    const std::size_t expected_width,
    std::vector<DescriptorTuple>* destination,
    const std::string_view label,
    std::string* detail) {
  if (destination == nullptr || detail == nullptr ||
      source.values.size() != expected_width ||
      expected_width > source.values.max_size()) {
    if (detail != nullptr) {
      *detail = "recursive CTE " + std::string(label) +
                " row width is invalid";
    }
    return false;
  }
  DescriptorTuple normalized;
  if (!ReserveRecursiveCteVector(
          &normalized.values, expected_width, label, detail)) {
    return false;
  }
  try {
    for (const auto& value : source.values) {
      normalized.values.push_back(value);
    }
    destination->push_back(std::move(normalized));
  } catch (const std::bad_alloc&) {
    *detail = "recursive CTE " + std::string(label) +
              " row allocation was refused";
    return false;
  } catch (const std::length_error&) {
    *detail = "recursive CTE " + std::string(label) +
              " row capacity exceeds the container limit";
    return false;
  }
  return true;
}

class RecursiveCteInt64IdentitySet {
 public:
  bool Initialize(const std::size_t maximum_identity_count,
                  std::string* detail) {
    if (detail == nullptr || maximum_identity_count == 0 ||
        maximum_identity_count >
            std::numeric_limits<std::size_t>::max() / 2) {
      if (detail != nullptr) {
        *detail = "recursive CTE UNION DISTINCT identity capacity "
                  "overflowed";
      }
      return false;
    }
    const auto minimum_slots = maximum_identity_count * 2;
    std::size_t slot_count = 1;
    while (slot_count < minimum_slots) {
      if (slot_count >
          std::numeric_limits<std::size_t>::max() / 2) {
        *detail = "recursive CTE UNION DISTINCT identity capacity "
                  "overflowed";
        return false;
      }
      slot_count *= 2;
    }
    if (slot_count > slots_.max_size() ||
        slot_count > occupied_.max_size()) {
      *detail = "recursive CTE UNION DISTINCT identity capacity exceeds "
                "the container limit";
      return false;
    }
    if (!ReserveRecursiveCteVector(
            &slots_, slot_count, "UNION DISTINCT identity slot", detail) ||
        !ReserveRecursiveCteVector(
            &occupied_, slot_count,
            "UNION DISTINCT identity occupancy", detail)) {
      return false;
    }
    try {
      slots_.resize(slot_count);
      occupied_.resize(slot_count, 0);
    } catch (const std::bad_alloc&) {
      *detail = "recursive CTE UNION DISTINCT identity allocation was "
                "refused";
      return false;
    } catch (const std::length_error&) {
      *detail = "recursive CTE UNION DISTINCT identity capacity exceeds "
                "the container limit";
      return false;
    }
    if (slots_.size() != slot_count || slots_.capacity() != slot_count ||
        occupied_.size() != slot_count ||
        occupied_.capacity() != slot_count) {
      *detail = "recursive CTE UNION DISTINCT identity capacity differs "
                "from its charged logical capacity";
      return false;
    }
    mask_ = slot_count - 1;
    maximum_identity_count_ = maximum_identity_count;
    return true;
  }

  enum class InsertResult : std::uint8_t {
    inserted = 1,
    duplicate,
    full,
  };

  InsertResult Insert(const std::int64_t value, const bool allow_new) {
    if (slots_.empty()) {
      return InsertResult::full;
    }
    auto hash = static_cast<std::uint64_t>(value);
    hash ^= hash >> 30;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94d049bb133111eb);
    hash ^= hash >> 31;
    auto slot = static_cast<std::size_t>(hash) & mask_;
    for (std::size_t probe = 0; probe < slots_.size(); ++probe) {
      if (occupied_[slot] == 0) {
        if (!allow_new || size_ == maximum_identity_count_) {
          return InsertResult::full;
        }
        occupied_[slot] = 1;
        slots_[slot] = value;
        ++size_;
        return InsertResult::inserted;
      }
      if (slots_[slot] == value) return InsertResult::duplicate;
      slot = (slot + 1) & mask_;
    }
    return InsertResult::full;
  }

 private:
  std::vector<std::int64_t> slots_;
  std::vector<std::uint8_t> occupied_;
  std::size_t mask_ = 0;
  std::size_t maximum_identity_count_ = 0;
  std::size_t size_ = 0;
};

RecursiveCtePayloadMeasurement MeasureRecursiveCtePayload(
    const DescriptorBatch& batch,
    const CanonicalRecursiveCteCancellationProbe& cancellation_requested,
    const std::size_t iteration_ordinal,
    const std::string_view phase) {
  RecursiveCtePayloadMeasurement result;
  for (const auto& row : batch.rows) {
    auto cancellation = PollRecursiveCteCancellation(
        cancellation_requested, iteration_ordinal, phase);
    if (!cancellation.diagnostic.ok) {
      result.cancellation = std::move(cancellation);
      return result;
    }
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
              std::numeric_limits<std::size_t>::max() - result.bytes ||
          value.binary_value.size() >
              std::numeric_limits<std::size_t>::max() - result.bytes -
                  value.encoded_value.size()) {
        result.detail =
            "recursive CTE materialized payload accounting overflowed";
        return result;
      }
      result.bytes += value.encoded_value.size();
      result.bytes += value.binary_value.size();
    }
  }
  result.ok = true;
  return result;
}

DescriptorRuntimeDiagnostic BindRecursiveCteMemoryState(
    const TypedPhysicalNodeDag& dag,
    const PhysicalNodeRecord& selected_node,
    const bool enforce_payload_memory_grant,
    const std::size_t retained_input_payload_bytes,
    std::shared_ptr<CanonicalRecursiveCteMemoryState>* state) {
  if (state == nullptr) {
    return RecursiveCteMemoryRefusal(
        "recursive CTE memory receipt state is absent");
  }
  if (!*state) {
    *state = std::make_shared<CanonicalRecursiveCteMemoryState>();
  }
  auto& receipt = **state;
  // Legacy direct contract callers predate exact selected-node payload
  // grants. The canonical live registration explicitly activates enforcement;
  // wrappers and external contract callers remain source compatible.
  if (!enforce_payload_memory_grant && !receipt.enforced) {
    receipt.retained_input_payload_bytes = retained_input_payload_bytes;
    return {};
  }
  if (dag.memory_budget_bytes == 0 ||
      selected_node.memory_bytes_required == 0 ||
      selected_node.memory_bytes_required > dag.memory_budget_bytes ||
      selected_node.memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return RecursiveCteMemoryRefusal(
        "recursive CTE selected-node memory grant is invalid");
  }
  const PhysicalAdmissionEvidence* resource_evidence = nullptr;
  for (const auto& evidence : dag.admission_evidence) {
    if (evidence.stage != PhysicalAdmissionStage::kResource) continue;
    if (resource_evidence != nullptr) {
      return RecursiveCteMemoryRefusal(
          "recursive CTE resource evidence is not unique");
    }
    resource_evidence = &evidence;
  }
  if (resource_evidence == nullptr ||
      resource_evidence->evidence_uuid.empty()) {
    return RecursiveCteMemoryRefusal(
        "recursive CTE resource evidence is absent");
  }
  if (receipt.enforced) {
    if (receipt.grant_bytes != selected_node.memory_bytes_required ||
        receipt.retained_input_payload_bytes !=
            retained_input_payload_bytes ||
        receipt.selected_plan_uuid != dag.selected_plan_uuid ||
        receipt.selected_physical_node_id !=
            selected_node.physical_node_id ||
        receipt.causal_counter_id != selected_node.causal_counter_id ||
        receipt.selected_alternative_uuid !=
            selected_node.selected_alternative_uuid ||
        receipt.cost_vector_uuid != selected_node.cost_vector_uuid ||
        receipt.resource_snapshot_uuid != dag.resource_snapshot_uuid ||
        receipt.resource_epoch != dag.resource_epoch ||
        receipt.resource_evidence_uuid !=
            resource_evidence->evidence_uuid) {
      return RecursiveCteMemoryRefusal(
          "recursive CTE memory receipt identity drifted");
    }
    return {};
  }
  receipt.enforced = true;
  receipt.grant_bytes =
      static_cast<std::size_t>(selected_node.memory_bytes_required);
  receipt.retained_input_payload_bytes = retained_input_payload_bytes;
  receipt.selected_plan_uuid = dag.selected_plan_uuid;
  receipt.selected_physical_node_id = selected_node.physical_node_id;
  receipt.causal_counter_id = selected_node.causal_counter_id;
  receipt.selected_alternative_uuid =
      selected_node.selected_alternative_uuid;
  receipt.cost_vector_uuid = selected_node.cost_vector_uuid;
  receipt.resource_snapshot_uuid = dag.resource_snapshot_uuid;
  receipt.resource_epoch = dag.resource_epoch;
  receipt.resource_evidence_uuid = resource_evidence->evidence_uuid;
  return {};
}

DescriptorRuntimeDiagnostic BindRecursiveCteStructuralMemory(
    const std::shared_ptr<CanonicalRecursiveCteMemoryState>& state,
    const std::size_t structural_bytes) {
  if (!state || structural_bytes == 0) {
    return RecursiveCteMemoryRefusal(
        "recursive CTE structural memory bound is absent");
  }
  if (state->resident_structural_bytes == 0) {
    state->resident_structural_bytes = structural_bytes;
  } else if (state->resident_structural_bytes < structural_bytes) {
    return RecursiveCteMemoryRefusal(
        "recursive CTE structural memory bound drifted");
  }
  if (state->enforced &&
      (state->retained_input_payload_bytes > state->grant_bytes ||
       state->resident_structural_bytes >
           state->grant_bytes - state->retained_input_payload_bytes)) {
    return RecursiveCteMemoryRefusal(
        "recursive CTE structural memory exceeded its selected-node memory grant");
  }
  return {};
}

bool ObserveRecursiveCtePayload(
    const std::shared_ptr<CanonicalRecursiveCteMemoryState>& state,
    const std::initializer_list<std::size_t> transient_payloads,
    const std::string_view phase) {
  if (!state) return false;
  std::size_t current = state->retained_input_payload_bytes;
  const auto add = [&](const std::size_t bytes) {
    if (bytes > std::numeric_limits<std::size_t>::max() - current) {
      return false;
    }
    current += bytes;
    return true;
  };
  if (!add(state->kernel_live_payload_bytes) ||
      !add(state->auxiliary_live_payload_bytes)) {
    state->refusal_detail =
        "recursive CTE materialized payload accounting overflowed " +
        std::string(phase);
    return false;
  }
  for (const auto bytes : transient_payloads) {
    if (!add(bytes)) {
      state->refusal_detail =
          "recursive CTE materialized payload accounting overflowed " +
          std::string(phase);
      return false;
    }
  }
  state->current_live_payload_bytes = current;
  state->peak_live_payload_bytes =
      std::max(state->peak_live_payload_bytes, current);
  if (state->resident_structural_bytes >
      std::numeric_limits<std::size_t>::max() - current) {
    state->refusal_detail =
        "recursive CTE resident memory accounting overflowed " +
        std::string(phase);
    return false;
  }
  state->current_live_memory_bytes =
      current + state->resident_structural_bytes;
  state->peak_live_memory_bytes = std::max(
      state->peak_live_memory_bytes, state->current_live_memory_bytes);
  if (state->enforced &&
      state->current_live_memory_bytes > state->grant_bytes) {
    state->refusal_detail =
        "recursive CTE resident memory exceeded its selected-node "
        "memory grant " + std::string(phase);
    return false;
  }
  return true;
}

}  // namespace

bool BoundCanonicalRecursiveCteStructuralBytes(
    const std::vector<ExecutorColumnDescriptor>& base_columns,
    const ExecutorColumnDescriptor* search_sequence_column,
    const ExecutorColumnDescriptor* cycle_mark_column,
    const std::vector<CanonicalDescriptorOrderTerm>* equality_terms,
    const CanonicalRecursiveCteStructuralCapacity& capacity,
    std::size_t* structural_bytes,
    std::string* detail) {
  if (structural_bytes == nullptr || detail == nullptr ||
      base_columns.empty() ||
      capacity.maximum_iteration_count == 0 ||
      capacity.maximum_working_row_count == 0 ||
      capacity.maximum_recursive_output_row_count == 0 ||
      capacity.maximum_result_row_count == 0) {
    if (detail != nullptr) {
      *detail = "recursive CTE structural capacity contract is invalid";
    }
    return false;
  }
  const bool search_cycle =
      capacity.profile ==
      CanonicalRecursiveCteStructuralProfile::kSearchCycle;
  if (search_cycle != (search_sequence_column != nullptr) ||
      search_cycle != (cycle_mark_column != nullptr) ||
      (search_cycle && base_columns.size() >
                           std::numeric_limits<std::size_t>::max() - 2)) {
    *detail = "recursive CTE projected structural descriptors are invalid";
    return false;
  }
  const auto add = [](const std::size_t left, const std::size_t right,
                      std::size_t* result) {
    if (result == nullptr ||
        right > std::numeric_limits<std::size_t>::max() - left) {
      return false;
    }
    *result = left + right;
    return true;
  };
  const auto multiply = [](const std::size_t left,
                           const std::size_t right,
                           std::size_t* result) {
    if (result == nullptr ||
        (left != 0 &&
         right > std::numeric_limits<std::size_t>::max() / left)) {
      return false;
    }
    *result = left * right;
    return true;
  };
  const auto string_storage = [&](const std::string& value,
                                  std::size_t* bytes) {
    return value.size() != std::numeric_limits<std::size_t>::max() &&
           add(*bytes, value.size() + 1, bytes);
  };
  const auto descriptor_storage = [&](const ExecutorColumnDescriptor& column,
                                      const bool include_stable_name,
                                      std::size_t* bytes) {
    return (!include_stable_name ||
            string_storage(column.stable_name, bytes)) &&
           string_storage(column.descriptor.descriptor_uuid.canonical,
                          bytes) &&
           string_storage(column.descriptor.descriptor_kind, bytes) &&
           string_storage(column.descriptor.canonical_type_name, bytes) &&
           string_storage(column.descriptor.encoded_descriptor, bytes);
  };
  const auto order_term_dynamic_storage = [&]() {
    std::size_t bytes = 0;
    if (equality_terms == nullptr) return std::optional<std::size_t>(bytes);
    if (equality_terms->size() != capacity.equality_term_count) {
      return std::optional<std::size_t>{};
    }
    for (const auto& term : *equality_terms) {
      if (!string_storage(term.collation_uuid, &bytes) ||
          !string_storage(term.text_seed.seed_pack_name, &bytes) ||
          !string_storage(term.text_seed.seed_pack_version, &bytes) ||
          !string_storage(term.text_seed.charset_name, &bytes) ||
          !string_storage(term.text_seed.collation_name, &bytes) ||
          !string_storage(term.timezone_seed.seed_pack_name, &bytes) ||
          !string_storage(term.timezone_seed.seed_pack_version, &bytes) ||
          !string_storage(term.timezone_seed.content_hash, &bytes)) {
        return std::optional<std::size_t>{};
      }
      std::size_t timezone_name_vector_bytes = 0;
      if (!multiply(term.timezone_seed.timezone_names.size(),
                    sizeof(std::string),
                    &timezone_name_vector_bytes) ||
          !add(bytes, timezone_name_vector_bytes, &bytes)) {
        return std::optional<std::size_t>{};
      }
      for (const auto& name : term.timezone_seed.timezone_names) {
        if (!string_storage(name, &bytes)) {
          return std::optional<std::size_t>{};
        }
      }
    }
    return std::optional<std::size_t>(bytes);
  };
  const auto for_each_column = [&](const auto& callback) {
    for (const auto& column : base_columns) {
      if (!callback(column)) return false;
    }
    if (search_sequence_column != nullptr &&
        !callback(*search_sequence_column)) {
      return false;
    }
    if (cycle_mark_column != nullptr && !callback(*cycle_mark_column)) {
      return false;
    }
    return true;
  };
  const auto batch_storage = [&](const std::size_t row_count,
                                 const bool projected,
                                 std::size_t* bytes) {
    std::size_t column_bytes = 0;
    std::size_t row_value_bytes = 0;
    const auto consume = [&](const ExecutorColumnDescriptor& column) {
      if (!add(column_bytes, sizeof(ExecutorColumnDescriptor),
               &column_bytes) ||
          !descriptor_storage(column, true, &column_bytes) ||
          !add(row_value_bytes,
               sizeof(scratchbird::engine::internal_api::EngineTypedValue),
               &row_value_bytes) ||
          !descriptor_storage(column, false, &row_value_bytes)) {
        return false;
      }
      return true;
    };
    if (projected) {
      if (!for_each_column(consume)) return false;
    } else {
      for (const auto& column : base_columns) {
        if (!consume(column)) return false;
      }
    }
    std::size_t tuple_bytes = 0;
    std::size_t rows_bytes = 0;
    std::size_t batch_bytes = 0;
    if (!add(sizeof(DescriptorTuple), row_value_bytes, &tuple_bytes) ||
        !multiply(row_count, tuple_bytes, &rows_bytes) ||
        !add(column_bytes, rows_bytes, &batch_bytes) ||
        !add(*bytes, batch_bytes, bytes)) {
      return false;
    }
    return true;
  };
  const auto row_storage = [&](const std::size_t row_count,
                               std::size_t* bytes) {
    std::size_t row_value_bytes = 0;
    for (const auto& column : base_columns) {
      if (!add(row_value_bytes,
               sizeof(scratchbird::engine::internal_api::EngineTypedValue),
               &row_value_bytes) ||
          !descriptor_storage(column, false, &row_value_bytes)) {
        return false;
      }
    }
    std::size_t tuple_bytes = 0;
    std::size_t rows_bytes = 0;
    return add(sizeof(DescriptorTuple), row_value_bytes, &tuple_bytes) &&
           multiply(row_count, tuple_bytes, &rows_bytes) &&
           add(*bytes, rows_bytes, bytes);
  };
  const auto add_scaled = [&](const std::size_t count,
                              const std::size_t width,
                              std::size_t* bytes) {
    std::size_t scaled = 0;
    return multiply(count, width, &scaled) && add(*bytes, scaled, bytes);
  };
  const auto order_term_dynamic_bytes = order_term_dynamic_storage();
  if (!order_term_dynamic_bytes.has_value()) {
    *detail = "recursive CTE equality-term dynamic storage overflowed";
    return false;
  }

  std::size_t working_bytes = 0;
  if (!batch_storage(capacity.maximum_anchor_row_count, false,
                     &working_bytes) ||
      !batch_storage(capacity.maximum_result_row_count, false,
                     &working_bytes) ||
      !batch_storage(capacity.maximum_working_row_count, false,
                     &working_bytes) ||
      !batch_storage(capacity.maximum_working_row_count, false,
                     &working_bytes) ||
      !batch_storage(capacity.maximum_recursive_output_row_count, false,
                     &working_bytes) ||
      !add_scaled(capacity.maximum_iteration_count,
                  sizeof(CanonicalRecursiveCteIteration),
                  &working_bytes)) {
    *detail = "recursive CTE working structural capacity overflowed";
    return false;
  }
  std::size_t total = working_bytes;
  if (capacity.profile ==
      CanonicalRecursiveCteStructuralProfile::kWorking) {
    *structural_bytes = total;
    return true;
  }
  if (capacity.profile ==
          CanonicalRecursiveCteStructuralProfile::kUnionAll ||
      capacity.profile ==
          CanonicalRecursiveCteStructuralProfile::kUnionDistinctInt64 ||
      capacity.profile ==
          CanonicalRecursiveCteStructuralProfile::kUnionDistinctTyped) {
    if (!batch_storage(capacity.maximum_anchor_row_count, false, &total) ||
        !batch_storage(capacity.maximum_anchor_row_count, false, &total)) {
      *detail = "recursive CTE UNION request structural capacity overflowed";
      return false;
    }
    if (capacity.profile !=
        CanonicalRecursiveCteStructuralProfile::kUnionAll) {
      const auto distinct_rows = std::min(
          capacity.maximum_recursive_output_row_count,
          capacity.maximum_working_row_count);
      if (capacity.equality_term_count == 0 ||
          !batch_storage(capacity.maximum_anchor_row_count, false, &total) ||
          !batch_storage(capacity.maximum_recursive_output_row_count,
                         false, &total) ||
          !batch_storage(distinct_rows, false, &total) ||
          !add_scaled(capacity.equality_term_count,
                      sizeof(CanonicalDescriptorOrderTerm), &total) ||
          !add_scaled(capacity.equality_term_count,
                      sizeof(CanonicalDescriptorOrderTerm), &total) ||
          !add(total, *order_term_dynamic_bytes, &total) ||
          !add(total, *order_term_dynamic_bytes, &total) ||
          !add(total, base_columns.size(), &total)) {
        *detail =
            "recursive CTE UNION DISTINCT structural capacity overflowed";
        return false;
      }
      if (capacity.profile ==
          CanonicalRecursiveCteStructuralProfile::kUnionDistinctInt64) {
        if (capacity.maximum_result_row_count >
            std::numeric_limits<std::size_t>::max() / 2) {
          *detail =
              "recursive CTE UNION DISTINCT identity capacity overflowed";
          return false;
        }
        const auto minimum_slots =
            capacity.maximum_result_row_count * 2;
        std::size_t slot_count = 1;
        while (slot_count < minimum_slots) {
          if (slot_count >
              std::numeric_limits<std::size_t>::max() / 2) {
            *detail =
                "recursive CTE UNION DISTINCT identity capacity overflowed";
            return false;
          }
          slot_count *= 2;
        }
        if (!add_scaled(slot_count,
                        sizeof(std::int64_t) + sizeof(std::uint8_t),
                        &total)) {
          *detail =
              "recursive CTE UNION DISTINCT identity storage overflowed";
          return false;
        }
      } else if (!row_storage(capacity.maximum_result_row_count, &total)) {
        *detail =
            "recursive CTE UNION DISTINCT representative storage overflowed";
        return false;
      }
    }
    if (!add_scaled(base_columns.size(), sizeof(std::uint32_t), &total)) {
      *detail = "recursive CTE term descriptor storage overflowed";
      return false;
    }
    *structural_bytes = total;
    return true;
  }
  if (!search_cycle) {
    *detail = "recursive CTE structural profile is invalid";
    return false;
  }
  total = 0;
  const auto next_working_rows = std::min(
      capacity.maximum_recursive_output_row_count,
      capacity.maximum_working_row_count);
  if (capacity.equality_term_count == 0 ||
      !batch_storage(capacity.maximum_anchor_row_count, false, &total) ||
      !batch_storage(capacity.maximum_anchor_row_count, false, &total) ||
      !batch_storage(capacity.maximum_result_row_count, true, &total) ||
      !batch_storage(capacity.maximum_working_row_count, false, &total) ||
      !batch_storage(next_working_rows, false, &total) ||
      !batch_storage(capacity.maximum_recursive_output_row_count, false,
                     &total) ||
      !add_scaled(capacity.maximum_recursive_output_row_count,
                  sizeof(std::size_t), &total) ||
      !add_scaled(capacity.maximum_result_row_count,
                  sizeof(std::size_t) * 2, &total) ||
      !add_scaled(capacity.maximum_working_row_count,
                  sizeof(std::size_t) * 2, &total) ||
      !add_scaled(std::min(capacity.maximum_result_row_count,
                           capacity.maximum_iteration_count),
                  sizeof(std::size_t), &total) ||
      !add_scaled(capacity.maximum_result_row_count,
                  sizeof(CanonicalRecursiveCteSearchCycleMetadata),
                  &total) ||
      !add_scaled(capacity.equality_term_count,
                  sizeof(CanonicalDescriptorOrderTerm), &total) ||
      !add_scaled(capacity.equality_term_count,
                  sizeof(CanonicalDescriptorOrderTerm), &total) ||
      !add(total, *order_term_dynamic_bytes, &total) ||
      !add(total, *order_term_dynamic_bytes, &total) ||
      !add(total, base_columns.size(), &total) ||
      !add_scaled(base_columns.size() + 2, sizeof(std::uint32_t), &total)) {
    *detail = "recursive CTE SEARCH/CYCLE structural capacity overflowed";
    return false;
  }
  *structural_bytes = total;
  return true;
}

// QOW-SOURCE-QRY-014-WORKING-V1
// Execute the recursive term against the current working relation, replace the
// working relation with the validated intermediate relation, and stop only
// when that intermediate relation is empty. All intermediate state remains
// local until convergence, so malformed input, resource excess, or a
// non-convergent recursive term cannot publish a partial CTE result.
namespace {
template <typename RecursiveStep>
CanonicalRecursiveCteWorkingResult ExecuteCanonicalRecursiveCteWorkingBound(
    const CanonicalRecursiveCteWorkingRequest& request,
    const DescriptorBatch& anchor_batch,
    const RecursiveStep& recursive_step,
    const bool recursive_step_bound,
    const std::string_view accepted_implementation_id,
    const std::size_t retained_input_payload_bytes,
    std::shared_ptr<CanonicalRecursiveCteMemoryState> memory_state) {
  CanonicalRecursiveCteWorkingResult result;
  const auto refuse_diagnostic = [&](DescriptorRuntimeDiagnostic diagnostic,
                                     const bool cancelled = false,
                                     const std::size_t ordinal = 0) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.cancellation_observed = cancelled;
    result.cancellation_iteration_ordinal = ordinal;
    result.working_state_cleaned = true;
    if (cancelled) {
      result.cancellation_evidence_uuid =
          request.cancellation_evidence_uuid;
    }
    return result;
  };
  const auto refuse = [&](std::string detail) {
    return refuse_diagnostic(RecursiveCteWorkingRefusal(std::move(detail)));
  };
  const auto refuse_memory = [&](std::string detail) {
    return refuse_diagnostic(RecursiveCteMemoryRefusal(std::move(detail)));
  };
  const auto poll_cancellation = [&](const std::size_t ordinal,
                                     const std::string_view phase) {
    return PollRecursiveCteCancellation(request.cancellation_requested,
                                        ordinal, phase);
  };
  const auto refuse_poll = [&](RecursiveCteCancellationPoll poll) {
    return refuse_diagnostic(std::move(poll.diagnostic), poll.cancelled,
                             poll.iteration_ordinal);
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected recursive CTE node is not the physical root");
  }

  const auto* selected_node = FindPhysicalNode(
      request.physical_dag, request.selected_physical_node_id);
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          accepted_implementation_id ||
      selected_node->input_physical_node_ids.size() != 2) {
    return refuse("recursive CTE working physical profile is not bound");
  }
  const auto memory_binding = BindRecursiveCteMemoryState(
      request.physical_dag, *selected_node,
      request.enforce_payload_memory_grant,
      retained_input_payload_bytes, &memory_state);
  if (!memory_binding.ok) {
    return refuse_diagnostic(memory_binding);
  }
  const auto measure_payload = [&](const DescriptorBatch& batch,
                                   const std::size_t ordinal,
                                   const std::string_view phase,
                                   std::size_t* bytes)
      -> std::optional<CanonicalRecursiveCteWorkingResult> {
    auto measured = MeasureRecursiveCtePayload(
        batch, request.cancellation_requested, ordinal, phase);
    if (measured.cancellation.has_value()) {
      return refuse_poll(std::move(*measured.cancellation));
    }
    if (!measured.ok) {
      return refuse_memory(std::move(measured.detail));
    }
    *bytes = measured.bytes;
    return std::nullopt;
  };
  const auto* anchor_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[0]);
  const auto* recursive_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[1]);
  const bool literal_values_anchor =
      anchor_node != nullptr &&
      anchor_node->node_kind == PhysicalNodeKind::kValues;
  const bool materialized_count_anchor =
      anchor_node != nullptr &&
      anchor_node->node_kind == PhysicalNodeKind::kAggregate &&
      anchor_node->engine_capability_validated &&
      anchor_node->input_physical_node_ids.size() == 1 &&
      anchor_node->output_descriptor_ids.size() == 1 &&
      anchor_node->logical_semantic_variant_id ==
          "aggregate.global-count-star.v1" &&
      anchor_node->implementation_id == "aggregate.count-star.v1";
  const bool exact_count_recursive_term =
      !materialized_count_anchor ||
      (recursive_node != nullptr &&
       recursive_node->engine_capability_validated &&
       recursive_node->input_physical_node_ids.empty() &&
       recursive_node->output_descriptor_ids.size() == 1 &&
       recursive_node->logical_semantic_variant_id ==
           "cte.recursive-term-int64-increment.v1" &&
       recursive_node->implementation_id ==
           "cte.recursive-term.int64-increment.typed.v1");
  if (anchor_node == nullptr || recursive_node == nullptr ||
      (!literal_values_anchor && !materialized_count_anchor) ||
      !exact_count_recursive_term ||
      recursive_node->node_kind != PhysicalNodeKind::kCte ||
      anchor_node->output_descriptor_ids !=
          recursive_node->output_descriptor_ids ||
      selected_node->output_descriptor_ids !=
          anchor_node->output_descriptor_ids) {
    return refuse("recursive CTE input or output descriptor handles drifted");
  }

  if (!recursive_step_bound || request.maximum_iteration_count == 0 ||
      request.maximum_working_row_count == 0 ||
      request.maximum_result_row_count == 0 ||
      !RecursiveCteCancellationEvidenceBound(
          request.physical_dag, request.cancellation_requested,
          request.cancellation_evidence_uuid) ||
      anchor_batch.rows.size() >
          request.maximum_working_row_count ||
      anchor_batch.rows.size() > request.maximum_result_row_count) {
    return refuse("recursive CTE working resource contract is invalid");
  }
  const auto maximum_recursive_output_row_count =
      request.maximum_recursive_output_row_count == 0
          ? request.maximum_working_row_count
          : request.maximum_recursive_output_row_count;
  CanonicalRecursiveCteStructuralCapacity structural_capacity;
  structural_capacity.profile =
      CanonicalRecursiveCteStructuralProfile::kWorking;
  structural_capacity.maximum_anchor_row_count =
      anchor_batch.rows.size();
  structural_capacity.maximum_iteration_count =
      request.maximum_iteration_count;
  structural_capacity.maximum_working_row_count =
      request.maximum_working_row_count;
  structural_capacity.maximum_recursive_output_row_count =
      maximum_recursive_output_row_count;
  structural_capacity.maximum_result_row_count =
      request.maximum_result_row_count;
  std::size_t structural_bytes = 0;
  std::string structural_detail;
  if (!BoundCanonicalRecursiveCteStructuralBytes(
          anchor_batch.columns, nullptr, nullptr, nullptr,
          structural_capacity, &structural_bytes, &structural_detail)) {
    return refuse_memory(std::move(structural_detail));
  }
  const auto structural_binding = BindRecursiveCteStructuralMemory(
      memory_state, structural_bytes);
  if (!structural_binding.ok) {
    return refuse_diagnostic(structural_binding);
  }
  const auto anchor_validation = ValidateRecursiveCteBatch(
      anchor_batch, anchor_node->output_descriptor_ids,
      request.cancellation_requested, 0, "while validating anchor rows");
  if (anchor_validation.cancellation.has_value()) {
    return refuse_poll(std::move(*anchor_validation.cancellation));
  }
  if (!anchor_validation.diagnostic.ok) {
    return refuse(anchor_validation.diagnostic.diagnostic_code + ":" +
                  anchor_validation.diagnostic.detail);
  }
  if (materialized_count_anchor &&
      (anchor_batch.columns.size() != 1 ||
       anchor_batch.columns.front().descriptor.canonical_type_name !=
           "int64" ||
       anchor_batch.rows.size() != 1 ||
       anchor_batch.rows.front().values.size() != 1 ||
       anchor_batch.rows.front().values.front().is_null ||
       anchor_batch.rows.front().values.front().state !=
           scratchbird::engine::internal_api::EngineValueState::value ||
       !DecodeInt64Value(anchor_batch.rows.front().values.front())
            .ok())) {
    return refuse(
        "recursive CTE COUNT(*) anchor is not one materialized non-null int64 value");
  }
  auto cancellation = poll_cancellation(0, "before anchor publication");
  if (!cancellation.diagnostic.ok) {
    return refuse_poll(std::move(cancellation));
  }

  std::size_t anchor_payload_bytes = 0;
  if (auto failure = measure_payload(
          anchor_batch, 0,
          "while measuring the recursive anchor",
          &anchor_payload_bytes)) {
    return std::move(*failure);
  }
  if (anchor_payload_bytes >
          std::numeric_limits<std::size_t>::max() / 3) {
    return refuse_memory(
        "recursive CTE anchor payload accounting overflowed");
  }
  memory_state->kernel_live_payload_bytes = anchor_payload_bytes * 3;
  if (!ObserveRecursiveCtePayload(
          memory_state, {}, "while retaining the recursive anchor")) {
    return refuse_memory(memory_state->refusal_detail);
  }
  DescriptorBatch accumulated;
  DescriptorBatch working;
  std::vector<CanonicalRecursiveCteIteration> iterations;
  std::string capacity_detail;
  try {
    accumulated.columns = anchor_batch.columns;
    working.columns = anchor_batch.columns;
    if (!ReserveRecursiveCteVector(
            &accumulated.rows, request.maximum_result_row_count,
            "accumulated row", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &working.rows, request.maximum_working_row_count,
            "working row", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &iterations, request.maximum_iteration_count,
            "iteration receipt", &capacity_detail)) {
      return refuse_memory(std::move(capacity_detail));
    }
    for (const auto& row : anchor_batch.rows) {
      if (!AppendNormalizedRecursiveCteTuple(
              row, anchor_batch.columns.size(),
              &accumulated.rows, "accumulated anchor",
              &capacity_detail) ||
          !AppendNormalizedRecursiveCteTuple(
              row, anchor_batch.columns.size(),
              &working.rows, "working anchor", &capacity_detail)) {
        return refuse_memory(std::move(capacity_detail));
      }
    }
  } catch (const std::bad_alloc&) {
    return refuse_memory(
        "recursive CTE retained container allocation was refused");
  } catch (const std::length_error&) {
    return refuse_memory(
        "recursive CTE retained container capacity exceeds the container limit");
  }
  std::size_t maximum_working = working.rows.size();
  std::size_t iteration_ordinal = 0;
  while (!working.rows.empty()) {
    if (iteration_ordinal == request.maximum_iteration_count) {
      return refuse("recursive CTE did not converge within the iteration bound");
    }
    ++iteration_ordinal;
    cancellation =
        poll_cancellation(iteration_ordinal, "before a recursive step");
    if (!cancellation.diagnostic.ok) {
      return refuse_poll(std::move(cancellation));
    }

    DescriptorBatch intermediate;
    const auto pre_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!pre_step_authority.ok) {
      return refuse(pre_step_authority.diagnostic_code + ":" +
                    pre_step_authority.detail);
    }
    try {
      intermediate = recursive_step(working, iteration_ordinal);
    } catch (const std::bad_alloc&) {
      return refuse_memory(
          memory_state && !memory_state->refusal_detail.empty()
              ? memory_state->refusal_detail
              : "recursive CTE step allocation was refused");
    } catch (const std::length_error&) {
      return refuse_memory(
          memory_state && !memory_state->refusal_detail.empty()
              ? memory_state->refusal_detail
              : "recursive CTE step capacity exceeds the container limit");
    } catch (const std::exception& error) {
      if (memory_state && !memory_state->refusal_detail.empty()) {
        return refuse_memory(memory_state->refusal_detail);
      }
      return refuse(std::string("recursive CTE step failed:") + error.what());
    } catch (...) {
      if (memory_state && !memory_state->refusal_detail.empty()) {
        return refuse_memory(memory_state->refusal_detail);
      }
      return refuse("recursive CTE step failed with an unknown exception");
    }
    const auto post_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!post_step_authority.ok) {
      return refuse(post_step_authority.diagnostic_code + ":" +
                    post_step_authority.detail);
    }
    cancellation =
        poll_cancellation(iteration_ordinal, "after a recursive step");
    if (!cancellation.diagnostic.ok) {
      return refuse_poll(std::move(cancellation));
    }
    if (intermediate.rows.size() >
        maximum_recursive_output_row_count) {
      return refuse_memory(
          "recursive CTE recursive output row bound was exceeded");
    }
    std::size_t intermediate_payload_bytes = 0;
    if (auto failure = measure_payload(
            intermediate, iteration_ordinal,
            "while measuring recursive output",
            &intermediate_payload_bytes)) {
      return std::move(*failure);
    }
    if (!ObserveRecursiveCtePayload(
            memory_state, {intermediate_payload_bytes},
            "after a recursive step")) {
      return refuse_memory(memory_state->refusal_detail);
    }
    const auto intermediate_validation = ValidateRecursiveCteBatch(
        intermediate, recursive_node->output_descriptor_ids,
        request.cancellation_requested, iteration_ordinal,
        "while validating recursive output rows");
    if (intermediate_validation.cancellation.has_value()) {
      return refuse_poll(std::move(*intermediate_validation.cancellation));
    }
    if (!intermediate_validation.diagnostic.ok) {
      return refuse(intermediate_validation.diagnostic.diagnostic_code + ":" +
                    intermediate_validation.diagnostic.detail);
    }
    if (!SameCanonicalCteColumns(intermediate.columns,
                                 anchor_batch.columns)) {
      return refuse("recursive CTE generated schema differs from its anchor");
    }
    if (intermediate.rows.size() > request.maximum_working_row_count ||
        intermediate.rows.size() >
            request.maximum_result_row_count - accumulated.rows.size()) {
      return refuse("recursive CTE working or result row bound was exceeded");
    }

    DescriptorBatch normalized_working;
    try {
      normalized_working.columns = anchor_batch.columns;
      if (!ReserveRecursiveCteVector(
              &normalized_working.rows,
              request.maximum_working_row_count,
              "next working row", &capacity_detail)) {
        return refuse_memory(std::move(capacity_detail));
      }
    } catch (const std::bad_alloc&) {
      return refuse_memory(
          "recursive CTE retained container allocation was refused");
    } catch (const std::length_error&) {
      return refuse_memory(
          "recursive CTE retained container capacity exceeds the container limit");
    }
    maximum_working =
        std::max(maximum_working, intermediate.rows.size());
    if (!ObserveRecursiveCtePayload(
            memory_state,
            {intermediate_payload_bytes, intermediate_payload_bytes,
             intermediate_payload_bytes},
            "before retaining recursive output")) {
      return refuse_memory(memory_state->refusal_detail);
    }
    try {
      for (const auto& row : intermediate.rows) {
        if (!AppendNormalizedRecursiveCteTuple(
                row, anchor_batch.columns.size(),
                &accumulated.rows, "accumulated recursive",
                &capacity_detail) ||
            !AppendNormalizedRecursiveCteTuple(
                row, anchor_batch.columns.size(),
                &normalized_working.rows, "next working",
                &capacity_detail)) {
          return refuse_memory(std::move(capacity_detail));
        }
      }
      iterations.push_back({iteration_ordinal, working.rows.size(),
                            normalized_working.rows.size()});
    } catch (const std::bad_alloc&) {
      return refuse_memory(
          "recursive CTE retained row allocation was refused");
    } catch (const std::length_error&) {
      return refuse_memory(
          "recursive CTE retained row capacity exceeds the container limit");
    }
    working = std::move(normalized_working);
    intermediate = {};
    std::size_t accumulated_payload_bytes = 0;
    std::size_t working_payload_bytes = 0;
    if (auto failure = measure_payload(
            accumulated, iteration_ordinal,
            "while measuring accumulated recursive rows",
            &accumulated_payload_bytes)) {
      return std::move(*failure);
    }
    if (auto failure = measure_payload(
            working, iteration_ordinal,
            "while measuring recursive working rows",
            &working_payload_bytes)) {
      return std::move(*failure);
    }
    if (anchor_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                accumulated_payload_bytes ||
        working_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                anchor_payload_bytes - accumulated_payload_bytes) {
      return refuse_memory(
          "recursive CTE retained payload accounting overflowed");
    }
    memory_state->kernel_live_payload_bytes =
        anchor_payload_bytes + accumulated_payload_bytes +
        working_payload_bytes;
    if (!ObserveRecursiveCtePayload(
            memory_state, {}, "after retaining recursive output")) {
      return refuse_memory(memory_state->refusal_detail);
    }
  }

  cancellation =
      poll_cancellation(iteration_ordinal, "before result publication");
  if (!cancellation.diagnostic.ok) {
    return refuse_poll(std::move(cancellation));
  }

  const auto output_validation = ValidateRecursiveCteBatch(
      accumulated, selected_node->output_descriptor_ids,
      request.cancellation_requested, iteration_ordinal,
      "while validating the recursive result");
  if (output_validation.cancellation.has_value()) {
    return refuse_poll(std::move(*output_validation.cancellation));
  }
  if (!output_validation.diagnostic.ok) {
    return refuse(output_validation.diagnostic.diagnostic_code + ":" +
                  output_validation.diagnostic.detail);
  }
  std::size_t output_payload_bytes = 0;
  if (auto failure = measure_payload(
          accumulated, iteration_ordinal,
          "while measuring the recursive result",
          &output_payload_bytes)) {
    return std::move(*failure);
  }
  if (anchor_payload_bytes >
      std::numeric_limits<std::size_t>::max() - output_payload_bytes) {
    return refuse_memory(
        "recursive CTE final payload accounting overflowed");
  }
  memory_state->kernel_live_payload_bytes =
      anchor_payload_bytes + output_payload_bytes;
  if (!ObserveRecursiveCtePayload(
          memory_state, {}, "before recursive result publication")) {
    return refuse_memory(memory_state->refusal_detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(accumulated);
  result.iterations = std::move(iterations);
  result.recursive_iteration_count = iteration_ordinal;
  result.maximum_observed_working_row_count = maximum_working;
  result.converged = true;
  result.working_state_cleaned = true;
  result.output_payload_bytes = output_payload_bytes;
  result.peak_live_payload_bytes =
      memory_state->peak_live_payload_bytes;
  result.resident_structural_bytes =
      memory_state->resident_structural_bytes;
  result.current_live_memory_bytes =
      memory_state->current_live_memory_bytes;
  result.peak_live_memory_bytes =
      memory_state->peak_live_memory_bytes;
  result.memory_grant_bytes = memory_state->grant_bytes;
  result.memory_grant_evidence_uuid =
      memory_state->resource_evidence_uuid;
  if (request.cancellation_requested) {
    result.cancellation_evidence_uuid = request.cancellation_evidence_uuid;
  }
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalRecursiveCteWorkingResult ExecuteCanonicalRecursiveCteWorking(
    const CanonicalRecursiveCteWorkingRequest& request) {
  return ExecuteCanonicalRecursiveCteWorkingBound(
      request, request.anchor_batch, request.recursive_step,
      static_cast<bool>(request.recursive_step),
      "cte.recursive.working.typed.v1",
      request.retained_input_payload_bytes, request.memory_state);
}

// QOW-SOURCE-QRY-014-UNION-V1
// Admit either UNION ALL, which preserves every anchor and recursive row, or
// descriptor-wide UNION DISTINCT. The legacy one-column int64 profile uses
// canonical decoded integer identity plus one shared SQL NULL identity; the
// general typed profile binds one canonical equality term per output column.
// Both remove duplicates against the complete accumulated result and current
// intermediate relation, then feed only newly admitted rows into the next
// recursive working transition.
CanonicalRecursiveCteUnionResult ExecuteCanonicalRecursiveCteUnion(
    const CanonicalRecursiveCteUnionRequest& request) {
  CanonicalRecursiveCteUnionResult result;
  result.union_mode = request.union_mode;
  const auto refuse = [&](std::string detail) {
    result.working_result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-UNION-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    result.working_result.working_state_cleaned = true;
    result.union_mode = request.union_mode;
    result.duplicate_row_count = 0;
    return result;
  };
  const auto refuse_memory = [&](std::string detail) {
    result = {};
    result.working_result.diagnostic =
        RecursiveCteMemoryRefusal(std::move(detail));
    result.working_result.working_state_cleaned = true;
    result.union_mode = request.union_mode;
    return result;
  };
  const auto refuse_poll = [&](RecursiveCteCancellationPoll poll) {
    result = {};
    result.working_result.diagnostic = std::move(poll.diagnostic);
    result.working_result.cancellation_observed = poll.cancelled;
    result.working_result.cancellation_iteration_ordinal =
        poll.iteration_ordinal;
    result.working_result.working_state_cleaned = true;
    if (poll.cancelled) {
      result.working_result.cancellation_evidence_uuid =
          request.working_request.cancellation_evidence_uuid;
    }
    result.union_mode = request.union_mode;
    return result;
  };

  if (request.union_mode != CanonicalRecursiveCteUnionMode::kAll &&
      request.union_mode != CanonicalRecursiveCteUnionMode::kDistinct) {
    return refuse("recursive CTE UNION mode is not bound");
  }
  if (request.working_request.maximum_working_row_count == 0 ||
      request.working_request.maximum_result_row_count == 0 ||
      request.working_request.anchor_batch.rows.size() >
          request.working_request.maximum_working_row_count ||
      request.working_request.anchor_batch.rows.size() >
          request.working_request.maximum_result_row_count) {
    return refuse("recursive CTE UNION row bounds are invalid");
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  const bool distinct_profile =
      selected_node != nullptr &&
      (selected_node->implementation_id ==
           "cte.recursive.union-distinct-int64.typed.v1" ||
       selected_node->implementation_id ==
           "cte.recursive.union-distinct.typed.v1");
  const bool profile_matches =
      request.union_mode == CanonicalRecursiveCteUnionMode::kAll
          ? selected_node != nullptr &&
                selected_node->implementation_id ==
                    "cte.recursive.union-all.typed.v1"
          : distinct_profile;
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      !profile_matches) {
    return refuse("recursive CTE UNION physical profile is not bound");
  }

  auto anchor_measurement = MeasureRecursiveCtePayload(
      request.working_request.anchor_batch,
      request.working_request.cancellation_requested, 0,
      "while measuring the recursive UNION anchor");
  if (anchor_measurement.cancellation.has_value()) {
    return refuse_poll(std::move(*anchor_measurement.cancellation));
  }
  if (!anchor_measurement.ok) {
    result.working_result = {};
    result.working_result.diagnostic =
        RecursiveCteMemoryRefusal(std::move(anchor_measurement.detail));
    result.working_result.working_state_cleaned = true;
    return result;
  }
  if (anchor_measurement.bytes >
      std::numeric_limits<std::size_t>::max() -
          request.working_request.retained_input_payload_bytes) {
    result.working_result = {};
    result.working_result.diagnostic = RecursiveCteMemoryRefusal(
        "recursive CTE UNION retained payload accounting overflowed");
    result.working_result.working_state_cleaned = true;
    return result;
  }
  auto memory_state = request.working_request.memory_state;
  const auto retained_payload_bytes =
      request.working_request.retained_input_payload_bytes +
      anchor_measurement.bytes;
  const auto memory_binding = BindRecursiveCteMemoryState(
      request.working_request.physical_dag, *selected_node,
      request.working_request.enforce_payload_memory_grant,
      retained_payload_bytes, &memory_state);
  if (!memory_binding.ok) {
    result.working_result = {};
    result.working_result.diagnostic = memory_binding;
    result.working_result.working_state_cleaned = true;
    return result;
  }

  CanonicalRecursiveCteStructuralCapacity structural_capacity;
  structural_capacity.profile =
      request.union_mode == CanonicalRecursiveCteUnionMode::kAll
          ? CanonicalRecursiveCteStructuralProfile::kUnionAll
          : selected_node->implementation_id ==
                    "cte.recursive.union-distinct-int64.typed.v1"
                ? CanonicalRecursiveCteStructuralProfile::kUnionDistinctInt64
                : CanonicalRecursiveCteStructuralProfile::kUnionDistinctTyped;
  structural_capacity.maximum_anchor_row_count =
      request.working_request.anchor_batch.rows.size();
  structural_capacity.maximum_iteration_count =
      request.working_request.maximum_iteration_count;
  structural_capacity.maximum_working_row_count =
      request.working_request.maximum_working_row_count;
  structural_capacity.maximum_recursive_output_row_count =
      request.working_request.maximum_recursive_output_row_count == 0
          ? request.working_request.maximum_working_row_count
          : request.working_request.maximum_recursive_output_row_count;
  structural_capacity.maximum_result_row_count =
      request.working_request.maximum_result_row_count;
  structural_capacity.equality_term_count =
      request.union_mode == CanonicalRecursiveCteUnionMode::kDistinct
          ? (request.equality_terms.empty()
                 ? request.working_request.anchor_batch.columns.size()
                 : request.equality_terms.size())
          : 0;
  std::size_t structural_bytes = 0;
  std::string structural_detail;
  if (!BoundCanonicalRecursiveCteStructuralBytes(
          request.working_request.anchor_batch.columns, nullptr, nullptr,
          request.equality_terms.empty() ? nullptr : &request.equality_terms,
          structural_capacity, &structural_bytes, &structural_detail)) {
    return refuse_memory(std::move(structural_detail));
  }
  const auto structural_binding = BindRecursiveCteStructuralMemory(
      memory_state, structural_bytes);
  if (!structural_binding.ok) {
    return refuse_memory(structural_binding.detail);
  }

  memory_state->kernel_live_payload_bytes = anchor_measurement.bytes;
  if (!ObserveRecursiveCtePayload(
          memory_state, {}, "while retaining the recursive UNION request")) {
    result.working_result = {};
    result.working_result.diagnostic =
        RecursiveCteMemoryRefusal(memory_state->refusal_detail);
    result.working_result.working_state_cleaned = true;
    return result;
  }
  const auto& working = request.working_request;
  CanonicalRecursiveCteWorkingResult working_result;
  std::size_t duplicate_count = 0;
  std::optional<RecursiveCteCancellationPoll> cancellation_failure;
  std::size_t identity_count = 0;
  std::size_t representative_count = 0;
  std::size_t comparison_count = 0;
  std::size_t raw_generated_payload_bytes = 0;
  std::size_t admitted_output_payload_bytes = 0;
  bool seen_null = false;
  RecursiveCteInt64IdentitySet seen_values;
  std::vector<DescriptorTuple> seen_rows;
  if (!working.recursive_step) {
    working_result = ExecuteCanonicalRecursiveCteWorkingBound(
        working, working.anchor_batch, working.recursive_step, false,
        selected_node->implementation_id, retained_payload_bytes,
        memory_state);
  } else if (request.union_mode ==
             CanonicalRecursiveCteUnionMode::kDistinct) {
    const auto poll_distinct =
        [probe = &working.cancellation_requested, &cancellation_failure](
            const std::size_t iteration, const std::string_view phase) {
          auto polled =
              PollRecursiveCteCancellation(*probe, iteration, phase);
          if (polled.diagnostic.ok) return;
          cancellation_failure = std::move(polled);
          throw std::runtime_error(
              cancellation_failure->diagnostic.detail);
        };
    const bool legacy_int64_profile =
        selected_node->implementation_id ==
        "cte.recursive.union-distinct-int64.typed.v1";
    if (legacy_int64_profile &&
        (working.anchor_batch.columns.size() != 1 ||
         working.anchor_batch.columns.front()
                 .descriptor.canonical_type_name != "int64")) {
      return refuse("recursive CTE UNION DISTINCT requires one int64 column");
    }
    std::vector<CanonicalDescriptorOrderTerm> equality_terms =
        request.equality_terms;
    if (legacy_int64_profile && equality_terms.empty()) {
      CanonicalDescriptorOrderTerm term;
      term.column = 0;
      term.expression_descriptor_id =
          working.anchor_batch.columns.front().descriptor_id;
      term.direction = CanonicalDescriptorOrderDirection::ascending;
      term.null_placement = CanonicalDescriptorNullPlacement::first;
      equality_terms.push_back(std::move(term));
    }
    if (equality_terms.size() != working.anchor_batch.columns.size() ||
        equality_terms.empty() ||
        request.maximum_value_comparison_count == 0) {
      return refuse(
          "recursive CTE UNION DISTINCT requires one bounded equality term "
          "per output column");
    }
    std::vector<std::uint8_t> covered;
    std::string capacity_detail;
    if (!ReserveRecursiveCteVector(
            &covered, working.anchor_batch.columns.size(),
            "UNION DISTINCT equality coverage", &capacity_detail)) {
      return refuse_memory(std::move(capacity_detail));
    }
    try {
      covered.assign(working.anchor_batch.columns.size(), 0);
    } catch (const std::bad_alloc&) {
      return refuse_memory(
          "recursive CTE UNION DISTINCT equality coverage allocation was refused");
    } catch (const std::length_error&) {
      return refuse_memory(
          "recursive CTE UNION DISTINCT equality coverage capacity exceeds the container limit");
    }
    for (const auto& term : equality_terms) {
      if (term.column >= working.anchor_batch.columns.size() ||
          covered[term.column] ||
          term.expression_descriptor_id !=
              working.anchor_batch.columns[term.column].descriptor_id ||
          term.direction != CanonicalDescriptorOrderDirection::ascending ||
          term.null_placement != CanonicalDescriptorNullPlacement::first) {
        return refuse(
            "recursive CTE UNION DISTINCT equality term coverage is invalid");
      }
      const auto term_validation = ValidateCanonicalDescriptorOrderTerm(
          term, working.anchor_batch.columns[term.column]);
      if (!term_validation.ok) {
        return refuse(term_validation.diagnostic_code + ":" +
                      term_validation.detail);
      }
      covered[term.column] = true;
    }

    if (legacy_int64_profile) {
      if (!seen_values.Initialize(
              working.maximum_result_row_count, &capacity_detail)) {
        return refuse_memory(std::move(capacity_detail));
      }
    } else if (!ReserveRecursiveCteVector(
                   &seen_rows, working.maximum_result_row_count,
                   "UNION DISTINCT representative", &capacity_detail)) {
      return refuse_memory(std::move(capacity_detail));
    }
    const auto equal_rows =
        [&equality_terms, &comparison_count,
         maximum = request.maximum_value_comparison_count, &poll_distinct](
            const DescriptorTuple& left, const DescriptorTuple& right,
            const std::size_t iteration) {
          if (left.values.size() != equality_terms.size() ||
              right.values.size() != equality_terms.size()) {
            throw std::runtime_error(
                "recursive UNION DISTINCT row is ragged");
          }
          for (const auto& term : equality_terms) {
            poll_distinct(iteration,
                          "while comparing recursive UNION DISTINCT rows");
            if (comparison_count == maximum) {
              throw std::runtime_error(
                  "recursive CTE UNION DISTINCT value comparison bound was "
                  "exceeded");
            }
            ++comparison_count;
            const auto compared = CompareCanonicalDescriptorOrderValues(
                left.values[term.column], right.values[term.column], term);
            if (!compared.diagnostic.ok) {
              throw std::runtime_error(compared.diagnostic.diagnostic_code +
                                       ":" + compared.diagnostic.detail);
            }
            if (compared.comparison != 0) return false;
          }
          return true;
        };
    const auto admit =
        [legacy_int64_profile, &identity_count,
         maximum = request.maximum_value_comparison_count,
         &representative_count,
         maximum_representatives = working.maximum_result_row_count,
         &seen_values, &seen_null, &seen_rows, &duplicate_count,
         &poll_distinct, &equal_rows, &memory_state,
         &raw_generated_payload_bytes,
         &admitted_output_payload_bytes](
            const DescriptorTuple& row, const std::size_t iteration) {
          if (legacy_int64_profile) {
            poll_distinct(
                iteration,
                "while admitting recursive UNION DISTINCT rows");
            if (row.values.size() != 1) {
              throw std::runtime_error(
                  "recursive UNION DISTINCT row is ragged");
            }
            if (identity_count == maximum) {
              throw std::runtime_error(
                  "recursive CTE UNION DISTINCT value comparison bound was "
                  "exceeded");
            }
            ++identity_count;
            const auto& value = row.values.front();
            if (value.state == scratchbird::engine::internal_api::
                                   EngineValueState::sql_null) {
              if (seen_null) {
                ++duplicate_count;
                return false;
              }
              if (representative_count == maximum_representatives) {
                throw std::runtime_error(
                    "recursive CTE UNION DISTINCT result row bound was "
                    "exceeded");
              }
              seen_null = true;
              ++representative_count;
              return true;
            }
            const auto decoded = DecodeInt64Value(value);
            if (!decoded.ok()) {
              throw std::runtime_error(
                  decoded.diagnostic.diagnostic_code + ":" +
                  decoded.diagnostic.detail);
            }
            const auto inserted = seen_values.Insert(
                decoded.value,
                representative_count < maximum_representatives);
            if (inserted ==
                RecursiveCteInt64IdentitySet::InsertResult::duplicate) {
              ++duplicate_count;
              return false;
            }
            if (inserted ==
                RecursiveCteInt64IdentitySet::InsertResult::full) {
              throw std::runtime_error(
                  "recursive CTE UNION DISTINCT result row bound was "
                  "exceeded");
            }
            ++representative_count;
            return true;
          }
        if (!equal_rows(row, row, iteration)) {
          throw std::runtime_error(
              "recursive UNION DISTINCT self comparison failed");
        }
        for (const auto& representative : seen_rows) {
          if (equal_rows(row, representative, iteration)) {
            ++duplicate_count;
            return false;
          }
        }
        if (seen_rows.size() == maximum_representatives) {
          throw std::runtime_error(
              "recursive CTE UNION DISTINCT result row bound was exceeded");
        }
        std::size_t row_payload_bytes = 0;
        if (!AddRecursiveCteTuplePayload(row, &row_payload_bytes) ||
            row_payload_bytes >
                std::numeric_limits<std::size_t>::max() -
                    memory_state->auxiliary_live_payload_bytes) {
          memory_state->refusal_detail =
              "recursive CTE UNION DISTINCT seen payload accounting "
              "overflowed";
          throw std::runtime_error(memory_state->refusal_detail);
        }
        const auto prospective_seen_payload =
            memory_state->auxiliary_live_payload_bytes + row_payload_bytes;
        const auto previous_seen_payload =
            memory_state->auxiliary_live_payload_bytes;
        memory_state->auxiliary_live_payload_bytes =
            prospective_seen_payload;
        if (!ObserveRecursiveCtePayload(
                memory_state,
                {raw_generated_payload_bytes,
                 admitted_output_payload_bytes},
                "before retaining a UNION DISTINCT representative")) {
          memory_state->auxiliary_live_payload_bytes =
              previous_seen_payload;
          throw std::runtime_error(memory_state->refusal_detail);
        }
        std::string capacity_detail;
        if (!AppendNormalizedRecursiveCteTuple(
                row, row.values.size(), &seen_rows,
                "UNION DISTINCT representative", &capacity_detail)) {
          memory_state->auxiliary_live_payload_bytes =
              previous_seen_payload;
          memory_state->refusal_detail = std::move(capacity_detail);
          throw std::runtime_error(memory_state->refusal_detail);
        }
        return true;
      };
    DescriptorBatch distinct_anchor;
    try {
      distinct_anchor.columns = working.anchor_batch.columns;
    } catch (const std::bad_alloc&) {
      return refuse_memory(
          "recursive CTE UNION DISTINCT anchor column allocation was refused");
    } catch (const std::length_error&) {
      return refuse_memory(
          "recursive CTE UNION DISTINCT anchor column capacity exceeds the container limit");
    }
    if (!ReserveRecursiveCteVector(
            &distinct_anchor.rows,
            std::min(working.anchor_batch.rows.size(),
                     working.maximum_working_row_count),
            "UNION DISTINCT anchor row", &capacity_detail)) {
      return refuse_memory(std::move(capacity_detail));
    }
    std::size_t distinct_anchor_payload_bytes = 0;
    try {
      for (const auto& row : working.anchor_batch.rows) {
        admitted_output_payload_bytes =
            distinct_anchor_payload_bytes;
        if (admit(row, 0)) {
          std::size_t row_payload_bytes = 0;
          if (!AddRecursiveCteTuplePayload(row, &row_payload_bytes) ||
              row_payload_bytes >
                  std::numeric_limits<std::size_t>::max() -
                      distinct_anchor_payload_bytes) {
            memory_state->refusal_detail =
                "recursive CTE UNION DISTINCT anchor payload accounting "
                "overflowed";
            throw std::runtime_error(memory_state->refusal_detail);
          }
          const auto prospective_anchor_payload =
              distinct_anchor_payload_bytes + row_payload_bytes;
          if (!ObserveRecursiveCtePayload(
                  memory_state, {prospective_anchor_payload},
                  "before retaining the UNION DISTINCT anchor")) {
            throw std::runtime_error(memory_state->refusal_detail);
          }
          if (!AppendNormalizedRecursiveCteTuple(
                  row, working.anchor_batch.columns.size(),
                  &distinct_anchor.rows, "UNION DISTINCT anchor",
                  &capacity_detail)) {
            memory_state->refusal_detail = std::move(capacity_detail);
            throw std::runtime_error(memory_state->refusal_detail);
          }
          distinct_anchor_payload_bytes = prospective_anchor_payload;
          admitted_output_payload_bytes =
              distinct_anchor_payload_bytes;
        }
      }
    } catch (const std::exception& error) {
      if (cancellation_failure.has_value()) {
        return refuse_poll(std::move(*cancellation_failure));
      }
      if (!memory_state->refusal_detail.empty()) {
        result.working_result = {};
        result.working_result.diagnostic =
            RecursiveCteMemoryRefusal(memory_state->refusal_detail);
        result.working_result.working_state_cleaned = true;
        return result;
      }
      return refuse(error.what());
    }
    const auto distinct_recursive_step =
        [recursive_step = &working.recursive_step, &admit, &memory_state,
         &raw_generated_payload_bytes,
         &admitted_output_payload_bytes, &cancellation_failure,
         maximum_working_row_count = working.maximum_working_row_count,
         maximum_recursive_output_row_count =
             structural_capacity.maximum_recursive_output_row_count,
         cancellation_requested = working.cancellation_requested](
            const DescriptorBatch& current, const std::size_t iteration) {
          auto generated = (*recursive_step)(current, iteration);
          if (generated.rows.size() >
              maximum_recursive_output_row_count) {
            return generated;
          }
          auto generated_measurement = MeasureRecursiveCtePayload(
              generated, cancellation_requested, iteration,
              "while measuring raw UNION DISTINCT output");
          if (generated_measurement.cancellation.has_value()) {
            cancellation_failure =
                std::move(*generated_measurement.cancellation);
            throw std::runtime_error(
                cancellation_failure->diagnostic.detail);
          }
          if (!generated_measurement.ok) {
            memory_state->refusal_detail =
                generated_measurement.detail;
            throw std::runtime_error(memory_state->refusal_detail);
          }
          raw_generated_payload_bytes = generated_measurement.bytes;
          if (!ObserveRecursiveCtePayload(
                  memory_state, {generated_measurement.bytes},
                  "after retaining raw UNION DISTINCT output")) {
            throw std::runtime_error(memory_state->refusal_detail);
          }
          DescriptorBatch distinct;
          std::string capacity_detail;
          try {
            distinct.columns = generated.columns;
          } catch (const std::bad_alloc&) {
            memory_state->refusal_detail =
                "recursive CTE UNION DISTINCT output column allocation was refused";
            throw std::runtime_error(memory_state->refusal_detail);
          } catch (const std::length_error&) {
            memory_state->refusal_detail =
                "recursive CTE UNION DISTINCT output column capacity exceeds the container limit";
            throw std::runtime_error(memory_state->refusal_detail);
          }
          if (!ReserveRecursiveCteVector(
                  &distinct.rows,
                  std::min(generated.rows.size(),
                           maximum_working_row_count),
                  "UNION DISTINCT output row", &capacity_detail)) {
            memory_state->refusal_detail = std::move(capacity_detail);
            throw std::runtime_error(memory_state->refusal_detail);
          }
          std::size_t distinct_payload_bytes = 0;
          admitted_output_payload_bytes = 0;
          for (const auto& row : generated.rows) {
            admitted_output_payload_bytes =
                distinct_payload_bytes;
            if (admit(row, iteration)) {
              if (distinct.rows.size() == maximum_working_row_count) {
                throw std::runtime_error(
                    "recursive CTE UNION DISTINCT working row bound was exceeded");
              }
              std::size_t row_payload_bytes = 0;
              if (!AddRecursiveCteTuplePayload(row, &row_payload_bytes) ||
                  row_payload_bytes >
                      std::numeric_limits<std::size_t>::max() -
                          distinct_payload_bytes) {
                memory_state->refusal_detail =
                    "recursive CTE UNION DISTINCT output payload "
                    "accounting overflowed";
                throw std::runtime_error(memory_state->refusal_detail);
              }
              const auto prospective_distinct_payload =
                  distinct_payload_bytes + row_payload_bytes;
              if (!ObserveRecursiveCtePayload(
                      memory_state,
                      {generated_measurement.bytes,
                       prospective_distinct_payload},
                      "before retaining UNION DISTINCT output")) {
                throw std::runtime_error(memory_state->refusal_detail);
              }
              if (!AppendNormalizedRecursiveCteTuple(
                      row, generated.columns.size(), &distinct.rows,
                      "UNION DISTINCT output", &capacity_detail)) {
                memory_state->refusal_detail =
                    std::move(capacity_detail);
                throw std::runtime_error(memory_state->refusal_detail);
              }
              distinct_payload_bytes = prospective_distinct_payload;
              admitted_output_payload_bytes =
                  distinct_payload_bytes;
            }
          }
          return distinct;
        };
    working_result = ExecuteCanonicalRecursiveCteWorkingBound(
        working, distinct_anchor, distinct_recursive_step,
        static_cast<bool>(working.recursive_step),
        selected_node->implementation_id, retained_payload_bytes,
        memory_state);
  } else {
    working_result = ExecuteCanonicalRecursiveCteWorkingBound(
        working, working.anchor_batch, working.recursive_step,
        static_cast<bool>(working.recursive_step),
        selected_node->implementation_id, retained_payload_bytes,
        memory_state);
  }
  if (!working_result.diagnostic.ok) {
    if (cancellation_failure.has_value()) {
      return refuse_poll(std::move(*cancellation_failure));
    }
    if (working_result.cancellation_observed ||
        working_result.diagnostic.diagnostic_code ==
            "QOW-DIAG-QRY-014-CANCELLATION-PROBE-V1") {
      result.working_result = std::move(working_result);
      result.union_mode = request.union_mode;
      return result;
    }
    if (working_result.diagnostic.diagnostic_code ==
        "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1") {
      result.working_result = std::move(working_result);
      result.union_mode = request.union_mode;
      return result;
    }
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse("recursive CTE UNION working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  result.working_result = std::move(working_result);
  result.union_mode = request.union_mode;
  result.duplicate_row_count = duplicate_count;
  result.mga_statement_context =
      request.working_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-014-SEARCH-CYCLE-V1
// Execute the accepted breadth-first SEARCH profile and descriptor-bound typed
// CYCLE keys. Parent indices bind every generated row to one current working
// row, allowing path-local cycle detection. A cycle row is emitted once with a
// typed TRUE mark and is never placed into the next working relation. The
// legacy physical profile is preserved as a one-column int64 specialization.
CanonicalRecursiveCteSearchCycleResult
ExecuteCanonicalRecursiveCteSearchCycle(
    const CanonicalRecursiveCteSearchCycleRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalRecursiveCteSearchCycleResult result;
  const auto refuse_diagnostic = [&](DescriptorRuntimeDiagnostic diagnostic,
                                     const bool cancelled = false,
                                     const std::size_t ordinal = 0) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.cancellation_observed = cancelled;
    result.cancellation_iteration_ordinal = ordinal;
    result.working_state_cleaned = true;
    if (cancelled) {
      result.cancellation_evidence_uuid =
          request.cancellation_evidence_uuid;
    }
    return result;
  };
  const auto refuse = [&](std::string detail) {
    DescriptorRuntimeDiagnostic diagnostic;
    diagnostic.ok = false;
    diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-SEARCH-CYCLE-REFUSAL-V1";
    diagnostic.detail = std::move(detail);
    return refuse_diagnostic(std::move(diagnostic));
  };
  const auto refuse_memory = [&](std::string detail) {
    return refuse_diagnostic(RecursiveCteMemoryRefusal(std::move(detail)));
  };
  const auto poll_cancellation = [&](const std::size_t ordinal,
                                     const std::string_view phase) {
    return PollRecursiveCteCancellation(request.cancellation_requested,
                                        ordinal, phase);
  };
  const auto refuse_poll = [&](RecursiveCteCancellationPoll poll) {
    return refuse_diagnostic(std::move(poll.diagnostic), poll.cancelled,
                             poll.iteration_ordinal);
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  }
  const auto* selected_node = FindPhysicalNode(
      request.physical_dag, request.selected_physical_node_id);
  const bool legacy_int64_profile =
      selected_node != nullptr &&
      selected_node->implementation_id ==
          "cte.recursive.search-breadth-cycle-int64.typed.v1";
  const bool generic_typed_profile =
      selected_node != nullptr &&
      selected_node->implementation_id ==
          "cte.recursive.search-breadth-cycle.typed.v1";
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      (!legacy_int64_profile && !generic_typed_profile) ||
      selected_node->input_physical_node_ids.size() != 2) {
    return refuse("recursive CTE SEARCH/CYCLE physical profile is not bound");
  }
  auto memory_state = request.memory_state;
  const auto memory_binding = BindRecursiveCteMemoryState(
      request.physical_dag, *selected_node,
      request.enforce_payload_memory_grant,
      request.retained_input_payload_bytes, &memory_state);
  if (!memory_binding.ok) {
    return refuse_diagnostic(memory_binding);
  }
  const auto measure_payload = [&](const DescriptorBatch& batch,
                                   const std::size_t ordinal,
                                   const std::string_view phase,
                                   std::size_t* bytes)
      -> std::optional<CanonicalRecursiveCteSearchCycleResult> {
    auto measured = MeasureRecursiveCtePayload(
        batch, request.cancellation_requested, ordinal, phase);
    if (measured.cancellation.has_value()) {
      return refuse_poll(std::move(*measured.cancellation));
    }
    if (!measured.ok) {
      return refuse_memory(std::move(measured.detail));
    }
    *bytes = measured.bytes;
    return std::nullopt;
  };
  if (request.search_order !=
      CanonicalRecursiveCteSearchOrder::kBreadthFirst) {
    return refuse("recursive CTE SEARCH order is outside the accepted profile");
  }

  const auto* anchor_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[0]);
  const auto* recursive_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[1]);
  if (anchor_node == nullptr || recursive_node == nullptr ||
      anchor_node->node_kind != PhysicalNodeKind::kValues ||
      recursive_node->node_kind != PhysicalNodeKind::kCte ||
      anchor_node->output_descriptor_ids !=
          recursive_node->output_descriptor_ids) {
    return refuse("recursive CTE SEARCH/CYCLE inputs are not bound");
  }
  if (!request.recursive_step || request.maximum_iteration_count == 0 ||
      request.maximum_working_row_count == 0 ||
      request.maximum_result_row_count == 0 ||
      !RecursiveCteCancellationEvidenceBound(
          request.physical_dag, request.cancellation_requested,
          request.cancellation_evidence_uuid) ||
      request.anchor_batch.rows.size() >
          request.maximum_working_row_count ||
      request.anchor_batch.rows.size() > request.maximum_result_row_count) {
    return refuse("recursive CTE SEARCH/CYCLE resource contract is invalid");
  }
  const auto maximum_recursive_output_row_count =
      request.maximum_recursive_output_row_count == 0
          ? request.maximum_working_row_count
          : request.maximum_recursive_output_row_count;
  CanonicalRecursiveCteStructuralCapacity structural_capacity;
  structural_capacity.profile =
      CanonicalRecursiveCteStructuralProfile::kSearchCycle;
  structural_capacity.maximum_anchor_row_count =
      request.anchor_batch.rows.size();
  structural_capacity.maximum_iteration_count =
      request.maximum_iteration_count;
  structural_capacity.maximum_working_row_count =
      request.maximum_working_row_count;
  structural_capacity.maximum_recursive_output_row_count =
      maximum_recursive_output_row_count;
  structural_capacity.maximum_result_row_count =
      request.maximum_result_row_count;
  structural_capacity.equality_term_count =
      request.cycle_key_terms.empty()
          ? request.anchor_batch.columns.size()
          : request.cycle_key_terms.size();
  std::size_t structural_bytes = 0;
  std::string structural_detail;
  if (!BoundCanonicalRecursiveCteStructuralBytes(
          request.anchor_batch.columns,
          &request.search_sequence_column, &request.cycle_mark_column,
          request.cycle_key_terms.empty() ? nullptr
                                          : &request.cycle_key_terms,
          structural_capacity, &structural_bytes, &structural_detail)) {
    return refuse_memory(std::move(structural_detail));
  }
  const auto structural_binding = BindRecursiveCteStructuralMemory(
      memory_state, structural_bytes);
  if (!structural_binding.ok) {
    return refuse_diagnostic(structural_binding);
  }
  if (!ObserveRecursiveCtePayload(
          memory_state, {},
          "before allocating SEARCH/CYCLE structural state")) {
    return refuse_memory(memory_state->refusal_detail);
  }
  std::vector<std::uint32_t> output_descriptor_ids;
  std::string capacity_detail;
  if (anchor_node->output_descriptor_ids.size() >
      output_descriptor_ids.max_size() - 2) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE output descriptor capacity exceeds the container limit");
  }
  if (!ReserveRecursiveCteVector(
          &output_descriptor_ids,
          anchor_node->output_descriptor_ids.size() + 2,
          "SEARCH/CYCLE output descriptor", &capacity_detail)) {
    return refuse_memory(std::move(capacity_detail));
  }
  try {
    output_descriptor_ids.insert(
        output_descriptor_ids.end(),
        anchor_node->output_descriptor_ids.begin(),
        anchor_node->output_descriptor_ids.end());
    output_descriptor_ids.push_back(
        request.search_sequence_column.descriptor_id);
    output_descriptor_ids.push_back(
        request.cycle_mark_column.descriptor_id);
  } catch (const std::bad_alloc&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE output descriptor allocation was refused");
  } catch (const std::length_error&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE output descriptor capacity exceeds the container limit");
  }
  const auto int64_type_uuid = CanonicalCoreDatatypeUuid("int64");
  const auto boolean_type_uuid = CanonicalCoreDatatypeUuid("boolean");
  const auto exact_generated_descriptor = [](
                                              const ExecutorColumnDescriptor&
                                                  column,
                                              const std::string_view type_name,
                                              const std::string& type_uuid) {
    return !type_uuid.empty() && !column.nullable &&
           column.descriptor.canonical_type_name == type_name &&
           column.descriptor.encoded_descriptor ==
               "type_uuid=" + type_uuid + ";nullability=non_null";
  };
  if (selected_node->output_descriptor_ids != output_descriptor_ids ||
      request.search_sequence_column.descriptor_id == 0 ||
      !exact_generated_descriptor(request.search_sequence_column, "int64",
                                  int64_type_uuid) ||
      request.cycle_mark_column.descriptor_id == 0 ||
      !exact_generated_descriptor(request.cycle_mark_column, "boolean",
                                  boolean_type_uuid)) {
    return refuse("recursive CTE SEARCH/CYCLE descriptors are not exact");
  }

  std::vector<CanonicalDescriptorOrderTerm> cycle_key_terms;
  try {
    cycle_key_terms = request.cycle_key_terms;
  } catch (const std::bad_alloc&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE key term allocation was refused");
  } catch (const std::length_error&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE key term capacity exceeds the container limit");
  }
  if (legacy_int64_profile && cycle_key_terms.empty()) {
    if (request.cycle_key_column >= request.anchor_batch.columns.size() ||
        request.cycle_key_expression_descriptor_id == 0 ||
        request.anchor_batch.columns[request.cycle_key_column].descriptor_id !=
            request.cycle_key_expression_descriptor_id ||
        request.anchor_batch.columns[request.cycle_key_column]
                .descriptor.canonical_type_name != "int64") {
      return refuse("legacy recursive CTE CYCLE key is not one int64 column");
    }
    CanonicalDescriptorOrderTerm term;
    term.column = request.cycle_key_column;
    term.expression_descriptor_id =
        request.cycle_key_expression_descriptor_id;
    term.direction = CanonicalDescriptorOrderDirection::ascending;
    term.null_placement = CanonicalDescriptorNullPlacement::first;
    cycle_key_terms.push_back(std::move(term));
  }
  if (cycle_key_terms.empty() ||
      request.maximum_value_comparison_count == 0) {
    return refuse(
        "recursive CTE SEARCH/CYCLE requires bounded typed cycle keys");
  }
  std::vector<std::uint8_t> covered;
  if (!ReserveRecursiveCteVector(
          &covered, request.anchor_batch.columns.size(),
          "SEARCH/CYCLE key coverage", &capacity_detail)) {
    return refuse_memory(std::move(capacity_detail));
  }
  try {
    covered.assign(request.anchor_batch.columns.size(), 0);
  } catch (const std::bad_alloc&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE key coverage allocation was refused");
  } catch (const std::length_error&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE key coverage capacity exceeds the container limit");
  }
  for (const auto& term : cycle_key_terms) {
    if (term.column >= request.anchor_batch.columns.size() ||
        covered[term.column] ||
        term.expression_descriptor_id !=
            request.anchor_batch.columns[term.column].descriptor_id ||
        term.direction != CanonicalDescriptorOrderDirection::ascending ||
        term.null_placement != CanonicalDescriptorNullPlacement::first) {
      return refuse("recursive CTE SEARCH/CYCLE key binding is invalid");
    }
    const auto term_validation = ValidateCanonicalDescriptorOrderTerm(
        term, request.anchor_batch.columns[term.column]);
    if (!term_validation.ok) {
      return refuse(term_validation.diagnostic_code + ":" +
                    term_validation.detail);
    }
    covered[term.column] = true;
  }

  const auto anchor_validation = ValidateRecursiveCteBatch(
      request.anchor_batch, anchor_node->output_descriptor_ids,
      request.cancellation_requested, 0, "while validating anchor rows");
  if (anchor_validation.cancellation.has_value()) {
    return refuse_poll(std::move(*anchor_validation.cancellation));
  }
  if (!anchor_validation.diagnostic.ok) {
    return refuse(anchor_validation.diagnostic.diagnostic_code + ":" +
                  anchor_validation.diagnostic.detail);
  }
  auto cancellation = poll_cancellation(0, "before anchor publication");
  if (!cancellation.diagnostic.ok) {
    return refuse_poll(std::move(cancellation));
  }

  std::size_t value_comparison_count = 0;
  const auto cycle_keys_equal = [&](const DescriptorTuple& left,
                                    const DescriptorTuple& right,
                                    const bool right_is_projected_output) {
    const auto base_width = request.anchor_batch.columns.size();
    const auto right_width =
        right_is_projected_output ? base_width + 2 : base_width;
    if (left.values.size() != base_width ||
        right.values.size() != right_width) {
      throw std::runtime_error("recursive CTE cycle-key row is ragged");
    }
    for (const auto& term : cycle_key_terms) {
      if (value_comparison_count ==
          request.maximum_value_comparison_count) {
        throw std::runtime_error(
            "recursive CTE SEARCH/CYCLE value comparison bound was exceeded");
      }
      ++value_comparison_count;
      const auto compared = CompareCanonicalDescriptorOrderValues(
          left.values[term.column], right.values[term.column], term);
      if (!compared.diagnostic.ok) {
        throw std::runtime_error(compared.diagnostic.diagnostic_code + ":" +
                                 compared.diagnostic.detail);
      }
      if (compared.comparison != 0) return false;
    }
    return true;
  };
  const auto sequence_value = [&](const std::uint64_t sequence) {
    if (sequence == 0 ||
        sequence > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())) {
      throw std::runtime_error(
          "recursive CTE SEARCH sequence exceeds int64 result width");
    }
    api::EngineTypedValue value;
    value.descriptor = request.search_sequence_column.descriptor;
    value.encoded_value = std::to_string(sequence);
    value.state = api::EngineValueState::value;
    return value;
  };
  const auto cycle_value = [&](const bool cycle) {
    api::EngineTypedValue value;
    value.descriptor = request.cycle_mark_column.descriptor;
    value.encoded_value = cycle ? "true" : "false";
    value.state = api::EngineValueState::value;
    return value;
  };
  const auto measure_projected_payload =
      [&](const DescriptorTuple& row, const std::uint64_t next_sequence,
          const bool cycle, std::size_t* bytes) {
        if (bytes == nullptr || next_sequence == 0 ||
            next_sequence > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
          return false;
        }
        *bytes = 0;
        if (!AddRecursiveCteTuplePayload(row, bytes)) return false;
        const auto sequence_bytes =
            RecursiveCteUnsignedDecimalWidth(next_sequence);
        const std::size_t cycle_bytes = cycle ? 4 : 5;
        if (sequence_bytes >
                std::numeric_limits<std::size_t>::max() - *bytes ||
            cycle_bytes >
                std::numeric_limits<std::size_t>::max() - *bytes -
                    sequence_bytes) {
          return false;
        }
        *bytes += sequence_bytes + cycle_bytes;
        return true;
      };

  std::size_t anchor_payload_bytes = 0;
  if (auto failure = measure_payload(
          request.anchor_batch, 0,
          "while measuring the SEARCH/CYCLE anchor",
          &anchor_payload_bytes)) {
    return std::move(*failure);
  }
  std::size_t output_payload_bytes = 0;
  std::size_t working_payload_bytes = anchor_payload_bytes;
  if (anchor_payload_bytes >
      std::numeric_limits<std::size_t>::max() / 2) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE anchor payload accounting overflowed");
  }
  memory_state->kernel_live_payload_bytes = anchor_payload_bytes * 2;
  if (!ObserveRecursiveCtePayload(
          memory_state, {}, "while retaining the SEARCH/CYCLE anchor")) {
    return refuse_memory(memory_state->refusal_detail);
  }
  DescriptorBatch output;
  DescriptorBatch working;
  constexpr auto kNoParent = std::numeric_limits<std::size_t>::max();
  struct RetainedPathNode {
    std::size_t output_row_index;
    std::size_t parent_node_index;
  };
  std::vector<RetainedPathNode> path_nodes;
  std::vector<std::size_t> working_path_node_indices;
  std::vector<std::size_t> ancestor_node_indices;
  std::vector<CanonicalRecursiveCteSearchCycleMetadata> metadata;
  const auto ancestor_capacity = std::min(
      request.maximum_result_row_count,
      request.maximum_iteration_count);
  try {
    if (request.anchor_batch.columns.size() >
        output.columns.max_size() - 2) {
      return refuse_memory(
          "recursive CTE SEARCH/CYCLE projected column capacity exceeds the container limit");
    }
    if (!ReserveRecursiveCteVector(
            &output.columns, request.anchor_batch.columns.size() + 2,
            "SEARCH/CYCLE output column", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &output.rows, request.maximum_result_row_count,
            "SEARCH/CYCLE output row", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &working.rows, request.maximum_working_row_count,
            "SEARCH/CYCLE working row", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &path_nodes, request.maximum_result_row_count,
            "SEARCH/CYCLE retained path", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &working_path_node_indices,
            request.maximum_working_row_count,
            "SEARCH/CYCLE working path index", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &ancestor_node_indices, ancestor_capacity,
            "SEARCH/CYCLE ancestry", &capacity_detail) ||
        !ReserveRecursiveCteVector(
            &metadata, request.maximum_result_row_count,
            "SEARCH/CYCLE metadata", &capacity_detail)) {
      return refuse_memory(std::move(capacity_detail));
    }
    output.columns.insert(output.columns.end(),
                          request.anchor_batch.columns.begin(),
                          request.anchor_batch.columns.end());
    output.columns.push_back(request.search_sequence_column);
    output.columns.push_back(request.cycle_mark_column);
    working.columns = request.anchor_batch.columns;
    for (const auto& row : request.anchor_batch.rows) {
      if (!AppendNormalizedRecursiveCteTuple(
              row, request.anchor_batch.columns.size(),
              &working.rows, "SEARCH/CYCLE working anchor",
              &capacity_detail)) {
        return refuse_memory(std::move(capacity_detail));
      }
    }
  } catch (const std::bad_alloc&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE retained container allocation was refused");
  } catch (const std::length_error&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE retained container capacity exceeds the container limit");
  }
  std::uint64_t sequence = 0;
  try {
    for (const auto& row : working.rows) {
      cancellation = poll_cancellation(0, "while binding anchor rows");
      if (!cancellation.diagnostic.ok) {
        return refuse_poll(std::move(cancellation));
      }
      if (!cycle_keys_equal(row, row, false)) {
        throw std::runtime_error(
            "recursive CTE cycle key self comparison failed");
      }
      std::size_t projected_payload_bytes = 0;
      if (sequence == std::numeric_limits<std::uint64_t>::max() ||
          !measure_projected_payload(
              row, sequence + 1, false,
              &projected_payload_bytes) ||
          projected_payload_bytes >
              std::numeric_limits<std::size_t>::max() -
                  output_payload_bytes ||
          !ObserveRecursiveCtePayload(
              memory_state, {projected_payload_bytes},
              "before retaining a projected SEARCH/CYCLE anchor row")) {
        if (memory_state->refusal_detail.empty()) {
          memory_state->refusal_detail =
              "recursive CTE SEARCH/CYCLE projected anchor payload "
              "accounting overflowed";
        }
        throw std::runtime_error(memory_state->refusal_detail);
      }
      ++sequence;
      DescriptorTuple projected;
      if (!ReserveRecursiveCteVector(
              &projected.values,
              request.anchor_batch.columns.size() + 2,
              "SEARCH/CYCLE projected value", &capacity_detail)) {
        memory_state->refusal_detail = std::move(capacity_detail);
        throw std::runtime_error(memory_state->refusal_detail);
      }
      projected.values.insert(projected.values.end(),
                              row.values.begin(), row.values.end());
      projected.values.push_back(sequence_value(sequence));
      projected.values.push_back(cycle_value(false));
      output.rows.push_back(std::move(projected));
      output_payload_bytes += projected_payload_bytes;
      memory_state->kernel_live_payload_bytes =
          anchor_payload_bytes + working_payload_bytes +
          output_payload_bytes;
      if (!ObserveRecursiveCtePayload(
              memory_state, {},
              "after retaining a projected SEARCH/CYCLE anchor row")) {
        throw std::runtime_error(memory_state->refusal_detail);
      }
      metadata.push_back(
          {output.rows.size() - 1, 0, sequence, false});
      path_nodes.push_back({output.rows.size() - 1, kNoParent});
      working_path_node_indices.push_back(path_nodes.size() - 1);
    }
  } catch (const std::bad_alloc&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE projected anchor allocation was refused");
  } catch (const std::length_error&) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE projected anchor capacity exceeds the container limit");
  } catch (const std::exception& error) {
    if (!memory_state->refusal_detail.empty()) {
      return refuse_memory(memory_state->refusal_detail);
    }
    return refuse(error.what());
  }

  std::size_t iteration = 0;
  std::size_t cycle_rows = 0;
  while (!working.rows.empty()) {
    if (iteration == request.maximum_iteration_count) {
      return refuse("recursive CTE SEARCH/CYCLE did not converge");
    }
    ++iteration;
    cancellation = poll_cancellation(iteration, "before a recursive step");
    if (!cancellation.diagnostic.ok) {
      return refuse_poll(std::move(cancellation));
    }
    CanonicalRecursiveCteGeneratedBatch generated;
    const auto pre_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!pre_step_authority.ok) {
      return refuse(pre_step_authority.diagnostic_code + ":" +
                    pre_step_authority.detail);
    }
    try {
      generated = request.recursive_step(working, iteration);
    } catch (const std::bad_alloc&) {
      return refuse_memory(
          memory_state && !memory_state->refusal_detail.empty()
              ? memory_state->refusal_detail
              : "recursive CTE SEARCH/CYCLE step allocation was refused");
    } catch (const std::length_error&) {
      return refuse_memory(
          memory_state && !memory_state->refusal_detail.empty()
              ? memory_state->refusal_detail
              : "recursive CTE SEARCH/CYCLE step capacity exceeds the container limit");
    } catch (const std::exception& error) {
      if (memory_state && !memory_state->refusal_detail.empty()) {
        return refuse_memory(memory_state->refusal_detail);
      }
      return refuse(std::string("recursive CTE SEARCH/CYCLE step failed:") +
                    error.what());
    } catch (...) {
      if (memory_state && !memory_state->refusal_detail.empty()) {
        return refuse_memory(memory_state->refusal_detail);
      }
      return refuse("recursive CTE SEARCH/CYCLE step failed");
    }
    const auto post_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!post_step_authority.ok) {
      return refuse(post_step_authority.diagnostic_code + ":" +
                    post_step_authority.detail);
    }
    cancellation = poll_cancellation(iteration, "after a recursive step");
    if (!cancellation.diagnostic.ok) {
      return refuse_poll(std::move(cancellation));
    }
    if (generated.batch.rows.size() >
        maximum_recursive_output_row_count) {
      return refuse_memory(
          "recursive CTE SEARCH/CYCLE recursive output row bound was exceeded");
    }
    if (generated.parent_working_row_indices.size() >
        maximum_recursive_output_row_count) {
      return refuse_memory(
          "recursive CTE SEARCH/CYCLE parent carrier row bound was exceeded");
    }
    std::size_t generated_payload_bytes = 0;
    if (auto failure = measure_payload(
            generated.batch, iteration,
            "while measuring recursive SEARCH/CYCLE output",
            &generated_payload_bytes)) {
      return std::move(*failure);
    }
    if (anchor_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                output_payload_bytes ||
        working_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                anchor_payload_bytes - output_payload_bytes ||
        generated_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                anchor_payload_bytes - output_payload_bytes -
                working_payload_bytes) {
      return refuse_memory(
          "recursive CTE SEARCH/CYCLE generated payload accounting "
          "overflowed");
    }
    memory_state->kernel_live_payload_bytes =
        anchor_payload_bytes + output_payload_bytes +
        working_payload_bytes + generated_payload_bytes;
    if (!ObserveRecursiveCtePayload(
            memory_state, {}, "after a SEARCH/CYCLE recursive step")) {
      return refuse_memory(memory_state->refusal_detail);
    }
    const auto generated_validation = ValidateRecursiveCteBatch(
        generated.batch, recursive_node->output_descriptor_ids,
        request.cancellation_requested, iteration,
        "while validating recursive SEARCH/CYCLE rows");
    if (generated_validation.cancellation.has_value()) {
      return refuse_poll(std::move(*generated_validation.cancellation));
    }
    if (!generated_validation.diagnostic.ok) {
      return refuse(generated_validation.diagnostic.diagnostic_code + ":" +
                    generated_validation.diagnostic.detail);
    }
    if (!SameCanonicalCteColumns(generated.batch.columns,
                                 request.anchor_batch.columns)) {
      return refuse(
          "recursive CTE SEARCH/CYCLE generated schema differs from its anchor");
    }
    if (generated.parent_working_row_indices.size() !=
            generated.batch.rows.size() ||
        working_path_node_indices.size() != working.rows.size() ||
        output.rows.size() > request.maximum_result_row_count ||
        generated.batch.rows.size() >
            request.maximum_result_row_count - output.rows.size()) {
      return refuse("recursive CTE SEARCH/CYCLE parent or result bound failed");
    }
    DescriptorBatch next_working;
    std::vector<std::size_t> next_working_path_node_indices;
    std::size_t next_working_payload_bytes = 0;
    try {
      next_working.columns = request.anchor_batch.columns;
      const auto next_working_capacity =
          std::min(generated.batch.rows.size(),
                   request.maximum_working_row_count);
      if (!ReserveRecursiveCteVector(
              &next_working.rows, next_working_capacity,
              "SEARCH/CYCLE next working row", &capacity_detail) ||
          !ReserveRecursiveCteVector(
              &next_working_path_node_indices,
              next_working_capacity,
              "SEARCH/CYCLE next working path index", &capacity_detail)) {
        memory_state->refusal_detail = std::move(capacity_detail);
        throw std::runtime_error(memory_state->refusal_detail);
      }
      for (std::size_t row_index = 0;
           row_index < generated.batch.rows.size(); ++row_index) {
        cancellation =
            poll_cancellation(iteration, "while processing generated rows");
        if (!cancellation.diagnostic.ok) {
          return refuse_poll(std::move(cancellation));
        }
        const auto parent_index =
            generated.parent_working_row_indices[row_index];
        if (parent_index >= working.rows.size() ||
            parent_index >= working_path_node_indices.size()) {
          return refuse("recursive CTE SEARCH/CYCLE parent is unresolved");
        }
        const auto parent_path_node_index =
            working_path_node_indices[parent_index];
        if (parent_path_node_index >= path_nodes.size()) {
          return refuse("recursive CTE SEARCH/CYCLE path parent is unresolved");
        }
        const auto& generated_row = generated.batch.rows[row_index];
        if (!cycle_keys_equal(generated_row, generated_row, false)) {
          throw std::runtime_error(
              "recursive CTE cycle key self comparison failed");
        }
        ancestor_node_indices.clear();
        auto ancestor_node_index = parent_path_node_index;
        while (ancestor_node_index != kNoParent) {
          if (ancestor_node_index >= path_nodes.size()) {
            return refuse(
                "recursive CTE SEARCH/CYCLE path ancestry is unresolved");
          }
          ancestor_node_indices.push_back(ancestor_node_index);
          const auto next_ancestor =
              path_nodes[ancestor_node_index].parent_node_index;
          if (next_ancestor != kNoParent &&
              next_ancestor >= ancestor_node_index) {
            return refuse(
                "recursive CTE SEARCH/CYCLE path ancestry is cyclic");
          }
          ancestor_node_index = next_ancestor;
        }
        bool cycle = false;
        for (auto ancestor = ancestor_node_indices.rbegin();
             ancestor != ancestor_node_indices.rend(); ++ancestor) {
          cancellation =
              poll_cancellation(iteration, "while comparing cycle ancestry");
          if (!cancellation.diagnostic.ok) {
            return refuse_poll(std::move(cancellation));
          }
          const auto output_row_index =
              path_nodes[*ancestor].output_row_index;
          if (output_row_index >= output.rows.size()) {
            return refuse(
                "recursive CTE SEARCH/CYCLE path output is unresolved");
          }
          if (cycle_keys_equal(generated_row,
                               output.rows[output_row_index], true)) {
            cycle = true;
            break;
          }
        }

        std::size_t projected_payload_bytes = 0;
        std::size_t generated_row_payload_bytes = 0;
        if (sequence == std::numeric_limits<std::uint64_t>::max() ||
            !measure_projected_payload(
                generated_row, sequence + 1, cycle,
                &projected_payload_bytes) ||
            !AddRecursiveCteTuplePayload(
                generated_row, &generated_row_payload_bytes) ||
            projected_payload_bytes >
                std::numeric_limits<std::size_t>::max() -
                    output_payload_bytes ||
            (!cycle && generated_row_payload_bytes >
                std::numeric_limits<std::size_t>::max() -
                    next_working_payload_bytes)) {
          memory_state->refusal_detail =
              "recursive CTE SEARCH/CYCLE retained payload accounting "
              "overflowed";
          throw std::runtime_error(memory_state->refusal_detail);
        }
        const auto prospective_output_payload =
            output_payload_bytes + projected_payload_bytes;
        const auto prospective_next_working_payload =
            next_working_payload_bytes +
            (cycle ? 0 : generated_row_payload_bytes);
        memory_state->kernel_live_payload_bytes =
            anchor_payload_bytes + output_payload_bytes +
            working_payload_bytes + generated_payload_bytes +
            next_working_payload_bytes;
        if (!ObserveRecursiveCtePayload(
                memory_state,
                {projected_payload_bytes,
                 cycle ? 0 : generated_row_payload_bytes},
                "before retaining a SEARCH/CYCLE result row")) {
          throw std::runtime_error(memory_state->refusal_detail);
        }
        ++sequence;
        DescriptorTuple projected;
        if (!ReserveRecursiveCteVector(
                &projected.values,
                request.anchor_batch.columns.size() + 2,
                "SEARCH/CYCLE projected value", &capacity_detail)) {
          memory_state->refusal_detail = std::move(capacity_detail);
          throw std::runtime_error(memory_state->refusal_detail);
        }
        projected.values.insert(projected.values.end(),
                                generated_row.values.begin(),
                                generated_row.values.end());
        projected.values.push_back(sequence_value(sequence));
        projected.values.push_back(cycle_value(cycle));
        output.rows.push_back(std::move(projected));
        output_payload_bytes = prospective_output_payload;
        metadata.push_back(
            {output.rows.size() - 1, iteration, sequence, cycle});
        if (cycle) {
          ++cycle_rows;
          continue;
        }
        if (next_working.rows.size() ==
            request.maximum_working_row_count) {
          return refuse("recursive CTE SEARCH/CYCLE working bound exceeded");
        }
        if (!AppendNormalizedRecursiveCteTuple(
                generated_row, request.anchor_batch.columns.size(),
                &next_working.rows, "SEARCH/CYCLE next working",
                &capacity_detail)) {
          memory_state->refusal_detail = std::move(capacity_detail);
          throw std::runtime_error(memory_state->refusal_detail);
        }
        next_working_payload_bytes =
            prospective_next_working_payload;
        path_nodes.push_back(
            {output.rows.size() - 1, parent_path_node_index});
        next_working_path_node_indices.push_back(path_nodes.size() - 1);
      }
    } catch (const std::bad_alloc&) {
      return refuse_memory(
          "recursive CTE SEARCH/CYCLE retained row allocation was refused");
    } catch (const std::length_error&) {
      return refuse_memory(
          "recursive CTE SEARCH/CYCLE retained row capacity exceeds the container limit");
    } catch (const std::exception& error) {
      if (!memory_state->refusal_detail.empty()) {
        return refuse_memory(memory_state->refusal_detail);
      }
      return refuse(error.what());
    }
    working = std::move(next_working);
    working_payload_bytes = next_working_payload_bytes;
    working_path_node_indices =
        std::move(next_working_path_node_indices);
    memory_state->kernel_live_payload_bytes =
        anchor_payload_bytes + output_payload_bytes +
        working_payload_bytes;
    if (!ObserveRecursiveCtePayload(
            memory_state, {}, "after retaining SEARCH/CYCLE output")) {
      return refuse_memory(memory_state->refusal_detail);
    }
  }

  cancellation = poll_cancellation(iteration, "before result publication");
  if (!cancellation.diagnostic.ok) {
    return refuse_poll(std::move(cancellation));
  }

  const auto output_validation = ValidateRecursiveCteBatch(
      output, output_descriptor_ids, request.cancellation_requested,
      iteration, "while validating the recursive SEARCH/CYCLE result");
  if (output_validation.cancellation.has_value()) {
    return refuse_poll(std::move(*output_validation.cancellation));
  }
  if (!output_validation.diagnostic.ok) {
    return refuse(output_validation.diagnostic.diagnostic_code + ":" +
                  output_validation.diagnostic.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  if (anchor_payload_bytes >
      std::numeric_limits<std::size_t>::max() - output_payload_bytes) {
    return refuse_memory(
        "recursive CTE SEARCH/CYCLE final payload accounting overflowed");
  }
  memory_state->kernel_live_payload_bytes =
      anchor_payload_bytes + output_payload_bytes;
  if (!ObserveRecursiveCtePayload(
          memory_state, {},
          "before SEARCH/CYCLE result publication")) {
    return refuse_memory(memory_state->refusal_detail);
  }
  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.row_metadata = std::move(metadata);
  result.recursive_iteration_count = iteration;
  result.cycle_row_count = cycle_rows;
  result.converged = true;
  result.working_state_cleaned = true;
  result.output_payload_bytes = output_payload_bytes;
  result.peak_live_payload_bytes =
      memory_state->peak_live_payload_bytes;
  result.resident_structural_bytes =
      memory_state->resident_structural_bytes;
  result.current_live_memory_bytes =
      memory_state->current_live_memory_bytes;
  result.peak_live_memory_bytes =
      memory_state->peak_live_memory_bytes;
  result.memory_grant_bytes = memory_state->grant_bytes;
  result.memory_grant_evidence_uuid =
      memory_state->resource_evidence_uuid;
  if (request.cancellation_requested) {
    result.cancellation_evidence_uuid = request.cancellation_evidence_uuid;
  }
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-014-RESOURCE-V1
// Charge every anchor and intermediate encoded value payload against the
// resource admission evidence bound into the selected physical DAG. The
// shared working executor still owns convergence and row/iteration limits;
// this wrapper adds one explicit byte grant and reports cleanup on every exit.
CanonicalRecursiveCteResourceResult ExecuteCanonicalRecursiveCteResource(
    const CanonicalRecursiveCteResourceRequest& request) {
  CanonicalRecursiveCteResourceResult result;
  result.working_state_cleaned = true;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    result.working_state_cleaned = true;
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.resource-bounded.typed.v1") {
    return refuse("recursive CTE resource physical profile is not bound");
  }
  const auto resource_evidence = std::find_if(
      request.working_request.physical_dag.admission_evidence.begin(),
      request.working_request.physical_dag.admission_evidence.end(),
      [](const PhysicalAdmissionEvidence& evidence) {
        return evidence.stage == PhysicalAdmissionStage::kResource;
      });
  if (request.maximum_materialized_value_bytes == 0 ||
      request.memory_grant_evidence_uuid.empty() ||
      resource_evidence ==
          request.working_request.physical_dag.admission_evidence.end() ||
      resource_evidence->evidence_uuid !=
          request.memory_grant_evidence_uuid) {
    return refuse("recursive CTE memory grant evidence is not bound");
  }

  const auto batch_bytes = [](const DescriptorBatch& batch) {
    std::size_t bytes = 0;
    for (const auto& row : batch.rows) {
      for (const auto& value : row.values) {
        if (value.encoded_value.size() >
            std::numeric_limits<std::size_t>::max() - bytes) {
          throw std::runtime_error("recursive CTE byte accounting overflow");
        }
        bytes += value.encoded_value.size();
        if (value.binary_value.size() >
            std::numeric_limits<std::size_t>::max() - bytes) {
          throw std::runtime_error("recursive CTE byte accounting overflow");
        }
        bytes += value.binary_value.size();
      }
    }
    return bytes;
  };

  auto materialized_bytes = std::make_shared<std::size_t>(0);
  try {
    *materialized_bytes = batch_bytes(request.working_request.anchor_batch);
  } catch (const std::exception& error) {
    return refuse(error.what());
  }
  if (*materialized_bytes > request.maximum_materialized_value_bytes) {
    return refuse("recursive CTE anchor exceeded the encoded-value byte grant");
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  for (auto& node : working.physical_dag.nodes) {
    if (node.physical_node_id == working.selected_physical_node_id) {
      node.implementation_id = "cte.recursive.working.typed.v1";
      break;
    }
  }
  const auto recursive_step = working.recursive_step;
  const auto maximum_bytes = request.maximum_materialized_value_bytes;
  const auto maximum_recursive_output_row_count =
      working.maximum_recursive_output_row_count == 0
          ? working.maximum_working_row_count
          : working.maximum_recursive_output_row_count;
  working.recursive_step =
      [recursive_step, materialized_bytes, maximum_bytes,
       maximum_recursive_output_row_count, batch_bytes](
          const DescriptorBatch& current, const std::size_t iteration) {
        auto intermediate = recursive_step(current, iteration);
        if (intermediate.rows.size() >
            maximum_recursive_output_row_count) {
          return intermediate;
        }
        const auto bytes = batch_bytes(intermediate);
        if (bytes > maximum_bytes - *materialized_bytes) {
          throw std::runtime_error(
              "recursive CTE encoded-value byte grant was exceeded");
        }
        *materialized_bytes += bytes;
        return intermediate;
      };

  auto working_result = ExecuteCanonicalRecursiveCteWorking(working);
  if (!working_result.diagnostic.ok) {
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse("recursive CTE resource working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  result.working_result = std::move(working_result);
  result.materialized_value_bytes = *materialized_bytes;
  result.working_state_cleaned = true;
  result.memory_grant_evidence_uuid = request.memory_grant_evidence_uuid;
  result.mga_statement_context =
      request.working_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-014-CANCELLATION-V1
// Probe cancellation before the anchor can be published and before each
// recursive step. The shared working executor keeps every accumulated and
// intermediate row private, allowing this wrapper to discard the entire state
// at the exact observed boundary without becoming transaction authority.
CanonicalRecursiveCteCancellationResult
ExecuteCanonicalRecursiveCteCancellation(
    const CanonicalRecursiveCteCancellationRequest& request) {
  CanonicalRecursiveCteCancellationResult result;
  result.working_state_cleaned = true;
  const auto refuse = [&](std::string detail, const bool cancelled = false,
                          const std::size_t ordinal = 0) {
    result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-CANCELLATION-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    result.cancelled = cancelled;
    result.cancellation_iteration_ordinal = ordinal;
    result.working_state_cleaned = true;
    if (cancelled) {
      result.cancellation_evidence_uuid =
          request.cancellation_evidence_uuid;
    }
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.cancellable.typed.v1") {
    return refuse("recursive CTE cancellation physical profile is not bound");
  }
  const auto policy_evidence = std::find_if(
      request.working_request.physical_dag.admission_evidence.begin(),
      request.working_request.physical_dag.admission_evidence.end(),
      [](const PhysicalAdmissionEvidence& evidence) {
        return evidence.stage == PhysicalAdmissionStage::kPolicyCapability;
      });
  if (!request.cancellation_requested ||
      request.cancellation_evidence_uuid.empty() ||
      policy_evidence ==
          request.working_request.physical_dag.admission_evidence.end() ||
      policy_evidence->evidence_uuid !=
          request.cancellation_evidence_uuid) {
    return refuse("recursive CTE cancellation evidence is not bound");
  }

  try {
    if (request.cancellation_requested(0)) {
      return refuse("recursive CTE was cancelled before anchor publication",
                    true, 0);
    }
  } catch (const std::exception& error) {
    return refuse(std::string("recursive CTE cancellation probe failed:") +
                  error.what());
  } catch (...) {
    return refuse("recursive CTE cancellation probe failed");
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  for (auto& node : working.physical_dag.nodes) {
    if (node.physical_node_id == working.selected_physical_node_id) {
      node.implementation_id = "cte.recursive.working.typed.v1";
      break;
    }
  }
  const auto recursive_step = working.recursive_step;
  const auto cancellation_probe = request.cancellation_requested;
  auto cancelled = std::make_shared<bool>(false);
  auto cancellation_ordinal = std::make_shared<std::size_t>(0);
  working.recursive_step =
      [recursive_step, cancellation_probe, cancelled, cancellation_ordinal](
          const DescriptorBatch& current, const std::size_t iteration) {
        if (cancellation_probe(iteration)) {
          *cancelled = true;
          *cancellation_ordinal = iteration;
          throw std::runtime_error("recursive CTE cancellation observed");
        }
        return recursive_step(current, iteration);
      };

  auto working_result = ExecuteCanonicalRecursiveCteWorking(working);
  if (*cancelled) {
    return refuse("recursive CTE cancellation observed", true,
                  *cancellation_ordinal);
  }
  if (!working_result.diagnostic.ok) {
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse("recursive CTE cancellation working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  result.working_result = std::move(working_result);
  result.cancelled = false;
  result.cancellation_iteration_ordinal = 0;
  result.working_state_cleaned = true;
  result.cancellation_evidence_uuid = request.cancellation_evidence_uuid;
  result.mga_statement_context =
      request.working_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-014-MGA-V1
// Consume engine-owned transaction-inventory evidence for the anchor boundary
// and every recursive transition, including the final empty transition. This
// executor validates ordered evidence but never chooses a snapshot, decides
// visibility/security, or acquires commit, rollback, WAL, or finality authority.
CanonicalRecursiveCteMgaResult ExecuteCanonicalRecursiveCteMgaBoundary(
    const CanonicalRecursiveCteMgaRequest& request) {
  CanonicalRecursiveCteMgaResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-MGA-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.working_request.physical_dag);
  const auto working_authority_validation =
      RevalidateCanonicalExecutionMgaAuthority(
          request.working_request.mga_authority,
          request.working_request.physical_dag);
  if (!authority_validation.ok || !working_authority_validation.ok ||
      !PhysicalMgaStatementContextEqual(
          request.mga_authority.statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse(authority_validation.ok && working_authority_validation.ok
                      ? "recursive CTE working context differs from MGA boundary"
                      : (!authority_validation.ok
                             ? authority_validation.diagnostic_code + ":" +
                                   authority_validation.detail
                             : working_authority_validation.diagnostic_code +
                                   ":" + working_authority_validation.detail));
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.mga-boundary.typed.v1") {
    return refuse("recursive CTE MGA physical profile is not bound");
  }

  const auto mga_admission = std::find_if(
      request.working_request.physical_dag.admission_evidence.begin(),
      request.working_request.physical_dag.admission_evidence.end(),
      [](const PhysicalAdmissionEvidence& evidence) {
        return evidence.stage ==
               PhysicalAdmissionStage::kMgaStatementBoundary;
      });
  if (request.transaction_inventory_id == 0 ||
      !IsCanonicalCteEvidenceUuid(
          request.transaction_inventory_evidence_uuid) ||
      mga_admission ==
          request.working_request.physical_dag.admission_evidence.end() ||
      mga_admission->evidence_uuid !=
          request.transaction_inventory_evidence_uuid) {
    return refuse("recursive CTE transaction inventory evidence is not bound");
  }
  if (request.maximum_boundary_rechecks == 0 ||
      request.iteration_evidence.empty() ||
      request.iteration_evidence.size() > request.maximum_boundary_rechecks) {
    return refuse("recursive CTE MGA boundary recheck contract is invalid");
  }

  std::unordered_set<std::string> evidence_uuids;
  evidence_uuids.insert(request.transaction_inventory_evidence_uuid);
  for (std::size_t index = 0; index < request.iteration_evidence.size();
       ++index) {
    const auto& evidence = request.iteration_evidence[index];
    if (evidence.iteration_ordinal != index ||
        evidence.creator_local_transaction_id == 0 ||
        !IsCanonicalCteEvidenceUuid(evidence.engine_evidence_uuid) ||
        !evidence_uuids.insert(evidence.engine_evidence_uuid).second) {
      return refuse("recursive CTE iteration MGA evidence is not bound");
    }
    if (evidence.visibility != CanonicalMgaVisibilityDecision::kVisible) {
      return refuse("recursive CTE iteration is not MGA-visible");
    }
    if (!CanonicalMgaCreatorVisibleToStatement(
            request.mga_authority.statement_context,
            evidence.creator_local_transaction_id)) {
      return refuse(
          "recursive CTE visible iteration contradicts captured MGA vector");
    }
    if (evidence.security_decision !=
        CanonicalMgaSecurityDecision::kAllowed) {
      return refuse("recursive CTE iteration is not security-allowed");
    }
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  working.mga_authority = request.mga_authority;
  for (auto& node : working.physical_dag.nodes) {
    if (node.physical_node_id == working.selected_physical_node_id) {
      node.implementation_id = "cte.recursive.working.typed.v1";
      break;
    }
  }
  auto working_result = ExecuteCanonicalRecursiveCteWorking(working);
  if (!working_result.diagnostic.ok) {
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  if (request.iteration_evidence.size() !=
      working_result.recursive_iteration_count + 1) {
    return refuse("recursive CTE MGA evidence cardinality is not exact");
  }
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.mga_authority.statement_context)) {
    return refuse("recursive CTE MGA working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }

  result.working_result = std::move(working_result);
  result.iteration_evidence_count = request.iteration_evidence.size();
  result.mga_boundary_proven = true;
  result.transaction_inventory_evidence_uuid =
      request.transaction_inventory_evidence_uuid;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
