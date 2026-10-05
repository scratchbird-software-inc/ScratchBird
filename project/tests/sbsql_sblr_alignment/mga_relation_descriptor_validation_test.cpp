// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "mga_relation_store/mga_contextual_text_sidecar_set_v2.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
std::size_t evolution_cases = 0;
long fail_after = -1;
std::size_t allocations = 0;
std::size_t injected_faults = 0;
}
void* operator new(std::size_t size) {
  ++allocations;
  if (fail_after >= 0 && fail_after-- == 0) throw std::bad_alloc{};
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace api = scratchbird::engine::internal_api;
namespace {
void Check(bool value, const char* detail) {
  if (!value) throw std::runtime_error(detail);
}
api::EngineUuid Id(unsigned value) {
  api::EngineUuid id;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<unsigned char>(value >> 8);
  id.bytes[15] = static_cast<unsigned char>(value);
  return id;
}
api::MgaRelationStorageDescriptor Fixture() {
  api::MgaRelationStorageDescriptor d;
  d.descriptor_uuid = Id(1); d.database_uuid = Id(2); d.schema_uuid = Id(3);
  d.relation_uuid = Id(4); d.primary_filespace_uuid = Id(5);
  d.relation_generation = 7;
  d.required_evidence_kinds = {"relation_descriptor", "row_version", "transaction_inventory", "dirty_manifest"};
  for (unsigned i = 0; i < 3; ++i) {
    api::MgaRelationColumnStorageDescriptor c;
    c.column_uuid = Id(10 + i); c.column_generation = 11;
    c.ordinal = i == 2 ? UINT32_MAX : i * 7;
    c.canonical_name_key = "c" + std::to_string(i);
    c.value_descriptor.descriptor_uuid = Id(20);
    c.value_descriptor.datatype_descriptor_uuid = Id(21);
    c.value_descriptor.datatype_descriptor_generation = 9;
    c.value_descriptor.type_uuid = Id(22);
    c.value_descriptor.encoded_descriptor = "base_type=INTEGER";
    d.columns.push_back(c);
  }
  return d;
}
void ExpectRejected(const api::MgaRelationStorageDescriptor& input, const char* reason) {
  const auto before = input;
  const auto result = api::ValidateMgaRelationStorageDescriptor(input);
  Check(result.error, reason);
  Check(result.code == "SB_ENGINE_API_INVALID_REQUEST", "invalid cohort lost typed API diagnostic");
  Check(input == before, "typed validation changed rejected descriptor");
}
void OrdinalEvolution() {
  using E = api::MgaColumnOrdinalStateError;
  constexpr std::uint64_t limit = 4294967296ULL;
  auto previous = Fixture();
  previous.columns[0].ordinal = 0;
  previous.columns[1].ordinal = 2;
  previous.columns[2].ordinal = 7;
  const auto retained = previous;
  // Independent finite model: remove any subset, optionally add one new
  // identity at every historical/current boundary, vary persisted high-water
  // and presentation order. The old highest ordinal is deliberately sparse.
  for (unsigned mask = 0; mask != 8; ++mask) {
    for (int added = -1; added != 11; ++added) {
      for (std::uint64_t high_water : std::array<std::uint64_t, 6>{7, 8, 9, 10, 11, limit}) {
        auto next = previous;
        next.columns.clear();
        for (unsigned i = 0; i != 3; ++i)
          if (mask & (1U << i)) next.columns.push_back(previous.columns[i]);
        if (added >= 0) {
          auto column = previous.columns.front();
          column.column_uuid = Id(101);
          column.ordinal = static_cast<std::uint32_t>(added);
          next.columns.push_back(column);
        }
        E expected = E::none;
        bool duplicate = false, outside = false;
        for (std::size_t i = 0; i < next.columns.size(); ++i) {
          outside |= next.columns[i].ordinal >= high_water;
          for (std::size_t j = i + 1; j < next.columns.size(); ++j)
            duplicate |= next.columns[i].ordinal == next.columns[j].ordinal;
        }
        if (next.columns.empty() || duplicate) expected = E::invalid_column_cohort;
        else if (outside) expected = E::column_not_below_high_water;
        else if (high_water < 8) expected = E::high_water_decreased;
        else if (added >= 0 && added < 8) expected = E::retired_ordinal_reused;
        const auto saved = next;
        for (unsigned order = 0; order != 2; ++order) {
          Check(api::ValidateMgaRelationColumnOrdinalEvolution(previous, 8, next, high_water) == expected,
                "evolution differs from independent non-reuse oracle");
          ++evolution_cases;
          std::reverse(next.columns.begin(), next.columns.end());
        }
        Check(next == saved && previous == retained, "evolution changed retained state");
      }
    }
  }
  auto next = previous;
  next.columns[1].ordinal = 3;
  Check(api::ValidateMgaRelationColumnOrdinalEvolution(previous, 8, next, 8) == E::surviving_ordinal_changed,
        "surviving identity accepted ordinal reassignment");
  next = previous;
  next.relation_uuid = Id(80);
  Check(api::ValidateMgaRelationColumnOrdinalEvolution(previous, 8, next, 8) == E::table_identity_mismatch,
        "different table borrowed ordinal history");
  next = previous;
  next.relation_uuid = {};
  Check(api::ValidateMgaRelationColumnOrdinalState(next, 0) == E::invalid_table_identity,
        "table identity precedence lost");
  for (auto bad : std::array<std::uint64_t, 3>{0, limit + 1, UINT64_MAX})
    Check(api::ValidateMgaRelationColumnOrdinalState(previous, bad) == E::invalid_high_water,
          "invalid high-water repaired from columns");
  next = previous;
  next.columns.back().ordinal = UINT32_MAX;
  Check(api::ValidateMgaRelationColumnOrdinalState(next, limit) == E::none &&
        api::ValidateMgaRelationColumnOrdinalState(next, UINT32_MAX) == E::column_not_below_high_water,
        "maximum stored ordinal/exhaustion confused");
  auto exhausted = next;
  exhausted.columns.pop_back();
  Check(api::ValidateMgaRelationColumnOrdinalEvolution(next, limit, exhausted, limit) == E::none,
        "drop at exhausted high-water refused");
  exhausted.columns.back().column_uuid = Id(102);
  Check(api::ValidateMgaRelationColumnOrdinalEvolution(next, limit, exhausted, limit) == E::retired_ordinal_reused,
        "exhausted table reused history after drop");
  // All fallible map allocations in success and every allocating refusal path.
  for (unsigned path = 0; path != 6; ++path) {
    next = previous;
    std::uint64_t high_water = 8;
    if (path == 1) next.columns.back().column_uuid = next.columns.front().column_uuid;
    if (path == 2) next.columns.back().ordinal = 8;
    if (path == 3) next.relation_uuid = Id(80);
    if (path == 4) next.columns.back().ordinal = 6;
    if (path == 5) { next.columns.back().column_uuid = Id(102); high_water = 9; }
    const auto saved = next;
    allocations = 0;
    (void)api::ValidateMgaRelationColumnOrdinalEvolution(previous, 8, next, high_water);
    const auto sites = allocations;
    Check(sites != 0, "evolution fault fixture did not allocate");
    for (std::size_t site = 0; site != sites; ++site) {
      bool threw = false;
      fail_after = static_cast<long>(site);
      try { (void)api::ValidateMgaRelationColumnOrdinalEvolution(previous, 8, next, high_water); }
      catch (const std::bad_alloc&) { threw = true; }
      fail_after = -1;
      Check(threw && previous == retained && next == saved, "evolution fault mutated state or returned success");
      ++injected_faults;
    }
  }
}
void CohortValidation() {
  const auto original = Fixture();
  Check(!api::ValidateMgaRelationStorageDescriptor(original).error,
        "valid sparse cohort with shared datatype binding refused");
  auto reordered = original;
  std::reverse(reordered.columns.begin(), reordered.columns.end());
  Check(!api::ValidateMgaRelationStorageDescriptor(reordered).error,
        "presentation order renumbered or invalidated stored ordinals");
  for (unsigned removed = 0; removed < original.columns.size(); ++removed) {
    auto next = original;
    next.columns.erase(next.columns.begin() + removed);
    Check(!api::ValidateMgaRelationStorageDescriptor(next).error &&
              api::MgaRelationStoragePreservesSurvivingColumnOrdinals(original, next),
          "removing a column invalidated surviving ordinals");
  }
  for (unsigned left = 0; left < original.columns.size(); ++left) {
    for (unsigned right = 0; right < original.columns.size(); ++right) {
      if (left == right) continue;
      auto bad = original;
      bad.columns[left].column_uuid = bad.columns[right].column_uuid;
      ExpectRejected(bad, "direct typed validator admitted duplicate column identity");
      bad = original;
      bad.columns[left].ordinal = bad.columns[right].ordinal;
      ExpectRejected(bad, "direct typed validator admitted duplicate stored ordinal");
    }
  }
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    auto bad = original;
    if (mutation == 0) bad.columns.clear();
    if (mutation == 1) bad.columns.front().column_uuid = {};
    if (mutation == 2) bad.columns.front().column_uuid.bytes[6] = 0x40;
    if (mutation == 3) bad.columns.front().column_generation = 0;
    if (mutation == 4) bad.columns.front().value_descriptor.datatype_descriptor_generation = 0;
    if (mutation == 5) bad.columns.front().value_descriptor.type_uuid = {};
    if (mutation == 6) bad.relation_generation = 0;
    ExpectRejected(bad, "direct typed validator lost prior binding checks");
  }
  Check(original == Fixture(), "validation changed retained predecessor");

  // Warm shared diagnostic machinery before counting this operation's actual
  // allocations. Exercise valid and both duplicate-rejection paths.
  for (unsigned path = 0; path < 3; ++path) {
    auto candidate = original;
    if (path == 1) candidate.columns.back().column_uuid = candidate.columns.front().column_uuid;
    if (path == 2) candidate.columns.back().ordinal = candidate.columns.front().ordinal;
    (void)api::ValidateMgaRelationStorageDescriptor(candidate);
    const auto before = candidate;
    allocations = 0;
    (void)api::ValidateMgaRelationStorageDescriptor(candidate);
    const auto sites = allocations;
    Check(sites >= 6, "typed validation allocation coverage unexpectedly empty");
    for (std::size_t site = 0; site < sites; ++site) {
      bool threw = false;
      fail_after = static_cast<long>(site);
      try { (void)api::ValidateMgaRelationStorageDescriptor(candidate); }
      catch (const std::bad_alloc&) { threw = true; }
      fail_after = -1;
      Check(threw && candidate == before, "typed validation allocation failure changed descriptor or returned success");
      ++injected_faults;
    }
  }
}

void SealedProjectionIntegration() {
  using Field = api::MgaContextualTextDescriptorFieldPairV2;
  using Bytes = api::MgaContextualTextRawBytesV2;
  auto relation = Fixture();
  // Real TEXT codec identities from its existing fixture profile, not newly
  // issued catalog identities or a fabricated current catalog source.
  const auto datatype_id = [](unsigned char tail) -> api::MgaContextualTextUuidV2 {
    return {0x01, 0x9d, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0xd7, tail};
  };
  api::MgaContextualTextProjectedColumnV2 text;
  text.column_ordinal = UINT32_MAX;
  text.column_uuid = relation.columns.back().column_uuid.bytes;
  text.comparable_persisted_text = true;
  text.projected_datatype_descriptor_uuid = datatype_id(0x18);
  text.projected_datatype_descriptor_generation = 1;
  text.projected_datatype_catalog_snapshot_uuid = datatype_id(0x01);
  text.projected_datatype_catalog_generation = 1;
  text.projected_datatype_registry_generation = 1;
  text.projected_resource_epoch = 77;
  auto& d = text.expected_text_descriptor;
  d.flags = d.malformed_sequence_policy = d.null_encoding = 1;
  d.descriptor_uuid = datatype_id(0x18); d.descriptor_generation = 1;
  d.type_uuid = datatype_id(0x19); d.type_generation = 1;
  d.codec_uuid = datatype_id(0x1a); d.codec_version = d.codec_generation = 1;
  d.character_limit = 257; d.byte_limit = 1028;
  d.charset_uuid = Id(30).bytes; d.charset_generation = 11;
  d.collation_uuid = Id(31).bytes; d.collation_generation = 12;
  d.normalization_policy_uuid = Id(32).bytes; d.normalization_policy_generation = 13;
  d.render_policy_uuid = Id(33).bytes; d.render_policy_generation = 14;
  d.canonicalization_profile_uuid = Id(34).bytes; d.canonicalization_profile_generation = 15;
  d.comparison_contract_uuid = Id(35).bytes; d.comparison_contract_generation = 16;
  d.equality_operation_uuid = Id(36).bytes; d.equality_operation_generation = 17;
  d.datatype_catalog_snapshot_uuid = text.projected_datatype_catalog_snapshot_uuid;
  d.datatype_catalog_generation = d.datatype_registry_generation = 1;
  d.resource_epoch = 77;
  auto& stored = relation.columns.back();
  // TEXT has its own value-descriptor occurrence; the two INTEGER columns
  // may still share their immutable occurrence without aliasing this one.
  stored.value_descriptor.descriptor_uuid = Id(23);
  stored.value_descriptor.descriptor_kind = "scalar";
  stored.value_descriptor.canonical_type_name = "TEXT";
  stored.value_descriptor.encoded_descriptor = "base_type=TEXT;character_length=257";
  stored.value_descriptor.datatype_descriptor_uuid.bytes = d.descriptor_uuid;
  stored.value_descriptor.datatype_descriptor_generation = d.descriptor_generation;
  stored.value_descriptor.type_uuid.bytes = d.type_uuid;
  stored.value_descriptor.charset_uuid.bytes = stored.charset_uuid.bytes = d.charset_uuid;
  stored.value_descriptor.collation_uuid.bytes = stored.collation_uuid.bytes = d.collation_uuid;
  stored.character_length = 257;
  std::vector<api::MgaContextualTextProjectedColumnV2> columns;
  for (const auto& c : relation.columns) {
    api::MgaContextualTextProjectedColumnV2 p;
    p.column_ordinal = c.ordinal; p.column_uuid = c.column_uuid.bytes;
    columns.push_back(p);
  }
  columns.back() = text;
  std::vector<Field> base;
  for (const auto& [key, value] : api::SerializeMgaRelationStorageDescriptor(relation)) {
    base.push_back({Bytes(key.begin(), key.end()), Bytes(value.begin(), value.end())});
  }
  const api::MgaContextualTextSidecarSetOwnerV2 owner{
      701, 19, relation.relation_uuid.bytes, relation.descriptor_uuid.bytes,
      relation.descriptor_generation};
  api::MgaContextualTextSidecarSetV2 sealed;
  api::MgaContextualTextSidecarSetDiagnosticV2 diagnostic;
  Check(api::BuildMgaContextualTextSidecarSetV2(owner, base, columns, &sealed, &diagnostic),
        "complete sparse TEXT projection could not be sealed");
  const auto decode_base = [](const auto& candidate) {
    std::vector<std::pair<std::string, std::string>> complete;
    for (const auto& pair : candidate.descriptor_fields) {
      complete.emplace_back(std::string(pair.key_raw_bytes.begin(), pair.key_raw_bytes.end()),
                            std::string(pair.value_raw_bytes.begin(), pair.value_raw_bytes.end()));
    }
    return api::DeserializeMgaRelationStorageDescriptor(complete);
  };
  Check(decode_base(sealed) == relation &&
        api::ValidateMgaContextualTextSidecarSetV2(owner, base, columns, sealed, &diagnostic),
        "base decode broke valid complete sparse TEXT seal");
  api::MgaContextualTextSidecarLookupResultV2 found;
  Check(api::LookupMgaContextualTextSidecarV2(owner, base, columns, sealed, UINT32_MAX,
                                            text.column_uuid, &found, &diagnostic) &&
            found.exact_blob.size() == 533,
        "stored ordinal did not resolve its sealed TEXT descriptor");
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    auto bad = sealed;
    if (mutation < 3) bad.descriptor_fields[base.size() + mutation].value_raw_bytes[0] ^= 1;
    if (mutation == 3) ++bad.owner.relation_descriptor_generation;
    Check(decode_base(bad) == relation,
          "base extractor consumed sidecar authority instead of base fields");
    Check(!api::ValidateMgaContextualTextSidecarSetV2(owner, base, columns, bad, &diagnostic) &&
              diagnostic.code == "CTB.TEXT.DESCRIPTOR_INVALID",
          "base decoding bypassed damaged sidecar blob/hash/seal/owner refusal");
    const auto prior_blob = found.exact_blob;
    Check(!api::LookupMgaContextualTextSidecarV2(owner, base, columns, bad, UINT32_MAX,
                                               text.column_uuid, &found, &diagnostic) &&
              found.exact_blob == prior_blob,
          "refused sidecar lookup published partial replacement");
  }
}
}  // namespace
int main() {
  try {
    CohortValidation();
    OrdinalEvolution();
    SealedProjectionIntegration();
    std::cout << "PASS direct typed relation descriptor validation and sealed projection; allocation faults="
              << injected_faults << "; evolution cases=" << evolution_cases << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
