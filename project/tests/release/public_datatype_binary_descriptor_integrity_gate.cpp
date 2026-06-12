// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_transport_api.hpp"
#include "datatype_advanced_family.hpp"
#include "datatype_binary.hpp"
#include "datatype_document.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace api = scratchbird::engine::internal_api;

using scratchbird::core::platform::byte;

bool Expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

std::vector<byte> Bytes(const std::string& value) {
  return {value.begin(), value.end()};
}

dt::DatatypeDescriptorRecord Record(std::string name, std::string value) {
  dt::DatatypeDescriptorRecord record;
  record.field_name = std::move(name);
  record.payload = Bytes(value);
  return record;
}

dt::DatatypeDescriptorEnvelope Envelope(
    dt::DatatypeDescriptorEnvelopeKind kind,
    std::vector<dt::DatatypeDescriptorRecord> records,
    dt::DatatypeDescriptorIntegrityProfile profile =
        dt::DatatypeDescriptorIntegrityProfile::strong) {
  dt::DatatypeDescriptorEnvelope envelope;
  envelope.kind = kind;
  envelope.integrity_profile = profile;
  envelope.records = std::move(records);
  return envelope;
}

bool DecodeHasRecord(const dt::DatatypeDescriptorEnvelopeResult& decoded,
                     const std::string& name,
                     const std::string& value) {
  for (const auto& record : decoded.envelope.records) {
    if (record.field_name == name) {
      return std::string(record.payload.begin(), record.payload.end()) == value;
    }
  }
  return false;
}

api::EngineDescriptor Descriptor(std::string uuid,
                                 std::string kind,
                                 std::string type_name,
                                 std::string encoded) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = std::move(uuid);
  descriptor.descriptor_kind = std::move(kind);
  descriptor.canonical_type_name = std::move(type_name);
  descriptor.encoded_descriptor = std::move(encoded);
  return descriptor;
}

}  // namespace

int main() {
  bool ok = true;

  const auto domain = Envelope(dt::DatatypeDescriptorEnvelopeKind::domain_policy,
                               {
                                   Record("domain_uuid", "019dffff-0000-7000-8000-000000000042"),
                                   Record("base_type", "decimal"),
                                   Record("policy_envelope", "sblr_predicate:length_gte:1"),
                               });
  const auto encoded_domain = dt::EncodeDatatypeDescriptorEnvelope(domain);
  ok = Expect(encoded_domain.ok(), "domain descriptor envelope did not encode") && ok;
  if (!encoded_domain.ok()) {
    return EXIT_FAILURE;
  }
  ok = Expect(encoded_domain.digest_algorithm == "sha256",
              "domain descriptor envelope did not use SHA-256") && ok;
  ok = Expect(encoded_domain.digest_bytes == 32,
              "domain descriptor envelope did not use a full strong digest") && ok;
  ok = Expect(encoded_domain.digest_material.size() == 32,
              "domain descriptor envelope did not expose full digest material") && ok;
  const auto decoded_domain = dt::DecodeDatatypeDescriptorEnvelope(encoded_domain.encoded);
  ok = Expect(decoded_domain.ok(), "domain descriptor envelope did not decode") && ok;
  ok = Expect(DecodeHasRecord(decoded_domain, "base_type", "decimal"),
              "domain descriptor envelope lost length-prefixed record") &&
       ok;

  auto corrupted_domain = encoded_domain.encoded;
  corrupted_domain.back() ^= static_cast<byte>(0x5a);
  const auto corrupt_domain_result = dt::DecodeDatatypeDescriptorEnvelope(corrupted_domain);
  ok = Expect(!corrupt_domain_result.ok(),
              "domain descriptor envelope accepted corrupted payload") &&
       ok;

  auto weak_domain = domain;
  weak_domain.integrity_profile = dt::DatatypeDescriptorIntegrityProfile::fast;
  const auto weak_result = dt::EncodeDatatypeDescriptorEnvelope(weak_domain);
  ok = Expect(!weak_result.ok(), "descriptor envelope accepted weak integrity profile") && ok;

  const std::vector<byte> protected_key = Bytes("descriptor-key-material");
  const auto protected_document = Envelope(
      dt::DatatypeDescriptorEnvelopeKind::document_descriptor,
      {Record("document_type", "json_document"), Record("canonical_format", "json_text")},
      dt::DatatypeDescriptorIntegrityProfile::protected_keyed);
  const auto protected_encoded =
      dt::EncodeDatatypeDescriptorEnvelope(protected_document, protected_key);
  ok = Expect(protected_encoded.ok(), "protected document descriptor did not encode") && ok;
  if (!protected_encoded.ok()) {
    return EXIT_FAILURE;
  }
  ok = Expect(protected_encoded.digest_algorithm == "hmac-sha256",
              "protected document descriptor did not use HMAC-SHA-256") && ok;
  ok = Expect(protected_encoded.digest_bytes == 32,
              "protected document descriptor did not use a full MAC digest") && ok;
  ok = Expect(!dt::DecodeDatatypeDescriptorEnvelope(protected_encoded.encoded).ok(),
              "protected descriptor decoded without key material") &&
       ok;
  ok = Expect(dt::DecodeDatatypeDescriptorEnvelope(protected_encoded.encoded, protected_key).ok(),
              "protected descriptor did not decode with key material") &&
       ok;
  ok = Expect(!dt::DecodeDatatypeDescriptorEnvelope(protected_encoded.encoded, Bytes("wrong-key")).ok(),
              "protected descriptor decoded with wrong key material") &&
       ok;

  const auto set_descriptor =
      Envelope(dt::DatatypeDescriptorEnvelopeKind::structured_descriptor,
               {
                   Record("structured_kind", "set"),
                   Record("element_descriptor_uuid", "019dffff-0000-7000-8000-000000000043"),
                   Record("duplicates_allowed", "0"),
                   Record("null_members_allowed", "0"),
               });
  ok = Expect(dt::DecodeDatatypeDescriptorEnvelope(
                  dt::EncodeDatatypeDescriptorEnvelope(set_descriptor).encoded)
                  .ok(),
              "structured set descriptor did not round trip") &&
       ok;

  dt::DocumentCanonicalizationRequest document_request;
  document_request.type_id = dt::CanonicalTypeId::json_document;
  document_request.encoded_value = "{\"release\":true}";
  const auto document = dt::CanonicalizeDocumentValue(document_request);
  ok = Expect(document.ok(), "document canonicalization did not accept json descriptor payload") && ok;
  const auto document_descriptor =
      Envelope(dt::DatatypeDescriptorEnvelopeKind::document_descriptor,
               {
                   Record("document_type", dt::CanonicalTypeName(document.canonical_type_id)),
                   Record("canonical_format", document.canonical_format),
                   Record("canonical_value", document.canonical_value),
               });
  ok = Expect(dt::DecodeDatatypeDescriptorEnvelope(
                  dt::EncodeDatatypeDescriptorEnvelope(document_descriptor).encoded)
                  .ok(),
              "document descriptor envelope did not round trip") &&
       ok;

  dt::AdvancedDatatypeFamilyRequest advanced_request;
  advanced_request.type_id = dt::CanonicalTypeId::dense_vector;
  advanced_request.operation = dt::AdvancedDatatypeOperationKind::nearest_neighbor;
  advanced_request.index_kind = dt::AdvancedDatatypeIndexKind::hnsw;
  advanced_request.descriptor_profile = "dimension=128;element_type=real32";
  advanced_request.vector_dimension = 128;
  const auto advanced = dt::EvaluateAdvancedDatatypeFamily(advanced_request);
  ok = Expect(advanced.ok(), "advanced family descriptor request did not validate") && ok;
  const auto advanced_descriptor =
      Envelope(dt::DatatypeDescriptorEnvelopeKind::advanced_family_descriptor,
               {
                   Record("family", dt::AdvancedDatatypeFamilyName(advanced.family)),
                   Record("operation", dt::AdvancedDatatypeOperationKindName(advanced_request.operation)),
                   Record("index", dt::AdvancedDatatypeIndexKindName(advanced_request.index_kind)),
                   Record("optimizer_admitted", advanced.optimizer_admitted ? "1" : "0"),
               });
  ok = Expect(dt::DecodeDatatypeDescriptorEnvelope(
                  dt::EncodeDatatypeDescriptorEnvelope(advanced_descriptor).encoded)
                  .ok(),
              "advanced family descriptor envelope did not round trip") &&
       ok;

  const std::string descriptor_payload("canonical=map\0field", 19);
  const std::string value_payload("value\0bytes\twith-tabs", 21);
  api::EngineDatatypeTransportRecord transport;
  transport.transport_scope = "backup";
  transport.descriptor = Descriptor("019dffff-0000-7000-8000-000000000044",
                                    "structured",
                                    "map",
                                    descriptor_payload);
  transport.value.descriptor = transport.descriptor;
  transport.value.encoded_value = value_payload;
  transport.compatibility_dialect = "postgresql";
  transport.reference_label = "jsonb";
  const auto encoded_transport = api::EncodeDatatypeTransportRecord(transport);
  ok = Expect(encoded_transport.ok, "datatype transport did not encode") && ok;
  ok = Expect(encoded_transport.encoded_envelope.rfind("SBDTTRAN1", 0) != 0,
              "datatype transport still used legacy text magic") &&
       ok;
  const auto decoded_transport = api::DecodeDatatypeTransportRecord(encoded_transport.encoded_envelope);
  ok = Expect(decoded_transport.ok, "datatype transport did not decode") && ok;
  ok = Expect(decoded_transport.record.descriptor.encoded_descriptor == descriptor_payload,
              "datatype transport lost binary descriptor payload") &&
       ok;
  ok = Expect(decoded_transport.record.value.encoded_value == value_payload,
              "datatype transport lost binary value payload") &&
       ok;
  ok = Expect(decoded_transport.record.reference_label_alias_only,
              "datatype transport did not preserve alias-only reference label policy") &&
       ok;

  auto corrupted_transport = encoded_transport.encoded_envelope;
  corrupted_transport.back() = static_cast<char>(corrupted_transport.back() ^ 0x33);
  ok = Expect(!api::DecodeDatatypeTransportRecord(corrupted_transport).ok,
              "datatype transport accepted corrupted envelope") &&
       ok;
  ok = Expect(!api::DecodeDatatypeTransportRecord("not-a-transport-envelope").ok,
              "datatype transport accepted non-envelope text") &&
       ok;

  if (!ok) {
    return 1;
  }
  std::cout << "public_datatype_binary_descriptor_integrity_gate=passed\n";
  return 0;
}
