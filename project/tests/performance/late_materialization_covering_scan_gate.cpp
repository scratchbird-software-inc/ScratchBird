// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "covering_index_payload.hpp"
#include "index_key_encoding.hpp"
#include "late_materialization_covering_scan_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "late_materialization_covering_scan_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasContaining(const std::vector<std::string>& values,
                   std::string_view expected) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(expected) != std::string::npos;
  });
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

platform::TypedUuid StableTypedUuid(platform::UuidKind kind,
                                    platform::byte seed) {
  platform::TypedUuid value;
  value.kind = kind;
  for (std::size_t i = 0; i < value.value.bytes.size(); ++i) {
    value.value.bytes[i] =
        static_cast<platform::byte>(seed + static_cast<platform::byte>(i));
  }
  value.value.bytes[6] =
      static_cast<platform::byte>((value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<platform::byte>((value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
}

platform::TypedUuid ParseTyped(platform::UuidKind kind,
                               const std::string& text) {
  const auto parsed = uuid::ParseDurableEngineIdentityUuid(kind, text);
  Require(parsed.ok(), "typed uuid parse failed");
  return parsed.value;
}

std::vector<platform::byte> EncodedKey(const std::string& index_uuid,
                                       const std::string& key) {
  const auto descriptor_uuid =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(descriptor_uuid.ok(), "index uuid parse for key encoding failed");
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = descriptor_uuid.value;
  component.payload.assign(key.begin(), key.end());
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "test key encoding failed");
  return encoded.encoded;
}

page::IndexBtreePhysicalTree MakeTree(const std::string& index_uuid) {
  auto initialized = page::InitializeIndexBtreePhysicalTree(
      ParseTyped(platform::UuidKind::object, index_uuid), 768);
  Require(initialized.ok(), "physical btree init failed");
  return std::move(initialized.tree);
}

page::IndexBtreeCell Cell(const std::string& index_uuid,
                          const std::string& key,
                          const std::string& row_uuid,
                          const std::string& version_uuid) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(index_uuid, key);
  cell.row_uuid = ParseTyped(platform::UuidKind::row, row_uuid);
  cell.version_uuid = ParseTyped(platform::UuidKind::row, version_uuid);
  return cell;
}

void InsertCell(page::IndexBtreePhysicalTree* tree,
                const page::IndexBtreeCell& cell) {
  page::IndexBtreePhysicalInsertRequest request;
  request.cell = cell;
  const auto inserted = page::InsertIndexBtreeCell(tree, request);
  Require(inserted.ok(), "physical insert failed");
}

exec::IndexedPhysicalOperatorRequest BasePhysicalRequest(
    const page::IndexBtreePhysicalTree* tree) {
  exec::IndexedPhysicalOperatorRequest request;
  request.kind = exec::IndexedPhysicalOperatorKind::range_scan;
  request.physical_tree = tree;
  request.lower_bound.unbounded = true;
  request.upper_bound.unbounded = true;
  request.plan_safe = true;
  request.physical_tree_available = true;
  request.encoded_key_proof = true;
  request.encoded_bounds_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  return request;
}

idx::CoveringIndexPayloadColumnRef Column(platform::byte seed,
                                          platform::u32 ordinal) {
  idx::CoveringIndexPayloadColumnRef column;
  column.column_uuid = StableTypedUuid(platform::UuidKind::object, seed);
  column.type_descriptor_uuid = StableTypedUuid(
      platform::UuidKind::object, static_cast<platform::byte>(seed + 40));
  column.projection_ordinal = ordinal;
  column.required = true;
  return column;
}

std::vector<platform::byte> Bytes(std::string_view text) {
  return std::vector<platform::byte>(text.begin(), text.end());
}

idx::CoveringIndexPayloadColumnValue InlineValue(
    const idx::CoveringIndexPayloadColumnRef& column,
    std::string_view value) {
  idx::CoveringIndexPayloadColumnValue out;
  out.column_uuid = column.column_uuid;
  out.projection_ordinal = column.projection_ordinal;
  out.kind = idx::CoveringIndexPayloadValueKind::inline_value;
  out.encoded_value = Bytes(value);
  out.binary_result_frame_compatible = true;
  out.redaction_safe = true;
  out.unredacted_authorized = true;
  return out;
}

struct Fixture {
  std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700610000000ull, 0x41);
  std::string table_uuid =
      UuidText(platform::UuidKind::object, 1700610001000ull, 0x42);
  std::string row_c =
      UuidText(platform::UuidKind::row, 1700610100000ull, 0x61);
  std::string row_a =
      UuidText(platform::UuidKind::row, 1700610101000ull, 0x62);
  std::string row_b =
      UuidText(platform::UuidKind::row, 1700610102000ull, 0x63);
  std::string version_c =
      UuidText(platform::UuidKind::row, 1700610200000ull, 0x71);
  std::string version_a =
      UuidText(platform::UuidKind::row, 1700610201000ull, 0x72);
  std::string version_b =
      UuidText(platform::UuidKind::row, 1700610202000ull, 0x73);
  page::IndexBtreePhysicalTree tree = MakeTree(index_uuid);

  Fixture() {
    InsertCell(&tree, Cell(index_uuid, "charlie", row_c, version_c));
    InsertCell(&tree, Cell(index_uuid, "alpha", row_a, version_a));
    InsertCell(&tree, Cell(index_uuid, "bravo", row_b, version_b));
  }
};

exec::IndexedPhysicalOperatorResult PhysicalStream(const Fixture& fixture) {
  const auto result =
      exec::ExecuteIndexedPhysicalOperator(BasePhysicalRequest(&fixture.tree));
  Require(result.ok, "IRC-060 physical stream refused");
  Require(result.locators.size() == 3, "IRC-060 stream locator count mismatch");
  Require(result.locators[0].row_uuid == fixture.row_a &&
              result.locators[1].row_uuid == fixture.row_b &&
              result.locators[2].row_uuid == fixture.row_c,
          "IRC-060 stream did not preserve physical key order");
  return result;
}

idx::CoveringIndexPayloadAssemblyResult AssemblePayload(
    const Fixture& fixture,
    const exec::IndexedPhysicalOperatorLocator& locator,
    std::string_view first,
    std::string_view second) {
  const auto c1 = Column(1, 0);
  const auto c2 = Column(2, 1);
  idx::CoveringIndexPayloadAssemblyRequest request;
  request.index_uuid = ParseTyped(platform::UuidKind::object,
                                  fixture.index_uuid);
  request.table_uuid = ParseTyped(platform::UuidKind::object,
                                  fixture.table_uuid);
  request.row_uuid = ParseTyped(platform::UuidKind::row, locator.row_uuid);
  request.version_uuid = ParseTyped(platform::UuidKind::row,
                                    locator.version_uuid);
  request.descriptor_result_contract_hash = "contract:irc061:v1";
  request.payload_generation = 10;
  request.redaction_policy_epoch = 20;
  request.security_policy_epoch = 30;
  request.freshness_generation = 40;
  request.descriptor_columns = {c1, c2};
  request.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  request.values = {InlineValue(c1, first), InlineValue(c2, second)};
  request.projection_only = true;
  request.result_contract_bound = true;
  const auto assembled = idx::AssembleCoveringIndexPayload(request);
  Require(assembled.ok(), "covering payload assembly failed");
  return assembled;
}

idx::CoveringIndexPayloadAdmission AdmitPayload(
    const idx::CoveringIndexPayloadRecord& record) {
  const auto c1 = Column(1, 0);
  const auto c2 = Column(2, 1);
  idx::CoveringIndexPayloadValidationRequest request;
  request.record = record;
  request.locator.encoded_key = Bytes("encoded-key");
  request.locator.row_uuid = record.row_uuid;
  request.locator.version_uuid = record.version_uuid;
  request.locator.leaf_page_number = 7;
  request.locator.cell_ordinal = 1;
  request.locator.physical_btree_locator_scan = true;
  request.required_columns = {c1, c2};
  request.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  request.expected_descriptor_result_contract_hash = "contract:irc061:v1";
  request.expected_payload_generation = 10;
  request.expected_redaction_policy_epoch = 20;
  request.expected_security_policy_epoch = 30;
  request.expected_freshness_generation = 40;
  request.descriptor_epoch_current = true;
  request.result_contract_current = true;
  request.redaction_epoch_current = true;
  request.security_epoch_current = true;
  request.freshness_current = true;
  request.result_frame_contract_proven = true;
  request.redaction_policy_safe = true;
  request.exact_predicate_recheck_planned = true;
  request.mga_visibility_recheck_planned = true;
  request.security_authorization_recheck_planned = true;
  request.exact_predicate_rechecked_by_engine = true;
  request.mga_visibility_rechecked_by_engine = true;
  request.security_authorized_by_engine = true;
  request.base_row_recheck_available = true;
  request.allow_index_only = true;
  const auto admission = idx::ValidateCoveringIndexPayloadForLocator(request);
  if (!admission.ok()) {
    std::cerr << "covering payload admission diagnostic="
              << admission.diagnostic.diagnostic_code << " detail=";
    if (!admission.diagnostic.arguments.empty()) {
      std::cerr << admission.diagnostic.arguments.front().value;
    }
    std::cerr << '\n';
  }
  Require(admission.ok(), "covering payload admission failed");
  return admission;
}

void RequireCommonAcceptedEvidence(const std::vector<std::string>& evidence) {
  Require(Has(evidence, "irc061.physical_row_id_stream_consumed=true"),
          "physical row-id stream consumption evidence missing");
  Require(Has(evidence, "irc061.row_order_preserved=true"),
          "row order evidence missing");
  Require(Has(evidence, "irc061.row_version_uuid_binding_preserved=true"),
          "row/version binding evidence missing");
  Require(Has(evidence, "irc061.mga_visibility_recheck.engine_owned=true"),
          "MGA engine-owned recheck evidence missing");
  Require(Has(evidence, "irc061.security_authorization_recheck.engine_owned=true"),
          "security engine-owned recheck evidence missing");
  Require(Has(evidence, "irc061.redaction_recheck.engine_owned=true"),
          "redaction engine-owned recheck evidence missing");
  Require(Has(evidence, "irc061.finality_authority=engine_transaction_inventory"),
          "MGA finality authority evidence missing");
  Require(Has(evidence, "irc061.index_payload_finality_authority=false"),
          "index payload non-finality evidence missing");
  Require(Has(evidence, "irc061.benchmark_clean=false"),
          "benchmark-clean false evidence missing");
}

void LateMaterializationConsumesPhysicalStream() {
  const Fixture fixture;
  const auto stream = PhysicalStream(fixture);
  std::vector<std::string> provider_order;
  const auto result = exec::ConsumeIndexedRowIdStreamForLateMaterialization(
      stream, {},
      [&](const exec::IndexedPhysicalOperatorLocator& locator) {
        provider_order.push_back(locator.row_uuid + ":" + locator.version_uuid);
        exec::LateMaterializationIndexedProviderResult out;
        out.ok = true;
        out.row.row_uuid = locator.row_uuid;
        out.row.version_uuid = locator.version_uuid;
        out.row.projected_values = {"base:" + locator.row_uuid};
        out.row.evidence = {"test.base_row_recheck=true"};
        out.evidence = {"test.provider.physical_locator_consumed=true"};
        return out;
      });
  Require(result.ok, "late materialization stream consumption failed");
  Require(result.runtime_route_capability,
          "late materialization runtime route capability missing");
  Require(!result.benchmark_clean, "late materialization claimed benchmark clean");
  Require(!result.full_table_scan_or_materialization,
          "late materialization consumed full table scan");
  Require(result.rows.size() == stream.locators.size(),
          "late materialization row count mismatch");
  for (std::size_t i = 0; i < stream.locators.size(); ++i) {
    Require(result.rows[i].row_uuid == stream.locators[i].row_uuid,
            "late materialization did not preserve row order");
    Require(result.rows[i].version_uuid == stream.locators[i].version_uuid,
            "late materialization did not preserve version binding");
    Require(provider_order[i] == stream.locators[i].row_uuid + ":" +
                               stream.locators[i].version_uuid,
            "provider was not driven by physical row-id stream order");
  }
  RequireCommonAcceptedEvidence(result.evidence);
  Require(Has(result.evidence,
              "irc061.late_materialization.base_row_recheck_handoff=true"),
          "base-row recheck handoff evidence missing");
}

void CoveringProjectionOnlyConsumesPayloads() {
  const Fixture fixture;
  const auto stream = PhysicalStream(fixture);
  std::vector<idx::CoveringIndexPayloadAdmission> admissions;
  admissions.push_back(AdmitPayload(AssemblePayload(fixture, stream.locators[0],
                                                    "alpha-name", "alpha-city")
                                        .record));
  admissions.push_back(AdmitPayload(AssemblePayload(fixture, stream.locators[1],
                                                    "bravo-name", "bravo-city")
                                        .record));
  admissions.push_back(AdmitPayload(AssemblePayload(fixture, stream.locators[2],
                                                    "charlie-name", "charlie-city")
                                        .record));
  exec::CoveringProjectionOnlyScanRequest request;
  request.physical_stream = &stream;
  for (const auto& admission : admissions) {
    request.admissions.push_back(&admission);
  }

  const auto result = exec::ExecuteCoveringProjectionOnlyScan(request);
  Require(result.ok, "covering projection-only scan failed");
  Require(result.projection_only, "covering scan was not projection-only");
  Require(result.runtime_route_capability,
          "covering runtime route capability missing");
  Require(!result.benchmark_clean, "covering scan claimed benchmark clean");
  Require(!result.full_table_scan_or_materialization,
          "covering scan used full table materialization");
  Require(result.rows.size() == stream.locators.size(),
          "covering projection row count mismatch");
  for (std::size_t i = 0; i < stream.locators.size(); ++i) {
    Require(result.rows[i].row_uuid == stream.locators[i].row_uuid,
            "covering scan did not preserve physical stream order");
    Require(result.rows[i].version_uuid == stream.locators[i].version_uuid,
            "covering scan did not preserve version binding");
    Require(result.rows[i].cells.size() == 2,
            "covering scan projected cell count mismatch");
  }
  RequireCommonAcceptedEvidence(result.evidence);
  Require(Has(result.evidence, "irc061.covering_projection_only_scan=true"),
          "covering projection-only evidence missing");
  Require(Has(result.evidence, "irc061.covering_full_table_scan_used=false"),
          "covering no-full-scan evidence missing");
  Require(Has(result.evidence,
              "irc061.covering_base_row_materialization_used=false"),
          "covering no-base-materialization evidence missing");
  Require(HasContaining(result.evidence,
                        "covering_payload.required_rechecks_proven=true"),
          "covering required recheck proof was not consumed");
}

template <typename Mutate>
void ExpectLateRefusal(std::string_view detail, Mutate mutate) {
  const Fixture fixture;
  auto stream = PhysicalStream(fixture);
  exec::IndexRuntimeEngineRecheckProof proof;
  mutate(stream, proof);
  const auto result = exec::ConsumeIndexedRowIdStreamForLateMaterialization(
      stream, proof,
      [](const exec::IndexedPhysicalOperatorLocator& locator) {
        exec::LateMaterializationIndexedProviderResult out;
        out.ok = true;
        out.row.row_uuid = locator.row_uuid;
        out.row.version_uuid = locator.version_uuid;
        return out;
      });
  Require(!result.ok, "late materialization refusal was not fail-closed");
  Require(result.diagnostic_detail == detail,
          "late materialization refusal detail mismatch");
}

template <typename Mutate>
void ExpectCoveringRefusal(std::string_view detail, Mutate mutate) {
  const Fixture fixture;
  auto stream = PhysicalStream(fixture);
  auto admission =
      AdmitPayload(AssemblePayload(fixture, stream.locators.front(),
                                   "alpha-name", "alpha-city")
                       .record);
  exec::CoveringProjectionOnlyScanRequest request;
  request.physical_stream = &stream;
  request.admissions = {&admission};
  mutate(stream, admission, request);
  const auto result = exec::ExecuteCoveringProjectionOnlyScan(request);
  Require(!result.ok, "covering scan refusal was not fail-closed");
  Require(result.diagnostic_detail == detail,
          "covering scan refusal detail mismatch");
}

void FailClosedCasesAreExact() {
  ExpectLateRefusal("physical_index_tree_required",
                    [](auto&, auto& proof) {
                      proof.physical_tree_present = false;
                    });
  ExpectLateRefusal("stale_or_unsafe_plan",
                    [](auto&, auto& proof) { proof.plan_safe = false; });
  ExpectLateRefusal("durable_mga_inventory_proof_required",
                    [](auto&, auto& proof) {
                      proof.durable_mga_inventory_proof = false;
                    });
  ExpectLateRefusal("mga_visibility_recheck_required",
                    [](auto&, auto& proof) {
                      proof.mga_visibility_rechecked_by_engine = false;
                    });
  ExpectLateRefusal("security_authorization_recheck_required",
                    [](auto&, auto& proof) {
                      proof.security_authorized_by_engine = false;
                    });
  ExpectLateRefusal("redaction_recheck_required",
                    [](auto&, auto& proof) {
                      proof.redaction_checked_by_engine = false;
                    });
  ExpectLateRefusal("unsafe_payload_freshness",
                    [](auto&, auto& proof) {
                      proof.payload_freshness_safe = false;
                    });
  ExpectLateRefusal("descriptor_or_map_scan_fallback_forbidden",
                    [](auto&, auto& proof) {
                      proof.descriptor_or_map_scan_fallback = true;
                    });
  ExpectLateRefusal("locator_mga_recheck_required",
                    [](auto& stream, auto&) {
                      stream.locators.front().mga_recheck_required = false;
                    });

  {
    const Fixture fixture;
    const auto stream = PhysicalStream(fixture);
    const auto result = exec::ConsumeIndexedRowIdStreamForLateMaterialization(
        stream, {},
        [&](const exec::IndexedPhysicalOperatorLocator& locator) {
          exec::LateMaterializationIndexedProviderResult out;
          out.ok = true;
          out.row.row_uuid = locator.row_uuid;
          out.row.version_uuid = fixture.version_c;
          return out;
        });
    Require(!result.ok &&
                result.diagnostic_detail == "base_row_locator_binding_mismatch",
            "base-row binding mismatch did not fail closed");
  }

  ExpectCoveringRefusal("covering_payload_missing",
                        [](auto&, auto&, auto& request) {
                          request.admissions.clear();
                        });
  ExpectCoveringRefusal("covering_projection_only_required",
                        [](auto&, auto& admission, auto&) {
                          admission.index_only_admitted = false;
                          admission.base_row_recheck_required = true;
                          admission.base_row_recheck_handoff_proven = true;
                        });
  ExpectCoveringRefusal("covering_payload_recheck_proof_missing",
                        [](auto&, auto& admission, auto&) {
                          admission.evidence.erase(
                              std::remove(admission.evidence.begin(),
                                          admission.evidence.end(),
                                          "covering_payload.required_rechecks_proven=true"),
                              admission.evidence.end());
                        });
  ExpectCoveringRefusal("covering_payload_authority_forbidden",
                        [](auto&, auto& admission, auto&) {
                          admission.transaction_finality_authority = true;
                        });
  ExpectCoveringRefusal("descriptor_or_map_scan_fallback_forbidden",
                        [](auto&, auto&, auto& request) {
                          request.proof.descriptor_or_map_scan_fallback = true;
                        });

  {
    const Fixture fixture;
    const auto stream = PhysicalStream(fixture);
    auto first = AdmitPayload(AssemblePayload(fixture, stream.locators[0],
                                             "alpha-name", "alpha-city")
                                  .record);
    auto duplicate = first;
    auto second = AdmitPayload(AssemblePayload(fixture, stream.locators[1],
                                              "bravo-name", "bravo-city")
                                   .record);
    auto third = AdmitPayload(AssemblePayload(fixture, stream.locators[2],
                                             "charlie-name", "charlie-city")
                                  .record);
    exec::CoveringProjectionOnlyScanRequest request;
    request.physical_stream = &stream;
    request.admissions = {&first, &duplicate, &second, &third};
    const auto result = exec::ExecuteCoveringProjectionOnlyScan(request);
    Require(!result.ok && result.diagnostic_detail == "covering_payload_duplicate",
            "duplicate covering payload admission did not fail closed");
  }

  {
    const Fixture fixture;
    const auto stream = PhysicalStream(fixture);
    auto first = AdmitPayload(AssemblePayload(fixture, stream.locators[0],
                                             "alpha-name", "alpha-city")
                                  .record);
    auto second = AdmitPayload(AssemblePayload(fixture, stream.locators[1],
                                              "bravo-name", "bravo-city")
                                   .record);
    auto third = AdmitPayload(AssemblePayload(fixture, stream.locators[2],
                                             "charlie-name", "charlie-city")
                                  .record);
    exec::IndexedPhysicalOperatorLocator extra_locator = stream.locators[0];
    extra_locator.row_uuid =
        UuidText(platform::UuidKind::row, 1700610300000ull, 0x74);
    extra_locator.version_uuid =
        UuidText(platform::UuidKind::row, 1700610400000ull, 0x75);
    auto extra = AdmitPayload(AssemblePayload(fixture, extra_locator,
                                             "extra-name", "extra-city")
                                  .record);
    exec::CoveringProjectionOnlyScanRequest request;
    request.physical_stream = &stream;
    request.admissions = {&first, &second, &third, &extra};
    const auto result = exec::ExecuteCoveringProjectionOnlyScan(request);
    Require(!result.ok && result.diagnostic_detail == "covering_payload_unconsumed",
            "extra covering payload admission did not fail closed");
  }
}

}  // namespace

int main() {
  LateMaterializationConsumesPhysicalStream();
  CoveringProjectionOnlyConsumesPayloads();
  FailClosedCasesAreExact();
  std::cout << "late_materialization_covering_scan_gate=passed\n";
  return EXIT_SUCCESS;
}
