// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "typed_delete_carrier_codec.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <limits>
#include <string_view>
#include <tuple>

namespace scratchbird::wire {
namespace {
bool Fail(TypedDeleteCarrierError* error, std::string_view field,
          std::string_view code = "SBLR.OPERAND_INVALID") {
  if (error) { error->diagnostic_code = code; error->field = field; }
  return false;
}
bool Present(std::span<const byte> value) {
  return std::any_of(value.begin(), value.end(), [](byte b) { return b != 0; });
}
void Put(std::vector<byte>& out, std::size_t offset, u64 value, unsigned width = 8) {
  for (unsigned i = 0; i < width; ++i) out[offset + i] = static_cast<byte>(value >> (i * 8));
}
u64 Get(std::span<const byte> in, std::size_t offset, unsigned width = 8) {
  u64 value = 0;
  for (unsigned i = 0; i < width; ++i) value |= static_cast<u64>(in[offset + i]) << (i * 8);
  return value;
}
template<std::size_t N> void Put(std::vector<byte>& out, std::size_t offset,
                                const std::array<byte, N>& value) {
  std::copy(value.begin(), value.end(), out.begin() + offset);
}
template<std::size_t N> void Get(std::span<const byte> in, std::size_t offset,
                                std::array<byte, N>& value) {
  std::copy_n(in.begin() + offset, N, value.begin());
}
void Header(std::vector<byte>& out, std::string_view magic, u16 header) {
  std::copy(magic.begin(), magic.end(), out.begin());
  Put(out, 4, 1, 2); Put(out, 6, header, 2); Put(out, 8, out.size(), 4);
}
bool HeaderValid(std::span<const byte> in, std::string_view magic, u16 header, u32 total) {
  return in.size() == total && total >= header &&
      std::equal(magic.begin(), magic.end(), in.begin()) &&
      Get(in, 4, 2) == 1 && Get(in, 6, 2) == header &&
      Get(in, 8, 4) == total && Get(in, 12, 4) == 0;
}
bool Digest(std::string_view domain, std::span<const byte> first,
            std::span<const byte> second, TypedUpdateHash* digest) {
  std::vector<byte> bytes(domain.begin(), domain.end());
  bytes.insert(bytes.end(), first.begin(), first.end());
  bytes.insert(bytes.end(), second.begin(), second.end());
  const auto hashed = scratchbird::core::hash::ComputeSha256Digest(bytes);
  if (!hashed.ok()) return false;
  *digest = hashed.digest; return true;
}
constexpr std::string_view descriptor_domain = "ScratchBird.SblrDmlDeleteRowsDescriptor.V1";
constexpr std::string_view result_domain = "ScratchBird.SblrDmlDeleteRowsResult.V1";
constexpr std::string_view journal_domain = "ScratchBird.SblrDmlDeleteRowsJournalRecord.V1";

bool DescriptorValid(const TypedDeleteDescriptorCarrier& v, TypedDeleteCarrierError* e) {
  if (!Present(v.descriptor_uuid)) return Fail(e, "descriptor_uuid");
  if (!Present(v.authenticated_statement_receipt_uuid)) return Fail(e, "authenticated_statement_receipt_uuid");
  if (!Present(v.operation_uuid)) return Fail(e, "operation_uuid");
  if (!Present(v.owning_transaction_uuid)) return Fail(e, "owning_transaction_uuid");
  if (!Present(v.statement_snapshot_uuid)) return Fail(e, "statement_snapshot_uuid");
  if (!Present(v.catalog_snapshot_uuid)) return Fail(e, "catalog_snapshot_uuid");
  if (!Present(v.security_context_uuid)) return Fail(e, "security_context_uuid");
  if (!Present(v.security_snapshot_uuid)) return Fail(e, "security_snapshot_uuid");
  if (!Present(v.target_relation_uuid)) return Fail(e, "target_relation_uuid");
  if (!Present(v.target_relation_occurrence_uuid)) return Fail(e, "target_relation_occurrence_uuid");
  if (!Present(v.predicate_expression_uuid)) return Fail(e, "predicate_expression_uuid");
  if (!Present(v.row_policy_set_uuid)) return Fail(e, "row_policy_set_uuid");
  if (!Present(v.constraint_set_uuid)) return Fail(e, "constraint_set_uuid");
  if (!Present(v.trigger_set_uuid)) return Fail(e, "trigger_set_uuid");
  if (!Present(v.deterministic_target_order_uuid)) return Fail(e, "deterministic_target_order_uuid");
  if (!Present(v.resource_budget_uuid)) return Fail(e, "resource_budget_uuid");
  if (!Present(v.recovery_token_uuid)) return Fail(e, "recovery_token_uuid");
  if (!Present(v.builtin_operator_snapshot_uuid)) return Fail(e, "builtin_operator_snapshot_uuid");
  if (!v.descriptor_generation) return Fail(e, "descriptor_generation");
  if (!v.structural_occurrence_id) return Fail(e, "structural_occurrence_id");
  if (!v.operation_generation) return Fail(e, "operation_generation");
  if (!v.owning_local_transaction_id) return Fail(e, "owning_local_transaction_id");
  if (!v.catalog_generation) return Fail(e, "catalog_generation");
  if (!v.datatype_registry_generation) return Fail(e, "datatype_registry_generation");
  if (!v.security_generation) return Fail(e, "security_generation");
  if (!v.target_relation_generation) return Fail(e, "target_relation_generation");
  if (!v.target_relation_occurrence_generation) return Fail(e, "target_relation_occurrence_generation");
  if (!v.predicate_expression_generation) return Fail(e, "predicate_expression_generation");
  if (!v.predicate_root_node_id) return Fail(e, "predicate_root_node_id");
  if (!v.row_policy_set_generation) return Fail(e, "row_policy_set_generation");
  if (!v.constraint_set_generation) return Fail(e, "constraint_set_generation");
  if (!v.trigger_set_generation) return Fail(e, "trigger_set_generation");
  if (!v.deterministic_target_order_generation) return Fail(e, "deterministic_target_order_generation");
  if (!v.resource_budget_generation) return Fail(e, "resource_budget_generation");
  if (!v.recovery_generation) return Fail(e, "recovery_generation");
  if (!v.executor_availability_generation) return Fail(e, "executor_availability_generation");
  if (!v.builtin_operator_registry_generation) return Fail(e, "builtin_operator_registry_generation");
  if (!Present(v.predicate_vector_sha256)) return Fail(e, "predicate_vector_sha256");
  if (!Present(v.row_policy_set_sha256)) return Fail(e, "row_policy_set_sha256");
  if (!Present(v.ordered_constraint_set_sha256)) return Fail(e, "ordered_constraint_set_sha256");
  if (!Present(v.ordered_trigger_set_sha256)) return Fail(e, "ordered_trigger_set_sha256");
  if ((v.predicate_node_count != 1 && v.predicate_node_count != 3) ||
      v.predicate_root_node_id != v.predicate_node_count || v.row_policy_count > 1 ||
      v.constraint_count > kTypedUpdateMaximumFrozenRecords ||
      v.trigger_count > kTypedUpdateMaximumFrozenRecords) return Fail(e, "counts");
  if (v.builtin_operator_snapshot_uuid != kTypedUpdateOperatorSnapshotUuid ||
      v.builtin_operator_registry_generation != 1) return Fail(e, "operator_snapshot", "DATATYPE.DESCRIPTOR_INVALID");
  return true;
}
bool ResultValid(const TypedDeleteResultCarrier& v, TypedDeleteCarrierError* e) {
  if (!Present(v.delete_descriptor_uuid) || !v.delete_descriptor_generation ||
      !Present(v.operation_uuid) || !Present(v.owning_transaction_uuid) ||
      !v.owning_local_transaction_id || !Present(v.relation_uuid) ||
      !v.relation_generation || !Present(v.publication_barrier_uuid) ||
      !v.publication_barrier_generation || !Present(v.effect_set_sha256) ||
      !Present(v.executor_evidence_sha256) || v.matched_count != v.deleted_count ||
      v.deleted_count > kTypedUpdateMaximumCandidateRows)
    return Fail(e, "result_fields", "DML.DELETE_FAILED");
  return true;
}
}  // namespace

bool EncodeTypedDeleteDescriptor(const TypedDeleteDescriptorCarrier& v,
    std::vector<byte>* out, TypedDeleteCarrierError* e) {
  if (e) *e = {};
  if (!out) return Fail(e, "output");
  if (!DescriptorValid(v, e)) return false;
  std::vector<byte> bytes(kTypedDeleteDescriptorBytes, 0);
  Header(bytes, "DDDC", kTypedDeleteDescriptorBytes);
  Put(bytes, 16, v.descriptor_uuid);
  Put(bytes, 40, v.authenticated_statement_receipt_uuid);
  Put(bytes, 64, v.operation_uuid);
  Put(bytes, 88, v.owning_transaction_uuid);
  Put(bytes, 112, v.statement_snapshot_uuid);
  Put(bytes, 128, v.catalog_snapshot_uuid);
  Put(bytes, 160, v.security_context_uuid);
  Put(bytes, 176, v.security_snapshot_uuid);
  Put(bytes, 200, v.target_relation_uuid);
  Put(bytes, 224, v.target_relation_occurrence_uuid);
  Put(bytes, 248, v.predicate_expression_uuid);
  Put(bytes, 320, v.row_policy_set_uuid);
  Put(bytes, 384, v.constraint_set_uuid);
  Put(bytes, 448, v.trigger_set_uuid);
  Put(bytes, 512, v.deterministic_target_order_uuid);
  Put(bytes, 536, v.resource_budget_uuid);
  Put(bytes, 560, v.recovery_token_uuid);
  Put(bytes, 592, v.builtin_operator_snapshot_uuid);
  Put(bytes, 32, v.descriptor_generation);
  Put(bytes, 56, v.structural_occurrence_id);
  Put(bytes, 80, v.operation_generation);
  Put(bytes, 104, v.owning_local_transaction_id);
  Put(bytes, 144, v.catalog_generation);
  Put(bytes, 152, v.datatype_registry_generation);
  Put(bytes, 192, v.security_generation);
  Put(bytes, 216, v.target_relation_generation);
  Put(bytes, 240, v.target_relation_occurrence_generation);
  Put(bytes, 264, v.predicate_expression_generation);
  Put(bytes, 272, v.predicate_root_node_id);
  Put(bytes, 336, v.row_policy_set_generation);
  Put(bytes, 400, v.constraint_set_generation);
  Put(bytes, 464, v.trigger_set_generation);
  Put(bytes, 528, v.deterministic_target_order_generation);
  Put(bytes, 552, v.resource_budget_generation);
  Put(bytes, 576, v.recovery_generation);
  Put(bytes, 584, v.executor_availability_generation);
  Put(bytes, 608, v.builtin_operator_registry_generation);
  Put(bytes, 280, v.predicate_node_count, 4);
  Put(bytes, 344, v.row_policy_count, 4);
  Put(bytes, 408, v.constraint_count, 4);
  Put(bytes, 472, v.trigger_count, 4);
  Put(bytes, 288, v.predicate_vector_sha256);
  Put(bytes, 352, v.row_policy_set_sha256);
  Put(bytes, 416, v.ordered_constraint_set_sha256);
  Put(bytes, 480, v.ordered_trigger_set_sha256);
  TypedUpdateHash digest{};
  if (!Digest(descriptor_domain, std::span<const byte>(bytes).first(616), {}, &digest))
    return Fail(e, "hash");
  Put(bytes, 616, digest); *out = std::move(bytes); return true;
}
bool DecodeAndValidateTypedDeleteDescriptor(std::span<const byte> in,
    TypedDeleteDescriptorCarrier* out, TypedDeleteCarrierError* e) {
  if (e) *e = {};
  if (!out || !HeaderValid(in, "DDDC", 648, 648)) return Fail(e, "header");
  for (auto offset : {284, 348, 412, 476})
    if (Get(in, offset, 4)) return Fail(e, "reserved");
  TypedDeleteDescriptorCarrier v;
  Get(in, 16, v.descriptor_uuid);
  Get(in, 40, v.authenticated_statement_receipt_uuid);
  Get(in, 64, v.operation_uuid);
  Get(in, 88, v.owning_transaction_uuid);
  Get(in, 112, v.statement_snapshot_uuid);
  Get(in, 128, v.catalog_snapshot_uuid);
  Get(in, 160, v.security_context_uuid);
  Get(in, 176, v.security_snapshot_uuid);
  Get(in, 200, v.target_relation_uuid);
  Get(in, 224, v.target_relation_occurrence_uuid);
  Get(in, 248, v.predicate_expression_uuid);
  Get(in, 320, v.row_policy_set_uuid);
  Get(in, 384, v.constraint_set_uuid);
  Get(in, 448, v.trigger_set_uuid);
  Get(in, 512, v.deterministic_target_order_uuid);
  Get(in, 536, v.resource_budget_uuid);
  Get(in, 560, v.recovery_token_uuid);
  Get(in, 592, v.builtin_operator_snapshot_uuid);
  Get(in, 288, v.predicate_vector_sha256);
  Get(in, 352, v.row_policy_set_sha256);
  Get(in, 416, v.ordered_constraint_set_sha256);
  Get(in, 480, v.ordered_trigger_set_sha256);
  v.descriptor_generation = static_cast<u64>(Get(in, 32));
  v.structural_occurrence_id = static_cast<u64>(Get(in, 56));
  v.operation_generation = static_cast<u64>(Get(in, 80));
  v.owning_local_transaction_id = static_cast<u64>(Get(in, 104));
  v.catalog_generation = static_cast<u64>(Get(in, 144));
  v.datatype_registry_generation = static_cast<u64>(Get(in, 152));
  v.security_generation = static_cast<u64>(Get(in, 192));
  v.target_relation_generation = static_cast<u64>(Get(in, 216));
  v.target_relation_occurrence_generation = static_cast<u64>(Get(in, 240));
  v.predicate_expression_generation = static_cast<u64>(Get(in, 264));
  v.predicate_root_node_id = static_cast<u64>(Get(in, 272));
  v.row_policy_set_generation = static_cast<u64>(Get(in, 336));
  v.constraint_set_generation = static_cast<u64>(Get(in, 400));
  v.trigger_set_generation = static_cast<u64>(Get(in, 464));
  v.deterministic_target_order_generation = static_cast<u64>(Get(in, 528));
  v.resource_budget_generation = static_cast<u64>(Get(in, 552));
  v.recovery_generation = static_cast<u64>(Get(in, 576));
  v.executor_availability_generation = static_cast<u64>(Get(in, 584));
  v.builtin_operator_registry_generation = static_cast<u64>(Get(in, 608));
  v.predicate_node_count = static_cast<u32>(Get(in, 280, 4));
  v.row_policy_count = static_cast<u32>(Get(in, 344, 4));
  v.constraint_count = static_cast<u32>(Get(in, 408, 4));
  v.trigger_count = static_cast<u32>(Get(in, 472, 4));
  Get(in, 616, v.descriptor_evidence_sha256);
  std::vector<byte> canonical;
  if (!EncodeTypedDeleteDescriptor(v, &canonical, e)) return false;
  if (!std::equal(canonical.begin(), canonical.end(), in.begin())) return Fail(e, "canonical_evidence");
  v.exact_bytes = std::move(canonical); *out = std::move(v); return true;
}

namespace {
using State = TypedUpdateJournalState;
bool ResultOwner(const TypedDeleteDescriptorCarrier& d, const TypedDeleteResultCarrier& r) {
  return d.descriptor_uuid == r.delete_descriptor_uuid &&
      d.descriptor_generation == r.delete_descriptor_generation &&
      d.operation_uuid == r.operation_uuid && d.owning_transaction_uuid == r.owning_transaction_uuid &&
      d.owning_local_transaction_id == r.owning_local_transaction_id &&
      d.target_relation_uuid == r.relation_uuid && d.target_relation_generation == r.relation_generation;
}
bool JournalBytes(const TypedDeleteJournalRecord& v, std::vector<byte>* out, TypedDeleteCarrierError* e) {
  const auto& d = v.descriptor;
  const auto state = v.lifecycle_state;
  const bool has_sp = Present(v.statement_savepoint_uuid);
  if (state < State::bound || state > State::aborted || !v.journal_sequence ||
      !Present(v.database_uuid) || !Present(v.authenticated_statement_receipt_uuid) ||
      !Present(v.owning_transaction_uuid) || !v.owning_local_transaction_id ||
      !Present(v.operation_uuid) || !Present(v.recovery_token_uuid) || !v.recovery_generation ||
      v.authenticated_statement_receipt_uuid != d.authenticated_statement_receipt_uuid ||
      v.owning_transaction_uuid != d.owning_transaction_uuid ||
      v.owning_local_transaction_id != d.owning_local_transaction_id ||
      v.operation_uuid != d.operation_uuid || v.recovery_token_uuid != d.recovery_token_uuid ||
      v.recovery_generation != d.recovery_generation ||
      has_sp != (v.statement_savepoint_generation != 0) ||
      (state == State::bound && has_sp) ||
      ((state == State::intent || state == State::prepared || state == State::published) && !has_sp) ||
      v.prior_result.has_value() != (state == State::prepared || state == State::published))
    return Fail(e, "journal_owner_or_state", "DML.DELETE_FAILED");
  if (v.prior_result && (!ResultOwner(d, *v.prior_result) ||
      v.prior_result->publication_barrier_uuid != v.statement_savepoint_uuid ||
      v.prior_result->publication_barrier_generation != v.statement_savepoint_generation))
    return Fail(e, "result_owner", "DML.DELETE_FAILED");
  std::vector<byte> descriptor, result;
  if (!EncodeTypedDeleteDescriptor(d, &descriptor, e) ||
      (v.prior_result && !EncodeTypedDeleteResult(*v.prior_result, &result, e))) {
    if (e) e->diagnostic_code = "DML.DELETE_FAILED";
    return false;
  }
  std::vector<byte> bytes(256 + descriptor.size() + result.size(), 0);
  Header(bytes, "DDJR", 256); bytes[16] = static_cast<byte>(state);
  Put(bytes, 24, v.journal_sequence); Put(bytes, 32, v.database_uuid);
  Put(bytes, 48, d.descriptor_uuid); Put(bytes, 64, d.descriptor_generation);
  Put(bytes, 72, v.authenticated_statement_receipt_uuid); Put(bytes, 88, v.owning_transaction_uuid);
  Put(bytes, 104, v.owning_local_transaction_id); Put(bytes, 112, v.operation_uuid);
  Put(bytes, 128, v.recovery_token_uuid); Put(bytes, 144, v.recovery_generation);
  Put(bytes, 152, v.statement_savepoint_uuid); Put(bytes, 168, v.statement_savepoint_generation);
  Put(bytes, 176, descriptor.size(), 4); Put(bytes, 180, result.size(), 4);
  Put(bytes, 184, descriptor.size() + result.size(), 4); Put(bytes, 192, v.prior_record_sha256);
  std::copy(descriptor.begin(), descriptor.end(), bytes.begin() + 256);
  std::copy(result.begin(), result.end(), bytes.begin() + 904);
  TypedUpdateHash digest{};
  if (!Digest(journal_domain, std::span<const byte>(bytes).first(224),
              std::span<const byte>(bytes).subspan(256), &digest))
    return Fail(e, "journal_hash", "DML.DELETE_FAILED");
  Put(bytes, 224, digest); *out = std::move(bytes); return true;
}
bool JournalChain(const TypedDeleteJournalRecord& v, const TypedDeleteJournalRecord* p,
                  TypedDeleteCarrierError* e) {
  if (!p) {
    if (v.lifecycle_state != State::bound || v.journal_sequence != 1 ||
        Present(v.prior_record_sha256)) return Fail(e, "first_record", "DML.DELETE_FAILED");
    return true;
  }
  // Confirm the predecessor object's fields still equal the exact decoded
  // record. The caller must have validated the entire preceding chain.
  std::vector<byte> previous_bytes;
  if (!JournalBytes(*p, &previous_bytes, e) || previous_bytes != p->exact_bytes ||
      !Present(p->record_evidence_sha256) ||
      !std::equal(p->record_evidence_sha256.begin(), p->record_evidence_sha256.end(),
                  previous_bytes.begin() + 224))
    return Fail(e, "predecessor_not_exact", "DML.DELETE_FAILED");
  if (p->journal_sequence == std::numeric_limits<u64>::max() ||
      v.journal_sequence != p->journal_sequence + 1 ||
      v.prior_record_sha256 != p->record_evidence_sha256 || v.database_uuid != p->database_uuid)
    return Fail(e, "sequence_or_database", "DML.DELETE_FAILED");
  const bool transition =
      (p->lifecycle_state == State::bound && (v.lifecycle_state == State::intent || v.lifecycle_state == State::aborted)) ||
      (p->lifecycle_state == State::intent && (v.lifecycle_state == State::prepared || v.lifecycle_state == State::aborted)) ||
      (p->lifecycle_state == State::prepared && (v.lifecycle_state == State::published || v.lifecycle_state == State::aborted));
  if (!transition) return Fail(e, "terminal_or_illegal_transition", "DML.DELETE_FAILED");
  std::vector<byte> descriptor, predecessor_descriptor;
  if (!EncodeTypedDeleteDescriptor(v.descriptor, &descriptor, e) ||
      !EncodeTypedDeleteDescriptor(p->descriptor, &predecessor_descriptor, e) ||
      descriptor != predecessor_descriptor)
    return Fail(e, "descriptor_changed", "DML.DELETE_FAILED");
  if (p->lifecycle_state != State::bound &&
      (v.statement_savepoint_uuid != p->statement_savepoint_uuid ||
       v.statement_savepoint_generation != p->statement_savepoint_generation))
    return Fail(e, "savepoint_changed", "DML.DELETE_FAILED");
  if (p->lifecycle_state == State::bound && v.lifecycle_state == State::aborted &&
      (Present(v.statement_savepoint_uuid) || v.statement_savepoint_generation))
    return Fail(e, "bound_abort_savepoint", "DML.DELETE_FAILED");
  if (p->lifecycle_state == State::prepared && v.lifecycle_state == State::published) {
    std::vector<byte> prior_result, next_result;
    if (!p->prior_result || !v.prior_result ||
        !EncodeTypedDeleteResult(*p->prior_result, &prior_result, e) ||
        !EncodeTypedDeleteResult(*v.prior_result, &next_result, e) || prior_result != next_result)
      return Fail(e, "prepared_result_changed", "DML.DELETE_FAILED");
  }
  return true;
}
}  // namespace

bool EncodeTypedDeleteJournal(const TypedDeleteJournalRecord& v,
    const TypedDeleteJournalRecord* predecessor, std::vector<byte>* out, TypedDeleteCarrierError* e) {
  if (e) *e = {};
  if (!out) return Fail(e, "output", "DML.DELETE_FAILED");
  std::vector<byte> bytes;
  if (!JournalBytes(v, &bytes, e) || !JournalChain(v, predecessor, e)) return false;
  *out = std::move(bytes); return true;
}
bool DecodeAndValidateTypedDeleteJournal(std::span<const byte> in,
    const TypedDeleteJournalRecord* predecessor, TypedDeleteJournalRecord* out, TypedDeleteCarrierError* e) {
  if (e) *e = {};
  if (!out || (in.size() != 904 && in.size() != 1160) ||
      !HeaderValid(in, "DDJR", 256, static_cast<u32>(in.size())) ||
      Present(in.subspan(17, 7)) || Get(in, 188, 4) ||
      Get(in, 176, 4) != 648 || Get(in, 180, 4) != in.size() - 904 ||
      Get(in, 184, 4) != in.size() - 256)
    return Fail(e, "journal_header", "DML.DELETE_FAILED");
  TypedDeleteJournalRecord v;
  v.lifecycle_state = static_cast<State>(in[16]); v.journal_sequence = Get(in, 24);
  Get(in, 32, v.database_uuid); Get(in, 72, v.authenticated_statement_receipt_uuid);
  Get(in, 88, v.owning_transaction_uuid); v.owning_local_transaction_id = Get(in, 104);
  Get(in, 112, v.operation_uuid); Get(in, 128, v.recovery_token_uuid);
  v.recovery_generation = Get(in, 144); Get(in, 152, v.statement_savepoint_uuid);
  v.statement_savepoint_generation = Get(in, 168); Get(in, 192, v.prior_record_sha256);
  Get(in, 224, v.record_evidence_sha256);
  if (!DecodeAndValidateTypedDeleteDescriptor(in.subspan(256, 648), &v.descriptor, e)) {
    if (e) e->diagnostic_code = "DML.DELETE_FAILED";
    return false;
  }
  if (in.size() == 1160) {
    v.prior_result.emplace();
    if (!DecodeAndValidateTypedDeleteResult(in.subspan(904, 256), &*v.prior_result, e)) return false;
  }
  std::vector<byte> canonical;
  if (!EncodeTypedDeleteJournal(v, predecessor, &canonical, e)) return false;
  if (!std::equal(canonical.begin(), canonical.end(), in.begin()))
    return Fail(e, "journal_evidence_or_binding", "DML.DELETE_FAILED");
  v.exact_bytes = std::move(canonical); *out = std::move(v); return true;
}
bool EncodeTypedDeleteResult(const TypedDeleteResultCarrier& v,
    std::vector<byte>* out, TypedDeleteCarrierError* e) {
  if (e) *e = {};
  if (!out) return Fail(e, "output", "DML.DELETE_FAILED");
  if (!ResultValid(v, e)) return false;
  std::vector<byte> bytes(256, 0); Header(bytes, "DDRS", 256);
  Put(bytes, 16, v.delete_descriptor_uuid); Put(bytes, 32, v.delete_descriptor_generation);
  Put(bytes, 40, v.operation_uuid); Put(bytes, 56, v.owning_transaction_uuid);
  Put(bytes, 72, v.owning_local_transaction_id); Put(bytes, 80, v.relation_uuid);
  Put(bytes, 96, v.relation_generation); Put(bytes, 104, v.matched_count);
  Put(bytes, 112, v.deleted_count); bytes[120] = 1;
  Put(bytes, 128, v.effect_set_sha256); Put(bytes, 160, v.executor_evidence_sha256);
  Put(bytes, 192, v.publication_barrier_uuid); Put(bytes, 208, v.publication_barrier_generation);
  TypedUpdateHash digest{};
  if (!Digest(result_domain, std::span<const byte>(bytes).first(216), {}, &digest))
    return Fail(e, "result_hash", "DML.DELETE_FAILED");
  Put(bytes, 216, digest); *out = std::move(bytes); return true;
}
bool DecodeAndValidateTypedDeleteResult(std::span<const byte> in,
    TypedDeleteResultCarrier* out, TypedDeleteCarrierError* e) {
  if (e) *e = {};
  if (!out || !HeaderValid(in, "DDRS", 256, 256)) return Fail(e, "result_header", "DML.DELETE_FAILED");
  if (in[120] != 1 || Present(in.subspan(121, 7)) || Present(in.subspan(248, 8)))
    return Fail(e, "result_reserved", "DML.DELETE_FAILED");
  TypedDeleteResultCarrier v;
  Get(in, 16, v.delete_descriptor_uuid); v.delete_descriptor_generation = Get(in, 32);
  Get(in, 40, v.operation_uuid); Get(in, 56, v.owning_transaction_uuid);
  v.owning_local_transaction_id = Get(in, 72); Get(in, 80, v.relation_uuid);
  v.relation_generation = Get(in, 96); v.matched_count = Get(in, 104);
  v.deleted_count = Get(in, 112); Get(in, 128, v.effect_set_sha256);
  Get(in, 160, v.executor_evidence_sha256); Get(in, 192, v.publication_barrier_uuid);
  v.publication_barrier_generation = Get(in, 208); Get(in, 216, v.result_evidence_sha256);
  std::vector<byte> canonical;
  if (!EncodeTypedDeleteResult(v, &canonical, e)) return false;
  if (!std::equal(canonical.begin(), canonical.end(), in.begin())) return Fail(e, "result_evidence", "DML.DELETE_FAILED");
  v.exact_bytes = std::move(canonical); *out = std::move(v); return true;
}
bool ValidateTypedDeleteDatatypeOperatorAuthority(
    const TypedDeleteDescriptorCarrier& descriptor, const TypedUpdatePredicateVector& predicate,
    const TypedUpdateDatatypeAuthorityVector& datatypes,
    const TypedUpdateBuiltinOperatorAuthorityVector& operators, TypedDeleteCarrierError* error) {
  if (error) *error = {};
  const auto refuse = [&](std::string_view field) { return Fail(error, field, "DML.DELETE_FAILED"); };
  TypedUpdateCarrierError shared_error;
  std::vector<byte> dddc, duev, dudv, duov;
  if (!EncodeTypedDeleteDescriptor(descriptor, &dddc, error) ||
      !EncodeTypedUpdatePredicateVector(predicate, &duev, &shared_error) ||
      !EncodeTypedUpdateDatatypeAuthorityVector(datatypes, &dudv, &shared_error) ||
      !EncodeTypedUpdateBuiltinOperatorAuthorityVector(operators, &duov, &shared_error))
    return refuse("datatype_operator_carrier_invalid");
  if (dddc != descriptor.exact_bytes || duev != predicate.exact_bytes ||
      dudv != datatypes.exact_bytes || duov != operators.exact_bytes)
    return refuse("datatype_operator_projection_changed");
  TypedDeleteDescriptorCarrier decoded_descriptor;
  TypedUpdatePredicateVector decoded_predicate;
  TypedUpdateDatatypeAuthorityVector decoded_datatypes;
  TypedUpdateBuiltinOperatorAuthorityVector decoded_operators;
  if (!DecodeAndValidateTypedDeleteDescriptor(dddc, &decoded_descriptor, error) ||
      !DecodeAndValidateTypedUpdatePredicateVector(duev, &decoded_predicate, &shared_error) ||
      !DecodeAndValidateTypedUpdateDatatypeAuthorityVector(dudv, &decoded_datatypes, &shared_error) ||
      !DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(duov, &decoded_operators, &shared_error))
    return refuse("datatype_operator_retained_bytes_invalid");
  if (descriptor.descriptor_evidence_sha256 != decoded_descriptor.descriptor_evidence_sha256 ||
      predicate.identity.vector_sha256 != decoded_predicate.identity.vector_sha256 ||
      datatypes.identity.vector_sha256 != decoded_datatypes.identity.vector_sha256 ||
      operators.identity.vector_sha256 != decoded_operators.identity.vector_sha256)
    return refuse("datatype_operator_evidence_projection_changed");
  for (std::size_t n = 0; n < predicate.records.size(); ++n)
    if (predicate.records[n].canonical_value_sha256 != decoded_predicate.records[n].canonical_value_sha256 ||
        predicate.records[n].node_evidence_sha256 != decoded_predicate.records[n].node_evidence_sha256)
      return refuse("predicate_evidence_projection_changed");
  const auto owned = [&](const TypedUpdateVectorIdentity& identity) {
    return identity.owner_descriptor_uuid == descriptor.descriptor_uuid &&
           identity.owner_descriptor_generation == descriptor.descriptor_generation;
  };
  TypedUpdateHash predicate_hash{};
  Get(duev, 72, predicate_hash);
  if (!owned(predicate.identity) || predicate.identity.vector_uuid != descriptor.predicate_expression_uuid ||
      predicate.identity.vector_generation != descriptor.predicate_expression_generation ||
      predicate.records.size() != descriptor.predicate_node_count || predicate.records.empty() ||
      predicate.records.back().node_id != descriptor.predicate_root_node_id ||
      predicate_hash != descriptor.predicate_vector_sha256)
    return refuse("DDDC_predicate_binding");
  if (datatypes.format_version != 1 || !owned(datatypes.identity) ||
      datatypes.identity.vector_uuid != kTypedUpdateDatatypeSnapshotUuid ||
      datatypes.identity.vector_generation != descriptor.datatype_registry_generation ||
      !owned(operators.identity) || operators.identity.vector_uuid != descriptor.builtin_operator_snapshot_uuid ||
      operators.identity.vector_generation != descriptor.builtin_operator_registry_generation)
    return refuse("DDDC_registry_binding");
  // Individual retained rows are authoritative projections too; callers may
  // not hide altered decoded fields behind unchanged enclosing carrier bytes.
  for (std::size_t n = 0; n < datatypes.records.size(); ++n) {
    const auto first = dudv.begin() + kTypedUpdateVectorHeaderBytes + n * kTypedUpdateDatatypeAuthorityRecordBytes;
    if (datatypes.records[n].record_evidence_sha256 != decoded_datatypes.records[n].record_evidence_sha256 ||
        datatypes.records[n].exact_bytes.size() != kTypedUpdateDatatypeAuthorityRecordBytes ||
        !std::equal(datatypes.records[n].exact_bytes.begin(), datatypes.records[n].exact_bytes.end(), first))
      return refuse("datatype_record_projection_changed");
  }
  for (std::size_t n = 0; n < operators.records.size(); ++n) {
    const auto first = duov.begin() + kTypedUpdateVectorHeaderBytes + n * kTypedUpdateBuiltinOperatorAuthorityRecordBytes;
    if (operators.records[n].record_evidence_sha256 != decoded_operators.records[n].record_evidence_sha256 ||
        operators.records[n].exact_bytes.size() != kTypedUpdateBuiltinOperatorAuthorityRecordBytes ||
        !std::equal(operators.records[n].exact_bytes.begin(), operators.records[n].exact_bytes.end(), first))
      return refuse("operator_record_projection_changed");
  }
  std::vector<bool> used(datatypes.records.size(), false);
  for (const auto& node : predicate.records) {
    const auto found = std::find_if(datatypes.records.begin(), datatypes.records.end(), [&](const auto& row) {
      return std::tie(row.descriptor_uuid, row.descriptor_generation, row.type_uuid, row.type_generation,
                      row.codec_id, row.codec_version, row.codec_generation) ==
             std::tie(node.output_descriptor_uuid, node.output_descriptor_generation,
                      node.output_type_uuid, node.output_type_generation, node.output_codec_id,
                      node.output_codec_version, node.output_codec_generation);
    });
    if (found == datatypes.records.end()) return refuse("predicate_datatype_missing");
    used[static_cast<std::size_t>(found - datatypes.records.begin())] = true;
    if ((node.value_state == TypedUpdateValueState::value &&
         node.canonical_value.size() != found->canonical_value_exact_bytes) ||
        (node.value_state != TypedUpdateValueState::value && !node.canonical_value.empty()))
      return refuse("predicate_value_extent");
    if (node.node_kind == TypedUpdatePredicateNodeKind::column_reference &&
        (node.referenced_relation_occurrence_uuid != descriptor.target_relation_occurrence_uuid ||
         node.referenced_relation_occurrence_generation != descriptor.target_relation_occurrence_generation))
      return refuse("predicate_relation_occurrence");
  }
  if (std::find(used.begin(), used.end(), false) != used.end()) return refuse("unreferenced_datatype");
  if (predicate.records.size() == 1) {
    const auto& node = predicate.records.front();
    if (node.node_kind != TypedUpdatePredicateNodeKind::canonical_boolean_constant ||
        node.value_state != TypedUpdateValueState::value || node.canonical_value != std::vector<byte>{1} ||
        !operators.records.empty()) return refuse("canonical_TRUE_profile");
    return true;
  }
  if (predicate.records.size() != 3 || operators.records.size() != 1) return refuse("equality_profile");
  const auto& left = predicate.records[0];
  const auto& right = predicate.records[1];
  const auto& comparison = predicate.records[2];
  const auto& op = operators.records[0];
  if (left.node_kind != TypedUpdatePredicateNodeKind::column_reference ||
      right.node_kind != TypedUpdatePredicateNodeKind::typed_literal ||
      comparison.node_kind != TypedUpdatePredicateNodeKind::comparison ||
      left.node_id != comparison.left_child_node_id || right.node_id != comparison.right_child_node_id ||
      comparison.operator_uuid != op.operator_uuid || comparison.operator_generation != op.operator_generation ||
      std::tie(op.left_descriptor_uuid, op.left_descriptor_generation, op.left_type_uuid, op.left_type_generation) !=
          std::tie(left.output_descriptor_uuid, left.output_descriptor_generation, left.output_type_uuid, left.output_type_generation) ||
      std::tie(op.right_descriptor_uuid, op.right_descriptor_generation, op.right_type_uuid, op.right_type_generation) !=
          std::tie(right.output_descriptor_uuid, right.output_descriptor_generation, right.output_type_uuid, right.output_type_generation) ||
      std::tie(op.result_descriptor_uuid, op.result_descriptor_generation, op.result_type_uuid, op.result_type_generation,
               op.result_codec_id, op.result_codec_version, op.result_codec_generation) !=
          std::tie(comparison.output_descriptor_uuid, comparison.output_descriptor_generation,
                   comparison.output_type_uuid, comparison.output_type_generation, comparison.output_codec_id,
                   comparison.output_codec_version, comparison.output_codec_generation))
    return refuse("equality_operator_binding");
  return true;
}
}  // namespace scratchbird::wire
