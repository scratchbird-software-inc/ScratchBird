// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ExistingTypedCarrierFixtureMain
#include "typed_result_transport_carrier_test.cpp"
#undef main
#include "core/hash/hash_digest.hpp"
#include <array>
#include <algorithm>

namespace {
unsigned identity_checks = 0, identity_failures = 0;
void CheckIdentity(bool ok, const char* detail) {
  ++identity_checks;
  if (!ok) {
    ++identity_failures;
    if (identity_failures < 8) std::cerr << "FAIL " << detail << '\n';
  }
}
void Reseal(std::vector<byte>& bytes, bool descriptor) {
  const std::size_t offset = descriptor ? 96 : 192;
  std::fill_n(bytes.begin() + offset, 32, 0);
  const std::string_view domain = descriptor
      ? "ScratchBird.PsResultDescriptorVector.V1" : "ScratchBird.PsRowDataPacket.V1";
  std::vector<byte> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(material);
  Require(hash.ok(), "independent evidence computation failed");
  std::copy(hash.digest.begin(), hash.digest.end(), bytes.begin() + offset);
}
template<class Result> void InvalidIdentity(const Result& result) {
  CheckIdentity(!result.ok() && result.diagnostic_code == "UUID.ENGINE_IDENTITY_NOT_V7",
                "present invalid system UUID accepted or wrong registered diagnostic");
}
void IdentityCases() {
  auto descriptor = Descriptor();
  const auto encoded = wire::EncodeTypedResultRowDescriptor(descriptor);
  Require(encoded.ok(), "valid system descriptor fixture refused");
  const auto authority = ExecuteAuthority();
  const auto carrier = CursorOpenCarrier(encoded.descriptor, encoded.encoded);
  const auto good = wire::ValidateTypedResultExecuteCarrierV1(authority, carrier,
                                                             AuthorityFor(encoded.descriptor));
  Require(good.ok(), "valid cursor fixture refused");
  std::vector<wire::TypedResultUuid> invalid;
  for (unsigned version = 0; version != 16; ++version) {
    if (version == 7) continue;
    auto id = Uuid(0x31); id[6] = static_cast<byte>(version << 4);
    invalid.push_back(id);
  }
  for (byte variant : std::array<byte, 3>{0, 0x40, 0xc0}) {
    auto id = Uuid(0x31); id[8] = variant; invalid.push_back(id);
  }
  for (const auto& bad : invalid) {
    // Four independently specified descriptor UUID offsets, no production
    // writer used to manufacture a post-repair invalid golden frame.
    for (unsigned field = 0; field != 4; ++field) {
      auto candidate = descriptor;
      auto* id = field == 0 ? &candidate.descriptor_uuid
               : field == 1 ? &candidate.datatype_catalog_snapshot_uuid
               : field == 2 ? &candidate.columns[0].descriptor_uuid
                            : &candidate.columns[0].type_uuid;
      *id = bad;
      InvalidIdentity(wire::EncodeTypedResultRowDescriptor(candidate));
      auto corrupt = encoded.encoded;
      // Descriptor header128; column UUID offsets16 and40 within its prefix.
      const auto offset = std::array<std::size_t, 4>{24, 48, 144, 168}[field];
      std::copy(bad.begin(), bad.end(), corrupt.begin() + offset);
      Reseal(corrupt, true);
      InvalidIdentity(wire::DecodeTypedResultRowDescriptor(corrupt));
    }
    for (unsigned field = 0; field != 8; ++field) {
      auto outer = carrier;
      auto expected = authority;
      auto* id = field == 0 ? &outer.server_request_uuid
               : field == 1 ? &outer.transaction_uuid
               : field == 2 ? &outer.cursor_uuid
               : field == 3 ? &outer.cursor_stream_descriptor.descriptor_uuid
               : field == 4 ? &outer.query_handle.execution_uuid
               : field == 5 ? &outer.query_handle.result_set_uuid
               : field == 6 ? &outer.query_handle.row_descriptor_uuid
                            : &outer.query_handle.snapshot_uuid;
      *id = bad;
      if (field == 0) expected.expected_server_request_uuid = bad;
      InvalidIdentity(wire::ValidateTypedResultExecuteCarrierV1(
          expected, outer, AuthorityFor(encoded.descriptor)));
    }
    auto batch = Batch(encoded.descriptor, carrier.query_handle, 0, true, false);
    auto binding = ExecuteBinding(carrier.query_handle, batch.rows.size());
    const auto packet = wire::EncodeTypedResultBatch(batch, encoded.descriptor, binding);
    Require(packet.ok(), "valid packet fixture refused");
    // Every inner and independent binding UUID, with independently resealed
    // inner bytes. Evidence must not admit an invalid system identity.
    for (unsigned field = 0; field != 11; ++field) {
      auto candidate = batch;
      auto outer = binding;
      std::array<wire::TypedResultUuid*, 11> ids{
          &candidate.execution_uuid, &candidate.result_set_uuid, &candidate.batch_uuid,
          &candidate.row_descriptor_uuid, &candidate.snapshot_uuid, &candidate.cursor_uuid,
          &outer.execution_uuid, &outer.result_set_uuid, &outer.snapshot_uuid,
          &outer.cursor_uuid, &outer.cursor_stream_descriptor_uuid};
      *ids[field] = bad;
      InvalidIdentity(wire::EncodeTypedResultBatch(candidate, encoded.descriptor, outer));
      auto corrupt = packet.encoded;
      if (field < 6) {
        const auto offset = std::array<std::size_t, 6>{24, 40, 56, 80, 152, 168}[field];
        std::copy(bad.begin(), bad.end(), corrupt.begin() + offset);
        Reseal(corrupt, false);
      }
      InvalidIdentity(wire::DecodeTypedResultBatch(corrupt, encoded.descriptor, outer));
    }

    for (unsigned field = 0; field != 13; ++field) {
      auto prior = good.cursor_state;
      auto fetch = FetchAuthority(carrier.cursor_stream_descriptor);
      wire::TypedResultFetchCarrierV1 response;
      response.cursor_uuid = carrier.cursor_uuid;
      response.end_of_cursor = true;
      std::array<wire::TypedResultUuid*, 13> ids{
          &fetch.cursor_uuid, &fetch.cursor_stream_descriptor_uuid, &response.cursor_uuid,
          &prior.cursor_uuid, &prior.cursor_stream_descriptor.descriptor_uuid,
          &prior.query_handle.execution_uuid, &prior.query_handle.result_set_uuid,
          &prior.query_handle.row_descriptor_uuid, &prior.query_handle.snapshot_uuid,
          &prior.row_descriptor.descriptor_uuid, &prior.row_descriptor.datatype_catalog_snapshot_uuid,
          &prior.row_descriptor.columns[0].descriptor_uuid, &prior.row_descriptor.columns[0].type_uuid};
      *ids[field] = bad;
      const auto refused = wire::ValidateTypedResultFetchCarrierV1(fetch, response, prior);
      InvalidIdentity(refused);
      CheckIdentity(refused.cursor_state.cursor_uuid == prior.cursor_uuid &&
                        refused.cursor_state.terminal == prior.terminal &&
                        refused.cursor_state.encoded_row_descriptor == prior.encoded_row_descriptor,
                    "UUID refusal changed the caller's live cursor state");
    }
  }
  auto missing = descriptor; missing.descriptor_uuid = {};
  CheckIdentity(!wire::EncodeTypedResultRowDescriptor(missing).ok(),
                "missing required UUID admitted");
  auto no_transaction = carrier;
  no_transaction.transaction_uuid = {}; no_transaction.local_transaction_id = 0;
  CheckIdentity(wire::ValidateTypedResultExecuteCarrierV1(
      authority, no_transaction, AuthorityFor(encoded.descriptor)).ok(),
      "permitted nil transaction absence became a UUID error");
  auto valid_batch = Batch(encoded.descriptor, carrier.query_handle, 0, true, false);
  auto valid_binding = ExecuteBinding(carrier.query_handle, valid_batch.rows.size());
  auto crossed = wire::EncodeTypedResultBatch(valid_batch, encoded.descriptor, valid_binding).encoded;
  const auto other_valid = Uuid(0x79);
  std::copy(other_valid.begin(), other_valid.end(), crossed.begin() + 24);
  Reseal(crossed, false);
  const auto mismatch = wire::DecodeTypedResultBatch(crossed, encoded.descriptor, valid_binding);
  CheckIdentity(!mismatch.ok() && mismatch.diagnostic_code != "UUID.ENGINE_IDENTITY_NOT_V7",
                "UUIDv7 validation replaced independent result authority binding");
  // Earlier versions remain data, not system identities. Exact binary payload
  // bytes survive both directions; never parse or normalize a UUID string.
  descriptor.columns[0].canonical_type_id = datatypes::CanonicalTypeId::uuid;
  descriptor.columns[0].codec_id = "datatype.uuid.v1";
  descriptor.columns[0].canonical_value_bytes = 16;
  for (unsigned version = 1; version != 8; ++version) {
    auto value = Uuid(0x77); value[6] = static_cast<byte>(version << 4);
    auto batch = Batch(descriptor, QueryHandle(descriptor), 0, true, false);
    batch.rows[0].cells[0].canonical_payload.assign(value.begin(), value.end());
    const auto binding = ExecuteBinding(QueryHandle(descriptor), batch.rows.size());
    const auto packet = wire::EncodeTypedResultBatch(batch, descriptor, binding);
    CheckIdentity(packet.ok(), "earlier admitted user UUID version refused");
    if (!packet.ok()) continue;
    const auto decoded = wire::DecodeTypedResultBatch(packet.encoded, descriptor, binding);
    CheckIdentity(decoded.ok() && decoded.batch.rows[0].cells[0].canonical_payload ==
                      std::vector<byte>(value.begin(), value.end()),
                  "user UUID bytes were rewritten by system identity validation");
  }
}
}
int main() {
  try { IdentityCases(); }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
  std::cout << identity_checks << " checks; " << identity_failures << " failures\n";
  return identity_failures ? 1 : 0;
}
